//===-- PatchableFunction.cpp - Patchable prologues for LLVM -------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements edits function bodies in place to support the
// "patchable-function" attribute.
//
//===----------------------------------------------------------------------===//

#include "llvm/CodeGen/PatchableFunction.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/TargetInstrInfo.h"
#include "llvm/CodeGen/TargetSubtargetInfo.h"
#include "llvm/InitializePasses.h"
#include "llvm/Pass.h"
#include "llvm/PassRegistry.h"

using namespace llvm;

namespace {
struct PatchableFunction {
  bool run(MachineFunction &F);
};

struct PatchableFunctionLegacy : public MachineFunctionPass {
  static char ID;
  PatchableFunctionLegacy() : MachineFunctionPass(ID) {
    initializePatchableFunctionLegacyPass(*PassRegistry::getPassRegistry());
  }
  bool runOnMachineFunction(MachineFunction &F) override {
    return PatchableFunction().run(F);
  }

  MachineFunctionProperties getRequiredProperties() const override {
    return MachineFunctionProperties().setNoVRegs();
  }
};

} // namespace

PreservedAnalyses
PatchableFunctionPass::run(MachineFunction &MF,
                           MachineFunctionAnalysisManager &MFAM) {
  MFPropsModifier _(*this, MF);
  if (!PatchableFunction().run(MF))
    return PreservedAnalyses::all();
  return getMachineFunctionPassPreservedAnalyses();
}

bool PatchableFunction::run(MachineFunction &MF) {
  MachineBasicBlock &FirstMBB = *MF.begin();

  if (MF.getFunction().hasFnAttribute("patchable-function-entry")) {
    const TargetInstrInfo *TII = MF.getSubtarget().getInstrInfo();
    // The initial .loc covers PATCHABLE_FUNCTION_ENTER.
    BuildMI(FirstMBB, FirstMBB.begin(), DebugLoc(),
            TII->get(TargetOpcode::PATCHABLE_FUNCTION_ENTER));
    return true;
  } else if (MF.getFunction().hasFnAttribute("patchable-function")) {
#ifndef NDEBUG
    Attribute PatchAttr = MF.getFunction().getFnAttribute("patchable-function");
    StringRef PatchType = PatchAttr.getValueAsString();
    assert(PatchType == "prologue-short-redirect" && "Only possibility today!");
#endif
    auto *TII = MF.getSubtarget().getInstrInfo();
    BuildMI(FirstMBB, FirstMBB.begin(), DebugLoc(),
            TII->get(TargetOpcode::PATCHABLE_OP))
        .addImm(2);
    MF.ensureAlignment(Align(16));
    return true;
  }
  return false;
}

char PatchableFunctionLegacy::ID = 0;
char &llvm::PatchableFunctionID = PatchableFunctionLegacy::ID;
INITIALIZE_PASS(PatchableFunctionLegacy, "patchable-function",
                "Implement the 'patchable-function' attribute", false, false)
