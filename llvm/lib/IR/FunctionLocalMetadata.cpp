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

// Returns the index of the entry equal to \p Entry, or Table.size() after
// adding the new entry to \p Index; the caller appends it.
template <typename EntryT>
static std::pair<uint32_t, bool> internEntry(FLEntryIndex<EntryT> &Index,
                                             ArrayRef<EntryT> Table,
                                             const EntryT &Entry) {
  unsigned Hash = Entry.getHash();
  uint32_t NewIdx = Table.size();
  auto [It, Inserted] =
      Index.insert_as((uint64_t)Hash << 32 | NewIdx,
                      typename FLEntryIndexInfo<EntryT>::Key{Entry, Table, Hash});
  return {Inserted ? NewIdx : uint32_t(*It), Inserted};
}

FLIndex<uint32_t>
DIFunctionLocalMetadata::getFLSrcLocIdx(const FLSrcLoc &SrcLoc) {
  auto [Idx, Inserted] = internEntry<FLSrcLoc>(SrcLocIndex, SrcLocs, SrcLoc);
  if (!Inserted)
    return Idx;
  SrcLocs.push_back(SrcLoc);
  if (SrcLoc.AtomGroup)
    getContext().updateDILocationAtomGroupWaterline(SrcLoc.AtomGroup + 1);
  return Idx;
}

FLIndex<uint32_t>
DIFunctionLocalMetadata::addInlinedCall(FLInlinedCall InlinedCall,
                                        bool Uniquable) {
  if (Uniquable) {
    auto [Idx, Inserted] = internEntry<FLInlinedCall>(
        UniquedInlinedCallIndex, InlinedCalls, InlinedCall);
    if (!Inserted)
      return Idx;
  }
  InlinedCalls.push_back(InlinedCall);
  UniquedInlinedCalls.push_back(Uniquable);
  return InlinedCalls.size() - 1;
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

void FLTableSubset::add(FLDebugLoc DL) {
  assert(!Finalized && "Subset is already numbered");
  addSrcLoc(DL.SrcLocIdx);
  for (FLIndex<uint32_t> IA = DL.InlinedAtIdx; IA && !InlinedCallMap[IA.get()];
       IA = Table.getInlinedCall(IA).InlinedAtIdx) {
    InlinedCallMap[IA.get()] = 1;
    addSrcLoc(Table.getInlinedCall(IA).SrcLocIdx);
  }
}

void FLTableSubset::add(const DILocation &Loc) {
#if LLVM_USE_FLMD_SOURCE_LOCS
  add(Loc.getAsDebugLoc().getUnderlyingStorage());
#endif
}

void FLTableSubset::addInstruction(const Instruction &I) {
#if LLVM_USE_FLMD_SOURCE_LOCS
  add(I.getDebugLocStorage().get());
  for (const DbgRecord &DR : I.getDbgRecordRange())
    add(DR.getDebugLocStorage().get());
#endif
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
  SmallVector<std::pair<unsigned, MDNode *>, 4> MDs;
  for (const Function &F : M)
    for (const Instruction &I : instructions(F)) {
      Subset.addInstruction(I);
      MDs.clear();
      I.getAllMetadataOtherThanDebugLoc(MDs);
      for (auto [Kind, N] : MDs) {
        if (auto *Loc = dyn_cast<DILocation>(N))
          Subset.add(*Loc);
        for (const MDOperand &Op : N->operands())
          if (auto *Loc = dyn_cast_or_null<DILocation>(Op.get()))
            Subset.add(*Loc);
      }
    }
  Subset.finalize();
  return Subset;
}

FLTableSubset FLTableSubset::all(const DIFunctionLocalMetadata &Table) {
  FLTableSubset Subset(Table);
  for (auto *Map :
       {&Subset.ScopeMap, &Subset.SrcLocMap, &Subset.InlinedCallMap})
    std::fill(Map->begin(), Map->end(), 1);
  Subset.finalize();
  return Subset;
}

FLDebugLoc FLTableSubset::map(FLDebugLoc DL) const {
  return FLDebugLoc(mapSrcLoc(DL.SrcLocIdx), mapInlinedCall(DL.InlinedAtIdx));
}
