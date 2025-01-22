#ifndef LLD_ELF_RELOCSCAN_H
#define LLD_ELF_RELOCSCAN_H

#include "Config.h"
#include "InputFiles.h"
#include "InputSection.h"
#include "LinkerScript.h"
#include "OutputSections.h"
#include "Relocations.h"
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
#include "llvm/Support/Endian.h"

using namespace llvm;
using namespace llvm::ELF;
using namespace llvm::object;

namespace lld::elf {

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
    // to allocate only one GOT entry for every 64 KBytes of local data.
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
  if (sym.isUndefWeak())
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

// True if this expression is of the form Sym - X, where X is a position in the
// file (PC, or GOT for example).
static bool isRelExpr(RelExpr expr) {
  return oneof<R_PC, R_GOTREL, R_GOTPLTREL, RE_ARM_PCA, RE_MIPS_GOTREL,
               RE_PPC64_CALL, RE_PPC64_RELAX_TOC, RE_AARCH64_PAGE_PC,
               R_RELAX_GOT_PC, RE_RISCV_PC_INDIRECT, RE_PPC64_RELAX_GOT_PC,
               RE_LOONGARCH_PAGE_PC>(expr);
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
      if (i == cies.begin() || i[-1].inputOff + i[-1].size <= off)
        Fatal(ctx) << ".eh_frame: relocation is not in any piece";
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
class RelocScan {
public:
  RelocScan(Ctx &ctx) : ctx(ctx) {}
  template <class ELFT>
  void scanSection(InputSectionBase &s, bool isEH = false);
  template <class ELFT, class RelTy>
  void scanOne(typename Relocs<RelTy>::const_iterator &i);

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
};
} // namespace

// MIPS has an odd notion of "paired" relocations to calculate addends.
// For example, if a relocation is of R_MIPS_HI16, there must be a
// R_MIPS_LO16 relocation after that, and an addend is calculated using
// the two relocations.
template <class ELFT, class RelTy>
int64_t RelocScan::computeMipsAddend(const RelTy &rel, RelExpr expr,
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

bool RelocScan::isStaticLinkTimeConstant(RelExpr e, RelType type,
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
  if (sym.isPreemptible || e == RE_AARCH64_AUTH)
    return false;
  if (!ctx.arg.isPic)
    return true;

  // Constant when referencing a non-preemptible symbol.
  if (e == R_SIZE || e == RE_RISCV_LEB128)
    return true;

  // For the target and the relocation, we want to know if they are
  // absolute or relative.
  bool absVal = isAbsoluteValue(sym);
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
  if (sym.isUndefWeak())
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

void RelocScan::processAux(RelExpr expr, RelType type, uint64_t offset,
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
    } else if (!isAbsoluteValue(sym)) {
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
  //
  // The behavior of an undefined weak reference is implementation defined. For
  // non-link-time constants, we resolve relocations statically (let
  // relocate{,Non}Alloc() resolve them) for -no-pie and try producing dynamic
  // relocations for -pie and -shared.
  //
  // The general expectation of -no-pie static linking is that there is no
  // dynamic relocation (except IRELATIVE). Emitting dynamic relocations for
  // -shared matches the spirit of its -z undefs default. -pie has freedom on
  // choices, and we choose dynamic relocations to be consistent with the
  // handling of GOT-generating relocations.
  if (isStaticLinkTimeConstant(expr, type, sym, offset) ||
      (!ctx.arg.isPic && sym.isUndefWeak())) {
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
          part.relaDyn->addReloc({R_AARCH64_AUTH_RELATIVE, sec, offset,
                                  DynamicReloc::AddendOnlyWithTargetVA, sym,
                                  addend, R_ABS});
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

unsigned RelocScan::handleTlsRelocation(RelExpr expr, RelType type,
                                        uint64_t offset, Symbol &sym,
                                        int64_t addend) {
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

  // LoongArch does not yet implement transition from TLSDESC to LE/IE, so
  // generate TLSDESC dynamic relocation for the dynamic linker to handle.
  if (ctx.arg.emachine == EM_LOONGARCH &&
      oneof<RE_LOONGARCH_TLSDESC_PAGE_PC, R_TLSDESC, R_TLSDESC_PC,
            R_TLSDESC_CALL>(expr)) {
    if (expr != R_TLSDESC_CALL) {
      sym.setFlags(NEEDS_TLSDESC);
      sec->addReloc({expr, type, offset, addend, &sym});
    }
    return 1;
  }

  bool isRISCV = ctx.arg.emachine == EM_RISCV;

  if (oneof<RE_AARCH64_TLSDESC_PAGE, R_TLSDESC, R_TLSDESC_CALL, R_TLSDESC_PC,
            R_TLSDESC_GOTPLT>(expr) &&
      ctx.arg.shared) {
    // R_RISCV_TLSDESC_{LOAD_LO12,ADD_LO12_I,CALL} reference a label. Do not
    // set NEEDS_TLSDESC on the label.
    if (expr != R_TLSDESC_CALL) {
      if (!isRISCV || type == R_RISCV_TLSDESC_HI20)
        sym.setFlags(NEEDS_TLSDESC);
      sec->addReloc({expr, type, offset, addend, &sym});
    }
    return 1;
  }

  // ARM, Hexagon, LoongArch and RISC-V do not support GD/LD to IE/LE
  // optimizations.
  // RISC-V supports TLSDESC to IE/LE optimizations.
  // For PPC64, if the file has missing R_PPC64_TLSGD/R_PPC64_TLSLD, disable
  // optimization as well.
  bool execOptimize =
      !ctx.arg.shared && ctx.arg.emachine != EM_ARM &&
      ctx.arg.emachine != EM_HEXAGON && ctx.arg.emachine != EM_LOONGARCH &&
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

  if (oneof<RE_AARCH64_TLSDESC_PAGE, R_TLSDESC, R_TLSDESC_CALL, R_TLSDESC_PC,
            R_TLSDESC_GOTPLT, R_TLSGD_GOT, R_TLSGD_GOTPLT, R_TLSGD_PC,
            RE_LOONGARCH_TLSGD_PAGE_PC>(expr)) {
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

  return 0;
}

template <class ELFT, class RelTy>
void RelocScan::scanOne(typename Relocs<RelTy>::const_iterator &i) {
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
  // Ignore R_*_NONE and other marker relocations.
  if (expr == R_NONE)
    return;
  int64_t addend = RelTy::HasAddend
                       ? getAddend<ELFT>(rel)
                       : ctx.target->getImplicitAddend(
                             sec->content().data() + rel.r_offset, type);
  if (LLVM_UNLIKELY(ctx.arg.emachine == EM_MIPS))
    addend += computeMipsAddend<ELFT>(rel, expr, sym.isLocal());
  else if (ctx.arg.emachine == EM_PPC64 && ctx.arg.isPic && type == R_PPC64_TOC)
    addend += getPPC64TocBase(ctx);

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

} // namespace lld::elf

#endif
