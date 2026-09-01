//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/CodeGen/AccelTable.h"
#include "TestAsmPrinter.h"
#include "llvm/CodeGen/DwarfStringPoolEntry.h"
#include "llvm/Testing/Support/Error.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

using namespace llvm;

namespace {

class AccelTableTest : public testing::Test {
protected:
  /// Split out because ASSERT_* can only be used in a function returning void.
  void setupTestPrinter() {
    auto ExpectedTestPrinter = TestAsmPrinter::create(
        "x86_64-pc-linux", /*DwarfVersion=*/5, dwarf::DWARF32);
    ASSERT_THAT_EXPECTED(ExpectedTestPrinter, Succeeded());
    TestPrinter = std::move(*ExpectedTestPrinter);
  }

  /// Returns false if the target is not available, in which case the caller
  /// should skip the test.
  bool init() {
    setupTestPrinter();
    return TestPrinter != nullptr;
  }

  /// Builds a table holding \p Names and returns the names in the order they
  /// would be emitted in.
  std::vector<StringRef>
  bucketOrder(ArrayRef<const DwarfStringPoolEntryWithExtString *> Names) {
    DWARF5AccelTable Table;
    for (const DwarfStringPoolEntryWithExtString *Name : Names)
      Table.addName(DwarfStringPoolEntryRef(*Name), /*DieOffset=*/0x11,
                    /*DefiningParentOffset=*/std::nullopt,
                    /*DieTag=*/dwarf::DW_TAG_variable, /*UnitID=*/0,
                    /*IsTU=*/false);
    Table.finalize(TestPrinter->getAP(), "names");

    std::vector<StringRef> Order;
    for (const AccelTableBase::HashList &Bucket : Table.getBuckets())
      for (const AccelTableBase::HashData *Hash : Bucket)
        Order.push_back(Hash->Name.getString());
    return Order;
  }

  std::unique_ptr<TestAsmPrinter> TestPrinter;
};

TEST_F(AccelTableTest, CollidingNamesOrderedIndependentlyOfInsertion) {
  if (!init())
    GTEST_SKIP();

  // The DWARF v5 hash folds case, so these three names always collide.
  DwarfStringPoolEntryWithExtString Lower = {{}, "fixups"};
  DwarfStringPoolEntryWithExtString Mixed = {{}, "Fixups"};
  DwarfStringPoolEntryWithExtString Upper = {{}, "FIXUPS"};
  // Lands in the same bucket as the three above, with a lower hash value.
  DwarfStringPoolEntryWithExtString Other = {{}, "gamma"};

  const uint32_t Hash = DWARF5AccelTableData::hash(Lower.String);
  ASSERT_EQ(Hash, DWARF5AccelTableData::hash(Mixed.String));
  ASSERT_EQ(Hash, DWARF5AccelTableData::hash(Upper.String));
  const uint32_t OtherHash = DWARF5AccelTableData::hash(Other.String);
  ASSERT_LT(OtherHash, Hash);
  // Four names with two distinct hashes give two buckets, and both hashes are
  // even, so all four names share bucket 0.
  ASSERT_EQ(Hash % 2, OtherHash % 2);

  // The hash value orders before the name, so "gamma" comes first even though
  // it sorts after the names it collides with.
  const auto Expected =
      testing::ElementsAre("gamma", "FIXUPS", "Fixups", "fixups");
  EXPECT_THAT(bucketOrder({&Lower, &Mixed, &Upper, &Other}), Expected);
  EXPECT_THAT(bucketOrder({&Other, &Upper, &Mixed, &Lower}), Expected);
  EXPECT_THAT(bucketOrder({&Mixed, &Other, &Lower, &Upper}), Expected);
}

} // end namespace
