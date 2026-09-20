#include "CodeGen/IRGen.h"
#include "Lexer/Lexer.h"
#include "Sema/Sema.h"

#include "mlir/IR/MLIRContext.h"
#include "mlir/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"

#include <gtest/gtest.h>
#include <string>

using namespace kelyra;

TEST(IRGen, MultipleReturnsAndClassCleanup) {
  auto Parsed = lex::Lexer().parse(R"(
class Resource { init() {} deinit() {} }
fn done() -> void { return; }
fn pair() -> (i32, bool) { let resource = Resource(); return 42, true; }
fn forward() -> (i32, bool) { return pair(); }
fn factory() -> fn() -> (i32, bool) { return forward; }
fn use() { let (value, ok) = factory()(); done(); }
)");
  ASSERT_TRUE(Parsed.ok());
  sema::Sema Analysis;
  ASSERT_TRUE(Analysis.Check(*Parsed.root));
  mlir::MLIRContext Context;
  codegen::IRGen Generator(Context, Analysis);
  auto Module = Generator.Generate(*Parsed.root);
  ASSERT_TRUE(mlir::succeeded(mlir::verify(*Module)));
  std::string Output;
  llvm::raw_string_ostream OS(Output);
  Module->print(OS);
  const auto Pair = Output.find("func.func private @pair");
  ASSERT_NE(Pair, std::string::npos);
  const auto Construction = Output.find("llvm.insertvalue", Pair);
  const auto Cleanup = Output.find(
      "call @" + Analysis.GetClass("Resource")->DestructorSymbol, Pair);
  const auto Return = Output.find("return ", Pair);
  EXPECT_LT(Construction, Cleanup);
  EXPECT_LT(Cleanup, Return);
  EXPECT_NE(Output.find("llvm.extractvalue"), std::string::npos);
}

TEST(IRGen, IntegerFunction) {
  lex::Lexer Lexer;
  auto Parsed = Lexer.parse(
      "fn calculate(a: i32, b: i32) -> i32 { return a + b * 2; }", "test.kly");
  ASSERT_TRUE(Parsed.ok());
  sema::Sema Analysis;
  ASSERT_TRUE(Analysis.Check(*Parsed.root));

  mlir::MLIRContext Context;
  codegen::IRGen Generator(Context, Analysis);
  auto Module = Generator.Generate(*Parsed.root);
  ASSERT_TRUE(mlir::succeeded(mlir::verify(*Module)));

  std::string Output;
  llvm::raw_string_ostream OS(Output);
  Module->print(OS);
  EXPECT_NE(Output.find("func.func private @calculate"), std::string::npos);
  EXPECT_NE(Output.find("arith.constant 2"), std::string::npos);
  EXPECT_NE(Output.find("arith.muli"), std::string::npos);
  EXPECT_NE(Output.find("arith.addi"), std::string::npos);
  EXPECT_EQ(Output.find("kelyra."), std::string::npos);
}

TEST(IRGen, UserAnnotationMetadataDoesNotChangeLowering) {
  lex::Lexer Lexer;
  auto Parsed = Lexer.parse(R"(
@target(function)
annotation route(path: meta.string);
@route("/")
fn handler() -> i32 { return 0; }
)");
  ASSERT_TRUE(Parsed.ok());
  sema::Sema Analysis;
  ASSERT_TRUE(Analysis.Check(*Parsed.root));

  mlir::MLIRContext Context;
  codegen::IRGen Generator(Context, Analysis);
  auto Module = Generator.Generate(*Parsed.root);
  ASSERT_TRUE(mlir::succeeded(mlir::verify(*Module)));

  std::string Output;
  llvm::raw_string_ostream OS(Output);
  Module->print(OS);
  EXPECT_NE(Output.find("func.func private @handler"), std::string::npos);
}

TEST(IRGen, WhenEmitsOnlySelectedBranch) {
  lex::Lexer Lexer;
  auto Parsed = Lexer.parse(R"(
fn choose() -> i32 {
  when false { return 99; } else { return 7; }
}
)");
  ASSERT_TRUE(Parsed.ok());
  sema::Sema Analysis;
  ASSERT_TRUE(Analysis.Check(*Parsed.root));

  mlir::MLIRContext Context;
  codegen::IRGen Generator(Context, Analysis);
  auto Module = Generator.Generate(*Parsed.root);
  ASSERT_TRUE(mlir::succeeded(mlir::verify(*Module)));

  std::string Output;
  llvm::raw_string_ostream OS(Output);
  Module->print(OS);
  EXPECT_NE(Output.find("arith.constant 7"), std::string::npos);
  EXPECT_EQ(Output.find("arith.constant 99"), std::string::npos);
  EXPECT_EQ(Output.find("cf.cond_br"), std::string::npos);
}

TEST(IRGen, RejectUnknownName) {
  lex::Lexer Lexer;
  auto Parsed = Lexer.parse(
      "fn calculate(a: i32) -> i32 { return missing + a; }", "test.kly");
  ASSERT_TRUE(Parsed.ok());
  sema::Sema Analysis;
  ASSERT_FALSE(Analysis.Check(*Parsed.root));
  ASSERT_EQ(Analysis.GetDiagnostics().size(), 1u);
  EXPECT_EQ(Analysis.GetDiagnostics().front().Kind,
            lex::DiagnosticKind::UnknownName);
}

TEST(IRGen, BuiltinTypeSignatures) {
  lex::Lexer Lexer;
  auto Parsed = Lexer.parse(R"(
fn id_i8(x: i8) -> i8 { return x; }
fn id_i16(x: i16) -> i16 { return x; }
fn id_i32(x: i32) -> i32 { return x; }
fn id_i64(x: i64) -> i64 { return x; }
fn id_i128(x: i128) -> i128 { return x; }
fn id_isize(x: isize) -> isize { return x; }
fn id_u8(x: u8) -> u8 { return x; }
fn id_u16(x: u16) -> u16 { return x; }
fn id_u32(x: u32) -> u32 { return x; }
fn id_u64(x: u64) -> u64 { return x; }
fn id_u128(x: u128) -> u128 { return x; }
fn id_usize(x: usize) -> usize { return x; }
fn id_f32(x: f32) -> f32 { return x; }
fn id_f64(x: f64) -> f64 { return x; }
fn id_f128(x: f128) -> f128 { return x; }
fn id_f256(x: f256) -> f256 { return x; }
fn id_f512(x: f512) -> f512 { return x; }
fn id_bool(x: bool) -> bool { return x; }
fn id_char(x: char) -> char { return x; }
)");
  ASSERT_TRUE(Parsed.ok());
  sema::Sema Analysis;
  ASSERT_TRUE(Analysis.Check(*Parsed.root));

  mlir::MLIRContext Context;
  codegen::IRGen Generator(Context, Analysis);
  auto Module = Generator.Generate(*Parsed.root);
  ASSERT_TRUE(mlir::succeeded(mlir::verify(*Module)));

  std::string Output;
  llvm::raw_string_ostream OS(Output);
  Module->print(OS);
  EXPECT_NE(Output.find("@id_i8(%arg0: i8) -> i8"), std::string::npos);
  EXPECT_NE(Output.find("@id_i128(%arg0: i128) -> i128"), std::string::npos);
  EXPECT_NE(Output.find("@id_isize(%arg0: i64) -> i64"), std::string::npos);
  EXPECT_NE(Output.find("@id_u128(%arg0: i128) -> i128"), std::string::npos);
  EXPECT_NE(Output.find("@id_usize(%arg0: i64) -> i64"), std::string::npos);
  EXPECT_NE(Output.find("@id_f128(%arg0: f128) -> f128"), std::string::npos);
  EXPECT_NE(Output.find("@id_f256(%arg0: !kelyra.f256) -> !kelyra.f256"),
            std::string::npos);
  EXPECT_NE(Output.find("@id_f512(%arg0: !kelyra.f512) -> !kelyra.f512"),
            std::string::npos);
  EXPECT_NE(Output.find("@id_bool(%arg0: i1) -> i1"), std::string::npos);
  EXPECT_NE(Output.find("@id_char(%arg0: i32) -> i32"), std::string::npos);
}

TEST(IRGen, TypedArithmeticAndLiterals) {
  lex::Lexer Lexer;
  auto Parsed = Lexer.parse(R"(
fn half(x: u16) -> u16 { return x / 2; }
fn offset(x: f32) -> f32 { return x + 1.5; }
fn enabled() -> bool { return true; }
fn maximum() -> u128 { return 340282366920938463463374607431768211455; }
)");
  ASSERT_TRUE(Parsed.ok());
  sema::Sema Analysis;
  ASSERT_TRUE(Analysis.Check(*Parsed.root));

  mlir::MLIRContext Context;
  codegen::IRGen Generator(Context, Analysis);
  auto Module = Generator.Generate(*Parsed.root);
  ASSERT_TRUE(mlir::succeeded(mlir::verify(*Module)));

  std::string Output;
  llvm::raw_string_ostream OS(Output);
  Module->print(OS);
  EXPECT_NE(Output.find("arith.divui"), std::string::npos);
  EXPECT_NE(Output.find("arith.addf"), std::string::npos);
  EXPECT_NE(Output.find("arith.constant true"), std::string::npos);
}

TEST(IRGen, MultidimensionalArrayAndControlFlow) {
  lex::Lexer Lexer;
  auto Parsed = Lexer.parse(R"(
fn get() -> i32 {
  let values: i32[2][3];
  let i = 0;
  while i < 2 {
    values[i][1] = i + 6;
    i = i + 1;
  }
  return values[1][1];
}
)");
  ASSERT_TRUE(Parsed.ok());
  sema::Sema Analysis;
  ASSERT_TRUE(Analysis.Check(*Parsed.root));

  mlir::MLIRContext UnsafeContext;
  codegen::IRGen UnsafeGenerator(UnsafeContext, Analysis);
  auto UnsafeModule = UnsafeGenerator.Generate(*Parsed.root);
  ASSERT_TRUE(mlir::succeeded(mlir::verify(*UnsafeModule)));
  std::string UnsafeOutput;
  llvm::raw_string_ostream UnsafeOS(UnsafeOutput);
  UnsafeModule->print(UnsafeOS);
  EXPECT_EQ(UnsafeOutput.find("cf.assert"), std::string::npos);

  mlir::MLIRContext Context;
  codegen::IRGen Generator(Context, Analysis, 1);
  auto Module = Generator.Generate(*Parsed.root);
  ASSERT_TRUE(mlir::succeeded(mlir::verify(*Module)));

  std::string Output;
  llvm::raw_string_ostream OS(Output);
  Module->print(OS);
  EXPECT_NE(Output.find("!llvm.array<2 x array<3 x i32>>"), std::string::npos);
  EXPECT_NE(Output.find("llvm.getelementptr"), std::string::npos);
  EXPECT_NE(Output.find("cf.assert"), std::string::npos);
  EXPECT_NE(Output.find("cf.cond_br"), std::string::npos);
}
