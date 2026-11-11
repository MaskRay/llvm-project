//===- SymbolTable.cpp ----------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Symbol table is a bag of all known symbols. We put all symbols of
// all input files to the symbol table. The symbol table is basically
// a hash table with the logic to resolve symbol name conflicts using
// the symbol types.
//
//===----------------------------------------------------------------------===//

#include "SymbolTable.h"
#include "Config.h"
#include "InputFiles.h"
#include "Symbols.h"
#include "lld/Common/Memory.h"
#include "lld/Common/Strings.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Demangle/Demangle.h"
#include "llvm/Support/Parallel.h"

using namespace llvm;
using namespace llvm::object;
using namespace llvm::ELF;
using namespace lld;
using namespace lld::elf;

void SymbolTable::wrap(Symbol *sym, Symbol *real, Symbol *wrap) {
  // Redirect __real_foo to the original foo and foo to the original __wrap_foo.
  auto rebind = [&](StringRef name, Symbol *to) {
    CachedHashStringRef key(name);
    internRaw(key);
    getShard(key).table.load()->probe(key).sym = to;
  };
  rebind(real->getName(), sym);
  rebind(sym->getName(), wrap);

  // Propagate symbol usage information to the redirected symbols.
  if (sym->isUsedInRegularObj)
    wrap->isUsedInRegularObj = true;
  if (real->isUsedInRegularObj)
    sym->isUsedInRegularObj = true;
  else if (!sym->isDefined())
    // Now that all references to sym have been redirected to wrap, if there are
    // no references to real (which has been redirected to sym), we only need to
    // keep sym if it was defined, otherwise it's unused and can be dropped.
    sym->isUsedInRegularObj = false;

  if (ctx.hasDynDbg) {
    if (sym->isDynDbgRef)
      wrap->isDynDbgRef = true;
    if (real->isDynDbgRef)
      sym->isDynDbgRef = true;
    else if (!sym->isDefined())
      sym->isDynDbgRef = false;
  }

  // Now renaming is complete, and no one refers to real. We drop real from
  // .symtab and .dynsym. If real is undefined, it is important that we don't
  // leave it in .dynsym, because otherwise it might lead to an undefined symbol
  // error in a subsequent link. If real is defined, we could emit real as an
  // alias for sym, but that could degrade the user experience of some tools
  // that can print out only one symbol for each location: sym is a preferred
  // name than real, but they might print out real instead.
  memcpy(static_cast<void *>(real), sym, sizeof(SymbolUnion));
  real->isUsedInRegularObj = false;
}

SymbolTable::Name SymbolTable::parseName(StringRef name) {
  // <name>@@<version> means the symbol is the default version. In that
  // case <name>@@<version> will be used to resolve references to <name>.
  //
  // Since this is a hot path, the following string search code is
  // optimized for speed. StringRef::find(char) is much faster than
  // StringRef::find(StringRef).
  StringRef stem = name;
  size_t pos = name.find('@');
  if (pos != StringRef::npos && pos + 1 < name.size() && name[pos + 1] == '@')
    stem = name.take_front(pos);
  return {CachedHashStringRef(stem), uint32_t(name.size()),
          pos != StringRef::npos};
}

Symbol *SymbolTable::intern(const Name &name) {
  Symbol *sym = internRaw(name.stem);
  if (name.versioned) {
    auto l = lockIf(concurrent, *sym);
    // If files disagree on the default version, keep the smallest name so
    // that the result does not depend on the order.
    StringRef full(name.stem.val().data(), name.size);
    if (name.stem.size() != name.size &&
        (sym->getName().size() == name.stem.size() || full < sym->getName()))
      sym->setName(full);
    sym->hasVersionSuffix = true;
  }
  return sym;
}

Symbol *SymbolTable::internRaw(CachedHashStringRef name) {
  Shard &shard = getShard(name);
  Table *t = shard.table.load(std::memory_order_acquire);
  if (t)
    if (Symbol *sym = t->find(name))
      return sym;

  auto l = lockIf(concurrent, shard.mu);
  t = shard.table.load(std::memory_order_relaxed);
  if (!t || (shard.numEntries + 1) * 2 > t->mask + 1) {
    auto newTable =
        std::make_unique<Table>(t ? (t->mask + 1) * 2 : initialCapacity);
    if (t) {
      for (size_t i = 0; i <= t->mask; ++i) {
        Slot &from = t->slots[i];
        Symbol *sym = from.sym.load(std::memory_order_relaxed);
        if (!sym)
          continue;
        Slot &to = newTable->probe(
            CachedHashStringRef(StringRef(from.name, from.size), from.hash));
        to.name = from.name;
        to.size = from.size;
        to.hash = from.hash;
        to.sym.store(sym, std::memory_order_relaxed);
      }
    }
    t = newTable.get();
    shard.tables.push_back(std::move(newTable));
    shard.table.store(t, std::memory_order_release);
  }
  Slot &slot = t->probe(name);
  Symbol *sym = slot.sym.load(std::memory_order_relaxed);
  if (!sym) {
    // The storage is value-initialized, so the Symbol fields are zero. Set the
    // ones that need a non-zero value.
    sym = reinterpret_cast<Symbol *>(makeThreadLocal<SymbolUnion>());
    sym->setName(name.val());
    sym->versionId = VER_NDX_GLOBAL;
    slot.name = name.val().data();
    slot.size = name.size();
    slot.hash = name.hash();
    slot.sym.store(sym, std::memory_order_release);
    ++shard.numEntries;
  }
  return sym;
}

void SymbolTable::freeRetiredTables() {
  for (Shard &shard : shards)
    if (shard.tables.size() > 1)
      shard.tables.erase(shard.tables.begin(), shard.tables.end() - 1);
}

// This variant of addSymbol is used by BinaryFile::parse to check duplicate
// symbol errors.
Symbol *SymbolTable::addAndCheckDuplicate(const Defined &newSym) {
  Symbol *sym = insert(newSym.getName());
  if (sym->isDefined())
    sym->checkDuplicate(ctx, newSym);
  sym->resolve(ctx, newSym);
  sym->isUsedInRegularObj = true;
  return sym;
}

void SymbolTable::append(
    ArrayRef<std::pair<uint32_t, ArrayRef<Symbol *>>> segs) {
  SmallVector<size_t, 0> offsets(segs.size());
  size_t n = symVector.size();
  for (size_t i = 0, e = segs.size(); i != e; ++i) {
    offsets[i] = n;
    n += segs[i].second.size();
  }
  symVector.resize_for_overwrite(n);
  parallelFor(0, segs.size(), [&](size_t i) {
    llvm::copy(segs[i].second, symVector.data() + offsets[i]);
  });
}

Symbol *SymbolTable::addSymbol(const Undefined &newSym) {
  assert(!curPriority && "files being resolved follow up by themselves");
  Symbol *sym = insert(newSym.getName());
  if (sym->resolve(ctx, newSym)) {
    if (InputFile *f = getLazyFile(*sym)) {
      if (f->lazy) {
        f->lazy = false;
        parseFile(ctx, f);
        recordWhyExtract(ctx, newSym.file, *f, *sym);
      }
    } else {
      sym->demote(newSym);
    }
  }
  return sym;
}

InputFile *SymbolTable::getLazyFile(const Symbol &sym) const {
  if (sym.isLazy())
    return sym.file;
  auto *s = dyn_cast<SharedSymbol>(&sym);
  return s && s->lazyPriority ? ctx.driver.files[s->lazyPriority - 1].get()
                              : nullptr;
}

Symbol *SymbolTable::find(StringRef name) {
  assert(!curPriority && "find() must not run while files are resolved");
  CachedHashStringRef key(name);
  Table *t = getShard(key).table.load(std::memory_order_relaxed);
  Symbol *sym = t ? t->find(key) : nullptr;
  return sym && sym->inserted ? sym : nullptr;
}

// A version script/dynamic list is only meaningful for a Defined symbol.
// A CommonSymbol will be converted to a Defined in replaceCommonSymbols().
// A lazy symbol may be made Defined if an LTO libcall extracts it.
static bool canBeVersioned(const Symbol &sym) {
  return sym.isDefined() || sym.isCommon() || sym.isLazy();
}

static std::string demangleForVersion(StringRef name) {
  auto [base, ver] = name.split('@');
  std::string s = demangle(base);
  if (!ver.empty() && !ver.starts_with('@'))
    s += ("@" + ver).str();
  return s;
}

// Map from demangled name to symbols, for exact lookups in extern "C++" blocks.
StringMap<SmallVector<Symbol *, 0>> &SymbolTable::getDemangledSyms() {
  if (!demangledSyms) {
    demangledSyms.emplace();
    for (Symbol *sym : symVector)
      if (canBeVersioned(*sym))
        (*demangledSyms)[demangleForVersion(sym->getName())].push_back(sym);
  }
  return *demangledSyms;
}

SmallVector<Symbol *, 0> SymbolTable::findByVersion(SymbolVersion ver) {
  if (ver.isExternCpp)
    return getDemangledSyms().lookup(ver.name);
  if (Symbol *sym = find(ver.name))
    if (canBeVersioned(*sym))
      return {sym};
  return {};
}

// Set symbol versions to symbols. This function handles patterns containing no
// wildcard characters. Return false if no symbol definition matches ver.
bool SymbolTable::assignExactVersion(SymbolVersion ver, uint16_t versionId) {
  // Get a list of symbols which we need to assign the version to.
  SmallVector<Symbol *, 0> syms = findByVersion(ver);

  auto getName = [&ctx = ctx](uint16_t ver) -> std::string {
    if (ver == VER_NDX_LOCAL)
      return "VER_NDX_LOCAL";
    if (ver == VER_NDX_GLOBAL)
      return "VER_NDX_GLOBAL";
    return ("version '" + ctx.arg.versionDefinitions[ver].name + "'").str();
  };

  // Assign the version.
  for (Symbol *sym : syms) {
    // Skip symbols containing version info because symbol versions specified
    // by symbol names take precedence over version scripts. See
    // parseSymbolVersion(ctx).
    if (sym->hasVersionSuffix)
      continue;

    // If the version has not been assigned, assign versionId to the symbol.
    if (!sym->versionScriptAssigned) {
      sym->versionScriptAssigned = true;
      sym->versionId = versionId;
    }
    if (sym->versionId == versionId)
      continue;

    Warn(ctx) << "attempt to reassign symbol '" << ver.name << "' of "
              << getName(sym->versionId) << " to " << getName(versionId);
  }
  return !syms.empty();
}

namespace {
struct WildcardPattern {
  SingleStringMatcher matcher;
  bool isExternCpp;
  uint16_t versionId;
  WildcardPattern(const SymbolVersion &ver, uint16_t versionId)
      : matcher(ver.name), isExternCpp(ver.isExternCpp), versionId(versionId) {}
};
} // namespace

// This function processes version scripts by updating the versionId
// member of symbols.
// If there's only one anonymous version definition in a version
// script file, the script does not actually define any symbol version,
// but just specifies symbols visibilities.
void SymbolTable::scanVersionScript() {
  SmallString<128> buf;
  // First, we assign versions to exact matching symbols,
  // i.e. version definitions not containing any glob meta-characters.
  for (VersionDefinition &v : ctx.arg.versionDefinitions) {
    auto assignExact = [&](SymbolVersion pat, uint16_t id, StringRef ver) {
      bool found = assignExactVersion(pat, id);
      // A definition foo@v1 is keyed by "foo@v1"; look it up so that the
      // pattern is not reported as undefined. Its version is governed by
      // parseSymbolVersion instead.
      buf.clear();
      found |= !findByVersion({(pat.name + "@" + v.name).toStringRef(buf),
                               pat.isExternCpp, /*hasWildCard=*/false})
                    .empty();
      if (!found && !ctx.arg.undefinedVersion)
        Err(ctx) << "version script assignment of '" << ver << "' to symbol '"
                 << pat.name << "' failed: symbol not defined";
    };
    for (SymbolVersion &pat : v.nonLocalPatterns)
      if (!pat.hasWildcard)
        assignExact(pat, v.id, v.name);
    for (SymbolVersion pat : v.localPatterns)
      if (!pat.hasWildcard)
        assignExact(pat, VER_NDX_LOCAL, "local");
  }

  // Next, collect wildcards in precedence order, where "*" patterns have the
  // lowest precedence in GNU ld. Because the last match takes precedence over
  // previous matches, we iterate over the definitions in the reverse order.
  SmallVector<WildcardPattern, 0> pats, asterisks;
  bool globalAsteriskFound = false;
  bool localAsteriskFound = false;
  bool asteriskReported = false;
  for (VersionDefinition &v : llvm::reverse(ctx.arg.versionDefinitions)) {
    for (bool isLocal : {false, true}) {
      uint16_t id = isLocal ? VER_NDX_LOCAL : v.id;
      for (SymbolVersion &pat :
           isLocal ? v.localPatterns : v.nonLocalPatterns) {
        if (!pat.hasWildcard)
          continue;
        if (pat.name != "*") {
          pats.emplace_back(pat, id);
          continue;
        }
        if (!asteriskReported) {
          if ((isLocal && globalAsteriskFound) ||
              (!isLocal && localAsteriskFound)) {
            Warn(ctx)
                << "wildcard pattern '*' is used for both 'local' and 'global' "
                   "scopes in version script";
            asteriskReported = true;
          } else if (!isLocal && globalAsteriskFound) {
            Warn(ctx) << "wildcard pattern '*' is used for multiple version "
                         "definitions in version script";
            asteriskReported = true;
          } else {
            localAsteriskFound = isLocal;
            globalAsteriskFound = !isLocal;
          }
        }
        asterisks.emplace_back(pat, id);
      }
    }
  }
  pats.append(asterisks);

  auto findFirstMatch = [&](ArrayRef<WildcardPattern> pats,
                            StringRef name) -> const WildcardPattern * {
    std::optional<std::string> demangled;
    for (auto &pat : pats) {
      if (pat.isExternCpp && !demangled)
        demangled = demangleForVersion(name);
      if (pat.matcher.match(pat.isExternCpp ? StringRef(*demangled) : name))
        return &pat;
    }
    return nullptr;
  };

  // Exact matching takes precedence over wildcard matching, so a wildcard
  // assigns a version only if none has been assigned.
  if (!pats.empty()) {
    parallelForEach(symVector, [&](Symbol *sym) {
      if (sym->versionScriptAssigned || sym->hasVersionSuffix ||
          !canBeVersioned(*sym))
        return;
      if (auto *pat = findFirstMatch(pats, sym->getName()))
        sym->versionId = pat->versionId;
    });
  }

  // Handle --dynamic-list. If a specified symbol is also matched by local: in a
  // version script, the version script takes precedence.
  SmallVector<Symbol *, 0> syms;
  pats.clear();
  for (SymbolVersion &ver : ctx.arg.dynamicList) {
    if (ver.hasWildcard) {
      pats.emplace_back(ver, 0);
    } else {
      for (Symbol *sym : findByVersion(ver))
        sym->isExported = sym->inDynamicList = true;
    }
  }
  if (!pats.empty()) {
    parallelForEach(symVector, [&](Symbol *sym) {
      if (!canBeVersioned(*sym))
        return;
      StringRef name = sym->getName();
      if (findFirstMatch(pats, name))
        sym->isExported = sym->inDynamicList = true;
    });
  }
}

Symbol *SymbolTable::addUnusedUndefined(StringRef name, uint8_t binding) {
  return addSymbol(Undefined{ctx.internalFile, name, binding, STV_DEFAULT, 0});
}
