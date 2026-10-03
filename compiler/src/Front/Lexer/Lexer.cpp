#include "Front/Lexer/Lexer.h"
#include <utility>

using namespace kelyra::lex;

void Lexer::Scan() {
  const auto &s = Result.source;
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
      Result.diagnostics.push_back(
          {Kind, {Result.File, start, r, tokenLine, tokenColumn + start - l}});
    };
    const char c = s[r++];
    if (Skip(c)) {
      advance(l, r);
      continue;
    }
    TokenKind kind;
    if (Alpha(c) || (c == '$' && r < s.size() && s[r] == '{')) {
      if (c == '$')
        r = l;
      bool Interpolated = false;
      while (r < s.size()) {
        if (Alpha(s[r]) || Digit(s[r])) {
          ++r;
          continue;
        }
        if (r + 1 >= s.size() || s[r] != '$' || s[r + 1] != '{')
          break;
        Interpolated = true;
        r += 2;
        unsigned Braces = 1;
        bool Quoted = false;
        while (r < s.size() && Braces != 0) {
          const char Part = s[r++];
          if (Quoted && Part == '\\' && r < s.size()) {
            ++r;
          } else if (Part == '"') {
            Quoted = !Quoted;
          } else if (!Quoted && Part == '{') {
            ++Braces;
          } else if (!Quoted && Part == '}') {
            --Braces;
          }
        }
        if (Braces != 0)
          error(l, DiagnosticKind::ExpectedRightBrace);
      }
      kind = Interpolated ? TokenKind::name : KeywordToken(std::string_view(s).substr(l, r - l));
    } else if (Digit(c)) {
      kind = TokenKind::number;
      while (r < s.size() && Digit(s[r]))
        ++r;
      if (r + 1 < s.size() && s[r] == '.' && Digit(s[r + 1])) {
        ++r;
        while (r < s.size() && Digit(s[r]))
          ++r;
      }
      if (r < s.size() && (s[r] == 'e' || s[r] == 'E')) {
        ++r;
        if (r < s.size() && (s[r] == '+' || s[r] == '-'))
          ++r;
        const auto exponent = r;
        while (r < s.size() && Digit(s[r]))
          ++r;
        if (r == exponent)
          error(l, DiagnosticKind::ExpectedExponentDigits);
      }
      if (r < s.size() && Alpha(s[r])) {
        while (r < s.size() && (Alpha(s[r]) || Digit(s[r])))
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
          if (std::string_view("\\\"nrt0").find(escaped) == std::string_view::npos)
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
      if (c == '{' && !Result.tokens.empty() &&
          Result.tokens.back().kind == TokenKind::keyword_asm) {
        Result.tokens.push_back(
            {TokenKind::punc_left_brace, {Result.File, l, l + 1, tokenLine, tokenColumn}});
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
        Result.tokens.push_back(
            {TokenKind::asm_text, {Result.File, BodyStart, r, BodyLine, BodyColumn}});
        advance(BodyStart, r);
        if (r == s.size()) {
          Result.diagnostics.push_back(
              {DiagnosticKind::ExpectedRightBrace, {Result.File, r, r, line, column}});
          continue;
        }
        Result.tokens.push_back(
            {TokenKind::punc_right_brace, {Result.File, r, r + 1, line, column}});
        advance(r, r + 1);
        ++r;
        continue;
      }
      kind = TokenKind::illegal;
      for (std::size_t Length = 3; Length != 0; --Length) {
        if (Length > s.size() - l)
          continue;
        kind = PunctuationToken(std::string_view(s).substr(l, Length));
        if (kind != TokenKind::illegal) {
          r = l + Length;
          break;
        }
      }
      if (kind == TokenKind::illegal) {
        error(l, DiagnosticKind::UnexpectedCharacter);
        advance(l, r);
        continue;
      }
    }
    Result.tokens.push_back({kind, {Result.File, l, r, tokenLine, tokenColumn}});
    advance(l, r);
  }
  Result.tokens.push_back({TokenKind::end, {Result.File, r, r, line, column}});
}

LexResult Lexer::Tokenize(std::string Source, std::string_view File) {
  Result = {};
  Result.File = File;
  Result.source = std::move(Source);
  Scan();
  return std::move(Result);
}

bool Lexer::Skip(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
bool Lexer::Digit(char c) { return c >= '0' && c <= '9'; }
bool Lexer::Alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
