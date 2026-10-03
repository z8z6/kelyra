#include "Front/Parser/Parser.h"
#include <algorithm>
#include <utility>

using namespace kelyra::lex;
using K = NodeKind;
constexpr std::size_t MaxDepth = 128;

struct ParseError {};
const Token &Parser::peek(std::size_t Offset) const {
  auto Index = Position;
  while (Offset != 0 && Index + 1 < Parsed.tokens.size()) {
    ++Index;
    while (Index + 1 < Parsed.tokens.size() && Parsed.tokens[Index].kind == TokenKind::comment)
      ++Index;
    --Offset;
  }
  return Parsed.tokens[Index];
}

std::string_view Parser::spelling(const Token &token) const {
  return std::string_view(Parsed.source).substr(token.Loc.Begin, token.Loc.Length());
}

bool Parser::at(std::string_view text) const { return spelling(peek()) == text; }

bool Parser::end() const { return peek().kind == TokenKind::end; }

void Parser::skipComments() {
  while (peek().kind == TokenKind::comment)
    ++Position;
}

Token Parser::take() {
  Token token = peek();
  if (!end()) {
    ++Position;
    skipComments();
  }
  return token;
}

bool Parser::eat(std::string_view text) {
  if (!at(text))
    return false;
  take();
  return true;
}

[[noreturn]] void Parser::fail(Location Loc, DiagnosticKind Kind) {
  Parsed.diagnostics.push_back({Kind, Loc});
  throw ParseError{};
}

Token Parser::expect(std::string_view text) {
  if (!at(text)) {
    DiagnosticKind Kind;
    if (text == "]")
      Kind = DiagnosticKind::ExpectedRightBracket;
    else if (text == ">")
      Kind = DiagnosticKind::ExpectedRightAngle;
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

Token Parser::name() {
  if (peek().kind != TokenKind::name)
    fail(peek().Loc, DiagnosticKind::ExpectedIdentifier);
  return take();
}

Parser::Ptr Parser::qualified(NodeKind Kind) {
  const auto first = Kind == NodeKind::ast_annotation && at("meta") ? take() : name();
  auto result = node(Kind, first.Loc, std::string(spelling(first)));
  while (eat(".")) {
    const auto part =
        at("annotation") || at("meta") || (Kind == NodeKind::ast_annotation && at("extern"))
            ? take()
            : name();
    result->text += ".";
    result->text += spelling(part);
    result->Loc.End = part.Loc.End;
  }
  return result;
}

Parser::Guard::Guard(Parser &Owner) : Owner(Owner) {
  if (Owner.Depth >= MaxDepth)
    Owner.fail(Owner.peek().Loc, DiagnosticKind::SyntaxNestingLimitExceeded);
  ++Owner.Depth;
}

Parser::Guard::~Guard() { --Owner.Depth; }

Parser::Ptr Parser::node(K kind, Location Loc, std::string text) {
  return std::make_unique<Node>(Node{kind, Loc, std::move(text), {}, 1});
}

void Parser::add(Node &parent, Ptr child) {
  const auto height = std::max(parent.height, child->height + 1);
  if (height > MaxDepth)
    fail(child->Loc, DiagnosticKind::AstNestingLimitExceeded);
  parent.height = height;
  parent.Loc.End = std::max(parent.Loc.End, child->Loc.End);
  parent.children.push_back(std::move(child));
}

Parser::Ptr Parser::type() {
  Guard guard(*this);
  if (at("[")) {
    const auto Loc = take().Loc;
    if (eat("]")) {
      auto Slice = node(K::ast_slice_type, Loc);
      Slice->text = eat("const") ? "const" : "";
      add(*Slice, type());
      return Slice;
    }
    auto Array = node(K::ast_array_type, Loc);
    const auto LengthStart = peek().Loc.Begin;
    auto Length = expr();
    Array->text = Parsed.source.substr(LengthStart, Length->Loc.End - LengthStart);
    add(*Array, std::move(Length));
    expect("]");
    add(*Array, type());
    return Array;
  }
  if (at("fn")) {
    auto result = node(K::ast_function_type, take().Loc);
    expect("(");
    if (!at(")")) {
      do {
        add(*result, type());
      } while (eat(",") && !at(")"));
    }
    const auto end = expect(")").Loc;
    if (eat("->"))
      add(*result, type());
    else
      add(*result, node(K::ast_type, end, "void"));
    return result;
  }
  if (at("*")) {
    const auto Loc = take().Loc;
    auto pointer = node(K::ast_pointer_type, Loc);
    add(*pointer, type());
    return pointer;
  }
  Ptr result;
  if (at("(")) {
    result = node(K::ast_result_types, take().Loc);
    add(*result, type());
    expect(",");
    add(*result, type());
    while (eat(",") && !at(")"))
      add(*result, type());
    result->Loc.End = expect(")").Loc.End;
    return result;
  } else if (peek().kind == lex::TokenKind::keyword_meta) {
    const auto Meta = take();
    result = node(K::ast_type, Meta.Loc, "meta");
    while (eat(".")) {
      const auto Part = name();
      result->text += ".";
      result->text += spelling(Part);
      result->Loc.End = Part.Loc.End;
    }
  } else {
    result = qualified(K::ast_type);
  }
  if (result->kind == K::ast_type && eat("<")) {
    auto Generic = node(K::ast_generic_type, result->Loc, result->text);
    do {
      add(*Generic, type());
    } while (eat(","));
    Generic->Loc.End = expect(">").Loc.End;
    result = std::move(Generic);
  }
  while (result->kind == K::ast_generic_type && eat(".")) {
    const auto Part = name();
    result->text += ".";
    result->text += spelling(Part);
    result->Loc.End = Part.Loc.End;
    if (eat("<")) {
      if (result->AssociatedOwnerArguments)
        fail(Part.Loc, DiagnosticKind::ExpectedDeclaration);
      result->AssociatedOwnerArguments = result->children.size();
      do {
        add(*result, type());
      } while (eat(","));
      result->Loc.End = expect(">").Loc.End;
    }
  }
  return result;
}

Parser::Ptr Parser::annotation() {
  const auto Start = expect("@").Loc;
  auto Result = peek().kind == lex::TokenKind::keyword_extern
                    ? node(K::ast_annotation, take().Loc, "extern")
                    : qualified(K::ast_annotation);
  const auto NameEnd = Result->Loc.End;
  Result->Loc.Begin = Start.Begin;
  Result->Loc.Line = Start.Line;
  Result->Loc.Column = Start.Column;
  if (!eat("(")) {
    Result->Loc.End = NameEnd;
    return Result;
  }
  if (!at(")")) {
    do {
      auto Argument = node(K::ast_annotation_argument, peek().Loc);
      if (peek().kind == lex::TokenKind::name && spelling(peek(1)) == "=") {
        const auto Name = take();
        Argument->text = spelling(Name);
        take();
      }
      auto Value = expr();
      if (peek().kind == TokenKind::name) {
        const auto Bound = take();
        auto Binding = node(K::ast_annotation_binding, Value->Loc, std::string(spelling(Bound)));
        add(*Binding, std::move(Value));
        Binding->Loc.End = Bound.Loc.End;
        Value = std::move(Binding);
      }
      add(*Argument, std::move(Value));
      add(*Result, std::move(Argument));
    } while (eat(",") && !at(")"));
  }
  Result->Loc.End = expect(")").Loc.End;
  return Result;
}

std::vector<Parser::Ptr> Parser::annotations() {
  std::vector<Ptr> Result;
  while (at("@"))
    Result.push_back(annotation());
  return Result;
}

void Parser::genericParameters(Node &Target, bool AllowPack) {
  if (!eat("<"))
    return;
  do {
    const auto Parameter = name();
    const bool Pack = AllowPack && eat("...");
    add(Target,
        node(Pack ? K::ast_generic_pack : K::ast_generic_parameter,
             Parameter.Loc,
             std::string(spelling(Parameter))));
    if (Pack && !at(">"))
      fail(peek().Loc, DiagnosticKind::ExpectedRightAngle);
  } while (eat(","));
  Target.Loc.End = expect(">").Loc.End;
}

void Parser::parameters(Node &Target) {
  expect("(");
  if (!at(")")) {
    do {
      auto Annotations = annotations();
      const auto Id = name();
      auto Parameter = node(K::ast_parameter,
                            Annotations.empty() ? Id.Loc : Annotations.front()->Loc,
                            std::string(spelling(Id)));
      for (auto &Annotation : Annotations)
        add(*Parameter, std::move(Annotation));
      expect(":");
      if (eat("..."))
        Parameter->kind = K::ast_parameter_pack;
      add(*Parameter, type());
      add(Target, std::move(Parameter));
    } while (eat(",") && !at(")"));
  }
  Target.Loc.End = expect(")").Loc.End;
}

int Parser::binding(std::string_view Op) {
  return GetBindingPower(Op == "as" ? TokenKind::keyword_as : PunctuationToken(Op));
}

bool Parser::comparison(const Node &node) {
  const int bp = binding(node.text);
  return node.kind == K::ast_binary && (bp == 30 || bp == 40);
}

Parser::Ptr Parser::expr(int minBp) {
  Guard guard(*this);
  const auto token = peek();
  const auto text = spelling(token);
  Ptr lhs;
  if (token.kind == TokenKind::name || token.kind == TokenKind::keyword_this) {
    take();
    lhs = node(K::ast_name, token.Loc, std::string(text));
  } else if (token.kind == TokenKind::number || token.kind == TokenKind::string ||
             token.kind == TokenKind::keyword_true || token.kind == TokenKind::keyword_false) {
    take();
    lhs = node(K::ast_literal, token.Loc, std::string(text));
  } else if (token.kind == TokenKind::keyword_meta) {
    take();
    if (at("{")) {
      lhs = blockExpression();
      lhs->kind = K::ast_meta_block;
      const auto End = lhs->Loc.End;
      lhs->Loc.Begin = token.Loc.Begin;
      lhs->Loc.End = End;
    } else {
      lhs = node(K::ast_meta, token.Loc);
      expect("(");
      add(*lhs, type());
      lhs->Loc.End = expect(")").Loc.End;
    }
  } else if (token.kind == TokenKind::keyword_match) {
    take();
    lhs = node(K::ast_match, token.Loc);
    add(*lhs, expr());
    expect("{");
    while (!at("}") && !end()) {
      auto Arm = node(K::ast_match_arm, peek().Loc);
      add(*Arm, expr());
      expect("=>");
      add(*Arm, expr());
      add(*lhs, std::move(Arm));
      if (!eat(","))
        break;
    }
    lhs->Loc.End = expect("}").Loc.End;
  } else if (text == "{") {
    lhs = blockExpression();
  } else if (text == "...") {
    take();
    lhs = node(K::ast_spread, token.Loc);
    add(*lhs, expr(70));
  } else if (text == "-" || text == "!" || text == "+" || text == "*" || text == "&") {
    take();
    lhs = node(K::ast_unary, token.Loc, std::string(text));
    add(*lhs, expr(70));
  } else if (text == "(") {
    take();
    lhs = node(K::ast_group, token.Loc);
    add(*lhs, expr());
    lhs->Loc.End = expect(")").Loc.End;
  } else {
    fail(token.Loc, DiagnosticKind::ExpectedExpression);
  }

  while (true) {
    const auto op = spelling(peek());
    if (op == "<" && minBp <= 80 && (lhs->kind == K::ast_name || lhs->kind == K::ast_member)) {
      const auto SavedPos = Position;
      const auto SavedDiagnostics = Parsed.diagnostics.size();
      try {
        take();
        std::vector<Ptr> Arguments;
        if (!at(">")) {
          do {
            Arguments.push_back(type());
          } while (eat(","));
        }
        const auto End = expect(">").Loc;
        if (at("(") || at(".")) {
          auto Applied = node(K::ast_generic_apply, lhs->Loc);
          add(*Applied, std::move(lhs));
          for (auto &Argument : Arguments)
            add(*Applied, std::move(Argument));
          Applied->Loc.End = End.End;
          lhs = std::move(Applied);
          continue;
        }
      } catch (const ParseError &) {
      }
      Position = SavedPos;
      Parsed.diagnostics.resize(SavedDiagnostics);
    }
    const int bp = GetBindingPower(peek().kind);
    if (bp < minBp)
      break;
    const auto operatorToken = take();
    if (op == "as") {
      auto expression = node(K::ast_cast, lhs->Loc);
      add(*expression, std::move(lhs));
      add(*expression, type());
      expression->Loc.End = expression->children.back()->Loc.End;
      lhs = std::move(expression);
      continue;
    }
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
        expression->Loc.End = expect(")").Loc.End;
      } else if (op == "[") {
        if (at(":")) {
          expression->kind = K::ast_slice;
          expression->text = "end";
          take();
          if (!at("]"))
            add(*expression, expr());
          else
            expression->text = "all";
        } else {
          add(*expression, expr());
          if (eat(":")) {
            expression->kind = K::ast_slice;
            expression->text = "start";
            if (!at("]")) {
              add(*expression, expr());
              expression->text = "both";
            }
          }
        }
        expression->Loc.End = expect("]").Loc.End;
      } else {
        const auto member = at("annotation") || at("extern") || at("meta") ? take() : name();
        expression->text = spelling(member);
        expression->Loc.End = member.Loc.End;
      }
      lhs = std::move(expression);
    } else {
      auto rhs = expr(bp + 1);
      if ((bp == 30 || bp == 40) && (comparison(*lhs) || comparison(*rhs)))
        fail(operatorToken.Loc, DiagnosticKind::ComparisonChainsRequireParentheses);
      auto expression = node(K::ast_binary, lhs->Loc, std::string(op));
      add(*expression, std::move(lhs));
      add(*expression, std::move(rhs));
      lhs = std::move(expression);
    }
  }
  return lhs;
}

void Parser::recover(bool top, std::size_t start) {
  if (Position == start && !end() && !(at("}") && !top))
    take();
  while (!end()) {
    if (at("fn") || at("class") || at("enum") ||
        (at("alias") && peek(1).kind == lex::TokenKind::name) || at("annotation") ||
        (top && (at("@") || at("pub"))))
      return;
    if (!top && (at("}") || at("let") || at("return") || at("if") || at("match") || at("when") ||
                 at("while") || at("for") || at("asm") || at("break") || at("continue")))
      return;
    if (eat(";"))
      return;
    take();
  }
}

Parser::Ptr Parser::block() {
  Guard guard(*this);
  auto result = node(K::ast_block, expect("{").Loc);
  while (!at("}") && !end()) {
    // Preserve a subsequent top-level declaration when this block lacks '}'.
    if (at("fn") || at("class") || at("enum") || at("annotation") || at("@"))
      fail(peek().Loc, DiagnosticKind::ExpectedRightBraceBeforeDeclaration);
    const auto start = Position;
    try {
      add(*result, stmt());
    } catch (const ParseError &) {
      recover(false, start);
    }
  }
  result->Loc.End = expect("}").Loc.End;
  return result;
}

Parser::Ptr Parser::blockExpression() {
  Guard guard(*this);
  auto Result = node(K::ast_block_expr, expect("{").Loc);
  while (!at("}") && !end()) {
    const auto Start = Position;
    const auto DiagnosticCount = Parsed.diagnostics.size();
    try {
      auto Tail = expr();
      if (at("}")) {
        add(*Result, std::move(Tail));
        break;
      }
    } catch (const ParseError &) {
    }
    Position = Start;
    Parsed.diagnostics.resize(DiagnosticCount);
    add(*Result, stmt());
  }
  Result->Loc.End = expect("}").Loc.End;
  return Result;
}

Parser::Ptr Parser::assembly() {
  const auto Start = take().Loc;
  auto Result = node(K::ast_asm, Start);
  expect("{");
  if (peek().kind != lex::TokenKind::asm_text)
    fail(peek().Loc, DiagnosticKind::ExpectedExpression);
  Result->text = spelling(take());
  expect("}");

  while (eat(".")) {
    const auto Method = at("in") ? take() : name();
    const auto MethodName = spelling(Method);
    expect("(");
    if (MethodName == "in" || MethodName == "out") {
      const auto Value = name();
      auto Binding = node(MethodName == "in" ? K::ast_asm_input : K::ast_asm_output,
                          Method.Loc,
                          std::string(spelling(Value)));
      if (eat(",")) {
        const auto Register = name();
        add(*Binding, node(K::ast_name, Register.Loc, std::string(spelling(Register))));
      }
      Binding->Loc.End = expect(")").Loc.End;
      add(*Result, std::move(Binding));
      continue;
    }
    if (MethodName != "op")
      fail(Method.Loc, DiagnosticKind::InvalidInlineAssembly);
    do {
      const auto OptionName = name();
      auto Option = node(K::ast_asm_option, OptionName.Loc, std::string(spelling(OptionName)));
      if (Option->text == "clobber" && eat("(")) {
        if (!at(")")) {
          do {
            const auto Register = name();
            add(*Option, node(K::ast_name, Register.Loc, std::string(spelling(Register))));
          } while (eat(","));
        }
        expect(")");
      }
      add(*Result, std::move(Option));
    } while (eat(","));
    expect(")");
  }
  Result->Loc.End = expect(";").Loc.End;
  return Result;
}

Parser::Ptr Parser::stmt() {
  Guard guard(*this);
  if (at("alias"))
    return decl();
  if (at("{"))
    return block();
  if (at("asm"))
    return assembly();
  if (at("let")) {
    auto result = node(K::ast_let, take().Loc, "let");
    if (at("(")) {
      auto bindings = node(K::ast_binding_list, take().Loc);
      do {
        const auto id = name();
        add(*bindings, node(K::ast_name, id.Loc, std::string(spelling(id))));
      } while (eat(",") && !at(")"));
      bindings->Loc.End = expect(")").Loc.End;
      add(*result, std::move(bindings));
      expect("=");
      add(*result, expr());
      result->Loc.End = expect(";").Loc.End;
      return result;
    }
    const auto id = name();
    add(*result, node(K::ast_name, id.Loc, std::string(spelling(id))));
    const bool annotated = eat(":");
    if (annotated)
      add(*result, type());
    if (eat("="))
      add(*result, expr());
    else if (!annotated)
      fail(peek().Loc, DiagnosticKind::ExpectedTypeAnnotationOrInitializer);
    result->Loc.End = expect(";").Loc.End;
    return result;
  }
  if (at("return") || at("break") || at("continue")) {
    const auto t = take();
    auto result = node(spelling(t) == "return"  ? K::ast_return
                       : spelling(t) == "break" ? K::ast_break
                                                : K::ast_continue,
                       t.Loc);
    if (result->kind == K::ast_return && !at(";")) {
      do {
        add(*result, expr());
      } while (eat(","));
    }
    result->Loc.End = expect(";").Loc.End;
    return result;
  }
  if (at("for")) {
    const auto Start = take().Loc;
    auto Result = node(K::ast_for, Start);
    const auto Binding = name();
    add(*Result, node(K::ast_name, Binding.Loc, std::string(spelling(Binding))));
    expect("in");
    add(*Result, expr());
    add(*Result, block());
    return Result;
  }
  if (at("if") || at("when") || at("while")) {
    const auto t = take();
    const auto Kind = spelling(t) == "if"     ? K::ast_if
                      : spelling(t) == "when" ? K::ast_when
                                              : K::ast_while;
    auto result = node(Kind, t.Loc);
    add(*result, expr());
    add(*result, block());
    if ((result->kind == K::ast_if || result->kind == K::ast_when) && eat("else"))
      add(*result, at(result->kind == K::ast_if ? "if" : "when") ? stmt() : block());
    return result;
  }
  auto lhs = expr();
  auto result = node(K::ast_expr_stmt, lhs->Loc);
  if (eat("=")) {
    if (lhs->kind != K::ast_name && lhs->kind != K::ast_member && lhs->kind != K::ast_index &&
        !(lhs->kind == K::ast_unary && lhs->text == "*"))
      fail(lhs->Loc, DiagnosticKind::InvalidAssignmentTarget);
    result->kind = K::ast_assign;
    add(*result, std::move(lhs));
    add(*result, expr());
  } else
    add(*result, std::move(lhs));
  result->Loc.End = expect(";").Loc.End;
  return result;
}

Parser::Ptr Parser::annotationWhen() {
  Guard guard(*this);
  auto Result = node(K::ast_when, take().Loc);
  add(*Result, expr());
  auto Then = node(K::ast_annotation_body, peek().Loc);
  classMembers(*Then, true);
  add(*Result, std::move(Then));
  if (eat("else")) {
    if (at("when")) {
      add(*Result, annotationWhen());
    } else {
      auto Else = node(K::ast_annotation_body, peek().Loc);
      classMembers(*Else, true);
      add(*Result, std::move(Else));
    }
  }
  Result->Loc.End = Result->children.back()->Loc.End;
  return Result;
}

void Parser::classMembers(Node &Result, bool AnnotationBody) {
  expect("{");
  while (!at("}") && !end()) {
    if (AnnotationBody && at("when")) {
      add(Result, annotationWhen());
      continue;
    }
    auto memberAnnotations = annotations();
    Token memberVisibility{};
    const bool memberPublic = at("pub");
    if (memberPublic)
      memberVisibility = take();
    const bool constructor = at("init") && (spelling(peek(1)) == "(" || spelling(peek(1)) == "<");
    const bool destructor = at("deinit") && spelling(peek(1)) == "(";
    if (at("alias")) {
      auto Alias = node(K::ast_alias_decl,
                        !memberAnnotations.empty() ? memberAnnotations.front()->Loc
                        : memberPublic             ? memberVisibility.Loc
                                                   : peek().Loc);
      take();
      Alias->text = spelling(name());
      for (auto &Annotation : memberAnnotations)
        add(*Alias, std::move(Annotation));
      if (memberPublic)
        add(*Alias, node(K::ast_public, memberVisibility.Loc));
      genericParameters(*Alias, false);
      expect("=");
      add(*Alias, type());
      Alias->Loc.End = expect(";").Loc.End;
      add(Result, std::move(Alias));
      continue;
    }
    if (at("fn") || constructor || destructor) {
      const auto start = take();
      const auto memberLoc = !memberAnnotations.empty() ? memberAnnotations.front()->Loc
                             : memberPublic             ? memberVisibility.Loc
                                                        : start.Loc;
      auto member = node(constructor  ? K::ast_constructor
                         : destructor ? K::ast_destructor
                                      : K::ast_function,
                         memberLoc,
                         constructor  ? "init"
                         : destructor ? "deinit"
                                      : std::string(spelling(name())));
      for (auto &annotation : memberAnnotations)
        add(*member, std::move(annotation));
      if (memberPublic)
        add(*member, node(K::ast_public, memberVisibility.Loc));
      if (constructor)
        genericParameters(*member);
      parameters(*member);
      if (member->kind == K::ast_function && eat("->"))
        add(*member, type());
      if (at(";"))
        member->Loc.End = take().Loc.End;
      else
        add(*member, block());
      add(Result, std::move(member));
      continue;
    }
    const bool Constant = eat("const");
    const auto id = name();
    const auto fieldLoc = !memberAnnotations.empty() ? memberAnnotations.front()->Loc
                          : memberPublic             ? memberVisibility.Loc
                                                     : id.Loc;
    auto field =
        node(Constant ? K::ast_const_field : K::ast_field, fieldLoc, std::string(spelling(id)));
    for (auto &annotation : memberAnnotations)
      add(*field, std::move(annotation));
    if (memberPublic)
      add(*field, node(K::ast_public, memberVisibility.Loc));
    expect(":");
    add(*field, type());
    if (Constant) {
      expect("=");
      add(*field, expr());
    }
    field->Loc.End = expect(";").Loc.End;
    add(Result, std::move(field));
  }
  Result.Loc.End = expect("}").Loc.End;
}

Parser::Ptr Parser::decl(std::vector<Ptr> annotations) {
  Token visibility{};
  const bool isPublic = at("pub");
  if (isPublic)
    visibility = take();
  if (!at("fn") && !at("class") && !at("enum") && !at("alias") && !at("annotation"))
    fail(peek().Loc, DiagnosticKind::ExpectedDeclaration);
  const auto t = take();
  auto Loc = t.Loc;
  if (!annotations.empty())
    Loc = annotations.front()->Loc;
  else if (isPublic)
    Loc = visibility.Loc;
  auto result = node(spelling(t) == "fn"      ? K::ast_function
                     : spelling(t) == "class" ? K::ast_class
                     : spelling(t) == "enum"  ? K::ast_enum
                     : spelling(t) == "alias" ? K::ast_alias_decl
                                              : K::ast_annotation_decl,
                     Loc);
  result->text = spelling(
      result->kind == K::ast_annotation_decl && (at("extern") || at("meta")) ? take() : name());
  for (auto &annotation : annotations)
    add(*result, std::move(annotation));
  if (isPublic)
    add(*result, node(K::ast_public, visibility.Loc));
  if (result->kind != K::ast_annotation_decl && result->kind != K::ast_enum)
    genericParameters(*result);
  if (result->kind == K::ast_enum) {
    if (eat(":"))
      add(*result, type());
    expect("{");
    if (at("}"))
      fail(peek().Loc, DiagnosticKind::ExpectedIdentifier);
    do {
      const auto Item = name();
      auto Variant = node(K::ast_enum_variant, Item.Loc, std::string(spelling(Item)));
      if (eat("="))
        add(*Variant, expr());
      add(*result, std::move(Variant));
    } while (eat(",") && !at("}"));
    result->Loc.End = expect("}").Loc.End;
  } else if (result->kind == K::ast_alias_decl) {
    expect("=");
    add(*result, type());
    result->Loc.End = expect(";").Loc.End;
  } else if (result->kind == K::ast_annotation_decl) {
    expect("(");
    if (!at(")")) {
      do {
        const auto Id = name();
        auto Parameter = node(K::ast_annotation_parameter, Id.Loc, std::string(spelling(Id)));
        expect(":");
        add(*Parameter, type());
        if (eat("="))
          add(*Parameter, expr());
        add(*result, std::move(Parameter));
      } while (eat(",") && !at(")"));
    }
    expect(")");
    if (eat("=")) {
      auto Composition = node(K::ast_annotation_uses, peek().Loc);
      do {
        if (!at("@"))
          fail(peek().Loc, DiagnosticKind::InvalidAnnotation);
        add(*Composition, annotation());
      } while (eat("+"));
      add(*result, std::move(Composition));
    }
    if (at("{")) {
      auto Body = node(K::ast_annotation_body, peek().Loc);
      classMembers(*Body, true);
      add(*result, std::move(Body));
      result->Loc.End = result->children.back()->Loc.End;
    } else {
      result->Loc.End = expect(";").Loc.End;
    }
  } else if (result->kind == K::ast_class) {
    if (eat(":")) {
      do {
        auto Base = type();
        auto Parent = node(K::ast_base_type, Base->Loc);
        add(*Parent, std::move(Base));
        add(*result, std::move(Parent));
      } while (eat(","));
    }
    classMembers(*result);
  } else {
    parameters(*result);
    if (eat("->"))
      add(*result, type());
    if (at(";"))
      result->Loc.End = take().Loc.End;
    else
      add(*result, block());
  }
  return result;
}

void Parser::run() {
  Parsed.root = node(K::ast_module, {Parsed.File, 0, Parsed.source.size(), 1, 1});
  std::vector<Ptr> LeadingAnnotations;
  if (at("@")) {
    const auto Start = Position;
    try {
      LeadingAnnotations = annotations();
    } catch (const ParseError &) {
      LeadingAnnotations.clear();
      recover(true, Start);
    }
  }
  if (at("module")) {
    const auto start = Position;
    try {
      take();
      auto declaration = qualified(K::ast_module_decl);
      if (!LeadingAnnotations.empty())
        declaration->Loc = LeadingAnnotations.front()->Loc;
      for (auto &Annotation : LeadingAnnotations)
        add(*declaration, std::move(Annotation));
      LeadingAnnotations.clear();
      declaration->Loc.End = expect(";").Loc.End;
      add(*Parsed.root, std::move(declaration));
    } catch (const ParseError &) {
      recover(true, start);
    }
  }
  bool SeenDeclaration = false;
  while (!end()) {
    const auto start = Position;
    try {
      std::vector<Ptr> Annotations = std::move(LeadingAnnotations);
      for (auto &Annotation : annotations())
        Annotations.push_back(std::move(Annotation));
      if (at("import") && !SeenDeclaration) {
        take();
        auto Import = qualified(K::ast_import);
        for (auto &Annotation : Annotations)
          add(*Import, std::move(Annotation));
        if (Import->text == "c" && peek().kind == lex::TokenKind::string) {
          const auto Header = take();
          const auto Text = spelling(Header);
          add(*Import,
              node(K::ast_literal, Header.Loc, std::string(Text.substr(1, Text.size() - 2))));
        }
        Import->Loc.End = expect(";").Loc.End;
        add(*Parsed.root, std::move(Import));
      } else {
        SeenDeclaration = true;
        add(*Parsed.root, decl(std::move(Annotations)));
      }
    } catch (const ParseError &) {
      recover(true, start);
    }
  }
  Parsed.root->Loc.End = Parsed.source.size();
}

void Parser::runExpression() {
  try {
    Parsed.root = expr();
    if (!end())
      fail(peek().Loc, DiagnosticKind::ExpectedEndOfExpression);
  } catch (const ParseError &) {
  }
}

ParseResult Parser::parse(std::string source, std::string_view File) {
  Parsed = {};
  Position = 0;
  Depth = 0;
  static_cast<LexResult &>(Parsed) = Lexer().Tokenize(std::move(source), File);
  if (Parsed.ok()) {
    skipComments();
    run();
  }
  return std::move(Parsed);
}

ParseResult Parser::parseExpression(std::string source, std::string_view File) {
  Parsed = {};
  Position = 0;
  Depth = 0;
  static_cast<LexResult &>(Parsed) = Lexer().Tokenize(std::move(source), File);
  if (Parsed.ok()) {
    skipComments();
    runExpression();
  }
  return std::move(Parsed);
}
