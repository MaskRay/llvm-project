//===- Target.cpp ---------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Machine-specific things, such as applying relocations, creation of
// GOT or PLT entries, etc., are handled in this file.
//
// Refer the ELF spec for the single letter variables, S, A or P, used
// in this file.
//
// Some functions defined in this file has "relaxTls" as part of their names.
// They do peephole optimization for TLS variables by rewriting instructions.
// They are not part of the ABI but optional optimization, so you can skip
// them if you are not interested in how TLS variables are optimized.
// See the following paper for the details.
//
//   Ulrich Drepper, ELF Handling For Thread-Local Storage
//   http://www.akkadia.org/drepper/tls.pdf
//
//===----------------------------------------------------------------------===//

#include "Target.h"
#include "InputFiles.h"
#include "OutputSections.h"
#include "RelocScan.h"
#include "SymbolTable.h"
#include "Symbols.h"
#include "lld/Common/ErrorHandler.h"
#include "llvm/Object/ELF.h"

using namespace llvm;
using namespace llvm::object;
using namespace llvm::ELF;
using namespace lld;
using namespace lld::elf;

std::string elf::toStr(Ctx &ctx, RelType type) {
  StringRef s = getELFRelocationTypeName(ctx.arg.emachine, type);
  if (s == "Unknown")
    return ("Unknown (" + Twine(type) + ")").str();
  return std::string(s);
}

const ELFSyncStream &elf::operator<<(const ELFSyncStream &s, RelType type) {
  s << toStr(s.ctx, type);
  return s;
}

void elf::setTarget(Ctx &ctx) {
  switch (ctx.arg.emachine) {
  case EM_386:
  case EM_IAMCU:
    return setX86TargetInfo(ctx);
  case EM_AARCH64:
    return setAArch64TargetInfo(ctx);
  case EM_AMDGPU:
    return setAMDGPUTargetInfo(ctx);
  case EM_ARM:
    return setARMTargetInfo(ctx);
  case EM_AVR:
    return setAVRTargetInfo(ctx);
  case EM_HEXAGON:
    return setHexagonTargetInfo(ctx);
  case EM_LOONGARCH:
    return setLoongArchTargetInfo(ctx);
  case EM_MIPS:
    return setMipsTargetInfo(ctx);
  case EM_MSP430:
    return setMSP430TargetInfo(ctx);
  case EM_PPC:
    return setPPCTargetInfo(ctx);
  case EM_PPC64:
    return setPPC64TargetInfo(ctx);
  case EM_RISCV:
    return setRISCVTargetInfo(ctx);
  case EM_SPARCV9:
    return setSPARCV9TargetInfo(ctx);
  case EM_S390:
    return setSystemZTargetInfo(ctx);
  case EM_X86_64:
    return setX86_64TargetInfo(ctx);
  default:
    Fatal(ctx) << "unsupported e_machine value: " << ctx.arg.emachine;
  }
}

ErrorPlace elf::getErrorPlace(Ctx &ctx, const uint8_t *loc) {
  assert(loc != nullptr);
  for (InputSectionBase *d : ctx.inputSections) {
    auto *isec = dyn_cast<InputSection>(d);
    if (!isec || !isec->getParent() || (isec->type & SHT_NOBITS))
      continue;

    const uint8_t *isecLoc =
        ctx.bufferStart
            ? (ctx.bufferStart + isec->getParent()->offset + isec->outSecOff)
            : isec->contentMaybeDecompress().data();
    if (isecLoc == nullptr) {
      assert(isa<SyntheticSection>(isec) && "No data but not synthetic?");
      continue;
    }
    if (isecLoc <= loc && loc < isecLoc + isec->getSize()) {
      std::string objLoc = isec->getLocation(loc - isecLoc);
      // Return object file location and source file location.
      ELFSyncStream msg(ctx, DiagLevel::None);
      if (isec->file)
        msg << isec->getSrcMsg(*ctx.dummySym, loc - isecLoc);
      return {isec, objLoc + ": ", std::string(msg.str())};
    }
  }
  return {};
}

TargetInfo::~TargetInfo() {}

int64_t TargetInfo::getImplicitAddend(const uint8_t *buf, RelType type) const {
  InternalErr(ctx, buf) << "cannot read addend for relocation " << type;
  return 0;
}

bool TargetInfo::usesOnlyLowPageBits(RelType type) const { return false; }

bool TargetInfo::needsThunk(RelExpr expr, RelType type, const InputFile *file,
                            uint64_t branchAddr, const Symbol &s,
                            int64_t a) const {
  return false;
}

bool TargetInfo::adjustPrologueForCrossSplitStack(uint8_t *loc, uint8_t *end,
                                                  uint8_t stOther) const {
  Err(ctx) << "target doesn't support split stacks";
  return false;
}

bool TargetInfo::inBranchRange(RelType type, uint64_t src, uint64_t dst) const {
  return true;
}

RelExpr TargetInfo::adjustTlsExpr(RelType type, RelExpr expr) const {
  return expr;
}

RelExpr TargetInfo::adjustGotPcExpr(RelType type, int64_t addend,
                                    const uint8_t *data) const {
  return R_GOT_PC;
}

template <class ELFT, class RelTy>
void TargetInfo::scanSectionImpl(InputSectionBase &sec, Relocs<RelTy> rels) {
  RelocScan rs(ctx, sec);
  // Many relocations end up in sec.relocations.
  sec.relocations.reserve(rels.size());

  // On SystemZ, all sections need to be sorted by r_offset, to allow TLS
  // relaxation to be handled correctly - see SystemZ::getTlsGdRelaxSkip.
  SmallVector<RelTy, 0> storage;
  if (ctx.arg.emachine == EM_S390)
    rels = sortRels(rels, storage);

  if constexpr (RelTy::IsCrel) {
    for (auto i = rels.begin(); i != rels.end(); ++i)
      rs.scan<ELFT, RelTy>(i, i->getType(false));
  } else {
    // The non-CREL code path has additional check for PPC64 TLS.
    rs.end = static_cast<const void *>(rels.end());
    for (auto i = rels.begin(); i != rs.end; ++i)
      rs.scan<ELFT, RelTy>(i, i->getType(false));
  }

  // Sort relocations by offset for more efficient searching for
  // R_RISCV_PCREL_HI20, ALIGN relocations, R_PPC64_ADDR64 and the
  // branch-to-branch optimization.
  if (is_contained({EM_RISCV, EM_LOONGARCH}, ctx.arg.emachine) ||
      (ctx.arg.emachine == EM_PPC64 && sec.name == ".toc") ||
      ctx.arg.branchToBranch)
    llvm::stable_sort(sec.relocs(),
                      [](const Relocation &lhs, const Relocation &rhs) {
                        return lhs.offset < rhs.offset;
                      });
}

template <class ELFT> void TargetInfo::scanSectionAux(InputSectionBase &sec) {
  const RelsOrRelas<ELFT> rels = sec.template relsOrRelas<ELFT>();
  if (rels.areRelocsCrel())
    scanSectionImpl<ELFT>(sec, rels.crels);
  else if (rels.areRelocsRel())
    scanSectionImpl<ELFT>(sec, rels.rels);
  else
    scanSectionImpl<ELFT>(sec, rels.relas);
}

void TargetInfo::scanSection(InputSectionBase &sec) {
  invokeELFT(scanSectionAux, sec);
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
  explicit OffsetGetter(EhInputSection &sec) {
    cies = sec.cies;
    fdes = sec.fdes;
    i = cies.begin();
    j = fdes.begin();
  }

  // Translates offsets in input sections to offsets in output sections.
  // Given offset must increase monotonically. We assume that Piece is
  // sorted by inputOff.
  uint64_t get(Ctx &ctx, uint64_t off) {
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
} // namespace

void TargetInfo::scanEhSection(EhInputSection &sec) {
  RelocScan rs(ctx, sec);
  OffsetGetter getter(sec);
  auto rels = sec.rels;
  sec.relocations.reserve(rels.size());
  for (auto &r : rels) {
    // Ignore R_*_NONE and other marker relocations.
    if (r.expr == R_NONE)
      continue;
    uint64_t offset = getter.get(ctx, r.offset);
    // Skip if the relocation offset is within a dead piece.
    if (offset == uint64_t(-1))
      continue;
    Symbol *sym = r.sym;
    if (sym->isUndefined() &&
        maybeReportUndefined(ctx, cast<Undefined>(*sym), sec, offset))
      continue;
    rs.process(r.expr, r.type, offset, *sym, r.addend);
  }
}

static void relocateImpl(const TargetInfo &target, InputSectionBase &sec,
                         uint64_t secAddr, uint8_t *buf) {
  auto &ctx = target.ctx;
  const unsigned bits = ctx.arg.is64 ? 64 : 32;
  for (const Relocation &rel : sec.relocs()) {
    uint8_t *loc = buf + rel.offset;
    const uint64_t val = SignExtend64(
        sec.getRelocTargetVA(ctx, rel, secAddr + rel.offset), bits);
    if (rel.expr != R_RELAX_HINT)
      target.relocate(loc, rel, val);
  }
}

void TargetInfo::relocateAlloc(InputSection &sec, uint8_t *buf) const {
  uint64_t secAddr = sec.getOutputSection()->addr + sec.outSecOff;
  relocateImpl(*this, sec, secAddr, buf);
}

// A variant of relocateAlloc that processes an EhInputSection.
void TargetInfo::relocateEh(EhInputSection &sec, uint8_t *buf) const {
  uint64_t secAddr = sec.getOutputSection()->addr + sec.getParent()->outSecOff;
  relocateImpl(*this, sec, secAddr, buf);
}

uint64_t TargetInfo::getImageBase() const {
  // Use --image-base if set. Fall back to the target default if not.
  if (ctx.arg.imageBase)
    return *ctx.arg.imageBase;
  return ctx.arg.isPic ? 0 : defaultImageBase;
}
