//===- RegAllocFast.cpp - A fast register allocator for debug code --------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
/// \file This register allocator allocates registers to a basic block at a
/// time, attempting to keep values in registers and reusing registers as
/// appropriate.
///
/// The allocator runs in two phases: an analysis prepass records per-virtual-
/// register facts (last-use position, cross-block liveness, call crossings,
/// copy hints) in one walk, and the allocation pass walks each block forward,
/// reloading lazily at uses and freeing registers at exact kill positions.
/// Values live across blocks are kept in stack slots at block boundaries;
/// values live across calls prefer callee-saved registers.
//
//===----------------------------------------------------------------------===//

#include "llvm/CodeGen/RegAllocFast.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/BitVector.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/IndexedMap.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/SparseSet.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineOperand.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/RegAllocCommon.h"
#include "llvm/CodeGen/RegAllocRegistry.h"
#include "llvm/CodeGen/RegisterClassInfo.h"
#include "llvm/CodeGen/TargetInstrInfo.h"
#include "llvm/CodeGen/TargetOpcodes.h"
#include "llvm/CodeGen/TargetRegisterInfo.h"
#include "llvm/CodeGen/TargetSubtargetInfo.h"
#include "llvm/InitializePasses.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/Pass.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"
#include <cassert>
#include <tuple>
#include <vector>

using namespace llvm;

#define DEBUG_TYPE "regalloc"

static cl::opt<bool> PreferCSRForCallCrossing(
    "fast-ra-prefer-csr",
    cl::desc("Prefer callee-saved registers for values that live across a "
             "call in their block"),
    cl::init(true), cl::Hidden);

STATISTIC(NumStores, "Number of stores added");
STATISTIC(NumLoads, "Number of loads added");
STATISTIC(NumCoalesced, "Number of copies coalesced");

static RegisterRegAlloc fastRegAlloc("fast", "fast register allocator",
                                     createFastRegisterAllocator);

namespace {

class RegAllocFastImpl {
public:
  RegAllocFastImpl(const RegAllocFilterFunc F = nullptr,
                   bool ClearVirtRegs_ = true)
      : ShouldAllocateRegisterImpl(F), StackSlotForVirtReg(-1),
        ClearVirtRegs(ClearVirtRegs_) {}

private:
  MachineFrameInfo *MFI = nullptr;
  MachineRegisterInfo *MRI = nullptr;
  const TargetRegisterInfo *TRI = nullptr;
  const TargetInstrInfo *TII = nullptr;
  RegisterClassInfo RegClassInfo;
  const RegAllocFilterFunc ShouldAllocateRegisterImpl;

  /// Basic block currently being allocated.
  MachineBasicBlock *MBB = nullptr;

  /// Maps virtual regs to the frame index where these values are spilled.
  IndexedMap<int, VirtReg2IndexFunctor> StackSlotForVirtReg;

  /// Per-virtual-register facts computed by the analysis prepass. Positions
  /// count non-debug instructions in layout order, starting at 1.
  struct VRegInfo {
    uint32_t LastPos = 0; ///< Position of the last def or use.
    int DefBlock = -1;    ///< Block number of the first def.
    int LastBlock = -1;   ///< Prepass scratch: block of the last event.
    MCPhysReg HintReg = 0; ///< Physical register copy hint.
    Register CopySrc;     ///< Prepass scratch: virtual COPY source.
    bool LiveCrossBlock = false; ///< Live across blocks; stack-homed.
    bool CrossesCall = false; ///< Some same-block segment crosses a call.
  };
  SmallVector<VRegInfo, 0> VRegInfos;

  /// Prepass scratch: per-unit position of the last use in the current live
  /// segment of a physical register.
  SmallVector<uint32_t, 0> LastPhysUsePos;

  /// Prepass scratch: units with a use in their current live segment.
  SmallVector<unsigned, 32> ActivePhysUnits;

  /// Prepass scratch: per-unit position of the last physreg def; units with
  /// a pending def are listed in ActiveDefUnits (which may contain duplicates
  /// and stale entries; a zero LastPhysDefPos identifies the latter).
  SmallVector<uint32_t, 0> LastPhysDefPos;
  SmallVector<unsigned, 32> ActiveDefUnits;

  /// Sorted (def position << 32 | unit) keys for physreg defs whose value is
  /// read later; defs not in this set are dead (at -O0 nothing computes dead
  /// flags, so this inference replaces them).
  SmallVector<uint64_t, 0> PhysDefsWithUse;

  /// Sorted (position << 32 | unit) keys marking the final reading operand
  /// of each physical register live segment; used to release pre-assigned
  /// registers at their final reader.
  SmallVector<uint64_t, 0> PhysSegmentEnds;

  /// Cursors into PhysSegmentEnds/PhysDefsWithUse; queries arrive in
  /// nondecreasing CurPos order, so each lookup is an amortized O(1) scan
  /// of the entries at CurPos.
  unsigned SegEndCursor = 0;
  unsigned DefUseCursor = 0;

  /// Pre-assigned registers consumed by their final reader in the current
  /// instruction, released once the instruction is fully processed.
  SmallVector<MCRegister, 2> ConsumedPreassigned;

  /// Units that were made pre-assigned in the current block; may contain
  /// stale entries (the unit state is authoritative).
  SmallVector<unsigned, 16> PreassignedUnits;

  VRegInfo &vregInfo(Register VirtReg) {
    return VRegInfos[VirtReg.virtRegIndex()];
  }
  const VRegInfo &vregInfo(Register VirtReg) const {
    return VRegInfos[VirtReg.virtRegIndex()];
  }

  bool posKeyContains(ArrayRef<uint64_t> Keys, unsigned &Cursor,
                      unsigned UnitIdx) const {
    while (Cursor != Keys.size() && (Keys[Cursor] >> 32) < CurPos)
      ++Cursor;
    for (unsigned I = Cursor, E = Keys.size();
         I != E && (Keys[I] >> 32) == CurPos; ++I)
      if (static_cast<uint32_t>(Keys[I]) == UnitIdx)
        return true;
    return false;
  }

  /// Position of the instruction currently being allocated; matches the
  /// prepass numbering.
  uint32_t CurPos = 0;

  /// Monotonic sequence number bumped on every register unit state change;
  /// used (only when debug info is present) to validate last-known value
  /// locations for DBG_VALUE rewriting.
  uint32_t StateSeq = 0;
  uint32_t BlockStartSeq = 0;
  bool TrackDbgLoc = false;
  /// Per-unit sequence of the last state change.
  SmallVector<uint32_t, 0> UnitChangeSeq;
  /// Per-vreg last register location and the sequence at which it was freed.
  SmallVector<std::pair<MCPhysReg, uint32_t>, 0> VRegLastLoc;

  /// Everything we know about a live virtual register.
  struct LiveReg {
    Register VirtReg;      ///< Virtual register number.
    MCPhysReg PhysReg = 0; ///< Currently held here; 0 = in stack slot only.
    bool StackValid = false; ///< Stack slot holds the current value.
    bool Error = false;      ///< Could not allocate.

    explicit LiveReg(Register VirtReg) : VirtReg(VirtReg) {}
    explicit LiveReg() = default;

    unsigned getSparseSetIndex() const { return VirtReg.virtRegIndex(); }
  };

  using LiveRegMap = SparseSet<LiveReg, unsigned, identity, uint16_t>;
  /// This map contains entries for each virtual register that is currently
  /// live within the block being allocated, in a register or a stack slot.
  LiveRegMap LiveVirtRegs;

  /// Stores assigned virtual registers present in the bundle MI.
  DenseMap<Register, LiveReg> BundleVirtRegsMap;

  DenseMap<Register, SmallVector<MachineOperand *, 2>> LiveDbgValueMap;

  /// State of a register unit.
  enum RegUnitState {
    /// A free register is not currently in use and can be allocated
    /// immediately without checking aliases.
    regFree,

    /// A pre-assigned register has been assigned before register allocation
    /// (e.g., setting up a call parameter).
    regPreAssigned,

    /// A register state may also be a virtual register number, indication
    /// that the physical register is currently allocated to a virtual
    /// register. In that case, LiveVirtRegs contains the inverse mapping.
  };

  /// Maps each physical register to a RegUnitState enum or virtual register.
  std::vector<unsigned> RegUnitStates;

  SmallVector<MachineInstr *, 32> Coalesced;

  /// Track register units that are used in the current instruction, and so
  /// cannot be allocated.
  ///
  /// If the lowest bit isn't set, the register unit is only blocked for
  /// early-clobber defs (physical uses and killed uses); otherwise it is
  /// blocked for all allocation. To avoid resetting the entire vector after
  /// every instruction, we track the instruction "generation" in the
  /// remaining 31 bits -- this means, that if UsedInInstr[Idx] < InstrGen,
  /// the register unit is unused. InstrGen is never zero and always
  /// incremented by two.
  ///
  /// Don't allocate inline storage: the number of register units is typically
  /// quite large (e.g., AArch64 > 100, X86 > 200, AMDGPU > 1000).
  uint32_t InstrGen;
  SmallVector<unsigned, 0> UsedInInstr;

  SmallVector<unsigned, 8> DefOperandIndexes;
  // Register masks attached to the current instruction.
  SmallVector<const uint32_t *> RegMasks;

  /// Virtual registers killed by the current instruction, freed after all
  /// uses are processed so that defs may reuse their registers.
  SmallVector<Register, 8> KilledUses;

  /// Lazily built per-class allocation orders with callee-saved registers
  /// first.
  DenseMap<const TargetRegisterClass *, SmallVector<MCPhysReg, 32>>
      CSRFirstOrders;

  bool overlapsCalleeSaved(MCPhysReg PhysReg) const {
    return RegClassInfo.getLastCalleeSavedAlias(PhysReg).isValid();
  }

  ArrayRef<MCPhysReg> getCSRFirstOrder(const TargetRegisterClass &RC) {
    auto [It, New] = CSRFirstOrders.try_emplace(&RC);
    if (New) {
      ArrayRef<MCPhysReg> Order = RegClassInfo.getOrder(&RC);
      It->second.assign(Order.begin(), Order.end());
      std::stable_partition(
          It->second.begin(), It->second.end(),
          [this](MCPhysReg R) { return overlapsCalleeSaved(R); });
    }
    return It->second;
  }

  void setRegUnitState(MCRegUnit Unit, unsigned NewState);
  unsigned getRegUnitState(MCRegUnit Unit) const;

  void setPhysRegState(MCRegister PhysReg, unsigned NewState);
  bool isPhysRegFree(MCRegister PhysReg) const;

  /// Mark a physreg as used in this instruction, blocking it for all
  /// allocation within the instruction.
  void markRegUsedInInstr(MCPhysReg PhysReg) {
    for (MCRegUnit Unit : TRI->regunits(PhysReg))
      UsedInInstr[static_cast<unsigned>(Unit)] = InstrGen | 1;
  }

  // Check if physreg is clobbered by instruction's regmask(s).
  bool isClobberedByRegMasks(MCRegister PhysReg) const {
    return llvm::any_of(RegMasks, [PhysReg](const uint32_t *Mask) {
      return MachineOperand::clobbersPhysReg(Mask, PhysReg);
    });
  }

  /// Check if a physreg or any of its aliases are used in this instruction.
  bool isRegUsedInInstr(MCPhysReg PhysReg, bool LookAtPhysRegUses) const {
    if (LookAtPhysRegUses && isClobberedByRegMasks(PhysReg))
      return true;
    for (MCRegUnit Unit : TRI->regunits(PhysReg))
      if (UsedInInstr[static_cast<unsigned>(Unit)] >=
          (InstrGen | !LookAtPhysRegUses))
        return true;
    return false;
  }

  /// Mark physical register as being used in a register use operand.
  /// Registers marked this way are still available for normal defs (so a
  /// def can reuse a killed use's register), but not for early-clobber defs.
  void markPhysRegUsedInInstr(MCPhysReg PhysReg) {
    for (MCRegUnit Unit : TRI->regunits(PhysReg)) {
      unsigned &Slot = UsedInInstr[static_cast<unsigned>(Unit)];
      if (Slot < InstrGen)
        Slot = InstrGen;
    }
  }

  /// Downgrade a normal use mark to a phys-use-level mark, making the
  /// register available for normal defs again.
  void downgradeRegUsedInInstr(MCPhysReg PhysReg) {
    for (MCRegUnit Unit : TRI->regunits(PhysReg))
      UsedInInstr[static_cast<unsigned>(Unit)] = InstrGen;
  }

  enum : unsigned {
    spillClean = 50,
    spillDirty = 100,
    spillPrefBonus = 20,
    spillImpossible = ~0u
  };

public:
  bool ClearVirtRegs;

  bool runOnMachineFunction(MachineFunction &MF);

private:
  void analyzeVRegs(MachineFunction &MF);
  void allocateBasicBlock(MachineBasicBlock &MBB);

  void addRegClassDefCounts(MutableArrayRef<unsigned> RegClassDefCounts,
                            Register Reg) const;

  void findAndSortDefOperandIndexes(const MachineInstr &MI);

  void allocateInstruction(MachineInstr &MI);
  void handleDebugValue(MachineInstr &MI);
  void handleBundle(MachineInstr &MI);

  bool displacePhysReg(MachineInstr &MI, MCRegister PhysReg,
                       bool KillOnSpill = true);
  void freeVirtReg(LiveReg &LR);

  unsigned calcSpillCost(MCPhysReg PhysReg, uint32_t &VictimLastPos) const;

  LiveRegMap::iterator findLiveVirtReg(Register VirtReg) {
    return LiveVirtRegs.find(VirtReg.virtRegIndex());
  }

  LiveRegMap::const_iterator findLiveVirtReg(Register VirtReg) const {
    return LiveVirtRegs.find(VirtReg.virtRegIndex());
  }

  void assignVirtToPhysReg(MachineInstr &MI, LiveReg &, MCRegister PhysReg);
  void allocVirtReg(MachineInstr &MI, LiveReg &LR, Register Hint,
                    bool LookAtPhysRegUses = false, bool PreferCSR = false);
  void allocVirtRegUndef(MachineOperand &MO);
  bool defineVirtReg(MachineInstr &MI, unsigned OpNum, Register VirtReg,
                     bool LookAtPhysRegUses = false);
  bool useVirtReg(MachineInstr &MI, MachineOperand &MO, Register VirtReg);

  MCPhysReg getErrorAssignment(bool AlreadyReported, MachineInstr &MI,
                               const TargetRegisterClass &RC);

  bool setPhysReg(MachineInstr &MI, MachineOperand &MO,
                  const LiveReg &Assignment);

  bool shouldAllocateRegister(const Register Reg) const;
  int getStackSpaceFor(Register VirtReg);
  void spill(MachineBasicBlock::iterator Before, Register VirtReg,
             MCPhysReg AssignedReg, bool Kill, bool LiveOut);
  void reload(MachineBasicBlock::iterator Before, Register VirtReg,
              MCPhysReg PhysReg);

  void dumpState() const;
};

class RegAllocFast : public MachineFunctionPass {
  RegAllocFastImpl Impl;

public:
  static char ID;

  RegAllocFast(const RegAllocFilterFunc F = nullptr, bool ClearVirtRegs_ = true)
      : MachineFunctionPass(ID), Impl(F, ClearVirtRegs_) {}

  bool runOnMachineFunction(MachineFunction &MF) override {
    return Impl.runOnMachineFunction(MF);
  }

  StringRef getPassName() const override { return "Fast Register Allocator"; }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.setPreservesCFG();
    MachineFunctionPass::getAnalysisUsage(AU);
  }

  MachineFunctionProperties getRequiredProperties() const override {
    return MachineFunctionProperties().setNoPHIs();
  }

  MachineFunctionProperties getSetProperties() const override {
    if (Impl.ClearVirtRegs) {
      return MachineFunctionProperties().setNoVRegs();
    }

    return MachineFunctionProperties();
  }

  MachineFunctionProperties getClearedProperties() const override {
    return MachineFunctionProperties().setIsSSA();
  }
};

} // end anonymous namespace

char RegAllocFast::ID = 0;

INITIALIZE_PASS(RegAllocFast, "regallocfast", "Fast Register Allocator", false,
                false)

bool RegAllocFastImpl::shouldAllocateRegister(const Register Reg) const {
  assert(Reg.isVirtual());
  if (!ShouldAllocateRegisterImpl)
    return true;

  return ShouldAllocateRegisterImpl(*TRI, *MRI, Reg);
}

void RegAllocFastImpl::setRegUnitState(MCRegUnit Unit, unsigned NewState) {
  RegUnitStates[static_cast<unsigned>(Unit)] = NewState;
  ++StateSeq;
  if (TrackDbgLoc)
    UnitChangeSeq[static_cast<unsigned>(Unit)] = StateSeq;
}

unsigned RegAllocFastImpl::getRegUnitState(MCRegUnit Unit) const {
  return RegUnitStates[static_cast<unsigned>(Unit)];
}

void RegAllocFastImpl::setPhysRegState(MCRegister PhysReg, unsigned NewState) {
  for (MCRegUnit Unit : TRI->regunits(PhysReg))
    setRegUnitState(Unit, NewState);
}

bool RegAllocFastImpl::isPhysRegFree(MCRegister PhysReg) const {
  for (MCRegUnit Unit : TRI->regunits(PhysReg)) {
    if (getRegUnitState(Unit) != regFree)
      return false;
  }
  return true;
}

/// This allocates space for the specified virtual register to be held on the
/// stack.
int RegAllocFastImpl::getStackSpaceFor(Register VirtReg) {
  // Find the location Reg would belong...
  int SS = StackSlotForVirtReg[VirtReg];
  // Already has space allocated?
  if (SS != -1)
    return SS;

  // Allocate a new stack object for this spill location...
  const TargetRegisterClass &RC = *MRI->getRegClass(VirtReg);
  unsigned Size = TRI->getSpillSize(RC);
  Align Alignment = TRI->getSpillAlign(RC);

  const MachineFunction &MF = MRI->getMF();
  auto &ST = MF.getSubtarget();
  Align CurrentAlign = ST.getFrameLowering()->getStackAlign();
  if (Alignment > CurrentAlign && !TRI->canRealignStack(MF))
    Alignment = CurrentAlign;

  int FrameIdx =
      MFI->CreateSpillStackObject(Size, Alignment, TRI->getSpillStackID(RC));

  // Assign the slot.
  StackSlotForVirtReg[VirtReg] = FrameIdx;
  return FrameIdx;
}

/// Insert spill instruction for \p AssignedReg before \p Before. Update
/// DBG_VALUEs with \p VirtReg operands with the stack slot.
void RegAllocFastImpl::spill(MachineBasicBlock::iterator Before,
                             Register VirtReg, MCPhysReg AssignedReg, bool Kill,
                             bool LiveOut) {
  LLVM_DEBUG(dbgs() << "Spilling " << printReg(VirtReg, TRI) << " in "
                    << printReg(AssignedReg, TRI));
  int FI = getStackSpaceFor(VirtReg);
  LLVM_DEBUG(dbgs() << " to stack slot #" << FI << '\n');

  const TargetRegisterClass &RC = *MRI->getRegClass(VirtReg);
  TII->storeRegToStackSlot(*MBB, Before, AssignedReg, Kill, FI, &RC, VirtReg);
  ++NumStores;

  // When we spill a virtual register, we will have spill instructions behind
  // every definition of it, meaning we can switch all the DBG_VALUEs over
  // to just reference the stack slot.
  auto LDVIt = LiveDbgValueMap.find(VirtReg);
  if (LDVIt == LiveDbgValueMap.end())
    return;
  MachineBasicBlock::iterator FirstTerm = MBB->getFirstTerminator();
  SmallVectorImpl<MachineOperand *> &LRIDbgOperands = LDVIt->second;
  SmallMapVector<MachineInstr *, SmallVector<const MachineOperand *>, 2>
      SpilledOperandsMap;
  for (MachineOperand *MO : LRIDbgOperands)
    SpilledOperandsMap[MO->getParent()].push_back(MO);
  for (const auto &MISpilledOperands : SpilledOperandsMap) {
    MachineInstr &DBG = *MISpilledOperands.first;
    // We don't have enough support for tracking operands of DBG_VALUE_LISTs.
    if (DBG.isDebugValueList())
      continue;
    MachineInstr *NewDV = buildDbgValueForSpill(
        *MBB, Before, *MISpilledOperands.first, FI, MISpilledOperands.second);
    assert(NewDV->getParent() == MBB && "dangling parent pointer");
    (void)NewDV;
    LLVM_DEBUG(dbgs() << "Inserting debug info due to spill:\n" << *NewDV);

    if (LiveOut) {
      // We need to insert a DBG_VALUE at the end of the block if the spill
      // slot is live out, but there is another use of the value after the
      // spill. This will allow LiveDebugValues to see the correct live out
      // value to propagate to the successors.
      MachineInstr *ClonedDV = MBB->getParent()->CloneMachineInstr(NewDV);
      MBB->insert(FirstTerm, ClonedDV);
      LLVM_DEBUG(dbgs() << "Cloning debug info due to live out spill\n");
    }

    // Rewrite unassigned dbg_values to use the stack slot.
    // TODO We can potentially do this for list debug values as well if we know
    // how the dbg_values are getting unassigned.
    if (DBG.isNonListDebugValue()) {
      MachineOperand &MO = DBG.getDebugOperand(0);
      if (MO.isReg() && MO.getReg() == 0) {
        updateDbgValueForSpill(DBG, FI, 0);
      }
    }
  }
  // Now this register is spilled there is should not be any DBG_VALUE
  // pointing to this register because they are all pointing to spilled value
  // now.
  LRIDbgOperands.clear();
}

/// Insert reload instruction for \p PhysReg before \p Before.
void RegAllocFastImpl::reload(MachineBasicBlock::iterator Before,
                              Register VirtReg, MCPhysReg PhysReg) {
  LLVM_DEBUG(dbgs() << "Reloading " << printReg(VirtReg, TRI) << " into "
                    << printReg(PhysReg, TRI) << '\n');
  int FI = getStackSpaceFor(VirtReg);
  const TargetRegisterClass &RC = *MRI->getRegClass(VirtReg);
  TII->loadRegFromStackSlot(*MBB, Before, PhysReg, FI, &RC, VirtReg);
  ++NumLoads;
}

/// Free the register held by \p LR (if any) and remove the entry from
/// LiveVirtRegs.
void RegAllocFastImpl::freeVirtReg(LiveReg &LR) {
  if (LR.PhysReg) {
    setPhysRegState(LR.PhysReg, regFree);
    // Remember where the value was: a later DBG_VALUE can still refer to
    // the register as long as nothing touches it.
    if (TrackDbgLoc)
      VRegLastLoc[LR.VirtReg.virtRegIndex()] = {LR.PhysReg, StateSeq};
  }
  LiveVirtRegs.erase(LR.VirtReg.virtRegIndex());
}

/// Displace all virtual registers held in units of \p PhysReg, spilling them
/// before \p MI if their stack copy is not current. Also clears pre-assigned
/// states. Returns true if any state was changed. \p KillOnSpill must be
/// false when \p MI still reads the register (e.g. a copy whose destination
/// holds its own source value).
bool RegAllocFastImpl::displacePhysReg(MachineInstr &MI, MCRegister PhysReg,
                                       bool KillOnSpill) {
  bool displacedAny = false;

  for (MCRegUnit Unit : TRI->regunits(PhysReg)) {
    switch (unsigned VirtReg = getRegUnitState(Unit)) {
    default: {
      LiveRegMap::iterator LRI = findLiveVirtReg(VirtReg);
      assert(LRI != LiveVirtRegs.end() && "datastructures in sync");
      if (!LRI->StackValid && !LRI->Error) {
        spill(MI.getIterator(), LRI->VirtReg, LRI->PhysReg, KillOnSpill,
              vregInfo(LRI->VirtReg).LiveCrossBlock);
        LRI->StackValid = true;
      }
      setPhysRegState(LRI->PhysReg, regFree);
      LRI->PhysReg = 0;
      displacedAny = true;
      break;
    }
    case regPreAssigned:
      setRegUnitState(Unit, regFree);
      displacedAny = true;
      break;
    case regFree:
      break;
    }
  }
  return displacedAny;
}

/// Return the cost of spilling clearing out PhysReg and aliases so it is free
/// for allocation. Returns 0 when PhysReg is free or disabled with all aliases
/// disabled - it can be allocated directly.
/// \returns spillImpossible when PhysReg or an alias can't be spilled.
unsigned RegAllocFastImpl::calcSpillCost(MCPhysReg PhysReg,
                                         uint32_t &VictimLastPos) const {
  unsigned Cost = 0;
  VictimLastPos = 0;
  for (MCRegUnit Unit : TRI->regunits(PhysReg)) {
    switch (unsigned VirtReg = getRegUnitState(Unit)) {
    case regFree:
      break;
    case regPreAssigned:
      LLVM_DEBUG(dbgs() << "Cannot spill pre-assigned "
                        << printReg(PhysReg, TRI) << '\n');
      return spillImpossible;
    default:
      if (!Cost)
        Cost = findLiveVirtReg(VirtReg)->StackValid ? spillClean : spillDirty;
      VictimLastPos = std::max(
          VictimLastPos, VRegInfos[Register(VirtReg).virtRegIndex()].LastPos);
      break;
    }
    // Match the pre-fold behavior: the first owned unit decides the cost and
    // later pre-assigned units do not veto the candidate.
    if (Cost)
      return Cost;
  }
  return Cost;
}

/// This method updates local state so that we know that PhysReg is the
/// proper container for VirtReg now.  The physical register must not be used
/// for anything else when this is called.
void RegAllocFastImpl::assignVirtToPhysReg(MachineInstr &AtMI, LiveReg &LR,
                                           MCRegister PhysReg) {
  Register VirtReg = LR.VirtReg;
  LLVM_DEBUG(dbgs() << "Assigning " << printReg(VirtReg, TRI) << " to "
                    << printReg(PhysReg, TRI) << '\n');
  assert(LR.PhysReg == 0 && "Already assigned a physreg");
  assert(PhysReg != 0 && "Trying to assign no register");
  LR.PhysReg = PhysReg;
  setPhysRegState(PhysReg, VirtReg.id());
}

/// Allocates a physical register for VirtReg.
void RegAllocFastImpl::allocVirtReg(MachineInstr &MI, LiveReg &LR,
                                    Register Hint0, bool LookAtPhysRegUses,
                                    bool PreferCSR) {
  const Register VirtReg = LR.VirtReg;
  assert(LR.PhysReg == 0);

  const TargetRegisterClass &RC = *MRI->getRegClass(VirtReg);
  LLVM_DEBUG(dbgs() << "Search register for " << printReg(VirtReg)
                    << " in class " << TRI->getRegClassName(&RC)
                    << " with hint " << printReg(Hint0, TRI) << '\n');

  // Take hint when possible. For call-crossing values, only take hints that
  // survive the call; a volatile hint would be displaced at the call.
  if (Hint0.isPhysical() && MRI->isAllocatable(Hint0) && RC.contains(Hint0) &&
      !isRegUsedInInstr(Hint0, LookAtPhysRegUses) &&
      (!PreferCSR || overlapsCalleeSaved(Hint0.asMCReg()))) {
    // Take hint if the register is currently free.
    if (isPhysRegFree(Hint0)) {
      LLVM_DEBUG(dbgs() << "\tPreferred Register: " << printReg(Hint0, TRI)
                        << '\n');
      assignVirtToPhysReg(MI, LR, Hint0);
      return;
    }
    LLVM_DEBUG(dbgs() << "\tPreferred Register: " << printReg(Hint0, TRI)
                      << " occupied\n");
  } else {
    Hint0 = Register();
  }

  MCPhysReg BestReg = 0;
  unsigned BestCost = spillImpossible;
  uint32_t BestVictimLastPos = 0;
  ArrayRef<MCPhysReg> AllocationOrder =
      PreferCSR ? getCSRFirstOrder(RC) : RegClassInfo.getOrder(&RC);
  for (MCPhysReg PhysReg : AllocationOrder) {
    LLVM_DEBUG(dbgs() << "\tRegister: " << printReg(PhysReg, TRI) << ' ');
    if (isRegUsedInInstr(PhysReg, LookAtPhysRegUses)) {
      LLVM_DEBUG(dbgs() << "already used in instr.\n");
      continue;
    }

    uint32_t VictimLastPos;
    unsigned Cost = calcSpillCost(PhysReg, VictimLastPos);
    LLVM_DEBUG(dbgs() << "Cost: " << Cost << " BestCost: " << BestCost << '\n');
    if (Cost == spillImpossible)
      continue;
    // Immediate take a register with cost 0.
    if (Cost == 0) {
      assignVirtToPhysReg(MI, LR, PhysReg);
      return;
    }

    if (PhysReg == Hint0)
      Cost -= spillPrefBonus;

    if (Cost > BestCost)
      continue;

    // Among equal-cost candidates, evict the value whose last use is
    // farthest away.
    if (Cost < BestCost || VictimLastPos > BestVictimLastPos) {
      BestReg = PhysReg;
      BestCost = Cost;
      BestVictimLastPos = VictimLastPos;
    }
  }

  if (!BestReg) {
    // Nothing we can do: Report an error and keep going with an invalid
    // allocation.
    LR.PhysReg = getErrorAssignment(LR.Error, MI, RC);
    LR.Error = true;
    return;
  }

  displacePhysReg(MI, BestReg);
  assignVirtToPhysReg(MI, LR, BestReg);
}

void RegAllocFastImpl::allocVirtRegUndef(MachineOperand &MO) {
  assert(MO.isUndef() && "expected undef use");
  Register VirtReg = MO.getReg();
  assert(VirtReg.isVirtual() && "Expected virtreg");
  if (!shouldAllocateRegister(VirtReg))
    return;

  LiveRegMap::iterator LRI = findLiveVirtReg(VirtReg);
  MCPhysReg PhysReg;
  bool IsRenamable = true;
  if (LRI != LiveVirtRegs.end() && LRI->PhysReg) {
    PhysReg = LRI->PhysReg;
  } else {
    const TargetRegisterClass &RC = *MRI->getRegClass(VirtReg);
    ArrayRef<MCPhysReg> AllocationOrder = RegClassInfo.getOrder(&RC);
    if (AllocationOrder.empty()) {
      // All registers in the class were reserved.
      //
      // It might be OK to take any entry from the class as this is an undef
      // use, but accepting this would give different behavior than greedy and
      // basic.
      PhysReg = getErrorAssignment(false, *MO.getParent(), RC);
      IsRenamable = false;
    } else
      PhysReg = AllocationOrder.front();
  }

  unsigned SubRegIdx = MO.getSubReg();
  if (SubRegIdx != 0) {
    PhysReg = TRI->getSubReg(PhysReg, SubRegIdx);
    MO.setSubReg(0);
  }
  MO.setReg(PhysReg);
  MO.setIsRenamable(IsRenamable);
}

/// Allocates a register for VirtReg definition. Typically the register is
/// already assigned from a previous use or def in this block, however we
/// still need to perform an allocation if not.
///
/// \return true if MI's MachineOperands were re-arranged/invalidated.
bool RegAllocFastImpl::defineVirtReg(MachineInstr &MI, unsigned OpNum,
                                     Register VirtReg, bool LookAtPhysRegUses) {
  assert(VirtReg.isVirtual() && "Not a virtual register");
  if (!shouldAllocateRegister(VirtReg))
    return false;
  MachineOperand &MO = MI.getOperand(OpNum);
  VRegInfo &Info = vregInfo(VirtReg);
  LiveRegMap::iterator LRI = LiveVirtRegs.insert(LiveReg(VirtReg)).first;
  if (LRI->PhysReg == 0) {
    Register Hint = Info.HintReg;
    if (!Hint && MI.isCopy() && MI.getNumOperands() == 2) {
      // Take over the register of a killed copy source.
      const MachineOperand &SrcMO = MI.getOperand(1);
      if (SrcMO.getReg().isPhysical() && SrcMO.isKill() && !SrcMO.getSubReg())
        Hint = SrcMO.getReg();
    }
    allocVirtReg(MI, *LRI, Hint, LookAtPhysRegUses,
                 PreferCSRForCallCrossing && Info.CrossesCall);
  }

  // The redefinition makes any stack copy stale.
  LRI->StackValid = false;

  MCPhysReg PhysReg = LRI->PhysReg;
  markRegUsedInInstr(PhysReg);
  if (MI.getOpcode() == TargetOpcode::BUNDLE) {
    BundleVirtRegsMap[VirtReg] = *LRI;
  }

  // A tied def must use the same register as its tied use. If the use (of a
  // different virtual register that stays live) ended up elsewhere, copy the
  // value over. This must happen before setPhysReg() below, which may
  // re-arrange the operand list.
  if (MO.isTied() && !LRI->Error) {
    unsigned TiedIdx = MI.findTiedOperandIdx(OpNum);
    MachineOperand &TiedMO = MI.getOperand(TiedIdx);
    if (TiedMO.getReg().isPhysical() && !TiedMO.isUndef() &&
        TiedMO.getReg() != Register(PhysReg)) {
      BuildMI(*MBB, MI.getIterator(), MI.getDebugLoc(),
              TII->get(TargetOpcode::COPY), PhysReg)
          .addReg(TiedMO.getReg(), getKillRegState(TiedMO.isKill()));
      TiedMO.setReg(PhysReg);
      TiedMO.setIsKill(true);
    }
  }

  bool Rearranged = setPhysReg(MI, MO, *LRI);

  // Values live across blocks are kept in their stack slot at block
  // boundaries: spill eagerly after the definition.
  if (Info.LiveCrossBlock && !MI.isImplicitDef() && !LRI->Error) {
    bool Kill = CurPos == Info.LastPos;
    spill(std::next(MI.getIterator()), VirtReg, PhysReg, Kill,
          /*LiveOut=*/true);
    LRI->StackValid = true;

    // We need to place additional spills for each indirect destination of an
    // INLINEASM_BR.
    if (MI.getOpcode() == TargetOpcode::INLINEASM_BR) {
      int FI = StackSlotForVirtReg[VirtReg];
      const TargetRegisterClass &RC = *MRI->getRegClass(VirtReg);
      for (MachineOperand &BrMO : MI.operands()) {
        if (BrMO.isMBB()) {
          MachineBasicBlock *Succ = BrMO.getMBB();
          TII->storeRegToStackSlot(*Succ, Succ->begin(), PhysReg, Kill, FI, &RC,
                                   VirtReg);
          ++NumStores;
          Succ->addLiveIn(PhysReg);
        }
      }
    }

    if (Kill)
      freeVirtReg(*LRI);
  } else if (Info.LastPos == CurPos && !Info.LiveCrossBlock && !LRI->Error) {
    // The def is the last event of this value: it is never used. Free the
    // register; the UsedInInstr mark keeps it away from this instruction's
    // other defs. (MO may be stale if setPhysReg re-arranged the operands.)
    if (!Rearranged && MO.isReg() && MO.isDef() && !MO.isDead())
      MO.setIsDead(true);
    freeVirtReg(*LRI);
  }

  return Rearranged;
}

/// Allocates a register for a VirtReg use, reloading the value if it is not
/// currently in a register.
/// \return true if MI's MachineOperands were re-arranged/invalidated.
bool RegAllocFastImpl::useVirtReg(MachineInstr &MI, MachineOperand &MO,
                                  Register VirtReg) {
  assert(VirtReg.isVirtual() && "Not a virtual register");
  if (!shouldAllocateRegister(VirtReg))
    return false;
  VRegInfo &Info = vregInfo(VirtReg);
  LiveRegMap::iterator LRI;
  bool New;
  std::tie(LRI, New) = LiveVirtRegs.insert(LiveReg(VirtReg));
  if (New) {
    // A new entry at a use means the value lives in its stack slot (defined
    // in another block, or spilled and displaced earlier in this block).
    LRI->StackValid = true;
  }

  // If necessary allocate a register and reload the value. (This includes
  // tied uses seen before their def; the def then reuses the register.)
  if (LRI->PhysReg == 0) {
    Register Hint = Info.HintReg;
    if (MI.isCopy() && MI.getNumOperands() == 2 &&
        MI.getOperand(1).getSubReg() == 0 &&
        MI.getOperand(0).getReg().isPhysical())
      Hint = MI.getOperand(0).getReg();
    allocVirtReg(MI, *LRI, Hint, false,
                 PreferCSRForCallCrossing && Info.CrossesCall);
    if (!LRI->Error)
      reload(MI.getIterator(), VirtReg, LRI->PhysReg);
  }

  // Exact kill: this is the last use or def of the value anywhere. Tied uses
  // are redefined by this instruction and stay live.
  bool Kill = CurPos == Info.LastPos && !MO.isTied();
  if (Kill && !MO.isKill() && !Info.LiveCrossBlock)
    MO.setIsKill(true);
  if (Kill)
    KilledUses.push_back(VirtReg);

  markRegUsedInInstr(LRI->PhysReg);
  if (MI.getOpcode() == TargetOpcode::BUNDLE) {
    BundleVirtRegsMap[VirtReg] = *LRI;
  }
  return setPhysReg(MI, MO, *LRI);
}

/// Query a physical register to use as a filler in contexts where the
/// allocation has failed. This will raise an error, but not abort the
/// compilation.
MCPhysReg RegAllocFastImpl::getErrorAssignment(bool AlreadyReported,
                                               MachineInstr &MI,
                                               const TargetRegisterClass &RC) {
  MachineFunction &MF = *MI.getMF();

  // Avoid repeating the error every time a register is used.
  bool EmitError = !MF.getProperties().hasFailedRegAlloc();
  if (EmitError)
    MF.getProperties().setFailedRegAlloc();

  // If the allocation order was empty, all registers in the class were
  // probably reserved. Fall back to taking the first register in the class,
  // even if it's reserved.
  ArrayRef<MCPhysReg> AllocationOrder = RegClassInfo.getOrder(&RC);
  if (AllocationOrder.empty()) {
    const Function &Fn = MF.getFunction();
    if (EmitError) {
      Fn.getContext().diagnose(DiagnosticInfoRegAllocFailure(
          "no registers from class available to allocate", Fn,
          MI.getDebugLoc()));
    }

    ArrayRef<MCPhysReg> RawRegs = RC.getRegisters();
    assert(!RawRegs.empty() && "register classes cannot have no registers");
    return RawRegs.front();
  }

  if (!AlreadyReported && EmitError) {
    // Nothing we can do: Report an error and keep going with an invalid
    // allocation.
    if (MI.isInlineAsm()) {
      MI.emitInlineAsmError(
          "inline assembly requires more registers than available");
    } else {
      const Function &Fn = MBB->getParent()->getFunction();
      Fn.getContext().diagnose(DiagnosticInfoRegAllocFailure(
          "ran out of registers during register allocation", Fn,
          MI.getDebugLoc()));
    }
  }

  return AllocationOrder.front();
}

/// Changes operand OpNum in MI the refer the PhysReg, considering subregs.
/// \return true if MI's MachineOperands were re-arranged/invalidated.
bool RegAllocFastImpl::setPhysReg(MachineInstr &MI, MachineOperand &MO,
                                  const LiveReg &Assignment) {
  MCPhysReg PhysReg = Assignment.PhysReg;
  assert(PhysReg && "assignments should always be to a valid physreg");

  if (LLVM_UNLIKELY(Assignment.Error)) {
    // Make sure we don't set renamable in error scenarios, as we may have
    // assigned to a reserved register.
    if (MO.isUse())
      MO.setIsUndef(true);
  }

  if (!MO.getSubReg()) {
    MO.setReg(PhysReg);
    MO.setIsRenamable(!Assignment.Error);
    return false;
  }

  // Handle subregister index.
  MO.setReg(TRI->getSubReg(PhysReg, MO.getSubReg()));
  MO.setIsRenamable(!Assignment.Error);

  // Note: We leave the subreg number around a little longer in case of defs.
  // This is so that the register freeing logic in allocateInstruction can
  // still recognize this as subregister defs. The code there will clear the
  // number.
  if (!MO.isDef())
    MO.setSubReg(0);

  // A kill flag implies killing the full register. Add corresponding super
  // register kill.
  if (MO.isKill()) {
    MI.addRegisterKilled(PhysReg, TRI, true);
    // Conservatively assume implicit MOs were re-arranged
    return true;
  }

  // A <def,read-undef> of a sub-register requires an implicit def of the full
  // register.
  if (MO.isDef() && MO.isUndef()) {
    if (MO.isDead())
      MI.addRegisterDead(PhysReg, TRI, true);
    else
      MI.addRegisterDefined(PhysReg, TRI);
    // Conservatively assume implicit MOs were re-arranged
    return true;
  }
  return false;
}

#ifndef NDEBUG

void RegAllocFastImpl::dumpState() const {
  for (MCRegUnit Unit : TRI->regunits()) {
    switch (unsigned VirtReg = getRegUnitState(Unit)) {
    case regFree:
      break;
    case regPreAssigned:
      dbgs() << " " << printRegUnit(Unit, TRI) << "[P]";
      break;
    default: {
      dbgs() << ' ' << printRegUnit(Unit, TRI) << '=' << printReg(VirtReg);
      LiveRegMap::const_iterator I = findLiveVirtReg(VirtReg);
      assert(I != LiveVirtRegs.end() && "have LiveVirtRegs entry");
      if (I->StackValid)
        dbgs() << "[S]";
      assert(TRI->hasRegUnit(I->PhysReg, Unit) && "inverse mapping present");
      break;
    }
    }
  }
  dbgs() << '\n';
  // Check that LiveVirtRegs is the inverse.
  for (const LiveReg &LR : LiveVirtRegs) {
    Register VirtReg = LR.VirtReg;
    assert(VirtReg.isVirtual() && "Bad map key");
    MCPhysReg PhysReg = LR.PhysReg;
    if (PhysReg != 0) {
      assert(Register::isPhysicalRegister(PhysReg) && "mapped to physreg");
      for (MCRegUnit Unit : TRI->regunits(PhysReg)) {
        assert(getRegUnitState(Unit) == VirtReg && "inverse map valid");
      }
    }
  }
}

#endif

/// Count number of defs consumed from each register class by \p Reg
void RegAllocFastImpl::addRegClassDefCounts(
    MutableArrayRef<unsigned> RegClassDefCounts, Register Reg) const {
  assert(RegClassDefCounts.size() == TRI->getNumRegClasses());

  if (Reg.isVirtual()) {
    if (!shouldAllocateRegister(Reg))
      return;
    const TargetRegisterClass *OpRC = MRI->getRegClass(Reg);
    for (unsigned RCIdx = 0, RCIdxEnd = TRI->getNumRegClasses();
         RCIdx != RCIdxEnd; ++RCIdx) {
      const TargetRegisterClass *IdxRC = TRI->getRegClass(RCIdx);
      // FIXME: Consider aliasing sub/super registers.
      if (OpRC->hasSubClassEq(IdxRC))
        ++RegClassDefCounts[RCIdx];
    }

    return;
  }

  for (unsigned RCIdx = 0, RCIdxEnd = TRI->getNumRegClasses();
       RCIdx != RCIdxEnd; ++RCIdx) {
    const TargetRegisterClass *IdxRC = TRI->getRegClass(RCIdx);
    for (MCRegAliasIterator Alias(Reg, TRI, true); Alias.isValid(); ++Alias) {
      if (IdxRC->contains(*Alias)) {
        ++RegClassDefCounts[RCIdx];
        break;
      }
    }
  }
}

/// Compute \ref DefOperandIndexes so it contains the indices of "def" operands
/// that are to be allocated. Those are ordered in a way that small classes,
/// early clobbers and livethroughs are allocated first.
void RegAllocFastImpl::findAndSortDefOperandIndexes(const MachineInstr &MI) {
  DefOperandIndexes.clear();

  for (unsigned I = 0, E = MI.getNumOperands(); I < E; ++I) {
    const MachineOperand &MO = MI.getOperand(I);
    if (MO.isReg() && MO.isDef() && MO.getReg().isVirtual() &&
        shouldAllocateRegister(MO.getReg()))
      DefOperandIndexes.push_back(I);
  }

  // Most instructions only have one virtual def, so there's no point in
  // computing the possible number of defs for every register class.
  if (DefOperandIndexes.size() <= 1)
    return;

  // Track number of defs which may consume a register from the class. This is
  // used to assign registers for possibly-too-small classes first. Example:
  // defs are eax, 3 * gr32_abcd, 2 * gr32 => we want to assign the gr32_abcd
  // registers first so that the gr32 don't use the gr32_abcd registers before
  // we assign these.
  SmallVector<unsigned> RegClassDefCounts(TRI->getNumRegClasses(), 0);

  for (const MachineOperand &MO : MI.all_defs())
    addRegClassDefCounts(RegClassDefCounts, MO.getReg());

  llvm::sort(DefOperandIndexes, [&](unsigned I0, unsigned I1) {
    const MachineOperand &MO0 = MI.getOperand(I0);
    const MachineOperand &MO1 = MI.getOperand(I1);
    Register Reg0 = MO0.getReg();
    Register Reg1 = MO1.getReg();
    const TargetRegisterClass &RC0 = *MRI->getRegClass(Reg0);
    const TargetRegisterClass &RC1 = *MRI->getRegClass(Reg1);

    // Identify regclass that are easy to use up completely just in this
    // instruction.
    unsigned ClassSize0 = RegClassInfo.getOrder(&RC0).size();
    unsigned ClassSize1 = RegClassInfo.getOrder(&RC1).size();

    bool SmallClass0 = ClassSize0 < RegClassDefCounts[RC0.getID()];
    bool SmallClass1 = ClassSize1 < RegClassDefCounts[RC1.getID()];
    if (SmallClass0 > SmallClass1)
      return true;
    if (SmallClass0 < SmallClass1)
      return false;

    // Allocate early clobbers and livethrough operands first.
    bool Livethrough0 = MO0.isEarlyClobber() || MO0.isTied() ||
                        (MO0.getSubReg() == 0 && !MO0.isUndef());
    bool Livethrough1 = MO1.isEarlyClobber() || MO1.isTied() ||
                        (MO1.getSubReg() == 0 && !MO1.isUndef());
    if (Livethrough0 > Livethrough1)
      return true;
    if (Livethrough0 < Livethrough1)
      return false;

    // Tie-break rule: operand index.
    return I0 < I1;
  });
}

// Returns true if MO is tied and the operand it's tied to is not Undef (not
// Undef is not the same thing as Def).
static bool isTiedToNotUndef(const MachineOperand &MO) {
  if (!MO.isTied())
    return false;
  const MachineInstr &MI = *MO.getParent();
  unsigned TiedIdx = MI.findTiedOperandIdx(MI.getOperandNo(&MO));
  const MachineOperand &TiedMO = MI.getOperand(TiedIdx);
  return !TiedMO.isUndef();
}

void RegAllocFastImpl::allocateInstruction(MachineInstr &MI) {
  InstrGen += 2;
  // In the event we ever get more than 2**31 instructions...
  if (LLVM_UNLIKELY(InstrGen == 0)) {
    UsedInInstr.assign(UsedInInstr.size(), 0);
    InstrGen = 2;
  }
  RegMasks.clear();
  BundleVirtRegsMap.clear();
  KilledUses.clear();
  ConsumedPreassigned.clear();

  // Scan for special cases.
  bool HasPhysRegUse = false;
  bool HasRegMask = false;
  bool HasVRegDef = false;
  bool HasDef = false;
  bool HasPhysDef = false;
  bool NeedToAssignLiveThroughs = false;
  bool HasUndefUse = false;
  for (MachineOperand &MO : MI.operands()) {
    if (MO.isReg()) {
      Register Reg = MO.getReg();
      if (Reg.isVirtual()) {
        if (!shouldAllocateRegister(Reg))
          continue;
        if (MO.isDef()) {
          HasDef = true;
          HasVRegDef = true;
          if (MO.isEarlyClobber())
            NeedToAssignLiveThroughs = true;
          if (isTiedToNotUndef(MO) || (MO.getSubReg() != 0 && !MO.isUndef()))
            NeedToAssignLiveThroughs = true;
        }
      } else if (Reg.isPhysical()) {
        if (!MRI->isReserved(Reg)) {
          if (MO.isDef())
            HasDef = HasPhysDef = true;
          if (MO.readsReg())
            HasPhysRegUse = true;
        }
      }
    } else if (MO.isRegMask()) {
      HasRegMask = true;
      RegMasks.push_back(MO.getRegMask());
    }
  }

  // Mark physical register uses so that virtual register allocation below
  // stays away from them. Pre-assigned registers whose final reader is this
  // instruction are released once the instruction is fully processed.
  if (HasPhysRegUse) {
    for (MachineOperand &MO : MI.operands()) {
      if (!MO.isReg() || !MO.readsReg())
        continue;
      Register Reg = MO.getReg();
      if (!Reg.isPhysical() || MRI->isReserved(Reg))
        continue;
      markPhysRegUsedInInstr(Reg);
      if (MO.isDef())
        continue;

      bool IsPreAssigned = false;
      bool IsFinalUse = true;
      for (MCRegUnit Unit : TRI->regunits(Reg.asMCReg())) {
        if (getRegUnitState(Unit) == regPreAssigned)
          IsPreAssigned = true;
        if (!posKeyContains(PhysSegmentEnds, SegEndCursor,
                            static_cast<unsigned>(Unit)))
          IsFinalUse = false;
      }
      if (IsPreAssigned && IsFinalUse &&
          !llvm::is_contained(ConsumedPreassigned, Reg.asMCReg())) {
        ConsumedPreassigned.push_back(Reg.asMCReg());
        MO.setIsKill(true);
      }
    }
  }

  // Allocate virtreg uses and insert reloads as necessary.
  // Implicit MOs can get moved/removed by useVirtReg(), so loop multiple
  // times to ensure no operand is missed.
  bool ReArrangedImplicitMOs = true;
  while (ReArrangedImplicitMOs) {
    ReArrangedImplicitMOs = false;
    for (MachineOperand &MO : MI.operands()) {
      if (!MO.isReg() || !MO.isUse())
        continue;
      Register Reg = MO.getReg();
      if (!Reg.isVirtual() || !shouldAllocateRegister(Reg))
        continue;

      if (MO.isUndef()) {
        HasUndefUse = true;
        continue;
      }

      assert(!MO.isInternalRead() && "Bundles not supported");
      assert(MO.readsReg() && "reading use");
      ReArrangedImplicitMOs = useVirtReg(MI, MO, Reg);
      if (ReArrangedImplicitMOs)
        break;
    }
  }

  // Allocate undef operands. This is a separate step because in a situation
  // like  ` = OP undef %X, %X`    both operands need the same register assign
  // so we should perform the normal assignment first.
  if (HasUndefUse) {
    for (MachineOperand &MO : MI.all_uses()) {
      Register Reg = MO.getReg();
      if (!Reg.isVirtual() || !shouldAllocateRegister(Reg))
        continue;
      if (!MO.isUndef())
        continue;
      allocVirtRegUndef(MO);
    }
  }

  // Free killed uses. Their registers stay unavailable to early-clobber defs
  // (the UsedInInstr mark is downgraded, not cleared) but become available
  // to normal defs, which enables two-address-style register reuse.
  for (Register VirtReg : KilledUses) {
    LiveRegMap::iterator LRI = findLiveVirtReg(VirtReg);
    if (LRI == LiveVirtRegs.end())
      continue;
    if (LRI->PhysReg)
      downgradeRegUsedInInstr(LRI->PhysReg);
    freeVirtReg(*LRI);
  }

  // Displace clobbered registers, spilling live values before the
  // instruction; release pre-assigned registers covered by the mask.
  if (HasRegMask) {
    assert(!RegMasks.empty() && "expected RegMask");
    // MRI bookkeeping.
    for (const auto *RM : RegMasks)
      MRI->addPhysRegsUsedFromRegMask(RM);

    for (LiveReg &LR : LiveVirtRegs) {
      MCPhysReg PhysReg = LR.PhysReg;
      if (PhysReg != 0 && isClobberedByRegMasks(PhysReg))
        displacePhysReg(MI, PhysReg);
    }

    // Release pre-assigned register units covered by the mask (e.g. argument
    // registers consumed by a call).
    for (unsigned UnitIdx : PreassignedUnits) {
      MCRegUnit Unit = static_cast<MCRegUnit>(UnitIdx);
      if (getRegUnitState(Unit) != regPreAssigned)
        continue;
      for (MCRegUnitRootIterator Root(Unit, TRI); Root.isValid(); ++Root) {
        if (isClobberedByRegMasks(*Root)) {
          setRegUnitState(Unit, regFree);
          break;
        }
      }
    }
  }

  // Release pre-assigned registers consumed by their final reader. This
  // happens before def processing: a def may take over the released register
  // (writes happen after reads), which lets the destination of a copy from a
  // call result coalesce with it.
  for (MCRegister Reg : ConsumedPreassigned) {
    for (MCRegUnit Unit : TRI->regunits(Reg))
      if (getRegUnitState(Unit) == regPreAssigned)
        setRegUnitState(Unit, regFree);
  }

  // Apply pre-assigned physreg defs to state.
  if (HasPhysDef) {
    for (MachineOperand &MO : MI.all_defs()) {
      Register Reg = MO.getReg();
      if (!Reg.isPhysical() || MRI->isReserved(Reg))
        continue;
      // Note: readsRegister must be queried here, not from scan-phase flags:
      // use operands rewritten to physregs above also read the register.
      displacePhysReg(MI, Reg, /*KillOnSpill=*/!MI.readsRegister(Reg, TRI));
      // Infer dead flags: at -O0 nothing computes them, and a def that is
      // never read must not stay pre-assigned (the cleanup below frees it).
      if (!MO.isDead()) {
        bool HasLaterUse = false;
        for (MCRegUnit Unit : TRI->regunits(Reg.asMCReg())) {
          if (posKeyContains(PhysDefsWithUse, DefUseCursor,
                             static_cast<unsigned>(Unit))) {
            HasLaterUse = true;
            break;
          }
        }
        if (!HasLaterUse)
          MO.setIsDead(true);
      }
      setPhysRegState(Reg, regPreAssigned);
      for (MCRegUnit Unit : TRI->regunits(Reg.asMCReg()))
        PreassignedUnits.push_back(static_cast<unsigned>(Unit));
      markRegUsedInInstr(Reg);
    }
  }

  // Allocate virtreg defs.
  if (HasVRegDef) {
    if (NeedToAssignLiveThroughs) {
      // Special handling for early clobbers, tied operands or subregister
      // defs: with multiple such defs, process small register classes and
      // early-clobbers first.
      bool ReArrangedImplicitOps = true;
      while (ReArrangedImplicitOps) {
        ReArrangedImplicitOps = false;
        findAndSortDefOperandIndexes(MI);
        for (unsigned OpIdx : DefOperandIndexes) {
          MachineOperand &MO = MI.getOperand(OpIdx);
          LLVM_DEBUG(dbgs() << "Allocating " << MO << '\n');
          Register Reg = MO.getReg();
          if (!Reg.isVirtual())
            continue;
          ReArrangedImplicitOps =
              defineVirtReg(MI, OpIdx, Reg,
                            /*LookAtPhysRegUses=*/MO.isEarlyClobber() ||
                                isTiedToNotUndef(MO));
          if (ReArrangedImplicitOps)
            break;
        }
      }
    } else {
      // Assign virtual register defs.
      bool ReArrangedImplicitOps = true;
      while (ReArrangedImplicitOps) {
        ReArrangedImplicitOps = false;
        for (MachineOperand &MO : MI.all_defs()) {
          Register Reg = MO.getReg();
          if (Reg.isVirtual()) {
            ReArrangedImplicitOps =
                defineVirtReg(MI, MI.getOperandNo(&MO), Reg);
            if (ReArrangedImplicitOps)
              break;
          }
        }
      }
    }
  }

  // Post-process defs: clear subreg-def markers left by setPhysReg() and
  // free dead physical register defs.
  if (HasDef) {
    for (MachineOperand &MO : reverse(MI.all_defs())) {
      Register Reg = MO.getReg();
      if (!Reg)
        continue;
      if (Reg.isVirtual()) {
        assert(!shouldAllocateRegister(Reg));
        continue;
      }
      assert(Reg.isPhysical());
      if (MO.getSubReg() != 0) {
        MO.setSubReg(0);
        continue;
      }
      if (MRI->isReserved(Reg))
        continue;
      if (!MO.isDead())
        continue;
      // Free dead defs (including rewritten virtual defs whose value is
      // never used).
      for (MCRegUnit Unit : TRI->regunits(Reg)) {
        unsigned State = getRegUnitState(Unit);
        if (State == regPreAssigned) {
          setRegUnitState(Unit, regFree);
        } else if (State != regFree) {
          LiveRegMap::iterator LRI = findLiveVirtReg(State);
          if (LRI != LiveVirtRegs.end())
            freeVirtReg(*LRI);
        }
      }
    }
  }

  LLVM_DEBUG(dbgs() << "<< " << MI);
  if (MI.isCopy() && MI.getNumOperands() == 2 &&
      (MI.getOperand(0).getReg() == MI.getOperand(1).getReg() ||
       MI.getOperand(0).isDead()) &&
      // Keep copies materializing a live-in: they are the def other passes
      // (and the two-stage allocation mode) expect to exist.
      !(MI.getOperand(1).getReg().isPhysical() &&
        MBB->isLiveIn(MI.getOperand(1).getReg().asMCReg()))) {
    LLVM_DEBUG(dbgs() << "Mark unnecessary copy for removal: " << MI);
    Coalesced.push_back(&MI);
  }
}

void RegAllocFastImpl::handleDebugValue(MachineInstr &MI) {
  // Ignore DBG_VALUEs that aren't based on virtual registers. These are
  // mostly constants and frame indices.
  assert(MI.isDebugValue() && "not a DBG_VALUE*");
  for (const auto &MO : MI.debug_operands()) {
    if (!MO.isReg())
      continue;
    Register Reg = MO.getReg();
    if (!Reg.isVirtual())
      continue;
    if (!shouldAllocateRegister(Reg))
      continue;

    LiveRegMap::iterator LRI = findLiveVirtReg(Reg);
    if (LRI != LiveVirtRegs.end() && LRI->PhysReg) {
      // The value is currently in a register. If it is later spilled, the
      // DBG_VALUE is switched over to the stack slot via LiveDbgValueMap.
      SmallVector<MachineOperand *> DbgOps(
          llvm::make_pointer_range(MI.getDebugOperandsForReg(Reg)));
      for (auto &RegMO : DbgOps)
        setPhysReg(MI, *RegMO, *LRI);
      LiveDbgValueMap[Reg].append(DbgOps.begin(), DbgOps.end());
      continue;
    }

    int SS = StackSlotForVirtReg[Reg];
    if (SS != -1) {
      // The value lives in (or last lived in) its stack slot.
      updateDbgValueForSpill(MI, SS, Reg);
      LLVM_DEBUG(dbgs() << "Rewrite DBG_VALUE for spilled memory: " << MI);
      continue;
    }

    // The value's live range ended, but the register that last held it may
    // be untouched since; if so, it still contains the value.
    auto [LastReg, LastSeq] = VRegLastLoc[Reg.virtRegIndex()];
    if (LastReg && LastSeq >= BlockStartSeq) {
      bool Untouched = true;
      for (MCRegUnit Unit : TRI->regunits(LastReg))
        if (UnitChangeSeq[static_cast<unsigned>(Unit)] > LastSeq)
          Untouched = false;
      if (Untouched) {
        LiveReg Loc(Reg);
        Loc.PhysReg = LastReg;
        for (auto &RegMO :
             llvm::make_pointer_range(MI.getDebugOperandsForReg(Reg)))
          setPhysReg(MI, *RegMO, Loc);
        continue;
      }
    }

    // The value was never materialized anywhere we can refer to.
    MI.setDebugValueUndef();
  }
}

void RegAllocFastImpl::handleBundle(MachineInstr &MI) {
  MachineBasicBlock::instr_iterator BundledMI = MI.getIterator();
  ++BundledMI;
  while (BundledMI->isBundledWithPred()) {
    for (MachineOperand &MO : BundledMI->operands()) {
      if (!MO.isReg())
        continue;

      Register Reg = MO.getReg();
      if (!Reg.isVirtual() || !shouldAllocateRegister(Reg))
        continue;

      auto DI = BundleVirtRegsMap.find(Reg);
      assert(DI != BundleVirtRegsMap.end() && "Unassigned virtual register");

      setPhysReg(MI, MO, DI->second);
    }

    ++BundledMI;
  }
}

/// The analysis prepass: record per-virtual-register definition/last-use
/// positions, use counts, cross-block liveness, call crossings, and physreg
/// copy hints, in one forward walk over the function.
void RegAllocFastImpl::analyzeVRegs(MachineFunction &MF) {
  unsigned NumVirtRegs = MRI->getNumVirtRegs();
  VRegInfos.assign(NumVirtRegs, VRegInfo());
  // LastPhysUsePos/LastPhysDefPos are all-zero outside this function; only
  // size them.
  if (LastPhysUsePos.size() < TRI->getNumRegUnits())
    LastPhysUsePos.resize(TRI->getNumRegUnits());
  if (LastPhysDefPos.size() < TRI->getNumRegUnits())
    LastPhysDefPos.resize(TRI->getNumRegUnits());
  PhysSegmentEnds.clear();
  PhysDefsWithUse.clear();
  ActiveDefUnits.clear();

  // Units of physical registers with a use in their current live segment.
  // May contain duplicates and stale (already ended) entries; a zero
  // LastPhysUsePos identifies the latter.
  ActivePhysUnits.clear();
  auto EndPhysSegment = [this](unsigned UnitIdx) {
    if (uint32_t P = LastPhysUsePos[UnitIdx]) {
      PhysSegmentEnds.push_back((uint64_t(P) << 32) | UnitIdx);
      LastPhysUsePos[UnitIdx] = 0;
    }
  };

  uint32_t Pos = 0;
  for (MachineBasicBlock &MBB : MF) {
    int BlockNum = MBB.getNumber();
    uint32_t LastCallPos = 0;
    for (MachineInstr &MI : MBB) {
      if (MI.isDebugInstr())
        continue;
      ++Pos;
      if (MI.isCall())
        LastCallPos = Pos;

      // Record physreg copy hints, looking through one level of virtual
      // copies (the pattern two-address lowering creates).
      if (MI.isFullCopy() && MI.getNumOperands() == 2) {
        Register Dst = MI.getOperand(0).getReg();
        Register Src = MI.getOperand(1).getReg();
        if (Dst.isVirtual() && Src.isPhysical())
          VRegInfos[Dst.virtRegIndex()].HintReg = Src.asMCReg();
        else if (Dst.isVirtual() && Src.isVirtual())
          VRegInfos[Dst.virtRegIndex()].CopySrc = Src;
        else if (Dst.isPhysical() && Src.isVirtual()) {
          VRegInfo &Info = VRegInfos[Src.virtRegIndex()];
          if (!Info.HintReg)
            Info.HintReg = Dst.asMCReg();
          if (Register Chain = Info.CopySrc) {
            VRegInfo &ChainInfo = VRegInfos[Chain.virtRegIndex()];
            if (!ChainInfo.HintReg)
              ChainInfo.HintReg = Dst.asMCReg();
          }
        }
      }

      bool HasPhysDefOrMask = false;
      for (MachineOperand &MO : MI.operands()) {
        if (!MO.isReg()) {
          HasPhysDefOrMask |= MO.isRegMask();
          continue;
        }
        Register Reg = MO.getReg();
        if (Reg.isPhysical()) {
          HasPhysDefOrMask |= MO.isDef();
          if (MO.readsReg()) {
            for (MCRegUnit Unit : TRI->regunits(Reg.asMCReg())) {
              unsigned UnitIdx = static_cast<unsigned>(Unit);
              if (!LastPhysUsePos[UnitIdx])
                ActivePhysUnits.push_back(UnitIdx);
              LastPhysUsePos[UnitIdx] = Pos;
              if (uint32_t DefPos = LastPhysDefPos[UnitIdx]) {
                PhysDefsWithUse.push_back((uint64_t(DefPos) << 32) | UnitIdx);
                // Later uses of the same def need no further entries.
                LastPhysDefPos[UnitIdx] = 0;
              }
            }
          }
          continue;
        }
        if (!Reg.isVirtual())
          continue;
        VRegInfo &Info = VRegInfos[Reg.virtRegIndex()];
        if (MO.isDef()) {
          if (Info.DefBlock < 0) {
            Info.DefBlock = BlockNum;
          } else if (Info.DefBlock != BlockNum) {
            // Defs in multiple blocks (lowered PHIs): stack-home the value.
            Info.LiveCrossBlock = true;
          }
        } else if (MO.readsReg()) {
          if (Info.DefBlock < 0 || Info.DefBlock != BlockNum) {
            // Use before any def (loop-carried) or use in a different block
            // than the def.
            Info.LiveCrossBlock = true;
          }
        } else {
          // Undef use: needs a register but not the value.
          continue;
        }
        // A call between the previous touch of this value in this block and
        // this one means a register holding the value must survive a call.
        if (Info.LastBlock == BlockNum && LastCallPos > Info.LastPos &&
            LastCallPos < Pos)
          Info.CrossesCall = true;
        Info.LastBlock = BlockNum;
        Info.LastPos = Pos;
      }

      // Physical register defs and regmask clobbers end the current live
      // segment of the affected units. This runs after recording this
      // instruction's reads: uses read the old value before defs write.
      if (!HasPhysDefOrMask)
        continue;
      for (const MachineOperand &MO : MI.operands()) {
        if (MO.isRegMask()) {
          for (unsigned UnitIdx : ActivePhysUnits) {
            if (!LastPhysUsePos[UnitIdx])
              continue;
            for (MCRegUnitRootIterator
                     Root(static_cast<MCRegUnit>(UnitIdx), TRI);
                 Root.isValid(); ++Root) {
              if (MachineOperand::clobbersPhysReg(MO.getRegMask(), *Root)) {
                EndPhysSegment(UnitIdx);
                break;
              }
            }
          }
        } else if (MO.isReg() && MO.isDef() && MO.getReg().isPhysical()) {
          for (MCRegUnit Unit : TRI->regunits(MO.getReg().asMCReg())) {
            unsigned UnitIdx = static_cast<unsigned>(Unit);
            EndPhysSegment(UnitIdx);
            if (!LastPhysDefPos[UnitIdx])
              ActiveDefUnits.push_back(UnitIdx);
            LastPhysDefPos[UnitIdx] = Pos;
          }
        }
      }
    }

    // Physical registers live into a successor survive the block: their last
    // use here is not the final use of the segment, and their last def is
    // not dead. Everything else ends at the block boundary.
    if (!ActivePhysUnits.empty() || !ActiveDefUnits.empty()) {
      SmallVector<unsigned, 16> LiveOutUnits;
      for (const MachineBasicBlock *Succ : MBB.successors())
        for (const auto &LI : Succ->liveins())
          for (MCRegUnit Unit : TRI->regunits(LI.PhysReg))
            LiveOutUnits.push_back(static_cast<unsigned>(Unit));
      for (unsigned UnitIdx : ActivePhysUnits) {
        if (llvm::is_contained(LiveOutUnits, UnitIdx))
          LastPhysUsePos[UnitIdx] = 0;
        else
          EndPhysSegment(UnitIdx);
      }
      ActivePhysUnits.clear();
      for (unsigned UnitIdx : ActiveDefUnits) {
        if (uint32_t DefPos = LastPhysDefPos[UnitIdx]) {
          if (llvm::is_contained(LiveOutUnits, UnitIdx))
            PhysDefsWithUse.push_back((uint64_t(DefPos) << 32) | UnitIdx);
          LastPhysDefPos[UnitIdx] = 0;
        }
      }
      ActiveDefUnits.clear();
    }
  }

  llvm::sort(PhysSegmentEnds);
  llvm::sort(PhysDefsWithUse);
}

void RegAllocFastImpl::allocateBasicBlock(MachineBasicBlock &MBB) {
  this->MBB = &MBB;
  LLVM_DEBUG(dbgs() << "\nAllocating " << MBB);

  RegUnitStates.assign(TRI->getNumRegUnits(), regFree);
  BlockStartSeq = ++StateSeq;
  assert(LiveVirtRegs.empty() && "Mapping not cleared from last block?");

  // Physical registers live into the block (arguments in the entry block,
  // exception values in landing pads, values from INLINEASM_BR) arrive
  // pre-assigned.
  PreassignedUnits.clear();
  for (const auto &LiveIn : MBB.liveins()) {
    setPhysRegState(LiveIn.PhysReg, regPreAssigned);
    for (MCRegUnit Unit : TRI->regunits(LiveIn.PhysReg))
      PreassignedUnits.push_back(static_cast<unsigned>(Unit));
  }

  Coalesced.clear();

  // Traverse the block forward, allocating instructions one by one. Newly
  // inserted spills/reloads are skipped by the early-inc iteration and do
  // not advance CurPos, keeping positions in sync with the prepass.
  for (MachineInstr &MI : make_early_inc_range(MBB)) {
    LLVM_DEBUG(dbgs() << "\n>> " << MI << "Regs:"; dumpState());

    // Special handling for debug values. Note that they are not allowed to
    // affect codegen of the other instructions in any way.
    if (MI.isDebugValue()) {
      handleDebugValue(MI);
      continue;
    }
    if (MI.isDebugInstr())
      continue;

    ++CurPos;
    allocateInstruction(MI);

    // Once BUNDLE header is assigned registers, same assignments need to be
    // done for bundled MIs.
    if (MI.getOpcode() == TargetOpcode::BUNDLE) {
      handleBundle(MI);
    }
  }

  LLVM_DEBUG(dbgs() << "End Regs:"; dumpState());

  // Everything surviving the block boundary is stack-homed. Cross-block
  // values were spilled at their defs, except those defined by IMPLICIT_DEF.
  for (LiveReg &LR : LiveVirtRegs) {
    if (LR.PhysReg != 0 && !LR.StackValid && !LR.Error &&
        vregInfo(LR.VirtReg).LiveCrossBlock)
      spill(MBB.getFirstTerminator(), LR.VirtReg, LR.PhysReg, /*Kill=*/true,
            /*LiveOut=*/true);
  }
  LiveVirtRegs.clear();

  // Erase all the coalesced copies. We are delaying it until now because
  // LiveVirtRegs might refer to the instrs.
  for (MachineInstr *MI : Coalesced)
    MBB.erase(MI);
  NumCoalesced += Coalesced.size();

  LLVM_DEBUG(MBB.dump());
}

bool RegAllocFastImpl::runOnMachineFunction(MachineFunction &MF) {
  LLVM_DEBUG(dbgs() << "********** FAST REGISTER ALLOCATION **********\n"
                    << "********** Function: " << MF.getName() << '\n');
  MRI = &MF.getRegInfo();
  const TargetSubtargetInfo &STI = MF.getSubtarget();
  TRI = STI.getRegisterInfo();
  TII = STI.getInstrInfo();
  MFI = &MF.getFrameInfo();
  MRI->freezeReservedRegs();
  RegClassInfo.runOnMachineFunction(MF);
  unsigned NumRegUnits = TRI->getNumRegUnits();
  InstrGen = 0;
  UsedInInstr.assign(NumRegUnits, 0);

  CSRFirstOrders.clear();

  // initialize the virtual->physical register map to have a 'null'
  // mapping for all virtual registers
  unsigned NumVirtRegs = MRI->getNumVirtRegs();
  StackSlotForVirtReg.resize(NumVirtRegs);
  LiveVirtRegs.setUniverse(NumVirtRegs);

  TrackDbgLoc = MF.getFunction().getSubprogram() != nullptr;
  if (TrackDbgLoc) {
    UnitChangeSeq.assign(NumRegUnits, 0);
    VRegLastLoc.assign(NumVirtRegs, {MCPhysReg(0), 0});
  }

  analyzeVRegs(MF);
  CurPos = 0;
  SegEndCursor = DefUseCursor = 0;

  // Loop over all of the basic blocks, eliminating virtual register references
  for (MachineBasicBlock &MBB : MF)
    allocateBasicBlock(MBB);

  if (ClearVirtRegs) {
    // All machine operands and other references to virtual registers have been
    // replaced. Remove the virtual registers.
    MRI->clearVirtRegs();
  }

  StackSlotForVirtReg.clear();
  LiveDbgValueMap.clear();
  return true;
}

PreservedAnalyses RegAllocFastPass::run(MachineFunction &MF,
                                        MachineFunctionAnalysisManager &) {
  MFPropsModifier _(*this, MF);
  RegAllocFastImpl Impl(Opts.Filter, Opts.ClearVRegs);
  bool Changed = Impl.runOnMachineFunction(MF);
  if (!Changed)
    return PreservedAnalyses::all();
  auto PA = getMachineFunctionPassPreservedAnalyses();
  PA.preserveSet<CFGAnalyses>();
  return PA;
}

void RegAllocFastPass::printPipeline(
    raw_ostream &OS, function_ref<StringRef(StringRef)> MapClassName2PassName) {
  bool PrintFilterName = Opts.FilterName != "all";
  bool PrintNoClearVRegs = !Opts.ClearVRegs;
  bool PrintSemicolon = PrintFilterName && PrintNoClearVRegs;

  OS << "regallocfast";
  if (PrintFilterName || PrintNoClearVRegs) {
    OS << '<';
    if (PrintFilterName)
      OS << "filter=" << Opts.FilterName;
    if (PrintSemicolon)
      OS << ';';
    if (PrintNoClearVRegs)
      OS << "no-clear-vregs";
    OS << '>';
  }
}

FunctionPass *llvm::createFastRegisterAllocator() { return new RegAllocFast(); }

FunctionPass *llvm::createFastRegisterAllocator(RegAllocFilterFunc Ftor,
                                                bool ClearVirtRegs) {
  return new RegAllocFast(Ftor, ClearVirtRegs);
}
