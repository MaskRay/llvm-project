//===- DenseMap.cpp - Shared DenseMap rehash ------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/ADT/DenseMap.h"

using namespace llvm;
using namespace llvm::densemap::detail;

namespace {

// The DenseMapInfo hashes a shared rehash can reproduce from the key bytes at
// the start of a bucket. DenseMapTest.KeyHashMatchesDenseMapInfo keeps these in
// lockstep with DenseMapInfo itself.
struct Mix64 {
  static unsigned hash(const char *Bucket) {
    uint64_t Key;
    std::memcpy(&Key, Bucket, sizeof(Key));
    return static_cast<unsigned>(mix(Key));
  }
};
struct Mul37At32 {
  static unsigned hash(const char *Bucket) {
    uint32_t Key;
    std::memcpy(&Key, Bucket, sizeof(Key));
    return Key * 37U;
  }
};
struct Mul37At64 {
  static unsigned hash(const char *Bucket) {
    uint64_t Key;
    std::memcpy(&Key, Bucket, sizeof(Key));
    return static_cast<unsigned>(Key * 37ULL);
  }
};

// BucketSize is a template parameter so the bucket copy is a couple of stores
// rather than a call to memcpy. Sharing the loop is only worthwhile if it does
// not give the saved size back in rehash time.
template <typename HashT, size_t BucketSize>
void rehashFixedSize(char *Dst, UsedT *DstUsed, unsigned Mask, const char *Src,
                     const UsedT *SrcUsed, unsigned SrcNumBuckets) {
  forEachUsed(SrcUsed, SrcNumBuckets, [&](unsigned I) {
    const char *SrcBucket = Src + static_cast<size_t>(I) * BucketSize;
    unsigned BucketNo = HashT::hash(SrcBucket) & Mask;
    while (used(DstUsed, BucketNo))
      BucketNo = (BucketNo + 1) & Mask;
    std::memcpy(Dst + static_cast<size_t>(BucketNo) * BucketSize, SrcBucket,
                BucketSize);
    setUsed(DstUsed, BucketNo);
  });
}

template <typename HashT>
void rehashAnySize(char *Dst, UsedT *DstUsed, unsigned Mask, const char *Src,
                   const UsedT *SrcUsed, unsigned SrcNumBuckets,
                   size_t BucketSize) {
  forEachUsed(SrcUsed, SrcNumBuckets, [&](unsigned I) {
    const char *SrcBucket = Src + static_cast<size_t>(I) * BucketSize;
    unsigned BucketNo = HashT::hash(SrcBucket) & Mask;
    while (used(DstUsed, BucketNo))
      BucketNo = (BucketNo + 1) & Mask;
    std::memcpy(Dst + static_cast<size_t>(BucketNo) * BucketSize, SrcBucket,
                BucketSize);
    setUsed(DstUsed, BucketNo);
  });
}

template <typename HashT>
void rehashForHash(char *Dst, UsedT *DstUsed, unsigned Mask, const char *Src,
                   const UsedT *SrcUsed, unsigned SrcNumBuckets,
                   size_t BucketSize) {
  switch (BucketSize) {
#define REHASH_CASE(N)                                                         \
  case N:                                                                      \
    return rehashFixedSize<HashT, N>(Dst, DstUsed, Mask, Src, SrcUsed,         \
                                     SrcNumBuckets);
    REHASH_CASE(4)
    REHASH_CASE(8)
    REHASH_CASE(12)
    REHASH_CASE(16)
    REHASH_CASE(24)
    REHASH_CASE(32)
    REHASH_CASE(40)
    REHASH_CASE(48)
#undef REHASH_CASE
  default:
    return rehashAnySize<HashT>(Dst, DstUsed, Mask, Src, SrcUsed, SrcNumBuckets,
                                BucketSize);
  }
}

} // namespace

void llvm::densemap::detail::rehashTrivial(
    char *Dst, UsedT *DstUsed, unsigned DstNumBuckets, const char *Src,
    const UsedT *SrcUsed, unsigned SrcNumBuckets, size_t BucketSize,
    RehashKeyHash Hash) {
  if (SrcNumBuckets == 0)
    return;
  const unsigned Mask = DstNumBuckets - 1;
  switch (Hash) {
  case RehashKeyHash::Mix64:
    return rehashForHash<Mix64>(Dst, DstUsed, Mask, Src, SrcUsed, SrcNumBuckets,
                                BucketSize);
  case RehashKeyHash::Mul37At32:
    return rehashForHash<Mul37At32>(Dst, DstUsed, Mask, Src, SrcUsed,
                                    SrcNumBuckets, BucketSize);
  case RehashKeyHash::Mul37At64:
    return rehashForHash<Mul37At64>(Dst, DstUsed, Mask, Src, SrcUsed,
                                    SrcNumBuckets, BucketSize);
  }
  llvm_unreachable("unhandled RehashKeyHash");
}
