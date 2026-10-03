#pragma once

#include "Front/AST/AST.h"
#include "Front/Lexer/Lexer.h"

namespace kelyra::lex {
struct ParseResult : LexResult {
  std::unique_ptr<Node> root;

  void DumpAstIfRequested() const;
};

class Parser {
  using Ptr = std::unique_ptr<Node>;

  ParseResult Parsed;
  std::size_t Position = 0;
  std::size_t Depth = 0;

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
  Ptr qualified(NodeKind Kind);
  class Guard {
    Parser &Owner;

  public:
    explicit Guard(Parser &Owner);
    ~Guard();
  };
  Ptr node(NodeKind kind, Location Loc, std::string text = {});
  void add(Node &parent, Ptr child);
  Ptr type();
  Ptr annotation();
  std::vector<Ptr> annotations();
  void genericParameters(Node &Target, bool AllowPack = true);
  void parameters(Node &Target);
  static int binding(std::string_view op);
  static bool comparison(const Node &node);
  Ptr expr(int minBp = 0);
  void recover(bool top, std::size_t start);
  Ptr block();
  Ptr blockExpression();
  Ptr assembly();
  Ptr stmt();
  void classMembers(Node &Result, bool AnnotationBody = false);
  Ptr annotationWhen();
  Ptr decl(std::vector<Ptr> annotations = {});
  void run();
  void runExpression();

public:
  ParseResult parse(std::string source, std::string_view File = {});
  ParseResult parseExpression(std::string source, std::string_view File = {});
};
bool ValidateOutput(bool ExclusiveAction = false);
} // namespace kelyra::lex
