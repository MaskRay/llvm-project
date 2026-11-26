//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The AArch64 part of the direct -O0 code generator; see
// llvm/CodeGen/DirectCodeGen.h. AAPCS64 ELF only.
//
// Frame layout, from high to low addresses:
// * incoming stack arguments;
// * frame objects and slots, at x29 + 16 and up, so that an offset is final
//   when it is allocated and fits a scaled unsigned load/store offset;
// * the frame record, at x29;
// * with a stack protector, the guard and below it the objects it guards;
// * outgoing stack arguments, at sp.
// The frame size is known only at the end, so the prologue is written last
// into space reserved at the function start, and every return branches to one
// epilogue at the function end.
//
//===----------------------------------------------------------------------===//

#include "AArch64.h"
#include "AArch64ExpandImm.h"
#include "AArch64MachineFunctionInfo.h"
#include "AArch64Subtarget.h"
#include "AArch64TargetMachine.h"
#include "GISel/AArch64GlobalISelUtils.h"
#include "MCTargetDesc/AArch64AddressingModes.h"
#include "MCTargetDesc/AArch64MCAsmInfo.h"
#include "MCTargetDesc/AArch64MCTargetDesc.h"
#include "Utils/AArch64BaseInfo.h"
#include "llvm/CodeGen/DirectCodeGen.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/Function.h"
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
// Register ids: 0-30 are X0-X30, 32-63 are V0-V31.
enum : uint8_t {
  X0 = 0,
  X1 = 1,
  X8 = 8,
  X9 = 9,
  X15 = 15,
  X16 = 16,
  X17 = 17,
  V0 = 32,
};
} // namespace

// Allocation order: temporaries first, argument registers last so that call
// operands rarely sit in them. X16 and X17 are scratch registers for single
// instructions, X18 is the platform register, and the callee-saved registers
// are not used.
constexpr uint8_t GPOrder[] = {9, 10, 11, 12, 13, 14, 15, 8,
                               7, 6,  5,  4,  3,  2,  1,  0};
constexpr uint8_t FPOrder[] = {48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59,
                               60, 61, 62, 63, 39, 38, 37, 36, 35, 34, 33, 32};

constexpr uint8_t ArgGP[] = {0, 1, 2, 3, 4, 5, 6, 7};
constexpr DirectCompiler::RegisterInfo AArch64Regs = {
    GPOrder,        FPOrder,         ArgGP,
    /*FirstFP=*/V0, /*NumFPArgs=*/8,
    /*NumRegs=*/64, {X0, X1},        /*FirstRetFP=*/V0,
    /*SRetArg=*/X8, /*RetHint=*/X0};

static MCRegister xreg(uint8_t R) {
  return R < 29    ? MCRegister(AArch64::X0 + R)
         : R == 29 ? MCRegister(AArch64::FP)
                   : MCRegister(AArch64::LR);
}
static MCRegister wreg(uint8_t R) { return AArch64::W0 + R; }
static MCRegister dreg(uint8_t R) { return AArch64::D0 + (R - V0); }
static MCRegister sreg(uint8_t R) { return AArch64::S0 + (R - V0); }

static MCOperand reg(MCRegister R) { return MCOperand::createReg(R); }
static MCOperand imm(int64_t V) { return MCOperand::createImm(V); }

namespace {
struct Mem {
  MCRegister Base;
  int64_t Off = 0;
};

class AArch64DirectCompiler final : public DirectCompiler {
public:
  AArch64DirectCompiler(const Function &F, const AArch64Subtarget &ST,
                        AsmPrinter &AP, const MCCodeEmitter &CE, ModuleState &G)
      : DirectCompiler(F, ST, AP, CE, G, AArch64Regs), ST(ST) {
    BTI = F.hasFnAttribute("branch-target-enforcement");
    Attribute Sign = F.getFnAttribute("sign-return-address");
    // "non-leaf" signs if the link register is saved, which is always.
    SignLR = Sign.isValid() && Sign.getValueAsString() != "none";
    DispBits = 24;
    // Variables are described relative to x29.
    Debug.SetupFrame = [](MachineFunction &MF) {
      auto *AFI = MF.getInfo<AArch64FunctionInfo>();
      AFI->setStackSizeSVE(0, 0);
      AFI->setHasStackFrame(true);
    };
  }

private:
  const AArch64Subtarget &ST;
  static constexpr unsigned DwarfFP = 29, DwarfLR = 30, DwarfSP = 31;
  // Whether indirect branch targets get a BTI, and whether the return address
  // is signed with the A key.
  bool BTI, SignLR;
  // Bytes reserved at the function start for the prologue.
  static constexpr unsigned PrologueSize = 36;
  // Bytes of guarded frame objects below the frame record.
  uint64_t GuardedSize = 0;
  // Whether the prologue sets X17 to the incoming stack pointer, for the code
  // that follows it to read stack arguments.
  bool NeedStackBase = false;
  // Variadic function state: frame offsets of the register save areas and of
  // the slot holding the address of the first variadic stack argument, and
  // the number of named register arguments.
  int32_t GPSaveArea = 0, FPSaveArea = 0, VAStackSlot = 0;
  unsigned VANumGP = 0, VANumFP = 0;
  // Branches to the epilogue.
  SmallVector<uint32_t, 4> Returns;
  const DILocation *ReturnLoc = nullptr;

  // Encoding.
  void ins(unsigned Opc, std::initializer_list<MCOperand> Ops) {
    MCInst I;
    I.setOpcode(Opc);
    for (const MCOperand &Op : Ops)
      I.addOperand(Op);
    emit(I);
  }
  MCRegister gpr(uint8_t R, bool Is64) { return Is64 ? xreg(R) : wreg(R); }
  void movRR(MCRegister Dst, MCRegister Src, bool Is64) {
    ins(Is64 ? AArch64::ORRXrs : AArch64::ORRWrs,
        {reg(Dst), reg(Is64 ? AArch64::XZR : AArch64::WZR), reg(Src), imm(0)});
  }
  void movImmReg(uint8_t R, uint64_t V, bool Is64) {
    SmallVector<AArch64_IMM::ImmInsnModel, 4> Insns;
    AArch64_IMM::expandMOVImm(V, Is64 ? 64 : 32, Insns);
    MCRegister D = gpr(R, Is64);
    for (const AArch64_IMM::ImmInsnModel &I : Insns) {
      switch (I.Opcode) {
      case AArch64::ORRWri:
      case AArch64::ORRXri:
      case AArch64::ANDXri:
      case AArch64::EORXri:
        ins(I.Opcode, {reg(D),
                       reg(I.Op1 == 0 ? (Is64 ? MCRegister(AArch64::XZR)
                                              : MCRegister(AArch64::WZR))
                                      : D),
                       imm(*I.Op2)});
        break;
      case AArch64::EONXrs:
      case AArch64::EORXrs:
      case AArch64::ORRWrs:
      case AArch64::ORRXrs:
        ins(I.Opcode, {reg(D), reg(D), reg(D), imm(*I.Op2)});
        break;
      case AArch64::MOVNWi:
      case AArch64::MOVNXi:
      case AArch64::MOVZWi:
      case AArch64::MOVZXi:
        ins(I.Opcode, {reg(D), imm(*I.Op1), imm(*I.Op2)});
        break;
      case AArch64::MOVKWi:
      case AArch64::MOVKXi:
        ins(I.Opcode, {reg(D), reg(D), imm(*I.Op1), imm(*I.Op2)});
        break;
      default:
        return fail("immediate");
      }
    }
  }
  void movImm(uint8_t R, int64_t V, unsigned Size) override {
    movImmReg(R, Size <= 4 ? uint32_t(V) : uint64_t(V), Size > 4);
  }
  // Dst = Src + Off. Src may be SP.
  void addImm(MCRegister Dst, MCRegister Src, int64_t Off) {
    uint64_t Abs = Off < 0 ? -uint64_t(Off) : Off;
    unsigned Opc = Off < 0 ? AArch64::SUBXri : AArch64::ADDXri;
    if (Abs >= (1u << 24))
      return fail("offset");
    if (Abs >> 12) {
      ins(Opc, {reg(Dst), reg(Src), imm(Abs >> 12), imm(12)});
      Src = Dst;
    }
    if ((Abs & 0xfff) || !(Abs >> 12))
      ins(Opc, {reg(Dst), reg(Src), imm(Abs & 0xfff), imm(0)});
  }
  // A load or store of Size bytes. X16 holds an address that fits neither
  // offset form.
  void memOp(unsigned Scaled, unsigned Unscaled, MCRegister Rt, Mem M,
             unsigned Size) {
    unsigned Shift = Log2_32(Size);
    if (M.Off >= 0 && !(M.Off & (Size - 1)) && M.Off >> Shift < 4096)
      return ins(Scaled, {reg(Rt), reg(M.Base), imm(M.Off >> Shift)});
    if (isInt<9>(M.Off))
      return ins(Unscaled, {reg(Rt), reg(M.Base), imm(M.Off)});
    if (Rt == AArch64::X16 || Rt == AArch64::W16)
      return fail("offset");
    addImm(AArch64::X16, M.Base, M.Off);
    ins(Scaled, {reg(Rt), reg(AArch64::X16), imm(0)});
  }
  Mem frame(int32_t Off) const { return {AArch64::FP, Off}; }
  void loadMem(uint8_t R, ValTy VT, Mem M) {
    if (VT.FP && VT.Size == 4)
      memOp(AArch64::LDRSui, AArch64::LDURSi, sreg(R), M, 4);
    else if (VT.FP)
      memOp(AArch64::LDRDui, AArch64::LDURDi, dreg(R), M, 8);
    else if (VT.Size == 1)
      memOp(AArch64::LDRBBui, AArch64::LDURBBi, wreg(R), M, 1);
    else if (VT.Size == 2)
      memOp(AArch64::LDRHHui, AArch64::LDURHHi, wreg(R), M, 2);
    else if (VT.Size == 4)
      memOp(AArch64::LDRWui, AArch64::LDURWi, wreg(R), M, 4);
    else
      memOp(AArch64::LDRXui, AArch64::LDURXi, xreg(R), M, 8);
  }
  void storeGPR(Mem M, unsigned Size, MCRegister X, MCRegister W) {
    if (Size == 1)
      memOp(AArch64::STRBBui, AArch64::STURBBi, W, M, 1);
    else if (Size == 2)
      memOp(AArch64::STRHHui, AArch64::STURHHi, W, M, 2);
    else if (Size == 4)
      memOp(AArch64::STRWui, AArch64::STURWi, W, M, 4);
    else
      memOp(AArch64::STRXui, AArch64::STURXi, X, M, 8);
  }
  void storeMem(Mem M, ValTy VT, uint8_t R) {
    if (VT.FP && VT.Size == 4)
      memOp(AArch64::STRSui, AArch64::STURSi, sreg(R), M, 4);
    else if (VT.FP)
      memOp(AArch64::STRDui, AArch64::STURDi, dreg(R), M, 8);
    else
      storeGPR(M, VT.Size, xreg(R), wreg(R));
  }

  int32_t allocFrame(uint64_t Size, uint64_t Alignment) override {
    uint64_t Off = alignTo(16 + FrameSize, Align(Alignment));
    FrameSize = Off + Size - 16;
    return Off;
  }
  int32_t allocGuardedFrame(uint64_t Size, uint64_t Alignment) override {
    GuardedSize = alignTo(GuardedSize + Size, Align(Alignment));
    return -int32_t(GuardedSize);
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
    if (Imm)
      return false;
    storeGPR(frame(Slot), 8, AArch64::XZR, AArch64::WZR);
    return true;
  }
  void regMove(uint8_t Dst, uint8_t Src) override {
    if (Dst == Src)
      return;
    if (Dst >= V0)
      ins(AArch64::FMOVDr, {reg(dreg(Dst)), reg(dreg(Src))});
    else
      movRR(xreg(Dst), xreg(Src), true);
  }
  void fpConstant(uint8_t R, uint64_t Bits, ValTy VT) override {
    if (Bits)
      movImmReg(X16, Bits, VT.Size == 8);
    if (VT.Size == 8)
      ins(AArch64::FMOVXDr,
          {reg(dreg(R)), reg(Bits ? AArch64::X16 : AArch64::XZR)});
    else
      ins(AArch64::FMOVWSr,
          {reg(sreg(R)), reg(Bits ? AArch64::W16 : AArch64::WZR)});
  }

  // Constants and globals.
  MCOperand symOp(const MCSymbol *Sym, int64_t Off, uint16_t Spec) {
    const MCExpr *E = MCSymbolRefExpr::create(Sym, Ctx);
    if (Off)
      E = MCBinaryExpr::createAdd(E, MCConstantExpr::create(Off, Ctx), Ctx);
    return MCOperand::createExpr(MCSpecifierExpr::create(E, Spec, Ctx));
  }
  void loadGOT(MCRegister X, const MCSymbol *S) {
    ins(AArch64::ADRP, {reg(X), symOp(S, 0, AArch64::S_GOT_PAGE)});
    ins(AArch64::LDRXui, {reg(X), reg(X), symOp(S, 0, AArch64::S_GOT_LO12)});
  }
  // Set X to the address of GV, directly or from the GOT. Return false if
  // the reference kind is not handled.
  bool globalAddrReg(const GlobalValue &GV, int64_t Off, MCRegister X) {
    if (GV.isThreadLocal())
      return false;
    unsigned Flags = ST.ClassifyGlobalReference(&GV, AP.TM);
    if (Flags == AArch64II::MO_NO_FLAG) {
      // An ADRP addend must stay within the 21-bit page range.
      int64_t Fold = Off >= 0 && Off < (1 << 20) ? Off : 0;
      const MCSymbol *S = sym(GV, true);
      ins(AArch64::ADRP, {reg(X), symOp(S, Fold, AArch64::S_ABS_PAGE)});
      ins(AArch64::ADDXri,
          {reg(X), reg(X), symOp(S, Fold, AArch64::S_LO12), imm(0)});
      Off -= Fold;
    } else if (Flags == AArch64II::MO_GOT) {
      loadGOT(X, sym(GV, false));
    } else {
      return false;
    }
    if (Off) {
      if (!isInt<24>(Off))
        return false;
      addImm(X, X, Off);
    }
    return true;
  }
  void globalAddr(const GlobalValue &GV, int64_t Off, uint8_t R) override {
    if (!globalAddrReg(GV, Off, xreg(R)))
      fail("global reference");
  }

  // Memory operand for a pointer. Tmp receives a register to release, if any.
  Mem addr(const Value *P, uint8_t &Tmp) {
    Tmp = NoReg;
    if (P->getType()->getPointerAddressSpace() != 0) {
      fail("address space");
      return frame(16);
    }
    if (auto *VI = lookup(P); VI) {
      if (VI->Kind == ValInfo::Alloca)
        return frame(VI->Slot);
      if (VI->Kind == ValInfo::DeferredGEP) {
        auto *GEP = cast<GetElementPtrInst>(P);
        int64_t Off = constGEPOffset(*GEP);
        Mem M = addr(GEP->getPointerOperand(), Tmp);
        M.Off += Off;
        if (!fitsDisp(M.Off))
          fail("gep offset");
        return M;
      }
    }
    Tmp = use(P);
    return {xreg(Tmp), 0};
  }
  void addDisp(uint8_t Dst, uint8_t Base, int64_t Disp) override {
    addImm(xreg(Dst), Base == NoReg ? MCRegister(AArch64::FP) : xreg(Base),
           Disp);
  }
  void addScaled(uint8_t R, uint8_t X, int64_t Stride) override {
    if (isPowerOf2_64(Stride)) {
      ins(AArch64::ADDXrs,
          {reg(xreg(R)), reg(xreg(R)), reg(xreg(X)),
           imm(AArch64_AM::getShifterImm(AArch64_AM::LSL, Log2_64(Stride)))});
      return;
    }
    movImmReg(X17, Stride, true);
    ins(AArch64::MADDXrrr,
        {reg(xreg(R)), reg(xreg(X)), reg(AArch64::X17), reg(xreg(R))});
  }
  uint8_t useDeferred(const Value *V, ValInfo &VI, ValTy VT,
                      uint8_t R) override {
    uint8_t Tmp;
    if (VI.Kind == ValInfo::DeferredGEP) {
      Mem M = addr(V, Tmp);
      R = resultReg(R, Tmp);
      if (M.Off || M.Base != xreg(R))
        addImm(xreg(R), M.Base, M.Off);
      return R;
    }
    Mem M = addr(cast<LoadInst>(V)->getPointerOperand(), Tmp);
    R = resultReg(R, Tmp);
    loadMem(R, VT, M);
    ZExtKnown = true;
    return R;
  }

  // Extensions of a narrow value held in a general-purpose register.
  void zext32(uint8_t R, ValTy VT) override {
    if (VT.Size >= 4)
      return;
    uint64_t Mask = VT.I1 ? 1 : VT.Size == 1 ? 0xff : 0xffff;
    ins(AArch64::ANDWri, {reg(wreg(R)), reg(wreg(R)),
                          imm(AArch64_AM::encodeLogicalImmediate(Mask, 32))});
  }
  void zextLow32(uint8_t R) override { movRR(wreg(R), wreg(R), false); }
  // Sign-extend to 32 or 64 bits.
  void sext(uint8_t R, ValTy VT, bool To64) {
    unsigned Bits = VT.I1 ? 1 : 8 * VT.Size;
    if (Bits >= (To64 ? 64u : 32u))
      return;
    ins(To64 ? AArch64::SBFMXri : AArch64::SBFMWri,
        {reg(gpr(R, To64)), reg(gpr(R, To64)), imm(0), imm(Bits - 1)});
  }
  void sext32(uint8_t R, ValTy VT) override { sext(R, VT, false); }
  void sext64(uint8_t R, ValTy VT) override { sext(R, VT, true); }

  // Compare R with an immediate if it fits; Is64 selects the register width.
  bool cmpImm(uint8_t R, bool Is64, int64_t V) {
    MCRegister Zero = Is64 ? AArch64::XZR : AArch64::WZR;
    if (V >= 0 && V < 4096)
      ins(Is64 ? AArch64::SUBSXri : AArch64::SUBSWri,
          {reg(Zero), reg(gpr(R, Is64)), imm(V), imm(0)});
    else if (V < 0 && V > -4096)
      ins(Is64 ? AArch64::ADDSXri : AArch64::ADDSWri,
          {reg(Zero), reg(gpr(R, Is64)), imm(-V), imm(0)});
    else
      return false;
    return true;
  }
  void cmpRR(MCRegister A, MCRegister B, bool Is64) {
    ins(Is64 ? AArch64::SUBSXrs : AArch64::SUBSWrs,
        {reg(Is64 ? AArch64::XZR : AArch64::WZR), reg(A), reg(B), imm(0)});
  }

  // Branches.
  // A condition above the AArch64CC range is a CBZ or CBNZ on the register in
  // the low five bits.
  enum : unsigned { CBFlag = 0x100, CBNonZero = 0x40, CB64 = 0x20 };
  // Branch::Pos is the offset of the instruction.
  uint32_t emitJcc(unsigned CC) override {
    uint32_t Pos = Code.size();
    if (CC & CBFlag) {
      bool Is64 = CC & CB64;
      unsigned Opc = CC & CBNonZero ? (Is64 ? AArch64::CBNZX : AArch64::CBNZW)
                                    : (Is64 ? AArch64::CBZX : AArch64::CBZW);
      ins(Opc, {reg(gpr(CC & 31, Is64)), imm(0)});
    } else {
      ins(AArch64::Bcc, {imm(CC), imm(0)});
    }
    return Pos;
  }
  uint32_t emitJmp() override {
    uint32_t Pos = Code.size();
    ins(AArch64::B, {imm(0)});
    return Pos;
  }
  unsigned invertCC(unsigned CC) override {
    if (CC & CBFlag)
      return CC ^ CBNonZero;
    return AArch64CC::getInvertedCondCode(AArch64CC::CondCode(CC));
  }
  unsigned emitCondition(const Value *C) override {
    if (auto *VI = lookup(C); VI && VI->Kind == ValInfo::DeferredCmp) {
      auto &I = *cast<ICmpInst>(C);
      const Value *L = I.getOperand(0);
      auto *Rhs = dyn_cast<Constant>(I.getOperand(1));
      ValTy VT = typeOf(L);
      if (!I.isEquality() || !Rhs || !Rhs->isNullValue() || VT.FP)
        return emitICmp(I);
      // The branch follows immediately, so the register is still intact.
      uint8_t R = use(L);
      if (VT.Size < 4)
        zext32Use(R, VT);
      release(R);
      return CBFlag | (I.getPredicate() == CmpInst::ICMP_NE ? CBNonZero : 0) |
             (VT.Size == 8 ? CB64 : 0) | R;
    }
    uint8_t R = use(C);
    ins(AArch64::ANDSWri, {reg(AArch64::WZR), reg(wreg(R)),
                           imm(AArch64_AM::encodeLogicalImmediate(1, 32))});
    release(R);
    return AArch64CC::NE;
  }
  unsigned emitCaseCompare(uint8_t R, bool Is64, int64_t V) override {
    if (!cmpImm(R, Is64, V)) {
      movImmReg(X17, Is64 ? V : uint32_t(V), Is64);
      cmpRR(gpr(R, Is64), gpr(X17, Is64), Is64);
    }
    return AArch64CC::EQ;
  }
  void emitTrap(bool Debug) override {
    ins(AArch64::BRK, {imm(Debug ? 0xf000 : 1)});
  }
  void emitLandingPadEntry() override {
    if (BTI)
      ins(AArch64::HINT, {imm(getBTIHintNum(false, /*JumpTarget=*/true))});
  }
  // Load the stack protector guard value, which glibc and musl keep in a
  // global variable.
  void loadStackGuard(MCRegister X) {
    loadGOT(X, libSym("__stack_chk_guard"));
    ins(AArch64::LDRXui, {reg(X), reg(X), imm(0)});
  }

  uint8_t exceptionPointerReg() override { return X0; }
  uint8_t exceptionSelectorReg() override { return X1; }

  bool prologue() override;
  bool finish() override;
  // The largest offset of a scaled 7-bit stp/ldp.
  static constexpr uint64_t MaxPairOffset = 504;
  // Whether the frame record is at sp and stp/ldp with writeback reach it.
  static bool isSmallFrame(uint64_t Record, uint64_t Size) {
    return Record == 0 && Size <= MaxPairOffset;
  }
  void recordPair(unsigned Opc, uint64_t Record);
  void emitEpilogue(uint64_t Record, uint64_t Size);
  bool resolveBranches();
  void emitPrologue(uint64_t Record, uint64_t Size);
  unsigned emitICmp(const ICmpInst &I) override;
  void setCC(uint8_t R, unsigned CC) override {
    ins(AArch64::CSINCWr, {reg(wreg(R)), reg(AArch64::WZR), reg(AArch64::WZR),
                           imm(invertCC(CC))});
  }
  void emitCallTo(const Function *Direct, const char *LibName,
                  bool MayThrow) override;
  void emitCall(const Instruction &I, const Value *Callee, const char *LibName,
                ArrayRef<const Value *> Args, const CallBase *Attrs,
                bool IsVarArg, bool MayThrow) override;
  void compileLoad(const LoadInst &I) override;
  void compileStore(const StoreInst &I) override;
  void compileBinary(const BinaryOperator &I) override;
  void compileFCmp(const FCmpInst &I) override;
  void compileSelect(const SelectInst &I) override;
  void compileCast(const CastInst &I) override;
  void compileFNeg(const UnaryOperator &I) override;
  void compileTargetIntrinsic(const IntrinsicInst &I) override;
  void compileRet(const ReturnInst &I) override;
};
} // namespace

bool AArch64DirectCompiler::prologue() {
  const Triple &TT = ST.getTargetTriple();
  if (StackGuardSlot && (!TT.isOSLinux() || TT.isAndroid())) {
    fail("stack protector guard");
    return false;
  }
  // Reserved for the frame setup; see finish().
  Code.append(PrologueSize, 0);
  if (StackGuardSlot) {
    loadStackGuard(AArch64::X16);
    storeGPR(frame(StackGuardSlot), 8, AArch64::X16, AArch64::W16);
  }

  unsigned NumGP = 0, NumFP = 0;
  int32_t StackOff = 0;
  for (const Argument &A : F.args()) {
    if (A.hasByValAttr()) {
      fail("argument attribute");
      return false;
    }
    ValTy VT;
    if (!classify(A.getType(), VT)) {
      fail("argument type");
      return false;
    }
    uint8_t R;
    if (A.hasStructRetAttr()) {
      R = X8;
    } else if (VT.FP ? NumFP != 8 : NumGP != 8) {
      R = VT.FP ? V0 + NumFP++ : X0 + NumGP++;
    } else {
      // Each stack argument takes 8 bytes. Copy a used one to its slot.
      if (!A.use_empty()) {
        NeedStackBase = true;
        memOp(AArch64::LDRXui, AArch64::LDURXi, AArch64::X16,
              {AArch64::X17, StackOff}, 8);
        storeGPR(frame(slotFor(&A)), 8, AArch64::X16, AArch64::W16);
      }
      StackOff += 8;
      continue;
    }
    assignArgReg(A, VT, R, /*NeedSlot=*/false);
  }

  if (F.isVarArg()) {
    GPSaveArea = allocFrame(64, 8);
    FPSaveArea = allocFrame(128, 16);
    VAStackSlot = allocFrame(8, 8);
    VANumGP = NumGP;
    VANumFP = NumFP;
    for (unsigned I = 0; I != 8; ++I)
      storeGPR(frame(GPSaveArea + 8 * I), 8, xreg(I), wreg(I));
    for (unsigned I = 0; I != 8; ++I)
      memOp(AArch64::STRQui, AArch64::STURQi, AArch64::Q0 + I,
            {AArch64::FP, FPSaveArea + 16 * I}, 16);
    NeedStackBase = true;
    if (StackOff)
      addImm(AArch64::X17, AArch64::X17, StackOff);
    storeGPR(frame(VAStackSlot), 8, AArch64::X17, AArch64::W17);
  }
  return true;
}

unsigned AArch64DirectCompiler::emitICmp(const ICmpInst &I) {
  const Value *L = I.getOperand(0), *Rhs = I.getOperand(1);
  ValTy VT = typeOf(L);
  if (VT.FP) {
    fail("icmp type");
    return AArch64CC::EQ;
  }
  bool Signed = I.isSigned(), W = VT.Size == 8;
  uint8_t A = use(L);
  if (VT.Size < 4)
    Signed ? sext32(A, VT) : zext32Use(A, VT);
  auto *CI = dyn_cast<ConstantInt>(Rhs);
  bool Done = false;
  if (CI)
    Done = cmpImm(A, W,
                  Signed || VT.Size >= 4 ? CI->getSExtValue()
                                         : int64_t(CI->getZExtValue()));
  else if (isa<ConstantPointerNull>(Rhs))
    Done = cmpImm(A, true, 0);
  if (!Done) {
    uint8_t B = use(Rhs);
    if (VT.Size < 4)
      Signed ? sext32(B, VT) : zext32Use(B, VT);
    cmpRR(gpr(A, W), gpr(B, W), W);
    release(B);
  }
  release(A);
  static const AArch64CC::CondCode CCs[] = {
      AArch64CC::EQ, AArch64CC::NE, AArch64CC::HI, AArch64CC::HS,
      AArch64CC::LO, AArch64CC::LS, AArch64CC::GT, AArch64CC::GE,
      AArch64CC::LT, AArch64CC::LE};
  static_assert(std::size(CCs) == CmpInst::LAST_ICMP_PREDICATE -
                                      CmpInst::FIRST_ICMP_PREDICATE + 1);
  return CCs[I.getPredicate() - CmpInst::FIRST_ICMP_PREDICATE];
}

void AArch64DirectCompiler::compileFCmp(const FCmpInst &I) {
  ValTy VT = typeOf(I.getOperand(0));
  if (!VT.FP || I.getType()->isVectorTy())
    return fail("fcmp type");
  CmpInst::Predicate Pred = I.getPredicate();
  if (Pred == CmpInst::FCMP_FALSE || Pred == CmpInst::FCMP_TRUE)
    return fail("fcmp predicate");
  // The condition for the predicate, and a second one to OR in if needed.
  AArch64CC::CondCode CC, CC2;
  AArch64GISelUtils::changeFCMPPredToAArch64CC(Pred, CC, CC2);
  uint8_t A = use(I.getOperand(0)), B = use(I.getOperand(1));
  if (VT.Size == 4)
    ins(AArch64::FCMPSrr, {reg(sreg(A)), reg(sreg(B))});
  else
    ins(AArch64::FCMPDrr, {reg(dreg(A)), reg(dreg(B))});
  release(A);
  release(B);
  uint8_t R = allocGP();
  MCRegister WR = wreg(R);
  setCC(R, CC);
  // csinc r, r, wzr, !cc2 keeps r unless cc2 holds, in which case it is 1.
  if (CC2 != AArch64CC::AL)
    ins(AArch64::CSINCWr,
        {reg(WR), reg(WR), reg(AArch64::WZR), imm(invertCC(CC2))});
  def(I, R);
}

void AArch64DirectCompiler::compileLoad(const LoadInst &I) {
  ValTy VT;
  SmallVector<AggPart, 2> Parts;
  if (!I.isAtomic() && aggParts(I.getType(), Parts)) {
    uint8_t Tmp;
    Mem M = addr(I.getPointerOperand(), Tmp);
    int32_t Slot = aggSlotFor(&I);
    for (unsigned P = 0; P != Parts.size(); ++P) {
      uint8_t R = Parts[P].VT.FP ? allocFP() : allocGP();
      loadMem(R, Parts[P].VT, {M.Base, int64_t(M.Off + Parts[P].MemOff)});
      storeSlot(aggPartSlot(Slot, P), Parts[P].VT, R);
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
  uint8_t R = loadDest(I, VT, Tmp);
  loadMem(R, VT, M);
  release(Tmp);
  def(I, R);
}

void AArch64DirectCompiler::compileStore(const StoreInst &I) {
  const Value *V = I.getValueOperand();
  ValTy VT;
  SmallVector<AggPart, 2> Parts;
  if (!I.isAtomic() && aggParts(V->getType(), Parts)) {
    auto *VI = lookup(V);
    if (!VI || !VI->Slot)
      return fail("aggregate store value");
    int32_t Slot = VI->Slot;
    uint8_t Tmp;
    Mem M = addr(I.getPointerOperand(), Tmp);
    for (unsigned P = 0; P != Parts.size(); ++P) {
      uint8_t R = Parts[P].VT.FP ? allocFP() : allocGP();
      loadSlot(R, Parts[P].VT, aggPartSlot(Slot, P));
      if (Parts[P].VT.I1)
        zext32(R, Parts[P].VT);
      storeMem({M.Base, int64_t(M.Off + Parts[P].MemOff)}, Parts[P].VT, R);
      release(R);
    }
    return release(Tmp);
  }
  if (I.isAtomic() || !classify(V->getType(), VT))
    return fail("store");
  uint8_t Tmp;
  auto *CI = dyn_cast<ConstantInt>(V);
  if ((CI && CI->isZero()) || isa<ConstantPointerNull>(V)) {
    Mem M = addr(I.getPointerOperand(), Tmp);
    storeGPR(M, VT.Size, AArch64::XZR, AArch64::WZR);
    return release(Tmp);
  }
  uint8_t R = use(V);
  if (VT.I1)
    zext32(R, VT);
  Mem M = addr(I.getPointerOperand(), Tmp);
  storeMem(M, VT, R);
  release(R);
  release(Tmp);
}

void AArch64DirectCompiler::compileBinary(const BinaryOperator &I) {
  ValTy VT;
  if (!classify(I.getType(), VT))
    return fail("binary operator type");
  const Value *L = I.getOperand(0), *Rhs = I.getOperand(1);
  if (VT.FP) {
    bool S = VT.Size == 4;
    unsigned Opc;
    switch (I.getOpcode()) {
    case Instruction::FAdd:
      Opc = S ? AArch64::FADDSrr : AArch64::FADDDrr;
      break;
    case Instruction::FSub:
      Opc = S ? AArch64::FSUBSrr : AArch64::FSUBDrr;
      break;
    case Instruction::FMul:
      Opc = S ? AArch64::FMULSrr : AArch64::FMULDrr;
      break;
    case Instruction::FDiv:
      Opc = S ? AArch64::FDIVSrr : AArch64::FDIVDrr;
      break;
    default:
      return fail("fp operator");
    }
    uint8_t A = use(L), B = use(Rhs);
    MCRegister RA = S ? sreg(A) : dreg(A), RB = S ? sreg(B) : dreg(B);
    ins(Opc, {reg(RA), reg(RA), reg(RB)});
    release(B);
    return def(I, A);
  }

  bool W = VT.Size == 8;
  auto Reg = [&](uint8_t R) { return gpr(R, W); };
  auto *CI = dyn_cast<ConstantInt>(Rhs);
  switch (I.getOpcode()) {
  case Instruction::Add:
  case Instruction::Sub: {
    bool IsSub = I.getOpcode() == Instruction::Sub;
    uint8_t A = use(L);
    int64_t Imm = CI ? CI->getSExtValue() : 0;
    if (CI && Imm > -4096 && Imm < 4096) {
      unsigned Opc = IsSub != (Imm < 0)
                         ? (W ? AArch64::SUBXri : AArch64::SUBWri)
                         : (W ? AArch64::ADDXri : AArch64::ADDWri);
      ins(Opc, {reg(Reg(A)), reg(Reg(A)), imm(std::abs(Imm)), imm(0)});
    } else {
      uint8_t B = use(Rhs);
      unsigned Opc = IsSub ? (W ? AArch64::SUBXrs : AArch64::SUBWrs)
                           : (W ? AArch64::ADDXrs : AArch64::ADDWrs);
      ins(Opc, {reg(Reg(A)), reg(Reg(A)), reg(Reg(B)), imm(0)});
      release(B);
    }
    return def(I, A);
  }
  case Instruction::And:
  case Instruction::Or:
  case Instruction::Xor: {
    unsigned RR, RI;
    if (I.getOpcode() == Instruction::And) {
      RR = W ? AArch64::ANDXrs : AArch64::ANDWrs;
      RI = W ? AArch64::ANDXri : AArch64::ANDWri;
    } else if (I.getOpcode() == Instruction::Or) {
      RR = W ? AArch64::ORRXrs : AArch64::ORRWrs;
      RI = W ? AArch64::ORRXri : AArch64::ORRWri;
    } else {
      RR = W ? AArch64::EORXrs : AArch64::EORWrs;
      RI = W ? AArch64::EORXri : AArch64::EORWri;
    }
    uint8_t A = use(L);
    unsigned Bits = W ? 64 : 32;
    uint64_t Imm =
        CI ? CI->getZExtValue() & maskTrailingOnes<uint64_t>(Bits) : 0;
    uint64_t Enc;
    if (CI && AArch64_AM::processLogicalImmediate(Imm, Bits, Enc)) {
      ins(RI, {reg(Reg(A)), reg(Reg(A)), imm(Enc)});
    } else {
      uint8_t B = use(Rhs);
      ins(RR, {reg(Reg(A)), reg(Reg(A)), reg(Reg(B)), imm(0)});
      release(B);
    }
    return def(I, A);
  }
  case Instruction::Mul: {
    uint8_t A = use(L), B = use(Rhs);
    ins(W ? AArch64::MADDXrrr : AArch64::MADDWrrr,
        {reg(Reg(A)), reg(Reg(A)), reg(Reg(B)),
         reg(W ? AArch64::XZR : AArch64::WZR)});
    release(B);
    return def(I, A);
  }
  case Instruction::Shl:
  case Instruction::LShr:
  case Instruction::AShr: {
    bool IsShl = I.getOpcode() == Instruction::Shl;
    bool IsAShr = I.getOpcode() == Instruction::AShr;
    unsigned Bits = W ? 64 : 32;
    uint8_t A = use(L);
    if (!IsShl && VT.Size < 4)
      IsAShr ? sext32(A, VT) : zext32(A, VT);
    if (CI) {
      unsigned S = CI->getZExtValue() & (Bits - 1);
      unsigned Opc = IsAShr ? (W ? AArch64::SBFMXri : AArch64::SBFMWri)
                            : (W ? AArch64::UBFMXri : AArch64::UBFMWri);
      if (IsShl)
        ins(Opc, {reg(Reg(A)), reg(Reg(A)), imm((Bits - S) % Bits),
                  imm(Bits - 1 - S)});
      else
        ins(Opc, {reg(Reg(A)), reg(Reg(A)), imm(S), imm(Bits - 1)});
    } else {
      uint8_t B = use(Rhs);
      unsigned Opc = IsShl    ? (W ? AArch64::LSLVXr : AArch64::LSLVWr)
                     : IsAShr ? (W ? AArch64::ASRVXr : AArch64::ASRVWr)
                              : (W ? AArch64::LSRVXr : AArch64::LSRVWr);
      ins(Opc, {reg(Reg(A)), reg(Reg(A)), reg(Reg(B))});
      release(B);
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
    uint8_t A = use(L), B = use(Rhs);
    if (VT.Size < 4) {
      Signed ? sext32(A, VT) : zext32(A, VT);
      Signed ? sext32(B, VT) : zext32(B, VT);
    }
    unsigned Div = Signed ? (W ? AArch64::SDIVXr : AArch64::SDIVWr)
                          : (W ? AArch64::UDIVXr : AArch64::UDIVWr);
    if (!IsRem) {
      ins(Div, {reg(Reg(A)), reg(Reg(A)), reg(Reg(B))});
    } else {
      // a - (a / b) * b
      MCRegister Q = Reg(X17);
      ins(Div, {reg(Q), reg(Reg(A)), reg(Reg(B))});
      ins(W ? AArch64::MSUBXrrr : AArch64::MSUBWrrr,
          {reg(Reg(A)), reg(Q), reg(Reg(B)), reg(Reg(A))});
    }
    release(B);
    return def(I, A);
  }
  default:
    return fail("binary operator");
  }
}

void AArch64DirectCompiler::compileSelect(const SelectInst &I) {
  ValTy VT, CT;
  if (!classify(I.getType(), VT) ||
      !classify(I.getCondition()->getType(), CT) || !CT.I1)
    return fail("select");
  uint8_t C = use(I.getCondition());
  uint8_t T = use(I.getTrueValue()), E = use(I.getFalseValue());
  ins(AArch64::ANDSWri, {reg(AArch64::WZR), reg(wreg(C)),
                         imm(AArch64_AM::encodeLogicalImmediate(1, 32))});
  if (VT.FP) {
    bool S = VT.Size == 4;
    MCRegister RT = S ? sreg(T) : dreg(T), RE = S ? sreg(E) : dreg(E);
    ins(S ? AArch64::FCSELSrrr : AArch64::FCSELDrrr,
        {reg(RT), reg(RT), reg(RE), imm(AArch64CC::NE)});
  } else {
    ins(AArch64::CSELXr,
        {reg(xreg(T)), reg(xreg(T)), reg(xreg(E)), imm(AArch64CC::NE)});
  }
  release(C);
  release(E);
  def(I, T);
}

void AArch64DirectCompiler::compileCast(const CastInst &I) {
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
      if (D.FP && D.Size == 8)
        ins(AArch64::FMOVXDr, {reg(dreg(N)), reg(xreg(R))});
      else if (D.FP)
        ins(AArch64::FMOVWSr, {reg(sreg(N)), reg(wreg(R))});
      else if (D.Size == 8)
        ins(AArch64::FMOVDXr, {reg(xreg(N)), reg(dreg(R))});
      else
        ins(AArch64::FMOVSWr, {reg(wreg(N)), reg(sreg(R))});
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
    if (auto *VI = lookup(Op); VI && VI->Kind == ValInfo::DeferredLoad) {
      uint8_t Tmp;
      Mem M = addr(cast<LoadInst>(Op)->getPointerOperand(), Tmp);
      uint8_t R = resultReg(NoReg, Tmp);
      bool To64 = D.Size == 8;
      if (S.Size == 1)
        memOp(To64 ? AArch64::LDRSBXui : AArch64::LDRSBWui,
              To64 ? AArch64::LDURSBXi : AArch64::LDURSBWi, gpr(R, To64), M, 1);
      else if (S.Size == 2)
        memOp(To64 ? AArch64::LDRSHXui : AArch64::LDRSHWui,
              To64 ? AArch64::LDURSHXi : AArch64::LDURSHWi, gpr(R, To64), M, 2);
      else if (To64)
        memOp(AArch64::LDRSWui, AArch64::LDURSWi, xreg(R), M, 4);
      else
        memOp(AArch64::LDRWui, AArch64::LDURWi, wreg(R), M, 4);
      return def(I, R);
    }
    uint8_t R = use(Op);
    D.Size == 8 ? sext64(R, S) : sext32(R, S);
    return def(I, R);
  }
  case Instruction::SIToFP:
  case Instruction::UIToFP: {
    bool Unsigned = I.getOpcode() == Instruction::UIToFP;
    uint8_t R = use(Op);
    if (S.Size < 4)
      Unsigned ? zext32(R, S) : sext32(R, S);
    bool W = S.Size == 8;
    uint8_t X = allocFP();
    unsigned Opc;
    if (D.Size == 8)
      Opc = Unsigned ? (W ? AArch64::UCVTFUXDri : AArch64::UCVTFUWDri)
                     : (W ? AArch64::SCVTFUXDri : AArch64::SCVTFUWDri);
    else
      Opc = Unsigned ? (W ? AArch64::UCVTFUXSri : AArch64::UCVTFUWSri)
                     : (W ? AArch64::SCVTFUXSri : AArch64::SCVTFUWSri);
    ins(Opc, {reg(D.Size == 8 ? dreg(X) : sreg(X)), reg(gpr(R, W))});
    release(R);
    return def(I, X);
  }
  case Instruction::FPToSI:
  case Instruction::FPToUI: {
    bool Unsigned = I.getOpcode() == Instruction::FPToUI;
    uint8_t X = use(Op);
    bool W = D.Size == 8;
    uint8_t R = allocGP();
    unsigned Opc;
    if (S.Size == 8)
      Opc = Unsigned ? (W ? AArch64::FCVTZUUXDr : AArch64::FCVTZUUWDr)
                     : (W ? AArch64::FCVTZSUXDr : AArch64::FCVTZSUWDr);
    else
      Opc = Unsigned ? (W ? AArch64::FCVTZUUXSr : AArch64::FCVTZUUWSr)
                     : (W ? AArch64::FCVTZSUXSr : AArch64::FCVTZSUWSr);
    ins(Opc, {reg(gpr(R, W)), reg(S.Size == 8 ? dreg(X) : sreg(X))});
    release(X);
    return def(I, R);
  }
  case Instruction::FPExt: {
    uint8_t X = use(Op);
    ins(AArch64::FCVTDSr, {reg(dreg(X)), reg(sreg(X))});
    return def(I, X);
  }
  case Instruction::FPTrunc: {
    uint8_t X = use(Op);
    ins(AArch64::FCVTSDr, {reg(sreg(X)), reg(dreg(X))});
    return def(I, X);
  }
  default:
    return fail("cast");
  }
}

void AArch64DirectCompiler::compileFNeg(const UnaryOperator &I) {
  ValTy VT;
  if (!classify(I.getType(), VT) || !VT.FP)
    return fail("fneg");
  uint8_t X = use(I.getOperand(0));
  if (VT.Size == 8)
    ins(AArch64::FNEGDr, {reg(dreg(X)), reg(dreg(X))});
  else
    ins(AArch64::FNEGSr, {reg(sreg(X)), reg(sreg(X))});
  def(I, X);
}

void AArch64DirectCompiler::emitCallTo(const Function *Direct,
                                       const char *LibName, bool MayThrow) {
  unsigned Flags = Direct ? ST.classifyGlobalFunctionReference(Direct, AP.TM)
                          : unsigned(AArch64II::MO_NO_FLAG);
  size_t Start;
  if (Flags == AArch64II::MO_NO_FLAG) {
    const MCSymbol *Sym = LibName ? libSym(LibName) : sym(*Direct, true);
    Start = Code.size();
    ins(AArch64::BL,
        {MCOperand::createExpr(MCSymbolRefExpr::create(Sym, Ctx))});
  } else if (Flags == AArch64II::MO_GOT) {
    loadGOT(AArch64::X16, sym(*Direct, false));
    Start = Code.size();
    ins(AArch64::BLR, {reg(AArch64::X16)});
  } else {
    return fail("call reference kind");
  }
  recordCall(Start, MayThrow);
}

void AArch64DirectCompiler::emitCall(const Instruction &I, const Value *Callee,
                                     const char *LibName,
                                     ArrayRef<const Value *> Args,
                                     const CallBase *Attrs, bool IsVarArg,
                                     bool MayThrow) {
  SmallVector<ArgLoc, 8> Locs;
  unsigned NumGP = 0, NumFP = 0, Stack = 0;
  for (unsigned Idx = 0; Idx != Args.size(); ++Idx) {
    ArgLoc L{Args[Idx]};
    if (!classify(L.V->getType(), L.VT))
      return fail("call argument type");
    if (Attrs && Attrs->isByValArgument(Idx))
      return fail("byval call argument");
    L.Ext = argExt(Attrs, Idx, L.VT);
    if (Attrs && Attrs->paramHasAttr(Idx, Attribute::StructRet)) {
      L.Reg = X8;
    } else if (L.VT.FP ? NumFP < 8 : NumGP < 8) {
      L.Reg = L.VT.FP ? V0 + NumFP++ : X0 + NumGP++;
    } else {
      L.StackOff = Stack;
      Stack += 8;
    }
    Locs.push_back(L);
  }
  const Function *Direct = Callee ? dyn_cast<Function>(Callee) : nullptr;
  bool Indirect = !LibName && !Direct;
  spillForCall(Indirect ? Callee : nullptr, Args, Locs);

  uint8_t CalleeReg = NoReg;
  if (Indirect) {
    auto *VI = lookup(Callee);
    if (VI && VI->Reg != NoReg && !isArgReg(VI->Reg)) {
      CalleeReg = use(Callee);
    } else {
      CalleeReg = X9;
      for (uint8_t R = X9; R <= X15; ++R)
        if (!Owner[R]) {
          CalleeReg = R;
          break;
        }
      claim(CalleeReg);
      useInto(Callee, CalleeReg);
    }
  }

  // The callee may rely on an i1 being zero-extended to 8 bits even without
  // an attribute.
  auto Extend = [&](const ArgLoc &L, uint8_t R) {
    if (L.VT.FP || L.VT.Size >= 4)
      return;
    if (L.Ext == ArgExt::Sign)
      sext32(R, L.VT);
    else if (L.Ext == ArgExt::Zero || L.VT.I1)
      zext32(R, L.VT);
  };
  for (const ArgLoc &L : Locs) {
    if (L.Reg != NoReg)
      continue;
    Mem M{AArch64::SP, L.StackOff};
    if (auto *CI = dyn_cast<ConstantInt>(L.V); CI && CI->isZero()) {
      storeGPR(M, 8, AArch64::XZR, AArch64::WZR);
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
  if (FailReason)
    return;

  if (Indirect) {
    size_t Start = Code.size();
    ins(AArch64::BLR, {reg(xreg(CalleeReg))});
    recordCall(Start, MayThrow);
  } else {
    emitCallTo(Direct, LibName, MayThrow);
    if (FailReason)
      return;
  }
  // setjmp returns a second time through an indirect branch.
  if (BTI && Attrs && Attrs->hasFnAttr(Attribute::ReturnsTwice) &&
      !ST.noBTIAtReturnTwice())
    ins(AArch64::HINT, {imm(getBTIHintNum(false, /*JumpTarget=*/true))});
  finishCall(I);
}

void AArch64DirectCompiler::compileTargetIntrinsic(const IntrinsicInst &I) {
  switch (I.getIntrinsicID()) {
  case Intrinsic::fmuladd: {
    ValTy VT;
    if (!classify(I.getType(), VT) || !VT.FP)
      return fail("fmuladd type");
    bool S = VT.Size == 4;
    auto Reg = [&](uint8_t R) { return S ? sreg(R) : dreg(R); };
    // The regular pipeline fuses as well, and the results must agree.
    uint8_t A = use(I.getArgOperand(0)), B = use(I.getArgOperand(1));
    uint8_t C = use(I.getArgOperand(2));
    ins(S ? AArch64::FMADDSrrr : AArch64::FMADDDrrr,
        {reg(Reg(A)), reg(Reg(A)), reg(Reg(B)), reg(Reg(C))});
    release(B);
    release(C);
    return def(I, A);
  }
  case Intrinsic::vastart: {
    // struct { void *stack, *gr_top, *vr_top; int gr_offs, vr_offs; }
    uint8_t Tmp;
    Mem M = addr(I.getArgOperand(0), Tmp);
    uint8_t T = allocGP();
    loadSlot(T, GP64, VAStackSlot);
    storeGPR(M, 8, xreg(T), wreg(T));
    addImm(xreg(T), AArch64::FP, GPSaveArea + 64);
    storeGPR({M.Base, M.Off + 8}, 8, xreg(T), wreg(T));
    addImm(xreg(T), AArch64::FP, FPSaveArea + 128);
    storeGPR({M.Base, M.Off + 16}, 8, xreg(T), wreg(T));
    movImm(T, -int64_t(8 - VANumGP) * 8, 4);
    storeGPR({M.Base, M.Off + 24}, 4, xreg(T), wreg(T));
    movImm(T, -int64_t(8 - VANumFP) * 16, 4);
    storeGPR({M.Base, M.Off + 28}, 4, xreg(T), wreg(T));
    release(T);
    return release(Tmp);
  }
  case Intrinsic::vacopy: {
    uint8_t DstTmp, SrcTmp;
    Mem Dst = addr(I.getArgOperand(0), DstTmp);
    Mem Src = addr(I.getArgOperand(1), SrcTmp);
    uint8_t T = allocGP();
    for (unsigned Off = 0; Off != 32; Off += 8) {
      loadMem(T, GP64, {Src.Base, Src.Off + Off});
      storeGPR({Dst.Base, Dst.Off + Off}, 8, xreg(T), wreg(T));
    }
    release(T);
    release(DstTmp);
    return release(SrcTmp);
  }
  default:
    return fail("intrinsic");
  }
}

void AArch64DirectCompiler::compileRet(const ReturnInst &I) {
  setReturnValue(I);
  if (StackGuardSlot) {
    memOp(AArch64::LDRXui, AArch64::LDURXi, AArch64::X17, frame(StackGuardSlot),
          8);
    loadStackGuard(AArch64::X16);
    cmpRR(AArch64::X16, AArch64::X17, true);
    size_t Ok = jccFwd(AArch64CC::EQ);
    emitNoReturnCall("__stack_chk_fail", /*MayThrow=*/false);
    bindFwd(Ok);
  }
  EpilogueStart = Code.size();
  Returns.push_back(Code.size());
  ReturnLoc = I.getDebugLoc().get();
  ins(AArch64::B, {imm(0)});
}

// stp/ldp of the frame record at sp + Record, or with its address in x16 if
// that is out of range.
void AArch64DirectCompiler::recordPair(unsigned Opc, uint64_t Record) {
  bool Near = Record <= MaxPairOffset;
  MCRegister Base = AArch64::SP;
  if (!Near) {
    Base = AArch64::X16;
    addImm(Base, AArch64::SP, Record);
  }
  ins(Opc, {reg(AArch64::FP), reg(AArch64::LR), reg(Base),
            imm(Near ? Record / 8 : 0)});
}

// A return that ends the function falls through into the epilogue.
void AArch64DirectCompiler::emitEpilogue(uint64_t Record, uint64_t Size) {
  bool FallThrough = Returns.back() + 4 == Code.size();
  if (FallThrough) {
    Code.truncate(Returns.pop_back_val());
    if (G.Text)
      Insts.pop_back();
  } else if (!Debug.Rows.empty()) {
    Debug.Rows.push_back({uint32_t(Code.size()), ReturnLoc,
                          /*NewBlock=*/true, /*Epilogue=*/true, nullptr});
  }
  for (uint32_t Pos : Returns)
    Branches.push_back({Pos, uint32_t(Code.size()), nullptr, false});
  if (isSmallFrame(Record, Size)) {
    ins(AArch64::LDPXpost, {reg(AArch64::SP), reg(AArch64::FP),
                            reg(AArch64::LR), reg(AArch64::SP), imm(Size / 8)});
  } else {
    recordPair(AArch64::LDPXi, Record);
    CFI.emplace_back(Code.size(),
                     MCCFIInstruction::cfiDefCfa(nullptr, DwarfSP, Size));
    addImm(AArch64::SP, AArch64::SP, Size);
  }
  CFI.emplace_back(Code.size(),
                   MCCFIInstruction::cfiDefCfa(nullptr, DwarfSP, 0));
  CFI.emplace_back(Code.size(),
                   MCCFIInstruction::createRestore(nullptr, DwarfLR));
  CFI.emplace_back(Code.size(),
                   MCCFIInstruction::createRestore(nullptr, DwarfFP));
  if (SignLR) {
    ins(AArch64::AUTIASP, {});
    CFI.emplace_back(Code.size(),
                     MCCFIInstruction::createNegateRAState(nullptr));
  }
  ins(AArch64::RET, {reg(AArch64::LR)});
}

bool AArch64DirectCompiler::resolveBranches() {
  for (const Branch &B : Branches) {
    int64_t Disp = (int64_t(targetOf(B)) - B.Pos) / 4;
    if (B.Cond ? !isInt<19>(Disp) : !isInt<26>(Disp)) {
      fail("branch range");
      return false;
    }
    uint32_t Field =
        B.Cond ? (uint32_t(Disp) & 0x7ffff) << 5 : uint32_t(Disp) & 0x3ffffff;
    char *P = &Code[B.Pos];
    support::endian::write32le(P, support::endian::read32le(P) | Field);
  }
  return true;
}

// Write the prologue at the end of the space reserved for it and drop the
// unused part.
void AArch64DirectCompiler::emitPrologue(uint64_t Record, uint64_t Size) {
  // The offset of each CFI directive relative to the prologue start.
  SmallVector<std::pair<unsigned, MCCFIInstruction>, 6> PrologueCFI;
  size_t Start = Code.size();
  auto Here = [&] { return Code.size() - Start; };
  int64_t RecordOff = int64_t(Record) - int64_t(Size);
  // PACIASP is also a landing pad for an indirect call, which is possible
  // under the same condition as in AArch64BranchTargets.
  if (SignLR) {
    ins(AArch64::PACIASP, {});
    PrologueCFI.emplace_back(Here(),
                             MCCFIInstruction::createNegateRAState(nullptr));
  } else if (BTI && (!F.hasLocalLinkage() || F.hasAddressTaken())) {
    ins(AArch64::HINT, {imm(getBTIHintNum(/*CallTarget=*/true, false))});
  }
  if (isSmallFrame(Record, Size)) {
    ins(AArch64::STPXpre, {reg(AArch64::SP), reg(AArch64::FP), reg(AArch64::LR),
                           reg(AArch64::SP), imm(-int64_t(Size) / 8)});
    PrologueCFI.emplace_back(Here(),
                             MCCFIInstruction::cfiDefCfaOffset(nullptr, Size));
  } else {
    addImm(AArch64::SP, AArch64::SP, -int64_t(Size));
    PrologueCFI.emplace_back(Here(),
                             MCCFIInstruction::cfiDefCfaOffset(nullptr, Size));
    recordPair(AArch64::STPXi, Record);
  }
  PrologueCFI.emplace_back(
      Here(), MCCFIInstruction::createOffset(nullptr, DwarfFP, RecordOff));
  PrologueCFI.emplace_back(
      Here(), MCCFIInstruction::createOffset(nullptr, DwarfLR, RecordOff + 8));
  if (Record <= MaxPairOffset)
    ins(AArch64::ADDXri,
        {reg(AArch64::FP), reg(AArch64::SP), imm(Record), imm(0)});
  else
    movRR(AArch64::FP, AArch64::X16, true);
  PrologueCFI.emplace_back(
      Here(), MCCFIInstruction::cfiDefCfa(nullptr, DwarfFP, Size - Record));
  if (NeedStackBase)
    addImm(AArch64::X17, AArch64::FP, Size - Record);

  unsigned Len = Here(), Removed = PrologueSize - Len;
  std::copy(Code.begin() + Start, Code.end(), Code.begin() + Removed);
  Code.truncate(Start);
  Code.erase(Code.begin(), Code.begin() + Removed);
  if (Removed)
    remapOffsets([&](uint32_t Off) { return Off - Removed; });
  CFI.insert(CFI.begin(), PrologueCFI.begin(), PrologueCFI.end());
  if (G.Text) {
    // The recorded prologue moves to the front as well.
    auto *First = partition_point(
        Insts, [&](const auto &P) { return P.first < Start - Removed; });
    for (auto &P : make_range(First, Insts.end()))
      P.first -= Start - Removed;
    std::rotate(Insts.begin(), First, Insts.end());
    for (const Branch &B : Branches)
      setBranchTarget(B.Pos - Removed, B.Cond ? 1 : 0, targetOf(B) - Removed);
  }
}

bool AArch64DirectCompiler::finish() {
  // The frame record is Record bytes above sp, and Size is the whole frame.
  uint64_t Record = alignTo(MaxOutgoing, 16) + alignTo(GuardedSize, 16);
  uint64_t Size = Record + alignTo(16 + FrameSize, 16);
  if (Size >= (1u << 24)) {
    fail("frame size");
    return false;
  }
  // With stack probing, the store of the frame record is the only probe. The
  // caller may leave 1024 bytes above sp unprobed, so the record must be
  // within 4096 - 1024 bytes of the incoming sp, and what is below it within
  // the 1024 bytes this function may leave to its callees.
  if ((Size - Record > 4096 - 1024 || Record > 1024) &&
      (F.hasFnAttribute("probe-stack") ||
       F.getParent()->getModuleFlag("probe-stack"))) {
    fail("stack probe");
    return false;
  }
  StackSize = Size;
  if (!Returns.empty())
    emitEpilogue(Record, Size);
  if (!resolveBranches())
    return false;
  emitPrologue(Record, Size);
  return true;
}

namespace {
class AArch64DirectCodeGen final : public DirectCodeGen {
public:
  static char ID;
  AArch64DirectCodeGen(AArch64TargetMachine &TM,
                       const DirectCodeGenOptions &Opts)
      : DirectCodeGen(ID, TM, Opts, &AArch64AsmPrinterID) {}

  StringRef getPassName() const override {
    return "AArch64 Direct Code Generator";
  }

private:
  const AArch64TargetMachine &getAArch64TM() const {
    return static_cast<const AArch64TargetMachine &>(TM);
  }
  bool isHandledAttr(Attribute A) const override {
    if (!A.isStringAttribute())
      return DirectCodeGen::isHandledAttr(A);
    StringRef K = A.getKindAsString(), V = A.getValueAsString();
    // Signing with the B key needs a .cfi_b_key_frame directive.
    return K == "branch-target-enforcement" || K == "guarded-control-stack" ||
           (K == "sign-return-address" &&
            (V == "none" || V == "non-leaf" || V == "all")) ||
           (K == "sign-return-address-key" && V == "a_key") ||
           DirectCodeGen::isHandledAttr(A);
  }
  // A call to a function with SME attributes may need a streaming mode
  // change or a lazy ZA save around it.
  bool changesCalls(Attribute A) const override {
    return A.isStringAttribute() && A.getKindAsString().starts_with("aarch64_");
  }
  // Flags that select module-level output, that function attributes repeat,
  // or that are off.
  bool isHandledModuleFlag(const Module::ModuleFlagEntry &Flag) const override {
    StringRef Key = Flag.Key->getString();
    static const StringRef Any[] = {"aarch64-elf-pauthabi-platform",
                                    "aarch64-elf-pauthabi-version",
                                    "ptrauth-init-fini",
                                    "ptrauth-init-fini-address-discrimination",
                                    "branch-target-enforcement",
                                    "guarded-control-stack",
                                    "sign-return-address",
                                    "sign-return-address-all"};
    static const StringRef OffOnly[] = {"ptrauth-elf-got",
                                        "ptrauth-sign-personality",
                                        "sign-return-address-with-bkey"};
    auto *CI = mdconst::dyn_extract_or_null<ConstantInt>(Flag.Val);
    return is_contained(Any, Key) ||
           (is_contained(OffOnly, Key) && CI && CI->isZero());
  }
  const char *unsupportedSubtargetReason(const Function &F) override;
  bool compile(const Function &F, AsmPrinter &AP, const MCCodeEmitter &CE,
               DirectCompiler::ModuleState &G) override {
    AArch64DirectCompiler C(F, *getAArch64TM().getSubtargetImpl(F), AP, CE, G);
    return runAndEmit(F, C);
  }
};
} // namespace

char AArch64DirectCodeGen::ID = 0;

// Target features and the triple select passes that would rewrite or
// instrument the code. They may come from -mattr as well as "target-features",
// so query the subtarget.
const char *
AArch64DirectCodeGen::unsupportedSubtargetReason(const Function &F) {
  const AArch64TargetMachine &TM = getAArch64TM();
  const AArch64Subtarget &ST = *TM.getSubtargetImpl(F);
  if (!ST.isTargetELF() || !ST.isLittleEndian() || ST.isTargetILP32() ||
      ST.isLFI() || TM.getCodeModel() != CodeModel::Small)
    return "target";
  if (!ST.hasFPARMv8() || ST.requiresStrictAlign() ||
      ST.hasCustomCallingConv() || ST.hardenSlsRetBr() || ST.hardenSlsBlr() ||
      ST.fixCortexA53_835769() || ST.isLRReservedForRA() ||
      ST.getNumXRegisterReserved() != ST.isXRegisterReserved(18))
    return "target feature";
  return nullptr;
}

FunctionPass *
llvm::createAArch64DirectCodeGenPass(AArch64TargetMachine &TM,
                                     const DirectCodeGenOptions &Opts) {
  return new AArch64DirectCodeGen(TM, Opts);
}
