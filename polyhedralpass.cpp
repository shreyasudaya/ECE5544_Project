#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/Analysis/AliasAnalysis.h>
#include <llvm/Analysis/AssumptionCache.h>
#include <llvm/Analysis/DependenceAnalysis.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/Analysis/LoopNestAnalysis.h>
#include <llvm/Analysis/ScalarEvolution.h>
#include <llvm/Analysis/TargetLibraryInfo.h>
#include <llvm/Analysis/TargetTransformInfo.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/PassManager.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Passes/PassPlugin.h>
#include <llvm/Support/Casting.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Transforms/Scalar/LoopPassManager.h>
#include <llvm/Transforms/Utils/UnrollLoop.h>

using namespace llvm;

namespace {

constexpr const char *kPassName = "polyhedral-pass";

cl::opt<unsigned> PolyTileSize(
    "poly-tile-size",
    cl::desc("Blocking factor used by the polyhedral pass"),
    cl::init(8));

cl::opt<bool> PolyEnableBlocking(
    "poly-enable-blocking",
    cl::desc("Enable the blocking stage after affine legality checks"),
    cl::init(true));

struct AffineTerm {
  const Value *Symbol = nullptr;
  int64_t Coefficient = 0;
};

struct AffineExpr {
  int64_t Constant = 0;
  std::vector<AffineTerm> Terms;

  bool isConstant() const { return Terms.empty(); }

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
        OS << " + ";
      }

      if (Term.Symbol != nullptr && Term.Symbol->hasName()) {
        OS << Term.Coefficient << "*" << Term.Symbol->getName();
      } else {
        OS << Term.Coefficient << "*sym";
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
  Instruction *Inst = nullptr;
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
  PHINode *Induction = nullptr;
  std::string InductionName;
  LoopBound Bounds;
  Value *StartValue = nullptr;
  Value *BoundValue = nullptr;
  ICmpInst *Compare = nullptr;
  ICmpInst::Predicate Predicate = ICmpInst::BAD_ICMP_PREDICATE;
};

struct LoopNestDescriptor {
  SmallVector<LoopDescriptor, 4> Loops;
  std::vector<MemoryAccess> Accesses;

  bool empty() const { return Loops.empty(); }
  unsigned depth() const { return static_cast<unsigned>(Loops.size()); }
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

void addTerm(AffineExpr &Expr, const Value *Symbol, int64_t Coefficient) {
  if (Coefficient == 0 || Symbol == nullptr) {
    return;
  }

  for (AffineTerm &Term : Expr.Terms) {
    if (Term.Symbol == Symbol) {
      Term.Coefficient += Coefficient;
      return;
    }
  }

  Expr.Terms.push_back({Symbol, Coefficient});
}

AffineExpr addExpr(const AffineExpr &LHS, const AffineExpr &RHS) {
  AffineExpr Result;
  Result.Constant = LHS.Constant + RHS.Constant;
  Result.Terms = LHS.Terms;
  for (const AffineTerm &Term : RHS.Terms) {
    addTerm(Result, Term.Symbol, Term.Coefficient);
  }
  return Result;
}

AffineExpr negateExpr(const AffineExpr &Expr) {
  AffineExpr Result;
  Result.Constant = -Expr.Constant;
  for (const AffineTerm &Term : Expr.Terms) {
    addTerm(Result, Term.Symbol, -Term.Coefficient);
  }
  return Result;
}

AffineExpr subExpr(const AffineExpr &LHS, const AffineExpr &RHS) {
  return addExpr(LHS, negateExpr(RHS));
}

AffineExpr mulExpr(const AffineExpr &Expr, int64_t Factor) {
  AffineExpr Result;
  Result.Constant = Expr.Constant * Factor;
  for (const AffineTerm &Term : Expr.Terms) {
    addTerm(Result, Term.Symbol, Term.Coefficient * Factor);
  }
  return Result;
}

AffineExpr makeUnknownExpr(const Value *V) {
  AffineExpr Expr;
  addTerm(Expr, V, 1);
  return Expr;
}

AffineExpr makeExprFromValue(const Value *V) {
  if (const std::optional<int64_t> Constant = getConstantIntValue(V)) {
    AffineExpr Expr;
    Expr.Constant = *Constant;
    return Expr;
  }

  if (const auto *Cast = dyn_cast<CastInst>(V)) {
    return makeExprFromValue(Cast->getOperand(0));
  }

  if (const auto *BO = dyn_cast<BinaryOperator>(V)) {
    const Value *Op0 = BO->getOperand(0);
    const Value *Op1 = BO->getOperand(1);

    switch (BO->getOpcode()) {
    case Instruction::Add:
      return addExpr(makeExprFromValue(Op0), makeExprFromValue(Op1));
    case Instruction::Sub:
      return subExpr(makeExprFromValue(Op0), makeExprFromValue(Op1));
    case Instruction::Mul:
      if (const std::optional<int64_t> C = getConstantIntValue(Op0)) {
        return mulExpr(makeExprFromValue(Op1), *C);
      }
      if (const std::optional<int64_t> C = getConstantIntValue(Op1)) {
        return mulExpr(makeExprFromValue(Op0), *C);
      }
      break;
    default:
      break;
    }
  }

  return makeUnknownExpr(V);
}

bool exprMentionsSymbol(const AffineExpr &Expr, const Value *Symbol) {
  return llvm::any_of(Expr.Terms, [&](const AffineTerm &Term) {
    return Term.Symbol == Symbol && Term.Coefficient != 0;
  });
}

bool exprEquals(const AffineExpr &LHS, const AffineExpr &RHS) {
  AffineExpr Delta = subExpr(LHS, RHS);
  if (Delta.Constant != 0) {
    return false;
  }

  return llvm::all_of(Delta.Terms, [](const AffineTerm &Term) {
    return Term.Coefficient == 0;
  });
}

std::optional<unsigned> getConstantTripCount(const LoopDescriptor &LoopDesc) {
  if (!LoopDesc.Bounds.Lower.isConstant() || !LoopDesc.Bounds.Upper.isConstant() ||
      LoopDesc.Bounds.Step <= 0) {
    return std::nullopt;
  }

  int64_t Span = LoopDesc.Bounds.Upper.Constant - LoopDesc.Bounds.Lower.Constant;
  if (Span <= 0 || (Span % LoopDesc.Bounds.Step) != 0) {
    return std::nullopt;
  }

  return static_cast<unsigned>(Span / LoopDesc.Bounds.Step);
}

int findSubscriptPosition(const MemoryAccess &Access, const Value *Symbol) {
  for (int I = static_cast<int>(Access.Subscripts.size()) - 1; I >= 0; --I) {
    if (exprMentionsSymbol(Access.Subscripts[static_cast<size_t>(I)], Symbol)) {
      return I + 1;
    }
  }

  return 0;
}

struct FlattenedGEP {
  const Value *BasePointer = nullptr;
  std::vector<AffineExpr> Subscripts;
};

FlattenedGEP flattenGEP(const Value *PointerOperand) {
  if (const auto *GEP = dyn_cast<GetElementPtrInst>(PointerOperand)) {
    FlattenedGEP Flattened = flattenGEP(GEP->getPointerOperand());
    if (Flattened.BasePointer == nullptr) {
      Flattened.BasePointer = GEP->getPointerOperand();
    }
    for (const Use &Index : GEP->indices()) {
      Flattened.Subscripts.push_back(makeExprFromValue(Index.get()));
    }
    return Flattened;
  }

  FlattenedGEP Base;
  Base.BasePointer = PointerOperand;
  return Base;
}

std::optional<int64_t> getUnitStrideStep(PHINode *Induction, Loop *L) {
  if (Induction == nullptr || L == nullptr) {
    return std::nullopt;
  }

  BasicBlock *Latch = L->getLoopLatch();
  if (Latch == nullptr) {
    return std::nullopt;
  }

  Value *BackedgeValue = Induction->getIncomingValueForBlock(Latch);
  auto *BO = dyn_cast_or_null<BinaryOperator>(BackedgeValue);
  if (BO == nullptr) {
    return std::nullopt;
  }

  if (BO->getOpcode() == Instruction::Add) {
    if (BO->getOperand(0) == Induction) {
      return getConstantIntValue(BO->getOperand(1));
    }
    if (BO->getOperand(1) == Induction) {
      return getConstantIntValue(BO->getOperand(0));
    }
  }

  if (BO->getOpcode() == Instruction::Sub && BO->getOperand(0) == Induction) {
    if (const std::optional<int64_t> C = getConstantIntValue(BO->getOperand(1))) {
      return -*C;
    }
  }

  return std::nullopt;
}

std::optional<LoopDescriptor> buildLoopDescriptor(Loop &L) {
  LoopDescriptor Descriptor;
  Descriptor.LoopNode = &L;

  PHINode *Induction = L.getCanonicalInductionVariable();
  if (Induction == nullptr) {
    return std::nullopt;
  }

  Descriptor.Induction = Induction;
  Descriptor.InductionName = valueNameOrFallback(Induction, "iv");

  BasicBlock *Preheader = L.getLoopPreheader();
  BasicBlock *Exiting = L.getExitingBlock();
  if (Preheader == nullptr || Exiting == nullptr) {
    return std::nullopt;
  }

  Descriptor.StartValue = Induction->getIncomingValueForBlock(Preheader);
  if (Descriptor.StartValue == nullptr) {
    return std::nullopt;
  }
  Descriptor.Bounds.Lower = makeExprFromValue(Descriptor.StartValue);

  const std::optional<int64_t> Step = getUnitStrideStep(Induction, &L);
  if (!Step) {
    return std::nullopt;
  }
  Descriptor.Bounds.Step = *Step;

  auto *BI = dyn_cast<BranchInst>(Exiting->getTerminator());
  if (BI == nullptr || !BI->isConditional()) {
    return std::nullopt;
  }

  auto *Cmp = dyn_cast<ICmpInst>(BI->getCondition());
  if (Cmp == nullptr) {
    return std::nullopt;
  }

  if (Cmp->getOperand(0) != Induction) {
    return std::nullopt;
  }

  switch (Cmp->getPredicate()) {
  case ICmpInst::ICMP_SLT:
  case ICmpInst::ICMP_ULT:
    break;
  default:
    return std::nullopt;
  }

  Descriptor.Compare = Cmp;
  Descriptor.Predicate = Cmp->getPredicate();
  Descriptor.BoundValue = Cmp->getOperand(1);
  Descriptor.Bounds.Upper = makeExprFromValue(Descriptor.BoundValue);
  return Descriptor;
}

SmallVector<Loop *, 4> linearizePerfectNest(Loop &Root) {
  SmallVector<Loop *, 4> Result;
  Loop *Current = &Root;
  Result.push_back(Current);

  while (!Current->getSubLoops().empty()) {
    if (Current->getSubLoops().size() != 1) {
      break;
    }
    Loop *Child = Current->getSubLoops().front();
    Result.push_back(Child);
    Current = Child;
  }

  return Result;
}

std::vector<MemoryAccess> collectMemoryAccesses(Loop *Innermost) {
  std::vector<MemoryAccess> Accesses;
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

        FlattenedGEP Flattened = flattenGEP(PointerOperand);
        Access.BasePointer = Flattened.BasePointer;
        Access.BaseName = valueNameOrFallback(Access.BasePointer, "ptr");
        Access.Subscripts = std::move(Flattened.Subscripts);

        Accesses.push_back(std::move(Access));
      }
  }

  return Accesses;
}

std::vector<MemoryAccess>
collectMemoryAccesses(ArrayRef<LoopDescriptor> Loops) {
  if (Loops.empty()) {
    return {};
  }
  return collectMemoryAccesses(Loops.back().LoopNode);
}

std::optional<LoopNestDescriptor> analyzeLoopNest(LoopNest &LN,
                                                  ScalarEvolution &SE) {
  SmallVector<Loop *, 4> PerfectLoops =
      linearizePerfectNest(LN.getOutermostLoop());
  if (PerfectLoops.size() < 2) {
    return std::nullopt;
  }

  LoopNestDescriptor Descriptor;
  for (Loop *LoopNode : PerfectLoops) {
    std::optional<LoopDescriptor> LoopDesc = buildLoopDescriptor(*LoopNode);
    if (!LoopDesc) {
      return std::nullopt;
    }
    Descriptor.Loops.push_back(std::move(*LoopDesc));
  }

  Descriptor.Accesses = collectMemoryAccesses(Descriptor.Loops);
  return Descriptor;
}

std::optional<LoopNestDescriptor> analyzeLoopNest(Loop &Root,
                                                  ScalarEvolution &SE) {
  SmallVector<Loop *, 4> PerfectLoops = linearizePerfectNest(Root);
  if (PerfectLoops.size() < 2) {
    return std::nullopt;
  }

  LoopNestDescriptor Descriptor;
  for (Loop *LoopNode : PerfectLoops) {
    std::optional<LoopDescriptor> LoopDesc = buildLoopDescriptor(*LoopNode);
    if (!LoopDesc) {
      return std::nullopt;
    }
    Descriptor.Loops.push_back(std::move(*LoopDesc));
  }

  Descriptor.Accesses = collectMemoryAccesses(Descriptor.Loops);
  return Descriptor;
}

bool isRectangularPair(const LoopDescriptor &Outer, const LoopDescriptor &Inner) {
  if (Outer.LoopNode == nullptr || Inner.LoopNode == nullptr ||
      Outer.Induction == nullptr || Inner.Induction == nullptr) {
    return false;
  }

  if (Outer.Bounds.Step != 1 || Inner.Bounds.Step != 1) {
    return false;
  }

  if (Outer.Induction->getType() != Inner.Induction->getType()) {
    return false;
  }

  if (exprMentionsSymbol(Inner.Bounds.Lower, Outer.Induction) ||
      exprMentionsSymbol(Inner.Bounds.Upper, Outer.Induction)) {
    return false;
  }

  return true;
}

bool hasUnsafeSameBaseDependence(const LoopNestDescriptor &Nest) {
  for (size_t I = 0; I < Nest.Accesses.size(); ++I) {
    for (size_t J = I + 1; J < Nest.Accesses.size(); ++J) {
      const MemoryAccess &A = Nest.Accesses[I];
      const MemoryAccess &B = Nest.Accesses[J];

      if (A.BasePointer != B.BasePointer) {
        continue;
      }

      if (A.AccessKind != MemoryAccess::Kind::Store &&
          B.AccessKind != MemoryAccess::Kind::Store) {
        continue;
      }

      if (A.Subscripts.size() != B.Subscripts.size()) {
        return true;
      }

      for (size_t Dim = 0; Dim < A.Subscripts.size(); ++Dim) {
        if (!exprEquals(A.Subscripts[Dim], B.Subscripts[Dim])) {
          return true;
        }
      }
    }
  }

  return false;
}

bool isInterchangeProfitable(const LoopNestDescriptor &Nest) {
  if (Nest.depth() < 2) {
    return false;
  }

  const Value *OuterIV = Nest.Loops[0].Induction;
  const Value *InnerIV = Nest.Loops[1].Induction;

  int CurrentScore = 0;
  int SwappedScore = 0;

  for (const MemoryAccess &Access : Nest.Accesses) {
    const int OuterPos = findSubscriptPosition(Access, OuterIV);
    const int InnerPos = findSubscriptPosition(Access, InnerIV);
    if (OuterPos == 0 || InnerPos == 0 || OuterPos == InnerPos) {
      continue;
    }

    if (InnerPos > OuterPos) {
      ++CurrentScore;
    } else {
      ++SwappedScore;
    }
  }

  return SwappedScore > CurrentScore && SwappedScore > 0;
}

bool setCompareBound(LoopDescriptor &Descriptor, Value *NewBound) {
  if (Descriptor.Compare == nullptr || NewBound == nullptr ||
      Descriptor.Induction == nullptr) {
    return false;
  }

  Descriptor.Compare->setOperand(1, NewBound);
  Descriptor.BoundValue = NewBound;
  Descriptor.Bounds.Upper = makeExprFromValue(NewBound);
  return true;
}

bool swapBodyInductionUses(LoopDescriptor &Outer, LoopDescriptor &Inner) {
  PHINode *OuterIV = Outer.Induction;
  PHINode *InnerIV = Inner.Induction;
  Loop *InnerLoop = Inner.LoopNode;
  BasicBlock *InnerHeader = InnerLoop->getHeader();
  if (OuterIV == nullptr || InnerIV == nullptr || InnerLoop == nullptr ||
      InnerHeader == nullptr) {
    return false;
  }

  IRBuilder<> Builder(&*InnerHeader->getFirstInsertionPt());
  Value *Zero = ConstantInt::get(OuterIV->getType(), 0);
  Value *OuterReplacement =
      Builder.CreateAdd(InnerIV, Zero, Outer.InductionName + ".swapped");
  Value *InnerReplacement =
      Builder.CreateAdd(OuterIV, Zero, Inner.InductionName + ".swapped");

  BasicBlock *InnerLatch = InnerLoop->getLoopLatch();
  bool Changed = false;

  for (BasicBlock *BB : InnerLoop->blocks()) {
    if (BB == InnerHeader || BB == InnerLatch) {
      continue;
    }

    for (Instruction &Inst : *BB) {
      for (unsigned OperandIndex = 0; OperandIndex < Inst.getNumOperands();
           ++OperandIndex) {
        Value *Operand = Inst.getOperand(OperandIndex);
        if (Operand == OuterIV) {
          Inst.setOperand(OperandIndex, OuterReplacement);
          Changed = true;
        } else if (Operand == InnerIV) {
          Inst.setOperand(OperandIndex, InnerReplacement);
          Changed = true;
        }
      }
    }
  }

  return Changed;
}

bool interchangeLoops(LoopNestDescriptor &Nest) {
  if (Nest.depth() < 2) {
    return false;
  }

  LoopDescriptor &Outer = Nest.Loops[0];
  LoopDescriptor &Inner = Nest.Loops[1];

  if (!isRectangularPair(Outer, Inner)) {
    return false;
  }

  BasicBlock *OuterPreheader = Outer.LoopNode->getLoopPreheader();
  BasicBlock *InnerPreheader = Inner.LoopNode->getLoopPreheader();
  if (OuterPreheader == nullptr || InnerPreheader == nullptr) {
    return false;
  }

  Value *OldOuterStart = Outer.StartValue;
  Value *OldInnerStart = Inner.StartValue;
  Value *OldOuterBound = Outer.BoundValue;
  Value *OldInnerBound = Inner.BoundValue;
  if (OldOuterStart == nullptr || OldInnerStart == nullptr ||
      OldOuterBound == nullptr || OldInnerBound == nullptr) {
    return false;
  }

  Outer.Induction->setIncomingValueForBlock(OuterPreheader, OldInnerStart);
  Inner.Induction->setIncomingValueForBlock(InnerPreheader, OldOuterStart);
  Outer.StartValue = OldInnerStart;
  Inner.StartValue = OldOuterStart;
  Outer.Bounds.Lower = makeExprFromValue(OldInnerStart);
  Inner.Bounds.Lower = makeExprFromValue(OldOuterStart);

  bool RewroteBody = swapBodyInductionUses(Outer, Inner);
  if (!setCompareBound(Outer, OldInnerBound) ||
      !setCompareBound(Inner, OldOuterBound)) {
    return false;
  }

  if (!RewroteBody) {
    return false;
  }

  errs() << "[" << kPassName << "] interchanged loops "
         << Outer.InductionName << " <-> " << Inner.InductionName << "\n";
  return true;
}

bool tryBlocking(LoopNestDescriptor &Nest, LoopStandardAnalysisResults &AR) {
  if (!PolyEnableBlocking || Nest.depth() < 2) {
    return false;
  }

  Loop *Outer = Nest.Loops[0].LoopNode;
  if (Outer == nullptr) {
    return false;
  }

  unsigned TripCount = AR.SE.getSmallConstantTripCount(Outer);
  if (TripCount < 2) {
    return false;
  }

  unsigned Count = std::min<unsigned>(PolyTileSize, TripCount);
  if (Count < 2) {
    return false;
  }

  Function *F = Outer->getHeader()->getParent();
  DependenceInfo DI(F, &AR.AA, &AR.SE, &AR.LI);
  if (!isSafeToUnrollAndJam(Outer, AR.SE, AR.DT, DI, AR.LI)) {
    errs() << "[" << kPassName
           << "] blocking skipped: unroll-and-jam safety check failed\n";
    return false;
  }

  Loop *EpilogueLoop = nullptr;
  LoopUnrollResult Result = UnrollAndJamLoop(
      Outer, Count, TripCount, 1, true, &AR.LI, &AR.SE, &AR.DT, &AR.AC,
      &AR.TTI, nullptr, &EpilogueLoop);

  if (Result == LoopUnrollResult::Unmodified) {
    errs() << "[" << kPassName
           << "] blocking skipped: utility declined the transform\n";
    return false;
  }

  errs() << "[" << kPassName << "] applied blocking factor " << Count << "\n";
  return true;
}

bool unrollLoopByCount(Loop *L, unsigned Count, bool Runtime,
                       LoopStandardAnalysisResults &AR) {
  if (L == nullptr || Count < 2) {
    return false;
  }

  UnrollLoopOptions Options;
  Options.Count = Count;
  Options.Force = true;
  Options.Runtime = Runtime;
  Options.AllowExpensiveTripCount = true;
  Options.UnrollRemainder = true;
  Options.ForgetAllSCEV = false;
  Options.SCEVExpansionBudget = 32;

  Loop *RemainderLoop = nullptr;
  LoopUnrollResult Result =
      UnrollLoop(L, Options, &AR.LI, &AR.SE, &AR.DT, &AR.AC, &AR.TTI, nullptr,
                 true, &RemainderLoop, &AR.AA);

  if (Result == LoopUnrollResult::Unmodified) {
    return false;
  }
  return true;
}

bool hasSingleStore(const LoopNestDescriptor &Nest) {
  return llvm::count_if(Nest.Accesses, [](const MemoryAccess &Access) {
           return Access.AccessKind == MemoryAccess::Kind::Store;
         }) == 1;
}

bool hasLoadStoreSeparation(const LoopNestDescriptor &Nest) {
  const MemoryAccess *Store = nullptr;
  for (const MemoryAccess &Access : Nest.Accesses) {
    if (Access.AccessKind == MemoryAccess::Kind::Store) {
      Store = &Access;
      break;
    }
  }
  if (Store == nullptr) {
    return false;
  }

  for (const MemoryAccess &Access : Nest.Accesses) {
    if (Access.AccessKind != MemoryAccess::Kind::Load) {
      continue;
    }

    if (Access.BasePointer == Store->BasePointer) {
      if (Access.Subscripts.size() != Store->Subscripts.size()) {
        return false;
      }
      for (size_t I = 0; I < Access.Subscripts.size(); ++I) {
        if (!exprEquals(Access.Subscripts[I], Store->Subscripts[I])) {
          return false;
        }
      }
    }
  }

  return true;
}

bool trySmallKernelUnroll(LoopNestDescriptor &Nest, LoopStandardAnalysisResults &AR) {
  if (Nest.depth() < 2) {
    return false;
  }

  bool Changed = false;

  // Full-unroll tiny affine kernel loops such as the 5x5 conv stencil.
  for (int Index = static_cast<int>(Nest.Loops.size()) - 1; Index >= 0; --Index) {
    Loop *L = Nest.Loops[static_cast<size_t>(Index)].LoopNode;
    unsigned TripCount =
        getConstantTripCount(Nest.Loops[static_cast<size_t>(Index)])
            .value_or(AR.SE.getSmallConstantTripCount(L));
    if (TripCount >= 2 && TripCount <= 8) {
      if (unrollLoopByCount(L, TripCount, false, AR)) {
        errs() << "[" << kPassName << "] fully unrolled tiny kernel loop "
               << Nest.Loops[static_cast<size_t>(Index)].InductionName
               << " by " << TripCount << "\n";
        Changed = true;
      }
    }
  }

  return Changed;
}

bool tryStreamingInnerUnroll(LoopNestDescriptor &Nest,
                             LoopStandardAnalysisResults &AR) {
  if (Nest.depth() < 2 || !hasSingleStore(Nest) || !hasLoadStoreSeparation(Nest)) {
    return false;
  }

  Loop *Inner = Nest.Loops.back().LoopNode;
  unsigned TripCount = AR.SE.getSmallConstantTripCount(Inner);
  if (TripCount < 8) {
    return false;
  }

  unsigned Count = std::min<unsigned>(4, TripCount);
  if (!unrollLoopByCount(Inner, Count, false, AR)) {
    return false;
  }

  errs() << "[" << kPassName << "] unrolled streaming inner loop "
         << Nest.Loops.back().InductionName << " by " << Count << "\n";
  return true;
}

bool trySingleLoopFallback(Loop *L, LoopStandardAnalysisResults &AR) {
  if (L == nullptr) {
    return false;
  }

  LoopNestDescriptor Nest;
  if (std::optional<LoopDescriptor> MaybeDesc = buildLoopDescriptor(*L)) {
    Nest.Loops.push_back(std::move(*MaybeDesc));
  }
  Nest.Accesses = collectMemoryAccesses(L);

  unsigned TripCount = AR.SE.getSmallConstantTripCount(L);
  if (!L->getSubLoops().empty() || TripCount < 8 || !hasSingleStore(Nest) ||
      !hasLoadStoreSeparation(Nest)) {
    return false;
  }

  unsigned Count = std::min<unsigned>(4, TripCount);
  if (!unrollLoopByCount(L, Count, false, AR)) {
    return false;
  }

  errs() << "[" << kPassName << "] unrolled single streaming loop "
         << valueNameOrFallback(L->getCanonicalInductionVariable(), "iv")
         << " by " << Count << "\n";
  return true;
}

void emitNestSummary(const LoopNestDescriptor &Nest, Function &F) {
  errs() << "[" << kPassName << "] analyzing function " << F.getName()
         << " with nest depth " << Nest.depth() << "\n";

  for (const LoopDescriptor &LoopDesc : Nest.Loops) {
    errs() << "  loop " << LoopDesc.InductionName << ": ["
           << LoopDesc.Bounds.Lower.str() << ", "
           << LoopDesc.Bounds.Upper.str() << ") step " << LoopDesc.Bounds.Step
           << "\n";
  }

  for (const MemoryAccess &Access : Nest.Accesses) {
    errs() << "  access " << Access.str() << "\n";
  }
}

void collectLoopsRecursively(Loop *Root, SmallVectorImpl<Loop *> &Loops) {
  if (Root == nullptr) {
    return;
  }

  for (Loop *Child : Root->getSubLoops()) {
    collectLoopsRecursively(Child, Loops);
  }
  Loops.push_back(Root);
}

class PolyhedralPass : public PassInfoMixin<PolyhedralPass> {
public:
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &FAM) {
    LoopInfo &LI = FAM.getResult<LoopAnalysis>(F);
    if (LI.empty()) {
      return PreservedAnalyses::all();
    }

    LoopStandardAnalysisResults AR{
        FAM.getResult<AAManager>(F),
        FAM.getResult<AssumptionAnalysis>(F),
        FAM.getResult<DominatorTreeAnalysis>(F),
        LI,
        FAM.getResult<ScalarEvolutionAnalysis>(F),
        FAM.getResult<TargetLibraryAnalysis>(F),
        FAM.getResult<TargetIRAnalysis>(F),
        nullptr,
        nullptr,
        nullptr,
    };

    SmallVector<Loop *, 16> Loops;
    for (Loop *TopLevel : LI) {
      collectLoopsRecursively(TopLevel, Loops);
    }

    bool Changed = false;
    for (Loop *Root : Loops) {
      std::optional<LoopNestDescriptor> MaybeNest = analyzeLoopNest(*Root, AR.SE);
      if (!MaybeNest) {
        continue;
      }

      LoopNestDescriptor Nest = std::move(*MaybeNest);
      emitNestSummary(Nest, F);

      if (!isRectangularPair(Nest.Loops[0], Nest.Loops[1])) {
        errs() << "[" << kPassName
               << "] skipped: nest is not rectangular/unit-stride\n";
        continue;
      }

      if (hasUnsafeSameBaseDependence(Nest)) {
        errs() << "[" << kPassName
               << "] skipped: conservative same-base dependence check failed\n";
        continue;
      }

      bool LocalChange = false;
      if (isInterchangeProfitable(Nest)) {
        LocalChange = interchangeLoops(Nest);
      }

      if (!LocalChange) {
        LocalChange = trySmallKernelUnroll(Nest, AR);
      }

      if (!LocalChange) {
        LocalChange = tryStreamingInnerUnroll(Nest, AR);
      }

      if (!LocalChange) {
        LocalChange = tryBlocking(Nest, AR);
      }

      Changed |= LocalChange;
    }

    if (!Changed) {
      for (Loop *Root : Loops) {
        if (trySingleLoopFallback(Root, AR)) {
          Changed = true;
        }
      }
    }

    return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
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
