#include "Lexer/Lexer.h"

#include "llvm/Support/CommandLine.h"
#include "llvm/Support/LSP/Logging.h"
#include "llvm/Support/LSP/Protocol.h"
#include "llvm/Support/LSP/Transport.h"
#include "llvm/Support/Program.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

using namespace kelyra;
using namespace llvm;
using namespace llvm::lsp;

namespace {
cl::opt<bool> Stdio("stdio", cl::desc("Use standard input/output for LSP"),
                    cl::init(false));

enum class SymbolType {
  Function,
  Class,
  Method,
  Field,
  Parameter,
  Variable,
  Module
};

struct Span {
  std::size_t Offset = 0;
  std::size_t Length = 0;
};

struct Symbol {
  SymbolType Kind;
  std::string Name;
  std::string Type;
  std::string Detail;
  std::string Module;
  Span Definition;
  Span Scope;
  bool Public = false;
  std::string Documentation;
  std::string Owner;
};

std::size_t Utf8Length(unsigned char C) {
  if ((C & 0x80) == 0)
    return 1;
  if ((C & 0xe0) == 0xc0)
    return 2;
  if ((C & 0xf0) == 0xe0)
    return 3;
  if ((C & 0xf8) == 0xf0)
    return 4;
  return 1;
}

unsigned Utf16Width(std::string_view Text, std::size_t Offset) {
  const auto C = static_cast<unsigned char>(Text[Offset]);
  return Utf8Length(C) == 4 ? 2 : 1;
}

Position PositionAt(std::string_view Source, std::size_t Offset) {
  Offset = std::min(Offset, Source.size());
  Position Result;
  for (std::size_t I = 0; I < Offset;) {
    if (Source[I] == '\n') {
      ++Result.line;
      Result.character = 0;
      ++I;
      continue;
    }
    Result.character += Utf16Width(Source, I);
    I +=
        std::min(Utf8Length(static_cast<unsigned char>(Source[I])), Offset - I);
  }
  return Result;
}

std::size_t OffsetAt(std::string_view Source, Position Position) {
  std::size_t Offset = 0;
  for (int Line = 0; Line < Position.line && Offset < Source.size(); ++Line) {
    const auto End = Source.find('\n', Offset);
    Offset = End == std::string_view::npos ? Source.size() : End + 1;
  }
  int Character = 0;
  while (Offset < Source.size() && Source[Offset] != '\n' &&
         Character < Position.character) {
    Character += Utf16Width(Source, Offset);
    Offset += Utf8Length(static_cast<unsigned char>(Source[Offset]));
  }
  return Offset;
}

bool IsType(const lex::Node &Node) {
  return Node.kind == lex::TokenKind::ast_type ||
         Node.kind == lex::TokenKind::ast_pointer_type ||
         Node.kind == lex::TokenKind::ast_array_type ||
         Node.kind == lex::TokenKind::ast_result_types ||
         Node.kind == lex::TokenKind::ast_function_type;
}

std::string TypeName(const lex::Node &Node) {
  if (Node.kind == lex::TokenKind::ast_function_type) {
    std::string Result = "fn(";
    for (std::size_t I = 0; I + 1 < Node.children.size(); ++I) {
      if (I)
        Result += ", ";
      Result += TypeName(*Node.children[I]);
    }
    return Result + ") -> " + TypeName(*Node.children.back());
  }
  if (Node.kind == lex::TokenKind::ast_result_types) {
    std::string Result = "(";
    for (const auto &Child : Node.children) {
      if (Result.size() > 1)
        Result += ", ";
      Result += TypeName(*Child);
    }
    return Result + ")";
  }
  if (Node.kind == lex::TokenKind::ast_type)
    return Node.text;
  if (Node.children.empty())
    return {};
  auto Result = TypeName(*Node.children.front());
  if (Node.kind == lex::TokenKind::ast_pointer_type)
    return "*" + Result;
  if (Node.kind == lex::TokenKind::ast_array_type)
    return Result + "[" + Node.text + "]";
  return Result;
}

struct Document {
  // One `import` declaration: the module it names and the span to jump to.
  struct ImportRef {
    std::string Module;
    Span Extent;
    bool Wildcard = false;
  };

  std::string Path;
  URIForFile Uri;
  int64_t Version = 0;
  lex::ParseResult Parsed;
  std::string Module;
  Span ModuleSpan;
  std::vector<ImportRef> ImportRefs;
  // `import c "header.h"` and `import c.*`: the resolved header and the span
  // of the string literal, so both C declarations and the header are reachable.
  std::vector<std::pair<std::string, Span>> CHeaders;
  bool CImportWildcard = false;
  std::vector<Symbol> Symbols;

  std::string_view Spelling(const lex::Token &Token) const {
    return std::string_view(Parsed.source)
        .substr(Token.Loc.Offset, Token.Loc.Len);
  }

  static std::string CleanComment(std::string_view Text) {
    if (Text.starts_with("//"))
      Text.remove_prefix(2);
    else if (Text.starts_with("/*") && Text.ends_with("*/")) {
      Text.remove_prefix(2);
      Text.remove_suffix(2);
    }
    while (!Text.empty() &&
           std::isspace(static_cast<unsigned char>(Text.front())))
      Text.remove_prefix(1);
    while (!Text.empty() &&
           std::isspace(static_cast<unsigned char>(Text.back())))
      Text.remove_suffix(1);
    return std::string(Text);
  }

  std::string DocumentationAt(std::size_t Offset) const {
    std::vector<std::string> Lines;
    std::size_t Index = 0;
    while (Index < Parsed.tokens.size() &&
           Parsed.tokens[Index].Loc.Offset < Offset)
      ++Index;
    auto Cursor = Offset;
    while (Index > 0) {
      const auto &Token = Parsed.tokens[Index - 1];
      if (Token.kind != lex::TokenKind::comment)
        break;
      const auto Gap = std::string_view(Parsed.source)
                           .substr(Token.Loc.End(), Cursor - Token.Loc.End());
      if (std::count(Gap.begin(), Gap.end(), '\n') > 1 ||
          std::any_of(Gap.begin(), Gap.end(),
                      [](unsigned char C) { return !std::isspace(C); }))
        break;
      const auto LineStart = Parsed.source.rfind('\n', Token.Loc.Offset);
      const auto PrefixStart =
          LineStart == std::string::npos ? 0 : LineStart + 1;
      const auto Prefix =
          std::string_view(Parsed.source)
              .substr(PrefixStart, Token.Loc.Offset - PrefixStart);
      if (std::any_of(Prefix.begin(), Prefix.end(),
                      [](unsigned char C) { return !std::isspace(C); }))
        break;
      Lines.push_back(CleanComment(Spelling(Token)));
      Cursor = Token.Loc.Offset;
      --Index;
    }
    std::reverse(Lines.begin(), Lines.end());
    std::string Result;
    for (const auto &Line : Lines) {
      if (!Result.empty())
        Result += '\n';
      Result += Line;
    }
    return Result;
  }

  Span FindDeclaration(const lex::Node &Node, lex::TokenKind Keyword) const {
    bool SawKeyword = false;
    for (const auto &Token : Parsed.tokens) {
      if (Token.Loc.Offset < Node.Loc.Offset)
        continue;
      if (Token.Loc.Offset >= Node.Loc.End())
        break;
      if (Keyword == lex::TokenKind::name && Token.kind == Keyword &&
          Spelling(Token) == Node.text)
        return {Token.Loc.Offset, Token.Loc.Len};
      if (Token.kind == Keyword) {
        SawKeyword = true;
        continue;
      }
      if (SawKeyword && Token.kind == lex::TokenKind::name &&
          Spelling(Token) == Node.text)
        return {Token.Loc.Offset, Token.Loc.Len};
    }
    return {Node.Loc.Offset, Node.text.size()};
  }

  const Symbol *FindVisible(std::string_view Name, std::size_t Offset) const {
    const Symbol *Best = nullptr;
    for (const auto &Candidate : Symbols) {
      const bool Forward = Candidate.Kind == SymbolType::Function ||
                           Candidate.Kind == SymbolType::Class ||
                           Candidate.Kind == SymbolType::Method ||
                           Candidate.Kind == SymbolType::Field;
      if (Candidate.Name != Name ||
          (!Forward && Candidate.Definition.Offset > Offset) ||
          Offset < Candidate.Scope.Offset ||
          Offset > Candidate.Scope.Offset + Candidate.Scope.Length)
        continue;
      if (!Best || Candidate.Scope.Offset >= Best->Scope.Offset)
        Best = &Candidate;
    }
    return Best;
  }

  std::string InferType(const lex::Node &Node, std::size_t Offset) const {
    using K = lex::TokenKind;
    if (Node.kind == K::ast_literal) {
      if (Node.text == "true" || Node.text == "false")
        return "bool";
      if (!Node.text.empty() && Node.text.front() == '"')
        return "*c.char";
      return Node.text.find_first_of(".eE") == std::string::npos ? "i32"
                                                                 : "f64";
    }
    if (Node.kind == K::ast_name) {
      if (const auto *Found = FindVisible(Node.text, Offset))
        return Found->Type;
    }
    if (Node.kind == K::ast_unary && !Node.children.empty()) {
      auto Result = InferType(*Node.children.front(), Offset);
      if (Node.text == "&")
        return "*" + Result;
      if (Node.text == "*" && !Result.empty() && Result.front() == '*')
        Result.erase(Result.begin());
      return Result;
    }
    if ((Node.kind == K::ast_group || Node.kind == K::ast_binary) &&
        !Node.children.empty())
      return InferType(*Node.children.front(), Offset);
    if (Node.kind == K::ast_index && !Node.children.empty()) {
      auto Result = InferType(*Node.children.front(), Offset);
      const auto Bracket = Result.rfind('[');
      if (Bracket != std::string::npos)
        Result.erase(Bracket);
      return Result;
    }
    if (Node.kind == K::ast_call && !Node.children.empty()) {
      const lex::Node *Callee = Node.children.front().get();
      const std::string Name = Callee->kind == K::ast_member ? Callee->text
                               : Callee->kind == K::ast_name ? Callee->text
                                                             : "";
      for (const auto &Candidate : Symbols)
        if ((Candidate.Kind == SymbolType::Function ||
             Candidate.Kind == SymbolType::Class) &&
            Candidate.Name == Name)
          return Candidate.Type;
    }
    return {};
  }

  void CollectBlock(const lex::Node &Node, Span Scope) {
    using K = lex::TokenKind;
    if (Node.kind == K::ast_block)
      Scope = {Node.Loc.Offset, Node.Loc.Len};
    if (Node.kind == K::ast_let && !Node.children.empty()) {
      const auto &Name = *Node.children.front();
      std::string Type;
      const lex::Node *Initializer = nullptr;
      if (Node.children.size() > 1 && IsType(*Node.children[1])) {
        Type = TypeName(*Node.children[1]);
        if (Node.children.size() > 2)
          Initializer = Node.children[2].get();
      } else if (Node.children.size() > 1) {
        Initializer = Node.children[1].get();
      }
      if (Type.empty() && Initializer)
        Type = InferType(*Initializer, Name.Loc.Offset);
      if (Name.kind == K::ast_binding_list) {
        llvm::StringRef Remaining(Type);
        if (Remaining.starts_with("(") && Remaining.ends_with(")"))
          Remaining = Remaining.drop_front().drop_back();
        else
          Remaining = {};
        for (const auto &Binding : Name.children) {
          unsigned Depth = 0;
          std::size_t Separator = 0;
          for (; Separator < Remaining.size(); ++Separator) {
            const auto C = Remaining[Separator];
            if (C == ',' && Depth == 0)
              break;
            if (C == '(')
              ++Depth;
            else if (C == ')' && Depth)
              --Depth;
          }
          const auto BindingType = Remaining.take_front(Separator).trim().str();
          Remaining = Separator < Remaining.size()
                          ? Remaining.drop_front(Separator + 1)
                          : llvm::StringRef();
          Symbols.push_back({SymbolType::Variable,
                             Binding->text,
                             BindingType,
                             "let " + Binding->text + ": " + BindingType,
                             Module,
                             {Binding->Loc.Offset, Binding->Loc.Len},
                             Scope});
        }
        return;
      }
      Symbols.push_back(
          {SymbolType::Variable,
           Name.text,
           Type,
           Type.empty() ? "let " + Name.text : "let " + Name.text + ": " + Type,
           Module,
           {Name.Loc.Offset, Name.Loc.Len},
           Scope});
      Symbols.back().Documentation = DocumentationAt(Node.Loc.Offset);
    }
    for (const auto &Child : Node.children)
      CollectBlock(*Child, Scope);
  }

  void Rebuild(std::string Source) {
    Parsed = lex::Lexer().parse(std::move(Source), Path);
    Module.clear();
    ModuleSpan = {};
    ImportRefs.clear();
    CHeaders.clear();
    CImportWildcard = false;
    Symbols.clear();
    if (!Parsed.root)
      return;
    using K = lex::TokenKind;
    for (const auto &Node : Parsed.root->children) {
      if (Node->kind == K::ast_module_decl) {
        Module = Node->text;
        ModuleSpan = {Node->Loc.Offset, Node->text.size()};
      } else if (Node->kind == K::ast_import) {
        if (Node->text == "c") {
          if (Node->children.empty())
            continue;
          const auto &Header = *Node->children.front();
          CHeaders.emplace_back(
              (std::filesystem::path(Path).parent_path() / Header.text)
                  .lexically_normal()
                  .string(),
              Span{Header.Loc.Offset + 1, Header.Loc.Len - 2});
        } else if (Node->text == "c.*") {
          CImportWildcard = true;
        } else if (Node->text.ends_with(".*")) {
          ImportRefs.push_back({Node->text.substr(0, Node->text.size() - 2),
                                {Node->Loc.Offset, Node->Loc.Len},
                                true});
        } else {
          ImportRefs.push_back(
              {Node->text, {Node->Loc.Offset, Node->Loc.Len}, false});
        }
      }
    }

    const Span FileScope{0, Parsed.source.size()};
    for (const auto &Node : Parsed.root->children) {
      if (Node->kind != K::ast_function && Node->kind != K::ast_class)
        continue;
      const bool IsFunction = Node->kind == K::ast_function;
      const auto Definition =
          FindDeclaration(*Node, IsFunction ? K::keyword_fn : K::keyword_class);
      bool IsPublic = false;
      std::string ReturnType;
      std::string Detail =
          IsFunction ? "fn " + Node->text + "(" : "class " + Node->text;
      bool First = true;
      for (const auto &Child : Node->children) {
        if (Child->kind == K::ast_public)
          IsPublic = true;
        if (Child->kind == K::ast_parameter) {
          if (!First)
            Detail += ", ";
          Detail += Child->text + ": " + TypeName(*Child->children.front());
          First = false;
        } else if (IsType(*Child)) {
          ReturnType = TypeName(*Child);
        }
      }
      if (IsFunction) {
        Detail += ")";
        if (!ReturnType.empty())
          Detail += " -> " + ReturnType;
      }
      Symbols.push_back({IsFunction ? SymbolType::Function : SymbolType::Class,
                         Node->text, IsFunction ? ReturnType : Node->text,
                         Detail, Module, Definition, FileScope, IsPublic});
      Symbols.back().Documentation = DocumentationAt(Node->Loc.Offset);
      if (!IsFunction) {
        const Span ClassScope{Node->Loc.Offset, Node->Loc.Len};
        for (const auto &Member : Node->children) {
          if (Member->kind == K::ast_public ||
              Member->kind == K::ast_annotation)
            continue;
          const bool Field = Member->kind == K::ast_field;
          std::string Type;
          bool Public = false;
          std::string Detail = Field ? Member->text + ": " : Member->text + "(";
          bool First = true;
          for (const auto &Part : Member->children) {
            if (Part->kind == K::ast_public)
              Public = true;
            if (IsType(*Part))
              Type = TypeName(*Part);
            if (Part->kind == K::ast_parameter) {
              if (!First)
                Detail += ", ";
              Detail += Part->text + ": " + TypeName(*Part->children.front());
              First = false;
            }
          }
          if (Field)
            Detail += Type;
          else {
            Detail += ")";
            if (!Type.empty())
              Detail += " -> " + Type;
          }
          const auto Definition = Member->kind == K::ast_function
                                      ? FindDeclaration(*Member, K::keyword_fn)
                                      : FindDeclaration(*Member, K::name);
          Symbols.push_back({Field ? SymbolType::Field : SymbolType::Method,
                             Member->text, Type, Detail, Module, Definition,
                             ClassScope, Public,
                             DocumentationAt(Member->Loc.Offset), Node->text});
        }
      }
    }

    const auto CollectFunction = [&](const lex::Node *Node,
                                     const lex::Node *Owner) {
      const lex::Node *Body = nullptr;
      for (const auto &Child : Node->children)
        if (Child->kind == K::ast_block)
          Body = Child.get();
      if (!Body)
        return;
      const Span FunctionScope{Body->Loc.Offset, Body->Loc.Len};
      if (Owner)
        Symbols.push_back({SymbolType::Parameter, "this", "*" + Owner->text,
                           "this: *" + Owner->text, Module,
                           FindDeclaration(*Owner, K::keyword_class),
                           FunctionScope});
      for (const auto &Child : Node->children) {
        if (Child->kind != K::ast_parameter || Child->children.empty())
          continue;
        Symbols.push_back(
            {SymbolType::Parameter,
             Child->text,
             TypeName(*Child->children.front()),
             Child->text + ": " + TypeName(*Child->children.front()),
             Module,
             {Child->Loc.Offset, Child->text.size()},
             FunctionScope});
      }
      CollectBlock(*Body, FunctionScope);
    };
    for (const auto &Node : Parsed.root->children) {
      if (Node->kind == K::ast_function)
        CollectFunction(Node.get(), nullptr);
      if (Node->kind == K::ast_class)
        for (const auto &Member : Node->children)
          if (Member->kind == K::ast_function ||
              Member->kind == K::ast_constructor ||
              Member->kind == K::ast_destructor)
            CollectFunction(Member.get(), Node.get());
    }
  }

  Range ToRange(Span SourceSpan) const {
    return {PositionAt(Parsed.source, SourceSpan.Offset),
            PositionAt(Parsed.source, SourceSpan.Offset + SourceSpan.Length)};
  }
};

class Server {
  std::map<std::string, Document> Documents;

  // The compiler resolves modules against the enclosing Kelp workspace, so the
  // server mirrors the manifests: workspace members, cached dependency clones
  // under `.kelp/dependencies`, and local path dependencies. Only these keys
  // of a manifest matter here, so a full TOML parser is not needed.
  struct ProjectManifest {
    bool Workspace = false;
    std::vector<std::string> Members;
    std::vector<std::string> Paths;
  };

  static std::string Trim(std::string_view Text) {
    while (!Text.empty() &&
           std::isspace(static_cast<unsigned char>(Text.front())))
      Text.remove_prefix(1);
    while (!Text.empty() &&
           std::isspace(static_cast<unsigned char>(Text.back())))
      Text.remove_suffix(1);
    return std::string(Text);
  }

  // Quoted values on one line, with comments outside quotes removed first.
  static std::vector<std::string> QuotedValues(std::string_view Line) {
    std::string Code;
    bool Quoted = false;
    for (const char Character : Line) {
      if (Character == '"')
        Quoted = !Quoted;
      if (Character == '#' && !Quoted)
        break;
      Code += Character;
    }
    std::vector<std::string> Values;
    for (std::size_t Index = 0; Index < Code.size(); ++Index) {
      if (Code[Index] != '"')
        continue;
      const auto End = Code.find('"', Index + 1);
      if (End == std::string::npos)
        break;
      Values.push_back(Code.substr(Index + 1, End - Index - 1));
      Index = End;
    }
    return Values;
  }

  static ProjectManifest ReadManifest(const std::filesystem::path &Path) {
    ProjectManifest Result;
    std::ifstream Input(Path);
    if (!Input)
      return Result;
    std::string Section;
    std::string Line;
    while (std::getline(Input, Line)) {
      // Only a line that starts with `[` opens a section: array values such as
      // `members = ["app"]` contain brackets too.
      const auto First = Line.find_first_not_of(" \t");
      if (First != std::string::npos && Line[First] == '[') {
        const auto Close = Line.find(']', First);
        if (Close != std::string::npos) {
          Section =
              Trim(std::string_view(Line).substr(First + 1, Close - First - 1));
          if (Section == "workspace")
            Result.Workspace = true;
          continue;
        }
      }
      const auto Equals = Line.find('=');
      if (Equals == std::string::npos || Section.empty())
        continue;
      const auto Key = Trim(std::string_view(Line).substr(0, Equals));
      const auto Values = QuotedValues(Line);
      if (Values.empty())
        continue;
      if (Section == "workspace" && Key == "members")
        Result.Members = Values;
      else if (Section.starts_with("dependencies.") && Key == "path")
        Result.Paths.push_back(Values.front());
    }
    return Result;
  }

  // The outermost ancestor that declares `[workspace]`, matching Kelp's shared
  // dependency cache location.
  static std::filesystem::path
  WorkspaceRoot(const std::filesystem::path &Root) {
    std::filesystem::path Result = Root;
    std::error_code Error;
    for (auto Current = Root;
         !Current.empty() && Current != Current.root_path();
         Current = Current.parent_path()) {
      const auto Manifest = Current / "kelp.toml";
      if (!std::filesystem::exists(Manifest, Error)) {
        Error.clear();
        continue;
      }
      if (ReadManifest(Manifest).Workspace)
        Result = Current;
    }
    return Result;
  }

  void Index(const std::filesystem::path &Path) {
    std::ifstream Input(Path, std::ios::binary);
    std::string Source((std::istreambuf_iterator<char>(Input)), {});
    auto Uri = URIForFile::fromFile(
        std::filesystem::absolute(Path).lexically_normal().string());
    if (Input && Uri)
      Set(*Uri, std::move(Source), 0);
    else if (!Uri)
      consumeError(Uri.takeError());
  }

  void IndexSources(const std::filesystem::path &Directory) {
    std::error_code Error;
    std::filesystem::directory_iterator It(
        Directory, std::filesystem::directory_options::skip_permission_denied,
        Error);
    if (Error)
      return;
    for (const auto &Entry : It) {
      std::error_code KindError;
      const auto Name = Entry.path().filename().string();
      if (Entry.is_directory(KindError)) {
        if (Name == ".git" || Name == "build" || Name == "node_modules")
          continue;
        // Only the shared dependency cache under `.kelp` holds sources.
        IndexSources(Name == ".kelp" ? Entry.path() / "dependencies"
                                     : Entry.path());
        continue;
      }
      if (Entry.path().extension() == ".kly")
        Index(Entry.path());
    }
  }

  void LoadProject(const std::filesystem::path &Directory,
                   std::set<std::string> &Visited) {
    std::error_code Error;
    const auto Key =
        std::filesystem::weakly_canonical(Directory, Error).string();
    if (Error || !Visited.insert(Key).second)
      return;
    IndexSources(Directory);
    const auto Manifest = ReadManifest(Directory / "kelp.toml");
    for (const auto &Member : Manifest.Members)
      LoadProject((Directory / Member).lexically_normal(), Visited);
    for (const auto &Path : Manifest.Paths)
      LoadProject((Directory / Path).lexically_normal(), Visited);
  }

  Document *Get(const URIForFile &Uri) {
    const auto It = Documents.find(Uri.file().str());
    return It == Documents.end() ? nullptr : &It->second;
  }

  Document &Set(const URIForFile &Uri, std::string Source, int64_t Version) {
    auto [It, Inserted] = Documents.try_emplace(Uri.file().str());
    auto &Doc = It->second;
    if (Inserted) {
      Doc.Path = Uri.file().str();
      Doc.Uri = Uri;
    }
    Doc.Version = Version;
    Doc.Rebuild(std::move(Source));
    return Doc;
  }

  const lex::Token *TokenAt(const Document &Doc, std::size_t Offset,
                            std::size_t *Index = nullptr) const {
    for (std::size_t I = 0; I < Doc.Parsed.tokens.size(); ++I) {
      const auto &Token = Doc.Parsed.tokens[I];
      if ((Token.kind == lex::TokenKind::name ||
           Token.kind == lex::TokenKind::keyword_this) &&
          Offset >= Token.Loc.Offset && Offset <= Token.Loc.End()) {
        if (Index)
          *Index = I;
        return &Token;
      }
    }
    return nullptr;
  }

  std::pair<std::string, std::string>
  QualifiedName(const Document &Doc, std::size_t TokenIndex) const {
    std::vector<std::string> Parts{
        std::string(Doc.Spelling(Doc.Parsed.tokens[TokenIndex]))};
    while (TokenIndex >= 2 &&
           Doc.Parsed.tokens[TokenIndex - 1].kind == lex::TokenKind::punc_dot &&
           (Doc.Parsed.tokens[TokenIndex - 2].kind == lex::TokenKind::name ||
            Doc.Parsed.tokens[TokenIndex - 2].kind ==
                lex::TokenKind::keyword_this)) {
      Parts.push_back(
          std::string(Doc.Spelling(Doc.Parsed.tokens[TokenIndex - 2])));
      TokenIndex -= 2;
    }
    std::reverse(Parts.begin(), Parts.end());
    std::string Qualifier;
    for (std::size_t I = 0; I + 1 < Parts.size(); ++I) {
      if (!Qualifier.empty())
        Qualifier += '.';
      Qualifier += Parts[I];
    }
    return {Qualifier, Parts.back()};
  }

  std::pair<const Document *, const Symbol *>
  FindMember(const Document &Current, std::string Type,
             std::string_view Name) const {
    if (Type.starts_with("*"))
      Type.erase(Type.begin());
    for (const auto &[Path, Doc] : Documents)
      for (const auto &Member : Doc.Symbols) {
        if (Member.Owner.empty() || Member.Name != Name)
          continue;
        const auto Qualified =
            Doc.Module.empty() ? Member.Owner : Doc.Module + "." + Member.Owner;
        if ((Type == Qualified || (&Doc == &Current && Type == Member.Owner)) &&
            (Doc.Module == Current.Module || Member.Public))
          return {&Doc, &Member};
      }
    return {};
  }

  // Member access dereferences a pointer receiver implicitly.
  static std::string Dereference(std::string Type) {
    while (!Type.empty() && Type.front() == '*')
      Type.erase(Type.begin());
    return Type;
  }

  std::string ReceiverType(const Document &Doc, std::string_view Qualifier,
                           std::size_t Offset) const {
    const auto Dot = Qualifier.find('.');
    const auto *Root = Doc.FindVisible(Qualifier.substr(0, Dot), Offset);
    if (!Root)
      return {};
    std::string Type = Dereference(Root->Type);
    std::size_t Start = Dot;
    while (Start != std::string_view::npos) {
      ++Start;
      const auto End = Qualifier.find('.', Start);
      const auto [Owner, Member] =
          FindMember(Doc, Type, Qualifier.substr(Start, End - Start));
      if (!Member)
        return {};
      Type = Dereference(Member->Type);
      if (Type.find('.') == std::string::npos && !Owner->Module.empty())
        Type = Owner->Module + "." + Type;
      Start = End;
    }
    return Type;
  }

  std::pair<const Document *, const Symbol *> Resolve(const URIForFile &Uri,
                                                      Position Position) const {
    const auto It = Documents.find(Uri.file().str());
    if (It == Documents.end())
      return {};
    const auto &Doc = It->second;
    const auto Offset = OffsetAt(Doc.Parsed.source, Position);
    std::size_t TokenIndex = 0;
    const auto *Token = TokenAt(Doc, Offset, &TokenIndex);
    if (!Token)
      return {};
    const auto [Qualifier, Name] = QualifiedName(Doc, TokenIndex);
    if (!Qualifier.empty()) {
      const auto Type = ReceiverType(Doc, Qualifier, Offset);
      if (!Type.empty())
        return FindMember(Doc, Type, Name);
    }
    if (Qualifier.empty()) {
      if (const auto *Local = Doc.FindVisible(Name, Offset))
        return {&Doc, Local};
    }
    const Document *FallbackDoc = nullptr;
    const Symbol *Fallback = nullptr;
    const Document *ImportedDoc = nullptr;
    const Symbol *Imported = nullptr;
    for (const auto &[Path, CandidateDoc] : Documents) {
      for (const auto &Candidate : CandidateDoc.Symbols) {
        if (Candidate.Name != Name || !Candidate.Owner.empty() ||
            Candidate.Kind == SymbolType::Variable ||
            Candidate.Kind == SymbolType::Parameter)
          continue;
        if (!Qualifier.empty() && Candidate.Module != Qualifier)
          continue;
        if (&CandidateDoc == &Doc)
          return {&CandidateDoc, &Candidate};
        if (!Candidate.Public)
          continue;
        // A name from an imported module beats any other public symbol now
        // that dependency sources are indexed too.
        if (Qualifier.empty() && !Imported &&
            std::any_of(Doc.ImportRefs.begin(), Doc.ImportRefs.end(),
                        [&](const Document::ImportRef &Ref) {
                          return Ref.Module == Candidate.Module;
                        })) {
          ImportedDoc = &CandidateDoc;
          Imported = &Candidate;
        }
        if (!Fallback) {
          FallbackDoc = &CandidateDoc;
          Fallback = &Candidate;
        }
      }
    }
    if (Imported)
      return {ImportedDoc, Imported};
    return {FallbackDoc, Fallback};
  }

  // True when `Name` names a declaration of this document or of a module it
  // imports, which is what a C interop declaration must not shadow.
  bool HasImportedSymbol(const Document &Doc, std::string_view Name) const {
    for (const auto &Candidate : Doc.Symbols)
      if (Candidate.Name == Name && Candidate.Owner.empty())
        return true;
    for (const auto &[Path, Other] : Documents) {
      if (&Other == &Doc ||
          !std::any_of(Doc.ImportRefs.begin(), Doc.ImportRefs.end(),
                       [&](const Document::ImportRef &Ref) {
                         return Ref.Module == Other.Module;
                       }))
        continue;
      for (const auto &Candidate : Other.Symbols)
        if (Candidate.Name == Name && Candidate.Owner.empty() &&
            Candidate.Public)
          return true;
    }
    return false;
  }

  // The module named by `module X;` or `import X;` at this offset.
  std::string ModuleAt(const Document &Doc, std::size_t Offset) const {
    using K = lex::TokenKind;
    for (const auto &Node : Doc.Parsed.root->children) {
      if (Node->kind != K::ast_module_decl && Node->kind != K::ast_import)
        continue;
      if (Offset < Node->Loc.Offset || Offset > Node->Loc.End())
        continue;
      if (Node->text == "c" || Node->text == "c.*")
        return {};
      return Node->text.ends_with(".*")
                 ? Node->text.substr(0, Node->text.size() - 2)
                 : Node->text;
    }
    return {};
  }

  const Document *FindModule(std::string_view Name) const {
    for (const auto &[Path, Candidate] : Documents)
      if (!Name.empty() && Candidate.Module == Name)
        return &Candidate;
    return nullptr;
  }

  // The `import c "header.h"` string literal at this offset.
  std::optional<std::string> CHeaderAt(const Document &Doc,
                                       std::size_t Offset) const {
    for (const auto &[Path, Span] : Doc.CHeaders)
      if (Offset >= Span.Offset && Offset <= Span.Offset + Span.Length)
        return Path;
    return std::nullopt;
  }

  // The column of `Name` in `Line` when it appears as a whole word.
  static std::optional<std::size_t> WordColumn(std::string_view Line,
                                               std::string_view Name) {
    const auto IsWord = [](char Character) {
      return std::isalnum(static_cast<unsigned char>(Character)) ||
             Character == '_';
    };
    for (auto Position = Line.find(Name); Position != std::string_view::npos;
         Position = Line.find(Name, Position + 1)) {
      const bool Start = Position == 0 || !IsWord(Line[Position - 1]);
      const auto End = Position + Name.size();
      const bool Stop = End >= Line.size() || !IsWord(Line[End]);
      if (Start && Stop)
        return Position;
    }
    return std::nullopt;
  }

  // A C declaration found by scanning an `import c` header.
  struct CDeclaration {
    std::string Header;
    Position Start;
    Position End;
    std::string Line;
  };

  // C declarations live in the headers `import c` pulls in, so search them
  // textually, following nested includes a few levels deep.
  std::optional<CDeclaration> SearchHeader(const std::string &Header,
                                           std::string_view Name, int Depth,
                                           std::set<std::string> &Seen) const {
    if (Depth > 3 || !Seen.insert(Header).second)
      return std::nullopt;
    std::ifstream Input(Header, std::ios::binary);
    if (!Input)
      return std::nullopt;
    const std::string Text((std::istreambuf_iterator<char>(Input)), {});
    std::vector<std::pair<std::string, bool>> Includes;
    std::optional<std::size_t> Preprocessor;
    std::size_t Start = 0;
    while (Start <= Text.size()) {
      auto End = Text.find('\n', Start);
      if (End == std::string::npos)
        End = Text.size();
      const std::string_view Line(Text.data() + Start, End - Start);
      if (const auto Column = WordColumn(Line, Name)) {
        const auto First = Line.find_first_not_of(" \t");
        if (First != std::string_view::npos && Line[First] == '#') {
          // A `#define` may declare the name, but a real declaration wins.
          if (!Preprocessor)
            Preprocessor = Start + *Column;
        } else {
          const auto Target = Start + *Column;
          auto Trimmed = Line;
          const auto Left = Trimmed.find_first_not_of(" \t");
          const auto Right = Trimmed.find_last_not_of(" \t\r");
          if (Left != std::string_view::npos && Right != std::string_view::npos)
            Trimmed = Trimmed.substr(Left, Right - Left + 1);
          return CDeclaration{Header, PositionAt(Text, Target),
                              PositionAt(Text, Target + Name.size()),
                              std::string(Trimmed)};
        }
      }
      const auto Include = Line.find("#include");
      if (Include != std::string_view::npos) {
        const auto Open = Line.find_first_of("\"<", Include + 8);
        if (Open != std::string_view::npos) {
          const char Close = Line[Open] == '"' ? '"' : '>';
          const auto Closing = Line.find(Close, Open + 1);
          if (Closing != std::string_view::npos)
            Includes.emplace_back(
                std::string(Line.substr(Open + 1, Closing - Open - 1)),
                Line[Open] == '"');
        }
      }
      if (End == Text.size())
        break;
      Start = End + 1;
    }
    const auto Directory = std::filesystem::path(Header).parent_path();
    static const char *SystemPaths[] = {
        "/usr/local/include", "/usr/include/x86_64-linux-gnu", "/usr/include"};
    std::error_code Error;
    for (const auto &[Include, Quoted] : Includes) {
      std::vector<std::filesystem::path> Candidates{Directory / Include};
      if (!Quoted)
        for (const char *SystemPath : SystemPaths)
          Candidates.emplace_back(std::filesystem::path(SystemPath) / Include);
      for (const auto &Candidate : Candidates) {
        if (!std::filesystem::exists(Candidate, Error)) {
          Error.clear();
          continue;
        }
        if (auto Found =
                SearchHeader(Candidate.string(), Name, Depth + 1, Seen))
          return Found;
      }
    }
    if (Preprocessor) {
      const auto Target = *Preprocessor;
      return CDeclaration{Header,
                          PositionAt(Text, Target),
                          PositionAt(Text, Target + Name.size()),
                          {}};
    }
    return std::nullopt;
  }

  std::optional<CDeclaration> FindCDeclaration(const Document &Doc,
                                               std::string_view Name) const {
    std::set<std::string> Seen;
    for (const auto &[Header, Span] : Doc.CHeaders)
      if (auto Found = SearchHeader(Header, Name, 0, Seen))
        return Found;
    return std::nullopt;
  }

  static std::optional<Location> LocationOf(const CDeclaration &Declaration) {
    auto Uri = URIForFile::fromFile(Declaration.Header);
    if (!Uri) {
      consumeError(Uri.takeError());
      return std::nullopt;
    }
    return Location{*Uri, Range{Declaration.Start, Declaration.End}};
  }

  // Everywhere a target is defined: a Kelyra symbol, a module, or a C
  // declaration from an `import c` header.
  std::optional<Location>
  ResolveLocation(const TextDocumentPositionParams &Params) const {
    const auto It = Documents.find(Params.textDocument.uri.file().str());
    if (It == Documents.end())
      return std::nullopt;
    const auto &Doc = It->second;
    const auto Offset = OffsetAt(Doc.Parsed.source, Params.position);
    if (const auto Module = ModuleAt(Doc, Offset); !Module.empty())
      if (const auto *Target = FindModule(Module))
        return Location{Target->Uri, Target->ToRange(Target->ModuleSpan)};
    if (const auto Header = CHeaderAt(Doc, Offset))
      if (auto Uri = URIForFile::fromFile(*Header)) {
        const auto Start = PositionAt(Doc.Parsed.source, Offset);
        return Location{*Uri, Range{Start, Start}};
      } else {
        consumeError(Uri.takeError());
      }
    const auto [TargetDoc, Symbol] =
        Resolve(Params.textDocument.uri, Params.position);
    if (Symbol && (Symbol->Kind == SymbolType::Variable ||
                   Symbol->Kind == SymbolType::Parameter || TargetDoc == &Doc ||
                   HasImportedSymbol(Doc, Symbol->Name)))
      return Location{TargetDoc->Uri, TargetDoc->ToRange(Symbol->Definition)};
    std::size_t TokenIndex = 0;
    if (const auto *Token = TokenAt(Doc, Offset, &TokenIndex)) {
      const auto [Qualifier, Name] = QualifiedName(Doc, TokenIndex);
      // `c.printf(...)` and unprefixed names after `import c.*` come from the
      // C headers, so look there before giving up on the name.
      if ((Qualifier == "c" || (Qualifier.empty() && Doc.CImportWildcard)) &&
          !(Qualifier.empty() && Doc.FindVisible(Name, Offset)))
        if (auto Found = FindCDeclaration(Doc, Name))
          if (auto Location = LocationOf(*Found))
            return Location;
    }
    if (Symbol)
      return Location{TargetDoc->Uri, TargetDoc->ToRange(Symbol->Definition)};
    return std::nullopt;
  }

  // True when `Other` imports `Module`, which is what makes an unqualified
  // reference to it legal in that document.
  static bool Imports(const Document &Other, std::string_view Module) {
    return std::any_of(
        Other.ImportRefs.begin(), Other.ImportRefs.end(),
        [&](const Document::ImportRef &Ref) { return Ref.Module == Module; });
  }

  std::vector<Location> ReferencesAt(const ReferenceParams &Params) const {
    const auto It = Documents.find(Params.textDocument.uri.file().str());
    if (It == Documents.end())
      return {};
    const auto &Doc = It->second;
    const auto Offset = OffsetAt(Doc.Parsed.source, Params.position);
    std::vector<Location> Result;
    // A module is referenced by every import that names it.
    if (const auto Module = ModuleAt(Doc, Offset); !Module.empty()) {
      for (const auto &[Path, Other] : Documents)
        for (const auto &Ref : Other.ImportRefs)
          if (Ref.Module == Module)
            Result.push_back({Other.Uri, Other.ToRange(Ref.Extent)});
      return Result;
    }
    const auto [TargetDoc, Symbol] =
        Resolve(Params.textDocument.uri, Params.position);
    if (!Symbol)
      return {};
    for (const auto &[Path, Other] : Documents) {
      for (std::size_t Index = 0; Index < Other.Parsed.tokens.size(); ++Index) {
        const auto &Token = Other.Parsed.tokens[Index];
        if (Token.kind != lex::TokenKind::name ||
            Other.Spelling(Token) != Symbol->Name)
          continue;
        const bool Declaration = &Other == TargetDoc &&
                                 Token.Loc.Offset == Symbol->Definition.Offset;
        if (Declaration && !Params.context.includeDeclaration)
          continue;
        if (Symbol->Kind == SymbolType::Variable ||
            Symbol->Kind == SymbolType::Parameter) {
          // Locals only exist inside their own scope of their own document.
          if (&Other != TargetDoc || Token.Loc.Offset < Symbol->Scope.Offset ||
              Token.Loc.Offset > Symbol->Scope.Offset + Symbol->Scope.Length)
            continue;
        } else {
          const auto [Qualifier, Name] = QualifiedName(Other, Index);
          if (!Qualifier.empty()) {
            if (Symbol->Owner.empty()) {
              if (Qualifier != Symbol->Module)
                continue;
            } else if (Qualifier == "this") {
              // `this.field` belongs to the class declared in this document.
              if (&Other != TargetDoc)
                continue;
            } else {
              const auto Type =
                  ReceiverType(Other, Qualifier, Token.Loc.Offset);
              const auto Expected =
                  Symbol->Owner.find('.') == std::string::npos
                      ? TargetDoc->Module + "." + Symbol->Owner
                      : Symbol->Owner;
              // A receiver keeps its bare type name inside the owning file.
              if (Type != Expected &&
                  !(&Other == TargetDoc && Type == Symbol->Owner))
                continue;
            }
          } else {
            if (&Other != TargetDoc && !Imports(Other, Symbol->Module))
              continue;
            // A local of the same name shadows the symbol at this offset.
            if (const auto *Local = Other.FindVisible(Name, Token.Loc.Offset);
                Local && Local != Symbol &&
                (Local->Kind == SymbolType::Variable ||
                 Local->Kind == SymbolType::Parameter))
              continue;
            // A parameter that shadows the symbol is declared right here.
            const bool DeclaresOther = std::any_of(
                Other.Symbols.begin(), Other.Symbols.end(),
                [&](const auto &Candidate) {
                  return &Candidate != Symbol && Candidate.Name == Name &&
                         Candidate.Definition.Offset == Token.Loc.Offset &&
                         (Candidate.Kind == SymbolType::Variable ||
                          Candidate.Kind == SymbolType::Parameter);
                });
            if (DeclaresOther)
              continue;
          }
        }
        Result.push_back(
            {Other.Uri, Other.ToRange({Token.Loc.Offset, Token.Loc.Len})});
      }
    }
    return Result;
  }

public:
  void LoadWorkspace(const InitializeParams &Params) {
    std::string Root;
    if (Params.rootUri) {
      auto Uri = URIForFile::fromURI(*Params.rootUri);
      if (Uri)
        Root = Uri->file().str();
      else
        consumeError(Uri.takeError());
    } else if (Params.rootPath) {
      Root = *Params.rootPath;
    }
    if (Root.empty())
      return;
    // Members, cached clones, and local path dependencies all contribute
    // modules the compiler can see, so index everything the manifests reach.
    std::set<std::string> Visited;
    LoadProject(WorkspaceRoot(Root), Visited);
    LoadProject(Root, Visited);
  }

  std::vector<Diagnostic> Open(const TextDocumentItem &Item) {
    auto &Doc = Set(Item.uri, Item.text, Item.version);
    std::vector<Diagnostic> Result;
    for (const auto &Entry : Doc.Parsed.diagnostics) {
      Diagnostic Diagnostic;
      const auto Length = std::max<std::size_t>(Entry.Loc.Len, 1);
      Diagnostic.range = Doc.ToRange({Entry.Loc.Offset, Length});
      Diagnostic.severity = DiagnosticSeverity::Error;
      Diagnostic.source = "kelyra";
      Diagnostic.message = std::string(lex::GetDiagnosticInfo(Entry.Kind).Msg);
      Result.push_back(std::move(Diagnostic));
    }
    return Result;
  }

  std::vector<Diagnostic> Change(const DidChangeTextDocumentParams &Params) {
    auto *Doc = Get(Params.textDocument.uri);
    if (!Doc)
      return {};
    auto Source = Doc->Parsed.source;
    if (llvm::failed(TextDocumentContentChangeEvent::applyTo(
            Params.contentChanges, Source)))
      return {};
    TextDocumentItem Item{Params.textDocument.uri, "kelyra", std::move(Source),
                          Params.textDocument.version};
    return Open(Item);
  }

  void Close(const URIForFile &Uri) {
    std::ifstream Input(Uri.file().str(), std::ios::binary);
    if (!Input) {
      Documents.erase(Uri.file().str());
      return;
    }
    Set(Uri, std::string((std::istreambuf_iterator<char>(Input)), {}), 0);
  }

  std::optional<Hover> HoverAt(const TextDocumentPositionParams &Params) const {
    const auto It = Documents.find(Params.textDocument.uri.file().str());
    if (It != Documents.end()) {
      const auto &Doc = It->second;
      const auto Offset = OffsetAt(Doc.Parsed.source, Params.position);
      // Module paths and C headers are not symbols, so describe them directly.
      if (const auto Module = ModuleAt(Doc, Offset); !Module.empty())
        if (const auto *Target = FindModule(Module)) {
          Hover Result(Doc.ToRange({Offset, 1}));
          Result.contents.value =
              "```kelyra\nmodule " + Module + "\n```\n\n" + Target->Path;
          return Result;
        }
      if (const auto Header = CHeaderAt(Doc, Offset)) {
        Hover Result(Doc.ToRange({Offset, 1}));
        Result.contents.value = "C header `" + *Header + "`";
        return Result;
      }
      std::size_t TokenIndex = 0;
      if (const auto *Token = TokenAt(Doc, Offset, &TokenIndex)) {
        const auto [Qualifier, Name] = QualifiedName(Doc, TokenIndex);
        if ((Qualifier == "c" || (Qualifier.empty() && Doc.CImportWildcard)) &&
            !(Qualifier.empty() && Doc.FindVisible(Name, Offset))) {
          if (auto Found = FindCDeclaration(Doc, Name)) {
            Hover Result(Doc.ToRange({Offset, 1}));
            Result.contents.value =
                Found->Line.empty()
                    ? "```c\n#define " + Name + "\n```\n\nDeclared in `" +
                          Found->Header + "`"
                    : "```c\n" + Found->Line + "\n```\n\nDeclared in `" +
                          Found->Header + "`";
            return Result;
          }
        }
      }
    }
    const auto [Doc, Found] = Resolve(Params.textDocument.uri, Params.position);
    if (!Doc || !Found)
      return std::nullopt;
    Hover Result(Doc->ToRange(Found->Definition));
    Result.contents.value = "```kelyra\n" + Found->Detail + "\n```";
    if (!Found->Documentation.empty())
      Result.contents.value += "\n\n" + Found->Documentation;
    return Result;
  }

  std::vector<Location>
  DefinitionAt(const TextDocumentPositionParams &Params) const {
    if (auto Found = ResolveLocation(Params))
      return {*Found};
    return {};
  }

  std::vector<Location> References(const ReferenceParams &Params) const {
    return ReferencesAt(Params);
  }

  std::optional<SignatureHelp>
  SignatureAt(const TextDocumentPositionParams &Params) const {
    const auto It = Documents.find(Params.textDocument.uri.file().str());
    if (It == Documents.end())
      return std::nullopt;
    const auto &Doc = It->second;
    const auto &Tokens = Doc.Parsed.tokens;
    const auto Offset = OffsetAt(Doc.Parsed.source, Params.position);
    std::size_t Cursor = Tokens.size();
    for (std::size_t I = 0; I < Tokens.size(); ++I)
      if (Tokens[I].Loc.Offset >= Offset) {
        Cursor = I;
        break;
      }
    // Find the innermost call parenthesis that is still open at the cursor.
    std::size_t Open = Tokens.size();
    int Depth = 0;
    for (std::size_t I = Cursor; I-- > 0;) {
      const auto Kind = Tokens[I].kind;
      if (Kind == lex::TokenKind::punc_right_paren) {
        ++Depth;
        continue;
      }
      if (Kind != lex::TokenKind::punc_left_paren)
        continue;
      if (Depth == 0) {
        Open = I;
        break;
      }
      --Depth;
    }
    if (Open == Tokens.size() || Open == 0)
      return std::nullopt;
    const auto &Callee = Tokens[Open - 1];
    if (Callee.kind != lex::TokenKind::name)
      return std::nullopt;
    // The active parameter is the count of top-level commas before the cursor.
    int Active = 0;
    int Nested = 0;
    for (std::size_t I = Open + 1; I < Cursor && I < Tokens.size(); ++I) {
      switch (Tokens[I].kind) {
      case lex::TokenKind::punc_left_paren:
      case lex::TokenKind::punc_left_bracket:
      case lex::TokenKind::punc_left_brace:
        ++Nested;
        break;
      case lex::TokenKind::punc_right_paren:
      case lex::TokenKind::punc_right_bracket:
      case lex::TokenKind::punc_right_brace:
        --Nested;
        break;
      case lex::TokenKind::punc_comma:
        if (Nested == 0)
          ++Active;
        break;
      default:
        break;
      }
    }
    auto [Owner, Found] =
        Resolve(Params.textDocument.uri,
                PositionAt(Doc.Parsed.source, Callee.Loc.Offset));
    if (!Found)
      return std::nullopt;
    if (Found->Kind == SymbolType::Class) {
      // A construction call shows the class's constructor signature.
      const Symbol *Constructor = nullptr;
      for (const auto &Candidate : Owner->Symbols)
        if (Candidate.Kind == SymbolType::Method && Candidate.Name == "init" &&
            Candidate.Owner == Found->Name)
          Constructor = &Candidate;
      if (!Constructor)
        return std::nullopt;
      Found = Constructor;
    }
    if (Found->Kind != SymbolType::Function &&
        Found->Kind != SymbolType::Method)
      return std::nullopt;
    const auto &Detail = Found->Detail;
    const auto OpenParen = Detail.find('(');
    const auto CloseParen = Detail.rfind(')');
    if (OpenParen == std::string::npos || CloseParen == std::string::npos ||
        CloseParen < OpenParen)
      return std::nullopt;
    SignatureInformation Information;
    Information.label = Detail;
    std::size_t Start = OpenParen + 1;
    int ParameterDepth = 0;
    for (std::size_t I = OpenParen + 1; I <= CloseParen; ++I) {
      const char Character = I < CloseParen ? Detail[I] : ',';
      if (Character == '(' || Character == '[') {
        ++ParameterDepth;
        continue;
      }
      if (Character == ')' || Character == ']') {
        --ParameterDepth;
        continue;
      }
      if (Character != ',' || ParameterDepth != 0)
        continue;
      std::string_view Text = std::string_view(Detail).substr(Start, I - Start);
      while (!Text.empty() &&
             std::isspace(static_cast<unsigned char>(Text.front())))
        Text.remove_prefix(1);
      while (!Text.empty() &&
             std::isspace(static_cast<unsigned char>(Text.back())))
        Text.remove_suffix(1);
      if (!Text.empty()) {
        ParameterInformation Parameter;
        Parameter.labelString = std::string(Text);
        const auto LabelStart =
            static_cast<unsigned>(Text.data() - Detail.data());
        Parameter.labelOffsets = std::make_pair(
            LabelStart, LabelStart + static_cast<unsigned>(Text.size()));
        Information.parameters.push_back(std::move(Parameter));
      }
      Start = I + 1;
    }
    SignatureHelp Help;
    const int Count = static_cast<int>(Information.parameters.size());
    Help.activeParameter = Count == 0 ? 0 : std::min(Active, Count - 1);
    Help.signatures.push_back(std::move(Information));
    return Help;
  }

  std::vector<DocumentSymbol>
  DocumentSymbols(const DocumentSymbolParams &Params) const {
    std::vector<DocumentSymbol> Result;
    const auto It = Documents.find(Params.textDocument.uri.file().str());
    if (It == Documents.end() || !It->second.Parsed.root)
      return Result;
    const auto &Doc = It->second;
    std::map<std::size_t, const Symbol *> Details;
    for (const auto &Symbol : Doc.Symbols)
      Details.emplace(Symbol.Definition.Offset, &Symbol);
    const auto Make = [&](const lex::Node &Node, SymbolKind Kind,
                          lex::TokenKind Keyword) {
      const auto Definition = Doc.FindDeclaration(Node, Keyword);
      DocumentSymbol Symbol(Node.text, Kind,
                            Doc.ToRange({Node.Loc.Offset, Node.Loc.Len}),
                            Doc.ToRange(Definition));
      if (const auto Found = Details.find(Definition.Offset);
          Found != Details.end())
        Symbol.detail = Found->second->Detail;
      return Symbol;
    };
    for (const auto &Child : Doc.Parsed.root->children) {
      if (Child->kind == lex::TokenKind::ast_function) {
        Result.push_back(
            Make(*Child, SymbolKind::Function, lex::TokenKind::keyword_fn));
      } else if (Child->kind == lex::TokenKind::ast_class) {
        auto Class =
            Make(*Child, SymbolKind::Class, lex::TokenKind::keyword_class);
        for (const auto &Member : Child->children) {
          if (Member->kind == lex::TokenKind::ast_field)
            Class.children.push_back(
                Make(*Member, SymbolKind::Field, lex::TokenKind::name));
          else if (Member->kind == lex::TokenKind::ast_function)
            Class.children.push_back(
                Make(*Member, SymbolKind::Method, lex::TokenKind::keyword_fn));
          else if (Member->kind == lex::TokenKind::ast_constructor)
            Class.children.push_back(
                Make(*Member, SymbolKind::Constructor, lex::TokenKind::name));
          else if (Member->kind == lex::TokenKind::ast_destructor)
            Class.children.push_back(
                Make(*Member, SymbolKind::Method, lex::TokenKind::name));
        }
        Result.push_back(std::move(Class));
      }
    }
    return Result;
  }

  CompletionList Complete(const CompletionParams &Params) const {
    CompletionList Result;
    const auto It = Documents.find(Params.textDocument.uri.file().str());
    if (It == Documents.end())
      return Result;
    const auto &Current = It->second;
    const auto Offset = OffsetAt(Current.Parsed.source, Params.position);
    std::set<std::string> Seen;
    auto Add = [&](StringRef Label, CompletionItemKind Kind, StringRef Detail) {
      if (!Seen.insert(Label.str()).second)
        return;
      CompletionItem Item(Label, Kind);
      Item.detail = Detail.str();
      Result.items.push_back(std::move(Item));
    };
    std::size_t Start = Offset;
    while (Start > 0) {
      const char C = Current.Parsed.source[Start - 1];
      if (!(std::isalnum(static_cast<unsigned char>(C)) || C == '_' ||
            C == '.'))
        break;
      --Start;
    }
    const auto Prefix =
        std::string_view(Current.Parsed.source).substr(Start, Offset - Start);
    const auto Dot = Prefix.rfind('.');
    if (Dot != std::string_view::npos) {
      const auto Type = ReceiverType(Current, Prefix.substr(0, Dot), Offset);
      if (!Type.empty()) {
        for (const auto &[Path, Doc] : Documents)
          for (const auto &Member : Doc.Symbols) {
            if (Member.Owner.empty() || Member.Name == "init" ||
                Member.Name == "deinit")
              continue;
            const auto [Owner, Found] = FindMember(Current, Type, Member.Name);
            if (Found == &Member)
              Add(Member.Name,
                  Member.Kind == SymbolType::Field ? CompletionItemKind::Field
                                                   : CompletionItemKind::Method,
                  Member.Detail);
          }
        return Result;
      }
    }
    for (const auto &Symbol : Current.Symbols) {
      if (!Symbol.Owner.empty() && Offset >= Symbol.Scope.Offset &&
          Offset <= Symbol.Scope.Offset + Symbol.Scope.Length &&
          Symbol.Name != "init" && Symbol.Name != "deinit")
        Add(Symbol.Name,
            Symbol.Kind == SymbolType::Field ? CompletionItemKind::Field
                                             : CompletionItemKind::Method,
            Symbol.Detail);
      if ((Symbol.Kind == SymbolType::Variable ||
           Symbol.Kind == SymbolType::Parameter) &&
          Symbol.Definition.Offset <= Offset && Offset >= Symbol.Scope.Offset &&
          Offset <= Symbol.Scope.Offset + Symbol.Scope.Length)
        Add(Symbol.Name, CompletionItemKind::Variable, Symbol.Detail);
    }
    for (const auto &[Path, Doc] : Documents)
      for (const auto &Symbol : Doc.Symbols) {
        if (&Doc != &Current && !Symbol.Public)
          continue;
        if (Symbol.Kind == SymbolType::Function)
          Add(Symbol.Name, CompletionItemKind::Function, Symbol.Detail);
        else if (Symbol.Kind == SymbolType::Class)
          Add(Symbol.Name, CompletionItemKind::Class, Symbol.Detail);
      }
    for (const auto Keyword :
         {"fn", "class", "init", "deinit", "this", "let", "pub", "if", "else",
          "while", "return", "break", "continue", "asm", "true", "false",
          "module", "import"})
      Add(Keyword, CompletionItemKind::Keyword, "Kelyra keyword");
    for (const auto Type :
         {"i8", "i16", "i32", "i64", "i128", "isize", "u8", "u16", "u32", "u64",
          "u128", "usize", "f32", "f64", "f128", "bool", "char", "void"})
      Add(Type, CompletionItemKind::Class, "Kelyra type");
    return Result;
  }
};

class LSPServer {
  Server &Language;
  bool Shutdown = false;

public:
  OutgoingNotification<PublishDiagnosticsParams> PublishDiagnostics;

  explicit LSPServer(Server &Language) : Language(Language) {}

  void Initialize(const InitializeParams &Params, Callback<json::Value> Reply) {
    Language.LoadWorkspace(Params);
    json::Object Capabilities{
        {
            "textDocumentSync",
            json::Object{
                {"openClose", true},
                {"change", static_cast<int>(TextDocumentSyncKind::Full)}},
        },
        {"hoverProvider", true},
        {"definitionProvider", true},
        {"referencesProvider", true},
        {"documentSymbolProvider", true},
        {"signatureHelpProvider",
         json::Object{{"triggerCharacters", json::Array{"(", ","}}}},
        {"completionProvider",
         json::Object{{"resolveProvider", false},
                      {"triggerCharacters", json::Array{"."}}}},
    };
    Reply(json::Object{{"serverInfo", json::Object{{"name", "kelyra-ls"},
                                                   {"version", "0.1.0"}}},
                       {"capabilities", std::move(Capabilities)}});
  }

  void Initialized(const InitializedParams &) {}
  void ShutdownRequest(const NoParams &, Callback<std::nullptr_t> Reply) {
    Shutdown = true;
    Reply(nullptr);
  }
  void DidOpen(const DidOpenTextDocumentParams &Params) {
    PublishDiagnosticsParams Diagnostics(Params.textDocument.uri,
                                         Params.textDocument.version);
    Diagnostics.diagnostics = Language.Open(Params.textDocument);
    PublishDiagnostics(Diagnostics);
  }
  void DidChange(const DidChangeTextDocumentParams &Params) {
    PublishDiagnosticsParams Diagnostics(Params.textDocument.uri,
                                         Params.textDocument.version);
    Diagnostics.diagnostics = Language.Change(Params);
    PublishDiagnostics(Diagnostics);
  }
  void DidClose(const DidCloseTextDocumentParams &Params) {
    Language.Close(Params.textDocument.uri);
    PublishDiagnostics(PublishDiagnosticsParams(Params.textDocument.uri, 0));
  }
  void HoverRequest(const TextDocumentPositionParams &Params,
                    Callback<std::optional<Hover>> Reply) {
    Reply(Language.HoverAt(Params));
  }
  void DefinitionRequest(const TextDocumentPositionParams &Params,
                         Callback<std::vector<Location>> Reply) {
    Reply(Language.DefinitionAt(Params));
  }

  void ReferencesRequest(const ReferenceParams &Params,
                         Callback<std::vector<Location>> Reply) {
    Reply(Language.References(Params));
  }
  void SignatureHelpRequest(const TextDocumentPositionParams &Params,
                            Callback<std::optional<SignatureHelp>> Reply) {
    Reply(Language.SignatureAt(Params));
  }
  void DocumentSymbolRequest(const DocumentSymbolParams &Params,
                             Callback<std::vector<DocumentSymbol>> Reply) {
    Reply(Language.DocumentSymbols(Params));
  }
  void CompletionRequest(const CompletionParams &Params,
                         Callback<CompletionList> Reply) {
    Reply(Language.Complete(Params));
  }
  bool WasShutdown() const { return Shutdown; }
};
} // namespace

int main(int Argc, char **Argv) {
  cl::ParseCommandLineOptions(Argc, Argv, "Kelyra language server\n");
  Logger::setLogLevel(Logger::Level::Error);
  sys::ChangeStdinToBinary();
  JSONTransport Transport(stdin, outs(), JSONStreamStyle::Standard, false);
  Server Language;
  LSPServer Server(Language);
  MessageHandler Handler(Transport);
  Handler.method("initialize", &Server, &LSPServer::Initialize);
  Handler.notification("initialized", &Server, &LSPServer::Initialized);
  Handler.method("shutdown", &Server, &LSPServer::ShutdownRequest);
  Handler.notification("textDocument/didOpen", &Server, &LSPServer::DidOpen);
  Handler.notification("textDocument/didChange", &Server,
                       &LSPServer::DidChange);
  Handler.notification("textDocument/didClose", &Server, &LSPServer::DidClose);
  Handler.method("textDocument/hover", &Server, &LSPServer::HoverRequest);
  Handler.method("textDocument/definition", &Server,
                 &LSPServer::DefinitionRequest);
  Handler.method("textDocument/references", &Server,
                 &LSPServer::ReferencesRequest);
  Handler.method("textDocument/signatureHelp", &Server,
                 &LSPServer::SignatureHelpRequest);
  Handler.method("textDocument/documentSymbol", &Server,
                 &LSPServer::DocumentSymbolRequest);
  Handler.method("textDocument/completion", &Server,
                 &LSPServer::CompletionRequest);
  Server.PublishDiagnostics =
      Handler.outgoingNotification<PublishDiagnosticsParams>(
          "textDocument/publishDiagnostics");
  if (auto Error = Transport.run(Handler)) {
    Logger::error("Transport error: {0}", Error);
    consumeError(std::move(Error));
    return 1;
  }
  return Server.WasShutdown() ? 0 : 1;
}
