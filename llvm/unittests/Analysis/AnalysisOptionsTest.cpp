//===- AnalysisOptionsTest.cpp --------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Analysis/AnalysisOptions.h"
#include "llvm/IR/LLVMContext.h"
#include "gtest/gtest.h"

using namespace llvm;

TEST(AnalysisOptionsTest, Parse) {
  AnalysisOptions O;
  SmallVector<const char *> Rest;
  const char *Args[] = {"opt",
                        "-inline-threshold=100",
                        "-o",
                        "out.bc",
                        "--inline-instr-cost",
                        "7",
                        "-inline-cost-full",
                        "-inline-caller-superset-nobuiltin=false",
                        "-inline-all-viable-calls=0",
                        "in.ll"};
  EXPECT_TRUE(O.parse(Args, Rest));
  EXPECT_EQ(O.InlineThreshold, 100);
  EXPECT_EQ(O.InstrCost, 7);
  EXPECT_TRUE(O.ComputeFullInlineCost);
  EXPECT_FALSE(O.InlineCallerSupersetNoBuiltin);
  EXPECT_FALSE(O.InlineAllViableCalls);
  EXPECT_EQ(O.CallPenalty, 25);
  EXPECT_EQ(Rest, SmallVector<const char *>({"opt", "-o", "out.bc", "in.ll"}));

  AnalysisOptions Bad;
  Rest.clear();
  const char *BadArgs[] = {"opt", "-inline-threshold=x"};
  EXPECT_FALSE(Bad.parse(BadArgs, Rest));
}

TEST(AnalysisOptionsTest, PerContext) {
  AnalysisOptions A, B;
  SmallVector<const char *> Rest;
  const char *ArgsA[] = {"-inline-instr-cost=1"};
  const char *ArgsB[] = {"-inline-instr-cost=2"};
  ASSERT_TRUE(A.parse(ArgsA, Rest));
  ASSERT_TRUE(B.parse(ArgsB, Rest));

  LLVMContext CtxA, CtxB, CtxNone;
  CtxA.setOptions(&A);
  CtxB.setOptions(&B);
  EXPECT_EQ(AnalysisOptions::get(CtxA).InstrCost, 1);
  EXPECT_EQ(AnalysisOptions::get(CtxB).InstrCost, 2);
  EXPECT_EQ(&AnalysisOptions::get(CtxNone), &AnalysisOptions::current());
}
