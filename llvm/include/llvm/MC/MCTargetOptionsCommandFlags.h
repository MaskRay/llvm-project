//===-- MCTargetOptionsCommandFlags.h --------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file contains machine code-specific flags that are shared between
// different command line tools.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_MC_MCTARGETOPTIONSCOMMANDFLAGS_H
#define LLVM_MC_MCTARGETOPTIONSCOMMANDFLAGS_H

#include "llvm/Support/Compiler.h"
#include <optional>

namespace llvm {

class MCTargetOptions;
class StringRef;

namespace mc {

/// Constructing this registers the options of MCTargetOptionsCommandFlags.td
/// with cl::ParseCommandLineOptions.
struct RegisterMCTargetOptionsFlags {
  LLVM_ABI RegisterMCTargetOptionsFlags();
};

/// The MCTargetOptions cl::ParseCommandLineOptions parsed.
LLVM_ABI MCTargetOptions InitMCTargetOptionsFromFlags();

/// The value of -mc-relax-all if it was given.
LLVM_ABI std::optional<bool> getExplicitRelaxAll();
LLVM_ABI StringRef getABIName();

} // namespace mc
} // namespace llvm

#endif // LLVM_MC_MCTARGETOPTIONSCOMMANDFLAGS_H
