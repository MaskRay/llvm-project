//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/CodeGen/OnePassCodeGen.h"
#include "llvm/ADT/BitVector.h"
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
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/LegacyPassManagers.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PseudoProbe.h"
#include "llvm/MC/MCAsmBackend.h"
#include "llvm/MC/MCAssembler.h"
#include "llvm/MC/MCCodeEmitter.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCObjectStreamer.h"
#include "llvm/Support/LEB128.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Transforms/Utils/Local.h"

using namespace llvm;

#define DEBUG_TYPE "onepass-codegen"

STATISTIC(NumCompiled, "Number of functions emitted by the one-pass compiler");
STATISTIC(NumFallback, "Number of functions left to the regular pipeline");
STATISTIC(NumDroppedDbg, "Number of debug records the one-pass compiler drops");

// The gates below are allowlists: anything they do not know sends the function
// (or the module) to the regular pipeline.

bool OnePassCodeGen::isHandledAttr(Attribute A) const {
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
    return is_contained(Known, K) || K.starts_with("no-builtin-");
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
  case Attribute::StackProtect:
  case Attribute::StackProtectReq:
  case Attribute::StackProtectStrong:
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

bool OnePassCompiler::ModuleState::attrsHandled(AttributeList AL) {
  auto [It, Inserted] = AttrVerdicts.try_emplace(AL.getRawPointer(), true);
  if (Inserted)
    for (AttributeSet AS : AL)
      for (Attribute A : AS)
        It->second &= Pass->isHandledAttr(A);
  return It->second;
}

OnePassCompiler::ModuleState::CalleeVerdict
OnePassCompiler::ModuleState::calleeVerdict(AttributeList AL) {
  auto [It, Inserted] =
      CalleeVerdicts.try_emplace(AL.getRawPointer(), CalleeOK);
  if (!Inserted)
    return It->second;
  CalleeVerdict &V = It->second;
  if (AL.hasFnAttr("dontcall-error") || AL.hasFnAttr("dontcall-warn"))
    V = CalleeDontCall;
  for (Attribute A : AL.getFnAttrs())
    if (Pass->changesCalls(A))
      V = CalleeFnAttr;
  for (unsigned Idx : AL.indexes())
    if (Idx != AttributeList::FunctionIndex)
      for (Attribute A : AL.getAttributes(Idx))
        if (!Pass->isHandledAttr(A))
          V = CalleeParamAttr;
  return V;
}

void OnePassCompiler::ModuleState::reset(const OnePassCodeGen &P,
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

OnePassCompiler::OnePassCompiler(const Function &F, const MCSubtargetInfo &STI,
                                 AsmPrinter &AP, const MCCodeEmitter &CE,
                                 ModuleState &G, ArrayRef<uint8_t> GPOrder,
                                 ArrayRef<uint8_t> FPOrder, uint8_t FirstFPReg)
    : Code(G.Code), Fixups(G.Fixups), F(F), STI(STI), DL(F.getDataLayout()),
      AP(AP), Ctx(AP.OutContext), CE(CE), G(G), BlockOff(G.BlockOff),
      Vals(G.Vals), NumArgs(F.arg_size()),
      Busy(reinterpret_cast<const Value *>(this)), GPAllocOrder(GPOrder),
      FPAllocOrder(FPOrder), FirstFPReg(FirstFPReg) {
  Code.clear();
  Fixups.clear();
  Vals.assign(NumArgs + F.getMaxInstructionNumber(), ValInfo());
  std::fill(std::begin(Owner), std::end(Owner), nullptr);
}

OnePassCompiler::~OnePassCompiler() = default;

void OnePassCompiler::failAttrs(const char *Reason, AttributeList AL) {
  if (!FailReason)
    for (AttributeSet AS : AL)
      for (Attribute A : AS)
        if (FailDetail.empty() && !G.Pass->isHandledAttr(A))
          FailDetail = A.getAsString();
  fail(Reason);
}

void OnePassCompiler::emit(const MCInst &I) {
  size_t Start = Code.size(), FirstFixup = Fixups.size();
  EncodingCache::Key K;
  auto *E = G.Encodings.slot(I, STI, K);
  if (E && EncodingCache::hit(*E, K)) {
    Code.append(E->Bytes, E->Bytes + E->Len);
    return;
  }
  CE.encodeInstruction(I, Code, Fixups, STI);
  for (size_t Idx = FirstFixup; Idx != Fixups.size(); ++Idx)
    Fixups[Idx].setOffset(Fixups[Idx].getOffset() + Start);
  if (E && Fixups.size() == FirstFixup && Code.size() - Start <= 15)
    EncodingCache::fill(*E, K, ArrayRef(Code).drop_front(Start));
}

void OnePassCompiler::emitBranch(const MCInst &I) {
  size_t NumFixups = Fixups.size();
  CE.encodeInstruction(I, Code, Fixups, STI);
  Fixups.truncate(NumFixups);
}

bool OnePassCompiler::classify(Type *T, ValTy &VT) {
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

void OnePassCompiler::spillReg(uint8_t R) {
  const Value *V = Owner[R];
  ValInfo &VI = info(V);
  if (!isClean(R))
    storeSlot(slotFor(VI), typeOf(V), R);
  VI.Reg = NoReg;
  release(R);
}

void OnePassCompiler::storeAndCache(const Value *V, ValTy VT, uint8_t R) {
  ValInfo &VI = info(V);
  storeSlot(slotFor(VI), VT, R);
  Owner[R] = V;
  Clean |= uint64_t(1) << R;
  VI.Reg = R;
}

void OnePassCompiler::dropClean() {
  for (uint64_t M = Clean; M; M &= M - 1)
    spillReg(llvm::countr_zero(M));
}

uint8_t OnePassCompiler::allocFrom(ArrayRef<uint8_t> Order) {
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

void OnePassCompiler::claim(uint8_t R) {
  if (Owner[R] == Busy) {
    fail("register conflict");
    return;
  }
  if (Owner[R])
    spillReg(R);
  Owner[R] = Busy;
}

MCSymbol *OnePassCompiler::sym(const GlobalValue &GV, bool PreferLocal) {
  MCSymbol *&S = G.Syms[PreferLocal][&GV];
  if (!S)
    S = PreferLocal ? AP.getSymbolPreferLocal(GV) : AP.getSymbol(&GV);
  return S;
}

MCSymbol *OnePassCompiler::libSym(const char *Name) {
  MCSymbol *&S = G.LibSyms[Name];
  if (!S)
    S = Ctx.getOrCreateSymbol(Name);
  return S;
}

uint8_t OnePassCompiler::constant(const Constant *C, uint8_t R) {
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

uint8_t OnePassCompiler::use(const Value *V) {
  ZExtKnown = false;
  if (auto *C = dyn_cast<Constant>(V))
    return constant(C, NoReg);
  ValTy VT = typeOf(V);
  ValInfo &VI = info(V);
  if (VI.Kind != ValInfo::Normal)
    return useDeferred(V, VI, VT, NoReg);
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

void OnePassCompiler::useInto(const Value *V, uint8_t R) {
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
    useDeferred(V, VI, VT, R);
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

void OnePassCompiler::useIn(const Value *V, uint8_t R) {
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

bool OnePassCompiler::isTemp(const Value &V, const BasicBlock *BB) {
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

void OnePassCompiler::def(const Instruction &I, uint8_t R) {
  if (I.use_empty()) {
    release(R);
  } else if (isTemp(I, I.getParent())) {
    Owner[R] = &I;
    info(&I).Reg = R;
  } else {
    storeAndCache(&I, typeOf(&I), R);
  }
}

void OnePassCompiler::dropOperands(const Instruction &I) {
  for (const Value *V : I.operands()) {
    auto It = lookup(V);
    if (!It)
      continue;
    // A deferred address or load holds its operands' registers.
    if (It->Kind == ValInfo::DeferredGEP || It->Kind == ValInfo::DeferredLoad) {
      dropOperands(*cast<Instruction>(V));
      continue;
    }
    if (It->Reg == NoReg)
      continue;
    release(It->Reg);
    It->Reg = NoReg;
  }
}

bool OnePassCompiler::frameOffset(const Value *P, int64_t &Off) {
  auto It = lookup(P);
  if (!It)
    return false;
  if (It->Kind == ValInfo::Alloca) {
    Off = It->Slot;
    return true;
  }
  auto *GEP = dyn_cast<GetElementPtrInst>(P);
  APInt GEPOff(64, 0);
  if (It->Kind != ValInfo::DeferredGEP || !GEP ||
      !frameOffset(GEP->getPointerOperand(), Off) ||
      !GEP->accumulateConstantOffset(DL, GEPOff))
    return false;
  Off += GEPOff.getSExtValue();
  return isInt<32>(Off);
}

bool OnePassCompiler::aggParts(Type *T, SmallVectorImpl<AggPart> &Parts) {
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

void OnePassCompiler::recordCall(size_t Start, bool MayThrow) {
  HasCalls = true;
  if (MayThrow && F.hasPersonalityFn()) {
    EH.CallSites.push_back(
        {uint32_t(Start), uint32_t(Code.size() - Start), 0, 0});
    CallSiteLPad.push_back(nullptr);
  }
}

unsigned OnePassCompiler::typeIdFor(const GlobalValue *GV) {
  auto It = find(EH.TypeInfos, GV);
  if (It == EH.TypeInfos.end()) {
    EH.TypeInfos.push_back(GV);
    return EH.TypeInfos.size();
  }
  return It - EH.TypeInfos.begin() + 1;
}

void OnePassCompiler::appendSLEB(int64_t V) {
  uint8_t Buf[10];
  unsigned N = encodeSLEB128(V, Buf);
  EH.Actions.append(Buf, Buf + N);
}

static bool hasPhis(const BasicBlock *BB) { return isa<PHINode>(BB->front()); }

void OnePassCompiler::edge(const BasicBlock *Pred, const BasicBlock *Succ) {
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

void OnePassCompiler::compileAlloca(const AllocaInst &I, bool Guarded) {
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
  uint64_t Alignment = std::max<uint64_t>(I.getAlign().value(), 8);
  VI.Slot = Guarded || GuardAllAllocas ? allocGuardedFrame(Bytes, Alignment)
                                       : allocFrame(Bytes, Alignment);
}

void OnePassCompiler::compileExtractValue(const ExtractValueInst &I) {
  SmallVector<AggPart, 2> Parts;
  const Value *Agg = I.getAggregateOperand();
  auto It = lookup(Agg);
  if (I.getNumIndices() != 1 || !aggParts(Agg->getType(), Parts) || !It ||
      !It->Slot)
    return fail("extractvalue");
  unsigned Idx = I.getIndices()[0];
  ValTy VT = Parts[Idx].VT;
  int32_t Slot = It->Slot;
  uint8_t R = VT.FP ? allocFP() : allocGP();
  loadSlot(R, VT, Slot + 8 * Idx);
  def(I, R);
}

void OnePassCompiler::compileInsertValue(const InsertValueInst &I) {
  SmallVector<AggPart, 2> Parts;
  const Value *Agg = I.getAggregateOperand();
  if (I.getNumIndices() != 1 || !aggParts(I.getType(), Parts))
    return fail("insertvalue");
  unsigned Idx = I.getIndices()[0];
  int32_t Slot = aggSlotFor(&I);
  if (!isa<UndefValue>(Agg)) {
    auto It = lookup(Agg);
    if (!It || !It->Slot)
      return fail("insertvalue aggregate");
    int32_t Src = It->Slot;
    uint8_t T = allocGP();
    for (unsigned P = 0; P != Parts.size(); ++P) {
      if (P == Idx)
        continue;
      loadSlot(T, {8, false, false}, Src + 8 * P);
      storeSlot(Slot + 8 * P, {8, false, false}, T);
    }
    release(T);
  }
  uint8_t R = use(I.getInsertedValueOperand());
  storeSlot(Slot + 8 * Idx, Parts[Idx].VT, R);
  release(R);
}

uint32_t OnePassCompiler::actionFor(const LandingPadInst &LPI) {
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

bool OnePassCompiler::checkCall(const CallBase &CB) {
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

void OnePassCompiler::compileCall(const CallInst &I) {
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

void OnePassCompiler::compileIntrinsic(const IntrinsicInst &I) {
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

void OnePassCompiler::compileInvoke(const InvokeInst &I) {
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
  edge(BB, I.getNormalDest());
  jmpTo(I.getNormalDest(), /*Force=*/Stub);
  EH.CallSites[CS].Action = Action;
  if (Stub) {
    EH.CallSites[CS].LandingPad = Code.size();
    emitLandingPadEntry();
    uint8_t Ptr = exceptionPointerReg(), Sel = exceptionSelectorReg();
    claim(Ptr);
    claim(Sel);
    edge(BB, Unwind);
    release(Ptr);
    release(Sel);
    jmpTo(Unwind, /*Force=*/true);
  } else {
    CallSiteLPad[CS] = Unwind;
  }
  HasLandingPads = true;
}

void OnePassCompiler::compileLandingPad(const LandingPadInst &I) {
  SmallVector<AggPart, 2> Parts;
  if (!aggParts(I.getType(), Parts) || Parts.size() != 2 || Parts[0].VT.FP ||
      Parts[1].VT.FP)
    return fail("landing pad type");
  if (!hasPhis(I.getParent()))
    emitLandingPadEntry();
  int32_t Slot = aggSlotFor(&I);
  storeSlot(Slot, {8, false, false}, exceptionPointerReg());
  storeSlot(Slot + 8, {8, false, false}, exceptionSelectorReg());
}

void OnePassCompiler::compileResume(const ResumeInst &I) {
  auto It = lookup(I.getValue());
  if (!It || !It->Slot)
    return fail("resume value");
  uint8_t Arg = firstArgReg();
  claim(Arg);
  loadSlot(Arg, {8, false, false}, It->Slot);
  emitCallTo(nullptr, "_Unwind_Resume", /*MayThrow=*/true);
  release(Arg);
  // As for the unreachable that DwarfEHPrepare puts after the noreturn call.
  const TargetOptions &O = AP.TM.Options;
  if (O.TrapUnreachable && !O.NoTrapAfterNoreturn)
    emitTrap(/*Debug=*/false);
}

void OnePassCompiler::compileCondBr(const CondBrInst &I) {
  const BasicBlock *BB = I.getParent();
  const BasicBlock *T = I.getSuccessor(0), *E = I.getSuccessor(1);
  if (auto *CI = dyn_cast<ConstantInt>(I.getCondition())) {
    const BasicBlock *Dest = CI->isZero() ? E : T;
    edge(BB, Dest);
    return jmpTo(Dest);
  }
  unsigned CC = emitCondition(I.getCondition());
  if (T == E) {
    edge(BB, T);
    return jmpTo(T);
  }
  if (T == NextBB && !hasPhis(E)) {
    jccTo(invertCC(CC), E);
    edge(BB, T);
  } else if (!hasPhis(T)) {
    jccTo(CC, T);
    edge(BB, E);
    jmpTo(E);
  } else if (!hasPhis(E)) {
    jccTo(invertCC(CC), E);
    edge(BB, T);
    jmpTo(T);
  } else {
    size_t Pos = jccFwd(invertCC(CC));
    edge(BB, T);
    jmpTo(T, /*Force=*/true);
    bindFwd(Pos);
    edge(BB, E);
    jmpTo(E);
  }
}

void OnePassCompiler::compileSwitch(const SwitchInst &I) {
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
      jccTo(CC, Succ);
    } else {
      size_t Pos = jccFwd(invertCC(CC));
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

void OnePassCompiler::compileInst(const Instruction &I) {
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
void OnePassCompiler::recordDebugRecords(const Instruction &I,
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
    auto It = lookup(Addr);
    auto *Arg = dyn_cast<Argument>(Addr);
    int64_t Off;
    uint64_t Size = 8;
    if (It && It->Kind == ValInfo::Alloca) {
      Off = It->Slot;
      Size =
          Arg ? DL.getTypeAllocSize(Arg->getParamByValType())
              : cast<AllocaInst>(Addr)->getAllocationSize(DL)->getFixedValue();
    } else if (Arg && It && It->Kind == ValInfo::Normal && It->Slot) {
      Off = It->Slot;
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

bool OnePassCompiler::run() {
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

  // The guard goes at the frame-pointer end of the frame, followed by the
  // objects an overflow is likely to start from, as in the regular pipeline.
  if (F.hasFnAttribute(Attribute::StackProtect) ||
      F.hasFnAttribute(Attribute::StackProtectStrong) ||
      F.hasFnAttribute(Attribute::StackProtectReq)) {
    DenseMap<const AllocaInst *, MachineFrameInfo::SSPLayoutKind> Layout;
    if (SSPLayoutAnalysis::requiresStackProtector(const_cast<Function *>(&F),
                                                  &Layout)) {
      StackGuardSlot = allocGuardedFrame(8, 8);
      // "ssp" singles out only large character arrays, but an overflow of
      // any other object must not get past the guard either. The stronger
      // levels single out every array and every object whose address escapes.
      GuardAllAllocas = F.hasFnAttribute(Attribute::StackProtect);
      for (auto Kind :
           {MachineFrameInfo::SSPLK_LargeArray,
            MachineFrameInfo::SSPLK_SmallArray, MachineFrameInfo::SSPLK_AddrOf})
        for (const Instruction &I : F.getEntryBlock())
          if (auto *AI = dyn_cast<AllocaInst>(&I);
              AI && AI->isStaticAlloca() && Layout.lookup(AI) == Kind)
            compileAlloca(*AI, /*Guarded=*/true);
      if (FailReason)
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
        recordDebugRecords(I, BlockStart);
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

  if (!finish())
    return false;
  // Built only now so that a fallback leaves no new metadata behind.
  for (unsigned Idx : DerefVars)
    Debug.Variables[Idx].Expr = DIExpression::prepend(
        Debug.Variables[Idx].Expr, DIExpression::DerefBefore);
  return true;
}

OnePassCodeGen::OnePassCodeGen(char &ID, TargetMachine &TM,
                               const OnePassCodeGenOptions &Opts,
                               const void *AsmPrinterID)
    : FunctionPass(ID), TM(TM), Opts(Opts), AsmPrinterID(AsmPrinterID) {}

void OnePassCodeGen::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.setPreservesAll();
}

bool OnePassCodeGen::doInitialization(Module &M) {
  G.reset(*this, M);
  AP = nullptr;
  ModuleState = Unknown;
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

bool OnePassCodeGen::finish(const Function &F, OnePassCompiler &C) {
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
  AP->emitCompiledFunction(F, {C.Code, C.Fixups, C.CFI, C.StackSize,
                               C.HasLandingPads ? &C.EH : nullptr,
                               C.HasDebug ? &C.Debug : nullptr});
  return true;
}

bool OnePassCodeGen::runOnFunction(Function &F) {
  if (ModuleState == Unknown) {
    const char *Reason = nullptr;
    AP = findAsmPrinter(getResolver()->getPMDataManager(), AsmPrinterID);
    if (!AP) {
      Reason = "no AsmPrinter in the function pass manager";
    } else if (AP->OutStreamer->hasRawTextSupport()) {
      Reason = "not an object streamer";
    } else {
      auto &OS = static_cast<MCObjectStreamer &>(*AP->OutStreamer);
      const Module &M = *F.getParent();
      const TargetOptions &O = TM.Options;
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
                                             "debugger-tuning",
                                             "EnableValueProfiling",
                                             "MemProfProfileFilename"};
      SmallVector<Module::ModuleFlagEntry, 8> Flags;
      M.getModuleFlagsMetadata(Flags);
      if (O.EmitCallSiteInfo || O.EmitCallGraphSection || O.BBAddrMap ||
          O.BBSections != BasicBlockSection::None ||
          O.FunctionSplitting == FunctionSplittingMode::All ||
          O.ForceDwarfFrameSection || O.MCOptions.EmitSFrameUnwind)
        Reason = "target option";
      else if ((Reason = unsupportedModuleReason(M, OS)))
        ;
      else if (EnableFSDiscriminator)
        Reason = "codegen option";
      // Pre-encoded bytes bypass branch-alignment padding.
      else if (OS.getAssembler().getBackend().allowAutoPadding())
        Reason = "branch alignment padding";
      else if (M.getContext().getMainRemarkStreamer() ||
               M.getContext().getDiagHandlerPtr()->isAnyRemarkEnabled())
        Reason = "remarks";
      else if (M.getNamedMetadata(PseudoProbeDescMetadataName))
        Reason = "pseudo probes";
      else if (any_of(Flags, [&](const Module::ModuleFlagEntry &Flag) {
                 StringRef Key = Flag.Key->getString();
                 // "nosanitize_*" marks a module a sanitizer has instrumented.
                 return !is_contained(KnownFlags, Key) &&
                        !Key.starts_with("nosanitize_") &&
                        !isHandledModuleFlag(Flag);
               }))
        Reason = "module flag";
    }
    ModuleState = Reason ? Unsupported : Supported;
    if (Reason && (Opts.Report || Opts.Abort))
      errs() << "onepass: module not supported: " << Reason << '\n';
  }
  if (ModuleState == Unsupported || F.hasAvailableExternallyLinkage() ||
      NumDone >= Opts.Limit)
    return false;
  FailReason = unsupportedSubtargetReason(F);
  FailDetail.clear();
  auto &OS = static_cast<MCObjectStreamer &>(*AP->OutStreamer);
  if (FailReason || !compile(F, *AP, OS.getAssembler().getEmitter(), G)) {
    ++NumFallback;
    if (Opts.Report || Opts.Abort)
      errs() << "onepass: " << F.getName() << ": " << FailReason
             << (FailDetail.empty() ? "" : " " + FailDetail) << '\n';
    if (Opts.Abort)
      reportFatalUsageError("one-pass compilation failed");
    return false;
  }
  ++NumCompiled;
  ++NumDone;
  static_cast<FPPassManager &>(getResolver()->getPMDataManager())
      .skipRemainingPasses();
  return false;
}
