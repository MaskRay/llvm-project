//===- DebugLoc.h - Debug Location Information ------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file defines a number of light weight data structures used
// to describe and track debug location information.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_IR_DEBUGLOC_H
#define LLVM_IR_DEBUGLOC_H

#include "llvm/ADT/DenseMap.h"
#include "llvm/Config/llvm-config.h"
#include "llvm/IR/FunctionLocalMetadata.h"
#include "llvm/IR/ModuleSlotTracker.h"
#include "llvm/IR/PseudoProbe.h"
#include "llvm/IR/TrackingMDRef.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/DataTypes.h"
#include "llvm/Support/Discriminator.h"
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <optional>

namespace llvm {

class LLVMContext;
class raw_ostream;
class DILocation;
class DILocalScope;

extern cl::opt<bool> EnableFSDiscriminator;

 #if LLVM_ENABLE_DEBUGLOC_TRACKING_COVERAGE
 #if LLVM_ENABLE_DEBUGLOC_TRACKING_ORIGIN
 extern bool DebugLocOriginCollectionEnabled;
struct DbgLocOrigin {
  static constexpr unsigned long MaxDepth = 16;
  using StackTracesTy =
      SmallVector<std::pair<int, std::array<void *, MaxDepth>>, 0>;
  StackTracesTy StackTraces;
  DbgLocOrigin(bool ShouldCollectTrace);
  void addTrace();
  const StackTracesTy &getOriginStackTraces() const { return StackTraces; };
};
#else
struct DbgLocOrigin {
  DbgLocOrigin(bool) {}
};
#endif
// Used to represent different "kinds" of DebugLoc, expressing that the
// instruction it is part of is either normal and should contain a valid
// DILocation, or otherwise describing the reason why the instruction does
// not contain a valid DILocation.
enum class DebugLocKind : uint8_t {
  // The instruction is expected to contain a valid DILocation.
  Normal,
  // The instruction is compiler-generated, i.e. it is not associated with any
  // line in the original source.
  CompilerGenerated,
  // The instruction has intentionally had its source location removed,
  // typically because it was moved outside of its original control-flow and
  // presenting the prior source location would be misleading for debuggers
  // or profilers.
  Dropped,
  // The instruction does not have a known or currently knowable source
  // location, e.g. the attribution is ambiguous in a way that can't be
  // represented, or determining the correct location is complicated and
  // requires future developer effort.
  Unknown,
  // DebugLoc is attached to an instruction that we don't expect to be
  // emitted, and so can omit a valid DILocation; we don't expect to ever try
  // and emit these into the line table, and trying to do so is a sign that
  // something has gone wrong (most likely a DebugLoc leaking from a transient
  // compiler-generated instruction).
  Temporary
};

// Extends a DILocation pointer to also store a DebugLocKind and Origin,
// allowing Debugify to ignore intentionally-empty DebugLocs and display the
// code responsible for generating unintentionally-empty DebugLocs.
// Currently we only need to track the Origin of this DILoc when using a
// DebugLoc that is not annotated (i.e. has DebugLocKind::Normal) and has a
// null DILocation, so only collect the origin stacktrace in those cases.
class DbgLocCoverageTracking : public DbgLocOrigin {
public:
  DebugLocKind Kind;
  // Default constructor for empty DebugLocs.
  DbgLocCoverageTracking()
      : DbgLocOrigin(true), Kind(DebugLocKind::Normal) {}
  // Valid or nullptr DILocation*, no annotative DebugLocKind.
  DbgLocCoverageTracking(bool HasValidLoc)
      : DbgLocOrigin(!HasValidLoc), Kind(DebugLocKind::Normal) {}
  // Explicit DebugLocKind, which always means an empty location.
  DbgLocCoverageTracking(DebugLocKind Kind)
      : DbgLocOrigin(Kind == DebugLocKind::Normal), Kind(Kind) {}
};
template <> struct simplify_type<DbgLocCoverageTracking> {
  using SimpleType = DILocation *;

  static DILocation *getSimplifiedValue(DbgLocCoverageTracking &MD) {
    return MD;
  }
};
template <> struct simplify_type<const DbgLocCoverageTracking> {
  using SimpleType = DILocation *;

  static DILocation *getSimplifiedValue(const DbgLocCoverageTracking &MD) {
    return MD;
  }
};

#else
struct DbgLocCoverageTracking {
  DbgLocCoverageTracking() {}
  DbgLocCoverageTracking(bool) {}
};
#endif // LLVM_ENABLE_DEBUGLOC_TRACKING_COVERAGE

/// DebugLoc out-of-context, to be stored in an Instruction. Requires a
/// FunctionLocalMetadata reference to be mapped to a concrete object.
/// Has one of 3 states:
/// - Valid SrcLocIdx: This is a normal non-empty FLDebugLoc. May be inlined (if
///   InlinedAtIdx is valid), or non-inlined.
/// - Invalid SrcLocIdx, valid InlinedAtIdx: This is an inlined call, and is not
///   a valid FLDebugLoc to be directly attached to an Instruction; the SrcLoc
///   is retrieved from the FLInlinedCall pointed to by InlinedAtIdx.
/// - Invalid SrcLocIdx, invalid InlinedAtIdx: This is an empty source location.
struct FLDebugLoc {
  FLIndex<uint32_t> SrcLocIdx;
  FLIndex<uint16_t> InlinedAtIdx;
  uint16_t AtomGroup : 13;
  uint16_t AtomRank : 3;

  FLDebugLoc() : SrcLocIdx(), InlinedAtIdx(), AtomGroup(0), AtomRank(0) {}
  FLDebugLoc(FLIndex<uint32_t> SrcLocIdx, FLIndex<uint16_t> InlinedAtIdx, uint16_t AtomGroup = 0, uint8_t AtomRank = 0)
      : SrcLocIdx(SrcLocIdx), InlinedAtIdx(InlinedAtIdx), AtomGroup(AtomGroup), AtomRank(AtomRank) {
    assert((SrcLocIdx || !AtomGroup && !AtomRank) && "Only DebugLoc with a valid SrcLoc may have an atom.");
  }

  bool operator==(const FLDebugLoc& Other) const {
    return asRawInt() == Other.asRawInt();
  }
  bool operator!=(const FLDebugLoc& Other) const {
    return asRawInt() != Other.asRawInt();
  }

  static FLDebugLoc getInlinedCallLoc(FLIndex<uint16_t> InlinedCallIdx) {
    return FLDebugLoc(FLIndex<uint32_t>(), InlinedCallIdx);
  }

  /// An FLDebugLoc is empty iff it has no SrcLoc and no InlinedAtIdx.
  explicit operator bool() const { return asRawInt(); }
  bool isValidLoc() const { return (bool)*this; }
  bool isInlinedInstrLoc() const { return InlinedAtIdx && SrcLocIdx; }
  bool isNonInlinedInstrLoc() const { return !InlinedAtIdx && SrcLocIdx; }
  bool isInstrLoc() const { return SrcLocIdx; }
  bool isInlinedCall() const { return InlinedAtIdx && !SrcLocIdx; }
  /// If this is an InlinedCall, return the FLDebugLoc for the call itself. Note
  /// that this removes the distinction between different inlined calls with
  /// identical source locations, meaning e.g. if a call is duplicated by loop
  /// unrolling and then inlined, each inlined call has a distinct index and the
  /// FLDebugLoc for each will not be equal, but the result of
  /// getLocForInlinedCall from each of them will be identical.
  FLDebugLoc getLocForInlinedCall(DIFunctionLocalMetadata *Context) const {
    assert(isInlinedCall() && "getLocForInlinedCallLoc requires an inlined call");
    FLInlinedCall InlinedCall = Context->getInlinedCall(InlinedAtIdx);
    return FLDebugLoc(InlinedCall.SrcLocIdx, InlinedCall.InlinedAtIdx);
  }
  FLIndex<uint16_t> getIdxForInlinedCall() const {
    assert(isInlinedCall() && "can only getIdxForInlinedCall for an inlined call");
    return InlinedAtIdx;
  }

  uint64_t asRawInt() const {
    static_assert(sizeof(*this) == sizeof(uint64_t));
    uint64_t Result;
    std::memcpy(&Result, this, sizeof(Result));
    return Result;
  }
  static FLDebugLoc fromRawInt(uint64_t Raw) {
    static_assert(sizeof(FLDebugLoc) == sizeof(uint64_t));
    FLDebugLoc Result;
    std::memcpy(&Result, &Raw, sizeof(Result));
    return Result;
  }

  FLInlinedCall getAsInlinedCall(DIFunctionLocalMetadata *Context) const {
    assert(isInlinedCall() && "getAsInlinedCall for non-inlined-call.");
    return Context->getInlinedCall(InlinedAtIdx);
  }
  FLDebugLoc getInlinedAt(DIFunctionLocalMetadata *Context) const {
    if (isInlinedCall())
      return FLDebugLoc::getInlinedCallLoc(getLocForInlinedCall(Context).InlinedAtIdx);
    return FLDebugLoc::getInlinedCallLoc(InlinedAtIdx);
  }
  FLIndex<uint16_t> getInlinedAtIdx(DIFunctionLocalMetadata *Context) const {
    if (isInlinedCall())
      return getLocForInlinedCall(Context).InlinedAtIdx;
    return InlinedAtIdx;
  }
  FLSrcLoc getSrcLoc(DIFunctionLocalMetadata *Context) const {
    if (isInlinedCall())
      return getLocForInlinedCall(Context).getSrcLoc(Context);
    return Context->getSrcLoc(SrcLocIdx);
  }
  DILocalScope *getScope(DIFunctionLocalMetadata *Context) const {
    if (isInlinedCall())
      return getLocForInlinedCall(Context).getScope(Context);
    FLSrcLoc SrcLoc = getSrcLoc(Context);
    return Context->getScope(SrcLoc.ScopeIdx);
  }
  // If we know in advance we know both, this can be faster.
  std::pair<FLSrcLoc, DILocalScope *> getSrcLocAndScope(DIFunctionLocalMetadata *Context) const {
    if (isInlinedCall())
      return getLocForInlinedCall(Context).getSrcLocAndScope(Context);
    FLSrcLoc SrcLoc = getSrcLoc(Context);
    DILocalScope *Scope = Context->getScope(SrcLoc.ScopeIdx);
    return {SrcLoc, Scope};
  }

  uint16_t getWholeAtom() const {
    return AtomGroup << 3 | AtomRank;
  }

  /// Create an FLDebugLoc from the equivalent DILocation. If the DILocation is
  /// an inlined call location, the inlined DISubprogram must also be provided.
  static FLDebugLoc getFromDILocation(const DILocation *DIL, DISubprogram *InlinedSP = nullptr);
};

/// Unwrapped data from FLSrcLoc storage.
struct SrcLocData {
  uint32_t Line;
  uint16_t Column;
  DILocalScope *Scope;
};

DIFunctionLocalMetadata *getFLMDForInstruction(const Instruction *I);
DIFunctionLocalMetadata *getFLMDForFunction(const Function *F);
class DebugLoc;

/// Helper class used to fetch the required context object to create a new
/// DebugLoc from any of the objects that can reach it (see comment above the
/// private functions above).
struct DebugLocContext {
#if LLVM_USE_FLMD_SOURCE_LOCS
  DIFunctionLocalMetadata *Context;
  /// Warning: Only use in conditionally-compiled code, where
  /// LLVM_USE_FLMD_SOURCE_LOCS is defined - otherwise this will cause an
  /// error when the flag is not defined.
  explicit DebugLocContext(DIFunctionLocalMetadata *FLMDContext) : Context(FLMDContext) {}
#else
  LLVMContext &Context;
  /// Warning: Only use in conditionally-compiled code, where
  /// LLVM_USE_FLMD_SOURCE_LOCS is not defined - otherwise this will cause an
  /// error when the flag is defined.
  explicit DebugLocContext(LLVMContext &LLVMContext) : Context(LLVMContext) {}
  DebugLocContext(DebugLoc DL) : Context(DL.getContext()) {
    assert(DL && "DebugLocContext can only be obtained from a non-empty DebugLoc.");
  }
#endif
  DebugLocContext(const Instruction *I);
  DebugLocContext(const Function *F);
  DebugLocContext(DebugLoc DL);
};

/// Debug location information stored directly inside an Instruction.
/// Underlying interface can be accessed via `get`, but care must be taken
/// since the stored type depends on `LLVM_USE_FLMD_SOURCE_LOCS`, so any code
/// that assumes a particular type must be conditional on that define.
class DbgLocStorage : public DbgLocCoverageTracking {
public:
#if LLVM_USE_FLMD_SOURCE_LOCS
  using LocType = FLDebugLoc;
#else
  using LocType = DILocation *;
#endif
  LocType Loc;
  DbgLocStorage() : DbgLocCoverageTracking(false), Loc() {}
  DbgLocStorage(LocType Loc) : DbgLocCoverageTracking((bool)Loc), Loc(Loc) {}

  /// Returns the underlying stored source location reference; this should only
  /// be called in contexts where LocType is known, i.e. in code conditionally
  /// compiled on LLVM_USE_FLMD_SOURCE_LOCS.
  LocType get() const { return Loc; }
  explicit operator bool() const { return (bool)Loc; }

  bool operator==(const DbgLocStorage &Other) const { return Loc == Other.Loc; }
  bool operator!=(const DbgLocStorage &Other) const { return Loc != Other.Loc; }

#if LLVM_USE_FLMD_SOURCE_LOCS
  bool operator<(const DbgLocStorage &Other) const { return Loc.asRawInt() < Other.Loc.asRawInt(); }
#else
  bool operator<(const DbgLocStorage &Other) const { return Loc < Other.Loc; }
#endif
////////////////////////////////////////////////////////////////////////////////
/// DILocation Compatibility
#if LLVM_USE_FLMD_SOURCE_LOCS
  bool operator==(const DILocation *Other) const { llvm_unreachable("Don't compare these"); }
  bool operator!=(const DILocation *Other) const { llvm_unreachable("Don't compare these"); }
#else
  bool operator==(const DILocation *Other) const { return Loc == Other; }
  bool operator!=(const DILocation *Other) const { return Loc != Other; }
#endif

  DebugLoc withContext(DebugLocContext Context) const;

#if LLVM_USE_FLMD_SOURCE_LOCS
  bool isSameSourceLocation(const DbgLocStorage &Other) const {
    return get().SrcLocIdx == Other.get().SrcLocIdx &&
      get().InlinedAtIdx == Other.get().InlinedAtIdx;
  }
  uint64_t getAtomGroup() const {
    return get().AtomGroup;
  }
  uint8_t getAtomRank() const {
    return get().AtomRank;
  }
  DbgLocStorage getWithoutAtom() const {
    FLDebugLoc Copy = get();
    Copy.AtomGroup = 0;
    Copy.AtomRank = 0;
    return Copy;
  }
  DbgLocStorage getWithAtom(uint16_t Group, uint16_t Rank) const {
    FLDebugLoc Copy = get();
    Copy.AtomGroup = Group;
    Copy.AtomRank = Rank;
    return Copy;
  }
  // bool isLineZero() const {
  //   return get().SrcLocIdx.get() == 0;
  // }
  // DbgLocStorage getWithLineZero() const {
  //   FLDebugLoc DL = get();
  //   DL.SrcLocIdx = 0;
  //   return DL;
  // }
  bool isInlinedAt(DbgLocStorage &Other) const {
    assert(Other.get().isInlinedCall() &&
      "Cannot be inlined at a non-inlined-call!");
    return get().InlinedAtIdx == Other.get().InlinedAtIdx;
  }
  /// Returns the InlinedAt value for an InstrLoc only, meaning this will assert
  /// if called on an InlinedCallLoc (meaning
  /// `Instr->getInstrInlinedAt().getInstrInlinedAt()` will *always* fail.)
  DbgLocStorage getInstrInlinedAt() const {
    assert(get().SrcLocIdx &&
      "getInstrInlinedAt called for a non-instruction loc!");
    return FLDebugLoc({}, get().InlinedAtIdx);
  }
#else
  bool isSameSourceLocation(const DbgLocStorage &Other) const;
  uint64_t getAtomGroup() const;
  uint8_t getAtomRank() const;
  // bool isLineZero() const {
  //   return get()->getLine() == 0;
  // }
  // DbgLocStorage getWithLineZero() const {
  //   return DILocation::get(get()->getContext(), 0, 0,
  //                          get()->getScope(),
  //                          get()->getInlinedAt());
  // }
  bool isInlinedAt(DbgLocStorage &Other) const;
  DbgLocStorage getInstrInlinedAt() const;
  DbgLocStorage getWithoutAtom() const;
  DbgLocStorage getWithAtom(uint16_t Group, uint16_t Rank) const;
#endif

////////////////////////////////////////////////////////////////////////////////
/// Coverage + Origin Tracking Features

#if LLVM_ENABLE_DEBUGLOC_TRACKING_COVERAGE
  DbgLocStorage(DebugLocKind Kind) : DbgLocCoverageTracking(Kind), Loc() {}
  DebugLocKind getKind() const { return Kind; }

  static inline DbgLocStorage getTemporary() {
    return DbgLocStorage(DebugLocKind::Temporary);
  }
  static inline DbgLocStorage getUnknown() {
    return DbgLocStorage(DebugLocKind::Unknown);
  }
  static inline DbgLocStorage getCompilerGenerated() {
    return DbgLocStorage(DebugLocKind::CompilerGenerated);
  }
  static inline DbgLocStorage getDropped() {
    return DbgLocStorage(DebugLocKind::Dropped);
  }
#else
  static inline DbgLocStorage getTemporary() { return DbgLocStorage(); }
  static inline DbgLocStorage getUnknown() { return DbgLocStorage(); }
  static inline DbgLocStorage getCompilerGenerated() { return DbgLocStorage(); }
  static inline DbgLocStorage getDropped() { return DbgLocStorage(); }
#endif // LLVM_ENABLE_DEBUGLOC_TRACKING_COVERAGE

  /// If this FLDebugLoc is non-empty, returns this DebugLoc; otherwise, selects
  /// \p Other.
  /// In coverage-tracking builds, this also accounts for whether this or
  /// \p Other have an annotative DebugLocKind applied, such that if both are
  /// empty but exactly one has an annotation, we prefer that annotated
  /// location.
  DbgLocStorage orElse(DbgLocStorage &Other) const {
    if (*this)
      return *this;
#if LLVM_ENABLE_DEBUGLOC_TRACKING_COVERAGE
    if (Other)
      return Other;
    if (getKind() != DebugLocKind::Normal)
      return *this;
    if (Other.getKind() != DebugLocKind::Normal)
      return Other;
    return *this;
#else
    return Other;
#endif // LLVM_ENABLE_DEBUGLOC_TRACKING_COVERAGE
  }

#if LLVM_ENABLE_DEBUGLOC_TRACKING_ORIGIN
  const DbgLocOrigin::StackTracesTy &getOriginStackTraces() const {
    return static_cast<DbgLocOrigin*>(this)->getOriginStackTraces();
  }
  DbgLocStorage getCopied() const {
    DbgLocStorage NewDL = *this;
    NewDL.addTrace();
    return NewDL;
  }
#else
  DbgLocStorage getCopied() const { return *this; }
#endif
/// End of Coverage + Origin Tracking Features
////////////////////////////////////////////////////////////////////////////////
};

class DebugLocMap;

/// A debug info location.
///
/// This class is a wrapper around an \a DILocation
/// pointer.
///
/// To avoid extra includes, \a DebugLoc doubles the \a DILocation API with a
/// one based on relatively opaque \a MDNode pointers.
class DebugLoc {
  DbgLocStorage Storage;
#if LLVM_USE_FLMD_SOURCE_LOCS
  DIFunctionLocalMetadata *FLContext = nullptr;
#endif
public:
  friend struct DenseMapInfo<DebugLoc>;
  friend struct DenseMapInfo<const DebugLoc>;
  friend hash_code hash_value(const DebugLoc &Val);

#if LLVM_USE_FLMD_SOURCE_LOCS
  DebugLoc(DbgLocStorage Loc, DIFunctionLocalMetadata *FLContext)
    : Storage(Loc), FLContext(FLContext) {}
  DebugLoc(DbgLocStorage Loc, DebugLocContext DLContext)
    : Storage(Loc), FLContext(DLContext.Context) {
    assert((!Storage || FLContext) && "Invalid context provided for non-empty Storage.");
  }
#else
  DebugLoc(DbgLocStorage Loc) : Storage(Loc) {}
  DebugLoc(DbgLocStorage Loc, DebugLocContext DLContext)
    : Storage(Loc) {}
#endif

  DebugLoc() : Storage() {}
  DebugLoc(std::nullptr_t) : Storage() {}
#if LLVM_USE_FLMD_SOURCE_LOCS
  LLVM_DEPRECATED("Implicit conversion disabled", "getFromDILocation")
  DebugLoc(const DILocation *L) {
    *this = DebugLoc::getFromDILocation(L);
  }
#else
  /// Construct from an \a DILocation.
  LLVM_DEPRECATED("Implicit conversion disabled", "getFromDILocation")
  DebugLoc(const DILocation *L) : Storage(const_cast<DILocation *>(L)) {}
#endif

DbgLocStorage getStorage() const { return Storage; }
operator DbgLocStorage() const { return Storage; }
  // Gets the underlying storage type in a DebugLoc, either a DILocation* or an
  // FLDebugLoc depending on the LLVM_USE_FLMD_SOURCE_LOCS flag. Because these
  // types have incompatible interfaces, this should only be called where the
  // status of the flag is known, i.e. in conditionally-compiled code.
  DbgLocStorage::LocType getUnderlyingStorage() const { return Storage.get(); }
#if LLVM_USE_FLMD_SOURCE_LOCS
  DIFunctionLocalMetadata *getFLContext() const { return FLContext; }
  // Several methods from FLDebugLoc are not relevant or possible for
  // DILocations, but are useful to be callable from a DebugLoc.
  bool isInlinedInstrLoc() const {
    return getUnderlyingStorage().isInlinedInstrLoc();
  }
  bool isNonInlinedInstrLoc() const {
    return getUnderlyingStorage().isNonInlinedInstrLoc();
  }
  bool isInstrLoc() const {
    return getUnderlyingStorage().isInstrLoc();
  }
  bool isInlinedCall() const {
    return getUnderlyingStorage().isInlinedCall();
  }
  DebugLoc getLocForInlinedCall() const {
    return DebugLoc(
      getUnderlyingStorage().getLocForInlinedCall(FLContext), FLContext);
  }
  FLIndex<uint16_t> getIdxForInlinedCall() const {
    return getUnderlyingStorage().getIdxForInlinedCall();
  }

  FLInlinedCall getAsInlinedCall() const {
    return getUnderlyingStorage().getAsInlinedCall(FLContext);
  }
  FLIndex<uint16_t> getInlinedAtIdx() const {
    return getUnderlyingStorage().getInlinedAtIdx(FLContext);
  }
  FLSrcLoc getSrcLoc() const {
    return getUnderlyingStorage().getSrcLoc(FLContext);
  }
  // If we know in advance we know both, this can be faster.
  std::pair<FLSrcLoc, DILocalScope *> getSrcLocAndScope() const {
    return getUnderlyingStorage().getSrcLocAndScope(FLContext);
  }
  std::pair<FLDebugLoc, DIFunctionLocalMetadata *> getAsFLDebugLoc() const;
  static DebugLoc getFromFLDebugLoc(FLDebugLoc FLDL, DIFunctionLocalMetadata *FLContext);
#endif


////////////////////////////////////////////////////////////////////////////////
/// Metadata/DILocation compatibility interface

  bool operator==(std::nullptr_t) const { return !Storage; }
  bool operator!=(std::nullptr_t) const { return (bool)Storage; }
  bool operator==(DILocation *RHS) const { return this->Storage == RHS; }
  bool operator!=(DILocation *RHS) const { return this->Storage != RHS; }
  bool operator==(const DILocation *RHS) const { return this->Storage == RHS; }
  bool operator!=(const DILocation *RHS) const { return this->Storage != RHS; }
  friend bool operator==(std::nullptr_t, const DebugLoc &RHS) { return !RHS; }
  friend bool operator!=(std::nullptr_t, const DebugLoc &RHS) { return (bool)RHS; }
  friend bool operator==(DILocation *LHS, const DebugLoc &RHS) { return RHS.Storage == LHS; }
  friend bool operator!=(DILocation *LHS, const DebugLoc &RHS) { return RHS.Storage != LHS; }
  friend bool operator==(const DILocation *LHS, const DebugLoc &RHS) { return RHS.Storage == LHS; }
  friend bool operator!=(const DILocation *LHS, const DebugLoc &RHS) { return RHS.Storage != LHS; }

  bool operator<(const DebugLoc &Other) const { return Storage < Other.Storage; }

private:
  /// The following set of functions are the "real" functions that we use to
  /// create a DebugLoc; all require a "context" object, which is the
  /// LLVMContext for non-FLMD DebugLocs, and DIFunctionLocalMetadata* for FLMD
  /// DebugLocs. To avoid compile errors when toggling the FLMD DebugLocs
  /// feature, passing the context objects directly is not enabled; instead, we
  /// pass an object which can be used to locate either context object depending
  /// on the feature toggle, using a wrapper `DebugLocContext` class. An
  /// explicit `DebugLocContext(<ActualContext>)` is also available, but should
  /// only be used in cases where the feature toggle is known (and will
  /// otherwise not compile).
#if LLVM_USE_FLMD_SOURCE_LOCS
  static DebugLoc get(
    DIFunctionLocalMetadata *Context, unsigned Line, unsigned Column, Metadata *Scope,
    DebugLoc InlinedAt = DebugLoc(), bool ImplicitCode = false, uint64_t AtomGroup = 0,
    uint8_t AtomRank = 0);
  static DebugLoc getDistinctInlinedCall(
    DIFunctionLocalMetadata *Context, unsigned Line, unsigned Column, Metadata *Scope,
    DebugLoc InlinedAt = DebugLoc(), bool ImplicitCode = false,
    uint64_t MaxAtomGroup = 0);
  static DebugLoc getUniquedInlinedCall(
    DIFunctionLocalMetadata *Context, unsigned Line, unsigned Column, Metadata *Scope,
    DebugLoc InlinedAt = DebugLoc(), bool ImplicitCode = false,
    uint64_t MaxAtomGroup = 0);
#else
  static DebugLoc get(
    LLVMContext &Context, unsigned Line, unsigned Column, Metadata *Scope,
    DebugLoc InlinedAt = DebugLoc(), bool ImplicitCode = false, uint64_t AtomGroup = 0,
    uint8_t AtomRank = 0);
  static DebugLoc getDistinctInlinedCall(
    LLVMContext &Context, unsigned Line, unsigned Column, Metadata *Scope,
    DebugLoc InlinedAt = DebugLoc(), bool ImplicitCode = false, uint64_t AtomGroup = 0,
    uint8_t AtomRank = 0);
  static DebugLoc getUniquedInlinedCall(
    LLVMContext &Context, unsigned Line, unsigned Column, Metadata *Scope,
    DebugLoc InlinedAt = DebugLoc(), bool ImplicitCode = false, uint64_t AtomGroup = 0,
    uint8_t AtomRank = 0);
#endif
public:

  /// Helper class used to fetch the required context object to create a new
  /// DebugLoc from any of the objects that can reach it (see comment above the
  /// private functions above).
  using DebugLocContext = llvm::DebugLocContext;
  static DebugLoc get(
    DebugLocContext Context, unsigned Line, unsigned Column, Metadata *Scope,
    DebugLoc InlinedAt = DebugLoc(), bool ImplicitCode = false, uint64_t AtomGroup = 0,
    uint8_t AtomRank = 0) {
    return DebugLoc::get(Context.Context, Line, Column, Scope, InlinedAt, ImplicitCode, AtomGroup, AtomRank);
  }
  static DebugLoc getDistinctInlinedCall(
    DebugLocContext Context, unsigned Line, unsigned Column, Metadata *Scope,
    DebugLoc InlinedAt = DebugLoc(), bool ImplicitCode = false, uint64_t MaxAtomGroup = 0) {
    return DebugLoc::getDistinctInlinedCall(Context.Context, Line, Column, Scope, InlinedAt, ImplicitCode, MaxAtomGroup);
  }
  static DebugLoc getUniquedInlinedCall(
    DebugLocContext Context, unsigned Line, unsigned Column, Metadata *Scope,
    DebugLoc InlinedAt = DebugLoc(), bool ImplicitCode = false, uint16_t MaxAtomGroup = 0) {
    return DebugLoc::getUniquedInlinedCall(Context.Context, Line, Column, Scope, InlinedAt, ImplicitCode, MaxAtomGroup);
  }
#if LLVM_USE_FLMD_SOURCE_LOCS
  DebugLocContext getDLContext() const {
    assert((bool)*this && "Can only get DL Context from a valid DebugLoc.");
    return DebugLocContext(getFLContext());
  }
#else
  DebugLocContext getDLContext() const;
#endif

  /// Creates a DebugLoc representing the same location as this DebugLoc, but
  /// as an inlined call.
  DebugLoc convertToInlinedCall() const;
  /// FIXME: Just replace this with `getAsDILocation` later.
  DILocation *convertToDILocation() const;

  static DebugLoc getFromMDNode(const MDNode *L);
  /// Create a DebugLoc from the equivalent DILocation. If the DILocation is an
  /// inlined call location, the inlined DISubprogram must also be provided.
  static DebugLoc getFromDILocation(const DILocation *L, DISubprogram *InlinedSP = nullptr);
  DILocation *getAsDILocation() const;

  /// Return \c this as a bar \a MDNode.
  LLVM_ABI MDNode *getAsMDNode() const;

  /// Get the underlying \a DILocation.
  ///
  /// \pre !*this or \c isa<DILocation>(getAsMDNode()).
  /// @{
  LLVM_DEPRECATED("Implicit conversion disabled", "getAsDILocation")
  DILocation *get() const;
  LLVM_DEPRECATED("Implicit conversion disabled", "getAsDILocation")
  operator DILocation *() const;
  LLVM_DEPRECATED("Implicit conversion disabled", "getAsDILocation")
  DILocation *operator->() const;
  LLVM_DEPRECATED("Implicit conversion disabled", "getAsDILocation")
  DILocation &operator*() const;
  /// @}

////////////////////////////////////////////////////////////////////////////////
/// Static source location methods

  /// When two instructions are combined into a single instruction we also
  /// need to combine the original locations into a single location.
  /// When the locations are the same we can use either location.
  /// When they differ, we need a third location which is distinct from
  /// either. If they share a common scope, use this scope and compare the
  /// line/column pair of the locations with the common scope:
  /// * if both match, keep the line and column;
  /// * if only the line number matches, keep the line and set the column as
  /// 0;
  /// * otherwise set line and column as 0.
  /// If they do not share a common scope the location is ambiguous and can't
  /// be represented in a line entry. In this case, set line and column as 0
  /// and use the scope of any location.
  ///
  /// \p LocA \p LocB: The locations to be merged.
  LLVM_ABI static DebugLoc getMergedLocation(DebugLoc LocA, DebugLoc LocB);
  LLVM_ABI static DebugLoc getMergedLocation(DbgLocStorage LocA, DbgLocStorage LocB, DebugLocContext Context);

  /// Try to combine the vector of locations passed as input in a single one.
  /// This function applies getMergedLocation() repeatedly left-to-right.
  ///
  /// \p Locs: The locations to be merged.
  LLVM_ABI static DebugLoc getMergedLocations(ArrayRef<DebugLoc> Locs);
  LLVM_ABI static DebugLoc getMergedLocations(ArrayRef<DbgLocStorage> Locs, DebugLocContext Context);

  enum { ReplaceLastInlinedAt = true };
  /// Rebuild the entire inlined-at chain for this instruction so that the top
  /// of the chain now is inlined-at the new call site.
  /// \param   InlinedAt The new outermost inlined-at in the chain.
  LLVM_ABI static DebugLoc
  appendInlinedAt(const DebugLoc &DL, DebugLoc InlinedAt, LLVMContext &Ctx,
                  DenseMap<const MDNode *, MDNode *> &Cache,
                  DebugLocMap &DLMap);

  /// Rebuild the entire inline-at chain by replacing the subprogram at the
  /// end of the chain with NewSP.
  /// The DLMap argument is only used when FLMD source locations are enabled.
  LLVM_ABI static DebugLoc
  replaceInlinedAtSubprogram(const DebugLoc &DL, DISubprogram &NewSP,
                             DebugLocContext NewFnContext,
                             DenseMap<const MDNode *, MDNode *> &Cache,
                             DebugLocMap &DLMap);

  static bool isPseudoProbeDiscriminator(unsigned Discriminator);

  /// Return the masked discriminator value for an input discrimnator value D
  /// (i.e. zero out the (B+1)-th and above bits for D (B is 0-base).
  // Example: an input of (0x1FF, 7) returns 0xFF.
  static unsigned getMaskedDiscriminator(unsigned D, unsigned B) {
    return (D & getN1Bits(B));
  }

  /// Return the bits used for base discriminators.
  static unsigned getBaseDiscriminatorBits() { return getBaseFSBitEnd(); }

  /// Returns the base discriminator for a given encoded discriminator \p D.
  static unsigned
  getBaseDiscriminatorFromDiscriminator(unsigned D,
                                        bool IsFSDiscriminator = false) {
    // Extract the dwarf base discriminator if it's encoded in the pseudo probe
    // discriminator.
    if (isPseudoProbeDiscriminator(D)) {
      auto DwarfBaseDiscriminator =
          PseudoProbeDwarfDiscriminator::extractDwarfBaseDiscriminator(D);
      if (DwarfBaseDiscriminator)
        return *DwarfBaseDiscriminator;
      // Return the probe id instead of zero for a pseudo probe discriminator.
      // This should help differenciate callsites with same line numbers to
      // achieve a decent AutoFDO profile under -fpseudo-probe-for-profiling,
      // where the original callsite dwarf discriminator is overwritten by
      // callsite probe information.
      return PseudoProbeDwarfDiscriminator::extractProbeIndex(D);
    }

    if (IsFSDiscriminator)
      return getMaskedDiscriminator(D, getBaseDiscriminatorBits());
    return getUnsignedFromPrefixEncoding(D);
  }

  /// Raw encoding of the discriminator. APIs such as cloneWithDuplicationFactor
  /// have certain special case behavior (e.g. treating empty duplication factor
  /// as the value '1').
  /// This API, in conjunction with cloneWithDiscriminator, may be used to
  /// encode the raw values provided.
  ///
  /// \p BD: base discriminator
  /// \p DF: duplication factor
  /// \p CI: copy index
  ///
  /// The return is std::nullopt if the values cannot be encoded in 32 bits -
  /// for example, values for BD or DF larger than 12 bits. Otherwise, the
  /// return is the encoded value.
  LLVM_ABI static std::optional<unsigned>
  encodeDiscriminator(unsigned BD, unsigned DF, unsigned CI);

  /// Raw decoder for values in an encoded discriminator D.
  LLVM_ABI static void decodeDiscriminator(unsigned D, unsigned &BD,
                                           unsigned &DF, unsigned &CI);

  /// Returns the duplication factor for a given encoded discriminator \p D, or
  /// 1 if no value or 0 is encoded.
  static unsigned getDuplicationFactorFromDiscriminator(unsigned D) {
    if (EnableFSDiscriminator)
      return 1;
    D = getNextComponentInDiscriminator(D);
    unsigned Ret = getUnsignedFromPrefixEncoding(D);
    if (Ret == 0)
      return 1;
    return Ret;
  }

  /// Returns the copy identifier for a given encoded discriminator \p D.
  static unsigned getCopyIdentifierFromDiscriminator(unsigned D) {
    return getUnsignedFromPrefixEncoding(
        getNextComponentInDiscriminator(getNextComponentInDiscriminator(D)));
  }

////////////////////////////////////////////////////////////////////////////////
/// Coverage + Origin Tracking Features

#if LLVM_ENABLE_DEBUGLOC_TRACKING_COVERAGE
  DebugLoc(DebugLocKind Kind) : Storage(Kind) {}
  DebugLocKind getKind() const { return Storage.Kind; }
#endif

#if LLVM_ENABLE_DEBUGLOC_TRACKING_COVERAGE
  static inline DebugLoc getTemporary() {
    return DebugLoc(DebugLocKind::Temporary);
  }
  static inline DebugLoc getUnknown() {
    return DebugLoc(DebugLocKind::Unknown);
  }
  static inline DebugLoc getCompilerGenerated() {
    return DebugLoc(DebugLocKind::CompilerGenerated);
  }
  static inline DebugLoc getDropped() {
    return DebugLoc(DebugLocKind::Dropped);
  }
#else
  static inline DebugLoc getTemporary() { return DebugLoc(); }
  static inline DebugLoc getUnknown() { return DebugLoc(); }
  static inline DebugLoc getCompilerGenerated() { return DebugLoc(); }
  static inline DebugLoc getDropped() { return DebugLoc(); }
#endif // LLVM_ENABLE_DEBUGLOC_TRACKING_COVERAGE

  /// If this DebugLoc is non-empty, returns this DebugLoc; otherwise, selects
  /// \p Other.
  /// In coverage-tracking builds, this also accounts for whether this or
  /// \p Other have an annotative DebugLocKind applied, such that if both are
  /// empty but exactly one has an annotation, we prefer that annotated
  /// location.
  DebugLoc orElse(DebugLoc Other) const {
    if (*this)
      return *this;
#if LLVM_ENABLE_DEBUGLOC_TRACKING_COVERAGE
    if (Other)
      return Other;
    if (getKind() != DebugLocKind::Normal)
      return *this;
    if (Other.getKind() != DebugLocKind::Normal)
      return Other;
    return *this;
#else
    return Other;
#endif // LLVM_ENABLE_DEBUGLOC_TRACKING_COVERAGE
  }

#if LLVM_ENABLE_DEBUGLOC_TRACKING_ORIGIN
  DebugLoc getCopied() const {
    DebugLoc NewDL = *this;
    NewDL.addTrace();
    return NewDL;
  }
#else
  DebugLoc getCopied() const { return *this; }
#endif

////////////////////////////////////////////////////////////////////////////////
/// LocType-dependent methods

  /// Check for null.
  ///
  /// Check for null in a way that is safe with broken debug info.  Unlike
  /// the conversion to \c DILocation, this doesn't require that \c Loc is of
  /// the right type.  Important for cases like \a llvm::StripDebugInfo() and
  /// \a Instruction::hasMetadata().
  explicit operator bool() const { return (bool)Storage; }

  /// Return true if the source locations match, ignoring isImplicitCode and
  /// source atom info.
  bool isSameSourceLocation(const DebugLoc &Other) const {
    if (Storage == Other.Storage)
      return true;
    return ((bool)*this == (bool)Other) && getLine() == Other.getLine() &&
           getCol() == Other.getCol() && getScope() == Other.getScope() &&
           getInlinedAt() == Other.getInlinedAt();
  }

  LLVM_ABI unsigned getLine() const;
  LLVM_ABI unsigned getCol() const;
  LLVM_ABI unsigned getColumn() const { return getCol(); }
  LLVM_ABI DILocalScope *getScope() const;
  LLVM_ABI DebugLoc getInlinedAt() const;

  /// Get the fully inlined-at scope for a DebugLoc.
  ///
  /// Gets the inlined-at scope for a DebugLoc.
  LLVM_ABI DILocalScope *getInlinedAtScope() const;

  /// Find the debug info location for the start of the function.
  ///
  /// Walk up the scope chain of given debug loc and find line number info
  /// for the function.
  ///
  /// FIXME: Remove this.  Users should use DILocation/DILocalScope API to
  /// find the subprogram, and then DILocation::get().
  LLVM_ABI DebugLoc getFnDebugLoc() const;

  /// Check if the DebugLoc corresponds to an implicit code.
  LLVM_ABI bool isImplicitCode() const;
  LLVM_ABI void setImplicitCode(bool ImplicitCode);

#if LLVM_USE_FLMD_SOURCE_LOCS
  bool operator==(const DebugLoc &DL) const {
    return (!Storage && !DL.Storage) || (Storage == DL.Storage && FLContext == DL.FLContext);
  }
  bool operator!=(const DebugLoc &DL) const {
    if (!Storage && !DL.Storage) return false;
    return Storage != DL.Storage || FLContext != DL.FLContext;
  }
#else
  bool operator==(const DebugLoc &DL) const { return Storage == DL.Storage; }
  bool operator!=(const DebugLoc &DL) const { return Storage != DL.Storage; }
#endif
  LLVM_ABI void dump() const;
  LLVM_ABI void dump(const Module *M) const;

  /// prints source location /path/to/file.exe:line:col @[inlined at]
  LLVM_ABI void print(raw_ostream &OS) const;

  LLVM_ABI void print(raw_ostream &OS, const Module *M,
                      bool IsForDebug = false) const;
  LLVM_ABI void print(raw_ostream &OS, ModuleSlotTracker &MST,
                      const Module *M = nullptr, bool IsForDebug = false) const;
  LLVM_ABI void printAsOperand(raw_ostream &OS,
                               const Module *M = nullptr) const;
  LLVM_ABI void printAsOperand(raw_ostream &OS, ModuleSlotTracker &MST,
                               const Module *M = nullptr) const;
  bool isDistinct() const;
  
  LLVMContext &getContext() const;

  uint64_t getAtomGroup() const;
  uint8_t getAtomRank() const;

  DebugLoc getWithoutAtom() const;
  DebugLoc getWithAtom(uint16_t Group, uint16_t Rank) const;
  /// Creates and returns a new atom group number. If this is an instruction
  /// loc, then the resulting atom group is one that can be applied to this
  /// DebugLoc, and any other DebugLocs within the same "atom context", meaning
  /// the combination of the current Function/InlinedAt (i.e. the tuple given
  /// below).
  /// If this is an inlined call DebugLoc, then returns a new atom group number
  /// for instruction locs inlined directly at this DebugLoc. Finally, if this
  /// is an empty DebugLoc, it returns a new atom group for non-inlined
  /// instructions in the current function.
  /// TODO: It may be worth defining clearly which functions are valid on empty
  /// DebugLocs and which are not - with DILocations the answer is "all of them"
  /// since they are a nullptr, but some methods like this may have actual
  /// meaning for FLMD DebugLocs, e.g. for a non-inlined instruction loc `DL`,
  /// `DL->getInlinedAt()->getNewAtomGroup()` is a totally valid construct which
  /// returns the correct result even though `DL->getInlinedAt()` is an empty
  /// DebugLoc.
  uint16_t getNewAtomGroup() const;
  /// Returns a DebugLoc that uniquely represents this DebugLoc's
  /// "atom context". The tuple of (AtomContext, AtomGroup) uniquely identify an
  /// atom group, i.e. two DebugLocs may have the same AtomGroup number but have
  /// different actual atom groups if they differ in AtomContext.
  DebugLoc getAtomContext() const;

  /// Return the linkage name of Subprogram. If the linkage name is empty,
  /// return scope name (the demangled name).
  StringRef getSubprogramLinkageName() const;

  DIFile *getFile() const;
  StringRef getFilename() const;
  StringRef getDirectory() const;
  std::optional<StringRef> getSource() const;

  DebugLoc getInlinedAtLocation() const;

  unsigned getDiscriminator() const;

  /// Returns a new DebugLoc with updated \p Discriminator.
  DebugLoc cloneWithDiscriminator(unsigned Discriminator) const;

  /// Returns a new DebugLoc with updated base discriminator \p BD. Only the
  /// base discriminator is set in the new DebugLoc, the other encoded values
  /// are elided.
  /// If the discriminator cannot be encoded, the function returns std::nullopt.
  std::optional<DebugLoc>
  cloneWithBaseDiscriminator(unsigned BD) const;

  /// Returns the duplication factor stored in the discriminator, or 1 if no
  /// duplication factor (or 0) is encoded.
  unsigned getDuplicationFactor() const;

  /// Returns the copy identifier stored in the discriminator.
  unsigned getCopyIdentifier() const;

  /// Returns the base discriminator stored in the discriminator.
  unsigned getBaseDiscriminator() const;

  /// Returns a new DebugLoc with duplication factor \p DF * current
  /// duplication factor encoded in the discriminator. The current duplication
  /// factor is as defined by getDuplicationFactor().
  /// Returns std::nullopt if encoding failed.
  std::optional<DebugLoc>
  cloneByMultiplyingDuplicationFactor(unsigned DF) const;

  Metadata *getRawScope() const;
  Metadata *getRawInlinedAt() const;
};

inline raw_ostream &operator<<(raw_ostream &OS, const DebugLoc &DL) {
  DL.print(OS);
  return OS;
}
raw_ostream &operator<<(raw_ostream &OS, const FLDebugLoc &DL);

#if LLVM_USE_FLMD_SOURCE_LOCS
// Maps indices of one FLMD context to another, e.g. when inlining or moving
// code between functions. SrcLocs and Scopes are copied into the destination
// context on first use, so each source entry is copied at most once per map.
// InlinedCalls are mapped explicitly by the user via insertInlinedCall.
class DebugLocMap {
  // Default initialize the SmallVectors with empty elements (which are all 0s).
  SmallVector<FLIndex<uint32_t>> SrcLocMap;
  SmallVector<FLIndex<uint16_t>> ScopeMap;
  SmallVector<FLIndex<uint16_t>> InlinedCallMap;
  DIFunctionLocalMetadata *SrcContext;
  DIFunctionLocalMetadata *DestContext;
public:
  DebugLocMap(Function *SrcFn, Function *DestFn);
  DebugLocMap(DIFunctionLocalMetadata *SrcContext,
              DIFunctionLocalMetadata *DestContext);

  DIFunctionLocalMetadata *getDestContext() const { return DestContext; }

  FLIndex<uint16_t> mapScope(FLIndex<uint16_t> In);
  FLIndex<uint32_t> mapSrcLoc(FLIndex<uint32_t> In) {
    if (!In)
      return In;
    FLIndex<uint32_t> &Out = SrcLocMap[In.get()];
    if (!Out) {
      FLSrcLoc SrcLoc = SrcContext->getSrcLoc(In);
      SrcLoc.ScopeIdx = mapScope(SrcLoc.ScopeIdx);
      DestContext->SrcLocs.push_back(SrcLoc);
      Out = DestContext->SrcLocs.size() - 1;
    }
    return Out;
  }

  FLIndex<uint32_t> getSrcLoc(FLIndex<uint32_t> In) {
    if (!In)
      return In;
    return SrcLocMap[In.get()];
  }
  void insertSrcLoc(FLIndex<uint32_t> In, FLIndex<uint32_t> Out) {
    assert(In && !SrcLocMap[In.get()] && "Must map existing unmapped index only.");
    SrcLocMap[In.get()] = Out;
  }

  FLIndex<uint16_t> getInlinedCall(FLIndex<uint16_t> In) {
    if (!In)
      return In;
    return InlinedCallMap[In.get()];
  }
  void insertInlinedCall(FLIndex<uint16_t> In, FLIndex<uint16_t> Out) {
    assert(In && !InlinedCallMap[In.get()] && "Must map existing unmapped index only.");
    InlinedCallMap[In.get()] = Out;
  }

  // Returns the mapped location if the non-inlined SrcLoc, or the InlinedAt
  // chain, of In has been mapped already; an inlined SrcLoc is then copied.
  FLDebugLoc getFLDebugLoc(FLDebugLoc In) {
    if (!In)
      return In;
    if (In.isNonInlinedInstrLoc()) {
      if (auto Found = getSrcLoc(In.SrcLocIdx))
        return FLDebugLoc(Found, {}, In.AtomGroup, In.AtomRank);
      return {};
    }
    if (auto Found = getInlinedCall(In.InlinedAtIdx))
      return FLDebugLoc(mapSrcLoc(In.SrcLocIdx), Found, In.AtomGroup,
                        In.AtomRank);
    return {};
  }

  DebugLoc getDebugLoc(DebugLoc In) {
    assert(In.getFLContext() == SrcContext && "Mapping DebugLoc from the wrong function.");
    return DebugLoc(getFLDebugLoc(In.getStorage().get()), DestContext);
  }
};
#else
class DebugLocMap {
public:
  DebugLocMap(Function *SrcFn, Function *DestFn) {}
};
#endif


inline DebugLocContext::DebugLocContext(DebugLoc DL) {
  // assert(DL && "DebugLocContext can only be obtained from a non-empty DebugLoc.");
  Context = DL.getDLContext().Context;
}
inline DebugLoc DbgLocStorage::withContext(DebugLocContext Context) const {
  return DebugLoc(*this, Context);
}

template <>
struct DenseMapInfo<DebugLoc> {
  static unsigned getHashValue(DebugLoc DL) {
    return hash_value(DL);
  }

  static bool isEqual(DebugLoc LHS, DebugLoc RHS) { return LHS == RHS; }
};

inline hash_code hash_value(const FLDebugLoc &Val) {
  return hash_value(Val.asRawInt());
}
template <>
struct DenseMapInfo<FLDebugLoc> {
  static unsigned getHashValue(FLDebugLoc DL) {
    return hash_value(DL);
  }

  static bool isEqual(FLDebugLoc LHS, FLDebugLoc RHS) { return LHS.asRawInt() == RHS.asRawInt(); }
};

inline hash_code hash_value(const DbgLocStorage &Val) {
  return hash_value(Val.get());
}
template <>
struct DenseMapInfo<DbgLocStorage> {
  static unsigned getHashValue(DbgLocStorage DL) {
    return hash_value(DL);
  }

  static bool isEqual(DbgLocStorage LHS, DbgLocStorage RHS) { return LHS == RHS; }
};


inline hash_code hash_value(const DebugLoc &Val) {
#if LLVM_USE_FLMD_SOURCE_LOCS
  if (Val)
    return hash_combine(Val.getStorage(), Val.getFLContext());
#endif
  return hash_value(Val.getStorage());
}


} // end namespace llvm

namespace std {
template<> struct hash<llvm::DebugLoc> {
  size_t operator()(const llvm::DebugLoc &Val) const {
    return llvm::hash_value(Val);
  }
};
template<> struct hash<llvm::DbgLocStorage> {
  size_t operator()(const llvm::DbgLocStorage &Val) const {
    return llvm::hash_value(Val);
  }
};
} // end namespace std

#endif // LLVM_IR_DEBUGLOC_H
