#include "../TestSource.h"
#include "Lexer/Formatter.h"
#include "Lexer/Lexer.h"

#include <gtest/gtest.h>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace kelyra::lex;
Lexer lexer;

void spans(const Node &node, std::size_t size) {
  ASSERT_TRUE(node.Loc.Offset <= node.Loc.End() && node.Loc.End() <= size)
      << "valid node span";
  for (const auto &child : node.children) {
    ASSERT_TRUE(child->Loc.Offset >= node.Loc.Offset &&
                child->Loc.End() <= node.Loc.End())
        << "child span contained";
    spans(*child, size);
  }
}
void expression(const std::string &source, const std::string &expected) {
  auto result = lexer.parseExpression(source);
  ASSERT_TRUE(result.ok() && result.root) << "expression parses: " + source;
  EXPECT_EQ(lexer.dumpAst(*result.root), expected) << source;
  spans(*result.root, source.size());
}
} // namespace

TEST(Frontend, ExpressionAst) {
  expression(
      "a + b * c",
      "(Binary \"+\" (Name \"a\") (Binary \"*\" (Name \"b\") (Name \"c\")))");
  expression(
      "a - b - c",
      "(Binary \"-\" (Binary \"-\" (Name \"a\") (Name \"b\")) (Name \"c\"))");
  expression("-(a + b)",
             "(Unary \"-\" (Group (Binary \"+\" (Name \"a\") (Name \"b\"))))");
  expression("f(x,)[i].value", "(Member \"value\" (Index (Call (Name \"f\") "
                               "(Name \"x\")) (Name \"i\")))");
  expression("a || b && c == d + e",
             "(Binary \"||\" (Name \"a\") (Binary \"&&\" (Name \"b\") (Binary "
             "\"==\" (Name \"c\") (Binary \"+\" (Name \"d\") (Name \"e\")))))");
  expression("!f()", "(Unary \"!\" (Call (Name \"f\")))");
  expression("1.25e-3", "(Literal \"1.25e-3\")");
  expression("true", "(Literal \"true\")");
  expression("\"a\\n\"", "(Literal \"\\\"a\\\\n\\\"\")");
  expression(
      "a / b % c",
      "(Binary \"%\" (Binary \"/\" (Name \"a\") (Name \"b\")) (Name \"c\"))");
}

TEST(Frontend, Format) {
  auto Parsed = lexer.parse(
      "module app.main; import math.vector; @Vertex struct Pair{left:i32,"
      "right:i32[4],} pub fn main()->i32{let x:c.Pair*=c.make(1,);if !x{"
      "return -1;}else{return 0;}} // end\n");
  ASSERT_TRUE(Parsed.ok());
  const std::string Expected = "module app.main;\n"
                               "import math.vector;\n\n"
                               "@Vertex\n"
                               "struct Pair {\n"
                               "  left: i32,\n"
                               "  right: i32[4],\n"
                               "}\n\n"
                               "pub fn main() -> i32 {\n"
                               "  let x: c.Pair* = c.make(1,);\n"
                               "  if !x {\n"
                               "    return -1;\n"
                               "  } else {\n"
                               "    return 0;\n"
                               "  }\n"
                               "}\n\n"
                               "// end\n";
  EXPECT_EQ(Format(Parsed), Expected);
  auto Formatted = lexer.parse(Expected);
  ASSERT_TRUE(Formatted.ok());
  EXPECT_EQ(Format(Formatted), Expected) << "formatting is idempotent";
}

TEST(Frontend, ExpressionValidation) {
  for (const std::string source :
       {"", "a +", "a b", "f(a", "a[]", "a.", "f(,)", "a < b < c", "a < b == c",
        "a == b < c", "1e+", "12abc", "@", "\"oops", "\"\\q\"", "/*"}) {
    SCOPED_TRACE(source);
    const auto result = lexer.parseExpression(source);
    ASSERT_FALSE(result.ok()) << "reject invalid expression: " + source;
    for (const auto &d : result.diagnostics)
      ASSERT_TRUE(d.Loc.Offset <= d.Loc.End() && d.Loc.End() <= source.size())
          << "valid error span";
  }
  ASSERT_TRUE(lexer.parseExpression("(a < b) == (c < d)").ok())
      << "explicit comparison groups";
  for (const std::string op :
       {"+", "-", "*", "/", "%", "==", "!=", "<", "<=", ">", ">=", "&&", "||"})
    expression("a " + op + " b",
               "(Binary \"" + op + "\" (Name \"a\") (Name \"b\"))");
  expression("+x", "(Unary \"+\" (Name \"x\"))");
}

TEST(Frontend, TokensAndComments) {
  auto tokens =
      lexer.parseExpression("/* outer /* inner */ end */ -12 // tail\n");
  ASSERT_TRUE(tokens.ok() && tokens.tokens.size() == 5)
      << "comments retained and skipped by parser";
  ASSERT_TRUE(tokens.tokens[0].kind == TokenKind::comment)
      << "block comment token";
  ASSERT_TRUE(tokens.source.substr(tokens.tokens[1].Loc.Offset, 1) == "-")
      << "minus separate from number";
  ASSERT_TRUE(tokens.tokens.back().kind == TokenKind::end) << "EOF token";
  ASSERT_TRUE(tokens.tokens.back().Loc.Offset == tokens.source.size())
      << "EOF location";
}

TEST(Frontend, TokenKinds) {
  auto result = lexer.parseExpression(
      "name 1 \"s\" let mut fn struct if else while return break continue "
      "true false meta when parallel extern defer module import pub -> = + - "
      "* / % == != < <= > >= && || ! ( ) { } [ ] , : ; . @");
  const std::vector<TokenKind> expected = {TokenKind::name,
                                           TokenKind::number,
                                           TokenKind::string,
                                           TokenKind::keyword_let,
                                           TokenKind::keyword_mut,
                                           TokenKind::keyword_fn,
                                           TokenKind::keyword_struct,
                                           TokenKind::keyword_if,
                                           TokenKind::keyword_else,
                                           TokenKind::keyword_while,
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
  EXPECT_EQ(lexer.parseExpression("include").tokens.front().kind,
            TokenKind::name);
}

TEST(Frontend, ModuleImportsAndVisibility) {
  const auto Source = kelyra::test::ReadSource("cli/modules/main.kly");
  auto Parsed = lexer.parse(Source);
  ASSERT_TRUE(Parsed.ok());
  EXPECT_EQ(lexer.dumpAst(*Parsed.root),
            "(Module (ModuleDecl \"app.main\") (Import \"math.vector\") "
            "(Function \"main\" (Public) (Type \"i32\") (Block (Return "
            "(Call (Member \"answer\" (Member \"vector\" (Name "
            "\"math\"))))))))");
}

TEST(Frontend, TokenLocations) {
  auto result = lexer.parseExpression("a\n  + b");
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
struct Pair { left: i32, right: i32[4], }
fn choose(x: i32, y: i32,) -> i32 {
  let mut z: i32 = x + y;
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
  auto program = lexer.parse(source);
  ASSERT_NE(program.root, nullptr);
  ASSERT_TRUE(program.ok()) << "complete module parses";
  ASSERT_EQ(program.root->children.size(), 3u) << "module declarations";
  ASSERT_TRUE(program.root->children[1]->kind == TokenKind::ast_function)
      << "function node";
  spans(*program.root, source.size());
  auto moved = std::move(program);
  ASSERT_TRUE(lexer.dumpAst(*moved.root).find("ArrayType \"4\"") !=
              std::string::npos)
      << "AST survives result move";
  ASSERT_TRUE(lexer.dumpAst(*moved.root).find("Annotation \"Vertex\"") !=
              std::string::npos)
      << "declaration annotation";
  ASSERT_TRUE(lexer.parse("").ok()) << "empty module";
  auto simple =
      lexer.parse("fn add(x: i32) -> i32 { let y = x + 1; return y; }");
  ASSERT_TRUE(
      simple.ok() &&
      lexer.dumpAst(*simple.root) ==
          "(Module (Function \"add\" (Parameter \"x\" (Type \"i32\")) (Type "
          "\"i32\") (Block (Let \"let\" (Name \"y\") (Binary \"+\" (Name "
          "\"x\") (Literal \"1\"))) (Return (Name \"y\")))))")
      << "full declaration AST";
}

TEST(Frontend, RejectInvalidModules) {
  for (const std::string invalid :
       {"let x = 1;", "fn f(x) {}", "fn f() { let x; }", "struct S { x i32 }",
        "fn f() { 1 = 2; }", "fn f() { a = b = c; }", "fn f() { return 1 }",
        "fn f() { if true return; }", "fn f() {", "fn f(x: i32[1.5]) {}",
        "fn f() { break 1; }", "fn f() { let fn = 1; }", "@ fn f() {}",
        "@Test let x = 1;", "module ; fn f() {}", "import ; fn f() {}"})
    ASSERT_FALSE(lexer.parse(invalid).ok())
        << "reject invalid module: " + invalid;
}

TEST(Frontend, ErrorRecovery) {
  auto recovery =
      lexer.parse("fn f() { let x = ; let y = ; return 7; } fn good() {}");
  ASSERT_EQ(recovery.diagnostics.size(), 2u)
      << "report independent statement errors";
  ASSERT_NE(recovery.root, nullptr);
  ASSERT_EQ(recovery.root->children.size(), 2u)
      << "recover subsequent function";
  ASSERT_FALSE(recovery.root->children[0]->children.empty());
  const auto &body = *recovery.root->children[0]->children.back();
  ASSERT_TRUE(body.children.size() == 1 &&
              body.children[0]->kind == TokenKind::ast_return)
      << "recover subsequent statement";
  auto missingBrace = lexer.parse("fn broken() { return; fn good() {}");
  ASSERT_FALSE(missingBrace.ok() && missingBrace.root->children.size() == 1 &&
               missingBrace.root->children[0]->text == "good")
      << "missing brace preserves next declaration";
  auto delimiter = lexer.parse("fn broken() { let x = } fn good() {}");
  ASSERT_TRUE(delimiter.diagnostics.size() == 1 &&
              delimiter.root->children.size() == 2)
      << "expression error preserves closing brace";
}

TEST(Frontend, DiagnosticLocations) {
  auto diagnostic = lexer.parse("fn f() {\n  let x = ;\n}", "test.kly");
  ASSERT_EQ(diagnostic.diagnostics.size(), 1u);
  EXPECT_EQ(diagnostic.diagnostics[0].Loc.Offset, 19u)
      << "exact diagnostic offset";
  EXPECT_EQ(diagnostic.diagnostics[0].Kind, DiagnosticKind::ExpectedExpression);
  EXPECT_EQ(GetDiagnosticInfo(DiagnosticKind::ExpectedExpression).Msg,
            "expected expression");
  std::ostringstream Output;
  Output << diagnostic.diagnostics[0];
  ASSERT_EQ(Output.str(), "test.kly:2:11: error: expected expression")
      << "GCC-compatible diagnostic";
}

TEST(Frontend, NestingLimits) {
  ASSERT_FALSE(
      lexer.parseExpression(std::string(200, '(') + "x" + std::string(200, ')'))
          .ok())
      << "group nesting limit";
  ASSERT_FALSE(lexer.parseExpression(std::string(200, '!') + "x").ok())
      << "unary nesting limit";
  std::string chain = "a";
  for (int i = 0; i < 200; ++i)
    chain += "+a";
  ASSERT_FALSE(lexer.parseExpression(chain).ok())
      << "left-associated AST depth limit";
  std::string blocks = "fn f() {";
  blocks += std::string(200, '{') + std::string(200, '}') + '}';
  ASSERT_FALSE(lexer.parse(blocks).ok()) << "block nesting limit";
}
