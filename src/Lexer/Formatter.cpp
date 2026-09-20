#include "Lexer/Formatter.h"

#include <string_view>
#include <vector>

using namespace kelyra::lex;

namespace {
class Writer {
  std::string Output;
  unsigned Indent = 0;
  bool LineStart = true;

public:
  void Write(std::string_view Text) {
    if (LineStart) {
      Output.append(Indent * 2, ' ');
      LineStart = false;
    }
    Output += Text;
  }

  void Space() {
    if (!LineStart && !Output.empty() && Output.back() != ' ')
      Output += ' ';
  }

  void TrimSpace() {
    while (!Output.empty() && Output.back() == ' ')
      Output.pop_back();
  }

  void NewLine(bool Blank = false) {
    TrimSpace();
    if (!Output.empty() && Output.back() != '\n')
      Output += '\n';
    if (Blank && !Output.empty() &&
        (Output.size() < 2 || Output[Output.size() - 2] != '\n'))
      Output += '\n';
    LineStart = true;
  }

  void PushIndent() { ++Indent; }
  void PopIndent() { --Indent; }
  bool IsLineStart() const { return LineStart; }

  void RawLines(std::string_view Text) {
    while (!Text.empty()) {
      const auto End = Text.find('\n');
      auto Line = Text.substr(0, End);
      const auto First = Line.find_first_not_of(" \t\r");
      const auto Last = Line.find_last_not_of(" \t\r");
      if (First != std::string_view::npos) {
        Write(Line.substr(First, Last - First + 1));
        NewLine();
      }
      if (End == std::string_view::npos)
        break;
      Text.remove_prefix(End + 1);
    }
  }

  std::string Take() {
    NewLine();
    return std::move(Output);
  }
};

bool IsOperator(TokenKind Kind) {
  return Kind >= TokenKind::op_arrow && Kind <= TokenKind::op_address;
}

bool IsUnary(const std::vector<Token> &Tokens, std::size_t Index) {
  const auto Kind = Tokens[Index].kind;
  if (Kind == TokenKind::op_not || Kind == TokenKind::op_address)
    return true;
  if (Kind != TokenKind::op_add && Kind != TokenKind::op_subtract &&
      Kind != TokenKind::op_multiply)
    return false;
  if (Index == 0)
    return true;
  const auto Previous = Tokens[Index - 1].kind;
  return IsOperator(Previous) || Previous == TokenKind::punc_left_paren ||
         Previous == TokenKind::punc_left_brace ||
         Previous == TokenKind::punc_left_bracket ||
         Previous == TokenKind::punc_comma ||
         Previous == TokenKind::punc_colon || Previous == TokenKind::punc_dot ||
         Previous == TokenKind::punc_semicolon ||
         Previous == TokenKind::keyword_if ||
         Previous == TokenKind::keyword_when ||
         Previous == TokenKind::keyword_while ||
         Previous == TokenKind::keyword_return;
}

bool StartsWord(TokenKind Kind) {
  return Kind == TokenKind::name || Kind == TokenKind::number ||
         Kind == TokenKind::string ||
         (Kind >= TokenKind::keyword_let && Kind <= TokenKind::keyword_pub);
}
} // namespace

std::string kelyra::lex::Format(const ParseResult &Parsed) {
  Writer Output;
  std::vector<bool> StructBraces;
  std::vector<bool> AsmBraces;
  unsigned Parentheses = 0;
  TokenKind Previous = TokenKind::end;
  bool Annotation = false;
  bool AsmChain = false;

  for (std::size_t Index = 0; Index < Parsed.tokens.size(); ++Index) {
    const auto &Token = Parsed.tokens[Index];
    if (Token.kind == TokenKind::end)
      break;
    const auto Text =
        std::string_view(Parsed.source).substr(Token.Loc.Offset, Token.Loc.Len);

    if (Token.kind == TokenKind::comment) {
      if (!Output.IsLineStart())
        Output.Space();
      Output.Write(Text);
      Output.NewLine();
      Previous = Token.kind;
      continue;
    }

    switch (Token.kind) {
    case TokenKind::punc_left_brace: {
      Output.Space();
      Output.Write(Text);
      Output.NewLine();
      Output.PushIndent();
      bool IsStruct = false;
      for (std::size_t I = Index; I > 0; --I) {
        const auto Kind = Parsed.tokens[I - 1].kind;
        if (Kind == TokenKind::keyword_struct) {
          IsStruct = true;
          break;
        }
        if (Kind == TokenKind::punc_left_brace ||
            Kind == TokenKind::punc_right_brace ||
            Kind == TokenKind::punc_semicolon)
          break;
      }
      StructBraces.push_back(IsStruct);
      AsmBraces.push_back(Previous == TokenKind::keyword_asm);
      break;
    }
    case TokenKind::punc_right_brace: {
      const bool WasAsm = !AsmBraces.empty() && AsmBraces.back();
      Output.PopIndent();
      Output.NewLine();
      Output.Write(Text);
      if (!StructBraces.empty())
        StructBraces.pop_back();
      if (!AsmBraces.empty())
        AsmBraces.pop_back();
      const auto Next = Index + 1 < Parsed.tokens.size()
                            ? Parsed.tokens[Index + 1].kind
                            : TokenKind::end;
      if (Next == TokenKind::keyword_else)
        Output.Space();
      else if (WasAsm && Next == TokenKind::punc_dot) {
        Output.NewLine();
        AsmChain = true;
      } else
        Output.NewLine(StructBraces.empty() && Next != TokenKind::end);
      break;
    }
    case TokenKind::punc_semicolon: {
      Output.Write(Text);
      const auto Next = Index + 1 < Parsed.tokens.size()
                            ? Parsed.tokens[Index + 1].kind
                            : TokenKind::end;
      const bool TopLevel = StructBraces.empty();
      const bool EndOfImports = TopLevel && Next != TokenKind::keyword_import &&
                                Next != TokenKind::keyword_module &&
                                Next != TokenKind::end;
      Output.NewLine(EndOfImports);
      AsmChain = false;
      break;
    }
    case TokenKind::punc_comma:
      Output.Write(Text);
      if (!StructBraces.empty() && StructBraces.back() && Parentheses == 0)
        Output.NewLine();
      else
        Output.Space();
      break;
    case TokenKind::punc_colon:
      Output.Write(Text);
      Output.Space();
      break;
    case TokenKind::punc_dot:
    case TokenKind::punc_right_bracket:
    case TokenKind::punc_right_paren:
      Output.TrimSpace();
      Output.Write(Text);
      if (Token.kind == TokenKind::punc_right_paren) {
        --Parentheses;
        if (Annotation && Parentheses == 0) {
          Output.NewLine();
          Annotation = false;
        }
        const auto Next = Index + 1 < Parsed.tokens.size()
                              ? Parsed.tokens[Index + 1].kind
                              : TokenKind::end;
        if (AsmChain && Next == TokenKind::punc_dot)
          Output.NewLine();
      }
      break;
    case TokenKind::punc_left_bracket:
      Output.Write(Text);
      break;
    case TokenKind::punc_left_paren:
      if (Previous == TokenKind::keyword_if ||
          Previous == TokenKind::keyword_when ||
          Previous == TokenKind::keyword_while)
        Output.Space();
      Output.Write(Text);
      ++Parentheses;
      break;
    case TokenKind::punc_at:
      Output.Write(Text);
      Annotation = true;
      break;
    case TokenKind::keyword_else:
      Output.Write(Text);
      Output.Space();
      break;
    case TokenKind::asm_text:
      Output.RawLines(Text);
      break;
    default:
      if (IsOperator(Token.kind)) {
        const bool Unary = IsUnary(Parsed.tokens, Index);
        if (Unary &&
            (StartsWord(Previous) || Previous == TokenKind::punc_right_paren ||
             Previous == TokenKind::punc_right_bracket))
          Output.Space();
        if (!Unary)
          Output.Space();
        Output.Write(Text);
        if (!Unary)
          Output.Space();
      } else {
        if (StartsWord(Token.kind) &&
            (StartsWord(Previous) || Previous == TokenKind::punc_right_paren ||
             Previous == TokenKind::punc_right_bracket))
          Output.Space();
        Output.Write(Text);
        const auto Next = Index + 1 < Parsed.tokens.size()
                              ? Parsed.tokens[Index + 1].kind
                              : TokenKind::end;
        if (Annotation && Token.kind == TokenKind::name &&
            Next != TokenKind::punc_dot && Next != TokenKind::punc_left_paren) {
          Output.NewLine();
          Annotation = false;
        }
      }
      break;
    }
    Previous = Token.kind;
  }
  return Output.Take();
}
