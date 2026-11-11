//===- SymbolTable.h --------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLD_ELF_SYMBOL_TABLE_H
#define LLD_ELF_SYMBOL_TABLE_H

#include "Symbols.h"
#include "llvm/ADT/CachedHashString.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/Support/Compiler.h"
#include <array>

namespace lld::elf {
struct Ctx;
class InputFile;
class SharedFile;

struct ArmCmseEntryFunction {
  Symbol *acleSeSym;
  Symbol *sym;
};

// SymbolTable is a bucket of all known symbols, including defined,
// undefined, or lazy symbols (the last one is symbols in archive
// files whose archive members are not yet loaded).
//
// We put all symbols of all files to a SymbolTable, and the
// SymbolTable selects the "best" symbols if there are name
// conflicts. For example, obviously, a defined symbol is better than
// an undefined symbol. Or, if there's a conflict between a lazy and a
// undefined, it'll read an archive member to read a real definition
// to replace the lazy symbol. The logic is implemented in the
// add*() functions, which are called by input files as they are parsed. There
// is one add* function per symbol type.
class SymbolTable {
public:
  SymbolTable(Ctx &ctx) : ctx(ctx) {}
  ArrayRef<Symbol *> getSymbols() const { return symVector; }

  void wrap(Symbol *sym, Symbol *real, Symbol *wrap);

  Symbol *insert(StringRef name) { return insert(intern(name)); }
  // Add a symbol returned by intern to the symbol table.
  Symbol *insert(Symbol *sym) {
    if (sym->inserted)
      return sym;
    if (!curPriority) {
      sym->inserted = true;
      symVector.push_back(sym);
    } else if (sym->auxIdx - 1 >= curPriority) {
      sym->auxIdx.store(curPriority, std::memory_order_relaxed);
      serialTouched.emplace_back(curPriority, sym);
    }
    return sym;
  }
  // A symbol name split for interning: <name>@@<version> is keyed by <name>.
  struct Name {
    llvm::CachedHashStringRef stem{StringRef(), 0};
    uint32_t size;
    bool versioned;
  };
  static Name parseName(StringRef name);
  // Return the symbol named `name`, creating one that is not yet in the symbol
  // table if needed. This is thread-safe while `concurrent` is true.
  Symbol *intern(const Name &name);
  Symbol *intern(StringRef name) { return intern(parseName(name)); }
  // A lookup misses the cache at the hash table slot and then at the name
  // stored there. Callers that intern many names call prefetch for a batch of
  // names, then prefetchName, then intern, so that the misses overlap.
  void prefetch(const Name &name) {
    if (Table *t = getShard(name.stem).table.load(std::memory_order_acquire))
      LLVM_PREFETCH(&t->slots[name.stem.hash() & t->mask], 0, 3);
  }
  void prefetchName(const Name &name) {
    Table *t = getShard(name.stem).table.load(std::memory_order_acquire);
    if (!t)
      return;
    Slot &s = t->slots[name.stem.hash() & t->mask];
    if (s.sym.load(std::memory_order_acquire) && s.hash == name.stem.hash()) {
      LLVM_PREFETCH(s.name, 0, 3);
      LLVM_PREFETCH(s.name + 64, 0, 3);
    }
  }
  // Size the hash table for about `numNames` names, counting duplicates.
  void reserve(size_t numNames) {
    initialCapacity = std::max<size_t>(64, llvm::bit_ceil(numNames / 512));
  }
  // Like intern, but `name` is used verbatim (no '@' handling). Used for COMDAT
  // group signatures.
  Symbol *internRaw(llvm::CachedHashStringRef name);
  // True while worker threads run. Shards and symbols are locked only then.
  bool concurrent = false;

  // While parseFiles resolves a batch of files, symbols are not appended to
  // symVector when first seen. Instead, the file with the lowest priority
  // among those that touch a symbol appends it at the end of the batch. This
  // keeps the order deterministic. The priority is kept in Symbol::auxIdx.
  //
  // Called by a file parsed by a worker thread, with the symbol locked.
  static void touch(Symbol &sym, uint32_t priority) {
    if (!sym.inserted &&
        sym.auxIdx.load(std::memory_order_relaxed) - 1 >= priority)
      sym.auxIdx.store(priority, std::memory_order_relaxed);
  }
  // Called at the end of a batch by each file. Return true if the file of
  // `priority` is the one that appends `sym`.
  static bool claim(Symbol &sym, uint32_t priority) {
    if (sym.auxIdx.load(std::memory_order_relaxed) != priority)
      return false;
    sym.auxIdx.store(0, std::memory_order_relaxed);
    sym.inserted = true;
    return true;
  }
  // Nonzero while a batch is resolved: the priority of the file that the
  // serial insert() calls are attributed to.
  uint32_t curPriority = 0;
  SmallVector<std::pair<uint32_t, Symbol *>, 0> serialTouched;
  void append(ArrayRef<std::pair<uint32_t, ArrayRef<Symbol *>>> segs);
  // Free the hash tables that have been replaced. No other thread may be
  // interning.
  void freeRetiredTables();
  bool hasTraced = false;

  // COMDAT groups define "link once" sections. If two COMDAT groups have the
  // same signature, only the one in the file with the lowest priority is
  // linked. `sig` is the symbol interned for the signature, and the priority
  // is kept in Symbol::dynsymIndex until the last file has been parsed.
  // Return false if the group is already known to be discarded: a file of
  // lower priority has it, or this file has registered the signature before.
  static bool addComdat(Symbol &sig, uint32_t priority) {
    auto &owner = sig.dynsymIndex;
    uint32_t cur = owner.load(std::memory_order_relaxed);
    while (cur - 1 >= priority)
      if (owner.compare_exchange_weak(cur, priority, std::memory_order_relaxed))
        return true;
    return false;
  }
  static uint32_t getComdatOwner(const Symbol &sig) {
    return sig.dynsymIndex.load(std::memory_order_relaxed);
  }

  template <typename T> Symbol *addSymbol(const T &newSym) {
    Symbol *sym = insert(newSym.getName());
    sym->resolve(ctx, newSym);
    return sym;
  }
  Symbol *addSymbol(const Undefined &newSym);
  // If a reference to `sym` may extract a lazy file, return the file.
  InputFile *getLazyFile(const Symbol &sym) const;
  Symbol *addAndCheckDuplicate(const Defined &newSym);

  void scanVersionScript();

  Symbol *find(StringRef name);

  Symbol *addUnusedUndefined(StringRef name,
                             uint8_t binding = llvm::ELF::STB_GLOBAL);

  // Set of .so files to not link the same shared object file more than once.
  llvm::DenseMap<llvm::CachedHashStringRef, SharedFile *> soNames;

  // A map from the symbol of a COMDAT group signature to the file that keeps
  // the group. Built by findComdatOwner.
  std::optional<llvm::DenseMap<const Symbol *, const InputFile *>> comdatOwners;

  // The Map of __acle_se_<sym>, <sym> pairs found in the input objects.
  // Key is the <sym> name.
  llvm::SmallMapVector<StringRef, ArmCmseEntryFunction, 1> cmseSymMap;

  // Map of symbols defined in the Arm CMSE import library. The linker must
  // preserve the addresses in the output objects.
  llvm::StringMap<Defined *> cmseImportLib;

  // True if <sym> from the input Arm CMSE import library is written to the
  // output Arm CMSE import library.
  llvm::StringMap<bool> inCMSEOutImpLib;

private:
  SmallVector<Symbol *, 0> findByVersion(SymbolVersion ver);

  llvm::StringMap<SmallVector<Symbol *, 0>> &getDemangledSyms();
  bool assignExactVersion(SymbolVersion ver, uint16_t versionId);

  Ctx &ctx;

  // The map from symbol name to the symbol is sharded by the high bits of the
  // hash. Each shard is an insert-only open addressing hash table. A lookup
  // of an existing name does not take the lock, so that threads interning
  // such names do not write to shared memory.
  struct Slot {
    // Published last, after the other members.
    std::atomic<Symbol *> sym;
    const char *name;
    uint32_t size;
    uint32_t hash;
  };
  struct Table {
    Table(size_t n) : mask(n - 1), slots(new Slot[n]()) {}
    // Return the symbol of `name`, or null. This can run while another thread
    // inserts; each slot's symbol is loaded once.
    Symbol *find(llvm::CachedHashStringRef name) const {
      for (size_t i = name.hash();; ++i) {
        Slot &s = slots[i & mask];
        Symbol *sym = s.sym.load(std::memory_order_acquire);
        if (!sym)
          return nullptr;
        if (s.hash == name.hash() && StringRef(s.name, s.size) == name.val())
          return sym;
      }
    }
    // Return the slot of `name`, or the empty slot where it is to be inserted.
    // The caller holds the shard's lock.
    Slot &probe(llvm::CachedHashStringRef name) {
      for (size_t i = name.hash();; ++i) {
        Slot &s = slots[i & mask];
        if (!s.sym.load(std::memory_order_relaxed) ||
            (s.hash == name.hash() && StringRef(s.name, s.size) == name.val()))
          return s;
      }
    }
    size_t mask;
    std::unique_ptr<Slot[]> slots;
  };
  struct alignas(64) Shard {
    std::atomic<Table *> table{nullptr};
    // The members below are written when a name is inserted. Keep them away
    // from `table`, which lookups read.
    alignas(64) size_t numEntries = 0;
    // `table` is the last element. The others have been replaced by a larger
    // table but may still be read by other threads.
    SmallVector<std::unique_ptr<Table>, 0> tables;
    std::mutex mu;
  };
  std::array<Shard, 256> shards;
  Shard &getShard(llvm::CachedHashStringRef key) {
    return shards[key.hash() >> 24];
  }
  size_t initialCapacity = 64;

  // Global symbols in insertion order, which must be deterministic even when
  // cross linking.
  SmallVector<Symbol *, 0> symVector;

  // A map from demangled symbol names to their symbol objects.
  // This mapping is 1:N because two symbols with different versions
  // can have the same name. We use this map to handle "extern C++ {}"
  // directive in version scripts.
  std::optional<llvm::StringMap<SmallVector<Symbol *, 0>>> demangledSyms;
};

} // namespace lld::elf

#endif
