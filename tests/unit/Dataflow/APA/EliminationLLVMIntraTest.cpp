#include "EliminationTestSupport.h"

TEST_F(APATest, LLVMReachabilitySkipsUnreachableBlock) {
  const char *Source = R"(
    define i32 @test(i1 %cond) {
    entry:
      br i1 %cond, label %then, label %else
    then:
      %live = add i32 1, 2
      br label %exit
    else:
      br label %exit
    dead:
      %deadv = add i32 40, 2
      br label %exit
    exit:
      %phi = phi i32 [ %live, %then ], [ 0, %else ]
      ret i32 %phi
    }
  )";

  auto Module = lotus::unittest::parseModule(Context, Source, "APATest");
  ASSERT_NE(Module, nullptr);

  auto *F = Module->getFunction("test");
  ASSERT_NE(F, nullptr);

  auto Result = elimination::runIntraElimReachable(F);
  ASSERT_TRUE(Result.hasSolveMetadata());
  EXPECT_EQ(Result.solveStatus(), elimination::SolveStatus::Ok);

  auto *Live = findInstructionByName(F, "live");
  auto *Dead = findInstructionByName(F, "deadv");
  auto *Ret = findFirst<llvm::ReturnInst>(F);
  ASSERT_NE(Live, nullptr);
  ASSERT_NE(Dead, nullptr);
  ASSERT_NE(Ret, nullptr);

  ASSERT_NE(Result.tryIN(Live), nullptr);
  EXPECT_TRUE(*Result.tryIN(Live));
  ASSERT_NE(Result.tryIN(Ret), nullptr);
  EXPECT_TRUE(*Result.tryIN(Ret));
  EXPECT_EQ(Result.tryIN(Dead), nullptr);
}
TEST_F(APATest, LLVMConstantPropagationTracksFoldedValuesAtReturn) {
  const char *Source = R"(
    define i32 @test() {
    entry:
      %sum = add i32 1, 2
      %scaled = mul i32 %sum, 4
      ret i32 %scaled
    }
  )";

  auto Module = lotus::unittest::parseModule(Context, Source, "APATest");
  ASSERT_NE(Module, nullptr);

  auto *F = Module->getFunction("test");
  ASSERT_NE(F, nullptr);

  auto Result = elimination::runIntraElimConstantPropagation(F);
  ASSERT_TRUE(Result.hasSolveMetadata());
  EXPECT_EQ(Result.solveStatus(), elimination::SolveStatus::Ok);

  auto *Scaled = findInstructionByName(F, "scaled");
  auto *Ret = findFirst<llvm::ReturnInst>(F);
  ASSERT_NE(Scaled, nullptr);
  ASSERT_NE(Ret, nullptr);

  auto *Facts = Result.tryIN(Ret);
  ASSERT_NE(Facts, nullptr);

  auto ScaledIt = Facts->find(Scaled);
  ASSERT_NE(ScaledIt, Facts->end());
}
