//===-- MCTargetOptionsCommandFlags.cpp -----------------------------------===//
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

#include "llvm/MC/MCTargetOptionsCommandFlags.h"
#include "llvm/MC/MCTargetOptions.h"
#include "llvm/Option/ArgList.h"
#include "llvm/Option/ArgValue.h"
#include "llvm/Option/OptTable.h"
#include "llvm/Option/Option.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/WithColor.h"

using namespace llvm;
using namespace llvm::opt;

namespace {
enum ID {
  OPT_INVALID = 0,
#define OPTION(...) LLVM_MAKE_OPT_ID(__VA_ARGS__),
#include "MCTargetOptionsCommandFlags.inc"
#undef OPTION
};

#define OPTTABLE_CODE
#include "MCTargetOptionsCommandFlags.inc"

std::optional<bool> ExplicitRelaxAll;

// The options cl::ParseCommandLineOptions parsed.
MCTargetOptions &flags() {
  static MCTargetOptions Flags;
  return Flags;
}

const OptTable &table() {
  static const OptTable Table(optionTables());
  return Table;
}

bool parse(ArrayRef<const char *> Args, SmallVectorImpl<const char *> &Rest,
           raw_ostream &Errs) {
  unsigned MissingIndex, MissingCount;
  InputArgList AL = table().ParseArgs(Args, MissingIndex, MissingCount);
  if (MissingCount) {
    WithColor::error(Errs) << "option '" << Args[MissingIndex]
                           << "' requires an argument\n";
    return false;
  }
  MCTargetOptions &Flags = flags();
  for (const Arg *A : AL) {
    bool Ok = true;
    switch (A->getOption().getID()) {
    case OPT_UNKNOWN:
    case OPT_INPUT:
      Rest.push_back(Args[A->getIndex()]);
      continue;
#define OPTION_VALUE(ID, TYPE, NAME, DEFAULT)                                  \
  case OPT_##ID: {                                                             \
    TYPE V;                                                                    \
    if ((Ok = parseArgValue(A->getValue(), V)))                                \
      Flags.NAME = std::move(V);                                               \
    break;                                                                     \
  }
#define OPTION_ENUM_VALUE(ID, TYPE, NAME, DEFAULT, ...)                        \
  case OPT_##ID: {                                                             \
    TYPE V;                                                                    \
    if ((Ok = parseEnumValue(A->getValue(), V, {__VA_ARGS__})))                \
      Flags.NAME = V;                                                          \
    break;                                                                     \
  }
#include "MCTargetOptionsCommandFlags.inc"
#undef OPTION_ENUM_VALUE
#undef OPTION_VALUE
    }
    if (!Ok) {
      WithColor::error(Errs) << "invalid value '" << A->getValue() << "' in '"
                             << A->getAsString(AL) << "'\n";
      return false;
    }
  }
  if (AL.hasArg(OPT_mc_relax_all_EQ))
    ExplicitRelaxAll = bool(Flags.MCRelaxAll);
  return true;
}

void printHelp(raw_ostream &OS, bool ShowHidden) {
  OS << '\n';
  table().printHelpOptions(OS, ShowHidden);
}

void reset() {
  flags() = MCTargetOptions();
  ExplicitRelaxAll.reset();
}

const cl::LibraryOptions Library = {parse, printHelp, reset};
} // namespace

mc::RegisterMCTargetOptionsFlags::RegisterMCTargetOptionsFlags() {
  cl::registerLibraryOptions(Library);
}

MCTargetOptions mc::InitMCTargetOptionsFromFlags() { return flags(); }

std::optional<bool> mc::getExplicitRelaxAll() { return ExplicitRelaxAll; }

StringRef mc::getABIName() { return flags().ABIName; }
