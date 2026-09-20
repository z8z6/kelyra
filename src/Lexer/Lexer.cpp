#include "Lexer/Lexer.h"

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <string_view>
#include <utility>

using namespace kelyra::lex;

using K = TokenKind;
constexpr std::size_t maxDepth = 128;

void Lexer::lex() {
  const auto &s = result.source;
  std::size_t r = 0;
  std::size_t line = 1;
  std::size_t column = 1;
  auto advance = [&](std::size_t begin, std::size_t end) {
    for (auto i = begin; i < end; ++i) {
      if (s[i] == '\n') {
        ++line;
        column = 1;
      } else if (s[i] == '\r' && (i + 1 == s.size() || s[i + 1] != '\n')) {
        ++line;
        column = 1;
      } else if (s[i] != '\r') {
        ++column;
      }
    }
  };
  while (r < s.size()) {
    const auto l = r;
    const auto tokenLine = line;
    const auto tokenColumn = column;
    auto error = [&](std::size_t start, DiagnosticKind Kind) {
      result.diagnostics.push_back({Kind,
                                    {result.File, start, tokenLine,
                                     tokenColumn + start - l, r - start}});
    };
    const char c = s[r++];
    if (skip(c)) {
      advance(l, r);
      continue;
    }
    TokenKind kind;
    if (alpha(c)) {
      while (r < s.size() && (alpha(s[r]) || digit(s[r])))
        ++r;
      kind = keyword(std::string_view(s).substr(l, r - l));
    } else if (digit(c)) {
      kind = TokenKind::number;
      while (r < s.size() && digit(s[r]))
        ++r;
      if (r + 1 < s.size() && s[r] == '.' && digit(s[r + 1])) {
        ++r;
        while (r < s.size() && digit(s[r]))
          ++r;
      }
      if (r < s.size() && (s[r] == 'e' || s[r] == 'E')) {
        ++r;
        if (r < s.size() && (s[r] == '+' || s[r] == '-'))
          ++r;
        const auto exponent = r;
        while (r < s.size() && digit(s[r]))
          ++r;
        if (r == exponent)
          error(l, DiagnosticKind::ExpectedExponentDigits);
      }
      if (r < s.size() && alpha(s[r])) {
        while (r < s.size() && (alpha(s[r]) || digit(s[r])))
          ++r;
        error(l, DiagnosticKind::InvalidNumericSuffix);
      }
    } else if (c == '"') {
      kind = TokenKind::string;
      bool closed = false;
      while (r < s.size()) {
        const char ch = s[r++];
        if (ch == '"') {
          closed = true;
          break;
        }
        if (ch == '\n' || ch == '\r')
          break;
        if (static_cast<unsigned char>(ch) < 32)
          error(r - 1, DiagnosticKind::ControlCharacterInString);
        if (ch == '\\') {
          if (r == s.size())
            break;
          const auto escape = r - 1;
          const char escaped = s[r++];
          if (std::string_view("\\\"nrt0").find(escaped) ==
              std::string_view::npos)
            error(escape, DiagnosticKind::UnsupportedStringEscape);
        }
      }
      if (!closed)
        error(l, DiagnosticKind::UnterminatedStringLiteral);
    } else if (c == '/' && r < s.size() && s[r] == '/') {
      kind = TokenKind::comment;
      while (r < s.size() && s[r] != '\n')
        ++r;
    } else if (c == '/' && r < s.size() && s[r] == '*') {
      kind = TokenKind::comment;
      ++r;
      std::size_t nesting = 1;
      while (r < s.size() && nesting) {
        if (r + 1 < s.size() && s[r] == '/' && s[r + 1] == '*') {
          ++nesting;
          r += 2;
        } else if (r + 1 < s.size() && s[r] == '*' && s[r + 1] == '/') {
          --nesting;
          r += 2;
        } else
          ++r;
      }
      if (nesting)
        error(l, DiagnosticKind::UnterminatedBlockComment);
    } else {
      if (c == '{' && !result.tokens.empty() &&
          result.tokens.back().kind == TokenKind::keyword_asm) {
        result.tokens.push_back({TokenKind::punc_left_brace,
                                 {result.File, l, tokenLine, tokenColumn, 1}});
        advance(l, r);
        const auto BodyStart = r;
        const auto BodyLine = line;
        const auto BodyColumn = column;
        std::size_t Nesting = 1;
        while (r < s.size() && Nesting != 0) {
          if (s[r] == '{')
            ++Nesting;
          else if (s[r] == '}')
            --Nesting;
          if (Nesting != 0)
            ++r;
        }
        result.tokens.push_back(
            {TokenKind::asm_text,
             {result.File, BodyStart, BodyLine, BodyColumn, r - BodyStart}});
        advance(BodyStart, r);
        if (r == s.size()) {
          result.diagnostics.push_back({DiagnosticKind::ExpectedRightBrace,
                                        {result.File, r, line, column, 0}});
          continue;
        }
        result.tokens.push_back(
            {TokenKind::punc_right_brace, {result.File, r, line, column, 1}});
        advance(r, r + 1);
        ++r;
        continue;
      }
      const auto pair = std::string_view(s).substr(l, 2);
      if (pair == "->" || pair == "==" || pair == "!=" || pair == "<=" ||
          pair == ">=" || pair == "&&" || pair == "||")
        ++r;
      else if (std::string_view("(){}[],:;.@+-*/%!=<>&").find(c) ==
               std::string_view::npos) {
        error(l, DiagnosticKind::UnexpectedCharacter);
        advance(l, r);
        continue;
      }
      kind = punctuation(std::string_view(s).substr(l, r - l));
    }
    result.tokens.push_back(
        {kind, {result.File, l, tokenLine, tokenColumn, r - l}});
    advance(l, r);
  }
  result.tokens.push_back({TokenKind::end, {result.File, r, line, column, 0}});
}

struct ParseError {};
const Token &Lexer::peek(std::size_t Offset) const {
  auto Index = pos;
  while (Offset != 0 && Index + 1 < result.tokens.size()) {
    ++Index;
    while (Index + 1 < result.tokens.size() &&
           result.tokens[Index].kind == TokenKind::comment)
      ++Index;
    --Offset;
  }
  return result.tokens[Index];
}

std::string_view Lexer::spelling(const Token &token) const {
  return std::string_view(result.source)
      .substr(token.Loc.Offset, token.Loc.Len);
}

bool Lexer::at(std::string_view text) const { return spelling(peek()) == text; }

bool Lexer::end() const { return peek().kind == TokenKind::end; }

void Lexer::skipComments() {
  while (peek().kind == TokenKind::comment)
    ++pos;
}

Token Lexer::take() {
  Token token = peek();
  if (!end()) {
    ++pos;
    skipComments();
  }
  return token;
}

bool Lexer::eat(std::string_view text) {
  if (!at(text))
    return false;
  take();
  return true;
}

[[noreturn]] void Lexer::fail(Location Loc, DiagnosticKind Kind) {
  result.diagnostics.push_back({Kind, Loc});
  throw ParseError{};
}

Token Lexer::expect(std::string_view text) {
  if (!at(text)) {
    DiagnosticKind Kind;
    if (text == "]")
      Kind = DiagnosticKind::ExpectedRightBracket;
    else if (text == ")")
      Kind = DiagnosticKind::ExpectedRightParen;
    else if (text == "}")
      Kind = DiagnosticKind::ExpectedRightBrace;
    else if (text == ";")
      Kind = DiagnosticKind::ExpectedSemicolon;
    else if (text == "{")
      Kind = DiagnosticKind::ExpectedLeftBrace;
    else if (text == ":")
      Kind = DiagnosticKind::ExpectedColon;
    else
      Kind = DiagnosticKind::ExpectedLeftParen;
    fail(peek().Loc, Kind);
  }
  return take();
}

Token Lexer::name() {
  if (peek().kind != TokenKind::name)
    fail(peek().Loc, DiagnosticKind::ExpectedIdentifier);
  return take();
}

Lexer::Ptr Lexer::qualified(TokenKind Kind, bool AllowWildcard) {
  const auto first = name();
  auto result = node(Kind, first.Loc, std::string(spelling(first)));
  while (eat(".")) {
    if (AllowWildcard && at("*")) {
      const auto Part = take();
      result->text += ".*";
      result->Loc.Len = Part.Loc.End() - result->Loc.Offset;
      break;
    }
    const auto part = name();
    result->text += ".";
    result->text += spelling(part);
    result->Loc.Len = part.Loc.End() - result->Loc.Offset;
  }
  return result;
}

Lexer::Guard::Guard(Lexer &lexer) : lexer(lexer) {
  if (lexer.depth >= maxDepth)
    lexer.fail(lexer.peek().Loc, DiagnosticKind::SyntaxNestingLimitExceeded);
  ++lexer.depth;
}

Lexer::Guard::~Guard() { --lexer.depth; }

Lexer::Ptr Lexer::node(K kind, Location Loc, std::string text) {
  return std::make_unique<Node>(Node{kind, Loc, std::move(text), {}, 1});
}

void Lexer::add(Node &parent, Ptr child) {
  const auto height = std::max(parent.height, child->height + 1);
  if (height > maxDepth)
    fail(child->Loc, DiagnosticKind::AstNestingLimitExceeded);
  parent.height = height;
  parent.Loc.Len = child->Loc.End() - parent.Loc.Offset;
  parent.children.push_back(std::move(child));
}

Lexer::Ptr Lexer::type() {
  Guard guard(*this);
  if (at("*")) {
    const auto Loc = take().Loc;
    auto pointer = node(K::ast_pointer_type, Loc);
    add(*pointer, type());
    return pointer;
  }
  Ptr result;
  if (peek().kind == K::keyword_meta) {
    const auto Meta = take();
    result = node(K::ast_type, Meta.Loc, "meta");
    while (eat(".")) {
      const auto Part = name();
      result->text += ".";
      result->text += spelling(Part);
      result->Loc.Len = Part.Loc.End() - result->Loc.Offset;
    }
  } else {
    result = qualified(K::ast_type);
  }
  while (at("[")) {
    take();
    auto array = node(K::ast_array_type, result->Loc);
    if (peek().kind != TokenKind::number ||
        spelling(peek()).find_first_not_of("0123456789") !=
            std::string_view::npos)
      fail(peek().Loc, DiagnosticKind::ExpectedIntegerArrayLength);
    array->text = spelling(take());
    const auto end = expect("]").Loc.End();
    add(*array, std::move(result));
    array->Loc.Len = end - array->Loc.Offset;
    result = std::move(array);
  }
  return result;
}

Lexer::Ptr Lexer::annotation() {
  const auto Start = expect("@").Loc;
  auto Result = qualified(K::ast_annotation);
  const auto NameEnd = Result->Loc.End();
  Result->Loc.Offset = Start.Offset;
  Result->Loc.Line = Start.Line;
  Result->Loc.Column = Start.Column;
  if (!eat("(")) {
    Result->Loc.Len = NameEnd - Start.Offset;
    return Result;
  }
  if (!at(")")) {
    do {
      auto Argument = node(K::ast_annotation_argument, peek().Loc);
      if (peek().kind == K::name && spelling(peek(1)) == "=") {
        const auto Name = take();
        Argument->text = spelling(Name);
        take();
      }
      if (peek().kind == K::keyword_annotation ||
          peek().kind == K::keyword_struct ||
          peek().kind == K::keyword_module) {
        const auto Value = take();
        add(*Argument,
            node(K::ast_name, Value.Loc, std::string(spelling(Value))));
      } else {
        add(*Argument, expr());
      }
      add(*Result, std::move(Argument));
    } while (eat(",") && !at(")"));
  }
  Result->Loc.Len = expect(")").Loc.End() - Start.Offset;
  return Result;
}

int Lexer::binding(std::string_view op) {
  if (op == "||")
    return 10;
  if (op == "&&")
    return 20;
  if (op == "==" || op == "!=")
    return 30;
  if (op == "<" || op == "<=" || op == ">" || op == ">=")
    return 40;
  if (op == "+" || op == "-")
    return 50;
  if (op == "*" || op == "/" || op == "%")
    return 60;
  if (op == "(" || op == "[" || op == ".")
    return 80;
  return -1;
}

bool Lexer::comparison(const Node &node) {
  const int bp = binding(node.text);
  return node.kind == K::ast_binary && (bp == 30 || bp == 40);
}

Lexer::Ptr Lexer::expr(int minBp) {
  Guard guard(*this);
  const auto token = peek();
  const auto text = spelling(token);
  Ptr lhs;
  if (token.kind == TokenKind::name) {
    take();
    lhs = node(K::ast_name, token.Loc, std::string(text));
  } else if (token.kind == TokenKind::number ||
             token.kind == TokenKind::string ||
             token.kind == TokenKind::keyword_true ||
             token.kind == TokenKind::keyword_false) {
    take();
    lhs = node(K::ast_literal, token.Loc, std::string(text));
  } else if (token.kind == TokenKind::keyword_meta) {
    take();
    lhs = node(K::ast_meta, token.Loc);
    expect("(");
    add(*lhs, type());
    lhs->Loc.Len = expect(")").Loc.End() - lhs->Loc.Offset;
  } else if (text == "-" || text == "!" || text == "+" || text == "*" ||
             text == "&") {
    take();
    lhs = node(K::ast_unary, token.Loc, std::string(text));
    add(*lhs, expr(70));
  } else if (text == "(") {
    take();
    lhs = node(K::ast_group, token.Loc);
    add(*lhs, expr());
    lhs->Loc.Len = expect(")").Loc.End() - lhs->Loc.Offset;
  } else {
    fail(token.Loc, DiagnosticKind::ExpectedExpression);
  }

  while (true) {
    const auto op = spelling(peek());
    const int bp = binding(op);
    if (bp < minBp)
      break;
    const auto operatorToken = take();
    if (bp == 80) {
      auto expression = node(op == "("   ? K::ast_call
                             : op == "[" ? K::ast_index
                                         : K::ast_member,
                             lhs->Loc);
      add(*expression, std::move(lhs));
      if (op == "(") {
        if (!at(")")) {
          do {
            add(*expression, expr());
          } while (eat(",") && !at(")"));
        }
        expression->Loc.Len = expect(")").Loc.End() - expression->Loc.Offset;
      } else if (op == "[") {
        add(*expression, expr());
        expression->Loc.Len = expect("]").Loc.End() - expression->Loc.Offset;
      } else {
        const auto member = name();
        expression->text = spelling(member);
        expression->Loc.Len = member.Loc.End() - expression->Loc.Offset;
      }
      lhs = std::move(expression);
    } else {
      auto rhs = expr(bp + 1);
      if ((bp == 30 || bp == 40) && (comparison(*lhs) || comparison(*rhs)))
        fail(operatorToken.Loc,
             DiagnosticKind::ComparisonChainsRequireParentheses);
      auto expression = node(K::ast_binary, lhs->Loc, std::string(op));
      add(*expression, std::move(lhs));
      add(*expression, std::move(rhs));
      lhs = std::move(expression);
    }
  }
  return lhs;
}

void Lexer::recover(bool top, std::size_t start) {
  if (pos == start && !end() && !(at("}") && !top))
    take();
  while (!end()) {
    if (at("fn") || at("struct") || at("annotation") ||
        (top && (at("@") || at("pub"))))
      return;
    if (!top &&
        (at("}") || at("let") || at("return") || at("if") || at("when") ||
         at("while") || at("asm") || at("break") || at("continue")))
      return;
    if (eat(";"))
      return;
    take();
  }
}

Lexer::Ptr Lexer::block() {
  Guard guard(*this);
  auto result = node(K::ast_block, expect("{").Loc);
  while (!at("}") && !end()) {
    // Preserve a subsequent top-level declaration when this block lacks '}'.
    if (at("fn") || at("struct") || at("annotation") || at("@"))
      fail(peek().Loc, DiagnosticKind::ExpectedRightBraceBeforeDeclaration);
    const auto start = pos;
    try {
      add(*result, stmt());
    } catch (const ParseError &) {
      recover(false, start);
    }
  }
  result->Loc.Len = expect("}").Loc.End() - result->Loc.Offset;
  return result;
}

Lexer::Ptr Lexer::assembly() {
  const auto Start = take().Loc;
  auto Result = node(K::ast_asm, Start);
  expect("{");
  if (peek().kind != K::asm_text)
    fail(peek().Loc, DiagnosticKind::ExpectedExpression);
  Result->text = spelling(take());
  expect("}");

  while (eat(".")) {
    const auto Method = name();
    const auto MethodName = spelling(Method);
    expect("(");
    if (MethodName == "in" || MethodName == "out") {
      const auto Value = name();
      auto Binding =
          node(MethodName == "in" ? K::ast_asm_input : K::ast_asm_output,
               Method.Loc, std::string(spelling(Value)));
      if (eat(",")) {
        const auto Register = name();
        add(*Binding,
            node(K::ast_name, Register.Loc, std::string(spelling(Register))));
      }
      Binding->Loc.Len = expect(")").Loc.End() - Binding->Loc.Offset;
      add(*Result, std::move(Binding));
      continue;
    }
    if (MethodName != "op")
      fail(Method.Loc, DiagnosticKind::InvalidInlineAssembly);
    do {
      const auto OptionName = name();
      auto Option = node(K::ast_asm_option, OptionName.Loc,
                         std::string(spelling(OptionName)));
      if (Option->text == "clobber" && eat("(")) {
        if (!at(")")) {
          do {
            const auto Register = name();
            add(*Option, node(K::ast_name, Register.Loc,
                              std::string(spelling(Register))));
          } while (eat(","));
        }
        expect(")");
      }
      add(*Result, std::move(Option));
    } while (eat(","));
    expect(")");
  }
  Result->Loc.Len = expect(";").Loc.End() - Result->Loc.Offset;
  return Result;
}

Lexer::Ptr Lexer::stmt() {
  Guard guard(*this);
  if (at("{"))
    return block();
  if (at("asm"))
    return assembly();
  if (at("let")) {
    auto result = node(K::ast_let, take().Loc, "let");
    const auto id = name();
    add(*result, node(K::ast_name, id.Loc, std::string(spelling(id))));
    const bool annotated = eat(":");
    if (annotated)
      add(*result, type());
    if (eat("="))
      add(*result, expr());
    else if (!annotated)
      fail(peek().Loc, DiagnosticKind::ExpectedTypeAnnotationOrInitializer);
    result->Loc.Len = expect(";").Loc.End() - result->Loc.Offset;
    return result;
  }
  if (at("return") || at("break") || at("continue")) {
    const auto t = take();
    auto result = node(spelling(t) == "return"  ? K::ast_return
                       : spelling(t) == "break" ? K::ast_break
                                                : K::ast_continue,
                       t.Loc);
    if (result->kind == K::ast_return && !at(";"))
      add(*result, expr());
    result->Loc.Len = expect(";").Loc.End() - result->Loc.Offset;
    return result;
  }
  if (at("if") || at("when") || at("while")) {
    const auto t = take();
    const auto Kind = spelling(t) == "if"     ? K::ast_if
                      : spelling(t) == "when" ? K::ast_when
                                              : K::ast_while;
    auto result = node(Kind, t.Loc);
    add(*result, expr());
    add(*result, block());
    if ((result->kind == K::ast_if || result->kind == K::ast_when) &&
        eat("else"))
      add(*result,
          at(result->kind == K::ast_if ? "if" : "when") ? stmt() : block());
    return result;
  }
  auto lhs = expr();
  auto result = node(K::ast_expr_stmt, lhs->Loc);
  if (eat("=")) {
    if (lhs->kind != K::ast_name && lhs->kind != K::ast_member &&
        lhs->kind != K::ast_index &&
        !(lhs->kind == K::ast_unary && lhs->text == "*"))
      fail(lhs->Loc, DiagnosticKind::InvalidAssignmentTarget);
    result->kind = K::ast_assign;
    add(*result, std::move(lhs));
    add(*result, expr());
  } else
    add(*result, std::move(lhs));
  result->Loc.Len = expect(";").Loc.End() - result->Loc.Offset;
  return result;
}

Lexer::Ptr Lexer::decl() {
  std::vector<Ptr> annotations;
  while (at("@"))
    annotations.push_back(annotation());
  Token visibility{};
  const bool isPublic = at("pub");
  if (isPublic)
    visibility = take();
  if (!at("fn") && !at("struct") && !at("annotation"))
    fail(peek().Loc, DiagnosticKind::ExpectedDeclaration);
  const auto t = take();
  auto Loc = t.Loc;
  if (!annotations.empty())
    Loc = annotations.front()->Loc;
  else if (isPublic)
    Loc = visibility.Loc;
  auto result = node(spelling(t) == "fn"       ? K::ast_function
                     : spelling(t) == "struct" ? K::ast_struct
                                               : K::ast_annotation_decl,
                     Loc);
  result->text = spelling(name());
  for (auto &annotation : annotations)
    add(*result, std::move(annotation));
  if (isPublic)
    add(*result, node(K::ast_public, visibility.Loc));
  if (result->kind == K::ast_annotation_decl) {
    expect("(");
    if (!at(")")) {
      do {
        const auto Id = name();
        auto Parameter = node(K::ast_annotation_parameter, Id.Loc,
                              std::string(spelling(Id)));
        expect(":");
        add(*Parameter, type());
        if (eat("="))
          add(*Parameter, expr());
        add(*result, std::move(Parameter));
      } while (eat(",") && !at(")"));
    }
    expect(")");
    result->Loc.Len = expect(";").Loc.End() - result->Loc.Offset;
  } else if (result->kind == K::ast_struct) {
    expect("{");
    while (!at("}") && !end()) {
      const auto id = name();
      auto field = node(K::ast_field, id.Loc, std::string(spelling(id)));
      expect(":");
      add(*field, type());
      add(*result, std::move(field));
      if (!eat(","))
        break;
    }
    result->Loc.Len = expect("}").Loc.End() - result->Loc.Offset;
  } else {
    expect("(");
    if (!at(")")) {
      do {
        const auto id = name();
        auto parameter =
            node(K::ast_parameter, id.Loc, std::string(spelling(id)));
        expect(":");
        add(*parameter, type());
        add(*result, std::move(parameter));
      } while (eat(",") && !at(")"));
    }
    expect(")");
    if (eat("->"))
      add(*result, type());
    add(*result, block());
  }
  return result;
}

void Lexer::run() {
  result.root =
      node(K::ast_module, {result.File, 0, 1, 1, result.source.size()});
  if (at("module")) {
    const auto start = pos;
    try {
      take();
      auto declaration = qualified(K::ast_module_decl);
      declaration->Loc.Len = expect(";").Loc.End() - declaration->Loc.Offset;
      add(*result.root, std::move(declaration));
    } catch (const ParseError &) {
      recover(true, start);
    }
  }
  while (at("import")) {
    const auto start = pos;
    try {
      take();
      auto Import = qualified(K::ast_import, true);
      if (Import->text == "c" && peek().kind == K::string) {
        const auto Header = take();
        const auto Text = spelling(Header);
        add(*Import, node(K::ast_literal, Header.Loc,
                          std::string(Text.substr(1, Text.size() - 2))));
      }
      Import->Loc.Len = expect(";").Loc.End() - Import->Loc.Offset;
      add(*result.root, std::move(Import));
    } catch (const ParseError &) {
      recover(true, start);
    }
  }
  while (!end()) {
    const auto start = pos;
    try {
      add(*result.root, decl());
    } catch (const ParseError &) {
      recover(true, start);
    }
  }
  result.root->Loc.Len = result.source.size();
}

void Lexer::runExpression() {
  try {
    result.root = expr();
    if (!end())
      fail(peek().Loc, DiagnosticKind::ExpectedEndOfExpression);
  } catch (const ParseError &) {
  }
}

void dump(const Node &node, std::ostream &os) {
  os << '(' << node.kind;
  if (!node.text.empty())
    os << ' ' << std::quoted(node.text);
  for (const auto &child : node.children) {
    os << ' ';
    dump(*child, os);
  }
  os << ')';
}

ParseResult Lexer::parse(std::string source, std::string_view File) {
  result = {};
  pos = 0;
  depth = 0;
  result.File = File;
  result.source = std::move(source);
  lex();
  if (result.ok()) {
    skipComments();
    run();
  }
  return std::move(result);
}

ParseResult Lexer::parseExpression(std::string source, std::string_view File) {
  result = {};
  pos = 0;
  depth = 0;
  result.File = File;
  result.source = std::move(source);
  lex();
  if (result.ok()) {
    skipComments();
    runExpression();
  }
  return std::move(result);
}

std::string Lexer::dumpAst(const Node &node) {
  std::ostringstream os;
  dump(node, os);
  return os.str();
}

bool Lexer::skip(char c) {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}
bool Lexer::digit(char c) { return c >= '0' && c <= '9'; }
bool Lexer::alpha(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
TokenKind Lexer::keyword(std::string_view s) {
  if (s == "let")
    return K::keyword_let;
  if (s == "fn")
    return K::keyword_fn;
  if (s == "struct")
    return K::keyword_struct;
  if (s == "annotation")
    return K::keyword_annotation;
  if (s == "if")
    return K::keyword_if;
  if (s == "else")
    return K::keyword_else;
  if (s == "while")
    return K::keyword_while;
  if (s == "return")
    return K::keyword_return;
  if (s == "break")
    return K::keyword_break;
  if (s == "continue")
    return K::keyword_continue;
  if (s == "true")
    return K::keyword_true;
  if (s == "false")
    return K::keyword_false;
  if (s == "meta")
    return K::keyword_meta;
  if (s == "when")
    return K::keyword_when;
  if (s == "parallel")
    return K::keyword_parallel;
  if (s == "extern")
    return K::keyword_extern;
  if (s == "defer")
    return K::keyword_defer;
  if (s == "module")
    return K::keyword_module;
  if (s == "import")
    return K::keyword_import;
  if (s == "pub")
    return K::keyword_pub;
  if (s == "asm")
    return K::keyword_asm;
  return K::name;
}

TokenKind Lexer::punctuation(std::string_view s) {
  if (s == "->")
    return K::op_arrow;
  if (s == "=")
    return K::op_assign;
  if (s == "+")
    return K::op_add;
  if (s == "-")
    return K::op_subtract;
  if (s == "*")
    return K::op_multiply;
  if (s == "/")
    return K::op_divide;
  if (s == "%")
    return K::op_remainder;
  if (s == "==")
    return K::op_equal;
  if (s == "!=")
    return K::op_not_equal;
  if (s == "<")
    return K::op_less;
  if (s == "<=")
    return K::op_less_equal;
  if (s == ">")
    return K::op_greater;
  if (s == ">=")
    return K::op_greater_equal;
  if (s == "&&")
    return K::op_and;
  if (s == "||")
    return K::op_or;
  if (s == "!")
    return K::op_not;
  if (s == "&")
    return K::op_address;
  if (s == "(")
    return K::punc_left_paren;
  if (s == ")")
    return K::punc_right_paren;
  if (s == "{")
    return K::punc_left_brace;
  if (s == "}")
    return K::punc_right_brace;
  if (s == "[")
    return K::punc_left_bracket;
  if (s == "]")
    return K::punc_right_bracket;
  if (s == ",")
    return K::punc_comma;
  if (s == ":")
    return K::punc_colon;
  if (s == ";")
    return K::punc_semicolon;
  if (s == ".")
    return K::punc_dot;
  if (s == "@")
    return K::punc_at;
  return K::illegal;
}
