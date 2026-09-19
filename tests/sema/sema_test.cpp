#include "../TestSource.h"
#include "Lexer/Lexer.h"
#include "Sema/Sema.h"
#include "Sema/Type.h"

#include <array>
#include <gtest/gtest.h>

using namespace kelyra;

TEST(Sema, BuiltinTypes) {
  constexpr std::array Names = {
      "i8",         "i16",     "i32",       "i64",    "i128",    "u8",
      "u16",        "u32",     "u64",       "u128",   "f32",     "f64",
      "f128",       "f256",    "f512",      "bool",   "char",    "c.char",
      "c.schar",    "c.uchar", "c.short",   "c.int",  "c.uint",  "c.long",
      "c.longlong", "c.size",  "c.ptrdiff", "c.bool", "c.wchar",
  };
  for (const auto Name : Names) {
    const auto Type = sema::ParseBuiltinType(Name);
    ASSERT_TRUE(Type.has_value()) << Name;
    EXPECT_EQ(sema::GetBuiltinTypeInfo(*Type).Name, Name);
  }
  EXPECT_FALSE(sema::ParseBuiltinType("i256").has_value());
}

TEST(Sema, RejectWideFloatArithmetic) {
  lex::Lexer Lexer;
  auto Parsed = Lexer.parse("fn bad(x: f256) -> f256 { return x + x; }");
  ASSERT_TRUE(Parsed.ok());
  sema::Sema Analysis;
  ASSERT_FALSE(Analysis.Check(*Parsed.root));
  ASSERT_EQ(Analysis.GetDiagnostics().size(), 1u);
  EXPECT_EQ(Analysis.GetDiagnostics().front().Kind,
            lex::DiagnosticKind::UnsupportedExpression);
}

TEST(Sema, KelyraIntegersBridgeCompatibleCTypes) {
  lex::Lexer Lexer;
  auto Parsed = Lexer.parse(R"(
fn take(value: c.longlong) -> c.longlong { return value; }
fn bridge(value: i64) -> i64 { return take(value); }
)");
  ASSERT_TRUE(Parsed.ok());
  sema::Sema Analysis;
  EXPECT_TRUE(Analysis.Check(*Parsed.root));
}

TEST(Sema, MultidimensionalArray) {
  lex::Lexer Lexer;
  auto Parsed = Lexer.parse(R"(
fn get() -> i32 {
  let values: i32[2][3];
  values[1][1] = 7;
  return values[1][1];
}
)");
  ASSERT_TRUE(Parsed.ok());
  sema::Sema Analysis;
  EXPECT_TRUE(Analysis.Check(*Parsed.root));
}

TEST(Sema, ModulesRespectPublicVisibility) {
  lex::Lexer Lexer;
  auto Main = Lexer.parse(test::ReadSource("cli/modules/main.kly"));
  auto Library = Lexer.parse(test::ReadSource("cli/modules/math/vector.kly"));
  ASSERT_TRUE(Main.ok());
  ASSERT_TRUE(Library.ok());
  sema::Sema Analysis;
  EXPECT_TRUE(Analysis.CheckModules(
      {{Main.root.get(), true}, {Library.root.get(), false}}));

  auto PrivateUse =
      Lexer.parse(test::ReadSource("cli/modules/private_main.kly"));
  ASSERT_TRUE(PrivateUse.ok());
  EXPECT_FALSE(Analysis.CheckModules(
      {{PrivateUse.root.get(), true}, {Library.root.get(), false}}));
  ASSERT_FALSE(Analysis.GetDiagnostics().empty());
  EXPECT_EQ(Analysis.GetDiagnostics().back().Kind,
            lex::DiagnosticKind::PrivateDeclaration);
}
