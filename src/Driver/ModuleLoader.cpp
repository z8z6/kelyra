#include "Driver/ModuleLoader.h"

#include "llvm/Support/MemoryBuffer.h"

#include <algorithm>
#include <iostream>

using namespace kelyra;

void ModuleLoader::AddModulePath(const std::string &Path) {
  ModulePaths.push_back(std::filesystem::absolute(Path).lexically_normal());
}

void ModuleLoader::AddExternalPath(const std::string &Path) {
  ExternalPaths.push_back(std::filesystem::absolute(Path).lexically_normal());
}

bool ModuleLoader::LoadEntry(const std::string &Path) {
  const auto Entry = std::filesystem::absolute(Path).lexically_normal();
  Root = Entry.parent_path();
  return Load(Entry, {}, true);
}

bool ModuleLoader::IsUnderExternalPath(
    const std::filesystem::path &Path) const {
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
  if (Progress)
    std::cerr << "  [parse " << Modules.size() + 1 << "] " << Path.string()
              << '\n';
  auto Buffer = llvm::MemoryBuffer::getFile(Path.string());
  if (!Buffer) {
    std::cerr << "cannot read source file: " << Path.string() << '\n';
    return false;
  }

  Modules.emplace_back();
  auto &Module = Modules.back();
  Module.Path = Path.string();
  Module.IsEntry = IsEntry;
  Module.IsExternal = !IsEntry && IsUnderExternalPath(Path);
  Module.Parsed = Lexer.parse((*Buffer)->getBuffer().str(), Module.Path);
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
  if (!Name.empty() && !States.emplace(Name, State::Loading).second) {
    std::cerr << Module.Path << ": error: cyclic or duplicate module '" << Name
              << "'\n";
    return false;
  }

  for (const auto &Child : Module.Parsed.root->children) {
    if (Child->kind != lex::TokenKind::ast_import)
      continue;
    if (Child->text == "c") {
      CHeaders.push_back(std::filesystem::absolute(
                             Path.parent_path() / Child->children.front()->text)
                             .lexically_normal()
                             .string());
      continue;
    }
    const bool Wildcard = Child->text.ends_with(".*");
    const auto Imported =
        Wildcard ? Child->text.substr(0, Child->text.size() - 2) : Child->text;
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
