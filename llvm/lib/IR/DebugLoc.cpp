//===-- DebugLoc.cpp - Implement DebugLoc class ---------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/IR/DebugLoc.h"
#include "llvm/Analysis/MemorySSAUpdater.h"
#include "llvm/Config/llvm-config.h"
#include "llvm/IR/DebugInfo.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/FunctionLocalMetadata.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/Support/Discriminator.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Transforms/Utils/ValueMapper.h"
#include <cstdint>
#include <memory>
#include <optional>

using namespace llvm;

namespace llvm {
extern LLVM_ABI cl::opt<bool> PickMergedSourceLocations;

raw_ostream &operator<<(raw_ostream &OS, const FLDebugLoc &DL) {
  OS << "!!dbgLoc(" <<
    "srcLoc: " << DL.SrcLocIdx.get();
  if (DL.InlinedAtIdx)
    OS << ", inlinedAt: " << DL.InlinedAtIdx.get();
  if (DL.AtomGroup)
    OS << ", atom: [" << DL.AtomGroup << ", " << DL.AtomRank << "]";
  OS << ")";
}
} // namespace llvm

#if LLVM_ENABLE_DEBUGLOC_TRACKING_ORIGIN
#include "llvm/Support/Signals.h"
namespace llvm {
bool DebugLocOriginCollectionEnabled = false;
} // namespace llvm

DbgLocOrigin::DbgLocOrigin(bool ShouldCollectTrace) {
  if (!ShouldCollectTrace || !DebugLocOriginCollectionEnabled)
    return;
  auto &[Depth, StackTrace] = StackTraces.emplace_back();
  Depth = sys::getStackTrace(StackTrace);
}
void DbgLocOrigin::addTrace() {
  // We only want to add new stacktraces if we already have one: addTrace exists
  // to provide more context to how missing DebugLocs have propagated through
  // the program, but by design if there is no existing stacktrace then we have
  // decided not to track this DebugLoc as being "missing".
  if (StackTraces.empty())
    return;
  auto &[Depth, StackTrace] = StackTraces.emplace_back();
  Depth = sys::getStackTrace(StackTrace);
}
#endif // LLVM_ENABLE_DEBUGLOC_TRACKING_ORIGIN

#if LLVM_USE_FLMD_SOURCE_LOCS
/// Stores the context needed to convert between MD and FLMD source locations.
namespace {
struct FLMDSourceLocConversionContext {
  DenseMap<DILocation *, FLIndex<uint16_t>> InlinedCallLocMap;
  DenseMap<std::pair<FLIndex<uint16_t>, DIFunctionLocalMetadata*>, DILocation *> InlinedCallIdxToDILocMap;
  DenseMap<FLIndex<uint16_t>, uint16_t> MaxAtomMap;
  // Whenever we convert DILocation->DebugLoc, we use the
  // DIFunctionLocalMetadata associated with the final inlinedAt DISubprogram.
  // If this does not already exist, we create one - this should stop being
  // necessary at some point, but for now this is how we create new FLMD.
  DenseMap<DISubprogram *, DIFunctionLocalMetadata *> SPToFLMDMap;
  DIFunctionLocalMetadata *getFLMDForSP(DISubprogram *SP) {
    if (auto Existing = SPToFLMDMap.find(SP); Existing != SPToFLMDMap.end())
      return Existing->second;
    llvm_unreachable("no");
  }
  DIFunctionLocalMetadata *makeClonedFLMDForSP(DISubprogram *SP, DIFunctionLocalMetadata *OldFLMD) {
    auto Existing = SPToFLMDMap.find(SP);
    assert(Existing == SPToFLMDMap.end() && "Should not already exist in map");
    llvm_unreachable("no");
  }
};
} // namespace

static FLMDSourceLocConversionContext FLMDConversionContext;


DIFunctionLocalMetadata *llvm::getFLMDForInstruction(const Instruction *I) {
  assert(I->getFunction() && "Instruction must be inside of function");
  return getFLMDForFunction(I->getFunction());
}
DIFunctionLocalMetadata *llvm::getFLMDForFunction(const Function *F) {
  return cast_if_present<DIFunctionLocalMetadata>(F->getMetadata(LLVMContext::MD_flmd));
}

static FLIndex<uint16_t> getInlineCallDILocationToFLIndex(DILocation *DIL, DISubprogram *InlinedSP) {
  // No inlinedAt -> empty inlinedAt index.
  if (!DIL)
    return FLIndex<uint16_t>();
  if(DIL->isDistinct()) {
    if (auto ExistingIdxIt = FLMDConversionContext.InlinedCallLocMap.find(DIL);
        ExistingIdxIt != FLMDConversionContext.InlinedCallLocMap.end()) {
      return ExistingIdxIt->second;
    }
  }
  assert(InlinedSP != nullptr);
  DILocalScope *InlineeScope = DIL->getScope();
  DISubprogram *InlineeSP = InlineeScope->getSubprogram();
  // Reference into LastInlineeFLMD->InlinedCalls.
  FLIndex<uint16_t> InlinedAtIdx = getInlineCallDILocationToFLIndex(DIL->getInlinedAt(), InlineeSP);
  // All indices refer to the context of the outermost function of the chain,
  // LastInlineeFLMD.
  DIFunctionLocalMetadata *LastInlineeFLMD = FLMDConversionContext.getFLMDForSP(DIL->getInlinedAtScope()->getSubprogram());
  FLIndex<uint32_t> SrcLocIdx = LastInlineeFLMD->getFLSrcLocIdx(DIL->getLine(), DIL->getColumn(), InlineeScope);
  FLIndex<uint16_t> NewIdx = LastInlineeFLMD->addInlinedCall(FLInlinedCall(SrcLocIdx, InlinedAtIdx, !DIL->isDistinct()));
  if (DIL->isDistinct()) {
    FLMDConversionContext.InlinedCallLocMap.insert({DIL, NewIdx});
    FLMDConversionContext.InlinedCallIdxToDILocMap.insert({{NewIdx, LastInlineeFLMD}, DIL});
  }
  return NewIdx;
}

FLDebugLoc FLDebugLoc::getFromDILocation(const DILocation *DIL, DISubprogram *InlinedSP) {
  llvm_unreachable("No conversions!");
  if (!DIL)
    return FLDebugLoc();
  if(DIL->isDistinct())
    return FLDebugLoc::getInlinedCallLoc(getInlineCallDILocationToFLIndex(const_cast<DILocation*>(DIL), InlinedSP));
  DILocalScope *OrigScope = DIL->getScope();
  FLIndex<uint16_t> InlinedAtIdx = getInlineCallDILocationToFLIndex(DIL->getInlinedAt(), OrigScope->getSubprogram());
  DIFunctionLocalMetadata *OrigFLMD = FLMDConversionContext.getFLMDForSP(DIL->getInlinedAtScope()->getSubprogram());
  FLIndex<uint32_t> SrcLocIdx = OrigFLMD->getFLSrcLocIdx(DIL->getLine(), DIL->getColumn(), OrigScope);
  return FLDebugLoc(SrcLocIdx, InlinedAtIdx, DIL->getAtomGroup(), DIL->getAtomRank());
}

DebugLoc DebugLoc::getFromDILocation(const DILocation *DIL, DISubprogram *InlinedSP) {
  llvm_unreachable("No conversions!");
  if (!DIL)
    return DebugLoc();
  FLDebugLoc Storage = FLDebugLoc::getFromDILocation(DIL, InlinedSP);
  DIFunctionLocalMetadata *FLContext = FLMDConversionContext.getFLMDForSP(DIL->getInlinedAtScope()->getSubprogram());
  return DebugLoc(Storage, FLContext);
}

static DILocation *getInlinedAtDILocation(DIFunctionLocalMetadata *FLMD,
                                          FLIndex<uint16_t> InlinedAtIdx) {
  if (!InlinedAtIdx)
    return nullptr;
  // If we've already got a DILocation for this inlined call, reuse it.
  if (auto ExistingIt = FLMDConversionContext.InlinedCallIdxToDILocMap.find({InlinedAtIdx, FLMD}); ExistingIt != FLMDConversionContext.InlinedCallIdxToDILocMap.end())
    return ExistingIt->second;
  // Otherwise, make a new one.
  FLInlinedCall InlinedCall = FLMD->getInlinedCall(InlinedAtIdx);
  DILocation *InlinedAt = getInlinedAtDILocation(FLMD, InlinedCall.InlinedAtIdx);
  FLSrcLoc SrcLoc = FLMD->getSrcLoc(InlinedCall.SrcLocIdx);
  DILocalScope *Scope = FLMD->getScope(SrcLoc.ScopeIdx);
  DILocation *Result;
  // if (InlinedCall.Uniquable)
  //   Result = DILocation::get(FLMD->getContext(), SrcLoc.Line, SrcLoc.Column, Scope, InlinedAt);
  // else {
  //   Result = DILocation::getDistinct(FLMD->getContext(), SrcLoc.Line, SrcLoc.Column, Scope, InlinedAt);
  //   FLMDConversionContext.InlinedCallIdxToDILocMap.insert({{InlinedAtIdx, FLMD}, Result});
  // }
  return Result;
}

std::pair<FLDebugLoc, DIFunctionLocalMetadata *> DebugLoc::getAsFLDebugLoc() const {
  return {getStorage().get(), FLContext};
}
DebugLoc DebugLoc::getFromFLDebugLoc(FLDebugLoc FLDL, DIFunctionLocalMetadata *FLContext) {
  return DebugLoc(FLDL, FLContext);
}

// Should be called directly on a DebugLoc obtained from an instruction or loop
// metadata, not from the result of DebugLoc::getInlinedAt.
DILocation *DebugLoc::getAsDILocation() const {
  llvm_unreachable("No conversions!");
  if (!*this)
    return nullptr;
  if (isDistinct())
    return getInlinedAtDILocation(FLContext, Storage.get().InlinedAtIdx);
  DILocation *InlinedAt = getInlinedAt().getAsDILocation();
  FLSrcLoc SrcLoc = Storage.get().getSrcLoc(FLContext);
  DILocalScope *Scope = Storage.get().getScope(FLContext);
  // DILocation *Result = DILocation::get(FLContext->getContext(), SrcLoc.Line,
  //   SrcLoc.Column, Scope, InlinedAt, false, Storage.get().AtomGroup,
  //   Storage.get().AtomRank);
  // return Result;
  return nullptr;
}
DILocation *DebugLoc::get() const {
  return getAsDILocation();
}
DebugLoc::operator DILocation *() const {
  return getAsDILocation();
}
DILocation *DebugLoc::operator->() const {
  return getAsDILocation();
}
DILocation &DebugLoc::operator*() const {
  return *getAsDILocation();
}
#else
DebugLoc::DebugLocContext DebugLoc::getDLContext() const {
  assert((bool)*this && "Can only get DL Context from a valid DebugLoc.");
  return DebugLocContext(getUnderlyingStorage()->getContext());
}

// std::pair<FLDebugLoc, DIFunctionLocalMetadata *> DebugLoc::getAsFLDebugLoc() const {
//   return {getStorage().get(), FLContext};
// }
// DebugLoc DebugLoc::getFromFLDebugLoc(FLDebugLoc FLDL, DIFunctionLocalMetadata *FLContext) {
//   return DebugLoc(FLDL, FLContext);
// }

DebugLoc DebugLoc::getFromDILocation(const DILocation *DIL, DISubprogram *InlinedSP) {
  DebugLoc DL;
  DL.Storage = DbgLocStorage(const_cast<DILocation*>(DIL));
  return DL;
}
DILocation *DebugLoc::getAsDILocation() const {
  return Storage.get();
}
DILocation *DebugLoc::get() const {
  return Storage.get();
}
DebugLoc::operator DILocation *() const {
  return Storage.get();
}
DILocation *DebugLoc::operator->() const {
  return Storage.get();
}
DILocation &DebugLoc::operator*() const {
  return *Storage.get();
}
#endif

//===----------------------------------------------------------------------===//
// DebugLoc Implementation
//===----------------------------------------------------------------------===//

#if LLVM_USE_FLMD_SOURCE_LOCS
static FLIndex<uint16_t> getInlinedCallIdx(DIFunctionLocalMetadata *Context,
                                           DebugLoc InlinedAt) {
  if (!InlinedAt)
    return {};
  assert(InlinedAt.getStorage().get().isInlinedCall() && "InlinedAt was not an inlined call reference.");
  assert(InlinedAt.getFLContext() == Context && "InlinedAt is from another function.");
  return InlinedAt.getStorage().get().getIdxForInlinedCall();
}
DebugLoc DebugLoc::get(
    DIFunctionLocalMetadata *Context, unsigned Line, unsigned Column, Metadata *Scope,
    DebugLoc InlinedAt, bool ImplicitCode, uint64_t AtomGroup,
    uint8_t AtomRank) {
  FLIndex<uint16_t> InlinedAtIdx = getInlinedCallIdx(Context, InlinedAt);
  FLIndex<uint32_t> SrcLocIdx =
      Context->getFLSrcLocIdx(Line, Column, cast<DILocalScope>(Scope));
  Context->updateAtomGroupWaterline(InlinedAtIdx, AtomGroup);
  FLDebugLoc FLDbgLoc(SrcLocIdx, InlinedAtIdx, AtomGroup, AtomRank);
  return DebugLoc(FLDbgLoc, Context);
}
static DebugLoc createInlinedCall(DIFunctionLocalMetadata *Context,
                                  unsigned Line, unsigned Column,
                                  Metadata *Scope, DebugLoc InlinedAt,
                                  bool Uniquable, uint64_t MaxAtomGroup) {
  FLIndex<uint16_t> InlinedAtIdx = getInlinedCallIdx(Context, InlinedAt);
  FLIndex<uint32_t> SrcLocIdx =
      Context->getFLSrcLocIdx(Line, Column, cast<DILocalScope>(Scope));
  FLInlinedCall InlinedCall(SrcLocIdx, InlinedAtIdx, Uniquable, MaxAtomGroup);
  FLIndex<uint16_t> NewInlinedAtIdx = Context->addInlinedCall(InlinedCall);
  return DebugLoc(FLDebugLoc::getInlinedCallLoc(NewInlinedAtIdx), Context);
}
DebugLoc DebugLoc::getDistinctInlinedCall(
    DIFunctionLocalMetadata *Context, unsigned Line, unsigned Column, Metadata *Scope,
    DebugLoc InlinedAt, bool ImplicitCode, uint64_t MaxAtomGroup) {
  return createInlinedCall(Context, Line, Column, Scope, InlinedAt, false,
                        MaxAtomGroup);
}
DebugLoc DebugLoc::getUniquedInlinedCall(
    DIFunctionLocalMetadata *Context, unsigned Line, unsigned Column, Metadata *Scope,
    DebugLoc InlinedAt, bool ImplicitCode, uint64_t MaxAtomGroup) {
  return createInlinedCall(Context, Line, Column, Scope, InlinedAt, true,
                        MaxAtomGroup);
}

DebugLoc::DebugLocContext::DebugLocContext(const Instruction *I) {
  assert(I->getParent() && I->getFunction() &&
    "Instruction cannot be used to get function context if not inserted in a "
    "function.");
  if (I->getFullDebugLoc()) {
    Context = I->getFullDebugLoc().getFLContext();
    return;
  }
  const Function *F = I->getFunction();
  Context = cast_if_present<DIFunctionLocalMetadata>(
    F->getMetadata(LLVMContext::MD_flmd));
  // if (!Context)
  //   const_cast<Function*>(F)->setMetadata(
  //     LLVMContext::MD_flmd,
  //     DIFunctionLocalMetadata::getDistinct(F->getContext()));
  // assert(Context && "Attempted to create DebugLoc for function without a "
  //   "DIFunctionLocalMetadata attachment.");
}
DebugLoc::DebugLocContext::DebugLocContext(const Function *F) {
  Context = cast_if_present<DIFunctionLocalMetadata>(
    F->getMetadata(LLVMContext::MD_flmd));
  // if (!Context)
  //   const_cast<Function*>(F)->setMetadata(
  //     LLVMContext::MD_flmd,
  //     DIFunctionLocalMetadata::getDistinct(F->getContext()));
  // assert(Context && "Attempted to create DebugLoc for function without a "
  //   "DIFunctionLocalMetadata attachment.");
}
DILocation *DebugLoc::convertToDILocation() const {
  return DILocation::get(getContext(), *this);
}
#else
DebugLoc DebugLoc::get(
    LLVMContext &Context, unsigned Line, unsigned Column, Metadata *Scope,
    DebugLoc InlinedAt, bool ImplicitCode, uint64_t AtomGroup,
    uint8_t AtomRank) {
  return DebugLoc::getFromDILocation(DILocation::get(Context, Line, Column, Scope, InlinedAt.getAsDILocation(), ImplicitCode, AtomGroup, AtomRank));
}
DebugLoc DebugLoc::getUniquedInlinedCall(
    LLVMContext &Context, unsigned Line, unsigned Column, Metadata *Scope,
    DebugLoc InlinedAt, bool ImplicitCode, uint64_t AtomGroup,
    uint8_t AtomRank) {
  return DebugLoc::getFromDILocation(DILocation::getDistinct(Context, Line, Column, Scope, InlinedAt.getAsDILocation(), ImplicitCode, AtomGroup, AtomRank));
}
DebugLoc DebugLoc::getDistinctInlinedCall(
    LLVMContext &Context, unsigned Line, unsigned Column, Metadata *Scope,
    DebugLoc InlinedAt, bool ImplicitCode, uint64_t AtomGroup,
    uint8_t AtomRank) {
  return DebugLoc::getFromDILocation(DILocation::getDistinct(Context, Line, Column, Scope, InlinedAt.getAsDILocation(), ImplicitCode, AtomGroup, AtomRank));
}

DebugLoc::DebugLocContext::DebugLocContext(const Instruction *I) :
    Context(I->getContext()) {
  assert(I->getParent() && I->getFunction() &&
    "Instruction cannot be used to get function context if not inserted in a "
    "function.");
}
DebugLoc::DebugLocContext::DebugLocContext(const Function *F) :
    Context(F->getContext()) {}

DILocation *DebugLoc::convertToDILocation() const {
  return getAsDILocation();
}
#endif

DebugLoc DebugLoc::getFromMDNode(const MDNode *MD) {
  return DebugLoc::getFromDILocation(dyn_cast_or_null<DILocation>(MD));
}

DebugLoc DebugLoc::convertToInlinedCall() const {
#if LLVM_USE_FLMD_SOURCE_LOCS
  uint64_t MaxAtomGroup = FLContext->MaxAtomGroup;
#else
  uint64_t MaxAtomGroup = 0;
#endif
  return DebugLoc::getDistinctInlinedCall(getDLContext(), getLine(), getColumn(), getScope(), getInlinedAt(), isImplicitCode(), MaxAtomGroup);
}

#if LLVM_USE_FLMD_SOURCE_LOCS
unsigned DebugLoc::getLine() const {
  assert(Storage && FLContext && "Expected valid DebugLoc");
  return Storage.get().getSrcLoc(FLContext).Line;
}

unsigned DebugLoc::getCol() const {
  assert(Storage && FLContext && "Expected valid DebugLoc");
  return Storage.get().getSrcLoc(FLContext).Column;
}

DILocalScope *DebugLoc::getScope() const {
  assert(Storage && FLContext && "Expected valid DebugLoc");
  return Storage.get().getScope(FLContext);
}

DebugLoc DebugLoc::getInlinedAt() const {
  assert(Storage && FLContext && "Expected valid DebugLoc");
  return DebugLoc(FLDebugLoc::getInlinedCallLoc(Storage.get().getInlinedAtIdx(FLContext)), FLContext);
}
DILocalScope *DebugLoc::getInlinedAtScope() const {
  DebugLoc RootDL = *this;
  while (DebugLoc InlinedAt = RootDL.getInlinedAt())
    RootDL = InlinedAt;
  return RootDL.Storage.get().getScope(FLContext);
}

DebugLoc DebugLoc::getFnDebugLoc() const {
  constexpr uint16_t SubprogramScopeIndex = 0;
  DISubprogram *InlinedAtSP = cast<DISubprogram>(FLContext->Scopes[SubprogramScopeIndex].Scope);
  FLDebugLoc NewFLDebugLoc(
    FLContext->getFLSrcLocIdx(
      InlinedAtSP->getScopeLine(), 0, SubprogramScopeIndex),
    FLIndex<uint16_t>());
  return DebugLoc(NewFLDebugLoc, FLContext);
}

MDNode *DebugLoc::getAsMDNode() const {
  return getAsDILocation();
}

bool DebugLoc::isImplicitCode() const {
  /// FIXME: We could add implicitcode to the FLSrcLoc, but do we actually need to?
  /// It may be appropriate in the FLDebugLoc, though - existing behaviour is
  /// somewhat inconsistent.
  return false;
}

void DebugLoc::setImplicitCode(bool ImplicitCode) {
}


DebugLocMap::DebugLocMap(Function *SrcFn, Function *DestFn)
    : DebugLocMap(getFLMDForFunction(SrcFn), getFLMDForFunction(DestFn)) {}

DebugLocMap::DebugLocMap(DIFunctionLocalMetadata *SrcContext,
                         DIFunctionLocalMetadata *DestContext)
    : SrcContext(SrcContext), DestContext(DestContext) {
  SrcLocMap.resize(SrcContext->SrcLocs.size());
  ScopeMap.resize(SrcContext->Scopes.size());
  InlinedCallMap.resize(SrcContext->InlinedCalls.size());
}

FLIndex<uint16_t> DebugLocMap::mapScope(FLIndex<uint16_t> In) {
  FLIndex<uint16_t> &Out = ScopeMap[In.get()];
  if (!Out)
    Out = DestContext->getFLScopeIdx(SrcContext->getScope(In).get());
  return Out;
}

DebugLoc DebugLoc::replaceInlinedAtSubprogram(
    const DebugLoc &RootLoc, DISubprogram &NewSP, DebugLocContext NewFnContext,
    DenseMap<const MDNode *, MDNode *> &Cache, DebugLocMap &DLMap) {
  assert(RootLoc && "RootLoc must be a valid location.");
  assert(DLMap.getDestContext() == NewFnContext.Context);
  // InlinedAt chain from the source function.
  SmallVector<FLDebugLoc> LocChain;
  DIFunctionLocalMetadata *SrcContext = RootLoc.getFLContext();
  // First cached result found while traversing up the InlinedAt chain, if any.
  FLDebugLoc CachedResult;

  // Collect the inline chain, stopping if we find a location that has already
  // been processed.
  for (FLDebugLoc Storage = RootLoc.getStorage().get(); Storage;
       Storage = Storage.getInlinedAt(SrcContext)) {
    CachedResult = DLMap.getFLDebugLoc(Storage);
    if (CachedResult)
      break;
    LocChain.push_back(Storage);
  }

  FLDebugLoc UpdatedLoc = CachedResult;
  if (!UpdatedLoc) {
    // If no cache hits, then back() is the end of the inline chain, that is,
    // the DILocation whose scope ends in the Subprogram to be replaced.
    FLDebugLoc LocToUpdate = LocChain.pop_back_val();
    DILocalScope *NewScope = DILocalScope::cloneScopeForSubprogram(
      *LocToUpdate.getScope(SrcContext), NewSP, NewSP.getContext(), Cache);
    // Unlike the DILocation version, we have different paths for if the root of
    // the inline chain is an inlined call, or if RootLoc was not inlined.
    FLIndex<uint32_t> OldSrcLocIdx = LocToUpdate.isInlinedCall()
        ? LocToUpdate.getAsInlinedCall(SrcContext).SrcLocIdx
        : LocToUpdate.SrcLocIdx;
    // The old SrcLoc may have already been mapped; if so, then reuse it,
    // otherwise create the mapping now.
    FLIndex<uint32_t> NewSrcLocIdx = DLMap.getSrcLoc(OldSrcLocIdx);
    if (!NewSrcLocIdx) {
      FLSrcLoc SrcLoc = SrcContext->getSrcLoc(OldSrcLocIdx);
      NewSrcLocIdx = NewFnContext.Context->getFLSrcLocIdx(SrcLoc.Line, SrcLoc.Column, NewScope);
      DLMap.insertSrcLoc(OldSrcLocIdx, NewSrcLocIdx);
    }
    if (LocToUpdate.isInlinedCall()) {
      FLInlinedCall OldCall = LocToUpdate.getAsInlinedCall(SrcContext);
      assert(!OldCall.InlinedAtIdx && "LocToUpdate should be the back of the InlinedAt chain.");
      FLInlinedCall NewCall(NewSrcLocIdx, {}, OldCall.Uniquable, OldCall.MaxAtomGroup);
      FLIndex<uint16_t> NewCallIdx = NewFnContext.Context->addInlinedCall(NewCall);
      DLMap.insertInlinedCall(LocToUpdate.getIdxForInlinedCall(), NewCallIdx);
      UpdatedLoc = FLDebugLoc::getInlinedCallLoc(NewCallIdx);
    } else {
      assert(LocToUpdate.isInstrLoc() && "LocToUpdate should be a non-inlined InstrLoc.");
      UpdatedLoc = FLDebugLoc(NewSrcLocIdx, {}, LocToUpdate.AtomGroup, LocToUpdate.AtomRank);
    }
  }

  // Recreate the location chain, bottom-up, starting at the new scope (or a
  // cached result). SrcLocs inside the chain keep their scopes but must be
  // copied into the new context.
  for (FLDebugLoc LocToUpdate : reverse(LocChain)) {
    assert(UpdatedLoc.isInlinedCall());
    if (LocToUpdate.isInlinedCall()) {
      FLInlinedCall OldCall = LocToUpdate.getAsInlinedCall(SrcContext);
      FLIndex<uint16_t> NewInlinedCallIdx = NewFnContext.Context->addInlinedCall(
        FLInlinedCall(DLMap.mapSrcLoc(OldCall.SrcLocIdx), UpdatedLoc.getIdxForInlinedCall(), OldCall.Uniquable, OldCall.MaxAtomGroup)
      );
      UpdatedLoc = FLDebugLoc::getInlinedCallLoc(NewInlinedCallIdx);
      DLMap.insertInlinedCall(LocToUpdate.getIdxForInlinedCall(), UpdatedLoc.getIdxForInlinedCall());
    } else {
      UpdatedLoc = FLDebugLoc(DLMap.mapSrcLoc(LocToUpdate.SrcLocIdx), UpdatedLoc.getIdxForInlinedCall(), LocToUpdate.AtomGroup, LocToUpdate.AtomRank);
    }
  }

  return DebugLoc(UpdatedLoc, NewFnContext.Context);
}


DebugLoc DebugLoc::appendInlinedAt(const DebugLoc &DL, DebugLoc InlinedAt,
                                   LLVMContext &Ctx,
                                   DenseMap<const MDNode *, MDNode *> &Cache,
                                   DebugLocMap &DLMap) {
  SmallVector<DebugLoc, 3> InlinedAtLocations;
  DebugLoc Last = InlinedAt;
  DebugLoc CurInlinedAt = DL;

  // Gather all the inlined-at nodes.
  while (DebugLoc IA = CurInlinedAt.getInlinedAt()) {
    // Skip any we've already built nodes for.
    if (auto Found = DLMap.getDebugLoc(IA)) {
      Last = Found;
      break;
    }

    InlinedAtLocations.push_back(IA);
    CurInlinedAt = IA;
  }

  // Starting from the top, rebuild the nodes to point to the new inlined-at
  // location (then rebuilding the rest of the chain behind it) and update the
  // map of already-constructed inlined-at nodes.
  // Key Instructions: InlinedAt fields don't need atom info.
  for (DebugLoc MD : reverse(InlinedAtLocations)) {
    assert(MD.isInlinedCall());
    Last = DebugLoc::getDistinctInlinedCall(
        InlinedAt, MD.getLine(), MD.getColumn(), MD.getScope(), Last, false,
        MD.getAsInlinedCall().MaxAtomGroup);
    DLMap.insertInlinedCall(MD.getIdxForInlinedCall(),
                            Last.getIdxForInlinedCall());
  }

  return Last;
}

DebugLoc DebugLoc::getMergedLocations(ArrayRef<DebugLoc> Locs) {
  if (Locs.empty())
    return DebugLoc();
  if (Locs.size() == 1)
    return Locs[0];
  DebugLoc Merged = Locs[0];
  for (const DebugLoc &DL : llvm::drop_begin(Locs)) {
    Merged = getMergedLocation(Merged, DL);
    if (!Merged)
      break;
  }
  return Merged;
}
DebugLoc DebugLoc::getMergedLocations(ArrayRef<DbgLocStorage> Locs, DebugLocContext Context) {
  if (Locs.empty())
    return DebugLoc();
  if (Locs.size() == 1)
    return Locs[0].withContext(Context);
  DebugLoc Merged = Locs[0].withContext(Context);
  for (DbgLocStorage DL : llvm::drop_begin(Locs)) {
    Merged = getMergedLocation(Merged, DL, Context);
    if (!Merged)
      break;
  }
  return Merged;
}

namespace {
using LineColumn = std::pair<unsigned /* Line */, unsigned /* Column */>;

/// Returns the location of DILocalScope, if present, or a default value.
static LineColumn getLocalScopeLocationOr(DIScope *S, LineColumn Default) {
  assert(isa<DILocalScope>(S) && "Expected DILocalScope.");

  if (isa<DILexicalBlockFile>(S))
    return Default;
  if (auto *LB = dyn_cast<DILexicalBlock>(S))
    return {LB->getLine(), LB->getColumn()};
  if (auto *SP = dyn_cast<DISubprogram>(S))
    return {SP->getLine(), 0u};

  llvm_unreachable("Unhandled type of DILocalScope.");
}

// Returns the nearest matching scope inside a subprogram.
template <typename MatcherT>
static std::pair<DIScope *, LineColumn>
getNearestMatchingScope(DebugLoc L1, DebugLoc L2) {
  MatcherT Matcher;

  DIScope *S1 = L1.getScope();
  DIScope *S2 = L2.getScope();

  LineColumn Loc1(L1.getLine(), L1.getColumn());
  for (; S1; S1 = S1->getScope()) {
    Loc1 = getLocalScopeLocationOr(S1, Loc1);
    Matcher.insert(S1, Loc1);
    if (isa<DISubprogram>(S1))
      break;
  }

  LineColumn Loc2(L2.getLine(), L2.getColumn());
  for (; S2; S2 = S2->getScope()) {
    Loc2 = getLocalScopeLocationOr(S2, Loc2);

    if (DIScope *S = Matcher.match(S2, Loc2))
      return std::make_pair(S, Loc2);

    if (isa<DISubprogram>(S2))
      break;
  }
  return std::make_pair(nullptr, LineColumn(L2.getLine(), L2.getColumn()));
}

// Matches equal scopes.
struct EqualScopesMatcher {
  SmallPtrSet<DIScope *, 8> Scopes;

  void insert(DIScope *S, LineColumn Loc) { Scopes.insert(S); }

  DIScope *match(DIScope *S, LineColumn Loc) {
    return Scopes.contains(S) ? S : nullptr;
  }
};

// Matches scopes with the same location.
struct ScopeLocationsMatcher {
  SmallMapVector<std::pair<DIFile *, LineColumn>, SmallSetVector<DIScope *, 8>,
                 8>
      Scopes;

  void insert(DIScope *S, LineColumn Loc) {
    Scopes[{S->getFile(), Loc}].insert(S);
  }

  DIScope *match(DIScope *S, LineColumn Loc) {
    auto *ScopesAtLoc = Scopes.find({S->getFile(), Loc});
    // No scope found with the given location.
    if (ScopesAtLoc == Scopes.end())
      return nullptr;

    // Prefer S over other scopes with the same location.
    if (ScopesAtLoc->second.contains(S))
      return S;

    if (!ScopesAtLoc->second.empty())
      return *ScopesAtLoc->second.begin();

    llvm_unreachable("Scopes must not have empty entries.");
  }
};
static DILexicalBlockBase *cloneAndReplaceParentScope(DILexicalBlockBase *LBB,
                                                      DIScope *NewParent) {
  TempMDNode ClonedScope = LBB->clone();
  cast<DILexicalBlockBase>(*ClonedScope).replaceScope(NewParent);
  return cast<DILexicalBlockBase>(
      MDNode::replaceWithUniqued(std::move(ClonedScope)));
}
} // end anonymous namespace

static DebugLoc getMergedDebugLoc(DebugLoc LocA, DebugLoc LocB) {
  if (LocA == LocB)
    return LocA;

  // For some use cases (SamplePGO), it is important to retain distinct source
  // locations. When this flag is set, we choose arbitrarily between A and B,
  // rather than computing a merged location using line 0, which is typically
  // not useful for PGO. If one of them is null, then try to return one which is
  // valid.
  if (PickMergedSourceLocations) {
    if (!LocA || !LocB)
      return LocA ? LocA : LocB;

    auto A = std::make_tuple(LocA.getLine(), LocA.getColumn(),
                             LocA.getDiscriminator(), LocA.getFilename(),
                             LocA.getDirectory());
    auto B = std::make_tuple(LocB.getLine(), LocB.getColumn(),
                             LocB.getDiscriminator(), LocB.getFilename(),
                             LocB.getDirectory());
    return A < B ? LocA : LocB;
  }

  if (!LocA || !LocB)
    return nullptr;

  DebugLoc::DebugLocContext C(LocA);

  using LocVec = SmallVector<DebugLoc>;
  LocVec ALocs;
  LocVec BLocs;
  SmallDenseMap<std::pair<const DISubprogram *, DebugLoc>, unsigned,
                4>
      ALookup;

  // Walk through LocA and its inlined-at locations, populate them in ALocs and
  // save the index for the subprogram and inlined-at pair, which we use to find
  // a matching starting location in LocB's chain.
  for (auto [L, I] = std::make_pair(LocA, 0U); L; L = L.getInlinedAt(), I++) {
    ALocs.push_back(L);
    auto Res = ALookup.try_emplace(
        {L.getScope()->getSubprogram(), L.getInlinedAt()}, I);
    assert(Res.second && "Multiple <SP, InlinedAt> pairs in a location chain?");
    (void)Res;
  }

  LocVec::reverse_iterator ARIt = ALocs.rend();
  LocVec::reverse_iterator BRIt = BLocs.rend();

  // Populate BLocs and look for a matching starting location, the first
  // location with the same subprogram and inlined-at location as in LocA's
  // chain. Since the two locations have the same inlined-at location we do
  // not need to look at those parts of the chains.
  for (auto [L, I] = std::make_pair(LocB, 0U); L; L = L.getInlinedAt(), I++) {
    BLocs.push_back(L);

    if (ARIt != ALocs.rend())
      // We have already found a matching starting location.
      continue;

    auto IT = ALookup.find({L.getScope()->getSubprogram(), L.getInlinedAt()});
    if (IT == ALookup.end())
      continue;

    // The + 1 is to account for the &*rev_it = &(it - 1) relationship.
    ARIt = LocVec::reverse_iterator(ALocs.begin() + IT->second + 1);
    BRIt = LocVec::reverse_iterator(BLocs.begin() + I + 1);

    // If we have found a matching starting location we do not need to add more
    // locations to BLocs, since we will only look at location pairs preceding
    // the matching starting location, and adding more elements to BLocs could
    // invalidate the iterator that we initialized here.
    break;
  }

  // Merge the two locations if possible, using the supplied
  // inlined-at location for the created location.
  DebugLoc LocAIA = LocA.getInlinedAt();
  DebugLoc LocBIA = LocB.getInlinedAt();
  auto MergeLocPair = [&C, LocAIA,
                       LocBIA](const DebugLoc L1, const DebugLoc L2,
                               DebugLoc InlinedAt) -> DebugLoc {
    if (L1 == L2)
      return DebugLoc::get(C, L1.getLine(), L1.getColumn(), L1.getScope(),
                             InlinedAt, L1.isImplicitCode(),
                             L1.getAtomGroup(), L1.getAtomRank());

    // If the locations originate from different subprograms we can't produce
    // a common location.
    if (L1.getScope()->getSubprogram() != L2.getScope()->getSubprogram())
      return DebugLoc();

    // If the locations are not both inlined calls or both instruction locs,
    // then we cannot produce a common location.
    // TODO: Is it possible that we could merge an inlined call with a
    // non-inlined call here if they were otherwise compatible? Should be fine
    // to handle if so.
    if (L1.isInlinedCall() != L2.isInlinedCall())
      return DebugLoc();
    bool IsInlinedCall = L1.isInlinedCall();
    uint16_t MaxMergedAtomGroup = IsInlinedCall ? std::max(L1.getAsInlinedCall().MaxAtomGroup, L2.getAsInlinedCall().MaxAtomGroup) : 0;

    // Find nearest common scope inside subprogram.
    DIScope *Scope = getNearestMatchingScope<EqualScopesMatcher>(L1, L2).first;
    assert(Scope && "No common scope in the same subprogram?");

    // Try using the nearest scope with common location if files are different.
    if (Scope->getFile() != L1.getFile() || L1.getFile() != L2.getFile()) {
      auto [CommonLocScope, CommonLoc] =
          getNearestMatchingScope<ScopeLocationsMatcher>(L1, L2);

      // If CommonLocScope is a DILexicalBlockBase, clone it and locate
      // a new scope inside the nearest common scope to preserve
      // lexical blocks structure.
      if (auto *LBB = dyn_cast<DILexicalBlockBase>(CommonLocScope);
          LBB && LBB != Scope)
        CommonLocScope = cloneAndReplaceParentScope(LBB, Scope);

      Scope = CommonLocScope;

      // If files are still different, assume that L1 and L2 were "included"
      // from CommonLoc. Use it as merged location.
      if (Scope->getFile() != L1.getFile() || L1.getFile() != L2.getFile()) {
        if (IsInlinedCall)
          return DebugLoc::getUniquedInlinedCall(C, CommonLoc.first, CommonLoc.second,
                                                 CommonLocScope, InlinedAt, false, MaxMergedAtomGroup);
        return DebugLoc::get(C, CommonLoc.first, CommonLoc.second,
                               CommonLocScope, InlinedAt);
      }
    }

    bool SameLine = L1.getLine() == L2.getLine();
    bool SameCol = L1.getColumn() == L2.getColumn();
    unsigned Line = SameLine ? L1.getLine() : 0;
    unsigned Col = SameLine && SameCol ? L1.getColumn() : 0;
    bool IsImplicitCode = L1.isImplicitCode() && L2.isImplicitCode();

    // Discard source location atom if the line becomes 0. And there's nothing
    // further to do if neither location has an atom number.
    if (!SameLine || !(L1.getAtomGroup() || L2.getAtomGroup())) {
      if (IsInlinedCall)
        return DebugLoc::getUniquedInlinedCall(C, Line, Col, Scope, InlinedAt, IsImplicitCode,
                              MaxMergedAtomGroup);
      return DebugLoc::get(C, Line, Col, Scope, InlinedAt, IsImplicitCode,
                             /*AtomGroup*/ 0, /*AtomRank*/ 0);
    }

    uint64_t Group = 0;
    uint64_t Rank = 0;
    // If we're preserving the same matching inlined-at field we can
    // preserve the atom.
    if (LocBIA == LocAIA && InlinedAt == LocBIA) {
      // Deterministically keep the lowest non-zero ranking atom group
      // number.
      // FIXME: It would be nice if we could track that an instruction
      // belongs to two source atoms.
      bool UseL1Atom = [L1, L2]() {
        if (L1.getAtomRank() == L2.getAtomRank()) {
          // Arbitrarily choose the lowest non-zero group number.
          if (!L1.getAtomGroup() || !L2.getAtomGroup())
            return !L2.getAtomGroup();
          return L1.getAtomGroup() < L2.getAtomGroup();
        }
        // Choose the lowest non-zero rank.
        if (!L1.getAtomRank() || !L2.getAtomRank())
          return !L2.getAtomRank();
        return L1.getAtomRank() < L2.getAtomRank();
      }();
      Group = UseL1Atom ? L1.getAtomGroup() : L2.getAtomGroup();
      Rank = UseL1Atom ? L1.getAtomRank() : L2.getAtomRank();
    } else {
      // If either instruction is part of a source atom, reassign it a new
      // atom group. This essentially regresses to non-key-instructions
      // behaviour (now that it's the only instruction in its group it'll
      // probably get is_stmt applied).
      Group = InlinedAt.getNewAtomGroup();
      Rank = 1;
    }
    if (IsInlinedCall)
      return DebugLoc::getUniquedInlinedCall(
        C, Line, Col, Scope, InlinedAt, IsImplicitCode, MaxMergedAtomGroup);
    return DebugLoc::get(C, Line, Col, Scope, InlinedAt, IsImplicitCode,
                           Group, Rank);
  };

  DebugLoc Result = ARIt != ALocs.rend() ? (*ARIt).getInlinedAt() : nullptr;

  // If we have found a common starting location, walk up the inlined-at chains
  // and try to produce common locations.
  for (; ARIt != ALocs.rend() && BRIt != BLocs.rend(); ++ARIt, ++BRIt) {
    DebugLoc Tmp = MergeLocPair(*ARIt, *BRIt, Result);

    if (!Tmp)
      // We have walked up to a point in the chains where the two locations
      // are irreconsilable. At this point Result contains the nearest common
      // location in the inlined-at chains of LocA and LocB, so we break here.
      break;

    Result = Tmp;
  }

  if (Result) {
    // If we weren't able to fully reconcile the inlinedAt chain between LocA
    // and LocB, the result may be an InlinedCall location; if that is the case,
    // get the location of that inlined call.
    if (Result.isInlinedCall())
      return Result.getLocForInlinedCall();
    return Result;
  }

  // We ended up with LocA and LocB as irreconsilable locations. Produce a
  // location at 0:0 with one of the locations' scope. The function has
  // historically picked A's scope, and a nullptr inlined-at location, so that
  // behavior is mimicked here but I am not sure if this is always the correct
  // way to handle this.
  // Key Instructions: it's fine to drop atom group and rank here, as line 0
  // is a nonsensical is_stmt location.
  // NB: Drive-by change in the switch to use Function-Local Metadata: use the
  // *InlinedAtScope*
  return DebugLoc::get(C, 0, 0, LocA.getInlinedAtScope());
}

DebugLoc DebugLoc::getMergedLocation(DebugLoc LocA, DebugLoc LocB) {
  if (!(PickMergedSourceLocations && (LocA || LocB)) && (!LocA || !LocB)) {
    // If coverage tracking is enabled, prioritize returning empty non-annotated
    // locations to empty annotated locations.
#if LLVM_ENABLE_DEBUGLOC_TRACKING_COVERAGE
    if (!LocA && LocA.getKind() == DebugLocKind::Normal)
      return LocA;
    if (!LocB && LocB.getKind() == DebugLocKind::Normal)
      return LocB;
#endif // LLVM_ENABLE_DEBUGLOC_TRACKING_COVERAGE
    if (!LocA)
      return LocA;
    return LocB;
  }
  return getMergedDebugLoc(LocA, LocB);
}
DebugLoc DebugLoc::getMergedLocation(DbgLocStorage LocA, DbgLocStorage LocB, DebugLocContext Context) {
  if (!(PickMergedSourceLocations && (LocA || LocB)) && (!LocA || !LocB)) {
    // If coverage tracking is enabled, prioritize returning empty non-annotated
    // locations to empty annotated locations.
#if LLVM_ENABLE_DEBUGLOC_TRACKING_COVERAGE
    if (!LocA && LocA.getKind() == DebugLocKind::Normal)
      return LocA;
    if (!LocB && LocB.getKind() == DebugLocKind::Normal)
      return LocB;
#endif // LLVM_ENABLE_DEBUGLOC_TRACKING_COVERAGE
    if (!LocA)
      return LocA.withContext(Context);
    return LocB.withContext(Context);
  }
  return getMergedDebugLoc(LocA.withContext(Context), LocB.withContext(Context));
}

#if !defined(NDEBUG) || defined(LLVM_ENABLE_DUMP)
LLVM_DUMP_METHOD void DebugLoc::dump() const { print(dbgs()); }
LLVM_DUMP_METHOD void DebugLoc::dump(const Module *M) const { print(dbgs(), M); }
#endif

void DebugLoc::print(raw_ostream &OS) const {
  if (!Storage)
    return;

  // Print source line info.
  auto *Scope = cast<DIScope>(getScope());
  OS << Scope->getFilename();
  OS << ':' << getLine();
  if (getCol() != 0)
    OS << ':' << getCol();

  if (DebugLoc InlinedAtDL = getInlinedAt()) {
    OS << " @[ ";
    InlinedAtDL.print(OS);
    OS << " ]";
  }
}

void DebugLoc::print(raw_ostream &OS, const Module *M, bool IsForDebug) const {
  return getAsDILocation()->print(OS, M, IsForDebug);
}
void DebugLoc::print(raw_ostream &OS, ModuleSlotTracker &MST, const Module *M,
                     bool IsForDebug) const {
  return getAsDILocation()->print(OS, MST, M, IsForDebug);
}
void DebugLoc::printAsOperand(raw_ostream &OS, const Module *M) const {
  return getAsDILocation()->printAsOperand(OS, M);
}
void DebugLoc::printAsOperand(raw_ostream &OS, ModuleSlotTracker &MST,
                     const Module *M) const {
  return getAsDILocation()->printAsOperand(OS, MST, M);
}
bool DebugLoc::isDistinct() const {
  return isInlinedCall() && !getAsInlinedCall().Uniquable;
}

LLVMContext &DebugLoc::getContext() const { return FLContext->getContext(); }

uint64_t DebugLoc::getAtomGroup() const {
  return Storage.Loc.AtomGroup;
}
uint8_t DebugLoc::getAtomRank() const {
  return Storage.Loc.AtomRank;
}

DebugLoc DebugLoc::getWithAtom(uint16_t Group, uint16_t Rank) const {
  FLDebugLoc Loc = Storage.Loc;
  Loc.AtomGroup = Group;
  Loc.AtomRank = Rank;
  return DebugLoc(Loc, FLContext);
}
DebugLoc DebugLoc::getWithoutAtom() const {
  FLDebugLoc Loc = Storage.Loc;
  Loc.AtomGroup = 0;
  Loc.AtomRank = 0;
  return DebugLoc(Loc, FLContext);
}

uint16_t DebugLoc::getNewAtomGroup() const {
  // Although we use different definitions of `getNewAtomGroup` for instruction
  // locs and inlined call locs, in practice the implementation is identical.
  return FLContext->getNewAtomGroup(getUnderlyingStorage().InlinedAtIdx);
}

DebugLoc DebugLoc::getAtomContext() const {
  assert(isInstrLoc() && "Only instruction locs have atoms.");
  return getInlinedAt();
}

StringRef DebugLoc::getSubprogramLinkageName() const {
  DISubprogram *SP = getScope()->getSubprogram();
  if (!SP)
    return "";
  auto Name = SP->getLinkageName();
  if (!Name.empty())
    return Name;
  return SP->getName();
}

DIFile *DebugLoc::getFile() const {
  return getScope()->getFile();
}
StringRef DebugLoc::getFilename() const {
  return getScope()->getFilename();
}
StringRef DebugLoc::getDirectory() const {
  return getScope()->getDirectory();
}
std::optional<StringRef> DebugLoc::getSource() const {
  return getScope()->getSource();
}

DebugLoc DebugLoc::getInlinedAtLocation() const {
  DebugLoc RootDL = *this;
  while (DebugLoc InlinedAt = RootDL.getInlinedAt())
    RootDL = InlinedAt;
  return RootDL;
}

unsigned DebugLoc::getDiscriminator() const {
  if (auto *F = dyn_cast<DILexicalBlockFile>(getScope()))
    return F->getDiscriminator();
  return 0;
}

/// Returns a new DebugLoc with updated \p Discriminator.
DebugLoc DebugLoc::cloneWithDiscriminator(unsigned Discriminator) const {
  DILocalScope *Scope = getScope();
  // Skip all parent DILexicalBlockFile that already have a discriminator
  // assigned. We do not want to have nested DILexicalBlockFiles that have
  // multiple discriminators because only the leaf DILexicalBlockFile's
  // dominator will be used.
  for (auto *LBF = dyn_cast<DILexicalBlockFile>(Scope);
       LBF && LBF->getDiscriminator() != 0;
       LBF = dyn_cast<DILexicalBlockFile>(Scope))
    Scope = LBF->getScope();
  DILexicalBlockFile *NewScope =
      DILexicalBlockFile::get(getContext(), Scope, getFile(), Discriminator);
  return DebugLoc::get(*this, getLine(), getColumn(), NewScope,
                       getInlinedAt(), isImplicitCode(), getAtomGroup(),
                       getAtomRank());
}
void DebugLoc::decodeDiscriminator(unsigned D, unsigned &BD, unsigned &DF,
                                     unsigned &CI) {
  BD = getUnsignedFromPrefixEncoding(D);
  DF = getUnsignedFromPrefixEncoding(getNextComponentInDiscriminator(D));
  CI = getUnsignedFromPrefixEncoding(
      getNextComponentInDiscriminator(getNextComponentInDiscriminator(D)));
}

/// Returns a new DebugLoc with updated base discriminator \p BD. Only the
/// base discriminator is set in the new DebugLoc, the other encoded values
/// are elided.
/// If the discriminator cannot be encoded, the function returns std::nullopt.
std::optional<DebugLoc>
DebugLoc::cloneWithBaseDiscriminator(unsigned D) const {
  // Do not interfere with pseudo probes. Pseudo probe at a callsite uses
  // the dwarf discriminator to store pseudo probe related information,
  // such as the probe id.
  if (isPseudoProbeDiscriminator(getDiscriminator()))
    return *this;

  unsigned BD, DF, CI;

  if (EnableFSDiscriminator) {
    BD = getBaseDiscriminator();
    if (D == BD)
      return *this;
    return cloneWithDiscriminator(D);
  }

  decodeDiscriminator(getDiscriminator(), BD, DF, CI);
  if (D == BD)
    return *this;
  if (std::optional<unsigned> Encoded = encodeDiscriminator(D, DF, CI))
    return cloneWithDiscriminator(*Encoded);
  return std::nullopt;
}

/// Returns the duplication factor stored in the discriminator, or 1 if no
/// duplication factor (or 0) is encoded.
unsigned DebugLoc::getDuplicationFactor() const {
  return getDuplicationFactorFromDiscriminator(getDiscriminator());
}

/// Returns the copy identifier stored in the discriminator.
unsigned DebugLoc::getCopyIdentifier() const {
  return getCopyIdentifierFromDiscriminator(getDiscriminator());
}

/// Returns the base discriminator stored in the discriminator.
unsigned DebugLoc::getBaseDiscriminator() const {
  return getBaseDiscriminatorFromDiscriminator(getDiscriminator(),
                                               EnableFSDiscriminator);
}

/// Returns a new DebugLoc with duplication factor \p DF * current
/// duplication factor encoded in the discriminator. The current duplication
/// factor is as defined by getDuplicationFactor().
/// Returns std::nullopt if encoding failed.
std::optional<DebugLoc>
DebugLoc::cloneByMultiplyingDuplicationFactor(unsigned DF) const {
  assert(!EnableFSDiscriminator && "FSDiscriminator should not call this.");
  // Do no interfere with pseudo probes. Pseudo probe doesn't need duplication
  // factor support as samples collected on cloned probes will be aggregated.
  // Also pseudo probe at a callsite uses the dwarf discriminator to store
  // pseudo probe related information, such as the probe id.
  if (isPseudoProbeDiscriminator(getDiscriminator()))
    return *this;

  DF *= getDuplicationFactor();
  if (DF <= 1)
    return *this;

  unsigned BD = getBaseDiscriminator();
  unsigned CI = getCopyIdentifier();
  if (std::optional<unsigned> D = encodeDiscriminator(BD, DF, CI))
    return cloneWithDiscriminator(*D);
  return std::nullopt;
}

Metadata *DebugLoc::getRawScope() const {
  assert(Storage && FLContext && "Expected valid DebugLoc");
  return Storage.Loc.getScope(FLContext);
}
Metadata *DebugLoc::getRawInlinedAt() const {
  llvm_unreachable("Invalid when FLMD enabled.");
}

bool DebugLoc::isPseudoProbeDiscriminator(unsigned Discriminator) {
  return DILocation::isPseudoProbeDiscriminator(Discriminator);
}

LLVM_ABI std::optional<unsigned>
DebugLoc::encodeDiscriminator(unsigned BD, unsigned DF, unsigned CI) {
  return DILocation::encodeDiscriminator(BD, DF, CI);
}
#else
unsigned DebugLoc::getLine() const {
  assert(Storage && FLContext && "Expected valid DebugLoc");
  return Storage.get()->getLine();
}

unsigned DebugLoc::getCol() const {
  assert(Storage && FLContext && "Expected valid DebugLoc");
  return Storage.get()->getColumn();
}

DILocalScope *DebugLoc::getScope() const {
  assert(Storage && FLContext && "Expected valid DebugLoc");
  return Storage.get()->getScope();
}

DebugLoc DebugLoc::getInlinedAt() const {
  assert(Storage && FLContext && "Expected valid DebugLoc");
  return DebugLoc::getFromDILocation(Storage.get()->getInlinedAt());
}

DILocalScope *DebugLoc::getInlinedAtScope() const {
  return cast<DILocation>(Storage.get())->getInlinedAtScope();
}

DebugLoc DebugLoc::getFnDebugLoc() const {
  // FIXME: Add a method on \a DILocation that does this work.
  const MDNode *Scope = getInlinedAtScope();
  if (auto *SP = getDISubprogram(Scope))
    return DebugLoc::get(SP->getContext(), SP->getScopeLine(), 0, SP);

  return DebugLoc();
}

MDNode *DebugLoc::getAsMDNode() const { return Storage.get(); }

bool DebugLoc::isImplicitCode() const {
  if (Storage)
    return Storage.get()->isImplicitCode();
  return true;
}

void DebugLoc::setImplicitCode(bool ImplicitCode) {
  if (Storage)
    Storage.get()->setImplicitCode(ImplicitCode);
}

DebugLoc DebugLoc::getAtomContext() const {
  return getInlinedAt();
}

DebugLoc DebugLoc::replaceInlinedAtSubprogram(
    const DebugLoc &RootLocDL, DISubprogram &NewSP, DebugLocContext NewFnContext,
    DenseMap<const MDNode *, MDNode *> &Cache, DebugLocMap &DLMap) {
  DILocation *RootLoc = RootLocDL.getAsDILocation();
  SmallVector<DILocation *> LocChain;
  DILocation *CachedResult = nullptr;

  // Collect the inline chain, stopping if we find a location that has already
  // been processed.
  for (DILocation *Loc = RootLoc; Loc; Loc = Loc->getInlinedAt()) {
    if (auto It = Cache.find(Loc); It != Cache.end()) {
      CachedResult = cast<DILocation>(It->second);
      break;
    }
    LocChain.push_back(Loc);
  }

  DILocation *UpdatedLoc = CachedResult;
  if (!UpdatedLoc) {
    // If no cache hits, then back() is the end of the inline chain, that is,
    // the DILocation whose scope ends in the Subprogram to be replaced.
    DILocation *LocToUpdate = LocChain.pop_back_val();
    DIScope *NewScope = DILocalScope::cloneScopeForSubprogram(
        *LocToUpdate->getScope(), NewSP, NewFnContext.Context, Cache);
    UpdatedLoc = DILocation::get(NewFnContext.Context, LocToUpdate->getLine(),
                                 LocToUpdate->getColumn(), NewScope);
    Cache[LocToUpdate] = UpdatedLoc;
  }

  // Recreate the location chain, bottom-up, starting at the new scope (or a
  // cached result).
  for (const DILocation *LocToUpdate : reverse(LocChain)) {
    UpdatedLoc =
        DILocation::get(NewFnContext.Context, LocToUpdate->getLine(), LocToUpdate->getColumn(),
                        LocToUpdate->getScope(), UpdatedLoc);
    Cache[LocToUpdate] = UpdatedLoc;
  }

  return DebugLoc::getFromDILocation(UpdatedLoc);
}

DebugLoc DebugLoc::appendInlinedAt(const DebugLoc &DL, DebugLoc InlinedAt,
                                   LLVMContext &Ctx,
                                   DenseMap<const MDNode *, MDNode *> &Cache,
                                   DebugLocMap &DLMap) {
  SmallVector<DILocation *, 3> InlinedAtLocations;
  DILocation *Last = InlinedAt.Storage.get();
  DILocation *CurInlinedAt = DL.Storage.get();

  // Gather all the inlined-at nodes.
  while (DILocation *IA = CurInlinedAt->getInlinedAt()) {
    // Skip any we've already built nodes for.
    if (auto *Found = Cache[IA]) {
      Last = cast<DILocation>(Found);
      break;
    }

    InlinedAtLocations.push_back(IA);
    CurInlinedAt = IA;
  }

  // Starting from the top, rebuild the nodes to point to the new inlined-at
  // location (then rebuilding the rest of the chain behind it) and update the
  // map of already-constructed inlined-at nodes.
  // Key Instructions: InlinedAt fields don't need atom info.
  for (const DILocation *MD : reverse(InlinedAtLocations))
    Cache[MD] = Last = DILocation::getDistinct(
        Ctx, MD->getLine(), MD->getColumn(), MD->getScope(), Last);

  return DebugLoc::getFromDILocation(Last);
}

DebugLoc DebugLoc::getMergedLocations(ArrayRef<DebugLoc> Locs) {
  if (Locs.empty())
    return DebugLoc();
  if (Locs.size() == 1)
    return Locs[0];
  DebugLoc Merged = Locs[0];
  for (const DebugLoc &DL : llvm::drop_begin(Locs)) {
    Merged = getMergedLocation(Merged, DL);
    if (!Merged)
      break;
  }
  return Merged;
}
DebugLoc DebugLoc::getMergedLocation(DebugLoc LocA, DebugLoc LocB) {
  if (!LocA || !LocB) {
    // If we are missing either location but have requested
    // PickMergedSourceLocations, then just forward straight to the
    // DILocation version.
    if (PickMergedSourceLocations)
      return DebugLoc::getFromDILocation(DILocation::getMergedLocation(LocA.getAsDILocation(), LocB.getAsDILocation()));
    // If coverage tracking is enabled, prioritize returning empty non-annotated
    // locations to empty annotated locations.
#if LLVM_ENABLE_DEBUGLOC_TRACKING_COVERAGE
    if (!LocA && LocA.getKind() == DebugLocKind::Normal)
      return LocA;
    if (!LocB && LocB.getKind() == DebugLocKind::Normal)
      return LocB;
#endif // LLVM_ENABLE_DEBUGLOC_TRACKING_COVERAGE
    if (!LocA)
      return LocA;
    return LocB;
  }
  return DebugLoc::getFromDILocation(DILocation::getMergedLocation(LocA.getAsDILocation(), LocB.getAsDILocation()));
}

#if !defined(NDEBUG) || defined(LLVM_ENABLE_DUMP)
LLVM_DUMP_METHOD void DebugLoc::dump() const { print(dbgs()); }
LLVM_DUMP_METHOD void DebugLoc::dump(const Module *M) const { print(dbgs(), M); }
#endif

void DebugLoc::print(raw_ostream &OS) const {
  if (!Storage)
    return;

  // Print source line info.
  auto *Scope = cast<DIScope>(getScope());
  OS << Scope->getFilename();
  OS << ':' << getLine();
  if (getCol() != 0)
    OS << ':' << getCol();

  if (DebugLoc InlinedAtDL = getInlinedAt()) {
    OS << " @[ ";
    InlinedAtDL.print(OS);
    OS << " ]";
  }
}

void DebugLoc::print(raw_ostream &OS, const Module *M, bool IsForDebug) const {
  return Storage.get()->print(OS, M, IsForDebug);
}
void DebugLoc::print(raw_ostream &OS, ModuleSlotTracker &MST, const Module *M,
                     bool IsForDebug) const {
  return Storage.get()->print(OS, MST, M, IsForDebug);
}
void DebugLoc::printAsOperand(raw_ostream &OS, const Module *M) const {
  return Storage.get()->printAsOperand(OS, M);
}
void DebugLoc::printAsOperand(raw_ostream &OS, ModuleSlotTracker &MST,
                     const Module *M) const {
  return Storage.get()->printAsOperand(OS, MST, M);
}
bool DebugLoc::isDistinct() const {
  return Storage.get()->isDistinct();
}

LLVMContext &DebugLoc::getContext() const { return Storage.get()->getContext(); }

uint64_t DebugLoc::getAtomGroup() const {
  return Storage.get()->getAtomGroup();
}
uint8_t DebugLoc::getAtomRank() const {
  return Storage.get()->getAtomRank();
}

DebugLoc DebugLoc::getWithoutAtom() const {
  return DebugLoc::getFromDILocation(Storage.get()->getWithoutAtom());
}

StringRef DebugLoc::getSubprogramLinkageName() const {
  return Storage.get()->getSubprogramLinkageName();
}

DIFile *DebugLoc::getFile() const {
  return Storage.get()->getFile();
}
StringRef DebugLoc::getFilename() const {
  return Storage.get()->getFilename();
}
StringRef DebugLoc::getDirectory() const {
  return Storage.get()->getDirectory();
}
std::optional<StringRef> DebugLoc::getSource() const {
  return Storage.get()->getSource();
}

DebugLoc DebugLoc::getInlinedAtLocation() const {
  return DebugLoc::getFromDILocation(Storage.get()->getInlinedAtLocation());
}

unsigned DebugLoc::getDiscriminator() const {
  return Storage.get()->getDiscriminator();
}

/// Returns a new DebugLoc with updated \p Discriminator.
DebugLoc DebugLoc::cloneWithDiscriminator(unsigned Discriminator) const {
  return DebugLoc::getFromDILocation(Storage.get()->cloneWithDiscriminator(Discriminator));
}

/// Returns a new DebugLoc with updated base discriminator \p BD. Only the
/// base discriminator is set in the new DebugLoc, the other encoded values
/// are elided.
/// If the discriminator cannot be encoded, the function returns std::nullopt.
std::optional<DebugLoc>
DebugLoc::cloneWithBaseDiscriminator(unsigned BD) const {
  std::optional<const DILocation*> DL = Storage.get()->cloneWithBaseDiscriminator(BD);
  if (DL)
    return DebugLoc::getFromDILocation(*DL);
  return std::nullopt;
}

/// Returns the duplication factor stored in the discriminator, or 1 if no
/// duplication factor (or 0) is encoded.
unsigned DebugLoc::getDuplicationFactor() const {
  return Storage.get()->getDuplicationFactor();
}

/// Returns the copy identifier stored in the discriminator.
unsigned DebugLoc::getCopyIdentifier() const {
  return Storage.get()->getCopyIdentifier();
}

/// Returns the base discriminator stored in the discriminator.
unsigned DebugLoc::getBaseDiscriminator() const {
  return Storage.get()->getBaseDiscriminator();
}

/// Returns a new DebugLoc with duplication factor \p DF * current
/// duplication factor encoded in the discriminator. The current duplication
/// factor is as defined by getDuplicationFactor().
/// Returns std::nullopt if encoding failed.
std::optional<DebugLoc>
DebugLoc::cloneByMultiplyingDuplicationFactor(unsigned DF) const {
  std::optional<const DILocation*> DL = Storage.get()->cloneByMultiplyingDuplicationFactor(DF);
  if (DL)
    return DebugLoc::getFromDILocation(*DL);
  return std::nullopt;
}

Metadata *DebugLoc::getRawScope() const {
  return Storage.get()->getRawScope();
}
Metadata *DebugLoc::getRawInlinedAt() const {
  return Storage.get()->getRawInlinedAt();
}

bool DebugLoc::isPseudoProbeDiscriminator(unsigned Discriminator) {
  return DILocation::isPseudoProbeDiscriminator(Discriminator);
}

LLVM_ABI std::optional<unsigned>
DebugLoc::encodeDiscriminator(unsigned BD, unsigned DF, unsigned CI) {
  return DILocation::encodeDiscriminator(BD, DF, CI);
}
#endif

// DILocation *TemporaryFLMDToMDSourceLocConversionContext::getTempDILocation(
//     LLVMContext &Context, unsigned Line, unsigned Column, Metadata *Scope,
//     Metadata *InlinedAt, bool ImplicitCode, uint64_t AtomGroup, uint8_t AtomRank) {
//   // Similar to DILocation::getImpl, but allocates each DILocation as if it were
//   // Distinct, but without ever storing it in the Context itself.
//   // This is a bit of a hack
//   return nullptr;
// }
// DILocation *TemporaryFLMDToMDSourceLocConversionContext::getDILocation(DebugLoc DL) {
//   // Assumption here: no pair of DebugLocs with different contents will have the
//   // same DILocation. If they did, then we would end up with two different
//   // allocations of the same value, which might(?) cause problems.
//   if (auto ExistingIt = FLDebugLocToDILocationMap.find(DL); ExistingIt != FLDebugLocToDILocationMap.end())
//     return ExistingIt->second.get();
//   DIFunctionLocalMetadata *Context = DL.getFLContext();
//   FLDebugLoc FLDL = DL.getStorage().get();
//   DILocation *InlinedAt = nullptr;
//   if (DebugLoc InlinedAtDL = DL.getInlinedAt())
//     InlinedAt = getDILocation(InlinedAtDL);
//   auto [SrcLoc, Scope] = DL.getStorage().Loc.getSrcLocAndScope(DL.getFLContext());
//   DILocation *TempDILoc = getTempDILocation(
//     Context->getContext(), SrcLoc.Line, SrcLoc.Column, Scope, InlinedAt, false,
//     FLDL.AtomGroup, FLDL.AtomRank);
//   FLDebugLocToDILocationMap.insert({DL, std::unique_ptr<DILocation>(TempDILoc)});
//   return TempDILoc;
// }
