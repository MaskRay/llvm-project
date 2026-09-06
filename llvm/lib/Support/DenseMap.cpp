//===- DenseMap.cpp - Shared DenseMap rehash ------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/ADT/DenseMap.h"
#include "llvm/Support/MemAlloc.h"
#include <cstring>

namespace llvm::densemap::detail {

// A nonzero FixedSize turns the bucket copy into a couple of stores; 0 takes
// the size from the argument.
template <size_t FixedSize>
static void rehashLoop(char *Dst, UsedT *DstUsed, unsigned Mask,
                       const char *Src, const UsedT *SrcUsed,
                       unsigned SrcNumBuckets, size_t RuntimeSize) {
  const size_t BucketSize = FixedSize ? FixedSize : RuntimeSize;
  forEachUsed(SrcUsed, SrcNumBuckets, [&](unsigned I) {
    const char *SrcBucket = Src + static_cast<size_t>(I) * BucketSize;
    // Calling DenseMapInfo keeps this in step with it. <T *> mixes a uintptr_t
    // for every T, so one copy serves every pointee type at the host's width.
    const void *Key;
    std::memcpy(&Key, SrcBucket, sizeof(Key));
    unsigned BucketNo = DenseMapInfo<const void *>::getHashValue(Key) & Mask;
    while (used(DstUsed, BucketNo))
      BucketNo = (BucketNo + 1) & Mask;
    std::memcpy(Dst + static_cast<size_t>(BucketNo) * BucketSize, SrcBucket,
                BucketSize);
    setUsed(DstUsed, BucketNo);
  });
}

void rehashPointerKeyed(char *Dst, UsedT *DstUsed, unsigned DstNumBuckets,
                        const char *Src, const UsedT *SrcUsed,
                        unsigned SrcNumBuckets, size_t BucketSize) {
  const unsigned Mask = DstNumBuckets - 1;
  // The sizes an LLVM build instantiates on a 64-bit host, where a pointer key
  // aligns the bucket to 8. Anything else takes the size at runtime rather than
  // paying for a loop each.
  switch (BucketSize) {
#define REHASH_CASE(N)                                                         \
  case N:                                                                      \
    return rehashLoop<N>(Dst, DstUsed, Mask, Src, SrcUsed, SrcNumBuckets,      \
                         BucketSize);
    REHASH_CASE(8)
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

TableRef growPointerKeyed(char *OldBuckets, unsigned OldNumBuckets,
                          unsigned MinNumBuckets, BucketTraits Traits) {
  const size_t BucketSize = Traits.Size;
  const unsigned NewNumBuckets = roundUpNumBuckets(MinNumBuckets);
  char *Storage = static_cast<char *>(
      allocate_buffer(allocBytes(BucketSize, NewNumBuckets), Traits.Align));
  UsedT *NewUsed = usedFor(Storage, BucketSize, NewNumBuckets);
  clearUsed(NewUsed, NewNumBuckets);

  if (OldNumBuckets) {
    rehashPointerKeyed(Storage, NewUsed, NewNumBuckets, OldBuckets,
                       usedFor(OldBuckets, BucketSize, OldNumBuckets),
                       OldNumBuckets, BucketSize);
    deallocate_buffer(OldBuckets, allocBytes(BucketSize, OldNumBuckets),
                      Traits.Align);
  }
  return {Storage, NewNumBuckets};
}

} // namespace llvm::densemap::detail
