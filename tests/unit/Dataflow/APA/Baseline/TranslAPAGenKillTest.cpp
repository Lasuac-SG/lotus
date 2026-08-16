/**
 * @file TranslAPAGenKillTest.cpp
 * @brief B1 unit tests for the TranslAPA baseline Gen/Kill semiring, mechanical
 *        atom translator, and memoized DAG fold interpreter.
 *
 * These tests are meaning-agnostic (paper §3): the translator recovers (Gen,
 * Kill) purely by probing a plain applyTransfer callable over a finite fact
 * universe. The end-to-end case reproduces the reaching-definitions example of
 * the paper's Fig. 1, whose path expression is 1 . (4 . 5 + 7)* . 10 and whose
 * reaching definitions at the exit are {1, 4, 10}.
 */

#include "Dataflow/APA/Baseline/TranslAPA/AtomTranslator.h"
#include "Dataflow/APA/Baseline/TranslAPA/FoldInterpreter.h"
#include "Dataflow/APA/Baseline/TranslAPA/GenKillSemiring.h"
#include "Dataflow/APA/Core/PathExpr.h"

#include <functional>
#include <set>
#include <vector>

#include <gtest/gtest.h>

using elimination::PathExprFactory;
using elimination::translapa::GenKillAtomTranslator;
using elimination::translapa::GenKillFact;
using elimination::translapa::GenKillSemiring;
using elimination::translapa::makeFoldInterpreter;

namespace {

using Fact = std::set<int>;

// Standard separable reaching-definitions transfer: an atom is a definition id;
// applying it kills all definitions of the same variable then gens itself.
struct RDModel {
  // variable owning each definition id (paper Fig. 1: x -> {1,4}, y -> {5,7,10})
  std::function<char(int)> var;

  Fact apply(int Def, const Fact &In) const {
    Fact Out;
    for (int D : In) {
      if (var(D) != var(Def)) {
        Out.insert(D);
      }
    }
    Out.insert(Def);
    return Out;
  }
};

RDModel figure1Model() {
  return RDModel{[](int D) -> char {
    return (D == 1 || D == 4) ? 'x' : 'y'; // 5,7,10 -> y
  }};
}

GenKillAtomTranslator<int, Fact> figure1Translator(const RDModel &M) {
  std::vector<int> Universe = {1, 4, 5, 7, 10};
  return GenKillAtomTranslator<int, Fact>(
      Universe, [M](const int &T, const Fact &In) { return M.apply(T, In); });
}

// Reference generic interpreter that mirrors SolverContext::eval exactly, used
// as the differential oracle the paper guarantees the semiring agrees with.
struct GenericEval {
  const RDModel &M;
  using Factory = PathExprFactory<int>;
  using Ref = typename Factory::Ref;
  using Kind = typename Factory::Kind;

  Fact meet(const Fact &A, const Fact &B) const {
    Fact Out = A;
    Out.insert(B.begin(), B.end());
    return Out; // may-union confluence
  }

  Fact eval(const Ref &E, const Fact &In) const {
    switch (E->K) {
    case Kind::Zero:
      return Fact{};
    case Kind::One:
      return In;
    case Kind::Atom:
      return M.apply(*E->Transfer, In);
    case Kind::Union:
      return meet(eval(E->L, In), eval(E->R, In));
    case Kind::Concat:
      return eval(E->R, eval(E->L, In)); // L first
    case Kind::Star: {
      Fact Cur = In;
      for (int I = 0; I < 100000; ++I) {
        Fact Next = meet(In, eval(E->L, Cur));
        if (Next == Cur) {
          return Cur;
        }
        Cur = std::move(Next);
      }
      return Cur;
    }
    }
    return Fact{};
  }
};

// ---- semiring algebra --------------------------------------------------------

// bit helpers for compact element construction in algebra tests.
llvm::BitVector bits(unsigned N, std::initializer_list<unsigned> Set) {
  llvm::BitVector B(N, false);
  for (unsigned I : Set) {
    B.set(I);
  }
  return B;
}

TEST(TranslAPAGenKill, UnitsAndApply) {
  GenKillSemiring S(3);
  // one() is identity: apply(one, x) == x.
  auto X = bits(3, {0, 2});
  EXPECT_EQ(S.apply(S.one(), X), X);
  // zero() absorbs in join: join(zero, e) == e.
  GenKillFact E{bits(3, {1}), bits(3, {0})};
  EXPECT_EQ(S.join(S.zero(), E), E);
  EXPECT_EQ(S.join(E, S.zero()), E);
}

TEST(TranslAPAGenKill, SeqOrientationLFirst) {
  // L = "gen 0, kill 1"; R = "gen 1, kill 0". Applying L then R to {} must gen
  // both bits but R's kill of 0 removes L's gen-0 that is not re-gen'd by R.
  GenKillSemiring S(2);
  GenKillFact L{bits(2, {0}), bits(2, {1})};
  GenKillFact R{bits(2, {1}), bits(2, {0})};
  auto Seq = S.seq(L, R); // R . L
  // Gen = R.Gen | (L.Gen \ R.Kill) = {1} | ({0}\{0}) = {1}
  EXPECT_EQ(Seq.Gen, bits(2, {1}));
  // Kill = L.Kill | R.Kill = {1} | {0} = {0,1}
  EXPECT_EQ(Seq.Kill, bits(2, {0, 1}));
  // apply(seq, {}) == R(L({})) == {1}
  EXPECT_EQ(S.apply(Seq, bits(2, {})), bits(2, {1}));
}

TEST(TranslAPAGenKill, JoinAndStar) {
  GenKillSemiring S(3);
  GenKillFact A{bits(3, {0}), bits(3, {0, 1})};
  GenKillFact B{bits(3, {2}), bits(3, {1, 2})};
  auto J = S.join(A, B);
  EXPECT_EQ(J.Gen, bits(3, {0, 2}));  // union
  EXPECT_EQ(J.Kill, bits(3, {1}));    // intersection
  auto St = S.star(A);
  EXPECT_EQ(St.Gen, A.Gen);
  EXPECT_EQ(St.Kill, bits(3, {})); // Kill* is empty
}

// ---- mechanical extraction ---------------------------------------------------

TEST(TranslAPAGenKill, MechanicalExtractionMatchesPaper) {
  auto M = figure1Model();
  auto T = figure1Translator(M);
  // D[1] = ({1}, {4}) in paper's Fig.1 walkthrough.
  auto G1 = T.translate(1);
  EXPECT_EQ(T.fromBits(G1.Gen), (Fact{1}));
  EXPECT_EQ(T.fromBits(G1.Kill), (Fact{4}));
  // D[5] = ({5}, {7,10}).
  auto G5 = T.translate(5);
  EXPECT_EQ(T.fromBits(G5.Gen), (Fact{5}));
  EXPECT_EQ(T.fromBits(G5.Kill), (Fact{7, 10}));
}

// ---- end-to-end fold over Fig. 1 --------------------------------------------

TEST(TranslAPAGenKill, Figure1ReachingDefinitions) {
  auto M = figure1Model();
  auto T = figure1Translator(M);
  GenKillSemiring S(T.universeSize());

  // Build p = 1 . (4 . 5 + 7)* . 10 as a hash-consed DAG.
  PathExprFactory<int> F;
  auto A1 = F.atom(1), A4 = F.atom(4), A5 = F.atom(5), A7 = F.atom(7),
       A10 = F.atom(10);
  auto Seq45 = F.concat(A4, A5);
  auto Branch = F.unite(Seq45, A7);
  auto Loop = F.star(Branch);
  auto Root = F.concat(A1, F.concat(Loop, A10));

  auto Interp = makeFoldInterpreter<int>(
      S, [&T](const int &Tr) { return T.translate(Tr); });
  GenKillFact Summary = Interp.fold(Root);

  // Applied to the empty entry fact, the reaching definitions at the exit are
  // exactly {1, 4, 10} (paper Fig. 1 / §2.3).
  llvm::BitVector Init(T.universeSize(), false);
  Fact Got = T.fromBits(S.apply(Summary, Init));
  EXPECT_EQ(Got, (Fact{1, 4, 10}));

  // Differential oracle: the closed-form semiring must agree with the generic
  // fixpoint-iterating interpreter on the same path expression.
  GenericEval Ref{M};
  Fact Reference = Ref.eval(Root, Fact{});
  EXPECT_EQ(Got, Reference);
}

} // namespace
