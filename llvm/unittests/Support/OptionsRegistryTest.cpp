//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Support/OptionsRegistry.h"
#include "gtest/gtest.h"

using namespace llvm;

namespace {
struct AOptions {
  int A = 1;
  static AOptions Global;
  static unsigned Slot;
};
AOptions AOptions::Global;
unsigned AOptions::Slot = OptionsRegistry::allocateSlot();

struct BOptions {
  int B = 2;
  static BOptions Global;
  static unsigned Slot;
};
BOptions BOptions::Global;
unsigned BOptions::Slot = -1;
} // namespace

TEST(OptionsRegistryTest, SetAndGet) {
  OptionsRegistry R;
  EXPECT_EQ(&R.get<AOptions>(), &AOptions::Global);
  // An unregistered struct reads Global.
  EXPECT_EQ(&R.get<BOptions>(), &BOptions::Global);

  BOptions::Slot = OptionsRegistry::allocateSlot();
  BOptions &B = R.set(BOptions{4});
  EXPECT_EQ(&R.get<BOptions>(), &B);
  EXPECT_EQ(&R.get<AOptions>(), &AOptions::Global);
  AOptions &A = R.set(AOptions{3});
  EXPECT_EQ(R.get<AOptions>().A, 3);
  EXPECT_EQ(&R.get<AOptions>(), &A);
  EXPECT_EQ(R.get<BOptions>().B, 4);

  OptionsRegistry Other;
  EXPECT_EQ(&Other.get<AOptions>(), &AOptions::Global);
  EXPECT_EQ(&Other.get<BOptions>(), &BOptions::Global);
}
