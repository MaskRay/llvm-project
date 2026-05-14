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
//===----------------------------------------------------------------------===//

#ifndef LLVM_ADT_DENSEMAP_H
#define LLVM_ADT_DENSEMAP_H

#include "llvm/ADT/DenseMapInfo.h"
#include "llvm/ADT/EpochTracker.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/STLForwardCompat.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/MemAlloc.h"
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

#if defined(__SSE2__) || defined(_M_X64)
#define LLVM_DM_SSE2 1
#include <emmintrin.h>
#endif

namespace llvm {

// ============================================================
// Hash mixing: 32-bit input -> 64-bit output.
// ============================================================
namespace densemap_detail {

// 32-bit input is well-mixed by DenseMapInfo (splitmix64 narrowed for
// pointers, *37 for ints). A single 64-bit multiply by an odd constant
// re-spreads it across the full 64 bits so high bits drive group position
// and low bits drive the h2 reduced hash without correlating with the
// already-narrowed 32-bit input.
inline std::uint64_t mixHash(std::uint32_t h) noexcept {
  return static_cast<std::uint64_t>(h) * 0xbf58476d1ce4e5b9ull;
}

// ============================================================
// group15: 16-byte metadata for 15 element slots.
//   byte[0..14]: 0=available, 1=sentinel, 2..255=reduced hash
//   byte[15]:    overflow byte (bit i set when hash%8==i overflowed)
// ============================================================
struct group15 {
  static constexpr unsigned N = 15;

  alignas(16) unsigned char m[16];

  void initialize() noexcept { std::memset(m, 0, 16); }
  void setSentinel() noexcept { m[N - 1] = 1; }

  static unsigned char reducedHash(std::uint64_t h) noexcept {
    unsigned char b = static_cast<unsigned char>(h & 0xFF);
    if (b <= 1)
      b += 8;
    return b;
  }

  void set(unsigned pos, std::uint64_t h) noexcept { m[pos] = reducedHash(h); }
  void reset(unsigned pos) noexcept { m[pos] = 0; }

  static bool isSentinel(const unsigned char *pc) noexcept { return *pc == 1; }
  static bool isOccupied(const unsigned char *pc) noexcept { return *pc != 0; }

#if defined(LLVM_DM_SSE2)
  int match(std::uint64_t h) const noexcept {
    __m128i meta = _mm_load_si128(reinterpret_cast<const __m128i *>(m));
    __m128i v = _mm_set1_epi8(static_cast<char>(reducedHash(h)));
    return _mm_movemask_epi8(_mm_cmpeq_epi8(meta, v)) & 0x7FFF;
  }
  int matchAvailable() const noexcept {
    __m128i meta = _mm_load_si128(reinterpret_cast<const __m128i *>(m));
    return _mm_movemask_epi8(_mm_cmpeq_epi8(meta, _mm_setzero_si128())) &
           0x7FFF;
  }
#else
  int match(std::uint64_t h) const noexcept {
    unsigned char rb = reducedHash(h);
    int mask = 0;
    for (unsigned i = 0; i < N; ++i)
      if (m[i] == rb)
        mask |= (1 << i);
    return mask;
  }
  int matchAvailable() const noexcept {
    int mask = 0;
    for (unsigned i = 0; i < N; ++i)
      if (m[i] == 0)
        mask |= (1 << i);
    return mask;
  }
#endif

  int matchOccupied() const noexcept { return (~matchAvailable()) & 0x7FFF; }

  bool isNotOverflowed(std::uint64_t h) const noexcept {
    return !(m[N] & (1u << (h % 8)));
  }
  void markOverflow(std::uint64_t h) noexcept {
    m[N] |= static_cast<unsigned char>(1u << (h % 8));
  }
};

// ============================================================
// Single allocation holding elements then 16-byte-aligned groups.
// The buffer base is always the elements pointer (allocate_buffer's
// return value, reinterpret_cast to BucketT*), so we deallocate with
// elements_ as the pointer.  Size and alignment are recomputed from
// numGroups + alignof(BucketT) at free time rather than stored.
// ============================================================
template <typename BucketT>
inline std::size_t bufferBytes(unsigned ng) noexcept {
  return sizeof(BucketT) * ng * group15::N + sizeof(group15) * ng + 15;
}

template <typename BucketT>
inline constexpr std::size_t bufferAlign() noexcept {
  return alignof(BucketT) > 16 ? alignof(BucketT) : 16;
}

template <typename BucketT> struct FOAAlloc {
  BucketT *elements;
  group15 *groups;
};

template <typename BucketT> FOAAlloc<BucketT> allocFOA(unsigned ng) {
  constexpr unsigned N = group15::N;
  std::size_t elemBytes = sizeof(BucketT) * ng * N;
  void *buf = allocate_buffer(bufferBytes<BucketT>(ng), bufferAlign<BucketT>());
  BucketT *elems = reinterpret_cast<BucketT *>(buf);
  std::uintptr_t ga = (reinterpret_cast<std::uintptr_t>(buf) + elemBytes + 15) &
                      ~std::uintptr_t(15);
  return {elems, reinterpret_cast<group15 *>(ga)};
}

inline unsigned computeSizeIndex(unsigned ng) noexcept {
  // sizeIndex = 64 - log2(ng) for ng >= 2; for ng==1: log2=0 -> si=63 avoids
  // shift by 64.
  if (ng <= 1u)
    return 63u;
  return static_cast<unsigned>(64 - __builtin_ctz(ng));
}

} // namespace densemap_detail

// ============================================================
// detail::DenseMapPair
// ============================================================
namespace detail {

template <typename KeyT, typename ValueT>
struct DenseMapPair : std::pair<KeyT, ValueT> {
  using Base = std::pair<KeyT, ValueT>;
  using Base::Base;
  KeyT &getFirst() { return this->first; }
  const KeyT &getFirst() const { return this->first; }
  ValueT &getSecond() { return this->second; }
  const ValueT &getSecond() const { return this->second; }
};

} // namespace detail

// ============================================================
// Forward declarations
// ============================================================
template <typename KeyT, typename ValueT,
          typename KeyInfoT = DenseMapInfo<KeyT>,
          typename Bucket = llvm::detail::DenseMapPair<KeyT, ValueT>,
          bool IsConst = false>
class DenseMapIterator;

template <typename DerivedT, typename KeyT, typename ValueT, typename KeyInfoT,
          typename BucketT>
class DenseMapBase;

// ============================================================
// DenseMapIterator (Boost regular-layout style)
// ============================================================
template <typename KeyT, typename ValueT, typename KeyInfoT, typename BucketT,
          bool IsConst>
class DenseMapIterator : public DebugEpochBase::HandleBase {
  friend class DenseMapIterator<KeyT, ValueT, KeyInfoT, BucketT, true>;
  friend class DenseMapIterator<KeyT, ValueT, KeyInfoT, BucketT, false>;
  template <typename, typename, typename, typename, typename>
  friend class DenseMapBase;

  using Group = densemap_detail::group15;
  static constexpr unsigned GroupN = Group::N;
  static constexpr unsigned GSize = sizeof(Group); // 16

  // pc_: pointer to metadata byte of the current slot.
  // p_:  pointer to current element; nullptr means end().
  unsigned char *pc_ = nullptr;
  BucketT *p_ = nullptr;

  void increment() noexcept {
    // Boost regular-layout increment logic.
    for (;;) {
      ++p_;
      // Are we leaving a group? Slot index within group = pc_ % GSize.
      if ((reinterpret_cast<std::uintptr_t>(pc_) % GSize) == GroupN - 1u) {
        // pc_ was at slot 14 (index 14 within 16-byte group).
        // Advance past the overflow byte (byte 15) to start of next group.
        // Distance from slot 14 to slot 0 of next group = GSize - (GroupN-1)
        // = 2.
        pc_ += static_cast<std::ptrdiff_t>(GSize - (GroupN - 1u));
        break; // fall through to SIMD scan
      }
      ++pc_;
      if (!Group::isOccupied(pc_))
        continue;
      if (LLVM_UNLIKELY(Group::isSentinel(pc_)))
        p_ = nullptr;
      return;
    }

    // pc_ now points to byte 0 of a group. Scan groups for next occupied slot.
    for (;;) {
      // pc_ must be aligned to GSize (start of group).
      Group *pg = reinterpret_cast<Group *>(pc_);
      int mask = pg->matchOccupied();
      if (mask != 0) {
        unsigned n =
            static_cast<unsigned>(__builtin_ctz(static_cast<unsigned>(mask)));
        if (LLVM_UNLIKELY(pg->m[n] == 1u)) { // sentinel
          p_ = nullptr;
        } else {
          pc_ += static_cast<std::ptrdiff_t>(n);
          p_ += static_cast<std::ptrdiff_t>(n);
        }
        return;
      }
      pc_ += static_cast<std::ptrdiff_t>(GSize);
      p_ += static_cast<std::ptrdiff_t>(GroupN);
    }
  }

public:
  using difference_type = std::ptrdiff_t;
  using value_type = std::conditional_t<IsConst, const BucketT, BucketT>;
  using pointer = value_type *;
  using reference = value_type &;
  using iterator_category = std::forward_iterator_tag;

  DenseMapIterator() = default;

  // Primary ctor: group pointer + slot + element pointer.
  DenseMapIterator(Group *pg, unsigned slot, BucketT *p,
                   const DebugEpochBase *epoch)
      : DebugEpochBase::HandleBase(epoch),
        pc_(pg ? reinterpret_cast<unsigned char *>(pg) + slot : nullptr),
        p_(p) {}

  // Converting ctor non-const -> const.
  template <bool ISrc, typename = std::enable_if_t<!ISrc && IsConst>>
  DenseMapIterator(
      const DenseMapIterator<KeyT, ValueT, KeyInfoT, BucketT, ISrc> &I)
      : DebugEpochBase::HandleBase(I), pc_(I.pc_), p_(I.p_) {}

  reference operator*() const {
    assert(p_ && "dereferencing end() iterator");
    return *p_;
  }
  pointer operator->() const { return &operator*(); }

  // Comparable across constness so iterator and const_iterator interoperate.
  template <bool RHSIsConst>
  bool operator==(const DenseMapIterator<KeyT, ValueT, KeyInfoT, BucketT,
                                         RHSIsConst> &o) const {
    return p_ == o.p_;
  }
  template <bool RHSIsConst>
  bool operator!=(const DenseMapIterator<KeyT, ValueT, KeyInfoT, BucketT,
                                         RHSIsConst> &o) const {
    return p_ != o.p_;
  }

  DenseMapIterator &operator++() {
    assert(p_ && "incrementing end() iterator");
    increment();
    return *this;
  }
  DenseMapIterator operator++(int) {
    DenseMapIterator tmp = *this;
    ++*this;
    return tmp;
  }
};

// ============================================================
// DenseMapBase - CRTP base
// ============================================================
template <typename DerivedT, typename KeyT, typename ValueT, typename KeyInfoT,
          typename BucketT>
class DenseMapBase : public DebugEpochBase {
  template <typename T>
  using const_arg_type_t = typename const_pointer_or_const_ref<T>::type;

  using Group = densemap_detail::group15;
  static constexpr unsigned GroupN = Group::N;

  DerivedT &derived() { return *static_cast<DerivedT *>(this); }
  const DerivedT &derived() const {
    return *static_cast<const DerivedT *>(this);
  }

  // CRTP accessors (delegated to Derived).
  Group *getGroups() { return derived().getGroups(); }
  const Group *getGroups() const { return derived().getGroups(); }
  BucketT *getElements() { return derived().getElements(); }
  const BucketT *getElements() const { return derived().getElements(); }
  unsigned getSizeIndex() const { return derived().getSizeIndex(); }
  unsigned getGroupsMask() const { return derived().getGroupsMask(); }
  unsigned getNumEntries() const { return derived().getNumEntries(); }
  void setNumEntries(unsigned n) { derived().setNumEntries(n); }
  unsigned numGroups() const { return getGroupsMask() + 1u; }
  unsigned capacity() const {
    unsigned ng = numGroups();
    return ng == 0u ? 0u : ng * GroupN - 1u;
  }
  // Pure function of the bucket count.  Boost's anti-drift mechanism (which
  // would decay maxLoad on overflow-causing erases) is omitted; LLVM
  // workloads are mostly insert-then-iterate/clear rather than steady-state
  // churn.
  unsigned getMaxLoad() const {
    unsigned cap = capacity();
    return cap <= 29u ? cap : (cap * 7u) / 8u;
  }
  unsigned groupIndex(std::uint64_t h) const {
    return static_cast<unsigned>(h >> getSizeIndex()) & getGroupsMask();
  }

  using Iter = DenseMapIterator<KeyT, ValueT, KeyInfoT, BucketT, false>;
  using CIter = DenseMapIterator<KeyT, ValueT, KeyInfoT, BucketT, true>;

  Iter makeIterator(Group *pg, unsigned slot, BucketT *p) {
    return Iter(pg, slot, p, this);
  }
  CIter makeConstIterator(const Group *pg, unsigned slot,
                          const BucketT *p) const {
    return CIter(const_cast<Group *>(pg), slot, const_cast<BucketT *>(p), this);
  }

  struct FindResult {
    Group *pg;
    unsigned slot;
    BucketT *p;
  };

  template <typename LookupKeyT>
  FindResult doFindWith(Group *groups, BucketT *elems, unsigned mask,
                        unsigned si, const LookupKeyT &val,
                        std::uint64_t h) const noexcept {
    unsigned pos = static_cast<unsigned>(h >> si) & mask;
    unsigned step = 0;
    for (;;) {
      Group *pg = groups + pos;
      BucketT *ge = elems + pos * GroupN;
      int cands = pg->match(h);
      while (cands) {
        unsigned i =
            static_cast<unsigned>(__builtin_ctz(static_cast<unsigned>(cands)));
        if (LLVM_LIKELY(KeyInfoT::isEqual(val, ge[i].getFirst())))
          return {pg, i, ge + i};
        cands &= cands - 1;
      }
      if (LLVM_LIKELY(pg->isNotOverflowed(h)))
        return {nullptr, 0, nullptr};
      if (LLVM_UNLIKELY(++step > mask))
        return {nullptr, 0, nullptr};
      pos = (pos + step) & mask;
    }
  }

  template <typename LookupKeyT> FindResult doFind(const LookupKeyT &val) {
    if (!getElements())
      return {nullptr, 0, nullptr};
    std::uint64_t h = densemap_detail::mixHash(KeyInfoT::getHashValue(val));
    return doFindWith(getGroups(), getElements(), getGroupsMask(),
                      getSizeIndex(), val, h);
  }

  template <typename LookupKeyT>
  FindResult doFind(const LookupKeyT &val) const {
    if (!getElements())
      return {nullptr, 0, nullptr};
    std::uint64_t h = densemap_detail::mixHash(KeyInfoT::getHashValue(val));
    return doFindWith(const_cast<Group *>(getGroups()),
                      const_cast<BucketT *>(getElements()), getGroupsMask(),
                      getSizeIndex(), val, h);
  }

protected:
  // Unchecked insert: room must exist, key must be absent.
  template <typename KeyArgT, typename... Ts>
  BucketT *uncheckedEmplace(std::uint64_t h, KeyArgT &&key, Ts &&...args) {
    unsigned mask = getGroupsMask();
    unsigned pos = groupIndex(h);
    unsigned step = 0;
    for (;;) {
      Group *pg = getGroups() + pos;
      int avail = pg->matchAvailable();
      if (avail) {
        unsigned i =
            static_cast<unsigned>(__builtin_ctz(static_cast<unsigned>(avail)));
        BucketT *p = getElements() + pos * GroupN + i;
        pg->set(i, h);
        ::new (&p->getFirst()) KeyT(std::forward<KeyArgT>(key));
        ::new (&p->getSecond()) ValueT(std::forward<Ts>(args)...);
        setNumEntries(getNumEntries() + 1);
        return p;
      }
      pg->markOverflow(h);
      if (LLVM_UNLIKELY(++step > mask))
        assert(false && "table full in uncheckedEmplace");
      pos = (pos + step) & mask;
    }
  }

  // Rehash: steal old allocation, allocate new, move elements, free old.
  // Cold slow path; keep it out of line so the inlined insert fast paths that
  // call it stay small.
  LLVM_ATTRIBUTE_NOINLINE void growToGroups(unsigned newNG) {
    // 1. Steal current allocation (clears internal ptrs without freeing).
    auto [oldGroups, oldElems, oldNG] = derived().stealAllocation();
    // 2. Allocate new storage (internal state now points to new).
    derived().allocateBuckets(newNG);
    // 3. Rehash all elements from old into new.
    for (unsigned g = 0; g < oldNG; g++) {
      Group *pg = oldGroups + g;
      BucketT *ge = oldElems + g * GroupN;
      int occ = pg->matchOccupied();
      while (occ) {
        unsigned i =
            static_cast<unsigned>(__builtin_ctz(static_cast<unsigned>(occ)));
        // Skip sentinel slot (last slot of last group).
        if (!(g == oldNG - 1u && i == GroupN - 1u)) {
          std::uint64_t h = densemap_detail::mixHash(
              KeyInfoT::getHashValue(ge[i].getFirst()));
          uncheckedEmplace(h, std::move(ge[i].getFirst()),
                           std::move(ge[i].getSecond()));
          ge[i].getFirst().~KeyT();
          ge[i].getSecond().~ValueT();
        }
        occ &= occ - 1;
      }
    }
    // 4. Free old allocation.
    if (oldElems)
      deallocate_buffer(oldElems, densemap_detail::bufferBytes<BucketT>(oldNG),
                        densemap_detail::bufferAlign<BucketT>());
  }

  void maybeGrow() {
    if (LLVM_UNLIKELY(getNumEntries() >= getMaxLoad())) {
      unsigned ng = numGroups();
      growToGroups(ng == 0u ? 1u : ng * 2u);
    }
  }

  // Fused find + insertion-point walk.  Single pass over the probe chain;
  // records the first available slot encountered so a successful miss can
  // insert without re-walking from pos0.
  template <typename KeyArgT, typename... Ts>
  std::pair<Iter, bool> tryEmplaceImpl(KeyArgT &&key, Ts &&...args) {
    if (!getElements())
      derived().allocateBuckets(1u);

    std::uint64_t h = densemap_detail::mixHash(KeyInfoT::getHashValue(key));
    unsigned mask = getGroupsMask();
    unsigned si = getSizeIndex();
    Group *groups = getGroups();
    BucketT *elems = getElements();
    unsigned pos = static_cast<unsigned>(h >> si) & mask;
    unsigned step = 0;

    Group *insertPg = nullptr;
    unsigned insertSlot = 0;

    for (;;) {
      Group *pg = groups + pos;
      BucketT *ge = elems + pos * GroupN;
      int cands = pg->match(h);
      while (cands) {
        unsigned i =
            static_cast<unsigned>(__builtin_ctz(static_cast<unsigned>(cands)));
        if (LLVM_LIKELY(KeyInfoT::isEqual(key, ge[i].getFirst())))
          return {makeIterator(pg, i, ge + i), false};
        cands &= cands - 1;
      }
      if (!insertPg) {
        int avail = pg->matchAvailable();
        if (avail) {
          insertPg = pg;
          insertSlot = static_cast<unsigned>(
              __builtin_ctz(static_cast<unsigned>(avail)));
        }
      }
      if (LLVM_LIKELY(pg->isNotOverflowed(h)))
        break;
      if (LLVM_UNLIKELY(++step > mask))
        break;
      pos = (pos + step) & mask;
    }

    incrementEpoch();

    if (LLVM_UNLIKELY(getNumEntries() >= getMaxLoad())) {
      unsigned ng = mask + 1u;
      growToGroups(ng == 0u ? 1u : ng * 2u);
      BucketT *p = uncheckedEmplace(h, std::forward<KeyArgT>(key),
                                    std::forward<Ts>(args)...);
      unsigned idx = static_cast<unsigned>(p - getElements());
      return {makeIterator(getGroups() + idx / GroupN, idx % GroupN, p), true};
    }

    if (LLVM_UNLIKELY(!insertPg)) {
      // No slot recorded along the chain (groups along probe were full); fall
      // back to a probing emplace which will mark overflow bits as it goes.
      BucketT *p = uncheckedEmplace(h, std::forward<KeyArgT>(key),
                                    std::forward<Ts>(args)...);
      unsigned idx = static_cast<unsigned>(p - getElements());
      return {makeIterator(getGroups() + idx / GroupN, idx % GroupN, p), true};
    }

    insertPg->set(insertSlot, h);
    BucketT *p = elems + (insertPg - groups) * GroupN + insertSlot;
    ::new (&p->getFirst()) KeyT(std::forward<KeyArgT>(key));
    ::new (&p->getSecond()) ValueT(std::forward<Ts>(args)...);
    setNumEntries(getNumEntries() + 1);
    return {makeIterator(insertPg, insertSlot, p), true};
  }

  void eraseAt(Group *pg, unsigned slot, BucketT *p) {
    incrementEpoch();
    p->getFirst().~KeyT();
    p->getSecond().~ValueT();
    pg->reset(slot);
    setNumEntries(getNumEntries() - 1u);
  }

protected:
  DenseMapBase() = default;

  void destroyAll() {
    if constexpr (std::is_trivially_destructible_v<KeyT> &&
                  std::is_trivially_destructible_v<ValueT>)
      return;
    unsigned ng = numGroups();
    if (ng == 0u || !getElements())
      return;
    for (unsigned g = 0; g < ng; g++) {
      Group *pg = getGroups() + g;
      BucketT *ge = getElements() + g * GroupN;
      int occ = pg->matchOccupied();
      while (occ) {
        unsigned i =
            static_cast<unsigned>(__builtin_ctz(static_cast<unsigned>(occ)));
        if (!(g == ng - 1u && i == GroupN - 1u)) {
          ge[i].getFirst().~KeyT();
          ge[i].getSecond().~ValueT();
        }
        occ &= occ - 1;
      }
    }
  }

  void reinitMetadata() {
    unsigned ng = numGroups();
    if (ng == 0u || !getGroups())
      return;
    for (unsigned g = 0; g < ng; g++)
      getGroups()[g].initialize();
    getGroups()[ng - 1u].setSentinel();
  }

public:
  using size_type = unsigned;
  using key_type = KeyT;
  using mapped_type = ValueT;
  using value_type = BucketT;
  using iterator = Iter;
  using const_iterator = CIter;

  iterator begin() {
    if (empty() || !getElements())
      return end();
    unsigned ng = numGroups();
    Group *groups = getGroups();
    BucketT *elems = getElements();
    for (unsigned g = 0; g < ng; g++) {
      int occ = groups[g].matchOccupied();
      if (!occ)
        continue;
      unsigned i =
          static_cast<unsigned>(__builtin_ctz(static_cast<unsigned>(occ)));
      if (g == ng - 1u && i == GroupN - 1u)
        break; // sentinel only
      return makeIterator(groups + g, i, elems + g * GroupN + i);
    }
    return end();
  }

  iterator end() { return Iter(nullptr, 0, nullptr, this); }

  const_iterator begin() const {
    if (empty() || !getElements())
      return end();
    unsigned ng = numGroups();
    const Group *groups = getGroups();
    const BucketT *elems = getElements();
    for (unsigned g = 0; g < ng; g++) {
      int occ = groups[g].matchOccupied();
      if (!occ)
        continue;
      unsigned i =
          static_cast<unsigned>(__builtin_ctz(static_cast<unsigned>(occ)));
      if (g == ng - 1u && i == GroupN - 1u)
        break;
      return makeConstIterator(groups + g, i, elems + g * GroupN + i);
    }
    return end();
  }

  const_iterator end() const { return CIter(nullptr, 0, nullptr, this); }

  bool empty() const { return getNumEntries() == 0u; }
  unsigned size() const { return getNumEntries(); }

  void reserve(size_type n) {
    if (n == 0u)
      return;
    // Need maxLoad >= n. maxLoad = (cap*7)/8, cap = ng*15-1.
    // Solve for ng: ng >= ceil((n*8/7 + 1) / 15)
    unsigned reqCap = (n * 8u + 6u) / 7u; // ceil(n / 0.875)
    unsigned reqNG = (reqCap + 1u + GroupN - 1u) / GroupN;
    if (reqNG < 1u)
      reqNG = 1u;
    if (reqNG > 1u && (reqNG & (reqNG - 1u)))
      reqNG = static_cast<unsigned>(NextPowerOf2(reqNG - 1u));
    if (reqNG <= numGroups())
      return;
    incrementEpoch();
    growToGroups(reqNG);
  }

  void clear() {
    incrementEpoch();
    if (empty())
      return;
    if (getNumEntries() * 4u < capacity() && numGroups() > 1u) {
      shrink_and_clear();
      return;
    }
    destroyAll();
    reinitMetadata();
    setNumEntries(0u);
  }

  void shrink_and_clear() {
    auto [reallocate, newNG] = derived().planShrinkAndClear();
    destroyAll();
    if (!reallocate) {
      reinitMetadata();
      setNumEntries(0u);
      return;
    }
    // Steal old allocation (without freeing), allocate new, free old.
    auto [oldGroups, oldElems, oldNG] = derived().stealAllocation();
    derived().allocateBuckets(newNG);
    // (No elements to rehash — we already destroyed them all.)
    if (oldElems)
      deallocate_buffer(oldElems, densemap_detail::bufferBytes<BucketT>(oldNG),
                        densemap_detail::bufferAlign<BucketT>());
  }

  bool contains(const_arg_type_t<KeyT> v) const {
    return doFind(v).pg != nullptr;
  }
  size_type count(const_arg_type_t<KeyT> v) const {
    return contains(v) ? 1u : 0u;
  }

  iterator find(const_arg_type_t<KeyT> v) {
    FindResult fr = doFind(v);
    if (!fr.pg)
      return end();
    return makeIterator(fr.pg, fr.slot, fr.p);
  }
  const_iterator find(const_arg_type_t<KeyT> v) const {
    FindResult fr = doFind(v);
    if (!fr.pg)
      return end();
    return makeConstIterator(fr.pg, fr.slot, fr.p);
  }

  template <class L> iterator find_as(const L &v) {
    FindResult fr = doFind(v);
    return fr.pg ? makeIterator(fr.pg, fr.slot, fr.p) : end();
  }
  template <class L> const_iterator find_as(const L &v) const {
    FindResult fr = doFind(v);
    return fr.pg ? makeConstIterator(fr.pg, fr.slot, fr.p) : end();
  }

  ValueT lookup(const_arg_type_t<KeyT> v) const {
    FindResult fr = doFind(v);
    return fr.pg ? fr.p->getSecond() : ValueT();
  }

  template <typename U = std::remove_cv_t<ValueT>>
  ValueT lookup_or(const_arg_type_t<KeyT> v, U &&def) const {
    FindResult fr = doFind(v);
    return fr.pg ? fr.p->getSecond() : ValueT(std::forward<U>(def));
  }

  ValueT &at(const_arg_type_t<KeyT> v) {
    auto it = find(v);
    assert(it != end() && "DenseMap::at failed due to a missing key");
    return it->second;
  }
  const ValueT &at(const_arg_type_t<KeyT> v) const {
    auto it = find(v);
    assert(it != end() && "DenseMap::at failed due to a missing key");
    return it->second;
  }

  std::pair<iterator, bool> insert(const std::pair<KeyT, ValueT> &kv) {
    return tryEmplaceImpl(kv.first, kv.second);
  }
  std::pair<iterator, bool> insert(std::pair<KeyT, ValueT> &&kv) {
    return tryEmplaceImpl(std::move(kv.first), std::move(kv.second));
  }

  template <typename... Ts>
  std::pair<iterator, bool> try_emplace(KeyT &&key, Ts &&...args) {
    return tryEmplaceImpl(std::move(key), std::forward<Ts>(args)...);
  }
  template <typename... Ts>
  std::pair<iterator, bool> try_emplace(const KeyT &key, Ts &&...args) {
    return tryEmplaceImpl(key, std::forward<Ts>(args)...);
  }

  template <typename L>
  std::pair<iterator, bool> insert_as(std::pair<KeyT, ValueT> &&kv,
                                      const L &lk) {
    FindResult fr = doFind(lk);
    if (fr.pg)
      return {makeIterator(fr.pg, fr.slot, fr.p), false};
    incrementEpoch();
    if (!getElements())
      derived().allocateBuckets(1u);
    else
      maybeGrow();
    std::uint64_t h = densemap_detail::mixHash(KeyInfoT::getHashValue(lk));
    BucketT *p = uncheckedEmplace(h, std::move(kv.first), std::move(kv.second));
    unsigned idx = static_cast<unsigned>(p - getElements());
    return {makeIterator(getGroups() + idx / GroupN, idx % GroupN, p), true};
  }

  template <typename It> void insert(It I, It E) {
    for (; I != E; ++I)
      insert(*I);
  }
  template <typename Range> void insert_range(Range &&R) {
    for (auto &&kv : R)
      insert(kv);
  }

  template <typename V>
  std::pair<iterator, bool> insert_or_assign(const KeyT &k, V &&v) {
    auto r = try_emplace(k, std::forward<V>(v));
    if (!r.second)
      r.first->second = std::forward<V>(v);
    return r;
  }
  template <typename V>
  std::pair<iterator, bool> insert_or_assign(KeyT &&k, V &&v) {
    auto r = try_emplace(std::move(k), std::forward<V>(v));
    if (!r.second)
      r.first->second = std::forward<V>(v);
    return r;
  }

  template <typename... Ts>
  std::pair<iterator, bool> emplace_or_assign(const KeyT &k, Ts &&...args) {
    auto r = try_emplace(k, std::forward<Ts>(args)...);
    if (!r.second)
      r.first->second = ValueT(std::forward<Ts>(args)...);
    return r;
  }
  template <typename... Ts>
  std::pair<iterator, bool> emplace_or_assign(KeyT &&k, Ts &&...args) {
    auto r = try_emplace(std::move(k), std::forward<Ts>(args)...);
    if (!r.second)
      r.first->second = ValueT(std::forward<Ts>(args)...);
    return r;
  }

  bool erase(const KeyT &v) {
    FindResult fr = doFind(v);
    if (!fr.pg)
      return false;
    eraseAt(fr.pg, fr.slot, fr.p);
    return true;
  }
  void erase(iterator I) {
    BucketT *p = &*I;
    unsigned idx = static_cast<unsigned>(p - getElements());
    eraseAt(getGroups() + idx / GroupN, idx % GroupN, p);
  }

  /// Remove entries that match the given predicate. \p Pred is invoked with a
  /// reference to each live bucket and must not access the map being modified.
  /// This is the safe replacement for erase-while-iterating.
  ///
  /// Returns whether anything was removed. If so, all iterators and references
  /// into the map are invalidated.
  template <typename Predicate> bool remove_if(Predicate Pred) {
    unsigned ng = numGroups();
    if (ng == 0u || !getElements())
      return false;
    // Erase does not relocate survivors, so we can scan the group metadata and
    // erase in place (mirrors destroyAll's iteration).
    bool Removed = false;
    for (unsigned g = 0; g < ng; ++g) {
      Group *pg = getGroups() + g;
      BucketT *ge = getElements() + g * GroupN;
      int occ = pg->matchOccupied();
      while (occ) {
        unsigned i =
            static_cast<unsigned>(__builtin_ctz(static_cast<unsigned>(occ)));
        occ &= occ - 1;
        // Skip the sentinel occupying the last group's final slot.
        if (g == ng - 1u && i == GroupN - 1u)
          continue;
        if (Pred(ge[i])) {
          eraseAt(pg, i, ge + i);
          Removed = true;
        }
      }
    }
    return Removed;
  }

  ValueT &operator[](const KeyT &k) { return try_emplace(k).first->second; }
  ValueT &operator[](KeyT &&k) {
    return try_emplace(std::move(k)).first->second;
  }

  bool isPointerIntoBucketsArray(const void *ptr) const {
    const BucketT *e = getElements();
    return e && ptr >= e && ptr < e + numGroups() * GroupN;
  }
  const void *getPointerIntoBucketsArray() const { return getElements(); }

  void swap(DerivedT &rhs) {
    incrementEpoch();
    rhs.incrementEpoch();
    derived().swapImpl(rhs);
  }

  size_t getMemorySize() const {
    unsigned ng = numGroups();
    if (!getElements())
      return 0;
    return ng * GroupN * sizeof(BucketT) + ng * sizeof(Group) + 16;
  }

  [[nodiscard]] auto keys() {
    return llvm::map_range(*this,
                           [](BucketT &P) -> KeyT & { return P.getFirst(); });
  }
  [[nodiscard]] auto values() {
    return llvm::map_range(
        *this, [](BucketT &P) -> ValueT & { return P.getSecond(); });
  }
  [[nodiscard]] auto keys() const {
    return llvm::map_range(
        *this, [](const BucketT &P) -> const KeyT & { return P.getFirst(); });
  }
  [[nodiscard]] auto values() const {
    return llvm::map_range(*this, [](const BucketT &P) -> const ValueT & {
      return P.getSecond();
    });
  }
};

// ============================================================
// Equality
// ============================================================
template <typename D, typename K, typename V, typename KI, typename B>
bool operator==(const DenseMapBase<D, K, V, KI, B> &L,
                const DenseMapBase<D, K, V, KI, B> &R) {
  if (L.size() != R.size())
    return false;
  for (auto &kv : L) {
    auto it = R.find(kv.first);
    if (it == R.end() || it->second != kv.second)
      return false;
  }
  return true;
}
template <typename D, typename K, typename V, typename KI, typename B>
bool operator!=(const DenseMapBase<D, K, V, KI, B> &L,
                const DenseMapBase<D, K, V, KI, B> &R) {
  return !(L == R);
}

// ============================================================
// DenseMap
// ============================================================
template <typename KeyT, typename ValueT,
          typename KeyInfoT = DenseMapInfo<KeyT>,
          typename BucketT = llvm::detail::DenseMapPair<KeyT, ValueT>>
class DenseMap : public DenseMapBase<DenseMap<KeyT, ValueT, KeyInfoT, BucketT>,
                                     KeyT, ValueT, KeyInfoT, BucketT> {
  using BaseT = DenseMapBase<DenseMap, KeyT, ValueT, KeyInfoT, BucketT>;
  using Group = densemap_detail::group15;
  friend class DenseMapBase<DenseMap, KeyT, ValueT, KeyInfoT, BucketT>;

  BucketT *elements_ = nullptr;
  Group *groups_ = nullptr;
  unsigned numEntries_ = 0;
  unsigned groupsMask_ = 0;
  unsigned sizeIndex_ = 63;

  void freeBuffer() {
    deallocate_buffer(elements_,
                      densemap_detail::bufferBytes<BucketT>(groupsMask_ + 1u),
                      densemap_detail::bufferAlign<BucketT>());
  }

  // CRTP accessors
  Group *getGroups() { return groups_; }
  const Group *getGroups() const { return groups_; }
  BucketT *getElements() { return elements_; }
  const BucketT *getElements() const { return elements_; }
  unsigned getSizeIndex() const { return sizeIndex_; }
  unsigned getGroupsMask() const { return groupsMask_; }
  unsigned getNumEntries() const { return numEntries_; }
  void setNumEntries(unsigned n) { numEntries_ = n; }

  // Detach current allocation and reset internal state (does NOT free).
  struct StolenAlloc {
    Group *groups;
    BucketT *elems;
    unsigned ng;
  };
  StolenAlloc stealAllocation() {
    unsigned ng = elements_ ? groupsMask_ + 1u : 0u;
    StolenAlloc s{groups_, elements_, ng};
    elements_ = nullptr;
    groups_ = nullptr;
    groupsMask_ = 0;
    sizeIndex_ = 63;
    numEntries_ = 0;
    return s;
  }

  void allocateBuckets(unsigned ng) {
    // Caller (growToGroups / shrink_and_clear) must have already stolen/freed
    // the old allocation. Internal state must be reset (stealAllocation
    // called).
    assert(!elements_ &&
           "stealAllocation must precede allocateBuckets in grow");
    if (ng == 0u)
      return;
    assert((ng & (ng - 1u)) == 0u);
    auto a = densemap_detail::allocFOA<BucketT>(ng);
    elements_ = a.elements;
    groups_ = a.groups;
    groupsMask_ = ng - 1u;
    sizeIndex_ = densemap_detail::computeSizeIndex(ng);
    numEntries_ = 0u;
    for (unsigned g = 0; g < ng; g++)
      groups_[g].initialize();
    groups_[ng - 1u].setSentinel();
  }

  void swapImpl(DenseMap &rhs) {
    std::swap(elements_, rhs.elements_);
    std::swap(groups_, rhs.groups_);
    std::swap(numEntries_, rhs.numEntries_);
    std::swap(groupsMask_, rhs.groupsMask_);
    std::swap(sizeIndex_, rhs.sizeIndex_);
  }

  std::pair<bool, unsigned> planShrinkAndClear() const {
    unsigned ng = groupsMask_ + 1u;
    if (numEntries_ == 0u)
      return ng <= 1u ? std::pair{false, 0u} : std::pair{true, 1u};
    unsigned rq = (numEntries_ + Group::N - 1u) / Group::N;
    if (rq < 1u)
      rq = 1u;
    if (rq & (rq - 1u))
      rq = static_cast<unsigned>(NextPowerOf2(rq - 1u));
    return rq == ng ? std::pair{false, 0u} : std::pair{true, rq};
  }

  void kill() {
    if (elements_) {
      freeBuffer();
      elements_ = nullptr;
      groups_ = nullptr;
    }
    groupsMask_ = 0;
    sizeIndex_ = 63;
    numEntries_ = 0;
  }

  void freeStorage() {
    if (elements_)
      freeBuffer();
    elements_ = nullptr;
    groups_ = nullptr;
    groupsMask_ = 0;
    sizeIndex_ = 63;
    numEntries_ = 0;
  }

public:
  explicit DenseMap(unsigned n = 0) {
    if (n > 0u)
      this->reserve(n);
  }
  DenseMap(const DenseMap &o) { copyFrom(o); }
  DenseMap(DenseMap &&o) noexcept { swapImpl(o); }

  template <typename It> DenseMap(const It &I, const It &E) {
    this->insert(I, E);
  }

  template <typename R> DenseMap(llvm::from_range_t, const R &r) {
    for (auto &&kv : r)
      this->insert(kv);
  }
  DenseMap(std::initializer_list<typename BaseT::value_type> v) {
    this->insert(v.begin(), v.end());
  }

  ~DenseMap() {
    this->destroyAll();
    freeStorage();
  }

  DenseMap &operator=(const DenseMap &o) {
    if (this != &o) {
      this->destroyAll();
      freeStorage();
      copyFrom(o);
    }
    return *this;
  }
  DenseMap &operator=(DenseMap &&o) noexcept {
    if (this != &o) {
      this->destroyAll();
      freeStorage();
      swapImpl(o);
    }
    return *this;
  }

private:
  void copyFrom(const DenseMap &o) {
    if (!o.elements_)
      return;
    unsigned ng = o.groupsMask_ + 1u;
    // Reset then allocate.
    allocateBuckets(ng);
    for (unsigned g = 0; g < ng; g++) {
      const Group *pg = o.groups_ + g;
      const BucketT *ge = o.elements_ + g * Group::N;
      int occ = pg->matchOccupied();
      while (occ) {
        unsigned i =
            static_cast<unsigned>(__builtin_ctz(static_cast<unsigned>(occ)));
        if (!(g == ng - 1u && i == Group::N - 1u)) {
          std::uint64_t h = densemap_detail::mixHash(
              KeyInfoT::getHashValue(ge[i].getFirst()));
          this->uncheckedEmplace(h, ge[i].getFirst(), ge[i].getSecond());
        }
        occ &= occ - 1;
      }
    }
  }
};

// SmallDenseMap is an alias for DenseMap.  The FOA implementation has no
// inline storage; the InlineBuckets template parameter is ignored and the
// first insertion allocates a single group15 (14 usable slots).
template <typename KeyT, typename ValueT, unsigned /*InlineBuckets*/ = 4,
          typename KeyInfoT = DenseMapInfo<KeyT>,
          typename BucketT = llvm::detail::DenseMapPair<KeyT, ValueT>>
using SmallDenseMap = DenseMap<KeyT, ValueT, KeyInfoT, BucketT>;

// ============================================================
// capacity_in_bytes
// ============================================================
template <typename K, typename V, typename KI>
inline size_t capacity_in_bytes(const DenseMap<K, V, KI> &X) {
  return X.getMemorySize();
}

} // namespace llvm

#endif // LLVM_ADT_DENSEMAP_H
