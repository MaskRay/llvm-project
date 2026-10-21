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

using namespace llvm;


FLIndex<uint16_t> DIFunctionLocalMetadata::getFLScopeIdx(DILocalScope *Scope) {
  for (uint16_t Idx = 0; Idx < Scopes.size(); ++Idx)
    if (Scopes[Idx].get() == Scope)
    return Idx;
  Scopes.push_back(FLScope(Scope));
  return Scopes.size() - 1;
}

// SmallVector has no shrink_to_fit; recreate the vector to free its buffer.
template <typename T> static void freeVector(T &V) {
  V.~T();
  new (&V) T();
}

void DIFunctionLocalMetadata::releaseStorage() {
  freeVector(Scopes);
  freeVector(SrcLocs);
  freeVector(InlinedCalls);
  freeVector(Loops);
  InstrLoops.shrink_and_clear();
}

FLMDBuilder::FLMDBuilder(DIFunctionLocalMetadata *FLContext, const DISubprogram *SP) : FLContext(FLContext) {
  /// If FLContext is already populated, load its contents into the builder map.
  if (!FLContext->Scopes.empty() || !FLContext->SrcLocs.empty()) {
    assert(FLContext->Scopes[0].Scope == SP &&
      "FLContext already contains non-matching subprogram?");
    assert(FLContext->SrcLocs[0] == FLSrcLoc(0, 0, 0) &&
      "FLContext already contains non-matching srcLocs?");
    assert(FLContext->SrcLocs[1] == FLSrcLoc(SP->getLine(), 0, 0) &&
      "FLContext already contains non-matching srcLocs?");
    assert(FLContext->SrcLocs[2] == FLSrcLoc(SP->getScopeLine(), 0, 0) &&
      "FLContext already contains non-matching srcLocs?");
    for (auto [Idx, SrcLoc] : enumerate(FLContext->SrcLocs))
      SrcLocMap.insert({SrcLoc, Idx});
    for (auto [Idx, Scope] : enumerate(FLContext->Scopes))
      ScopeMap.insert({Scope, Idx});
    for (auto [Idx, InlinedCall] : enumerate(FLContext->InlinedCalls))
      if (InlinedCall.Uniquable)
        UniqueInlinedCalls.insert({InlinedCall, Idx});
    return;
  }
  assert(FLContext->Scopes.empty() && FLContext->SrcLocs.empty()
    && "Creating a builder for an already-exinst FLContext currently unsupported.");
  FLContext->Scopes.push_back(FLScope{ const_cast<DISubprogram*>(SP) });
  ScopeMap.insert({FLScope{ const_cast<DISubprogram*>(SP) }, 0});
  FLContext->SrcLocs.push_back(FLSrcLoc(0, 0, 0));
  FLContext->SrcLocs.push_back(FLSrcLoc(SP->getLine(), 0, 0));
  FLContext->SrcLocs.push_back(FLSrcLoc(SP->getScopeLine(), 0, 0));
  SrcLocMap.insert({FLSrcLoc(0, 0, 0), 0});
  SrcLocMap.insert({FLSrcLoc(SP->getLine(), 0, 0), 1});
  SrcLocMap.insert({FLSrcLoc(SP->getScopeLine(), 0, 0), 2});
}
