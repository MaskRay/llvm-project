//===- PassesOptions.cpp - LLVMPasses' command line options ---------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Passes/PassesOptions.h"
#include "llvm/IR/LLVMContext.h"
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
#include "llvm/Passes/PassesOptions.inc"
#undef OPTION
};

#define OPTTABLE_CODE
#include "llvm/Passes/PassesOptions.inc"

const OptTable &table() {
  static const OptTable Table(optionTables());
  return Table;
}

void forEachName(function_ref<void(StringRef)> F) {
  const OptTable &T = table();
  for (unsigned I = 1, E = T.getNumOptions(); I <= E; ++I)
    if (!T.getOptionPrefix(I).empty())
      F(T.getOptionName(I).rtrim('='));
}

bool parseCurrent(ArrayRef<const char *> Args,
                  SmallVectorImpl<const char *> &Rest, raw_ostream &Errs) {
  return PassesOptions::current().parse(Args, Rest, Errs);
}

void printHelp(raw_ostream &OS, bool ShowHidden) {
  OS << '\n';
  table().printHelpOptions(OS, ShowHidden);
}

void reset() { PassesOptions::current() = PassesOptions(); }

const cl::LibraryOptions Library = {forEachName, parseCurrent, printHelp,
                                    reset};
struct Register {
  Register() { cl::registerLibraryOptions(Library); }
} X;
} // namespace

PassesOptions &PassesOptions::current() {
  static PassesOptions Current;
  return Current;
}

unsigned PassesOptions::slot() {
  static unsigned Slot = LLVMContext::allocateOptionsSlot();
  return Slot;
}

bool PassesOptions::parse(ArrayRef<const char *> Args,
                          SmallVectorImpl<const char *> &Rest,
                          raw_ostream &Errs) {
  unsigned MissingIndex, MissingCount;
  InputArgList AL = table().ParseArgs(Args, MissingIndex, MissingCount);
  if (MissingCount) {
    WithColor::error(Errs) << "option '" << Args[MissingIndex]
                           << "' requires an argument\n";
    return false;
  }
  for (const Arg *A : AL) {
    bool Ok = true;
    switch (A->getOption().getID()) {
    case OPT_UNKNOWN:
    case OPT_INPUT:
      Rest.push_back(Args[A->getIndex()]);
      continue;
#define OPTION_VALUE(ID, TYPE, NAME, DEFAULT)                                  \
  case OPT_##ID:                                                               \
    Ok = parseArgValue(A->getValue(), NAME);                                   \
    break;
#define OPTION_ENUM_VALUE(ID, TYPE, NAME, DEFAULT, ...)                        \
  case OPT_##ID:                                                               \
    Ok = parseEnumValue(A->getValue(), NAME, {__VA_ARGS__});                   \
    break;
#include "llvm/Passes/PassesOptions.inc"
#undef OPTION_ENUM_VALUE
#undef OPTION_VALUE
    }
    if (!Ok) {
      WithColor::error(Errs) << "invalid value '" << A->getValue() << "' in '"
                             << A->getAsString(AL) << "'\n";
      return false;
    }
  }
  return true;
}
