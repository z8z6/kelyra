#pragma once

#include "Lexer/Lexer.h"

#include <deque>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

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
  bool Progress;
  std::string TargetTriple;

  bool IsUnderExternalPath(const std::filesystem::path &Path) const;
  std::optional<std::filesystem::path>
  FindModule(const std::string &Name) const;
  bool Load(const std::filesystem::path &Path, std::string Expected,
            bool IsEntry);

public:
  explicit ModuleLoader(bool Progress = false, std::string TargetTriple = {})
      : Progress(Progress), TargetTriple(std::move(TargetTriple)) {}

  void AddModulePath(const std::string &Path);
  void AddExternalPath(const std::string &Path);
  bool LoadEntry(const std::string &Path);

  const std::deque<SourceModule> &GetModules() const { return Modules; }
  std::deque<SourceModule> &GetMutableModules() { return Modules; }
  const std::vector<std::string> &GetCHeaders() const { return CHeaders; }
};
} // namespace kelyra
