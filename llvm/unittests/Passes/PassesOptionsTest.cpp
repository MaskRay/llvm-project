//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Passes/PassesOptions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

using namespace llvm;
using testing::ElementsAre;
using testing::StrEq;

TEST(PassesOptionsTest, Parse) {
  PassesOptions O;
  SmallVector<const char *> Rest;
  const char *Args[] = {"opt",
                        "-enable-module-inliner",
                        "-o",
                        "out.bc",
                        "--preinline-threshold",
                        "7",
                        "-no-enable-chr",
                        "-enable-newgvn=1",
                        "-enable-ml-inliner=release",
                        "-attributor-enable",
                        "cgscc-light",
                        "in.ll"};
  EXPECT_TRUE(O.parse(Args, Rest, errs()));
  EXPECT_TRUE(O.EnableModuleInliner);
  EXPECT_EQ(O.PreInlineThreshold, 7);
  EXPECT_FALSE(O.EnableCHR);
  EXPECT_TRUE(O.RunNewGVN);
  EXPECT_EQ(O.UseInlineAdvisor, InliningAdvisorMode::Release);
  EXPECT_EQ(O.AttributorRun, AttributorRunOption::CGSCC_LIGHT);
  EXPECT_TRUE(O.EnableGlobalAnalyses);
  EXPECT_THAT(Rest, ElementsAre(StrEq("opt"), StrEq("-o"), StrEq("out.bc"),
                                StrEq("in.ll")));

  std::string Err;
  raw_string_ostream ErrOS(Err);
  const char *BadArgs[] = {"opt", "-enable-ml-inliner=sometimes"};
  EXPECT_FALSE(O.parse(BadArgs, Rest, ErrOS));
  EXPECT_EQ(Err,
            "invalid value 'sometimes' in '-enable-ml-inliner=sometimes'\n");
  Err.clear();
  const char *BadBool[] = {"opt", "-enable-chr="};
  EXPECT_FALSE(O.parse(BadBool, Rest, ErrOS));
  EXPECT_EQ(Err, "invalid value '' in '-enable-chr='\n");
}

TEST(PassesOptionsTest, ThroughCommandLine) {
  cl::ResetAllOptionOccurrences();
  const char *Args[] = {"opt", "-enable-newgvn", "-preinline-threshold=3"};
  EXPECT_TRUE(cl::ParseCommandLineOptions(std::size(Args), Args, "", &errs()));
  EXPECT_TRUE(PassesOptions::Global.RunNewGVN);
  EXPECT_EQ(PassesOptions::Global.PreInlineThreshold, 3);
  cl::ResetAllOptionOccurrences();
  EXPECT_FALSE(PassesOptions::Global.RunNewGVN);
  EXPECT_EQ(PassesOptions::Global.PreInlineThreshold, 75);

  std::string Err;
  raw_string_ostream ErrOS(Err);
  const char *BadArgs[] = {"opt", "-preinline-threshold"};
  EXPECT_FALSE(
      cl::ParseCommandLineOptions(std::size(BadArgs), BadArgs, "", &ErrOS));
  EXPECT_EQ(Err, "opt: option '-preinline-threshold' requires an argument\n");
  cl::ResetAllOptionOccurrences();
}

TEST(PassesOptionsTest, PerContext) {
  LLVMContext CtxA, CtxNone;
  PassesOptions &A = CtxA.setOptions(PassesOptions::Global);
  A.PreInlineThreshold = 1;
  EXPECT_EQ(&CtxA.getOptions<PassesOptions>(), &A);
  EXPECT_EQ(A.PreInlineThreshold, 1);
  EXPECT_EQ(&CtxNone.getOptions<PassesOptions>(), &PassesOptions::Global);
  EXPECT_EQ(PassesOptions::Global.PreInlineThreshold, 75);
}

TEST(PassesOptionsTest, PerPipeline) {
  PassesOptions A, B;
  A.EnableModuleInliner = true;
  B.EnableModuleInliner = false;
  A.MergeFunctions = true;
  auto Pipeline = [](const PassesOptions &O) {
    PipelineTuningOptions PTO(O);
    EXPECT_EQ(PTO.MergeFunctions, O.MergeFunctions);
    PassBuilder PB(nullptr, PTO);
    ModulePassManager MPM =
        PB.buildPerModuleDefaultPipeline(OptimizationLevel::O2);
    std::string S;
    raw_string_ostream OS(S);
    MPM.printPipeline(OS, [](StringRef Name) { return Name; });
    return S;
  };
  std::string WithA = Pipeline(A), WithB = Pipeline(B);
  EXPECT_TRUE(StringRef(WithA).contains("ModuleInlinerPass"));
  EXPECT_TRUE(StringRef(WithA).contains("MergeFunctionsPass"));
  EXPECT_FALSE(StringRef(WithB).contains("ModuleInlinerPass"));
  EXPECT_FALSE(StringRef(WithB).contains("MergeFunctionsPass"));
}
