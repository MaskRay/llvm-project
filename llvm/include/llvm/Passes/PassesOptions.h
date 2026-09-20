//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_PASSES_PASSESOPTIONS_H
#define LLVM_PASSES_PASSESOPTIONS_H

#include "llvm/Analysis/InlineAdvisor.h"

namespace llvm {

/// The Attributor runs of the default pipelines.
enum AttributorRunOption {
  NONE = 0,
  MODULE = 1 << 0,
  CGSCC = 1 << 1,
  MODULE_LIGHT = 1 << 2,
  CGSCC_LIGHT = 1 << 3,

  FULL = MODULE | CGSCC,
  LIGHT = MODULE_LIGHT | CGSCC_LIGHT
};

} // namespace llvm

// struct PassesOptions, from llvm/Passes/PassesOptions.td.
#define OPTIONS_STRUCT_DECL
#include "llvm/Passes/PassesOptions.inc"

#endif // LLVM_PASSES_PASSESOPTIONS_H
