//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/CodeGen/DirectCodeGen.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/CodeGen/StackProtector.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/DebugProgramInstruction.h"
#include "llvm/IR/DiagnosticInfo.h"
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
#include "llvm/MC/MCExpr.h"
#include "llvm/MC/MCObjectStreamer.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/LEB128.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Transforms/Utils/Local.h"

using namespace llvm;

#define DEBUG_TYPE "direct-codegen"

STATISTIC(NumCompiled,
          "Number of functions emitted by the direct code generator");
STATISTIC(NumFallback, "Number of functions left to the regular pipeline");
STATISTIC(NumDroppedDbg,
          "Number of debug records the direct code generator drops");

// The gates below are allowlists: anything they do not know sends the function
// (or the module) to the regular pipeline.

bool DirectCodeGen::isHandledAttr(Attribute A) const {
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
                                      "no-inline-line-tables",
                                      "sample-profile-suffix-elision-policy",
                                      "warn-stack-size"};
    return is_contained(Known, K) || K.starts_with("no-builtin-") ||
           (K == "probe-stack" && A.getValueAsString() == "inline-asm");
  }
  // No default: a new attribute must be classified here.
  switch (A.getKindAsEnum()) {
  // Hints for optimizations and markers for IR instrumentation, which do not
  // change unoptimized code. Scalable types are rejected wherever they
  // appear, so vscale_range is one of them.
  case Attribute::Alignment:
  case Attribute::AllocAlign:
  case Attribute::AllocatedPointer:
  case Attribute::AllocKind:
  case Attribute::AllocSize:
  case Attribute::AlwaysInline:
  case Attribute::Builtin:
  case Attribute::Captures:
  case Attribute::Cold:
  case Attribute::CoroDestroyOnlyWhenComplete:
  case Attribute::CoroElideSafe:
  case Attribute::DeadOnReturn:
  case Attribute::DeadOnUnwind:
  case Attribute::Dereferenceable:
  case Attribute::DereferenceableOrNull:
  case Attribute::DisableSanitizerInstrumentation:
  case Attribute::Flatten:
  case Attribute::Hot:
  case Attribute::ImmArg:
  case Attribute::Initializes:
  case Attribute::InlineHint:
  case Attribute::LikelyModuleLocal:
  case Attribute::Memory:
  case Attribute::MinSize:
  case Attribute::MustProgress:
  case Attribute::NoAlias:
  case Attribute::NoBuiltin:
  case Attribute::NoCallback:
  case Attribute::NoCreateUndefOrPoison:
  case Attribute::NoDivergenceSource:
  case Attribute::NoDuplicate:
  case Attribute::NoFPClass:
  case Attribute::NoFree:
  case Attribute::NoFreeObj:
  case Attribute::NoInline:
  case Attribute::NoIPA:
  case Attribute::NoMerge:
  case Attribute::NonNull:
  case Attribute::NoOutline:
  case Attribute::NoProfile:
  case Attribute::NoRecurse:
  case Attribute::NoSanitizeBounds:
  case Attribute::NoSanitizeCoverage:
  case Attribute::NoSync:
  case Attribute::NoUndef:
  case Attribute::NullPointerIsValid:
  case Attribute::OptForFuzzing:
  case Attribute::OptimizeForDebugging:
  case Attribute::OptimizeForSize:
  case Attribute::OptimizeNone:
  case Attribute::Range:
  case Attribute::ReadNone:
  case Attribute::ReadOnly:
  case Attribute::Returned:
  case Attribute::SanitizeAddress:
  case Attribute::SanitizeAllocToken:
  case Attribute::SanitizeConcurrency:
  case Attribute::SanitizeHWAddress:
  case Attribute::SanitizeMemory:
  case Attribute::SanitizeNumericalStability:
  case Attribute::SanitizeRealtime:
  case Attribute::SanitizeRealtimeBlocking:
  case Attribute::SanitizeThread:
  case Attribute::SanitizeType:
  case Attribute::SkipProfile:
  case Attribute::Speculatable:
  case Attribute::VScaleRange:
  case Attribute::WillReturn:
  case Attribute::Writable:
  case Attribute::WriteOnly:
  // Implemented.
  case Attribute::ByVal:
  case Attribute::NoExt:
  case Attribute::NonLazyBind:
  case Attribute::NoRedZone:
  case Attribute::NoReturn:
  case Attribute::NoUnwind:
  case Attribute::ReturnsTwice:
  case Attribute::SExt:
  case Attribute::StackProtect:
  case Attribute::StackProtectReq:
  case Attribute::StackProtectStrong:
  case Attribute::StructRet:
  case Attribute::UWTable:
  case Attribute::ZExt:
    return true;
  // Left to the regular pipeline, unless the target overrides this.
  case Attribute::ByRef:
  case Attribute::Convergent:
  case Attribute::DenormalFPEnv:
  case Attribute::ElementType:
  case Attribute::FnRetThunkExtern:
  case Attribute::HybridPatchable:
  case Attribute::InAlloca:
  case Attribute::InReg:
  case Attribute::JumpTable:
  case Attribute::Naked:
  case Attribute::Nest:
  case Attribute::NoCfCheck:
  case Attribute::NoImplicitFloat:
  case Attribute::Preallocated:
  case Attribute::PresplitCoroutine:
  case Attribute::SafeStack:
  case Attribute::SanitizeMemTag:
  case Attribute::ShadowCallStack:
  case Attribute::SpeculativeLoadHardening:
  case Attribute::StackAlignment:
  case Attribute::StrictFP:
  case Attribute::SwiftAsync:
  case Attribute::SwiftError:
  case Attribute::SwiftSelf:
  // Not attributes.
  case Attribute::None:
  case Attribute::EndAttrKinds:
  case Attribute::EmptyKey:
  case Attribute::TombstoneKey:
    return false;
  }
  llvm_unreachable("covered switch");
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
  case LLVMContext::MD_func_sanitize:
    return true;
  default:
    return false;
  }
}

bool DirectCompiler::ModuleState::attrsHandled(AttributeList AL) {
  auto [It, Inserted] = AttrVerdicts.try_emplace(AL.getRawPointer(), true);
  if (Inserted)
    for (AttributeSet AS : AL)
      for (Attribute A : AS)
        It->second &= Pass->isHandledAttr(A);
  return It->second;
}

static DirectCompiler::ModuleState::CalleeVerdict
computeCalleeVerdict(const DirectCodeGen &P, AttributeList AL) {
  using MS = DirectCompiler::ModuleState;
  for (unsigned Idx : AL.indexes())
    if (Idx != AttributeList::FunctionIndex)
      for (Attribute A : AL.getAttributes(Idx))
        if (!P.isHandledAttr(A))
          return MS::CalleeParamAttr;
  for (Attribute A : AL.getFnAttrs())
    if (P.changesCalls(A))
      return MS::CalleeFnAttr;
  if (AL.hasFnAttr("dontcall-error") || AL.hasFnAttr("dontcall-warn"))
    return MS::CalleeDontCall;
  return MS::CalleeOK;
}

DirectCompiler::ModuleState::CalleeVerdict
DirectCompiler::ModuleState::calleeVerdict(AttributeList AL) {
  auto [It, Inserted] =
      CalleeVerdicts.try_emplace(AL.getRawPointer(), CalleeOK);
  if (Inserted)
    It->second = computeCalleeVerdict(*Pass, AL);
  return It->second;
}

void DirectCompiler::ModuleState::reset(const DirectCodeGen &P,
                                        const Module &M) {
  Pass = &P;
  AttrVerdicts.clear();
  CalleeVerdicts.clear();
  Encodings.clear();
  Syms[0].clear();
  Syms[1].clear();
  LibSyms.clear();
  // Look the kind up without registering it.
  SmallVector<StringRef, 64> Names;
  M.getContext().getMDKindNames(Names);
  auto It = find(Names, "heapallocsite");
  HeapAllocSiteKind = It == Names.end() ? ~0u : It - Names.begin();
}

DirectCompiler::DirectCompiler(const Function &F, const MCSubtargetInfo &STI,
                               AsmPrinter &AP, const MCCodeEmitter &CE,
                               ModuleState &G, const RegisterInfo &Regs)
    : Code(G.Code), Fixups(G.Fixups), F(F), STI(STI), DL(F.getDataLayout()),
      AP(AP), Ctx(AP.OutContext), CE(CE), G(G), BlockOff(G.BlockOff),
      Vals(G.Vals), NumArgs(F.arg_size()),
      Busy(reinterpret_cast<const Value *>(this)), Regs(Regs) {
  assert(Regs.NumRegs <= MaxRegs && "register ids do not fit");
  for (uint8_t R : Regs.GPArgs)
    ArgRegs |= uint64_t(1) << R;
  if (Regs.SRetArg != NoReg)
    ArgRegs |= uint64_t(1) << Regs.SRetArg;
  ArgRegs |= maskTrailingOnes<uint64_t>(Regs.NumFPArgs) << Regs.FirstFP;
  Code.clear();
  Fixups.clear();
  Vals.assign(NumArgs + F.getMaxInstructionNumber(), ValInfo());
  std::fill_n(Owner, Regs.NumRegs, nullptr);
}

DirectCompiler::~DirectCompiler() = default;

AsmPrinter::CompiledFunction DirectCompiler::result(
    ArrayRef<std::pair<uint32_t, MCSymbol *>> SortedLabels) const {
  AsmPrinter::CompiledFunction CF;
  CF.Code = Code;
  CF.Fixups = Fixups;
  CF.Insts = Insts;
  CF.Labels = SortedLabels;
  CF.CFI = CFI;
  CF.StackSize = StackSize;
  CF.EH = HasLandingPads ? &EH : nullptr;
  CF.Debug = HasDebug ? &Debug : nullptr;
  return CF;
}

void DirectCompiler::failAttrs(const char *Reason, AttributeList AL) {
  if (!FailReason)
    for (AttributeSet AS : AL)
      for (Attribute A : AS)
        if (FailDetail.empty() && !G.Pass->isHandledAttr(A))
          FailDetail = A.getAsString();
  fail(Reason);
}

void DirectCompiler::encode(const MCInst &I, bool Branch) {
  if (G.Text)
    Insts.emplace_back(Code.size(), I);
  size_t Start = Code.size(), FirstFixup = Fixups.size();
  EncodingCache::Key K;
  auto *E = G.Encodings.slot(I, STI, Branch, K);
  if (E && EncodingCache::hit(*E, K)) {
    Code.resize_for_overwrite(Start + sizeof(E->Bytes));
    memcpy(Code.data() + Start, E->Bytes, sizeof(E->Bytes));
    Code.truncate(Start + E->Len);
    return;
  }
  CE.encodeInstruction(I, Code, Fixups, STI);
  if (Branch)
    Fixups.truncate(FirstFixup);
  for (size_t Idx = FirstFixup; Idx != Fixups.size(); ++Idx)
    Fixups[Idx].setOffset(Fixups[Idx].getOffset() + Start);
  if (E && Fixups.size() == FirstFixup && Code.size() - Start <= 15)
    EncodingCache::fill(*E, K, ArrayRef(Code).drop_front(Start));
}

std::pair<uint32_t, MCInst> *DirectCompiler::instAt(uint32_t Off) {
  return std::prev(upper_bound(
      Insts, Off, [](uint32_t O, const auto &P) { return O < P.first; }));
}

void DirectCompiler::setBranchTarget(uint32_t Pos, unsigned OpIdx,
                                     uint32_t Target) {
  MCSymbol *&Sym = Labels[Target];
  if (!Sym)
    Sym = Ctx.createTempSymbol();
  instAt(Pos)->second.getOperand(OpIdx) =
      MCOperand::createExpr(MCSymbolRefExpr::create(Sym, Ctx));
}

bool DirectCompiler::classify(Type *T, ValTy &VT) {
  if (T->isPointerTy()) {
    if (T->getPointerAddressSpace() != 0)
      return false;
    VT = GP64;
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

void DirectCompiler::spillReg(uint8_t R) {
  const Value *V = Owner[R];
  ValInfo &VI = info(V);
  if (!isClean(R))
    storeSlot(slotFor(VI), typeOf(V), R);
  VI.Reg = NoReg;
  release(R);
}

void DirectCompiler::storeAndCache(const Value *V, ValTy VT, uint8_t R) {
  ValInfo &VI = info(V);
  storeSlot(slotFor(VI), VT, R);
  Owner[R] = V;
  Clean |= uint64_t(1) << R;
  VI.Reg = R;
}

void DirectCompiler::dropClean() {
  for (uint64_t M = Clean; M; M &= M - 1)
    spillReg(llvm::countr_zero(M));
}

uint8_t DirectCompiler::allocFrom(ArrayRef<uint8_t> Order) {
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

void DirectCompiler::claim(uint8_t R) {
  if (Owner[R] == Busy) {
    fail("register conflict");
    return;
  }
  if (Owner[R])
    spillReg(R);
  Owner[R] = Busy;
}

MCSymbol *DirectCompiler::sym(const GlobalValue &GV, bool PreferLocal) {
  MCSymbol *&S = G.Syms[PreferLocal][&GV];
  if (!S)
    S = PreferLocal ? AP.getSymbolPreferLocal(GV) : AP.getSymbol(&GV);
  return S;
}

MCSymbol *DirectCompiler::libSym(const char *Name) {
  MCSymbol *&S = G.LibSyms[Name];
  if (!S)
    S = Ctx.getOrCreateSymbol(Name);
  return S;
}

uint8_t DirectCompiler::constant(const Constant *C, uint8_t R) {
  ValTy VT = typeOf(C);
  if (VT.FP) {
    if (R == NoReg)
      R = allocFP();
    uint64_t Bits = 0;
    if (auto *CF = dyn_cast<ConstantFP>(C))
      Bits = CF->getValueAPF().bitcastToAPInt().getZExtValue();
    else if (!isa<UndefValue>(C))
      fail("fp constant");
    fpConstant(R, Bits, VT);
    return R;
  }
  if (R == NoReg)
    R = allocGP();
  if (auto *CI = dyn_cast<ConstantInt>(C)) {
    movImm(R, VT.I1 ? CI->getZExtValue() : CI->getSExtValue(), VT.Size);
  } else if (isa<ConstantPointerNull>(C) || isa<UndefValue>(C)) {
    movImm(R, 0, 8);
  } else if (auto *GV = dyn_cast<GlobalValue>(C)) {
    globalAddr(*GV, 0, R);
  } else if (auto *Expr = dyn_cast<ConstantExpr>(C);
             Expr && Expr->getOpcode() == Instruction::IntToPtr &&
             isa<ConstantInt>(Expr->getOperand(0))) {
    movImm(R, cast<ConstantInt>(Expr->getOperand(0))->getZExtValue(), 8);
  } else if (C->getType()->isPointerTy()) {
    APInt Off(64, 0);
    const Value *Base = C->stripAndAccumulateConstantOffsets(
        DL, Off, /*AllowNonInbounds=*/true);
    if (auto *GV = dyn_cast<GlobalValue>(Base))
      globalAddr(*GV, Off.getSExtValue(), R);
    else
      fail("constant expression");
  } else {
    fail("constant");
  }
  return R;
}

uint8_t DirectCompiler::use(const Value *V) {
  ZExtKnown = false;
  if (auto *C = dyn_cast<Constant>(V))
    return constant(C, NoReg);
  ValTy VT = typeOf(V);
  ValInfo &VI = info(V);
  if (VI.Kind != ValInfo::Normal)
    return useSpecial(V, VI, VT, NoReg);
  if (VI.Reg != NoReg) {
    uint8_t R = VI.Reg;
    VI.Reg = NoReg;
    Owner[R] = Busy;
    Clean &= ~(uint64_t(1) << R);
    ZExtKnown = isa<LoadInst>(V) || isa<ZExtInst>(V);
    return R;
  }
  int32_t Slot = slotFor(VI);
  uint8_t R = VT.FP ? allocFP() : allocGP();
  loadSlot(R, VT, Slot);
  ZExtKnown = !VT.I1;
  return R;
}

void DirectCompiler::useInto(const Value *V, uint8_t R) {
  ZExtKnown = false;
  if (auto *C = dyn_cast<Constant>(V)) {
    constant(C, R);
    return;
  }
  ValTy VT = typeOf(V);
  if (isFPReg(R) != VT.FP)
    return fail("register class mismatch");
  ValInfo &VI = info(V);
  if (VI.Kind != ValInfo::Normal) {
    useSpecial(V, VI, VT, R);
    return;
  }
  if (VI.Reg != NoReg) {
    regMove(R, VI.Reg);
    if (!isClean(VI.Reg)) {
      Owner[VI.Reg] = nullptr;
      VI.Reg = NoReg;
    }
    ZExtKnown = isa<LoadInst>(V) || isa<ZExtInst>(V);
    return;
  }
  loadSlot(R, VT, slotFor(VI));
  ZExtKnown = !VT.I1;
}

void DirectCompiler::useIn(const Value *V, uint8_t R) {
  if (Owner[R] && Owner[R] == V) {
    info(V).Reg = NoReg;
    Owner[R] = Busy;
    Clean &= ~(uint64_t(1) << R);
    ZExtKnown = isa<LoadInst>(V) || isa<ZExtInst>(V);
    return;
  }
  claim(R);
  useInto(V, R);
}

uint8_t DirectCompiler::useSpecial(const Value *V, ValInfo &VI, ValTy VT,
                                   uint8_t R) {
  if (VI.Kind == ValInfo::DeferredCmp) {
    fail("deferred compare used as a value");
    return R == NoReg ? Regs.GPOrder[0] : R;
  }
  if (VI.Kind != ValInfo::Alloca)
    return useDeferred(V, VI, VT, R);
  if (R == NoReg)
    R = allocGP();
  addDisp(R, NoReg, VI.Slot);
  return R;
}

bool DirectCompiler::isTemp(const Value &V, const BasicBlock *BB) {
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

void DirectCompiler::assignArgReg(const Argument &A, ValTy VT, uint8_t R,
                                  bool NeedSlot) {
  if (!NeedSlot && A.use_empty())
    return;
  if (!NeedSlot && isTemp(A, &F.getEntryBlock())) {
    Owner[R] = &A;
    info(&A).Reg = R;
  } else {
    storeAndCache(&A, VT, R);
  }
}

uint8_t DirectCompiler::argRegHint(const Instruction &I) {
  const Value *V = &I;
  for (unsigned Depth = 0; Depth != 4; ++Depth) {
    if (!V->hasOneUse() || !V->getType()->isIntOrPtrTy())
      return NoReg;
    auto *U = dyn_cast<Instruction>(V->user_back());
    if (!U || U->getParent() != I.getParent())
      return NoReg;
    if (isa<ReturnInst>(U))
      return Regs.RetHint;
    if (auto *CI = dyn_cast<CallBase>(U)) {
      if (isa<IntrinsicInst>(CI) || CI->isInlineAsm())
        return NoReg;
      unsigned NumGP = 0;
      for (const Use &A : CI->args()) {
        if (Regs.SRetArg != NoReg &&
            CI->paramHasAttr(CI->getArgOperandNo(&A), Attribute::StructRet)) {
          if (A.get() == V)
            return Regs.SRetArg;
          continue;
        }
        if (A.get() == V)
          return NumGP < Regs.GPArgs.size() ? Regs.GPArgs[NumGP]
                                            : uint8_t(NoReg);
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
      InPlace = BO->getOperand(0) == V &&
                (BO->getOpcode() == Instruction::Add ||
                 BO->getOpcode() == Instruction::Sub ||
                 BO->getOpcode() == Instruction::Mul || BO->isBitwiseLogicOp());
    if (!InPlace)
      return NoReg;
    V = U;
  }
  return NoReg;
}

void DirectCompiler::def(const Instruction &I, uint8_t R) {
  if (I.use_empty()) {
    release(R);
  } else if (isTemp(I, I.getParent())) {
    Owner[R] = &I;
    info(&I).Reg = R;
  } else {
    storeAndCache(&I, typeOf(&I), R);
  }
}

void DirectCompiler::dropOperands(const Instruction &I) {
  for (const Value *V : I.operands()) {
    auto *VI = lookup(V);
    if (!VI)
      continue;
    // A deferred address or load holds its operands' registers.
    if (VI->Kind == ValInfo::DeferredGEP || VI->Kind == ValInfo::DeferredLoad) {
      dropOperands(*cast<Instruction>(V));
      continue;
    }
    if (VI->Reg == NoReg)
      continue;
    release(VI->Reg);
    VI->Reg = NoReg;
  }
}

bool DirectCompiler::frameOffset(const Value *P, int64_t &Off) {
  auto *VI = lookup(P);
  if (!VI)
    return false;
  if (VI->Kind == ValInfo::Alloca) {
    Off = VI->Slot;
    return true;
  }
  auto *GEP = dyn_cast<GetElementPtrInst>(P);
  APInt GEPOff(64, 0);
  if (VI->Kind != ValInfo::DeferredGEP || !GEP ||
      !frameOffset(GEP->getPointerOperand(), Off) ||
      !GEP->accumulateConstantOffset(DL, GEPOff))
    return false;
  Off += GEPOff.getSExtValue();
  return isInt<32>(Off);
}

int64_t DirectCompiler::constGEPOffset(const GetElementPtrInst &GEP) {
  APInt Off(64, 0);
  if (!GEP.accumulateConstantOffset(DL, Off))
    fail("gep offset");
  return Off.getSExtValue();
}

bool DirectCompiler::isFixedAddress(const Value *P) {
  auto *VI = lookup(P);
  if (!VI)
    return false;
  if (VI->Kind == ValInfo::Alloca)
    return true;
  return VI->Kind == ValInfo::DeferredGEP &&
         isFixedAddress(cast<GetElementPtrInst>(P)->getPointerOperand());
}

bool DirectCompiler::aggParts(Type *T, SmallVectorImpl<AggPart> &Parts) {
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
    P.RetReg = aggRetReg(P.VT.FP, P.VT.FP ? NumFP++ : NumGP++);
    Parts.push_back(P);
  }
  return true;
}

void DirectCompiler::spillForCall(const Value *IndirectCallee,
                                  ArrayRef<const Value *> Args,
                                  ArrayRef<ArgLoc> Locs) {
  for (const Value *&Slot : MutableArrayRef(Owner, Regs.NumRegs)) {
    const Value *V = Slot;
    if (!V || V == Busy)
      continue;
    uint8_t R = &Slot - Owner;
    // An argument already sitting in its own register stays there.
    bool Keep = (V == IndirectCallee || is_contained(Args, V)) &&
                (!isArgReg(R) || any_of(Locs, [&](const ArgLoc &L) {
                  return L.V == V && L.Reg == R;
                }));
    if (!Keep)
      spillReg(R);
  }
}

void DirectCompiler::finishCall(const Instruction &I) {
  dropClean();
  const Value *BusyVal = Busy;
  for (const Value *&V : MutableArrayRef(Owner, Regs.NumRegs))
    if (V == BusyVal)
      V = nullptr;
  if (I.getType()->isVoidTy())
    return;
  SmallVector<AggPart, 2> Parts;
  if (aggParts(I.getType(), Parts)) {
    int32_t Slot = aggSlotFor(&I);
    for (unsigned P = 0; P != Parts.size(); ++P)
      storeSlot(aggPartSlot(Slot, P), Parts[P].VT, Parts[P].RetReg);
    return;
  }
  ValTy RT;
  if (!classify(I.getType(), RT))
    return fail("call return type");
  uint8_t R = aggRetReg(RT.FP, 0);
  Owner[R] = Busy;
  def(I, R);
}

void DirectCompiler::setReturnValue(const ReturnInst &I) {
  const Value *V = I.getReturnValue();
  if (!V)
    return;
  SmallVector<AggPart, 2> Parts;
  if (aggParts(V->getType(), Parts)) {
    auto *VI = lookup(V);
    if (!VI || !VI->Slot)
      return fail("aggregate return value");
    for (unsigned P = 0; P != Parts.size(); ++P) {
      claim(Parts[P].RetReg);
      loadSlot(Parts[P].RetReg, Parts[P].VT, aggPartSlot(VI->Slot, P));
    }
    for (const AggPart &P : Parts)
      release(P.RetReg);
    return;
  }
  ValTy VT;
  if (!classify(V->getType(), VT))
    return fail("return type");
  uint8_t R = aggRetReg(VT.FP, 0);
  useIn(V, R);
  if (!VT.FP && VT.Size < 4) {
    if (F.hasRetAttribute(Attribute::SExt))
      sext32(R, VT);
    else if (F.hasRetAttribute(Attribute::ZExt))
      zext32(R, VT);
  }
  release(R);
}

void DirectCompiler::emitNoReturnCall(const char *LibName, bool MayThrow) {
  emitCallTo(nullptr, LibName, MayThrow);
  // As for the unreachable that DwarfEHPrepare and StackProtector put after
  // the call.
  const TargetOptions &O = AP.TM.Options;
  if (O.TrapUnreachable && !O.NoTrapAfterNoreturn)
    emitTrap(/*Debug=*/false);
}

void DirectCompiler::recordCall(size_t Start, bool MayThrow) {
  HasCalls = true;
  if (MayThrow && F.hasPersonalityFn()) {
    EH.CallSites.push_back(
        {uint32_t(Start), uint32_t(Code.size() - Start), 0, 0});
    CallSiteLPad.push_back(nullptr);
  }
}

unsigned DirectCompiler::typeIdFor(const GlobalValue *GV) {
  auto It = find(EH.TypeInfos, GV);
  if (It == EH.TypeInfos.end()) {
    EH.TypeInfos.push_back(GV);
    return EH.TypeInfos.size();
  }
  return It - EH.TypeInfos.begin() + 1;
}

void DirectCompiler::appendSLEB(int64_t V) {
  uint8_t Buf[10];
  unsigned N = encodeSLEB128(V, Buf);
  EH.Actions.append(Buf, Buf + N);
}

void DirectCompiler::resolveLandingPads() {
  for (size_t Idx = 0; Idx != EH.CallSites.size(); ++Idx)
    if (CallSiteLPad[Idx])
      EH.CallSites[Idx].LandingPad = BlockOff[CallSiteLPad[Idx]->getNumber()];
}

static bool hasPhis(const BasicBlock *BB) { return isa<PHINode>(BB->front()); }

void DirectCompiler::edge(const BasicBlock &Pred, const BasicBlock &Succ) {
  if (!hasPhis(&Succ))
    return;
  bool Hazard = false;
  for (const PHINode &PN : Succ.phis()) {
    auto *In = dyn_cast<PHINode>(PN.getIncomingValueForBlock(&Pred));
    if (In && In != &PN && In->getParent() == &Succ)
      Hazard = true;
  }
  SmallVector<std::pair<uint8_t, const PHINode *>, 8> Pending;
  for (const PHINode &PN : Succ.phis()) {
    const Value *V = PN.getIncomingValueForBlock(&Pred);
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
    if (auto *CI = dyn_cast<ConstantInt>(V);
        CI &&
        storeImmToSlot(Slot, VT.I1 ? CI->getZExtValue() : CI->getSExtValue()))
      continue;
    uint8_t R = use(V);
    storeSlot(Slot, VT, R);
    release(R);
  }
  for (auto [R, PN] : Pending) {
    storeSlot(slotFor(PN), typeOf(PN), R);
    release(R);
  }
}

void DirectCompiler::compileAlloca(const AllocaInst &I, bool Guarded) {
  if (!I.isStaticAlloca() || I.getAlign() > Align(16))
    return fail("dynamic or over-aligned alloca");
  std::optional<TypeSize> Size = I.getAllocationSize(DL);
  if (!Size || Size->isScalable() || Size->getFixedValue() > (1u << 28))
    return fail("alloca size");
  ValInfo &VI = info(&I);
  // Already placed next to the stack protector guard.
  if (VI.Kind == ValInfo::Alloca)
    return;
  VI.Kind = ValInfo::Alloca;
  uint64_t Bytes = std::max<uint64_t>(Size->getFixedValue(), 1);
  uint64_t Alignment = std::max<uint64_t>(I.getAlign().value(), MinAllocaAlign);
  VI.Slot = Guarded || GuardAllAllocas ? allocGuardedFrame(Bytes, Alignment)
                                       : allocFrame(Bytes, Alignment);
}

void DirectCompiler::compileGEP(const GetElementPtrInst &I) {
  if (I.getType()->isVectorTy() || I.getSourceElementType()->isScalableTy())
    return fail("vector gep");
  if (I.hasAllConstantIndices()) {
    // A fixed address can be rebuilt at each use, unless a block laid out
    // earlier already expects the value in its slot.
    ValInfo &VI = info(&I);
    if (isFixedAddress(I.getPointerOperand()) && !VI.Slot) {
      VI.Kind = ValInfo::DeferredGEP;
      return;
    }
    // Otherwise the base register is consumed by the single use.
    if (I.hasOneUse()) {
      auto *U = cast<Instruction>(I.user_back());
      auto *SI = dyn_cast<StoreInst>(U);
      if (U->getParent() == I.getParent() &&
          (isa<LoadInst>(U) || isa<GetElementPtrInst>(U) ||
           (SI && SI->getValueOperand() != &I))) {
        VI.Kind = ValInfo::DeferredGEP;
        return;
      }
    }
  }
  const Value *Base = I.getPointerOperand();
  uint8_t R = NoReg;
  int64_t Disp = 0;
  if (auto *VI = lookup(Base); VI && VI->Kind == ValInfo::Alloca)
    Disp = VI->Slot;
  else
    R = use(Base);
  // The base is the frame pointer while R is NoReg.
  auto Flush = [&] {
    if (!fitsDisp(Disp))
      return fail("gep offset");
    if (R == NoReg) {
      R = allocGP();
      addDisp(R, NoReg, Disp);
    } else if (Disp) {
      addDisp(R, R, Disp);
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
    sext64(X, XT);
    Flush();
    if (FailReason)
      return;
    addScaled(R, X, Stride);
    release(X);
  }
  Flush();
  if (!FailReason)
    def(I, R);
}

void DirectCompiler::compileICmp(const ICmpInst &I) {
  if (I.getType()->isVectorTy())
    return fail("vector compare");
  if (I.hasOneUse()) {
    auto *Br = dyn_cast<CondBrInst>(I.user_back());
    if (Br && Br->getParent() == I.getParent()) {
      info(&I).Kind = ValInfo::DeferredCmp;
      return;
    }
  }
  unsigned CC = emitICmp(I);
  uint8_t R = allocGP();
  setCC(R, CC);
  def(I, R);
}

void DirectCompiler::compileExtractValue(const ExtractValueInst &I) {
  SmallVector<AggPart, 2> Parts;
  const Value *Agg = I.getAggregateOperand();
  auto *VI = lookup(Agg);
  if (I.getNumIndices() != 1 || !aggParts(Agg->getType(), Parts) || !VI ||
      !VI->Slot)
    return fail("extractvalue");
  unsigned Idx = I.getIndices()[0];
  ValTy VT = Parts[Idx].VT;
  int32_t Slot = VI->Slot;
  uint8_t R = VT.FP ? allocFP() : allocGP();
  loadSlot(R, VT, aggPartSlot(Slot, Idx));
  def(I, R);
}

void DirectCompiler::compileInsertValue(const InsertValueInst &I) {
  SmallVector<AggPart, 2> Parts;
  const Value *Agg = I.getAggregateOperand();
  if (I.getNumIndices() != 1 || !aggParts(I.getType(), Parts))
    return fail("insertvalue");
  unsigned Idx = I.getIndices()[0];
  int32_t Slot = aggSlotFor(&I);
  if (!isa<UndefValue>(Agg)) {
    auto *VI = lookup(Agg);
    if (!VI || !VI->Slot)
      return fail("insertvalue aggregate");
    int32_t Src = VI->Slot;
    uint8_t T = allocGP();
    for (unsigned P = 0; P != Parts.size(); ++P) {
      if (P == Idx)
        continue;
      loadSlot(T, GP64, aggPartSlot(Src, P));
      storeSlot(aggPartSlot(Slot, P), GP64, T);
    }
    release(T);
  }
  uint8_t R = use(I.getInsertedValueOperand());
  storeSlot(aggPartSlot(Slot, Idx), Parts[Idx].VT, R);
  release(R);
}

uint32_t DirectCompiler::actionFor(const LandingPadInst &LPI) {
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

bool DirectCompiler::checkCall(const CallBase &CB) {
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
  switch (G.calleeVerdict(CalleeF->getAttributes())) {
  case ModuleState::CalleeOK:
    return true;
  case ModuleState::CalleeParamAttr:
    failAttrs("call attribute", CalleeF->getAttributes());
    break;
  case ModuleState::CalleeFnAttr:
    fail("callee attribute");
    break;
  case ModuleState::CalleeDontCall:
    fail("dontcall callee");
    break;
  }
  return false;
}

void DirectCompiler::compileCall(const CallInst &I) {
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

void DirectCompiler::compileIntrinsic(const IntrinsicInst &I) {
  switch (I.getIntrinsicID()) {
  case Intrinsic::lifetime_start:
  case Intrinsic::lifetime_end:
  case Intrinsic::donothing:
  case Intrinsic::assume:
  case Intrinsic::experimental_noalias_scope_decl:
  case Intrinsic::vaend:
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
  case Intrinsic::trap:
    return emitTrap(/*Debug=*/false);
  case Intrinsic::debugtrap:
    return emitTrap(/*Debug=*/true);
  default:
    return compileTargetIntrinsic(I);
  }
}

void DirectCompiler::compileInvoke(const InvokeInst &I) {
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
  // If the landing pad has PHIs, the moves go in a stub that keeps the
  // exception pointer and selector registers intact.
  bool Stub = hasPhis(Unwind);
  edge(*BB, *I.getNormalDest());
  jmpTo(*I.getNormalDest(), /*Force=*/Stub);
  EH.CallSites[CS].Action = Action;
  if (Stub) {
    EH.CallSites[CS].LandingPad = Code.size();
    emitLandingPadEntry();
    uint8_t Ptr = exceptionPointerReg(), Sel = exceptionSelectorReg();
    claim(Ptr);
    claim(Sel);
    edge(*BB, *Unwind);
    release(Ptr);
    release(Sel);
    jmpTo(*Unwind, /*Force=*/true);
  } else {
    CallSiteLPad[CS] = Unwind;
  }
  HasLandingPads = true;
}

void DirectCompiler::compileLandingPad(const LandingPadInst &I) {
  SmallVector<AggPart, 2> Parts;
  if (!aggParts(I.getType(), Parts) || Parts.size() != 2 || Parts[0].VT.FP ||
      Parts[1].VT.FP)
    return fail("landing pad type");
  if (!hasPhis(I.getParent()))
    emitLandingPadEntry();
  int32_t Slot = aggSlotFor(&I);
  storeSlot(aggPartSlot(Slot, 0), GP64, exceptionPointerReg());
  storeSlot(aggPartSlot(Slot, 1), GP64, exceptionSelectorReg());
}

void DirectCompiler::compileResume(const ResumeInst &I) {
  auto *VI = lookup(I.getValue());
  if (!VI || !VI->Slot)
    return fail("resume value");
  uint8_t Arg = Regs.GPArgs[0];
  claim(Arg);
  loadSlot(Arg, GP64, VI->Slot);
  emitNoReturnCall("_Unwind_Resume", /*MayThrow=*/true);
  release(Arg);
}

void DirectCompiler::compileCondBr(const CondBrInst &I) {
  const BasicBlock *BB = I.getParent();
  const BasicBlock *T = I.getSuccessor(0), *E = I.getSuccessor(1);
  if (auto *CI = dyn_cast<ConstantInt>(I.getCondition())) {
    const BasicBlock *Dest = CI->isZero() ? E : T;
    edge(*BB, *Dest);
    return jmpTo(*Dest);
  }
  unsigned CC = emitCondition(I.getCondition());
  if (T == E) {
    edge(*BB, *T);
    return jmpTo(*T);
  }
  if (T == NextBB && !hasPhis(E)) {
    jccTo(invertCC(CC), *E);
    edge(*BB, *T);
  } else if (!hasPhis(T)) {
    jccTo(CC, *T);
    edge(*BB, *E);
    jmpTo(*E);
  } else if (!hasPhis(E)) {
    jccTo(invertCC(CC), *E);
    edge(*BB, *T);
    jmpTo(*T);
  } else {
    size_t Pos = jccFwd(invertCC(CC));
    edge(*BB, *T);
    jmpTo(*T, /*Force=*/true);
    bindFwd(Pos);
    edge(*BB, *E);
    jmpTo(*E);
  }
}

void DirectCompiler::compileSwitch(const SwitchInst &I) {
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
    unsigned CC = emitCaseCompare(R, W, V);
    const BasicBlock *Succ = C.getCaseSuccessor();
    if (!hasPhis(Succ)) {
      jccTo(CC, *Succ);
    } else {
      size_t Pos = jccFwd(invertCC(CC));
      edge(*BB, *Succ);
      jmpTo(*Succ, /*Force=*/true);
      bindFwd(Pos);
    }
    if (FailReason)
      return;
  }
  release(R);
  edge(*BB, *I.getDefaultDest());
  jmpTo(*I.getDefaultDest());
}

void DirectCompiler::compileInst(const Instruction &I) {
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
    edge(*I.getParent(), *Succ);
    // As FastISel, keep a branch that is alone in its block, so that its line
    // has a location.
    return jmpTo(*Succ, /*Force=*/&I.getParent()->front() == &I);
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
      emitTrap(/*Debug=*/false);
    return;
  default:
    if (auto *BO = dyn_cast<BinaryOperator>(&I))
      return compileBinary(*BO);
    if (auto *CI = dyn_cast<CastInst>(&I))
      return compileCast(*CI);
    return fail(I.getOpcodeName());
  }
}

// Collect the source labels and variable locations attached to I. A variable
// lives in a frame object, or behind a pointer argument that has a frame slot.
// Code must not depend on debug info, so a record that cannot be described
// from the code as generated is dropped, as FastISel does when it cannot lower
// one.
void DirectCompiler::recordDebugRecords(const Instruction &I,
                                        bool &BlockStart) {
  for (const DbgRecord &DR : I.getDbgRecordRange()) {
    if (auto *DLR = dyn_cast<DbgLabelRecord>(&DR)) {
      Debug.Rows.push_back({uint32_t(Code.size()), DLR->getDebugLoc().get(),
                            BlockStart, false, DLR->getLabel()});
      BlockStart = false;
      continue;
    }
    auto *DVR = dyn_cast<DbgVariableRecord>(&DR);
    if (!DVR || !DVR->isDbgDeclare() || DVR->isKillLocation()) {
      ++NumDroppedDbg;
      continue;
    }
    const Value *Addr = DVR->getAddress();
    auto *VI = lookup(Addr);
    auto *Arg = dyn_cast<Argument>(Addr);
    int64_t Off;
    uint64_t Size = 8;
    if (VI && VI->Kind == ValInfo::Alloca) {
      Off = VI->Slot;
      Size =
          Arg ? DL.getTypeAllocSize(Arg->getParamByValType())
              : cast<AllocaInst>(Addr)->getAllocationSize(DL)->getFixedValue();
    } else if (Arg && VI && VI->Kind == ValInfo::Normal && VI->Slot) {
      Off = VI->Slot;
      DerefVars.push_back(Debug.Variables.size());
    } else if (frameOffset(Addr, Off)) {
      // A field of a frame object.
      Size = std::max<uint64_t>(
          divideCeil(DVR->getVariable()->getSizeInBits().value_or(64), 8), 1);
    } else {
      ++NumDroppedDbg;
      continue;
    }
    Debug.Variables.push_back({DVR->getVariable(), DVR->getExpression(),
                               DVR->getDebugLoc().get(), Off + FixedObjectBias,
                               Size});
  }
}

bool DirectCompiler::checkFunction() {
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
  if (!G.attrsHandled(F.getAttributes())) {
    failAttrs("function attribute", F.getAttributes());
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
  return true;
}

// The guard goes at the frame-pointer end of the frame, followed by the
// objects an overflow is likely to start from, as in the regular pipeline.
bool DirectCompiler::layoutStackProtector() {
  if (!F.hasFnAttribute(Attribute::StackProtect) &&
      !F.hasFnAttribute(Attribute::StackProtectStrong) &&
      !F.hasFnAttribute(Attribute::StackProtectReq))
    return true;
  DenseMap<const AllocaInst *, MachineFrameInfo::SSPLayoutKind> Layout;
  if (!SSPLayoutAnalysis::requiresStackProtector(const_cast<Function *>(&F),
                                                 &Layout))
    return true;
  StackGuardSlot = allocGuardedFrame(8, 8);
  // "ssp" singles out only large character arrays, but an overflow of any
  // other object must not get past the guard either. The stronger levels
  // single out every array and every object whose address escapes.
  GuardAllAllocas = F.hasFnAttribute(Attribute::StackProtect);
  for (auto Kind :
       {MachineFrameInfo::SSPLK_LargeArray, MachineFrameInfo::SSPLK_SmallArray,
        MachineFrameInfo::SSPLK_AddrOf})
    for (const Instruction &I : F.getEntryBlock())
      if (auto *AI = dyn_cast<AllocaInst>(&I);
          AI && AI->isStaticAlloca() && Layout.lookup(AI) == Kind)
        compileAlloca(*AI, /*Guarded=*/true);
  return !FailReason;
}

bool DirectCompiler::blockOrder(SmallVectorImpl<const BasicBlock *> &Order) {
  // Skip blocks that are unreachable from the entry.
  SmallVector<bool, 8> Reachable(F.getMaxBlockNumber());
  SmallVector<const BasicBlock *, 32> Worklist{&F.getEntryBlock()};
  Reachable[F.getEntryBlock().getNumber()] = true;
  while (!Worklist.empty())
    for (const BasicBlock *Succ : successors(Worklist.pop_back_val()))
      if (!Reachable[Succ->getNumber()]) {
        Reachable[Succ->getNumber()] = true;
        Worklist.push_back(Succ);
      }
  for (const BasicBlock &BB : F) {
    // The AsmPrinter emits labels for address-taken blocks, even unreachable
    // ones.
    if (BB.hasAddressTaken()) {
      fail("address-taken block");
      return false;
    }
    if (Reachable[BB.getNumber()])
      Order.push_back(&BB);
  }
  return true;
}

void DirectCompiler::addDebugRows(const Instruction &I, size_t Before,
                                  bool &BlockStart,
                                  const DILocation *&LastLoc) {
  const DILocation *Loc = I.getDebugLoc().get();
  // A return value set up before the epilogue gets its own row.
  size_t Epilogue = isa<ReturnInst>(I) ? EpilogueStart : Code.size();
  if (Epilogue != Before && (BlockStart || Loc != LastLoc)) {
    Debug.Rows.push_back({uint32_t(Before), Loc, BlockStart, false, nullptr});
    BlockStart = false;
  }
  if (isa<ReturnInst>(I)) {
    Debug.Rows.push_back({uint32_t(Epilogue), Loc, BlockStart, true, nullptr});
    BlockStart = false;
  }
  LastLoc = Loc;
}

bool DirectCompiler::compileBlock(const BasicBlock &BB) {
  BlockOff[BB.getNumber()] = Code.size();
  bool BlockStart = true;
  const DILocation *LastLoc = nullptr;
  for (const Instruction &I : BB) {
    if (RecordDebug)
      recordDebugRecords(I, BlockStart);
    size_t Before = Code.size();
    compileInst(I);
    if (FailReason)
      return false;
    if (RecordDebug && Code.size() != Before)
      addDebugRows(I, Before, BlockStart, LastLoc);
  }
  dropClean();
  if (any_of(ArrayRef(Owner, Regs.NumRegs), [](const Value *V) { return V; })) {
    fail("value live in a register at block end");
    return false;
  }
  return true;
}

bool DirectCompiler::run() {
  if (!checkFunction())
    return false;
  const DISubprogram *SP = F.getSubprogram();
  if (SP && SP->getUnit()->getEmissionKind() != DICompileUnit::NoDebug) {
    HasDebug = RecordDebug = true;
  } else if (AP.hasDebugInfo()) {
    // The debug handlers must see every function to terminate line sequences,
    // so this one gets an empty description.
    HasDebug = true;
  }
  if (!layoutStackProtector() || !prologue())
    return false;
  SmallVector<const BasicBlock *, 32> Order;
  if (!blockOrder(Order))
    return false;
  BlockOff.assign(F.getMaxBlockNumber(), 0);
  for (unsigned Idx = 0; Idx != Order.size(); ++Idx) {
    NextBB = Idx + 1 != Order.size() ? Order[Idx + 1] : nullptr;
    if (!compileBlock(*Order[Idx]))
      return false;
  }

  resolveLandingPads();
  if (!finish())
    return false;
  // Built only now so that a fallback leaves no new metadata behind.
  for (unsigned Idx : DerefVars)
    Debug.Variables[Idx].Expr = DIExpression::prepend(
        Debug.Variables[Idx].Expr, DIExpression::DerefBefore);
  return true;
}

DirectCodeGen::DirectCodeGen(char &ID, TargetMachine &TM,
                             const DirectCodeGenOptions &Opts,
                             const void *AsmPrinterID)
    : FunctionPass(ID), TM(TM), Opts(Opts), AsmPrinterID(AsmPrinterID) {}

DirectCodeGen::~DirectCodeGen() = default;

void DirectCodeGen::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.setPreservesAll();
}

bool DirectCodeGen::doInitialization(Module &M) {
  G.reset(*this, M);
  AP = nullptr;
  TextCE.reset();
  ModuleSupport = Unknown;
  return false;
}

// Skipping the remaining passes only stops emission if the AsmPrinter is one of
// them, so it must be in the same function pass manager.
static AsmPrinter *findAsmPrinter(PMDataManager &PM, const void *ID) {
  if (PM.getPassManagerType() != PMT_FunctionPassManager)
    return nullptr;
  auto &FPM = static_cast<FPPassManager &>(PM);
  for (unsigned I = 0, E = FPM.getNumContainedPasses(); I != E; ++I) {
    FunctionPass *P = FPM.getContainedPass(I);
    if (P->getPassID() == ID)
      return static_cast<AsmPrinter *>(P);
  }
  return nullptr;
}

bool DirectCodeGen::runAndEmit(const Function &F, DirectCompiler &C) {
  if (!C.run()) {
    FailReason = C.FailReason;
    FailDetail = std::move(C.FailDetail);
    return false;
  }
  // As PrologEpilogInserter.
  if (Attribute A = F.getFnAttribute("warn-stack-size"); A.isValid()) {
    uint64_t Threshold;
    if (!A.getValueAsString().getAsInteger(10, Threshold) &&
        C.StackSize > Threshold)
      F.getContext().diagnose(
          DiagnosticInfoStackSize(F, C.StackSize, Threshold, DS_Warning));
  }
  SmallVector<std::pair<uint32_t, MCSymbol *>, 0> Labels(C.Labels.begin(),
                                                         C.Labels.end());
  llvm::sort(Labels, less_first());
  AP->emitCompiledFunction(F, C.result(Labels));
  return true;
}

constexpr StringRef KnownModuleFlags[] = {"wchar_size",
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
                                          "debugger-tuning",
                                          "EnableValueProfiling",
                                          "MemProfProfileFilename",
                                          "probe-stack"};

const char *DirectCodeGen::setUpModule(const Module &M) {
  AP = findAsmPrinter(getResolver()->getPMDataManager(), AsmPrinterID);
  if (!AP)
    return "no AsmPrinter in the function pass manager";
  G.Text = AP->OutStreamer->hasRawTextSupport();
  if (G.Text)
    TextCE.reset(TM.getTarget().createMCCodeEmitter(*TM.getMCInstrInfo(),
                                                    AP->OutContext));
  return checkModule(M);
}

const char *DirectCodeGen::checkModule(const Module &M) {
  const TargetOptions &O = TM.Options;
  if (O.EmitCallSiteInfo || O.EmitCallGraphSection || O.BBAddrMap ||
      O.BBSections != BasicBlockSection::None ||
      O.FunctionSplitting == FunctionSplittingMode::All ||
      O.ForceDwarfFrameSection || O.MCOptions.EmitSFrameUnwind)
    return "target option";
  if (const char *Reason = unsupportedModuleReason(M))
    return Reason;
  if (EnableFSDiscriminator)
    return "codegen option";
  // Pre-encoded bytes bypass branch-alignment padding. Text and object output
  // must take the same decisions, so the backend is not the one of the
  // streamer, which only an object streamer has.
  std::unique_ptr<MCAsmBackend> MAB(TM.getTarget().createMCAsmBackend(
      TM.getMCSubtargetInfo(), TM.getMCRegisterInfo(), O.MCOptions));
  if (MAB && MAB->allowAutoPadding())
    return "branch alignment padding";
  if (M.getContext().getMainRemarkStreamer() ||
      M.getContext().getDiagHandlerPtr()->isAnyRemarkEnabled())
    return "remarks";
  if (M.getNamedMetadata(PseudoProbeDescMetadataName))
    return "pseudo probes";
  SmallVector<Module::ModuleFlagEntry, 8> Flags;
  M.getModuleFlagsMetadata(Flags);
  for (const Module::ModuleFlagEntry &Flag : Flags) {
    StringRef Key = Flag.Key->getString();
    // "nosanitize_*" marks a module a sanitizer has instrumented.
    if (!is_contained(KnownModuleFlags, Key) &&
        !Key.starts_with("nosanitize_") && !isHandledModuleFlag(Flag))
      return "module flag";
  }
  return nullptr;
}

bool DirectCodeGen::runOnFunction(Function &F) {
  if (ModuleSupport == Unknown) {
    const char *Reason = setUpModule(*F.getParent());
    ModuleSupport = Reason ? Unsupported : Supported;
    if (Reason && (Opts.Report || Opts.Abort))
      errs() << "direct-codegen: module not supported: " << Reason << '\n';
  }
  if (ModuleSupport == Unsupported || F.hasAvailableExternallyLinkage() ||
      NumDone >= Opts.Limit)
    return false;
  FailReason = unsupportedSubtargetReason(F);
  FailDetail.clear();
  const MCCodeEmitter &CE =
      TextCE ? *TextCE
             : static_cast<MCObjectStreamer &>(*AP->OutStreamer)
                   .getAssembler()
                   .getEmitter();
  if (FailReason || !compile(F, *AP, CE, G)) {
    ++NumFallback;
    if (Opts.Report || Opts.Abort)
      errs() << "direct-codegen: " << F.getName() << ": " << FailReason
             << (FailDetail.empty() ? "" : " " + FailDetail) << '\n';
    if (Opts.Abort)
      reportFatalUsageError("direct code generation failed");
    return false;
  }
  ++NumCompiled;
  ++NumDone;
  static_cast<FPPassManager &>(getResolver()->getPMDataManager())
      .skipRemainingPasses();
  return false;
}
