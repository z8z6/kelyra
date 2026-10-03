#include "Front/Lexer/Formatter.h"
#include "Front/Parser/Parser.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace {
void Usage(std::ostream &Output) {
  Output << "usage: kelyra-format [-i|--check] [--module-path=dir] <file>...\n"
            "  -i       format files in place\n"
            "  --check  fail if any file needs formatting\n";
}

bool IsPublic(const kelyra::lex::Node &Node) {
  return std::any_of(Node.children.begin(), Node.children.end(),
                     [](const auto &Part) {
                       return Part->kind == kelyra::lex::NodeKind::ast_public;
                     });
}

std::optional<std::filesystem::path>
FindModule(std::string Name, const std::filesystem::path &File,
           const std::vector<std::filesystem::path> &ModulePaths) {
  std::replace(Name.begin(), Name.end(), '.', '/');
  if (Name == "c")
    Name = "std/c";
  const auto Relative = std::filesystem::path(Name + ".kly");
  const auto Directory =
      Relative.parent_path() / Relative.stem() / Relative.filename();
  std::vector<std::filesystem::path> Roots = ModulePaths;
  for (auto Parent = std::filesystem::absolute(File).parent_path();
       !Parent.empty(); Parent = Parent.parent_path()) {
    Roots.push_back(Parent);
    if (Parent == Parent.parent_path())
      break;
  }
  Roots.emplace_back(KELYRA_STDLIB_SOURCE_DIR);
  for (const auto &Root : Roots)
    for (const auto &Candidate : {Root / Relative, Root / Directory}) {
      std::error_code Error;
      if (std::filesystem::is_regular_file(Candidate, Error))
        return Candidate;
    }
  return std::nullopt;
}

kelyra::lex::FormatSymbols
CollectSymbols(const kelyra::lex::ParseResult &Parsed,
               const std::filesystem::path &File,
               const std::vector<std::filesystem::path> &ModulePaths) {
  using K = kelyra::lex::NodeKind;
  kelyra::lex::FormatSymbols Result;
  Result.Complete = true;
  for (const auto &Child : Parsed.root->children)
    if (Child->kind == K::ast_module_decl)
      Result.CurrentModule = Child->text;
    else if (Child->kind == K::ast_class || Child->kind == K::ast_function ||
             Child->kind == K::ast_enum || Child->kind == K::ast_alias_decl ||
             Child->kind == K::ast_annotation_decl) {
      Result.TopLevelNames.insert(Child->text);
      Result.Visible[Result.CurrentModule].insert(Child->text);
    }
  const auto LoadVisibleModule = [&](const std::string &Name,
                                     bool AnnotationsOnly) {
    const auto Path = FindModule(Name, File, ModulePaths);
    if (!Path)
      return false;
    std::ifstream Input(*Path, std::ios::binary);
    if (!Input)
      return false;
    std::string Source((std::istreambuf_iterator<char>(Input)), {});
    auto Module = kelyra::lex::Parser().parse(std::move(Source), Path->string());
    if (!Module.ok())
      return false;
    auto &Names = Result.Visible[Name];
    for (const auto &Declaration : Module.root->children)
      if (IsPublic(*Declaration) &&
          (Declaration->kind == K::ast_annotation_decl ||
           (!AnnotationsOnly && (Declaration->kind == K::ast_class ||
                                 Declaration->kind == K::ast_function ||
                                 Declaration->kind == K::ast_enum ||
                                 Declaration->kind == K::ast_alias_decl))))
        Names.insert(Declaration->text);
    return true;
  };
  for (const auto &Child : Parsed.root->children) {
    if (Child->kind != K::ast_import)
      continue;
    if (!LoadVisibleModule(Child->text, false)) {
      Result.Complete = false;
      break;
    }
  }
  if (Result.Complete && Result.CurrentModule != "std.annotation" &&
      !Result.Visible.contains("std.annotation"))
    LoadVisibleModule("std.annotation", true);
  return Result;
}
} // namespace

int main(int Argc, char **Argv) {
  bool InPlace = false;
  bool Check = false;
  std::vector<std::string> Files;
  std::vector<std::filesystem::path> ModulePaths;
  for (int I = 1; I < Argc; ++I) {
    const std::string Argument = Argv[I];
    if (Argument == "-h" || Argument == "--help") {
      Usage(std::cout);
      return 0;
    }
    if (Argument == "-i")
      InPlace = true;
    else if (Argument == "--check")
      Check = true;
    else if (Argument.starts_with("--module-path="))
      ModulePaths.emplace_back(Argument.substr(14));
    else if (!Argument.empty() && Argument.front() == '-') {
      std::cerr << "unknown option: " << Argument << '\n';
      Usage(std::cerr);
      return 2;
    } else
      Files.push_back(Argument);
  }
  if (Files.empty() || (InPlace && Check) ||
      (!InPlace && !Check && Files.size() != 1)) {
    Usage(std::cerr);
    return 2;
  }

  bool Changed = false;
  for (const auto &Path : Files) {
    std::ifstream Input(Path, std::ios::binary);
    if (!Input) {
      std::cerr << "cannot open source file: " << Path << '\n';
      return 1;
    }
    std::string Source((std::istreambuf_iterator<char>(Input)), {});
    if (Input.bad()) {
      std::cerr << "cannot read source file: " << Path << '\n';
      return 1;
    }

    auto Parsed = kelyra::lex::Parser().parse(Source, Path);
    if (!Parsed.ok()) {
      for (const auto &Diagnostic : Parsed.diagnostics)
        std::cerr << Diagnostic << '\n';
      return 1;
    }
    const auto Symbols = CollectSymbols(Parsed, Path, ModulePaths);
    const auto Formatted = kelyra::lex::Format(Parsed, Symbols);
    if (Formatted == Source)
      continue;
    Changed = true;
    if (Check) {
      std::cerr << Path << ": needs formatting\n";
      continue;
    }
    if (!InPlace) {
      std::cout << Formatted;
      continue;
    }
    std::ofstream Output(Path, std::ios::binary | std::ios::trunc);
    if (!Output || !(Output << Formatted)) {
      std::cerr << "cannot write source file: " << Path << '\n';
      return 1;
    }
  }
  return Check && Changed ? 1 : 0;
}
