//===- Symbols.h ------------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file defines various types of Symbols.
//
//===----------------------------------------------------------------------===//

#ifndef LLD_ELF_SYMBOLS_H
#define LLD_ELF_SYMBOLS_H

#include "Config.h"
#include "lld/Common/LLVM.h"
#include "lld/Common/Memory.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/Object/ELF.h"
#include "llvm/Support/Compiler.h"
#include <atomic>
#include <mutex>
#include <thread>
#include <tuple>

namespace lld::elf {
class CommonSymbol;
class Defined;
class OutputSection;
class SectionBase;
class InputSectionBase;
class SharedSymbol;
class Symbol;
class Undefined;
class LazySymbol;
class InputFile;

// Returns a string representation for a symbol for diagnostics.
std::string toStr(Ctx &, const Symbol &);
const ELFSyncStream &operator<<(const ELFSyncStream &, const Symbol *);

void printTraceSymbol(const Symbol &sym, StringRef name);

// Lock `m` if `concurrent`, i.e. if other threads may be using it.
template <class T> std::unique_lock<T> lockIf(bool concurrent, T &m) {
  return concurrent ? std::unique_lock<T>(m) : std::unique_lock<T>();
}

enum {
  // True if an undefined or shared symbol is used from a live section.
  //
  // NOTE: In Writer.cpp the field is used to mark local defined symbols
  // which are referenced by relocations when -r or --emit-relocs is given.
  USED = 1 << 0,
  NEEDS_GOT = 1 << 1,
  NEEDS_PLT = 1 << 2,
  HAS_DIRECT_RELOC = 1 << 3,
  // True if this symbol needs a canonical PLT entry, or (during
  // postScanRelocations) a copy relocation.
  NEEDS_COPY = 1 << 4,
  NEEDS_TLSDESC = 1 << 5,
  NEEDS_TLSGD = 1 << 6,
  NEEDS_GOT_DTPREL = 1 << 7,
  NEEDS_TLSIE = 1 << 8,
  NEEDS_GOT_AUTH = 1 << 9,
  // 1 << 10 unused
  NEEDS_TLSDESC_AUTH = 1 << 11,
  // 1 << 12 unused
  // Set during symbol resolution if a non-weak reference of default
  // visibility in a relocatable object file no longer changes the symbol.
  SYM_REF_NOP = 1 << 14,
  // Spin lock held while a thread resolves the symbol.
  SYM_LOCKED = 1 << 15,
};

// The base class for real symbol classes.
class Symbol {
public:
  enum Kind {
    PlaceholderKind,
    DefinedKind,
    CommonKind,
    SharedKind,
    UndefinedKind,
    LazyKind,
  };

  Kind kind() const { return static_cast<Kind>(symbolKind); }

  // The file from which this symbol was created.
  InputFile *file;

  // Symbols are referenced by pointer throughout the linker, so an implicit
  // copy would create a dangerous duplicate; copy the needed members
  // explicitly instead. Deleting the copy operations also suppresses the
  // implicit move operations.
  Symbol(const Symbol &o) = delete;
  Symbol &operator=(const Symbol &) = delete;

protected:
  const char *nameData;
  // 32-bit size saves space.
  uint32_t nameSize;

public:
  // The next three fields have the same meaning as the ELF symbol attributes.
  // type and binding are placed in this order to optimize generating st_info,
  // which is defined as (binding << 4) + (type & 0xf), on a little-endian
  // system.
  uint8_t type : 4; // symbol type

  // Symbol binding. This is not overwritten by replace() to track
  // changes during resolution. In particular:
  //  - An undefined weak is still weak when it resolves to a shared library.
  //  - An undefined weak will not extract archive members, but we have to
  //    remember it is weak.
  uint8_t binding : 4;

  uint8_t stOther; // st_other field value

  uint8_t symbolKind;

  // True if this symbol is preemptible at load time.
  //
  // Primarily set in two locations, (a) parseVersionAndComputeIsPreemptible and
  // (b) demoteSymbolsAndComputeIsPreemptible.
  LLVM_PREFERRED_TYPE(bool)
  uint8_t isPreemptible : 1;

  // True if the symbol was used for linking and thus need to be added to the
  // output file's symbol table. This is true for all symbols except for
  // unreferenced DSO symbols, lazy (archive) symbols, and bitcode symbols that
  // are unreferenced except by other bitcode objects.
  LLVM_PREFERRED_TYPE(bool)
  uint8_t isUsedInRegularObj : 1;

  // Used by a Defined symbol with protected or default visibility, to record
  // whether it is required to be exported into .dynsym. This is set when any of
  // the following conditions hold:
  //
  // - If there is an interposable symbol from a DSO. Note: We also do this for
  //   STV_PROTECTED symbols which can't be interposed (to match BFD behavior).
  // - If -shared or --export-dynamic is specified, any symbol in an object
  //   file/bitcode sets this property, unless suppressed by LTO
  //   canBeOmittedFromSymbolTable().
  LLVM_PREFERRED_TYPE(bool)
  uint8_t isExported : 1;

  LLVM_PREFERRED_TYPE(bool)
  uint8_t ltoCanOmit : 1;

  // True if this symbol is specified by --trace-symbol option.
  LLVM_PREFERRED_TYPE(bool)
  uint8_t traced : 1;

  // True if the name contains '@'.
  LLVM_PREFERRED_TYPE(bool)
  uint8_t hasVersionSuffix : 1;

  // True if the symbol is in SymbolTable::getSymbols().
  LLVM_PREFERRED_TYPE(bool)
  uint8_t inserted : 1;

  // Symbol visibility. This is the computed minimum visibility of all
  // observed non-DSO symbols.
  uint8_t visibility() const { return stOther & 3; }
  void setVisibility(uint8_t visibility) {
    stOther = (stOther & ~3) | visibility;
  }

  uint8_t computeBinding(Ctx &) const;
  bool isGlobal() const { return binding == llvm::ELF::STB_GLOBAL; }
  bool isWeak() const { return binding == llvm::ELF::STB_WEAK; }

  bool isUndefined() const { return symbolKind == UndefinedKind; }
  bool isCommon() const { return symbolKind == CommonKind; }
  bool isDefined() const { return symbolKind == DefinedKind; }
  bool isShared() const { return symbolKind == SharedKind; }
  bool isPlaceholder() const { return symbolKind == PlaceholderKind; }

  bool isLocal() const { return binding == llvm::ELF::STB_LOCAL; }

  bool isLazy() const { return symbolKind == LazyKind; }

  // True if this is an undefined weak symbol. This only works once
  // all input files have been added.
  bool isUndefWeak() const { return isWeak() && isUndefined(); }

  StringRef getName() const { return {nameData, nameSize}; }

  void setName(StringRef s) {
    nameData = s.data();
    nameSize = s.size();
  }

  void parseSymbolVersion(Ctx &);

  // Get the NUL-terminated version suffix ("", "@...", or "@@...").
  //
  // For @@, the name has been truncated by insert(). For @, the name has been
  // truncated by Symbol::parseSymbolVersion(ctx).
  const char *getVersionSuffix() const { return nameData + nameSize; }

  uint32_t getGotIdx(Ctx &ctx) const { return ctx.symAux[auxIdx].gotIdx; }
  uint32_t getPltIdx(Ctx &ctx) const { return ctx.symAux[auxIdx].pltIdx; }
  uint32_t getTlsDescIdx(Ctx &ctx) const {
    return ctx.symAux[auxIdx].tlsDescIdx;
  }
  uint32_t getTlsGdIdx(Ctx &ctx) const { return ctx.symAux[auxIdx].tlsGdIdx; }

  bool isInGot(Ctx &ctx) const { return getGotIdx(ctx) != uint32_t(-1); }
  bool isInPlt(Ctx &ctx) const { return getPltIdx(ctx) != uint32_t(-1); }

  uint64_t getVA(Ctx &, int64_t addend = 0) const;

  uint64_t getGotOffset(Ctx &) const;
  uint64_t getGotVA(Ctx &) const;
  uint64_t getGotPltOffset(Ctx &) const;
  uint64_t getGotPltVA(Ctx &) const;
  uint64_t getPltOffset(Ctx &) const;
  uint64_t getPltVA(Ctx &) const;
  uint64_t getSize() const;
  OutputSection *getOutputSection() const;

  // The following two functions are used for symbol resolution.
  //
  // You are expected to call mergeProperties for all symbols in input
  // files so that attributes that are attached to names rather than
  // indivisual symbol (such as visibility) are merged together.
  //
  // Every time you read a new symbol from an input, you are supposed
  // to call resolve() with the new symbol. That function replaces
  // "this" object as a result of name resolution if the new symbol is
  // more appropriate to be included in the output.
  //
  // For example, if "this" is an undefined symbol and a new symbol is
  // a defined symbol, "this" is replaced with the new symbol.
  //
  // Resolution is order-independent: the definition with the lowest rank
  // wins, so files can be resolved in parallel and in any order. resolve()
  // never extracts an archive member. The Undefined overload returns true if
  // the caller has to follow up serially: a non-weak reference to a lazy
  // symbol (extract the member) or a non-default visibility reference to a
  // shared symbol (demote it to Undefined).
  void mergeProperties(const Symbol &other);
  bool resolve(Ctx &, const Undefined &other);
  void resolve(Ctx &, const CommonSymbol &other);
  void resolve(Ctx &, const Defined &other);
  void resolve(Ctx &, const LazySymbol &other);
  void resolve(Ctx &, const SharedSymbol &other);

  // If this is a lazy symbol, extract an input file and add the symbol
  // in the file to the symbol table. Calling this function on
  // non-lazy object causes a runtime error.
  void extract(Ctx &) const;

  void checkDuplicate(Ctx &, const Defined &other) const;

  // Replace a shared symbol with `ref`, a reference that it cannot satisfy.
  void demote(const Undefined &ref);

  void lock() {
    while (flags.fetch_or(SYM_LOCKED, std::memory_order_acquire) & SYM_LOCKED)
      while (flags.load(std::memory_order_relaxed) & SYM_LOCKED)
        std::this_thread::yield();
  }
  // Only the lock holder changes the other bits.
  void unlock() {
    flags.store(flags.load(std::memory_order_relaxed) & ~SYM_LOCKED,
                std::memory_order_release);
  }

private:
  // Lower is better. The high bits are the kind of definition and the low 32
  // bits are the priority of the file.
  uint64_t getRank() const;

protected:
  Symbol(Kind k, InputFile *file, StringRef name, uint8_t binding,
         uint8_t stOther, uint8_t type)
      : file(file), nameData(name.data()), nameSize(name.size()), type(type),
        binding(binding), stOther(stOther), symbolKind(k), isPreemptible(false),
        isUsedInRegularObj(false), isExported(false), ltoCanOmit(false),
        traced(false), hasVersionSuffix(false), inserted(false),
        isInIplt(false), gotInIgot(false), folded(false),
        archSpecificBit(false), scriptDefined(false), dsoDefined(false),
        dsoProtected(false), versionScriptAssigned(false), thunkAccessed(false),
        inDynamicList(false), referenced(false), referencedAfterWrap(false) {}

  void overwrite(Symbol &sym, Kind k) const {
    if (sym.traced)
      printTraceSymbol(*this, sym.getName());
    sym.file = file;
    sym.type = type;
    sym.binding = binding;
    sym.stOther = (stOther & ~3) | sym.visibility();
    sym.symbolKind = k;
  }

public:
  // True if this symbol is in the Iplt sub-section of the Plt and the Igot
  // sub-section of the .got.plt or .got.
  LLVM_PREFERRED_TYPE(bool)
  uint8_t isInIplt : 1;

  // True if this symbol needs a GOT entry and its GOT entry is actually in
  // Igot. This will be true only for certain non-preemptible ifuncs.
  LLVM_PREFERRED_TYPE(bool)
  uint8_t gotInIgot : 1;

  // True if defined relative to a section discarded by ICF.
  LLVM_PREFERRED_TYPE(bool)
  uint8_t folded : 1;

  // Allow reuse of a bit between architecture-exclusive symbol flags.
  // - needsTocRestore(): On PPC64, true if a call to this symbol needs to be
  //   followed by a restore of the toc pointer.
  // - isTagged(): On AArch64, true if the symbol needs special relocation and
  //   metadata semantics because it's tagged, under the AArch64 MemtagABI.
  LLVM_PREFERRED_TYPE(bool)
  uint8_t archSpecificBit : 1;
  bool needsTocRestore() const { return archSpecificBit; }
  bool isTagged() const { return archSpecificBit; }
  void setNeedsTocRestore(bool v) { archSpecificBit = v; }
  void setIsTagged(bool v) {
    archSpecificBit = v;
  }

  // True if this symbol is defined by a symbol assignment or wrapped by --wrap.
  //
  // LTO shouldn't inline the symbol because it doesn't know the final content
  // of the symbol.
  LLVM_PREFERRED_TYPE(bool)
  uint8_t scriptDefined : 1;

  // True if defined in a DSO. There may also be a definition in a relocatable
  // object file.
  LLVM_PREFERRED_TYPE(bool)
  uint8_t dsoDefined : 1;

  // True if defined in a DSO as protected visibility.
  LLVM_PREFERRED_TYPE(bool)
  uint8_t dsoProtected : 1;

  // Temporary flags used to communicate which symbol entries need PLT and GOT
  // entries during postScanRelocations();
  std::atomic<uint16_t> flags = 0;

  // A ctx.symAux index used to access GOT/PLT entry indexes. This is allocated
  // in postScanRelocations().
  //
  // While input files are parsed, and before the symbol is added to
  // SymbolTable::getSymbols(), this is the lowest priority among the files
  // that touch the symbol.
  std::atomic<uint32_t> auxIdx = 0;
  // Until the last file has been parsed, if the name is a COMDAT group
  // signature, this is the lowest priority among the files that contain the
  // group.
  std::atomic<uint32_t> dynsymIndex = 0;

  // If `file` is SharedFile (for SharedSymbol or copy-relocated Defined), this
  // represents the Verdef index within the input DSO, which will be converted
  // to a Verneed index in the output. Otherwise, this represents the Verdef
  // index (VER_NDX_LOCAL, VER_NDX_GLOBAL, or a named version).
  // VER_NDX_LOCAL indicates a defined symbol that has been localized by a
  // version script's local: directive or --exclude-libs.
  uint16_t versionId = 0;
  LLVM_PREFERRED_TYPE(bool)
  uint8_t versionScriptAssigned : 1;

  // True if targeted by a range extension thunk.
  LLVM_PREFERRED_TYPE(bool)
  uint8_t thunkAccessed : 1;

  // True if the symbol is in the --dynamic-list file. A Defined symbol with
  // protected or default visibility with this property is required to be
  // exported into .dynsym.
  LLVM_PREFERRED_TYPE(bool)
  uint8_t inDynamicList : 1;

  // Used to track if there has been at least one undefined reference to the
  // symbol. For Undefined and SharedSymbol, the binding may change to STB_WEAK
  // if the first undefined reference from a non-shared object is weak.
  LLVM_PREFERRED_TYPE(bool)
  uint8_t referenced : 1;

  // Used to track if this symbol will be referenced after wrapping is performed
  // (i.e. this will be true for foo if __real_foo is referenced, and will be
  // true for __wrap_foo if foo is referenced).
  LLVM_PREFERRED_TYPE(bool)
  uint8_t referencedAfterWrap : 1;

  // True if this symbol is referenced by the embedded unoptimized part of
  // dynamic debugging.
  LLVM_PREFERRED_TYPE(bool)
  uint8_t isDynDbgRef : 1;

  void setFlags(uint16_t bits) {
    flags.fetch_or(bits, std::memory_order_relaxed);
  }
  bool hasFlag(uint16_t bit) const {
    assert(llvm::has_single_bit(bit) && "bit must be a power of 2");
    return flags.load(std::memory_order_relaxed) & bit;
  }

  bool needsDynReloc() const {
    return flags.load(std::memory_order_relaxed) &
           (NEEDS_COPY | NEEDS_GOT | NEEDS_PLT | NEEDS_TLSDESC | NEEDS_TLSGD |
            NEEDS_GOT_DTPREL | NEEDS_TLSIE | NEEDS_GOT_AUTH |
            NEEDS_TLSDESC_AUTH);
  }
  void allocateAux(Ctx &ctx) {
    assert(auxIdx == 0);
    auxIdx.store(ctx.symAux.size(), std::memory_order_relaxed);
    ctx.symAux.emplace_back();
  }

  bool isSection() const { return type == llvm::ELF::STT_SECTION; }
  bool isTls() const { return type == llvm::ELF::STT_TLS; }
  bool isFunc() const { return type == llvm::ELF::STT_FUNC; }
  bool isGnuIFunc() const { return type == llvm::ELF::STT_GNU_IFUNC; }
  bool isObject() const { return type == llvm::ELF::STT_OBJECT; }
  bool isFile() const { return type == llvm::ELF::STT_FILE; }
};

// Represents a symbol that is defined in the current output file.
class Defined : public Symbol {
public:
  Defined(InputFile *file, StringRef name, uint8_t binding, uint8_t stOther,
          uint8_t type, uint64_t value, uint64_t size, SectionBase *section)
      : Symbol(DefinedKind, file, name, binding, stOther, type), value(value),
        size(size), section(section) {}
  void overwrite(Symbol &sym) const;

  static bool classof(const Symbol *s) { return s->isDefined(); }

  uint64_t value;
  uint64_t size;
  SectionBase *section;
};

// Represents a common symbol.
//
// On Unix, it is traditionally allowed to write variable definitions
// without initialization expressions (such as "int foo;") to header
// files. Such definition is called "tentative definition".
//
// Using tentative definition is usually considered a bad practice
// because you should write only declarations (such as "extern int
// foo;") to header files. Nevertheless, the linker and the compiler
// have to do something to support bad code by allowing duplicate
// definitions for this particular case.
//
// Common symbols represent variable definitions without initializations.
// The compiler creates common symbols when it sees variable definitions
// without initialization (you can suppress this behavior and let the
// compiler create a regular defined symbol by -fno-common).
//
// The linker allows common symbols to be replaced by regular defined
// symbols. If there are remaining common symbols after name resolution is
// complete, they are converted to regular defined symbols in a .bss
// section. (Therefore, the later passes don't see any CommonSymbols.)
class CommonSymbol : public Symbol {
public:
  CommonSymbol(InputFile *file, StringRef name, uint8_t binding,
               uint8_t stOther, uint8_t type, uint64_t alignment, uint64_t size)
      : Symbol(CommonKind, file, name, binding, stOther, type),
        alignment(alignment), size(size) {}
  void overwrite(Symbol &sym) const {
    Symbol::overwrite(sym, CommonKind);
    auto &s = static_cast<CommonSymbol &>(sym);
    s.alignment = alignment;
    s.size = size;
    s.dsoSize = dsoSize;
  }

  static bool classof(const Symbol *s) { return s->isCommon(); }

  uint32_t alignment;
  uint64_t size;
  // st_size of a shared definition, which may be larger. It is kept separate
  // so that the largest common symbol can be picked in any order.
  uint64_t dsoSize = 0;
};

class Undefined : public Symbol {
public:
  Undefined(InputFile *file, StringRef name, uint8_t binding, uint8_t stOther,
            uint8_t type, uint32_t discardedSecIdx = 0)
      : Symbol(UndefinedKind, file, name, binding, stOther, type),
        discardedSecIdx(discardedSecIdx) {}
  void overwrite(Symbol &sym) const {
    Symbol::overwrite(sym, UndefinedKind);
    auto &s = static_cast<Undefined &>(sym);
    s.discardedSecIdx = discardedSecIdx;
    s.nonPrevailing = nonPrevailing;
  }

  static bool classof(const Symbol *s) { return s->kind() == UndefinedKind; }

  // The section index if in a discarded section, 0 otherwise.
  uint32_t discardedSecIdx;
  bool nonPrevailing = false;
};

class SharedSymbol : public Symbol {
public:
  static bool classof(const Symbol *s) { return s->kind() == SharedKind; }

  SharedSymbol(InputFile &file, StringRef name, uint8_t binding,
               uint8_t stOther, uint8_t type, uint64_t value, uint64_t size,
               uint32_t alignment)
      : Symbol(SharedKind, &file, name, binding, stOther, type), value(value),
        size(size), alignment(alignment) {
    dsoProtected = visibility() == llvm::ELF::STV_PROTECTED;
    // GNU ifunc is a mechanism to allow user-supplied functions to
    // resolve PLT slot values at load-time. This is contrary to the
    // regular symbol resolution scheme in which symbols are resolved just
    // by name. Using this hook, you can program how symbols are solved
    // for you program. For example, you can make "memcpy" to be resolved
    // to a SSE-enabled version of memcpy only when a machine running the
    // program supports the SSE instruction set.
    //
    // Naturally, such symbols should always be called through their PLT
    // slots. What GNU ifunc symbols point to are resolver functions, and
    // calling them directly doesn't make sense (unless you are writing a
    // loader).
    //
    // For DSO symbols, we always call them through PLT slots anyway.
    // So there's no difference between GNU ifunc and regular function
    // symbols if they are in DSOs. So we can handle GNU_IFUNC as FUNC.
    if (this->type == llvm::ELF::STT_GNU_IFUNC)
      this->type = llvm::ELF::STT_FUNC;
  }
  void overwrite(Symbol &sym) const {
    Symbol::overwrite(sym, SharedKind);
    auto &s = static_cast<SharedSymbol &>(sym);
    s.dsoProtected = dsoProtected;
    s.value = value;
    s.size = size;
    s.alignment = alignment;
    s.lazyPriority = lazyPriority;
  }

  uint64_t value; // st_value
  uint64_t size;  // st_size
  uint32_t alignment;
  // If nonzero, the lowest priority among the lazy files that define the
  // symbol. A reference that the shared object cannot satisfy, or a non-weak
  // reference that precedes the shared object like the lazy file does,
  // extracts that file.
  uint32_t lazyPriority = 0;
  bool precededByLazy() const;
};

// LazySymbol symbols represent symbols in object files between --start-lib and
// --end-lib options. LLD also handles traditional archives as if all the files
// in the archive are surrounded by --start-lib and --end-lib.
//
// A special complication is the handling of weak undefined symbols. They should
// not load a file, but we have to remember we have seen both the weak undefined
// and the lazy. We represent that with a lazy symbol with a weak binding. This
// means that code looking for undefined symbols normally also has to take lazy
// symbols into consideration.
class LazySymbol : public Symbol {
public:
  LazySymbol(InputFile &file)
      : Symbol(LazyKind, &file, {}, llvm::ELF::STB_GLOBAL,
               llvm::ELF::STV_DEFAULT, llvm::ELF::STT_NOTYPE) {}
  void overwrite(Symbol &sym) const { Symbol::overwrite(sym, LazyKind); }

  static bool classof(const Symbol *s) { return s->kind() == LazyKind; }
};

// A buffer class that is large enough to hold any Symbol-derived
// object. We allocate memory using this class and instantiate a symbol
// using the placement new.

// It is important to keep the size of SymbolUnion small for performance and
// memory usage reasons. 64 bytes is a soft limit based on the size of Defined
// on a 64-bit system. This is enforced by a static_assert in Symbols.cpp.
union SymbolUnion {
  alignas(Defined) char a[sizeof(Defined)];
  alignas(CommonSymbol) char b[sizeof(CommonSymbol)];
  alignas(Undefined) char c[sizeof(Undefined)];
  alignas(SharedSymbol) char d[sizeof(SharedSymbol)];
  alignas(LazySymbol) char e[sizeof(LazySymbol)];
};

template <typename... T> Defined *makeDefined(T &&...args) {
  auto *sym = getSpecificAllocSingleton<SymbolUnion>().Allocate();
  auto &s = *new (reinterpret_cast<Defined *>(sym)) Defined(std::forward<T>(args)...);
  return &s;
}

void reportDuplicate(Ctx &, const Symbol &sym, const InputFile *newFile,
                     InputSectionBase *errSec, uint64_t errOffset);
void maybeWarnUnorderableSymbol(Ctx &, const Symbol *sym);
// Record for --why-extract that `reference` extracts `extracted` through `sym`.
void recordWhyExtract(Ctx &, const InputFile *reference,
                      const InputFile &extracted, const Symbol &sym);
bool computeIsPreemptible(Ctx &, const Symbol &sym);
void parseVersionAndComputeIsPreemptible(Ctx &);

} // namespace lld::elf

#endif
