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
unsigned AOptions::Slot = OptionsRegistry::allocateSlot(&AOptions::Global);

struct BOptions {
  int B = 2;
  static BOptions Global;
  static unsigned Slot;
};
BOptions BOptions::Global;
unsigned BOptions::Slot;
} // namespace

TEST(OptionsRegistryTest, SetAndGet) {
  OptionsRegistry Old;
  EXPECT_EQ(&Old.get<AOptions>(), &AOptions::Global);

  // A registry created before a struct's registration reads Global, and
  // gains the slot on set.
  BOptions::Slot = OptionsRegistry::allocateSlot(&BOptions::Global);
  EXPECT_EQ(&Old.get<BOptions>(), &BOptions::Global);
  AOptions &A = Old.set(AOptions{3});
  EXPECT_EQ(&Old.get<AOptions>(), &A);
  EXPECT_EQ(Old.get<AOptions>().A, 3);
  BOptions &B = Old.set(BOptions{4});
  EXPECT_EQ(&Old.get<BOptions>(), &B);
  EXPECT_EQ(Old.get<AOptions>().A, 3);

  OptionsRegistry New;
  EXPECT_EQ(&New.get<AOptions>(), &AOptions::Global);
  EXPECT_EQ(&New.get<BOptions>(), &BOptions::Global);
  New.set(BOptions{5});
  EXPECT_EQ(New.get<BOptions>().B, 5);
  EXPECT_EQ(&New.get<AOptions>(), &AOptions::Global);
}
