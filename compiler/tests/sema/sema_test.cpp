#include "../TestSource.h"
#include "Lexer/Lexer.h"
#include "Sema/Sema.h"
#include "Sema/Type.h"

#include <algorithm>
#include <array>
#include <set>
#include <unordered_set>
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

TEST(Sema, OverloadedFunctionsAndFunctionValues) {
  auto Parsed = lex::Lexer().parse(R"(
fn select(value: i32) -> i32 { return value; }
fn select(value: f32) -> f32 { return value; }
fn use_integer(value: i32) -> i32 { return select(value); }
fn use_float(value: f32) -> f32 { return select(value); }
fn callback() -> fn(i32) -> i32 { return select; }
)");
  ASSERT_TRUE(Parsed.ok());
  sema::Sema Analysis;
  EXPECT_TRUE(Analysis.Check(*Parsed.root));
  for (const auto Source : {
           "fn f(value: i32) {} fn f(value: i32) {}",
           "fn f(value: i32) -> i32 { return value; } "
           "fn f(value: i32) -> f32 { return 0.0; }",
           "fn f(value: i32) {} fn f(value: i64) {} fn use() { f(1); }",
           "fn f(value: i32) {} fn f(value: f32) {} "
           "fn use() { let value = f; }",
           "@extern(\"same\") fn f(value: i32); "
           "@extern(\"same\") fn f(value: f32);",
       }) {
    SCOPED_TRACE(Source);
    auto Invalid = lex::Lexer().parse(Source);
    ASSERT_TRUE(Invalid.ok());
    EXPECT_FALSE(Analysis.Check(*Invalid.root));
  }
}

TEST(Sema, OverloadedMethodsAndInterfaceMethods) {
  auto Parsed = lex::Lexer().parse(R"(
@interface class Reader {
  pub fn read(value: i32) -> i32;
  pub fn read(value: f32) -> f32;
}

class Base: Reader {
  @virtual pub fn read(value: i32) -> i32 { return value; }
  pub fn read(value: f32) -> f32 { return value; }
}
class Child: Base {
  @override pub fn read(value: i32) -> i32 { return value + 1; }
}
fn use(reader: *Reader, child: *Child, number: i32, fraction: f32) -> i32 {
  let a = reader.read(number);
  let b = reader.read(fraction);
  return child.read(a);
}
)");
  ASSERT_TRUE(Parsed.ok());
  sema::Sema Analysis;
  EXPECT_TRUE(Analysis.Check(*Parsed.root));
}

TEST(Sema, OverloadedFunctionsAcrossImports) {
  auto Alpha = lex::Lexer().parse(
      "module alpha; pub fn choose(value: i32) -> i32 { return value; }");
  auto Beta = lex::Lexer().parse(
      "module beta; pub fn choose(value: f32) -> f32 { return value; }");
  auto App = lex::Lexer().parse(R"(
module app;
import alpha;
import beta;
fn use(integer: i32, floating: f32) -> i32 {
  let a = choose(integer);
  let b = choose(floating);
  return a;
}
fn callback() -> fn(i32) -> i32 { return choose; }
)");
  ASSERT_TRUE(Alpha.ok());
  ASSERT_TRUE(Beta.ok());
  ASSERT_TRUE(App.ok());
  sema::Sema Analysis;
  EXPECT_TRUE(Analysis.CheckModules({{App.root.get(), true},
                                     {Alpha.root.get(), false},
                                     {Beta.root.get(), false}}));
}

TEST(Sema, AmbiguousOverloadListsCandidateSignatures) {
  auto Parsed = lex::Lexer().parse(R"(
fn choose(value: i32) {}
fn choose(value: i64) {}
fn use() { choose(1); }
)");
  ASSERT_TRUE(Parsed.ok());
  sema::Sema Analysis;
  ASSERT_FALSE(Analysis.Check(*Parsed.root));
  const auto &Diagnostics = Analysis.GetDiagnostics();
  const auto It = std::find_if(Diagnostics.begin(), Diagnostics.end(),
                               [](const auto &Diagnostic) {
                                 return Diagnostic.Kind ==
                                        lex::DiagnosticKind::AmbiguousOverload;
                               });
  ASSERT_NE(It, Diagnostics.end());
  EXPECT_NE(It->Detail.find("choose(i32)"), std::string::npos);
  EXPECT_NE(It->Detail.find("choose(i64)"), std::string::npos);
}

TEST(Sema, OverloadSymbolsDoNotDependOnDeclarationOrder) {
  auto Single = lex::Lexer().parse(
      "fn select(value: i32) -> i32 { return value; }");
  auto First = lex::Lexer().parse(R"(
fn select(value: i32) -> i32 { return value; }
fn select(value: f32) -> f32 { return value; }
)");
  auto Reversed = lex::Lexer().parse(R"(
fn select(value: f32) -> f32 { return value; }
fn select(value: i32) -> i32 { return value; }
)");
  ASSERT_TRUE(Single.ok());
  ASSERT_TRUE(First.ok());
  ASSERT_TRUE(Reversed.ok());
  sema::Sema Before;
  sema::Sema Left;
  sema::Sema Right;
  ASSERT_TRUE(Before.Check(*Single.root));
  ASSERT_TRUE(Left.Check(*First.root));
  ASSERT_TRUE(Right.Check(*Reversed.root));
  EXPECT_EQ(Left.GetSymbol(*First.root->children[0]),
            Right.GetSymbol(*Reversed.root->children[1]));
  EXPECT_EQ(Left.GetSymbol(*First.root->children[1]),
            Right.GetSymbol(*Reversed.root->children[0]));
  EXPECT_NE(Left.GetSymbol(*First.root->children[0]),
            Left.GetSymbol(*First.root->children[1]));
  EXPECT_EQ(Before.GetSymbol(*Single.root->children[0]),
            Left.GetSymbol(*First.root->children[0]));
}

TEST(Sema, FunctionValuesRespectImports) {
  auto Library = lex::Lexer().parse(
      "module library; pub fn visible() -> i32 { return 1; } "
      "fn hidden() -> i32 { return 2; }");
  ASSERT_TRUE(Library.ok());
  for (const auto Name :
       {"library.visible", "visible", "library.hidden", "hidden"}) {
    auto Main =
        lex::Lexer().parse(std::string("module app; import library; fn "
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
           "@interface @layout(\"c\") class Bad {}",
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
@layout(Layout.C)
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
           "@layout(\"c\") fn f() {}",
           "@layout(unknown) class Bad {}",
           "@layout(Layout.C) @layout(Layout.C) "
           "class Bad {}",
           "@layout(Layout.C) class Bad { value: char; }",
       }) {
    auto Invalid = lex::Lexer().parse(Source);
    ASSERT_TRUE(Invalid.ok()) << Source;
    EXPECT_FALSE(Analysis.Check(*Invalid.root)) << Source;
  }
}

TEST(Sema, CLayoutFunctionPointerField) {
  struct CCallbacks {
    char tag;
    int (*callback)(void *, int);
  };
  auto Parsed = lex::Lexer().parse(R"(
@layout(Layout.C)
class Callbacks {
  pub tag: c.char;
  pub callback: fn(*u8, i32) -> i32;
}
)");
  ASSERT_TRUE(Parsed.ok());
  sema::Sema Analysis;
  ASSERT_TRUE(Analysis.Check(*Parsed.root));
  const auto *Class = Analysis.GetClass("Callbacks");
  ASSERT_NE(Class, nullptr);
  EXPECT_EQ(Class->Fields[1].Offset, offsetof(CCallbacks, callback));
  EXPECT_EQ(Class->Size, sizeof(CCallbacks));
  EXPECT_EQ(Class->Alignment, alignof(CCallbacks));
}

TEST(Sema, ExternLinkLibrary) {
  auto Parsed = lex::Lexer().parse(R"(
@extern("CreateWindowExW", "user32")
@callconv(cc.System)
fn create_window() -> *u8;
)");
  ASSERT_TRUE(Parsed.ok());
  sema::Sema Analysis;
  ASSERT_TRUE(Analysis.Check(*Parsed.root));
  const std::unordered_set<std::string> Modules{""};
  EXPECT_EQ(Analysis.GetLinkLibraries(Modules),
            std::set<std::string>{"user32"});
  auto Invalid = lex::Lexer().parse(R"(
@extern("CreateWindowExW", "user32", "extra")
fn create_window() -> *u8;
)");
  ASSERT_TRUE(Invalid.ok());
  EXPECT_FALSE(Analysis.Check(*Invalid.root));
}

TEST(Sema, AnnotationEnumArgumentsBindByType) {
  auto Parsed = lex::Lexer().parse(R"(
enum First { A }
enum Second { B }
annotation tagged(first: First = First.A, second: Second = Second.B);
@tagged(Second.B, First.A)
fn use() -> i32 { return 1; }
)");
  ASSERT_TRUE(Parsed.ok());
  sema::Sema Analysis;
  EXPECT_TRUE(Analysis.Check(*Parsed.root));

  auto Ambiguous = lex::Lexer().parse(R"(
enum Choice { A }
annotation duplicate(first: Choice, second: Choice);
@duplicate(Choice.A)
fn use() -> i32 { return 1; }
)");
  ASSERT_TRUE(Ambiguous.ok());
  sema::Sema InvalidAnalysis;
  EXPECT_FALSE(InvalidAnalysis.Check(*Ambiguous.root));
}

TEST(Sema, EnumAnnotationsAndMatchValidation) {
  auto Valid = lex::Lexer().parse(R"(
enum Mode: u8 { Off, On = Off + 3 }
enum Signed: i8 { Negative = -128, Zero = 0, Positive = 127 }
alias Selected = Mode;
annotation choice(kind: Mode);
@choice(Mode.On)
class Box {}
@layout(Layout.System)
class Native {}
@interface
class Constants { const Two: i32 = 1 + 1; }
fn choose(mode: Mode) -> i32 {
  return match mode { (Mode.Off) => 0, Mode.On => 1 };
}
fn classify(value: i32) -> i32 {
  return match value { Constants.Two => 2, _ => 0 };
}
fn copy(value: Selected) -> Mode { return value; }

)");
  ASSERT_TRUE(Valid.ok());
  sema::Sema Analysis;
  ASSERT_TRUE(Analysis.Check(*Valid.root));
  const auto *Mode = Analysis.GetEnum("Mode");
  ASSERT_NE(Mode, nullptr);
  ASSERT_EQ(Mode->Variants.size(), 2u);
  EXPECT_EQ(Mode->Variants[1].Value, "3");
  const auto *Signed = Analysis.GetEnum("Signed");
  ASSERT_NE(Signed, nullptr);
  EXPECT_TRUE(Signed->HasZero);
  EXPECT_EQ(Signed->Variants.front().Value, "-128");
  for (const auto &Child : Valid.root->children)
    if (Child->kind == lex::TokenKind::ast_function && Child->text == "copy")
      for (const auto &Part : Child->children)
        if (Part->kind == lex::TokenKind::ast_parameter)
          EXPECT_EQ(sema::GetBitWidth(Analysis.GetType(*Part)), 8u);
  for (const auto Source : {
           "enum E { A, A }",
           "enum E { A = 1, B = 1 }",
           "enum E: u8 { A = 256 }",
           "enum E: u8 { A = 255, B }",
           "enum E { A = 1 } fn f() { let value: E; }",
           "enum E { A = 1 } class Box { value: E; }",
           "enum E { A = 1 } class Box { @static value: E; }",
           "enum E { A, B } fn f(e: E) -> i32 { return match e { E.A => 1 }; }",
           "fn f(x: i32) -> i32 { return match x { 1 => 1 }; }",
           "fn f(x: i32) -> i32 { return match x { 1 + 1 => 1, 2 => 2, _ => 0 "
           "}; }",
           "fn f(x: i32) -> i32 { return match x { _ => 1, 2 => 2 }; }",
           "enum E { A } fn f(e: E) -> i32 { return match e { E.A => 1, _ => 2 "
           "}; }",
           "fn f(x: bool) -> i32 { return match x { true => 1, true => 2, "
           "false => 0 }; }",
           "enum E { A } fn f() -> bool { return E.A == 0; }",
           "enum E { A } fn f() -> E { return 0 as E; }",
           "enum E { A } annotation choice(kind: E); @choice(E.B) class Box {}",
           "@layout(\"c\") class Box { field: i32; }",
       }) {
    SCOPED_TRACE(Source);
    auto Invalid = lex::Lexer().parse(Source);
    ASSERT_TRUE(Invalid.ok());
    EXPECT_FALSE(Analysis.Check(*Invalid.root));
  }
}

TEST(Sema, ImportedEnumKeepsItsTypeIdentity) {
  auto Library =
      lex::Lexer().parse("module colors; pub enum Color: u8 { Red, Blue } ");
  auto Main = lex::Lexer().parse(R"(
module app;
import colors;
fn choose(value: Color) -> i32 {
  return match value { Color.Red => 1, Color.Blue => 2 };
}
)");
  ASSERT_TRUE(Library.ok());
  ASSERT_TRUE(Main.ok());
  sema::Sema Analysis;
  EXPECT_TRUE(Analysis.CheckModules(
      {{Main.root.get(), true}, {Library.root.get(), false}}));
  auto Private =
      lex::Lexer().parse("module colors; enum Color: u8 { Red, Blue }");
  ASSERT_TRUE(Private.ok());
  EXPECT_FALSE(Analysis.CheckModules(
      {{Main.root.get(), true}, {Private.root.get(), false}}));
}

TEST(Sema, ClassesThisAndMetadata) {
  auto Parsed = lex::Lexer().parse(R"(
@target(Target.Class) annotation resource();
@target(Target.Method) annotation query();
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
annotation typed(value: std.meta.Type);
@typed(meta(Value)) fn annotated() {}
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
           "let a = Item(1); return a.value;",
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
       }) {
    auto Main = lex::Lexer().parse(
        std::string("module app; import library; fn main() -> i32 {") + Body +
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

TEST(Sema, RejectAmbiguousImportedFunction) {
  lex::Lexer Lexer;
  auto Main = Lexer.parse(R"(
module app.main;
import first;
import second;
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
            lex::DiagnosticKind::AmbiguousOverload);
}

TEST(Sema, PlainImportExposesPublicTypesAndFunctions) {
  auto Main = lex::Lexer().parse(R"(
module app;
import library;
fn make() -> Item { return Item(); }
fn answer() -> Number { return value(); }
)");
  auto Library = lex::Lexer().parse(R"(
module library;
pub class Item {}
pub alias Number = i32;
pub fn value() -> i32 { return 42; }
)");
  ASSERT_TRUE(Main.ok());
  ASSERT_TRUE(Library.ok());
  sema::Sema Analysis;
  EXPECT_TRUE(Analysis.CheckModules(
      {{Main.root.get(), true}, {Library.root.get(), false}}));
}

TEST(Sema, AmbiguousImportedTypeRequiresQualification) {
  auto First = lex::Lexer().parse("module first; pub class Item {}");
  auto Second = lex::Lexer().parse("module second; pub class Item {}");
  ASSERT_TRUE(First.ok());
  ASSERT_TRUE(Second.ok());
  for (const auto Source : {
           "module app; import first; import second; fn use(value: Item) {}",
           "module app; import first; import second; fn use(value: first.Item) "
           "{}",
       }) {
    auto Main = lex::Lexer().parse(Source);
    ASSERT_TRUE(Main.ok());
    sema::Sema Analysis;
    const bool Qualified =
        std::string_view(Source).find("first.Item") != std::string_view::npos;
    EXPECT_EQ(Analysis.CheckModules({{Main.root.get(), true},
                                     {First.root.get(), false},
                                     {Second.root.get(), false}}),
              Qualified);
    if (!Qualified) {
      ASSERT_FALSE(Analysis.GetDiagnostics().empty());
      EXPECT_EQ(Analysis.GetDiagnostics().back().Kind,
                lex::DiagnosticKind::AmbiguousName);
    }
  }
}

TEST(Sema, AmbiguousImportedAnnotationRequiresQualification) {
  auto First = lex::Lexer().parse("module first; pub annotation tag();");
  auto Second = lex::Lexer().parse("module second; pub annotation tag();");
  ASSERT_TRUE(First.ok());
  ASSERT_TRUE(Second.ok());
  for (const auto Source : {
           "module app; import first; import second; @tag class Item {}",
           "module app; import first; import second; @first.tag class Item {}",
       }) {
    auto Main = lex::Lexer().parse(Source);
    ASSERT_TRUE(Main.ok());
    sema::Sema Analysis;
    const bool Qualified =
        std::string_view(Source).find("@first.tag") != std::string_view::npos;
    EXPECT_EQ(Analysis.CheckModules({{Main.root.get(), true},
                                     {First.root.get(), false},
                                     {Second.root.get(), false}}),
              Qualified);
    if (!Qualified) {
      ASSERT_FALSE(Analysis.GetDiagnostics().empty());
      EXPECT_EQ(Analysis.GetDiagnostics().back().Kind,
                lex::DiagnosticKind::AmbiguousName);
    }
  }
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
@target(Target.Function)
annotation route(path: std.util.string.StringSlice, method: std.util.string.StringSlice = "GET");

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
@inline(InlineMode.Always) @deprecated("use automatic")
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
  EXPECT_EQ(Automatic.front().Arguments[0].Value.Text,
            "std.annotation.InlineMode.Auto");
  for (const auto Source : {
           "@inline(never) fn f() -> i32 { return 0; }",
           "@inline(InlineMode.Always) fn f();",
           "@inline(InlineMode.Always) class Box {}",
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
           "@target(Target.Annotation) annotation marker(); @marker fn f() -> i32 { "
           "return 0; }",
           "@target(Target.Unknown) annotation marker();",
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
@web.transient
@web.tag(1)
@web.tag(2)
fn handler() -> i32 { return 0; }
)");
  auto Web = Lexer.parse(R"(
module web;
@target(Target.Function)
@retention(Retention.Compile)
pub annotation route(path: std.util.string.StringSlice, method: std.util.string.StringSlice = "GET");
@target(Target.Function)
@retention(Retention.Source)
pub annotation transient();
@target(Target.Function)
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
annotation binding(value_type: std.meta.Type, function: std.meta.Function);
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

TEST(Sema, AnnotationParametersUseConcreteMetaHandles) {
  auto Parsed = lex::Lexer().parse(R"(
annotation mark();
class Box { pub value: i32; }
fn handler() {}
annotation references(
  symbol: std.meta.Symbol,
  kind: std.meta.Type,
  owner: std.meta.Class,
  field: std.meta.Field,
  function: std.meta.Function,
  marker: std.meta.Annotation,
  label: std.util.string.StringSlice = "ok",
);
@references(meta(Box), meta(Box), meta(Box), meta(Box.value),
            meta(handler), meta(mark))
fn target() {}
)");
  ASSERT_TRUE(Parsed.ok());
  sema::Sema Analysis;
  const bool Valid = Analysis.Check(*Parsed.root);
  for (const auto &Diagnostic : Analysis.GetDiagnostics())
    ADD_FAILURE() << Diagnostic;
  ASSERT_TRUE(Valid);
  const auto &Values = Analysis.GetAnnotations(*Parsed.root->children.back());
  ASSERT_EQ(Values.size(), 1u);
  ASSERT_EQ(Values.front().Arguments.size(), 7u);
  EXPECT_EQ(Values.front().Arguments.back().Value.Kind,
            sema::AnnotationValueKind::String);
}

TEST(Sema, AnnotationParametersRejectWrongMetaHandles) {
  for (const auto Source : {
           "annotation bad(value: meta.type); @bad(meta(i32)) fn use() {}",
           "annotation bad(value: std.meta.Function); @bad(meta(i32)) fn use() "
           "{}",
           "annotation bad(value: std.meta.Type); fn f() {} @bad(meta(f)) fn "
           "use() {}",
           "annotation bad(value: std.meta.Function); fn f() {} @bad(f) fn "
           "use() {}",
           "annotation bad(value: std.util.string.StringSlice); @bad(1) fn "
           "use() {}",
       }) {
    SCOPED_TRACE(Source);
    auto Parsed = lex::Lexer().parse(Source);
    ASSERT_TRUE(Parsed.ok());
    sema::Sema Analysis;
    EXPECT_FALSE(Analysis.Check(*Parsed.root));
  }
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
annotation callback(function: std.meta.Function);
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
annotation callback(function: std.meta.Function);
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
  when meta(target).name == "target" &&
       meta(target).has_annotation(meta(selected)) &&
       !meta(target).is_public() {
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

TEST(Sema, ArrayLengthRequiresCompileTimeInteger) {
  lex::Lexer Lexer;
  auto Parsed = Lexer.parse("fn f(x: [1.5]i32) {}");
  ASSERT_TRUE(Parsed.ok());
  sema::Sema Analysis;
  EXPECT_FALSE(Analysis.Check(*Parsed.root));
}

TEST(Sema, MetaBlockEvaluatesFunctionAndLocalControlFlow) {
  lex::Lexer Lexer;
  auto Parsed = Lexer.parse(R"(
@meta fn sum(limit: i32) -> i32 {
  let i = 0;
  let total = 0;
  while i < limit {
    total = total + i;
    i = i + 1;
  }
  return total;
}
fn use() -> i32 {
  return meta { let result = sum(4); result + 1 };
}
fn choose() -> i32 {
  when sum(4) == 6 { return 1; } else { return missing; }
}
)");
  ASSERT_TRUE(Parsed.ok());
  sema::Sema Analysis;
  const bool Valid = Analysis.Check(*Parsed.root);
  for (const auto &Diagnostic : Analysis.GetDiagnostics())
    ADD_FAILURE() << Diagnostic;
  EXPECT_TRUE(Valid);
}

TEST(Sema, CompilerIntrinsicRequiresSupportedStandardLibraryDeclaration) {
  for (const auto Source : {
           "@intrinsic fn __read_public(id: usize) -> bool;",
           "module std.meta; @intrinsic fn unknown(id: usize) -> bool;",
           "@intrinsic(\"meta.read_public\") fn read(id: usize) -> bool;",
           "module std.meta; @intrinsic(\"unknown\") fn read(id: usize) -> "
           "bool;",
           "module std.meta; @intrinsic(\"meta.read_public\") fn read(id: i32) "
           "-> bool;",
           "module std.meta; @intrinsic fn __declared_fields(id: usize) -> "
           "bool;",
           "module std.meta; @intrinsic(\"meta.read_public\") fn read(id: "
           "usize) -> bool { return false; }",
       }) {
    SCOPED_TRACE(Source);
    auto Parsed = lex::Lexer().parse(Source);
    ASSERT_TRUE(Parsed.ok());
    sema::Sema Analysis;
    ASSERT_FALSE(Analysis.Check(*Parsed.root));
    EXPECT_TRUE(std::any_of(
        Analysis.GetDiagnostics().begin(), Analysis.GetDiagnostics().end(),
        [](const lex::Diagnostic &Diagnostic) {
          return Diagnostic.Kind ==
                 lex::DiagnosticKind::InvalidIntrinsicDeclaration;
        }));
  }
}

TEST(Sema, ClassMetadataQueries) {
  auto Parsed = lex::Lexer().parse(R"(
class Plain {}
class Explicit { init(value: i32) {} deinit() {} }
class Derived: Plain {}
@interface class Contract {}
@final class FinalClass {}
fn inspect() -> i32 {
  when !meta(Plain).has_constructor() && meta(Plain).has_default_constructor() &&
       !meta(Plain).has_destructor() && !meta(Plain).has_base_class() &&
       meta(Explicit).has_constructor() && !meta(Explicit).has_default_constructor() &&
       meta(Explicit).has_destructor() && meta(Derived).has_base_class() &&
       meta(Contract).is_interface() && meta(FinalClass).is_final() {
    return 1;
  } else {
    return missing;
  }
}
)");
  ASSERT_TRUE(Parsed.ok());
  sema::Sema Analysis;
  EXPECT_TRUE(Analysis.Check(*Parsed.root));
}

TEST(Sema, MemberMetadataQueries) {
  auto Parsed = lex::Lexer().parse(R"(
class Inspect {
  pub value: i32;
  @static
  pub shared: i32;
  pub fn read() -> i32 { return value; }
  @static
  pub fn create() -> i32 { return 1; }
  init() { value = 0; }
  deinit() {}
}
@interface class Contract {
  const CAPACITY: i32 = 64;
}
fn inspect() -> i32 {
  when meta(Inspect).has_member("value") &&
       meta(Inspect).has_field("shared") &&
       meta(Inspect).has_function("read") &&
       meta(Inspect).has_function("init") &&
       !meta(Inspect).has_field("read") &&
       !meta(Inspect).has_function("missing") &&
       !meta(Inspect.value).is_static() &&
       meta(Inspect.shared).is_static() &&
       meta(Inspect.read).is_method() &&
       !meta(Inspect.read).is_static() &&
       meta(Inspect.create).is_static() &&
       meta(Inspect.init).is_constructor() &&
       meta(Inspect.deinit).is_destructor() &&
       meta(Contract.CAPACITY).is_static() {
    return 1;
  } else {
    return missing;
  }
}
)");
  ASSERT_TRUE(Parsed.ok());
  sema::Sema Analysis;
  const bool Valid = Analysis.Check(*Parsed.root);
  for (const auto &Diagnostic : Analysis.GetDiagnostics())
    ADD_FAILURE() << Diagnostic;
  EXPECT_TRUE(Valid);
  if (!Valid)
    return;
  const auto ClassId =
      Analysis.GetReflection().Find("Inspect", sema::MetaKind::Class);
  ASSERT_TRUE(ClassId.has_value());
  std::vector<std::string> Names;
  for (const auto Id : Analysis.GetReflection().Get(*ClassId).Children)
    Names.push_back(Analysis.GetReflection().Get(Id).Name);
  EXPECT_EQ(Names, (std::vector<std::string>{"value", "shared", "read",
                                             "create", "init", "deinit"}));
}

TEST(Sema, MemberMetadataQueriesRespectVisibility) {
  lex::Lexer Lexer;
  auto Main = Lexer.parse(R"(
module app;
import library;
fn inspect() -> i32 {
  when meta(library.Inspect).has_field("visible") &&
       !meta(library.Inspect).has_field("hidden") &&
       meta(library.Inspect).has_function("read") &&
       !meta(library.Inspect).has_function("secret") {
    return 1;
  } else {
    return missing;
  }
}
)");
  auto Library = Lexer.parse(R"(
module library;
pub class Inspect {
  pub visible: i32;
  hidden: i32;
  pub fn read() -> i32 { return visible; }
  fn secret() -> i32 { return hidden; }
}
)");
  ASSERT_TRUE(Main.ok());
  ASSERT_TRUE(Library.ok());
  sema::Sema Analysis;
  const bool Valid = Analysis.CheckModules(
      {{Main.root.get(), true}, {Library.root.get(), false}});
  for (const auto &Diagnostic : Analysis.GetDiagnostics())
    ADD_FAILURE() << Diagnostic;
  EXPECT_TRUE(Valid);
}

TEST(Sema, MetaQueriesUseLoadedModuleDefinition) {
  lex::Lexer Lexer;
  auto Main = Lexer.parse(R"(
module app;
import std.meta;
pub fn target() {}
fn inspect() -> i32 {
  when meta(target).is_public() { return 1; } else { return 0; }
}
)");
  auto Meta = Lexer.parse("module std.meta; pub class Symbol {} ");
  ASSERT_TRUE(Main.ok());
  ASSERT_TRUE(Meta.ok());
  sema::Sema Analysis;
  EXPECT_FALSE(Analysis.CheckModules(
      {{Main.root.get(), true}, {Meta.root.get(), false, true}}));
  EXPECT_TRUE(std::any_of(
      Analysis.GetDiagnostics().begin(), Analysis.GetDiagnostics().end(),
      [](const auto &Diagnostic) {
        return Diagnostic.Kind == lex::DiagnosticKind::InvalidWhenCondition;
      }));
}

TEST(Sema, MetaQueryExecutesStandardLibraryMethodBody) {
  lex::Lexer Lexer;
  auto Main = Lexer.parse(R"(
module app;
import std.meta;
pub fn target() {}
fn inspect() -> i32 {
  when meta(target).is_public() { return 1; } else { return missing; }
}
)");
  auto Meta = Lexer.parse(R"(
@meta module std.meta;
pub class Symbol {
  pub id: usize;
  pub fn is_public() -> bool {
    let result = std.meta.__read_public(id);
    if result { return true; }
    return false;
  }
}
@intrinsic
pub fn __read_public(id: usize) -> bool;
)");
  ASSERT_TRUE(Main.ok());
  ASSERT_TRUE(Meta.ok());
  sema::Sema Analysis;
  EXPECT_TRUE(Analysis.CheckModules(
      {{Main.root.get(), true}, {Meta.root.get(), false, true}}));
}

TEST(Sema, MetaModuleIsCompileTimeOnly) {
  lex::Lexer Lexer;
  auto Meta = Lexer.parse(R"(
@meta module catalog;
pub class Descriptor {}
pub fn flag() -> bool { return true; }
)");
  ASSERT_TRUE(Meta.ok());
  {
    auto Main = Lexer.parse("module app; import catalog; fn use() -> bool { "
                            "return catalog.flag(); }");
    ASSERT_TRUE(Main.ok());
    sema::Sema Analysis;
    EXPECT_FALSE(Analysis.CheckModules(
        {{Main.root.get(), true}, {Meta.root.get(), false}}));
    EXPECT_TRUE(Analysis.IsMetaModule("catalog"));
  }
  {
    auto Main = Lexer.parse("module app; import catalog; class Store { "
                            "value: catalog.Descriptor; }");
    ASSERT_TRUE(Main.ok());
    sema::Sema Analysis;
    EXPECT_FALSE(Analysis.CheckModules(
        {{Main.root.get(), true}, {Meta.root.get(), false}}));
  }
}

TEST(Sema, MetaClassFunctionAndMethodAreCompileTimeOnly) {
  lex::Lexer Lexer;
  auto Library = Lexer.parse(R"(
module catalog;
@meta pub class Descriptor {}
@meta pub fn inspect() -> bool { return true; }
pub class Service {
  @meta pub fn describe() -> bool { return true; }
  pub fn run() -> bool { return true; }
}
)");
  ASSERT_TRUE(Library.ok());
  const auto Reject = [&](std::string_view Source) {
    auto Main = Lexer.parse(std::string(Source));
    EXPECT_TRUE(Main.ok());
    if (!Main.ok())
      return;
    sema::Sema Analysis;
    EXPECT_FALSE(Analysis.CheckModules(
        {{Main.root.get(), true}, {Library.root.get(), false}}));
  };
  Reject(
      "module app; import catalog; class Store { value: catalog.Descriptor; }");
  Reject("module app; import catalog; fn use() -> bool { return "
         "catalog.inspect(); }");
  Reject("module app; import catalog; fn use() -> bool { return "
         "catalog.Service().describe(); }");
  auto Main =
      Lexer.parse("module app; import catalog; fn use() -> bool { "
                  "let service = catalog.Service(); return service.run(); }");
  ASSERT_TRUE(Main.ok());
  sema::Sema Analysis;
  EXPECT_TRUE(Analysis.CheckModules(
      {{Main.root.get(), true}, {Library.root.get(), false}}));
}

TEST(Sema, MetaEntrypointIsRejected) {
  lex::Lexer Lexer;
  auto Source =
      Lexer.parse("module app; @meta @main fn main() -> i32 { return 0; }");
  ASSERT_TRUE(Source.ok());
  sema::Sema Analysis;
  EXPECT_FALSE(Analysis.CheckModules({{Source.root.get(), true}}));
}
