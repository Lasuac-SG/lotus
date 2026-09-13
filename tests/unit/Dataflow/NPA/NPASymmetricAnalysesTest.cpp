#include "Alias/Infrastructure/AliasAnalysisWrapper/AliasAnalysisWrapper.h"
#include "Dataflow/NPA/Analyses/Inter/ReachableBlocks.h"
#include "Dataflow/NPA/Analyses/Intra/ConstantPropagation.h"
#include "Dataflow/NPA/Analyses/Intra/Interval.h"
#include "Dataflow/NPA/Analyses/Intra/MaybeUninitialized.h"
#include "Dataflow/NPA/Analyses/Intra/Nullability.h"
#include "Dataflow/NPA/Analyses/Intra/Taint.h"
#include "TestUtils/LLVMHelpers.h"
#include "gtest/gtest.h"

namespace {

TEST(NPASymmetricAnalyses, IntraproceduralScalarClientsProduceBlockFacts) {
  llvm::LLVMContext Context;
  auto Module = lotus::unittest::parseModuleChecked(Context, R"(
    define i32 @main() {
    entry:
      %sum = add i32 2, 3
      br label %next
    next:
      ret i32 %sum
    }
  )");
  auto *Main = Module->getFunction("main");
  auto *Sum = lotus::unittest::findInstructionByName(Main, "sum");
  auto *Next = lotus::unittest::findBlock(Main, "next");
  ASSERT_NE(Sum, nullptr);
  ASSERT_NE(Next, nullptr);

  auto Constants = npa::ConstantPropagation::run(*Main);
  auto ConstantFact = Constants.blockFacts.find({Next});
  ASSERT_NE(ConstantFact, Constants.blockFacts.end());
  auto Constant = ConstantFact->second.values.find(Sum);
  ASSERT_NE(Constant, ConstantFact->second.values.end());
  ASSERT_TRUE(Constant->second.isConstant());
  EXPECT_EQ(Constant->second.constant.getSExtValue(), 5);

  auto Intervals = npa::IntraIntervalAnalysis::run(*Main);
  auto IntervalFact = Intervals.blockFacts.find({Next});
  ASSERT_NE(IntervalFact, Intervals.blockFacts.end());
  auto Interval = IntervalFact->second.values.find(Sum);
  ASSERT_NE(Interval, IntervalFact->second.values.end());
  ASSERT_TRUE(Interval->second.isExact());
  EXPECT_EQ(Interval->second.lower.getSExtValue(), 5);
}

TEST(NPASymmetricAnalyses, IntraproceduralFactClientsUseIntraEngine) {
  llvm::LLVMContext Context;
  auto Module = lotus::unittest::parseModuleChecked(Context, R"(
    define i32 @main() {
    entry:
      %slot = alloca i8*, align 8
      store i8* null, i8** %slot, align 8
      %pointer = load i8*, i8** %slot, align 8
      br label %next
    next:
      ret i32 0
    }
  )");
  auto *Main = Module->getFunction("main");
  auto *Next = lotus::unittest::findBlock(Main, "next");
  auto *Pointer = lotus::unittest::findInstructionByName(Main, "pointer");
  ASSERT_NE(Next, nullptr);
  ASSERT_NE(Pointer, nullptr);

  auto Uninitialized = npa::MaybeUninitialized::run(*Main);
  EXPECT_FALSE(Uninitialized.blockFacts.empty());
  EXPECT_TRUE(Uninitialized.status.overall_converged);

  npa::Nullability::Options NullOptions;
  auto Nullability = npa::Nullability::run(*Main, NullOptions);
  EXPECT_TRUE(Nullability.isMaybeNull(Next, Pointer));

  lotus::AliasAnalysisWrapper AliasAnalysis(*Module,
                                            lotus::AAConfig::BasicAA());
  npa::Taint::Options TaintOptions;
  auto Taint = npa::Taint::run(*Main, AliasAnalysis, TaintOptions);
  EXPECT_FALSE(Taint.status.configuration_error);
  EXPECT_FALSE(Taint.blockFacts.empty());
}

TEST(NPASymmetricAnalyses, InterproceduralReachableBlocksVisitsCallees) {
  llvm::LLVMContext Context;
  auto Module = lotus::unittest::parseModuleChecked(Context, R"(
    define void @callee() {
    entry:
      ret void
    }
    define i32 @main() {
    entry:
      call void @callee()
      ret i32 0
    }
  )");
  auto Result = npa::InterReachableBlocks::run(*Module);
  auto *Main = Module->getFunction("main");
  auto *Callee = Module->getFunction("callee");
  EXPECT_NE(Result.reachableBlocks.count(&Main->getEntryBlock()), 0u);
  EXPECT_NE(Result.reachableBlocks.count(&Callee->getEntryBlock()), 0u);
}

} // namespace
