#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <llvm/ADT/SmallVector.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/PassManager.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Passes/PassPlugin.h>
#include <llvm/Support/Casting.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;

namespace {

constexpr const char *kPassName = "polyhedral-pass";

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

struct DependenceInfo {
  const MemoryAccess *Source = nullptr;
  const MemoryAccess *Sink = nullptr;
  SmallVector<int, 4> DirectionVector;
  bool Proven = false;
};

struct LoopDescriptor {
  Loop *LoopNode = nullptr;
  const PHINode *Induction = nullptr;
  std::string InductionName;
  LoopBound Bounds;
};

struct LoopNestDescriptor {
  SmallVector<LoopDescriptor, 4> Loops;
  std::vector<MemoryAccess> Accesses;
  std::vector<DependenceInfo> Dependences;

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

AffineExpr makeUnknownExpr(const Value *V) {
  AffineExpr Expr;
  Expr.Terms.push_back({V, 1});
  return Expr;
}

AffineExpr makeExprFromValue(const Value *V) {
  if (const std::optional<int64_t> Constant = getConstantIntValue(V)) {
    AffineExpr Expr;
    Expr.Constant = *Constant;
    return Expr;
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
    LoopInfo &LI = FAM.getResult<LoopAnalysis>(F);

    std::vector<LoopNestDescriptor> Nests = collectLoopNests(LI);
    bool Changed = false;

    for (const LoopNestDescriptor &Nest : Nests) {
      emitNestSummary(Nest, F);
      Changed |= tryLoopInterchange(Nest, F);
      Changed |= tryLoopTiling(Nest, F, 32);
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

    PHINode *Induction = L.getCanonicalInductionVariable();
    if (Induction == nullptr) {
      return std::nullopt;
    }

    Descriptor.Induction = Induction;
    Descriptor.InductionName = valueNameOrFallback(Induction, "iv");
    Descriptor.Bounds.Step = 1;

    if (BasicBlock *Preheader = L.getLoopPreheader()) {
      if (Value *Start = Induction->getIncomingValueForBlock(Preheader)) {
        Descriptor.Bounds.Lower = makeExprFromValue(Start);
      }
    }

    if (BasicBlock *Exiting = L.getExitingBlock()) {
      if (auto *BI = dyn_cast<BranchInst>(Exiting->getTerminator())) {
        if (auto *Cmp = dyn_cast<ICmpInst>(BI->getCondition())) {
          Value *BoundValue = Cmp->getOperand(0) == Induction
                                  ? Cmp->getOperand(1)
                                  : Cmp->getOperand(0);
          Descriptor.Bounds.Upper = makeExprFromValue(BoundValue);
        }
      }
    }

    return Descriptor;
  }

  std::vector<MemoryAccess> collectMemoryAccesses(const LoopNestDescriptor &Nest) const {
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

  bool tryLoopInterchange(const LoopNestDescriptor &Nest, Function &F) const {
    if (Nest.depth() < 2) {
      return false;
    }

    errs() << "[" << kPassName << "] candidate interchange in function "
           << F.getName() << " on loops " << Nest.Loops[0].InductionName
           << " <-> " << Nest.Loops[1].InductionName << "\n";
    errs() << "[" << kPassName
           << "] TODO: add dependence check and loop rewriting for interchange\n";
    return false;
  }

  bool tryLoopTiling(const LoopNestDescriptor &Nest, Function &F,
                     unsigned TileSize) const {
    if (Nest.depth() < 2) {
      return false;
    }

    errs() << "[" << kPassName << "] candidate tiling in function " << F.getName()
           << " with tile size " << TileSize << "\n";
    errs() << "[" << kPassName
           << "] TODO: add legality check and strip-mine/interchange rewrite\n";
    return false;
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
