//===- PerThreadBumpPtrAllocator.h ------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_SUPPORT_PERTHREADBUMPPTRALLOCATOR_H
#define LLVM_SUPPORT_PERTHREADBUMPPTRALLOCATOR_H

#include "llvm/Support/Allocator.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/raw_ostream.h"
#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

namespace llvm {
namespace parallel {

/// PerThreadAllocator wraps a possibly thread-unsafe allocator (e.g.
/// BumpPtrAllocator) to allow lock-free concurrent allocation: each thread gets
/// its own wrapped allocator on first use, and the memory is owned collectively
/// until Reset(). Unlike the previous implementation it does not use
/// parallel::getThreadIndex(), so it works on any thread (including one calling
/// a parallel algorithm that participates in the work) and does not depend on
/// the thread pool being sized ahead of time.
template <typename AllocatorTy>
class PerThreadAllocator
    : public AllocatorBase<PerThreadAllocator<AllocatorTy>> {
  // Shared state is heap-allocated so the wrapper stays movable despite holding
  // a mutex. Per-thread arena pointers (see the cache below) point into Arenas;
  // std::unique_ptr keeps those pointers stable as the vector grows.
  struct Storage {
    std::mutex Mutex;
    std::vector<std::unique_ptr<AllocatorTy>> Arenas;
  };

  // Monotonic per-AllocatorTy id source. Ids are never reused, so a stale cache
  // slot from a destroyed instance is never misattributed to a new one.
  static std::atomic<unsigned> &nextId() {
    static std::atomic<unsigned> N{0};
    return N;
  }

  /// Return this thread's arena for this instance, creating it on first use.
  AllocatorTy &getArena() {
    // One cache per AllocatorTy, keyed by instance id. Caller-agnostic: any
    // thread lazily gets its own arena; no getThreadIndex()/getThreadCount().
    static thread_local std::vector<AllocatorTy *> Cache;
    if (Id >= Cache.size())
      Cache.resize(Id + 1, nullptr);
    AllocatorTy *&A = Cache[Id];
    if (LLVM_UNLIKELY(A == nullptr)) {
      auto Owned = std::make_unique<AllocatorTy>();
      A = Owned.get();
      std::lock_guard<std::mutex> Guard(S->Mutex);
      S->Arenas.push_back(std::move(Owned));
    }
    return *A;
  }

public:
  PerThreadAllocator()
      : S(std::make_unique<Storage>()),
        Id(nextId().fetch_add(1, std::memory_order_relaxed)) {}

  /// \defgroup Methods which could be called asynchronously:
  ///
  /// @{

  using AllocatorBase<PerThreadAllocator<AllocatorTy>>::Allocate;

  using AllocatorBase<PerThreadAllocator<AllocatorTy>>::Deallocate;

  /// Allocate \a Size bytes of \a Alignment aligned memory.
  void *Allocate(size_t Size, size_t Alignment) {
    return getArena().Allocate(Size, Alignment);
  }

  /// Deallocate \a Ptr to \a Size bytes of memory allocated by this
  /// allocator.
  void Deallocate(const void *Ptr, size_t Size, size_t Alignment) {
    return getArena().Deallocate(Ptr, Size, Alignment);
  }

  /// Return allocator corresponding to the current thread.
  AllocatorTy &getThreadLocalAllocator() { return getArena(); }

  // Return number of used allocators (i.e. threads that have allocated).
  size_t getNumberOfAllocators() const {
    std::lock_guard<std::mutex> Guard(S->Mutex);
    return S->Arenas.size();
  }
  /// @}

  /// \defgroup Methods which could not be called asynchronously:
  ///
  /// @{

  /// Reset state of allocators.
  void Reset() {
    for (auto &A : S->Arenas)
      A->Reset();
  }

  /// Return total memory size used by all allocators.
  size_t getTotalMemory() const {
    size_t TotalMemory = 0;
    for (auto &A : S->Arenas)
      TotalMemory += A->getTotalMemory();
    return TotalMemory;
  }

  /// Set red zone for all allocators.
  void setRedZoneSize(size_t NewSize) {
    for (auto &A : S->Arenas)
      A->setRedZoneSize(NewSize);
  }

  /// Print statistic for each allocator.
  void PrintStats() const {
    unsigned Idx = 0;
    for (auto &A : S->Arenas) {
      errs() << "\n Allocator " << Idx++ << "\n";
      A->PrintStats();
    }
  }
  /// @}

protected:
  std::unique_ptr<Storage> S;
  // Instance id, kept in the object (not the pimpl) so the allocation fast path
  // avoids an extra indirection. Never reused across instances.
  unsigned Id;
};

using PerThreadBumpPtrAllocator = PerThreadAllocator<BumpPtrAllocator>;

} // end namespace parallel
} // end namespace llvm

#endif // LLVM_SUPPORT_PERTHREADBUMPPTRALLOCATOR_H
