//===- llvm/ADT/DenseMap.h - Dense probed hash table ------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// This file defines the DenseMap class.
///
/// The hash table is a from-scratch C++ reimplementation of Jackson Allan's
/// *Verstable* algorithm, expressed under LLVM's existing DenseMap public API
/// so the rest of the project compiles unchanged.  It is quadratic-probing
/// open addressing with a parallel uint16 metadata array.  Each metadata slot
/// encodes a 4-bit hash fragment, a 1-bit "this bucket roots a chain whose
/// home is here" flag, and an 11-bit quadratic-displacement link to the next
/// bucket in the chain.  Erase is tombstone-free: the bucket is unlinked from
/// its intrusive chain (sole key / chain tail / interior key swapped with the
/// tail).  Home-bucket eviction keeps every chain rooted at its home bucket.
/// Max load factor is 0.875.
///
/// Unlike the upstream tombstone implementation, empty/tombstone sentinel keys
/// are never stored in buckets; occupancy lives entirely in the metadata
/// array, so only occupied buckets ever hold a constructed key/value.
///
/// `SmallDenseMap` keeps its API and template signature but is heap-backed:
/// the inline small-buffer optimization is not modeled by this algorithm
/// variant, so it is just an alias for `DenseMap`.
///
//===----------------------------------------------------------------------===//

#ifndef LLVM_ADT_DENSEMAP_H
#define LLVM_ADT_DENSEMAP_H

#include "llvm/ADT/ADL.h"
#include "llvm/ADT/DenseMapInfo.h"
#include "llvm/ADT/EpochTracker.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/STLForwardCompat.h"
#include "llvm/Support/AlignOf.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/MemAlloc.h"
#include "llvm/Support/ReverseIteration.h"
#include "llvm/Support/type_traits.h"
#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <iterator>
#include <new>
#include <type_traits>
#include <utility>

namespace llvm {

namespace detail {

// We extend a pair to allow users to override the bucket type with their own
// implementation without requiring two members.
template <typename KeyT, typename ValueT>
struct DenseMapPair : std::pair<KeyT, ValueT> {
  using std::pair<KeyT, ValueT>::pair;

  KeyT &getFirst() { return std::pair<KeyT, ValueT>::first; }
  const KeyT &getFirst() const { return std::pair<KeyT, ValueT>::first; }
  ValueT &getSecond() { return std::pair<KeyT, ValueT>::second; }
  const ValueT &getSecond() const { return std::pair<KeyT, ValueT>::second; }
};

// Metadata bit layout for the Verstable algorithm.
namespace densemap_vt {
using meta_t = uint16_t;
constexpr meta_t Empty = 0x0000;
constexpr meta_t FragMask = 0xF000;    // 4-bit hash fragment
constexpr meta_t HomeMask = 0x0800;    // bucket roots a chain
constexpr meta_t DispMask = 0x07FF;    // displacement link / chain end
constexpr unsigned MinBucketCount = 8; // power of two

inline meta_t fragOf(uint64_t Hash) {
  return static_cast<meta_t>((Hash >> 48) & FragMask);
}
inline size_t quadratic(meta_t Displacement) {
  size_t D = Displacement;
  return (D * D + D) / 2;
}
// A shared dummy slot so an empty (unallocated) map has a valid metadata
// pointer; it is never written and reads here are guarded by NumBuckets == 0.
inline meta_t EmptyMetadata = Empty;
} // namespace densemap_vt

} // end namespace detail

template <typename KeyT, typename ValueT,
          typename KeyInfoT = DenseMapInfo<KeyT>,
          typename Bucket = llvm::detail::DenseMapPair<KeyT, ValueT>,
          bool IsConst = false>
class DenseMapIterator;

template <typename DerivedT, typename KeyT, typename ValueT, typename KeyInfoT,
          typename BucketT>
class DenseMapBase : public DebugEpochBase {
  template <typename T>
  using const_arg_type_t = typename const_pointer_or_const_ref<T>::type;

  using meta_t = llvm::detail::densemap_vt::meta_t;

public:
  using size_type = unsigned;
  using key_type = KeyT;
  using mapped_type = ValueT;
  using value_type = BucketT;

  using iterator = DenseMapIterator<KeyT, ValueT, KeyInfoT, BucketT>;
  using const_iterator =
      DenseMapIterator<KeyT, ValueT, KeyInfoT, BucketT, true>;

  [[nodiscard]] inline iterator begin() {
    return iterator::makeBegin(getBuckets(), getMetadata(), getNumBuckets(),
                               empty(), *this);
  }
  [[nodiscard]] inline iterator end() {
    return iterator::makeEnd(getBuckets(), getMetadata(), getNumBuckets(),
                             *this);
  }
  [[nodiscard]] inline const_iterator begin() const {
    return const_iterator::makeBegin(getBuckets(), getMetadata(),
                                     getNumBuckets(), empty(), *this);
  }
  [[nodiscard]] inline const_iterator end() const {
    return const_iterator::makeEnd(getBuckets(), getMetadata(), getNumBuckets(),
                                   *this);
  }

  // Return an iterator to iterate over keys in the map.
  [[nodiscard]] inline auto keys() {
    return map_range(*this, [](const BucketT &P) { return P.getFirst(); });
  }

  // Return an iterator to iterate over values in the map.
  [[nodiscard]] inline auto values() {
    return map_range(*this, [](const BucketT &P) { return P.getSecond(); });
  }

  [[nodiscard]] inline auto keys() const {
    return map_range(*this, [](const BucketT &P) { return P.getFirst(); });
  }

  [[nodiscard]] inline auto values() const {
    return map_range(*this, [](const BucketT &P) { return P.getSecond(); });
  }

  [[nodiscard]] bool empty() const { return getNumEntries() == 0; }
  [[nodiscard]] unsigned size() const { return getNumEntries(); }

  /// Grow the densemap so that it can contain at least \p NumEntries items
  /// before resizing again.
  void reserve(size_type NumEntries) {
    auto NumBuckets = getMinBucketToReserveForEntries(NumEntries);
    incrementEpoch();
    if (NumBuckets > getNumBuckets())
      grow(NumBuckets);
  }

  void clear() {
    incrementEpoch();
    if (getNumEntries() == 0)
      return;

    // If the capacity of the array is huge, and the # elements used is small,
    // shrink the array.
    if (getNumEntries() * 4 < getNumBuckets() && getNumBuckets() > 64) {
      shrink_and_clear();
      return;
    }

    destroyAll();
    std::memset(getMetadata(), 0,
                sizeof(meta_t) * static_cast<size_t>(getNumBuckets()));
    setNumEntries(0);
    setNumTombstones(0);
  }

  void shrink_and_clear() {
    auto [Reallocate, NewNumBuckets] = derived().planShrinkAndClear();
    destroyAll();
    if (!Reallocate) {
      initEmpty();
      return;
    }
    derived().deallocateBuckets();
    initWithExactBucketCount(NewNumBuckets);
  }

  /// Return true if the specified key is in the map, false otherwise.
  [[nodiscard]] bool contains(const_arg_type_t<KeyT> Val) const {
    return doFind(Val) != nullptr;
  }

  /// Return 1 if the specified key is in the map, 0 otherwise.
  [[nodiscard]] size_type count(const_arg_type_t<KeyT> Val) const {
    return contains(Val) ? 1 : 0;
  }

  [[nodiscard]] iterator find(const_arg_type_t<KeyT> Val) {
    return find_as(Val);
  }
  [[nodiscard]] const_iterator find(const_arg_type_t<KeyT> Val) const {
    return find_as(Val);
  }

  /// Alternate version of find() which allows a different, and possibly
  /// less expensive, key type.
  /// The DenseMapInfo is responsible for supplying methods
  /// getHashValue(LookupKeyT) and isEqual(LookupKeyT, KeyT) for each key
  /// type used.
  template <class LookupKeyT>
  [[nodiscard]] iterator find_as(const LookupKeyT &Val) {
    if (BucketT *Bucket = doFind(Val))
      return makeIterator(Bucket);
    return end();
  }
  template <class LookupKeyT>
  [[nodiscard]] const_iterator find_as(const LookupKeyT &Val) const {
    if (const BucketT *Bucket = doFind(Val))
      return makeConstIterator(Bucket);
    return end();
  }

  /// Return the entry for the specified key, or a default constructed value if
  /// no such entry exists.
  [[nodiscard]] ValueT lookup(const_arg_type_t<KeyT> Val) const {
    if (const BucketT *Bucket = doFind(Val))
      return Bucket->getSecond();
    return ValueT();
  }

  // Return the entry with the specified key, or \p Default. This variant is
  // useful, because `lookup` cannot be used with non-default-constructible
  // values.
  template <typename U = std::remove_cv_t<ValueT>>
  [[nodiscard]] ValueT lookup_or(const_arg_type_t<KeyT> Val,
                                 U &&Default) const {
    if (const BucketT *Bucket = doFind(Val))
      return Bucket->getSecond();
    return Default;
  }

  /// Return the entry for the specified key, or abort if no such entry exists.
  [[nodiscard]] ValueT &at(const_arg_type_t<KeyT> Val) {
    auto Iter = this->find(std::move(Val));
    assert(Iter != this->end() && "DenseMap::at failed due to a missing key");
    return Iter->second;
  }

  /// Return the entry for the specified key, or abort if no such entry exists.
  [[nodiscard]] const ValueT &at(const_arg_type_t<KeyT> Val) const {
    auto Iter = this->find(std::move(Val));
    assert(Iter != this->end() && "DenseMap::at failed due to a missing key");
    return Iter->second;
  }

  // Inserts key,value pair into the map if the key isn't already in the map.
  // If the key is already in the map, it returns false and doesn't update the
  // value.
  std::pair<iterator, bool> insert(const std::pair<KeyT, ValueT> &KV) {
    return try_emplace_impl(KV.first, KV.second);
  }

  // Inserts key,value pair into the map if the key isn't already in the map.
  // If the key is already in the map, it returns false and doesn't update the
  // value.
  std::pair<iterator, bool> insert(std::pair<KeyT, ValueT> &&KV) {
    return try_emplace_impl(std::move(KV.first), std::move(KV.second));
  }

  // Inserts key,value pair into the map if the key isn't already in the map.
  // The value is constructed in-place if the key is not in the map, otherwise
  // it is not moved.
  template <typename... Ts>
  std::pair<iterator, bool> try_emplace(KeyT &&Key, Ts &&...Args) {
    return try_emplace_impl(std::move(Key), std::forward<Ts>(Args)...);
  }

  // Inserts key,value pair into the map if the key isn't already in the map.
  // The value is constructed in-place if the key is not in the map, otherwise
  // it is not moved.
  template <typename... Ts>
  std::pair<iterator, bool> try_emplace(const KeyT &Key, Ts &&...Args) {
    return try_emplace_impl(Key, std::forward<Ts>(Args)...);
  }

  /// Alternate version of insert() which allows a different, and possibly
  /// less expensive, key type.
  /// The DenseMapInfo is responsible for supplying methods
  /// getHashValue(LookupKeyT) and isEqual(LookupKeyT, KeyT) for each key
  /// type used.
  template <typename LookupKeyT>
  std::pair<iterator, bool> insert_as(std::pair<KeyT, ValueT> &&KV,
                                      const LookupKeyT &Val) {
    bool Inserted;
    BucketT *TheBucket = findOrInsertBucket(Val, Inserted);
    if (Inserted) {
      ::new (&TheBucket->getFirst()) KeyT(std::move(KV.first));
      ::new (&TheBucket->getSecond()) ValueT(std::move(KV.second));
    }
    return {makeIterator(TheBucket), Inserted};
  }

  /// Range insertion of pairs.
  template <typename InputIt> void insert(InputIt I, InputIt E) {
    for (; I != E; ++I)
      insert(*I);
  }

  /// Inserts range of 'std::pair<KeyT, ValueT>' values into the map.
  template <typename Range> void insert_range(Range &&R) {
    insert(adl_begin(R), adl_end(R));
  }

  template <typename V>
  std::pair<iterator, bool> insert_or_assign(const KeyT &Key, V &&Val) {
    auto Ret = try_emplace(Key, std::forward<V>(Val));
    if (!Ret.second)
      Ret.first->second = std::forward<V>(Val);
    return Ret;
  }

  template <typename V>
  std::pair<iterator, bool> insert_or_assign(KeyT &&Key, V &&Val) {
    auto Ret = try_emplace(std::move(Key), std::forward<V>(Val));
    if (!Ret.second)
      Ret.first->second = std::forward<V>(Val);
    return Ret;
  }

  template <typename... Ts>
  std::pair<iterator, bool> emplace_or_assign(const KeyT &Key, Ts &&...Args) {
    auto Ret = try_emplace(Key, std::forward<Ts>(Args)...);
    if (!Ret.second)
      Ret.first->second = ValueT(std::forward<Ts>(Args)...);
    return Ret;
  }

  template <typename... Ts>
  std::pair<iterator, bool> emplace_or_assign(KeyT &&Key, Ts &&...Args) {
    auto Ret = try_emplace(std::move(Key), std::forward<Ts>(Args)...);
    if (!Ret.second)
      Ret.first->second = ValueT(std::forward<Ts>(Args)...);
    return Ret;
  }

  bool erase(const KeyT &Val) {
    BucketT *TheBucket = doFind(Val);
    if (!TheBucket)
      return false; // not in map.
    eraseBucket(TheBucket);
    return true;
  }
  void erase(iterator I) { eraseBucket(&*I); }

  /// Overloads of erase that invoke \p OnMoved with a reference to each
  /// surviving bucket whose contents were relocated to close the gap left by
  /// the erased entry.  Callers holding external pointers into the bucket
  /// array can use this to update them surgically without scanning the whole
  /// map.
  template <typename OnMovedT> bool erase(const KeyT &Val, OnMovedT &&OnMoved) {
    BucketT *TheBucket = doFind(Val);
    if (!TheBucket)
      return false;
    eraseBucket(TheBucket, std::forward<OnMovedT>(OnMoved));
    return true;
  }
  template <typename OnMovedT> void erase(iterator I, OnMovedT &&OnMoved) {
    eraseBucket(&*I, std::forward<OnMovedT>(OnMoved));
  }

  ValueT &operator[](const KeyT &Key) {
    bool Inserted;
    BucketT *TheBucket = findOrInsertBucket(Key, Inserted);
    if (Inserted) {
      ::new (&TheBucket->getFirst()) KeyT(Key);
      ::new (&TheBucket->getSecond()) ValueT();
    }
    return TheBucket->getSecond();
  }

  ValueT &operator[](KeyT &&Key) {
    bool Inserted;
    BucketT *TheBucket = findOrInsertBucket(Key, Inserted);
    if (Inserted) {
      ::new (&TheBucket->getFirst()) KeyT(std::move(Key));
      ::new (&TheBucket->getSecond()) ValueT();
    }
    return TheBucket->getSecond();
  }

  /// Return true if the specified pointer points somewhere into the DenseMap's
  /// array of buckets (i.e. either to a key or value in the DenseMap).
  [[nodiscard]] bool isPointerIntoBucketsArray(const void *Ptr) const {
    return Ptr >= getBuckets() && Ptr < getBucketsEnd();
  }

  /// getPointerIntoBucketsArray() - Return an opaque pointer into the buckets
  /// array.  In conjunction with the previous method, this can be used to
  /// determine whether an insertion caused the DenseMap to reallocate.
  [[nodiscard]] const void *getPointerIntoBucketsArray() const {
    return getBuckets();
  }

  void swap(DerivedT &RHS) {
    this->incrementEpoch();
    RHS.incrementEpoch();
    derived().swapImpl(RHS);
  }

protected:
  DenseMapBase() = default;

  struct ExactBucketCount {};

  void initWithExactBucketCount(unsigned NewNumBuckets) {
    if (derived().allocateBuckets(NewNumBuckets)) {
      initEmpty();
    } else {
      setNumEntries(0);
      setNumTombstones(0);
    }
  }

  void destroyAll() {
    if constexpr (std::is_trivially_destructible_v<KeyT> &&
                  std::is_trivially_destructible_v<ValueT>)
      return;

    if (getNumBuckets() == 0) // Nothing to do.
      return;

    BucketT *B = getBuckets();
    const meta_t *M = getMetadata();
    for (unsigned I = 0, E = getNumBuckets(); I != E; ++I)
      if (M[I] != llvm::detail::densemap_vt::Empty)
        destroyBucket(B + I);
  }

  void initEmpty() {
    static_assert(std::is_base_of_v<DenseMapBase, DerivedT>,
                  "Must pass the derived type to this template!");
    setNumEntries(0);
    setNumTombstones(0);

    unsigned NumBuckets = getNumBuckets();
    assert((NumBuckets & (NumBuckets - 1)) == 0 &&
           "# initial buckets must be a power of two!");
    std::memset(getMetadata(), 0,
                sizeof(meta_t) * static_cast<size_t>(NumBuckets));
  }

  /// Returns the number of buckets to allocate to ensure that the DenseMap can
  /// accommodate \p NumEntries without need to grow().
  unsigned getMinBucketToReserveForEntries(unsigned NumEntries) {
    // Ensure that "NumEntries * 4 < NumBuckets * 3"
    if (NumEntries == 0)
      return 0;
    // +1 is required because of the strict inequality.
    // For example, if NumEntries is 48, we need to return 128.
    return NextPowerOf2(NumEntries * 4 / 3 + 1);
  }

  void copyFrom(const DerivedT &other) {
    this->destroyAll();
    derived().deallocateBuckets();
    setNumEntries(0);
    setNumTombstones(0);
    if (!derived().allocateBuckets(other.getNumBuckets())) {
      // The bucket list is empty.  No work to do.
      return;
    }

    assert(&other != this);
    assert(getNumBuckets() == other.getNumBuckets());

    setNumEntries(other.getNumEntries());
    setNumTombstones(other.getNumTombstones());

    const unsigned NumBuckets = getNumBuckets();
    BucketT *Buckets = getBuckets();
    const BucketT *OtherBuckets = other.getBuckets();
    meta_t *M = getMetadata();
    const meta_t *OtherM = other.getMetadata();
    std::memcpy(M, OtherM, sizeof(meta_t) * static_cast<size_t>(NumBuckets));

    if constexpr (std::is_trivially_copyable_v<KeyT> &&
                  std::is_trivially_copyable_v<ValueT>) {
      std::memcpy(reinterpret_cast<void *>(Buckets), OtherBuckets,
                  static_cast<size_t>(NumBuckets) * sizeof(BucketT));
    } else {
      for (unsigned I = 0; I != NumBuckets; ++I)
        if (M[I] != llvm::detail::densemap_vt::Empty) {
          ::new (&Buckets[I].getFirst()) KeyT(OtherBuckets[I].getFirst());
          ::new (&Buckets[I].getSecond()) ValueT(OtherBuckets[I].getSecond());
        }
    }
  }

private:
  DerivedT &derived() { return *static_cast<DerivedT *>(this); }
  const DerivedT &derived() const {
    return *static_cast<const DerivedT *>(this);
  }

  static void destroyBucket(BucketT *B) {
    B->getSecond().~ValueT();
    B->getFirst().~KeyT();
  }

  // Move-construct *Dst from *Src, then destroy *Src.  Dst is raw storage.
  static void relocateBucket(BucketT *Dst, BucketT *Src) {
    ::new (&Dst->getFirst()) KeyT(std::move(Src->getFirst()));
    ::new (&Dst->getSecond()) ValueT(std::move(Src->getSecond()));
    Src->getSecond().~ValueT();
    Src->getFirst().~KeyT();
  }

  template <typename LookupKeyT>
  static uint64_t getHash(const LookupKeyT &Val) {
    return llvm::densemap::detail::mix(static_cast<uint64_t>(
        static_cast<uint32_t>(KeyInfoT::getHashValue(Val))));
  }

  // Earliest empty bucket reachable from Home by quadratic probing.
  bool findFirstEmpty(size_t Home, size_t &EmptySlot, meta_t &Disp) const {
    const meta_t *M = getMetadata();
    unsigned Mask = getNumBuckets() - 1;
    Disp = 1;
    size_t Lin = 1;
    while (true) {
      EmptySlot = (Home + Lin) & Mask;
      if (M[EmptySlot] == llvm::detail::densemap_vt::Empty)
        return true;
      if (++Disp == llvm::detail::densemap_vt::DispMask)
        return false;
      Lin += Disp;
    }
  }

  // Keep the chain ordered by displacement: find the link after which a new
  // key with quadratic displacement DispToEmpty should be spliced.
  size_t findInsertLocationInChain(size_t Home, meta_t DispToEmpty) const {
    using namespace llvm::detail::densemap_vt;
    const meta_t *M = getMetadata();
    unsigned Mask = getNumBuckets() - 1;
    size_t Cand = Home;
    while (true) {
      meta_t D = M[Cand] & DispMask;
      if (D > DispToEmpty)
        return Cand;
      Cand = (Home + quadratic(D)) & Mask;
    }
  }

  // Vacate a home bucket squatted by a key that does not belong there.
  bool evict(size_t Bucket) {
    using namespace llvm::detail::densemap_vt;
    meta_t *M = getMetadata();
    BucketT *B = getBuckets();
    unsigned Mask = getNumBuckets() - 1;
    size_t Home = static_cast<size_t>(getHash(B[Bucket].getFirst())) & Mask;
    size_t Prev = Home;
    while (true) {
      size_t Next = (Home + quadratic(M[Prev] & DispMask)) & Mask;
      if (Next == Bucket)
        break;
      Prev = Next;
    }
    M[Prev] = (M[Prev] & ~DispMask) | (M[Bucket] & DispMask);

    size_t EmptySlot;
    meta_t Disp;
    if (!findFirstEmpty(Home, EmptySlot, Disp))
      return false;
    Prev = findInsertLocationInChain(Home, Disp);
    relocateBucket(B + EmptySlot, B + Bucket);
    M[EmptySlot] = (M[Bucket] & FragMask) | (M[Prev] & DispMask);
    M[Prev] = (M[Prev] & ~DispMask) | Disp;
    return true;
  }

  // Verstable find/insert core.  On return:
  //  - Inserted == false: the key was present; the returned bucket holds it.
  //  - Inserted == true:  the returned bucket is raw storage spliced into the
  //    table with NumEntries already incremented; the caller must construct
  //    the key and value in place.
  // Returns nullptr if blocked by the load factor / displacement limit; the
  // caller then grows and retries.
  template <typename LookupKeyT>
  BucketT *insertRaw(const LookupKeyT &Val, bool &Inserted) {
    using namespace llvm::detail::densemap_vt;
    assert(!KeyInfoT::isEqual(Val, KeyInfoT::getEmptyKey()) &&
           !KeyInfoT::isEqual(Val, KeyInfoT::getTombstoneKey()) &&
           "Empty/Tombstone value shouldn't be inserted into map!");
    meta_t *M = getMetadata();
    BucketT *B = getBuckets();
    unsigned NumBuckets = getNumBuckets();
    unsigned Mask = NumBuckets - 1;
    uint64_t Hash = getHash(Val);
    meta_t HF = fragOf(Hash);
    size_t Home = static_cast<size_t>(Hash) & Mask;

    // Load factor 0.875: (NumEntries + 1) * 8 > NumBuckets * 7.
    auto Overloaded = [&] {
      return (static_cast<uint64_t>(getNumEntries()) + 1) * 8 >
             static_cast<uint64_t>(NumBuckets) * 7;
    };

    // Case 1: home bucket empty or holding a non-belonging key.
    if (!(M[Home] & HomeMask)) {
      if (Overloaded() || (M[Home] != Empty && !evict(Home)))
        return nullptr;
      M[Home] = HF | HomeMask | DispMask;
      setNumEntries(getNumEntries() + 1);
      Inserted = true;
      return B + Home;
    }

    // Case 2: home bucket roots a chain.
    {
      size_t Cur = Home;
      while (true) {
        if ((M[Cur] & FragMask) == HF &&
            KeyInfoT::isEqual(Val, B[Cur].getFirst())) {
          Inserted = false;
          return B + Cur;
        }
        meta_t D = M[Cur] & DispMask;
        if (D == DispMask)
          break;
        Cur = (Home + quadratic(D)) & Mask;
      }
    }

    size_t EmptySlot;
    meta_t Disp;
    if (Overloaded() || !findFirstEmpty(Home, EmptySlot, Disp))
      return nullptr;
    size_t Prev = findInsertLocationInChain(Home, Disp);
    M[EmptySlot] = HF | (M[Prev] & DispMask);
    M[Prev] = (M[Prev] & ~DispMask) | Disp;
    setNumEntries(getNumEntries() + 1);
    Inserted = true;
    return B + EmptySlot;
  }

  template <typename LookupKeyT>
  BucketT *findOrInsertBucket(const LookupKeyT &Val, bool &Inserted) {
    while (true) {
      if (getNumBuckets() != 0) {
        if (BucketT *B = insertRaw(Val, Inserted))
          return B;
      }
      grow(getNumBuckets() ? getNumBuckets() * 2
                           : llvm::detail::densemap_vt::MinBucketCount);
    }
  }

  // Move every live entry of *this into Dst, which must be large enough that
  // every insertion succeeds.  *this is left empty.
  void moveAllInto(DerivedT &Dst) {
    if (getNumBuckets() == 0)
      return;
    BucketT *B = getBuckets();
    meta_t *M = getMetadata();
    for (unsigned I = 0, E = getNumBuckets(); I != E; ++I) {
      if (M[I] == llvm::detail::densemap_vt::Empty)
        continue;
      bool Inserted;
      BucketT *D = Dst.insertRaw(B[I].getFirst(), Inserted);
      assert(D && Inserted && "migration into a larger table must succeed");
      (void)Inserted;
      relocateBucket(D, B + I);
      M[I] = llvm::detail::densemap_vt::Empty;
    }
    setNumEntries(0);
  }

  // Reinsert every live entry of *this into a freshly sized table, moving the
  // key/value across, and adopt the new storage.  If a (practically
  // unreachable) displacement overflow blocks an insertion, the partially
  // filled table is migrated forward into a doubled one rather than back into
  // *this, so no entry is ever lost and *this stays consistent for the
  // not-yet-moved entries.
  void grow(unsigned MinNumBuckets) {
    incrementEpoch();
    unsigned NumBuckets = DerivedT::roundUpNumBuckets(MinNumBuckets);
    DerivedT Tmp(NumBuckets, typename DerivedT::ExactBucketCount{});
    if (getNumBuckets() != 0) {
      BucketT *B = getBuckets();
      meta_t *M = getMetadata();
      for (unsigned I = 0, E = getNumBuckets(); I != E; ++I) {
        if (M[I] == llvm::detail::densemap_vt::Empty)
          continue;
        bool Inserted;
        BucketT *Dst;
        while (!(Dst = Tmp.insertRaw(B[I].getFirst(), Inserted))) {
          DerivedT Bigger(Tmp.getNumBuckets() * 2,
                          typename DerivedT::ExactBucketCount{});
          Tmp.moveAllInto(Bigger);
          Tmp.swapImpl(Bigger);
        }
        assert(Inserted && "duplicate key while growing");
        (void)Inserted;
        relocateBucket(Dst, B + I);
        // The source entry has been moved out and destroyed; mark its slot
        // empty so the old storage (swapped into Tmp below) is not destroyed
        // a second time by Tmp's destructor.
        M[I] = llvm::detail::densemap_vt::Empty;
      }
      setNumEntries(0);
    }
    derived().swapImpl(Tmp);
  }

  // Unlink and destroy the entry occupying *Found.  Verstable's chain-unlink
  // erase relocates the chain tail, so erase invalidates all iterators and
  // references, not only the erased one; signal that to the epoch checker.
  void eraseBucket(BucketT *Found) {
    eraseBucket(Found, [](BucketT &) {});
  }

  template <typename OnMovedT>
  void eraseBucket(BucketT *Found, OnMovedT &&OnMoved) {
    using namespace llvm::detail::densemap_vt;
    incrementEpoch();
    meta_t *M = getMetadata();
    BucketT *B = getBuckets();
    unsigned Mask = getNumBuckets() - 1;
    size_t Ib = static_cast<size_t>(Found - B);
    setNumEntries(getNumEntries() - 1);

    // Case 1: sole key in its chain.
    if ((M[Ib] & HomeMask) && (M[Ib] & DispMask) == DispMask) {
      destroyBucket(B + Ib);
      M[Ib] = Empty;
      return;
    }

    size_t Home = (M[Ib] & HomeMask)
                      ? Ib
                      : (static_cast<size_t>(getHash(B[Ib].getFirst())) & Mask);

    // Case 2: last key in a multi-key chain - unlink the penultimate.
    if ((M[Ib] & DispMask) == DispMask) {
      size_t Cur = Home;
      while (true) {
        meta_t D = M[Cur] & DispMask;
        size_t Next = (Home + quadratic(D)) & Mask;
        if (Next == Ib) {
          M[Cur] |= DispMask;
          destroyBucket(B + Ib);
          M[Ib] = Empty;
          return;
        }
        Cur = Next;
      }
    }

    // Case 3: interior key - swap with the chain's last key, erase the end.
    size_t Cur = Ib;
    while (true) {
      size_t Prev = Cur;
      Cur = (Home + quadratic(M[Cur] & DispMask)) & Mask;
      if ((M[Cur] & DispMask) == DispMask) {
        destroyBucket(B + Ib);
        relocateBucket(B + Ib, B + Cur);
        M[Ib] = (M[Ib] & ~FragMask) | (M[Cur] & FragMask);
        M[Prev] |= DispMask;
        M[Cur] = Empty;
        OnMoved(B[Ib]);
        return;
      }
    }
  }

  template <typename KeyArgT, typename... Ts>
  std::pair<iterator, bool> try_emplace_impl(KeyArgT &&Key, Ts &&...Args) {
    bool Inserted;
    BucketT *TheBucket = findOrInsertBucket(Key, Inserted);
    if (Inserted) {
      ::new (&TheBucket->getFirst()) KeyT(std::forward<KeyArgT>(Key));
      ::new (&TheBucket->getSecond()) ValueT(std::forward<Ts>(Args)...);
    }
    return {makeIterator(TheBucket), Inserted};
  }

  iterator makeIterator(BucketT *TheBucket) {
    return iterator::makeIterator(TheBucket, getBuckets(), getMetadata(),
                                  getNumBuckets(), *this);
  }

  const_iterator makeConstIterator(const BucketT *TheBucket) const {
    return const_iterator::makeIterator(TheBucket, getBuckets(), getMetadata(),
                                        getNumBuckets(), *this);
  }

  unsigned getNumEntries() const { return derived().getNumEntries(); }

  void setNumEntries(unsigned Num) { derived().setNumEntries(Num); }

  unsigned getNumTombstones() const { return derived().getNumTombstones(); }

  void setNumTombstones(unsigned Num) { derived().setNumTombstones(Num); }

  const BucketT *getBuckets() const { return derived().getBuckets(); }

  BucketT *getBuckets() { return derived().getBuckets(); }

  const meta_t *getMetadata() const { return derived().getMetadata(); }

  meta_t *getMetadata() { return derived().getMetadata(); }

  unsigned getNumBuckets() const { return derived().getNumBuckets(); }

  BucketT *getBucketsEnd() { return getBuckets() + getNumBuckets(); }

  const BucketT *getBucketsEnd() const {
    return getBuckets() + getNumBuckets();
  }

  template <typename LookupKeyT>
  const BucketT *doFind(const LookupKeyT &Val) const {
    using namespace llvm::detail::densemap_vt;
    const unsigned NumBuckets = getNumBuckets();
    if (NumBuckets == 0)
      return nullptr;
    const meta_t *M = getMetadata();
    const BucketT *B = getBuckets();
    unsigned Mask = NumBuckets - 1;
    uint64_t Hash = getHash(Val);
    size_t Home = static_cast<size_t>(Hash) & Mask;
    if (!(M[Home] & HomeMask))
      return nullptr;
    meta_t HF = fragOf(Hash);
    size_t Cur = Home;
    while (true) {
      if ((M[Cur] & FragMask) == HF &&
          LLVM_LIKELY(KeyInfoT::isEqual(Val, B[Cur].getFirst())))
        return B + Cur;
      meta_t D = M[Cur] & DispMask;
      if (D == DispMask)
        return nullptr;
      Cur = (Home + quadratic(D)) & Mask;
    }
  }

  template <typename LookupKeyT> BucketT *doFind(const LookupKeyT &Val) {
    return const_cast<BucketT *>(
        static_cast<const DenseMapBase *>(this)->doFind(Val));
  }

public:
  /// Return the approximate size (in bytes) of the actual map.
  /// This is just the raw memory used by DenseMap.
  /// If entries are pointers to objects, the size of the referenced objects
  /// are not included.
  [[nodiscard]] size_t getMemorySize() const {
    return getNumBuckets() * sizeof(BucketT);
  }
};

/// Equality comparison for DenseMap.
///
/// Iterates over elements of LHS confirming that each (key, value) pair in LHS
/// is also in RHS, and that no additional pairs are in RHS.
/// Equivalent to N calls to RHS.find and N value comparisons. Amortized
/// complexity is linear, worst case is O(N^2) (if every hash collides).
template <typename DerivedT, typename KeyT, typename ValueT, typename KeyInfoT,
          typename BucketT>
[[nodiscard]] bool
operator==(const DenseMapBase<DerivedT, KeyT, ValueT, KeyInfoT, BucketT> &LHS,
           const DenseMapBase<DerivedT, KeyT, ValueT, KeyInfoT, BucketT> &RHS) {
  if (LHS.size() != RHS.size())
    return false;

  for (auto &KV : LHS) {
    auto I = RHS.find(KV.first);
    if (I == RHS.end() || I->second != KV.second)
      return false;
  }

  return true;
}

/// Inequality comparison for DenseMap.
///
/// Equivalent to !(LHS == RHS). See operator== for performance notes.
template <typename DerivedT, typename KeyT, typename ValueT, typename KeyInfoT,
          typename BucketT>
[[nodiscard]] bool
operator!=(const DenseMapBase<DerivedT, KeyT, ValueT, KeyInfoT, BucketT> &LHS,
           const DenseMapBase<DerivedT, KeyT, ValueT, KeyInfoT, BucketT> &RHS) {
  return !(LHS == RHS);
}

template <typename KeyT, typename ValueT,
          typename KeyInfoT = DenseMapInfo<KeyT>,
          typename BucketT = llvm::detail::DenseMapPair<KeyT, ValueT>>
class DenseMap : public DenseMapBase<DenseMap<KeyT, ValueT, KeyInfoT, BucketT>,
                                     KeyT, ValueT, KeyInfoT, BucketT> {
  friend class DenseMapBase<DenseMap, KeyT, ValueT, KeyInfoT, BucketT>;

  // Lift some types from the dependent base class into this class for
  // simplicity of referring to them.
  using BaseT = DenseMapBase<DenseMap, KeyT, ValueT, KeyInfoT, BucketT>;
  using meta_t = llvm::detail::densemap_vt::meta_t;

  BucketT *Buckets = nullptr;
  meta_t *Metadata = &llvm::detail::densemap_vt::EmptyMetadata;
  unsigned NumEntries = 0;
  unsigned NumTombstones = 0;
  unsigned NumBuckets = 0;

  explicit DenseMap(unsigned NumBuckets, typename BaseT::ExactBucketCount) {
    this->initWithExactBucketCount(NumBuckets);
  }

public:
  /// Create a DenseMap with an optional \p NumElementsToReserve to guarantee
  /// that this number of elements can be inserted in the map without grow().
  explicit DenseMap(unsigned NumElementsToReserve = 0)
      : DenseMap(BaseT::getMinBucketToReserveForEntries(NumElementsToReserve),
                 typename BaseT::ExactBucketCount{}) {}

  DenseMap(const DenseMap &other) : DenseMap() { this->copyFrom(other); }

  DenseMap(DenseMap &&other) : DenseMap() { this->swap(other); }

  template <typename InputIt>
  DenseMap(const InputIt &I, const InputIt &E) : DenseMap(std::distance(I, E)) {
    this->insert(I, E);
  }

  template <typename RangeT>
  DenseMap(llvm::from_range_t, const RangeT &Range)
      : DenseMap(adl_begin(Range), adl_end(Range)) {}

  DenseMap(std::initializer_list<typename BaseT::value_type> Vals)
      : DenseMap(Vals.begin(), Vals.end()) {}

  ~DenseMap() {
    this->destroyAll();
    deallocateBuckets();
  }

  DenseMap &operator=(const DenseMap &other) {
    if (&other != this)
      this->copyFrom(other);
    return *this;
  }

  DenseMap &operator=(DenseMap &&other) {
    this->destroyAll();
    deallocateBuckets();
    this->initWithExactBucketCount(0);
    this->swap(other);
    return *this;
  }

private:
  void swapImpl(DenseMap &RHS) {
    std::swap(Buckets, RHS.Buckets);
    std::swap(Metadata, RHS.Metadata);
    std::swap(NumEntries, RHS.NumEntries);
    std::swap(NumTombstones, RHS.NumTombstones);
    std::swap(NumBuckets, RHS.NumBuckets);
  }

  unsigned getNumEntries() const { return NumEntries; }

  void setNumEntries(unsigned Num) { NumEntries = Num; }

  unsigned getNumTombstones() const { return NumTombstones; }

  void setNumTombstones(unsigned Num) { NumTombstones = Num; }

  BucketT *getBuckets() const { return Buckets; }

  meta_t *getMetadata() const { return Metadata; }

  unsigned getNumBuckets() const { return NumBuckets; }

  void deallocateBuckets() {
    if (NumBuckets == 0)
      return;
    deallocate_buffer(Buckets, sizeof(BucketT) * NumBuckets, alignof(BucketT));
    deallocate_buffer(Metadata, sizeof(meta_t) * NumBuckets, alignof(meta_t));
    Buckets = nullptr;
    Metadata = &llvm::detail::densemap_vt::EmptyMetadata;
    NumBuckets = 0;
  }

  bool allocateBuckets(unsigned Num) {
    if (Num == 0) {
      NumBuckets = 0;
      Buckets = nullptr;
      Metadata = &llvm::detail::densemap_vt::EmptyMetadata;
      return false;
    }
    NumBuckets = std::max(llvm::detail::densemap_vt::MinBucketCount,
                          static_cast<unsigned>(NextPowerOf2(Num - 1)));
    Buckets = static_cast<BucketT *>(
        allocate_buffer(sizeof(BucketT) * NumBuckets, alignof(BucketT)));
    Metadata = static_cast<meta_t *>(
        allocate_buffer(sizeof(meta_t) * NumBuckets, alignof(meta_t)));
    return true;
  }

  static unsigned roundUpNumBuckets(unsigned MinNumBuckets) {
    return std::max(64u,
                    static_cast<unsigned>(NextPowerOf2(MinNumBuckets - 1)));
  }

  // Plan how to shrink the bucket table.  Return:
  // - {false, 0} to reuse the existing bucket table
  // - {true, N} to reallocate a bucket table with N entries
  std::pair<bool, unsigned> planShrinkAndClear() const {
    unsigned NewNumBuckets = 0;
    if (NumEntries)
      NewNumBuckets = std::max(64u, 1u << (Log2_32_Ceil(NumEntries) + 1));
    if (NewNumBuckets == NumBuckets)
      return {false, 0};          // Reuse.
    return {true, NewNumBuckets}; // Reallocate.
  }
};

/// This Verstable port does not model the inline small-buffer optimisation,
/// so SmallDenseMap is just an alias for a heap-backed DenseMap; InlineBuckets
/// is accepted for API compatibility but ignored.
template <typename KeyT, typename ValueT, unsigned InlineBuckets = 4,
          typename KeyInfoT = DenseMapInfo<KeyT>,
          typename BucketT = llvm::detail::DenseMapPair<KeyT, ValueT>>
using SmallDenseMap = DenseMap<KeyT, ValueT, KeyInfoT, BucketT>;

template <typename KeyT, typename ValueT, typename KeyInfoT, typename Bucket,
          bool IsConst>
class DenseMapIterator : DebugEpochBase::HandleBase {
  friend class DenseMapIterator<KeyT, ValueT, KeyInfoT, Bucket, true>;
  friend class DenseMapIterator<KeyT, ValueT, KeyInfoT, Bucket, false>;

  using meta_t = llvm::detail::densemap_vt::meta_t;

public:
  using difference_type = ptrdiff_t;
  using value_type = std::conditional_t<IsConst, const Bucket, Bucket>;
  using pointer = value_type *;
  using reference = value_type &;
  using iterator_category = std::forward_iterator_tag;

private:
  // The bucket pointer and the parallel metadata pointer iterate together;
  // both become reverse_iterators under LLVM_ENABLE_REVERSE_ITERATION so the
  // direction is handled uniformly (mirrors upstream DenseMap's maybeReverse).
  using BucketItTy =
      std::conditional_t<shouldReverseIterate<KeyT>(),
                         std::reverse_iterator<pointer>, pointer>;
  using MetaItTy =
      std::conditional_t<shouldReverseIterate<KeyT>(),
                         std::reverse_iterator<const meta_t *>, const meta_t *>;

  BucketItTy Ptr = {};
  BucketItTy End = {};
  MetaItTy Meta = {};

  DenseMapIterator(BucketItTy Pos, BucketItTy E, MetaItTy M,
                   const DebugEpochBase &Epoch)
      : DebugEpochBase::HandleBase(&Epoch), Ptr(Pos), End(E), Meta(M) {
    assert(isHandleInSync() && "invalid construction!");
  }

  void advancePastEmptyBuckets() {
    while (Ptr != End && *Meta == llvm::detail::densemap_vt::Empty) {
      ++Ptr;
      ++Meta;
    }
  }

  template <typename T> static auto maybeReverse(iterator_range<T> Range) {
    if constexpr (shouldReverseIterate<KeyT>())
      return llvm::reverse(Range);
    else
      return Range;
  }

public:
  DenseMapIterator() = default;

  static DenseMapIterator makeBegin(pointer Buckets, const meta_t *Metadata,
                                    unsigned NumBuckets, bool IsEmpty,
                                    const DebugEpochBase &Epoch) {
    // When the map is empty, avoid the overhead of advancing past empties.
    if (IsEmpty)
      return makeEnd(Buckets, Metadata, NumBuckets, Epoch);
    auto BR = maybeReverse(llvm::make_range(Buckets, Buckets + NumBuckets));
    auto MR = maybeReverse(llvm::make_range(Metadata, Metadata + NumBuckets));
    DenseMapIterator Iter(BR.begin(), BR.end(), MR.begin(), Epoch);
    Iter.advancePastEmptyBuckets();
    return Iter;
  }

  static DenseMapIterator makeEnd(pointer Buckets, const meta_t *Metadata,
                                  unsigned NumBuckets,
                                  const DebugEpochBase &Epoch) {
    auto BR = maybeReverse(llvm::make_range(Buckets, Buckets + NumBuckets));
    auto MR = maybeReverse(llvm::make_range(Metadata, Metadata + NumBuckets));
    return DenseMapIterator(BR.end(), BR.end(), MR.end(), Epoch);
  }

  static DenseMapIterator makeIterator(pointer P, pointer Buckets,
                                       const meta_t *Metadata,
                                       unsigned NumBuckets,
                                       const DebugEpochBase &Epoch) {
    auto BR = maybeReverse(llvm::make_range(Buckets, Buckets + NumBuckets));
    const meta_t *MP = Metadata + (P - Buckets);
    constexpr int Offset = shouldReverseIterate<KeyT>() ? 1 : 0;
    return DenseMapIterator(BucketItTy(P + Offset), BR.end(),
                            MetaItTy(MP + Offset), Epoch);
  }

  // Converting ctor from non-const iterators to const iterators. SFINAE'd out
  // for const iterator destinations so it doesn't end up as a user defined copy
  // constructor.
  template <bool IsConstSrc,
            typename = std::enable_if_t<!IsConstSrc && IsConst>>
  DenseMapIterator(
      const DenseMapIterator<KeyT, ValueT, KeyInfoT, Bucket, IsConstSrc> &I)
      : DebugEpochBase::HandleBase(I), Ptr(I.Ptr), End(I.End), Meta(I.Meta) {}

  [[nodiscard]] reference operator*() const {
    assert(isHandleInSync() && "invalid iterator access!");
    assert(Ptr != End && "dereferencing end() iterator");
    return *Ptr;
  }
  [[nodiscard]] pointer operator->() const { return &operator*(); }

  [[nodiscard]] friend bool operator==(const DenseMapIterator &LHS,
                                       const DenseMapIterator &RHS) {
    assert((!LHS.getEpochAddress() || LHS.isHandleInSync()) &&
           "handle not in sync!");
    assert((!RHS.getEpochAddress() || RHS.isHandleInSync()) &&
           "handle not in sync!");
    assert(LHS.getEpochAddress() == RHS.getEpochAddress() &&
           "comparing incomparable iterators!");
    return LHS.Ptr == RHS.Ptr;
  }

  [[nodiscard]] friend bool operator!=(const DenseMapIterator &LHS,
                                       const DenseMapIterator &RHS) {
    return !(LHS == RHS);
  }

  inline DenseMapIterator &operator++() { // Preincrement
    assert(isHandleInSync() && "invalid iterator access!");
    assert(Ptr != End && "incrementing end() iterator");
    ++Ptr;
    ++Meta;
    advancePastEmptyBuckets();
    return *this;
  }
  DenseMapIterator operator++(int) { // Postincrement
    assert(isHandleInSync() && "invalid iterator access!");
    DenseMapIterator tmp = *this;
    ++*this;
    return tmp;
  }
};

template <typename KeyT, typename ValueT, typename KeyInfoT>
[[nodiscard]] inline size_t
capacity_in_bytes(const DenseMap<KeyT, ValueT, KeyInfoT> &X) {
  return X.getMemorySize();
}

} // end namespace llvm

#endif // LLVM_ADT_DENSEMAP_H
