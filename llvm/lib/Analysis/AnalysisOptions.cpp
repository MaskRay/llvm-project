//===- AnalysisOptions.cpp - Options of LLVMAnalysis ----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Analysis/AnalysisOptions.h"
#include "llvm/Option/ArgList.h"
#include "llvm/Option/ArgValue.h"
#include "llvm/Option/OptTable.h"
#include "llvm/Option/Option.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;
using namespace llvm::opt;

namespace {
enum ID {
  OPT_INVALID = 0,
#define OPTION(...) LLVM_MAKE_OPT_ID(__VA_ARGS__),
#include "llvm/Analysis/AnalysisOptions.inc"
#undef OPTION
};

#define OPTTABLE_CODE
#include "llvm/Analysis/AnalysisOptions.inc"
#undef OPTTABLE_CODE

class AnalysisOptTable : public OptTable {
public:
  AnalysisOptTable() : OptTable(OptionTables) {}
};
} // namespace

AnalysisOptions &AnalysisOptions::current() {
  static AnalysisOptions Options;
  return Options;
}

// Until every tool parses library options itself, cl:: feeds current().
static bool parseCurrent(ArrayRef<const char *> Args,
                         SmallVectorImpl<const char *> &Rest) {
  return AnalysisOptions::current().parse(Args, Rest);
}
static struct RegisterParser {
  RegisterParser() { cl::addLibraryOptionsParser(parseCurrent); }
} X;

bool AnalysisOptions::parse(ArrayRef<const char *> Args,
                            SmallVectorImpl<const char *> &Rest) {
  static const AnalysisOptTable Table;
  unsigned MissingIndex, MissingCount;
  InputArgList AL = Table.ParseArgs(Args, MissingIndex, MissingCount);
  if (MissingCount) {
    errs() << "error: option '" << Args[MissingIndex]
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
    Ok = parseArgValue(*A, NAME);                                              \
    break;
#include "llvm/Analysis/AnalysisOptions.inc"
#undef OPTION_VALUE
    }
    if (!Ok) {
      errs() << "error: invalid value '" << A->getValue() << "' in '"
             << A->getAsString(AL) << "'\n";
      return false;
    }
  }
  return true;
}
