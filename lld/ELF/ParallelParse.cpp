//===- ParallelParse.cpp - Parallel input file parsing --------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Parallel file parsing pipeline for ELF object files, enabled via the
// LLD_PARALLEL_PARSE environment variable. Inspired by the Wild linker.
//
// Phases (all work is parallel except the final SymbolTable install):
//   1. Read symbols + COMDAT group names from each object file.
//   2. Populate per-bucket DenseMaps keyed by symbol name.
//   3. BFS-activate archive members that satisfy strong undef references.
//   4. Create Symbol objects and resolve them per bucket; the per-bucket maps
//      become the installed SymbolTable shard maps. Also pre-populate
//      sharded COMDAT groups.
//   5. For each activated file: initSectionsAndLocalSyms(), wire globals from
//      a flat globalToSym[] array, then run the standard postParse().
//
//===----------------------------------------------------------------------===//

#include "ParallelParse.h"
#include "Config.h"
#include "InputFiles.h"
#include "InputSection.h"
#include "SymbolTable.h"
#include "Symbols.h"
#include "Target.h"
#include "lld/Common/Memory.h"
#include "llvm/Object/ELF.h"
#include "llvm/Support/Parallel.h"
#include "llvm/Support/TimeProfiler.h"

using namespace llvm;
using namespace llvm::ELF;
using namespace llvm::object;
using namespace lld;
using namespace lld::elf;

namespace {
constexpr unsigned kNumBuckets = 32;

// A symbol reference collected in phase 1.
struct PendingSym {
  CachedHashStringRef name;
  uint32_t elfIdx;
  uint16_t fileIdx;
  uint8_t flags; // bit 0 = isDefined, bit 1 = isWeakUndef
  bool isDefined() const { return flags & 1; }
  bool isStrongUndef() const { return flags == 0; }
};

struct FileData {
  SmallVector<PendingSym, 0> buckets[kNumBuckets];
  SmallVector<CachedHashStringRef, 0> comdatNames;
};

struct SymRef {
  uint16_t fileIdx;
  uint32_t elfIdx;
  bool isDef;
};
} // namespace

template <class ELFT>
static void readFileSymbols(ELFFileBase *file, uint16_t fileIdx,
                            FileData &out) {
  auto eSyms = file->getELFSyms<ELFT>();
  StringRef strtab = file->getStringTable();
  const char *strData = strtab.data();
  size_t strSize = strtab.size();
  auto obj = file->getObj<ELFT>();

  // Collect COMDAT group signature names.
  for (auto &sec : file->getELFShdrs<ELFT>()) {
    if (sec.sh_type != SHT_GROUP || sec.sh_info >= eSyms.size())
      continue;
    uint32_t nameOff = eSyms[sec.sh_info].st_name;
    if (nameOff >= strSize)
      continue;
    auto entries =
        cantFail(obj.template getSectionContentsAsArray<uint32_t>(sec));
    if (!entries.empty() && (entries[0] & GRP_COMDAT))
      out.comdatNames.push_back(
          CachedHashStringRef(StringRef(strData + nameOff)));
  }

  // Collect global symbol references, bucketed by name hash.
  for (size_t i = file->getFirstGlobal(), e = eSyms.size(); i < e; ++i) {
    uint32_t nameOff = eSyms[i].st_name;
    StringRef name = LLVM_UNLIKELY(nameOff >= strSize)
                         ? StringRef()
                         : StringRef(strData + nameOff);
    StringRef stem = name;
    size_t pos = name.find('@');
    if (pos != StringRef::npos && pos + 1 < name.size() && name[pos + 1] == '@')
      stem = name.take_front(pos);

    CachedHashStringRef key(stem);
    uint8_t flags = 0;
    if (eSyms[i].st_shndx != SHN_UNDEF) {
      flags |= 1;
      if (LLVM_UNLIKELY(eSyms[i].st_shndx == SHN_COMMON))
        file->hasCommonSyms = true;
    } else if (eSyms[i].getBinding() == STB_WEAK) {
      flags |= 2;
    }
    out.buckets[key.hash() % kNumBuckets].push_back(
        {key, (uint32_t)i, fileIdx, flags});
  }
}

template <class ELFT>
static void doParallelParse(Ctx &ctx, ArrayRef<ELFFileBase *> objFiles,
                            ArrayRef<uint32_t> firstGlobalId,
                            uint32_t totalGlobals, ArrayRef<bool> isArchive) {
  // ---- Phase 1: Read symbols + comdat names (parallel per file) ----
  auto fileData = std::make_unique<FileData[]>(objFiles.size());
  {
    llvm::TimeTraceScope ts("Read symbols (parallel)");
    parallelFor(0, objFiles.size(), [&](size_t i) {
      readFileSymbols<ELFT>(objFiles[i], (uint16_t)i, fileData[i]);
    });
  }

  // ---- Phase 2: Populate per-bucket DenseMaps ----
  DenseMap<CachedHashStringRef, SmallVector<SymRef, 1>> buckets[kNumBuckets];
  {
    llvm::TimeTraceScope ts("Populate symbol DB (parallel)");
    parallelFor(0, (size_t)kNumBuckets, [&](size_t b) {
      size_t n = 0;
      for (size_t i = 0; i < objFiles.size(); ++i)
        n += fileData[i].buckets[b].size();
      buckets[b].reserve(n);
      for (size_t i = 0; i < objFiles.size(); ++i)
        for (const auto &ps : fileData[i].buckets[b])
          buckets[b][ps.name].push_back(
              {ps.fileIdx, ps.elfIdx, ps.isDefined()});
    });
  }

  // ---- Phase 3: BFS archive activation ----
  auto activated = std::make_unique<std::atomic<bool>[]>(objFiles.size());
  for (size_t i = 0; i < objFiles.size(); ++i)
    activated[i].store(!isArchive[i], std::memory_order_relaxed);
  {
    llvm::TimeTraceScope ts("Resolve symbols + activate archives");
    const size_t numThreads = parallel::getThreadCount();
    SmallVector<uint16_t, 0> cur;
    for (size_t i = 0; i < objFiles.size(); ++i)
      if (!isArchive[i])
        cur.push_back(i);
    auto queues = std::make_unique<SmallVector<uint16_t, 0>[]>(numThreads);

    while (!cur.empty()) {
      parallelFor(0, cur.size(), [&](size_t wi) {
        const unsigned tid = parallel::getThreadIndex();
        for (unsigned b = 0; b < kNumBuckets; ++b) {
          for (const auto &ps : fileData[cur[wi]].buckets[b]) {
            if (!ps.isStrongUndef())
              continue;
            auto it = buckets[b].find(ps.name);
            if (it == buckets[b].end())
              continue;
            // Already satisfied by some active def?
            bool satisfied = false;
            for (const SymRef &r : it->second)
              if (r.isDef &&
                  activated[r.fileIdx].load(std::memory_order_relaxed)) {
                satisfied = true;
                break;
              }
            if (satisfied)
              continue;
            // Activate the first file carrying a def.
            for (const SymRef &r : it->second)
              if (r.isDef) {
                if (!activated[r.fileIdx].exchange(true,
                                                   std::memory_order_relaxed))
                  queues[tid].push_back(r.fileIdx);
                break;
              }
          }
        }
      });
      cur.clear();
      for (size_t t = 0; t < numThreads; ++t) {
        cur.append(std::move(queues[t]));
        queues[t].clear();
      }
    }
  }

  // ---- Phase 4: Create Symbol objects + resolve + build shard maps ----
  uint32_t shardOffsets[kNumBuckets + 1];
  shardOffsets[0] = 0;
  for (unsigned b = 0; b < kNumBuckets; ++b)
    shardOffsets[b + 1] = shardOffsets[b] + buckets[b].size();
  uint32_t totalUnique = shardOffsets[kNumBuckets];

  SmallVector<Symbol *, 0> symVector(totalUnique);
  auto shardMaps =
      std::make_unique<DenseMap<CachedHashStringRef, int>[]>(kNumBuckets);
  auto globalToSym = std::make_unique<Symbol *[]>(totalGlobals);
  // Batch-allocate all SymbolUnions contiguously for cache-friendly access.
  SymbolUnion *symStorage =
      getSpecificAllocSingleton<SymbolUnion>().Allocate(totalUnique);
  memset(symStorage, 0, sizeof(SymbolUnion) * totalUnique);
  // Per-symbol anchor key, used to compute a deterministic symVector order
  // below. The key is (ownerFileIdx << 32) | elfIdxInOwner, with ~0ULL
  // meaning "no activated ref" (dead symbol).
  auto anchors = std::make_unique<uint64_t[]>(totalUnique);

  {
    llvm::TimeTraceScope ts("Create symbols + resolve (parallel)");
    parallelFor(0, (size_t)kNumBuckets, [&](size_t b) {
      auto &shardMap = shardMaps[b];
      shardMap.reserve(buckets[b].size() * 4 / 3);
      unsigned local = 0;
      for (auto &kv : buckets[b]) {
        int idx = shardOffsets[b] + local++;
        shardMap[kv.first] = idx;

        auto *sym = reinterpret_cast<Symbol *>(&symStorage[idx]);
        // Recover the full name (with @@version) from the string table.
        StringRef fullName(kv.first.val().data());
        sym->setName(fullName);
        sym->partition = 1;
        sym->versionId = VER_NDX_GLOBAL;
        if (fullName.find('@') != StringRef::npos)
          sym->hasVersionSuffix = true;
        symVector[idx] = sym;

        for (const SymRef &r : kv.second) {
          if (!activated[r.fileIdx].load(std::memory_order_relaxed))
            continue;
          auto *f = objFiles[r.fileIdx];
          auto eSym = f->template getELFSyms<ELFT>()[r.elfIdx];
          if (!r.isDef) {
            sym->resolve(ctx, Undefined(f, StringRef(), eSym.getBinding(),
                                        eSym.st_other, eSym.getType()));
            sym->referenced = true;
          } else if (LLVM_UNLIKELY(eSym.st_shndx == SHN_COMMON)) {
            sym->resolve(ctx,
                         CommonSymbol(ctx, f, StringRef(), eSym.getBinding(),
                                      eSym.st_other, eSym.getType(),
                                      eSym.st_value, eSym.st_size));
          } else {
            sym->resolve(ctx, Defined(ctx, f, StringRef(), eSym.getBinding(),
                                      eSym.st_other, eSym.getType(),
                                      eSym.st_value, eSym.st_size, nullptr));
          }
          sym->isUsedInRegularObj = true;
          // Flat globalToSym[] for O(1) wiring in phase 5.
          uint32_t gid = firstGlobalId[r.fileIdx] +
                         (r.elfIdx - objFiles[r.fileIdx]->getFirstGlobal());
          globalToSym[gid] = sym;
        }

        // Compute the anchor from the resolved owner (sym->file). If the
        // symbol has no activated ref, mark it dead (~0ULL); such symbols
        // are filtered out of .symtab via !isUsedInRegularObj.
        uint64_t key = ~uint64_t(0);
        if (sym->file)
          for (const SymRef &r : kv.second)
            if (objFiles[r.fileIdx] == sym->file) {
              key = (uint64_t(r.fileIdx) << 32) | r.elfIdx;
              break;
            }
        anchors[idx] = key;
      }
    });
  }

  // Reorder symVector into wild-style input-file order: symbols grouped by
  // their owning file, in elfIdx order within each file. This makes the
  // symbol-table output independent of the per-build hash seed and of
  // --threads. Dead symbols end up at the tail.
  {
    llvm::TimeTraceScope ts("Order symbols (parallel sort)");
    auto perm = std::make_unique<uint32_t[]>(totalUnique);
    for (uint32_t i = 0; i < totalUnique; ++i)
      perm[i] = i;
    llvm::parallelSort(
        perm.get(), perm.get() + totalUnique,
        [&](uint32_t a, uint32_t b) { return anchors[a] < anchors[b]; });

    SmallVector<Symbol *, 0> ordered(totalUnique);
    auto oldToNew = std::make_unique<uint32_t[]>(totalUnique);
    for (uint32_t newIdx = 0; newIdx < totalUnique; ++newIdx) {
      uint32_t oldIdx = perm[newIdx];
      ordered[newIdx] = symVector[oldIdx];
      oldToNew[oldIdx] = newIdx;
    }
    symVector = std::move(ordered);

    parallelFor(0, (size_t)kNumBuckets, [&](size_t b) {
      for (auto &kv : shardMaps[b])
        kv.second = (int)oldToNew[kv.second];
    });
  }

  // Pre-populate COMDAT groups in parallel using sharded DenseMaps. Each
  // shard fits in L2 cache, avoiding the serial bottleneck of one large map.
  {
    llvm::TimeTraceScope ts("Pre-populate comdat groups (parallel)");
    auto shards =
        std::make_unique<DenseMap<CachedHashStringRef, const InputFile *>[]>(
            kNumBuckets);
    size_t shardCounts[kNumBuckets] = {};
    for (size_t i = 0; i < objFiles.size(); ++i) {
      if (!activated[i].load(std::memory_order_relaxed))
        continue;
      for (const auto &name : fileData[i].comdatNames)
        shardCounts[name.hash() % kNumBuckets]++;
    }
    parallelFor(0, (size_t)kNumBuckets, [&](size_t b) {
      shards[b].reserve(shardCounts[b]);
      for (size_t i = 0; i < objFiles.size(); ++i) {
        if (!activated[i].load(std::memory_order_relaxed))
          continue;
        for (const auto &name : fileData[i].comdatNames)
          if (name.hash() % kNumBuckets == b)
            shards[b].try_emplace(name, objFiles[i]);
      }
    });
    ctx.symtab->comdatGroupShards = std::move(shards);
    ctx.symtab->numComdatShards = kNumBuckets;
  }

  fileData.reset();

  ctx.parallelParse = true;
  for (size_t i = 0; i < objFiles.size(); ++i) {
    if (!activated[i].load(std::memory_order_relaxed))
      continue;
    objFiles[i]->lazy = false;
    ctx.objectFiles.push_back(objFiles[i]);
  }

  // ---- Phase 5: Init sections + wire globals + postParse (parallel) ----
  // Reuses ObjFile<ELFT>::postParse() for section binding, TLS/binding
  // validation, and duplicate detection. The loop only has to materialize
  // the symbols[] array from the pre-built globalToSym[].
  {
    llvm::TimeTraceScope ts("Init sections + wire + postParse (parallel)");
    Symbol **g2s = globalToSym.get();
    parallelFor(0, objFiles.size(), [&](size_t i) {
      if (!activated[i].load(std::memory_order_relaxed))
        return;
      auto *f = cast<ObjFile<ELFT>>(objFiles[i]);
      f->initSectionsAndLocalSyms(/*ignoreComdats=*/false);
      uint32_t firstGlobal = f->getFirstGlobal();
      uint32_t end = f->getNumSymbols();
      uint32_t base = firstGlobalId[i];
      for (uint32_t j = firstGlobal; j < end; ++j)
        if (Symbol *sym = g2s[base + (j - firstGlobal)])
          f->setSymbol(j, sym);
      f->postParse();
    });
  }

  ctx.symtab->installShardedSymbols(std::move(shardMaps), kNumBuckets,
                                    std::move(symVector));
}

// Helper that satisfies invokeELFT's `f<ELFT>(args)` expansion.
template <class ELFT> static void parseShared(Ctx &, SharedFile *sf) {
  sf->parse<ELFT>();
}

void elf::parallelParseFiles(
    Ctx &ctx, SmallVector<std::unique_ptr<InputFile>, 0> &files) {
  llvm::TimeTraceScope timeScope("Parse input files");

  // Partition into ELF object files vs everything else.
  SmallVector<ELFFileBase *, 0> objFiles;
  SmallVector<InputFile *, 0> nonObjFiles;
  for (auto &f : files) {
    if (f->kind() == InputFile::ObjKind)
      objFiles.push_back(cast<ELFFileBase>(f.get()));
    else
      nonObjFiles.push_back(f.get());
  }

  // Assign contiguous global-symbol ID ranges per object file.
  SmallVector<bool, 0> isArchive(objFiles.size());
  SmallVector<uint32_t, 0> firstGlobalId(objFiles.size());
  uint32_t totalGlobals = 0;
  for (size_t i = 0; i < objFiles.size(); ++i) {
    auto *f = objFiles[i];
    isArchive[i] = !f->archiveName.empty() && f->lazy;
    firstGlobalId[i] = totalGlobals;
    if (f->getNumSymbols() > f->getFirstGlobal())
      totalGlobals += f->getNumSymbols() - f->getFirstGlobal();
  }

  if (!objFiles.empty())
    invokeELFT(doParallelParse, ctx, objFiles, firstGlobalId, totalGlobals,
               isArchive);

  // Parse non-obj inputs serially.
  llvm::TimeTraceScope ts("Parse non-obj files");
  for (InputFile *f : nonObjFiles) {
    if (auto *sf = dyn_cast<SharedFile>(f)) {
      invokeELFT(parseShared, ctx, sf);
    } else if (auto *bf = dyn_cast<BitcodeFile>(f)) {
      if (f->lazy) {
        bf->parseLazy();
      } else {
        ctx.bitcodeFiles.push_back(bf);
        bf->parse();
      }
    } else if (auto *bf = dyn_cast<BinaryFile>(f)) {
      ctx.binaryFiles.push_back(bf);
      bf->parse();
    }
  }
}
