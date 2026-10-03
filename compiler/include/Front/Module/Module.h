//
// Created by zhou_zhengming on 2026/10/2.
//

#pragma once

#include "Front/Parser/Parser.h"
#include "Front/Sema/CDeclarations.h"

#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace kelyra {
class Module {
  // AST locations refer to this storage, including after Module is moved.
  std::shared_ptr<std::string> SourcePath;

public:
  std::filesystem::path Path;
  std::string Name;
  bool IsExternal = false;
  bool IsEntry = false;
  lex::ParseResult Lex;
  std::vector<const Module *> Imports;

  Module() = default;
  void SetPath(const std::filesystem::path &Path, std::string_view DiagnosticPath = {});
  const std::string &GetSourcePath() const { return *SourcePath; }
};

class ModuleLoader {
  enum class State { Loading, Loaded };

  lex::Parser Parser;
  std::filesystem::path Root;
  bool Library = false;
  std::vector<std::filesystem::path> ModuleSearchPath;
  std::unordered_map<std::string, Module *> ByName;
  std::unordered_map<const Module *, State> States;
  std::vector<const Module *> CompilationModules;
  std::vector<std::string> CHeaders;
  const Module *Entry = nullptr;

  bool LoadLibrary();
  bool LoadInput();
  bool LoadLibrarySources(const std::filesystem::path &Path);
  bool LoadSource(const std::filesystem::path &Path, const std::string &Expected,
                  bool IsEntry = false, bool SkipDisabled = false);
  bool LoadImported(const std::string &Name);
  bool ResolveImports(Module &Module);
  bool Activate(Module &Module);
  Module *Insert(Module NewModule);
  std::optional<std::filesystem::path> FindModule(const std::string &Name,
                                                  bool LocalOnly = false) const;

public:
  // Own all loaded source and interface modules; addresses remain stable.
  std::map<std::filesystem::path, Module> Modules;
  sema::CDeclarations LibraryDeclarations;

  bool Load();
  const std::filesystem::path &GetSourceRoot() const { return Root; }
  bool IsLibrary() const { return Library; }
  // Available after a successful Load().
  const Module &GetEntry() const { return *Entry; }
  // Entry first, followed by its dependencies and explicit library sources.
  // Module identity is immutable; its owned AST remains mutable for lowering.
  const std::vector<const Module *> &GetModules() const { return CompilationModules; }
  const Module *FindLoadedModule(std::string_view Name) const {
    const auto Found = ByName.find(std::string(Name));
    return Found == ByName.end() ? nullptr : Found->second;
  }
  const std::vector<std::string> &GetCHeaders() const { return CHeaders; }
};
} // namespace kelyra
