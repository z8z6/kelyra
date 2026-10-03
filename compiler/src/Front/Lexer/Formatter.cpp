#include "Front/Lexer/Formatter.h"

#include <algorithm>
#include <cctype>
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

  unsigned Column() const {
    const auto Last = Output.rfind('\n');
    return static_cast<unsigned>(Output.size() -
                                 (Last == std::string::npos ? 0 : Last + 1));
  }

  unsigned IndentColumn() const { return Indent * 2; }

  void PadTo(unsigned Column) {
    if (LineStart)
      Write("");
    if (const auto Current = this->Column(); Current < Column)
      Output.append(Column - Current, ' ');
  }

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
         Previous == TokenKind::keyword_for ||
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
  if (Node.kind == NodeKind::ast_pointer_type)
    Pointers.insert(Node.Loc.Begin);
  if (Node.kind == NodeKind::ast_array_type && !Node.children.empty())
    ArrayElements.insert(Node.children.back()->Loc.Begin);
  if (Node.kind == NodeKind::ast_slice_type && Node.text.empty() &&
      !Node.children.empty())
    ArrayElements.insert(Node.children.front()->Loc.Begin);
  for (const auto &Child : Node.children)
    CollectTypeOffsets(*Child, Pointers, ArrayElements);
}

void CollectAnnotationEnds(const Node &Node,
                           std::unordered_set<std::size_t> &Ends,
                           bool InParameter = false) {
  if (Node.kind == NodeKind::ast_annotation_uses)
    return;
  InParameter |= Node.kind == NodeKind::ast_parameter ||
                 Node.kind == NodeKind::ast_parameter_pack;
  if (Node.kind == NodeKind::ast_annotation && !InParameter)
    Ends.insert(Node.Loc.End);
  for (const auto &Child : Node.children)
    CollectAnnotationEnds(*Child, Ends, InParameter);
}

struct ParameterLayout {
  bool Multiline = false;
  bool FirstOnNewLine = false;
  unsigned MaxPrefix = 0;
  unsigned FirstWidth = 0;
  std::unordered_map<std::size_t, unsigned> Starts;
  std::unordered_map<std::size_t, unsigned> ColonPads;
  std::unordered_set<std::size_t> Names;
  std::unordered_set<std::size_t> Commas;
};

void CollectParameterLayouts(
    const Node &Node, const ParseResult &Parsed,
    std::unordered_map<std::size_t, ParameterLayout> &Layouts) {
  const auto Kind = Node.kind;
  if (Kind == NodeKind::ast_function || Kind == NodeKind::ast_constructor ||
      Kind == NodeKind::ast_destructor) {
    const auto &Tokens = Parsed.tokens;
    const auto First = std::find_if(Tokens.begin(), Tokens.end(),
                                    [&](const Token &Token) {
                                      return Token.Loc.Begin >= Node.Loc.Begin;
                                    });
    auto Name = std::find_if(First, Tokens.end(), [&](const Token &Token) {
      return Token.kind == TokenKind::keyword_fn ||
             (Token.kind == TokenKind::name &&
              std::string_view(Parsed.source)
                      .substr(Token.Loc.Begin, Token.Loc.Length()) == Node.text);
    });
    if (Name != Tokens.end()) {
      const auto Open = std::find_if(Name + 1, Tokens.end(),
                                     [](const Token &Token) {
                                       return Token.kind == TokenKind::punc_left_paren;
                                     });
      if (Open != Tokens.end()) {
        ParameterLayout Layout;
        std::vector<std::pair<std::size_t, unsigned>> ColonWidths;
        unsigned MaxNameWidth = 0;
        unsigned Depth = 1;
        auto Close = Open + 1;
        for (; Close != Tokens.end() && Depth; ++Close) {
          if (Close->kind == TokenKind::punc_left_paren)
            ++Depth;
          else if (Close->kind == TokenKind::punc_right_paren)
            --Depth;
          else if (Depth == 1 && Close->kind == TokenKind::punc_comma)
            Layout.Commas.insert(Close->Loc.Begin);
        }
        if (Close != Tokens.end()) {
          const auto End = (Close - 1)->Loc.Begin;
          Layout.Multiline = std::string_view(Parsed.source)
                                 .substr(Open->Loc.End, End - Open->Loc.End)
                                 .find('\n') != std::string_view::npos;
        }
        for (const auto &Child : Node.children) {
          if (Child->kind != NodeKind::ast_parameter &&
              Child->kind != NodeKind::ast_parameter_pack)
            continue;
          const auto Start = Child->Loc.Begin;
          auto ParameterName = std::find_if(
              Tokens.begin(), Tokens.end(), [&](const Token &Token) {
                return Token.Loc.Begin >= Start && Token.kind == TokenKind::name &&
                       std::string_view(Parsed.source)
                               .substr(Token.Loc.Begin, Token.Loc.Length()) == Child->text;
              });
          if (ParameterName == Tokens.end())
            continue;
          if (Layout.Starts.empty()) {
            Layout.FirstWidth = Child->Loc.End - Start;
            Layout.FirstOnNewLine =
                std::string_view(Parsed.source)
                    .substr(Open->Loc.End, Start - Open->Loc.End)
                    .find('\n') != std::string_view::npos;
          }
          const auto Prefix = std::string_view(Parsed.source)
                                  .substr(Start, ParameterName->Loc.Begin - Start);
          unsigned Width = 0;
          bool Space = false;
          for (const char Character : Prefix)
            if (Character == ' ' || Character == '\t' || Character == '\n' ||
                Character == '\r')
              Space = true;
            else {
              if (Space && Width)
                ++Width;
              ++Width;
              Space = false;
            }
          if (Width)
            ++Width;
          for (auto Token = std::find_if(
                   Tokens.begin(), Tokens.end(), [&](const auto &Item) {
                     return Item.Loc.Begin >= Start;
                   });
               Token != Tokens.end() &&
               Token->Loc.Begin < ParameterName->Loc.Begin;
               ++Token) {
            if (Token->kind == TokenKind::punc_comma &&
                Token->Loc.End < Parsed.source.size() &&
                !std::isspace(static_cast<unsigned char>(
                    Parsed.source[Token->Loc.End])))
              ++Width;
          }
          Layout.Starts.emplace(Start, Width);
          Layout.Names.insert(ParameterName->Loc.Begin);
          Layout.MaxPrefix = std::max(Layout.MaxPrefix, Width);
          const auto Colon =
              std::find_if(ParameterName + 1, Tokens.end(), [](const auto &Item) {
                return Item.kind == TokenKind::punc_colon;
              });
          if (Colon != Tokens.end() && Colon->Loc.Begin < Child->Loc.End) {
            const auto NameWidth = static_cast<unsigned>(Child->text.size());
            ColonWidths.emplace_back(Colon->Loc.Begin, NameWidth);
            MaxNameWidth = std::max(MaxNameWidth, NameWidth);
          }
        }
        for (const auto &[Offset, Width] : ColonWidths)
          Layout.ColonPads.emplace(Offset, MaxNameWidth - Width);
        Layouts.emplace(Open->Loc.Begin, std::move(Layout));
      }
    }
  }
  for (const auto &Child : Node.children)
    CollectParameterLayouts(*Child, Parsed, Layouts);
}

void CollectClassLayout(const Node &Node, const ParseResult &Parsed,
                        std::unordered_map<std::size_t, unsigned> &NamePads,
                        std::unordered_map<std::size_t, unsigned> &ColonPads,
                        std::unordered_set<std::size_t> &BlankBefore) {
  if (Node.kind == NodeKind::ast_class) {
    const auto IsField = [](NodeKind Kind) {
      return Kind == NodeKind::ast_field || Kind == NodeKind::ast_const_field;
    };
    const auto IsMethod = [](NodeKind Kind) {
      return Kind == NodeKind::ast_function ||
             Kind == NodeKind::ast_constructor ||
             Kind == NodeKind::ast_destructor;
    };
    const auto &Members = Node.children;
    for (std::size_t Index = 0; Index < Members.size();) {
      const auto &Member = *Members[Index];
      if (Index && ((IsField(Member.kind) && IsMethod(Members[Index - 1]->kind)) ||
                    (IsMethod(Member.kind) && IsField(Members[Index - 1]->kind))))
        BlankBefore.insert(Member.Loc.Begin);
      if (!IsField(Member.kind)) {
        ++Index;
        continue;
      }
      struct FieldAlignment {
        std::size_t NameOffset;
        std::size_t ColonOffset;
        unsigned PrefixWidth;
        unsigned NameWidth;
      };
      std::vector<FieldAlignment> Group;
      unsigned MaximumPrefix = 0;
      unsigned MaximumName = 0;
      do {
        const auto &Field = *Members[Index];
        unsigned PrefixWidth = 0;
        if (Field.kind == NodeKind::ast_const_field)
          PrefixWidth += 6;
        if (std::any_of(Field.children.begin(), Field.children.end(),
                        [](const auto &Part) {
                          return Part->kind == NodeKind::ast_public;
                        }))
          PrefixWidth += 4;
        const auto Name = std::find_if(
            Parsed.tokens.begin(), Parsed.tokens.end(), [&](const Token &Token) {
              return Token.Loc.Begin >= Field.Loc.Begin &&
                     Token.kind == TokenKind::name &&
                     std::string_view(Parsed.source)
                             .substr(Token.Loc.Begin, Token.Loc.Length()) == Field.text;
            });
        if (Name != Parsed.tokens.end()) {
          const auto Colon = std::find_if(Name + 1, Parsed.tokens.end(),
                                          [](const Token &Token) {
                                            return Token.kind == TokenKind::punc_colon;
                                          });
          if (Colon != Parsed.tokens.end() &&
              Colon->Loc.Begin < Field.Loc.End) {
            const auto NameWidth = static_cast<unsigned>(Field.text.size());
            Group.push_back({Name->Loc.Begin, Colon->Loc.Begin, PrefixWidth,
                             NameWidth});
            MaximumPrefix = std::max(MaximumPrefix, PrefixWidth);
            MaximumName = std::max(MaximumName, NameWidth);
          }
        }
        ++Index;
      } while (Index < Members.size() && IsField(Members[Index]->kind));
      for (const auto &Field : Group) {
        NamePads.emplace(Field.NameOffset,
                         MaximumPrefix - Field.PrefixWidth);
        ColonPads.emplace(Field.ColonOffset,
                          MaximumName - Field.NameWidth);
      }
    }
  }
  for (const auto &Child : Node.children)
    CollectClassLayout(*Child, Parsed, NamePads, ColonPads, BlankBefore);
}

void CollectGenericAngles(const Node &Node, std::string_view Source,
                          std::unordered_set<std::size_t> &Angles) {
  if (Node.kind == NodeKind::ast_generic_type ||
      Node.kind == NodeKind::ast_generic_apply) {
    const auto Start = Node.kind == NodeKind::ast_generic_apply
                           ? Node.children.front()->Loc.End
                           : Node.Loc.Begin;
    const auto Open = Source.find('<', Start);
    if (Open < Node.Loc.End)
      Angles.insert(Open);
    if (Node.AssociatedOwnerArguments) {
      const auto OwnerClose = Source.find(
          '>', Node.children[Node.AssociatedOwnerArguments - 1]->Loc.End);
      if (OwnerClose < Node.Loc.End) {
        Angles.insert(OwnerClose);
        const auto AliasOpen = Source.find('<', OwnerClose + 1);
        if (AliasOpen < Node.Loc.End)
          Angles.insert(AliasOpen);
      }
    }
    const auto Close = Source.find('>', Node.children.back()->Loc.End);
    if (Close < Node.Loc.End)
      Angles.insert(Close);
  }
  if (Node.kind == NodeKind::ast_class ||
      Node.kind == NodeKind::ast_function ||
      Node.kind == NodeKind::ast_alias_decl ||
      Node.kind == NodeKind::ast_constructor) {
    const kelyra::lex::Node *First = nullptr;
    const kelyra::lex::Node *Last = nullptr;
    for (const auto &Child : Node.children)
      if (Child->kind == NodeKind::ast_generic_parameter ||
          Child->kind == NodeKind::ast_generic_pack) {
        if (!First)
          First = Child.get();
        Last = Child.get();
      }
    if (First) {
      const auto Open = Source.find('<', Node.Loc.Begin);
      const auto Close = Source.find('>', Last->Loc.End);
      if (Open < First->Loc.Begin)
        Angles.insert(Open);
      if (Close < Node.Loc.End)
        Angles.insert(Close);
    }
  }
  for (const auto &Child : Node.children)
    CollectGenericAngles(*Child, Source, Angles);
}

void CollectLogicalBreaks(
    const Node &Node, const ParseResult &Parsed,
    std::unordered_map<std::size_t, std::size_t> &BreakAfter,
    bool NestedLogical = false) {
  const bool Logical = Node.kind == NodeKind::ast_binary &&
                       (Node.text == "||" || Node.text == "&&");
  if (Logical && !NestedLogical) {
    std::vector<const kelyra::lex::Node *> Operands;
    const auto Flatten = [&](const auto &Self,
                             const kelyra::lex::Node &Part) -> void {
      if (Part.kind == NodeKind::ast_binary && Part.text == Node.text &&
          Part.children.size() == 2) {
        Self(Self, *Part.children[0]);
        Self(Self, *Part.children[1]);
      } else {
        Operands.push_back(&Part);
      }
    };
    Flatten(Flatten, Node);
    unsigned Width = 0;
    bool Space = false;
    for (const char Character : std::string_view(Parsed.source).substr(
             Node.Loc.Begin, Node.Loc.Length())) {
      if (std::isspace(static_cast<unsigned char>(Character))) {
        Space = true;
      } else {
        Width += 1 + (Space && Width ? 1 : 0);
        Space = false;
      }
    }
    if (Operands.size() > 1 && Width + 4 > 80)
      for (std::size_t Index = 1; Index < Operands.size(); ++Index)
        for (const auto &Token : Parsed.tokens)
          if (Token.Loc.Begin >= Operands[Index - 1]->Loc.End &&
              Token.Loc.End <= Operands[Index]->Loc.Begin &&
              std::string_view(Parsed.source).substr(Token.Loc.Begin,
                                                     Token.Loc.Length()) == Node.text) {
            BreakAfter.emplace(Token.Loc.Begin, Node.Loc.End);
            break;
          }
  }
  for (const auto &Child : Node.children)
    CollectLogicalBreaks(*Child, Parsed, BreakAfter,
                         Logical && Child->kind == NodeKind::ast_binary &&
                             Child->text == Node.text);
}

std::vector<Token> NormalizeImports(const ParseResult &Parsed) {
  struct Import {
    std::size_t Begin;
    std::size_t StatementBegin;
    std::size_t StatementEnd;
    std::size_t End;
    std::string Module;
    std::string Identity;

    bool Decorated() const {
      return Begin != StatementBegin || End != StatementEnd;
    }
  };

  std::unordered_map<std::size_t, std::size_t> ImportAnnotations;
  if (Parsed.root)
    for (const auto &Child : Parsed.root->children) {
      if (Child->kind != NodeKind::ast_import)
        continue;
      for (const auto &Part : Child->children)
        if (Part->kind == NodeKind::ast_annotation) {
          const auto [It, Inserted] = ImportAnnotations.try_emplace(
              Child->Loc.Begin, Part->Loc.Begin);
          if (!Inserted)
            It->second = std::min(It->second, Part->Loc.Begin);
        }
    }

  std::vector<Import> Imports;
  for (std::size_t Index = 0; Index < Parsed.tokens.size(); ++Index) {
    if (Parsed.tokens[Index].kind != TokenKind::keyword_import)
      continue;

    Import Current{Index, Index, Index, Index, {}, {}};
    if (Index + 1 < Parsed.tokens.size())
      if (const auto It =
              ImportAnnotations.find(Parsed.tokens[Index + 1].Loc.Begin);
          It != ImportAnnotations.end())
        while (Current.Begin > 0 &&
               Parsed.tokens[Current.Begin - 1].Loc.Begin >= It->second)
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
                            .substr(Token.Loc.Begin, Token.Loc.Length());
      Current.Identity += Text;
      Current.Identity += '\x1f';
      if (Token.kind != TokenKind::string)
        Current.Module += Text;
    }
    Imports.push_back(std::move(Current));
  }
  if (Imports.size() < 2)
    return Parsed.tokens;

  const auto FirstIndex = Imports.front().Begin;
  std::vector<bool> IsImportToken(Parsed.tokens.size());
  for (const auto &Import : Imports)
    for (std::size_t Index = Import.Begin; Index <= Import.End; ++Index)
      IsImportToken[Index] = true;

  std::unordered_set<std::string> Seen;
  std::erase_if(Imports, [&](const Import &Import) {
    const bool Duplicate = !Seen.insert(Import.Identity).second;
    return !Import.Decorated() && Duplicate;
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

std::vector<Token> NormalizeDeclarations(const ParseResult &Parsed,
                                         const std::vector<Token> &Tokens) {
  if (!Parsed.root)
    return Tokens;
  struct Unit {
    std::size_t Begin;
    std::size_t End;
    std::string Name;
  };
  std::vector<Unit> Classes;
  std::vector<Unit> Functions;
  std::vector<bool> Moved(Tokens.size());
  for (const auto &Declaration : Parsed.root->children) {
    const bool Class = Declaration->kind == NodeKind::ast_class;
    if (!Class && Declaration->kind != NodeKind::ast_function)
      continue;
    auto Begin =
        std::find_if(Tokens.begin(), Tokens.end(), [&](const Token &Token) {
          return Token.Loc.Begin >= Declaration->Loc.Begin;
        });
    if (Begin == Tokens.end() || Begin->kind == TokenKind::end)
      continue;
    auto End = std::find_if(Begin, Tokens.end(), [&](const Token &Token) {
      return Token.Loc.End >= Declaration->Loc.End;
    });
    if (End == Tokens.end())
      continue;
    auto First = std::size_t(Begin - Tokens.begin());
    auto Last = std::size_t(End - Tokens.begin());
    if (Tokens[Last].kind != TokenKind::punc_semicolon &&
        Tokens[Last].kind != TokenKind::punc_right_brace &&
        Last + 1 < Tokens.size() &&
        Tokens[Last + 1].kind == TokenKind::punc_semicolon)
      ++Last;
    while (First && Tokens[First - 1].kind == TokenKind::comment &&
           !Moved[First - 1]) {
      const auto &Comment = Tokens[First - 1];
      const auto Gap = std::string_view(Parsed.source)
                           .substr(Comment.Loc.End, Tokens[First].Loc.Begin -
                                                          Comment.Loc.End);
      if (std::count(Gap.begin(), Gap.end(), '\n') > 1 ||
          (First > 1 && Tokens[First - 2].Loc.Line == Comment.Loc.Line &&
           Tokens[First - 2].kind != TokenKind::comment))
        break;
      --First;
    }
    if (Last + 1 < Tokens.size() &&
        Tokens[Last + 1].kind == TokenKind::comment &&
        Tokens[Last + 1].Loc.Line == Tokens[Last].Loc.Line)
      ++Last;
    if (std::any_of(Moved.begin() + First, Moved.begin() + Last + 1,
                    [](bool Value) { return Value; }))
      continue;
    std::fill(Moved.begin() + First, Moved.begin() + Last + 1, true);
    (Class ? Classes : Functions).push_back({First, Last, Declaration->text});
  }
  const auto ByName = [](const Unit &Left, const Unit &Right) {
    return Left.Name < Right.Name;
  };
  std::stable_sort(Classes.begin(), Classes.end(), ByName);
  std::stable_sort(Functions.begin(), Functions.end(), ByName);
  std::vector<Token> Result;
  std::vector<Token> Tail;
  Result.reserve(Tokens.size());
  const auto FinalDeclaration = std::max_element(
      Parsed.root->children.begin(), Parsed.root->children.end(),
      [](const auto &Left, const auto &Right) {
        return Left->Loc.End < Right->Loc.End;
      });
  const auto FinalOffset = FinalDeclaration == Parsed.root->children.end()
                               ? 0
                               : (*FinalDeclaration)->Loc.End;
  for (std::size_t Index = 0; Index < Tokens.size(); ++Index)
    if (!Moved[Index] && Tokens[Index].kind != TokenKind::end) {
      if (Tokens[Index].Loc.Begin >= FinalOffset)
        Tail.push_back(Tokens[Index]);
      else
        Result.push_back(Tokens[Index]);
    }
  for (const auto &Group : {Classes, Functions})
    for (const auto &Item : Group)
      Result.insert(Result.end(), Tokens.begin() + Item.Begin,
                    Tokens.begin() + Item.End + 1);
  Result.insert(Result.end(), Tail.begin(), Tail.end());
  Result.push_back(Tokens.back());
  return Result;
}

std::string MemberName(const Node &Node) {
  if (Node.kind == NodeKind::ast_name)
    return Node.text;
  if (Node.kind == NodeKind::ast_member && Node.children.size() == 1) {
    auto Parent = MemberName(*Node.children.front());
    if (!Parent.empty())
      return Parent + "." + Node.text;
  }
  return {};
}

std::vector<Token> ShortenQualifiedNames(const ParseResult &Parsed,
                                         const std::vector<Token> &Tokens,
                                         const FormatSymbols &Symbols) {
  if (!Parsed.root || !Symbols.Complete)
    return Tokens;
  std::vector<std::unordered_set<std::string>> Shadows;
  const auto Shadowed = [&](std::string_view Name) {
    for (auto Scope = Shadows.rbegin(); Scope != Shadows.rend(); ++Scope)
      if (Scope->contains(std::string(Name)))
        return true;
    return false;
  };
  std::unordered_set<std::size_t> Omit;
  const auto Visit = [&](const auto &Self, const Node &Node,
                         bool InInheritedClass) -> void {
    bool Scoped = false;
    if (Node.kind == NodeKind::ast_class) {
      InInheritedClass = std::any_of(
          Node.children.begin(), Node.children.end(), [](const auto &Child) {
            return Child->kind == NodeKind::ast_base_type;
          });
      Shadows.emplace_back();
      Scoped = true;
      for (const auto &Part : Node.children)
        if (Part->kind == NodeKind::ast_alias_decl ||
            Part->kind == NodeKind::ast_field ||
            Part->kind == NodeKind::ast_const_field ||
            Part->kind == NodeKind::ast_function ||
            Part->kind == NodeKind::ast_constructor ||
            Part->kind == NodeKind::ast_destructor ||
            Part->kind == NodeKind::ast_generic_parameter)
          Shadows.back().insert(Part->text);
    } else if (Node.kind == NodeKind::ast_function ||
               Node.kind == NodeKind::ast_constructor ||
               Node.kind == NodeKind::ast_destructor) {
      Shadows.emplace_back();
      Scoped = true;
      for (const auto &Part : Node.children)
        if (Part->kind == NodeKind::ast_parameter ||
            Part->kind == NodeKind::ast_generic_parameter)
          Shadows.back().insert(Part->text);
    } else if (Node.kind == NodeKind::ast_block ||
               Node.kind == NodeKind::ast_block_expr) {
      Shadows.emplace_back();
      for (const auto &Child : Node.children) {
        Self(Self, *Child, InInheritedClass);
        if (Child->kind == NodeKind::ast_alias_decl)
          Shadows.back().insert(Child->text);
        if (Child->kind == NodeKind::ast_let && !Child->children.empty() &&
            Child->children.front()->kind == NodeKind::ast_name)
          Shadows.back().insert(Child->children.front()->text);
      }
      Shadows.pop_back();
      return;
    } else if (Node.kind == NodeKind::ast_for && Node.children.size() == 3) {
      Self(Self, *Node.children[1], InInheritedClass);
      Shadows.emplace_back();
      Shadows.back().insert(Node.children.front()->text);
      Self(Self, *Node.children.back(), InInheritedClass);
      Shadows.pop_back();
      return;
    }
    std::string Qualified;
    if (Node.kind == NodeKind::ast_type ||
        Node.kind == NodeKind::ast_generic_type ||
        Node.kind == NodeKind::ast_annotation)
      Qualified = Node.text;
    else if (Node.kind == NodeKind::ast_member && !InInheritedClass)
      Qualified = MemberName(Node);
    std::string Module;
    std::string Symbol;
    for (const auto &[Candidate, Names] : Symbols.Visible) {
      if (!Qualified.starts_with(Candidate + ".") ||
          Candidate.size() <= Module.size())
        continue;
      auto Remainder = std::string_view(Qualified).substr(Candidate.size() + 1);
      const auto Dot = Remainder.find('.');
      const auto Name = std::string(Remainder.substr(0, Dot));
      if (Names.contains(Name)) {
        Module = Candidate;
        Symbol = Name;
      }
    }
    const auto ModuleHead = Module.substr(0, Module.find('.'));
    if (!Module.empty() && !Shadowed(Symbol) && !Shadowed(ModuleHead) &&
        !Symbols.TopLevelNames.contains(ModuleHead) &&
        (Module == Symbols.CurrentModule ||
         !Symbols.TopLevelNames.contains(Symbol))) {
      bool Ambiguous = false;
      if (Module != Symbols.CurrentModule)
        for (const auto &[Other, Names] : Symbols.Visible)
          if (Other != Module && Names.contains(Symbol))
            Ambiguous = true;
      if (!Ambiguous) {
        const auto Start =
            Node.Loc.Begin + (Node.kind == NodeKind::ast_annotation ? 1 : 0);
        auto First =
            std::find_if(Tokens.begin(), Tokens.end(), [&](const Token &Token) {
              return Token.Loc.Begin == Start;
            });
        const auto Segments = 1 + std::count(Module.begin(), Module.end(), '.');
        auto Index = std::size_t(First - Tokens.begin());
        bool Matches = Index + Segments * 2 <= Tokens.size();
        if (Matches) {
          std::size_t SegmentStart = 0;
          for (std::size_t I = 0; I < Segments; ++I) {
            const auto SegmentEnd = Module.find('.', SegmentStart);
            const auto Expected =
                Module.substr(SegmentStart, SegmentEnd == std::string::npos
                                                ? std::string::npos
                                                : SegmentEnd - SegmentStart);
            if (std::string_view(Parsed.source)
                    .substr(Tokens[Index + I * 2].Loc.Begin,
                            Tokens[Index + I * 2].Loc.Length()) != Expected ||
                Tokens[Index + I * 2 + 1].kind != TokenKind::punc_dot) {
              Matches = false;
              break;
            }
            SegmentStart = SegmentEnd + 1;
          }
        }
        if (Matches)
          for (std::size_t I = 0; I < Segments * 2; ++I)
            Omit.insert(Tokens[Index + I].Loc.Begin);
      }
    }
    for (const auto &Child : Node.children)
      Self(Self, *Child, InInheritedClass);
    if (Scoped)
      Shadows.pop_back();
  };
  Visit(Visit, *Parsed.root, false);
  if (Omit.empty())
    return Tokens;
  std::vector<Token> Result;
  Result.reserve(Tokens.size());
  for (const auto &Token : Tokens)
    if (!Omit.contains(Token.Loc.Begin))
      Result.push_back(Token);
  return Result;
}
} // namespace

std::string kelyra::lex::Format(const ParseResult &Parsed,
                                const FormatSymbols &Symbols) {
  const auto Tokens = ShortenQualifiedNames(
      Parsed, NormalizeDeclarations(Parsed, NormalizeImports(Parsed)), Symbols);
  std::unordered_set<std::size_t> TypePointers;
  std::unordered_set<std::size_t> ArrayElements;
  std::unordered_set<std::size_t> GenericAngles;
  std::unordered_set<std::size_t> AnnotationEnds;
  std::unordered_set<std::size_t> BlankAfterComments;
  std::unordered_set<std::size_t> BlankBeforeMembers;
  std::unordered_map<std::size_t, unsigned> FieldColonPads;
  std::unordered_map<std::size_t, unsigned> FieldNamePads;
  std::unordered_map<std::size_t, ParameterLayout> ParameterLayouts;
  std::unordered_map<std::size_t, std::size_t> LogicalBreaks;
  if (Parsed.root) {
    CollectTypeOffsets(*Parsed.root, TypePointers, ArrayElements);
    CollectGenericAngles(*Parsed.root, Parsed.source, GenericAngles);
    CollectAnnotationEnds(*Parsed.root, AnnotationEnds);
    CollectClassLayout(*Parsed.root, Parsed, FieldNamePads, FieldColonPads,
                       BlankBeforeMembers);
    CollectParameterLayouts(*Parsed.root, Parsed, ParameterLayouts);
    CollectLogicalBreaks(*Parsed.root, Parsed, LogicalBreaks);
  }
  for (std::size_t Index = 0; Index + 1 < Parsed.tokens.size(); ++Index) {
    const auto &Comment = Parsed.tokens[Index];
    const auto &Next = Parsed.tokens[Index + 1];
    if (Comment.kind != TokenKind::comment || Next.kind == TokenKind::end)
      continue;
    const auto Gap =
        std::string_view(Parsed.source)
            .substr(Comment.Loc.End, Next.Loc.Begin - Comment.Loc.End);
    if (std::count(Gap.begin(), Gap.end(), '\n') > 1)
      BlankAfterComments.insert(Comment.Loc.Begin);
  }
  Writer Output;
  std::vector<bool> StructBraces;
  std::vector<bool> AsmBraces;
  unsigned Parentheses = 0;
  struct ActiveParameters {
    const ParameterLayout *Layout;
    unsigned Depth;
    unsigned NameColumn;
    unsigned BaseColumn;
    bool Multiline;
    bool BreakFirst;
  };
  std::vector<ActiveParameters> ActiveParameterLists;
  std::vector<std::size_t> ContinuationEnds;
  std::size_t LastSourceEnd = 0;
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
        std::string_view(Parsed.source).substr(Token.Loc.Begin, Token.Loc.Length());

    while (!ContinuationEnds.empty() &&
           Token.Loc.Begin >= ContinuationEnds.back()) {
      Output.PopIndent();
      ContinuationEnds.pop_back();
    }
    if (Index && Token.Loc.Begin >= LastSourceEnd &&
        Output.IsLineStart()) {
      const auto Gap = std::string_view(Parsed.source).substr(
          LastSourceEnd, Token.Loc.Begin - LastSourceEnd);
      if (std::count(Gap.begin(), Gap.end(), '\n') > 1)
        Output.NewLine(true);
    }
    LastSourceEnd = Token.Loc.End;

    if (BlankBeforeMembers.contains(Token.Loc.Begin))
      Output.NewLine(true);
    if (const auto Pad = FieldNamePads.find(Token.Loc.Begin);
        Pad != FieldNamePads.end()) {
      if (Output.IsLineStart())
        Output.PadTo(Output.IndentColumn());
      Output.PadTo(Output.Column() + Pad->second);
    }
    if (!ActiveParameterLists.empty()) {
      const auto &Active = ActiveParameterLists.back();
      if (Active.Multiline) {
        const auto Start = Active.Layout->Starts.find(Token.Loc.Begin);
        if (Active.BreakFirst) {
          if (Start != Active.Layout->Starts.end())
            Output.PadTo(Active.BaseColumn);
          if (Active.Layout->Names.contains(Token.Loc.Begin))
            Output.PadTo(Active.NameColumn);
        } else if (Start != Active.Layout->Starts.end() &&
                   Active.NameColumn >= Start->second) {
          Output.PadTo(Active.NameColumn - Start->second);
        }
      }
    }

    if (Token.kind == TokenKind::comment) {
      if (!Output.IsLineStart())
        Output.Space();
      Output.Write(Text);
      Output.NewLine(BlankAfterComment ||
                     BlankAfterComments.contains(Token.Loc.Begin));
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
        if (Kind == TokenKind::keyword_class ||
            Kind == TokenKind::keyword_enum ||
            Kind == TokenKind::keyword_match) {
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
      if (Next == TokenKind::punc_comma || Next == TokenKind::punc_semicolon ||
          Next == TokenKind::punc_right_paren)
        break;
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
      Output.TrimSpace();
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
      if (!ActiveParameterLists.empty() &&
          ActiveParameterLists.back().Depth == Parentheses &&
          ActiveParameterLists.back().Multiline &&
          ActiveParameterLists.back().Layout->Commas.contains(Token.Loc.Begin))
        Output.NewLine();
      else if (!StructBraces.empty() && StructBraces.back() && Parentheses == 0)
        Output.NewLine();
      else
        Output.Space();
      break;
    case TokenKind::punc_colon:
      if (const auto Pad = FieldColonPads.find(Token.Loc.Begin);
          Pad != FieldColonPads.end())
        Output.PadTo(Output.Column() + Pad->second);
      if (!ActiveParameterLists.empty() && ActiveParameterLists.back().Multiline)
        if (const auto Pad = ActiveParameterLists.back().Layout->ColonPads.find(
                Token.Loc.Begin);
            Pad != ActiveParameterLists.back().Layout->ColonPads.end())
          Output.PadTo(Output.Column() + Pad->second);
      Output.Write(Text);
      Output.Space();
      break;
    case TokenKind::punc_dot:
    case TokenKind::punc_right_bracket:
    case TokenKind::punc_right_paren:
      if (Token.kind == TokenKind::punc_right_paren &&
          !ActiveParameterLists.empty() &&
          ActiveParameterLists.back().Depth == Parentheses &&
          ActiveParameterLists.back().BreakFirst)
        Output.NewLine();
      Output.TrimSpace();
      Output.Write(Text);
      if (Token.kind == TokenKind::punc_right_paren) {
        if (!ActiveParameterLists.empty() &&
            ActiveParameterLists.back().Depth == Parentheses)
          ActiveParameterLists.pop_back();
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
          Previous == TokenKind::keyword_while ||
          Previous == TokenKind::keyword_for)
        Output.Space();
      Output.Write(Text);
      ++Parentheses;
      if (const auto Layout = ParameterLayouts.find(Token.Loc.Begin);
          Layout != ParameterLayouts.end()) {
        const auto First = Index + 1 < Tokens.size()
                               ? Layout->second.Starts.find(Tokens[Index + 1].Loc.Begin)
                               : Layout->second.Starts.end();
        const auto FirstPrefix = First == Layout->second.Starts.end()
                                     ? 0U
                                     : First->second;
        const bool BreakFirst =
            Layout->second.FirstOnNewLine ||
            (Layout->second.Multiline && Layout->second.MaxPrefix) ||
            Output.Column() + Layout->second.FirstWidth > 80;
        const unsigned BaseColumn = Output.IndentColumn() + 2;
        ActiveParameterLists.push_back({
            &Layout->second, Parentheses,
            BreakFirst ? BaseColumn + Layout->second.MaxPrefix
                       : std::max(Output.Column() + FirstPrefix,
                                  Output.IndentColumn() + Layout->second.MaxPrefix),
            BaseColumn, Layout->second.Multiline || BreakFirst, BreakFirst});
        if (BreakFirst)
          Output.NewLine();
      }
      break;
    case TokenKind::punc_at:
      if (Previous == TokenKind::name ||
          Previous == TokenKind::punc_right_paren)
        Output.Space();
      Output.Write(Text);
      break;
    case TokenKind::keyword_else:
      Output.Write(Text);
      Output.Space();
      break;
    case TokenKind::keyword_as:
      Output.Space();
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
        if (GenericAngles.contains(Token.Loc.Begin)) {
          Output.TrimSpace();
          Output.Write(Text);
          break;
        }
        const bool TypePointer = TypePointers.contains(Token.Loc.Begin);
        const bool Unary = TypePointer || IsUnary(Tokens, Index);
        if (Unary &&
            (StartsWord(Previous) || Previous == TokenKind::punc_right_paren ||
             Previous == TokenKind::punc_right_bracket) &&
            !TypePointer)
          Output.Space();
        if (!Unary)
          Output.Space();
        Output.Write(Text);
        if (const auto Break = LogicalBreaks.find(Token.Loc.Begin);
            Break != LogicalBreaks.end()) {
          if (ContinuationEnds.empty() ||
              ContinuationEnds.back() != Break->second) {
            ContinuationEnds.push_back(Break->second);
            Output.PushIndent();
          }
          Output.NewLine();
        } else if (!Unary)
          Output.Space();
      } else {
        if (StartsWord(Token.kind) &&
            (StartsWord(Previous) || Previous == TokenKind::punc_right_paren ||
             (Previous == TokenKind::punc_right_bracket &&
              !ArrayElements.contains(Token.Loc.Begin) &&
              Token.kind != TokenKind::keyword_const)))
          Output.Space();
        Output.Write(Text);
      }
      break;
    }
    if (AnnotationEnds.contains(Token.Loc.End))
      Output.NewLine();
    Previous = Token.kind;
  }
  return Output.Take();
}
