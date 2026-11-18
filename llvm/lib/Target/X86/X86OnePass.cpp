//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// A single-pass -O0 code generator. It compiles an IR function straight to
// machine code in a private buffer, without MachineFunction, and hands the
// bytes to AsmPrinter. Any function it cannot handle is left untouched for the
// regular pipeline.
//
// Value model:
// * static allocas live in fixed frame slots;
// * a value with a single use in its own block lives only in a register;
// * every other value is stored to its own frame slot at the definition, and
//   its register is kept as a clean copy until it is needed or a call or the
//   block end is reached.
// Constant-offset addresses and loads used by the next instruction are folded
// into memory operands. Only caller-saved registers are used and the frame
// pointer is always set up.
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
#include "llvm/ADT/BitVector.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/CodeGen/AsmPrinter.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/DebugProgramInstruction.h"
#include "llvm/IR/EHPersonalities.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GetElementPtrTypeIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/LegacyPassManagers.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PseudoProbe.h"
#include "llvm/MC/MCAsmBackend.h"
#include "llvm/MC/MCAssembler.h"
#include "llvm/MC/MCCodeEmitter.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCDwarf.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/MC/MCFixup.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCObjectStreamer.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/LEB128.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Transforms/Utils/Local.h"

using namespace llvm;

#define DEBUG_TYPE "x86-onepass"

STATISTIC(NumCompiled, "Number of functions emitted by the one-pass compiler");
STATISTIC(NumFallback, "Number of functions left to the regular pipeline");
STATISTIC(NumDroppedDbg, "Number of debug records the one-pass compiler drops");

// The gates below are allowlists: anything they do not know sends the function
// (or the module) to the regular pipeline.

// Attributes that either do not affect code generation or are handled here.
static bool isHandledAttr(Attribute A) {
  if (A.isStringAttribute()) {
    StringRef K = A.getKindAsString();
    static const StringRef Known[] = {"alloc-family",
                                      "frame-pointer",
                                      "min-legal-vector-width",
                                      "no-trapping-math",
                                      "stack-protector-buffer-size",
                                      "target-cpu",
                                      "target-features",
                                      "tune-cpu",
                                      "no-builtins",
                                      "no-infs-fp-math",
                                      "no-nans-fp-math",
                                      "no-signed-zeros-fp-math",
                                      "approx-func-fp-math",
                                      "unsafe-fp-math",
                                      "less-precise-fpmad",
                                      "disable-tail-calls",
                                      "no-jump-tables",
                                      "no-inline-line-tables"};
    return is_contained(Known, K) || K.starts_with("no-builtin-");
  }
  switch (A.getKindAsEnum()) {
  // Function attributes.
  case Attribute::AllocKind:
  case Attribute::AllocSize:
  case Attribute::AlwaysInline:
  case Attribute::Builtin:
  case Attribute::Cold:
  case Attribute::Hot:
  case Attribute::InlineHint:
  case Attribute::LikelyModuleLocal:
  case Attribute::Memory:
  case Attribute::MinSize:
  case Attribute::MustProgress:
  case Attribute::NoBuiltin:
  case Attribute::NoCallback:
  case Attribute::NoDuplicate:
  case Attribute::NoFree:
  case Attribute::NoInline:
  case Attribute::NoMerge:
  case Attribute::NonLazyBind:
  case Attribute::NoProfile:
  case Attribute::NoRecurse:
  case Attribute::NoRedZone:
  case Attribute::NoReturn:
  case Attribute::NoSync:
  case Attribute::NoUnwind:
  case Attribute::OptimizeForSize:
  case Attribute::OptimizeNone:
  case Attribute::ReturnsTwice:
  case Attribute::Speculatable:
  case Attribute::UWTable:
  case Attribute::WillReturn:
  // Parameter and return attributes.
  case Attribute::AllocatedPointer:
  case Attribute::Alignment:
  case Attribute::ByVal:
  case Attribute::Captures:
  case Attribute::DeadOnReturn:
  case Attribute::DeadOnUnwind:
  case Attribute::Dereferenceable:
  case Attribute::DereferenceableOrNull:
  case Attribute::ImmArg:
  case Attribute::Initializes:
  case Attribute::NoAlias:
  case Attribute::NoExt:
  case Attribute::NoFPClass:
  case Attribute::NoFreeObj:
  case Attribute::NonNull:
  case Attribute::NoUndef:
  case Attribute::Range:
  case Attribute::ReadNone:
  case Attribute::ReadOnly:
  case Attribute::Returned:
  case Attribute::SExt:
  case Attribute::StructRet:
  case Attribute::Writable:
  case Attribute::WriteOnly:
  case Attribute::ZExt:
    return true;
  default:
    return false;
  }
}

static bool isHandledMetadata(unsigned Kind) {
  switch (Kind) {
  case LLVMContext::MD_tbaa:
  case LLVMContext::MD_tbaa_struct:
  case LLVMContext::MD_prof:
  case LLVMContext::MD_fpmath:
  case LLVMContext::MD_range:
  case LLVMContext::MD_invariant_load:
  case LLVMContext::MD_invariant_group:
  case LLVMContext::MD_alias_scope:
  case LLVMContext::MD_noalias:
  case LLVMContext::MD_nontemporal:
  case LLVMContext::MD_nonnull:
  case LLVMContext::MD_noundef:
  case LLVMContext::MD_align:
  case LLVMContext::MD_dereferenceable:
  case LLVMContext::MD_dereferenceable_or_null:
  case LLVMContext::MD_unpredictable:
  case LLVMContext::MD_loop:
  case LLVMContext::MD_access_group:
  case LLVMContext::MD_mem_parallel_loop_access:
  case LLVMContext::MD_callees:
  case LLVMContext::MD_annotation:
  case LLVMContext::MD_nosanitize:
    return true;
  default:
    return false;
  }
}

// Return null if the module can be one-pass compiled, or the reason it cannot.
static const char *unsupportedModuleReason(const Module &M,
                                           const X86TargetMachine &TM,
                                           MCObjectStreamer &OS) {
  const TargetOptions &O = TM.Options;
  if (O.EmitCallSiteInfo || O.EmitCallGraphSection || O.BBAddrMap ||
      O.BBSections != BasicBlockSection::None ||
      O.FunctionSplitting == FunctionSplittingMode::All ||
      O.ForceDwarfFrameSection || O.MCOptions.EmitSFrameUnwind)
    return "target option";
  // Options of skipped passes that apply regardless of function attributes.
  const X86Options &CL = TM.getCLOpts();
  if (CL.indirect_branch_tracking || CL.speculative_load_hardening ||
      CL.seses_enable_without_lvi_cfi || EnableFSDiscriminator)
    return "codegen option";
  // Pre-encoded bytes bypass branch-alignment padding.
  if (OS.getAssembler().getBackend().allowAutoPadding())
    return "branch alignment padding";
  if (M.getContext().getMainRemarkStreamer() ||
      M.getContext().getDiagHandlerPtr()->isAnyRemarkEnabled())
    return "remarks";
  if (M.getNamedMetadata(PseudoProbeDescMetadataName))
    return "pseudo probes";
  static const StringRef KnownFlags[] = {"wchar_size",
                                         "PIC Level",
                                         "PIE Level",
                                         "uwtable",
                                         "frame-pointer",
                                         "Debug Info Version",
                                         "Dwarf Version",
                                         "DWARF64",
                                         "CodeView",
                                         "CodeViewGHash",
                                         "Code Model",
                                         "MaxTLSAlign",
                                         "RtLibUseGOT",
                                         "direct-access-external-data",
                                         "openmp",
                                         "ThinLTO",
                                         "EnableSplitLTOUnit",
                                         "min_enum_size",
                                         "SemanticInterposition",
                                         "debug-info-assignment-tracking",
                                         "long-double-type",
                                         "debugger-tuning"};
  SmallVector<Module::ModuleFlagEntry, 8> Flags;
  M.getModuleFlagsMetadata(Flags);
  for (const Module::ModuleFlagEntry &Flag : Flags)
    if (!is_contained(KnownFlags, Flag.Key->getString()))
      return "module flag";
  return nullptr;
}

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
  NoReg = 0xff,
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
struct ValTy {
  uint8_t Size = 0; // in bytes
  bool FP = false;
  bool I1 = false;
};

struct Mem {
  MCRegister Base;
  int64_t Disp = 0;
  MCRegister Index = {};
  unsigned Scale = 1;
  const MCExpr *Expr = nullptr;
  Mem(MCRegister Base, int64_t Disp, MCRegister Index = {}, unsigned Scale = 1,
      const MCExpr *Expr = nullptr)
      : Base(Base), Disp(Disp), Index(Index), Scale(Scale), Expr(Expr) {}
};

struct ValInfo {
  enum : uint8_t { Normal, Alloca, DeferredCmp, DeferredGEP, DeferredLoad };
  // Frame-pointer-relative offset of the spill slot (or of the object for an
  // alloca). 0 means no slot yet.
  int32_t Slot = 0;
  uint8_t Reg = NoReg;
  uint8_t Kind = Normal;
};

// Encodings of instructions whose operands are all registers and immediates.
// They need no fixups, so the bytes depend only on the instruction and the
// subtarget.
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

private:
  static constexpr unsigned Size = 4096;
  std::unique_ptr<Entry[]> Table{new Entry[Size]};

public:
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
};

// Per-module state shared by all functions.
struct ModuleCache {
  // Kind id of !heapallocsite, which only CodeView emission consumes.
  unsigned HeapAllocSiteKind = 0;
  DenseMap<const void *, bool> AttrVerdicts, ParamVerdicts;
  bool attrsHandled(AttributeList AL) {
    return handled(AttrVerdicts, AL, /*ParamsOnly=*/false);
  }
  bool paramAttrsHandled(AttributeList AL) {
    return handled(ParamVerdicts, AL, /*ParamsOnly=*/true);
  }
  // Whether every attribute in AL, or every return and parameter attribute,
  // is handled. Lists are uniqued, so the verdict is cached by pointer.
  static bool handled(DenseMap<const void *, bool> &Cache, AttributeList AL,
                      bool ParamsOnly) {
    auto [It, Inserted] = Cache.try_emplace(AL.getRawPointer(), true);
    if (Inserted)
      for (unsigned Idx : AL.indexes())
        if (!ParamsOnly || Idx != AttributeList::FunctionIndex)
          for (Attribute A : AL.getAttributes(Idx))
            It->second &= isHandledAttr(A);
    return It->second;
  }

  // Symbols of globals, without and with a preferred local alias, and of
  // library functions.
  DenseMap<const GlobalValue *, MCSymbol *> Syms[2];
  DenseMap<const char *, MCSymbol *> LibSyms;
  unsigned DwarfRSP = 0, DwarfRBP = 0;
  EncodingCache Encodings;

  // Per-function buffers, kept across functions to reuse their storage.
  DenseMap<const Value *, ValInfo> Vals;
  SmallVector<char, 0> Code;
  SmallVector<MCFixup, 0> Fixups;
  SmallVector<uint32_t, 0> BlockOff;
};

class Compiler {
public:
  Compiler(const Function &F, const X86Subtarget &ST, AsmPrinter &AP,
           const MCCodeEmitter &CE, ModuleCache &G)
      : Code(G.Code), Fixups(G.Fixups), F(F), ST(ST), DL(F.getDataLayout()),
        AP(AP), Ctx(AP.OutContext), CE(CE), G(G), Vals(G.Vals),
        Busy(reinterpret_cast<const Value *>(this)), BlockOff(G.BlockOff) {
    Code.clear();
    Fixups.clear();
    Vals.clear();
    std::fill(std::begin(Owner), std::end(Owner), nullptr);
    std::fill(std::begin(Clean), std::end(Clean), false);
  }

  bool run();

  SmallVector<char, 0> &Code;
  SmallVector<MCFixup, 0> &Fixups;
  SmallVector<std::pair<unsigned, MCCFIInstruction>, 8> CFI;
  uint64_t StackSize = 0;
  // Exception table contents; meaningful only if HasLandingPads.
  AsmPrinter::CompiledFunctionEH EH;
  bool HasLandingPads = false;
  // Line rows and variable locations; meaningful only if HasDebug.
  AsmPrinter::CompiledFunctionDebug Debug;
  bool HasDebug = false;
  // Whether the function has a subprogram, i.e. rows and variables are
  // collected.
  bool RecordDebug = false;
  // Variables behind a pointer argument; their expressions get a deref.
  SmallVector<unsigned, 0> DerefVars;
  const char *FailReason = nullptr;
  std::string FailDetail;

private:
  const Function &F;
  const X86Subtarget &ST;
  const DataLayout &DL;
  AsmPrinter &AP;
  MCContext &Ctx;
  const MCCodeEmitter &CE;
  ModuleCache &G;

  DenseMap<const Value *, ValInfo> &Vals;
  const Value *Owner[NumRegs];
  // Whether the register is a copy of a value that is also in its frame slot,
  // so that it can be dropped without a store.
  bool Clean[NumRegs];
  // Owner value for a register held by the instruction being compiled.
  const Value *const Busy;
  uint64_t FrameSize = 0;
  unsigned MaxOutgoing = 0;
  // Position of the frame size immediate in the prologue.
  size_t FramePatch = 0;
  // Offset of the epilogue of the return being compiled.
  size_t EpilogueStart = 0;
  // Trailing bytes of the frame adjustment instruction that are not needed.
  unsigned FrameBytesToDrop = 0;
  // Variadic function state: frame offset of the register save area, the
  // initial va_list offsets, and the frame offset of the first stack argument.
  int32_t RegSaveArea = 0;
  unsigned VAGPOffset = 0, VAFPOffset = 0;
  int32_t VAStackOff = 0;
  bool HasCalls = false;
  const Argument *SRetArg = nullptr;
  const BasicBlock *NextBB = nullptr;
  // Exception handling. Call sites are recorded for every call that may throw
  // in a function with a personality. CallSiteLPad holds the landing pad block
  // of a call site until the block offsets are final.
  SmallVector<const BasicBlock *, 8> CallSiteLPad;
  void recordCall(size_t Start, bool MayThrow) {
    HasCalls = true;
    if (MayThrow && F.hasPersonalityFn()) {
      EH.CallSites.push_back(
          {uint32_t(Start), uint32_t(Code.size() - Start), 0, 0});
      CallSiteLPad.push_back(nullptr);
    }
  }
  DenseMap<const LandingPadInst *, uint32_t> PadAction;
  unsigned typeIdFor(const GlobalValue *GV) {
    auto It = find(EH.TypeInfos, GV);
    if (It == EH.TypeInfos.end()) {
      EH.TypeInfos.push_back(GV);
      return EH.TypeInfos.size();
    }
    return It - EH.TypeInfos.begin() + 1;
  }
  void appendSLEB(int64_t V) {
    uint8_t Buf[10];
    unsigned N = encodeSLEB128(V, Buf);
    EH.Actions.append(Buf, Buf + N);
  }
  uint32_t actionFor(const LandingPadInst &LPI);
  void emitCallTo(const Function *Direct, const char *LibName, bool MayThrow);
  void compileInvoke(const InvokeInst &I);
  void compileLandingPad(const LandingPadInst &I);
  void compileResume(const ResumeInst &I);

  // Whether the last use()/useInto() produced a value whose bits above its
  // type are zero.
  bool ZExtKnown = false;
  SmallVector<uint32_t, 0> &BlockOff;
  // Branches in emission order. Pos is the offset of the rel32 field; Target
  // is a code offset, taken from BB once all blocks are placed.
  struct Branch {
    uint32_t Pos;
    uint32_t Target;
    const BasicBlock *BB;
  };
  SmallVector<Branch, 0> Branches;

  void fail(const char *Reason) {
    if (!FailReason)
      FailReason = Reason;
  }
  // Fail, naming the first unhandled attribute of AL for the report.
  void failAttrs(const char *Reason, AttributeList AL) {
    if (!FailReason)
      for (AttributeSet AS : AL)
        for (Attribute A : AS)
          if (FailDetail.empty() && !isHandledAttr(A))
            FailDetail = A.getAsString();
    fail(Reason);
  }

  // Encoding.
  void emit(const MCInst &I) {
    size_t Start = Code.size(), FirstFixup = Fixups.size();
    EncodingCache::Key K;
    auto *E = G.Encodings.slot(I, ST, K);
    if (E && EncodingCache::hit(*E, K)) {
      Code.append(E->Bytes, E->Bytes + E->Len);
      return;
    }
    CE.encodeInstruction(I, Code, Fixups, ST);
    for (size_t Idx = FirstFixup; Idx != Fixups.size(); ++Idx)
      Fixups[Idx].setOffset(Fixups[Idx].getOffset() + Start);
    if (E && Fixups.size() == FirstFixup && Code.size() - Start <= 15)
      EncodingCache::fill(*E, K, ArrayRef(Code).drop_front(Start));
  }
  // The encoder emits a fixup even for a constant branch target. The
  // displacement is patched in the buffer instead.
  void emitBranch(const MCInst &I) {
    size_t NumFixups = Fixups.size();
    CE.encodeInstruction(I, Code, Fixups, ST);
    Fixups.truncate(NumFixups);
  }
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
    I.addOperand(MCOperand::createReg(MCRegister()));
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
  void movImm(uint8_t R, int64_t V, unsigned Size) {
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
  size_t jccFwd(X86::CondCode CC) {
    MCInst I;
    I.setOpcode(X86::JCC_4);
    I.addOperand(MCOperand::createImm(0));
    I.addOperand(MCOperand::createImm(CC));
    emitBranch(I);
    Branches.push_back({uint32_t(Code.size() - 4), 0, nullptr});
    return Branches.size() - 1;
  }
  void bindFwd(size_t Idx) { Branches[Idx].Target = Code.size(); }
  void jccTo(X86::CondCode CC, const BasicBlock *BB) {
    Branches[jccFwd(CC)].BB = BB;
  }
  void relaxBranches();
  void jmpTo(const BasicBlock *BB, bool Force = false) {
    if (BB == NextBB && !Force)
      return;
    MCInst I;
    I.setOpcode(X86::JMP_4);
    I.addOperand(MCOperand::createImm(0));
    emitBranch(I);
    Branches.push_back({uint32_t(Code.size() - 4), 0, BB});
  }

  // Types and values.
  bool classify(Type *T, ValTy &VT) {
    if (T->isPointerTy()) {
      if (T->getPointerAddressSpace() != 0)
        return false;
      VT = {8, false, false};
      return true;
    }
    if (auto *IT = dyn_cast<IntegerType>(T)) {
      switch (IT->getBitWidth()) {
      case 1:
        VT = {1, false, true};
        return true;
      case 8:
      case 16:
      case 32:
      case 64:
        VT = {uint8_t(IT->getBitWidth() / 8), false, false};
        return true;
      default:
        return false;
      }
    }
    if (T->isFloatTy()) {
      VT = {4, true, false};
      return true;
    }
    if (T->isDoubleTy()) {
      VT = {8, true, false};
      return true;
    }
    return false;
  }
  ValTy typeOf(const Value *V) {
    ValTy VT;
    if (!classify(V->getType(), VT)) {
      fail("unsupported type");
      VT = {8, false, false};
    }
    return VT;
  }
  int32_t slotFor(const Value *V) { return slotFor(Vals[V]); }
  int32_t slotFor(ValInfo &VI) {
    if (!VI.Slot) {
      FrameSize += 8;
      VI.Slot = -int32_t(FrameSize);
    }
    return VI.Slot;
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
  // Slots hold the whole general-purpose register so reloads of any width work.
  void storeSlot(int32_t Slot, ValTy VT, uint8_t R) {
    if (!VT.FP)
      VT.Size = 8;
    storeMem(frame(Slot), VT, R);
  }
  void regMove(uint8_t Dst, uint8_t Src) {
    if (Dst == Src)
      return;
    if (Dst >= XMM0)
      opRR(X86::MOVAPSrr, XMM[Dst - XMM0], XMM[Src - XMM0]);
    else
      opRR(X86::MOV64rr, GR64[Dst], GR64[Src]);
  }

  // Register management.
  void release(uint8_t R) {
    if (R != NoReg) {
      Owner[R] = nullptr;
      Clean[R] = false;
    }
  }
  void spillReg(uint8_t R) {
    const Value *V = Owner[R];
    ValInfo &VI = Vals[V];
    if (!Clean[R])
      storeSlot(slotFor(VI), typeOf(V), R);
    VI.Reg = NoReg;
    release(R);
  }
  // Store R to the slot of V and keep R as a clean copy.
  void storeAndCache(const Value *V, ValTy VT, uint8_t R) {
    ValInfo &VI = Vals[V];
    storeSlot(slotFor(VI), VT, R);
    Owner[R] = V;
    Clean[R] = true;
    VI.Reg = R;
  }
  // Forget all clean copies. Nothing is emitted, so this is safe wherever
  // control flow merges or registers are clobbered.
  void dropClean() {
    for (uint8_t R = 0; R != NumRegs; ++R)
      if (Clean[R])
        spillReg(R);
  }
  uint8_t allocFrom(ArrayRef<uint8_t> Order) {
    for (uint8_t R : Order)
      if (!Owner[R]) {
        Owner[R] = Busy;
        return R;
      }
    for (uint8_t R : Order)
      if (Owner[R] != Busy) {
        spillReg(R);
        Owner[R] = Busy;
        return R;
      }
    fail("out of registers");
    return Order[0];
  }
  uint8_t allocGP() { return allocFrom(GPOrder); }
  uint8_t allocFP() { return allocFrom(FPOrder); }
  // Take R for the current instruction, spilling a value that lives there.
  void claim(uint8_t R) {
    if (Owner[R] == Busy) {
      fail("register conflict");
      return;
    }
    if (Owner[R])
      spillReg(R);
    Owner[R] = Busy;
  }

  // Constants and globals.
  MCSymbol *sym(const GlobalValue &GV, bool PreferLocal) {
    MCSymbol *&S = G.Syms[PreferLocal][&GV];
    if (!S)
      S = PreferLocal ? AP.getSymbolPreferLocal(GV) : AP.getSymbol(&GV);
    return S;
  }
  const MCExpr *symExpr(const MCSymbol *Sym, uint16_t Spec = 0) {
    return MCSymbolRefExpr::create(Sym, Spec, Ctx);
  }
  void globalAddr(const GlobalValue *GV, int64_t Off, uint8_t R) {
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
  // Materialize a constant into R, or into a fresh register if R is NoReg.
  uint8_t constant(const Constant *C, uint8_t R) {
    ValTy VT = typeOf(C);
    if (VT.FP) {
      if (R == NoReg)
        R = allocFP();
      uint64_t Bits = 0;
      if (auto *CF = dyn_cast<ConstantFP>(C))
        Bits = CF->getValueAPF().bitcastToAPInt().getZExtValue();
      else if (!isa<UndefValue>(C))
        fail("fp constant");
      MCRegister X = XMM[R - XMM0];
      if (!Bits) {
        opRRR(X86::XORPSrr, X, X, X);
      } else {
        uint8_t G = allocGP();
        movImm(G, Bits, VT.Size);
        if (VT.Size == 8)
          opRR(X86::MOV64toPQIrr, X, GR64[G]);
        else
          opRR(X86::MOVDI2PDIrr, X, GR32[G]);
        release(G);
      }
      return R;
    }
    if (R == NoReg)
      R = allocGP();
    if (auto *CI = dyn_cast<ConstantInt>(C)) {
      movImm(R, VT.I1 ? CI->getZExtValue() : CI->getSExtValue(), VT.Size);
    } else if (isa<ConstantPointerNull>(C) || isa<UndefValue>(C)) {
      zeroGP(R);
    } else if (auto *GV = dyn_cast<GlobalValue>(C)) {
      globalAddr(GV, 0, R);
    } else if (auto *CE = dyn_cast<ConstantExpr>(C);
               CE && CE->getOpcode() == Instruction::IntToPtr &&
               isa<ConstantInt>(CE->getOperand(0))) {
      movImm(R, cast<ConstantInt>(CE->getOperand(0))->getZExtValue(), 8);
    } else if (C->getType()->isPointerTy()) {
      APInt Off(64, 0);
      const Value *Base = C->stripAndAccumulateConstantOffsets(
          DL, Off, /*AllowNonInbounds=*/true);
      if (auto *GV = dyn_cast<GlobalValue>(Base))
        globalAddr(GV, Off.getSExtValue(), R);
      else
        fail("constant expression");
    } else {
      fail("constant");
    }
    return R;
  }

  // Get V into a register owned by the current instruction.
  uint8_t use(const Value *V) {
    ZExtKnown = false;
    if (auto *C = dyn_cast<Constant>(V))
      return constant(C, NoReg);
    ValTy VT = typeOf(V);
    // Vals may grow below, so VI is not used after a call that can insert.
    ValInfo &VI = Vals[V];
    if (VI.Kind == ValInfo::Alloca) {
      Mem M = frame(VI.Slot);
      uint8_t R = allocGP();
      opRM(X86::LEA64r, GR64[R], M);
      return R;
    }
    if (VI.Kind == ValInfo::DeferredCmp) {
      fail("deferred compare used as a value");
      return RAX;
    }
    if (VI.Kind == ValInfo::DeferredGEP) {
      uint8_t Tmp;
      Mem M = addr(V, Tmp);
      uint8_t R = Tmp != NoReg ? Tmp : allocGP();
      opRM(X86::LEA64r, GR64[R], M);
      return R;
    }
    if (VI.Kind == ValInfo::DeferredLoad) {
      uint8_t Tmp;
      Mem M = addr(cast<LoadInst>(V)->getPointerOperand(), Tmp);
      uint8_t R = Tmp != NoReg ? Tmp : allocGP();
      loadMem(R, VT, M);
      ZExtKnown = true;
      return R;
    }
    if (VI.Reg != NoReg) {
      uint8_t R = VI.Reg;
      VI.Reg = NoReg;
      Owner[R] = Busy;
      Clean[R] = false;
      ZExtKnown = isa<LoadInst>(V) || isa<ZExtInst>(V);
      return R;
    }
    int32_t Slot = slotFor(VI);
    uint8_t R = VT.FP ? allocFP() : allocGP();
    loadMem(R, VT, frame(Slot));
    ZExtKnown = !VT.I1;
    return R;
  }
  // Get V into R, which the caller has claimed.
  void useInto(const Value *V, uint8_t R) {
    ZExtKnown = false;
    if (auto *C = dyn_cast<Constant>(V)) {
      constant(C, R);
      return;
    }
    ValTy VT = typeOf(V);
    if ((R >= XMM0) != VT.FP)
      return fail("register class mismatch");
    ValInfo &VI = Vals[V];
    if (VI.Kind == ValInfo::Alloca)
      return opRM(X86::LEA64r, GR64[R], frame(VI.Slot));
    if (VI.Kind == ValInfo::DeferredCmp)
      return fail("deferred compare used as a value");
    if (VI.Kind == ValInfo::DeferredGEP) {
      uint8_t Tmp;
      Mem M = addr(V, Tmp);
      opRM(X86::LEA64r, GR64[R], M);
      return release(Tmp);
    }
    if (VI.Kind == ValInfo::DeferredLoad) {
      uint8_t Tmp;
      Mem M = addr(cast<LoadInst>(V)->getPointerOperand(), Tmp);
      loadMem(R, VT, M);
      ZExtKnown = true;
      return release(Tmp);
    }
    if (VI.Reg != NoReg) {
      regMove(R, VI.Reg);
      if (!Clean[VI.Reg]) {
        Owner[VI.Reg] = nullptr;
        VI.Reg = NoReg;
      }
      ZExtKnown = isa<LoadInst>(V) || isa<ZExtInst>(V);
      return;
    }
    loadMem(R, VT, frame(slotFor(VI)));
    ZExtKnown = !VT.I1;
  }
  // Like claim + useInto, but keeps V in place if it already lives in R.
  void useIn(const Value *V, uint8_t R) {
    if (Owner[R] && Owner[R] == V) {
      Vals[V].Reg = NoReg;
      Owner[R] = Busy;
      Clean[R] = false;
      ZExtKnown = isa<LoadInst>(V) || isa<ZExtInst>(V);
      return;
    }
    claim(R);
    useInto(V, R);
  }
  static bool isTemp(const Value &V, const BasicBlock *BB) {
    if (!V.hasOneUse())
      return false;
    auto *U = dyn_cast<Instruction>(V.user_back());
    if (!U)
      return false;
    // A PHI input stays in its register until the edge is emitted, which
    // happens exactly once for an unconditional branch.
    if (auto *PN = dyn_cast<PHINode>(U))
      return PN->getIncomingBlock(*V.use_begin()) == BB &&
             isa<UncondBrInst>(BB->getTerminator());
    return U->getParent() == BB;
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
  // Record the result of I, held in R by the current instruction.
  void def(const Instruction &I, uint8_t R) {
    if (I.use_empty()) {
      release(R);
    } else if (isTemp(I, I.getParent())) {
      Owner[R] = &I;
      Vals[&I].Reg = R;
    } else {
      storeAndCache(&I, typeOf(&I), R);
    }
  }
  // Free registers of single-use operands of an instruction that emits nothing.
  void dropOperands(const Instruction &I) {
    for (const Value *V : I.operands()) {
      auto It = Vals.find(V);
      if (It == Vals.end())
        continue;
      // A deferred address or load holds its operands' registers.
      if (It->second.Kind == ValInfo::DeferredGEP ||
          It->second.Kind == ValInfo::DeferredLoad) {
        dropOperands(*cast<Instruction>(V));
        continue;
      }
      if (It->second.Reg == NoReg)
        continue;
      release(It->second.Reg);
      It->second.Reg = NoReg;
    }
  }

  // Extensions of a narrow value held in a general-purpose register.
  void zext32(uint8_t R, ValTy VT) {
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
  // Zero-extend unless the last use() already produced an extended value.
  void zext32Use(uint8_t R, ValTy VT) {
    if (!ZExtKnown)
      zext32(R, VT);
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
    auto It = Vals.find(P);
    if (It == Vals.end())
      return false;
    if (It->second.Kind == ValInfo::Alloca)
      return true;
    return It->second.Kind == ValInfo::DeferredGEP &&
           isFixedAddress(cast<GetElementPtrInst>(P)->getPointerOperand());
  }

  // The RBP offset of a frame object or a constant offset from one, if it fits
  // a displacement. Unlike addr(), this never fails the function.
  bool frameOffset(const Value *P, int64_t &Off) {
    auto It = Vals.find(P);
    if (It == Vals.end())
      return false;
    if (It->second.Kind == ValInfo::Alloca) {
      Off = It->second.Slot;
      return true;
    }
    auto *GEP = dyn_cast<GetElementPtrInst>(P);
    APInt GEPOff(64, 0);
    if (It->second.Kind != ValInfo::DeferredGEP || !GEP ||
        !frameOffset(GEP->getPointerOperand(), Off) ||
        !GEP->accumulateConstantOffset(DL, GEPOff))
      return false;
    Off += GEPOff.getSExtValue();
    return isInt<32>(Off);
  }

  // Memory operand for a pointer. Tmp receives a register to release, if any.
  Mem addr(const Value *P, uint8_t &Tmp) {
    Tmp = NoReg;
    // Address spaces 256-258 are segment-relative.
    if (P->getType()->getPointerAddressSpace() != 0) {
      fail("address space");
      return frame(0);
    }
    if (auto It = Vals.find(P); It != Vals.end()) {
      if (It->second.Kind == ValInfo::Alloca)
        return frame(It->second.Slot);
      if (It->second.Kind == ValInfo::DeferredGEP) {
        auto *GEP = cast<GetElementPtrInst>(P);
        APInt Off(64, 0);
        GEP->accumulateConstantOffset(DL, Off);
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

  // A struct of one or two scalars is kept in a 16-byte frame slot, one
  // 8-byte part per element, and is returned in RAX:RDX / XMM0:XMM1.
  struct AggPart {
    ValTy VT;
    uint64_t MemOff; // offset of the element in the in-memory struct
    uint8_t RetReg;
  };
  bool aggParts(Type *T, SmallVectorImpl<AggPart> &Parts) {
    auto *STy = dyn_cast<StructType>(T);
    if (!STy || STy->getNumElements() == 0 || STy->getNumElements() > 2)
      return false;
    const StructLayout *SL = DL.getStructLayout(STy);
    unsigned NumGP = 0, NumFP = 0;
    for (unsigned I = 0; I != STy->getNumElements(); ++I) {
      AggPart P;
      if (!classify(STy->getElementType(I), P.VT))
        return false;
      P.MemOff = SL->getElementOffset(I);
      P.RetReg = P.VT.FP ? XMM0 + NumFP++ : (NumGP++ ? RDX : RAX);
      Parts.push_back(P);
    }
    return true;
  }
  int32_t aggSlotFor(const Value *V) {
    ValInfo &VI = Vals[V];
    if (!VI.Slot) {
      FrameSize += 16;
      VI.Slot = -int32_t(FrameSize);
    }
    return VI.Slot;
  }
  void compileExtractValue(const ExtractValueInst &I);
  void compileInsertValue(const InsertValueInst &I);

  bool prologue();
  void edge(const BasicBlock *Pred, const BasicBlock *Succ);
  X86::CondCode emitICmp(const ICmpInst &I);
  void emitCall(const Instruction &I, const Value *Callee, const char *LibName,
                ArrayRef<const Value *> Args, const CallBase *Attrs,
                bool IsVarArg, bool MayThrow);
  void compileInst(const Instruction &I);
  void compileAlloca(const AllocaInst &I);
  bool canFoldLoad(const LoadInst &I, ValTy VT);
  void compileLoad(const LoadInst &I);
  void compileStore(const StoreInst &I);
  void compileGEP(const GetElementPtrInst &I);
  void compileBinary(const BinaryOperator &I);
  void compileICmp(const ICmpInst &I);
  void compileFCmp(const FCmpInst &I);
  void compileSelect(const SelectInst &I);
  void compileCast(const CastInst &I);
  void compileFNeg(const UnaryOperator &I);
  bool checkCall(const CallBase &CB);
  void compileCall(const CallInst &I);
  void compileIntrinsic(const IntrinsicInst &I);
  void compileCondBr(const CondBrInst &I);
  void compileSwitch(const SwitchInst &I);
  void compileRet(const ReturnInst &I);
};
} // namespace

static bool hasPhis(const BasicBlock *BB) { return isa<PHINode>(BB->front()); }

bool Compiler::prologue() {
  unsigned DwarfRBP = G.DwarfRBP;
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
      ValInfo &VI = Vals[&A];
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
      Vals[&A].Slot = StackOff;
      StackOff += 8;
      continue;
    }
    uint8_t R = VT.FP ? XMM0 + NumFP++ : ArgGP[NumGP++];
    if (&A == SRetArg || !A.use_empty()) {
      if (&A != SRetArg && isTemp(A, Entry)) {
        Owner[R] = &A;
        Vals[&A].Reg = R;
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

void Compiler::edge(const BasicBlock *Pred, const BasicBlock *Succ) {
  if (!hasPhis(Succ))
    return;
  bool Hazard = false;
  for (const PHINode &PN : Succ->phis()) {
    auto *In = dyn_cast<PHINode>(PN.getIncomingValueForBlock(Pred));
    if (In && In != &PN && In->getParent() == Succ)
      Hazard = true;
  }
  SmallVector<std::pair<uint8_t, const PHINode *>, 8> Pending;
  for (const PHINode &PN : Succ->phis()) {
    const Value *V = PN.getIncomingValueForBlock(Pred);
    if (V == &PN || (isa<UndefValue>(V) && !Hazard))
      continue;
    ValTy VT = typeOf(&PN);
    if (FailReason)
      return;
    if (Hazard) {
      Pending.emplace_back(use(V), &PN);
      continue;
    }
    int32_t Slot = slotFor(&PN);
    auto *CI = dyn_cast<ConstantInt>(V);
    if (CI && isInt<32>(CI->getSExtValue())) {
      opMI(X86::MOV64mi32, frame(Slot),
           VT.I1 ? CI->getZExtValue() : CI->getSExtValue());
      continue;
    }
    uint8_t R = use(V);
    storeSlot(Slot, VT, R);
    release(R);
  }
  for (auto [R, PN] : Pending) {
    storeSlot(slotFor(PN), typeOf(PN), R);
    release(R);
  }
}

X86::CondCode Compiler::emitICmp(const ICmpInst &I) {
  const Value *L = I.getOperand(0), *Rhs = I.getOperand(1);
  ValTy VT = typeOf(L);
  if (VT.FP) {
    fail("icmp type");
    return X86::COND_E;
  }
  bool Signed = I.isSigned(), W = VT.Size == 8;
  // A folded load compares in memory at its own width.
  if (auto It = Vals.find(L);
      It != Vals.end() && It->second.Kind == ValInfo::DeferredLoad) {
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

void Compiler::compileICmp(const ICmpInst &I) {
  if (I.getType()->isVectorTy())
    return fail("vector compare");
  if (I.hasOneUse()) {
    auto *Br = dyn_cast<CondBrInst>(I.user_back());
    if (Br && Br->getParent() == I.getParent()) {
      Vals[&I].Kind = ValInfo::DeferredCmp;
      return;
    }
  }
  X86::CondCode CC = emitICmp(I);
  uint8_t R = allocGP();
  opRI(X86::SETCCr, GR8[R], CC);
  def(I, R);
}

void Compiler::compileFCmp(const FCmpInst &I) {
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

void Compiler::compileAlloca(const AllocaInst &I) {
  if (!I.isStaticAlloca() || I.getAlign() > Align(16))
    return fail("dynamic or over-aligned alloca");
  std::optional<TypeSize> Size = I.getAllocationSize(DL);
  if (!Size || Size->isScalable() || Size->getFixedValue() > (1u << 28))
    return fail("alloca size");
  uint64_t A = std::max<uint64_t>(I.getAlign().value(), 8);
  FrameSize =
      alignTo(FrameSize + std::max<uint64_t>(Size->getFixedValue(), 1), A);
  ValInfo &VI = Vals[&I];
  VI.Kind = ValInfo::Alloca;
  VI.Slot = -int32_t(FrameSize);
}

// Whether the load can be folded into its user as a memory operand. The user
// must be the next instruction, so no store comes in between.
bool Compiler::canFoldLoad(const LoadInst &I, ValTy VT) {
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

void Compiler::compileLoad(const LoadInst &I) {
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
    Vals[&I].Kind = ValInfo::DeferredLoad;
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

void Compiler::compileStore(const StoreInst &I) {
  const Value *V = I.getValueOperand();
  ValTy VT;
  SmallVector<AggPart, 2> Parts;
  if (!I.isAtomic() && aggParts(V->getType(), Parts)) {
    auto It = Vals.find(V);
    if (It == Vals.end() || !It->second.Slot)
      return fail("aggregate store value");
    int32_t Slot = It->second.Slot;
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

void Compiler::compileGEP(const GetElementPtrInst &I) {
  if (I.getType()->isVectorTy())
    return fail("vector gep");
  if (I.hasAllConstantIndices()) {
    // An address relative to RBP or RIP can be rebuilt at each use, unless a
    // block laid out earlier already expects the value in its slot.
    auto It = Vals.find(&I);
    if (isFixedAddress(I.getPointerOperand()) &&
        (It == Vals.end() || !It->second.Slot)) {
      Vals[&I].Kind = ValInfo::DeferredGEP;
      return;
    }
    // Otherwise the base register is consumed by the single use.
    if (I.hasOneUse()) {
      auto *U = cast<Instruction>(I.user_back());
      auto *SI = dyn_cast<StoreInst>(U);
      if (U->getParent() == I.getParent() &&
          (isa<LoadInst>(U) || (SI && SI->getValueOperand() != &I))) {
        Vals[&I].Kind = ValInfo::DeferredGEP;
        return;
      }
    }
  }
  const Value *Base = I.getPointerOperand();
  uint8_t R = NoReg;
  int64_t Disp = 0;
  if (auto It = Vals.find(Base);
      It != Vals.end() && It->second.Kind == ValInfo::Alloca)
    Disp = It->second.Slot;
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

void Compiler::compileBinary(const BinaryOperator &I) {
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

void Compiler::compileSelect(const SelectInst &I) {
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

void Compiler::compileCast(const CastInst &I) {
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
    if (auto It = Vals.find(Op);
        It != Vals.end() && It->second.Kind == ValInfo::DeferredLoad) {
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

void Compiler::compileExtractValue(const ExtractValueInst &I) {
  SmallVector<AggPart, 2> Parts;
  const Value *Agg = I.getAggregateOperand();
  auto It = Vals.find(Agg);
  if (I.getNumIndices() != 1 || !aggParts(Agg->getType(), Parts) ||
      It == Vals.end() || !It->second.Slot)
    return fail("extractvalue");
  unsigned Idx = I.getIndices()[0];
  ValTy VT = Parts[Idx].VT;
  int32_t Slot = It->second.Slot;
  uint8_t R = VT.FP ? allocFP() : allocGP();
  loadMem(R, VT, frame(Slot + 8 * Idx));
  def(I, R);
}

void Compiler::compileInsertValue(const InsertValueInst &I) {
  SmallVector<AggPart, 2> Parts;
  const Value *Agg = I.getAggregateOperand();
  if (I.getNumIndices() != 1 || !aggParts(I.getType(), Parts))
    return fail("insertvalue");
  unsigned Idx = I.getIndices()[0];
  int32_t Slot = aggSlotFor(&I);
  if (!isa<UndefValue>(Agg)) {
    auto It = Vals.find(Agg);
    if (It == Vals.end() || !It->second.Slot)
      return fail("insertvalue aggregate");
    int32_t Src = It->second.Slot;
    uint8_t T = allocGP();
    for (unsigned P = 0; P != Parts.size(); ++P) {
      if (P == Idx)
        continue;
      opRM(X86::MOV64rm, GR64[T], frame(Src + 8 * P));
      opMR(X86::MOV64mr, frame(Slot + 8 * P), GR64[T]);
    }
    release(T);
  }
  uint8_t R = use(I.getInsertedValueOperand());
  storeSlot(Slot + 8 * Idx, Parts[Idx].VT, R);
  release(R);
}

void Compiler::compileFNeg(const UnaryOperator &I) {
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
void Compiler::emitCallTo(const Function *Direct, const char *LibName,
                          bool MayThrow) {
  if (Direct &&
      (Direct->isAbsoluteSymbolRef() || AP.TM.isLargeGlobalValue(Direct)))
    return fail("global reference");
  const MCSymbol *Sym;
  if (LibName) {
    MCSymbol *&S = G.LibSyms[LibName];
    if (!S)
      S = Ctx.getOrCreateSymbol(LibName);
    Sym = S;
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
}

uint32_t Compiler::actionFor(const LandingPadInst &LPI) {
  auto It = PadAction.find(&LPI);
  if (It != PadAction.end())
    return It->second;
  // A cleanup-only landing pad needs no action record. Otherwise the records
  // are laid out in clause order, each linking to the next one, with the
  // cleanup last.
  uint32_t First = 0;
  if (LPI.getNumClauses()) {
    First = EH.Actions.size() + 1;
    for (unsigned C = 0, E = LPI.getNumClauses(); C != E; ++C) {
      const Value *TI = LPI.getClause(C)->stripPointerCasts();
      auto *GV = dyn_cast<GlobalValue>(TI);
      if (!LPI.isCatch(C) || (!GV && !isa<ConstantPointerNull>(TI))) {
        fail("landing pad clause");
        return 0;
      }
      appendSLEB(typeIdFor(GV));
      appendSLEB(C + 1 == E && !LPI.isCleanup() ? 0 : 1);
    }
    if (LPI.isCleanup()) {
      appendSLEB(0);
      appendSLEB(0);
    }
  }
  PadAction[&LPI] = First;
  return First;
}

void Compiler::compileInvoke(const InvokeInst &I) {
  if (!G.attrsHandled(I.getAttributes()))
    return failAttrs("call attribute", I.getAttributes());
  if (const Function *Callee = I.getCalledFunction();
      Callee && Callee->isIntrinsic())
    return fail("call kind");
  if (!checkCall(I))
    return;
  uint32_t Action = actionFor(*I.getLandingPadInst());
  SmallVector<const Value *, 8> Args(I.args());
  emitCall(I, I.getCalledOperand()->stripPointerCasts(), nullptr, Args, &I,
           I.getFunctionType()->isVarArg(), /*MayThrow=*/true);
  if (FailReason)
    return;
  size_t CS = EH.CallSites.size() - 1;
  const BasicBlock *BB = I.getParent(), *Unwind = I.getUnwindDest();
  // The exception pointer and selector arrive in RAX and RDX. If the landing
  // pad has PHIs, the moves go in a stub that keeps both registers intact.
  bool Stub = hasPhis(Unwind);
  edge(BB, I.getNormalDest());
  jmpTo(I.getNormalDest(), /*Force=*/Stub);
  EH.CallSites[CS].Action = Action;
  if (Stub) {
    EH.CallSites[CS].LandingPad = Code.size();
    claim(RAX);
    claim(RDX);
    edge(BB, Unwind);
    release(RAX);
    release(RDX);
    jmpTo(Unwind, /*Force=*/true);
  } else {
    CallSiteLPad[CS] = Unwind;
  }
  HasLandingPads = true;
}

void Compiler::compileLandingPad(const LandingPadInst &I) {
  SmallVector<AggPart, 2> Parts;
  if (!aggParts(I.getType(), Parts) || Parts.size() != 2 || Parts[0].VT.FP ||
      Parts[1].VT.FP)
    return fail("landing pad type");
  int32_t Slot = aggSlotFor(&I);
  opMR(X86::MOV64mr, frame(Slot), X86::RAX);
  opMR(X86::MOV64mr, frame(Slot + 8), X86::RDX);
}

void Compiler::compileResume(const ResumeInst &I) {
  auto It = Vals.find(I.getValue());
  if (It == Vals.end() || !It->second.Slot)
    return fail("resume value");
  claim(RDI);
  opRM(X86::MOV64rm, X86::RDI, frame(It->second.Slot));
  emitCallTo(nullptr, "_Unwind_Resume", /*MayThrow=*/true);
  release(RDI);
  // As for the unreachable that DwarfEHPrepare puts after the noreturn call.
  const TargetOptions &O = AP.TM.Options;
  if (O.TrapUnreachable && !O.NoTrapAfterNoreturn)
    op0(X86::TRAP);
}

void Compiler::emitCall(const Instruction &I, const Value *Callee,
                        const char *LibName, ArrayRef<const Value *> Args,
                        const CallBase *Attrs, bool IsVarArg, bool MayThrow) {
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
    auto It = Vals.find(Callee);
    if (It != Vals.end() && It->second.Reg != NoReg) {
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

bool Compiler::checkCall(const CallBase &CB) {
  if (CB.isInlineAsm() || CB.hasOperandBundles() ||
      CB.getCallingConv() != CallingConv::C) {
    fail("call kind");
    return false;
  }
  // The callee's own attributes matter too: an ABI attribute may appear only
  // on the declaration, and "dontcall-*" is diagnosed by instruction selection.
  auto *CalleeF =
      dyn_cast<Function>(CB.getCalledOperand()->stripPointerCasts());
  if (!CalleeF)
    return true;
  if (!G.paramAttrsHandled(CalleeF->getAttributes())) {
    failAttrs("call attribute", CalleeF->getAttributes());
    return false;
  }
  if (CalleeF->hasFnAttribute("dontcall-error") ||
      CalleeF->hasFnAttribute("dontcall-warn")) {
    fail("dontcall callee");
    return false;
  }
  return true;
}

void Compiler::compileCall(const CallInst &I) {
  // Call-site attributes such as "trap-func-name" also apply to intrinsics.
  if (!G.attrsHandled(I.getAttributes()))
    return failAttrs("call attribute", I.getAttributes());
  if (auto *II = dyn_cast<IntrinsicInst>(&I)) {
    if (I.hasOperandBundles() && II->getIntrinsicID() != Intrinsic::assume)
      return fail("call kind");
    return compileIntrinsic(*II);
  }
  if (I.isMustTailCall())
    return fail("call kind");
  if (!checkCall(I))
    return;
  SmallVector<const Value *, 8> Args(I.args());
  emitCall(I, I.getCalledOperand()->stripPointerCasts(), nullptr, Args, &I,
           I.getFunctionType()->isVarArg(), !I.doesNotThrow());
}

void Compiler::compileIntrinsic(const IntrinsicInst &I) {
  switch (I.getIntrinsicID()) {
  case Intrinsic::lifetime_start:
  case Intrinsic::lifetime_end:
  case Intrinsic::donothing:
  case Intrinsic::assume:
  case Intrinsic::experimental_noalias_scope_decl:
    return dropOperands(I);
  case Intrinsic::memcpy:
  case Intrinsic::memmove:
  case Intrinsic::memset: {
    ValTy LT;
    if (!classify(I.getArgOperand(2)->getType(), LT) || LT.Size != 8)
      return fail("mem intrinsic length type");
    const char *Name = I.getIntrinsicID() == Intrinsic::memcpy    ? "memcpy"
                       : I.getIntrinsicID() == Intrinsic::memmove ? "memmove"
                                                                  : "memset";
    const Value *Args[] = {I.getArgOperand(0), I.getArgOperand(1),
                           I.getArgOperand(2)};
    return emitCall(I, nullptr, Name, Args, nullptr, false,
                    /*MayThrow=*/false);
  }
  case Intrinsic::expect:
    return def(I, use(I.getArgOperand(0)));
  case Intrinsic::eh_typeid_for: {
    const Value *TI = I.getArgOperand(0)->stripPointerCasts();
    auto *GV = dyn_cast<GlobalValue>(TI);
    if (!GV && !isa<ConstantPointerNull>(TI))
      return fail("eh.typeid.for");
    uint8_t R = allocGP();
    movImm(R, typeIdFor(GV), 4);
    return def(I, R);
  }
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
  case Intrinsic::vaend:
    return dropOperands(I);
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
  case Intrinsic::trap:
    return op0(X86::TRAP);
  case Intrinsic::debugtrap:
    return op0(X86::INT3);
  default:
    return fail("intrinsic");
  }
}

void Compiler::compileCondBr(const CondBrInst &I) {
  const BasicBlock *BB = I.getParent();
  const BasicBlock *T = I.getSuccessor(0), *E = I.getSuccessor(1);
  const Value *C = I.getCondition();
  X86::CondCode CC = X86::COND_NE;
  auto It = Vals.find(C);
  if (It != Vals.end() && It->second.Kind == ValInfo::DeferredCmp) {
    CC = emitICmp(*cast<ICmpInst>(C));
  } else if (auto *CI = dyn_cast<ConstantInt>(C)) {
    const BasicBlock *Dest = CI->isZero() ? E : T;
    edge(BB, Dest);
    return jmpTo(Dest);
  } else {
    uint8_t R = use(C);
    opRI(X86::TEST8ri, GR8[R], 1);
    release(R);
  }
  if (T == E) {
    edge(BB, T);
    return jmpTo(T);
  }
  if (T == NextBB && !hasPhis(E)) {
    jccTo(X86::GetOppositeBranchCondition(CC), E);
    edge(BB, T);
  } else if (!hasPhis(T)) {
    jccTo(CC, T);
    edge(BB, E);
    jmpTo(E);
  } else if (!hasPhis(E)) {
    jccTo(X86::GetOppositeBranchCondition(CC), E);
    edge(BB, T);
    jmpTo(T);
  } else {
    size_t Pos = jccFwd(X86::GetOppositeBranchCondition(CC));
    edge(BB, T);
    jmpTo(T, /*Force=*/true);
    bindFwd(Pos);
    edge(BB, E);
    jmpTo(E);
  }
}

void Compiler::compileSwitch(const SwitchInst &I) {
  const BasicBlock *BB = I.getParent();
  ValTy VT;
  if (!classify(I.getCondition()->getType(), VT) || VT.FP)
    return fail("switch type");
  bool W = VT.Size == 8;
  uint8_t R = use(I.getCondition());
  if (VT.Size < 4)
    zext32Use(R, VT);
  for (const auto &C : I.cases()) {
    int64_t V = W ? C.getCaseValue()->getSExtValue()
                  : int32_t(C.getCaseValue()->getZExtValue());
    if (!W) {
      opRI(X86::CMP32ri, GR32[R], V);
    } else if (isInt<32>(V)) {
      opRI(X86::CMP64ri32, GR64[R], V);
    } else {
      uint8_t T = allocGP();
      opRI(X86::MOV64ri, GR64[T], V);
      opRR(X86::CMP64rr, GR64[R], GR64[T]);
      release(T);
    }
    const BasicBlock *Succ = C.getCaseSuccessor();
    if (!hasPhis(Succ)) {
      jccTo(X86::COND_E, Succ);
    } else {
      size_t Pos = jccFwd(X86::COND_NE);
      edge(BB, Succ);
      jmpTo(Succ, /*Force=*/true);
      bindFwd(Pos);
    }
    if (FailReason)
      return;
  }
  release(R);
  edge(BB, I.getDefaultDest());
  jmpTo(I.getDefaultDest());
}

void Compiler::compileRet(const ReturnInst &I) {
  uint8_t R = NoReg;
  SmallVector<AggPart, 2> Parts;
  if (const Value *V = I.getReturnValue(); V && aggParts(V->getType(), Parts)) {
    auto It = Vals.find(V);
    if (It == Vals.end() || !It->second.Slot)
      return fail("aggregate return value");
    int32_t Slot = It->second.Slot;
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
  unsigned DwarfRSP = G.DwarfRSP, DwarfRBP = G.DwarfRBP;
  EpilogueStart = Code.size();
  opRR(X86::MOV64rr, X86::RSP, X86::RBP);
  opR(X86::POP64r, X86::RBP);
  CFI.emplace_back(Code.size(),
                   MCCFIInstruction::cfiDefCfa(nullptr, DwarfRSP, 8));
  op0(X86::RET64);
  CFI.emplace_back(Code.size(),
                   MCCFIInstruction::cfiDefCfa(nullptr, DwarfRBP, 16));
}

void Compiler::compileInst(const Instruction &I) {
  if (I.hasMetadataOtherThanDebugLoc()) {
    SmallVector<std::pair<unsigned, MDNode *>, 4> MDs;
    I.getAllMetadataOtherThanDebugLoc(MDs);
    for (const auto &MD : MDs)
      if (!isHandledMetadata(MD.first) && MD.first != G.HeapAllocSiteKind)
        return fail("instruction metadata");
  }
  // Edge code may run on only some paths, so a terminator starts with no
  // clean copies.
  if (I.isTerminator() && !isa<InvokeInst>(I))
    dropClean();
  // Do not execute a dead load or division: the regular pipeline drops them.
  // An unused alloca emits nothing but may still hold a debug variable.
  if (I.use_empty() && !I.getType()->isVoidTy() && !I.isTerminator() &&
      !isa<AllocaInst>(I) && wouldInstructionBeTriviallyDead(&I))
    return dropOperands(I);
  switch (I.getOpcode()) {
  case Instruction::Alloca:
    return compileAlloca(cast<AllocaInst>(I));
  case Instruction::Load:
    return compileLoad(cast<LoadInst>(I));
  case Instruction::Store:
    return compileStore(cast<StoreInst>(I));
  case Instruction::GetElementPtr:
    return compileGEP(cast<GetElementPtrInst>(I));
  case Instruction::ICmp:
    return compileICmp(cast<ICmpInst>(I));
  case Instruction::FCmp:
    return compileFCmp(cast<FCmpInst>(I));
  case Instruction::Select:
    return compileSelect(cast<SelectInst>(I));
  case Instruction::FNeg:
    return compileFNeg(cast<UnaryOperator>(I));
  case Instruction::Freeze:
    return def(I, use(I.getOperand(0)));
  case Instruction::ExtractValue:
    return compileExtractValue(cast<ExtractValueInst>(I));
  case Instruction::InsertValue:
    return compileInsertValue(cast<InsertValueInst>(I));
  case Instruction::Call:
    return compileCall(cast<CallInst>(I));
  case Instruction::Invoke:
    return compileInvoke(cast<InvokeInst>(I));
  case Instruction::LandingPad:
    return compileLandingPad(cast<LandingPadInst>(I));
  case Instruction::Resume:
    return compileResume(cast<ResumeInst>(I));
  case Instruction::PHI:
    return;
  case Instruction::UncondBr: {
    const BasicBlock *Succ = cast<UncondBrInst>(I).getSuccessor();
    edge(I.getParent(), Succ);
    // As FastISel, keep a branch that is alone in its block, so that its line
    // has a location.
    return jmpTo(Succ, /*Force=*/&I.getParent()->front() == &I);
  }
  case Instruction::CondBr:
    return compileCondBr(cast<CondBrInst>(I));
  case Instruction::Switch:
    return compileSwitch(cast<SwitchInst>(I));
  case Instruction::Ret:
    return compileRet(cast<ReturnInst>(I));
  case Instruction::Unreachable:
    if (cast<UnreachableInst>(I).shouldLowerToTrap(
            AP.TM.Options.TrapUnreachable, AP.TM.Options.NoTrapAfterNoreturn))
      op0(X86::TRAP);
    return;
  default:
    if (auto *BO = dyn_cast<BinaryOperator>(&I))
      return compileBinary(*BO);
    if (auto *CI = dyn_cast<CastInst>(&I))
      return compileCast(*CI);
    return fail(I.getOpcodeName());
  }
}

// Resolve branch targets, shrinking rel32 jumps whose target is within rel8
// range. Removing bytes only brings targets closer, so deciding with the
// original offsets is safe.
void Compiler::relaxBranches() {
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
  if (Removed.empty())
    return;
  for (auto &Row : Debug.Rows)
    Row.Offset = NewOff(Row.Offset);
  for (MCFixup &Fixup : Fixups)
    Fixup.setOffset(NewOff(Fixup.getOffset()));
  for (auto &Entry : CFI)
    Entry.first = NewOff(Entry.first);
}

bool Compiler::run() {
  if (F.hasPersonalityFn()) {
    EHPersonality Per = classifyEHPersonality(F.getPersonalityFn());
    if ((Per != EHPersonality::GNU_CXX && Per != EHPersonality::GNU_C) ||
        !F.needsUnwindTableEntry() ||
        AP.getModuleExceptionModel() != ExceptionHandling::DwarfCFI) {
      fail("personality");
      return false;
    }
  }
  if (F.getCallingConv() != CallingConv::C || F.hasGC() || F.hasPrefixData() ||
      F.hasPrologueData()) {
    fail("function kind");
    return false;
  }
  const DISubprogram *SP = F.getSubprogram();
  if (SP && SP->getUnit()->getEmissionKind() != DICompileUnit::NoDebug) {
    HasDebug = RecordDebug = true;
  } else if (AP.hasDebugInfo()) {
    // The debug handlers must see every function to terminate line sequences,
    // so this one gets an empty description.
    HasDebug = true;
  }
  if (!G.attrsHandled(F.getAttributes())) {
    fail("function attribute");
    return false;
  }
  if (F.hasMetadata()) {
    SmallVector<std::pair<unsigned, MDNode *>, 4> MDs;
    F.getAllMetadata(MDs);
    for (const auto &MD : MDs)
      if (MD.first != LLVMContext::MD_dbg && !isHandledMetadata(MD.first)) {
        fail("function metadata");
        return false;
      }
  }

  if (!prologue())
    return false;

  // Skip blocks that are unreachable from the entry.
  BitVector Reachable(F.getMaxBlockNumber());
  SmallVector<const BasicBlock *, 32> Worklist{&F.getEntryBlock()};
  Reachable.set(F.getEntryBlock().getNumber());
  while (!Worklist.empty())
    for (const BasicBlock *Succ : successors(Worklist.pop_back_val()))
      if (!Reachable.test(Succ->getNumber())) {
        Reachable.set(Succ->getNumber());
        Worklist.push_back(Succ);
      }
  SmallVector<const BasicBlock *, 32> Order;
  for (const BasicBlock &BB : F) {
    // The AsmPrinter emits labels for address-taken blocks, even unreachable
    // ones.
    if (BB.hasAddressTaken()) {
      fail("address-taken block");
      return false;
    }
    if (Reachable.test(BB.getNumber()))
      Order.push_back(&BB);
  }

  BlockOff.assign(F.getMaxBlockNumber(), 0);
  for (unsigned Idx = 0; Idx != Order.size(); ++Idx) {
    const BasicBlock *BB = Order[Idx];
    NextBB = Idx + 1 != Order.size() ? Order[Idx + 1] : nullptr;
    BlockOff[BB->getNumber()] = Code.size();
    bool BlockStart = true;
    const DILocation *LastLoc = nullptr;
    for (const Instruction &I : *BB) {
      if (RecordDebug)
        for (const DbgRecord &DR : I.getDbgRecordRange()) {
          if (auto *DLR = dyn_cast<DbgLabelRecord>(&DR)) {
            Debug.Rows.push_back({uint32_t(Code.size()),
                                  DLR->getDebugLoc().get(), BlockStart, false,
                                  DLR->getLabel()});
            BlockStart = false;
            continue;
          }
          // A variable lives in a frame object, or behind a pointer argument
          // that has a frame slot. Code must not depend on debug info, so a
          // record that cannot be described from the code as generated is
          // dropped, as FastISel does when it cannot lower one.
          auto *DVR = dyn_cast<DbgVariableRecord>(&DR);
          if (!DVR || !DVR->isDbgDeclare() || DVR->isKillLocation()) {
            ++NumDroppedDbg;
            continue;
          }
          const Value *Addr = DVR->getAddress();
          auto It = Vals.find(Addr);
          auto *Arg = dyn_cast<Argument>(Addr);
          int64_t Off;
          uint64_t Size = 8;
          if (It != Vals.end() && It->second.Kind == ValInfo::Alloca) {
            Off = It->second.Slot;
            Size = Arg ? DL.getTypeAllocSize(Arg->getParamByValType())
                       : cast<AllocaInst>(Addr)
                             ->getAllocationSize(DL)
                             ->getFixedValue();
          } else if (Arg && It != Vals.end() &&
                     It->second.Kind == ValInfo::Normal && It->second.Slot) {
            Off = It->second.Slot;
            DerefVars.push_back(Debug.Variables.size());
          } else if (It != Vals.end() &&
                     It->second.Kind == ValInfo::DeferredGEP &&
                     isFixedAddress(Addr)) {
            // A field of a frame object.
            if (!frameOffset(Addr, Off)) {
              ++NumDroppedDbg;
              continue;
            }
            Size = std::max<uint64_t>(
                divideCeil(DVR->getVariable()->getSizeInBits().value_or(64), 8),
                1);
          } else {
            ++NumDroppedDbg;
            continue;
          }
          // A fixed frame object at offset X is at rbp + X + 16.
          Debug.Variables.push_back({DVR->getVariable(), DVR->getExpression(),
                                     DVR->getDebugLoc().get(), Off - 16, Size});
        }
      size_t Before = Code.size();
      compileInst(I);
      if (FailReason)
        return false;
      const DILocation *Loc = I.getDebugLoc().get();
      if (RecordDebug && Code.size() != Before) {
        // A return value set up before the epilogue gets its own row.
        size_t Epilogue = isa<ReturnInst>(I) ? EpilogueStart : Code.size();
        if (Epilogue != Before && (BlockStart || Loc != LastLoc)) {
          Debug.Rows.push_back(
              {uint32_t(Before), Loc, BlockStart, false, nullptr});
          BlockStart = false;
        }
        if (isa<ReturnInst>(I)) {
          Debug.Rows.push_back(
              {uint32_t(Epilogue), Loc, BlockStart, true, nullptr});
          BlockStart = false;
        }
        LastLoc = Loc;
      }
    }
    dropClean();
    for (const Value *V : Owner)
      if (V) {
        fail("value live in a register at block end");
        return false;
      }
  }

  // Keep frame offsets well inside the range of a 32-bit displacement.
  if (FrameSize + uint64_t(MaxOutgoing) > (1u << 30)) {
    fail("frame size");
    return false;
  }
  // The prologue reserved "sub rsp, imm32" (7 bytes). An empty frame, or a
  // leaf function with a small frame that fits in the red zone, needs no
  // adjustment at all; a small frame takes the imm8 form.
  unsigned Total = alignTo(FrameSize + MaxOutgoing, 16);
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
  // Built only now so that a fallback leaves no new metadata behind.
  for (unsigned Idx : DerefVars)
    Debug.Variables[Idx].Expr = DIExpression::prepend(
        Debug.Variables[Idx].Expr, DIExpression::DerefBefore);
  return true;
}

namespace {
class X86OnePassCodeGen : public FunctionPass {
  X86TargetMachine &TM;
  OnePassCodeGenOptions Opts;
  unsigned NumDone = 0;
  ModuleCache G;
  enum { Unknown, Supported, Unsupported } ModuleState = Unknown;

public:
  static char ID;
  X86OnePassCodeGen(X86TargetMachine &TM, const OnePassCodeGenOptions &Opts)
      : FunctionPass(ID), TM(TM), Opts(Opts) {}

  StringRef getPassName() const override {
    return "X86 One-Pass Code Generator";
  }
  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.setPreservesAll();
  }
  bool doInitialization(Module &M) override {
    G.AttrVerdicts.clear();
    G.ParamVerdicts.clear();
    G.Encodings.clear();
    G.Syms[0].clear();
    G.Syms[1].clear();
    G.LibSyms.clear();
    const MCRegisterInfo &MRI = TM.getMCRegisterInfo();
    G.DwarfRSP = MRI.getDwarfRegNum(X86::RSP, true);
    G.DwarfRBP = MRI.getDwarfRegNum(X86::RBP, true);
    // Look the kind up without registering it.
    SmallVector<StringRef, 64> Names;
    M.getContext().getMDKindNames(Names);
    auto It = find(Names, "heapallocsite");
    G.HeapAllocSiteKind = It == Names.end() ? ~0u : It - Names.begin();
    AP = nullptr;
    ModuleState = Unknown;
    return false;
  }
  bool runOnFunction(Function &F) override;

private:
  AsmPrinter *AP = nullptr;
};
} // namespace

char X86OnePassCodeGen::ID = 0;

// Target features and the triple select passes that would rewrite or
// instrument the code: hardening, reserved registers, sandboxing. They may come
// from -mattr as well as "target-features", so query the subtarget.
static const char *unsupportedSubtargetReason(const X86Subtarget &ST,
                                              const X86TargetMachine &TM) {
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

// Skipping the remaining passes only stops emission if the AsmPrinter is one of
// them, so it must be in the same function pass manager.
static AsmPrinter *findAsmPrinter(PMDataManager &PM) {
  if (PM.getPassManagerType() != PMT_FunctionPassManager)
    return nullptr;
  auto &FPM = static_cast<FPPassManager &>(PM);
  for (unsigned I = 0, E = FPM.getNumContainedPasses(); I != E; ++I) {
    FunctionPass *P = FPM.getContainedPass(I);
    if (P->getPassID() == &X86AsmPrinter::ID)
      return static_cast<X86AsmPrinter *>(P);
  }
  return nullptr;
}

bool X86OnePassCodeGen::runOnFunction(Function &F) {
  if (ModuleState == Unknown) {
    const char *Reason = nullptr;
    AP = findAsmPrinter(getResolver()->getPMDataManager());
    if (!AP)
      Reason = "no AsmPrinter in the function pass manager";
    else if (AP->OutStreamer->hasRawTextSupport())
      Reason = "not an object streamer";
    else
      Reason = unsupportedModuleReason(
          *F.getParent(), TM,
          static_cast<MCObjectStreamer &>(*AP->OutStreamer));
    ModuleState = Reason ? Unsupported : Supported;
    if (Reason && (Opts.Report || Opts.Abort))
      errs() << "onepass: module not supported: " << Reason << '\n';
  }
  if (ModuleState == Unsupported || F.hasAvailableExternallyLinkage() ||
      NumDone >= Opts.Limit)
    return false;
  const X86Subtarget &ST = *TM.getSubtargetImpl(F);
  auto &OS = static_cast<MCObjectStreamer &>(*AP->OutStreamer);
  Compiler C(F, ST, *AP, OS.getAssembler().getEmitter(), G);
  const char *Reason = unsupportedSubtargetReason(ST, TM);
  if (Reason || !C.run()) {
    ++NumFallback;
    if (Opts.Report || Opts.Abort)
      errs() << "onepass: " << F.getName() << ": "
             << (Reason ? Reason : C.FailReason)
             << (Reason || C.FailDetail.empty() ? "" : " " + C.FailDetail)
             << '\n';
    if (Opts.Abort)
      reportFatalUsageError("one-pass compilation failed");
    return false;
  }
  AP->emitCompiledFunction(F, {C.Code, C.Fixups, C.CFI, C.StackSize,
                               C.HasLandingPads ? &C.EH : nullptr,
                               C.HasDebug ? &C.Debug : nullptr});
  ++NumCompiled;
  ++NumDone;
  static_cast<FPPassManager &>(getResolver()->getPMDataManager())
      .skipRemainingPasses();
  return false;
}

FunctionPass *
llvm::createX86OnePassCodeGenPass(X86TargetMachine &TM,
                                  const OnePassCodeGenOptions &Opts) {
  return new X86OnePassCodeGen(TM, Opts);
}
