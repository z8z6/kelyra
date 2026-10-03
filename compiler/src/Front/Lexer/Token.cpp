#include "Front/Lexer/Token.h"

using namespace kelyra::lex;

namespace {
enum class Category { None, Keyword, Punctuation };
struct TokenInfo {
  TokenKind Kind;
  std::string_view Spelling;
  Category Type;
};
constexpr TokenInfo Tokens[] = {
#define TOKEN(Kind, Spelling, Name, Type, Binding) {TokenKind::Kind, Spelling, Category::Type},
#include "Front/Lexer/Token.def"
#undef TOKEN
};
} // namespace

std::string kelyra::lex::GetTokenName(TokenKind Kind) {
  switch (Kind) {
#define TOKEN(Kind, Spelling, Name, Category, Binding)                                             \
  case TokenKind::Kind:                                                                            \
    return Name;
#include "Front/Lexer/Token.def"
#undef TOKEN
  }
  return "Unknown";
}

std::string_view kelyra::lex::GetTokenSpelling(TokenKind Kind) {
  switch (Kind) {
#define TOKEN(Kind, Spelling, Name, Category, Binding)                                             \
  case TokenKind::Kind:                                                                            \
    return Spelling;
#include "Front/Lexer/Token.def"
#undef TOKEN
  }
  return {};
}

TokenKind kelyra::lex::KeywordToken(std::string_view Spelling) {
  for (const auto &Token : Tokens)
    if (Token.Type == Category::Keyword && Token.Spelling == Spelling)
      return Token.Kind;
  return TokenKind::name;
}

TokenKind kelyra::lex::PunctuationToken(std::string_view Spelling) {
  for (const auto &Token : Tokens)
    if (Token.Type == Category::Punctuation && Token.Spelling == Spelling)
      return Token.Kind;
  return TokenKind::illegal;
}

int kelyra::lex::GetBindingPower(TokenKind Kind) {
  switch (Kind) {
#define TOKEN(Kind, Spelling, Name, Category, Binding)                                             \
  case TokenKind::Kind:                                                                            \
    return Binding;
#include "Front/Lexer/Token.def"
#undef TOKEN
  }
  return -1;
}
