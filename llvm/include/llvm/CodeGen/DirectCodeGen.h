//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Target-independent part of the direct -O0 code generator. It compiles an
// IR function straight to machine code in a private buffer, without
// MachineFunction, and hands the bytes to AsmPrinter. Any function it cannot
// handle is left untouched for the regular pipeline.
//
// Value model:
// * static allocas live in fixed frame slots;
// * a value with a single use in its own block, or that is the PHI input of
//   an unconditional branch, stays in its register and gets a slot only if
//   it is spilled;
// * every other value is stored to its own frame slot at the definition, and
//   its register is kept as a clean copy until it is needed or a call or the
//   block end is reached.
// Only caller-saved registers are used and the frame pointer is always set up.
//
// A target derives DirectCompiler, which selects instructions and lays out
// the frame, and DirectCodeGen, the pass that drives it.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CODEGEN_DIRECTCODEGEN_H
#define LLVM_CODEGEN_DIRECTCODEGEN_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/CodeGen/AsmPrinter.h"
#include "llvm/CodeGen/TargetPassConfig.h"
#include "llvm/IR/Argument.h"
#include "llvm/IR/Attributes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/MC/MCDwarf.h"
#include "llvm/MC/MCFixup.h"
#include "llvm/MC/MCInst.h"
#include "llvm/Pass.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/MathExtras.h"
#include <memory>

namespace llvm {
class Constant;
class DataLayout;
class DirectCodeGen;
class IntrinsicInst;
class MCCodeEmitter;
class MCSubtargetInfo;
class TargetMachine;

// Compiles one function.
class LLVM_ABI DirectCompiler {
public:
  enum : uint8_t { MaxRegs = 64, NoReg = 0xff };

  // The registers of a target. They are ids below NumRegs; ids from FirstFP
  // on are floating-point registers.
  struct RegisterInfo {
    ArrayRef<uint8_t> GPOrder, FPOrder; // allocation orders
    ArrayRef<uint8_t> GPArgs;           // integer argument registers
    uint8_t FirstFP, NumFPArgs, NumRegs;
    // Registers that return a two-scalar struct: the integer ones, and the
    // first floating-point one.
    uint8_t RetGP[2], FirstRetFP;
    // The register of an sret argument if it is not an integer argument, and
    // the register argRegHint() gives for a returned value.
    uint8_t SRetArg, RetHint;
  };

  struct ValTy {
    uint8_t Size = 0; // in bytes
    bool FP = false;
    bool I1 = false;
  };
  // A pointer, or the whole of a general-purpose register.
  static constexpr ValTy GP64{8, false, false};

  struct ValInfo {
    enum : uint8_t { Normal, Alloca, DeferredCmp, DeferredGEP, DeferredLoad };
    // Frame offset of the spill slot (or of the object for an alloca). 0
    // means no slot yet.
    int32_t Slot = 0;
    uint8_t Reg = NoReg;
    // Normal, a frame object, or a value the target rebuilds at each use.
    uint8_t Kind = Normal;
  };

  // Encodings of instructions whose operands are all registers and
  // immediates. They need no fixups (or are branches whose fixup is dropped),
  // so the bytes depend only on the instruction and the subtarget.
  class EncodingCache {
  public:
    struct Key {
      const MCSubtargetInfo *STI;
      unsigned Opcode, Flags, NumOps, ImmMask;
      bool Branch;
      int64_t Ops[8];
      bool operator==(const Key &O) const {
        if (STI != O.STI || Opcode != O.Opcode || Flags != O.Flags ||
            NumOps != O.NumOps || ImmMask != O.ImmMask || Branch != O.Branch)
          return false;
        // A loop rather than std::equal, which becomes a bcmp call.
        for (unsigned Idx = 0; Idx != NumOps; ++Idx)
          if (Ops[Idx] != O.Ops[Idx])
            return false;
        return true;
      }
    };
    struct Entry {
      Key K;
      uint8_t Len = 0;
      // One spare byte so that a hit copies a fixed 16 bytes.
      char Bytes[16];
    };

    // Return the slot for I, or null if I cannot be cached. A slot with Len 0
    // or a different key is a miss.
    Entry *slot(const MCInst &I, const MCSubtargetInfo &STI, bool Branch,
                Key &K) {
      if (I.getNumOperands() > std::size(K.Ops))
        return nullptr;
      K.STI = &STI;
      K.Opcode = I.getOpcode();
      K.Flags = I.getFlags();
      K.NumOps = I.getNumOperands();
      K.ImmMask = 0;
      K.Branch = Branch;
      uint64_t H = K.Opcode * 0x9E3779B97F4A7C15ULL;
      for (unsigned Idx = 0; Idx != K.NumOps; ++Idx) {
        const MCOperand &Op = I.getOperand(Idx);
        if (Op.isReg()) {
          K.Ops[Idx] = Op.getReg().id();
        } else if (Op.isImm()) {
          K.Ops[Idx] = Op.getImm();
          K.ImmMask |= 1u << Idx;
        } else {
          return nullptr;
        }
        H = (H ^ uint64_t(K.Ops[Idx])) * 0x9E3779B97F4A7C15ULL;
      }
      return &Table[(H >> 40) % Size];
    }
    static bool hit(const Entry &E, const Key &K) { return E.Len && E.K == K; }
    static void fill(Entry &E, const Key &K, ArrayRef<char> Bytes) {
      E.K = K;
      E.Len = Bytes.size();
      llvm::copy(Bytes, E.Bytes);
    }
    void clear() {
      for (unsigned Idx = 0; Idx != Size; ++Idx)
        Table[Idx].Len = 0;
    }

  private:
    static constexpr unsigned Size = 4096;
    std::unique_ptr<Entry[]> Table{new Entry[Size]};
  };

  // Per-module state shared by all functions.
  struct ModuleState {
    const DirectCodeGen *Pass = nullptr;
    // Kind id of !heapallocsite, which only CodeView emission consumes.
    unsigned HeapAllocSiteKind = 0;
    // Whether every attribute of a list is handled. Lists are uniqued, so the
    // verdict is cached by pointer.
    DenseMap<const void *, bool> AttrVerdicts;
    LLVM_ABI bool attrsHandled(AttributeList AL);
    // Why a call to a function with these attributes is not handled.
    enum CalleeVerdict : uint8_t {
      CalleeOK,
      CalleeParamAttr,
      CalleeFnAttr,
      CalleeDontCall
    };
    DenseMap<const void *, CalleeVerdict> CalleeVerdicts;
    LLVM_ABI CalleeVerdict calleeVerdict(AttributeList AL);

    // Symbols of globals, without and with a preferred local alias, and of
    // library functions.
    DenseMap<const GlobalValue *, MCSymbol *> Syms[2];
    DenseMap<const char *, MCSymbol *> LibSyms;
    EncodingCache Encodings;

    // Per-function buffers, kept across functions to reuse their storage.
    // Vals is indexed by argument number, then by instruction number.
    SmallVector<ValInfo, 0> Vals;
    SmallVector<char, 0> Code;
    SmallVector<MCFixup, 0> Fixups;
    SmallVector<uint32_t, 0> BlockOff;
    // Whether the output is text, which prints Insts instead of emitting Code.
    bool Text = false;

    void reset(const DirectCodeGen &P, const Module &M);
  };

  virtual ~DirectCompiler();

  // Compile the function. On failure FailReason says why.
  bool run();

  SmallVector<char, 0> &Code;
  SmallVector<MCFixup, 0> &Fixups;
  // For text output, each instruction with its offset in Code, and the labels
  // of branch targets.
  SmallVector<std::pair<uint32_t, MCInst>, 0> Insts;
  DenseMap<uint32_t, MCSymbol *> Labels;
  SmallVector<std::pair<unsigned, MCCFIInstruction>, 8> CFI;
  // As MachineFrameInfo::getStackSize.
  uint64_t StackSize = 0;
  // Exception table contents; meaningful only if HasLandingPads.
  AsmPrinter::CompiledFunctionEH EH;
  bool HasLandingPads = false;
  // Line rows and variable locations; meaningful only if HasDebug.
  AsmPrinter::CompiledFunctionDebug Debug;
  bool HasDebug = false;
  const char *FailReason = nullptr;
  std::string FailDetail;

  // The compiled function, given Labels sorted by offset.
  AsmPrinter::CompiledFunction
  result(ArrayRef<std::pair<uint32_t, MCSymbol *>> SortedLabels) const;

protected:
  DirectCompiler(const Function &F, const MCSubtargetInfo &STI, AsmPrinter &AP,
                 const MCCodeEmitter &CE, ModuleState &G,
                 const RegisterInfo &Regs);

  const Function &F;
  const MCSubtargetInfo &STI;
  const DataLayout &DL;
  AsmPrinter &AP;
  MCContext &Ctx;
  const MCCodeEmitter &CE;
  ModuleState &G;

  void fail(const char *Reason) {
    if (!FailReason)
      FailReason = Reason;
  }
  // Fail, naming the first unhandled attribute of AL for the report.
  void failAttrs(const char *Reason, AttributeList AL);

  // Encode I at the end of Code.
  void emit(const MCInst &I) { encode(I, false); }
  // As emit, for a branch whose displacement is patched in the buffer: the
  // encoder's fixup is dropped.
  void emitBranch(const MCInst &I) { encode(I, true); }
  void encode(const MCInst &I, bool Branch);
  // For text output, the recorded instruction that contains offset Off.
  std::pair<uint32_t, MCInst> *instAt(uint32_t Off);
  // For text output, make operand OpIdx of the branch at Pos refer to a label
  // at Target.
  void setBranchTarget(uint32_t Pos, unsigned OpIdx, uint32_t Target);

  // Types and values.
  bool classify(Type *T, ValTy &VT);
  ValTy typeOf(const Value *V) {
    ValTy VT;
    if (!classify(V->getType(), VT)) {
      fail("unsupported type");
      VT = GP64;
    }
    return VT;
  }
  // The state of an argument or instruction, or null for other values.
  ValInfo *lookup(const Value *V) {
    if (auto *A = dyn_cast<Argument>(V))
      return &Vals[A->getArgNo()];
    if (auto *I = dyn_cast<Instruction>(V))
      return &Vals[NumArgs + I->getNumber()];
    return nullptr;
  }
  ValInfo &info(const Value *V) { return *lookup(V); }
  int32_t slotFor(const Value *V) { return slotFor(info(V)); }
  int32_t slotFor(ValInfo &VI) {
    if (!VI.Slot)
      VI.Slot = allocFrame(8, 8);
    return VI.Slot;
  }

  // Register management.
  bool isFPReg(uint8_t R) const { return R >= Regs.FirstFP; }
  bool isArgReg(uint8_t R) const { return ArgRegs >> R & 1; }
  void release(uint8_t R) {
    if (R != NoReg) {
      Owner[R] = nullptr;
      Clean &= ~(uint64_t(1) << R);
    }
  }
  bool isClean(uint8_t R) const { return Clean >> R & 1; }
  void spillReg(uint8_t R);
  // Store R to the slot of V and keep R as a clean copy.
  void storeAndCache(const Value *V, ValTy VT, uint8_t R);
  // Forget all clean copies. Nothing is emitted, so this is safe wherever
  // control flow merges or registers are clobbered.
  void dropClean();
  uint8_t allocFrom(ArrayRef<uint8_t> Order);
  uint8_t allocGP() { return allocFrom(Regs.GPOrder); }
  uint8_t allocFP() { return allocFrom(Regs.FPOrder); }
  // Take R for the current instruction, spilling a value that lives there.
  void claim(uint8_t R);

  MCSymbol *sym(const GlobalValue &GV, bool PreferLocal);
  MCSymbol *libSym(const char *Name);
  // Materialize a constant into R, or into a fresh register if R is NoReg.
  uint8_t constant(const Constant *C, uint8_t R);

  // Get V into a register owned by the current instruction. A register V
  // lives in is taken over.
  uint8_t use(const Value *V);
  // Copy V into R, which the caller has claimed. A clean copy of V stays
  // where it is.
  void useInto(const Value *V, uint8_t R);
  // As claim + useInto, but take R over if V already lives in it.
  void useIn(const Value *V, uint8_t R);
  static bool isTemp(const Value &V, const BasicBlock *BB);
  // As useDeferred, for any kind but Normal.
  uint8_t useSpecial(const Value *V, ValInfo &VI, ValTy VT, uint8_t R);
  // Record that argument A arrives in R. NeedSlot forces a frame slot.
  void assignArgReg(const Argument &A, ValTy VT, uint8_t R, bool NeedSlot);
  // If the value of I reaches a call in this block as a register argument or
  // is returned, possibly through operations that compute in place, return
  // that register.
  uint8_t argRegHint(const Instruction &I);
  // Record the result of I, held in R by the current instruction.
  void def(const Instruction &I, uint8_t R);
  // Release the registers of the operands of I, which emits nothing.
  void dropOperands(const Instruction &I);
  // Zero-extend to 32 bits unless the last use() already produced an extended
  // value.
  void zext32Use(uint8_t R, ValTy VT) {
    if (!ZExtKnown)
      zext32(R, VT);
  }
  void zext64(uint8_t R, ValTy VT) {
    if (VT.Size == 4)
      zextLow32(R);
    else
      zext32(R, VT);
  }
  void zext64Use(uint8_t R, ValTy VT) {
    if (!ZExtKnown)
      zext64(R, VT);
  }

  // Whether the load can be folded into its user as a memory operand. The
  // user must be the next instruction, so no store comes in between.
  bool canFoldLoad(const LoadInst &I, ValTy VT);
  // The register to load I into. Tmp, the register of the address, is taken
  // over if it is chosen.
  uint8_t loadDest(const LoadInst &I, ValTy VT, uint8_t &Tmp);
  // The register for a value computed from an address whose register is Tmp:
  // R, or if R is NoReg, Tmp or a new register. An unused Tmp is released.
  uint8_t resultReg(uint8_t R, uint8_t Tmp) {
    if (R == NoReg)
      return Tmp != NoReg ? Tmp : allocGP();
    release(Tmp);
    return R;
  }

  // The offset of a frame object or a constant offset from one, if it fits 32
  // bits. This never fails the function.
  bool frameOffset(const Value *P, int64_t &Off);

  // A struct of one or two scalars is kept in a 16-byte frame slot, one
  // 8-byte part per element, and is returned in two registers.
  struct AggPart {
    ValTy VT;
    uint64_t MemOff; // offset of the element in the in-memory struct
    uint8_t RetReg;
  };
  bool aggParts(Type *T, SmallVectorImpl<AggPart> &Parts);
  static int32_t aggPartSlot(int32_t Slot, unsigned Idx) {
    return Slot + 8 * Idx;
  }
  int32_t aggSlotFor(const Value *V) {
    ValInfo &VI = info(V);
    if (!VI.Slot)
      VI.Slot = allocFrame(16, 8);
    return VI.Slot;
  }

  // Pos locates the branch in a target-defined way; Target is a code offset,
  // taken from BB once all blocks are placed.
  struct Branch {
    uint32_t Pos;
    uint32_t Target;
    const BasicBlock *BB;
    bool Cond;
  };
  SmallVector<Branch, 0> Branches;
  // Jump to BB unless it is the next block; Force emits the jump regardless.
  void jmpTo(const BasicBlock &BB, bool Force = false) {
    if (&BB != NextBB || Force)
      Branches.push_back({emitJmp(), 0, &BB, false});
  }
  void jccTo(unsigned CC, const BasicBlock &BB) {
    Branches[jccFwd(CC)].BB = &BB;
  }
  // Emit a conditional jump to a local label bound later with bindFwd.
  size_t jccFwd(unsigned CC) {
    Branches.push_back({emitJcc(CC), 0, nullptr, true});
    return Branches.size() - 1;
  }
  void bindFwd(size_t Idx) { Branches[Idx].Target = Code.size(); }
  uint32_t targetOf(const Branch &B) const {
    return B.BB ? BlockOff[B.BB->getNumber()] : B.Target;
  }

  // How a call passes an argument.
  enum class ArgExt : uint8_t { None, Zero, Sign };
  struct ArgLoc {
    const Value *V;
    ValTy VT = {};
    uint8_t Reg = NoReg;
    unsigned StackOff = 0;
    ArgExt Ext = ArgExt::None;
    // For a byval argument, the bytes copied to StackOff.
    unsigned ByValSize = 0;
  };
  // The extension of argument Idx of a call, or of a library call if Attrs is
  // null.
  static ArgExt argExt(const CallBase *Attrs, unsigned Idx, ValTy VT);
  // Spill every live value except call operands sitting in registers that
  // argument setup does not touch.
  void spillForCall(const Value *IndirectCallee, ArrayRef<const Value *> Args,
                    ArrayRef<ArgLoc> Locs);
  // Forget the registers a call clobbers and record the result of I.
  void finishCall(const Instruction &I);
  void emitNoReturnCall(const char *LibName, bool MayThrow);
  // Move the value I returns to its registers.
  void setReturnValue(const ReturnInst &I);
  // Record the call instruction emitted since Start.
  void recordCall(size_t Start, bool MayThrow);
  unsigned typeIdFor(const GlobalValue *GV);
  // Move the PHI inputs of the edge Pred -> Succ into the PHI slots.
  void edge(const BasicBlock &Pred, const BasicBlock &Succ);
  bool checkCall(const CallBase &CB);

  // Final code offset of each block, by block number.
  SmallVector<uint32_t, 0> &BlockOff;
  // Map every recorded code offset through NewOff after bytes were removed.
  template <typename FnT> void remapOffsets(FnT NewOff) {
    for (auto &CS : EH.CallSites) {
      uint32_t End = NewOff(CS.Start + CS.Length);
      CS.Start = NewOff(CS.Start);
      CS.Length = End - CS.Start;
      if (CS.LandingPad)
        CS.LandingPad = NewOff(CS.LandingPad);
    }
    for (auto &Row : Debug.Rows)
      Row.Offset = NewOff(Row.Offset);
    for (MCFixup &Fixup : Fixups)
      Fixup.setOffset(NewOff(Fixup.getOffset()));
    for (auto &Entry : CFI)
      Entry.first = NewOff(Entry.first);
    for (auto &Entry : Insts)
      Entry.first = NewOff(Entry.first);
  }

  SmallVector<ValInfo, 0> &Vals;
  unsigned NumArgs;
  const Value *Owner[MaxRegs];
  // Registers that are a copy of a value that is also in its frame slot, so
  // that they can be dropped without a store.
  uint64_t Clean = 0;
  // Owner value for a register held by the instruction being compiled.
  const Value *const Busy;
  // Bytes of frame objects and slots allocated so far.
  uint64_t FrameSize = 0;
  // Largest outgoing stack argument area of a call.
  unsigned MaxOutgoing = 0;
  // Offset of the epilogue of the return being compiled.
  size_t EpilogueStart = 0;
  bool HasCalls = false;
  const BasicBlock *NextBB = nullptr;
  // Whether the last use()/useInto() produced a value whose bits above its
  // type are zero.
  bool ZExtKnown = false;
  // Added to a frame offset to get the offset of the same object as a fixed
  // MachineFrameInfo object, for debug info.
  int64_t FixedObjectBias = 0;
  // Frame offset of the stack protector guard, or 0 if the function has none.
  // It is allocated before any other frame object.
  int32_t StackGuardSlot = 0;
  // Whether every static alloca is guarded, not only those the stack
  // protector analysis singles out.
  bool GuardAllAllocas = false;
  // Minimum alignment of a static alloca in the frame.
  uint64_t MinAllocaAlign = 8;
  // Width of the signed constant offsets that address arithmetic handles.
  unsigned DispBits = 32;
  bool fitsDisp(int64_t Disp) const { return isIntN(DispBits, Disp); }
  // The offset that a GEP with constant indices adds.
  int64_t constGEPOffset(const GetElementPtrInst &GEP);

  // Target hooks.

  // Allocate Size bytes in the frame and return their frame offset.
  virtual int32_t allocFrame(uint64_t Size, uint64_t Alignment) = 0;
  // As allocFrame, for the stack protector guard and then the objects it
  // guards: an overflow of one must reach the guard before the return address
  // or any other frame object.
  virtual int32_t allocGuardedFrame(uint64_t Size, uint64_t Alignment) {
    return allocFrame(Size, Alignment);
  }
  // Slots hold the whole general-purpose register so reloads of any width
  // work.
  virtual void storeSlot(int32_t Slot, ValTy VT, uint8_t R) = 0;
  virtual void loadSlot(uint8_t R, ValTy VT, int32_t Slot) = 0;
  // Store an integer constant to a slot; return false if there is no
  // instruction for it.
  virtual bool storeImmToSlot(int32_t Slot, int64_t Imm) = 0;
  virtual void regMove(uint8_t Dst, uint8_t Src) = 0;
  virtual void movImm(uint8_t R, int64_t V, unsigned Size) = 0;
  virtual void fpConstant(uint8_t R, uint64_t Bits, ValTy VT) = 0;
  virtual void globalAddr(const GlobalValue &GV, int64_t Off, uint8_t R) = 0;
  virtual void zext32(uint8_t R, ValTy VT) = 0;
  // Zero the upper 32 bits of R.
  virtual void zextLow32(uint8_t R) = 0;
  virtual void sext32(uint8_t R, ValTy VT) = 0;
  virtual void sext64(uint8_t R, ValTy VT) = 0;
  // Dst = Base + Disp, where Base NoReg is the frame pointer.
  virtual void addDisp(uint8_t Dst, uint8_t Base, int64_t Disp) = 0;
  // R += X * Stride.
  virtual void addScaled(uint8_t R, uint8_t X, int64_t Stride) = 0;
  // Whether the target addresses P without a register: a frame object or a
  // constant offset from one.
  virtual bool isFixedAddress(const Value *P);
  // Rebuild a deferred address or load into R, or into a register of the
  // hook's choice if R is NoReg, and return the register.
  virtual uint8_t useDeferred(const Value *V, ValInfo &VI, ValTy VT,
                              uint8_t R) = 0;
  // The register that returns element Idx of its class of a two-scalar
  // struct.
  uint8_t aggRetReg(bool FP, unsigned Idx) const {
    return FP ? Regs.FirstRetFP + Idx : Regs.RetGP[Idx];
  }
  // The registers that hold the exception pointer and selector at a landing
  // pad.
  virtual uint8_t exceptionPointerReg() = 0;
  virtual uint8_t exceptionSelectorReg() = 0;

  // Emit the prologue and assign the arguments.
  virtual bool prologue() = 0;
  // Patch the frame size and resolve branches once all blocks are compiled.
  virtual bool finish() = 0;

  // Branches. Condition codes are target-defined.
  // Emit a jump with a zero displacement and return its Branch::Pos.
  virtual uint32_t emitJmp() = 0;
  virtual uint32_t emitJcc(unsigned CC) = 0;
  virtual unsigned invertCC(unsigned CC) = 0;
  // Test the i1 value C and return the condition code for true.
  virtual unsigned emitCondition(const Value *C) = 0;
  // Compare the operands of I and return the condition code for true.
  virtual unsigned emitICmp(const ICmpInst &I) = 0;
  // Set R to 1 if CC holds and to 0 otherwise.
  virtual void setCC(uint8_t R, unsigned CC) = 0;
  // Compare R with a switch case value and return the condition code for
  // equal.
  virtual unsigned emitCaseCompare(uint8_t R, bool Is64, int64_t V) = 0;
  virtual void emitTrap(bool Debug) = 0;
  // Called where the unwinder enters a landing pad.
  virtual void emitLandingPadEntry() = 0;

  // Emit a direct call to a function or a library symbol.
  virtual void emitCallTo(const Function *Direct, const char *LibName,
                          bool MayThrow) = 0;
  // Emit a call with its arguments and record the result of I.
  virtual void emitCall(const Instruction &I, const Value *Callee,
                        const char *LibName, ArrayRef<const Value *> Args,
                        const CallBase *Attrs, bool IsVarArg,
                        bool MayThrow) = 0;

  // Whether a load folds into the compare that uses it.
  virtual bool canFoldLoadInto(const ICmpInst &Cmp, const LoadInst &L) {
    return false;
  }
  virtual void compileLoad(const LoadInst &I) = 0;
  virtual void compileStore(const StoreInst &I) = 0;
  virtual void compileBinary(const BinaryOperator &I) = 0;
  virtual void compileFCmp(const FCmpInst &I) = 0;
  virtual void compileSelect(const SelectInst &I) = 0;
  virtual void compileCast(const CastInst &I) = 0;
  virtual void compileFNeg(const UnaryOperator &I) = 0;
  virtual void compileRet(const ReturnInst &I) = 0;
  // Intrinsics the target-independent code does not handle.
  virtual void compileTargetIntrinsic(const IntrinsicInst &I) = 0;

private:
  const RegisterInfo Regs;
  uint64_t ArgRegs = 0;
  // Whether the function has a subprogram, i.e. rows and variables are
  // collected.
  bool RecordDebug = false;
  // Variables behind a pointer argument; their expressions get a deref.
  SmallVector<unsigned, 0> DerefVars;
  // Call sites are recorded for every call that may throw in a function with
  // a personality. CallSiteLPad holds the landing pad block of a call site
  // until the block offsets are final.
  SmallVector<const BasicBlock *, 8> CallSiteLPad;
  DenseMap<const LandingPadInst *, uint32_t> PadAction;

  // Steps of run(); each returns false if the function fails.
  bool checkFunction();
  bool layoutStackProtector();
  bool blockOrder(SmallVectorImpl<const BasicBlock *> &Order);
  bool compileBlock(const BasicBlock &BB);
  // Add the line rows of I, whose code starts at Before.
  void addDebugRows(const Instruction &I, size_t Before, bool &BlockStart,
                    const DILocation *&LastLoc);
  void appendSLEB(int64_t V);
  // Set the landing pad offset of each call site from its block.
  void resolveLandingPads();
  uint32_t actionFor(const LandingPadInst &LPI);
  void recordDebugRecords(const Instruction &I, bool &BlockStart);
  void compileInst(const Instruction &I);
  void compileAlloca(const AllocaInst &I, bool Guarded = false);
  void compileGEP(const GetElementPtrInst &I);
  void compileICmp(const ICmpInst &I);
  void compileExtractValue(const ExtractValueInst &I);
  void compileInsertValue(const InsertValueInst &I);
  void compileCall(const CallInst &I);
  void compileIntrinsic(const IntrinsicInst &I);
  void compileInvoke(const InvokeInst &I);
  void compileLandingPad(const LandingPadInst &I);
  void compileResume(const ResumeInst &I);
  void compileCondBr(const CondBrInst &I);
  void compileSwitch(const SwitchInst &I);
};

// Defined here so that the target's compileLoad and emitCall, which call them
// for every load and argument, can inline them.
inline bool DirectCompiler::canFoldLoad(const LoadInst &I, ValTy VT) {
  if (VT.FP || VT.I1 || I.isVolatile() || !I.hasOneUse())
    return false;
  auto *U = cast<Instruction>(I.user_back());
  if (U != I.getNextNode())
    return false;
  auto *Cmp = dyn_cast<ICmpInst>(U);
  return isa<SExtInst>(U) || (Cmp && canFoldLoadInto(*Cmp, I));
}

inline uint8_t DirectCompiler::loadDest(const LoadInst &I, ValTy VT,
                                        uint8_t &Tmp) {
  if (VT.FP)
    return allocFP();
  if (uint8_t Hint = argRegHint(I); Hint != NoReg && !Owner[Hint]) {
    Owner[Hint] = Busy;
    return Hint;
  }
  return Tmp != NoReg ? std::exchange(Tmp, NoReg) : allocGP();
}

inline DirectCompiler::ArgExt DirectCompiler::argExt(const CallBase *Attrs,
                                                     unsigned Idx, ValTy VT) {
  if (!Attrs) {
    // A library call: memset takes its byte as an int.
    return VT.FP ? ArgExt::None : ArgExt::Zero;
  }
  if (Attrs->paramHasAttr(Idx, Attribute::ZExt))
    return ArgExt::Zero;
  if (Attrs->paramHasAttr(Idx, Attribute::SExt))
    return ArgExt::Sign;
  return ArgExt::None;
}

// The pass that runs a target's DirectCompiler on each function, emits the
// result through the AsmPrinter of its function pass manager and skips the
// remaining passes for that function.
class LLVM_ABI DirectCodeGen : public FunctionPass {
public:
  ~DirectCodeGen() override;
  void getAnalysisUsage(AnalysisUsage &AU) const override;
  bool doInitialization(Module &M) override;
  bool runOnFunction(Function &F) override;

  // Attributes that either do not affect code generation or are handled.
  virtual bool isHandledAttr(Attribute A) const;
  // Whether a function attribute of a callee changes the code of a call to
  // it, in a way that is not handled.
  virtual bool changesCalls(Attribute A) const { return false; }

protected:
  // \p AsmPrinterID is the pass ID of the target's AsmPrinter.
  DirectCodeGen(char &ID, TargetMachine &TM, const DirectCodeGenOptions &Opts,
                const void *AsmPrinterID);

  // Module flags the target handles beyond the target-independent ones.
  virtual bool isHandledModuleFlag(const Module::ModuleFlagEntry &Flag) const {
    return false;
  }
  // Return null if the module can be compiled, or the reason it cannot,
  // for options and features that passes of the regular pipeline implement.
  virtual const char *unsupportedModuleReason(const Module &M) {
    return nullptr;
  }
  virtual const char *unsupportedSubtargetReason(const Function &F) = 0;
  // Construct the target's compiler for \p F and pass it to runAndEmit().
  virtual bool compile(const Function &F, AsmPrinter &AP,
                       const MCCodeEmitter &CE,
                       DirectCompiler::ModuleState &G) = 0;
  // Run \p C and emit its function. Return false if it failed.
  bool runAndEmit(const Function &F, DirectCompiler &C);

  TargetMachine &TM;

private:
  // Find the AsmPrinter and set up text output, then return null if the
  // module can be compiled, or the reason it cannot.
  const char *setUpModule(const Module &M);
  const char *checkModule(const Module &M);

  DirectCodeGenOptions Opts;
  const void *AsmPrinterID;
  unsigned NumDone = 0;
  DirectCompiler::ModuleState G;
  enum { Unknown, Supported, Unsupported } ModuleSupport = Unknown;
  AsmPrinter *AP = nullptr;
  // A text streamer has no code emitter.
  std::unique_ptr<MCCodeEmitter> TextCE;
  const char *FailReason = nullptr;
  std::string FailDetail;
};

} // namespace llvm

#endif
