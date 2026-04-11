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
// Unlike the serial path, which parses each file independently and relies on
// SymbolTable::insert to serialize access to a single hash map, the parallel
// pipeline shards symbol resolution across 32 buckets keyed by a hash of the
// symbol name. Both ELF object files and non-lazy bitcode files feed the same
// bucket map in command-line order, so phase 4 naturally iterates refs in
// input order — the "first strong (or weak) def wins" rule is then just the
// natural outcome of walking each bucket entry's ref list front-to-back.
//
// Phases (all work is parallel except the final non-obj loop in the caller):
//   1. Read symbols + COMDAT group names from each file.
//   2. Populate per-bucket DenseMaps keyed by symbol name.
//   3. BFS-activate archive members that satisfy strong undef references.
//   4. Create Symbol objects and resolve them per bucket; the per-bucket maps
//      become the installed SymbolTable shard maps. Bitcode refs are recorded
//      in the bucket for ordering but not resolved here — BitcodeFile::parse
//      in the non-obj loop does the LTO-aware resolution. Comdat groups are
//      pre-populated into sharded DenseMaps here too.
//   5. For each activated obj file: initSectionsAndLocalSyms(), wire globals
//      from a flat globalToSym[] array, then run the standard postParse().
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
#include "llvm/IR/Comdat.h"
#include "llvm/LTO/LTO.h"
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

// Flag bits for PendingSym::flags and SymRef::flags. kBitcode marks refs read
// from an lto::InputFile (its elfIdx indexes the IR symbol table); otherwise
// elfIdx indexes the ELF .symtab.
constexpr uint8_t kIsDef = 1;
constexpr uint8_t kIsWeak = 2;
constexpr uint8_t kBitcode = 4;

struct PendingSym {
  CachedHashStringRef name;
  uint32_t elfIdx;
  uint16_t fileIdx; // index into allFiles
  uint8_t flags;
  bool isStrongUndef() const { return (flags & (kIsDef | kIsWeak)) == 0; }
};

struct FileData {
  // Symbol refs bucketed by hash(name) % kNumBuckets.
  SmallVector<PendingSym, 0> buckets[kNumBuckets];
  // COMDAT group signature names, bucketed the same way.
  SmallVector<CachedHashStringRef, 0> comdatBuckets[kNumBuckets];
};

struct SymRef {
  uint16_t fileIdx; // index into allFiles
  uint32_t elfIdx;
  uint8_t flags; // kIsDef | kIsWeak | kBitcode
};
} // namespace

static void addComdatName(CachedHashStringRef name, FileData &out) {
  out.comdatBuckets[name.hash() % kNumBuckets].push_back(name);
}

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
      addComdatName(CachedHashStringRef(StringRef(strData + nameOff)), out);
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
      flags |= kIsDef;
      if (eSyms[i].getBinding() == STB_WEAK)
        flags |= kIsWeak;
      if (LLVM_UNLIKELY(eSyms[i].st_shndx == SHN_COMMON))
        file->hasCommonSyms = true;
    } else if (eSyms[i].getBinding() == STB_WEAK) {
      flags |= kIsWeak;
    }
    out.buckets[key.hash() % kNumBuckets].push_back(
        {key, (uint32_t)i, fileIdx, flags});
  }
}

// Collect IR symbol references from a non-lazy BitcodeFile. Assumes names have
// been pre-saved via ctx.uniqueSaver so they are NUL-terminated in persistent
// memory. Marked with kBitcode so phase 4 defers actual resolution to
// BitcodeFile::parse() but can still use the ordering to preserve
// first-weak-wins across bc+obj mixes and so phase 3 BFS can satisfy obj→bc
// strong undef refs.
static void readBcSymbols(BitcodeFile *bf, uint16_t fileIdx, FileData &out) {
  // Comdat groups declared by the bitcode file.
  for (auto [name, kind] : bf->obj->getComdatTable())
    if (kind != llvm::Comdat::NoDeduplicate)
      addComdatName(CachedHashStringRef(name), out);

  uint32_t irIdx = 0;
  for (const auto &irSym : bf->obj->symbols()) {
    StringRef name = irSym.getName();
    if (name.empty()) {
      ++irIdx;
      continue;
    }
    // IR symbols don't use @@version mangling, so stem == name.
    CachedHashStringRef key(name);
    uint8_t flags = kBitcode;
    if (!irSym.isUndefined())
      flags |= kIsDef;
    if (irSym.isWeak())
      flags |= kIsWeak;
    out.buckets[key.hash() % kNumBuckets].push_back(
        {key, irIdx, fileIdx, flags});
    ++irIdx;
  }
}

template <class ELFT>
static void doParallelParse(Ctx &ctx, ArrayRef<InputFile *> allFiles,
                            ArrayRef<uint32_t> firstGlobalId,
                            uint32_t totalGlobals,
                            const DenseSet<StringRef> &tracedNames) {
  const size_t N = allFiles.size();

  // ---- Phase 1: Read symbols + comdat names (parallel per file) ----
  auto fileData = std::make_unique<FileData[]>(N);
  {
    llvm::TimeTraceScope ts("Read symbols (parallel)");
    parallelFor(0, N, [&](size_t i) {
      InputFile *f = allFiles[i];
      if (f->kind() == InputFile::ObjKind)
        readFileSymbols<ELFT>(cast<ELFFileBase>(f), (uint16_t)i, fileData[i]);
      else if (auto *bf = dyn_cast<BitcodeFile>(f))
        if (!bf->lazy)
          readBcSymbols(bf, (uint16_t)i, fileData[i]);
    });
  }

  // ---- Phase 2: Populate per-bucket DenseMaps ----
  DenseMap<CachedHashStringRef, SmallVector<SymRef, 1>> buckets[kNumBuckets];
  {
    llvm::TimeTraceScope ts("Populate symbol DB (parallel)");
    parallelFor(0, (size_t)kNumBuckets, [&](size_t b) {
      size_t n = 0;
      for (size_t i = 0; i < N; ++i)
        n += fileData[i].buckets[b].size();
      buckets[b].reserve(n);
      for (size_t i = 0; i < N; ++i)
        for (const auto &ps : fileData[i].buckets[b])
          buckets[b][ps.name].push_back({ps.fileIdx, ps.elfIdx, ps.flags});
    });
  }

  // ---- Phase 3: BFS archive activation ----
  // activated[i] starts true for non-lazy files and false for archive members
  // (obj or bc, including --start-lib groups).
  auto activated = std::make_unique<std::atomic<bool>[]>(N);
  for (size_t i = 0; i < N; ++i)
    activated[i].store(!allFiles[i]->lazy, std::memory_order_relaxed);
  {
    llvm::TimeTraceScope ts("Resolve symbols + activate archives");
    const size_t numThreads = parallel::getThreadCount();
    SmallVector<uint16_t, 0> cur;
    for (size_t i = 0; i < N; ++i)
      if (!allFiles[i]->lazy)
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
            // Already satisfied by some active def? Scan once: we both check
            // for an active def and pick the first inactive def as a fallback.
            const SymRef *pick = nullptr;
            bool satisfied = false;
            for (const SymRef &r : it->second) {
              if (!(r.flags & kIsDef))
                continue;
              if (activated[r.fileIdx].load(std::memory_order_relaxed)) {
                satisfied = true;
                break;
              }
              if (!pick)
                pick = &r;
            }
            if (satisfied || !pick)
              continue;
            if (!activated[pick->fileIdx].exchange(true,
                                                   std::memory_order_relaxed))
              queues[tid].push_back(pick->fileIdx);
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
  const uint32_t totalUnique = shardOffsets[kNumBuckets];

  SmallVector<Symbol *, 0> symVector(totalUnique);
  auto shardMaps =
      std::make_unique<DenseMap<CachedHashStringRef, int>[]>(kNumBuckets);
  // Stores a symVector index per obj-global slot (or ~0u for unused). Indices
  // (rather than Symbol*) so they survive the storage reorder below.
  auto globalToSym = std::make_unique<uint32_t[]>(totalGlobals);
  std::fill_n(globalToSym.get(), totalGlobals, ~uint32_t(0));
  // Batch-allocate all SymbolUnions contiguously for cache-friendly access.
  SymbolUnion *symStorage =
      getSpecificAllocSingleton<SymbolUnion>().Allocate(totalUnique);
  memset(symStorage, 0, sizeof(SymbolUnion) * totalUnique);
  // Per-symbol anchor key for the wild-style sort below. High bits encode a
  // tier (obj-owned / bc-owned / pure-undef), low bits encode the anchor ref's
  // (fileIdx, elfIdx) so the sort is deterministic and hash-seed independent.
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
        // Recover the full name (with @@version) via strlen on the stored
        // pointer. Obj names live in .strtab (NUL-terminated); bitcode names
        // were pre-saved via ctx.uniqueSaver (also NUL-terminated).
        StringRef fullName(kv.first.val().data());
        sym->setName(fullName);
        sym->partition = 1;
        sym->versionId = VER_NDX_GLOBAL;
        if (fullName.find('@') != StringRef::npos)
          sym->hasVersionSuffix = true;
        if (!tracedNames.empty() && tracedNames.count(kv.first.val()))
          sym->traced = true;
        symVector[idx] = sym;

        // Walk refs in command-line order. Bitcode refs are not resolved here
        // — BitcodeFile::parse() in the outer non-obj loop does the LTO-aware
        // resolution. But we track "has a bitcode weak def been seen earlier
        // in the command line?" so that a later obj weak def is suppressed,
        // preserving first-weak-wins across bc+obj (lto/weak.ll). We also
        // remember the first inactive (lazy) obj def so phase-4 can fall
        // back to LazySymbol below without a second walk.
        bool seenBcWeakDefBefore = false;
        const SymRef *lazyDef = nullptr;
        for (const SymRef &r : kv.second) {
          if (r.flags & kBitcode) {
            // Only count bc weak defs whose file is (or will be) active.
            if ((r.flags & (kIsDef | kIsWeak)) == (kIsDef | kIsWeak) &&
                activated[r.fileIdx].load(std::memory_order_relaxed))
              seenBcWeakDefBefore = true;
            continue;
          }
          auto *of = cast<ELFFileBase>(allFiles[r.fileIdx]);
          if (!activated[r.fileIdx].load(std::memory_order_relaxed)) {
            if ((r.flags & kIsDef) && !lazyDef)
              lazyDef = &r;
            continue;
          }
          // Wire globalToSym[gid] for every activated obj ref so phase 5 can
          // setSymbol() safely even if we skip resolving this def below.
          uint32_t gid =
              firstGlobalId[r.fileIdx] + (r.elfIdx - of->getFirstGlobal());
          globalToSym[gid] = (uint32_t)idx;
          auto eSym = of->template getELFSyms<ELFT>()[r.elfIdx];
          // Mark used-in-regular-obj for every activated obj ref, including
          // weak defs we will suppress below. LTO needs this flag on weak bc
          // syms that are also defined in a regular obj; otherwise it would
          // set ltoCanOmit and drop the bc def, leaving the obj ref
          // unresolved (lto/devirt_validate_vtable_typeinfos).
          sym->isUsedInRegularObj = true;
          // Suppress obj weak def that comes after a bitcode weak def.
          if ((r.flags & kIsDef) && (r.flags & kIsWeak) && seenBcWeakDefBefore)
            continue;
          if (!(r.flags & kIsDef)) {
            sym->resolve(ctx, Undefined(of, StringRef(), eSym.getBinding(),
                                        eSym.st_other, eSym.getType()));
            sym->referenced = true;
          } else if (LLVM_UNLIKELY(eSym.st_shndx == SHN_COMMON)) {
            sym->resolve(ctx,
                         CommonSymbol(ctx, of, StringRef(), eSym.getBinding(),
                                      eSym.st_other, eSym.getType(),
                                      eSym.st_value, eSym.st_size));
          } else {
            sym->resolve(ctx, Defined(ctx, of, StringRef(), eSym.getBinding(),
                                      eSym.st_other, eSym.getType(),
                                      eSym.st_value, eSym.st_size, nullptr));
          }
        }

        // If nothing active claimed this symbol but a lazy archive obj defines
        // it, expose a Lazy symbol. A later Undefined reference then triggers
        // extract() on the owning obj file. Bitcode refs are not eligible:
        // lazy bc goes through parseLazy → LazySymbol in the non-obj loop.
        if (sym->isPlaceholder() && lazyDef)
          sym->resolve(
              ctx, LazySymbol{*cast<ELFFileBase>(allFiles[lazyDef->fileIdx])});

        // Anchor key. Three tiers, selected by the two high bits:
        //  00: obj-owned, anchored by the obj ref that matches sym->file.
        //  01: bc-owned (or no obj owner but has a def), anchored by the
        //      first def ref seen.
        //  10: pure undef or empty, anchored by the first ref.
        // This keeps the sort deterministic and makes obj-defs → bc-defs →
        // undefs the natural order, matching the serial path's symVector
        // layout (lto/aarch64-pac-got-func depends on this for GOT order).
        const SymRef *pick = nullptr;
        uint64_t tier = 0;
        if (sym->file) {
          for (const SymRef &r : kv.second)
            if (!(r.flags & kBitcode) && allFiles[r.fileIdx] == sym->file) {
              pick = &r;
              break;
            }
        }
        if (!pick) {
          tier = 1;
          for (const SymRef &r : kv.second)
            if (r.flags & kIsDef) {
              pick = &r;
              break;
            }
        }
        if (!pick) {
          tier = 2;
          if (!kv.second.empty())
            pick = &kv.second.front();
        }
        anchors[idx] =
            (tier << 62) |
            (pick ? (uint64_t(pick->fileIdx) << 32) | pick->elfIdx : 0);
      }
    });
  }

  // Reorder symVector into wild-style input-file order. Independent of the
  // per-build hash seed and of --threads. Also reorder the underlying
  // SymbolUnion storage so later passes (markLive, demoteSymbols, scan
  // relocations, writeTo) that iterate symVector touch Symbol bytes in
  // linear memory order — matching the cache behavior of the serial
  // bump-allocated path.
  {
    llvm::TimeTraceScope ts("Order symbols (parallel sort)");
    auto perm = std::make_unique<uint32_t[]>(totalUnique);
    for (uint32_t i = 0; i < totalUnique; ++i)
      perm[i] = i;
    llvm::parallelSort(
        perm.get(), perm.get() + totalUnique,
        [&](uint32_t a, uint32_t b) { return anchors[a] < anchors[b]; });

    SymbolUnion *newStorage =
        getSpecificAllocSingleton<SymbolUnion>().Allocate(totalUnique);
    auto oldToNew = std::make_unique<uint32_t[]>(totalUnique);
    parallelFor(0, (size_t)totalUnique, [&](size_t newIdx) {
      uint32_t oldIdx = perm[newIdx];
      memcpy(&newStorage[newIdx], &symStorage[oldIdx], sizeof(SymbolUnion));
      symVector[newIdx] = reinterpret_cast<Symbol *>(&newStorage[newIdx]);
      oldToNew[oldIdx] = newIdx;
    });

    parallelFor(0, (size_t)kNumBuckets, [&](size_t b) {
      for (auto &kv : shardMaps[b])
        kv.second = (int)oldToNew[kv.second];
    });
    parallelFor(0, (size_t)totalGlobals, [&](size_t i) {
      uint32_t oldIdx = globalToSym[i];
      if (oldIdx != ~uint32_t(0))
        globalToSym[i] = oldToNew[oldIdx];
    });
  }

  // Pre-populate COMDAT groups using sharded DenseMaps. Each shard
  // independently walks files in command-line order so the first-file-wins
  // rule holds, and parallelism is across the 32 shards. Obj comdat names
  // were cached by readFileSymbols; bitcode comdat names were cached by
  // readBcSymbols. Both are already bucketed by hash(name) % kNumBuckets so
  // shard b only visits the comdats that belong to it.
  {
    llvm::TimeTraceScope ts("Pre-populate comdat groups");
    auto shards =
        std::make_unique<DenseMap<CachedHashStringRef, const InputFile *>[]>(
            kNumBuckets);
    parallelFor(0, (size_t)kNumBuckets, [&](size_t b) {
      auto &shard = shards[b];
      for (size_t i = 0; i < N; ++i) {
        InputFile *f = allFiles[i];
        if (f->kind() == InputFile::ObjKind &&
            !activated[i].load(std::memory_order_relaxed))
          continue;
        for (CachedHashStringRef name : fileData[i].comdatBuckets[b])
          shard.try_emplace(name, f);
      }
    });
    ctx.symtab->comdatGroupShards = std::move(shards);
    ctx.symtab->numComdatShards = kNumBuckets;
  }

  ctx.parallelParse = true;
  for (size_t i = 0; i < N; ++i) {
    if (!activated[i].load(std::memory_order_relaxed))
      continue;
    InputFile *f = allFiles[i];
    f->lazy = false;
    if (f->kind() == InputFile::ObjKind)
      ctx.objectFiles.push_back(cast<ELFFileBase>(f));
  }

  // ---- Phase 5: Init sections + wire globals + postParse (parallel) ----
  // Reuses ObjFile<ELFT>::postParse() for section binding, TLS/binding
  // validation, and duplicate detection. Bitcode files are handled by the
  // non-obj loop in the caller.
  {
    llvm::TimeTraceScope ts("Init sections + wire + postParse (parallel)");
    const uint32_t *g2s = globalToSym.get();
    ArrayRef<Symbol *> syms = symVector;
    parallelFor(0, N, [&](size_t i) {
      InputFile *f = allFiles[i];
      if (f->kind() != InputFile::ObjKind ||
          !activated[i].load(std::memory_order_relaxed))
        return;
      auto *obj = cast<ObjFile<ELFT>>(f);
      obj->initSectionsAndLocalSyms(/*ignoreComdats=*/false);
      uint32_t firstGlobal = obj->getFirstGlobal();
      uint32_t end = obj->getNumSymbols();
      uint32_t base = firstGlobalId[i];
      for (uint32_t j = firstGlobal; j < end; ++j) {
        uint32_t idx = g2s[base + (j - firstGlobal)];
        if (idx != ~uint32_t(0))
          obj->setSymbol(j, syms[idx]);
      }
      obj->postParse();
    });
  }

  ctx.symtab->installShardedSymbols(std::move(shardMaps), kNumBuckets,
                                    std::move(symVector));
  ctx.parallelParseNumObjs = ctx.objectFiles.size();
}

// Helper that satisfies invokeELFT's `f<ELFT>(args)` expansion.
template <class ELFT> static void parseShared(Ctx &, SharedFile *sf) {
  sf->parse<ELFT>();
}

void elf::parallelParseFiles(
    Ctx &ctx, SmallVector<std::unique_ptr<InputFile>, 0> &files) {
  llvm::TimeTraceScope timeScope("Parse input files");

  // Capture the set of names the driver has already marked with --trace-symbol
  // (see LinkerDriver::link). installShardedSymbols() replaces the Symbol*
  // objects, so the new syms need their `traced` flag set in phase 4 BEFORE
  // any resolve() fires, otherwise the usual trace messages are lost.
  DenseSet<StringRef> tracedNames;
  for (Symbol *s : ctx.symtab->getSymbols())
    if (s->traced) {
      // Phase 4 keys traced lookup by the bucket key (stem, without @@ver).
      StringRef stem = s->getName();
      size_t pos = stem.find('@');
      if (pos != StringRef::npos && pos + 1 < stem.size() &&
          stem[pos + 1] == '@')
        stem = stem.take_front(pos);
      tracedNames.insert(stem);
    }

  // Partition into compatible input files. Non-obj files are deferred to a
  // serial post-phase loop below.
  SmallVector<InputFile *, 0> allFiles;
  SmallVector<InputFile *, 0> nonObjFiles;
  allFiles.reserve(files.size());
  for (auto &f : files) {
    if (!isCompatible(ctx, f.get()))
      continue;
    allFiles.push_back(f.get());
    if (f->kind() != InputFile::ObjKind)
      nonObjFiles.push_back(f.get());
  }

  // ELF obj names live in each file's .strtab which is NUL-terminated, so
  // CachedHashStringRef(data()) lookups can recover the full name via strlen.
  // LLVM IR symbol names are NOT NUL-terminated. Serially save each non-lazy
  // bitcode file's symbol names through the (thread-unsafe) unique string
  // saver now, so readBcSymbols (which runs in parallel) can use IR names
  // safely and so phase 4's name-recovery via strlen stays correct. Mirrors
  // what createBitcodeSymbol() does in the serial path.
  for (InputFile *f : allFiles) {
    auto *bf = dyn_cast<BitcodeFile>(f);
    if (!bf || bf->lazy)
      continue;
    for (const auto &irSym : bf->obj->symbols())
      const_cast<lto::InputFile::Symbol &>(irSym).Name =
          ctx.uniqueSaver.save(irSym.getName());
  }

  // Assign contiguous global-symbol ID ranges per obj file. Non-obj positions
  // inherit the previous value so every allFiles index maps to a valid base
  // (non-obj entries contribute 0 globals).
  SmallVector<uint32_t, 0> firstGlobalId(allFiles.size());
  uint32_t totalGlobals = 0;
  for (size_t i = 0; i < allFiles.size(); ++i) {
    firstGlobalId[i] = totalGlobals;
    if (allFiles[i]->kind() == InputFile::ObjKind) {
      auto *o = cast<ELFFileBase>(allFiles[i]);
      if (o->getNumSymbols() > o->getFirstGlobal())
        totalGlobals += o->getNumSymbols() - o->getFirstGlobal();
    }
  }

  if (!allFiles.empty())
    invokeELFT(doParallelParse, ctx, allFiles, firstGlobalId, totalGlobals,
               tracedNames);

  // Flush nonPrevailingSyms now so that symbols in discarded COMDAT sections
  // are demoted to Undefined BEFORE BitcodeFile::parse() runs below. Otherwise
  // a later bitcode file that owns the same COMDAT would see the obj file's
  // stale Defined and BitcodeFile::postParse() would wrongly report a
  // duplicate. The serial driver path does this later in Driver.cpp, which
  // is fine there because bitcode files are parsed before objects; the
  // parallel path parses bitcode last.
  for (auto &it : ctx.nonPrevailingSyms) {
    Symbol &sym = *it.first;
    Undefined(sym.file, sym.getName(), sym.binding, sym.stOther, sym.type,
              it.second)
        .overwrite(sym);
    cast<Undefined>(sym).nonPrevailing = true;
  }
  ctx.nonPrevailingSyms.clear();

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
