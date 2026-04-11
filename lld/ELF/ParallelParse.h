//===- ParallelParse.h - Parallel input file parsing ------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLD_ELF_PARALLEL_PARSE_H
#define LLD_ELF_PARALLEL_PARSE_H

#include "InputFiles.h"

namespace lld::elf {
struct Ctx;

void parallelParseFiles(Ctx &ctx,
                        SmallVector<std::unique_ptr<InputFile>, 0> &files);

} // namespace lld::elf

#endif // LLD_ELF_PARALLEL_PARSE_H
