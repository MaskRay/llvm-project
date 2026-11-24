//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The X86 part of the one-pass -O0 code generator; see
// llvm/CodeGen/OnePassCodeGen.h. x86-64 ELF with a fixed rbp frame.
// Constant-offset addresses and loads used by the next instruction are folded
// into memory operands.
//
//===----------------------------------------------------------------------===//

#include "MCTargetDesc/X86BaseInfo.h"
#include "MCTargetDesc/X86EncodingOptimization.h"
#include "MCTargetDesc/X86MCAsmInfo.h"
#include "MCTargetDesc/X86MCTargetDesc.h"
#include "X86.h"
#include "X86AsmPrinter.h"
#include "X86InstrInfo.h"
#include "X86Subtarget.h"
#include "X86TargetMachine.h"
#include "llvm/CodeGen/OnePassCodeGen.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GetElementPtrTypeIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCDwarf.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/MC/MCInst.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/MathExtras.h"

using namespace llvm;

namespace {
// Register ids: 0-15 are the general-purpose registers in hardware encoding
// order, 16-31 are XMM0-XMM15.
enum : uint8_t {
  RAX = 0,
  RCX = 1,
  RDX = 2,
  RSI = 6,
  RDI = 7,
  R8 = 8,
  R9 = 9,
  R10 = 10,
  R11 = 11,
  XMM0 = 16,
  NumRegs = 32,
};
} // namespace

const MCPhysReg GR64[16] = {X86::RAX, X86::RCX, X86::RDX, X86::RBX,
                            X86::RSP, X86::RBP, X86::RSI, X86::RDI,
                            X86::R8,  X86::R9,  X86::R10, X86::R11,
                            X86::R12, X86::R13, X86::R14, X86::R15};
const MCPhysReg GR32[16] = {X86::EAX,  X86::ECX,  X86::EDX,  X86::EBX,
                            X86::ESP,  X86::EBP,  X86::ESI,  X86::EDI,
                            X86::R8D,  X86::R9D,  X86::R10D, X86::R11D,
                            X86::R12D, X86::R13D, X86::R14D, X86::R15D};
const MCPhysReg GR16[16] = {X86::AX,   X86::CX,   X86::DX,   X86::BX,
                            X86::SP,   X86::BP,   X86::SI,   X86::DI,
                            X86::R8W,  X86::R9W,  X86::R10W, X86::R11W,
                            X86::R12W, X86::R13W, X86::R14W, X86::R15W};
const MCPhysReg GR8[16] = {X86::AL,   X86::CL,   X86::DL,   X86::BL,
                           X86::SPL,  X86::BPL,  X86::SIL,  X86::DIL,
                           X86::R8B,  X86::R9B,  X86::R10B, X86::R11B,
                           X86::R12B, X86::R13B, X86::R14B, X86::R15B};
const MCPhysReg XMM[16] = {X86::XMM0,  X86::XMM1,  X86::XMM2,  X86::XMM3,
                           X86::XMM4,  X86::XMM5,  X86::XMM6,  X86::XMM7,
                           X86::XMM8,  X86::XMM9,  X86::XMM10, X86::XMM11,
                           X86::XMM12, X86::XMM13, X86::XMM14, X86::XMM15};

// Allocation order. Argument registers come last so that call operands rarely
// sit in them.
const uint8_t GPOrder[] = {RAX, R10, R11, R9, R8, RCX, RDX, RSI, RDI};
const uint8_t FPOrder[] = {24, 25, 26, 27, 28, 29, 30, 31,
                           23, 22, 21, 20, 19, 18, 17, 16};
const uint8_t ArgGP[] = {RDI, RSI, RDX, RCX, R8, R9};

static bool isArgReg(uint8_t R) {
  return R >= XMM0 ? R < XMM0 + 8 : is_contained(ArgGP, R);
}

namespace {
struct Mem {
  MCRegister Base;
  int64_t Disp = 0;
  MCRegister Index = {};
  unsigned Scale = 1;
  const MCExpr *Expr = nullptr;
  MCRegister Segment = {};
  Mem(MCRegister Base, int64_t Disp, MCRegister Index = {}, unsigned Scale = 1,
      const MCExpr *Expr = nullptr)
      : Base(Base), Disp(Disp), Index(Index), Scale(Scale), Expr(Expr) {}
};

class X86OnePassCompiler final : public OnePassCompiler {
public:
  X86OnePassCompiler(const Function &F, const X86Subtarget &ST, AsmPrinter &AP,
                     const MCCodeEmitter &CE, ModuleState &G, unsigned DwarfRSP,
                     unsigned DwarfRBP, bool IBT)
      : OnePassCompiler(F, ST, AP, CE, G, GPOrder, FPOrder, XMM0), ST(ST),
        DwarfRSP(DwarfRSP), DwarfRBP(DwarfRBP), IBT(IBT) {
    // A fixed frame object at offset X is at rbp + X + 16.
    FixedObjectBias = -16;
  }

private:
  const X86Subtarget &ST;
  unsigned DwarfRSP, DwarfRBP;
  // Whether indirect branch targets get an ENDBR64.
  bool IBT;
  // Position of the frame size immediate in the prologue.
  size_t FramePatch = 0;
  // Trailing bytes of the frame adjustment instruction that are not needed.
  unsigned FrameBytesToDrop = 0;
  // Variadic function state: frame offset of the register save area, the
  // initial va_list offsets, and the frame offset of the first stack argument.
  int32_t RegSaveArea = 0;
  unsigned VAGPOffset = 0, VAFPOffset = 0;
  int32_t VAStackOff = 0;
  const Argument *SRetArg = nullptr;
  // Branches in emission order. Pos is the offset of the rel32 field; Target
  // is a code offset, taken from BB once all blocks are placed.
  struct Branch {
    uint32_t Pos;
    uint32_t Target;
    const BasicBlock *BB;
  };
  SmallVector<Branch, 0> Branches;

  // Encoding.
  static void addMem(MCInst &I, const Mem &M, MCContext &Ctx) {
    I.addOperand(MCOperand::createReg(M.Base));
    I.addOperand(MCOperand::createImm(M.Scale));
    I.addOperand(MCOperand::createReg(M.Index));
    if (M.Expr)
      I.addOperand(MCOperand::createExpr(
          M.Disp ? MCBinaryExpr::createAdd(
                       M.Expr, MCConstantExpr::create(M.Disp, Ctx), Ctx)
                 : M.Expr));
    else
      I.addOperand(MCOperand::createImm(M.Disp));
    I.addOperand(MCOperand::createReg(M.Segment));
  }
  void op0(unsigned Opc) {
    MCInst I;
    I.setOpcode(Opc);
    emit(I);
  }
  void opR(unsigned Opc, MCRegister A) {
    MCInst I;
    I.setOpcode(Opc);
    I.addOperand(MCOperand::createReg(A));
    emit(I);
  }
  void opRR(unsigned Opc, MCRegister A, MCRegister B) {
    MCInst I;
    I.setOpcode(Opc);
    I.addOperand(MCOperand::createReg(A));
    I.addOperand(MCOperand::createReg(B));
    emit(I);
  }
  void opRRR(unsigned Opc, MCRegister A, MCRegister B, MCRegister C) {
    MCInst I;
    I.setOpcode(Opc);
    I.addOperand(MCOperand::createReg(A));
    I.addOperand(MCOperand::createReg(B));
    I.addOperand(MCOperand::createReg(C));
    emit(I);
  }
  void opRI(unsigned Opc, MCRegister A, int64_t Imm) {
    MCInst I;
    I.setOpcode(isInt<8>(Imm) ? X86::getOpcodeForShortImmediateForm(Opc) : Opc);
    I.addOperand(MCOperand::createReg(A));
    I.addOperand(MCOperand::createImm(Imm));
    emit(I);
  }
  void opRRI(unsigned Opc, MCRegister A, MCRegister B, int64_t Imm) {
    MCInst I;
    I.setOpcode(isInt<8>(Imm) ? X86::getOpcodeForShortImmediateForm(Opc) : Opc);
    I.addOperand(MCOperand::createReg(A));
    I.addOperand(MCOperand::createReg(B));
    I.addOperand(MCOperand::createImm(Imm));
    emit(I);
  }
  void opRM(unsigned Opc, MCRegister A, const Mem &M) {
    MCInst I;
    I.setOpcode(Opc);
    I.addOperand(MCOperand::createReg(A));
    addMem(I, M, Ctx);
    emit(I);
  }
  void opMR(unsigned Opc, const Mem &M, MCRegister A) {
    MCInst I;
    I.setOpcode(Opc);
    addMem(I, M, Ctx);
    I.addOperand(MCOperand::createReg(A));
    emit(I);
  }
  void opMI(unsigned Opc, const Mem &M, int64_t Imm) {
    MCInst I;
    I.setOpcode(isInt<8>(Imm) ? X86::getOpcodeForShortImmediateForm(Opc) : Opc);
    addMem(I, M, Ctx);
    I.addOperand(MCOperand::createImm(Imm));
    emit(I);
  }
  // XOR32rr-style two-address instruction on one register.
  void zeroGP(uint8_t R) { opRRR(X86::XOR32rr, GR32[R], GR32[R], GR32[R]); }
  void movImm(uint8_t R, int64_t V, unsigned Size) override {
    if (Size <= 4)
      V = uint32_t(V);
    if (V == 0)
      zeroGP(R);
    else if (isUInt<32>(V))
      opRI(X86::MOV32ri, GR32[R], V);
    else if (isInt<32>(V))
      opRI(X86::MOV64ri32, GR64[R], V);
    else
      opRI(X86::MOV64ri, GR64[R], V);
  }
  Mem frame(int32_t Off) const { return Mem{X86::RBP, Off}; }

  // Local forward jumps and jumps to blocks. The rel32 is always the last
  // four bytes of the instruction.
  void patch32(size_t Pos, int32_t V) {
    support::endian::write32le(&Code[Pos], V);
  }
  // Emit a conditional jump to a local label and return its branch index.
  size_t jccFwd(unsigned CC) override {
    MCInst I;
    I.setOpcode(X86::JCC_4);
    I.addOperand(MCOperand::createImm(0));
    I.addOperand(MCOperand::createImm(CC));
    emitBranch(I);
    Branches.push_back({uint32_t(Code.size() - 4), 0, nullptr});
    return Branches.size() - 1;
  }
  void bindFwd(size_t Idx) override { Branches[Idx].Target = Code.size(); }
  void jccTo(unsigned CC, const BasicBlock *BB) override {
    Branches[jccFwd(CC)].BB = BB;
  }
  unsigned invertCC(unsigned CC) override {
    return X86::GetOppositeBranchCondition(X86::CondCode(CC));
  }
  void relaxBranches();
  void jmpTo(const BasicBlock *BB, bool Force = false) override {
    if (BB == NextBB && !Force)
      return;
    MCInst I;
    I.setOpcode(X86::JMP_4);
    I.addOperand(MCOperand::createImm(0));
    emitBranch(I);
    Branches.push_back({uint32_t(Code.size() - 4), 0, BB});
  }

  int32_t allocFrame(uint64_t Size, uint64_t Alignment) override {
    FrameSize = alignTo(FrameSize + Size, Alignment);
    return -int32_t(FrameSize);
  }
  void loadMem(uint8_t R, ValTy VT, const Mem &M) {
    if (VT.FP)
      opRM(VT.Size == 4 ? X86::MOVSSrm : X86::MOVSDrm, XMM[R - XMM0], M);
    else if (VT.Size == 1)
      opRM(X86::MOVZX32rm8, GR32[R], M);
    else if (VT.Size == 2)
      opRM(X86::MOVZX32rm16, GR32[R], M);
    else if (VT.Size == 4)
      opRM(X86::MOV32rm, GR32[R], M);
    else
      opRM(X86::MOV64rm, GR64[R], M);
  }
  void storeMem(const Mem &M, ValTy VT, uint8_t R) {
    if (VT.FP)
      opMR(VT.Size == 4 ? X86::MOVSSmr : X86::MOVSDmr, M, XMM[R - XMM0]);
    else if (VT.Size == 1)
      opMR(X86::MOV8mr, M, GR8[R]);
    else if (VT.Size == 2)
      opMR(X86::MOV16mr, M, GR16[R]);
    else if (VT.Size == 4)
      opMR(X86::MOV32mr, M, GR32[R]);
    else
      opMR(X86::MOV64mr, M, GR64[R]);
  }
  void storeSlot(int32_t Slot, ValTy VT, uint8_t R) override {
    if (!VT.FP)
      VT.Size = 8;
    storeMem(frame(Slot), VT, R);
  }
  void loadSlot(uint8_t R, ValTy VT, int32_t Slot) override {
    loadMem(R, VT, frame(Slot));
  }
  bool storeImmToSlot(int32_t Slot, int64_t Imm) override {
    if (!isInt<32>(Imm))
      return false;
    opMI(X86::MOV64mi32, frame(Slot), Imm);
    return true;
  }
  void regMove(uint8_t Dst, uint8_t Src) override {
    if (Dst == Src)
      return;
    if (Dst >= XMM0)
      opRR(X86::MOVAPSrr, XMM[Dst - XMM0], XMM[Src - XMM0]);
    else
      opRR(X86::MOV64rr, GR64[Dst], GR64[Src]);
  }

  // Constants and globals.
  const MCExpr *symExpr(const MCSymbol *Sym, uint16_t Spec = 0) {
    return MCSymbolRefExpr::create(Sym, Spec, Ctx);
  }
  void globalAddr(const GlobalValue *GV, int64_t Off, uint8_t R) override {
    if (GV->isThreadLocal() || GV->isAbsoluteSymbolRef() ||
        AP.TM.isLargeGlobalValue(GV) || !isInt<32>(Off))
      return fail("global reference");
    unsigned char Flag = ST.classifyGlobalReference(GV);
    if (Flag == X86II::MO_NO_FLAG) {
      opRM(X86::LEA64r, GR64[R],
           {X86::RIP, Off, MCRegister(), 1, symExpr(sym(*GV, true))});
    } else if (Flag == X86II::MO_GOTPCREL ||
               Flag == X86II::MO_GOTPCREL_NORELAX) {
      opRM(X86::MOV64rm, GR64[R],
           {X86::RIP, 0, MCRegister(), 1,
            symExpr(sym(*GV, false), Flag == X86II::MO_GOTPCREL
                                         ? X86::S_GOTPCREL
                                         : X86::S_GOTPCREL_NORELAX)});
      if (Off)
        opRRI(X86::ADD64ri32, GR64[R], GR64[R], Off);
    } else {
      fail("global reference kind");
    }
  }
  void fpConstant(uint8_t R, uint64_t Bits, ValTy VT) override {
    MCRegister X = XMM[R - XMM0];
    if (!Bits)
      return opRRR(X86::XORPSrr, X, X, X);
    uint8_t G = allocGP();
    movImm(G, Bits, VT.Size);
    if (VT.Size == 8)
      opRR(X86::MOV64toPQIrr, X, GR64[G]);
    else
      opRR(X86::MOVDI2PDIrr, X, GR32[G]);
    release(G);
  }

  uint8_t useDeferred(const Value *V, ValInfo &VI, ValTy VT,
                      uint8_t R) override {
    uint8_t Tmp = NoReg;
    switch (VI.Kind) {
    case ValInfo::Alloca:
      if (R == NoReg)
        R = allocGP();
      opRM(X86::LEA64r, GR64[R], frame(VI.Slot));
      return R;
    case ValInfo::DeferredGEP: {
      Mem M = addr(V, Tmp);
      if (R == NoReg)
        R = Tmp != NoReg ? Tmp : allocGP();
      else
        release(Tmp);
      opRM(X86::LEA64r, GR64[R], M);
      return R;
    }
    case ValInfo::DeferredLoad: {
      Mem M = addr(cast<LoadInst>(V)->getPointerOperand(), Tmp);
      if (R == NoReg)
        R = Tmp != NoReg ? Tmp : allocGP();
      else
        release(Tmp);
      loadMem(R, VT, M);
      ZExtKnown = true;
      return R;
    }
    default:
      fail("deferred compare used as a value");
      return R == NoReg ? uint8_t(RAX) : R;
    }
  }
  // If the value of I reaches a call in this block as a register argument,
  // possibly through operations that compute in place, return that register.
  static uint8_t argRegHint(const Instruction &I) {
    const Value *V = &I;
    for (unsigned Depth = 0; Depth != 4; ++Depth) {
      if (!V->hasOneUse() || !V->getType()->isIntOrPtrTy())
        return NoReg;
      auto *U = dyn_cast<Instruction>(V->user_back());
      if (!U || U->getParent() != I.getParent())
        return NoReg;
      if (auto *CI = dyn_cast<CallBase>(U)) {
        if (isa<IntrinsicInst>(CI) || CI->isInlineAsm())
          return NoReg;
        unsigned NumGP = 0;
        for (const Use &A : CI->args()) {
          if (A.get() == V)
            return NumGP < 6 ? ArgGP[NumGP] : uint8_t(NoReg);
          if (A->getType()->isIntOrPtrTy())
            ++NumGP;
          else if (!A->getType()->isFloatingPointTy())
            return NoReg;
        }
        return NoReg;
      }
      bool InPlace = false;
      if (isa<CastInst>(U))
        InPlace = U->getType()->isIntOrPtrTy();
      else if (isa<GetElementPtrInst>(U))
        InPlace = U->getOperand(0) == V;
      else if (auto *BO = dyn_cast<BinaryOperator>(U))
        InPlace =
            BO->getOperand(0) == V &&
            (BO->getOpcode() == Instruction::Add ||
             BO->getOpcode() == Instruction::Sub ||
             BO->getOpcode() == Instruction::Mul || BO->isBitwiseLogicOp());
      if (!InPlace)
        return NoReg;
      V = U;
    }
    return NoReg;
  }

  // Extensions of a narrow value held in a general-purpose register.
  void zext32(uint8_t R, ValTy VT) override {
    if (VT.I1)
      opRRI(X86::AND32ri, GR32[R], GR32[R], 1);
    else if (VT.Size == 1)
      opRR(X86::MOVZX32rr8, GR32[R], GR8[R]);
    else if (VT.Size == 2)
      opRR(X86::MOVZX32rr16, GR32[R], GR16[R]);
  }
  void sext32(uint8_t R, ValTy VT) {
    if (VT.I1) {
      opRRI(X86::AND32ri, GR32[R], GR32[R], 1);
      opRR(X86::NEG32r, GR32[R], GR32[R]);
    } else if (VT.Size == 1) {
      opRR(X86::MOVSX32rr8, GR32[R], GR8[R]);
    } else if (VT.Size == 2) {
      opRR(X86::MOVSX32rr16, GR32[R], GR16[R]);
    }
  }
  void sext64(uint8_t R, ValTy VT) {
    if (VT.I1) {
      opRRI(X86::AND32ri, GR32[R], GR32[R], 1);
      opRR(X86::NEG64r, GR64[R], GR64[R]);
    } else if (VT.Size == 1) {
      opRR(X86::MOVSX64rr8, GR64[R], GR8[R]);
    } else if (VT.Size == 2) {
      opRR(X86::MOVSX64rr16, GR64[R], GR16[R]);
    } else if (VT.Size == 4) {
      opRR(X86::MOVSX64rr32, GR64[R], GR32[R]);
    }
  }
  void zext64Use(uint8_t R, ValTy VT) {
    if (!ZExtKnown)
      zext64(R, VT);
  }
  void zext64(uint8_t R, ValTy VT) {
    if (VT.Size == 4)
      opRR(X86::MOV32rr, GR32[R], GR32[R]);
    else
      zext32(R, VT);
  }

  // A global addressed RIP-relative without the GOT.
  bool isDirectGlobal(const Value *P) {
    auto *GV = dyn_cast<GlobalVariable>(P);
    return GV && !GV->isThreadLocal() && GV->getAddressSpace() == 0 &&
           !GV->isAbsoluteSymbolRef() && !AP.TM.isLargeGlobalValue(GV) &&
           ST.classifyGlobalReference(GV) == X86II::MO_NO_FLAG;
  }
  // Whether addr(P) needs no register: a frame object, a direct global, or a
  // constant offset from one.
  bool isFixedAddress(const Value *P) {
    if (isDirectGlobal(P))
      return true;
    auto It = lookup(P);
    if (!It)
      return false;
    if (It->Kind == ValInfo::Alloca)
      return true;
    return It->Kind == ValInfo::DeferredGEP &&
           isFixedAddress(cast<GetElementPtrInst>(P)->getPointerOperand());
  }

  // Memory operand for a pointer. Tmp receives a register to release, if any.
  Mem addr(const Value *P, uint8_t &Tmp) {
    Tmp = NoReg;
    // Address spaces 256-258 are segment-relative.
    if (P->getType()->getPointerAddressSpace() != 0) {
      fail("address space");
      return frame(0);
    }
    if (auto It = lookup(P); It) {
      if (It->Kind == ValInfo::Alloca)
        return frame(It->Slot);
      if (It->Kind == ValInfo::DeferredGEP) {
        auto *GEP = cast<GetElementPtrInst>(P);
        APInt Off(64, 0);
        if (!GEP->accumulateConstantOffset(DL, Off))
          fail("gep offset");
        Mem M = addr(GEP->getPointerOperand(), Tmp);
        M.Disp += Off.getSExtValue();
        if (!isInt<32>(M.Disp))
          fail("gep offset");
        return M;
      }
    }
    if (isDirectGlobal(P))
      return {X86::RIP, 0, MCRegister(), 1,
              symExpr(sym(*cast<GlobalVariable>(P), true))};
    Tmp = use(P);
    return Mem{GR64[Tmp], 0};
  }

  uint8_t aggRetReg(bool FP, unsigned Idx) override {
    return FP ? XMM0 + Idx : (Idx ? RDX : RAX);
  }
  uint8_t exceptionPointerReg() override { return RAX; }
  uint8_t exceptionSelectorReg() override { return RDX; }
  uint8_t firstArgReg() override { return RDI; }

  bool prologue() override;
  bool finish() override;
  unsigned emitCondition(const Value *C) override {
    if (auto It = lookup(C); It && It->Kind == ValInfo::DeferredCmp)
      return emitICmp(*cast<ICmpInst>(C));
    uint8_t R = use(C);
    opRI(X86::TEST8ri, GR8[R], 1);
    release(R);
    return X86::COND_NE;
  }
  unsigned emitCaseCompare(uint8_t R, bool Is64, int64_t V) override {
    if (!Is64) {
      opRI(X86::CMP32ri, GR32[R], V);
    } else if (isInt<32>(V)) {
      opRI(X86::CMP64ri32, GR64[R], V);
    } else {
      uint8_t T = allocGP();
      opRI(X86::MOV64ri, GR64[T], V);
      opRR(X86::CMP64rr, GR64[R], GR64[T]);
      release(T);
    }
    return X86::COND_E;
  }
  void emitTrap(bool Debug) override { op0(Debug ? X86::INT3 : X86::TRAP); }
  void emitLandingPadEntry() override {
    if (IBT)
      op0(X86::ENDBR64);
  }
  // The guard value is at %fs:0x28. R11 is free in the prologue and at a
  // return.
  Mem stackGuard() {
    Mem M{MCRegister(), 0x28};
    M.Segment = X86::FS;
    return M;
  }
  X86::CondCode emitICmp(const ICmpInst &I);
  void emitCallTo(const Function *Direct, const char *LibName,
                  bool MayThrow) override;
  void emitCall(const Instruction &I, const Value *Callee, const char *LibName,
                ArrayRef<const Value *> Args, const CallBase *Attrs,
                bool IsVarArg, bool MayThrow) override;
  bool canFoldLoad(const LoadInst &I, ValTy VT);
  void compileLoad(const LoadInst &I) override;
  void compileStore(const StoreInst &I) override;
  void compileGEP(const GetElementPtrInst &I) override;
  void compileBinary(const BinaryOperator &I) override;
  void compileICmp(const ICmpInst &I) override;
  void compileFCmp(const FCmpInst &I) override;
  void compileSelect(const SelectInst &I) override;
  void compileCast(const CastInst &I) override;
  void compileFNeg(const UnaryOperator &I) override;
  void compileTargetIntrinsic(const IntrinsicInst &I) override;
  void compileRet(const ReturnInst &I) override;
};
} // namespace

bool X86OnePassCompiler::prologue() {
  const Triple &TT = ST.getTargetTriple();
  if (StackGuardSlot && !TT.isOSGlibc() && !TT.isMusl()) {
    fail("stack protector guard");
    return false;
  }
  // As X86IndirectBranchTracking: the function may be called indirectly.
  if (IBT && (F.hasAddressTaken() || !F.hasLocalLinkage()))
    op0(X86::ENDBR64);
  opR(X86::PUSH64r, X86::RBP);
  CFI.emplace_back(Code.size(), MCCFIInstruction::cfiDefCfaOffset(nullptr, 16));
  CFI.emplace_back(Code.size(),
                   MCCFIInstruction::createOffset(nullptr, DwarfRBP, -16));
  opRR(X86::MOV64rr, X86::RBP, X86::RSP);
  CFI.emplace_back(Code.size(),
                   MCCFIInstruction::createDefCfaRegister(nullptr, DwarfRBP));
  // The frame size is patched in later, so keep the 32-bit immediate form.
  MCInst Sub;
  Sub.setOpcode(X86::SUB64ri32);
  Sub.addOperand(MCOperand::createReg(X86::RSP));
  Sub.addOperand(MCOperand::createReg(X86::RSP));
  Sub.addOperand(MCOperand::createImm(0));
  emit(Sub);
  FramePatch = Code.size() - 4;
  if (StackGuardSlot) {
    opRM(X86::MOV64rm, X86::R11, stackGuard());
    opMR(X86::MOV64mr, frame(StackGuardSlot), X86::R11);
  }

  if (F.isVarArg()) {
    FrameSize = alignTo(FrameSize + 176, 16);
    RegSaveArea = -int32_t(FrameSize);
    for (unsigned I = 0; I != 6; ++I)
      opMR(X86::MOV64mr, frame(RegSaveArea + 8 * I), GR64[ArgGP[I]]);
    opRR(X86::TEST8rr, X86::AL, X86::AL);
    size_t Skip = jccFwd(X86::COND_E);
    for (unsigned I = 0; I != 8; ++I)
      opMR(X86::MOVAPSmr, frame(RegSaveArea + 48 + 16 * I), XMM[I]);
    bindFwd(Skip);
  }

  unsigned NumGP = 0, NumFP = 0;
  int32_t StackOff = 16;
  const BasicBlock *Entry = &F.getEntryBlock();
  for (const Argument &A : F.args()) {
    if (A.hasByValAttr()) {
      uint64_t Size = DL.getTypeAllocSize(A.getParamByValType());
      if (A.getParamAlign().valueOrOne() > Align(8) || Size > (1u << 20)) {
        fail("byval argument");
        return false;
      }
      ValInfo &VI = info(&A);
      VI.Kind = ValInfo::Alloca;
      VI.Slot = StackOff;
      StackOff += alignTo(Size, 8);
      continue;
    }
    if (A.hasInAllocaAttr() || A.hasPreallocatedAttr() || A.hasNestAttr() ||
        A.hasInRegAttr() || A.hasSwiftSelfAttr() || A.hasSwiftErrorAttr() ||
        A.hasAttribute(Attribute::SwiftAsync)) {
      fail("argument attribute");
      return false;
    }
    ValTy VT;
    if (!classify(A.getType(), VT)) {
      fail("argument type");
      return false;
    }
    if (A.hasStructRetAttr())
      SRetArg = &A;
    if (VT.FP ? NumFP == 8 : NumGP == 6) {
      info(&A).Slot = StackOff;
      StackOff += 8;
      continue;
    }
    uint8_t R = VT.FP ? XMM0 + NumFP++ : ArgGP[NumGP++];
    if (&A == SRetArg || !A.use_empty()) {
      if (&A != SRetArg && isTemp(A, Entry)) {
        Owner[R] = &A;
        info(&A).Reg = R;
      } else {
        storeAndCache(&A, VT, R);
      }
    }
  }
  VAGPOffset = NumGP * 8;
  VAFPOffset = 48 + NumFP * 16;
  VAStackOff = StackOff;
  return true;
}

X86::CondCode X86OnePassCompiler::emitICmp(const ICmpInst &I) {
  const Value *L = I.getOperand(0), *Rhs = I.getOperand(1);
  ValTy VT = typeOf(L);
  if (VT.FP) {
    fail("icmp type");
    return X86::COND_E;
  }
  bool Signed = I.isSigned(), W = VT.Size == 8;
  // A folded load compares in memory at its own width.
  if (auto It = lookup(L); It && It->Kind == ValInfo::DeferredLoad) {
    auto *CI = dyn_cast<ConstantInt>(Rhs);
    int64_t Imm = CI ? CI->getSExtValue() : 0;
    if (!W || isInt<32>(Imm)) {
      uint8_t Tmp;
      Mem M = addr(cast<LoadInst>(L)->getPointerOperand(), Tmp);
      static const unsigned Opc[] = {
          0, X86::CMP8mi, X86::CMP16mi,  0, X86::CMP32mi, 0,
          0, 0,           X86::CMP64mi32};
      opMI(Opc[VT.Size], M, SignExtend64(Imm, 8 * VT.Size));
      release(Tmp);
      return X86::getX86ConditionCode(I.getPredicate()).first;
    }
  }
  uint8_t A = use(L);
  if (VT.Size < 4)
    Signed ? sext32(A, VT) : zext32Use(A, VT);
  auto *CI = dyn_cast<ConstantInt>(Rhs);
  if (CI && (!W || isInt<32>(CI->getSExtValue()))) {
    int64_t Imm =
        Signed || VT.Size >= 4 ? CI->getSExtValue() : CI->getZExtValue();
    opRI(W ? X86::CMP64ri32 : X86::CMP32ri, W ? GR64[A] : GR32[A],
         W ? Imm : int32_t(Imm));
  } else if (isa<ConstantPointerNull>(Rhs)) {
    opRI(X86::CMP64ri32, GR64[A], 0);
  } else {
    uint8_t B = use(Rhs);
    if (VT.Size < 4)
      Signed ? sext32(B, VT) : zext32Use(B, VT);
    opRR(W ? X86::CMP64rr : X86::CMP32rr, W ? GR64[A] : GR32[A],
         W ? GR64[B] : GR32[B]);
    release(B);
  }
  release(A);
  return X86::getX86ConditionCode(I.getPredicate()).first;
}

void X86OnePassCompiler::compileICmp(const ICmpInst &I) {
  if (I.getType()->isVectorTy())
    return fail("vector compare");
  if (I.hasOneUse()) {
    auto *Br = dyn_cast<CondBrInst>(I.user_back());
    if (Br && Br->getParent() == I.getParent()) {
      info(&I).Kind = ValInfo::DeferredCmp;
      return;
    }
  }
  X86::CondCode CC = emitICmp(I);
  uint8_t R = allocGP();
  opRI(X86::SETCCr, GR8[R], CC);
  def(I, R);
}

void X86OnePassCompiler::compileFCmp(const FCmpInst &I) {
  ValTy VT = typeOf(I.getOperand(0));
  if (!VT.FP || I.getType()->isVectorTy())
    return fail("fcmp type");
  uint8_t A = use(I.getOperand(0)), B = use(I.getOperand(1));
  auto [CC, Swap] = X86::getX86ConditionCode(I.getPredicate());
  X86::CondCode CC2 = X86::COND_INVALID;
  if (I.getPredicate() == CmpInst::FCMP_OEQ) {
    CC = X86::COND_E;
    CC2 = X86::COND_NP;
  } else if (I.getPredicate() == CmpInst::FCMP_UNE) {
    CC = X86::COND_NE;
    CC2 = X86::COND_P;
  } else if (CC == X86::COND_INVALID) {
    return fail("fcmp predicate");
  }
  if (Swap)
    std::swap(A, B);
  opRR(VT.Size == 4 ? X86::UCOMISSrr : X86::UCOMISDrr, XMM[A - XMM0],
       XMM[B - XMM0]);
  release(A);
  release(B);
  uint8_t R = allocGP();
  opRI(X86::SETCCr, GR8[R], CC);
  if (CC2 != X86::COND_INVALID) {
    uint8_t T = allocGP();
    opRI(X86::SETCCr, GR8[T], CC2);
    opRRR(CC == X86::COND_E ? X86::AND32rr : X86::OR32rr, GR32[R], GR32[R],
          GR32[T]);
    release(T);
  }
  def(I, R);
}

// Whether the load can be folded into its user as a memory operand. The user
// must be the next instruction, so no store comes in between.
bool X86OnePassCompiler::canFoldLoad(const LoadInst &I, ValTy VT) {
  if (VT.FP || VT.I1 || I.isVolatile() || !I.hasOneUse())
    return false;
  auto *U = dyn_cast<Instruction>(I.user_back());
  if (U != I.getNextNode())
    return false;
  if (isa<SExtInst>(U))
    return true;
  auto *Cmp = dyn_cast<ICmpInst>(U);
  if (!Cmp || Cmp->getOperand(0) != &I ||
      !isa<ConstantInt, ConstantPointerNull>(Cmp->getOperand(1)))
    return false;
  // A compare deferred into its branch must still be emitted next.
  auto *Br =
      Cmp->hasOneUse() ? dyn_cast<CondBrInst>(Cmp->user_back()) : nullptr;
  return !Br || Br->getParent() != Cmp->getParent() || Br == Cmp->getNextNode();
}

void X86OnePassCompiler::compileLoad(const LoadInst &I) {
  ValTy VT;
  SmallVector<AggPart, 2> Parts;
  if (!I.isAtomic() && aggParts(I.getType(), Parts)) {
    uint8_t Tmp;
    Mem M = addr(I.getPointerOperand(), Tmp);
    int32_t Slot = aggSlotFor(&I);
    for (unsigned P = 0; P != Parts.size(); ++P) {
      uint8_t R = Parts[P].VT.FP ? allocFP() : allocGP();
      Mem Elt = M;
      Elt.Disp += Parts[P].MemOff;
      loadMem(R, Parts[P].VT, Elt);
      storeSlot(Slot + 8 * P, Parts[P].VT, R);
      release(R);
    }
    return release(Tmp);
  }
  if (I.isAtomic() || !classify(I.getType(), VT))
    return fail("load");
  if (canFoldLoad(I, VT)) {
    info(&I).Kind = ValInfo::DeferredLoad;
    return;
  }
  uint8_t Tmp;
  Mem M = addr(I.getPointerOperand(), Tmp);
  uint8_t R;
  uint8_t Hint = VT.FP ? uint8_t(NoReg) : argRegHint(I);
  if (VT.FP) {
    R = allocFP();
  } else if (Hint != NoReg && !Owner[Hint]) {
    R = Hint;
    Owner[R] = Busy;
  } else if (Tmp != NoReg) {
    R = Tmp;
    Tmp = NoReg;
  } else {
    R = allocGP();
  }
  loadMem(R, VT, M);
  release(Tmp);
  def(I, R);
}

void X86OnePassCompiler::compileStore(const StoreInst &I) {
  const Value *V = I.getValueOperand();
  ValTy VT;
  SmallVector<AggPart, 2> Parts;
  if (!I.isAtomic() && aggParts(V->getType(), Parts)) {
    auto It = lookup(V);
    if (!It || !It->Slot)
      return fail("aggregate store value");
    int32_t Slot = It->Slot;
    uint8_t Tmp;
    Mem M = addr(I.getPointerOperand(), Tmp);
    for (unsigned P = 0; P != Parts.size(); ++P) {
      uint8_t R = Parts[P].VT.FP ? allocFP() : allocGP();
      Mem Elt = M;
      Elt.Disp += Parts[P].MemOff;
      loadMem(R, Parts[P].VT, frame(Slot + 8 * P));
      if (Parts[P].VT.I1)
        opRRI(X86::AND32ri, GR32[R], GR32[R], 1);
      storeMem(Elt, Parts[P].VT, R);
      release(R);
    }
    return release(Tmp);
  }
  if (I.isAtomic() || !classify(V->getType(), VT))
    return fail("store");
  uint8_t Tmp;
  auto *CI = dyn_cast<ConstantInt>(V);
  if ((CI && (VT.Size < 8 || isInt<32>(CI->getSExtValue()))) ||
      isa<ConstantPointerNull>(V)) {
    int64_t Imm = !CI ? 0 : VT.I1 ? CI->getZExtValue() : CI->getSExtValue();
    Mem M = addr(I.getPointerOperand(), Tmp);
    static const unsigned Opc[] = {
        0, X86::MOV8mi, X86::MOV16mi, 0, X86::MOV32mi, 0, 0, 0, X86::MOV64mi32};
    opMI(Opc[VT.Size], M, SignExtend64(Imm, 8 * VT.Size));
    release(Tmp);
    return;
  }
  uint8_t R = use(V);
  if (VT.I1)
    opRRI(X86::AND32ri, GR32[R], GR32[R], 1);
  Mem M = addr(I.getPointerOperand(), Tmp);
  storeMem(M, VT, R);
  release(R);
  release(Tmp);
}

void X86OnePassCompiler::compileGEP(const GetElementPtrInst &I) {
  if (I.getType()->isVectorTy() || I.getSourceElementType()->isScalableTy())
    return fail("vector gep");
  if (I.hasAllConstantIndices()) {
    // An address relative to RBP or RIP can be rebuilt at each use, unless a
    // block laid out earlier already expects the value in its slot.
    auto It = lookup(&I);
    if (isFixedAddress(I.getPointerOperand()) && (!It || !It->Slot)) {
      info(&I).Kind = ValInfo::DeferredGEP;
      return;
    }
    // Otherwise the base register is consumed by the single use.
    if (I.hasOneUse()) {
      auto *U = cast<Instruction>(I.user_back());
      auto *SI = dyn_cast<StoreInst>(U);
      if (U->getParent() == I.getParent() &&
          (isa<LoadInst>(U) || (SI && SI->getValueOperand() != &I))) {
        info(&I).Kind = ValInfo::DeferredGEP;
        return;
      }
    }
  }
  const Value *Base = I.getPointerOperand();
  uint8_t R = NoReg;
  int64_t Disp = 0;
  if (auto It = lookup(Base); It && It->Kind == ValInfo::Alloca)
    Disp = It->Slot;
  else
    R = use(Base);
  // The base is RBP while R is NoReg.
  auto Flush = [&] {
    if (!isInt<32>(Disp))
      return fail("gep offset");
    if (R == NoReg) {
      R = allocGP();
      opRM(X86::LEA64r, GR64[R], frame(Disp));
    } else if (Disp) {
      opRM(X86::LEA64r, GR64[R], Mem{GR64[R], Disp});
    }
    Disp = 0;
  };
  for (gep_type_iterator GTI = gep_type_begin(I), E = gep_type_end(I); GTI != E;
       ++GTI) {
    const Value *Idx = GTI.getOperand();
    if (StructType *STy = GTI.getStructTypeOrNull()) {
      Disp += DL.getStructLayout(STy)->getElementOffset(
          cast<ConstantInt>(Idx)->getZExtValue());
      continue;
    }
    TypeSize TS = GTI.getSequentialElementStride(DL);
    if (TS.isScalable() || Idx->getType()->isVectorTy())
      return fail("gep index");
    int64_t Stride = TS.getFixedValue();
    if (auto *CI = dyn_cast<ConstantInt>(Idx)) {
      Disp += CI->getSExtValue() * Stride;
      continue;
    }
    ValTy XT = typeOf(Idx);
    uint8_t X = use(Idx);
    if (XT.Size < 8)
      sext64(X, XT);
    Flush();
    if (FailReason)
      return;
    if (Stride == 1 || Stride == 2 || Stride == 4 || Stride == 8) {
      opRM(X86::LEA64r, GR64[R], {GR64[R], 0, GR64[X], unsigned(Stride)});
    } else {
      if (!isInt<32>(Stride))
        return fail("gep stride");
      opRRI(X86::IMUL64rri32, GR64[X], GR64[X], Stride);
      opRRR(X86::ADD64rr, GR64[R], GR64[R], GR64[X]);
    }
    release(X);
  }
  Flush();
  if (!FailReason)
    def(I, R);
}

void X86OnePassCompiler::compileBinary(const BinaryOperator &I) {
  ValTy VT;
  if (!classify(I.getType(), VT))
    return fail("binary operator type");
  const Value *L = I.getOperand(0), *Rhs = I.getOperand(1);
  if (VT.FP) {
    bool S = VT.Size == 4;
    unsigned Opc;
    switch (I.getOpcode()) {
    case Instruction::FAdd:
      Opc = S ? X86::ADDSSrr : X86::ADDSDrr;
      break;
    case Instruction::FSub:
      Opc = S ? X86::SUBSSrr : X86::SUBSDrr;
      break;
    case Instruction::FMul:
      Opc = S ? X86::MULSSrr : X86::MULSDrr;
      break;
    case Instruction::FDiv:
      Opc = S ? X86::DIVSSrr : X86::DIVSDrr;
      break;
    default:
      return fail("fp operator");
    }
    uint8_t A = use(L), B = use(Rhs);
    opRRR(Opc, XMM[A - XMM0], XMM[A - XMM0], XMM[B - XMM0]);
    release(B);
    return def(I, A);
  }

  bool W = VT.Size == 8;
  auto Reg = [&](uint8_t R) { return MCRegister(W ? GR64[R] : GR32[R]); };
  auto *CI = dyn_cast<ConstantInt>(Rhs);
  switch (I.getOpcode()) {
  case Instruction::Add:
  case Instruction::Sub:
  case Instruction::And:
  case Instruction::Or:
  case Instruction::Xor:
  case Instruction::Mul: {
    unsigned RR, RI;
    switch (I.getOpcode()) {
    case Instruction::Add:
      RR = W ? X86::ADD64rr : X86::ADD32rr;
      RI = W ? X86::ADD64ri32 : X86::ADD32ri;
      break;
    case Instruction::Sub:
      RR = W ? X86::SUB64rr : X86::SUB32rr;
      RI = W ? X86::SUB64ri32 : X86::SUB32ri;
      break;
    case Instruction::And:
      RR = W ? X86::AND64rr : X86::AND32rr;
      RI = W ? X86::AND64ri32 : X86::AND32ri;
      break;
    case Instruction::Or:
      RR = W ? X86::OR64rr : X86::OR32rr;
      RI = W ? X86::OR64ri32 : X86::OR32ri;
      break;
    case Instruction::Xor:
      RR = W ? X86::XOR64rr : X86::XOR32rr;
      RI = W ? X86::XOR64ri32 : X86::XOR32ri;
      break;
    default:
      RR = W ? X86::IMUL64rr : X86::IMUL32rr;
      RI = W ? X86::IMUL64rri32 : X86::IMUL32rri;
      break;
    }
    uint8_t A = use(L);
    if (CI && (!W || isInt<32>(CI->getSExtValue()))) {
      opRRI(RI, Reg(A), Reg(A),
            W ? CI->getSExtValue() : int32_t(CI->getZExtValue()));
    } else {
      uint8_t B = use(Rhs);
      opRRR(RR, Reg(A), Reg(A), Reg(B));
      release(B);
    }
    return def(I, A);
  }
  case Instruction::Shl:
  case Instruction::LShr:
  case Instruction::AShr: {
    bool IsShl = I.getOpcode() == Instruction::Shl;
    bool IsAShr = I.getOpcode() == Instruction::AShr;
    unsigned CL, RI;
    if (IsShl) {
      CL = W ? X86::SHL64rCL : X86::SHL32rCL;
      RI = W ? X86::SHL64ri : X86::SHL32ri;
    } else if (IsAShr) {
      CL = W ? X86::SAR64rCL : X86::SAR32rCL;
      RI = W ? X86::SAR64ri : X86::SAR32ri;
    } else {
      CL = W ? X86::SHR64rCL : X86::SHR32rCL;
      RI = W ? X86::SHR64ri : X86::SHR32ri;
    }
    if (!CI)
      claim(RCX);
    uint8_t A = use(L);
    if (!IsShl && VT.Size < 4)
      IsAShr ? sext32(A, VT) : zext32(A, VT);
    if (CI) {
      opRRI(RI, Reg(A), Reg(A), CI->getZExtValue() & (W ? 63 : 31));
    } else {
      useInto(Rhs, RCX);
      opRR(CL, Reg(A), Reg(A));
      release(RCX);
    }
    return def(I, A);
  }
  case Instruction::UDiv:
  case Instruction::SDiv:
  case Instruction::URem:
  case Instruction::SRem: {
    bool Signed = I.getOpcode() == Instruction::SDiv ||
                  I.getOpcode() == Instruction::SRem;
    bool IsRem = I.getOpcode() == Instruction::URem ||
                 I.getOpcode() == Instruction::SRem;
    claim(RAX);
    claim(RDX);
    uint8_t B = use(Rhs);
    useInto(L, RAX);
    if (VT.Size < 4) {
      Signed ? sext32(B, VT) : zext32(B, VT);
      Signed ? sext32(RAX, VT) : zext32(RAX, VT);
    }
    if (Signed) {
      op0(W ? X86::CQO : X86::CDQ);
      opR(W ? X86::IDIV64r : X86::IDIV32r, Reg(B));
    } else {
      zeroGP(RDX);
      opR(W ? X86::DIV64r : X86::DIV32r, Reg(B));
    }
    release(B);
    release(IsRem ? RAX : RDX);
    return def(I, IsRem ? RDX : RAX);
  }
  default:
    return fail("binary operator");
  }
}

void X86OnePassCompiler::compileSelect(const SelectInst &I) {
  ValTy VT, CT;
  if (!classify(I.getType(), VT) ||
      !classify(I.getCondition()->getType(), CT) || !CT.I1)
    return fail("select");
  uint8_t C = use(I.getCondition());
  uint8_t T = use(I.getTrueValue()), E = use(I.getFalseValue());
  opRI(X86::TEST8ri, GR8[C], 1);
  if (VT.FP) {
    size_t Pos = jccFwd(X86::COND_NE);
    regMove(T, E);
    bindFwd(Pos);
  } else {
    bool W = VT.Size == 8;
    MCInst MI;
    MI.setOpcode(W ? X86::CMOV64rr : X86::CMOV32rr);
    MI.addOperand(MCOperand::createReg(W ? GR64[T] : GR32[T]));
    MI.addOperand(MCOperand::createReg(W ? GR64[T] : GR32[T]));
    MI.addOperand(MCOperand::createReg(W ? GR64[E] : GR32[E]));
    MI.addOperand(MCOperand::createImm(X86::COND_E));
    emit(MI);
  }
  release(C);
  release(E);
  def(I, T);
}

void X86OnePassCompiler::compileCast(const CastInst &I) {
  ValTy S, D;
  const Value *Op = I.getOperand(0);
  if (!classify(Op->getType(), S) || !classify(I.getType(), D))
    return fail("cast type");
  switch (I.getOpcode()) {
  case Instruction::Trunc:
  case Instruction::PtrToInt:
  case Instruction::PtrToAddr:
  case Instruction::IntToPtr:
  case Instruction::BitCast: {
    uint8_t R = use(Op);
    if (S.FP != D.FP) {
      uint8_t N = D.FP ? allocFP() : allocGP();
      if (D.FP)
        opRR(D.Size == 8 ? X86::MOV64toPQIrr : X86::MOVDI2PDIrr, XMM[N - XMM0],
             D.Size == 8 ? GR64[R] : GR32[R]);
      else
        opRR(D.Size == 8 ? X86::MOVPQIto64rr : X86::MOVPDI2DIrr,
             D.Size == 8 ? GR64[N] : GR32[N], XMM[R - XMM0]);
      release(R);
      R = N;
    } else if (!D.FP && D.Size > S.Size) {
      zext64(R, S);
    }
    return def(I, R);
  }
  case Instruction::ZExt: {
    uint8_t R = use(Op);
    zext64Use(R, S);
    return def(I, R);
  }
  case Instruction::SExt: {
    if (auto It = lookup(Op); It && It->Kind == ValInfo::DeferredLoad) {
      uint8_t Tmp;
      Mem M = addr(cast<LoadInst>(Op)->getPointerOperand(), Tmp);
      uint8_t R = Tmp != NoReg ? Tmp : allocGP();
      static const unsigned Opc32[] = {0, X86::MOVSX32rm8, X86::MOVSX32rm16};
      static const unsigned Opc64[] = {0, X86::MOVSX64rm8, X86::MOVSX64rm16, 0,
                                       X86::MOVSX64rm32};
      if (D.Size == 8)
        opRM(Opc64[S.Size], GR64[R], M);
      else
        opRM(Opc32[S.Size], GR32[R], M);
      return def(I, R);
    }
    uint8_t R = use(Op);
    D.Size == 8 ? sext64(R, S) : sext32(R, S);
    return def(I, R);
  }
  case Instruction::SIToFP:
  case Instruction::UIToFP: {
    bool Unsigned = I.getOpcode() == Instruction::UIToFP;
    if (Unsigned && S.Size == 8)
      return fail("uitofp i64");
    uint8_t R = use(Op);
    bool W = S.Size == 8 || Unsigned;
    if (Unsigned)
      zext64(R, S);
    else if (S.Size < 4)
      sext32(R, S);
    uint8_t X = allocFP();
    unsigned Opc = D.Size == 8 ? (W ? X86::CVTSI642SDrr : X86::CVTSI2SDrr)
                               : (W ? X86::CVTSI642SSrr : X86::CVTSI2SSrr);
    opRR(Opc, XMM[X - XMM0], W ? GR64[R] : GR32[R]);
    release(R);
    return def(I, X);
  }
  case Instruction::FPToSI:
  case Instruction::FPToUI: {
    bool Unsigned = I.getOpcode() == Instruction::FPToUI;
    if (Unsigned && D.Size == 8)
      return fail("fptoui i64");
    uint8_t X = use(Op);
    bool W = D.Size == 8 || Unsigned;
    uint8_t R = allocGP();
    unsigned Opc = S.Size == 8 ? (W ? X86::CVTTSD2SI64rr : X86::CVTTSD2SIrr)
                               : (W ? X86::CVTTSS2SI64rr : X86::CVTTSS2SIrr);
    opRR(Opc, W ? GR64[R] : GR32[R], XMM[X - XMM0]);
    release(X);
    return def(I, R);
  }
  case Instruction::FPExt:
  case Instruction::FPTrunc: {
    uint8_t X = use(Op);
    opRR(I.getOpcode() == Instruction::FPExt ? X86::CVTSS2SDrr
                                             : X86::CVTSD2SSrr,
         XMM[X - XMM0], XMM[X - XMM0]);
    return def(I, X);
  }
  default:
    return fail("cast");
  }
}

void X86OnePassCompiler::compileFNeg(const UnaryOperator &I) {
  ValTy VT;
  if (!classify(I.getType(), VT) || !VT.FP)
    return fail("fneg");
  uint8_t X = use(I.getOperand(0)), G = allocGP();
  MCRegister XR = XMM[X - XMM0];
  if (VT.Size == 8) {
    opRR(X86::MOVPQIto64rr, GR64[G], XR);
    opRRI(X86::BTC64ri8, GR64[G], GR64[G], 63);
    opRR(X86::MOV64toPQIrr, XR, GR64[G]);
  } else {
    opRR(X86::MOVPDI2DIrr, GR32[G], XR);
    opRRI(X86::XOR32ri, GR32[G], GR32[G], int32_t(0x80000000));
    opRR(X86::MOVDI2PDIrr, XR, GR32[G]);
  }
  release(G);
  def(I, X);
}

// Emit a direct call to a function or a library symbol.
void X86OnePassCompiler::emitCallTo(const Function *Direct, const char *LibName,
                                    bool MayThrow) {
  if (Direct &&
      (Direct->isAbsoluteSymbolRef() || AP.TM.isLargeGlobalValue(Direct)))
    return fail("global reference");
  const MCSymbol *Sym;
  if (LibName) {
    Sym = libSym(LibName);
  } else {
    Sym = sym(*Direct, true);
  }
  unsigned char Flag =
      ST.classifyGlobalFunctionReference(Direct, *F.getParent());
  MCInst MI;
  if (Flag == X86II::MO_NO_FLAG || Flag == X86II::MO_PLT) {
    MI.setOpcode(X86::CALL64pcrel32);
    MI.addOperand(MCOperand::createExpr(
        symExpr(Sym, Flag == X86II::MO_PLT ? X86::S_PLT : 0)));
  } else if (Flag == X86II::MO_GOTPCREL || Flag == X86II::MO_GOTPCREL_NORELAX) {
    MI.setOpcode(X86::CALL64m);
    addMem(MI,
           {X86::RIP, 0, MCRegister(), 1,
            symExpr(LibName ? Sym : sym(*Direct, false),
                    Flag == X86II::MO_GOTPCREL ? X86::S_GOTPCREL
                                               : X86::S_GOTPCREL_NORELAX)},
           Ctx);
  } else {
    return fail("call reference kind");
  }
  size_t Start = Code.size();
  emit(MI);
  recordCall(Start, MayThrow);
  // setjmp returns a second time through an indirect jump.
  if (IBT && Direct && Direct->hasFnAttribute(Attribute::ReturnsTwice))
    op0(X86::ENDBR64);
}

void X86OnePassCompiler::emitCall(const Instruction &I, const Value *Callee,
                                  const char *LibName,
                                  ArrayRef<const Value *> Args,
                                  const CallBase *Attrs, bool IsVarArg,
                                  bool MayThrow) {
  struct ArgLoc {
    const Value *V;
    ValTy VT;
    uint8_t Reg;
    unsigned StackOff;
    char Ext;
    unsigned ByValSize;
  };
  SmallVector<ArgLoc, 8> Locs;
  unsigned NumGP = 0, NumFP = 0, Stack = 0;
  for (unsigned Idx = 0; Idx != Args.size(); ++Idx) {
    ArgLoc L{Args[Idx], {}, NoReg, 0, 0, 0};
    if (!classify(L.V->getType(), L.VT))
      return fail("call argument type");
    if (Attrs && Attrs->isByValArgument(Idx)) {
      uint64_t Size = DL.getTypeAllocSize(Attrs->getParamByValType(Idx));
      if (Attrs->getParamAlign(Idx).valueOrOne() > Align(8) || Size > 512)
        return fail("byval call argument");
      L.ByValSize = std::max<uint64_t>(Size, 1);
      L.StackOff = Stack;
      Stack += alignTo(L.ByValSize, 8);
      Locs.push_back(L);
      continue;
    }
    if (Attrs) {
      if (Attrs->paramHasAttr(Idx, Attribute::ZExt))
        L.Ext = 'z';
      else if (Attrs->paramHasAttr(Idx, Attribute::SExt))
        L.Ext = 's';
    } else if (!L.VT.FP) {
      // A library call: memset takes its byte as an int.
      L.Ext = 'z';
    }
    if (L.VT.FP ? NumFP < 8 : NumGP < 6) {
      L.Reg = L.VT.FP ? XMM0 + NumFP++ : ArgGP[NumGP++];
    } else {
      L.StackOff = Stack;
      Stack += 8;
    }
    Locs.push_back(L);
  }
  const Function *Direct = Callee ? dyn_cast<Function>(Callee) : nullptr;
  bool Indirect = !LibName && !Direct;

  // Spill every live value except call operands sitting in registers that
  // argument setup does not touch.
  for (uint8_t R = 0; R != NumRegs; ++R) {
    const Value *V = Owner[R];
    if (!V || V == Busy)
      continue;
    bool IsOperand = (Indirect && V == Callee) || is_contained(Args, V);
    bool Keep = IsOperand && !isArgReg(R) && !(R == RAX && V == Callee);
    // An argument already sitting in its own register stays there.
    if (IsOperand && isArgReg(R))
      for (const ArgLoc &L : Locs)
        Keep |= L.V == V && L.Reg == R;
    if (!Keep)
      spillReg(R);
  }

  uint8_t CalleeReg = NoReg;
  if (Indirect) {
    auto It = lookup(Callee);
    if (It && It->Reg != NoReg) {
      CalleeReg = use(Callee);
    } else {
      CalleeReg = !Owner[R11] ? R11 : R10;
      claim(CalleeReg);
      useInto(Callee, CalleeReg);
    }
  }

  auto Extend = [&](const ArgLoc &L, uint8_t R) {
    if (L.VT.FP || L.VT.Size >= 4)
      return;
    if (L.Ext == 's')
      sext32(R, L.VT);
    else if (L.Ext == 'z')
      zext32(R, L.VT);
  };
  for (const ArgLoc &L : Locs) {
    if (L.Reg != NoReg)
      continue;
    Mem M{X86::RSP, L.StackOff};
    if (L.ByValSize) {
      uint8_t Tmp;
      Mem Src = addr(L.V, Tmp);
      uint8_t T = allocGP();
      // Copy exactly ByValSize bytes; reading past the object could fault.
      for (unsigned Off = 0, Step = 8; Off != L.ByValSize; Off += Step) {
        while (Off + Step > L.ByValSize)
          Step /= 2;
        Mem S = Src, D = M;
        S.Disp += Off;
        D.Disp += Off;
        ValTy VT{uint8_t(Step), false, false};
        loadMem(T, VT, S);
        storeMem(D, VT, T);
      }
      release(T);
      release(Tmp);
      continue;
    }
    auto *CI = dyn_cast<ConstantInt>(L.V);
    if (CI && isInt<32>(CI->getSExtValue())) {
      opMI(X86::MOV64mi32, M,
           L.VT.I1 || L.Ext == 'z' ? int64_t(CI->getZExtValue())
                                   : CI->getSExtValue());
      continue;
    }
    uint8_t R = use(L.V);
    Extend(L, R);
    ValTy SlotTy = L.VT;
    if (!SlotTy.FP)
      SlotTy.Size = 8;
    storeMem(M, SlotTy, R);
    release(R);
  }
  for (const ArgLoc &L : Locs) {
    if (L.Reg == NoReg)
      continue;
    useIn(L.V, L.Reg);
    Extend(L, L.Reg);
  }
  MaxOutgoing = std::max(MaxOutgoing, Stack);
  if (IsVarArg) {
    claim(RAX);
    if (NumFP)
      opRI(X86::MOV8ri, X86::AL, NumFP);
    else
      zeroGP(RAX);
  }
  if (FailReason)
    return;

  if (Indirect) {
    size_t Start = Code.size();
    opR(X86::CALL64r, GR64[CalleeReg]);
    recordCall(Start, MayThrow);
  } else {
    emitCallTo(Direct, LibName, MayThrow);
    if (FailReason)
      return;
  }
  // The call clobbers every register in use.
  dropClean();
  for (uint8_t R = 0; R != NumRegs; ++R)
    if (Owner[R] == Busy)
      Owner[R] = nullptr;

  if (I.getType()->isVoidTy())
    return;
  SmallVector<AggPart, 2> Parts;
  if (aggParts(I.getType(), Parts)) {
    int32_t Slot = aggSlotFor(&I);
    for (unsigned P = 0; P != Parts.size(); ++P)
      storeSlot(Slot + 8 * P, Parts[P].VT, Parts[P].RetReg);
    return;
  }
  ValTy RT;
  if (!classify(I.getType(), RT))
    return fail("call return type");
  uint8_t R = RT.FP ? uint8_t(XMM0) : uint8_t(RAX);
  Owner[R] = Busy;
  def(I, R);
}

void X86OnePassCompiler::compileTargetIntrinsic(const IntrinsicInst &I) {
  switch (I.getIntrinsicID()) {
  case Intrinsic::fmuladd: {
    ValTy VT;
    if (!classify(I.getType(), VT) || !VT.FP)
      return fail("fmuladd type");
    bool S = VT.Size == 4;
    uint8_t A = use(I.getArgOperand(0)), B = use(I.getArgOperand(1));
    opRRR(S ? X86::MULSSrr : X86::MULSDrr, XMM[A - XMM0], XMM[A - XMM0],
          XMM[B - XMM0]);
    release(B);
    uint8_t C = use(I.getArgOperand(2));
    opRRR(S ? X86::ADDSSrr : X86::ADDSDrr, XMM[A - XMM0], XMM[A - XMM0],
          XMM[C - XMM0]);
    release(C);
    return def(I, A);
  }
  case Intrinsic::vastart: {
    uint8_t Tmp;
    Mem M = addr(I.getArgOperand(0), Tmp);
    Mem Field = M;
    opMI(X86::MOV32mi, Field, VAGPOffset);
    Field.Disp = M.Disp + 4;
    opMI(X86::MOV32mi, Field, VAFPOffset);
    uint8_t T = allocGP();
    opRM(X86::LEA64r, GR64[T], frame(VAStackOff));
    Field.Disp = M.Disp + 8;
    opMR(X86::MOV64mr, Field, GR64[T]);
    opRM(X86::LEA64r, GR64[T], frame(RegSaveArea));
    Field.Disp = M.Disp + 16;
    opMR(X86::MOV64mr, Field, GR64[T]);
    release(T);
    return release(Tmp);
  }
  case Intrinsic::vacopy: {
    uint8_t DstTmp, SrcTmp;
    Mem Dst = addr(I.getArgOperand(0), DstTmp);
    Mem Src = addr(I.getArgOperand(1), SrcTmp);
    uint8_t T = allocGP();
    for (unsigned Off = 0; Off != 24; Off += 8) {
      Mem S = Src, D = Dst;
      S.Disp += Off;
      D.Disp += Off;
      opRM(X86::MOV64rm, GR64[T], S);
      opMR(X86::MOV64mr, D, GR64[T]);
    }
    release(T);
    release(DstTmp);
    return release(SrcTmp);
  }
  default:
    return fail("intrinsic");
  }
}

void X86OnePassCompiler::compileRet(const ReturnInst &I) {
  uint8_t R = NoReg;
  SmallVector<AggPart, 2> Parts;
  if (const Value *V = I.getReturnValue(); V && aggParts(V->getType(), Parts)) {
    auto It = lookup(V);
    if (!It || !It->Slot)
      return fail("aggregate return value");
    int32_t Slot = It->Slot;
    for (unsigned P = 0; P != Parts.size(); ++P) {
      claim(Parts[P].RetReg);
      loadMem(Parts[P].RetReg, Parts[P].VT, frame(Slot + 8 * P));
    }
    for (const AggPart &P : Parts)
      release(P.RetReg);
  } else if (V) {
    ValTy VT;
    if (!classify(V->getType(), VT))
      return fail("return type");
    R = VT.FP ? uint8_t(XMM0) : uint8_t(RAX);
    useIn(V, R);
    if (!VT.FP && VT.Size < 4) {
      if (F.hasRetAttribute(Attribute::SExt))
        sext32(R, VT);
      else if (F.hasRetAttribute(Attribute::ZExt))
        zext32(R, VT);
    }
  } else if (SRetArg) {
    R = RAX;
    useIn(SRetArg, R);
  }
  release(R);
  if (StackGuardSlot) {
    opRM(X86::MOV64rm, X86::R11, stackGuard());
    opRM(X86::CMP64rm, X86::R11, frame(StackGuardSlot));
    size_t Ok = jccFwd(X86::COND_E);
    emitCallTo(nullptr, "__stack_chk_fail", /*MayThrow=*/false);
    const TargetOptions &O = AP.TM.Options;
    if (O.TrapUnreachable && !O.NoTrapAfterNoreturn)
      op0(X86::TRAP);
    bindFwd(Ok);
  }
  EpilogueStart = Code.size();
  opRR(X86::MOV64rr, X86::RSP, X86::RBP);
  opR(X86::POP64r, X86::RBP);
  CFI.emplace_back(Code.size(),
                   MCCFIInstruction::cfiDefCfa(nullptr, DwarfRSP, 8));
  op0(X86::RET64);
  CFI.emplace_back(Code.size(),
                   MCCFIInstruction::cfiDefCfa(nullptr, DwarfRBP, 16));
}

// Resolve branch targets, shrinking rel32 jumps whose target is within rel8
// range. Removing bytes only brings targets closer, so deciding with the
// original offsets is safe.
void X86OnePassCompiler::relaxBranches() {
  // Old end offset of each shrunk jump and the bytes removed up to it.
  SmallVector<std::pair<uint32_t, uint32_t>, 32> Removed;
  auto NewOff = [&](uint32_t Off) {
    auto It = upper_bound(
        Removed, Off, [](uint32_t O, const auto &P) { return O < P.first; });
    return It == Removed.begin() ? Off : Off - std::prev(It)->second;
  };
  size_t Src = 0, Dst = 0;
  uint32_t Total = 0;
  if (FrameBytesToDrop) {
    Src = FramePatch + 4;
    Dst = Src - FrameBytesToDrop;
    Total = FrameBytesToDrop;
    Removed.emplace_back(Src, Total);
  }
  SmallVector<bool, 64> Short(Branches.size());
  for (size_t Idx = 0; Idx != Branches.size(); ++Idx) {
    Branch &B = Branches[Idx];
    if (B.BB)
      B.Target = BlockOff[B.BB->getNumber()];
    if (!isInt<8>(int64_t(B.Target) - (B.Pos + 4)))
      continue;
    Short[Idx] = true;
    // jmp rel32 (E9 cd) becomes EB cb; jcc rel32 (0F 8x cd) becomes 7x cb.
    bool IsJmp = uint8_t(Code[B.Pos - 1]) == 0xe9;
    uint8_t Opc = IsJmp ? 0xeb : 0x70 | (uint8_t(Code[B.Pos - 1]) & 0xf);
    size_t Start = B.Pos - (IsJmp ? 1 : 2);
    std::copy(Code.begin() + Src, Code.begin() + Start, Code.begin() + Dst);
    Dst += Start - Src;
    Code[Dst++] = Opc;
    ++Dst;
    Src = B.Pos + 4;
    Total += (B.Pos + 4 - Start) - 2;
    Removed.emplace_back(B.Pos + 4, Total);
  }
  std::copy(Code.begin() + Src, Code.end(), Code.begin() + Dst);
  Code.truncate(Dst + Code.size() - Src);

  for (size_t Idx = 0; Idx != Branches.size(); ++Idx) {
    const Branch &B = Branches[Idx];
    uint32_t End = NewOff(B.Pos + 4), Target = NewOff(B.Target);
    if (Short[Idx])
      Code[End - 1] = char(int8_t(Target - End));
    else
      patch32(End - 4, Target - End);
  }
  remapOffsets(NewOff, !Removed.empty());
}

bool X86OnePassCompiler::finish() {
  // Keep frame offsets well inside the range of a 32-bit displacement.
  if (FrameSize + uint64_t(MaxOutgoing) > (1u << 30)) {
    fail("frame size");
    return false;
  }
  // The prologue reserved "sub rsp, imm32" (7 bytes). An empty frame, or a
  // leaf function with a small frame that fits in the red zone, needs no
  // adjustment at all; a small frame takes the imm8 form.
  unsigned Total = alignTo(FrameSize + MaxOutgoing, 16);
  // A frame that spans a guard page needs probes, which are not implemented.
  if (Total >= 4096 && F.hasFnAttribute("probe-stack")) {
    fail("stack probe");
    return false;
  }
  bool NoAdjust = Total == 0 || (!HasCalls && FrameSize <= 128 &&
                                 !F.hasFnAttribute(Attribute::NoRedZone));
  // The pushed rbp plus the adjustment, as X86FrameLowering counts it.
  StackSize = 8 + (NoAdjust ? 0 : Total);
  if (NoAdjust) {
    FrameBytesToDrop = 7;
  } else if (Total <= 127) {
    // sub rsp, imm32 (48 81 EC id) becomes sub rsp, imm8 (48 83 EC ib).
    Code[FramePatch - 2] = char(0x83);
    Code[FramePatch] = char(Total);
    FrameBytesToDrop = 3;
  } else {
    patch32(FramePatch, Total);
  }
  relaxBranches();
  return true;
}

namespace {
class X86OnePassCodeGen final : public OnePassCodeGen {
  unsigned DwarfRSP = 0, DwarfRBP = 0;
  bool IBT = false;

public:
  static char ID;
  X86OnePassCodeGen(X86TargetMachine &TM, const OnePassCodeGenOptions &Opts)
      : OnePassCodeGen(ID, TM, Opts, &X86AsmPrinter::ID) {}

  StringRef getPassName() const override {
    return "X86 One-Pass Code Generator";
  }
  bool doInitialization(Module &M) override {
    const MCRegisterInfo &MRI = TM.getMCRegisterInfo();
    DwarfRSP = MRI.getDwarfRegNum(X86::RSP, true);
    DwarfRBP = MRI.getDwarfRegNum(X86::RBP, true);
    IBT = M.getModuleFlag("cf-protection-branch");
    return OnePassCodeGen::doInitialization(M);
  }
  bool isHandledAttr(Attribute A) const override {
    if (A.isStringAttribute())
      return (A.getKindAsString() == "probe-stack" &&
              A.getValueAsString() == "inline-asm") ||
             OnePassCodeGen::isHandledAttr(A);
    switch (A.getKindAsEnum()) {
    case Attribute::StackProtect:
    case Attribute::StackProtectReq:
    case Attribute::StackProtectStrong:
      return true;
    default:
      return OnePassCodeGen::isHandledAttr(A);
    }
  }

private:
  const X86TargetMachine &getX86TM() const {
    return static_cast<const X86TargetMachine &>(TM);
  }
  // The shadow stack needs no code; "probe-stack" matters per function.
  bool isHandledModuleFlag(const Module::ModuleFlagEntry &Flag) const override {
    StringRef Key = Flag.Key->getString();
    return Key == "cf-protection-branch" || Key == "cf-protection-return" ||
           Key == "probe-stack";
  }
  // Options of skipped passes that apply regardless of function attributes.
  const char *unsupportedModuleReason(const Module &M,
                                      MCObjectStreamer &OS) override {
    const X86Options &CL = getX86TM().getCLOpts();
    if (CL.indirect_branch_tracking || CL.speculative_load_hardening ||
        CL.seses_enable_without_lvi_cfi)
      return "codegen option";
    return nullptr;
  }
  const char *unsupportedSubtargetReason(const Function &F) override;
  bool compile(const Function &F, AsmPrinter &AP, const MCCodeEmitter &CE,
               OnePassCompiler::ModuleState &G) override {
    X86OnePassCompiler C(F, *getX86TM().getSubtargetImpl(F), AP, CE, G,
                         DwarfRSP, DwarfRBP, IBT);
    return finish(F, C);
  }
};
} // namespace

char X86OnePassCodeGen::ID = 0;

// Target features and the triple select passes that would rewrite or
// instrument the code: hardening, reserved registers, sandboxing. They may come
// from -mattr as well as "target-features", so query the subtarget.
const char *X86OnePassCodeGen::unsupportedSubtargetReason(const Function &F) {
  const X86TargetMachine &TM = getX86TM();
  const X86Subtarget &ST = *TM.getSubtargetImpl(F);
  if (!ST.is64Bit() || !ST.isTargetELF() || ST.isOSWindows() ||
      ST.getTargetTriple().isLFI() || ST.isTarget64BitILP32() ||
      TM.getCodeModel() != CodeModel::Small)
    return "target";
  if (!ST.hasSSE2() || ST.useSoftFloat() || ST.hasUserReservedRegisters() ||
      ST.useIndirectThunkCalls() || ST.useIndirectThunkBranches() ||
      ST.useLVILoadHardening() ||
      ST.useSpeculativeExecutionSideEffectSuppression() || ST.hardenSlsRet() ||
      ST.hardenSlsIJmp() ||
      (TM.getCLOpts().enable_branch_hint && ST.hasBranchHint()))
    return "target feature";
  return nullptr;
}

FunctionPass *
llvm::createX86OnePassCodeGenPass(X86TargetMachine &TM,
                                  const OnePassCodeGenOptions &Opts) {
  return new X86OnePassCodeGen(TM, Opts);
}
