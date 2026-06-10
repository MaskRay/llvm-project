//===- ParallelParse.cpp - Parallel input file parsing --------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Parallel file parsing pipeline for ELF object files. Symbol resolution is
// sharded across 32 buckets keyed by a hash of the symbol name; obj, bitcode
// and shared files feed each bucket in command-line order, so "first def
// wins" falls out of walking each name's ref list front-to-back. Phases:
//   1. Read symbols + COMDAT group names from each file (including lazy
//      archive members, obj and bitcode alike).
//   2. Populate per-bucket databases keyed by symbol name.
//   3. Positional activation: replay serial archive-extraction semantics
//      using file positions and undef/lazy pairing times.
//   4. Create Symbol objects and resolve them per bucket; the bucket maps
//      become the installed SymbolTable shard maps. Bitcode and shared refs
//      are ordered here but resolved by BitcodeFile::parse/SharedFile::parse
//      in the serial non-obj loop.
//   5. Wire globals from a flat globalToSym[] array.
// Section initialization, local symbols, and postParse run later through the
// driver's usual passes, exactly as in a serial link.
//
//===----------------------------------------------------------------------===//

#include "Config.h"
#include "InputFiles.h"
#include "InputSection.h"
#include "SymbolTable.h"
#include "Symbols.h"
#include "Target.h"
#include "lld/Common/Memory.h"
#include "llvm/ADT/BitVector.h"
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

// Flag bits for PendingSym::flags and RefNode::flags. kBitcode marks refs
// read from an lto::InputFile (its elfIdx indexes the IR symbol table);
// otherwise elfIdx indexes the ELF .symtab.
constexpr uint8_t kIsDef = 1;
constexpr uint8_t kIsWeak = 2;
constexpr uint8_t kBitcode = 4;
constexpr uint8_t kShared = 8;
constexpr uint8_t kCommon = 16;

// Trivial POD so FileData::syms can be resized without construction; the
// (ptr, len, hash) triple reconstitutes the bucket key without rehashing.
struct PendingSym {
  const char *namePtr;
  uint32_t nameLen;
  uint32_t hash;
  uint32_t nameId; // per-bucket unique-name index, assigned in phase 2
  uint32_t elfIdx;
  uint16_t fileIdx; // index into allFiles
  uint8_t flags;
  CachedHashStringRef name() const {
    return CachedHashStringRef(StringRef(namePtr, nameLen), hash);
  }
  bool isStrongUndef() const { return (flags & (kIsDef | kIsWeak)) == 0; }
};

// Trivial POD mirror of CachedHashStringRef, for the same reason.
struct ComdatName {
  const char *ptr;
  uint32_t len;
  uint32_t hash;
  CachedHashStringRef name() const {
    return CachedHashStringRef(StringRef(ptr, len), hash);
  }
};

// Per-file phase-1 output: symbol refs and comdat names in one flat buffer
// each, stably counting-sorted by hash % kNumBuckets, so each file costs two
// heap allocations and per-file symtab order is preserved within buckets.
struct FileData {
  SmallVector<PendingSym, 0> syms;
  SmallVector<ComdatName, 0> comdats;
  uint32_t symOff[kNumBuckets + 1] = {};
  uint32_t comdatOff[kNumBuckets + 1] = {};

  MutableArrayRef<PendingSym> bucket(unsigned b) {
    return MutableArrayRef<PendingSym>(syms).slice(symOff[b],
                                                   symOff[b + 1] - symOff[b]);
  }
  ArrayRef<ComdatName> comdatBucket(unsigned b) const {
    return ArrayRef<ComdatName>(comdats).slice(comdatOff[b],
                                               comdatOff[b + 1] - comdatOff[b]);
  }
};

// Stable counting sort of v by hash % kNumBuckets, filling off.
template <class T>
static void bucketSort(SmallVector<T, 0> &v, uint32_t (&off)[kNumBuckets + 1]) {
  uint32_t cnt[kNumBuckets];
  memset(cnt, 0, sizeof(cnt));
  for (const T &t : v)
    ++cnt[t.hash % kNumBuckets];
  off[0] = 0;
  for (unsigned b = 0; b < kNumBuckets; ++b)
    off[b + 1] = off[b] + cnt[b];
  SmallVector<T, 0> sorted;
  sorted.resize_for_overwrite(v.size());
  uint32_t pos[kNumBuckets];
  memcpy(pos, off, sizeof(pos));
  for (const T &t : v)
    sorted[pos[t.hash % kNumBuckets]++] = t;
  v = std::move(sorted);
}

static void finalizeFileData(FileData &fd) {
  bucketSort(fd.syms, fd.symOff);
  bucketSort(fd.comdats, fd.comdatOff);
}

// One node in a per-bucket flat ref array. Nodes for the same name form a
// singly-linked chain in command-line order (kNoRef terminates); a second
// defs-only chain lets the phase-3 BFS skip undef refs entirely.
struct RefNode {
  uint32_t elfIdx;
  uint32_t next;
  uint32_t nextDef;
  uint16_t fileIdx; // index into allFiles
  uint8_t flags;    // kIsDef | kIsWeak | kBitcode
};

constexpr uint32_t kNoRef = ~uint32_t(0);

// Per-unique-name chain heads/tails. Indexed by the nameId written back into
// PendingSym during phase 2, so phase 3 needs no hash lookups.
struct NameEnt {
  uint32_t head = kNoRef, tail = kNoRef;
  uint32_t defHead = kNoRef, defTail = kNoRef;
};

// The map value is the index into names (the per-bucket nameId). Phase 4
// turns the maps into the installed SymbolTable shard maps by rewriting the
// values to symVector indices.
struct Bucket {
  DenseMap<CachedHashStringRef, int> map;
  SmallVector<NameEnt, 0> names;
  SmallVector<RefNode, 0> refs;
};
} // namespace

static void addComdatName(CachedHashStringRef name, FileData &out) {
  out.comdats.push_back(
      {name.val().data(), (uint32_t)name.val().size(), name.hash()});
}

template <class ELFT>
static void readFileSymbols(ELFFileBase *file, uint16_t fileIdx,
                            FileData &out) {
  auto eSyms = file->getELFSyms<ELFT>();
  StringRef strtab = file->getStringTable();
  const char *strData = strtab.data();
  size_t strSize = strtab.size();

  // Collect COMDAT group signature names, and record ARM attributes and
  // dependent-library sections for processDeferredSections().
  cast<ObjFile<ELFT>>(file)->scanEarlySections([&](StringRef signature) {
    addComdatName(CachedHashStringRef(signature), out);
  });

  // Collect global symbol references, bucketed by name hash.
  for (size_t i = file->getFirstGlobal(), e = eSyms.size(); i < e; ++i) {
    uint32_t nameOff = eSyms[i].st_name;
    StringRef name = LLVM_UNLIKELY(nameOff >= strSize)
                         ? StringRef()
                         : StringRef(strData + nameOff);
    StringRef stem = getVersionedStem(name);
    uint32_t hash = CachedHashStringRef(stem).hash();
    uint8_t flags = 0;
    if (eSyms[i].st_shndx != SHN_UNDEF) {
      flags |= kIsDef;
      if (eSyms[i].getBinding() == STB_WEAK)
        flags |= kIsWeak;
      if (LLVM_UNLIKELY(eSyms[i].st_shndx == SHN_COMMON)) {
        file->hasCommonSyms = true;
        flags |= kCommon;
      }
    } else if (eSyms[i].getBinding() == STB_WEAK) {
      flags |= kIsWeak;
    }
    out.syms.push_back({stem.data(), (uint32_t)stem.size(), hash, 0,
                        (uint32_t)i, fileIdx, flags});
  }
  finalizeFileData(out);
}

// Collect IR symbol references from a BitcodeFile. Names must have been
// pre-saved via ctx.uniqueSaver (NUL-terminated, persistent). kBitcode defers
// resolution to BitcodeFile::parse()/parseLazy() while preserving ref
// ordering; lazy members participate in archive activation like lazy objs.
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
    uint32_t hash = CachedHashStringRef(name).hash();
    uint8_t flags = kBitcode;
    if (!irSym.isUndefined())
      flags |= kIsDef;
    if (irSym.isWeak())
      flags |= kIsWeak;
    if (irSym.isCommon())
      flags |= kCommon;
    out.syms.push_back(
        {name.data(), (uint32_t)name.size(), hash, 0, irIdx, fileIdx, flags});
    ++irIdx;
  }
  finalizeFileData(out);
}

// Collect dynamic symbol names from a SharedFile so that phase 3 can apply
// the serial archive-vs-DSO rules (a DSO definition that precedes the
// undef/lazy pairing suppresses extraction; DSO strong undefs extract) and
// so the serial-order anchors account for DSO insertions. kShared refs are
// not resolved in phase 4; SharedFile::parse() does that later.
template <class ELFT>
static void readSharedSymbols(ELFFileBase *file, uint16_t fileIdx,
                              FileData &out) {
  auto eSyms = file->getELFSyms<ELFT>();
  StringRef strtab = file->getStringTable();
  const char *strData = strtab.data();
  size_t strSize = strtab.size();
  for (size_t i = file->getFirstGlobal(), e = eSyms.size(); i < e; ++i) {
    uint32_t nameOff = eSyms[i].st_name;
    if (LLVM_UNLIKELY(nameOff >= strSize))
      continue;
    StringRef name(strData + nameOff);
    uint8_t flags = kShared;
    if (eSyms[i].st_shndx != SHN_UNDEF)
      flags |= kIsDef;
    if (eSyms[i].getBinding() == STB_WEAK)
      flags |= kIsWeak;
    out.syms.push_back({name.data(), (uint32_t)name.size(),
                        CachedHashStringRef(name).hash(), 0, (uint32_t)i,
                        fileIdx, flags});
  }
  finalizeFileData(out);
}

template <class ELFT>
static void
doParallelParse(Ctx &ctx, ArrayRef<InputFile *> allFiles,
                ArrayRef<uint32_t> firstGlobalId, uint32_t totalGlobals,
                const DenseSet<StringRef> &tracedNames,
                const DenseMap<CachedHashStringRef, uint32_t> &preOrder,
                SmallVector<InputFile *, 0> &nonObjFiles) {
  const size_t N = allFiles.size();

  // ---- Phase 1: Read symbols + comdat names (parallel per file) ----
  auto fileData = std::make_unique<FileData[]>(N);
  {
    llvm::TimeTraceScope ts("Read symbols (parallel)");
    parallelFor(0, N, [&](size_t i) {
      InputFile *f = allFiles[i];
      if (f->kind() == InputFile::ObjKind)
        readFileSymbols<ELFT>(cast<ELFFileBase>(f), (uint16_t)i, fileData[i]);
      else if (f->kind() == InputFile::SharedKind)
        readSharedSymbols<ELFT>(cast<ELFFileBase>(f), (uint16_t)i, fileData[i]);
      else if (auto *bf = dyn_cast<BitcodeFile>(f))
        readBcSymbols(bf, (uint16_t)i, fileData[i]);
    });
  }

  // ---- Phase 2: Populate per-bucket symbol DB ----
  auto db = std::make_unique<Bucket[]>(kNumBuckets);
  {
    llvm::TimeTraceScope ts("Populate symbol DB (parallel)");
    parallelFor(0, (size_t)kNumBuckets, [&](size_t b) {
      size_t n = 0;
      for (size_t i = 0; i < N; ++i)
        n += fileData[i].bucket(b).size();
      Bucket &bucket = db[b];
      // n counts refs; unique names are typically several times fewer.
      bucket.map.reserve(n / 4);
      bucket.refs.reserve(n);
      for (size_t i = 0; i < N; ++i) {
        for (auto &ps : fileData[i].bucket(b)) {
          auto [it, inserted] =
              bucket.map.try_emplace(ps.name(), (int)bucket.names.size());
          uint32_t id = it->second;
          if (inserted)
            bucket.names.emplace_back();
          ps.nameId = id;
          NameEnt &ne = bucket.names[id];
          uint32_t idx = bucket.refs.size();
          bucket.refs.push_back(
              {ps.elfIdx, kNoRef, kNoRef, ps.fileIdx, ps.flags});
          if (ne.head == kNoRef)
            ne.head = idx;
          else
            bucket.refs[ne.tail].next = idx;
          ne.tail = idx;
          if (ps.flags & kIsDef) {
            if (ne.defHead == kNoRef)
              ne.defHead = idx;
            else
              bucket.refs[ne.defTail].nextDef = idx;
            ne.defTail = idx;
          }
        }
      }
    });
  }

  uint32_t shardOffsets[kNumBuckets + 1];
  shardOffsets[0] = 0;
  for (unsigned b = 0; b < kNumBuckets; ++b)
    shardOffsets[b + 1] = shardOffsets[b] + db[b].names.size();
  const uint32_t totalUnique = shardOffsets[kNumBuckets];

  // ---- Phase 3: positional archive activation ----
  // pos[i] is the position at which file i's symbols join resolution: i+1
  // for files live from the start (0 is reserved for pre-parseFiles -u
  // undefs), the serial extraction time T for activated lazy members, or
  // kNeverPos. Mimicking the serial rules: a strong undef at position P
  // pairs with the first inactive lazy definition L (command-line order) at
  // time T = max(P, L+1); the member is extracted unless a non-lazy
  // definition (regular or DSO) is available at a position <= T. Activated
  // members' strong undefs then act at position T. Positions make the
  // archive-vs-DSO interaction match serial semantics (shared-lazy.s) and
  // yield extraction times/causes for --why-extract and --warn-backrefs.
  constexpr uint32_t kNeverPos = UINT32_MAX;
  auto pos = std::make_unique<std::atomic<uint32_t>[]>(N);
  llvm::BitVector wasLazy(N);
  for (size_t i = 0; i < N; ++i) {
    wasLazy[i] = allFiles[i]->lazy;
    pos[i].store(allFiles[i]->lazy ? kNeverPos : i + 1,
                 std::memory_order_relaxed);
  }
  // Winning (minimal) extraction record per file, for diagnostics and
  // serial-order reconstruction.
  struct ExtractInfo {
    uint32_t t = UINT32_MAX;
    uint32_t trigFile = UINT32_MAX; // ~0u also means -u/internal
    uint32_t trigSym = UINT32_MAX;  // trigger's symtab/IR index
    uint32_t preIdx = UINT32_MAX;   // pre-reorder symVector index
  };
  SmallVector<ExtractInfo, 0> extractInfo(N);

  {
    llvm::TimeTraceScope ts("Resolve symbols + activate archives");
    const size_t numThreads = parallel::getThreadCount();
    struct ExtractCand {
      uint32_t member, t, trigFile, trigSym, preIdx;
    };
    auto queues = std::make_unique<SmallVector<ExtractCand, 0>[]>(numThreads);

    // commonTrig models --fortran-common: a COMMON definition searches
    // archives for a strong non-common definition, and is not satisfied by
    // DSO definitions or other commons.
    auto decide = [&](unsigned b, uint32_t nameId, uint32_t p,
                      uint32_t trigFile, uint32_t trigSym, bool commonTrig,
                      SmallVector<ExtractCand, 0> &q) {
      Bucket &bucket = db[b];
      const RefNode *lazyPick = nullptr;
      uint32_t minDef = kNeverPos;
      for (uint32_t ri = bucket.names[nameId].defHead; ri != kNoRef;
           ri = bucket.refs[ri].nextDef) {
        const RefNode &r = bucket.refs[ri];
        if (commonTrig && (r.flags & (kShared | kCommon | kIsWeak)))
          continue;
        uint32_t dp = pos[r.fileIdx].load(std::memory_order_relaxed);
        if (dp == kNeverPos) {
          if (!lazyPick)
            lazyPick = &r;
          continue;
        }
        if (dp <= p)
          return; // defined no later than the reference
        minDef = std::min(minDef, dp);
        if (lazyPick && minDef <= uint32_t(lazyPick->fileIdx) + 1)
          return; // defined before the undef/lazy pairing
      }
      if (!lazyPick)
        return;
      uint32_t t = std::max(p, uint32_t(lazyPick->fileIdx) + 1);
      if (minDef <= t)
        return; // defined before the undef/lazy pairing
      q.push_back(
          {lazyPick->fileIdx, t, trigFile, trigSym, shardOffsets[b] + nameId});
    };

    auto drain = [&](SmallVector<uint32_t, 0> &next) {
      for (size_t t = 0; t < numThreads; ++t) {
        for (const ExtractCand &c : queues[t]) {
          ExtractInfo &ei = extractInfo[c.member];
          if (std::tie(c.t, c.trigFile, c.trigSym, c.preIdx) <
              std::tie(ei.t, ei.trigFile, ei.trigSym, ei.preIdx)) {
            if (ei.t == UINT32_MAX)
              next.push_back(c.member);
            ei = {c.t, c.trigFile, c.trigSym, c.preIdx};
            pos[c.member].store(c.t, std::memory_order_relaxed);
          }
        }
        queues[t].clear();
      }
    };

    SmallVector<uint32_t, 0> cur;
    for (size_t i = 0; i < N; ++i)
      if (!allFiles[i]->lazy)
        cur.push_back(i);
    // Pre-parseFiles -u undefined symbols act at position 0.
    for (auto [ui, name] : llvm::enumerate(ctx.arg.undefined)) {
      CachedHashStringRef key(getVersionedStem(name));
      unsigned b = key.hash() % kNumBuckets;
      auto it = db[b].map.find(key);
      if (it != db[b].map.end())
        decide(b, it->second, 0, UINT32_MAX, ui, false, queues[0]);
    }
    drain(cur);

    while (!cur.empty()) {
      parallelFor(0, cur.size(), [&](size_t wi) {
        uint32_t fi = cur[wi];
        uint32_t p = pos[fi].load(std::memory_order_relaxed);
        for (unsigned b = 0; b < kNumBuckets; ++b)
          for (const auto &ps : fileData[fi].bucket(b)) {
            if (ps.isStrongUndef())
              decide(b, ps.nameId, p, fi, ps.elfIdx, false,
                     queues[parallel::getThreadIndex()]);
            else if (ctx.arg.fortranCommon &&
                     (ps.flags & (kIsDef | kCommon | kShared)) ==
                         (kIsDef | kCommon))
              decide(b, ps.nameId, p, fi, ps.elfIdx, true,
                     queues[parallel::getThreadIndex()]);
          }
      });
      cur.clear();
      drain(cur);
    }
  }
  auto activeAt = [&](size_t i) {
    return pos[i].load(std::memory_order_relaxed) != kNeverPos;
  };

  // Compares allFiles indices by serial parse order: a file joins at its
  // command-line position; an activated lazy member at its extraction time,
  // after its trigger, ordered by the trigger's symbol order (mimicking
  // serial recursive extraction). Inactive lazy members keep their
  // command-line position (where serial parseLazy runs).
  auto serialLess = [&](uint32_t a, uint32_t b) {
    auto key = [&](uint32_t i) {
      uint32_t p = pos[i].load(std::memory_order_relaxed);
      const ExtractInfo &ei = extractInfo[i];
      bool extracted = ei.t != UINT32_MAX;
      return std::make_tuple(p == kNeverPos ? i + 1 : p, extracted,
                             extracted ? ei.trigFile + 1 : 0,
                             extracted ? ei.trigSym : 0, i);
    };
    return key(a) < key(b);
  };

  // Reorder the caller's serial non-obj parse to match serial order (e.g.
  // this orders ctx.bitcodeFiles and LTO resolutions as serial recursive
  // extraction would).
  {
    SmallVector<uint32_t, 0> idxs;
    idxs.reserve(nonObjFiles.size());
    size_t j = 0;
    for (size_t i = 0; i < N && j < nonObjFiles.size(); ++i)
      if (allFiles[i] == nonObjFiles[j]) {
        idxs.push_back(i);
        ++j;
      }
    llvm::sort(idxs, serialLess);
    for (size_t k = 0; k < idxs.size(); ++k)
      nonObjFiles[k] = allFiles[idxs[k]];
  }

  // ---- Phase 4: Create Symbol objects + resolve + build shard maps ----

  SmallVector<Symbol *, 0> symVector;
  symVector.resize_for_overwrite(totalUnique);
  auto shardMaps =
      std::make_unique<DenseMap<CachedHashStringRef, int>[]>(kNumBuckets);
  // Stores a symVector index per obj-global slot (or ~0u for unused). Indices
  // (rather than Symbol*) so they survive the storage reorder below.
  std::unique_ptr<uint32_t[]> globalToSym(new uint32_t[totalGlobals]);
  memset(globalToSym.get(), 0xff, totalGlobals * sizeof(uint32_t));
  // Phase-4 scratch SymbolUnion storage, zeroed per bucket range inside the
  // parallelFor. The reorder below copies the Symbols into their final
  // bump-allocated home, so this buffer is freed at scope exit instead of
  // lingering in the allocator.
  std::unique_ptr<SymbolUnion[]> symStorageBuf(new SymbolUnion[totalUnique]);
  SymbolUnion *symStorage = symStorageBuf.get();
  // Per-symbol anchor key for the wild-style sort below. High bits encode a
  // tier (obj-owned / bc-owned / pure-undef), low bits encode the anchor ref's
  // (fileIdx, elfIdx) so the sort is deterministic and hash-seed independent.
  std::unique_ptr<uint64_t[]> anchors(new uint64_t[totalUnique]);

  {
    llvm::TimeTraceScope ts("Create symbols + resolve (parallel)");
    parallelFor(0, (size_t)kNumBuckets, [&](size_t b) {
      Bucket &bucket = db[b];
      memset(&symStorage[shardOffsets[b]], 0,
             (shardOffsets[b + 1] - shardOffsets[b]) * sizeof(SymbolUnion));
      for (auto &kv : bucket.map) {
        NameEnt &ne = bucket.names[kv.second];
        int idx = shardOffsets[b] + kv.second;

        auto *sym = reinterpret_cast<Symbol *>(&symStorage[idx]);
        // The bucket key is the stem; a "@@version" suffix, if any, follows
        // the NUL-terminated stem in place (obj names live in .strtab;
        // bitcode names were pre-saved via ctx.uniqueSaver).
        StringRef stem = kv.first.val();
        bool hasSuffix = stem.data()[stem.size()] != '\0';
        StringRef fullName = hasSuffix ? StringRef(stem.data()) : stem;
        sym->setName(fullName);
        sym->versionId = VER_NDX_GLOBAL;
        if (hasSuffix || stem.contains('@'))
          sym->hasVersionSuffix = true;
        if (!tracedNames.empty() && tracedNames.count(stem))
          sym->traced = true;
        symVector[idx] = sym;

        // Walk refs in command-line order. Bitcode refs are not resolved here
        // (BitcodeFile::parse() is), but an earlier active bc weak def must
        // suppress a later obj weak def to preserve first-weak-wins across
        // bc+obj mixes (lto/weak.ll). Also remember the first inactive lazy
        // def for the LazySymbol fallback below.
        bool seenBcWeakDefBefore = false;
        bool seenBcStrongDefBefore = false;
        bool seenActiveBcDef = false;
        const RefNode *lazyDef = nullptr;
        for (uint32_t ri = ne.head; ri != kNoRef; ri = bucket.refs[ri].next) {
          const RefNode &r = bucket.refs[ri];
          // Shared refs only participate in activation and ordering;
          // SharedFile::parse() resolves them later.
          if (r.flags & kShared)
            continue;
          if (r.flags & kBitcode) {
            if (r.flags & kIsDef) {
              if (activeAt(r.fileIdx)) {
                seenActiveBcDef = true;
                // COMMON defs suppress nothing: they merge with obj defs
                // commutatively.
                if (!(r.flags & kCommon)) {
                  if (r.flags & kIsWeak)
                    seenBcWeakDefBefore = true;
                  else
                    seenBcStrongDefBefore = true;
                }
              } else if (!lazyDef) {
                lazyDef = &r;
              }
            }
            continue;
          }
          auto *of = cast<ELFFileBase>(allFiles[r.fileIdx]);
          if (!activeAt(r.fileIdx)) {
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
          // Set for every activated obj ref, including suppressed weak defs;
          // otherwise LTO would set ltoCanOmit and drop a bc def that a
          // regular obj references (lto/devirt_validate_vtable_typeinfos).
          sym->isUsedInRegularObj = true;
          // An earlier bitcode def wins over this obj def (first-weak-wins
          // for weak; a strong bc def wins outright): leave the resolution to
          // BitcodeFile::parse(). Genuine duplicates are still diagnosed by
          // postParse. COMMON defs are exempt: they merge.
          if ((r.flags & (kIsDef | kCommon)) == kIsDef &&
              (seenBcStrongDefBefore ||
               ((r.flags & kIsWeak) && seenBcWeakDefBefore)))
            continue;
          if (!(r.flags & kIsDef)) {
            sym->resolve(ctx, Undefined(of, StringRef(), eSym.getBinding(),
                                        eSym.st_other, eSym.getType()));
            sym->referenced = true;
          } else if (LLVM_UNLIKELY(eSym.st_shndx == SHN_COMMON)) {
            if (eSym.st_value == 0 || eSym.st_value >= UINT32_MAX)
              Err(ctx) << of << ": common symbol '" << sym->getName()
                       << "' has invalid alignment: "
                       << uint64_t(eSym.st_value);
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

        // If nothing active claimed this symbol but the first inactive lazy
        // def is an archive obj, expose a LazySymbol so a later Undefined
        // reference can extract() the owning file. If the first lazy def is
        // bitcode, its parseLazy() installs the LazySymbol instead; if an
        // active bc file defines the symbol, its parse() will define it
        // (installing a LazySymbol would make later bc undefs spuriously
        // extract the obj member).
        if (sym->isPlaceholder() && lazyDef && !seenActiveBcDef &&
            !(lazyDef->flags & kBitcode))
          sym->resolve(
              ctx, LazySymbol{*cast<ELFFileBase>(allFiles[lazyDef->fileIdx])});

        // Anchor key for the deterministic sort below: mimic the serial
        // symbol table insertion order so that the output .symtab matches
        // the serial path. (Transitional; can be relaxed to any
        // deterministic order once serial parity is no longer needed.)
        // Serial inserts, per file in command-line order: all globals of a
        // non-lazy object file in symtab index order; definitions only for
        // lazy files; bitcode definitions before bitcode undefs. The
        // pre-parseFiles insertions in preOrder come first.
        auto preIt = preOrder.find(kv.first);
        if (preIt != preOrder.end()) {
          anchors[idx] = preIt->second;
        } else {
          const RefNode *pick = nullptr;
          for (uint32_t ri = ne.head; ri != kNoRef; ri = bucket.refs[ri].next) {
            const RefNode &r = bucket.refs[ri];
            // Lazy files insert only definitions.
            if (!(r.flags & kIsDef) && allFiles[r.fileIdx]->lazy)
              continue;
            pick = &r;
            break;
          }
          if (!pick)
            pick = &bucket.refs[ne.head];
          uint64_t bcUndef = (pick->flags & (kBitcode | kIsDef)) == kBitcode;
          anchors[idx] = ((uint64_t(pick->fileIdx) + 1) << 33) |
                         (bcUndef << 32) | pick->elfIdx;
        }
      }
    });
  }

  // Reorder symVector into input-file order, independent of the per-build
  // hash seed and of --threads. The SymbolUnion storage is reordered too so
  // later passes iterating symVector touch Symbol bytes in linear order.
  std::unique_ptr<uint32_t[]> oldToNew(new uint32_t[totalUnique]);
  {
    llvm::TimeTraceScope ts("Order symbols (parallel sort)");
    std::unique_ptr<uint32_t[]> perm(new uint32_t[totalUnique]);
    for (uint32_t i = 0; i < totalUnique; ++i)
      perm[i] = i;
    llvm::parallelSort(
        perm.get(), perm.get() + totalUnique,
        [&](uint32_t a, uint32_t b) { return anchors[a] < anchors[b]; });

    SymbolUnion *newStorage =
        getSpecificAllocSingleton<SymbolUnion>().Allocate(totalUnique);
    parallelFor(0, (size_t)totalUnique, [&](size_t newIdx) {
      uint32_t oldIdx = perm[newIdx];
      memcpy(&newStorage[newIdx], &symStorage[oldIdx], sizeof(SymbolUnion));
      symVector[newIdx] = reinterpret_cast<Symbol *>(&newStorage[newIdx]);
      oldToNew[oldIdx] = newIdx;
    });

    // Rewrite the bucket maps' nameId values to final symVector indices,
    // turning them into the SymbolTable shard maps installed below.
    parallelFor(0, (size_t)kNumBuckets, [&](size_t b) {
      for (auto &kv : db[b].map)
        kv.second = (int)oldToNew[shardOffsets[b] + kv.second];
      shardMaps[b] = std::move(db[b].map);
    });
    parallelFor(0, (size_t)totalGlobals, [&](size_t i) {
      uint32_t oldIdx = globalToSym[i];
      if (oldIdx != ~uint32_t(0))
        globalToSym[i] = oldToNew[oldIdx];
    });
  }

  // Record --why-extract and --warn-backrefs for pipeline activations in
  // extraction order, as the serial path would.
  if (!ctx.arg.whyExtract.empty() || ctx.arg.warnBackrefs) {
    SmallVector<uint32_t, 0> order;
    for (uint32_t m = 0; m < N; ++m)
      if (extractInfo[m].t != UINT32_MAX)
        order.push_back(m);
    llvm::sort(order, serialLess);
    for (uint32_t m : order) {
      const ExtractInfo &ei = extractInfo[m];
      Symbol &sym = *symVector[oldToNew[ei.preIdx]];
      InputFile *trig =
          ei.trigFile == UINT32_MAX ? ctx.internalFile : allFiles[ei.trigFile];
      if (!ctx.arg.whyExtract.empty())
        ctx.whyExtractRecords.emplace_back(toStr(ctx, trig), allFiles[m], sym);
      // A backward reference: the member's group precedes the trigger's.
      // Weak references are not reported, and a subsequent lazy definition
      // dismisses the warning (linking sandwich).
      if (ctx.arg.warnBackrefs && ei.trigFile != UINT32_MAX && !sym.isWeak() &&
          allFiles[m]->groupId < trig->groupId) {
        unsigned b =
            std::upper_bound(shardOffsets, shardOffsets + kNumBuckets + 1,
                             ei.preIdx) -
            shardOffsets - 1;
        uint32_t nameId = ei.preIdx - shardOffsets[b];
        bool sandwich = false;
        for (uint32_t ri = db[b].names[nameId].defHead; ri != kNoRef;
             ri = db[b].refs[ri].nextDef) {
          const RefNode &r = db[b].refs[ri];
          if (r.fileIdx != m && wasLazy[r.fileIdx]) {
            sandwich = true;
            break;
          }
        }
        if (!sandwich)
          ctx.backwardReferences.try_emplace(&sym,
                                             std::make_pair(trig, allFiles[m]));
      }
    }
  }

  // Pre-populate COMDAT groups. Each shard independently walks files in
  // command-line order so the first-file-wins rule holds; the phase-1 caches
  // are bucketed so shard b only visits its own comdat names.
  {
    llvm::TimeTraceScope ts("Pre-populate comdat groups");
    auto shards =
        std::make_unique<DenseMap<CachedHashStringRef, const InputFile *>[]>(
            kNumBuckets);
    parallelFor(0, (size_t)kNumBuckets, [&](size_t b) {
      auto &shard = shards[b];
      for (size_t i = 0; i < N; ++i) {
        if (!activeAt(i))
          continue;
        for (const ComdatName &name : fileData[i].comdatBucket(b))
          shard.try_emplace(name.name(), allFiles[i]);
      }
    });
    ctx.symtab->installComdatShards(std::move(shards), kNumBuckets);
  }

  // Commit activations in serial parse order: -t traces, ctx.objectFiles
  // (which determines output section order), and the deferred-section
  // epilogue below all match what serial extraction would produce.
  ctx.parallelParse = true;
  const size_t firstObj = ctx.objectFiles.size();
  {
    SmallVector<uint32_t, 0> active;
    for (size_t i = 0; i < N; ++i)
      if (activeAt(i))
        active.push_back(i);
    llvm::sort(active, serialLess);
    for (uint32_t i : active) {
      InputFile *f = allFiles[i];
      if (ctx.arg.trace)
        Msg(ctx) << f;
      f->lazy = false;
      if (f->kind() == InputFile::ObjKind)
        ctx.objectFiles.push_back(cast<ELFFileBase>(f));
    }
  }

  // ---- Phase 5: Wire global symbols (parallel) ----
  // Section initialization, local symbols, and postParse run later through
  // the driver's usual passes, exactly as in a serial link.
  {
    llvm::TimeTraceScope ts("Wire symbols (parallel)");
    const uint32_t *g2s = globalToSym.get();
    ArrayRef<Symbol *> syms = symVector;
    parallelFor(0, N, [&](size_t i) {
      InputFile *f = allFiles[i];
      if (f->kind() == InputFile::ObjKind && activeAt(i))
        cast<ELFFileBase>(f)->wireSymbols(g2s + firstGlobalId[i], syms);
    });
  }

  // Serial epilogue for order-dependent section handling deferred by
  // initializeSections (ARM attributes retention; dependent libraries, which
  // may append input files for the caller to parse). The objectFiles slice
  // pushed above is the activated obj files in command-line order.
  for (ELFFileBase *f : ArrayRef(ctx.objectFiles).drop_front(firstObj))
    cast<ObjFile<ELFT>>(f)->processDeferredSections();

  ctx.symtab->installShardedSymbols(std::move(shardMaps), kNumBuckets,
                                    std::move(symVector));

  // Free phase-local state on worker threads. The per-file FileData buffers
  // and per-bucket maps hold many heap allocations; destructing them during
  // the main thread's stack unwinding would serialize the frees.
  {
    llvm::TimeTraceScope ts("Free phase-local data (parallel)");
    parallelFor(0, N + kNumBuckets, [&](size_t i) {
      if (i < N)
        FileData fd = std::move(fileData[i]);
      else
        Bucket bucket = std::move(db[i - N]);
    });
  }
}

// importCmseSymbols is explicitly instantiated in ARM.cpp.
extern template void ObjFile<ELF32LE>::importCmseSymbols();
extern template void ObjFile<ELF32BE>::importCmseSymbols();
extern template void ObjFile<ELF64LE>::importCmseSymbols();
extern template void ObjFile<ELF64BE>::importCmseSymbols();

template <class ELFT> static void importCmse(Ctx &, InputFile *f) {
  cast<ObjFile<ELFT>>(*f).importCmseSymbols();
}

void elf::parseFiles(Ctx &ctx,
                     SmallVector<std::unique_ptr<InputFile>, 0> &files) {
  llvm::TimeTraceScope timeScope("Parse input files");
  const size_t numFiles = files.size();

  // installShardedSymbols() replaces the Symbol* objects, so --trace-symbol
  // flags set by the driver must be re-applied in phase 4 BEFORE any
  // resolve() fires, otherwise the trace messages are lost. preOrder records
  // the pre-parseFiles insertions (-u, --trace-symbol), which the serial
  // path places at the head of symVector; phase 4 keys both by the bucket
  // key (stem, without @@ver).
  DenseSet<StringRef> tracedNames;
  DenseMap<CachedHashStringRef, uint32_t> preOrder;
  for (Symbol *s : ctx.symtab->getSymbols()) {
    StringRef stem = getVersionedStem(s->getName());
    preOrder.try_emplace(CachedHashStringRef(stem), preOrder.size());
    if (s->traced)
      tracedNames.insert(stem);
  }

  // Partition into compatible input files. Non-obj files are deferred to a
  // serial post-phase loop below.
  SmallVector<InputFile *, 0> allFiles;
  SmallVector<InputFile *, 0> nonObjFiles;
  allFiles.reserve(files.size());
  InputFile *firstCompat = nullptr;
  for (auto &f : files) {
    // ctx.objectFiles etc. are not populated yet; pass the first compatible
    // file so incompatibility diagnostics can name a reference.
    if (!isCompatible(ctx, f.get(), firstCompat))
      continue;
    if (!firstCompat && !f->lazy && (f->isElf() || isa<BitcodeFile>(f.get())))
      firstCompat = f.get();
    allFiles.push_back(f.get());
    if (f->kind() != InputFile::ObjKind)
      nonObjFiles.push_back(f.get());
  }

  // ELF obj names live in NUL-terminated .strtab; LLVM IR names are not
  // NUL-terminated. Serially save bitcode symbol names through the
  // (thread-unsafe) unique saver now so the parallel phases and phase 4's
  // full-name recovery can rely on NUL termination, mirroring
  // createBitcodeSymbol() in the serial path.
  for (InputFile *f : allFiles)
    if (auto *bf = dyn_cast<BitcodeFile>(f))
      for (const auto &irSym : bf->obj->symbols())
        const_cast<lto::InputFile::Symbol &>(irSym).Name =
            ctx.uniqueSaver.save(irSym.getName());

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
               tracedNames, preOrder, nonObjFiles);

  // Parse non-obj inputs serially. The activation loop already printed -t
  // traces for them in command-line order.
  {
    llvm::TimeTraceScope ts("Parse non-obj files");
    for (InputFile *f : nonObjFiles)
      parseFile(ctx, f);
  }

  // Dependent-library processing appended new input files after the pipeline
  // snapshot. Parse them through the serial path (which itself may append
  // more); the driver's late-extraction pass handles their
  // initSectionsAndLocalSyms/postParse.
  for (size_t i = numFiles; i < files.size(); ++i) {
    InputFile *f = files[i].get();
    if (!f)
      continue;
    if (ctx.arg.trace && !f->lazy)
      Msg(ctx) << f;
    parseFile(ctx, f);
  }

  // Pre-parseFiles addUnusedUndefined entries for -u/--undefined live in the
  // serial symMap which installShardedSymbols shadowed. Re-add them so they
  // route to the shards: this both marks the winner referenced and lets a
  // strong undef extract a lazy def.
  for (StringRef name : ctx.arg.undefined)
    ctx.symtab->addUnusedUndefined(name)->referenced = true;

  if (ctx.driver.armCmseImpLib)
    invokeELFT(importCmse, ctx, ctx.driver.armCmseImpLib.get());
}
