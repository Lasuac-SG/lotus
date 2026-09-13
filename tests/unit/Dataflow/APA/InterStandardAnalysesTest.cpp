#include "EliminationTestSupport.h"

using namespace elimination_test;

TEST_F(APATest, InterproceduralSignMapsArgumentsAndReturnValues) {
  const char *Source = R"(
    define i32 @negate(i32 %x) {
    entry:
      %neg = sub i32 0, %x
      ret i32 %neg
    }

    define i32 @main() {
    entry:
      %call = call i32 @negate(i32 7)
      %after = add i32 %call, 1
      ret i32 %after
    }
  )";

  auto Module = lotus::unittest::parseModule(Context, Source, "APATest");
  ASSERT_NE(Module, nullptr);
  auto *Main = Module->getFunction("main");
  auto *Negate = Module->getFunction("negate");
  auto *Call = findInstructionByName(Main, "call");
  auto *After = findInstructionByName(Main, "after");
  ASSERT_NE(Main, nullptr);
  ASSERT_NE(Negate, nullptr);
  ASSERT_NE(Call, nullptr);
  ASSERT_NE(After, nullptr);

  auto Check = [&](const auto &Result) {
    auto *CalleeEntry = &Negate->getEntryBlock().front();
    const auto *EntryFact = Result.tryIN(CalleeEntry, {Call});
    ASSERT_NE(EntryFact, nullptr);
    auto Formal = EntryFact->find(Negate->getArg(0));
    ASSERT_NE(Formal, EntryFact->end());
    EXPECT_EQ(Formal->second, elimination::SignValue::positive());

    const auto *AfterFact = Result.tryIN(After, {});
    ASSERT_NE(AfterFact, nullptr);
    auto Returned = AfterFact->find(Call);
    ASSERT_NE(Returned, AfterFact->end());
    EXPECT_EQ(Returned->second, elimination::SignValue::negative());
  };

  Check(elimination::runInterElimSign(Main));
  Check(elimination::runInterSummaryElimSign(Main));
}

TEST_F(APATest, InterproceduralNonNullMapsArgumentsAndReturnValues) {
  const char *Source = R"(
    define i8* @identity(i8* %p) {
    entry:
      ret i8* %p
    }

    define i32 @main() {
    entry:
      %storage = alloca i8
      %call = call i8* @identity(i8* %storage)
      %value = load i8, i8* %call
      %result = zext i8 %value to i32
      ret i32 %result
    }
  )";

  auto Module = lotus::unittest::parseModule(Context, Source, "APATest");
  ASSERT_NE(Module, nullptr);
  auto *Main = Module->getFunction("main");
  auto *Identity = Module->getFunction("identity");
  auto *Call = findInstructionByName(Main, "call");
  auto *Load = findInstructionByName(Main, "value");
  ASSERT_NE(Main, nullptr);
  ASSERT_NE(Identity, nullptr);
  ASSERT_NE(Call, nullptr);
  ASSERT_NE(Load, nullptr);

  auto Check = [&](const auto &Result) {
    auto *CalleeEntry = &Identity->getEntryBlock().front();
    const auto *EntryFact = Result.tryIN(CalleeEntry, {Call});
    ASSERT_NE(EntryFact, nullptr);
    EXPECT_NE(EntryFact->count(Identity->getArg(0)), 0u);

    const auto *LoadFact = Result.tryIN(Load, {});
    ASSERT_NE(LoadFact, nullptr);
    EXPECT_NE(LoadFact->count(Call), 0u);
  };

  Check(elimination::runInterElimNonNull(Main));
  Check(elimination::runInterSummaryElimNonNull(Main));
}

TEST_F(APATest, InterproceduralAvailableExpressionsSurvivePureCalls) {
  const char *Source = R"(
    define void @callee() {
    entry:
      ret void
    }

    define i32 @main() {
    entry:
      %first = add i32 1, 2
      call void @callee()
      %second = add i32 1, 2
      ret i32 %second
    }
  )";

  auto Module = lotus::unittest::parseModule(Context, Source, "APATest");
  ASSERT_NE(Module, nullptr);
  auto *Main = Module->getFunction("main");
  auto *First = findInstructionByName(Main, "first");
  auto *Second = findInstructionByName(Main, "second");
  ASSERT_NE(Main, nullptr);
  ASSERT_NE(First, nullptr);
  ASSERT_NE(Second, nullptr);
  const auto Key = elimination::makeExpressionKey(First);

  auto Check = [&](const auto &Result) {
    const auto *Fact = Result.tryIN(Second, {});
    ASSERT_NE(Fact, nullptr);
    EXPECT_NE(Fact->count(Key), 0u);
  };

  Check(elimination::runInterElimAvailableExpressions(Main));
  Check(elimination::runInterSummaryElimAvailableExpressions(Main));
}

TEST_F(APATest, InterproceduralAvailableExpressionsKillsLoadsModifiedByCallee) {
  const char *Source = R"(
    @global = global i32 0

    define void @store() {
    entry:
      store i32 1, i32* @global
      ret void
    }

    define i32 @main() {
    entry:
      %first = load i32, i32* @global
      call void @store()
      %second = load i32, i32* @global
      ret i32 %second
    }
  )";

  auto Module = lotus::unittest::parseModule(Context, Source, "APATest");
  ASSERT_NE(Module, nullptr);
  auto *Main = Module->getFunction("main");
  auto *First = findInstructionByName(Main, "first");
  auto *Second = findInstructionByName(Main, "second");
  ASSERT_NE(Main, nullptr);
  ASSERT_NE(First, nullptr);
  ASSERT_NE(Second, nullptr);
  const auto Key = elimination::makeExpressionKey(First);

  auto Check = [&](const auto &Result) {
    const auto *Fact = Result.tryIN(Second, {});
    ASSERT_NE(Fact, nullptr);
    EXPECT_EQ(Fact->count(Key), 0u);
  };

  Check(elimination::runInterElimAvailableExpressions(Main));
  Check(elimination::runInterSummaryElimAvailableExpressions(Main));
}
