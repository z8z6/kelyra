#include "Lexer/Formatter.h"

#include <algorithm>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
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

void CollectTypeOffsets(const Node &Node,
                        std::unordered_set<std::size_t> &Pointers,
                        std::unordered_set<std::size_t> &ArrayElements) {
  if (Node.kind == TokenKind::ast_pointer_type)
    Pointers.insert(Node.Loc.Offset);
  if (Node.kind == TokenKind::ast_array_type && !Node.children.empty())
    ArrayElements.insert(Node.children.front()->Loc.Offset);
  for (const auto &Child : Node.children)
    CollectTypeOffsets(*Child, Pointers, ArrayElements);
}

void CollectAnnotationEnds(const Node &Node,
                           std::unordered_set<std::size_t> &Ends) {
  if (Node.kind == TokenKind::ast_annotation)
    Ends.insert(Node.Loc.End());
  for (const auto &Child : Node.children)
    CollectAnnotationEnds(*Child, Ends);
}

void CollectGenericAngles(const Node &Node, std::string_view Source,
                          std::unordered_set<std::size_t> &Angles) {
  if (Node.kind == TokenKind::ast_generic_type ||
      Node.kind == TokenKind::ast_generic_apply) {
    const auto Start = Node.kind == TokenKind::ast_generic_apply
                           ? Node.children.front()->Loc.End()
                           : Node.Loc.Offset + Node.text.size();
    const auto Open = Source.find('<', Start);
    if (Open < Node.Loc.End())
      Angles.insert(Open);
    if (Node.Loc.End() > 0 && Source[Node.Loc.End() - 1] == '>')
      Angles.insert(Node.Loc.End() - 1);
  }
  if (Node.kind == TokenKind::ast_class ||
      Node.kind == TokenKind::ast_function ||
      Node.kind == TokenKind::ast_alias_decl ||
      Node.kind == TokenKind::ast_constructor) {
    const kelyra::lex::Node *First = nullptr;
    const kelyra::lex::Node *Last = nullptr;
    for (const auto &Child : Node.children)
      if (Child->kind == TokenKind::ast_generic_parameter ||
          Child->kind == TokenKind::ast_generic_pack) {
        if (!First)
          First = Child.get();
        Last = Child.get();
      }
    if (First) {
      const auto Open = Source.find('<', Node.Loc.Offset);
      const auto Close = Source.find('>', Last->Loc.End());
      if (Open < First->Loc.Offset)
        Angles.insert(Open);
      if (Close < Node.Loc.End())
        Angles.insert(Close);
    }
  }
  for (const auto &Child : Node.children)
    CollectGenericAngles(*Child, Source, Angles);
}

std::vector<Token> NormalizeImports(const ParseResult &Parsed) {
  struct Import {
    std::size_t Begin;
    std::size_t StatementBegin;
    std::size_t StatementEnd;
    std::size_t End;
    std::string Module;
    std::string Identity;
    bool Plain;
    bool Wildcard;

    bool Decorated() const {
      return Begin != StatementBegin || End != StatementEnd;
    }
  };

  std::unordered_map<std::size_t, std::size_t> ImportAnnotations;
  if (Parsed.root)
    for (const auto &Child : Parsed.root->children) {
      if (Child->kind != TokenKind::ast_import)
        continue;
      for (const auto &Part : Child->children)
        if (Part->kind == TokenKind::ast_annotation) {
          const auto [It, Inserted] = ImportAnnotations.try_emplace(
              Child->Loc.Offset, Part->Loc.Offset);
          if (!Inserted)
            It->second = std::min(It->second, Part->Loc.Offset);
        }
    }

  std::vector<Import> Imports;
  for (std::size_t Index = 0; Index < Parsed.tokens.size(); ++Index) {
    if (Parsed.tokens[Index].kind != TokenKind::keyword_import)
      continue;

    Import Current{Index, Index, Index, Index, {}, {}, true, false};
    if (Index + 1 < Parsed.tokens.size())
      if (const auto It =
              ImportAnnotations.find(Parsed.tokens[Index + 1].Loc.Offset);
          It != ImportAnnotations.end())
        while (Current.Begin > 0 &&
               Parsed.tokens[Current.Begin - 1].Loc.Offset >= It->second)
          --Current.Begin;
    while (Current.Begin > 0 &&
           Parsed.tokens[Current.Begin - 1].kind == TokenKind::comment) {
      const auto Comment = Current.Begin - 1;
      if (Comment > 0 &&
          Parsed.tokens[Comment - 1].kind == TokenKind::punc_semicolon &&
          Parsed.tokens[Comment - 1].Loc.Line ==
              Parsed.tokens[Comment].Loc.Line)
        break;
      Current.Begin = Comment;
    }
    for (++Index; Index < Parsed.tokens.size(); ++Index) {
      const auto &Token = Parsed.tokens[Index];
      if (Token.kind == TokenKind::punc_semicolon) {
        Current.StatementEnd = Current.End = Index;
        if (Index + 1 < Parsed.tokens.size() &&
            Parsed.tokens[Index + 1].kind == TokenKind::comment &&
            Parsed.tokens[Index + 1].Loc.Line == Token.Loc.Line)
          Current.End = Index + 1;
        break;
      }
      if (Token.kind == TokenKind::comment)
        continue;
      const auto Text = std::string_view(Parsed.source)
                            .substr(Token.Loc.Offset, Token.Loc.Len);
      Current.Identity += Text;
      Current.Identity += '\x1f';
      if (Token.kind == TokenKind::string)
        Current.Plain = false;
      else
        Current.Module += Text;
    }
    Current.Wildcard = Current.Module.ends_with(".*");
    Imports.push_back(std::move(Current));
  }
  if (Imports.size() < 2)
    return Parsed.tokens;

  const auto FirstIndex = Imports.front().Begin;
  std::vector<bool> IsImportToken(Parsed.tokens.size());
  for (const auto &Import : Imports)
    for (std::size_t Index = Import.Begin; Index <= Import.End; ++Index)
      IsImportToken[Index] = true;

  std::unordered_set<std::string> Wildcards;
  for (const auto &Import : Imports)
    if (Import.Wildcard)
      Wildcards.insert(Import.Module.substr(0, Import.Module.size() - 2));

  std::unordered_set<std::string> Seen;
  std::erase_if(Imports, [&](const Import &Import) {
    const bool Duplicate = !Seen.insert(Import.Identity).second;
    return !Import.Decorated() &&
           (Duplicate || (Import.Plain && !Import.Wildcard &&
                          Wildcards.contains(Import.Module)));
  });
  std::stable_sort(Imports.begin(), Imports.end(),
                   [](const Import &Left, const Import &Right) {
                     return Left.Module < Right.Module ||
                            (Left.Module == Right.Module &&
                             Left.Identity < Right.Identity);
                   });

  std::vector<Token> Tokens;
  Tokens.reserve(Parsed.tokens.size());
  Tokens.insert(Tokens.end(), Parsed.tokens.begin(),
                Parsed.tokens.begin() + FirstIndex);
  for (const auto &Import : Imports)
    Tokens.insert(Tokens.end(), Parsed.tokens.begin() + Import.Begin,
                  Parsed.tokens.begin() + Import.End + 1);
  for (std::size_t Index = FirstIndex; Index < Parsed.tokens.size(); ++Index)
    if (!IsImportToken[Index])
      Tokens.push_back(Parsed.tokens[Index]);
  return Tokens;
}
} // namespace

std::string kelyra::lex::Format(const ParseResult &Parsed) {
  const auto Tokens = NormalizeImports(Parsed);
  std::unordered_set<std::size_t> TypePointers;
  std::unordered_set<std::size_t> ArrayElements;
  std::unordered_set<std::size_t> GenericAngles;
  std::unordered_set<std::size_t> AnnotationEnds;
  if (Parsed.root) {
    CollectTypeOffsets(*Parsed.root, TypePointers, ArrayElements);
    CollectGenericAngles(*Parsed.root, Parsed.source, GenericAngles);
    CollectAnnotationEnds(*Parsed.root, AnnotationEnds);
  }
  Writer Output;
  std::vector<bool> StructBraces;
  std::vector<bool> AsmBraces;
  unsigned Parentheses = 0;
  std::vector<unsigned> AnnotatedParameterDepths;
  TokenKind Previous = TokenKind::end;
  bool AsmChain = false;
  bool BlankAfterComment = false;
  bool ModuleDeclaration = false;
  const bool HasImports =
      std::any_of(Tokens.begin(), Tokens.end(), [](const Token &Token) {
        return Token.kind == TokenKind::keyword_import;
      });

  for (std::size_t Index = 0; Index < Tokens.size(); ++Index) {
    const auto &Token = Tokens[Index];
    if (Token.kind == TokenKind::end)
      break;
    const auto Text =
        std::string_view(Parsed.source).substr(Token.Loc.Offset, Token.Loc.Len);

    if (Token.kind == TokenKind::comment) {
      if (!Output.IsLineStart())
        Output.Space();
      Output.Write(Text);
      Output.NewLine(BlankAfterComment);
      BlankAfterComment = false;
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
        const auto Kind = Tokens[I - 1].kind;
        if (Kind == TokenKind::keyword_class) {
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
      const auto Next =
          Index + 1 < Tokens.size() ? Tokens[Index + 1].kind : TokenKind::end;
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
      std::size_t NextIndex = Index + 1;
      const bool InlineComment = NextIndex < Tokens.size() &&
                                 Tokens[NextIndex].kind == TokenKind::comment &&
                                 Tokens[NextIndex].Loc.Line == Token.Loc.Line;
      while (NextIndex < Tokens.size() &&
             Tokens[NextIndex].kind == TokenKind::comment)
        ++NextIndex;
      const auto Next =
          NextIndex < Tokens.size() ? Tokens[NextIndex].kind : TokenKind::end;
      const bool TopLevel = StructBraces.empty();
      const bool EndOfImports = TopLevel && Next != TokenKind::keyword_import &&
                                Next != TokenKind::keyword_module &&
                                Next != TokenKind::end;
      const bool Blank = EndOfImports || (ModuleDeclaration && HasImports);
      if (InlineComment) {
        Output.Space();
        BlankAfterComment = Blank;
      } else {
        Output.NewLine(Blank);
      }
      ModuleDeclaration = false;
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
      if (Token.kind == TokenKind::punc_right_paren &&
          !AnnotatedParameterDepths.empty() &&
          AnnotatedParameterDepths.back() == Parentheses) {
        Output.NewLine();
        Output.PopIndent();
        AnnotatedParameterDepths.pop_back();
      }
      Output.TrimSpace();
      Output.Write(Text);
      if (Token.kind == TokenKind::punc_right_paren) {
        --Parentheses;
        const auto Next =
            Index + 1 < Tokens.size() ? Tokens[Index + 1].kind : TokenKind::end;
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
      if (Parentheses && (Previous == TokenKind::punc_left_paren ||
                          Previous == TokenKind::punc_comma)) {
        Output.NewLine();
        if (AnnotatedParameterDepths.empty() ||
            AnnotatedParameterDepths.back() != Parentheses) {
          Output.PushIndent();
          AnnotatedParameterDepths.push_back(Parentheses);
        }
      }
      Output.Write(Text);
      break;
    case TokenKind::keyword_else:
      Output.Write(Text);
      Output.Space();
      break;
    case TokenKind::asm_text:
      Output.RawLines(Text);
      break;
    default:
      if (Token.kind == TokenKind::keyword_module)
        ModuleDeclaration = true;
      if (IsOperator(Token.kind)) {
        if (GenericAngles.contains(Token.Loc.Offset)) {
          Output.TrimSpace();
          Output.Write(Text);
          break;
        }
        const bool TypePointer = TypePointers.contains(Token.Loc.Offset);
        const bool Unary = TypePointer || IsUnary(Tokens, Index);
        if (Unary &&
            (StartsWord(Previous) || Previous == TokenKind::punc_right_paren ||
             Previous == TokenKind::punc_right_bracket) &&
            !TypePointer)
          Output.Space();
        if (!Unary)
          Output.Space();
        Output.Write(Text);
        if (!Unary)
          Output.Space();
      } else {
        if (StartsWord(Token.kind) &&
            (StartsWord(Previous) || Previous == TokenKind::punc_right_paren ||
             (Previous == TokenKind::punc_right_bracket &&
              !ArrayElements.contains(Token.Loc.Offset))))
          Output.Space();
        Output.Write(Text);
      }
      break;
    }
    if (AnnotationEnds.contains(Token.Loc.End()))
      Output.NewLine();
    Previous = Token.kind;
  }
  return Output.Take();
}
