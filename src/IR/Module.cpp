//
// Created by zzm on 2026/9/22
// Part of RVision
//

#include "IR/Module.h"
#include <iostream>
#include <algorithm>
#include <optional>

using namespace kelyra;

bool ModuleLoader::IsUnderExternalPath(const std::filesystem::path &Path) const {
  const auto Normalized = Path.lexically_normal();
  for (const auto &External : ExternalPaths) {
    const auto Relative = Normalized.lexically_relative(External);
    if (!Relative.empty() && Relative.native().rfind("..", 0) != 0)
      return true;
  }
  return false;
}

std::optional<std::filesystem::path>
ModuleLoader::FindModule(const std::string &Name) const {
  std::string Relative = Name;
  std::replace(Relative.begin(), Relative.end(), '.', '/');
  Relative += ".kly";
  const std::filesystem::path RelativePath(Relative);
  std::error_code Error;
  const auto EntryCandidate = Root / RelativePath;
  if (std::filesystem::is_regular_file(EntryCandidate, Error))
    return EntryCandidate;
  for (const auto &ModulePath : ModulePaths) {
    const auto Candidate = ModulePath / RelativePath;
    if (std::filesystem::is_regular_file(Candidate, Error))
      return Candidate;
  }
  return std::nullopt;
}

bool ModuleLoader::Load(const std::filesystem::path &Path, std::string Expected,
          bool IsEntry) {
  if (Option::Progress)
    std::cerr << "  [parse " << Modules.size() + 1 << "] " << Path.string()
              << '\n';
  std::ifstream Input(Path, std::ios::binary);
  if (!Input) {
    std::cerr << "cannot open source file: " << Path.string() << '\n';
    return false;
  }
  std::string Source((std::istreambuf_iterator<char>(Input)), {});
  if (Input.bad()) {
    std::cerr << "cannot read source file: " << Path.string() << '\n';
    return false;
  }

  Modules.emplace_back();
  auto &Module = Modules.back();
  Module.Path = Path.string();
  Module.IsEntry = IsEntry;
  // The entry is always compiled; external paths only cover linked modules.
  Module.IsExternal = !IsEntry && IsUnderExternalPath(Path);
  Module.Parsed = Lexer.parse(std::move(Source), Module.Path);
  for (const auto &Diagnostic : Module.Parsed.diagnostics)
    std::cerr << Diagnostic << '\n';
  if (!Module.Parsed.ok())
    return false;

  std::string Name;
  for (const auto &Child : Module.Parsed.root->children)
    if (Child->kind == lex::TokenKind::ast_module_decl)
      Name = Child->text;
  if (!Expected.empty() && Name != Expected) {
    std::cerr << Module.Path << ": error: expected module '" << Expected
              << "'\n";
    return false;
  }
  if (Name.empty() && !IsEntry) {
    std::cerr << Module.Path << ": error: imported file has no module "
              << "declaration\n";
    return false;
  }
  if (!Name.empty()) {
    const auto [It, Inserted] = States.emplace(Name, State::Loading);
    if (!Inserted) {
      std::cerr << Module.Path << ": error: cyclic or duplicate module '"
                << Name << "'\n";
      return false;
    }
  }

  for (const auto &Child : Module.Parsed.root->children) {
    if (Child->kind != lex::TokenKind::ast_import)
      continue;
    if (Child->text == "c") {
      if (Child->children.size() != 1) {
        std::cerr << Module.Path
                  << ": error: 'import c' requires a header string\n";
        return false;
      }
      CHeaders.push_back(
          std::filesystem::absolute(Path.parent_path() /
                                    Child->children.front()->text)
              .lexically_normal()
              .string());
      continue;
    }
    const bool Wildcard = Child->text.ends_with(".*");
    const auto Imported = Wildcard
                              ? Child->text.substr(0, Child->text.size() - 2)
                              : Child->text;
    if (Imported == "c")
      continue;
    const auto Known = States.find(Imported);
    if (Known != States.end()) {
      if (Known->second == State::Loading) {
        std::cerr << Module.Path << ": error: cyclic module import '"
                  << Imported << "'\n";
        return false;
      }
      continue;
    }
    const auto Found = FindModule(Imported);
    if (!Found) {
      std::cerr << Module.Path << ": error: cannot find module '" << Imported
                << "'\n";
      return false;
    }
    if (!Load(*Found, Imported, false))
      return false;
  }
  if (!Name.empty())
    States[Name] = State::Loaded;
  return true;
}
