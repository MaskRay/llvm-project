//=--- X86MCExpr.h - X86 specific MC expression classes ---*- C++ -*-=//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file describes X86-specific MCExprs, i.e, registers used for
// extended variable assignments.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_X86_MCTARGETDESC_X86MCEXPR_H
#define LLVM_LIB_TARGET_X86_MCTARGETDESC_X86MCEXPR_H

#include "X86ATTInstPrinter.h"
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/ErrorHandling.h"

namespace llvm {

class X86MCExpr : public MCTargetExpr {

private:
  const MCRegister Reg; // All

  explicit X86MCExpr(MCRegister R) : Reg(R) {}

public:
  enum VariantKind {
    VK_None,

    VK_X86_ABS8 = 100,
    VK_DTPOFF,
    VK_DTPREL,
    VK_GOT,
    VK_GOTENT,
    VK_GOTNTPOFF,
    VK_GOTOFF,
    VK_GOTPCREL,
    VK_GOTPCREL_NORELAX,
    VK_GOTREL,
    VK_GOTTPOFF,
    VK_INDNTPOFF,
    VK_NTPOFF,
    VK_PCREL,
    VK_PLT,
    VK_X86_PLTOFF,
    VK_SECREL,
    VK_SIZE,
    VK_TLSCALL,
    VK_TLSDESC,
    VK_TLSGD,
    VK_TLSLD,
    VK_TLSLDM,
    VK_TLVP,
    VK_TLVPPAGE,
    VK_TLVPPAGEOFF,
    VK_TPOFF,
  };

  /// @name Construction
  /// @{

  static const X86MCExpr *create(MCRegister Reg, MCContext &Ctx) {
    return new (Ctx) X86MCExpr(Reg);
  }

  /// @}
  /// @name Accessors
  /// @{

  /// getSubExpr - Get the child of this expression.
  MCRegister getReg() const { return Reg; }

  /// @}

  void printImpl(raw_ostream &OS, const MCAsmInfo *MAI) const override {
    if (!MAI || MAI->getAssemblerDialect() == 0)
      OS << '%';
    OS << X86ATTInstPrinter::getRegisterName(Reg);
  }

  bool evaluateAsRelocatableImpl(MCValue &Res, const MCAssembler *Asm,
                                 const MCFixup *Fixup) const override {
    return false;
  }
  // Register values should be inlined as they are not valid .set expressions.
  bool inlineAssignedExpr() const override { return true; }
  bool isEqualTo(const MCExpr *X) const override {
    if (auto *E = dyn_cast<X86MCExpr>(X))
      return getReg() == E->getReg();
    return false;
  }
  void visitUsedExpr(MCStreamer &Streamer) const override {}
  MCFragment *findAssociatedFragment() const override { return nullptr; }

  // There are no TLS X86MCExprs at the moment.
  void fixELFSymbolsInTLSFixups(MCAssembler &Asm) const override {}

  static bool classof(const MCExpr *E) {
    return E->getKind() == MCExpr::Target;
  }
};

static inline X86MCExpr::VariantKind
getVariantKind(const MCSymbolRefExpr *SRE) {
  return X86MCExpr::VariantKind(SRE->getKind());
}
} // end namespace llvm

#endif
