#pragma once

#include "Lexer/Lexer.h"

#include <string>
#include <unordered_map>
#include <unordered_set>

namespace kelyra::lex {
struct FormatSymbols {
  std::string CurrentModule;
  std::unordered_map<std::string, std::unordered_set<std::string>> Visible;
  std::unordered_set<std::string> TopLevelNames;
  bool Complete = false;
};

std::string Format(const ParseResult &Parsed,
                   const FormatSymbols &Symbols = {});
}
