#include "Dataflow/APA/Analyses/Intra/AffineEqualities.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instructions.h"

#include "Dataflow/APA/Domains/AffineRelationDomain.h"
#include "Dataflow/APA/Solver/Solver.h"

#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace elimination {
namespace {

using D = AffineRelationDomain;

// An integer scalar the modular (Z/2^w) domain can represent.
bool isCandidate(const llvm::Value *V) {
  if (V == nullptr) {
    return false;
  }
  auto *IntTy = llvm::dyn_cast<llvm::IntegerType>(V->getType());
  return IntTy != nullptr && IntTy->getBitWidth() >= 1 &&
         IntTy->getBitWidth() <= 64;
}

// The affine-relation domain does linear algebra over a single ring Z/2^w;
// mixing bit-widths yields imprecise/bottom results. So we track scalars of one
// dominant width only (most frequent; ties broken toward the wider). Values of
// other widths become untracked (their definitions havoc/identity — sound).
unsigned dominantWidth(llvm::Function &F) {
  std::map<unsigned, unsigned> Counts;
  auto Consider = [&](const llvm::Value *V) {
    if (isCandidate(V)) {
      ++Counts[llvm::cast<llvm::IntegerType>(V->getType())->getBitWidth()];
    }
  };
  for (auto &Arg : F.args()) {
    Consider(&Arg);
  }
  for (auto &I : llvm::instructions(F)) {
    Consider(&I);
  }
  unsigned Best = 0, BestCount = 0;
  for (const auto &[W, C] : Counts) {
    if (C > BestCount || (C == BestCount && W > Best)) {
      Best = W;
      BestCount = C;
    }
  }
  return Best;
}

// Build the per-function vocabulary of tracked scalars (single dominant width):
// arguments first, then instruction results (also recorded as locals). The
// domain is configured from a pointer to this, so it must outlive the solve.
AffineRelationVocabulary buildVocabulary(llvm::Function &F) {
  const unsigned Width = dominantWidth(F);
  AffineRelationVocabulary Vocab;
  auto Add = [&](const llvm::Value *V) {
    if (!isCandidate(V) ||
        llvm::cast<llvm::IntegerType>(V->getType())->getBitWidth() != Width ||
        Vocab.indices.count(V)) {
      return;
    }
    const unsigned Idx = static_cast<unsigned>(Vocab.values.size());
    Vocab.indices.emplace(V, Idx);
    Vocab.actualBitWidths.emplace(V, Width);
    Vocab.values.push_back(V);
  };
  if (Width != 0) {
    for (auto &Arg : F.args()) {
      Add(&Arg);
    }
    for (auto &I : llvm::instructions(F)) {
      Add(&I);
    }
  }
  return Vocab;
}

// Fold a binary operator into (constant, terms) over its DIRECT operands only
// (no recursive inlining — operands are vocabulary variables carrying their own
// transfers). Returns nullopt when the result is not affine (e.g. mul of two
// variables, div, rem, bitwise ops), so the caller havocs the destination.
// Membership uses the CONFIGURED vocabulary (D::isTrackedValue).
struct AffineForm {
  int64_t constant = 0;
  std::vector<std::pair<const llvm::Value *, int64_t>> terms;
};

std::optional<AffineForm> foldBinary(const llvm::BinaryOperator &BO) {
  const llvm::Value *A = BO.getOperand(0);
  const llvm::Value *B = BO.getOperand(1);
  auto *CA = llvm::dyn_cast<llvm::ConstantInt>(A);
  auto *CB = llvm::dyn_cast<llvm::ConstantInt>(B);

  auto addOperand = [](AffineForm &F, const llvm::Value *V,
                       const llvm::ConstantInt *C, int64_t Sign) -> bool {
    if (C != nullptr) {
      F.constant += Sign * C->getSExtValue();
      return true;
    }
    if (D::isTrackedValue(V)) {
      F.terms.emplace_back(V, Sign);
      return true;
    }
    return false; // untracked non-constant operand -> not affine
  };

  AffineForm Form;
  switch (BO.getOpcode()) {
  case llvm::Instruction::Add:
    if (addOperand(Form, A, CA, +1) && addOperand(Form, B, CB, +1)) {
      return Form;
    }
    return std::nullopt;
  case llvm::Instruction::Sub:
    if (addOperand(Form, A, CA, +1) && addOperand(Form, B, CB, -1)) {
      return Form;
    }
    return std::nullopt;
  case llvm::Instruction::Mul: {
    // Affine only when at least one factor is constant.
    if (CA != nullptr && CB != nullptr) {
      Form.constant = CA->getSExtValue() * CB->getSExtValue();
      return Form;
    }
    if (CA != nullptr && D::isTrackedValue(B)) {
      Form.terms.emplace_back(B, CA->getSExtValue());
      return Form;
    }
    if (CB != nullptr && D::isTrackedValue(A)) {
      Form.terms.emplace_back(A, CB->getSExtValue());
      return Form;
    }
    return std::nullopt;
  }
  default:
    return std::nullopt;
  }
}

// The affine transformer of a single instruction's execution. Instructions that
// do not define a TRACKED value leave all tracked variables unchanged
// (identity); a tracked definition is either an exact affine assignment or a
// sound havoc (forget) of just that destination.
AffineFact instructionTransfer(const llvm::Instruction &I) {
  if (!D::isTrackedValue(&I)) {
    return D::identity();
  }
  if (auto *BO = llvm::dyn_cast<llvm::BinaryOperator>(&I)) {
    if (auto Form = foldBinary(*BO)) {
      return D::makeAffineAssignment(&I, Form->constant, Form->terms);
    }
  }
  return D::makeForget(&I);
}

// --- Memoizing (transformer-composition) interpreter
// -------------------------- Instead of the generic tree-walking eval (cost ∝
// expanded tree, which blows up on real affine functions), compute each unique
// path-expression DAG node's transformer exactly once (memoized by Expr
// pointer) and combine bottom-up. Cost ∝ unique DAG nodes. Because the affine
// domain is a Kleene algebra of transformers and initialFact() is the
// compositional unit (identity), the node transformer IS the IN relation, so
// IN(node) = T(ExprTo(node)) — identical to the tree eval but without
// recomputing shared subexpressions.
using ExprFactory = PathExprFactory<llvm::Instruction *>;
using ExprRef = ExprFactory::Ref;
using ExprNode = ExprFactory::Expr;
using ExprKind = ExprFactory::Kind;

// Reflexive-transitive closure transformer T* = 1 ⊕ T ⊕ T² ⊕ … , matching the
// tree interpreter's Star fixpoint (X = 1 ⊕ T·X). Converges finitely for affine
// relations (Howell normal form, bounded lattice height).
AffineFact closureOf(const AffineFact &T, std::size_t Limit) {
  AffineFact C = D::zero();
  for (std::size_t i = 0; i < Limit; ++i) {
    AffineFact Next = D::combine(D::identity(), D::extend(T, C));
    if (D::equal(Next, C)) {
      return C;
    }
    C = std::move(Next);
  }
  return C;
}

AffineFact memoTransfer(const ExprRef &E,
                        std::unordered_map<const ExprNode *, AffineFact> &Memo,
                        std::size_t Limit) {
  if (!E) {
    return D::zero();
  }
  auto It = Memo.find(E.get());
  if (It != Memo.end()) {
    return It->second;
  }
  // Children are computed into local copies BEFORE inserting into Memo (a
  // recursive insert may rehash and invalidate references).
  AffineFact R;
  switch (E->K) {
  case ExprKind::Zero:
    R = D::zero();
    break;
  case ExprKind::One:
    R = D::identity();
    break;
  case ExprKind::Atom:
    R = instructionTransfer(**E->Transfer);
    break;
  case ExprKind::Union: {
    AffineFact L = memoTransfer(E->L, Memo, Limit);
    AffineFact Rr = memoTransfer(E->R, Memo, Limit);
    R = D::combine(L, Rr);
    break;
  }
  case ExprKind::Concat: {
    AffineFact L = memoTransfer(E->L, Memo, Limit);
    AffineFact Rr = memoTransfer(E->R, Memo, Limit);
    R = D::extend(Rr, L); // apply L first, then R
    break;
  }
  case ExprKind::Star:
    R = closureOf(memoTransfer(E->L, Memo, Limit), Limit);
    break;
  }
  Memo.emplace(E.get(), R);
  return R;
}

// Fill every node's IN fact by memoized transformer evaluation of its optimized
// path expression. One shared Memo across the whole batch → each unique DAG
// node's transformer is computed once (cost ∝ unique nodes, not tree size).
void memoInterpret(llvm::Function &F, AffineEqualitiesResult &Result,
                   std::size_t Limit) {
  std::unordered_map<const ExprNode *, AffineFact> Memo;
  for (auto &I : llvm::instructions(F)) {
    ExprRef E = Result.ExprTo(&I);
    if (!E) {
      continue;
    }
    Result.IN(&I) = memoTransfer(E, Memo, Limit);
  }
}

class ElimAffineProblem : public LLVMIntraEliminationProblem<AffineFact> {
public:
  explicit ElimAffineProblem(llvm::Function *F)
      : LLVMIntraEliminationProblem<AffineFact>(F) {}

  AffineFact applyTransfer(const transfer_t &T,
                           const AffineFact &In) const override {
    if (T == nullptr) {
      return In;
    }
    return D::extend(instructionTransfer(*T), In);
  }

  // CFG merge / path-expression Union: affine hull (join), NOT the domain's
  // constraint-intersecting meet().
  AffineFact join(const AffineFact &Lhs, const AffineFact &Rhs) const override {
    return D::combine(Lhs, Rhs);
  }

  bool equal(const AffineFact &Lhs, const AffineFact &Rhs) const override {
    return D::equal(Lhs, Rhs);
  }

  // Neutral element of the join (bottom).
  AffineFact bottom() const override { return D::zero(); }

  // Seed at entry before any transfer: the identity (diagonal) relation.
  AffineFact initialFact() const override { return D::identity(); }
};

} // namespace

AffineEqualitiesResult runIntraElimAffineEqualities(llvm::Function *F,
                                                    EliminationOptions Opts) {
  if (F == nullptr || F->isDeclaration()) {
    return AffineEqualitiesResult{};
  }

  // The domain is parameterized by a static vocabulary singleton; configure it
  // for this function and keep the vocabulary alive across the (synchronous)
  // solve. Not thread-safe — one function at a time.
  AffineRelationVocabulary Vocab = buildVocabulary(*F);
  D::configure(&Vocab);

  ElimAffineProblem Problem(F);
  IntraEliminationSolver<LLVMAnalysisTypes<AffineFact>> Solver(Problem, Opts);
  auto Status = Solver.solve();
  auto Out = Solver.getResults();
  auto Diag = Solver.getDiagnostics();
  if (Opts.InterpMemo) {
    // The generic engines skipped the tree interpreter (InterpMemo); fill IN
    // facts here via the memoizing transformer interpreter over the (optimized)
    // ExprTo batch, and record its time as the interpretation cost.
    const auto MemoStart = std::chrono::steady_clock::now();
    const std::size_t Limit =
        Opts.MaxStarIterations ? Opts.MaxStarIterations : 100000;
    memoInterpret(*F, Out, Limit);
    Diag.interp_time_us += static_cast<std::size_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - MemoStart)
            .count());
  }
  Out.setSolveMetadata(Status, Diag);
  return Out;
}

} // namespace elimination
