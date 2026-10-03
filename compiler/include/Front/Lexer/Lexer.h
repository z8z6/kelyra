#pragma once

#include "Front/Lexer/Token.h"
#include <string>
#include <string_view>
#include <vector>

namespace kelyra::lex {
struct LexResult {
  std::string_view File;
  std::string source;
  std::vector<Token> tokens; // Includes comments and a final End token.
  std::vector<Diagnostic> diagnostics;
  bool ok() const { return diagnostics.empty(); }
};

class Lexer {
  LexResult Result;
  void Scan();
  static bool Skip(char C);
  static bool Digit(char C);
  static bool Alpha(char C);

public:
  LexResult Tokenize(std::string Source, std::string_view File = {});
};
} // namespace kelyra::lex
