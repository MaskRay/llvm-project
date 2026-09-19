//===- PassesOptions.h - LLVMPasses' command line options -------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_PASSES_PASSESOPTIONS_H
#define LLVM_PASSES_PASSESOPTIONS_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/InlineAdvisor.h"
#include "llvm/Support/Compiler.h"
#include <string>

namespace llvm {
class raw_ostream;

#define OPTION_ENUMS
#include "llvm/Passes/PassesOptions.inc"

/// The command line options of LLVMPasses, declared in
/// llvm/Passes/PassesOptions.td. Code reads them through
/// LLVMContext::getOptions<PassesOptions>().
struct PassesOptions {
#define OPTION_VALUE(ID, TYPE, NAME, DEFAULT) TYPE NAME{DEFAULT};
#define OPTION_ENUM_VALUE(ID, TYPE, NAME, DEFAULT, ...) TYPE NAME{DEFAULT};
#include "llvm/Passes/PassesOptions.inc"
#undef OPTION_ENUM_VALUE
#undef OPTION_VALUE

  /// The instance cl::ParseCommandLineOptions fills, which a context reads
  /// until a tool attaches its own.
  LLVM_ABI static PassesOptions &current();
  /// The struct's slot in LLVMContext.
  LLVM_ABI static unsigned slot();

  /// Parses the options this struct declares out of \p Args, appending every
  /// other argument to \p Rest in order; reports errors to \p Errs and
  /// returns false.
  LLVM_ABI bool parse(ArrayRef<const char *> Args,
                      SmallVectorImpl<const char *> &Rest, raw_ostream &Errs);
};

} // namespace llvm

#endif // LLVM_PASSES_PASSESOPTIONS_H
