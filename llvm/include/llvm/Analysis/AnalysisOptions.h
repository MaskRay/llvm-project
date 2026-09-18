//===- AnalysisOptions.h - Options of LLVMAnalysis --------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_ANALYSIS_ANALYSISOPTIONS_H
#define LLVM_ANALYSIS_ANALYSISOPTIONS_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/Support/Compiler.h"
#include <cstddef>
#include <cstdint>
#include <optional>

namespace llvm {

/// The command line options of LLVMAnalysis, declared in AnalysisOptions.td.
struct AnalysisOptions {
  static constexpr LLVMContext::OptionsKind Kind =
      LLVMContext::OptionsKind::Analysis;

#define OPTION_VALUE(ID, TYPE, NAME, DEFAULT) TYPE NAME{DEFAULT};
#include "llvm/Analysis/AnalysisOptions.inc"
#undef OPTION_VALUE

  /// The options of \p Ctx, falling back to current() for a context no tool
  /// attached options to.
  static const AnalysisOptions &get(const LLVMContext &Ctx) {
    if (const AnalysisOptions *O = Ctx.getOptions<AnalysisOptions>())
      return *O;
    return current();
  }

  /// The process-wide instance that tools parse into, for callers without a
  /// context.
  LLVM_ABI static AnalysisOptions &current();

  /// Parse the options this struct declares out of \p Args, appending every
  /// other argument to \p Rest in order. Reports errors and returns false.
  LLVM_ABI bool parse(ArrayRef<const char *> Args,
                      SmallVectorImpl<const char *> &Rest);
};

} // namespace llvm

#endif // LLVM_ANALYSIS_ANALYSISOPTIONS_H
