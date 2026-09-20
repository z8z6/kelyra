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

enum class SymbolType { Function, Struct, Field, Parameter, Variable, Module };

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
         Node.kind == lex::TokenKind::ast_array_type;
}

std::string TypeName(const lex::Node &Node) {
  if (Node.kind == lex::TokenKind::ast_type)
    return Node.text;
  if (Node.children.empty())
    return {};
  auto Result = TypeName(*Node.children.front());
  if (Node.kind == lex::TokenKind::ast_pointer_type)
    return Result + "*";
  if (Node.kind == lex::TokenKind::ast_array_type)
    return Result + "[" + Node.text + "]";
  return Result;
}

struct Document {
  std::string Path;
  URIForFile Uri;
  int64_t Version = 0;
  lex::ParseResult Parsed;
  std::string Module;
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
      if (Candidate.Name != Name || Candidate.Definition.Offset > Offset ||
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
        if (Candidate.Kind == SymbolType::Function && Candidate.Name == Name)
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
    Symbols.clear();
    if (!Parsed.root)
      return;
    using K = lex::TokenKind;
    for (const auto &Node : Parsed.root->children)
      if (Node->kind == K::ast_module_decl)
        Module = Node->text;

    const Span FileScope{0, Parsed.source.size()};
    for (const auto &Node : Parsed.root->children) {
      if (Node->kind != K::ast_function && Node->kind != K::ast_struct)
        continue;
      const bool IsFunction = Node->kind == K::ast_function;
      const auto Definition = FindDeclaration(
          *Node, IsFunction ? K::keyword_fn : K::keyword_struct);
      bool IsPublic = false;
      std::string ReturnType;
      std::string Detail =
          IsFunction ? "fn " + Node->text + "(" : "struct " + Node->text;
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
      Symbols.push_back({IsFunction ? SymbolType::Function : SymbolType::Struct,
                         Node->text, ReturnType, Detail, Module, Definition,
                         FileScope, IsPublic});
      Symbols.back().Documentation = DocumentationAt(Node->Loc.Offset);
    }

    for (const auto &Node : Parsed.root->children) {
      if (Node->kind != K::ast_function)
        continue;
      const lex::Node *Body = nullptr;
      for (const auto &Child : Node->children)
        if (Child->kind == K::ast_block)
          Body = Child.get();
      if (!Body)
        continue;
      const Span FunctionScope{Body->Loc.Offset, Body->Loc.Len};
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
    }
  }

  Range ToRange(Span SourceSpan) const {
    return {PositionAt(Parsed.source, SourceSpan.Offset),
            PositionAt(Parsed.source, SourceSpan.Offset + SourceSpan.Length)};
  }
};

class Server {
  std::map<std::string, Document> Documents;

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
      if (Token.kind == lex::TokenKind::name && Offset >= Token.Loc.Offset &&
          Offset <= Token.Loc.End()) {
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
           Doc.Parsed.tokens[TokenIndex - 2].kind == lex::TokenKind::name) {
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
    if (Qualifier.empty()) {
      if (const auto *Local = Doc.FindVisible(Name, Offset))
        return {&Doc, Local};
    }
    const Document *FallbackDoc = nullptr;
    const Symbol *Fallback = nullptr;
    for (const auto &[Path, CandidateDoc] : Documents) {
      for (const auto &Candidate : CandidateDoc.Symbols) {
        if (Candidate.Name != Name || Candidate.Kind == SymbolType::Field)
          continue;
        if (!Qualifier.empty() && Candidate.Module != Qualifier)
          continue;
        if (&CandidateDoc == &Doc)
          return {&CandidateDoc, &Candidate};
        if (Candidate.Public && !Fallback) {
          FallbackDoc = &CandidateDoc;
          Fallback = &Candidate;
        }
      }
    }
    return {FallbackDoc, Fallback};
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
    std::error_code Error;
    std::filesystem::recursive_directory_iterator It(
        Root, std::filesystem::directory_options::skip_permission_denied,
        Error);
    for (const std::filesystem::recursive_directory_iterator End; It != End;
         It.increment(Error)) {
      if (Error) {
        Error.clear();
        continue;
      }
      if (It->is_directory()) {
        const auto Name = It->path().filename().string();
        if (Name == ".git" || Name == "build" || Name == ".kelp" ||
            Name == "node_modules")
          It.disable_recursion_pending();
        continue;
      }
      if (It->path().extension() != ".kly")
        continue;
      std::ifstream Input(It->path(), std::ios::binary);
      std::string Source((std::istreambuf_iterator<char>(Input)), {});
      auto Uri = URIForFile::fromFile(
          std::filesystem::absolute(It->path()).lexically_normal().string());
      if (Input && Uri)
        Set(*Uri, std::move(Source), 0);
      else if (!Uri)
        consumeError(Uri.takeError());
    }
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
    const auto [Doc, Found] = Resolve(Params.textDocument.uri, Params.position);
    if (!Doc || !Found)
      return {};
    return {{Doc->Uri, Doc->ToRange(Found->Definition)}};
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
    for (const auto &Symbol : Current.Symbols) {
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
        else if (Symbol.Kind == SymbolType::Struct)
          Add(Symbol.Name, CompletionItemKind::Struct, Symbol.Detail);
      }
    for (const auto Keyword :
         {"fn", "struct", "let", "pub", "if", "else", "while", "return",
          "break", "continue", "asm", "true", "false", "module", "import"})
      Add(Keyword, CompletionItemKind::Keyword, "Kelyra keyword");
    for (const auto Type :
         {"i8", "i16", "i32", "i64", "i128", "isize", "u8", "u16", "u32", "u64",
          "u128", "usize", "f32", "f64", "f128", "bool", "char"})
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
