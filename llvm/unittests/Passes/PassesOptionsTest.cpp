//===- PassesOptionsTest.cpp ----------------------------------------------===//
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
                        "-enable-chr=false",
                        "-enable-ml-inliner=release",
                        "-attributor-enable",
                        "cgscc-light",
                        "in.ll"};
  EXPECT_TRUE(O.parse(Args, Rest, errs()));
  EXPECT_TRUE(O.EnableModuleInliner);
  EXPECT_EQ(O.PreInlineThreshold, 7);
  EXPECT_FALSE(O.EnableCHR);
  EXPECT_EQ(O.UseInlineAdvisor, InliningAdvisorMode::Release);
  EXPECT_EQ(O.AttributorRun, AttributorRunOption::CGSCC_LIGHT);
  EXPECT_TRUE(O.EnableGlobalAnalyses);
  EXPECT_THAT(Rest, ElementsAre(StrEq("opt"), StrEq("-o"), StrEq("out.bc"),
                                StrEq("in.ll")));

  std::string Err;
  raw_string_ostream ErrOS(Err);
  const char *BadArgs[] = {"opt", "-enable-ml-inliner=sometimes"};
  EXPECT_FALSE(O.parse(BadArgs, Rest, ErrOS));
  EXPECT_EQ(Err, "error: invalid value 'sometimes' in "
                 "'-enable-ml-inliner=sometimes'\n");
}

TEST(PassesOptionsTest, ThroughCommandLine) {
  cl::ResetAllOptionOccurrences();
  const char *Args[] = {"opt", "-enable-newgvn", "-preinline-threshold=3"};
  EXPECT_TRUE(cl::ParseCommandLineOptions(std::size(Args), Args, "", &errs()));
  EXPECT_TRUE(PassesOptions::current().RunNewGVN);
  EXPECT_EQ(PassesOptions::current().PreInlineThreshold, 3);
  cl::ResetAllOptionOccurrences();
  EXPECT_FALSE(PassesOptions::current().RunNewGVN);
  EXPECT_EQ(PassesOptions::current().PreInlineThreshold, 75);
}

TEST(PassesOptionsTest, PerContext) {
  PassesOptions A;
  A.PreInlineThreshold = 1;
  LLVMContext CtxA, CtxNone;
  CtxA.setOptions(A);
  EXPECT_EQ(CtxA.getOptions<PassesOptions>().PreInlineThreshold, 1);
  EXPECT_EQ(&CtxNone.getOptions<PassesOptions>(), &PassesOptions::current());
}

TEST(PassesOptionsTest, PerPipeline) {
  PassesOptions A, B;
  A.EnableModuleInliner = true;
  B.EnableModuleInliner = false;
  auto Pipeline = [](const PassesOptions &O) {
    PassBuilder PB(nullptr, PipelineTuningOptions(), std::nullopt, nullptr,
                   vfs::getRealFileSystem(), &O);
    ModulePassManager MPM =
        PB.buildPerModuleDefaultPipeline(OptimizationLevel::O2);
    std::string S;
    raw_string_ostream OS(S);
    MPM.printPipeline(OS, [](StringRef Name) { return Name; });
    return S;
  };
  EXPECT_TRUE(StringRef(Pipeline(A)).contains("ModuleInlinerPass"));
  EXPECT_FALSE(StringRef(Pipeline(B)).contains("ModuleInlinerPass"));
}
