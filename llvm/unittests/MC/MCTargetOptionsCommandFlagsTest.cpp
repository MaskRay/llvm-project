//===- MCTargetOptionsCommandFlagsTest.cpp --------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/MC/MCTargetOptionsCommandFlags.h"
#include "llvm/MC/MCTargetOptions.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"
#include "gtest/gtest.h"

using namespace llvm;

static mc::RegisterMCTargetOptionsFlags Register;

TEST(MCTargetOptionsCommandFlagsTest, Parse) {
  cl::ResetAllOptionOccurrences();
  const char *Args[] = {"llvm-mc",
                        "-mc-relax-all",
                        "--dwarf-version=5",
                        "-emit-dwarf-unwind",
                        "no-compact-unwind",
                        "-W",
                        "--x86-relax-relocations=false",
                        "-target-abi=lp64d",
                        "-reloc-section-sym=none",
                        "-asm-macro-max-nesting-depth=7"};
  std::string Err;
  raw_string_ostream ErrOS(Err);
  ASSERT_TRUE(cl::ParseCommandLineOptions(std::size(Args), Args, "", &ErrOS));
  MCTargetOptions O = mc::InitMCTargetOptionsFromFlags();
  EXPECT_TRUE(O.MCRelaxAll);
  EXPECT_EQ(mc::getExplicitRelaxAll(), true);
  EXPECT_EQ(O.DwarfVersion, 5);
  EXPECT_EQ(O.EmitDwarfUnwind, EmitDwarfUnwindType::NoCompactUnwind);
  EXPECT_TRUE(O.MCNoWarn);
  EXPECT_FALSE(O.X86RelaxRelocations);
  EXPECT_EQ(O.ABIName, "lp64d");
  EXPECT_EQ(mc::getABIName(), "lp64d");
  EXPECT_EQ(O.RelocSectionSym, RelocSectionSymType::None);
  EXPECT_EQ(O.AsmMacroMaxNestingDepth, 7u);

  cl::ResetAllOptionOccurrences();
  O = mc::InitMCTargetOptionsFromFlags();
  EXPECT_FALSE(O.MCRelaxAll);
  EXPECT_EQ(mc::getExplicitRelaxAll(), std::nullopt);
  EXPECT_TRUE(O.X86RelaxRelocations);
  EXPECT_EQ(O.AsmMacroMaxNestingDepth, 100u);
}

TEST(MCTargetOptionsCommandFlagsTest, Errors) {
  cl::ResetAllOptionOccurrences();
  std::string Err;
  raw_string_ostream ErrOS(Err);
  const char *BadValue[] = {"llvm-mc", "-emit-dwarf-unwind=sometimes"};
  EXPECT_FALSE(
      cl::ParseCommandLineOptions(std::size(BadValue), BadValue, "", &ErrOS));
  EXPECT_EQ(Err, "error: invalid value 'sometimes' in "
                 "'-emit-dwarf-unwind=sometimes'\n");

  Err.clear();
  const char *Missing[] = {"llvm-mc", "-dwarf-version"};
  EXPECT_FALSE(
      cl::ParseCommandLineOptions(std::size(Missing), Missing, "", &ErrOS));
  EXPECT_EQ(Err, "error: option '-dwarf-version' requires an argument\n");
}
