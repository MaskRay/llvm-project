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
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

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

FLTableSubset::FLTableSubset(const DIFunctionLocalMetadata &Table)
    : Table(Table), ScopeMap(Table.Scopes.size()),
      SrcLocMap(Table.SrcLocs.size()),
      InlinedCallMap(Table.InlinedCalls.size()) {}

void FLTableSubset::addSrcLoc(FLIndex<uint32_t> Idx) {
  if (!Idx || SrcLocMap[Idx.get()])
    return;
  SrcLocMap[Idx.get()] = 1;
  ScopeMap[Table.getSrcLoc(Idx).ScopeIdx.get()] = 1;
}

void FLTableSubset::add(FLIndex<uint32_t> SrcLocIdx,
                        FLIndex<uint32_t> InlinedAtIdx) {
  assert(!Finalized && "Subset is already numbered");
  addSrcLoc(SrcLocIdx);
  for (FLIndex<uint32_t> IA = InlinedAtIdx; IA && !InlinedCallMap[IA.get()];
       IA = Table.getInlinedCall(IA).InlinedAtIdx) {
    InlinedCallMap[IA.get()] = 1;
    addSrcLoc(Table.getInlinedCall(IA).SrcLocIdx);
  }
}

void FLTableSubset::finalize() {
  auto Number = [](std::vector<uint32_t> &Map, SmallVectorImpl<uint32_t> &Old) {
    for (uint32_t Idx = 0, E = Map.size(); Idx != E; ++Idx)
      if (Map[Idx]) {
        Old.push_back(Idx);
        Map[Idx] = Old.size();
      }
  };
  Number(ScopeMap, Scopes);
  Number(SrcLocMap, SrcLocs);
  Number(InlinedCallMap, InlinedCalls);
  Finalized = true;
}

FLTableSubset FLTableSubset::collect(const DIFunctionLocalMetadata &Table,
                                     const Module &M) {
  FLTableSubset Subset(Table);
#if LLVM_USE_FLMD_SOURCE_LOCS
  auto AddLoc = [&](const DILocation *Loc) {
    FLDebugLoc DL = Loc->getAsDebugLoc().getUnderlyingStorage();
    Subset.add(DL.SrcLocIdx, DL.InlinedAtIdx);
  };
  SmallVector<std::pair<unsigned, MDNode *>, 4> MDs;
  for (const Function &F : M)
    for (const Instruction &I : instructions(F)) {
      FLDebugLoc DL = I.getDebugLocStorage().get();
      Subset.add(DL.SrcLocIdx, DL.InlinedAtIdx);
      for (const DbgRecord &DR : I.getDbgRecordRange()) {
        FLDebugLoc RDL = DR.getDebugLocStorage().get();
        Subset.add(RDL.SrcLocIdx, RDL.InlinedAtIdx);
      }
      MDs.clear();
      I.getAllMetadataOtherThanDebugLoc(MDs);
      for (auto [Kind, N] : MDs) {
        if (auto *Loc = dyn_cast<DILocation>(N))
          AddLoc(Loc);
        for (const MDOperand &Op : N->operands())
          if (auto *Loc = dyn_cast_or_null<DILocation>(Op.get()))
            AddLoc(Loc);
      }
    }
#endif
  Subset.finalize();
  return Subset;
}
