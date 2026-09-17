//===- UniformityAnalysis.h ---------------------*- C++ -*-----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
/// \file
/// \brief LLVM IR instance of the generic uniformity analysis
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_ANALYSIS_UNIFORMITYANALYSIS_H
#define LLVM_ANALYSIS_UNIFORMITYANALYSIS_H

#include "llvm/ADT/BitVector.h"
#include "llvm/ADT/GenericUniformityInfo.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/IR/Argument.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/PassManager.h"
#include "llvm/IR/SSAContext.h"
#include "llvm/Pass.h"

namespace llvm {

/// IR specialization: index instruction keys by Instruction::getNumber() (a
/// BitVector) and keep arguments in a small pointer set. Because instruction
/// numbers are not reused until a renumbering, a cached result stays safe when
/// instructions are deleted after the analysis runs -- a would-be dangling
/// pointer becomes a stale, unreused number that simply reads as "not uniform".
template <> class UniformValueSet<SSAContext> {
  BitVector InstUniform;
  SmallPtrSet<const Argument *, 8> ArgUniform;

public:
  // Number \p F densely and size the instruction vector. Call before inserts.
  void initialize(const Function &F) {
    const_cast<Function &>(F).renumberInstructions();
    InstUniform.clear();
    InstUniform.resize(F.getMaxInstNumber());
    ArgUniform.clear();
  }
  bool contains(const Value *V) const {
    if (const auto *I = dyn_cast<Instruction>(V)) {
      unsigned N = I->getNumber();
      return N < InstUniform.size() && InstUniform[N];
    }
    if (const auto *A = dyn_cast<Argument>(V))
      return ArgUniform.contains(A);
    return false;
  }
  void insert(const Value *V) {
    if (const auto *I = dyn_cast<Instruction>(V)) {
      unsigned N = I->getNumber();
      if (N < InstUniform.size())
        InstUniform.set(N);
    } else if (const auto *A = dyn_cast<Argument>(V))
      ArgUniform.insert(A);
  }
  bool erase(const Value *V) {
    if (const auto *I = dyn_cast<Instruction>(V)) {
      unsigned N = I->getNumber();
      if (N < InstUniform.size() && InstUniform.test(N)) {
        InstUniform.reset(N);
        return true;
      }
      return false;
    }
    if (const auto *A = dyn_cast<Argument>(V))
      return ArgUniform.erase(A);
    return false;
  }
};

extern template class GenericUniformityInfo<SSAContext>;
using UniformityInfo = GenericUniformityInfo<SSAContext>;

/// Analysis pass which computes \ref UniformityInfo.
class UniformityInfoAnalysis
    : public AnalysisInfoMixin<UniformityInfoAnalysis> {
  friend AnalysisInfoMixin<UniformityInfoAnalysis>;
  static AnalysisKey Key;

public:
  /// Provide the result typedef for this analysis pass.
  using Result = UniformityInfo;

  /// Run the analysis pass over a function and produce a dominator tree.
  LLVM_ABI UniformityInfo run(Function &F, FunctionAnalysisManager &);

  // TODO: verify analysis
};

/// Printer pass for the \c UniformityInfo.
class UniformityInfoPrinterPass
    : public RequiredPassInfoMixin<UniformityInfoPrinterPass> {
  raw_ostream &OS;

public:
  LLVM_ABI explicit UniformityInfoPrinterPass(raw_ostream &OS);

  LLVM_ABI PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM);
};

/// Legacy analysis pass which computes a \ref CycleInfo.
class LLVM_ABI UniformityInfoWrapperPass : public FunctionPass {
  Function *Fn = nullptr;
  UniformityInfo UI;

public:
  static char ID;

  UniformityInfoWrapperPass();

  UniformityInfo &getUniformityInfo() { return UI; }
  const UniformityInfo &getUniformityInfo() const { return UI; }

  bool runOnFunction(Function &F) override;
  void getAnalysisUsage(AnalysisUsage &AU) const override;
  void releaseMemory() override;
  void print(raw_ostream &OS, const Module *M = nullptr) const override;

  // TODO: verify analysis
};

} // namespace llvm

#endif // LLVM_ANALYSIS_UNIFORMITYANALYSIS_H
