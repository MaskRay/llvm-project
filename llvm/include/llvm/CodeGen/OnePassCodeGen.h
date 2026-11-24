//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Target-independent part of the one-pass -O0 code generator. It compiles an
// IR function straight to machine code in a private buffer, without
// MachineFunction, and hands the bytes to AsmPrinter. Any function it cannot
// handle is left untouched for the regular pipeline.
//
// Value model:
// * static allocas live in fixed frame slots;
// * a value with a single use in its own block lives only in a register;
// * every other value is stored to its own frame slot at the definition, and
//   its register is kept as a clean copy until it is needed or a call or the
//   block end is reached.
// Only caller-saved registers are used and the frame pointer is always set up.
//
// A target derives OnePassCompiler, which selects instructions and lays out
// the frame, and OnePassCodeGen, the pass that drives it.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CODEGEN_ONEPASSCODEGEN_H
#define LLVM_CODEGEN_ONEPASSCODEGEN_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/CodeGen/AsmPrinter.h"
#include "llvm/CodeGen/TargetPassConfig.h"
#include "llvm/IR/Argument.h"
#include "llvm/IR/Attributes.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Module.h"
#include "llvm/MC/MCDwarf.h"
#include "llvm/MC/MCFixup.h"
#include "llvm/MC/MCInst.h"
#include "llvm/Pass.h"
#include "llvm/Support/Compiler.h"
#include <memory>

namespace llvm {
class CondBrInst;
class Constant;
class DataLayout;
class ExtractValueInst;
class FCmpInst;
class GetElementPtrInst;
class ICmpInst;
class InsertValueInst;
class IntrinsicInst;
class InvokeInst;
class LandingPadInst;
class LoadInst;
class MCCodeEmitter;
class MCSubtargetInfo;
class OnePassCodeGen;
class ResumeInst;
class ReturnInst;
class SelectInst;
class StoreInst;
class SwitchInst;
class TargetMachine;

// Compiles one function. Registers are small target-defined ids; ids from
// FirstFPReg on are floating-point registers.
class LLVM_ABI OnePassCompiler {
public:
  enum : uint8_t { MaxRegs = 64, NoReg = 0xff };

  struct ValTy {
    uint8_t Size = 0; // in bytes
    bool FP = false;
    bool I1 = false;
  };

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
  // immediates. They need no fixups, so the bytes depend only on the
  // instruction and the subtarget.
  class EncodingCache {
  public:
    struct Key {
      const MCSubtargetInfo *STI;
      unsigned Opcode, Flags, NumOps, ImmMask;
      int64_t Ops[8];
      bool operator==(const Key &O) const {
        return STI == O.STI && Opcode == O.Opcode && Flags == O.Flags &&
               NumOps == O.NumOps && ImmMask == O.ImmMask &&
               std::equal(Ops, Ops + NumOps, O.Ops);
      }
    };
    struct Entry {
      Key K;
      uint8_t Len = 0;
      char Bytes[15];
    };

    // Return the slot for I, or null if I cannot be cached. A slot with Len 0
    // or a different key is a miss.
    Entry *slot(const MCInst &I, const MCSubtargetInfo &STI, Key &K) {
      if (I.getNumOperands() > std::size(K.Ops))
        return nullptr;
      K.STI = &STI;
      K.Opcode = I.getOpcode();
      K.Flags = I.getFlags();
      K.NumOps = I.getNumOperands();
      K.ImmMask = 0;
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
    const OnePassCodeGen *Pass = nullptr;
    // Kind id of !heapallocsite, which only CodeView emission consumes.
    unsigned HeapAllocSiteKind = 0;
    // Whether every attribute of a list, or every return and parameter
    // attribute, is handled. Lists are uniqued, so the verdict is cached by
    // pointer.
    DenseMap<const void *, bool> AttrVerdicts, ParamVerdicts;
    bool attrsHandled(AttributeList AL) {
      return handled(AttrVerdicts, AL, /*ParamsOnly=*/false);
    }
    bool paramAttrsHandled(AttributeList AL) {
      return handled(ParamVerdicts, AL, /*ParamsOnly=*/true);
    }

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

    void reset(const OnePassCodeGen &P, const Module &M);

  private:
    LLVM_ABI bool handled(DenseMap<const void *, bool> &Cache, AttributeList AL,
                          bool ParamsOnly);
  };

  virtual ~OnePassCompiler();

  // Compile the function. On failure FailReason says why.
  bool run();

  SmallVector<char, 0> &Code;
  SmallVector<MCFixup, 0> &Fixups;
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

protected:
  OnePassCompiler(const Function &F, const MCSubtargetInfo &STI, AsmPrinter &AP,
                  const MCCodeEmitter &CE, ModuleState &G,
                  ArrayRef<uint8_t> GPOrder, ArrayRef<uint8_t> FPOrder,
                  uint8_t FirstFPReg);

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
  void emit(const MCInst &I);
  // As emit, for a branch whose displacement is patched in the buffer: the
  // encoder's fixup is dropped.
  void emitBranch(const MCInst &I);

  // Types and values.
  bool classify(Type *T, ValTy &VT);
  ValTy typeOf(const Value *V) {
    ValTy VT;
    if (!classify(V->getType(), VT)) {
      fail("unsupported type");
      VT = {8, false, false};
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
  bool isFPReg(uint8_t R) const { return R >= FirstFPReg; }
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
  uint8_t allocGP() { return allocFrom(GPAllocOrder); }
  uint8_t allocFP() { return allocFrom(FPAllocOrder); }
  // Take R for the current instruction, spilling a value that lives there.
  void claim(uint8_t R);

  MCSymbol *sym(const GlobalValue &GV, bool PreferLocal);
  MCSymbol *libSym(const char *Name);
  // Materialize a constant into R, or into a fresh register if R is NoReg.
  uint8_t constant(const Constant *C, uint8_t R);

  // Get V into a register owned by the current instruction.
  uint8_t use(const Value *V);
  // Get V into R, which the caller has claimed.
  void useInto(const Value *V, uint8_t R);
  // Like claim + useInto, but keeps V in place if it already lives in R.
  void useIn(const Value *V, uint8_t R);
  static bool isTemp(const Value &V, const BasicBlock *BB);
  // Record the result of I, held in R by the current instruction.
  void def(const Instruction &I, uint8_t R);
  // Free registers of single-use operands of an instruction that emits
  // nothing.
  void dropOperands(const Instruction &I);
  // Zero-extend to 32 bits unless the last use() already produced an extended
  // value.
  void zext32Use(uint8_t R, ValTy VT) {
    if (!ZExtKnown)
      zext32(R, VT);
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
  int32_t aggSlotFor(const Value *V) {
    ValInfo &VI = info(V);
    if (!VI.Slot)
      VI.Slot = allocFrame(16, 8);
    return VI.Slot;
  }

  // Record the call instruction emitted since Start.
  void recordCall(size_t Start, bool MayThrow);
  unsigned typeIdFor(const GlobalValue *GV);
  // Move the PHI inputs of the edge Pred -> Succ into the PHI slots.
  void edge(const BasicBlock *Pred, const BasicBlock *Succ);
  bool checkCall(const CallBase &CB);

  // Final code offset of each block, by block number.
  SmallVector<uint32_t, 0> &BlockOff;
  // Set the landing pad offset of each call site, and if NewOff is given, map
  // every recorded code offset through it after bytes were removed.
  template <typename FnT> void remapOffsets(FnT NewOff, bool Changed) {
    for (size_t Idx = 0; Idx != EH.CallSites.size(); ++Idx) {
      auto &CS = EH.CallSites[Idx];
      if (CallSiteLPad[Idx])
        CS.LandingPad = BlockOff[CallSiteLPad[Idx]->getNumber()];
      uint32_t End = NewOff(CS.Start + CS.Length);
      CS.Start = NewOff(CS.Start);
      CS.Length = End - CS.Start;
      if (CS.LandingPad)
        CS.LandingPad = NewOff(CS.LandingPad);
    }
    if (!Changed)
      return;
    for (auto &Row : Debug.Rows)
      Row.Offset = NewOff(Row.Offset);
    for (MCFixup &Fixup : Fixups)
      Fixup.setOffset(NewOff(Fixup.getOffset()));
    for (auto &Entry : CFI)
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

  // Target hooks.

  // Allocate Size bytes in the frame and return their frame offset.
  virtual int32_t allocFrame(uint64_t Size, uint64_t Alignment) = 0;
  // Slots hold the whole general-purpose register so reloads of any width
  // work.
  virtual void storeSlot(int32_t Slot, ValTy VT, uint8_t R) = 0;
  virtual void loadSlot(uint8_t R, ValTy VT, int32_t Slot) = 0;
  // Store an integer constant to a slot; return false if there is no
  // instruction for it.
  virtual bool storeImmToSlot(int32_t Slot, int64_t Imm) { return false; }
  virtual void regMove(uint8_t Dst, uint8_t Src) = 0;
  virtual void movImm(uint8_t R, int64_t V, unsigned Size) = 0;
  virtual void fpConstant(uint8_t R, uint64_t Bits, ValTy VT) = 0;
  virtual void globalAddr(const GlobalValue *GV, int64_t Off, uint8_t R) = 0;
  virtual void zext32(uint8_t R, ValTy VT) = 0;
  // Get a value whose kind is not Normal into R, or into a register of the
  // hook's choice if R is NoReg, and return the register.
  virtual uint8_t useDeferred(const Value *V, ValInfo &VI, ValTy VT,
                              uint8_t R) = 0;
  // The register that returns element Idx of its class of a two-scalar
  // struct.
  virtual uint8_t aggRetReg(bool FP, unsigned Idx) = 0;
  // The registers that hold the exception pointer and selector at a landing
  // pad, and the first integer argument register.
  virtual uint8_t exceptionPointerReg() = 0;
  virtual uint8_t exceptionSelectorReg() = 0;
  virtual uint8_t firstArgReg() = 0;

  // Emit the prologue and assign the arguments.
  virtual bool prologue() = 0;
  // Patch the frame size and resolve branches once all blocks are compiled.
  virtual bool finish() = 0;

  // Branches. Condition codes are target-defined.
  // Jump to BB unless it is the next block; Force emits the jump regardless.
  virtual void jmpTo(const BasicBlock *BB, bool Force = false) = 0;
  virtual void jccTo(unsigned CC, const BasicBlock *BB) = 0;
  // Emit a conditional jump to a local label bound later with bindFwd.
  virtual size_t jccFwd(unsigned CC) = 0;
  virtual void bindFwd(size_t Idx) = 0;
  virtual unsigned invertCC(unsigned CC) = 0;
  // Test the i1 value C and return the condition code for true.
  virtual unsigned emitCondition(const Value *C) = 0;
  // Compare R with a switch case value and return the condition code for
  // equal.
  virtual unsigned emitCaseCompare(uint8_t R, bool Is64, int64_t V) = 0;
  virtual void emitTrap(bool Debug) = 0;
  // Called where the unwinder enters a landing pad.
  virtual void emitLandingPadEntry() {}

  // Emit a direct call to a function or a library symbol.
  virtual void emitCallTo(const Function *Direct, const char *LibName,
                          bool MayThrow) = 0;
  // Emit a call with its arguments and record the result of I.
  virtual void emitCall(const Instruction &I, const Value *Callee,
                        const char *LibName, ArrayRef<const Value *> Args,
                        const CallBase *Attrs, bool IsVarArg,
                        bool MayThrow) = 0;

  virtual void compileLoad(const LoadInst &I) = 0;
  virtual void compileStore(const StoreInst &I) = 0;
  virtual void compileGEP(const GetElementPtrInst &I) = 0;
  virtual void compileBinary(const BinaryOperator &I) = 0;
  virtual void compileICmp(const ICmpInst &I) = 0;
  virtual void compileFCmp(const FCmpInst &I) = 0;
  virtual void compileSelect(const SelectInst &I) = 0;
  virtual void compileCast(const CastInst &I) = 0;
  virtual void compileFNeg(const UnaryOperator &I) = 0;
  virtual void compileRet(const ReturnInst &I) = 0;
  // Intrinsics the target-independent code does not handle.
  virtual void compileTargetIntrinsic(const IntrinsicInst &I) {
    fail("intrinsic");
  }

private:
  ArrayRef<uint8_t> GPAllocOrder, FPAllocOrder;
  uint8_t FirstFPReg;
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

  void appendSLEB(int64_t V);
  uint32_t actionFor(const LandingPadInst &LPI);
  void recordDebugRecords(const Instruction &I, bool &BlockStart);
  void compileInst(const Instruction &I);
  void compileAlloca(const AllocaInst &I);
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

// The pass that runs a target's OnePassCompiler on each function, emits the
// result through the AsmPrinter of its function pass manager and skips the
// remaining passes for that function.
class LLVM_ABI OnePassCodeGen : public FunctionPass {
public:
  void getAnalysisUsage(AnalysisUsage &AU) const override;
  bool doInitialization(Module &M) override;
  bool runOnFunction(Function &F) override;

  // Attributes that either do not affect code generation or are handled.
  virtual bool isHandledAttr(Attribute A) const;

protected:
  // \p AsmPrinterID is the pass ID of the target's AsmPrinter.
  OnePassCodeGen(char &ID, TargetMachine &TM, const OnePassCodeGenOptions &Opts,
                 const void *AsmPrinterID);

  // Module flags the target handles beyond the target-independent ones.
  virtual bool isHandledModuleFlag(const Module::ModuleFlagEntry &Flag) const {
    return false;
  }
  // Return null if the module can be compiled, or the reason it cannot,
  // for options and features that passes of the regular pipeline implement.
  virtual const char *unsupportedModuleReason(const Module &M,
                                              MCObjectStreamer &OS) {
    return nullptr;
  }
  virtual const char *unsupportedSubtargetReason(const Function &F) = 0;
  // Construct the target's compiler for \p F and pass it to finish().
  virtual bool compile(const Function &F, AsmPrinter &AP,
                       const MCCodeEmitter &CE,
                       OnePassCompiler::ModuleState &G) = 0;
  // Run \p C and emit its function. Return false if it failed.
  bool finish(const Function &F, OnePassCompiler &C);

  TargetMachine &TM;

private:
  OnePassCodeGenOptions Opts;
  const void *AsmPrinterID;
  unsigned NumDone = 0;
  OnePassCompiler::ModuleState G;
  enum { Unknown, Supported, Unsupported } ModuleState = Unknown;
  AsmPrinter *AP = nullptr;
  const char *FailReason = nullptr;
  std::string FailDetail;
};

} // namespace llvm

#endif
