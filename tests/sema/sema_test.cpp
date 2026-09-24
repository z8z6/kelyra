#include "../TestSource.h"
#include "Lexer/Lexer.h"
#include "Sema/Sema.h"
#include "Sema/Type.h"

#include <algorithm>
#include <array>
#include <gtest/gtest.h>

using namespace kelyra;

TEST(Sema, FunctionValues) {
  auto Parsed = lex::Lexer().parse(R"(
fn increment(value: i32) -> i32 { return value + 1; }
fn choose() -> fn(i32) -> i32 { return increment; }
fn apply(callback: fn(i32) -> i32) -> i32 { return callback(1); }
fn use() -> i32 { let callback: fn(i32) -> i32 = choose(); return callback(4) + choose()(5); }
)");
  ASSERT_TRUE(Parsed.ok());
  sema::Sema Analysis;
  ASSERT_TRUE(Analysis.Check(*Parsed.root));
  for (const auto Source : {
           "fn f() -> fn() -> i32 { return 1; }",
           "fn f() -> i32 { return 1; } fn g() -> fn(i32) -> i32 { return f; }",
           "fn f() -> i32 { return 1; } fn g() { let value = f; value(1); }",
           "fn f() -> i32 { return 1; } fn g() { f = f; }",
           "fn f() -> i32 { return 1; } fn g() { let value = &f; }",
           "fn f() -> i32 { return 1; } fn g() -> bool { return f == f; }",
           "fn g(callback: fn(void)) {}",
       }) {
    SCOPED_TRACE(Source);
    auto Invalid = lex::Lexer().parse(Source);
    ASSERT_TRUE(Invalid.ok());
    EXPECT_FALSE(Analysis.Check(*Invalid.root));
  }
}

TEST(Sema, FunctionValuesRespectImports) {
  auto Library = lex::Lexer().parse(
      "module library; pub fn visible() -> i32 { return 1; } "
      "fn hidden() -> i32 { return 2; }");
  ASSERT_TRUE(Library.ok());
  for (const auto Name :
       {"library.visible", "visible", "library.hidden", "hidden"}) {
    auto Main =
        lex::Lexer().parse(std::string("module app; import library.*; fn "
                                       "factory() -> fn() -> i32 { return ") +
                           Name + "; }");
    ASSERT_TRUE(Main.ok());
    sema::Sema Analysis;
    EXPECT_EQ(Analysis.CheckModules(
                  {{Main.root.get(), true}, {Library.root.get(), false}}),
              std::string_view(Name).ends_with("visible"));
  }
}

TEST(Sema, ExternalModuleUsesDeclarationsOnly) {
  auto Main = lex::Lexer().parse(
      "module app; import library; fn main() -> i32 { return "
      "library.answer(); }");
  auto Library = lex::Lexer().parse(
      "module library; pub fn answer() -> i32 { return missing(); }");
  ASSERT_TRUE(Main.ok());
  ASSERT_TRUE(Library.ok());
  sema::Sema Analysis;
  EXPECT_TRUE(Analysis.CheckModules(
      {{Main.root.get(), true}, {Library.root.get(), false, true}}));
  EXPECT_FALSE(Analysis.CheckModules(
      {{Main.root.get(), true}, {Library.root.get(), false, false}}));
}

TEST(Sema, VoidAndMultipleReturns) {
  auto Parsed = lex::Lexer().parse(R"(
fn explicit() -> void { return; }
fn implicit() { explicit(); }
fn pair() -> (i32, bool) { return 7, true; }
fn forward() -> (i32, bool) { return pair(); }
fn use() -> i32 { let (value, ok) = forward(); if ok { return value; } return 0; }
)");
  ASSERT_TRUE(Parsed.ok());
  sema::Sema Analysis;
  ASSERT_TRUE(Analysis.Check(*Parsed.root));
  const auto &Reflection = Analysis.GetReflection();
  auto Pair = Reflection.Find("pair", sema::MetaKind::Function);
  ASSERT_TRUE(Pair);
  const auto &Results = Reflection.Get(Reflection.Get(*Pair).Type);
  EXPECT_EQ(Results.TypeKind, sema::MetaTypeKind::Results);
  EXPECT_EQ(Results.Children.size(), 2u);
  for (const auto Source : {
           "fn f() -> void { return 1; }",
           "fn f(value: void) {}",
           "fn f() { let x: void; }",
           "fn f() { let x: [2]void; }",
           "fn f() { let x: *void; }",
           "fn f() -> (i32, void) { return 1, 2; }",
           "fn f() -> (i32, bool) { return 1; }",
           "fn f() -> (i32, bool) { return 1, 2; }",
           "fn f() -> (i32, bool) { return 1, true, 2; }",
           "fn f() -> (i32, bool) { return 1, true; } fn g() { let x = f(); }",
           "fn f() -> (i32, bool) { return 1, true; } fn g() { let (x, y, z) = "
           "f(); }",
           "fn f() -> (i32, bool) { return 1, true; } fn g() { let (x, x) = "
           "f(); }",
           "fn f() -> (i32, bool) { return 1, true; } fn g() -> bool { return "
           "f() == f(); }",
           "fn f(x: (i32, bool)) {}",
       }) {
    SCOPED_TRACE(Source);
    auto Invalid = lex::Lexer().parse(Source);
    ASSERT_TRUE(Invalid.ok());
    EXPECT_FALSE(Analysis.Check(*Invalid.root));
  }
}

TEST(Sema, InterfaceDeclaration) {
  auto Parsed = lex::Lexer().parse(R"(
@interface class Reader {
  const CAPACITY: i32 = 60 + 4;
  fn read(count: i32) -> i32;
  fn ready() -> bool { return true; }
}

fn capacity() -> i32 { return Reader.CAPACITY; }
)");
  ASSERT_TRUE(Parsed.ok());
  sema::Sema Analysis;
  EXPECT_TRUE(Analysis.Check(*Parsed.root));
  EXPECT_TRUE(Analysis.GetReflection().Find("std.annotation.interface",
                                            sema::MetaKind::Annotation));
  for (const auto Source : {
           "@interface class Bad { value: i32; }",
           "@interface class Bad { init() {} }",
           "@interface class Bad { deinit() {} }",
           "@interface class Bad { const VALUE: i32 = missing; }",
           "class Bad { const VALUE: i32 = 1; }",
           "@interface class Bad { fn missing(); } fn f() { let x = Bad(); }",
           "@interface class Bad { const VALUE: i32 = 1; } fn f() { Bad.VALUE "
           "= 2; }",
           "@interface @interface class Bad {}",
           "@interface(1) class Bad {}",
           "@interface @layout(c) class Bad {}",
           "@interface fn bad() {}",
           "annotation interface();",
       }) {
    SCOPED_TRACE(Source);
    auto Invalid = lex::Lexer().parse(Source);
    ASSERT_TRUE(Invalid.ok());
    EXPECT_FALSE(Analysis.Check(*Invalid.root));
  }
  EXPECT_FALSE(lex::Lexer().parse("class Bad { fn missing(); }").ok());
}

TEST(Sema, NamedEntrypointAnnotation) {
  auto Parsed = lex::Lexer().parse("@main fn launch() -> i32 { return 42; } "
                                   "fn main() -> i32 { return 1; }");
  ASSERT_TRUE(Parsed.ok());
  sema::Sema Analysis;
  ASSERT_TRUE(Analysis.Check(*Parsed.root));
  EXPECT_TRUE(Analysis.CheckEntrypoint(*Parsed.root));
  EXPECT_EQ(Analysis.GetSymbol(*Parsed.root->children.front()), "main");

  for (const auto Source : {
           "fn main() -> i32 { return 1; }",
           "@main fn one() -> i32 { return 1; } @main fn two() -> i32 { return "
           "2; }",
           "@main fn launch(value: i32) -> i32 { return value; }",
       }) {
    SCOPED_TRACE(Source);
    auto Invalid = lex::Lexer().parse(Source);
    ASSERT_TRUE(Invalid.ok());
    ASSERT_TRUE(Analysis.Check(*Invalid.root));
    EXPECT_FALSE(Analysis.CheckEntrypoint(*Invalid.root));
  }
  auto InvalidTarget = lex::Lexer().parse("@main class Bad {}");
  ASSERT_TRUE(InvalidTarget.ok());
  EXPECT_FALSE(Analysis.Check(*InvalidTarget.root));
}

TEST(Sema, EntrypointCanBeImported) {
  auto Main = lex::Lexer().parse("module app; import worker;");
  auto Worker = lex::Lexer().parse(
      "module worker; @main fn launch() -> i32 { return 42; }");
  ASSERT_TRUE(Main.ok());
  ASSERT_TRUE(Worker.ok());
  sema::Sema Analysis;
  ASSERT_TRUE(Analysis.CheckModules(
      {{Main.root.get(), true}, {Worker.root.get(), false}}));
  EXPECT_TRUE(Analysis.CheckEntrypoint(*Main.root));
  EXPECT_EQ(Analysis.GetSymbol(*Worker.root->children.back()), "main");
}

TEST(Sema, RejectInvalidTransferMethods) {
  for (const auto Source : {
           "class Item { fn copy() {} }",
           "class Item { fn move(other: Item) {} }",
           "class Item { fn copy(other: *Item) -> i32 { return 1; } }",
           "class Item { value: i32; fn copy(other: *Item) {} }",
           "class Item { value: i32; fn move(other: *Item) { value = value; } "
           "}",
           "class Item { value: i32; } fn f() { let a = Item(); a.copy(&a); }",
       }) {
    SCOPED_TRACE(Source);
    auto Parsed = lex::Lexer().parse(Source);
    ASSERT_TRUE(Parsed.ok());
    sema::Sema Analysis;
    EXPECT_FALSE(Analysis.Check(*Parsed.root));
  }
}

TEST(Sema, ClassLayoutPadding) {
  auto Parsed = lex::Lexer().parse(R"(
class Mixed {
  flag: bool; number: i64; tail: u8;
  init() { flag = true; number = 42; tail = 7; }
}
)");
  ASSERT_TRUE(Parsed.ok());
  sema::Sema Analysis;
  ASSERT_TRUE(Analysis.Check(*Parsed.root));
  const auto *Class = Analysis.GetClass("Mixed");
  ASSERT_NE(Class, nullptr);
  EXPECT_EQ(Class->Size, 24u);
  EXPECT_EQ(Class->Alignment, 8u);
  EXPECT_EQ(Class->Fields[1].Offset, 8u);
  EXPECT_EQ(Class->Fields[2].Offset, 16u);
}

TEST(Sema, CLayoutAnnotation) {
  struct CPair {
    char tag;
    float fraction;
    double measure;
    long value;
  };
  auto Parsed = lex::Lexer().parse(R"(
@layout(c)
class Pair {
  pub tag: c.char;
  pub fraction: c.float;
  pub measure: c.double;
  pub value: c.long;
}
)");
  ASSERT_TRUE(Parsed.ok());
  sema::Sema Analysis;
  ASSERT_TRUE(Analysis.Check(*Parsed.root));
  const auto *Class = Analysis.GetClass("Pair");
  ASSERT_NE(Class, nullptr);
  EXPECT_TRUE(Class->CLayout);
  EXPECT_EQ(Class->Fields[0].Offset, 0u);
  EXPECT_EQ(Class->Fields[1].Offset, offsetof(CPair, fraction));
  EXPECT_EQ(Class->Fields[2].Offset, offsetof(CPair, measure));
  EXPECT_EQ(Class->Fields[3].Offset, offsetof(CPair, value));
  EXPECT_EQ(Class->Size, sizeof(CPair));
  for (const auto Source : {
           "@layout(c) fn f() {}",
           "@layout(unknown) class Bad {}",
           "@layout(c) @layout(c) class Bad {}",
           "@layout(c) class Bad { value: char; }",
       }) {
    auto Invalid = lex::Lexer().parse(Source);
    ASSERT_TRUE(Invalid.ok()) << Source;
    EXPECT_FALSE(Analysis.Check(*Invalid.root)) << Source;
  }
}

TEST(Sema, ClassesThisAndMetadata) {
  auto Parsed = lex::Lexer().parse(R"(
@target(class) annotation resource();
@target(method) annotation query();
@resource
class Value {
  number: i32;
  init(number: i32) { this.number = number; }
  @query fn get() -> i32 { return read(); }
  fn read() -> i32 { return number; }
  fn set(number: i32) { this.number = number; }
  deinit() { return; }
}
fn use() -> i32 {
  let value = Value(7);
  let pointer: *Value = &value;
  pointer.set(8);
  return (*pointer).get();
}
annotation typed(value: meta.type);
@typed(Value) fn annotated() {}
)");
  ASSERT_TRUE(Parsed.ok());
  sema::Sema Analysis;
  ASSERT_TRUE(Analysis.Check(*Parsed.root));
  const auto &Reflection = Analysis.GetReflection();
  ASSERT_TRUE(Reflection.Find("Value", sema::MetaKind::Class));
  EXPECT_TRUE(Reflection.Find("Value.number", sema::MetaKind::Field));
  EXPECT_TRUE(Reflection.Find("Value.get", sema::MetaKind::Method));
  EXPECT_TRUE(Reflection.Find("Value.init", sema::MetaKind::Constructor));
  EXPECT_TRUE(Reflection.Find("Value.deinit", sema::MetaKind::Destructor));
}

TEST(Sema, RejectInvalidClassLifetimes) {
  for (const auto Source : {
           "class A { init() {} init() {} }",
           "class A { init() {} deinit(x: i32) {} }",
           "class A { init() {} pub deinit() {} }",
           "class A { x: i32; init() {} }",
           "class A { x: i32; y: i32; init() { y = 1; x = 2; } }",
           "class A { x: i32; init(x: i32) { x = x; } }",
           "class A { x: i32; init() { x = x; } }",
           "class A { x: i32; init() { x = this.x; } }",
           "class A { x: i32; init() { x = read(); } fn read() -> i32 { return "
           "x; } }",
           "class A { x: i32; init() { x = leak(this); } } fn leak(p: *A) -> "
           "i32 { return 0; }",
           "class A { x: A; init() { x = A(); } }",
           "class A { b: B; init() { b = B(); } } class B { a: A; init() { a = "
           "A(); } }",
           "class A { init() {} } fn f() { let a: A; }",
           "class A { init() {} } fn f() { A(); }",
           "class A { init() {} } fn f() { let a = A(); a.deinit(); }",
           "class A { init() {} } fn f() { let a = A(); a.init(); }",
           "class A { init() {} } fn f() { let a: [2]A; }",
           "class A { init() {} } fn A() {}",
           "class A { init() {} fn f() {} fn g() { f(); } } fn f() {}",
           "class A { x: i32; x: i32; init() { x = 1; x = 2; } }",
           "class A { init() {} fn init() {} }",
           "fn f() {} fn g() { let value = f(); }",
           "fn f() { return 1; }",
           "class A { init() {} fn f() { this = this; } }",
       }) {
    SCOPED_TRACE(Source);
    auto Parsed = lex::Lexer().parse(Source);
    ASSERT_TRUE(Parsed.ok());
    sema::Sema Analysis;
    EXPECT_FALSE(Analysis.Check(*Parsed.root));
    EXPECT_FALSE(Analysis.GetDiagnostics().empty());
  }
}

TEST(Sema, ClassDefaultConstructor) {
  lex::Lexer Lexer;
  auto Parsed = Lexer.parse(R"(
class Empty {}
class Pair { first: i32; second: bool; }
class Zero { value: i32; init() { value = 7; } }
class Nested { pair: Pair; zero: Zero; }
fn use() -> i32 {
  let a = Empty();
  let b = Pair();
  let c = Zero();
  let d = Nested();
  return d.zero.value + b.first;
}
)");
  ASSERT_TRUE(Parsed.ok());
  sema::Sema Analysis;
  ASSERT_TRUE(Analysis.Check(*Parsed.root));
  for (const auto Name : {"Empty", "Pair", "Zero", "Nested"}) {
    const auto *Class = Analysis.GetClass(Name);
    ASSERT_NE(Class, nullptr) << Name;
    EXPECT_TRUE(Class->DefaultConstructible) << Name;
  }
  for (const auto Source : {
           // A field class without a zero-argument constructor cannot be
           // default constructed.
           "class Inner { n: i32; init(n: i32) { this.n = n; } } "
           "class Outer { inner: Inner; }",
           // The generated constructor takes no arguments.
           "class Empty {} fn f() -> i32 { let a = Empty(1); return 0; }",
       }) {
    SCOPED_TRACE(Source);
    auto Invalid = Lexer.parse(Source);
    ASSERT_TRUE(Invalid.ok());
    sema::Sema Other;
    EXPECT_FALSE(Other.Check(*Invalid.root));
  }
  // A generated constructor may not reach a non-public field constructor in
  // another module.
  auto Library = Lexer.parse("module library; pub class Hidden { init() {} }");
  auto Main = Lexer.parse(
      "module app; import library; class Outer { hidden: library.Hidden; } "
      "fn build() -> i32 { let outer = Outer(); return 0; }");
  ASSERT_TRUE(Library.ok());
  ASSERT_TRUE(Main.ok());
  sema::Sema Modules;
  EXPECT_FALSE(Modules.CheckModules(
      {{Main.root.get(), true}, {Library.root.get(), false}}));
}

TEST(Sema, ClassModuleVisibility) {
  auto Library = lex::Lexer().parse(R"(
module library;
pub class Item {
  secret: i32;
  pub value: i32;
  pub init(value: i32) { secret = value; this.value = value; }
  pub fn get() -> i32 { return secret; }
  fn hidden() {}
}
class Hidden { pub init() {} }
pub class PrivateInit { init() {} }
)");
  ASSERT_TRUE(Library.ok());
  for (const auto Body : {
           "let a = library.Item(1); return a.get();",
           "let a = library.Item(1); return a.value;",
           "let a: library.Item = library.Item(1); return a.get();",
       }) {
    auto Main = lex::Lexer().parse(
        std::string("module app; import library; fn main() -> i32 {") + Body +
        "}");
    ASSERT_TRUE(Main.ok());
    sema::Sema Analysis;
    EXPECT_TRUE(Analysis.CheckModules(
        {{Main.root.get(), true}, {Library.root.get(), false}}));
  }
  for (const auto Body : {
           "let a = library.Item(1); return a.secret;",
           "let a = library.Item(1); a.hidden(); return 0;",
           "let a = library.Hidden(); return 0;",
           "let a = library.PrivateInit(); return 0;",
           "let a = Item(1); return 0;",
       }) {
    auto Main = lex::Lexer().parse(
        std::string("module app; import library.*; fn main() -> i32 {") + Body +
        "}");
    ASSERT_TRUE(Main.ok());
    sema::Sema Analysis;
    EXPECT_FALSE(Analysis.CheckModules(
        {{Main.root.get(), true}, {Library.root.get(), false}}));
  }
}

TEST(Sema, BuiltinTypes) {
  constexpr std::array Names = {
      "i8",   "i16",  "i32",  "i64",  "i128",  "isize", "u8",
      "u16",  "u32",  "u64",  "u128", "usize", "f32",   "f64",
      "f128", "f256", "f512", "bool", "char",
  };
  for (const auto Name : Names) {
    const auto Type = sema::ParseBuiltinType(Name);
    ASSERT_TRUE(Type.has_value()) << Name;
    EXPECT_EQ(sema::GetBuiltinTypeInfo(*Type).Name, Name);
  }
  EXPECT_FALSE(sema::ParseBuiltinType("c.int").has_value());
  EXPECT_EQ(sema::ParseBuiltinType("__c_int"), sema::BuiltinType::CInt);
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
  let values: [2][3]i32;
  values[1][1] = 7;
  return values[1][1];
}
)");
  ASSERT_TRUE(Parsed.ok());
  sema::Sema Analysis;
  EXPECT_TRUE(Analysis.Check(*Parsed.root));
}

TEST(Sema, PointerAndArrayOrderAreDistinct) {
  auto Parsed = lex::Lexer().parse(R"(
fn invalid() {
  let values: [2]i32;
  let whole: *[2]i32 = &values;
  let elements: [2]*i32;
  whole = elements;
}
)");
  ASSERT_TRUE(Parsed.ok());
  sema::Sema Analysis;
  EXPECT_FALSE(Analysis.Check(*Parsed.root));
  ASSERT_FALSE(Analysis.GetDiagnostics().empty());
  EXPECT_EQ(Analysis.GetDiagnostics().back().Kind,
            lex::DiagnosticKind::TypeMismatch);
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

TEST(Sema, InlineAndDeprecatedAnnotations) {
  auto Parsed = lex::Lexer().parse(R"(
@inline fn automatic() -> i32 { return 1; }
@inline(always) @deprecated("use automatic")
fn old() -> i32 { return 2; }
fn caller() -> i32 { return old(); }
)");
  ASSERT_TRUE(Parsed.ok());
  sema::Sema Analysis;
  const bool Valid = Analysis.Check(*Parsed.root);
  for (const auto &Diagnostic : Analysis.GetDiagnostics())
    ADD_FAILURE() << Diagnostic;
  ASSERT_TRUE(Valid);
  ASSERT_EQ(Analysis.GetWarnings().size(), 1u);
  EXPECT_EQ(Analysis.GetWarnings().front().Message,
            "use of deprecated function 'old': use automatic");
  const auto &Automatic = Analysis.GetAnnotations(*Parsed.root->children[0]);
  ASSERT_EQ(Automatic.size(), 1u);
  EXPECT_EQ(Automatic.front().Arguments[0].Value.Text, "auto");
  for (const auto Source : {
           "@inline(never) fn f() -> i32 { return 0; }",
           "@inline(always) fn f();",
           "@inline(always) class Box {}",
           "@deprecated class Box {}",
       }) {
    auto Invalid = lex::Lexer().parse(Source);
    ASSERT_TRUE(Invalid.ok()) << Source;
    EXPECT_FALSE(Analysis.Check(*Invalid.root)) << Source;
  }
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

TEST(Sema, ReflectAnnotationSelectsInstanceFields) {
  auto Parsed = lex::Lexer().parse(R"(
@reflect class Whole {
  pub visible: i32;
  hidden: i32;
  @reflect selected: i32;
}

class Partial {
  pub excluded: i32;
  @std.annotation.reflect included: i32;
}

class Plain { pub field: i32; }
)");
  ASSERT_TRUE(Parsed.ok());
  sema::Sema Analysis;
  ASSERT_TRUE(Analysis.Check(*Parsed.root));
  const auto &Reflection = Analysis.GetReflection();
  const auto Reflected = [&](std::string_view Name, sema::MetaKind Kind) {
    const auto Id = Reflection.Find(Name, Kind);
    EXPECT_TRUE(Id.has_value()) << Name;
    return Id && Reflection.Get(*Id).RuntimeReflected;
  };
  EXPECT_TRUE(Reflected("Whole", sema::MetaKind::Class));
  EXPECT_TRUE(Reflected("Whole.visible", sema::MetaKind::Field));
  EXPECT_FALSE(Reflected("Whole.hidden", sema::MetaKind::Field));
  EXPECT_TRUE(Reflected("Whole.selected", sema::MetaKind::Field));
  EXPECT_TRUE(Reflected("Partial", sema::MetaKind::Class));
  EXPECT_FALSE(Reflected("Partial.excluded", sema::MetaKind::Field));
  EXPECT_TRUE(Reflected("Partial.included", sema::MetaKind::Field));
  EXPECT_FALSE(Reflected("Plain", sema::MetaKind::Class));
  EXPECT_FALSE(Reflected("Plain.field", sema::MetaKind::Field));
}

TEST(Sema, ReflectAnnotationRejectsInvalidTargets) {
  for (const auto Source : {
           "@reflect fn wrong() {}",
           "@interface class Wrong { @reflect const VALUE: i32 = 1; }",
           "@reflect() @reflect class Wrong {}",
           "@reflect(1) class Wrong {}",
       }) {
    SCOPED_TRACE(Source);
    auto Parsed = lex::Lexer().parse(Source);
    ASSERT_TRUE(Parsed.ok());
    sema::Sema Analysis;
    EXPECT_FALSE(Analysis.Check(*Parsed.root));
  }
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
