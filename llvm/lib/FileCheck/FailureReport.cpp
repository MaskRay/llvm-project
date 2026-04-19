//===- FailureReport.cpp - Per-directive FileCheck status report ----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/FileCheck/FileCheck.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/WithColor.h"
#include "llvm/Support/raw_ostream.h"
#include <numeric>

using namespace llvm;

namespace {

constexpr size_t kMaxSnippetWidth = 80;

StringRef getLineFrom(const SourceMgr &SM, SMLoc Loc) {
  if (!Loc.isValid())
    return "";
  unsigned BufID = SM.FindBufferContainingLoc(Loc);
  if (!BufID)
    return "";
  const MemoryBuffer *MB = SM.getMemoryBuffer(BufID);
  StringRef Rest(Loc.getPointer(), MB->getBufferEnd() - Loc.getPointer());
  StringRef Line = Rest.take_until([](char C) { return C == '\n'; });
  Line.consume_back("\r");
  return Line;
}

StringRef getLineByNumber(SourceMgr &SM, unsigned BufID, unsigned LineNo) {
  if (BufID == 0 || LineNo == 0)
    return "";
  SMLoc Start = SM.FindLocForLineAndColumn(BufID, LineNo, 1);
  return getLineFrom(SM, Start);
}

unsigned findInputBuffer(const SourceMgr &SM, unsigned CheckBufID) {
  for (unsigned B = 1, E = SM.getNumBuffers(); B <= E; ++B)
    if (B != CheckBufID)
      return B;
  return 0;
}

std::string truncateSnippet(StringRef S, size_t Max = kMaxSnippetWidth) {
  if (S.size() <= Max)
    return S.str();
  return (S.take_front(Max - 1).str() + "\xe2\x80\xa6"); // U+2026
}

bool isFailureDiag(FileCheckDiag::MatchType T) {
  switch (T) {
  case FileCheckDiag::MatchNoneButExpected:
  case FileCheckDiag::MatchFoundButExcluded:
  case FileCheckDiag::MatchFoundButWrongLine:
  case FileCheckDiag::MatchNoneForInvalidPattern:
    return true;
  default:
    return false;
  }
}

bool isMatchedDiag(FileCheckDiag::MatchType T) {
  return T == FileCheckDiag::MatchFoundAndExpected ||
         T == FileCheckDiag::MatchNoneAndExcluded;
}

} // namespace

namespace llvm {

void renderFailureReport(ArrayRef<FileCheckDiag> Diags, SourceMgr &SM,
                         StringRef InputFile, raw_ostream &OS) {
  // Pre-pass: map fuzzy candidates by CheckLoc and resolve each diag's
  // buffer + line number once so the sort comparator and the render loop
  // don't re-walk SourceMgr's buffer list per access.
  DenseMap<const char *, unsigned> FuzzyByCheckLoc;
  SmallVector<std::pair<unsigned, unsigned>, 16> Keys(Diags.size());
  unsigned MainCheckBufID = 0;
  for (unsigned I = 0, E = Diags.size(); I != E; ++I) {
    const FileCheckDiag &D = Diags[I];
    if (D.MatchTy == FileCheckDiag::MatchFuzzy)
      FuzzyByCheckLoc.try_emplace(D.CheckLoc.getPointer(), D.InputStartLine);
    if (!D.CheckLoc.isValid()) {
      Keys[I] = {0u, 0u};
      continue;
    }
    unsigned B = SM.FindBufferContainingLoc(D.CheckLoc);
    unsigned L = B ? SM.getLineAndColumn(D.CheckLoc, B).first : 0u;
    Keys[I] = {B, L};
    if (!MainCheckBufID && B)
      MainCheckBufID = B;
  }
  unsigned InputBufID = findInputBuffer(SM, MainCheckBufID);

  // Engine emits CHECK-NOT-fired diags after the parent CHECK that caught
  // them, which produces non-monotonic line order.  Stable-sort by the
  // precomputed (buffer, line) keys so the report reads in source order.
  SmallVector<unsigned, 16> Order(Diags.size());
  std::iota(Order.begin(), Order.end(), 0u);
  std::stable_sort(Order.begin(), Order.end(),
                   [&](unsigned A, unsigned B) { return Keys[A] < Keys[B]; });

  unsigned Failed = 0, Matched = 0, Excluded = 0;
  bool AnyOutput = false;
  bool LastWasFailure = false;

  for (unsigned Idx : Order) {
    const FileCheckDiag &D = Diags[Idx];
    if (D.MatchTy == FileCheckDiag::MatchFuzzy ||
        D.MatchTy == FileCheckDiag::MatchFoundButDiscarded ||
        D.MatchTy == FileCheckDiag::MatchFoundErrorNote)
      continue;

    std::string Directive = D.CheckTy.getDescription("CHECK");
    unsigned CheckLine = Keys[Idx].second;
    StringRef Pattern = getLineFrom(SM, D.CheckLoc);

    if (isMatchedDiag(D.MatchTy)) {
      ++Matched;
      if (D.MatchTy == FileCheckDiag::MatchNoneAndExcluded)
        ++Excluded;
      if (LastWasFailure)
        OS << "\n";
      OS << "  " << CheckLine << "  " << Directive << "  "
         << truncateSnippet(Pattern);
      if (D.MatchTy == FileCheckDiag::MatchFoundAndExpected) {
        OS << "    matched @ " << D.InputStartLine;
        // Sentinel set by replayLenient when an adjacency-claiming
        // directive (CHECK-NEXT/SAME/EMPTY) matched somewhere other than
        // strict semantics would have required.
        if (D.Note == kFileCheckNonAdjacentNote)
          OS << " (non-adjacent)";
      } else {
        OS << "    not present (as expected)";
      }
      OS << "\n";
      AnyOutput = true;
      LastWasFailure = false;
      continue;
    }

    if (!isFailureDiag(D.MatchTy))
      continue;

    ++Failed;
    if (AnyOutput)
      OS << "\n";
    AnyOutput = true;
    LastWasFailure = true;

    WithColor(OS, raw_ostream::RED, /*Bold=*/true)
        << CheckLine << ": error: " << Directive << ": ";

    switch (D.MatchTy) {
    case FileCheckDiag::MatchNoneButExpected:
      WithColor(OS, raw_ostream::RED, /*Bold=*/true)
          << "pattern not found in input\n";
      break;
    case FileCheckDiag::MatchFoundButExcluded:
      WithColor(OS, raw_ostream::RED, /*Bold=*/true) << "unexpected match\n";
      break;
    case FileCheckDiag::MatchFoundButWrongLine:
      WithColor(OS, raw_ostream::RED, /*Bold=*/true)
          << "matched on wrong line\n";
      break;
    case FileCheckDiag::MatchNoneForInvalidPattern:
      WithColor(OS, raw_ostream::RED, /*Bold=*/true) << "pattern is invalid\n";
      break;
    default:
      OS << "\n";
      break;
    }

    OS << "  pattern:  " << Pattern << "\n";

    if (D.MatchTy == FileCheckDiag::MatchFoundButExcluded ||
        D.MatchTy == FileCheckDiag::MatchFoundButWrongLine) {
      StringRef Snippet = getLineByNumber(SM, InputBufID, D.InputStartLine);
      OS << "  found at: " << InputFile << ":" << D.InputStartLine << " \""
         << truncateSnippet(Snippet) << "\"\n";
    } else if (D.MatchTy == FileCheckDiag::MatchNoneButExpected) {
      auto It = FuzzyByCheckLoc.find(D.CheckLoc.getPointer());
      if (It != FuzzyByCheckLoc.end()) {
        unsigned NearLine = It->second;
        StringRef Cand = getLineByNumber(SM, InputBufID, NearLine);
        OS << "  nearest:  " << InputFile << ":" << NearLine << " \""
           << truncateSnippet(Cand) << "\"\n";
      }
    }
  }

  if (!AnyOutput)
    return;

  if (Failed > 0) {
    OS << "\n";
    WithColor(OS, raw_ostream::YELLOW, /*Bold=*/true)
        << "FileCheck: " << Failed << " failed, " << Matched << " matched";
    if (Excluded > 0)
      OS << " (" << Excluded << " correctly excluded)";
    OS << ".\n";
  }
}

} // namespace llvm
