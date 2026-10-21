//===- Function.cpp - Implement the Global object classes -----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements function-local metadata types.
//
//===----------------------------------------------------------------------===//

#include "llvm/IR/FunctionLocalMetadata.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/LLVMContext.h"

using namespace llvm;

FLIndex<uint32_t> DIFunctionLocalMetadata::getFLScopeIdx(const MDNode *Scope) {
  auto [It, Inserted] = ScopeIndex.try_emplace(Scope, Scopes.size());
  if (Inserted)
    Scopes.push_back(FLScope(const_cast<MDNode *>(Scope)));
  return It->second;
}

FLIndex<uint32_t>
DIFunctionLocalMetadata::getFLSrcLocIdx(const FLSrcLoc &SrcLoc) {
  unsigned Hash = SrcLoc.getHash();
  uint32_t NewIdx = SrcLocs.size();
  auto [It, Inserted] = SrcLocIndex.insert_as(
      (uint64_t)Hash << 32 | NewIdx,
      FLSrcLocIndexInfo::Key{SrcLoc, SrcLocs, Hash});
  if (!Inserted)
    return uint32_t(*It);
  SrcLocs.push_back(SrcLoc);
  if (SrcLoc.AtomGroup)
    getContext().updateDILocationAtomGroupWaterline(SrcLoc.AtomGroup + 1);
  return NewIdx;
}

FLIndex<uint32_t>
DIFunctionLocalMetadata::addInlinedCall(FLInlinedCall InlinedCall,
                                        bool Uniquable) {
  uint32_t NewIdx = InlinedCalls.size();
  if (Uniquable) {
    auto [It, Inserted] =
        UniquedInlinedCalls.try_emplace(InlinedCall.asRawInt(), NewIdx);
    if (!Inserted)
      return It->second;
  }
  InlinedCalls.push_back(InlinedCall);
  return NewIdx;
}

bool DIFunctionLocalMetadata::isUniquedInlinedCall(
    FLIndex<uint32_t> Idx) const {
  auto It = UniquedInlinedCalls.find(getInlinedCall(Idx).asRawInt());
  return It != UniquedInlinedCalls.end() && It->second == Idx.get();
}

void DIFunctionLocalMetadata::indexEntries(
    uint32_t FirstScope, uint32_t FirstSrcLoc, uint32_t FirstInlinedCall,
    ArrayRef<bool> UniquableInlinedCalls) {
  assert(UniquableInlinedCalls.size() == InlinedCalls.size() - FirstInlinedCall);
  for (uint32_t Idx = FirstScope; Idx != Scopes.size(); ++Idx)
    ScopeIndex.try_emplace(Scopes[Idx].Scope, Idx);
  for (uint32_t Idx = FirstSrcLoc; Idx != SrcLocs.size(); ++Idx) {
    unsigned Hash = SrcLocs[Idx].getHash();
    SrcLocIndex.insert_as((uint64_t)Hash << 32 | Idx,
                          FLSrcLocIndexInfo::Key{SrcLocs[Idx], SrcLocs, Hash});
  }
  for (uint32_t Idx = FirstInlinedCall; Idx != InlinedCalls.size(); ++Idx)
    if (UniquableInlinedCalls[Idx - FirstInlinedCall])
      UniquedInlinedCalls.try_emplace(InlinedCalls[Idx].asRawInt(), Idx);
}
