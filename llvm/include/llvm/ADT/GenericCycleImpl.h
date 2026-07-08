//===- GenericCycleImpl.h -------------------------------------*- C++ -*---===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// This template implementation resides in a separate file so that it
/// does not get injected into every .cpp file that includes the
/// generic header.
///
/// DO NOT INCLUDE THIS FILE WHEN MERELY USING CYCLEINFO.
///
/// This file should only be included by files that implement a
/// specialization of the relevant templates. Currently these are:
/// - llvm/lib/IR/CycleInfo.cpp
/// - llvm/lib/CodeGen/MachineCycleAnalysis.cpp
///
//===----------------------------------------------------------------------===//

#ifndef LLVM_ADT_GENERICCYCLEIMPL_H
#define LLVM_ADT_GENERICCYCLEIMPL_H

#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/DepthFirstIterator.h"
#include "llvm/ADT/GenericCycleInfo.h"
#include "llvm/ADT/StringExtras.h"

#define DEBUG_TYPE "generic-cycle-impl"

namespace llvm {

template <typename ContextT>
bool GenericCycle<ContextT>::contains(const GenericCycle *C) const {
  if (!C)
    return false;

  if (Depth > C->Depth)
    return false;
  while (Depth < C->Depth)
    C = C->ParentCycle;
  return this == C;
}

template <typename ContextT>
bool GenericCycle<ContextT>::contains(const BlockT *Block) const {
  assert(CI && "cycle is not associated with a GenericCycleInfo");
  // Block is in this cycle iff its innermost cycle is this cycle or nested in
  // it.
  return contains(CI->getCycle(Block));
}

template <typename ContextT>
void GenericCycle<ContextT>::getExitBlocks(
    SmallVectorImpl<BlockT *> &TmpStorage) const {
  if (!ExitBlocksCache.empty()) {
    TmpStorage.append(ExitBlocksCache.begin(), ExitBlocksCache.end());
    return;
  }

  size_t NumExitBlocks = 0;
  for (BlockT *Block : blocks()) {
    llvm::append_range(ExitBlocksCache, successors(Block));

    for (size_t Idx = NumExitBlocks, End = ExitBlocksCache.size(); Idx < End;
         ++Idx) {
      BlockT *Succ = ExitBlocksCache[Idx];
      if (!contains(Succ)) {
        auto ExitEndIt = ExitBlocksCache.begin() + NumExitBlocks;
        if (std::find(ExitBlocksCache.begin(), ExitEndIt, Succ) == ExitEndIt)
          ExitBlocksCache[NumExitBlocks++] = Succ;
      }
    }

    ExitBlocksCache.resize(NumExitBlocks);
  }

  TmpStorage.append(ExitBlocksCache.begin(), ExitBlocksCache.end());
}

template <typename ContextT>
void GenericCycle<ContextT>::getExitingBlocks(
    SmallVectorImpl<BlockT *> &TmpStorage) const {
  for (BlockT *Block : blocks()) {
    for (BlockT *Succ : successors(Block)) {
      if (!contains(Succ)) {
        TmpStorage.push_back(Block);
        break;
      }
    }
  }
}

template <typename ContextT>
auto GenericCycle<ContextT>::getCyclePreheader() const -> BlockT * {
  BlockT *Predecessor = getCyclePredecessor();
  if (!Predecessor)
    return nullptr;

  assert(isReducible() && "Cycle Predecessor must be in a reducible cycle!");

  if (succ_size(Predecessor) != 1)
    return nullptr;

  // Make sure we are allowed to hoist instructions into the predecessor.
  if (!Predecessor->isLegalToHoistInto())
    return nullptr;

  return Predecessor;
}

template <typename ContextT>
auto GenericCycle<ContextT>::getCyclePredecessor() const -> BlockT * {
  if (!isReducible())
    return nullptr;

  BlockT *Out = nullptr;

  // Loop over the predecessors of the header node...
  BlockT *Header = getHeader();
  for (const auto Pred : predecessors(Header)) {
    if (!contains(Pred)) {
      if (Out && Out != Pred)
        return nullptr;
      Out = Pred;
    }
  }

  return Out;
}

/// \brief Verify that this is actually a well-formed cycle in the CFG.
template <typename ContextT> void GenericCycle<ContextT>::verifyCycle() const {
#ifndef NDEBUG
  assert(!Blocks.empty() && "Cycle cannot be empty.");
  DenseSet<BlockT *> Blocks;
  for (BlockT *BB : blocks()) {
    assert(Blocks.insert(BB).second); // duplicates in block list?
  }
  assert(!Entries.empty() && "Cycle must have one or more entries.");

  DenseSet<BlockT *> Entries;
  for (BlockT *Entry : entries()) {
    assert(Entries.insert(Entry).second); // duplicate entry?
    assert(contains(Entry));
  }

  // Setup for using a depth-first iterator to visit every block in the cycle.
  SmallVector<BlockT *, 8> ExitBBs;
  getExitBlocks(ExitBBs);
  df_iterator_default_set<BlockT *> VisitSet;
  VisitSet.insert(ExitBBs.begin(), ExitBBs.end());

  // Keep track of the BBs visited.
  SmallPtrSet<BlockT *, 8> VisitedBBs;

  // Check the individual blocks.
  for (BlockT *BB : depth_first_ext(getHeader(), VisitSet)) {
    assert(llvm::any_of(llvm::children<BlockT *>(BB),
                        [&](BlockT *B) { return contains(B); }) &&
           "Cycle block has no in-cycle successors!");

    assert(llvm::any_of(llvm::inverse_children<BlockT *>(BB),
                        [&](BlockT *B) { return contains(B); }) &&
           "Cycle block has no in-cycle predecessors!");

    DenseSet<BlockT *> OutsideCyclePreds;
    for (BlockT *B : llvm::inverse_children<BlockT *>(BB))
      if (!contains(B))
        OutsideCyclePreds.insert(B);

    if (Entries.contains(BB)) {
      assert(!OutsideCyclePreds.empty() && "Entry is unreachable!");
    } else if (!OutsideCyclePreds.empty()) {
      // A non-entry block shouldn't be reachable from outside the cycle,
      // though it is permitted if the predecessor is not itself actually
      // reachable.
      BlockT *EntryBB = &BB->getParent()->front();
      for (BlockT *CB : depth_first(EntryBB))
        assert(!OutsideCyclePreds.contains(CB) &&
               "Non-entry block reachable from outside!");
    }
    assert(BB != &getHeader()->getParent()->front() &&
           "Cycle contains function entry block!");

    VisitedBBs.insert(BB);
  }

  if (VisitedBBs.size() != getNumBlocks()) {
    dbgs() << "The following blocks are unreachable in the cycle:\n  ";
    ListSeparator LS;
    for (auto *BB : Blocks) {
      if (!VisitedBBs.count(BB)) {
        dbgs() << LS;
        BB->printAsOperand(dbgs());
      }
    }
    dbgs() << "\n";
    llvm_unreachable("Unreachable block in cycle");
  }

  verifyCycleNest();
#endif
}

/// \brief Verify the parent-child relations of this cycle.
///
/// Note that this does \em not check that cycle is really a cycle in the CFG.
template <typename ContextT>
void GenericCycle<ContextT>::verifyCycleNest() const {
#ifndef NDEBUG
  // Check the subcycles.
  for (GenericCycle *Child : children()) {
    // Each block in each subcycle should be contained within this cycle.
    for (BlockT *BB : Child->blocks()) {
      assert(contains(BB) &&
             "Cycle does not contain all the blocks of a subcycle!");
    }
    assert(Child->Depth == Depth + 1);
  }

  // Check the parent cycle pointer.
  if (ParentCycle) {
    assert(is_contained(ParentCycle->children(), this) &&
           "Cycle is not a subcycle of its parent!");
    assert(ParentCycle->TopLevelCycle == TopLevelCycle &&
           "Top level cycle of parent cycle must be the same");
  } else {
    assert(TopLevelCycle == this &&
           "Cycle without parent must be top-level cycle");
  }
#endif
}

/// \brief Helper class for computing cycle information.
template <typename ContextT> class GenericCycleInfoCompute {
  using BlockT = typename ContextT::BlockT;
  using FunctionT = typename ContextT::FunctionT;
  using CycleInfoT = GenericCycleInfo<ContextT>;
  using CycleT = typename CycleInfoT::CycleT;

  CycleInfoT &Info;

  // Per-block state indexed by block number. The cycle computation follows the
  // single-pass DFS loop-identification algorithm of Wei et al., "A New
  // Algorithm for Identifying Loops in Decompilation" (SAS 2007), which tags
  // every block with its innermost loop header on the fly. The fields are
  // packed into one struct (as in tpde's Analyzer) so the DFS and header
  // tagging, which touch several of them per block, hit a single cache line.
  struct BlockInfo {
    BlockT *ILoopHeader =
        nullptr;            // Innermost loop header, null if in no cycle.
    unsigned DFSPPos = 0;   // 1-based position on the current DFS path;
                            // 0 once the block leaves it.
    bool Traversed = false; // Block has been visited by the DFS.
    bool SelfLoop = false;  // Block has an edge to itself.
  };
  SmallVector<BlockInfo, 8> BlockInfos;
  SmallVector<BlockT *, 8> Preorder; // Blocks in DFS preorder.

  GenericCycleInfoCompute(const GenericCycleInfoCompute &) = delete;
  GenericCycleInfoCompute &operator=(const GenericCycleInfoCompute &) = delete;

  static unsigned num(const BlockT *B) {
    return GraphTraits<const BlockT *>::getNumber(B);
  }

  BlockInfo &info(const BlockT *B) { return BlockInfos[num(B)]; }

  /// Weave loop header \p H (and its own header chain) into the loop header
  /// chain of \p B. Each node's chain (B, ILoopHeader(B), ...) is a singly
  /// linked list of its loop headers, sorted innermost-first by DFS-path
  /// position; this is an in-place merge of B's chain and H's chain, replacing
  /// the UNION-FIND merge of the classical Havlak algorithm. Both chains are
  /// paths toward the root of the loop-nesting forest and share an outer
  /// suffix, so the merge is done once \p B reaches the pending head \p H
  /// (which also makes the degenerate B == H a no-op). Invariant: DFSPPos(B) >=
  /// DFSPPos(H).
  void tagLoopHeader(BlockT *B, BlockT *H) {
    if (!H)
      return;
    while (B != H) {
      BlockT *IH = info(B).ILoopHeader;
      if (!IH) {
        info(B).ILoopHeader = H; // B's chain ended: append the rest of H's.
        return;
      }
      if (info(IH).DFSPPos >= info(H).DFSPPos) {
        B = IH; // IH is the inner one: keep it and walk on.
      } else {
        info(B).ILoopHeader =
            H; // H is inner: splice it in, then walk H's list,
        B = H; // with the displaced IH now pending.
        H = IH;
      }
    }
  }

public:
  GenericCycleInfoCompute(CycleInfoT &Info) : Info(Info) {}

  void run(FunctionT *F);

  static void updateDepth(CycleT *SubTree);

private:
  void dfs(BlockT *EntryBlock);
};

template <typename ContextT>
auto GenericCycleInfo<ContextT>::getTopLevelParentCycle(
    const BlockT *Block) const -> CycleT * {
  CycleT *Cycle = getCycle(Block);
  return Cycle ? Cycle->TopLevelCycle : nullptr;
}

template <typename ContextT>
void GenericCycleInfo<ContextT>::moveTopLevelCycleToNewParent(CycleT *NewParent,
                                                              CycleT *Child) {
  assert((!Child->ParentCycle && !NewParent->ParentCycle) &&
         "NewParent and Child must be both top level cycle!\n");
  auto &CurrentContainer =
      Child->ParentCycle ? Child->ParentCycle->Children : TopLevelCycles;
  auto Pos = llvm::find_if(CurrentContainer, [=](const auto &Ptr) -> bool {
    return Child == Ptr.get();
  });
  assert(Pos != CurrentContainer.end());
  NewParent->Children.push_back(std::move(*Pos));
  *Pos = std::move(CurrentContainer.back());
  CurrentContainer.pop_back();
  Child->ParentCycle = NewParent;
  Child->TopLevelCycle = NewParent;
  for (CycleT *Cycle : depth_first(Child))
    Cycle->TopLevelCycle = NewParent;

  llvm::append_range(NewParent->Blocks, Child->blocks());
  NewParent->clearCache();
  Child->clearCache();
}

template <typename ContextT>
void GenericCycleInfo<ContextT>::verifyBlockNumberEpoch(
    const FunctionT *Fn) const {
  assert(BlockNumberEpoch ==
             GraphTraits<const FunctionT *>::getNumberEpoch(Fn) &&
         "CycleInfo used with outdated block number epoch");
}

template <typename ContextT>
void GenericCycleInfo<ContextT>::addToBlockMap(BlockT *Block, CycleT *Cycle) {
  // The caller should ensure that BlockMap is large enough.
  verifyBlockNumberEpoch(Block->getParent());
  unsigned Number = GraphTraits<BlockT *>::getNumber(Block);
  BlockMap[Number] = Cycle;
}

template <typename ContextT>
void GenericCycleInfo<ContextT>::addBlockToCycle(BlockT *Block, CycleT *Cycle) {
  // Make sure BlockMap is large enough for the new block.
  unsigned Number = GraphTraits<BlockT *>::getNumber(Block);
  if (Number >= BlockMap.size())
    BlockMap.resize(GraphTraits<FunctionT *>::getMaxNumber(Block->getParent()));

  // FixMe: Appending NewBlock is fine as a set of blocks in a cycle. When
  // printing, cycle NewBlock is at the end of list but it should be in the
  // middle to represent actual traversal of a cycle.
  Cycle->appendBlock(Block);
  addToBlockMap(Block, Cycle);

  CycleT *ParentCycle = Cycle->getParentCycle();
  while (ParentCycle) {
    Cycle = ParentCycle;
    Cycle->appendBlock(Block);
    ParentCycle = Cycle->getParentCycle();
  }

  Cycle->clearCache();
}

/// \brief Main function of the cycle info computations.
template <typename ContextT>
void GenericCycleInfoCompute<ContextT>::run(FunctionT *F) {
  BlockT *EntryBlock = GraphTraits<FunctionT *>::getEntryNode(F);
  LLVM_DEBUG(errs() << "Entry block: " << Info.Context.print(EntryBlock)
                    << "\n");

  unsigned MaxNumber = GraphTraits<FunctionT *>::getMaxNumber(F);
  BlockInfos.assign(MaxNumber, BlockInfo{});

  // Single depth-first traversal that tags every block with its innermost loop
  // header (ILoopHeader) on the fly.
  dfs(EntryBlock);

  // A block is a loop header if it is the innermost loop header of some block,
  // or if it has a self-edge. Create one cycle per header, keyed by block
  // number. Cycles are created in preorder of their headers.
  SmallVector<CycleT *, 8> HeaderToCycle(MaxNumber, nullptr);
  SmallVector<std::unique_ptr<CycleT>, 8> Cycles;

  auto getOrCreateCycle = [&](BlockT *Header) {
    CycleT *&C = HeaderToCycle[num(Header)];
    if (!C) {
      Cycles.push_back(std::make_unique<CycleT>());
      C = Cycles.back().get();
      C->CI = &Info;
      C->appendEntry(Header); // The header is always an entry.
      LLVM_DEBUG(errs() << "Found cycle for header: "
                        << Info.Context.print(Header) << "\n");
    }
  };

  for (BlockT *Block : Preorder) {
    if (BlockT *Header = info(Block).ILoopHeader)
      getOrCreateCycle(Header);
    if (info(Block).SelfLoop)
      getOrCreateCycle(Block);
  }

  // The innermost cycle containing \p B: the cycle B heads if B is itself a
  // header, otherwise the cycle headed by B's innermost loop header.
  auto innermostHeader = [&](BlockT *B) -> BlockT * {
    if (HeaderToCycle[num(B)])
      return B;
    return info(B).ILoopHeader;
  };

  // Populate each cycle's block set (which includes the blocks of nested
  // cycles) and the block-to-innermost-cycle map. Iterating in preorder makes
  // each cycle's header its first block.
  for (BlockT *Block : Preorder) {
    BlockT *H = innermostHeader(Block);
    if (!H)
      continue;
    Info.BlockMap[num(Block)] = HeaderToCycle[num(H)];
    for (BlockT *C = H; C; C = info(C).ILoopHeader)
      HeaderToCycle[num(C)]->appendBlock(Block);
  }

  // Wire up the cycle forest. A cycle headed by H is nested in the cycle headed
  // by ILoopHeader[H]; otherwise it is a top-level cycle. Transferring
  // ownership in reverse preorder (children before parents) keeps the top-level
  // and child lists in reverse preorder of their headers.
  for (auto &UP : Cycles)
    if (BlockT *ParentHeader = info(UP->getHeader()).ILoopHeader)
      UP->ParentCycle = HeaderToCycle[num(ParentHeader)];
  for (std::unique_ptr<CycleT> &UP : llvm::reverse(Cycles)) {
    CycleT *C = UP.get();
    if (C->ParentCycle)
      C->ParentCycle->Children.push_back(std::move(UP));
    else
      Info.TopLevelCycles.push_back(std::move(UP));
  }

  // Set the top-level-cycle back-pointers and compute cycle depths.
  for (auto *TLC : Info.toplevel_cycles()) {
    LLVM_DEBUG(errs() << "top-level cycle: "
                      << Info.Context.print(TLC->getHeader()) << "\n");
    TLC->ParentCycle = nullptr;
    for (CycleT *C : depth_first(TLC)) {
      C->TopLevelCycle = TLC;
      C->Depth = C->ParentCycle ? C->ParentCycle->Depth + 1 : 1;
    }
  }

  // Compute the entries of each cycle. A block B in a cycle C is an entry iff
  // it is the header or it has a reachable predecessor outside C. Cycle depths
  // are set above, so containment is answered through the innermost-cycle map.
  // Walking B's header chain from innermost outward, an edge from Pred makes B
  // an entry of every cycle up to (but excluding) the first one that contains
  // Pred.
  for (BlockT *Block : Preorder) {
    BlockT *H = innermostHeader(Block);
    if (!H)
      continue;
    for (BlockT *Pred : predecessors(Block)) {
      if (!info(Pred).Traversed)
        continue; // Ignore unreachable predecessors.
      CycleT *PredCycle = Info.getCycle(Pred);
      for (BlockT *C = H; C; C = info(C).ILoopHeader) {
        CycleT *Cycle = HeaderToCycle[num(C)];
        if (Cycle->contains(PredCycle))
          break;
        if (!Cycle->isEntry(Block))
          Cycle->appendEntry(Block);
      }
    }
  }
}

/// \brief Recompute depth values of \p SubTree and all descendants.
template <typename ContextT>
void GenericCycleInfoCompute<ContextT>::updateDepth(CycleT *SubTree) {
  for (CycleT *Cycle : depth_first(SubTree))
    Cycle->Depth = Cycle->ParentCycle ? Cycle->ParentCycle->Depth + 1 : 1;
}

/// \brief Single-pass DFS that tags every block with its innermost loop header.
///
/// This is an iterative rendering of \c trav_loops_DFS from Wei et al. Each
/// block \p B0 in the DFS classifies each successor \p B1 as:
///   - a tree edge (B1 unvisited): recurse, then propagate B1's loop header;
///   - a back edge (B1 on the current DFS path): B1 is a loop header of B0;
///   - a re-entry into an already-finished loop (irreducible control flow):
///     walk B1's header chain to the nearest header still on the DFS path.
/// Forward/cross edges into loop-free code are ignored. Fills ILoopHeader,
/// SelfLoop and Preorder.
template <typename ContextT>
void GenericCycleInfoCompute<ContextT>::dfs(BlockT *EntryBlock) {
  // Successors are visited in reverse order to match the historical
  // stack-based traversal order (and thus the cycle print order). The
  // successor iterators remain valid on their own, so they are stored in the
  // frame directly rather than copying the successor list into it.
  using SuccIterT = decltype(successors(std::declval<BlockT *>()).begin());
  struct Frame {
    BlockT *Block;
    std::reverse_iterator<SuccIterT> SuccIt;
    std::reverse_iterator<SuccIterT> SuccEnd;
  };
  SmallVector<Frame, 8> Stack;
  unsigned Counter = 0;

  auto pushNode = [&](BlockT *Block) {
    LLVM_DEBUG(errs() << "DFS visiting block: " << Info.Context.print(Block)
                      << "\n");
    BlockInfo &BI = info(Block);
    BI.Traversed = true;
    BI.DFSPPos = ++Counter;
    Preorder.push_back(Block);
    auto Succs = successors(Block);
    Stack.push_back(Frame{Block, std::make_reverse_iterator(Succs.end()),
                          std::make_reverse_iterator(Succs.begin())});
  };

  pushNode(EntryBlock);
  while (!Stack.empty()) {
    Frame &F = Stack.back();
    if (F.SuccIt != F.SuccEnd) {
      BlockT *B0 = F.Block;
      BlockT *B1 = *F.SuccIt++;
      if (B1 == B0)
        info(B0).SelfLoop = true;
      BlockInfo &B1Info = info(B1);
      if (!B1Info.Traversed) {
        // Tree edge: descend. Propagation happens when B1's frame is popped.
        pushNode(B1);
      } else if (B1Info.DFSPPos > 0) {
        // Back edge: B1 is an ancestor on the DFS path, hence a loop header.
        tagLoopHeader(B0, B1);
      } else if (BlockT *H = B1Info.ILoopHeader) {
        // B1 is finished but belongs to a loop: re-entry / cross edge into an
        // already-closed (possibly irreducible) loop. Tag B0 with the nearest
        // header in B1's chain still on the DFS path; tagLoopHeader is a no-op
        // if the walk runs off the end (H == nullptr).
        while (H && info(H).DFSPPos == 0)
          H = info(H).ILoopHeader;
        tagLoopHeader(B0, H);
      }
    } else {
      // Finished B0: leave the DFS path and propagate its loop header to the
      // parent (the tree-edge tagging of Wei's algorithm).
      BlockT *B0 = F.Block;
      info(B0).DFSPPos = 0;
      Stack.pop_back();
      if (!Stack.empty())
        tagLoopHeader(Stack.back().Block, info(B0).ILoopHeader);
    }
  }
}

/// \brief Reset the object to its initial state.
template <typename ContextT> void GenericCycleInfo<ContextT>::clear() {
  TopLevelCycles.clear();
  BlockMap.clear();
}

/// \brief Compute the cycle info for a function.
template <typename ContextT>
void GenericCycleInfo<ContextT>::compute(FunctionT &F) {
  GenericCycleInfoCompute<ContextT> Compute(*this);
  Context = ContextT(&F);
  BlockNumberEpoch = GraphTraits<FunctionT *>::getNumberEpoch(&F);
  BlockMap.resize(GraphTraits<FunctionT *>::getMaxNumber(&F));

  LLVM_DEBUG(errs() << "Computing cycles for function: " << F.getName()
                    << "\n");
  Compute.run(&F);
}

template <typename ContextT>
void GenericCycleInfo<ContextT>::splitCriticalEdge(BlockT *Pred, BlockT *Succ,
                                                   BlockT *NewBlock) {
  // Edge Pred-Succ is replaced by edges Pred-NewBlock and NewBlock-Succ, all
  // cycles that had blocks Pred and Succ also get NewBlock.
  CycleT *Cycle = getSmallestCommonCycle(getCycle(Pred), getCycle(Succ));
  if (!Cycle)
    return;

  addBlockToCycle(NewBlock, Cycle);
  verifyCycleNest();
}

/// \brief Find the innermost cycle containing a given block.
///
/// \returns the innermost cycle containing \p Block or nullptr if
///          it is not contained in any cycle.
template <typename ContextT>
auto GenericCycleInfo<ContextT>::getCycle(const BlockT *Block) const
    -> CycleT * {
  verifyBlockNumberEpoch(Block->getParent());
  unsigned Number = GraphTraits<const BlockT *>::getNumber(Block);
  return Number < BlockMap.size() ? BlockMap[Number] : nullptr;
}

/// \brief Find the innermost cycle containing both given cycles.
///
/// \returns the innermost cycle containing both \p A and \p B
///          or nullptr if there is no such cycle.
template <typename ContextT>
auto GenericCycleInfo<ContextT>::getSmallestCommonCycle(CycleT *A,
                                                        CycleT *B) const
    -> CycleT * {
  if (!A || !B)
    return nullptr;

  // If cycles A and B have different depth replace them with parent cycle
  // until they have the same depth.
  while (A->getDepth() > B->getDepth())
    A = A->getParentCycle();
  while (B->getDepth() > A->getDepth())
    B = B->getParentCycle();

  // Cycles A and B are at same depth but may be disjoint, replace them with
  // parent cycles until we find cycle that contains both or we run out of
  // parent cycles.
  while (A != B) {
    A = A->getParentCycle();
    B = B->getParentCycle();
  }

  return A;
}

/// \brief Find the innermost cycle containing both given blocks.
///
/// \returns the innermost cycle containing both \p A and \p B
///          or nullptr if there is no such cycle.
template <typename ContextT>
auto GenericCycleInfo<ContextT>::getSmallestCommonCycle(BlockT *A,
                                                        BlockT *B) const
    -> CycleT * {
  return getSmallestCommonCycle(getCycle(A), getCycle(B));
}

/// \brief get the depth for the cycle which containing a given block.
///
/// \returns the depth for the innermost cycle containing \p Block or 0 if it is
///          not contained in any cycle.
template <typename ContextT>
unsigned GenericCycleInfo<ContextT>::getCycleDepth(const BlockT *Block) const {
  CycleT *Cycle = getCycle(Block);
  if (!Cycle)
    return 0;
  return Cycle->getDepth();
}

/// \brief Verify the internal consistency of the cycle tree.
///
/// Note that this does \em not check that cycles are really cycles in the CFG,
/// or that the right set of cycles in the CFG were found.
template <typename ContextT>
void GenericCycleInfo<ContextT>::verifyCycleNest(bool VerifyFull) const {
#ifndef NDEBUG
  DenseSet<BlockT *> CycleHeaders;

  for (CycleT *TopCycle : toplevel_cycles()) {
    for (CycleT *Cycle : depth_first(TopCycle)) {
      BlockT *Header = Cycle->getHeader();
      assert(CycleHeaders.insert(Header).second);
      if (VerifyFull)
        Cycle->verifyCycle();
      else
        Cycle->verifyCycleNest();
      // Check the block map entries for blocks contained in this cycle.
      for (BlockT *BB : Cycle->blocks()) {
        CycleT *CycleInBlockMap = getCycle(BB);
        assert(CycleInBlockMap != nullptr);
        assert(Cycle->contains(CycleInBlockMap));
      }
    }
  }
#endif
}

/// \brief Verify that the entire cycle tree well-formed.
template <typename ContextT> void GenericCycleInfo<ContextT>::verify() const {
  verifyCycleNest(/*VerifyFull=*/true);
}

/// \brief Print the cycle info.
template <typename ContextT>
void GenericCycleInfo<ContextT>::print(raw_ostream &Out) const {
  for (const auto *TLC : toplevel_cycles()) {
    for (const CycleT *Cycle : depth_first(TLC)) {
      for (unsigned I = 0; I < Cycle->Depth; ++I)
        Out << "    ";

      Out << Cycle->print(Context) << '\n';
    }
  }
}

} // namespace llvm

#undef DEBUG_TYPE

#endif // LLVM_ADT_GENERICCYCLEIMPL_H
