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
inline constexpr uint64_t buildMask() { return 0; }

template <typename... Tails>
inline constexpr uint64_t buildMask(int head, Tails... tails) {
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

inline RelType getMipsPairType(RelType type, bool isLocal) {
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
inline bool isAbsolute(const Symbol &sym) {
  if (sym.isUndefined())
    return true;
  if (const auto *dr = dyn_cast<Defined>(&sym))
    return dr->section == nullptr; // Absolute symbol.
  return false;
}

inline bool isAbsoluteValue(const Symbol &sym) {
  return isAbsolute(sym) || sym.isTls();
}

// Returns true if Expr refers a PLT entry.
inline bool needsPlt(RelExpr expr) {
  return oneof<R_PLT, R_PLT_PC, R_PLT_GOTREL, R_PLT_GOTPLT, R_GOTPLT_GOTREL,
               R_GOTPLT_PC, RE_LOONGARCH_PLT_PAGE_PC, RE_PPC32_PLTREL,
               RE_PPC64_CALL_PLT>(expr);
}

// True if this expression is of the form Sym - X, where X is a position in the
// file (PC, or GOT for example).
inline bool isRelExpr(RelExpr expr) {
  return oneof<R_PC, R_GOTREL, R_GOTPLTREL, RE_ARM_PCA, RE_MIPS_GOTREL,
               RE_PPC64_CALL, RE_PPC64_RELAX_TOC, RE_AARCH64_PAGE_PC,
               R_RELAX_GOT_PC, RE_RISCV_PC_INDIRECT, RE_PPC64_RELAX_GOT_PC,
               RE_LOONGARCH_PAGE_PC>(expr);
}

inline RelExpr fromPlt(RelExpr expr) {
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
inline SmallSet<SharedSymbol *, 4> getSymbolsAt(Ctx &ctx, SharedSymbol &ss) {
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

namespace {
// This class encapsulates states needed to scan relocations for one
// InputSectionBase.
class RelocScan {
public:
  RelocScan(Ctx &ctx, InputSectionBase &isec) : ctx(ctx), sec(&isec) {}
  template <class ELFT, class RelTy>
  void scan(typename Relocs<RelTy>::const_iterator &i, RelType type);

  Ctx &ctx;
  InputSectionBase *sec;

  // End of relocations, used by Mips/PPC64.
  const void *end = nullptr;

  template <class ELFT, class RelTy>
  int64_t computeMipsAddend(const RelTy &rel, RelExpr expr, bool isLocal) const;
  bool isStaticLinkTimeConstant(RelExpr e, RelType type, const Symbol &sym,
                                uint64_t relOff) const;
  void process(RelExpr expr, RelType type, uint64_t offset, Symbol &sym,
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
inline void maybeReportDiscarded(Ctx &ctx, ELFSyncStream &msg, Undefined &sym) {
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
inline bool canDefineSymbolInExecutable(Ctx &ctx, Symbol &sym) {
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
// This only handles relocation types expected in process().
//
// For instance, we know the offset from a relocation to its target at
// link-time if the relocation is PC-relative and refers a
// non-interposable function in the same executable. This function
// will return true for such relocation.
//
// If this function returns false, that means we need to emit a
// dynamic relocation so that the relocation will be fixed at load-time.
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
void RelocScan::process(RelExpr expr, RelType type, uint64_t offset,
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
inline unsigned handleMipsTlsRelocation(Ctx &ctx, RelType type, Symbol &sym,
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

inline unsigned handleAArch64PAuthTlsRelocation(InputSectionBase *sec,
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
unsigned RelocScan::handleTlsRelocation(RelExpr expr, RelType type,
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
void RelocScan::scan(typename Relocs<RelTy>::const_iterator &it, RelType type) {
  const RelTy &rel = *it;
  uint32_t symIndex = rel.getSymbol(ctx.arg.isMips64EL);
  Symbol &sym = sec->getFile<ELFT>()->getSymbol(symIndex);
  // Get an offset in an output section this relocation is applied to.
  uint64_t offset = rel.r_offset;

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
    if ((type == R_PPC64_TLSGD && expr == R_TLSDESC_CALL) ||
        (type == R_PPC64_TLSLD && expr == R_TLSLD_HINT)) {
      // Offset the 4-byte aligned R_PPC64_TLSGD by one byte in the NOTOC
      // case, so we can discern it later from the toc-case.
      auto it1 = it;
      ++it1;
      if (it1->getType(/*isMips64EL=*/false) == R_PPC64_REL24_NOTOC)
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
      it += processed - 1;
      return;
    }
  }

  process(expr, type, offset, sym, addend);
}

} // namespace lld::elf

#endif
