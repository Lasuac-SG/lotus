#include "llvm/IR/Instructions.h"

#include "Dataflow/Mono/Analyses/Inter/AvailableExpressions.h"
#include "Dataflow/Mono/Analyses/Inter/LiveVariables.h"
#include "Dataflow/Mono/Analyses/Inter/Reachability.h"
#include "Dataflow/Mono/Analyses/Inter/ReachingDefinitions.h"
#include "Dataflow/Mono/Analyses/Inter/UninitializedVariables.h"
#include "Dataflow/Mono/Analyses/Intra/Taint.h"
#include "TestUtils/LLVMHelpers.h"
#include "gtest/gtest.h"

namespace {

TEST(MonoSymmetricAnalyses, InterproceduralForwardClientsCrossCalls) {
  llvm::LLVMContext Context;
  auto Module = lotus::unittest::parseModuleChecked(Context, R"(
    define i32 @callee(i32 %x) {
    entry:
      %defined = add i32 %x, 1
      ret i32 %defined
    }

    define i32 @main() {
    entry:
      %first = add i32 1, 2
      %call = call i32 @callee(i32 %first)
      %second = add i32 1, 2
      ret i32 %second
    }
  )");
  auto *Main = Module->getFunction("main");
  auto *Callee = Module->getFunction("callee");
  auto *Call = lotus::unittest::findInstructionByName(Main, "call");
  auto *First = lotus::unittest::findInstructionByName(Main, "first");
  auto *Second = lotus::unittest::findInstructionByName(Main, "second");
  auto *Defined = lotus::unittest::findInstructionByName(Callee, "defined");
  ASSERT_NE(Call, nullptr);
  ASSERT_NE(First, nullptr);
  ASSERT_NE(Second, nullptr);
  ASSERT_NE(Defined, nullptr);

  auto Available = mono::runInterMonoAvailableExpressions(Main);
  ASSERT_NE(Available, nullptr);
  llvm::SmallVector<llvm::Value *, 2> Operands{First->getOperand(0),
                                               First->getOperand(1)};
  mono::AvailableExpression Expression(llvm::Instruction::Add, Operands);
  EXPECT_NE(Available->IN(Second, {}).count(Expression), 0u);

  auto Definitions = mono::runInterMonoReachingDefinitions(Main);
  ASSERT_NE(Definitions, nullptr);
  EXPECT_NE(Definitions->IN(Second, {}).count(Defined), 0u);
}

TEST(MonoSymmetricAnalyses, InterproceduralBackwardClientsVisitCallees) {
  llvm::LLVMContext Context;
  auto Module = lotus::unittest::parseModuleChecked(Context, R"(
    define i32 @identity(i32 %x) {
    entry:
      ret i32 %x
    }

    define i32 @main(i32 %input) {
    entry:
      %call = call i32 @identity(i32 %input)
      ret i32 %call
    }
  )");
  auto *Main = Module->getFunction("main");
  auto *Identity = Module->getFunction("identity");
  auto *IdentityReturn = Identity->getEntryBlock().getTerminator();

  auto Reachability = mono::runInterMonoReachability(Main);
  ASSERT_NE(Reachability, nullptr);
  auto HasIdentityContext = [IdentityReturn](const auto &Result) {
    for (const auto &Entry : Result.getINMap()) {
      if (Entry.first.Inst == IdentityReturn)
        return true;
    }
    return false;
  };
  EXPECT_TRUE(HasIdentityContext(*Reachability));

  auto Live = mono::runInterMonoLiveVariables(Main);
  ASSERT_NE(Live, nullptr);
  EXPECT_TRUE(HasIdentityContext(*Live));
}

TEST(MonoSymmetricAnalyses, InterproceduralUninitializedReturnIsPropagated) {
  llvm::LLVMContext Context;
  auto Module = lotus::unittest::parseModuleChecked(Context, R"(
    define i32 @produce() {
    entry:
      ret i32 undef
    }

    define i32 @main() {
    entry:
      %call = call i32 @produce()
      %after = add i32 %call, 1
      ret i32 %after
    }
  )");
  auto *Main = Module->getFunction("main");
  auto *Call = lotus::unittest::findInstructionByName(Main, "call");
  auto *After = lotus::unittest::findInstructionByName(Main, "after");
  auto Result = mono::runInterMonoUninitializedVariables(Main);
  ASSERT_NE(Result, nullptr);
  EXPECT_NE(Result->IN(After, {}).count(Call), 0u);
}

TEST(MonoSymmetricAnalyses, IntraproceduralTaintUsesSharedConfiguration) {
  llvm::LLVMContext Context;
  auto Module = lotus::unittest::parseModuleChecked(Context, R"(
    declare i32 @source()
    define i32 @main() {
    entry:
      %tainted = call i32 @source()
      %after = add i32 %tainted, 1
      ret i32 %after
    }
  )");
  auto *Main = Module->getFunction("main");
  auto *Tainted = lotus::unittest::findInstructionByName(Main, "tainted");
  auto *After = lotus::unittest::findInstructionByName(Main, "after");
  mono::MonoTaintConfig Config;
  Config.SourceFunctions.insert("source");
  auto Result = mono::runIntraMonoTaint(Main, Config);
  ASSERT_NE(Result, nullptr);
  EXPECT_NE(Result->IN(After).count(Tainted), 0u);
}

} // namespace
