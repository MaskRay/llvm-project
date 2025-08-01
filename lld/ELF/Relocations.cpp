//===- Relocations.cpp ----------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file contains platform-independent functions to process relocations.
// I'll describe the overview of this file here.
//
// Simple relocations are easy to handle for the linker. For example,
// for R_X86_64_PC64 relocs, the linker just has to fix up locations
// with the relative offsets to the target symbols. It would just be
// reading records from relocation sections and applying them to output.
//
// But not all relocations are that easy to handle. For example, for
// R_386_GOTOFF relocs, the linker has to create new GOT entries for
// symbols if they don't exist, and fix up locations with GOT entry
// offsets from the beginning of GOT section. So there is more than
// fixing addresses in relocation processing.
//
// ELF defines a large number of complex relocations.
//
// The functions in this file analyze relocations and do whatever needs
// to be done. It includes, but not limited to, the following.
//
//  - create GOT/PLT entries
//  - create new relocations in .dynsym to let the dynamic linker resolve
//    them at runtime (since ELF supports dynamic linking, not all
//    relocations can be resolved at link-time)
//  - create COPY relocs and reserve space in .bss
//  - replace expensive relocs (in terms of runtime cost) with cheap ones
//  - error out infeasible combinations such as PIC and non-relative relocs
//
// Note that the functions in this file don't actually apply relocations
// because it doesn't know about the output file nor the output file buffer.
// It instead stores Relocation objects to InputSection's Relocations
// vector to let it apply later in InputSection::writeTo.
//
//===----------------------------------------------------------------------===//

#include "Relocations.h"
#include "Config.h"
#include "InputFiles.h"
#include "LinkerScript.h"
#include "OutputSections.h"
#include "SymbolTable.h"
#include "Symbols.h"
#include "SyntheticSections.h"
#include "Target.h"
#include "Thunks.h"
#include "lld/Common/ErrorHandler.h"
#include "lld/Common/Memory.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/Demangle/Demangle.h"
#include <algorithm>

using namespace llvm;
using namespace llvm::ELF;
using namespace llvm::object;
using namespace llvm::support::endian;
using namespace lld;
using namespace lld::elf;

static void printDefinedLocation(ELFSyncStream &s, const Symbol &sym) {
  s << "\n>>> defined in " << sym.file;
}

// Construct a message in the following format.
//
// >>> defined in /home/alice/src/foo.o
// >>> referenced by bar.c:12 (/home/alice/src/bar.c:12)
// >>>               /home/alice/src/bar.o:(.text+0x1)
static void printLocation(ELFSyncStream &s, InputSectionBase &sec,
                          const Symbol &sym, uint64_t off) {
  printDefinedLocation(s, sym);
  s << "\n>>> referenced by ";
  auto tell = s.tell();
  s << sec.getSrcMsg(sym, off);
  if (tell != s.tell())
    s << "\n>>>               ";
  s << sec.getObjMsg(off);
}

void elf::reportRangeError(Ctx &ctx, uint8_t *loc, const Relocation &rel,
                           const Twine &v, int64_t min, uint64_t max) {
  ErrorPlace errPlace = getErrorPlace(ctx, loc);
  auto diag = Err(ctx);
  diag << errPlace.loc << "relocation " << rel.type
       << " out of range: " << v.str() << " is not in [" << min << ", " << max
       << ']';

  if (rel.sym) {
    if (!rel.sym->isSection())
      diag << "; references '" << rel.sym << '\'';
    else if (auto *d = dyn_cast<Defined>(rel.sym))
      diag << "; references section '" << d->section->name << "'";

    if (ctx.arg.emachine == EM_X86_64 && rel.type == R_X86_64_PC32 &&
        rel.sym->getOutputSection() &&
        (rel.sym->getOutputSection()->flags & SHF_X86_64_LARGE)) {
      diag << "; R_X86_64_PC32 should not reference a section marked "
              "SHF_X86_64_LARGE";
    }
  }
  if (!errPlace.srcLoc.empty())
    diag << "\n>>> referenced by " << errPlace.srcLoc;
  if (rel.sym && !rel.sym->isSection())
    printDefinedLocation(diag, *rel.sym);

  if (errPlace.isec && errPlace.isec->name.starts_with(".debug"))
    diag << "; consider recompiling with -fdebug-types-section to reduce size "
            "of debug sections";
}

void elf::reportRangeError(Ctx &ctx, uint8_t *loc, int64_t v, int n,
                           const Symbol &sym, const Twine &msg) {
  auto diag = Err(ctx);
  diag << getErrorPlace(ctx, loc).loc << msg << " is out of range: " << v
       << " is not in [" << llvm::minIntN(n) << ", " << llvm::maxIntN(n) << "]";
  if (!sym.getName().empty()) {
    diag << "; references '" << &sym << '\'';
    printDefinedLocation(diag, sym);
  }
}

// Build a bitmask with one bit set for each 64 subset of RelExpr.
static constexpr uint64_t buildMask() { return 0; }

template <typename... Tails>
static constexpr uint64_t buildMask(int head, Tails... tails) {
  return (0 <= head && head < 64 ? uint64_t(1) << head : 0) |
         buildMask(tails...);
}

// Return true if `Expr` is one of `Exprs`.
// There are more than 64 but less than 128 RelExprs, so we divide the set of
// exprs into [0, 64) and [64, 128) and represent each range as a constant
// 64-bit mask. Then we decide which mask to test depending on the value of
// expr and use a simple shift and bitwise-and to test for membership.
template <RelExpr... Exprs> static bool oneof(RelExpr expr) {
  assert(0 <= expr && (int)expr < 128 &&
         "RelExpr is too large for 128-bit mask!");

  if (expr >= 64)
    return (uint64_t(1) << (expr - 64)) & buildMask((Exprs - 64)...);
  return (uint64_t(1) << expr) & buildMask(Exprs...);
}

static RelType getMipsPairType(RelType type, bool isLocal) {
  switch (type) {
  case R_MIPS_HI16:
    return R_MIPS_LO16;
  case R_MIPS_GOT16:
    // In case of global symbol, the R_MIPS_GOT16 relocation does not
    // have a pair. Each global symbol has a unique entry in the GOT
    // and a corresponding instruction with help of the R_MIPS_GOT16
    // relocation loads an address of the symbol. In case of local
    // symbol, the R_MIPS_GOT16 relocation creates a GOT entry to hold
    // the high 16 bits of the symbol's value. A paired R_MIPS_LO16
    // relocations handle low 16 bits of the address. That allows
    // to allocate only one GOT entry for every 64 KiB of local data.
    return isLocal ? R_MIPS_LO16 : R_MIPS_NONE;
  case R_MICROMIPS_GOT16:
    return isLocal ? R_MICROMIPS_LO16 : R_MIPS_NONE;
  case R_MIPS_PCHI16:
    return R_MIPS_PCLO16;
  case R_MICROMIPS_HI16:
    return R_MICROMIPS_LO16;
  default:
    return R_MIPS_NONE;
  }
}

// True if non-preemptable symbol always has the same value regardless of where
// the DSO is loaded.
static bool isAbsolute(const Symbol &sym) {
  if (sym.isUndefined())
    return true;
  if (const auto *dr = dyn_cast<Defined>(&sym))
    return dr->section == nullptr; // Absolute symbol.
  return false;
}

static bool isAbsoluteValue(const Symbol &sym) {
  return isAbsolute(sym) || sym.isTls();
}

// Returns true if Expr refers a PLT entry.
static bool needsPlt(RelExpr expr) {
  return oneof<R_PLT, R_PLT_PC, R_PLT_GOTREL, R_PLT_GOTPLT, R_GOTPLT_GOTREL,
               R_GOTPLT_PC, RE_LOONGARCH_PLT_PAGE_PC, RE_PPC32_PLTREL,
               RE_PPC64_CALL_PLT>(expr);
}

bool lld::elf::needsGot(RelExpr expr) {
  return oneof<R_GOT, RE_AARCH64_AUTH_GOT, RE_AARCH64_AUTH_GOT_PC, R_GOT_OFF,
               RE_MIPS_GOT_LOCAL_PAGE, RE_MIPS_GOT_OFF, RE_MIPS_GOT_OFF32,
               RE_AARCH64_GOT_PAGE_PC, RE_AARCH64_AUTH_GOT_PAGE_PC,
               RE_AARCH64_AUTH_GOT_PAGE_PC, R_GOT_PC, R_GOTPLT,
               RE_AARCH64_GOT_PAGE, RE_LOONGARCH_GOT, RE_LOONGARCH_GOT_PAGE_PC>(
      expr);
}

// True if this expression is of the form Sym - X, where X is a position in the
// file (PC, or GOT for example).
static bool isRelExpr(RelExpr expr) {
  return oneof<R_PC, R_GOTREL, R_GOTPLTREL, RE_ARM_PCA, RE_MIPS_GOTREL,
               RE_PPC64_CALL, RE_PPC64_RELAX_TOC, RE_AARCH64_PAGE_PC,
               R_RELAX_GOT_PC, RE_RISCV_PC_INDIRECT, RE_PPC64_RELAX_GOT_PC,
               RE_LOONGARCH_PAGE_PC>(expr);
}

static RelExpr toPlt(RelExpr expr) {
  switch (expr) {
  case RE_LOONGARCH_PAGE_PC:
    return RE_LOONGARCH_PLT_PAGE_PC;
  case RE_PPC64_CALL:
    return RE_PPC64_CALL_PLT;
  case R_PC:
    return R_PLT_PC;
  case R_ABS:
    return R_PLT;
  case R_GOTREL:
    return R_PLT_GOTREL;
  default:
    return expr;
  }
}

static RelExpr fromPlt(RelExpr expr) {
  // We decided not to use a plt. Optimize a reference to the plt to a
  // reference to the symbol itself.
  switch (expr) {
  case R_PLT_PC:
  case RE_PPC32_PLTREL:
    return R_PC;
  case RE_LOONGARCH_PLT_PAGE_PC:
    return RE_LOONGARCH_PAGE_PC;
  case RE_PPC64_CALL_PLT:
    return RE_PPC64_CALL;
  case R_PLT:
    return R_ABS;
  case R_PLT_GOTPLT:
    return R_GOTPLTREL;
  case R_PLT_GOTREL:
    return R_GOTREL;
  default:
    return expr;
  }
}

// Returns true if a given shared symbol is in a read-only segment in a DSO.
template <class ELFT> static bool isReadOnly(SharedSymbol &ss) {
  using Elf_Phdr = typename ELFT::Phdr;

  // Determine if the symbol is read-only by scanning the DSO's program headers.
  const auto &file = cast<SharedFile>(*ss.file);
  for (const Elf_Phdr &phdr :
       check(file.template getObj<ELFT>().program_headers()))
    if ((phdr.p_type == ELF::PT_LOAD || phdr.p_type == ELF::PT_GNU_RELRO) &&
        !(phdr.p_flags & ELF::PF_W) && ss.value >= phdr.p_vaddr &&
        ss.value < phdr.p_vaddr + phdr.p_memsz)
      return true;
  return false;
}

// Returns symbols at the same offset as a given symbol, including SS itself.
//
// If two or more symbols are at the same offset, and at least one of
// them are copied by a copy relocation, all of them need to be copied.
// Otherwise, they would refer to different places at runtime.
template <class ELFT>
static SmallSet<SharedSymbol *, 4> getSymbolsAt(Ctx &ctx, SharedSymbol &ss) {
  using Elf_Sym = typename ELFT::Sym;

  const auto &file = cast<SharedFile>(*ss.file);

  SmallSet<SharedSymbol *, 4> ret;
  for (const Elf_Sym &s : file.template getGlobalELFSyms<ELFT>()) {
    if (s.st_shndx == SHN_UNDEF || s.st_shndx == SHN_ABS ||
        s.getType() == STT_TLS || s.st_value != ss.value)
      continue;
    StringRef name = check(s.getName(file.getStringTable()));
    Symbol *sym = ctx.symtab->find(name);
    if (auto *alias = dyn_cast_or_null<SharedSymbol>(sym))
      ret.insert(alias);
  }

  // The loop does not check SHT_GNU_verneed, so ret does not contain
  // non-default version symbols. If ss has a non-default version, ret won't
  // contain ss. Just add ss unconditionally. If a non-default version alias is
  // separately copy relocated, it and ss will have different addresses.
  // Fortunately this case is impractical and fails with GNU ld as well.
  ret.insert(&ss);
  return ret;
}

// When a symbol is copy relocated or we create a canonical plt entry, it is
// effectively a defined symbol. In the case of copy relocation the symbol is
// in .bss and in the case of a canonical plt entry it is in .plt. This function
// replaces the existing symbol with a Defined pointing to the appropriate
// location.
static void replaceWithDefined(Ctx &ctx, Symbol &sym, SectionBase &sec,
                               uint64_t value, uint64_t size) {
  Symbol old = sym;
  Defined(ctx, sym.file, StringRef(), sym.binding, sym.stOther, sym.type, value,
          size, &sec)
      .overwrite(sym);

  sym.versionId = old.versionId;
  sym.isUsedInRegularObj = true;
  // A copy relocated alias may need a GOT entry.
  sym.flags.store(old.flags.load(std::memory_order_relaxed) & NEEDS_GOT,
                  std::memory_order_relaxed);
}

// Reserve space in .bss or .bss.rel.ro for copy relocation.
//
// The copy relocation is pretty much a hack. If you use a copy relocation
// in your program, not only the symbol name but the symbol's size, RW/RO
// bit and alignment become part of the ABI. In addition to that, if the
// symbol has aliases, the aliases become part of the ABI. That's subtle,
// but if you violate that implicit ABI, that can cause very counter-
// intuitive consequences.
//
// So, what is the copy relocation? It's for linking non-position
// independent code to DSOs. In an ideal world, all references to data
// exported by DSOs should go indirectly through GOT. But if object files
// are compiled as non-PIC, all data references are direct. There is no
// way for the linker to transform the code to use GOT, as machine
// instructions are already set in stone in object files. This is where
// the copy relocation takes a role.
//
// A copy relocation instructs the dynamic linker to copy data from a DSO
// to a specified address (which is usually in .bss) at load-time. If the
// static linker (that's us) finds a direct data reference to a DSO
// symbol, it creates a copy relocation, so that the symbol can be
// resolved as if it were in .bss rather than in a DSO.
//
// As you can see in this function, we create a copy relocation for the
// dynamic linker, and the relocation contains not only symbol name but
// various other information about the symbol. So, such attributes become a
// part of the ABI.
//
// Note for application developers: I can give you a piece of advice if
// you are writing a shared library. You probably should export only
// functions from your library. You shouldn't export variables.
//
// As an example what can happen when you export variables without knowing
// the semantics of copy relocations, assume that you have an exported
// variable of type T. It is an ABI-breaking change to add new members at
// end of T even though doing that doesn't change the layout of the
// existing members. That's because the space for the new members are not
// reserved in .bss unless you recompile the main program. That means they
// are likely to overlap with other data that happens to be laid out next
// to the variable in .bss. This kind of issue is sometimes very hard to
// debug. What's a solution? Instead of exporting a variable V from a DSO,
// define an accessor getV().
template <class ELFT> static void addCopyRelSymbol(Ctx &ctx, SharedSymbol &ss) {
  // Copy relocation against zero-sized symbol doesn't make sense.
  uint64_t symSize = ss.getSize();
  if (symSize == 0 || ss.alignment == 0)
    Err(ctx) << "cannot create a copy relocation for symbol " << &ss;

  // See if this symbol is in a read-only segment. If so, preserve the symbol's
  // memory protection by reserving space in the .bss.rel.ro section.
  bool isRO = isReadOnly<ELFT>(ss);
  BssSection *sec = make<BssSection>(ctx, isRO ? ".bss.rel.ro" : ".bss",
                                     symSize, ss.alignment);
  OutputSection *osec = (isRO ? ctx.in.bssRelRo : ctx.in.bss)->getParent();

  // At this point, sectionBases has been migrated to sections. Append sec to
  // sections.
  if (osec->commands.empty() ||
      !isa<InputSectionDescription>(osec->commands.back()))
    osec->commands.push_back(make<InputSectionDescription>(""));
  auto *isd = cast<InputSectionDescription>(osec->commands.back());
  isd->sections.push_back(sec);
  osec->commitSection(sec);

  // Look through the DSO's dynamic symbol table for aliases and create a
  // dynamic symbol for each one. This causes the copy relocation to correctly
  // interpose any aliases.
  for (SharedSymbol *sym : getSymbolsAt<ELFT>(ctx, ss))
    replaceWithDefined(ctx, *sym, *sec, 0, sym->size);

  ctx.mainPart->relaDyn->addSymbolReloc(ctx.target->copyRel, *sec, 0, ss);
}

// .eh_frame sections are mergeable input sections, so their input
// offsets are not linearly mapped to output section. For each input
// offset, we need to find a section piece containing the offset and
// add the piece's base address to the input offset to compute the
// output offset. That isn't cheap.
//
// This class is to speed up the offset computation. When we process
// relocations, we access offsets in the monotonically increasing
// order. So we can optimize for that access pattern.
//
// For sections other than .eh_frame, this class doesn't do anything.
namespace {
class OffsetGetter {
public:
  OffsetGetter() = default;
  explicit OffsetGetter(InputSectionBase &sec) {
    if (auto *eh = dyn_cast<EhInputSection>(&sec)) {
      cies = eh->cies;
      fdes = eh->fdes;
      i = cies.begin();
      j = fdes.begin();
    }
  }

  // Translates offsets in input sections to offsets in output sections.
  // Given offset must increase monotonically. We assume that Piece is
  // sorted by inputOff.
  uint64_t get(Ctx &ctx, uint64_t off) {
    if (cies.empty())
      return off;

    while (j != fdes.end() && j->inputOff <= off)
      ++j;
    auto it = j;
    if (j == fdes.begin() || j[-1].inputOff + j[-1].size <= off) {
      while (i != cies.end() && i->inputOff <= off)
        ++i;
      if (i == cies.begin() || i[-1].inputOff + i[-1].size <= off) {
        Err(ctx) << ".eh_frame: relocation is not in any piece";
        return 0;
      }
      it = i;
    }

    // Offset -1 means that the piece is dead (i.e. garbage collected).
    if (it[-1].outputOff == -1)
      return -1;
    return it[-1].outputOff + (off - it[-1].inputOff);
  }

private:
  ArrayRef<EhSectionPiece> cies, fdes;
  ArrayRef<EhSectionPiece>::iterator i, j;
};

// This class encapsulates states needed to scan relocations for one
// InputSectionBase.
class RelocationScanner {
public:
  RelocationScanner(Ctx &ctx) : ctx(ctx) {}
  template <class ELFT>
  void scanSection(InputSectionBase &s, bool isEH = false);

private:
  Ctx &ctx;
  InputSectionBase *sec;
  OffsetGetter getter;

  // End of relocations, used by Mips/PPC64.
  const void *end = nullptr;

  template <class RelTy> RelType getMipsN32RelType(RelTy *&rel) const;
  template <class ELFT, class RelTy>
  int64_t computeMipsAddend(const RelTy &rel, RelExpr expr, bool isLocal) const;
  bool isStaticLinkTimeConstant(RelExpr e, RelType type, const Symbol &sym,
                                uint64_t relOff) const;
  void processAux(RelExpr expr, RelType type, uint64_t offset, Symbol &sym,
                  int64_t addend) const;
  unsigned handleTlsRelocation(RelExpr expr, RelType type, uint64_t offset,
                               Symbol &sym, int64_t addend);

  template <class ELFT, class RelTy>
  void scanOne(typename Relocs<RelTy>::const_iterator &i);
  template <class ELFT, class RelTy> void scan(Relocs<RelTy> rels);
};
} // namespace

// MIPS has an odd notion of "paired" relocations to calculate addends.
// For example, if a relocation is of R_MIPS_HI16, there must be a
// R_MIPS_LO16 relocation after that, and an addend is calculated using
// the two relocations.
template <class ELFT, class RelTy>
int64_t RelocationScanner::computeMipsAddend(const RelTy &rel, RelExpr expr,
                                             bool isLocal) const {
  if (expr == RE_MIPS_GOTREL && isLocal)
    return sec->getFile<ELFT>()->mipsGp0;

  // The ABI says that the paired relocation is used only for REL.
  // See p. 4-17 at ftp://www.linux-mips.org/pub/linux/mips/doc/ABI/mipsabi.pdf
  // This generalises to relocation types with implicit addends.
  if (RelTy::HasAddend)
    return 0;

  RelType type = rel.getType(ctx.arg.isMips64EL);
  RelType pairTy = getMipsPairType(type, isLocal);
  if (pairTy == R_MIPS_NONE)
    return 0;

  const uint8_t *buf = sec->content().data();
  uint32_t symIndex = rel.getSymbol(ctx.arg.isMips64EL);

  // To make things worse, paired relocations might not be contiguous in
  // the relocation table, so we need to do linear search. *sigh*
  for (const RelTy *ri = &rel; ri != static_cast<const RelTy *>(end); ++ri)
    if (ri->getType(ctx.arg.isMips64EL) == pairTy &&
        ri->getSymbol(ctx.arg.isMips64EL) == symIndex)
      return ctx.target->getImplicitAddend(buf + ri->r_offset, pairTy);

  Warn(ctx) << "can't find matching " << pairTy << " relocation for " << type;
  return 0;
}

// Custom error message if Sym is defined in a discarded section.
template <class ELFT>
static void maybeReportDiscarded(Ctx &ctx, ELFSyncStream &msg, Undefined &sym) {
  auto *file = dyn_cast<ObjFile<ELFT>>(sym.file);
  if (!file || !sym.discardedSecIdx)
    return;
  ArrayRef<typename ELFT::Shdr> objSections =
      file->template getELFShdrs<ELFT>();

  if (sym.type == ELF::STT_SECTION) {
    msg << "relocation refers to a discarded section: ";
    msg << CHECK2(
        file->getObj().getSectionName(objSections[sym.discardedSecIdx]), file);
  } else {
    msg << "relocation refers to a symbol in a discarded section: " << &sym;
  }
  msg << "\n>>> defined in " << file;

  Elf_Shdr_Impl<ELFT> elfSec = objSections[sym.discardedSecIdx - 1];
  if (elfSec.sh_type != SHT_GROUP)
    return;

  // If the discarded section is a COMDAT.
  StringRef signature = file->getShtGroupSignature(objSections, elfSec);
  if (const InputFile *prevailing =
          ctx.symtab->comdatGroups.lookup(CachedHashStringRef(signature))) {
    msg << "\n>>> section group signature: " << signature
        << "\n>>> prevailing definition is in " << prevailing;
    if (sym.nonPrevailing) {
      msg << "\n>>> or the symbol in the prevailing group had STB_WEAK "
             "binding and the symbol in a non-prevailing group had STB_GLOBAL "
             "binding. Mixing groups with STB_WEAK and STB_GLOBAL binding "
             "signature is not supported";
    }
  }
}

// Check whether the definition name def is a mangled function name that matches
// the reference name ref.
static bool canSuggestExternCForCXX(StringRef ref, StringRef def) {
  llvm::ItaniumPartialDemangler d;
  std::string name = def.str();
  if (d.partialDemangle(name.c_str()))
    return false;
  char *buf = d.getFunctionName(nullptr, nullptr);
  if (!buf)
    return false;
  bool ret = ref == buf;
  free(buf);
  return ret;
}

// Suggest an alternative spelling of an "undefined symbol" diagnostic. Returns
// the suggested symbol, which is either in the symbol table, or in the same
// file of sym.
static const Symbol *getAlternativeSpelling(Ctx &ctx, const Undefined &sym,
                                            std::string &pre_hint,
                                            std::string &post_hint) {
  DenseMap<StringRef, const Symbol *> map;
  if (sym.file->kind() == InputFile::ObjKind) {
    auto *file = cast<ELFFileBase>(sym.file);
    // If sym is a symbol defined in a discarded section, maybeReportDiscarded()
    // will give an error. Don't suggest an alternative spelling.
    if (sym.discardedSecIdx != 0 &&
        file->getSections()[sym.discardedSecIdx] == &InputSection::discarded)
      return nullptr;

    // Build a map of local defined symbols.
    for (const Symbol *s : sym.file->getSymbols())
      if (s->isLocal() && s->isDefined() && !s->getName().empty())
        map.try_emplace(s->getName(), s);
  }

  auto suggest = [&](StringRef newName) -> const Symbol * {
    // If defined locally.
    if (const Symbol *s = map.lookup(newName))
      return s;

    // If in the symbol table and not undefined.
    if (const Symbol *s = ctx.symtab->find(newName))
      if (!s->isUndefined())
        return s;

    return nullptr;
  };

  // This loop enumerates all strings of Levenshtein distance 1 as typo
  // correction candidates and suggests the one that exists as a non-undefined
  // symbol.
  StringRef name = sym.getName();
  for (size_t i = 0, e = name.size(); i != e + 1; ++i) {
    // Insert a character before name[i].
    std::string newName = (name.substr(0, i) + "0" + name.substr(i)).str();
    for (char c = '0'; c <= 'z'; ++c) {
      newName[i] = c;
      if (const Symbol *s = suggest(newName))
        return s;
    }
    if (i == e)
      break;

    // Substitute name[i].
    newName = std::string(name);
    for (char c = '0'; c <= 'z'; ++c) {
      newName[i] = c;
      if (const Symbol *s = suggest(newName))
        return s;
    }

    // Transpose name[i] and name[i+1]. This is of edit distance 2 but it is
    // common.
    if (i + 1 < e) {
      newName[i] = name[i + 1];
      newName[i + 1] = name[i];
      if (const Symbol *s = suggest(newName))
        return s;
    }

    // Delete name[i].
    newName = (name.substr(0, i) + name.substr(i + 1)).str();
    if (const Symbol *s = suggest(newName))
      return s;
  }

  // Case mismatch, e.g. Foo vs FOO.
  for (auto &it : map)
    if (name.equals_insensitive(it.first))
      return it.second;
  for (Symbol *sym : ctx.symtab->getSymbols())
    if (!sym->isUndefined() && name.equals_insensitive(sym->getName()))
      return sym;

  // The reference may be a mangled name while the definition is not. Suggest a
  // missing extern "C".
  if (name.starts_with("_Z")) {
    std::string buf = name.str();
    llvm::ItaniumPartialDemangler d;
    if (!d.partialDemangle(buf.c_str()))
      if (char *buf = d.getFunctionName(nullptr, nullptr)) {
        const Symbol *s = suggest(buf);
        free(buf);
        if (s) {
          pre_hint = ": extern \"C\" ";
          return s;
        }
      }
  } else {
    const Symbol *s = nullptr;
    for (auto &it : map)
      if (canSuggestExternCForCXX(name, it.first)) {
        s = it.second;
        break;
      }
    if (!s)
      for (Symbol *sym : ctx.symtab->getSymbols())
        if (canSuggestExternCForCXX(name, sym->getName())) {
          s = sym;
          break;
        }
    if (s) {
      pre_hint = " to declare ";
      post_hint = " as extern \"C\"?";
      return s;
    }
  }

  return nullptr;
}

static void reportUndefinedSymbol(Ctx &ctx, const UndefinedDiag &undef,
                                  bool correctSpelling) {
  Undefined &sym = *undef.sym;
  ELFSyncStream msg(ctx, DiagLevel::None);

  auto visibility = [&]() {
    switch (sym.visibility()) {
    case STV_INTERNAL:
      return "internal ";
    case STV_HIDDEN:
      return "hidden ";
    case STV_PROTECTED:
      return "protected ";
    default:
      return "";
    }
  };

  switch (ctx.arg.ekind) {
  case ELF32LEKind:
    maybeReportDiscarded<ELF32LE>(ctx, msg, sym);
    break;
  case ELF32BEKind:
    maybeReportDiscarded<ELF32BE>(ctx, msg, sym);
    break;
  case ELF64LEKind:
    maybeReportDiscarded<ELF64LE>(ctx, msg, sym);
    break;
  case ELF64BEKind:
    maybeReportDiscarded<ELF64BE>(ctx, msg, sym);
    break;
  default:
    llvm_unreachable("");
  }
  if (msg.str().empty())
    msg << "undefined " << visibility() << "symbol: " << &sym;

  const size_t maxUndefReferences = 3;
  for (UndefinedDiag::Loc l :
       ArrayRef(undef.locs).take_front(maxUndefReferences)) {
    InputSectionBase &sec = *l.sec;
    uint64_t offset = l.offset;

    msg << "\n>>> referenced by ";
    // In the absence of line number information, utilize DW_TAG_variable (if
    // present) for the enclosing symbol (e.g. var in `int *a[] = {&undef};`).
    Symbol *enclosing = sec.getEnclosingSymbol(offset);

    ELFSyncStream msg1(ctx, DiagLevel::None);
    auto tell = msg.tell();
    msg << sec.getSrcMsg(enclosing ? *enclosing : sym, offset);
    if (tell != msg.tell())
      msg << "\n>>>               ";
    msg << sec.getObjMsg(offset);
  }

  if (maxUndefReferences < undef.locs.size())
    msg << "\n>>> referenced " << (undef.locs.size() - maxUndefReferences)
        << " more times";

  if (correctSpelling) {
    std::string pre_hint = ": ", post_hint;
    if (const Symbol *corrected =
            getAlternativeSpelling(ctx, sym, pre_hint, post_hint)) {
      msg << "\n>>> did you mean" << pre_hint << corrected << post_hint
          << "\n>>> defined in: " << corrected->file;
    }
  }

  if (sym.getName().starts_with("_ZTV"))
    msg << "\n>>> the vtable symbol may be undefined because the class is "
           "missing its key function "
           "(see https://lld.llvm.org/missingkeyfunction)";
  if (ctx.arg.gcSections && ctx.arg.zStartStopGC &&
      sym.getName().starts_with("__start_")) {
    msg << "\n>>> the encapsulation symbol needs to be retained under "
           "--gc-sections properly; consider -z nostart-stop-gc "
           "(see https://lld.llvm.org/ELF/start-stop-gc)";
  }

  if (undef.isWarning)
    Warn(ctx) << msg.str();
  else
    ctx.e.error(msg.str(), ErrorTag::SymbolNotFound, {sym.getName()});
}

void elf::reportUndefinedSymbols(Ctx &ctx) {
  // Find the first "undefined symbol" diagnostic for each diagnostic, and
  // collect all "referenced from" lines at the first diagnostic.
  DenseMap<Symbol *, UndefinedDiag *> firstRef;
  for (UndefinedDiag &undef : ctx.undefErrs) {
    assert(undef.locs.size() == 1);
    if (UndefinedDiag *canon = firstRef.lookup(undef.sym)) {
      canon->locs.push_back(undef.locs[0]);
      undef.locs.clear();
    } else
      firstRef[undef.sym] = &undef;
  }

  // Enable spell corrector for the first 2 diagnostics.
  for (auto [i, undef] : llvm::enumerate(ctx.undefErrs))
    if (!undef.locs.empty())
      reportUndefinedSymbol(ctx, undef, i < 2);
}

// Report an undefined symbol if necessary.
// Returns true if the undefined symbol will produce an error message.
static bool maybeReportUndefined(Ctx &ctx, Undefined &sym,
                                 InputSectionBase &sec, uint64_t offset) {
  std::lock_guard<std::mutex> lock(ctx.relocMutex);
  // If versioned, issue an error (even if the symbol is weak) because we don't
  // know the defining filename which is required to construct a Verneed entry.
  if (sym.hasVersionSuffix) {
    ctx.undefErrs.push_back({&sym, {{&sec, offset}}, false});
    return true;
  }
  if (sym.isWeak())
    return false;

  bool canBeExternal = !sym.isLocal() && sym.visibility() == STV_DEFAULT;
  if (ctx.arg.unresolvedSymbols == UnresolvedPolicy::Ignore && canBeExternal)
    return false;

  // clang (as of 2019-06-12) / gcc (as of 8.2.1) PPC64 may emit a .rela.toc
  // which references a switch table in a discarded .rodata/.text section. The
  // .toc and the .rela.toc are incorrectly not placed in the comdat. The ELF
  // spec says references from outside the group to a STB_LOCAL symbol are not
  // allowed. Work around the bug.
  //
  // PPC32 .got2 is similar but cannot be fixed. Multiple .got2 is infeasible
  // because .LC0-.LTOC is not representable if the two labels are in different
  // .got2
  if (sym.discardedSecIdx != 0 && (sec.name == ".got2" || sec.name == ".toc"))
    return false;

  bool isWarning =
      (ctx.arg.unresolvedSymbols == UnresolvedPolicy::Warn && canBeExternal) ||
      ctx.arg.noinhibitExec;
  ctx.undefErrs.push_back({&sym, {{&sec, offset}}, isWarning});
  return !isWarning;
}

// MIPS N32 ABI treats series of successive relocations with the same offset
// as a single relocation. The similar approach used by N64 ABI, but this ABI
// packs all relocations into the single relocation record. Here we emulate
// this for the N32 ABI. Iterate over relocation with the same offset and put
// theirs types into the single bit-set.
template <class RelTy>
RelType RelocationScanner::getMipsN32RelType(RelTy *&rel) const {
  uint32_t type = 0;
  uint64_t offset = rel->r_offset;

  int n = 0;
  while (rel != static_cast<const RelTy *>(end) && rel->r_offset == offset)
    type |= (rel++)->getType(ctx.arg.isMips64EL) << (8 * n++);
  return type;
}

template <bool shard = false>
static void addRelativeReloc(Ctx &ctx, InputSectionBase &isec,
                             uint64_t offsetInSec, Symbol &sym, int64_t addend,
                             RelExpr expr, RelType type) {
  Partition &part = isec.getPartition(ctx);

  if (sym.isTagged()) {
    part.relaDyn->addRelativeReloc<shard>(ctx.target->relativeRel, isec,
                                          offsetInSec, sym, addend, type, expr);
    // With MTE globals, we always want to derive the address tag by `ldg`-ing
    // the symbol. When we have a RELATIVE relocation though, we no longer have
    // a reference to the symbol. Because of this, when we have an addend that
    // puts the result of the RELATIVE relocation out-of-bounds of the symbol
    // (e.g. the addend is outside of [0, sym.getSize()]), the AArch64 MemtagABI
    // says we should store the offset to the start of the symbol in the target
    // field. This is described in further detail in:
    // https://github.com/ARM-software/abi-aa/blob/main/memtagabielf64/memtagabielf64.rst#841extended-semantics-of-r_aarch64_relative
    if (addend < 0 || static_cast<uint64_t>(addend) >= sym.getSize())
      isec.relocations.push_back({expr, type, offsetInSec, addend, &sym});
    return;
  }

  // Add a relative relocation. If relrDyn section is enabled, and the
  // relocation offset is guaranteed to be even, add the relocation to
  // the relrDyn section, otherwise add it to the relaDyn section.
  // relrDyn sections don't support odd offsets. Also, relrDyn sections
  // don't store the addend values, so we must write it to the relocated
  // address.
  if (part.relrDyn && isec.addralign >= 2 && offsetInSec % 2 == 0) {
    isec.addReloc({expr, type, offsetInSec, addend, &sym});
    if (shard)
      part.relrDyn->relocsVec[parallel::getThreadIndex()].push_back(
          {&isec, isec.relocs().size() - 1});
    else
      part.relrDyn->relocs.push_back({&isec, isec.relocs().size() - 1});
    return;
  }
  part.relaDyn->addRelativeReloc<shard>(ctx.target->relativeRel, isec,
                                        offsetInSec, sym, addend, type, expr);
}

template <class PltSection, class GotPltSection>
static void addPltEntry(Ctx &ctx, PltSection &plt, GotPltSection &gotPlt,
                        RelocationBaseSection &rel, RelType type, Symbol &sym) {
  plt.addEntry(sym);
  gotPlt.addEntry(sym);
  if (sym.isPreemptible)
    rel.addReloc(
        {type, &gotPlt, sym.getGotPltOffset(ctx), true, sym, 0, R_ADDEND});
  else
    rel.addReloc(
        {type, &gotPlt, sym.getGotPltOffset(ctx), false, sym, 0, R_ABS});
}

void elf::addGotEntry(Ctx &ctx, Symbol &sym) {
  ctx.in.got->addEntry(sym);
  uint64_t off = sym.getGotOffset(ctx);

  // If preemptible, emit a GLOB_DAT relocation.
  if (sym.isPreemptible) {
    ctx.mainPart->relaDyn->addReloc(
        {ctx.target->gotRel, ctx.in.got.get(), off, true, sym, 0, R_ADDEND});
    return;
  }

  // Otherwise, the value is either a link-time constant or the load base
  // plus a constant.
  if (!ctx.arg.isPic || isAbsolute(sym))
    ctx.in.got->addConstant({R_ABS, ctx.target->symbolicRel, off, 0, &sym});
  else
    addRelativeReloc(ctx, *ctx.in.got, off, sym, 0, R_ABS,
                     ctx.target->symbolicRel);
}

static void addGotAuthEntry(Ctx &ctx, Symbol &sym) {
  ctx.in.got->addEntry(sym);
  ctx.in.got->addAuthEntry(sym);
  uint64_t off = sym.getGotOffset(ctx);

  // If preemptible, emit a GLOB_DAT relocation.
  if (sym.isPreemptible) {
    ctx.mainPart->relaDyn->addReloc({R_AARCH64_AUTH_GLOB_DAT, ctx.in.got.get(),
                                     off, true, sym, 0, R_ADDEND});
    return;
  }

  // Signed GOT requires dynamic relocation.
  ctx.in.got->getPartition(ctx).relaDyn->addReloc(
      {R_AARCH64_AUTH_RELATIVE, ctx.in.got.get(), off, false, sym, 0, R_ABS});
}

static void addTpOffsetGotEntry(Ctx &ctx, Symbol &sym) {
  ctx.in.got->addEntry(sym);
  uint64_t off = sym.getGotOffset(ctx);
  if (!sym.isPreemptible && !ctx.arg.shared) {
    ctx.in.got->addConstant({R_TPREL, ctx.target->symbolicRel, off, 0, &sym});
    return;
  }
  ctx.mainPart->relaDyn->addAddendOnlyRelocIfNonPreemptible(
      ctx.target->tlsGotRel, *ctx.in.got, off, sym, ctx.target->symbolicRel);
}

// Return true if we can define a symbol in the executable that
// contains the value/function of a symbol defined in a shared
// library.
static bool canDefineSymbolInExecutable(Ctx &ctx, Symbol &sym) {
  // If the symbol has default visibility the symbol defined in the
  // executable will preempt it.
  // Note that we want the visibility of the shared symbol itself, not
  // the visibility of the symbol in the output file we are producing.
  if (!sym.dsoProtected)
    return true;

  // If we are allowed to break address equality of functions, defining
  // a plt entry will allow the program to call the function in the
  // .so, but the .so and the executable will no agree on the address
  // of the function. Similar logic for objects.
  return ((sym.isFunc() && ctx.arg.ignoreFunctionAddressEquality) ||
          (sym.isObject() && ctx.arg.ignoreDataAddressEquality));
}

// Returns true if a given relocation can be computed at link-time.
// This only handles relocation types expected in processAux.
//
// For instance, we know the offset from a relocation to its target at
// link-time if the relocation is PC-relative and refers a
// non-interposable function in the same executable. This function
// will return true for such relocation.
//
// If this function returns false, that means we need to emit a
// dynamic relocation so that the relocation will be fixed at load-time.
bool RelocationScanner::isStaticLinkTimeConstant(RelExpr e, RelType type,
                                                 const Symbol &sym,
                                                 uint64_t relOff) const {
  // These expressions always compute a constant
  if (oneof<
          R_GOTPLT, R_GOT_OFF, R_RELAX_HINT, RE_MIPS_GOT_LOCAL_PAGE,
          RE_MIPS_GOTREL, RE_MIPS_GOT_OFF, RE_MIPS_GOT_OFF32, RE_MIPS_GOT_GP_PC,
          RE_AARCH64_GOT_PAGE_PC, RE_AARCH64_AUTH_GOT_PAGE_PC, R_GOT_PC,
          R_GOTONLY_PC, R_GOTPLTONLY_PC, R_PLT_PC, R_PLT_GOTREL, R_PLT_GOTPLT,
          R_GOTPLT_GOTREL, R_GOTPLT_PC, RE_PPC32_PLTREL, RE_PPC64_CALL_PLT,
          RE_PPC64_RELAX_TOC, RE_RISCV_ADD, RE_AARCH64_GOT_PAGE,
          RE_AARCH64_AUTH_GOT, RE_AARCH64_AUTH_GOT_PC, RE_LOONGARCH_PLT_PAGE_PC,
          RE_LOONGARCH_GOT, RE_LOONGARCH_GOT_PAGE_PC>(e))
    return true;

  // These never do, except if the entire file is position dependent or if
  // only the low bits are used.
  if (e == R_GOT || e == R_PLT)
    return ctx.target->usesOnlyLowPageBits(type) || !ctx.arg.isPic;
  // R_AARCH64_AUTH_ABS64 requires a dynamic relocation.
  if (e == RE_AARCH64_AUTH)
    return false;

  // The behavior of an undefined weak reference is implementation defined.
  // (We treat undefined non-weak the same as undefined weak.) For static
  // -no-pie linking, dynamic relocations are generally avoided (except
  // IRELATIVE). Emitting dynamic relocations for -shared aligns with its -z
  // undefs default. Dynamic -no-pie linking and -pie allow flexibility.
  if (sym.isPreemptible)
    return sym.isUndefined() && !ctx.arg.isPic;
  if (!ctx.arg.isPic)
    return true;

  // Constant when referencing a non-preemptible symbol.
  if (e == R_SIZE || e == RE_RISCV_LEB128)
    return true;

  // For the target and the relocation, we want to know if they are
  // absolute or relative.
  bool absVal = isAbsoluteValue(sym) && e != RE_PPC64_TOCBASE;
  bool relE = isRelExpr(e);
  if (absVal && !relE)
    return true;
  if (!absVal && relE)
    return true;
  if (!absVal && !relE)
    return ctx.target->usesOnlyLowPageBits(type);

  assert(absVal && relE);

  // Allow R_PLT_PC (optimized to R_PC here) to a hidden undefined weak symbol
  // in PIC mode. This is a little strange, but it allows us to link function
  // calls to such symbols (e.g. glibc/stdlib/exit.c:__run_exit_handlers).
  // Normally such a call will be guarded with a comparison, which will load a
  // zero from the GOT.
  if (sym.isUndefined())
    return true;

  // We set the final symbols values for linker script defined symbols later.
  // They always can be computed as a link time constant.
  if (sym.scriptDefined)
      return true;

  auto diag = Err(ctx);
  diag << "relocation " << type << " cannot refer to absolute symbol: " << &sym;
  printLocation(diag, *sec, sym, relOff);
  return true;
}

// The reason we have to do this early scan is as follows
// * To mmap the output file, we need to know the size
// * For that, we need to know how many dynamic relocs we will have.
// It might be possible to avoid this by outputting the file with write:
// * Write the allocated output sections, computing addresses.
// * Apply relocations, recording which ones require a dynamic reloc.
// * Write the dynamic relocations.
// * Write the rest of the file.
// This would have some drawbacks. For example, we would only know if .rela.dyn
// is needed after applying relocations. If it is, it will go after rw and rx
// sections. Given that it is ro, we will need an extra PT_LOAD. This
// complicates things for the dynamic linker and means we would have to reserve
// space for the extra PT_LOAD even if we end up not using it.
void RelocationScanner::processAux(RelExpr expr, RelType type, uint64_t offset,
                                   Symbol &sym, int64_t addend) const {
  // If non-ifunc non-preemptible, change PLT to direct call and optimize GOT
  // indirection.
  const bool isIfunc = sym.isGnuIFunc();
  if (!sym.isPreemptible && (!isIfunc || ctx.arg.zIfuncNoplt)) {
    if (expr != R_GOT_PC) {
      // The 0x8000 bit of r_addend of R_PPC_PLTREL24 is used to choose call
      // stub type. It should be ignored if optimized to R_PC.
      if (ctx.arg.emachine == EM_PPC && expr == RE_PPC32_PLTREL)
        addend &= ~0x8000;
      // R_HEX_GD_PLT_B22_PCREL (call a@GDPLT) is transformed into
      // call __tls_get_addr even if the symbol is non-preemptible.
      if (!(ctx.arg.emachine == EM_HEXAGON &&
            (type == R_HEX_GD_PLT_B22_PCREL ||
             type == R_HEX_GD_PLT_B22_PCREL_X ||
             type == R_HEX_GD_PLT_B32_PCREL_X)))
        expr = fromPlt(expr);
    } else if (!isAbsoluteValue(sym) ||
               (type == R_PPC64_PCREL_OPT && ctx.arg.emachine == EM_PPC64)) {
      expr = ctx.target->adjustGotPcExpr(type, addend,
                                         sec->content().data() + offset);
      // If the target adjusted the expression to R_RELAX_GOT_PC, we may end up
      // needing the GOT if we can't relax everything.
      if (expr == R_RELAX_GOT_PC)
        ctx.in.got->hasGotOffRel.store(true, std::memory_order_relaxed);
    }
  }

  // We were asked not to generate PLT entries for ifuncs. Instead, pass the
  // direct relocation on through.
  if (LLVM_UNLIKELY(isIfunc) && ctx.arg.zIfuncNoplt) {
    std::lock_guard<std::mutex> lock(ctx.relocMutex);
    sym.isExported = true;
    ctx.mainPart->relaDyn->addSymbolReloc(type, *sec, offset, sym, addend,
                                          type);
    return;
  }

  if (needsGot(expr)) {
    if (ctx.arg.emachine == EM_MIPS) {
      // MIPS ABI has special rules to process GOT entries and doesn't
      // require relocation entries for them. A special case is TLS
      // relocations. In that case dynamic loader applies dynamic
      // relocations to initialize TLS GOT entries.
      // See "Global Offset Table" in Chapter 5 in the following document
      // for detailed description:
      // ftp://www.linux-mips.org/pub/linux/mips/doc/ABI/mipsabi.pdf
      ctx.in.mipsGot->addEntry(*sec->file, sym, addend, expr);
    } else if (!sym.isTls() || ctx.arg.emachine != EM_LOONGARCH) {
      // Many LoongArch TLS relocs reuse the RE_LOONGARCH_GOT type, in which
      // case the NEEDS_GOT flag shouldn't get set.
      if (expr == RE_AARCH64_AUTH_GOT || expr == RE_AARCH64_AUTH_GOT_PAGE_PC ||
          expr == RE_AARCH64_AUTH_GOT_PC)
        sym.setFlags(NEEDS_GOT | NEEDS_GOT_AUTH);
      else
        sym.setFlags(NEEDS_GOT | NEEDS_GOT_NONAUTH);
    }
  } else if (needsPlt(expr)) {
    sym.setFlags(NEEDS_PLT);
  } else if (LLVM_UNLIKELY(isIfunc)) {
    sym.setFlags(HAS_DIRECT_RELOC);
  }

  // If the relocation is known to be a link-time constant, we know no dynamic
  // relocation will be created, pass the control to relocateAlloc() or
  // relocateNonAlloc() to resolve it.
  if (isStaticLinkTimeConstant(expr, type, sym, offset)) {
    sec->addReloc({expr, type, offset, addend, &sym});
    return;
  }

  // Use a simple -z notext rule that treats all sections except .eh_frame as
  // writable. GNU ld does not produce dynamic relocations in .eh_frame (and our
  // SectionBase::getOffset would incorrectly adjust the offset).
  //
  // For MIPS, we don't implement GNU ld's DW_EH_PE_absptr to DW_EH_PE_pcrel
  // conversion. We still emit a dynamic relocation.
  bool canWrite = (sec->flags & SHF_WRITE) ||
                  !(ctx.arg.zText ||
                    (isa<EhInputSection>(sec) && ctx.arg.emachine != EM_MIPS));
  if (canWrite) {
    RelType rel = ctx.target->getDynRel(type);
    if (oneof<R_GOT, RE_LOONGARCH_GOT>(expr) ||
        (rel == ctx.target->symbolicRel && !sym.isPreemptible)) {
      addRelativeReloc<true>(ctx, *sec, offset, sym, addend, expr, type);
      return;
    }
    if (rel != 0) {
      if (ctx.arg.emachine == EM_MIPS && rel == ctx.target->symbolicRel)
        rel = ctx.target->relativeRel;
      std::lock_guard<std::mutex> lock(ctx.relocMutex);
      Partition &part = sec->getPartition(ctx);
      if (ctx.arg.emachine == EM_AARCH64 && type == R_AARCH64_AUTH_ABS64) {
        // For a preemptible symbol, we can't use a relative relocation. For an
        // undefined symbol, we can't compute offset at link-time and use a
        // relative relocation. Use a symbolic relocation instead.
        if (sym.isPreemptible) {
          part.relaDyn->addSymbolReloc(type, *sec, offset, sym, addend, type);
        } else if (part.relrAuthDyn && sec->addralign >= 2 && offset % 2 == 0) {
          // When symbol values are determined in
          // finalizeAddressDependentContent, some .relr.auth.dyn relocations
          // may be moved to .rela.dyn.
          sec->addReloc({expr, type, offset, addend, &sym});
          part.relrAuthDyn->relocs.push_back({sec, sec->relocs().size() - 1});
        } else {
          part.relaDyn->addReloc({R_AARCH64_AUTH_RELATIVE, sec, offset, false,
                                  sym, addend, R_ABS});
        }
        return;
      }
      part.relaDyn->addSymbolReloc(rel, *sec, offset, sym, addend, type);

      // MIPS ABI turns using of GOT and dynamic relocations inside out.
      // While regular ABI uses dynamic relocations to fill up GOT entries
      // MIPS ABI requires dynamic linker to fills up GOT entries using
      // specially sorted dynamic symbol table. This affects even dynamic
      // relocations against symbols which do not require GOT entries
      // creation explicitly, i.e. do not have any GOT-relocations. So if
      // a preemptible symbol has a dynamic relocation we anyway have
      // to create a GOT entry for it.
      // If a non-preemptible symbol has a dynamic relocation against it,
      // dynamic linker takes it st_value, adds offset and writes down
      // result of the dynamic relocation. In case of preemptible symbol
      // dynamic linker performs symbol resolution, writes the symbol value
      // to the GOT entry and reads the GOT entry when it needs to perform
      // a dynamic relocation.
      // ftp://www.linux-mips.org/pub/linux/mips/doc/ABI/mipsabi.pdf p.4-19
      if (ctx.arg.emachine == EM_MIPS)
        ctx.in.mipsGot->addEntry(*sec->file, sym, addend, expr);
      return;
    }
  }

  // When producing an executable, we can perform copy relocations (for
  // STT_OBJECT) and canonical PLT (for STT_FUNC) if sym is defined by a DSO.
  // Copy relocations/canonical PLT entries are unsupported for
  // R_AARCH64_AUTH_ABS64.
  if (!ctx.arg.shared && sym.isShared() &&
      !(ctx.arg.emachine == EM_AARCH64 && type == R_AARCH64_AUTH_ABS64)) {
    if (!canDefineSymbolInExecutable(ctx, sym)) {
      auto diag = Err(ctx);
      diag << "cannot preempt symbol: " << &sym;
      printLocation(diag, *sec, sym, offset);
      return;
    }

    if (sym.isObject()) {
      // Produce a copy relocation.
      if (auto *ss = dyn_cast<SharedSymbol>(&sym)) {
        if (!ctx.arg.zCopyreloc) {
          auto diag = Err(ctx);
          diag << "unresolvable relocation " << type << " against symbol '"
               << ss << "'; recompile with -fPIC or remove '-z nocopyreloc'";
          printLocation(diag, *sec, sym, offset);
        }
        sym.setFlags(NEEDS_COPY);
      }
      sec->addReloc({expr, type, offset, addend, &sym});
      return;
    }

    // This handles a non PIC program call to function in a shared library. In
    // an ideal world, we could just report an error saying the relocation can
    // overflow at runtime. In the real world with glibc, crt1.o has a
    // R_X86_64_PC32 pointing to libc.so.
    //
    // The general idea on how to handle such cases is to create a PLT entry and
    // use that as the function value.
    //
    // For the static linking part, we just return a plt expr and everything
    // else will use the PLT entry as the address.
    //
    // The remaining problem is making sure pointer equality still works. We
    // need the help of the dynamic linker for that. We let it know that we have
    // a direct reference to a so symbol by creating an undefined symbol with a
    // non zero st_value. Seeing that, the dynamic linker resolves the symbol to
    // the value of the symbol we created. This is true even for got entries, so
    // pointer equality is maintained. To avoid an infinite loop, the only entry
    // that points to the real function is a dedicated got entry used by the
    // plt. That is identified by special relocation types (R_X86_64_JUMP_SLOT,
    // R_386_JMP_SLOT, etc).

    // For position independent executable on i386, the plt entry requires ebx
    // to be set. This causes two problems:
    // * If some code has a direct reference to a function, it was probably
    //   compiled without -fPIE/-fPIC and doesn't maintain ebx.
    // * If a library definition gets preempted to the executable, it will have
    //   the wrong ebx value.
    if (sym.isFunc()) {
      if (ctx.arg.pie && ctx.arg.emachine == EM_386) {
        auto diag = Err(ctx);
        diag << "symbol '" << &sym
             << "' cannot be preempted; recompile with -fPIE";
        printLocation(diag, *sec, sym, offset);
      }
      sym.setFlags(NEEDS_COPY | NEEDS_PLT);
      sec->addReloc({expr, type, offset, addend, &sym});
      return;
    }
  }

  auto diag = Err(ctx);
  diag << "relocation " << type << " cannot be used against ";
  if (sym.getName().empty())
    diag << "local symbol";
  else
    diag << "symbol '" << &sym << "'";
  diag << "; recompile with -fPIC";
  printLocation(diag, *sec, sym, offset);
}

// This function is similar to the `handleTlsRelocation`. MIPS does not
// support any relaxations for TLS relocations so by factoring out MIPS
// handling in to the separate function we can simplify the code and do not
// pollute other `handleTlsRelocation` by MIPS `ifs` statements.
// Mips has a custom MipsGotSection that handles the writing of GOT entries
// without dynamic relocations.
static unsigned handleMipsTlsRelocation(Ctx &ctx, RelType type, Symbol &sym,
                                        InputSectionBase &c, uint64_t offset,
                                        int64_t addend, RelExpr expr) {
  if (expr == RE_MIPS_TLSLD) {
    ctx.in.mipsGot->addTlsIndex(*c.file);
    c.addReloc({expr, type, offset, addend, &sym});
    return 1;
  }
  if (expr == RE_MIPS_TLSGD) {
    ctx.in.mipsGot->addDynTlsEntry(*c.file, sym);
    c.addReloc({expr, type, offset, addend, &sym});
    return 1;
  }
  return 0;
}

static unsigned handleAArch64PAuthTlsRelocation(InputSectionBase *sec,
                                                RelExpr expr, RelType type,
                                                uint64_t offset, Symbol &sym,
                                                int64_t addend) {
  // Do not optimize signed TLSDESC to LE/IE (as described in pauthabielf64).
  // https://github.com/ARM-software/abi-aa/blob/main/pauthabielf64/pauthabielf64.rst#general-restrictions
  // > PAUTHELF64 only supports the descriptor based TLS (TLSDESC).
  if (oneof<RE_AARCH64_AUTH_TLSDESC_PAGE, RE_AARCH64_AUTH_TLSDESC>(expr)) {
    sym.setFlags(NEEDS_TLSDESC | NEEDS_TLSDESC_AUTH);
    sec->addReloc({expr, type, offset, addend, &sym});
    return 1;
  }

  // TLSDESC_CALL hint relocation should not be emitted by compiler with signed
  // TLSDESC enabled.
  if (expr == R_TLSDESC_CALL)
    sym.setFlags(NEEDS_TLSDESC_NONAUTH);

  return 0;
}

// Notes about General Dynamic and Local Dynamic TLS models below. They may
// require the generation of a pair of GOT entries that have associated dynamic
// relocations. The pair of GOT entries created are of the form GOT[e0] Module
// Index (Used to find pointer to TLS block at run-time) GOT[e1] Offset of
// symbol in TLS block.
//
// Returns the number of relocations processed.
unsigned RelocationScanner::handleTlsRelocation(RelExpr expr, RelType type,
                                                uint64_t offset, Symbol &sym,
                                                int64_t addend) {
  bool isAArch64 = ctx.arg.emachine == EM_AARCH64;

  if (isAArch64)
    if (unsigned processed = handleAArch64PAuthTlsRelocation(
            sec, expr, type, offset, sym, addend))
      return processed;

  if (expr == R_TPREL || expr == R_TPREL_NEG) {
    if (ctx.arg.shared) {
      auto diag = Err(ctx);
      diag << "relocation " << type << " against " << &sym
           << " cannot be used with -shared";
      printLocation(diag, *sec, sym, offset);
      return 1;
    }
    return 0;
  }

  if (ctx.arg.emachine == EM_MIPS)
    return handleMipsTlsRelocation(ctx, type, sym, *sec, offset, addend, expr);

  bool isRISCV = ctx.arg.emachine == EM_RISCV;

  if (oneof<RE_AARCH64_TLSDESC_PAGE, R_TLSDESC, R_TLSDESC_CALL, R_TLSDESC_PC,
            R_TLSDESC_GOTPLT, RE_LOONGARCH_TLSDESC_PAGE_PC>(expr) &&
      ctx.arg.shared) {
    // R_RISCV_TLSDESC_{LOAD_LO12,ADD_LO12_I,CALL} reference a label. Do not
    // set NEEDS_TLSDESC on the label.
    if (expr != R_TLSDESC_CALL) {
      if (isAArch64)
        sym.setFlags(NEEDS_TLSDESC | NEEDS_TLSDESC_NONAUTH);
      else if (!isRISCV || type == R_RISCV_TLSDESC_HI20)
        sym.setFlags(NEEDS_TLSDESC);
      sec->addReloc({expr, type, offset, addend, &sym});
    }
    return 1;
  }

  // LoongArch supports IE to LE, DESC GD/LD to IE/LE optimizations in
  // non-extreme code model.
  bool execOptimizeInLoongArch =
      ctx.arg.emachine == EM_LOONGARCH &&
      (type == R_LARCH_TLS_IE_PC_HI20 || type == R_LARCH_TLS_IE_PC_LO12 ||
       type == R_LARCH_TLS_DESC_PC_HI20 || type == R_LARCH_TLS_DESC_PC_LO12 ||
       type == R_LARCH_TLS_DESC_LD || type == R_LARCH_TLS_DESC_CALL ||
       type == R_LARCH_TLS_DESC_PCREL20_S2);

  // ARM, Hexagon, LoongArch and RISC-V do not support GD/LD to IE/LE
  // optimizations.
  // RISC-V supports TLSDESC to IE/LE optimizations.
  // For PPC64, if the file has missing R_PPC64_TLSGD/R_PPC64_TLSLD, disable
  // optimization as well.
  bool execOptimize =
      !ctx.arg.shared && ctx.arg.emachine != EM_ARM &&
      ctx.arg.emachine != EM_HEXAGON &&
      (ctx.arg.emachine != EM_LOONGARCH || execOptimizeInLoongArch) &&
      !(isRISCV && expr != R_TLSDESC_PC && expr != R_TLSDESC_CALL) &&
      !sec->file->ppc64DisableTLSRelax;

  // If we are producing an executable and the symbol is non-preemptable, it
  // must be defined and the code sequence can be optimized to use
  // Local-Exesec->
  //
  // ARM and RISC-V do not support any relaxations for TLS relocations, however,
  // we can omit the DTPMOD dynamic relocations and resolve them at link time
  // because them are always 1. This may be necessary for static linking as
  // DTPMOD may not be expected at load time.
  bool isLocalInExecutable = !sym.isPreemptible && !ctx.arg.shared;

  // Local Dynamic is for access to module local TLS variables, while still
  // being suitable for being dynamically loaded via dlopen. GOT[e0] is the
  // module index, with a special value of 0 for the current module. GOT[e1] is
  // unused. There only needs to be one module index entry.
  if (oneof<R_TLSLD_GOT, R_TLSLD_GOTPLT, R_TLSLD_PC, R_TLSLD_HINT>(expr)) {
    // Local-Dynamic relocs can be optimized to Local-Exesec->
    if (execOptimize) {
      sec->addReloc({ctx.target->adjustTlsExpr(type, R_RELAX_TLS_LD_TO_LE),
                     type, offset, addend, &sym});
      return ctx.target->getTlsGdRelaxSkip(type);
    }
    if (expr == R_TLSLD_HINT)
      return 1;
    ctx.needsTlsLd.store(true, std::memory_order_relaxed);
    sec->addReloc({expr, type, offset, addend, &sym});
    return 1;
  }

  // Local-Dynamic relocs can be optimized to Local-Exesec->
  if (expr == R_DTPREL) {
    if (execOptimize)
      expr = ctx.target->adjustTlsExpr(type, R_RELAX_TLS_LD_TO_LE);
    sec->addReloc({expr, type, offset, addend, &sym});
    return 1;
  }

  // Local-Dynamic sequence where offset of tls variable relative to dynamic
  // thread pointer is stored in the got. This cannot be optimized to
  // Local-Exesec->
  if (expr == R_TLSLD_GOT_OFF) {
    sym.setFlags(NEEDS_GOT_DTPREL);
    sec->addReloc({expr, type, offset, addend, &sym});
    return 1;
  }

  // LoongArch does not support transition from TLSDESC to LE/IE in the extreme
  // code model, in which NEEDS_TLSDESC should set, rather than NEEDS_TLSGD. So
  // we check independently.
  if (ctx.arg.emachine == EM_LOONGARCH &&
      oneof<RE_LOONGARCH_TLSDESC_PAGE_PC, R_TLSDESC, R_TLSDESC_PC,
            R_TLSDESC_CALL>(expr) &&
      !execOptimize) {
    if (expr != R_TLSDESC_CALL) {
      sym.setFlags(NEEDS_TLSDESC);
      sec->addReloc({expr, type, offset, addend, &sym});
    }
    return 1;
  }

  if (oneof<RE_AARCH64_TLSDESC_PAGE, R_TLSDESC, R_TLSDESC_CALL, R_TLSDESC_PC,
            R_TLSDESC_GOTPLT, R_TLSGD_GOT, R_TLSGD_GOTPLT, R_TLSGD_PC,
            RE_LOONGARCH_TLSGD_PAGE_PC, RE_LOONGARCH_TLSDESC_PAGE_PC>(expr)) {
    if (!execOptimize) {
      sym.setFlags(NEEDS_TLSGD);
      sec->addReloc({expr, type, offset, addend, &sym});
      return 1;
    }

    // Global-Dynamic/TLSDESC can be optimized to Initial-Exec or Local-Exec
    // depending on the symbol being locally defined or not.
    //
    // R_RISCV_TLSDESC_{LOAD_LO12,ADD_LO12_I,CALL} reference a non-preemptible
    // label, so TLSDESC=>IE will be categorized as R_RELAX_TLS_GD_TO_LE. We fix
    // the categorization in RISCV::relocateAllosec->
    if (sym.isPreemptible) {
      sym.setFlags(NEEDS_TLSGD_TO_IE);
      sec->addReloc({ctx.target->adjustTlsExpr(type, R_RELAX_TLS_GD_TO_IE),
                     type, offset, addend, &sym});
    } else {
      sec->addReloc({ctx.target->adjustTlsExpr(type, R_RELAX_TLS_GD_TO_LE),
                     type, offset, addend, &sym});
    }
    return ctx.target->getTlsGdRelaxSkip(type);
  }

  if (oneof<R_GOT, R_GOTPLT, R_GOT_PC, RE_AARCH64_GOT_PAGE_PC,
            RE_LOONGARCH_GOT_PAGE_PC, R_GOT_OFF, R_TLSIE_HINT>(expr)) {
    ctx.hasTlsIe.store(true, std::memory_order_relaxed);
    // Initial-Exec relocs can be optimized to Local-Exec if the symbol is
    // locally defined.  This is not supported on SystemZ.
    if (execOptimize && isLocalInExecutable && ctx.arg.emachine != EM_S390) {
      sec->addReloc({R_RELAX_TLS_IE_TO_LE, type, offset, addend, &sym});
    } else if (expr != R_TLSIE_HINT) {
      sym.setFlags(NEEDS_TLSIE);
      // R_GOT needs a relative relocation for PIC on i386 and Hexagon.
      if (expr == R_GOT && ctx.arg.isPic &&
          !ctx.target->usesOnlyLowPageBits(type))
        addRelativeReloc<true>(ctx, *sec, offset, sym, addend, expr, type);
      else
        sec->addReloc({expr, type, offset, addend, &sym});
    }
    return 1;
  }

  // LoongArch TLS GD/LD relocs reuse the RE_LOONGARCH_GOT, in which
  // NEEDS_TLSIE shouldn't set. So we check independently.
  if (ctx.arg.emachine == EM_LOONGARCH && expr == RE_LOONGARCH_GOT &&
      execOptimize && isLocalInExecutable) {
    ctx.hasTlsIe.store(true, std::memory_order_relaxed);
    sec->addReloc({R_RELAX_TLS_IE_TO_LE, type, offset, addend, &sym});
    return 1;
  }

  return 0;
}

template <class ELFT, class RelTy>
void RelocationScanner::scanOne(typename Relocs<RelTy>::const_iterator &i) {
  const RelTy &rel = *i;
  uint32_t symIndex = rel.getSymbol(ctx.arg.isMips64EL);
  Symbol &sym = sec->getFile<ELFT>()->getSymbol(symIndex);
  RelType type;
  if constexpr (ELFT::Is64Bits || RelTy::IsCrel) {
    type = rel.getType(ctx.arg.isMips64EL);
    ++i;
  } else {
    // CREL is unsupported for MIPS N32.
    if (ctx.arg.mipsN32Abi) {
      type = getMipsN32RelType(i);
    } else {
      type = rel.getType(ctx.arg.isMips64EL);
      ++i;
    }
  }
  // Get an offset in an output section this relocation is applied to.
  uint64_t offset = getter.get(ctx, rel.r_offset);
  if (offset == uint64_t(-1))
    return;

  RelExpr expr =
      ctx.target->getRelExpr(type, sym, sec->content().data() + offset);
  int64_t addend = RelTy::HasAddend
                       ? getAddend<ELFT>(rel)
                       : ctx.target->getImplicitAddend(
                             sec->content().data() + rel.r_offset, type);
  if (LLVM_UNLIKELY(ctx.arg.emachine == EM_MIPS))
    addend += computeMipsAddend<ELFT>(rel, expr, sym.isLocal());
  else if (ctx.arg.emachine == EM_PPC64 && ctx.arg.isPic && type == R_PPC64_TOC)
    addend += getPPC64TocBase(ctx);

  // Ignore R_*_NONE and other marker relocations.
  if (expr == R_NONE)
    return;

  // Error if the target symbol is undefined. Symbol index 0 may be used by
  // marker relocations, e.g. R_*_NONE and R_ARM_V4BX. Don't error on them.
  if (sym.isUndefined() && symIndex != 0 &&
      maybeReportUndefined(ctx, cast<Undefined>(sym), *sec, offset))
    return;

  if (ctx.arg.emachine == EM_PPC64) {
    // We can separate the small code model relocations into 2 categories:
    // 1) Those that access the compiler generated .toc sections.
    // 2) Those that access the linker allocated got entries.
    // lld allocates got entries to symbols on demand. Since we don't try to
    // sort the got entries in any way, we don't have to track which objects
    // have got-based small code model relocs. The .toc sections get placed
    // after the end of the linker allocated .got section and we do sort those
    // so sections addressed with small code model relocations come first.
    if (type == R_PPC64_TOC16 || type == R_PPC64_TOC16_DS)
      sec->file->ppc64SmallCodeModelTocRelocs = true;

    // Record the TOC entry (.toc + addend) as not relaxable. See the comment in
    // InputSectionBase::relocateAlloc().
    if (type == R_PPC64_TOC16_LO && sym.isSection() && isa<Defined>(sym) &&
        cast<Defined>(sym).section->name == ".toc")
      ctx.ppc64noTocRelax.insert({&sym, addend});

    if ((type == R_PPC64_TLSGD && expr == R_TLSDESC_CALL) ||
        (type == R_PPC64_TLSLD && expr == R_TLSLD_HINT)) {
      // Skip the error check for CREL, which does not set `end`.
      if constexpr (!RelTy::IsCrel) {
        if (i == end) {
          auto diag = Err(ctx);
          diag << "R_PPC64_TLSGD/R_PPC64_TLSLD may not be the last "
                  "relocation";
          printLocation(diag, *sec, sym, offset);
          return;
        }
      }

      // Offset the 4-byte aligned R_PPC64_TLSGD by one byte in the NOTOC
      // case, so we can discern it later from the toc-case.
      if (i->getType(/*isMips64EL=*/false) == R_PPC64_REL24_NOTOC)
        ++offset;
    }
  }

  // If the relocation does not emit a GOT or GOTPLT entry but its computation
  // uses their addresses, we need GOT or GOTPLT to be created.
  //
  // The 5 types that relative GOTPLT are all x86 and x86-64 specific.
  if (oneof<R_GOTPLTONLY_PC, R_GOTPLTREL, R_GOTPLT, R_PLT_GOTPLT,
            R_TLSDESC_GOTPLT, R_TLSGD_GOTPLT>(expr)) {
    ctx.in.gotPlt->hasGotPltOffRel.store(true, std::memory_order_relaxed);
  } else if (oneof<R_GOTONLY_PC, R_GOTREL, RE_PPC32_PLTREL, RE_PPC64_TOCBASE,
                   RE_PPC64_RELAX_TOC>(expr)) {
    ctx.in.got->hasGotOffRel.store(true, std::memory_order_relaxed);
  }

  // Process TLS relocations, including TLS optimizations. Note that
  // R_TPREL and R_TPREL_NEG relocations are resolved in processAux.
  //
  // Some RISCV TLSDESC relocations reference a local NOTYPE symbol,
  // but we need to process them in handleTlsRelocation.
  if (sym.isTls() || oneof<R_TLSDESC_PC, R_TLSDESC_CALL>(expr)) {
    if (unsigned processed =
            handleTlsRelocation(expr, type, offset, sym, addend)) {
      i += processed - 1;
      return;
    }
  }

  processAux(expr, type, offset, sym, addend);
}

// R_PPC64_TLSGD/R_PPC64_TLSLD is required to mark `bl __tls_get_addr` for
// General Dynamic/Local Dynamic code sequences. If a GD/LD GOT relocation is
// found but no R_PPC64_TLSGD/R_PPC64_TLSLD is seen, we assume that the
// instructions are generated by very old IBM XL compilers. Work around the
// issue by disabling GD/LD to IE/LE relaxation.
template <class RelTy>
static void checkPPC64TLSRelax(InputSectionBase &sec, Relocs<RelTy> rels) {
  // Skip if sec is synthetic (sec.file is null) or if sec has been marked.
  if (!sec.file || sec.file->ppc64DisableTLSRelax)
    return;
  bool hasGDLD = false;
  for (const RelTy &rel : rels) {
    RelType type = rel.getType(false);
    switch (type) {
    case R_PPC64_TLSGD:
    case R_PPC64_TLSLD:
      return; // Found a marker
    case R_PPC64_GOT_TLSGD16:
    case R_PPC64_GOT_TLSGD16_HA:
    case R_PPC64_GOT_TLSGD16_HI:
    case R_PPC64_GOT_TLSGD16_LO:
    case R_PPC64_GOT_TLSLD16:
    case R_PPC64_GOT_TLSLD16_HA:
    case R_PPC64_GOT_TLSLD16_HI:
    case R_PPC64_GOT_TLSLD16_LO:
      hasGDLD = true;
      break;
    }
  }
  if (hasGDLD) {
    sec.file->ppc64DisableTLSRelax = true;
    Warn(sec.file->ctx)
        << sec.file
        << ": disable TLS relaxation due to R_PPC64_GOT_TLS* relocations "
           "without "
           "R_PPC64_TLSGD/R_PPC64_TLSLD relocations";
  }
}

template <class ELFT, class RelTy>
void RelocationScanner::scan(Relocs<RelTy> rels) {
  // Not all relocations end up in Sec->Relocations, but a lot do.
  sec->relocations.reserve(rels.size());

  if (ctx.arg.emachine == EM_PPC64)
    checkPPC64TLSRelax<RelTy>(*sec, rels);

  // For EhInputSection, OffsetGetter expects the relocations to be sorted by
  // r_offset. In rare cases (.eh_frame pieces are reordered by a linker
  // script), the relocations may be unordered.
  // On SystemZ, all sections need to be sorted by r_offset, to allow TLS
  // relaxation to be handled correctly - see SystemZ::getTlsGdRelaxSkip.
  SmallVector<RelTy, 0> storage;
  if (isa<EhInputSection>(sec) || ctx.arg.emachine == EM_S390)
    rels = sortRels(rels, storage);

  if constexpr (RelTy::IsCrel) {
    for (auto i = rels.begin(); i != rels.end();)
      scanOne<ELFT, RelTy>(i);
  } else {
    // The non-CREL code path has additional check for PPC64 TLS.
    end = static_cast<const void *>(rels.end());
    for (auto i = rels.begin(); i != end;)
      scanOne<ELFT, RelTy>(i);
  }

  // Sort relocations by offset for more efficient searching for
  // R_RISCV_PCREL_HI20, ALIGN relocations, R_PPC64_ADDR64 and the
  // branch-to-branch optimization.
  if (is_contained({EM_RISCV, EM_LOONGARCH}, ctx.arg.emachine) ||
      (ctx.arg.emachine == EM_PPC64 && sec->name == ".toc") ||
      ctx.arg.branchToBranch)
    llvm::stable_sort(sec->relocs(),
                      [](const Relocation &lhs, const Relocation &rhs) {
                        return lhs.offset < rhs.offset;
                      });
}

template <class ELFT>
void RelocationScanner::scanSection(InputSectionBase &s, bool isEH) {
  sec = &s;
  getter = OffsetGetter(s);
  const RelsOrRelas<ELFT> rels = s.template relsOrRelas<ELFT>(!isEH);
  if (rels.areRelocsCrel())
    scan<ELFT>(rels.crels);
  else if (rels.areRelocsRel())
    scan<ELFT>(rels.rels);
  else
    scan<ELFT>(rels.relas);
}

template <class ELFT> void elf::scanRelocations(Ctx &ctx) {
  // Scan all relocations. Each relocation goes through a series of tests to
  // determine if it needs special treatment, such as creating GOT, PLT,
  // copy relocations, etc. Note that relocations for non-alloc sections are
  // directly processed by InputSection::relocateNonAlloc.

  // Deterministic parallellism needs sorting relocations which is unsuitable
  // for -z nocombreloc. MIPS and PPC64 use global states which are not suitable
  // for parallelism.
  bool serial = !ctx.arg.zCombreloc || ctx.arg.emachine == EM_MIPS ||
                ctx.arg.emachine == EM_PPC64;
  parallel::TaskGroup tg;
  auto outerFn = [&]() {
    for (ELFFileBase *f : ctx.objectFiles) {
      auto fn = [f, &ctx]() {
        RelocationScanner scanner(ctx);
        for (InputSectionBase *s : f->getSections()) {
          if (s && s->kind() == SectionBase::Regular && s->isLive() &&
              (s->flags & SHF_ALLOC) &&
              !(s->type == SHT_ARM_EXIDX && ctx.arg.emachine == EM_ARM))
            scanner.template scanSection<ELFT>(*s);
        }
      };
      if (serial)
        fn();
      else
        tg.spawn(fn);
    }
    auto scanEH = [&] {
      RelocationScanner scanner(ctx);
      for (Partition &part : ctx.partitions) {
        for (EhInputSection *sec : part.ehFrame->sections)
          scanner.template scanSection<ELFT>(*sec, /*isEH=*/true);
        if (part.armExidx && part.armExidx->isLive())
          for (InputSection *sec : part.armExidx->exidxSections)
            if (sec->isLive())
              scanner.template scanSection<ELFT>(*sec);
      }
    };
    if (serial)
      scanEH();
    else
      tg.spawn(scanEH);
  };
  // If `serial` is true, call `spawn` to ensure that `scanner` runs in a thread
  // with valid getThreadIndex().
  if (serial)
    tg.spawn(outerFn);
  else
    outerFn();
}

RelocationBaseSection &elf::getIRelativeSection(Ctx &ctx) {
  // Prior to Android V, there was a bug that caused RELR relocations to be
  // applied after packed relocations. This meant that resolvers referenced by
  // IRELATIVE relocations in the packed relocation section would read
  // unrelocated globals with RELR relocations when
  // --pack-relative-relocs=android+relr is enabled. Work around this by placing
  // IRELATIVE in .rela.plt.
  return ctx.arg.androidPackDynRelocs ? *ctx.in.relaPlt
                                      : *ctx.mainPart->relaDyn;
}

static bool handleNonPreemptibleIfunc(Ctx &ctx, Symbol &sym, uint16_t flags) {
  // Handle a reference to a non-preemptible ifunc. These are special in a
  // few ways:
  //
  // - Unlike most non-preemptible symbols, non-preemptible ifuncs do not have
  //   a fixed value. But assuming that all references to the ifunc are
  //   GOT-generating or PLT-generating, the handling of an ifunc is
  //   relatively straightforward. We create a PLT entry in Iplt, which is
  //   usually at the end of .plt, which makes an indirect call using a
  //   matching GOT entry in igotPlt, which is usually at the end of .got.plt.
  //   The GOT entry is relocated using an IRELATIVE relocation in relaDyn,
  //   which is usually at the end of .rela.dyn.
  //
  // - Despite the fact that an ifunc does not have a fixed value, compilers
  //   that are not passed -fPIC will assume that they do, and will emit
  //   direct (non-GOT-generating, non-PLT-generating) relocations to the
  //   symbol. This means that if a direct relocation to the symbol is
  //   seen, the linker must set a value for the symbol, and this value must
  //   be consistent no matter what type of reference is made to the symbol.
  //   This can be done by creating a PLT entry for the symbol in the way
  //   described above and making it canonical, that is, making all references
  //   point to the PLT entry instead of the resolver. In lld we also store
  //   the address of the PLT entry in the dynamic symbol table, which means
  //   that the symbol will also have the same value in other modules.
  //   Because the value loaded from the GOT needs to be consistent with
  //   the value computed using a direct relocation, a non-preemptible ifunc
  //   may end up with two GOT entries, one in .got.plt that points to the
  //   address returned by the resolver and is used only by the PLT entry,
  //   and another in .got that points to the PLT entry and is used by
  //   GOT-generating relocations.
  //
  // - The fact that these symbols do not have a fixed value makes them an
  //   exception to the general rule that a statically linked executable does
  //   not require any form of dynamic relocation. To handle these relocations
  //   correctly, the IRELATIVE relocations are stored in an array which a
  //   statically linked executable's startup code must enumerate using the
  //   linker-defined symbols __rela?_iplt_{start,end}.
  if (!sym.isGnuIFunc() || sym.isPreemptible || ctx.arg.zIfuncNoplt)
    return false;
  // Skip unreferenced non-preemptible ifunc.
  if (!(flags & (NEEDS_GOT | NEEDS_PLT | HAS_DIRECT_RELOC)))
    return true;

  sym.isInIplt = true;

  // Create an Iplt and the associated IRELATIVE relocation pointing to the
  // original section/value pairs. For non-GOT non-PLT relocation case below, we
  // may alter section/value, so create a copy of the symbol to make
  // section/value fixed.
  auto *directSym = makeDefined(cast<Defined>(sym));
  directSym->allocateAux(ctx);
  auto &dyn = getIRelativeSection(ctx);
  addPltEntry(ctx, *ctx.in.iplt, *ctx.in.igotPlt, dyn, ctx.target->iRelativeRel,
              *directSym);
  sym.allocateAux(ctx);
  ctx.symAux.back().pltIdx = ctx.symAux[directSym->auxIdx].pltIdx;

  if (flags & HAS_DIRECT_RELOC) {
    // Change the value to the IPLT and redirect all references to it.
    auto &d = cast<Defined>(sym);
    d.section = ctx.in.iplt.get();
    d.value = d.getPltIdx(ctx) * ctx.target->ipltEntrySize;
    d.size = 0;
    // It's important to set the symbol type here so that dynamic loaders
    // don't try to call the PLT as if it were an ifunc resolver.
    d.type = STT_FUNC;

    if (flags & NEEDS_GOT) {
      assert(!(flags & NEEDS_GOT_AUTH) &&
             "R_AARCH64_AUTH_IRELATIVE is not supported yet");
      addGotEntry(ctx, sym);
    }
  } else if (flags & NEEDS_GOT) {
    // Redirect GOT accesses to point to the Igot.
    sym.gotInIgot = true;
  }
  return true;
}

void elf::postScanRelocations(Ctx &ctx) {
  auto fn = [&](Symbol &sym) {
    auto flags = sym.flags.load(std::memory_order_relaxed);
    if (handleNonPreemptibleIfunc(ctx, sym, flags))
      return;

    if (sym.isTagged() && sym.isDefined())
      ctx.mainPart->memtagGlobalDescriptors->addSymbol(sym);

    if (!sym.needsDynReloc())
      return;
    sym.allocateAux(ctx);

    if (flags & NEEDS_GOT) {
      if ((flags & NEEDS_GOT_AUTH) && (flags & NEEDS_GOT_NONAUTH)) {
        auto diag = Err(ctx);
        diag << "both AUTH and non-AUTH GOT entries for '" << sym.getName()
             << "' requested, but only one type of GOT entry per symbol is "
                "supported";
        return;
      }
      if (flags & NEEDS_GOT_AUTH)
        addGotAuthEntry(ctx, sym);
      else
        addGotEntry(ctx, sym);
    }
    if (flags & NEEDS_PLT)
      addPltEntry(ctx, *ctx.in.plt, *ctx.in.gotPlt, *ctx.in.relaPlt,
                  ctx.target->pltRel, sym);
    if (flags & NEEDS_COPY) {
      if (sym.isObject()) {
        invokeELFT(addCopyRelSymbol, ctx, cast<SharedSymbol>(sym));
        // NEEDS_COPY is cleared for sym and its aliases so that in
        // later iterations aliases won't cause redundant copies.
        assert(!sym.hasFlag(NEEDS_COPY));
      } else {
        assert(sym.isFunc() && sym.hasFlag(NEEDS_PLT));
        if (!sym.isDefined()) {
          replaceWithDefined(ctx, sym, *ctx.in.plt,
                             ctx.target->pltHeaderSize +
                                 ctx.target->pltEntrySize * sym.getPltIdx(ctx),
                             0);
          sym.setFlags(NEEDS_COPY);
          if (ctx.arg.emachine == EM_PPC) {
            // PPC32 canonical PLT entries are at the beginning of .glink
            cast<Defined>(sym).value = ctx.in.plt->headerSize;
            ctx.in.plt->headerSize += 16;
            cast<PPC32GlinkSection>(*ctx.in.plt).canonical_plts.push_back(&sym);
          }
        }
      }
    }

    if (!sym.isTls())
      return;
    bool isLocalInExecutable = !sym.isPreemptible && !ctx.arg.shared;
    GotSection *got = ctx.in.got.get();

    if (flags & NEEDS_TLSDESC) {
      if ((flags & NEEDS_TLSDESC_AUTH) && (flags & NEEDS_TLSDESC_NONAUTH)) {
        Err(ctx)
            << "both AUTH and non-AUTH TLSDESC entries for '" << sym.getName()
            << "' requested, but only one type of TLSDESC entry per symbol is "
               "supported";
        return;
      }
      got->addTlsDescEntry(sym);
      RelType tlsDescRel = ctx.target->tlsDescRel;
      if (flags & NEEDS_TLSDESC_AUTH) {
        got->addTlsDescAuthEntry();
        tlsDescRel = ELF::R_AARCH64_AUTH_TLSDESC;
      }
      ctx.mainPart->relaDyn->addAddendOnlyRelocIfNonPreemptible(
          tlsDescRel, *got, got->getTlsDescOffset(sym), sym, tlsDescRel);
    }
    if (flags & NEEDS_TLSGD) {
      got->addDynTlsEntry(sym);
      uint64_t off = got->getGlobalDynOffset(sym);
      if (isLocalInExecutable)
        // Write one to the GOT slot.
        got->addConstant({R_ADDEND, ctx.target->symbolicRel, off, 1, &sym});
      else
        ctx.mainPart->relaDyn->addSymbolReloc(ctx.target->tlsModuleIndexRel,
                                              *got, off, sym);

      // If the symbol is preemptible we need the dynamic linker to write
      // the offset too.
      uint64_t offsetOff = off + ctx.arg.wordsize;
      if (sym.isPreemptible)
        ctx.mainPart->relaDyn->addSymbolReloc(ctx.target->tlsOffsetRel, *got,
                                              offsetOff, sym);
      else
        got->addConstant({R_ABS, ctx.target->tlsOffsetRel, offsetOff, 0, &sym});
    }
    if (flags & NEEDS_TLSGD_TO_IE) {
      got->addEntry(sym);
      ctx.mainPart->relaDyn->addSymbolReloc(ctx.target->tlsGotRel, *got,
                                            sym.getGotOffset(ctx), sym);
    }
    if (flags & NEEDS_GOT_DTPREL) {
      got->addEntry(sym);
      got->addConstant(
          {R_ABS, ctx.target->tlsOffsetRel, sym.getGotOffset(ctx), 0, &sym});
    }

    if ((flags & NEEDS_TLSIE) && !(flags & NEEDS_TLSGD_TO_IE))
      addTpOffsetGotEntry(ctx, sym);
  };

  GotSection *got = ctx.in.got.get();
  if (ctx.needsTlsLd.load(std::memory_order_relaxed) && got->addTlsIndex()) {
    if (ctx.arg.shared)
      ctx.mainPart->relaDyn->addReloc(
          {ctx.target->tlsModuleIndexRel, got, got->getTlsIndexOff()});
    else
      got->addConstant({R_ADDEND, ctx.target->symbolicRel,
                        got->getTlsIndexOff(), 1, ctx.dummySym});
  }

  assert(ctx.symAux.size() == 1);
  for (Symbol *sym : ctx.symtab->getSymbols())
    fn(*sym);

  // Local symbols may need the aforementioned non-preemptible ifunc and GOT
  // handling. They don't need regular PLT.
  for (ELFFileBase *file : ctx.objectFiles)
    for (Symbol *sym : file->getLocalSymbols())
      fn(*sym);

  if (ctx.arg.branchToBranch)
    ctx.target->applyBranchToBranchOpt();
}

static bool mergeCmp(const InputSection *a, const InputSection *b) {
  // std::merge requires a strict weak ordering.
  if (a->outSecOff < b->outSecOff)
    return true;

  // FIXME dyn_cast<ThunkSection> is non-null for any SyntheticSection.
  if (a->outSecOff == b->outSecOff && a != b) {
    auto *ta = dyn_cast<ThunkSection>(a);
    auto *tb = dyn_cast<ThunkSection>(b);

    // Check if Thunk is immediately before any specific Target
    // InputSection for example Mips LA25 Thunks.
    if (ta && ta->getTargetInputSection() == b)
      return true;

    // Place Thunk Sections without specific targets before
    // non-Thunk Sections.
    if (ta && !tb && !ta->getTargetInputSection())
      return true;
  }

  return false;
}

// Call Fn on every executable InputSection accessed via the linker script
// InputSectionDescription::Sections.
static void forEachInputSectionDescription(
    ArrayRef<OutputSection *> outputSections,
    llvm::function_ref<void(OutputSection *, InputSectionDescription *)> fn) {
  for (OutputSection *os : outputSections) {
    if (!(os->flags & SHF_ALLOC) || !(os->flags & SHF_EXECINSTR))
      continue;
    for (SectionCommand *bc : os->commands)
      if (auto *isd = dyn_cast<InputSectionDescription>(bc))
        fn(os, isd);
  }
}

ThunkCreator::ThunkCreator(Ctx &ctx) : ctx(ctx) {}

ThunkCreator::~ThunkCreator() {}

// Thunk Implementation
//
// Thunks (sometimes called stubs, veneers or branch islands) are small pieces
// of code that the linker inserts inbetween a caller and a callee. The thunks
// are added at link time rather than compile time as the decision on whether
// a thunk is needed, such as the caller and callee being out of range, can only
// be made at link time.
//
// It is straightforward to tell given the current state of the program when a
// thunk is needed for a particular call. The more difficult part is that
// the thunk needs to be placed in the program such that the caller can reach
// the thunk and the thunk can reach the callee; furthermore, adding thunks to
// the program alters addresses, which can mean more thunks etc.
//
// In lld we have a synthetic ThunkSection that can hold many Thunks.
// The decision to have a ThunkSection act as a container means that we can
// more easily handle the most common case of a single block of contiguous
// Thunks by inserting just a single ThunkSection.
//
// The implementation of Thunks in lld is split across these areas
// Relocations.cpp : Framework for creating and placing thunks
// Thunks.cpp : The code generated for each supported thunk
// Target.cpp : Target specific hooks that the framework uses to decide when
//              a thunk is used
// Synthetic.cpp : Implementation of ThunkSection
// Writer.cpp : Iteratively call framework until no more Thunks added
//
// Thunk placement requirements:
// Mips LA25 thunks. These must be placed immediately before the callee section
// We can assume that the caller is in range of the Thunk. These are modelled
// by Thunks that return the section they must precede with
// getTargetInputSection().
//
// ARM interworking and range extension thunks. These thunks must be placed
// within range of the caller. All implemented ARM thunks can always reach the
// callee as they use an indirect jump via a register that has no range
// restrictions.
//
// Thunk placement algorithm:
// For Mips LA25 ThunkSections; the placement is explicit, it has to be before
// getTargetInputSection().
//
// For thunks that must be placed within range of the caller there are many
// possible choices given that the maximum range from the caller is usually
// much larger than the average InputSection size. Desirable properties include:
// - Maximize reuse of thunks by multiple callers
// - Minimize number of ThunkSections to simplify insertion
// - Handle impact of already added Thunks on addresses
// - Simple to understand and implement
//
// In lld for the first pass, we pre-create one or more ThunkSections per
// InputSectionDescription at Target specific intervals. A ThunkSection is
// placed so that the estimated end of the ThunkSection is within range of the
// start of the InputSectionDescription or the previous ThunkSection. For
// example:
// InputSectionDescription
// Section 0
// ...
// Section N
// ThunkSection 0
// Section N + 1
// ...
// Section N + K
// Thunk Section 1
//
// The intention is that we can add a Thunk to a ThunkSection that is well
// spaced enough to service a number of callers without having to do a lot
// of work. An important principle is that it is not an error if a Thunk cannot
// be placed in a pre-created ThunkSection; when this happens we create a new
// ThunkSection placed next to the caller. This allows us to handle the vast
// majority of thunks simply, but also handle rare cases where the branch range
// is smaller than the target specific spacing.
//
// The algorithm is expected to create all the thunks that are needed in a
// single pass, with a small number of programs needing a second pass due to
// the insertion of thunks in the first pass increasing the offset between
// callers and callees that were only just in range.
//
// A consequence of allowing new ThunkSections to be created outside of the
// pre-created ThunkSections is that in rare cases calls to Thunks that were in
// range in pass K, are out of range in some pass > K due to the insertion of
// more Thunks in between the caller and callee. When this happens we retarget
// the relocation back to the original target and create another Thunk.

// Remove ThunkSections that are empty, this should only be the initial set
// precreated on pass 0.

// Insert the Thunks for OutputSection OS into their designated place
// in the Sections vector, and recalculate the InputSection output section
// offsets.
// This may invalidate any output section offsets stored outside of InputSection
void ThunkCreator::mergeThunks(ArrayRef<OutputSection *> outputSections) {
  forEachInputSectionDescription(
      outputSections, [&](OutputSection *os, InputSectionDescription *isd) {
        if (isd->thunkSections.empty())
          return;

        // Remove any zero sized precreated Thunks.
        llvm::erase_if(isd->thunkSections,
                       [](const std::pair<ThunkSection *, uint32_t> &ts) {
                         return ts.first->getSize() == 0;
                       });

        // ISD->ThunkSections contains all created ThunkSections, including
        // those inserted in previous passes. Extract the Thunks created this
        // pass and order them in ascending outSecOff.
        std::vector<ThunkSection *> newThunks;
        for (std::pair<ThunkSection *, uint32_t> ts : isd->thunkSections)
          if (ts.second == pass)
            newThunks.push_back(ts.first);
        llvm::stable_sort(newThunks,
                          [](const ThunkSection *a, const ThunkSection *b) {
                            return a->outSecOff < b->outSecOff;
                          });

        // Merge sorted vectors of Thunks and InputSections by outSecOff
        SmallVector<InputSection *, 0> tmp;
        tmp.reserve(isd->sections.size() + newThunks.size());

        std::merge(isd->sections.begin(), isd->sections.end(),
                   newThunks.begin(), newThunks.end(), std::back_inserter(tmp),
                   mergeCmp);

        isd->sections = std::move(tmp);
      });
}

constexpr uint32_t HEXAGON_MASK_END_PACKET = 3 << 14;
constexpr uint32_t HEXAGON_END_OF_PACKET = 3 << 14;
constexpr uint32_t HEXAGON_END_OF_DUPLEX = 0 << 14;

// Return the distance between the packet start and the instruction in the
// relocation.
static int getHexagonPacketOffset(const InputSection &isec,
                                  const Relocation &rel) {
  const ArrayRef<uint8_t> data = isec.content();

  // Search back as many as 3 instructions.
  for (unsigned i = 0;; i++) {
    if (i == 3 || rel.offset < (i + 1) * 4)
      return i * 4;
    uint32_t instWord = 0;
    const ArrayRef<uint8_t> instWordContents =
        data.drop_front(rel.offset - (i + 1) * 4);
    memcpy(&instWord, instWordContents.data(), sizeof(instWord));
    if (((instWord & HEXAGON_MASK_END_PACKET) == HEXAGON_END_OF_PACKET) ||
        ((instWord & HEXAGON_MASK_END_PACKET) == HEXAGON_END_OF_DUPLEX))
      return i * 4;
  }
}
static int64_t getPCBias(Ctx &ctx, const InputSection &isec,
                         const Relocation &rel) {
  if (ctx.arg.emachine == EM_ARM) {
    switch (rel.type) {
    case R_ARM_THM_JUMP19:
    case R_ARM_THM_JUMP24:
    case R_ARM_THM_CALL:
      return 4;
    default:
      return 8;
    }
  }
  if (ctx.arg.emachine == EM_HEXAGON)
    return -getHexagonPacketOffset(isec, rel);
  return 0;
}

// Find or create a ThunkSection within the InputSectionDescription (ISD) that
// is in range of Src. An ISD maps to a range of InputSections described by a
// linker script section pattern such as { .text .text.* }.
ThunkSection *ThunkCreator::getISDThunkSec(OutputSection *os,
                                           InputSection *isec,
                                           InputSectionDescription *isd,
                                           const Relocation &rel,
                                           uint64_t src) {
  // See the comment in getThunk for -pcBias below.
  const int64_t pcBias = getPCBias(ctx, *isec, rel);
  for (std::pair<ThunkSection *, uint32_t> tp : isd->thunkSections) {
    ThunkSection *ts = tp.first;
    uint64_t tsBase = os->addr + ts->outSecOff - pcBias;
    uint64_t tsLimit = tsBase + ts->getSize();
    if (ctx.target->inBranchRange(rel.type, src,
                                  (src > tsLimit) ? tsBase : tsLimit))
      return ts;
  }

  // No suitable ThunkSection exists. This can happen when there is a branch
  // with lower range than the ThunkSection spacing or when there are too
  // many Thunks. Create a new ThunkSection as close to the InputSection as
  // possible. Error if InputSection is so large we cannot place ThunkSection
  // anywhere in Range.
  uint64_t thunkSecOff = isec->outSecOff;
  if (!ctx.target->inBranchRange(rel.type, src,
                                 os->addr + thunkSecOff + rel.addend)) {
    thunkSecOff = isec->outSecOff + isec->getSize();
    if (!ctx.target->inBranchRange(rel.type, src,
                                   os->addr + thunkSecOff + rel.addend))
      Fatal(ctx) << "InputSection too large for range extension thunk "
                 << isec->getObjMsg(src - (os->addr << isec->outSecOff));
  }
  return addThunkSection(os, isd, thunkSecOff);
}

// Add a Thunk that needs to be placed in a ThunkSection that immediately
// precedes its Target.
ThunkSection *ThunkCreator::getISThunkSec(InputSection *isec) {
  ThunkSection *ts = thunkedSections.lookup(isec);
  if (ts)
    return ts;

  // Find InputSectionRange within Target Output Section (TOS) that the
  // InputSection (IS) that we need to precede is in.
  OutputSection *tos = isec->getParent();
  for (SectionCommand *bc : tos->commands) {
    auto *isd = dyn_cast<InputSectionDescription>(bc);
    if (!isd || isd->sections.empty())
      continue;

    InputSection *first = isd->sections.front();
    InputSection *last = isd->sections.back();

    if (isec->outSecOff < first->outSecOff || last->outSecOff < isec->outSecOff)
      continue;

    ts = addThunkSection(tos, isd, isec->outSecOff);
    thunkedSections[isec] = ts;
    return ts;
  }

  return nullptr;
}

// Create one or more ThunkSections per OS that can be used to place Thunks.
// We attempt to place the ThunkSections using the following desirable
// properties:
// - Within range of the maximum number of callers
// - Minimise the number of ThunkSections
//
// We follow a simple but conservative heuristic to place ThunkSections at
// offsets that are multiples of a Target specific branch range.
// For an InputSectionDescription that is smaller than the range, a single
// ThunkSection at the end of the range will do.
//
// For an InputSectionDescription that is more than twice the size of the range,
// we place the last ThunkSection at range bytes from the end of the
// InputSectionDescription in order to increase the likelihood that the
// distance from a thunk to its target will be sufficiently small to
// allow for the creation of a short thunk.
void ThunkCreator::createInitialThunkSections(
    ArrayRef<OutputSection *> outputSections) {
  uint32_t thunkSectionSpacing = ctx.target->getThunkSectionSpacing();
  forEachInputSectionDescription(
      outputSections, [&](OutputSection *os, InputSectionDescription *isd) {
        if (isd->sections.empty())
          return;

        uint32_t isdBegin = isd->sections.front()->outSecOff;
        uint32_t isdEnd =
            isd->sections.back()->outSecOff + isd->sections.back()->getSize();
        uint32_t lastThunkLowerBound = -1;
        if (isdEnd - isdBegin > thunkSectionSpacing * 2)
          lastThunkLowerBound = isdEnd - thunkSectionSpacing;

        uint32_t isecLimit;
        uint32_t prevIsecLimit = isdBegin;
        uint32_t thunkUpperBound = isdBegin + thunkSectionSpacing;

        for (const InputSection *isec : isd->sections) {
          isecLimit = isec->outSecOff + isec->getSize();
          if (isecLimit > thunkUpperBound) {
            addThunkSection(os, isd, prevIsecLimit);
            thunkUpperBound = prevIsecLimit + thunkSectionSpacing;
          }
          if (isecLimit > lastThunkLowerBound)
            break;
          prevIsecLimit = isecLimit;
        }
        addThunkSection(os, isd, isecLimit);
      });
}

ThunkSection *ThunkCreator::addThunkSection(OutputSection *os,
                                            InputSectionDescription *isd,
                                            uint64_t off) {
  auto *ts = make<ThunkSection>(ctx, os, off);
  ts->partition = os->partition;
  if ((ctx.arg.fixCortexA53Errata843419 || ctx.arg.fixCortexA8) &&
      !isd->sections.empty()) {
    // The errata fixes are sensitive to addresses modulo 4 KiB. When we add
    // thunks we disturb the base addresses of sections placed after the thunks
    // this makes patches we have generated redundant, and may cause us to
    // generate more patches as different instructions are now in sensitive
    // locations. When we generate more patches we may force more branches to
    // go out of range, causing more thunks to be generated. In pathological
    // cases this can cause the address dependent content pass not to converge.
    // We fix this by rounding up the size of the ThunkSection to 4KiB, this
    // limits the insertion of a ThunkSection on the addresses modulo 4 KiB,
    // which means that adding Thunks to the section does not invalidate
    // errata patches for following code.
    // Rounding up the size to 4KiB has consequences for code-size and can
    // trip up linker script defined assertions. For example the linux kernel
    // has an assertion that what LLD represents as an InputSectionDescription
    // does not exceed 4 KiB even if the overall OutputSection is > 128 Mib.
    // We use the heuristic of rounding up the size when both of the following
    // conditions are true:
    // 1.) The OutputSection is larger than the ThunkSectionSpacing. This
    //     accounts for the case where no single InputSectionDescription is
    //     larger than the OutputSection size. This is conservative but simple.
    // 2.) The InputSectionDescription is larger than 4 KiB. This will prevent
    //     any assertion failures that an InputSectionDescription is < 4 KiB
    //     in size.
    uint64_t isdSize = isd->sections.back()->outSecOff +
                       isd->sections.back()->getSize() -
                       isd->sections.front()->outSecOff;
    if (os->size > ctx.target->getThunkSectionSpacing() && isdSize > 4096)
      ts->roundUpSizeForErrata = true;
  }
  isd->thunkSections.push_back({ts, pass});
  return ts;
}

static bool isThunkSectionCompatible(InputSection *source,
                                     SectionBase *target) {
  // We can't reuse thunks in different loadable partitions because they might
  // not be loaded. But partition 1 (the main partition) will always be loaded.
  if (source->partition != target->partition)
    return target->partition == 1;
  return true;
}

std::pair<Thunk *, bool> ThunkCreator::getThunk(InputSection *isec,
                                                Relocation &rel, uint64_t src) {
  SmallVector<std::unique_ptr<Thunk>, 0> *thunkVec = nullptr;
  // Arm and Thumb have a PC Bias of 8 and 4 respectively, this is cancelled
  // out in the relocation addend. We compensate for the PC bias so that
  // an Arm and Thumb relocation to the same destination get the same keyAddend,
  // which is usually 0.
  const int64_t pcBias = getPCBias(ctx, *isec, rel);
  const int64_t keyAddend = rel.addend + pcBias;

  // We use a ((section, offset), addend) pair to find the thunk position if
  // possible so that we create only one thunk for aliased symbols or ICFed
  // sections. There may be multiple relocations sharing the same (section,
  // offset + addend) pair. We may revert the relocation back to its original
  // non-Thunk target, so we cannot fold offset + addend.
  if (auto *d = dyn_cast<Defined>(rel.sym))
    if (!d->isInPlt(ctx) && d->section)
      thunkVec = &thunkedSymbolsBySectionAndAddend[{{d->section, d->value},
                                                    keyAddend}];
  if (!thunkVec)
    thunkVec = &thunkedSymbols[{rel.sym, keyAddend}];

  // Check existing Thunks for Sym to see if they can be reused
  for (auto &t : *thunkVec)
    if (isThunkSectionCompatible(isec, t->getThunkTargetSym()->section) &&
        t->isCompatibleWith(*isec, rel) &&
        ctx.target->inBranchRange(rel.type, src,
                                  t->getThunkTargetSym()->getVA(ctx, -pcBias)))
      return std::make_pair(t.get(), false);

  // No existing compatible Thunk in range, create a new one
  thunkVec->push_back(addThunk(ctx, *isec, rel));
  return std::make_pair(thunkVec->back().get(), true);
}

std::pair<Thunk *, bool> ThunkCreator::getSyntheticLandingPad(Defined &d,
                                                              int64_t a) {
  auto [it, isNew] = landingPadsBySectionAndAddend.try_emplace(
      {{d.section, d.value}, a}, nullptr);
  if (isNew)
    it->second = addLandingPadThunk(ctx, d, a);
  return {it->second.get(), isNew};
}

// Return true if the relocation target is an in range Thunk.
// Return false if the relocation is not to a Thunk. If the relocation target
// was originally to a Thunk, but is no longer in range we revert the
// relocation back to its original non-Thunk target.
bool ThunkCreator::normalizeExistingThunk(Relocation &rel, uint64_t src) {
  if (Thunk *t = thunks.lookup(rel.sym)) {
    if (ctx.target->inBranchRange(rel.type, src,
                                  rel.sym->getVA(ctx, rel.addend)))
      return true;
    rel.sym = &t->destination;
    rel.addend = t->addend;
    if (rel.sym->isInPlt(ctx))
      rel.expr = toPlt(rel.expr);
  }
  return false;
}

// When indirect branches are restricted, such as AArch64 BTI Thunks may need
// to target a linker generated landing pad instead of the target. This needs
// to be done once per pass as the need for a BTI thunk is dependent whether
// a thunk is short or long. We iterate over all the thunks to make sure we
// catch thunks that have been created but are no longer live. Non-live thunks
// are not reachable via normalizeExistingThunk() but are still written.
bool ThunkCreator::addSyntheticLandingPads() {
  bool addressesChanged = false;
  for (Thunk *t : allThunks) {
    if (!t->needsSyntheticLandingPad())
      continue;
    Thunk *lpt;
    bool isNew;
    auto &dr = cast<Defined>(t->destination);
    std::tie(lpt, isNew) = getSyntheticLandingPad(dr, t->addend);
    if (isNew) {
      addressesChanged = true;
      getISThunkSec(cast<InputSection>(dr.section))->addThunk(lpt);
    }
    t->landingPad = lpt->getThunkTargetSym();
  }
  return addressesChanged;
}

// Process all relocations from the InputSections that have been assigned
// to InputSectionDescriptions and redirect through Thunks if needed. The
// function should be called iteratively until it returns false.
//
// PreConditions:
// All InputSections that may need a Thunk are reachable from
// OutputSectionCommands.
//
// All OutputSections have an address and all InputSections have an offset
// within the OutputSection.
//
// The offsets between caller (relocation place) and callee
// (relocation target) will not be modified outside of createThunks().
//
// PostConditions:
// If return value is true then ThunkSections have been inserted into
// OutputSections. All relocations that needed a Thunk based on the information
// available to createThunks() on entry have been redirected to a Thunk. Note
// that adding Thunks changes offsets between caller and callee so more Thunks
// may be required.
//
// If return value is false then no more Thunks are needed, and createThunks has
// made no changes. If the target requires range extension thunks, currently
// ARM, then any future change in offset between caller and callee risks a
// relocation out of range error.
bool ThunkCreator::createThunks(uint32_t pass,
                                ArrayRef<OutputSection *> outputSections) {
  this->pass = pass;
  bool addressesChanged = false;

  if (pass == 0 && ctx.target->getThunkSectionSpacing())
    createInitialThunkSections(outputSections);

  if (ctx.arg.emachine == EM_AARCH64)
    addressesChanged = addSyntheticLandingPads();

  // Create all the Thunks and insert them into synthetic ThunkSections. The
  // ThunkSections are later inserted back into InputSectionDescriptions.
  // We separate the creation of ThunkSections from the insertion of the
  // ThunkSections as ThunkSections are not always inserted into the same
  // InputSectionDescription as the caller.
  forEachInputSectionDescription(
      outputSections, [&](OutputSection *os, InputSectionDescription *isd) {
        for (InputSection *isec : isd->sections)
          for (Relocation &rel : isec->relocs()) {
            uint64_t src = isec->getVA(rel.offset);

            // If we are a relocation to an existing Thunk, check if it is
            // still in range. If not then Rel will be altered to point to its
            // original target so another Thunk can be generated.
            if (pass > 0 && normalizeExistingThunk(rel, src))
              continue;

            if (!ctx.target->needsThunk(rel.expr, rel.type, isec->file, src,
                                        *rel.sym, rel.addend))
              continue;

            Thunk *t;
            bool isNew;
            std::tie(t, isNew) = getThunk(isec, rel, src);

            if (isNew) {
              // Find or create a ThunkSection for the new Thunk
              ThunkSection *ts;
              if (auto *tis = t->getTargetInputSection())
                ts = getISThunkSec(tis);
              else
                ts = getISDThunkSec(os, isec, isd, rel, src);
              ts->addThunk(t);
              thunks[t->getThunkTargetSym()] = t;
              allThunks.push_back(t);
            }

            // Redirect relocation to Thunk, we never go via the PLT to a Thunk
            rel.sym = t->getThunkTargetSym();
            rel.expr = fromPlt(rel.expr);

            // On AArch64 and PPC, a jump/call relocation may be encoded as
            // STT_SECTION + non-zero addend, clear the addend after
            // redirection.
            if (ctx.arg.emachine != EM_MIPS)
              rel.addend = -getPCBias(ctx, *isec, rel);
          }

        for (auto &p : isd->thunkSections)
          addressesChanged |= p.first->assignOffsets();
      });

  for (auto &p : thunkedSections)
    addressesChanged |= p.second->assignOffsets();

  // Merge all created synthetic ThunkSections back into OutputSection
  mergeThunks(outputSections);
  return addressesChanged;
}

// The following aid in the conversion of call x@GDPLT to call __tls_get_addr
// hexagonNeedsTLSSymbol scans for relocations would require a call to
// __tls_get_addr.
// hexagonTLSSymbolUpdate rebinds the relocation to __tls_get_addr.
bool elf::hexagonNeedsTLSSymbol(ArrayRef<OutputSection *> outputSections) {
  bool needTlsSymbol = false;
  forEachInputSectionDescription(
      outputSections, [&](OutputSection *os, InputSectionDescription *isd) {
        for (InputSection *isec : isd->sections)
          for (Relocation &rel : isec->relocs())
            if (rel.sym->type == llvm::ELF::STT_TLS && rel.expr == R_PLT_PC) {
              needTlsSymbol = true;
              return;
            }
      });
  return needTlsSymbol;
}

void elf::hexagonTLSSymbolUpdate(Ctx &ctx) {
  Symbol *sym = ctx.symtab->find("__tls_get_addr");
  if (!sym)
    return;
  bool needEntry = true;
  forEachInputSectionDescription(
      ctx.outputSections, [&](OutputSection *os, InputSectionDescription *isd) {
        for (InputSection *isec : isd->sections)
          for (Relocation &rel : isec->relocs())
            if (rel.sym->type == llvm::ELF::STT_TLS && rel.expr == R_PLT_PC) {
              if (needEntry) {
                if (sym->auxIdx == 0)
                  sym->allocateAux(ctx);
                addPltEntry(ctx, *ctx.in.plt, *ctx.in.gotPlt, *ctx.in.relaPlt,
                            ctx.target->pltRel, *sym);
                needEntry = false;
              }
              rel.sym = sym;
            }
      });
}

static bool matchesRefTo(const NoCrossRefCommand &cmd, StringRef osec) {
  if (cmd.toFirst)
    return cmd.outputSections[0] == osec;
  return llvm::is_contained(cmd.outputSections, osec);
}

template <class ELFT, class Rels>
static void scanCrossRefs(Ctx &ctx, const NoCrossRefCommand &cmd,
                          OutputSection *osec, InputSection *sec, Rels rels) {
  for (const auto &r : rels) {
    Symbol &sym = sec->file->getSymbol(r.getSymbol(ctx.arg.isMips64EL));
    // A legal cross-reference is when the destination output section is
    // nullptr, osec for a self-reference, or a section that is described by the
    // NOCROSSREFS/NOCROSSREFS_TO command.
    auto *dstOsec = sym.getOutputSection();
    if (!dstOsec || dstOsec == osec || !matchesRefTo(cmd, dstOsec->name))
      continue;

    std::string toSymName;
    if (!sym.isSection())
      toSymName = toStr(ctx, sym);
    else if (auto *d = dyn_cast<Defined>(&sym))
      toSymName = d->section->name;
    Err(ctx) << sec->getLocation(r.r_offset)
             << ": prohibited cross reference from '" << osec->name << "' to '"
             << toSymName << "' in '" << dstOsec->name << "'";
  }
}

// For each output section described by at least one NOCROSSREFS(_TO) command,
// scan relocations from its input sections for prohibited cross references.
template <class ELFT> void elf::checkNoCrossRefs(Ctx &ctx) {
  for (OutputSection *osec : ctx.outputSections) {
    for (const NoCrossRefCommand &noxref : ctx.script->noCrossRefs) {
      if (!llvm::is_contained(noxref.outputSections, osec->name) ||
          (noxref.toFirst && noxref.outputSections[0] == osec->name))
        continue;
      for (SectionCommand *cmd : osec->commands) {
        auto *isd = dyn_cast<InputSectionDescription>(cmd);
        if (!isd)
          continue;
        parallelForEach(isd->sections, [&](InputSection *sec) {
          invokeOnRelocs(*sec, scanCrossRefs<ELFT>, ctx, noxref, osec, sec);
        });
      }
    }
  }
}

template void elf::scanRelocations<ELF32LE>(Ctx &);
template void elf::scanRelocations<ELF32BE>(Ctx &);
template void elf::scanRelocations<ELF64LE>(Ctx &);
template void elf::scanRelocations<ELF64BE>(Ctx &);

template void elf::checkNoCrossRefs<ELF32LE>(Ctx &);
template void elf::checkNoCrossRefs<ELF32BE>(Ctx &);
template void elf::checkNoCrossRefs<ELF64LE>(Ctx &);
template void elf::checkNoCrossRefs<ELF64BE>(Ctx &);
