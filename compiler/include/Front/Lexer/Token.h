#pragma once

#include "Front/Lexer/Diagnostic.h"
#include <ostream>
#include <string>
#include <string_view>

namespace kelyra::lex {
enum class TokenKind {
#define TOKEN(Kind, Spelling, Name, Category, Binding) Kind,
#include "Front/Lexer/Token.def"
#undef TOKEN
};

struct Token {
  TokenKind kind;
  Location Loc;
};

std::string GetTokenName(TokenKind Kind);
std::string_view GetTokenSpelling(TokenKind Kind);
TokenKind KeywordToken(std::string_view Spelling);
TokenKind PunctuationToken(std::string_view Spelling);
int GetBindingPower(TokenKind Kind);

inline std::ostream &operator<<(std::ostream &Stream, TokenKind Kind) {
  return Stream << GetTokenName(Kind);
}
} // namespace kelyra::lex
