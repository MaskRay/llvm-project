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

  Symbol *insert(StringRef name);

  template <typename T> Symbol *addSymbol(const T &newSym) {
    Symbol *sym = insert(newSym.getName());
    sym->resolve(ctx, newSym);
    return sym;
  }
  Symbol *addAndCheckDuplicate(Ctx &, const Defined &newSym);

  void scanVersionScript();

  Symbol *find(StringRef name);
  Symbol *find(llvm::CachedHashStringRef key);

  void handleDynamicList();

  Symbol *addUnusedUndefined(StringRef name,
                             uint8_t binding = llvm::ELF::STB_GLOBAL);

  // Install sharded symbol maps from parallel parse pipeline. After this call,
  // find() and insert() route through the shard maps instead of symMap.
  void installShardedSymbols(
      std::unique_ptr<llvm::DenseMap<llvm::CachedHashStringRef, int>[]> maps,
      unsigned numShards, SmallVector<Symbol *, 0> &&syms);

  // Set of .so files to not link the same shared object file more than once.
  llvm::DenseMap<llvm::CachedHashStringRef, SharedFile *> soNames;

  // Comdat groups define "link once" sections. If two comdat groups have the
  // same name, only one of them is linked, and the other is ignored. This map
  // is used to uniquify them.
  llvm::DenseMap<llvm::CachedHashStringRef, const InputFile *> comdatGroups;

  // Sharded comdat groups for parallel parse. When non-empty, findComdatOwner()
  // routes lookups through these shards instead of comdatGroups.
  std::unique_ptr<
      llvm::DenseMap<llvm::CachedHashStringRef, const InputFile *>[]>
      comdatGroupShards;
  unsigned numComdatShards = 0;

  const InputFile *findComdatOwner(llvm::CachedHashStringRef name) const {
    if (numComdatShards) {
      auto &shard = comdatGroupShards[name.hash() % numComdatShards];
      auto it = shard.find(name);
      return it != shard.end() ? it->second : nullptr;
    }
    auto it = comdatGroups.find(name);
    return it != comdatGroups.end() ? it->second : nullptr;
  }

  // Returns true if `f` is (or becomes) the owner of the comdat group named
  // `name`. If no owner exists yet, `f` is registered as the owner. Matches
  // the `isNew || owner == self` pattern used in the serial
  // ObjFile<ELFT>::parse() path. Routes through the sharded map when parallel
  // parse is active, so late-joining BitcodeFile::parse() calls interoperate
  // with phase 4's pre-population.
  bool addComdatGroup(llvm::CachedHashStringRef name, const InputFile *f) {
    if (numComdatShards) {
      auto &shard = comdatGroupShards[name.hash() % numComdatShards];
      auto [it, isNew] = shard.try_emplace(name, f);
      return isNew || it->second == f;
    }
    auto [it, isNew] = comdatGroups.try_emplace(name, f);
    return isNew || it->second == f;
  }

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
  SmallVector<Symbol *, 0> findAllByVersion(SymbolVersion ver,
                                            bool includeNonDefault);

  llvm::StringMap<SmallVector<Symbol *, 0>> &getDemangledSyms();
  bool assignExactVersion(SymbolVersion ver, uint16_t versionId,
                          StringRef versionName, bool includeNonDefault);
  void assignWildcardVersion(SymbolVersion ver, uint16_t versionId,
                             bool includeNonDefault);

  Ctx &ctx;

  // Global symbols and a map from symbol name to the index. The order is not
  // defined. We can use an arbitrary order, but it has to be deterministic even
  // when cross linking.
  llvm::DenseMap<llvm::CachedHashStringRef, int> symMap;
  SmallVector<Symbol *, 0> symVector;

  // Sharded mode: parallel parse installs per-bucket DenseMaps here.
  // When numShards > 0, find()/insert() route through shardMaps.
  unsigned numShards = 0;
  std::unique_ptr<llvm::DenseMap<llvm::CachedHashStringRef, int>[]> shardMaps;

  // A map from demangled symbol names to their symbol objects.
  // This mapping is 1:N because two symbols with different versions
  // can have the same name. We use this map to handle "extern C++ {}"
  // directive in version scripts.
  std::optional<llvm::StringMap<SmallVector<Symbol *, 0>>> demangledSyms;
};

} // namespace lld::elf

#endif
