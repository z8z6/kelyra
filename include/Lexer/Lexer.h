#pragma once

#include "Lexer/Diagnostic.h"
#include "Lexer/Token.h"
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace kelyra::lex {
struct Token {
  TokenKind kind;
  Location Loc;
};

// Children are in source order. Declarations: annotations first. Annotation
// declarations: parameters with type and optional default. Functions:
// parameters, optional return type, body. Let: name, optional type, optional
// initializer. If/When: condition, then, optional else. Text holds a name,
// operator, literal spelling, or "let" for Let.
struct Node {
  TokenKind kind;
  Location Loc;
  std::string text;
  std::vector<std::unique_ptr<Node>> children;
  std::size_t height = 1;
  bool GenericInstance = false;
  bool GenericArgument = false;
  std::string GenericOriginModule;
};

struct ParseResult {
  std::string_view File;
  std::string source;
  std::vector<Token> tokens; // Includes comments and a final End token.
  std::unique_ptr<Node> root;
  std::vector<Diagnostic> diagnostics;
  bool ok() const { return diagnostics.empty(); }
};

class Lexer {
  using Ptr = std::unique_ptr<Node>;

  ParseResult result;
  std::size_t pos = 0;
  std::size_t depth = 0;

  static bool skip(char c);
  static bool digit(char c);
  static bool alpha(char c);
  static TokenKind keyword(std::string_view s);
  static TokenKind punctuation(std::string_view s);
  void lex();
  const Token &peek(std::size_t Offset = 0) const;
  std::string_view spelling(const Token &token) const;
  bool at(std::string_view spelling) const;
  bool end() const;
  void skipComments();
  Token take();
  bool eat(std::string_view spelling);
  [[noreturn]] void fail(Location Loc, DiagnosticKind Kind);
  Token expect(std::string_view spelling);
  Token name();
  Ptr qualified(TokenKind Kind, bool AllowWildcard = false);
  class Guard {
    Lexer &lexer;

  public:
    explicit Guard(Lexer &lexer);
    ~Guard();
  };
  Ptr node(TokenKind kind, Location Loc, std::string text = {});
  void add(Node &parent, Ptr child);
  Ptr type();
  Ptr annotation();
  static int binding(std::string_view op);
  static bool comparison(const Node &node);
  Ptr expr(int minBp = 0);
  void recover(bool top, std::size_t start);
  Ptr block();
  Ptr assembly();
  Ptr stmt();
  Ptr decl(std::vector<Ptr> annotations = {});
  void run();
  void runExpression();

public:
  ParseResult parse(std::string source, std::string_view File = {});
  ParseResult parseExpression(std::string source, std::string_view File = {});
  std::string dumpAst(const Node &node);
};
} // namespace kelyra::lex
