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

#ifndef LLVM_IR_FUNCTIONLOCALMETADATA_H
#define LLVM_IR_FUNCTIONLOCALMETADATA_H

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/Hashing.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Config/llvm-config.h"
#include "llvm/IR/Metadata.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/DataTypes.h"
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace llvm {

class DILocalScope;
class DIFunctionLocalMetadata;

/// Small wrapper class for an FLMD index, which may be "no index".
/// Should be checked to see if value is present before using, otherwise the
/// implicit/explicit index value will be invalid.
/// Internally uses 0 as the "no index" value, and stores the internal index value as the
/// actual index + 1, subtracting to get the real index.
template <typename IndexType>
class FLIndex {
  friend struct DenseMapInfo<FLIndex<IndexType>>;
  IndexType Index;
public:
  FLIndex() : Index(0) {}
  FLIndex(IndexType InIndex) : Index(InIndex + 1) {
    assert(InIndex < std::numeric_limits<IndexType>::max() && "Hit maximum index limit!");
  }
  operator bool() const {
      return Index;
  }
  IndexType get() const {
      return Index - 1;
  }
  operator IndexType() const {
      return get();
  }
  bool operator==(const FLIndex &Other) const { return Index == Other.Index; }
  bool operator!=(const FLIndex &Other) const { return Index != Other.Index; }

  IndexType asRaw() const {
    return Index;
  }
  static FLIndex fromRaw(IndexType RawIndex) {
    FLIndex Index;
    Index.Index = RawIndex;
    return Index;
  }

  friend inline hash_code hash_value(const FLIndex<IndexType> &Idx) {
    return hash_value(Idx.Index);
  }
};

template <typename IndexType>
struct DenseMapInfo<FLIndex<IndexType>> {
  static unsigned getHashValue(FLIndex<IndexType> DL) {
    return hash_value(DL.Index);
  }

  static bool isEqual(FLIndex<IndexType> LHS, FLIndex<IndexType> RHS) {
    return LHS.Index == RHS.Index;
  }
};

/// A call site that code has been inlined at. Whether the site is uniqued or
/// distinct is recorded by the context's uniquing index, not in the entry.
struct FLInlinedCall {
  FLIndex<uint32_t> SrcLocIdx;
  FLIndex<uint32_t> InlinedAtIdx;
  FLInlinedCall() = default;
  FLInlinedCall(FLIndex<uint32_t> SrcLocIdx, FLIndex<uint32_t> InlinedAtIdx)
      : SrcLocIdx(SrcLocIdx), InlinedAtIdx(InlinedAtIdx) {}
  uint64_t asRawInt() const {
    return (uint64_t)SrcLocIdx.asRaw() << 32 | InlinedAtIdx.asRaw();
  }
  static FLInlinedCall fromRawInt(uint64_t RawInt) {
    return FLInlinedCall(FLIndex<uint32_t>::fromRaw(RawInt >> 32),
                         FLIndex<uint32_t>::fromRaw(RawInt));
  }
  bool operator==(const FLInlinedCall &Other) const {
    return asRawInt() == Other.asRawInt();
  }
  bool operator!=(const FLInlinedCall &Other) const {
    return asRawInt() != Other.asRawInt();
  }
};

/// Stores source location information, including the source atom.
struct FLSrcLoc {
  uint32_t Line = 0;
  uint16_t Column = 0;
  uint8_t AtomRank = 0;
  uint8_t ImplicitCode = 0;
  FLIndex<uint32_t> ScopeIdx;
  uint32_t AtomGroup = 0;
  FLSrcLoc() = default;
  // A column that does not fit becomes unknown, as for DILocation.
  FLSrcLoc(uint32_t Line, unsigned Column, FLIndex<uint32_t> ScopeIdx,
           uint32_t AtomGroup = 0, uint8_t AtomRank = 0,
           bool ImplicitCode = false)
      : Line(Line), Column(Column < (1u << 16) ? Column : 0),
        AtomRank(AtomRank), ImplicitCode(ImplicitCode), ScopeIdx(ScopeIdx),
        AtomGroup(AtomGroup) {
    assert(ScopeIdx && "SrcLoc needs valid scope!");
  }
  bool operator==(const FLSrcLoc &Other) const {
    return std::memcmp(this, &Other, sizeof(*this)) == 0;
  }
  unsigned getHash() const {
    static_assert(sizeof(*this) == 16, "FLSrcLoc must have no padding");
    uint64_t Raw[2];
    std::memcpy(Raw, this, sizeof(Raw));
    return static_cast<unsigned>(size_t(hash_combine(Raw[0], Raw[1])));
  }
};

/// Unique FLMD, just a wrapper around a DILocalScope.
struct FLScope {
  MDNode *Scope;
  FLScope() = default;
  FLScope(MDNode *Scope) : Scope(Scope) {}
  FLScope(DILocalScope *Scope);
  operator DILocalScope*();
  bool operator==(const FLScope &Other) {
    return Scope == Other.Scope;
  }
  DILocalScope *get();
  operator const DILocalScope*() const;
  const DILocalScope *get() const;
};

/// Uniquing index for FLSrcLocs: each bucket holds (hash << 32 | index) so the
/// set needs no access to the table when it grows.
struct FLSrcLocIndexInfo {
  struct Key {
    const FLSrcLoc &Loc;
    ArrayRef<FLSrcLoc> Table;
    unsigned Hash;
  };
  static uint64_t getEmptyKey() { return ~0ULL; }
  static uint64_t getTombstoneKey() { return ~0ULL - 1; }
  static unsigned getHashValue(uint64_t V) { return V >> 32; }
  static unsigned getHashValue(const Key &K) { return K.Hash; }
  static bool isEqual(uint64_t LHS, uint64_t RHS) { return LHS == RHS; }
  static bool isEqual(const Key &K, uint64_t V) {
    if (V >= getTombstoneKey() || (V >> 32) != K.Hash)
      return false;
    return K.Table[uint32_t(V)] == K.Loc;
  }
};

/// Storage for all source locations in an LLVMContext. There is exactly one
/// per LLVMContext (see LLVMContext::getFLContext), shared by every function.
class DIFunctionLocalMetadata : public MDNode {
  DIFunctionLocalMetadata(LLVMContext &C, StorageType Storage) : MDNode(C, DIFunctionLocalMetadataKind, Storage, {}) {}
  ~DIFunctionLocalMetadata() = default;

  DenseMap<const MDNode *, uint32_t> ScopeIndex;
  DenseSet<uint64_t, FLSrcLocIndexInfo> SrcLocIndex;
  // Uniqued inlined calls only; distinct sites are never found by lookup.
  DenseMap<uint64_t, uint32_t> UniquedInlinedCalls;

public:
  friend class LLVMContextImpl;
  friend class MDNode;
  SmallVector<FLScope, 0> Scopes;
  SmallVector<FLSrcLoc, 0> SrcLocs;
  SmallVector<FLInlinedCall, 0> InlinedCalls;

  static DIFunctionLocalMetadata *getDistinct(LLVMContext &Context);
  static TempDIFunctionLocalMetadata getTemporary(LLVMContext &Context);
  TempDIFunctionLocalMetadata clone() const { return cloneImpl(); }
  TempDIFunctionLocalMetadata cloneImpl() const {
    llvm_unreachable("Does this make any sense?");
  }

  bool empty() const {
    return Scopes.empty() && SrcLocs.empty() && InlinedCalls.empty();
  }

  FLScope getScope(FLIndex<uint32_t> Idx) const {
    assert(Idx && Idx.get() < Scopes.size() && "Invalid scope index");
    return Scopes[Idx.get()];
  }
  FLSrcLoc getSrcLoc(FLIndex<uint32_t> Idx) const {
    assert(Idx && Idx.get() < SrcLocs.size() && "Invalid srcloc index");
    return SrcLocs[Idx.get()];
  }
  FLInlinedCall getInlinedCall(FLIndex<uint32_t> Idx) const {
    assert(Idx && Idx.get() < InlinedCalls.size() && "Invalid inlined call index");
    return InlinedCalls[Idx.get()];
  }

  FLIndex<uint32_t> getFLScopeIdx(const MDNode *Scope);
  FLIndex<uint32_t> getFLSrcLocIdx(const FLSrcLoc &SrcLoc);
  FLIndex<uint32_t> getFLSrcLocIdx(uint32_t Line, unsigned Column,
                                   const MDNode *Scope, uint32_t AtomGroup = 0,
                                   uint8_t AtomRank = 0,
                                   bool ImplicitCode = false) {
    return getFLSrcLocIdx(FLSrcLoc(Line, Column, getFLScopeIdx(Scope),
                                   AtomGroup, AtomRank, ImplicitCode));
  }
  /// Returns the existing uniqued site equal to \p InlinedCall if \p Uniquable,
  /// otherwise appends a new site.
  FLIndex<uint32_t> addInlinedCall(FLInlinedCall InlinedCall, bool Uniquable);
  bool isUniquedInlinedCall(FLIndex<uint32_t> Idx) const;

  /// Add the entries from the given positions on, which were appended to the
  /// tables directly, to the uniquing indexes. \p UniquableInlinedCalls has
  /// one element per appended inlined call.
  void indexEntries(uint32_t FirstScope, uint32_t FirstSrcLoc,
                    uint32_t FirstInlinedCall,
                    ArrayRef<bool> UniquableInlinedCalls);

  static bool classof(const Metadata *MD) {
    return MD->getMetadataID() == DIFunctionLocalMetadataKind;
  }
};

} // end namespace llvm


#endif // LLVM_IR_FUNCTIONLOCALMETADATA_H
