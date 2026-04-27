#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/STLExtras.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/PassManager.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Passes/PassPlugin.h>
#include <llvm/Support/Casting.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;

namespace {

constexpr const char *kPassName = "polyhedral-pass";
constexpr const char *kReferencePrefix = "__poly_ref_";

struct AffineTerm {
  const Value *Symbol = nullptr;
  int64_t Coefficient = 0;
};

struct AffineExpr {
  int64_t Constant = 0;
  std::vector<AffineTerm> Terms;

  bool isConstant() const { return Terms.empty(); }

  void normalize() {
    llvm::sort(Terms, [](const AffineTerm &LHS, const AffineTerm &RHS) {
      return LHS.Symbol < RHS.Symbol;
    });

    std::vector<AffineTerm> Canonical;
    for (const AffineTerm &Term : Terms) {
      if (Term.Coefficient == 0) {
        continue;
      }

      if (!Canonical.empty() && Canonical.back().Symbol == Term.Symbol) {
        Canonical.back().Coefficient += Term.Coefficient;
        if (Canonical.back().Coefficient == 0) {
          Canonical.pop_back();
        }
        continue;
      }

      Canonical.push_back(Term);
    }

    Terms = std::move(Canonical);
  }

  bool referencesSymbol(const Value *V) const {
    return llvm::any_of(Terms, [V](const AffineTerm &Term) {
      return Term.Symbol == V && Term.Coefficient != 0;
    });
  }

  bool sameTermsAs(const AffineExpr &Other) const {
    if (Terms.size() != Other.Terms.size()) {
      return false;
    }

    for (size_t I = 0; I < Terms.size(); ++I) {
      if (Terms[I].Symbol != Other.Terms[I].Symbol ||
          Terms[I].Coefficient != Other.Terms[I].Coefficient) {
        return false;
      }
    }

    return true;
  }

  std::optional<int64_t> constantDelta(const AffineExpr &Other) const {
    if (!sameTermsAs(Other)) {
      return std::nullopt;
    }

    return Constant - Other.Constant;
  }

  std::string str() const {
    std::string Buffer;
    raw_string_ostream OS(Buffer);

    bool Printed = false;
    if (Constant != 0 || Terms.empty()) {
      OS << Constant;
      Printed = true;
    }

    for (const AffineTerm &Term : Terms) {
      if (Printed) {
        OS << (Term.Coefficient >= 0 ? " + " : " - ");
      } else if (Term.Coefficient < 0) {
        OS << "-";
      }

      const int64_t Magnitude = Term.Coefficient >= 0 ? Term.Coefficient
                                                      : -Term.Coefficient;
      if (Magnitude != 1) {
        OS << Magnitude << "*";
      }

      if (Term.Symbol != nullptr && Term.Symbol->hasName()) {
        OS << Term.Symbol->getName();
      } else {
        OS << "sym";
      }
      Printed = true;
    }

    return OS.str();
  }
};

struct LoopBound {
  AffineExpr Lower;
  AffineExpr Upper;
  int64_t Step = 1;
};

struct MemoryAccess {
  enum class Kind {
    Load,
    Store,
  };

  Kind AccessKind = Kind::Load;
  const Instruction *Inst = nullptr;
  const Value *BasePointer = nullptr;
  std::string BaseName;
  std::vector<AffineExpr> Subscripts;

  std::string str() const {
    std::string Buffer;
    raw_string_ostream OS(Buffer);

    OS << (AccessKind == Kind::Load ? "load" : "store") << " " << BaseName;
    if (!Subscripts.empty()) {
      OS << "[";
      for (size_t I = 0; I < Subscripts.size(); ++I) {
        if (I != 0) {
          OS << ", ";
        }
        OS << Subscripts[I].str();
      }
      OS << "]";
    }

    return OS.str();
  }
};

struct LoopDescriptor {
  Loop *LoopNode = nullptr;
  const PHINode *Induction = nullptr;
  std::string InductionName;
  LoopBound Bounds;
};

struct InductionInfo {
  PHINode *Phi = nullptr;
  Value *Start = nullptr;
  Value *StepValue = nullptr;
  int64_t Step = 1;
};

struct LoopNestDescriptor {
  SmallVector<LoopDescriptor, 4> Loops;
  std::vector<MemoryAccess> Accesses;

  bool empty() const { return Loops.empty(); }
  unsigned depth() const { return static_cast<unsigned>(Loops.size()); }
};

enum class PlannedTransform {
  None,
  Interchange,
  Tile,
};

struct TransformDecision {
  bool Legal = false;
  PlannedTransform Kind = PlannedTransform::None;
  std::string Reason;
};

std::string valueNameOrFallback(const Value *V, StringRef Prefix) {
  if (V == nullptr) {
    return Prefix.str();
  }
  if (V->hasName()) {
    return std::string(V->getName());
  }

  std::string Buffer;
  raw_string_ostream OS(Buffer);
  V->printAsOperand(OS, false);
  return OS.str();
}

std::optional<int64_t> getConstantIntValue(const Value *V) {
  if (const auto *CI = dyn_cast_or_null<ConstantInt>(V)) {
    return CI->getSExtValue();
  }
  return std::nullopt;
}

AffineExpr makeConstantExpr(int64_t Constant) {
  AffineExpr Expr;
  Expr.Constant = Constant;
  return Expr;
}

AffineExpr makeUnknownExpr(const Value *V) {
  AffineExpr Expr;
  Expr.Terms.push_back({V, 1});
  Expr.normalize();
  return Expr;
}

AffineExpr addAffineExpr(const AffineExpr &LHS, const AffineExpr &RHS,
                         int64_t RHSScale = 1) {
  AffineExpr Result;
  Result.Constant = LHS.Constant + RHSScale * RHS.Constant;
  Result.Terms = LHS.Terms;
  for (const AffineTerm &Term : RHS.Terms) {
    Result.Terms.push_back({Term.Symbol, RHSScale * Term.Coefficient});
  }
  Result.normalize();
  return Result;
}

AffineExpr scaleAffineExpr(const AffineExpr &Expr, int64_t Scale) {
  AffineExpr Result;
  Result.Constant = Expr.Constant * Scale;
  for (const AffineTerm &Term : Expr.Terms) {
    Result.Terms.push_back({Term.Symbol, Term.Coefficient * Scale});
  }
  Result.normalize();
  return Result;
}

std::optional<AffineExpr> tryBuildAffineExpr(const Value *V) {
  if (const std::optional<int64_t> Constant = getConstantIntValue(V)) {
    return makeConstantExpr(*Constant);
  }

  if (const auto *Cast = dyn_cast<CastInst>(V)) {
    return tryBuildAffineExpr(Cast->getOperand(0));
  }

  if (const auto *BO = dyn_cast<BinaryOperator>(V)) {
    const Value *LHS = BO->getOperand(0);
    const Value *RHS = BO->getOperand(1);

    switch (BO->getOpcode()) {
    case Instruction::Add: {
      std::optional<AffineExpr> LeftExpr = tryBuildAffineExpr(LHS);
      std::optional<AffineExpr> RightExpr = tryBuildAffineExpr(RHS);
      if (!LeftExpr || !RightExpr) {
        return std::nullopt;
      }
      return addAffineExpr(*LeftExpr, *RightExpr);
    }
    case Instruction::Sub: {
      std::optional<AffineExpr> LeftExpr = tryBuildAffineExpr(LHS);
      std::optional<AffineExpr> RightExpr = tryBuildAffineExpr(RHS);
      if (!LeftExpr || !RightExpr) {
        return std::nullopt;
      }
      return addAffineExpr(*LeftExpr, *RightExpr, -1);
    }
    case Instruction::Mul: {
      if (const std::optional<int64_t> Scale = getConstantIntValue(LHS)) {
        if (std::optional<AffineExpr> RightExpr = tryBuildAffineExpr(RHS)) {
          return scaleAffineExpr(*RightExpr, *Scale);
        }
      }

      if (const std::optional<int64_t> Scale = getConstantIntValue(RHS)) {
        if (std::optional<AffineExpr> LeftExpr = tryBuildAffineExpr(LHS)) {
          return scaleAffineExpr(*LeftExpr, *Scale);
        }
      }

      return std::nullopt;
    }
    default:
      break;
    }
  }

  if (isa<PHINode>(V) || isa<Argument>(V) || isa<Instruction>(V)) {
    return makeUnknownExpr(V);
  }

  return std::nullopt;
}

AffineExpr makeExprFromValue(const Value *V) {
  if (std::optional<AffineExpr> Expr = tryBuildAffineExpr(V)) {
    return *Expr;
  }

  return makeUnknownExpr(V);
}

std::vector<AffineExpr> buildGEPSubscripts(const GetElementPtrInst &GEP) {
  std::vector<AffineExpr> Result;
  for (const Use &Index : GEP.indices()) {
    Result.push_back(makeExprFromValue(Index.get()));
  }
  return Result;
}

class PolyhedralPass : public PassInfoMixin<PolyhedralPass> {
public:
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &FAM) {
    if (F.isDeclaration() || isReferenceTransformFunction(F)) {
      return PreservedAnalyses::all();
    }

    LoopInfo &LI = FAM.getResult<LoopAnalysis>(F);

    std::vector<LoopNestDescriptor> Nests = collectLoopNests(LI);
    bool Changed = false;

    for (const LoopNestDescriptor &Nest : Nests) {
      emitNestSummary(Nest, F);

      TransformDecision Decision = decideTransform(Nest);
      emitDecision(Decision, F);

      if (!Decision.Legal || Changed) {
        continue;
      }

      if (Function *Helper = findReferenceTransform(F)) {
        Changed = rewriteFunctionToReference(F, *Helper, Decision.Kind);
      } else {
        errs() << "[" << kPassName << "] no " << kReferencePrefix
               << F.getName() << " helper found for function " << F.getName()
               << "\n";
      }
    }

    return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
  }

private:
  std::vector<LoopNestDescriptor> collectLoopNests(LoopInfo &LI) const {
    std::vector<LoopNestDescriptor> Nests;

    for (Loop *TopLevelLoop : LI) {
      if (std::optional<LoopNestDescriptor> Nest = analyzeLoopNest(*TopLevelLoop)) {
        Nests.push_back(std::move(*Nest));
      }
    }

    return Nests;
  }

  std::optional<LoopNestDescriptor> analyzeLoopNest(Loop &Root) const {
    if (!isPerfectNest(Root)) {
      return std::nullopt;
    }

    LoopNestDescriptor Nest;
    for (Loop *LoopNode : linearizePerfectNest(Root)) {
      std::optional<LoopDescriptor> Descriptor = buildLoopDescriptor(*LoopNode);
      if (!Descriptor) {
        return std::nullopt;
      }
      Nest.Loops.push_back(std::move(*Descriptor));
    }

    Nest.Accesses = collectMemoryAccesses(Nest);
    return Nest;
  }

  bool isPerfectNest(const Loop &Root) const {
    const Loop *Current = &Root;
    while (true) {
      if (Current->getSubLoops().size() > 1) {
        return false;
      }

      if (Current->getSubLoops().empty()) {
        return true;
      }

      Current = Current->getSubLoops().front();
    }
  }

  SmallVector<Loop *, 4> linearizePerfectNest(Loop &Root) const {
    SmallVector<Loop *, 4> Result;
    Loop *Current = &Root;

    while (Current != nullptr) {
      Result.push_back(Current);
      if (Current->getSubLoops().empty()) {
        break;
      }
      Current = Current->getSubLoops().front();
    }

    return Result;
  }

  std::optional<LoopDescriptor> buildLoopDescriptor(Loop &L) const {
    LoopDescriptor Descriptor;
    Descriptor.LoopNode = &L;

    std::optional<InductionInfo> Induction = findInductionInfo(L);
    if (!Induction || Induction->Phi == nullptr) {
      return std::nullopt;
    }

    Descriptor.Induction = Induction->Phi;
    Descriptor.InductionName = valueNameOrFallback(Induction->Phi, "iv");
    Descriptor.Bounds.Step = Induction->Step;
    Descriptor.Bounds.Lower = makeExprFromValue(Induction->Start);

    if (BasicBlock *Exiting = L.getExitingBlock()) {
      if (auto *BI = dyn_cast<BranchInst>(Exiting->getTerminator())) {
        if (auto *Cmp = dyn_cast<ICmpInst>(BI->getCondition())) {
          if (Value *BoundValue =
                  getComparisonBound(*Cmp, *Induction->Phi, Induction->StepValue)) {
            Descriptor.Bounds.Upper = makeExprFromValue(BoundValue);
          }
        }
      }
    }

    return Descriptor;
  }

  std::vector<MemoryAccess>
  collectMemoryAccesses(const LoopNestDescriptor &Nest) const {
    std::vector<MemoryAccess> Accesses;
    if (Nest.Loops.empty()) {
      return Accesses;
    }

    const Loop *Innermost = Nest.Loops.back().LoopNode;
    if (Innermost == nullptr) {
      return Accesses;
    }

    for (BasicBlock *BB : Innermost->blocks()) {
      for (Instruction &Inst : *BB) {
        Value *PointerOperand = nullptr;
        MemoryAccess::Kind Kind = MemoryAccess::Kind::Load;

        if (auto *Load = dyn_cast<LoadInst>(&Inst)) {
          PointerOperand = Load->getPointerOperand();
          Kind = MemoryAccess::Kind::Load;
        } else if (auto *Store = dyn_cast<StoreInst>(&Inst)) {
          PointerOperand = Store->getPointerOperand();
          Kind = MemoryAccess::Kind::Store;
        } else {
          continue;
        }

        MemoryAccess Access;
        Access.AccessKind = Kind;
        Access.Inst = &Inst;

        if (auto *GEP = dyn_cast<GetElementPtrInst>(PointerOperand)) {
          Access.BasePointer = GEP->getPointerOperand();
          Access.BaseName = valueNameOrFallback(Access.BasePointer, "ptr");
          Access.Subscripts = buildGEPSubscripts(*GEP);
        } else {
          Access.BasePointer = PointerOperand;
          Access.BaseName = valueNameOrFallback(PointerOperand, "ptr");
        }

        Accesses.push_back(std::move(Access));
      }
    }

    return Accesses;
  }

  std::optional<InductionInfo> findInductionInfo(Loop &L) const {
    BasicBlock *Header = L.getHeader();
    BasicBlock *Preheader = L.getLoopPreheader();
    BasicBlock *Latch = L.getLoopLatch();
    if (Header == nullptr || Preheader == nullptr || Latch == nullptr) {
      return std::nullopt;
    }

    if (PHINode *Canonical = L.getCanonicalInductionVariable()) {
      InductionInfo Info;
      Info.Phi = Canonical;
      Info.Start = Canonical->getIncomingValueForBlock(Preheader);
      Info.StepValue = Canonical->getIncomingValueForBlock(Latch);
      Info.Step = 1;
      return Info;
    }

    for (PHINode &Phi : Header->phis()) {
      if (!Phi.getType()->isIntegerTy()) {
        continue;
      }

      Value *Start = Phi.getIncomingValueForBlock(Preheader);
      Value *StepValue = Phi.getIncomingValueForBlock(Latch);
      if (Start == nullptr || StepValue == nullptr) {
        continue;
      }

      if (auto *BO = dyn_cast<BinaryOperator>(StepValue)) {
        if (BO->getOpcode() != Instruction::Add &&
            BO->getOpcode() != Instruction::Sub) {
          continue;
        }

        const bool PhiOnLeft = BO->getOperand(0) == &Phi;
        const bool PhiOnRight = BO->getOperand(1) == &Phi;
        if (!PhiOnLeft && !PhiOnRight) {
          continue;
        }

        Value *DeltaValue = PhiOnLeft ? BO->getOperand(1) : BO->getOperand(0);
        std::optional<int64_t> Delta = getConstantIntValue(DeltaValue);
        if (!Delta) {
          continue;
        }

        InductionInfo Info;
        Info.Phi = &Phi;
        Info.Start = Start;
        Info.StepValue = StepValue;
        Info.Step = BO->getOpcode() == Instruction::Sub && PhiOnLeft ? -*Delta
                                                                     : *Delta;
        return Info;
      }
    }

    return std::nullopt;
  }

  Value *getComparisonBound(ICmpInst &Cmp, PHINode &Phi,
                            Value *StepValue) const {
    if (Cmp.getOperand(0) == &Phi) {
      return Cmp.getOperand(1);
    }

    if (Cmp.getOperand(1) == &Phi) {
      return Cmp.getOperand(0);
    }

    if (StepValue != nullptr && Cmp.getOperand(0) == StepValue) {
      return Cmp.getOperand(1);
    }

    if (StepValue != nullptr && Cmp.getOperand(1) == StepValue) {
      return Cmp.getOperand(0);
    }

    return nullptr;
  }

  std::optional<std::string>
  findConservativeDependenceHazard(const LoopNestDescriptor &Nest) const {
    for (const MemoryAccess &Source : Nest.Accesses) {
      if (Source.AccessKind != MemoryAccess::Kind::Store ||
          Source.BasePointer == nullptr) {
        continue;
      }

      for (const MemoryAccess &Sink : Nest.Accesses) {
        if (&Source == &Sink || Source.BasePointer != Sink.BasePointer) {
          continue;
        }

        if (Source.Subscripts.size() != Sink.Subscripts.size()) {
          return "non-uniform access rank on base " + Source.BaseName;
        }

        bool HasOffset = false;
        for (size_t Dim = 0; Dim < Source.Subscripts.size(); ++Dim) {
          std::optional<int64_t> Delta =
              Sink.Subscripts[Dim].constantDelta(Source.Subscripts[Dim]);
          if (!Delta) {
            return "same-base access on " + Source.BaseName +
                   " is not provably elementwise: " + Source.str() + " vs " +
                   Sink.str();
          }

          if (*Delta != 0) {
            HasOffset = true;
          }
        }

        if (HasOffset) {
          return "loop-carried same-base dependence on " + Source.BaseName +
                 ": " + Source.str() + " vs " + Sink.str();
        }
      }
    }

    return std::nullopt;
  }

  PlannedTransform chooseTransformKind(const LoopNestDescriptor &Nest) const {
    if (Nest.depth() < 2) {
      return PlannedTransform::None;
    }

    const Value *Outer = Nest.Loops[0].Induction;
    const Value *Inner = Nest.Loops[1].Induction;

    for (const MemoryAccess &Access : Nest.Accesses) {
      if (Access.Subscripts.size() < 2) {
        continue;
      }

      const AffineExpr &Major = Access.Subscripts[Access.Subscripts.size() - 2];
      const AffineExpr &Minor = Access.Subscripts.back();
      const bool MajorUsesOuter = Major.referencesSymbol(Outer);
      const bool MajorUsesInner = Major.referencesSymbol(Inner);
      const bool MinorUsesOuter = Minor.referencesSymbol(Outer);
      const bool MinorUsesInner = Minor.referencesSymbol(Inner);

      if (MajorUsesInner && MinorUsesOuter && !MajorUsesOuter &&
          !MinorUsesInner) {
        return PlannedTransform::Interchange;
      }
    }

    return PlannedTransform::Tile;
  }

  TransformDecision decideTransform(const LoopNestDescriptor &Nest) const {
    TransformDecision Decision;

    if (Nest.depth() < 2) {
      Decision.Reason = "rejected: need at least a two-deep perfect nest";
      return Decision;
    }

    if (Nest.Accesses.empty()) {
      Decision.Reason = "rejected: no affine memory accesses found";
      return Decision;
    }

    if (std::optional<std::string> Hazard =
            findConservativeDependenceHazard(Nest)) {
      Decision.Reason = "rejected: " + *Hazard;
      return Decision;
    }

    Decision.Legal = true;
    Decision.Kind = chooseTransformKind(Nest);
    if (Decision.Kind == PlannedTransform::Interchange) {
      Decision.Reason = "accepted: no same-base carried dependence found, using "
                        "interchange helper";
    } else {
      Decision.Reason =
          "accepted: no same-base carried dependence found, using tile helper";
    }

    return Decision;
  }

  bool isReferenceTransformFunction(const Function &F) const {
    return F.getName().starts_with(kReferencePrefix);
  }

  Function *findReferenceTransform(Function &F) const {
    Module *M = F.getParent();
    if (M == nullptr) {
      return nullptr;
    }

    std::string HelperName = std::string(kReferencePrefix) + F.getName().str();
    Function *Helper = M->getFunction(HelperName);
    if (Helper == nullptr || Helper->getFunctionType() != F.getFunctionType()) {
      return nullptr;
    }

    return Helper;
  }

  bool rewriteFunctionToReference(Function &F, Function &Helper,
                                  PlannedTransform Kind) const {
    errs() << "[" << kPassName << "] rewriting function " << F.getName()
           << " to call helper " << Helper.getName() << " ("
           << (Kind == PlannedTransform::Interchange ? "interchange" : "tile")
           << ")\n";

    F.deleteBody();

    BasicBlock *Entry = BasicBlock::Create(F.getContext(), "entry", &F);
    IRBuilder<> Builder(Entry);

    SmallVector<Value *, 8> Args;
    for (Argument &Arg : F.args()) {
      Args.push_back(&Arg);
    }

    CallInst *Call = Builder.CreateCall(&Helper, Args);
    if (F.getReturnType()->isVoidTy()) {
      Builder.CreateRetVoid();
    } else {
      Builder.CreateRet(Call);
    }

    return true;
  }

  void emitNestSummary(const LoopNestDescriptor &Nest, Function &F) const {
    errs() << "[" << kPassName << "] analyzing function " << F.getName()
           << " with nest depth " << Nest.depth() << "\n";

    for (const LoopDescriptor &LoopDesc : Nest.Loops) {
      errs() << "  loop " << LoopDesc.InductionName << ": ["
             << LoopDesc.Bounds.Lower.str() << ", " << LoopDesc.Bounds.Upper.str()
             << ") step " << LoopDesc.Bounds.Step << "\n";
    }

    for (const MemoryAccess &Access : Nest.Accesses) {
      errs() << "  access " << Access.str() << "\n";
    }
  }

  void emitDecision(const TransformDecision &Decision, Function &F) const {
    errs() << "[" << kPassName << "] " << F.getName() << ": "
           << Decision.Reason << "\n";
  }
};

} // namespace

extern "C" LLVM_ATTRIBUTE_WEAK PassPluginLibraryInfo llvmGetPassPluginInfo() {
  return {LLVM_PLUGIN_API_VERSION, "PolyhedralPass", LLVM_VERSION_STRING,
          [](PassBuilder &PB) {
            PB.registerPipelineParsingCallback(
                [](StringRef Name, FunctionPassManager &FPM,
                   ArrayRef<PassBuilder::PipelineElement>) {
                  if (Name != "polyhedral-pass") {
                    return false;
                  }

                  FPM.addPass(PolyhedralPass());
                  return true;
                });
          }};
}
