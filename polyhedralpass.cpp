#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <isl/ctx.h>
#include <isl/map.h>
#include <isl/set.h>
#include <isl/union_map.h>
#include <isl/union_set.h>
#include <llvm/Transforms/Utils/BasicBlockUtils.h>
#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/Analysis/AliasAnalysis.h>
#include <llvm/Analysis/AssumptionCache.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/Analysis/LoopNestAnalysis.h>
#include <llvm/Analysis/ScalarEvolution.h>
#include <llvm/Analysis/ScalarEvolutionExpressions.h>
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

cl::opt<bool> PolyEnableInterchange(
    "poly-enable-interchange",
    cl::desc("Enable loop interchange to maximize locality before tiling"),
    cl::init(true));

// --------------------------------------------------------------------------
// Phase 1: Affine Modeling & SCEV Integration
// --------------------------------------------------------------------------

struct AffineTerm {
  const Value *Symbol = nullptr;
  int64_t Coefficient = 0;
};

struct TileLoop {
  BasicBlock *Header;
  BasicBlock *Latch;
  PHINode *Induction;
  Value *NextIV;
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
      if (Printed) OS << " + ";
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
  enum class Kind { Load, Store };
  Kind AccessKind = Kind::Load;
  Instruction *Inst = nullptr;
  const Value *BasePointer = nullptr;
  std::string BaseName;
  std::vector<AffineExpr> Subscripts;
};

struct LoopDescriptor {
  Loop *LoopNode = nullptr;
  PHINode *Induction = nullptr;
  Value *NextValue = nullptr; // Result of the loop increment
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
  if (V == nullptr) return Prefix.str();
  if (V->hasName()) return std::string(V->getName());
  std::string Buffer;
  raw_string_ostream OS(Buffer);
  V->printAsOperand(OS, false);
  return OS.str();
}

void addTerm(AffineExpr &Expr, const Value *Symbol, int64_t Coefficient) {
  if (Coefficient == 0 || Symbol == nullptr) return;
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

AffineExpr createExprFromSCEV(const SCEV *S, ScalarEvolution &SE) {
  AffineExpr Expr;
  if (!S) return Expr;

  if (auto *Const = dyn_cast<SCEVConstant>(S)) {
    Expr.Constant = Const->getAPInt().getSExtValue();
    return Expr;
  }
  
  if (auto *Add = dyn_cast<SCEVAddExpr>(S)) {
    for (const SCEV *Op : Add->operands()) {
      Expr = addExpr(Expr, createExprFromSCEV(Op, SE));
    }
    return Expr;
  }
  
  if (auto *Mul = dyn_cast<SCEVMulExpr>(S)) {
    if (Mul->getNumOperands() == 2) {
      if (auto *C = dyn_cast<SCEVConstant>(Mul->getOperand(0))) {
        return mulExpr(createExprFromSCEV(Mul->getOperand(1), SE), C->getAPInt().getSExtValue());
      }
      if (auto *C = dyn_cast<SCEVConstant>(Mul->getOperand(1))) {
        return mulExpr(createExprFromSCEV(Mul->getOperand(0), SE), C->getAPInt().getSExtValue());
      }
    }
  }

  if (auto *AddRec = dyn_cast<SCEVAddRecExpr>(S)) {
    const Loop *L = AddRec->getLoop();
    if (PHINode *IV = L->getCanonicalInductionVariable()) {
      AffineExpr Start = createExprFromSCEV(AddRec->getStart(), SE);
      AffineExpr Step = createExprFromSCEV(AddRec->getStepRecurrence(SE), SE);
      if (Step.Terms.empty()) { 
        AffineExpr IVExpr = makeUnknownExpr(IV);
        return addExpr(Start, mulExpr(IVExpr, Step.Constant));
      }
    }
  }
  
  if (auto *Cast = dyn_cast<SCEVCastExpr>(S)) {
    return createExprFromSCEV(Cast->getOperand(), SE);
  }
  
  if (auto *Unknown = dyn_cast<SCEVUnknown>(S)) {
    return makeUnknownExpr(Unknown->getValue());
  }

  return Expr;
}

struct FlattenedGEP {
  const Value *BasePointer = nullptr;
  std::vector<AffineExpr> Subscripts;
};

FlattenedGEP flattenGEP(const Value *PointerOperand, ScalarEvolution &SE) {
  if (const auto *GEP = dyn_cast<GetElementPtrInst>(PointerOperand)) {
    FlattenedGEP Flattened = flattenGEP(GEP->getPointerOperand(), SE);
    if (Flattened.BasePointer == nullptr) Flattened.BasePointer = GEP->getPointerOperand();
    for (const Use &Index : GEP->indices()) {
      Flattened.Subscripts.push_back(createExprFromSCEV(SE.getSCEV(Index.get()), SE));
    }
    return Flattened;
  }
  FlattenedGEP Base;
  Base.BasePointer = PointerOperand;
  return Base;
}

std::optional<LoopDescriptor> buildLoopDescriptor(Loop &L, ScalarEvolution &SE) {
  LoopDescriptor Descriptor;
  Descriptor.LoopNode = &L;

  PHINode *Induction = L.getCanonicalInductionVariable();
  if (!Induction) return std::nullopt;

  Descriptor.Induction = Induction;
  Descriptor.InductionName = valueNameOrFallback(Induction, "iv");

  BasicBlock *Preheader = L.getLoopPreheader();
  BasicBlock *Exiting = L.getExitingBlock();
  BasicBlock *Latch = L.getLoopLatch();
  if (!Preheader || !Exiting || !Latch) return std::nullopt;

  Descriptor.StartValue = Induction->getIncomingValueForBlock(Preheader);
  Descriptor.NextValue = Induction->getIncomingValueForBlock(Latch);
  if (!Descriptor.StartValue || !Descriptor.NextValue) return std::nullopt;

  const SCEVAddRecExpr *AR = dyn_cast<SCEVAddRecExpr>(SE.getSCEV(Induction));
  if (!AR) return std::nullopt;
  
  Descriptor.Bounds.Lower = createExprFromSCEV(AR->getStart(), SE);
  AffineExpr StepExpr = createExprFromSCEV(AR->getStepRecurrence(SE), SE);
  if (!StepExpr.Terms.empty() || StepExpr.Constant <= 0) return std::nullopt;
  Descriptor.Bounds.Step = StepExpr.Constant;

  auto *BI = dyn_cast<BranchInst>(Exiting->getTerminator());
  if (!BI || !BI->isConditional()) return std::nullopt;

  auto *Cmp = dyn_cast<ICmpInst>(BI->getCondition());
  if (!Cmp || Cmp->getOperand(0) != Induction) return std::nullopt;

  Descriptor.Compare = Cmp;
  Descriptor.Predicate = Cmp->getPredicate();
  Descriptor.BoundValue = Cmp->getOperand(1);
  Descriptor.Bounds.Upper = createExprFromSCEV(SE.getSCEV(Descriptor.BoundValue), SE);
  
  return Descriptor;
}

SmallVector<Loop *, 4> linearizePerfectNest(Loop &Root) {
  SmallVector<Loop *, 4> Result;
  Loop *Current = &Root;
  Result.push_back(Current);

  while (!Current->getSubLoops().empty()) {
    if (Current->getSubLoops().size() != 1) break;
    Loop *Child = Current->getSubLoops().front();
    Result.push_back(Child);
    Current = Child;
  }
  return Result;
}

std::vector<MemoryAccess> collectMemoryAccesses(ArrayRef<LoopDescriptor> Loops, ScalarEvolution &SE) {
  std::vector<MemoryAccess> Accesses;
  if (Loops.empty()) return Accesses;
  Loop *Innermost = Loops.back().LoopNode;

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
      } else continue;

      MemoryAccess Access;
      Access.AccessKind = Kind;
      Access.Inst = &Inst;

      FlattenedGEP Flattened = flattenGEP(PointerOperand, SE);
      Access.BasePointer = Flattened.BasePointer;
      Access.BaseName = valueNameOrFallback(Access.BasePointer, "ptr");
      Access.Subscripts = std::move(Flattened.Subscripts);
      Accesses.push_back(std::move(Access));
    }
  }
  return Accesses;
}

std::optional<LoopNestDescriptor> analyzeLoopNest(Loop &Root, ScalarEvolution &SE) {
  SmallVector<Loop *, 4> PerfectLoops = linearizePerfectNest(Root);
  if (PerfectLoops.size() < 2) return std::nullopt;

  LoopNestDescriptor Descriptor;
  for (Loop *LoopNode : PerfectLoops) {
    std::optional<LoopDescriptor> LoopDesc = buildLoopDescriptor(*LoopNode, SE);
    if (!LoopDesc) return std::nullopt;
    Descriptor.Loops.push_back(std::move(*LoopDesc));
  }

  Descriptor.Accesses = collectMemoryAccesses(Descriptor.Loops, SE);
  return Descriptor;
}

bool exprMentionsSymbol(const AffineExpr &Expr, const Value *Symbol) {
  return llvm::any_of(Expr.Terms, [&](const AffineTerm &Term) {
    return Term.Symbol == Symbol && Term.Coefficient != 0;
  });
}

bool isRectangularNest(const LoopNestDescriptor &Nest) {
  for (unsigned i = 0; i < Nest.depth(); ++i) {
    if (Nest.Loops[i].Bounds.Step != 1) return false;
    for (unsigned j = 0; j < i; ++j) {
      if (exprMentionsSymbol(Nest.Loops[i].Bounds.Lower, Nest.Loops[j].Induction) ||
          exprMentionsSymbol(Nest.Loops[i].Bounds.Upper, Nest.Loops[j].Induction)) {
        return false;
      }
    }
  }
  return true;
}

// --------------------------------------------------------------------------
// Phase 2: N-Dimensional ISL Native Dependence Checking
// --------------------------------------------------------------------------

std::string sanitizeISLName(std::string Name) {
  std::string Clean = "";
  for (char C : Name) {
    if (std::isalnum(C)) Clean += C;
    else Clean += "_";
  }
  if (!Clean.empty() && std::isdigit(Clean[0])) Clean = "V_" + Clean;
  if (Clean.empty()) Clean = "unknown";
  return Clean;
}

std::string toISLString(const AffineExpr &Expr) {
  std::string Buffer;
  raw_string_ostream OS(Buffer);
  bool Printed = false;
  if (Expr.Constant != 0 || Expr.Terms.empty()) {
    OS << Expr.Constant;
    Printed = true;
  }
  for (const AffineTerm &Term : Expr.Terms) {
    if (Printed && Term.Coefficient > 0) OS << " + ";
    else if (Printed && Term.Coefficient < 0) OS << " - ";
    else if (!Printed && Term.Coefficient < 0) OS << "-";

    int64_t AbsCoeff = std::abs(Term.Coefficient);
    if (AbsCoeff != 1 || Term.Symbol == nullptr) {
      OS << AbsCoeff;
      if (Term.Symbol != nullptr) OS << "*";
    }
    if (Term.Symbol != nullptr) {
      OS << sanitizeISLName(valueNameOrFallback(Term.Symbol, "sym"));
    }
    Printed = true;
  }
  return OS.str();
}

std::string buildISLTuple(const LoopNestDescriptor &Nest, StringRef Suffix = "") {
  std::string Res = "Stmt[";
  for (unsigned i = 0; i < Nest.depth(); ++i) {
    if (i > 0) Res += ", ";
    Res += sanitizeISLName(Nest.Loops[i].InductionName) + Suffix.str();
  }
  Res += "]";
  return Res;
}

isl_union_map *getDependencies(const LoopNestDescriptor &Nest, isl_ctx *ctx) {
  std::string Tuple = buildISLTuple(Nest);
  std::string TupleP = buildISLTuple(Nest, "_p");

  std::string DomainStr = "{ " + Tuple + " : ";
  for (unsigned i = 0; i < Nest.depth(); ++i) {
    if (i > 0) DomainStr += " and ";
    std::string IV = sanitizeISLName(Nest.Loops[i].InductionName);
    DomainStr += toISLString(Nest.Loops[i].Bounds.Lower) + " <= " + IV + " < " + toISLString(Nest.Loops[i].Bounds.Upper);
  }
  DomainStr += " }";

  std::string ReadStr = "{ ";
  std::string WriteStr = "{ ";
  bool HasReads = false, HasWrites = false;

  for (const MemoryAccess &Access : Nest.Accesses) {
    std::string MapElem = Tuple + " -> " + sanitizeISLName(Access.BaseName) + "[";
    if (Access.Subscripts.empty()) MapElem += "0";
    else {
      for (size_t k = 0; k < Access.Subscripts.size(); ++k) {
        if (k > 0) MapElem += ", ";
        MapElem += toISLString(Access.Subscripts[k]);
      }
    }
    MapElem += "]";

    if (Access.AccessKind == MemoryAccess::Kind::Load) {
      if (HasReads) ReadStr += "; ";
      ReadStr += MapElem;
      HasReads = true;
    } else {
      if (HasWrites) WriteStr += "; ";
      WriteStr += MapElem;
      HasWrites = true;
    }
  }
  ReadStr += " }";
  WriteStr += " }";

  if (!HasReads) ReadStr = "{}";
  if (!HasWrites) WriteStr = "{}";

  isl_union_set *Domain = isl_union_set_read_from_str(ctx, DomainStr.c_str());
  isl_union_map *Reads = isl_union_map_read_from_str(ctx, ReadStr.c_str());
  isl_union_map *Writes = isl_union_map_read_from_str(ctx, WriteStr.c_str());

  if (!Domain || !Reads || !Writes) {
    if (Domain) isl_union_set_free(Domain);
    if (Reads) isl_union_map_free(Reads);
    if (Writes) isl_union_map_free(Writes);
    return nullptr;
  }

  Reads = isl_union_map_intersect_domain(Reads, isl_union_set_copy(Domain));
  Writes = isl_union_map_intersect_domain(Writes, isl_union_set_copy(Domain));

  isl_union_map *WW = isl_union_map_apply_range(isl_union_map_copy(Writes), isl_union_map_reverse(isl_union_map_copy(Writes)));
  isl_union_map *WR = isl_union_map_apply_range(isl_union_map_copy(Writes), isl_union_map_reverse(isl_union_map_copy(Reads)));
  isl_union_map *RW = isl_union_map_apply_range(isl_union_map_copy(Reads), isl_union_map_reverse(isl_union_map_copy(Writes)));

  isl_union_map *Conflicts = isl_union_map_union(isl_union_map_union(WW, WR), RW);

  std::string BeforeStr = "{ " + Tuple + " -> " + TupleP + " : ";
  for (unsigned i = 0; i < Nest.depth(); ++i) {
    if (i > 0) BeforeStr += " or (";
    for (unsigned j = 0; j < i; ++j) {
      std::string IV = sanitizeISLName(Nest.Loops[j].InductionName);
      BeforeStr += IV + " == " + IV + "_p and ";
    }
    std::string IVi = sanitizeISLName(Nest.Loops[i].InductionName);
    BeforeStr += IVi + " < " + IVi + "_p";
  }
  for (unsigned i = 1; i < Nest.depth(); ++i) BeforeStr += ")";
  BeforeStr += " }";

  isl_union_map *Before = isl_union_map_read_from_str(ctx, BeforeStr.c_str());
  isl_union_map *Deps = isl_union_map_intersect(Conflicts, Before);

  isl_union_set_free(Domain);
  isl_union_map_free(Reads);
  isl_union_map_free(Writes);

  return Deps;
}

bool isTilingLegal(const LoopNestDescriptor &Nest, isl_union_map *Deps, isl_ctx *ctx) {
  if (Nest.depth() < 2 || !Deps) return false;

  std::string Tuple = buildISLTuple(Nest);
  std::string TupleP = buildISLTuple(Nest, "_p");
  bool IsLegal = true;

  for (unsigned k = 0; k < Nest.depth(); ++k) {
    std::string KStr = sanitizeISLName(Nest.Loops[k].InductionName);
    std::string NegativeDistStr = "{ " + Tuple + " -> " + TupleP + " : " +
                                  KStr + "_p < " + KStr + " }";

    isl_union_map *NegativeDist = isl_union_map_read_from_str(ctx, NegativeDistStr.c_str());
    isl_union_map *Violations = isl_union_map_intersect(isl_union_map_copy(Deps), NegativeDist);
    
    if (!isl_union_map_is_empty(Violations)) {
      IsLegal = false;
      isl_union_map_free(Violations);
      break; 
    }
    isl_union_map_free(Violations);
  }

  if (!IsLegal) {
    errs() << "[" << kPassName << "] Polyhedral operations illegal: ISL detected negative dependence distance.\n";
  } else {
    errs() << "[" << kPassName << "] ISL verified " << Nest.depth() << "-Deep band is fully permutable!\n";
  }
  return IsLegal;
}

// --------------------------------------------------------------------------
// Phase 3: Loop Interchange & Tiling
// --------------------------------------------------------------------------

bool applyLoopInterchange(LoopNestDescriptor &Nest, unsigned IdxOuter, unsigned IdxInner) {
  LoopDescriptor &OuterLD = Nest.Loops[IdxOuter];
  LoopDescriptor &InnerLD = Nest.Loops[IdxInner];
  
  if (OuterLD.Induction->getType() != InnerLD.Induction->getType()) {
    errs() << "[" << kPassName << "] Interchange failed: IV types mismatch.\n";
    return false;
  }
  
  // 1. Swap incoming Start Values (Preheader incoming to PHI)
  BasicBlock *PreOuter = OuterLD.LoopNode->getLoopPreheader();
  BasicBlock *PreInner = InnerLD.LoopNode->getLoopPreheader();
  int PreIdxOuter = OuterLD.Induction->getBasicBlockIndex(PreOuter);
  int PreIdxInner = InnerLD.Induction->getBasicBlockIndex(PreInner);
  
  OuterLD.Induction->setIncomingValue(PreIdxOuter, InnerLD.StartValue);
  InnerLD.Induction->setIncomingValue(PreIdxInner, OuterLD.StartValue);
  
  // 2. Swap Bounds in Compare Instructions
  OuterLD.Compare->setOperand(1, InnerLD.BoundValue);
  InnerLD.Compare->setOperand(1, OuterLD.BoundValue);
  
  // 3. Swap IV uses in the payload (Exclude loop control instructions)
  auto isControlInst = [](LoopDescriptor &LD, User *U) {
    if (U == LD.NextValue || U == LD.Compare) return true;
    if (auto *PN = dyn_cast<PHINode>(U)) {
      if (PN->getParent() == LD.LoopNode->getHeader()) return true;
      BasicBlock *Exit = LD.LoopNode->getExitBlock();
      if (Exit && PN->getParent() == Exit) return true;
    }
    return false;
  };
  
  SmallVector<Use*, 8> OuterUses;
  SmallVector<Use*, 8> InnerUses;
  for (Use &U : OuterLD.Induction->uses()) {
    if (!isControlInst(OuterLD, U.getUser())) OuterUses.push_back(&U);
  }
  for (Use &U : InnerLD.Induction->uses()) {
    if (!isControlInst(InnerLD, U.getUser())) InnerUses.push_back(&U);
  }
  
  for (Use *U : OuterUses) U->set(InnerLD.Induction);
  for (Use *U : InnerUses) U->set(OuterLD.Induction);
  
  // 4. Update the LoopDescriptor semantics 
  // (We purposefully DO NOT swap the structural LoopNode/Induction pointers to maintain tree layout)
  std::swap(OuterLD.StartValue, InnerLD.StartValue);
  std::swap(OuterLD.BoundValue, InnerLD.BoundValue);
  std::swap(OuterLD.Bounds, InnerLD.Bounds);
  std::swap(OuterLD.InductionName, InnerLD.InductionName);

  // Rename in IR for clarity
  StringRef OuterName = OuterLD.Induction->getName();
  StringRef InnerName = InnerLD.Induction->getName();
  OuterLD.Induction->setName(InnerName + ".interchanged");
  InnerLD.Induction->setName(OuterName + ".interchanged");

  return true;
}

bool optimizeLoopOrder(LoopNestDescriptor &Nest) {
  if (Nest.depth() < 2) return false;
  
  // Simple heuristic: find which IV is used in the last subscript of the most memory accesses
  std::map<Value*, int> Score;
  for (const auto &Access : Nest.Accesses) {
    if (Access.Subscripts.empty()) continue;
    const AffineExpr &LastSub = Access.Subscripts.back();
    for (const AffineTerm &Term : LastSub.Terms) {
      if (Term.Symbol) Score[const_cast<Value*>(Term.Symbol)]++;
    }
  }
  
  Value *BestIV = nullptr;
  int MaxScore = 0;
  for (auto &Pair : Score) {
    if (Pair.second > MaxScore) {
      MaxScore = Pair.second;
      BestIV = Pair.first;
    }
  }
  
  if (!BestIV) return false;
  
  unsigned BestIdx = 0;
  for (unsigned i = 0; i < Nest.depth(); ++i) {
    if (Nest.Loops[i].Induction == BestIV) {
      BestIdx = i;
      break;
    }
  }
  
  // Interchange if the best dimension for spatial locality isn't already innermost
  unsigned InnermostIdx = Nest.depth() - 1;
  if (BestIdx != InnermostIdx) {
    errs() << "[" << kPassName << "] Interchanging loop '" << Nest.Loops[BestIdx].InductionName 
           << "' with innermost loop '" << Nest.Loops[InnermostIdx].InductionName 
           << "' to maximize spatial locality.\n";
    return applyLoopInterchange(Nest, BestIdx, InnermostIdx);
  }
  return false;
}

bool applyPolyhedralTiling(LoopNestDescriptor &Nest, Function &F) {
  if (!PolyEnableBlocking || Nest.depth() < 2) return false;
  
  LLVMContext &Ctx = F.getContext();
  unsigned TSize = PolyTileSize;
  
  Loop *OutermostLoop = Nest.Loops[0].LoopNode;
  BasicBlock *OriginalPreheader = OutermostLoop->getLoopPreheader();
  BasicBlock *OriginalHeader = OutermostLoop->getHeader();
  BasicBlock *OriginalExit = OutermostLoop->getExitBlock();
  
  if (!OriginalPreheader || !OriginalExit) {
    errs() << "[" << kPassName << "] Tiling failed: Outermost loop lacks canonical preheader/exit.\n";
    return false;
  }

  Type *IVType = Nest.Loops[0].Induction->getType();
  Value *TileSizeVal = ConstantInt::get(IVType, TSize);
  
  std::vector<TileLoop> TileLoops(Nest.depth());
  BasicBlock *CurrentPreheader = OriginalPreheader;
  
  for (unsigned i = 0; i < Nest.depth(); ++i) {
    LoopDescriptor &LD = Nest.Loops[i];
    std::string TName = "tile." + LD.InductionName;
    
    BasicBlock *THeader = BasicBlock::Create(Ctx, TName + ".header", &F, OriginalHeader);
    BasicBlock *TLatch = BasicBlock::Create(Ctx, TName + ".latch", &F, OriginalHeader);
    
    CurrentPreheader->getTerminator()->replaceSuccessorWith(
      CurrentPreheader->getTerminator()->getSuccessor(0), THeader);
      
    IRBuilder<> HeaderBuilder(THeader);
    PHINode *TInduction = HeaderBuilder.CreatePHI(IVType, 2, TName + ".iv");
    TInduction->addIncoming(LD.StartValue, CurrentPreheader);
    
    IRBuilder<> LatchBuilder(TLatch);
    Value *TNext = LatchBuilder.CreateAdd(TInduction, TileSizeVal, TName + ".next");
    TInduction->addIncoming(TNext, TLatch);
    
    Value *TCmp = LatchBuilder.CreateICmp(LD.Predicate, TNext, LD.BoundValue, TName + ".cmp");
    BranchInst *LatchBr = LatchBuilder.CreateCondBr(TCmp, THeader, TLatch); 
    
    TileLoops[i] = {THeader, TLatch, TInduction, TNext};
    CurrentPreheader = THeader; 
  }
  
  IRBuilder<> InnerTileHeaderBuilder(TileLoops.back().Header);
  InnerTileHeaderBuilder.CreateBr(OriginalHeader);
  
  for (BasicBlock *Pred : predecessors(OriginalExit)) {
    if (Pred != TileLoops[0].Latch) { 
      Pred->getTerminator()->replaceSuccessorWith(OriginalExit, TileLoops.back().Latch);
    }
  }
  
  for (unsigned i = Nest.depth() - 1; i > 0; --i) {
    BranchInst *LatchBr = cast<BranchInst>(TileLoops[i].Latch->getTerminator());
    LatchBr->setSuccessor(1, TileLoops[i-1].Latch);
  }
  
  BranchInst *OutermostLatchBr = cast<BranchInst>(TileLoops[0].Latch->getTerminator());
  OutermostLatchBr->setSuccessor(1, OriginalExit);
  
  for (unsigned i = 0; i < Nest.depth(); ++i) {
    LoopDescriptor &LD = Nest.Loops[i];
    TileLoop &TL = TileLoops[i];
    
    for (unsigned j = 0; j < LD.Induction->getNumIncomingValues(); ++j) {
      if (!LD.LoopNode->contains(LD.Induction->getIncomingBlock(j))) {
        LD.Induction->setIncomingValue(j, TL.Induction);
      }
    }
    
    BasicBlock *PointPreheader = LD.LoopNode->getLoopPreheader();
    IRBuilder<> BoundBuilder(PointPreheader->getTerminator());
    
    Value *EndBound = BoundBuilder.CreateAdd(TL.Induction, TileSizeVal, "clamp." + LD.InductionName + ".end");
    Value *Cmp = BoundBuilder.CreateICmp(LD.Predicate, EndBound, LD.BoundValue, "clamp." + LD.InductionName + ".cmp");
    Value *MinBound = BoundBuilder.CreateSelect(Cmp, EndBound, LD.BoundValue, "clamp." + LD.InductionName + ".min");
    
    if (LD.Compare) {
      LD.Compare->setOperand(1, MinBound);
    }
  }

  errs() << "[" << kPassName << "] Successfully tiled " << Nest.depth() 
         << " loops with block size " << TSize << ".\n";
         
  return true; 
}

// --------------------------------------------------------------------------
// Pass Pipeline Registration
// --------------------------------------------------------------------------

void emitNestSummary(const LoopNestDescriptor &Nest, Function &F) {
  errs() << "[" << kPassName << "] analyzing function " << F.getName()
         << " with nest depth " << Nest.depth() << "\n";
}

void collectLoopsRecursively(Loop *Root, SmallVectorImpl<Loop *> &Loops) {
  if (!Root) return;
  for (Loop *Child : Root->getSubLoops()) collectLoopsRecursively(Child, Loops);
  Loops.push_back(Root);
}

class PolyhedralPass : public PassInfoMixin<PolyhedralPass> {
public:
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &FAM) {
    LoopInfo &LI = FAM.getResult<LoopAnalysis>(F);
    if (LI.empty()) return PreservedAnalyses::all();

    LoopStandardAnalysisResults AR{
        FAM.getResult<AAManager>(F),
        FAM.getResult<AssumptionAnalysis>(F),
        FAM.getResult<DominatorTreeAnalysis>(F),
        LI,
        FAM.getResult<ScalarEvolutionAnalysis>(F),
        FAM.getResult<TargetLibraryAnalysis>(F),
        FAM.getResult<TargetIRAnalysis>(F),
        nullptr, nullptr, nullptr,
    };

    SmallVector<Loop *, 16> Loops;
    for (Loop *TopLevel : LI) collectLoopsRecursively(TopLevel, Loops);

    bool Changed = false;
    for (Loop *Root : Loops) {
      std::optional<LoopNestDescriptor> MaybeNest = analyzeLoopNest(*Root, AR.SE);
      if (!MaybeNest) continue;

      LoopNestDescriptor Nest = std::move(*MaybeNest);
      emitNestSummary(Nest, F);

      if (!isRectangularNest(Nest)) {
        errs() << "[" << kPassName << "] skipped: nest is not rectangular/unit-stride\n";
        continue;
      }

      isl_ctx *ctx = isl_ctx_alloc();
      isl_union_map *Deps = getDependencies(Nest, ctx);

      if (!Deps) {
        errs() << "[" << kPassName << "] ISL engine failed to parse access bounds.\n";
        isl_ctx_free(ctx);
        continue;
      }

      bool LocalChange = false;
      
      if (isTilingLegal(Nest, Deps, ctx)) {
        if (PolyEnableInterchange) {
          LocalChange |= optimizeLoopOrder(Nest);
        }
        LocalChange |= applyPolyhedralTiling(Nest, F);
      }

      isl_union_map_free(Deps);
      isl_ctx_free(ctx);
      Changed |= LocalChange;
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
                  if (Name != "polyhedral-pass") return false;
                  FPM.addPass(PolyhedralPass());
                  return true;
                });
          }};
}