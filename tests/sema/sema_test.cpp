#include "../TestSource.h"
#include "Lexer/Lexer.h"
#include "Sema/Sema.h"
#include "Sema/Type.h"

#include <algorithm>
#include <array>
#include <gtest/gtest.h>

using namespace kelyra;

TEST(Sema, BuiltinTypes) {
  constexpr std::array Names = {
      "i8",      "i16",    "i32",        "i64",     "i128",      "isize",
      "u8",      "u16",    "u32",        "u64",     "u128",      "usize",
      "f32",     "f64",    "f128",       "f256",    "f512",      "bool",
      "char",    "c.char", "c.schar",    "c.uchar", "c.short",   "c.int",
      "c.uint",  "c.long", "c.longlong", "c.size",  "c.ptrdiff", "c.bool",
      "c.wchar",
  };
  for (const auto Name : Names) {
    const auto Type = sema::ParseBuiltinType(Name);
    ASSERT_TRUE(Type.has_value()) << Name;
    EXPECT_EQ(sema::GetBuiltinTypeInfo(*Type).Name, Name);
  }
  EXPECT_FALSE(sema::ParseBuiltinType("i256").has_value());
}

TEST(Sema, PointerAddressAndDereference) {
  lex::Lexer Lexer;
  auto Parsed = Lexer.parse(R"(
fn update(value: i32) -> i32 {
  let pointer: *i32 = &value;
  *pointer = *pointer + 1;
  return value;
}
)");
  ASSERT_TRUE(Parsed.ok());
  sema::Sema Analysis;
  EXPECT_TRUE(Analysis.Check(*Parsed.root));
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

TEST(Sema, RejectAmbiguousWildcardFunction) {
  lex::Lexer Lexer;
  auto Main = Lexer.parse(R"(
module app.main;
import first.*;
import second.*;
fn main() -> i32 { return answer(); }
)");
  auto First =
      Lexer.parse("module first; pub fn answer() -> i32 { return 1; }");
  auto Second =
      Lexer.parse("module second; pub fn answer() -> i32 { return 2; }");
  ASSERT_TRUE(Main.ok());
  ASSERT_TRUE(First.ok());
  ASSERT_TRUE(Second.ok());
  sema::Sema Analysis;
  EXPECT_FALSE(Analysis.CheckModules({{Main.root.get(), true},
                                      {First.root.get(), false},
                                      {Second.root.get(), false}}));
  ASSERT_FALSE(Analysis.GetDiagnostics().empty());
  EXPECT_EQ(Analysis.GetDiagnostics().back().Kind,
            lex::DiagnosticKind::AmbiguousName);
}

TEST(Sema, InlineAssemblyAndForwardFunction) {
  lex::Lexer Lexer;
  auto Parsed = Lexer.parse(R"(
fn main() -> i32 { return later(); }
fn later() -> i32 {
  let value: i32 = 1;
  let result: i32 = 0;
  asm { mov {result}, {value} }
    .in(value).out(result).op(intel, nomem, nostack);
  return result;
}
)");
  ASSERT_TRUE(Parsed.ok());
  sema::Sema Analysis;
  EXPECT_TRUE(Analysis.Check(*Parsed.root));
}

TEST(Sema, UserDefinedAnnotations) {
  lex::Lexer Lexer;
  auto Parsed = Lexer.parse(R"(
@target(function)
annotation route(path: meta.string, method: meta.string = "GET");

@route("/users", method = "POST")
fn handler() -> i32 { return 0; }
)");
  ASSERT_TRUE(Parsed.ok());
  sema::Sema Analysis;
  ASSERT_TRUE(Analysis.Check(*Parsed.root));
  const auto &Instances =
      Analysis.GetAnnotations(*Parsed.root->children.back());
  ASSERT_EQ(Instances.size(), 1u);
  EXPECT_EQ(Instances.front().Name, "route");
  ASSERT_EQ(Instances.front().Arguments.size(), 2u);
  EXPECT_EQ(Instances.front().Arguments[0].Name, "path");
  EXPECT_EQ(Instances.front().Arguments[0].Value.Text, "\"/users\"");
  EXPECT_EQ(Instances.front().Arguments[1].Name, "method");
  EXPECT_EQ(Instances.front().Arguments[1].Value.Text, "\"POST\"");
}

TEST(Sema, RejectInvalidUserAnnotations) {
  lex::Lexer Lexer;
  for (const std::string Source : {
           "annotation flag(value: bool); @flag(1) fn f() -> i32 { return 0; }",
           "annotation flag(value: bool); @flag(true, true) fn f() -> i32 { "
           "return 0; }",
           "annotation tiny(value: u8); @tiny(256) fn f() -> i32 { return 0; }",
           "annotation flag(value: bool); @flag(true) @flag(false) fn f() -> "
           "i32 { return 0; }",
           "@target(annotation) annotation marker(); @marker fn f() -> i32 { "
           "return 0; }",
           "@missing fn f() -> i32 { return 0; }",
       }) {
    auto Parsed = Lexer.parse(Source);
    ASSERT_TRUE(Parsed.ok()) << Source;
    sema::Sema Analysis;
    EXPECT_FALSE(Analysis.Check(*Parsed.root)) << Source;
  }
}

TEST(Sema, AnnotationModulesDefaultsAndRepeatable) {
  lex::Lexer Lexer;
  auto Main = Lexer.parse(R"(
module app;
import web;
@web.route("/users")
@web.tag(1)
@web.tag(2)
fn handler() -> i32 { return 0; }
)");
  auto Web = Lexer.parse(R"(
module web;
@target(function)
pub annotation route(path: meta.string, method: meta.string = "GET");
@target(function)
@repeatable
pub annotation tag(value: i32);
)");
  ASSERT_TRUE(Main.ok());
  ASSERT_TRUE(Web.ok());
  sema::Sema Analysis;
  ASSERT_TRUE(Analysis.CheckModules(
      {{Main.root.get(), true}, {Web.root.get(), false}}));
  const auto &Instances = Analysis.GetAnnotations(*Main.root->children.back());
  ASSERT_EQ(Instances.size(), 3u);
  ASSERT_EQ(Instances.front().Arguments.size(), 2u);
  EXPECT_EQ(Instances.front().Arguments[1].Value.Text, "\"GET\"");
  EXPECT_EQ(Instances[1].Name, "web.tag");
  EXPECT_EQ(Instances[2].Name, "web.tag");
}

TEST(Sema, ReflectionMetadataAndReferences) {
  lex::Lexer Lexer;
  auto Parsed = Lexer.parse(R"(
annotation binding(value_type: meta.type, function: meta.symbol);
fn convert(value: i32) -> i32 { return value; }
@binding(meta(*i32), meta(convert))
fn registered() -> i32 { return 0; }
)");
  ASSERT_TRUE(Parsed.ok());
  sema::Sema Analysis;
  ASSERT_TRUE(Analysis.Check(*Parsed.root));

  const auto &Reflection = Analysis.GetReflection();
  const auto Function = Reflection.Find("convert", sema::MetaKind::Function);
  ASSERT_TRUE(Function.has_value());
  const auto &FunctionInfo = Reflection.Get(*Function);
  EXPECT_EQ(FunctionInfo.Children.size(), 1u);
  EXPECT_EQ(Reflection.Get(FunctionInfo.Children.front()).Kind,
            sema::MetaKind::Parameter);
  EXPECT_NE(FunctionInfo.Type, sema::InvalidMetaId);

  const auto &Instances =
      Analysis.GetAnnotations(*Parsed.root->children.back());
  ASSERT_EQ(Instances.size(), 1u);
  ASSERT_EQ(Instances.front().Arguments.size(), 2u);
  const auto &TypeValue = Instances.front().Arguments[0].Value;
  const auto &SymbolValue = Instances.front().Arguments[1].Value;
  EXPECT_EQ(TypeValue.Kind, sema::AnnotationValueKind::Type);
  const auto &ReflectedType = Reflection.Get(TypeValue.Reference);
  EXPECT_EQ(ReflectedType.Kind, sema::MetaKind::Type);
  EXPECT_EQ(ReflectedType.TypeKind, sema::MetaTypeKind::Pointer);
  EXPECT_NE(ReflectedType.Type, sema::InvalidMetaId);
  EXPECT_EQ(SymbolValue.Kind, sema::AnnotationValueKind::Symbol);
  EXPECT_EQ(SymbolValue.Reference, *Function);

  const auto Registered = Reflection.GetId(*Parsed.root->children.back());
  ASSERT_TRUE(Registered.has_value());
  EXPECT_EQ(Reflection.Get(*Registered).Annotations.size(), 1u);
}

TEST(Sema, ReflectionReferencesRespectModuleVisibility) {
  lex::Lexer Lexer;
  auto Main = Lexer.parse(R"(
module app;
import library;
annotation callback(function: meta.symbol);
@callback(meta(library.public_callback))
fn registered() -> i32 { return 0; }
)");
  auto Library = Lexer.parse(R"(
module library;
pub fn public_callback() -> i32 { return 1; }
fn private_callback() -> i32 { return 2; }
)");
  ASSERT_TRUE(Main.ok());
  ASSERT_TRUE(Library.ok());
  sema::Sema Analysis;
  ASSERT_TRUE(Analysis.CheckModules(
      {{Main.root.get(), true}, {Library.root.get(), false}}));

  auto PrivateMain = Lexer.parse(R"(
module app;
import library;
annotation callback(function: meta.symbol);
@callback(meta(library.private_callback))
fn registered() -> i32 { return 0; }
)");
  ASSERT_TRUE(PrivateMain.ok());
  EXPECT_FALSE(Analysis.CheckModules(
      {{PrivateMain.root.get(), true}, {Library.root.get(), false}}));
}

TEST(Sema, MetaValueCannotEscapeToRuntime) {
  lex::Lexer Lexer;
  auto Parsed = Lexer.parse("fn invalid() -> i32 { return meta(i32); }");
  ASSERT_TRUE(Parsed.ok());
  sema::Sema Analysis;
  ASSERT_FALSE(Analysis.Check(*Parsed.root));
  EXPECT_EQ(Analysis.GetDiagnostics().back().Kind,
            lex::DiagnosticKind::MetaValueInRuntimeExpression);
}

TEST(Sema, WhenEvaluatesOnlySelectedBranch) {
  lex::Lexer Lexer;
  auto Parsed = Lexer.parse(R"(
annotation selected();
@selected
fn target() -> i32 { return 0; }
fn choose() -> i32 {
  when meta(target).has_annotation(selected) && !meta(target).is_public {
    return 7;
  } else {
    return missing;
  }
}
)");
  ASSERT_TRUE(Parsed.ok());
  sema::Sema Analysis;
  EXPECT_TRUE(Analysis.Check(*Parsed.root));

  auto Invalid =
      Lexer.parse("fn invalid(flag: bool) -> i32 { when flag { return 1; } }");
  ASSERT_TRUE(Invalid.ok());
  ASSERT_FALSE(Analysis.Check(*Invalid.root));
  EXPECT_TRUE(std::any_of(
      Analysis.GetDiagnostics().begin(), Analysis.GetDiagnostics().end(),
      [](const lex::Diagnostic &Diagnostic) {
        return Diagnostic.Kind == lex::DiagnosticKind::InvalidWhenCondition;
      }));
}
