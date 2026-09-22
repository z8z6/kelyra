//
// Created by zzm on 2026/9/22
// Part of RVision
//

#pragma once

#include <deque>
#include <filesystem>

namespace kelyra {
struct SourceModule {
  std::string Path;
  lex::ParseResult Parsed;
  bool IsEntry = false;
  bool IsExternal = false;
};

class ModuleLoader {
  enum class State { Loading, Loaded };

  std::filesystem::path Root;
  std::vector<std::filesystem::path> ModulePaths;
  std::vector<std::filesystem::path> ExternalPaths;
  lex::Lexer Lexer;
  std::deque<SourceModule> Modules;
  std::unordered_map<std::string, State> States;
  std::vector<std::string> CHeaders;

  bool IsUnderExternalPath(const std::filesystem::path &Path) const;

  std::optional<std::filesystem::path> FindModule(const std::string &Name) const;

  bool Load(const std::filesystem::path &Path, std::string Expected, bool IsEntry);

public:
  void AddModulePath(const std::string &Path) {
    ModulePaths.push_back(std::filesystem::absolute(Path).lexically_normal());
  }

  void AddExternalPath(const std::string &Path) {
    ExternalPaths.push_back(std::filesystem::absolute(Path).lexically_normal());
  }

  bool LoadEntry(const std::string &Path) {
    const auto Entry = std::filesystem::absolute(Path).lexically_normal();
    Root = Entry.parent_path();
    return Load(Entry, {}, true);
  }

  const std::deque<SourceModule> &GetModules() const { return Modules; }
  const std::vector<std::string> &GetCHeaders() const { return CHeaders; }
};
}
