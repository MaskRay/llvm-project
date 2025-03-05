//===-- PPCMCExpr.cpp - PPC specific MC expression classes ----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "PPCMCExpr.h"
#include "PPCFixupKinds.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/MC/MCAssembler.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCObjectStreamer.h"
#include "llvm/MC/MCSymbolELF.h"
#include "llvm/Support/Casting.h"

using namespace llvm;

#define DEBUG_TYPE "ppcmcexpr"

const PPCMCExpr *PPCMCExpr::create(VariantKind Kind, const MCExpr *Expr,
                                   MCContext &Ctx) {
  return new (Ctx) PPCMCExpr(Kind, Expr);
}

void PPCMCExpr::printImpl(raw_ostream &OS, const MCAsmInfo *MAI) const {
  getSubExpr()->print(OS, MAI);

  if (MAI) {
    OS << '@' << MAI->getVariantKindName(Kind);
    return;
  }

  switch (Kind) {
  default:
    llvm_unreachable("Invalid kind!");
  case VK_PPC_LO:
    OS << "@l";
    break;
  case VK_PPC_HI:
    OS << "@h";
    break;
  case VK_PPC_HA:
    OS << "@ha";
    break;
  case VK_PPC_HIGH:
    OS << "@high";
    break;
  case VK_PPC_HIGHA:
    OS << "@higha";
    break;
  case VK_PPC_HIGHER:
    OS << "@higher";
    break;
  case VK_PPC_HIGHERA:
    OS << "@highera";
    break;
  case VK_PPC_HIGHEST:
    OS << "@highest";
    break;
  case VK_PPC_HIGHESTA:
    OS << "@highesta";
    break;
  }
}

bool
PPCMCExpr::evaluateAsConstant(int64_t &Res) const {
  MCValue Value;

  if (!getSubExpr()->evaluateAsRelocatable(Value, nullptr, nullptr))
    return false;

  if (!Value.isAbsolute())
    return false;
  auto Tmp = evaluateAsInt64(Value.getConstant());
  if (!Tmp)
    return false;
  Res = *Tmp;
  return true;
}

std::optional<int64_t> PPCMCExpr::evaluateAsInt64(int64_t Value) const {
  switch (Kind) {
    case VK_PPC_LO:
      return Value & 0xffff;
    case VK_PPC_HI:
      return (Value >> 16) & 0xffff;
    case VK_PPC_HA:
      return ((Value + 0x8000) >> 16) & 0xffff;
    case VK_PPC_HIGH:
      return (Value >> 16) & 0xffff;
    case VK_PPC_HIGHA:
      return ((Value + 0x8000) >> 16) & 0xffff;
    case VK_PPC_HIGHER:
      return (Value >> 32) & 0xffff;
    case VK_PPC_HIGHERA:
      return ((Value + 0x8000) >> 32) & 0xffff;
    case VK_PPC_HIGHEST:
      return (Value >> 48) & 0xffff;
    case VK_PPC_HIGHESTA:
      return ((Value + 0x8000) >> 48) & 0xffff;
    default:
      return {};
    }
}

bool PPCMCExpr::evaluateAsRelocatableImpl(MCValue &Res, const MCAssembler *Asm,
                                          const MCFixup *Fixup) const {
  if (!getSubExpr()->evaluateAsRelocatable(Res, Asm, Fixup))
    return false;

  std::optional<int64_t> MaybeInt = evaluateAsInt64(Res.getConstant());
  if (Res.isAbsolute() && MaybeInt) {
    int64_t Result = *MaybeInt;
    bool IsHalf16 = Fixup && Fixup->getTargetKind() == PPC::fixup_ppc_half16;
    bool IsHalf16DS =
        Fixup && Fixup->getTargetKind() == PPC::fixup_ppc_half16ds;
    bool IsHalf16DQ =
        Fixup && Fixup->getTargetKind() == PPC::fixup_ppc_half16dq;
    bool IsHalf = IsHalf16 || IsHalf16DS || IsHalf16DQ;

    if (!IsHalf && Result >= 0x8000)
      return false;
    if ((IsHalf16DS && (Result & 0x3)) || (IsHalf16DQ && (Result & 0xf)))
      return false;

    Res = MCValue::get(Result);
  } else {
    Res = MCValue::get(Res.getSymA(), Res.getSymB(), Res.getConstant(),
                       getKind());
  }

  return true;
}

void PPCMCExpr::visitUsedExpr(MCStreamer &Streamer) const {
  Streamer.visitUsedExpr(*getSubExpr());
}

static void fixELFSymbolsInTLSFixupsImpl(const MCExpr *Expr, MCAssembler &Asm) {
  switch (Expr->getKind()) {
  case MCExpr::Target:
    llvm_unreachable("Can't handle nested target expression");
    break;
  case MCExpr::Constant:
    break;

  case MCExpr::Binary: {
    const MCBinaryExpr *BE = cast<MCBinaryExpr>(Expr);
    fixELFSymbolsInTLSFixupsImpl(BE->getLHS(), Asm);
    fixELFSymbolsInTLSFixupsImpl(BE->getRHS(), Asm);
    break;
  }

  case MCExpr::SymbolRef: {
    // We're known to be under a TLS fixup, so any symbol should be
    // modified. There should be only one.
    const MCSymbolRefExpr &SymRef = *cast<MCSymbolRefExpr>(Expr);
    cast<MCSymbolELF>(SymRef.getSymbol()).setType(ELF::STT_TLS);
    break;
  }

  case MCExpr::Unary:
    fixELFSymbolsInTLSFixupsImpl(cast<MCUnaryExpr>(Expr)->getSubExpr(), Asm);
    break;
  }
}

void PPCMCExpr::fixELFSymbolsInTLSFixups(MCAssembler &Asm) const {
  switch (getKind()) {
  case PPCMCExpr::VK_PPC_DTPMOD:
  case PPCMCExpr::VK_PPC_TPREL_LO:
  case PPCMCExpr::VK_PPC_TPREL_HI:
  case PPCMCExpr::VK_PPC_TPREL_HA:
  case PPCMCExpr::VK_PPC_TPREL_HIGH:
  case PPCMCExpr::VK_PPC_TPREL_HIGHA:
  case PPCMCExpr::VK_PPC_TPREL_HIGHER:
  case PPCMCExpr::VK_PPC_TPREL_HIGHERA:
  case PPCMCExpr::VK_PPC_TPREL_HIGHEST:
  case PPCMCExpr::VK_PPC_TPREL_HIGHESTA:
  case PPCMCExpr::VK_PPC_DTPREL_LO:
  case PPCMCExpr::VK_PPC_DTPREL_HI:
  case PPCMCExpr::VK_PPC_DTPREL_HA:
  case PPCMCExpr::VK_PPC_DTPREL_HIGH:
  case PPCMCExpr::VK_PPC_DTPREL_HIGHA:
  case PPCMCExpr::VK_PPC_DTPREL_HIGHER:
  case PPCMCExpr::VK_PPC_DTPREL_HIGHERA:
  case PPCMCExpr::VK_PPC_DTPREL_HIGHEST:
  case PPCMCExpr::VK_PPC_DTPREL_HIGHESTA:
  case PPCMCExpr::VK_PPC_GOT_TPREL:
  case PPCMCExpr::VK_PPC_GOT_TPREL_LO:
  case PPCMCExpr::VK_PPC_GOT_TPREL_HI:
  case PPCMCExpr::VK_PPC_GOT_TPREL_HA:
  case PPCMCExpr::VK_PPC_GOT_TPREL_PCREL:
  case PPCMCExpr::VK_PPC_GOT_DTPREL:
  case PPCMCExpr::VK_PPC_GOT_DTPREL_LO:
  case PPCMCExpr::VK_PPC_GOT_DTPREL_HI:
  case PPCMCExpr::VK_PPC_GOT_DTPREL_HA:
  case PPCMCExpr::VK_PPC_TLS:
  case PPCMCExpr::VK_PPC_TLS_PCREL:
  case PPCMCExpr::VK_PPC_GOT_TLSGD:
  case PPCMCExpr::VK_PPC_GOT_TLSGD_LO:
  case PPCMCExpr::VK_PPC_GOT_TLSGD_HI:
  case PPCMCExpr::VK_PPC_GOT_TLSGD_HA:
  case PPCMCExpr::VK_PPC_GOT_TLSGD_PCREL:
  case PPCMCExpr::VK_PPC_GOT_TLSLD:
  case PPCMCExpr::VK_PPC_GOT_TLSLD_LO:
  case PPCMCExpr::VK_PPC_GOT_TLSLD_HI:
  case PPCMCExpr::VK_PPC_GOT_TLSLD_HA:
    fixELFSymbolsInTLSFixupsImpl(getSubExpr(), Asm);
    break;
  default:
    break;
  }
}
