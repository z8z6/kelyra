#include "../TestSource.h"
#include "Front/Lexer/Formatter.h"
#include "Front/Parser/Parser.h"

#include <gtest/gtest.h>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace kelyra::lex;
Parser parser;

void spans(const Node &node, std::size_t size) {
  ASSERT_TRUE(node.Loc.Begin <= node.Loc.End && node.Loc.End <= size) << "valid node span";
  for (const auto &child : node.children) {
    ASSERT_TRUE(child->Loc.Begin >= node.Loc.Begin && child->Loc.End <= node.Loc.End)
        << "child span contained";
    spans(*child, size);
  }
}
void expression(const std::string &source, const std::string &expected) {
  auto result = parser.parseExpression(source);
  ASSERT_TRUE(result.ok() && result.root) << "expression parses: " + source;
  EXPECT_EQ(DumpAst(*result.root), expected) << source;
  spans(*result.root, source.size());
}
} // namespace

TEST(Frontend, ExpressionAst) {
  expression("a + b * c", "(Binary \"+\" (Name \"a\") (Binary \"*\" (Name \"b\") (Name \"c\")))");
  expression("a - b - c", "(Binary \"-\" (Binary \"-\" (Name \"a\") (Name \"b\")) (Name \"c\"))");
  expression("-(a + b)", "(Unary \"-\" (Group (Binary \"+\" (Name \"a\") (Name \"b\"))))");
  expression("f(x,)[i].value",
             "(Member \"value\" (Index (Call (Name \"f\") "
             "(Name \"x\")) (Name \"i\")))");
  expression("a || b && c == d + e",
             "(Binary \"||\" (Name \"a\") (Binary \"&&\" (Name \"b\") (Binary "
             "\"==\" (Name \"c\") (Binary \"+\" (Name \"d\") (Name \"e\")))))");
  expression("!f()", "(Unary \"!\" (Call (Name \"f\")))");
  expression("1.25e-3", "(Literal \"1.25e-3\")");
  expression("true", "(Literal \"true\")");
  expression("meta(*u8)", "(Meta (PointerType (Type \"u8\")))");
  expression("meta { let x = 2; x + 3 }",
             "(MetaBlock (Let \"let\" (Name \"x\") (Literal \"2\")) "
             "(Binary \"+\" (Name \"x\") (Literal \"3\")))");
  expression("\"a\\n\"", "(Literal \"\\\"a\\\\n\\\"\")");
  expression("identity<i32>(value)",
             "(Call (GenericApply (Name \"identity\") (Type \"i32\")) "
             "(Name \"value\"))");
  expression("a / b % c", "(Binary \"%\" (Binary \"/\" (Name \"a\") (Name \"b\")) (Name \"c\"))");
}

TEST(Frontend, Format) {
  auto Parsed =
      parser.parse("module app.main; import math.vector; @Vertex class Pair{left:i32;"
                   "right:[4]i32;} pub fn main()->i32{let x:*c.Pair=c.make(1,);let value=1;"
                   "let pointer:*i32=&value;*pointer=*pointer+1;if !x{"
                   "return -1;}else{return 0;}} // end\n");
  ASSERT_TRUE(Parsed.ok());
  const std::string Expected = "module app.main;\n"
                               "\n"
                               "import math.vector;\n\n"
                               "@Vertex\n"
                               "class Pair {\n"
                               "  left : i32;\n"
                               "  right: [4]i32;\n"
                               "}\n\n"
                               "pub fn main() -> i32 {\n"
                               "  let x: *c.Pair = c.make(1,);\n"
                               "  let value = 1;\n"
                               "  let pointer: *i32 = &value;\n"
                               "  *pointer = *pointer + 1;\n"
                               "  if !x {\n"
                               "    return -1;\n"
                               "  } else {\n"
                               "    return 0;\n"
                               "  }\n"
                               "}\n\n"
                               "// end\n";
  EXPECT_EQ(Format(Parsed), Expected);
  auto Formatted = parser.parse(Expected);
  ASSERT_TRUE(Formatted.ok());
  EXPECT_EQ(Format(Formatted), Expected) << "formatting is idempotent";
}

TEST(Frontend, EnumMatchFormatting) {
  auto Parsed = parser.parse("enum Flag:u8{Off,On=2,} fn pick(flag:Flag)->i32{"
                             "let value=match flag{Flag.Off=>0,Flag.On=>{let x=1;x},};"
                             "return value;}");
  ASSERT_TRUE(Parsed.ok());
  const auto Formatted = Format(Parsed);
  EXPECT_NE(Formatted.find("enum Flag: u8 {\n"), std::string::npos);
  EXPECT_NE(Formatted.find("Flag.On => {\n"), std::string::npos);
  EXPECT_NE(Formatted.find("  };\n  return value;"), std::string::npos);
  auto Again = parser.parse(Formatted);
  ASSERT_TRUE(Again.ok());
  EXPECT_EQ(Format(Again), Formatted);
}

TEST(Frontend, AnnotationImplementationFormatting) {
  auto Parsed = parser.parse("@target(Target.Class T) annotation mark(value:i32)=@A(value)+@B{"
                             "@static count:i32;@static pub fn next()->i32{return count;}}"
                             "@mark(3) class Item{}");
  ASSERT_TRUE(Parsed.ok());
  const auto Formatted = Format(Parsed);
  EXPECT_NE(Formatted.find("@target(Target.Class T)\n"), std::string::npos);
  EXPECT_NE(Formatted.find("= @A(value) + @B {"), std::string::npos);
  EXPECT_NE(Formatted.find("@static\n  count: i32;"), std::string::npos);
  auto Again = parser.parse(Formatted);
  ASSERT_TRUE(Again.ok());
  EXPECT_EQ(Format(Again), Formatted);
}

TEST(Frontend, AnnotationConditionalMembers) {
  auto Parsed = parser.parse("@target(Target.Class T) annotation choice(enabled:bool){when enabled{"
                             "pub fn one()->i32{return 1;}}else when meta(T).has_field(\"id\"){"
                             "id_copy:i32;}else{pub fn none()->i32{return 0;}}}");
  ASSERT_TRUE(Parsed.ok());
  const auto Formatted = Format(Parsed);
  EXPECT_NE(Formatted.find("when enabled {"), std::string::npos);
  EXPECT_NE(Formatted.find("else when meta(T).has_field(\"id\")"), std::string::npos);
  auto Again = parser.parse(Formatted);
  ASSERT_TRUE(Again.ok());
  EXPECT_EQ(Format(Again), Formatted);
}

TEST(Frontend, MetaModuleAnnotation) {
  auto Parsed = parser.parse("@meta module catalog; @target(Target.Module) pub annotation meta();");
  ASSERT_TRUE(Parsed.ok());
  const auto Formatted = Format(Parsed);
  EXPECT_NE(Formatted.find("@meta\nmodule catalog;"), std::string::npos);
  auto Again = parser.parse(Formatted);
  ASSERT_TRUE(Again.ok());
}

TEST(Frontend, PrefixArrayTypes) {
  const std::string Source = "fn types() { let whole:*[2]i32; let elements:[2]*i32; "
                             "let computed:[1+2]i32; }";
  auto Parsed = parser.parse(Source);
  ASSERT_TRUE(Parsed.ok());
  EXPECT_EQ(Format(Parsed),
            "fn types() {\n"
            "  let whole: *[2]i32;\n"
            "  let elements: [2]*i32;\n"
            "  let computed: [1 + 2]i32;\n"
            "}\n");
  const auto Ast = DumpAst(*Parsed.root);
  EXPECT_NE(Ast.find("(PointerType (ArrayType \"2\" (Literal \"2\") "
                     "(Type \"i32\")))"),
            std::string::npos);
  EXPECT_NE(Ast.find("(ArrayType \"2\" (Literal \"2\") "
                     "(PointerType (Type \"i32\")))"),
            std::string::npos);
  EXPECT_FALSE(parser.parse("fn old(value: i32[2]) {}").ok());
}

TEST(Frontend, GenericSyntaxAndFormatting) {
  auto Parsed = parser.parse("class Box<T>{value:T;init(value:T){this.value=value;}}"
                             "class Pair<T,U>{first:T;second:U;}"
                             "alias Ptr<T> = *T;"
                             "fn identity<T>(value:T)->T{return value;}"
                             "fn use()->i32{let pair:Pair<i32,Box<i32>> = "
                             "Pair<i32,Box<i32>>(1,Box<i32>(42));"
                             "let box:Box<i32> = Box<i32>(identity<i32>(42));"
                             "return box.value;}");
  ASSERT_TRUE(Parsed.ok());
  const auto Formatted = Format(Parsed);
  EXPECT_NE(Formatted.find("class Box<T> {"), std::string::npos);
  EXPECT_NE(Formatted.find("fn identity<T>(value: T) -> T"), std::string::npos);
  EXPECT_NE(Formatted.find("class Pair<T, U> {"), std::string::npos);
  EXPECT_NE(Formatted.find("alias Ptr<T> = *T;"), std::string::npos);
  EXPECT_NE(Formatted.find("Pair<i32, Box<i32>>(1, Box<i32>(42))"), std::string::npos);
  EXPECT_NE(Formatted.find("Box<i32>(identity<i32>(42))"), std::string::npos);
  auto Again = parser.parse(Formatted);
  ASSERT_TRUE(Again.ok());
  EXPECT_EQ(Format(Again), Formatted);
}

TEST(Frontend, ScopedAliasesAndOrderedDeclarations) {
  auto Parsed = parser.parse("fn zebra(){alias Local=i32;let value:Local=1;}"
                             "class Beta<T>{alias Item=T;alias Pointer<U> = *U;value:Item;}"
                             "fn alpha(){let ptr:Beta<i32>.Pointer<u8>;}class Alpha{}");
  ASSERT_TRUE(Parsed.ok());
  const auto Formatted = Format(Parsed);
  EXPECT_LT(Formatted.find("class Alpha"), Formatted.find("class Beta"));
  EXPECT_LT(Formatted.find("class Beta"), Formatted.find("fn alpha"));
  EXPECT_LT(Formatted.find("fn alpha"), Formatted.find("fn zebra"));
  EXPECT_NE(Formatted.find("Beta<i32>.Pointer<u8>"), std::string::npos);
  auto Again = parser.parse(Formatted);
  ASSERT_TRUE(Again.ok());
  EXPECT_EQ(Format(Again), Formatted);
}

TEST(Frontend, OrderedDeclarationsPreserveDetachedComments) {
  auto Parsed = parser.parse("// file header\n\n"
                             "fn zebra() {}\n"
                             "// section\n\n"
                             "class Beta {}\n"
                             "class Alpha {}\n");
  ASSERT_TRUE(Parsed.ok());
  const auto Formatted = Format(Parsed);
  EXPECT_TRUE(Formatted.starts_with("// file header\n\n"));
  auto Again = parser.parse(Formatted);
  ASSERT_TRUE(Again.ok());
  EXPECT_EQ(Format(Again), Formatted);
}

TEST(Frontend, ShortensOnlyUnambiguousImportedNames) {
  auto Parsed = parser.parse("module main; import demo.math;"
                             "fn use(value: demo.math.Vec4) -> i32 {"
                             "return demo.math.make(); }");
  ASSERT_TRUE(Parsed.ok());
  FormatSymbols Symbols;
  Symbols.CurrentModule = "main";
  Symbols.Visible["demo.math"] = {"Vec4", "make"};
  Symbols.Complete = true;
  const auto Formatted = Format(Parsed, Symbols);
  EXPECT_NE(Formatted.find("value: Vec4"), std::string::npos);
  EXPECT_NE(Formatted.find("return make();"), std::string::npos);
  Symbols.Visible["other.math"] = {"make"};
  EXPECT_NE(Format(Parsed, Symbols).find("demo.math.make()"), std::string::npos);
  auto Shadowed = parser.parse("module main; import demo.math;"
                               "fn use(demo: i32) -> i32 { return demo.math.make(); }");
  ASSERT_TRUE(Shadowed.ok());
  Symbols.Visible.erase("other.math");
  EXPECT_NE(Format(Shadowed, Symbols).find("demo.math.make()"), std::string::npos);
  auto Unrelated = parser.parse("module main; import demo.math;"
                                "fn first() { alias make = i32; }"
                                "fn second() -> i32 { return demo.math.make(); }");
  ASSERT_TRUE(Unrelated.ok());
  EXPECT_NE(Format(Unrelated, Symbols).find("return make();"), std::string::npos);
  auto Inherited = parser.parse("module main; import demo.math;"
                                "class Derived: Base { fn call() -> i32 {"
                                "return demo.math.make(); } }");
  ASSERT_TRUE(Inherited.ok());
  EXPECT_NE(Format(Inherited, Symbols).find("demo.math.make()"), std::string::npos);
  auto QualifiedAnnotation = parser.parse("@std.annotation.final class Box {}");
  ASSERT_TRUE(QualifiedAnnotation.ok());
  Symbols.Visible["std.annotation"] = {"final"};
  EXPECT_NE(Format(QualifiedAnnotation, Symbols).find("@final"), std::string::npos);
}

TEST(Frontend, GenericClassMemberCall) {
  auto Parsed = parser.parseExpression("Cache<i32>.instance()");
  ASSERT_TRUE(Parsed.ok());
  const auto Ast = DumpAst(*Parsed.root);
  EXPECT_NE(Ast.find("GenericApply"), std::string::npos);
  EXPECT_NE(Ast.find("instance"), std::string::npos);
}

TEST(Frontend, ForwardConstructorSyntaxAndFormatting) {
  const auto Parsed = parser.parse("class Box<T>{value:T;init<Args...>(@forward args:...Args)"
                                   "{this.value=T(...args);}}");
  ASSERT_TRUE(Parsed.ok());
  const auto Ast = DumpAst(*Parsed.root);
  EXPECT_NE(Ast.find("GenericPack"), std::string::npos);
  EXPECT_NE(Ast.find("ParameterPack"), std::string::npos);
  EXPECT_NE(Ast.find("Spread"), std::string::npos);
  const auto Formatted = Format(Parsed);
  EXPECT_NE(Formatted.find("init<Args...>(@forward args: ...Args)"), std::string::npos);
  const auto Again = parser.parse(Formatted);
  ASSERT_TRUE(Again.ok());
  EXPECT_EQ(Format(Again), Formatted);
}

TEST(Frontend, FormatFieldsAndAnnotatedParameters) {
  const auto Parsed = parser.parse(R"(
class Thing {
  pub x:i32;
  pub longer_name: f64 ;
  @tag pub fn run(@forward first:i32,
                  second:f64) -> void { return; }
}
)");
  ASSERT_TRUE(Parsed.ok());
  const auto Formatted = Format(Parsed);
  EXPECT_NE(Formatted.find("  pub x          : i32;\n"
                           "  pub longer_name: f64;\n\n"
                           "  @tag\n  pub fn run(\n"
                           "    @forward first : i32,\n"
                           "             second: f64\n  )"),
            std::string::npos)
      << Formatted;
  const auto Again = parser.parse(Formatted);
  ASSERT_TRUE(Again.ok());
  EXPECT_EQ(Format(Again), Formatted);
}

TEST(Frontend, FormatShaderParameterAnnotations) {
  const auto Parsed = parser.parse(R"(
@fragment
pub fn fragment_main(@location(0) uv: Vec2,
                   @binding(0,0) image: Texture2D,
                   @binding(0, 1) sampler: SamplerState) -> FragmentOutput {
  return FragmentOutput(sample_2d(image, sampler, uv));
}
)");
  ASSERT_TRUE(Parsed.ok());
  const auto Formatted = Format(Parsed);
  EXPECT_NE(Formatted.find("pub fn fragment_main(\n"
                           "  @location(0)   uv     : Vec2,\n"
                           "  @binding(0, 0) image  : Texture2D,\n"
                           "  @binding(0, 1) sampler: SamplerState\n"
                           ") -> FragmentOutput"),
            std::string::npos)
      << Formatted;
  const auto Again = parser.parse(Formatted);
  ASSERT_TRUE(Again.ok());
  EXPECT_EQ(Format(Again), Formatted);
}

TEST(Frontend, FormatLongFirstParameterOnNewLine) {
  const auto Parsed =
      parser.parse("fn format_a_very_long_function_name_with_many_descriptive_words("
                   "first_argument:i32, second_argument:i32) -> void { return; }");
  ASSERT_TRUE(Parsed.ok());
  const auto Formatted = Format(Parsed);
  EXPECT_NE(Formatted.find("(\n  first_argument : i32,\n"
                           "  second_argument: i32\n)"),
            std::string::npos)
      << Formatted;
  const auto Again = parser.parse(Formatted);
  ASSERT_TRUE(Again.ok());
  EXPECT_EQ(Format(Again), Formatted);
}

TEST(Frontend, FormatPointerCasts) {
  const auto Parsed = parser.parse(R"(
fn worker(context: *u8) -> *u8 {
  let slot = context as*usize;
  *slot = Counter.instance() as usize;
  return 0 as*u8;
}
)");
  ASSERT_TRUE(Parsed.ok());
  const auto Formatted = Format(Parsed);
  EXPECT_NE(Formatted.find("let slot = context as *usize;"), std::string::npos);
  EXPECT_NE(Formatted.find("*slot = Counter.instance() as usize;"), std::string::npos);
  EXPECT_NE(Formatted.find("return 0 as *u8;"), std::string::npos);
  const auto Again = parser.parse(Formatted);
  ASSERT_TRUE(Again.ok());
  EXPECT_EQ(Format(Again), Formatted);
}

TEST(Frontend, FormatLongLogicalConditionAndBlankLines) {
  const auto Parsed = parser.parse(R"(
fn check(box: *u8, other: *u8, same: *u8) -> i32 {
  if box as usize == 0 || other as usize == 0 || box as usize != same as usize || box as usize == other as usize {
    return 3;
  }

  let first = 1;

  let second = 2;
  return first + second;
}
)");
  ASSERT_TRUE(Parsed.ok());
  const auto Formatted = Format(Parsed);
  EXPECT_NE(Formatted.find("  if box as usize == 0 ||\n"
                           "    other as usize == 0 ||\n"
                           "    box as usize != same as usize ||\n"
                           "    box as usize == other as usize {"),
            std::string::npos)
      << Formatted;
  EXPECT_NE(Formatted.find("  }\n\n  let first = 1;\n\n"
                           "  let second = 2;"),
            std::string::npos)
      << Formatted;
  const auto Again = parser.parse(Formatted);
  ASSERT_TRUE(Again.ok());
  EXPECT_EQ(Format(Again), Formatted);
}

TEST(Frontend, FormatAnnotatedFieldNamesAndColons) {
  const auto Parsed = parser.parse(R"(
@reflect
class Actor {
  pub health: i32;
  hidden: i32;
  @reflect
  id: u64;
}
)");
  ASSERT_TRUE(Parsed.ok());
  const auto Formatted = Format(Parsed);
  EXPECT_NE(Formatted.find("  pub health: i32;\n"
                           "      hidden: i32;\n"
                           "  @reflect\n"
                           "      id    : u64;"),
            std::string::npos)
      << Formatted;
  const auto Again = parser.parse(Formatted);
  ASSERT_TRUE(Again.ok());
  EXPECT_EQ(Format(Again), Formatted);
}

TEST(Frontend, PreserveBlankLineBetweenParameters) {
  const auto Parsed = parser.parse(R"(
fn sum(first: i32,

       second: i32) -> i32 {
  return first + second;
}
)");
  ASSERT_TRUE(Parsed.ok());
  const auto Formatted = Format(Parsed);
  EXPECT_NE(Formatted.find("first : i32,\n\n"), std::string::npos) << Formatted;
  const auto Again = parser.parse(Formatted);
  ASSERT_TRUE(Again.ok());
  EXPECT_EQ(Format(Again), Formatted);
}

TEST(Frontend, GenericFunctionPackSyntax) {
  const auto Parsed = parser.parse("fn invoke<Args...>(args:...Args)->i32{return answer(...args);}"
                                   "fn answer()->i32{return 42;}"
                                   "fn use()->i32{return invoke<>();}");
  ASSERT_TRUE(Parsed.ok());
  const auto Ast = DumpAst(*Parsed.root);
  EXPECT_NE(Ast.find("GenericPack"), std::string::npos);
  EXPECT_NE(Ast.find("ParameterPack"), std::string::npos);
  EXPECT_NE(Ast.find("Spread"), std::string::npos);
  EXPECT_NE(Ast.find("GenericApply"), std::string::npos);
  const auto Formatted = Format(Parsed);
  EXPECT_NE(Formatted.find("invoke<>()"), std::string::npos);
  const auto Again = parser.parse(Formatted);
  ASSERT_TRUE(Again.ok());
  EXPECT_EQ(Format(Again), Formatted);
}

TEST(Frontend, InterfaceDeclaration) {
  const std::string Source = R"(
@interface pub class Reader {
  pub const CAPACITY: i32 = 64;
  pub fn read(count: i32) -> i32;
  pub fn ready() -> bool { return true; }
}
)";
  auto Parsed = parser.parse(Source);
  ASSERT_TRUE(Parsed.ok());
  const auto Ast = DumpAst(*Parsed.root);
  EXPECT_NE(Ast.find("(Class \"Reader\""), std::string::npos);
  EXPECT_NE(Ast.find("(ConstField \"CAPACITY\""), std::string::npos);
  EXPECT_TRUE(parser.parse(Format(Parsed)).ok());
  EXPECT_FALSE(parser.parse("interface Reader {}").ok());
}

TEST(Frontend, ModuleTargetCondition) {
  auto Parsed = parser.parse("@cfg(os.Linux) module platform.linux;\nfn "
                             "value() -> i32 { return 1; }");
  ASSERT_TRUE(Parsed.ok());
  const auto Ast = DumpAst(*Parsed.root);
  EXPECT_NE(Ast.find("(ModuleDecl \"platform.linux\" (Annotation \"cfg\""), std::string::npos);
  const auto Formatted = Format(Parsed);
  EXPECT_NE(Formatted.find("@cfg(os.Linux)\nmodule platform.linux;"), std::string::npos);
  EXPECT_TRUE(parser.parse(Formatted).ok());
  EXPECT_TRUE(parser.parseExpression("meta(std.annotation.main)").ok());
}

TEST(Frontend, FormatEachAnnotationOnOwnLine) {
  auto Parsed = parser.parse("@cfg(os.Linux) @trace module sample; "
                             "@cfg(os.Linux) import math; "
                             "@tag @mark pub class Thing { "
                             "@tag @mark pub value:i32; "
                             "@tag @mark pub fn run()->i32{return 1;} "
                             "} @extern fn external()->i32;");
  ASSERT_TRUE(Parsed.ok());
  const auto Formatted = Format(Parsed);
  EXPECT_NE(Formatted.find("@cfg(os.Linux)\n@trace\nmodule sample;"), std::string::npos);
  EXPECT_NE(Formatted.find("@cfg(os.Linux)\nimport math;"), std::string::npos);
  EXPECT_NE(Formatted.find("@tag\n@mark\npub class Thing {"), std::string::npos);
  EXPECT_NE(Formatted.find("  @tag\n  @mark\n  pub value: i32;"), std::string::npos);
  EXPECT_NE(Formatted.find("  @tag\n  @mark\n  pub fn run() -> i32 {"), std::string::npos);
  EXPECT_NE(Formatted.find("@extern\nfn external() -> i32;"), std::string::npos);
  auto Reparsed = parser.parse(Formatted);
  ASSERT_TRUE(Reparsed.ok()) << Formatted;
  EXPECT_EQ(Format(Reparsed), Formatted);
}

TEST(Frontend, ClassAndMultipleReturnRoundTrip) {
  const std::string Source = R"(
@tag pub class Pair {
  @tag pub left: i32;
  pub init(left: i32) { this.left = left; }
  @tag pub fn values() -> (i32, bool) { return left, true; }
  deinit() { return; }
}
fn use() -> void { let pair = Pair(1); let (value, ok) = pair.values(); }
fn factory() -> fn(i32) -> (i32, bool) { return convert; }
fn accept(callback: fn()) -> void { callback(); }
)";
  auto Parsed = parser.parse(Source);
  ASSERT_TRUE(Parsed.ok());
  spans(*Parsed.root, Source.size());
  const auto Formatted = Format(Parsed);
  auto Again = parser.parse(Formatted);
  ASSERT_TRUE(Again.ok());
  EXPECT_LT(Formatted.find("fn accept"), Formatted.find("fn factory"));
  EXPECT_LT(Formatted.find("fn factory"), Formatted.find("fn use"));
  EXPECT_EQ(Format(Again), Formatted);
  EXPECT_FALSE(parser.parse("struct Pair { left: i32 }").ok());
}

TEST(Frontend, FormatImports) {
  auto Parsed = parser.parse("module app.main; import zebra; import alpha.part; import zebra; "
                             "import alpha.part; import middle; fn main() { return; }");
  ASSERT_TRUE(Parsed.ok());
  const std::string Expected = "module app.main;\n"
                               "\n"
                               "import alpha.part;\n"
                               "import middle;\n"
                               "import zebra;\n\n"
                               "fn main() {\n"
                               "  return;\n"
                               "}\n";
  EXPECT_EQ(Format(Parsed), Expected);
  auto Formatted = parser.parse(Expected);
  ASSERT_TRUE(Formatted.ok());
  EXPECT_EQ(Format(Formatted), Expected) << "formatting is idempotent";
}

TEST(Frontend, FormatImportsKeepsCommentsAttached) {
  auto Parsed = parser.parse("module app.main;\n"
                             "// zebra docs\n"
                             "import zebra; // zebra tail\n"
                             "// alpha docs\n"
                             "import alpha;\n"
                             "fn main() { return; }");
  ASSERT_TRUE(Parsed.ok());
  const std::string Expected = "module app.main;\n\n"
                               "// alpha docs\n"
                               "import alpha;\n"
                               "// zebra docs\n"
                               "import zebra; // zebra tail\n\n"
                               "fn main() {\n"
                               "  return;\n"
                               "}\n";
  EXPECT_EQ(Format(Parsed), Expected);
  auto Formatted = parser.parse(Expected);
  ASSERT_TRUE(Formatted.ok());
  EXPECT_EQ(Format(Formatted), Expected) << "formatting is idempotent";
}

TEST(Frontend, FormatImportsKeepAnnotationsAttached) {
  auto Parsed = parser.parse("@cfg(os.Linux) import zebra; "
                             "@cfg(os.Windows) import alpha; "
                             "fn main() { return; }");
  ASSERT_TRUE(Parsed.ok());
  const auto Formatted = Format(Parsed);
  EXPECT_NE(Formatted.find("@cfg(os.Windows)\nimport alpha;"), std::string::npos);
  EXPECT_NE(Formatted.find("@cfg(os.Linux)\nimport zebra;"), std::string::npos);
  EXPECT_LT(Formatted.find("import alpha;"), Formatted.find("import zebra;"));
  auto Reparsed = parser.parse(Formatted);
  ASSERT_TRUE(Reparsed.ok()) << Formatted;
  EXPECT_EQ(Format(Reparsed), Formatted);
}

TEST(Frontend, FormatCImportsByHeader) {
  auto Parsed = parser.parse("import c \"zebra.h\"; import c \"alpha.h\"; fn main() { return; }");
  ASSERT_TRUE(Parsed.ok());
  EXPECT_EQ(Format(Parsed),
            "import c \"alpha.h\";\n"
            "import c \"zebra.h\";\n\n"
            "fn main() {\n"
            "  return;\n"
            "}\n");
}

TEST(Frontend, ExpressionValidation) {
  for (const std::string source : {"",
                                   "a +",
                                   "a b",
                                   "f(a",
                                   "a[]",
                                   "a.",
                                   "f(,)",
                                   "a < b < c",
                                   "a < b == c",
                                   "a == b < c",
                                   "1e+",
                                   "12abc",
                                   "@",
                                   "\"oops",
                                   "\"\\q\"",
                                   "/*"}) {
    SCOPED_TRACE(source);
    const auto result = parser.parseExpression(source);
    ASSERT_FALSE(result.ok()) << "reject invalid expression: " + source;
    for (const auto &d : result.diagnostics)
      ASSERT_TRUE(d.Loc.Begin <= d.Loc.End && d.Loc.End <= source.size()) << "valid error span";
  }
  ASSERT_TRUE(parser.parseExpression("(a < b) == (c < d)").ok()) << "explicit comparison groups";
  for (const std::string op :
       {"+", "-", "*", "/", "%", "==", "!=", "<", "<=", ">", ">=", "&&", "||"})
    expression("a " + op + " b", "(Binary \"" + op + "\" (Name \"a\") (Name \"b\"))");
  expression("+x", "(Unary \"+\" (Name \"x\"))");
  expression("*pointer", "(Unary \"*\" (Name \"pointer\"))");
  expression("&value", "(Unary \"&\" (Name \"value\"))");
}

TEST(Frontend, TokensAndComments) {
  auto tokens = parser.parseExpression("/* outer /* inner */ end */ -12 // tail\n");
  ASSERT_TRUE(tokens.ok() && tokens.tokens.size() == 5)
      << "comments retained and skipped by parser";
  ASSERT_TRUE(tokens.tokens[0].kind == TokenKind::comment) << "block comment token";
  ASSERT_TRUE(tokens.source.substr(tokens.tokens[1].Loc.Begin, 1) == "-")
      << "minus separate from number";
  ASSERT_TRUE(tokens.tokens.back().kind == TokenKind::end) << "EOF token";
  ASSERT_TRUE(tokens.tokens.back().Loc.Begin == tokens.source.size()) << "EOF location";
}

TEST(Frontend, TokenKinds) {
  auto result = parser.parseExpression(
      "name 1 \"s\" let fn class interface const annotation if else while for "
      "in "
      "return break "
      "continue "
      "true false meta when parallel extern defer module import pub -> = + - "
      "* / % == != < <= > >= && || ! & ( ) { } [ ] , : ; . @");
  const std::vector<TokenKind> expected = {TokenKind::name,
                                           TokenKind::number,
                                           TokenKind::string,
                                           TokenKind::keyword_let,
                                           TokenKind::keyword_fn,
                                           TokenKind::keyword_class,
                                           TokenKind::name,
                                           TokenKind::keyword_const,
                                           TokenKind::keyword_annotation,
                                           TokenKind::keyword_if,
                                           TokenKind::keyword_else,
                                           TokenKind::keyword_while,
                                           TokenKind::keyword_for,
                                           TokenKind::keyword_in,
                                           TokenKind::keyword_return,
                                           TokenKind::keyword_break,
                                           TokenKind::keyword_continue,
                                           TokenKind::keyword_true,
                                           TokenKind::keyword_false,
                                           TokenKind::keyword_meta,
                                           TokenKind::keyword_when,
                                           TokenKind::keyword_parallel,
                                           TokenKind::keyword_extern,
                                           TokenKind::keyword_defer,
                                           TokenKind::keyword_module,
                                           TokenKind::keyword_import,
                                           TokenKind::keyword_pub,
                                           TokenKind::op_arrow,
                                           TokenKind::op_assign,
                                           TokenKind::op_add,
                                           TokenKind::op_subtract,
                                           TokenKind::op_multiply,
                                           TokenKind::op_divide,
                                           TokenKind::op_remainder,
                                           TokenKind::op_equal,
                                           TokenKind::op_not_equal,
                                           TokenKind::op_less,
                                           TokenKind::op_less_equal,
                                           TokenKind::op_greater,
                                           TokenKind::op_greater_equal,
                                           TokenKind::op_and,
                                           TokenKind::op_or,
                                           TokenKind::op_not,
                                           TokenKind::op_address,
                                           TokenKind::punc_left_paren,
                                           TokenKind::punc_right_paren,
                                           TokenKind::punc_left_brace,
                                           TokenKind::punc_right_brace,
                                           TokenKind::punc_left_bracket,
                                           TokenKind::punc_right_bracket,
                                           TokenKind::punc_comma,
                                           TokenKind::punc_colon,
                                           TokenKind::punc_semicolon,
                                           TokenKind::punc_dot,
                                           TokenKind::punc_at,
                                           TokenKind::end};
  std::vector<TokenKind> actual;
  for (const auto &token : result.tokens)
    actual.push_back(token.kind);
  EXPECT_EQ(actual, expected);
  EXPECT_EQ(parser.parseExpression("include").tokens.front().kind, TokenKind::name);
  EXPECT_EQ(parser.parseExpression("mut").tokens.front().kind, TokenKind::name);
}

TEST(Frontend, UserDefinedAnnotations) {
  const std::string Source = R"(
@target(Target.Function)
pub annotation route(path: std.util.string.StringSlice, method: std.util.string.StringSlice = "GET",);

@route("/users", method = "POST")
fn create_user() -> i32 { return 0; }
)";
  auto Parsed = parser.parse(Source);
  ASSERT_TRUE(Parsed.ok());
  EXPECT_EQ(DumpAst(*Parsed.root),
            "(Module (AnnotationDecl \"route\" (Annotation \"target\" "
            "(AnnotationArgument (Member \"Function\" (Name \"Target\")))) (Public) "
            "(AnnotationParameter \"path\" (Type \"std.util.string.StringSlice\")) "
            "(AnnotationParameter \"method\" (Type \"std.util.string.StringSlice\") "
            "(Literal \"\\\"GET\\\"\"))) (Function \"create_user\" "
            "(Annotation \"route\" (AnnotationArgument (Literal "
            "\"\\\"/users\\\"\")) (AnnotationArgument \"method\" (Literal "
            "\"\\\"POST\\\"\"))) (Type \"i32\") (Block (Return (Literal "
            "\"0\")))))");

  const auto Formatted = Format(Parsed);
  auto Reparsed = parser.parse(Formatted);
  ASSERT_TRUE(Reparsed.ok()) << Formatted;
  EXPECT_EQ(Format(Reparsed), Formatted);
}

TEST(Frontend, TargetEnumAndIdentifierInterpolation) {
  const std::string Source = R"(
@target(Target.Field F)
annotation getter() {
  pub fn get_${F.name}() -> F.type { return F; }
}
)";
  auto Parsed = parser.parse(Source);
  ASSERT_TRUE(Parsed.ok());
  EXPECT_EQ(Format(parser.parse(Format(Parsed))), Format(Parsed));
  EXPECT_FALSE(parser.parse("@target(class) annotation old();").ok());
}

TEST(Frontend, WhenStatement) {
  auto Parsed = parser.parse("fn choose() -> i32 { when meta(choose).is_public() { return 1; } "
                             "else when false { return 2; } else { return 3; } }");
  ASSERT_TRUE(Parsed.ok());
  EXPECT_EQ(DumpAst(*Parsed.root),
            "(Module (Function \"choose\" (Type \"i32\") (Block (When "
            "(Call (Member \"is_public\" (Meta (Type \"choose\")))) (Block "
            "(Return (Literal \"1\"))) (When (Literal \"false\") (Block "
            "(Return (Literal \"2\"))) (Block (Return (Literal "
            "\"3\"))))))))");
}

TEST(Frontend, ModuleImportsAndVisibility) {
  const auto Source = kelyra::test::ReadSource("cli/modules/main.kly");
  auto Parsed = parser.parse(Source);
  ASSERT_TRUE(Parsed.ok());
  EXPECT_EQ(DumpAst(*Parsed.root),
            "(Module (ModuleDecl \"main\") (Import \"math.vector\") "
            "(Function \"main\" (Annotation \"main\") (Public) (Type \"i32\") "
            "(Block (Return "
            "(Call (Member \"answer\" (Member \"vector\" (Name "
            "\"math\"))))))))");
}

TEST(Frontend, ModuleImport) {
  auto Parsed = parser.parse("import math.vector; fn main() { return; }");
  ASSERT_TRUE(Parsed.ok());
  EXPECT_FALSE(parser.parse("import math.vector.*;").ok());
  EXPECT_EQ(DumpAst(*Parsed.root),
            "(Module (Import \"math.vector\") (Function \"main\" (Block "
            "(Return))))");
  EXPECT_EQ(Format(Parsed), "import math.vector;\n\nfn main() {\n  return;\n}\n");
}

TEST(Frontend, InlineAssembly) {
  auto Parsed = parser.parse(R"(
fn add(left: i32, right: i32) -> i32 {
  let result: i32 = left;
  asm {
    add {result}, {right}
  }.in(result).in(right).out(result).op(intel, nomem, nostack);
  return result;
}
)");
  ASSERT_TRUE(Parsed.ok());
  const auto Ast = DumpAst(*Parsed.root);
  EXPECT_NE(Ast.find("(Asm \""), std::string::npos);
  EXPECT_NE(Ast.find("add {result}, {right}"), std::string::npos);
}

TEST(Frontend, TokenLocations) {
  auto result = parser.parseExpression("a\n  + b");
  ASSERT_EQ(result.tokens.size(), 4u);
  EXPECT_EQ(result.tokens[0].Loc.Line, 1u);
  EXPECT_EQ(result.tokens[0].Loc.Column, 1u);
  EXPECT_EQ(result.tokens[1].Loc.Line, 2u);
  EXPECT_EQ(result.tokens[1].Loc.Column, 3u);
  EXPECT_EQ(result.tokens[2].Loc.Line, 2u);
  EXPECT_EQ(result.tokens[2].Loc.Column, 5u);
  EXPECT_EQ(result.tokens[3].Loc.Line, 2u);
  EXPECT_EQ(result.tokens[3].Loc.Column, 6u);
}

TEST(Frontend, ModuleAst) {
  const std::string source = R"(
@Vertex
class Pair { left: i32; right: [4]i32; }
fn choose(x: i32, y: i32,) -> i32 {
  let z: i32 = x + y;
  let inferred = false;
  let pending: i32;
  while z > 0 {
    z = z - 1;
    if z == 3 { continue; } else if z == 1 { break; } else { log(z); }
  }
  { obj.field = z; values[0] = z; }
  return z;
}

fn empty() { return; }
)";
  auto program = parser.parse(source);
  ASSERT_NE(program.root, nullptr);
  ASSERT_TRUE(program.ok()) << "complete module parses";
  ASSERT_EQ(program.root->children.size(), 3u) << "module declarations";
  ASSERT_TRUE(program.root->children[1]->kind == NodeKind::ast_function) << "function node";
  spans(*program.root, source.size());
  auto moved = std::move(program);
  ASSERT_TRUE(DumpAst(*moved.root).find("ArrayType \"4\"") != std::string::npos)
      << "AST survives result move";
  ASSERT_TRUE(DumpAst(*moved.root).find("Annotation \"Vertex\"") != std::string::npos)
      << "declaration annotation";
  ASSERT_TRUE(parser.parse("").ok()) << "empty module";
  auto simple = parser.parse("fn add(x: i32) -> i32 { let y = x + 1; return y; }");
  ASSERT_TRUE(simple.ok() && DumpAst(*simple.root) ==
                                 "(Module (Function \"add\" (Parameter \"x\" (Type \"i32\")) (Type "
                                 "\"i32\") (Block (Let \"let\" (Name \"y\") (Binary \"+\" (Name "
                                 "\"x\") (Literal \"1\"))) (Return (Name \"y\")))))")
      << "full declaration AST";
}

TEST(Frontend, ForInStatement) {
  const auto Parsed = parser.parse(R"(
fn scan(values: *Range) -> i32 {
  let total: i32 = 0;
  for value in values {
    total = total + value;
  }
  return total;
}
)");
  ASSERT_TRUE(Parsed.ok());
  EXPECT_NE(DumpAst(*Parsed.root).find("(For"), std::string::npos);
  EXPECT_TRUE(parser.parse(Format(Parsed)).ok());
}

TEST(Frontend, RejectInvalidModules) {
  for (const std::string invalid : {"let x = 1;",
                                    "fn f(x) {}",
                                    "fn f() { let x; }",
                                    "class S { x i32 }",
                                    "fn f() { 1 = 2; }",
                                    "fn f() { a = b = c; }",
                                    "fn f() { return 1 }",
                                    "fn f() { if true return; }",
                                    "fn f() {",
                                    "fn f() { break 1; }",
                                    "fn f() { let fn = 1; }",
                                    "@ fn f() {}",
                                    "@Test let x = 1;",
                                    "module ; fn f() {}",
                                    "import ; fn f() {}"})
    ASSERT_FALSE(parser.parse(invalid).ok()) << "reject invalid module: " + invalid;
}

TEST(Frontend, ErrorRecovery) {
  auto recovery = parser.parse("fn f() { let x = ; let y = ; return 7; } fn good() {}");
  ASSERT_EQ(recovery.diagnostics.size(), 2u) << "report independent statement errors";
  ASSERT_NE(recovery.root, nullptr);
  ASSERT_EQ(recovery.root->children.size(), 2u) << "recover subsequent function";
  ASSERT_FALSE(recovery.root->children[0]->children.empty());
  const auto &body = *recovery.root->children[0]->children.back();
  ASSERT_TRUE(body.children.size() == 1 && body.children[0]->kind == NodeKind::ast_return)
      << "recover subsequent statement";
  auto missingBrace = parser.parse("fn broken() { return; fn good() {}");
  ASSERT_FALSE(missingBrace.ok() && missingBrace.root->children.size() == 1 &&
               missingBrace.root->children[0]->text == "good")
      << "missing brace preserves next declaration";
  auto delimiter = parser.parse("fn broken() { let x = } fn good() {}");
  ASSERT_TRUE(delimiter.diagnostics.size() == 1 && delimiter.root->children.size() == 2)
      << "expression error preserves closing brace";
}

TEST(Frontend, DiagnosticLocations) {
  auto diagnostic = parser.parse("fn f() {\n  let x = ;\n}", "test.kly");
  ASSERT_EQ(diagnostic.diagnostics.size(), 1u);
  EXPECT_EQ(diagnostic.diagnostics[0].Loc.Begin, 19u) << "exact diagnostic offset";
  EXPECT_EQ(diagnostic.diagnostics[0].Kind, DiagnosticKind::ExpectedExpression);
  EXPECT_EQ(GetDiagnosticInfo(DiagnosticKind::ExpectedExpression).Msg, "expected expression");
  std::ostringstream Output;
  Output << diagnostic.diagnostics[0];
  ASSERT_EQ(Output.str(), "test.kly:2:11: error: expected expression")
      << "GCC-compatible diagnostic";
}

TEST(Frontend, NestingLimits) {
  ASSERT_FALSE(parser.parseExpression(std::string(200, '(') + "x" + std::string(200, ')')).ok())
      << "group nesting limit";
  ASSERT_FALSE(parser.parseExpression(std::string(200, '!') + "x").ok()) << "unary nesting limit";
  std::string chain = "a";
  for (int i = 0; i < 200; ++i)
    chain += "+a";
  ASSERT_FALSE(parser.parseExpression(chain).ok()) << "left-associated AST depth limit";
  std::string blocks = "fn f() {";
  blocks += std::string(200, '{') + std::string(200, '}') + '}';
  ASSERT_FALSE(parser.parse(blocks).ok()) << "block nesting limit";
}

TEST(Frontend, ClonePreservesDeclarationOrigins) {
  auto Parsed = kelyra::lex::Parser().parse("class Item { value: i32; }");
  ASSERT_TRUE(Parsed.ok());
  auto &Declaration = *Parsed.root->children.front();
  Declaration.GenericOriginModule = "library.generics";
  Declaration.AnnotationOriginModule = "library.annotations";
  Declaration.GenericInstance = true;
  Declaration.children.back()->AnnotationOriginModule = "library.fields";
  auto Cloned = kelyra::lex::Clone(Declaration);
  EXPECT_NE(Cloned.get(), &Declaration);
  EXPECT_EQ(Cloned->GenericOriginModule, "library.generics");
  EXPECT_EQ(Cloned->AnnotationOriginModule, "library.annotations");
  EXPECT_TRUE(Cloned->GenericInstance);
  EXPECT_EQ(Cloned->children.back()->AnnotationOriginModule, "library.fields");
  EXPECT_EQ(Cloned->Loc.Begin, Declaration.Loc.Begin);
  EXPECT_EQ(Cloned->Loc.End, Declaration.Loc.End);
}
