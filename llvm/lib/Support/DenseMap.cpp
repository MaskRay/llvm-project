//===- DenseMap.cpp - Shared DenseMap rehash ------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/ADT/DenseMap.h"
#include <cstring>

using namespace llvm::densemap;

// A nonzero FixedSize turns the bucket copy into a couple of stores; 0 takes
// the size from the argument.
template <size_t FixedSize>
static void rehashLoop(char *Dst, detail::UsedT *DstUsed, unsigned Mask,
                       const char *Src, const detail::UsedT *SrcUsed,
                       unsigned SrcNumBuckets, size_t VarSize) {
  const size_t BucketSize = FixedSize ? FixedSize : VarSize;
  detail::forEachUsed(SrcUsed, SrcNumBuckets, [&](unsigned I) {
    const char *SrcBucket = Src + static_cast<size_t>(I) * BucketSize;
    // Calling DenseMapInfo keeps this in step with it. <T *> mixes a uintptr_t
    // for every T, so one copy serves every pointee type at the host's width.
    const void *Key;
    std::memcpy(&Key, SrcBucket, sizeof(Key));
    unsigned BucketNo =
        llvm::DenseMapInfo<const void *>::getHashValue(Key) & Mask;
    while (detail::used(DstUsed, BucketNo))
      BucketNo = (BucketNo + 1) & Mask;
    std::memcpy(Dst + static_cast<size_t>(BucketNo) * BucketSize, SrcBucket,
                BucketSize);
    detail::setUsed(DstUsed, BucketNo);
  });
}

void detail::rehashPointerKeyed(char *Dst, detail::UsedT *DstUsed,
                                unsigned DstNumBuckets, const char *Src,
                                const detail::UsedT *SrcUsed,
                                unsigned SrcNumBuckets, size_t BucketSize) {
  if (SrcNumBuckets == 0)
    return;
  const unsigned Mask = DstNumBuckets - 1;
  // The sizes an LLVM build instantiates; the rest take the size at runtime
  // rather than paying for a loop each.
  switch (BucketSize) {
#define REHASH_CASE(N)                                                         \
  case N:                                                                      \
    return rehashLoop<N>(Dst, DstUsed, Mask, Src, SrcUsed, SrcNumBuckets, N);
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
    return rehashLoop<0>(Dst, DstUsed, Mask, Src, SrcUsed, SrcNumBuckets,
                         BucketSize);
  }
}
