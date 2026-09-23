#include "Driver/ModuleLoader.h"
#include "Support/BuiltinAnnotation.h"
#include "Support/Log.h"

#include "llvm/Support/MemoryBuffer.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/TargetParser/Triple.h"

#include <algorithm>
#include <sstream>

using namespace kelyra;

namespace {
bool ApplyTargetConditions(lex::Node &Root, const std::string &Path,
                           const std::string &TargetTriple,
                           bool &ModuleEnabled) {
  using K = lex::TokenKind;
  const llvm::Triple Target(TargetTriple.empty()
                                ? llvm::sys::getDefaultTargetTriple()
                                : TargetTriple);
  const std::string OS = Target.isOSWindows() ? "windows"
                         : Target.isOSLinux() ? "linux"
                         : Target.isMacOSX()  ? "macos"
                                              : Target.getOSName().str();
  const std::string Arch = Target.getArchName().str();
  bool Valid = true;
  auto &Declarations = Root.children;
  Declarations.erase(
      std::remove_if(
          Declarations.begin(), Declarations.end(),
          [&](std::unique_ptr<lex::Node> &Declaration) {
            bool Enabled = true;
            auto &Parts = Declaration->children;
            Parts.erase(
                std::remove_if(
                    Parts.begin(), Parts.end(),
                    [&](const std::unique_ptr<lex::Node> &Part) {
                      if (Part->kind != K::ast_annotation ||
                          !IsBuiltinAnnotation(Part->text, "cfg"))
                        return false;
                      if (Part->children.empty()) {
                        Valid = false;
                        return true;
                      }
                      bool SeenOS = false;
                      bool SeenArch = false;
                      for (const auto &Child : Part->children) {
                        const auto &Argument = *Child;
                        if (Argument.kind != K::ast_annotation_argument ||
                            Argument.children.size() != 1) {
                          Valid = false;
                          continue;
                        }
                        const auto &Value = *Argument.children.front();
                        const bool IsOS = Argument.text == "os";
                        const bool IsArch = Argument.text == "arch";
                        if (Value.kind != K::ast_literal ||
                            Value.text.size() < 2 ||
                            Value.text.front() != '"' ||
                            Value.text.back() != '"' || (!IsOS && !IsArch) ||
                            (IsOS && SeenOS) || (IsArch && SeenArch)) {
                          Valid = false;
                          continue;
                        }
                        SeenOS |= IsOS;
                        SeenArch |= IsArch;
                        const auto Wanted =
                            Value.text.substr(1, Value.text.size() - 2);
                        Enabled &= Wanted == (IsOS ? OS : Arch);
                      }
                      return true;
                    }),
                Parts.end());
            if (Declaration->kind == K::ast_import &&
                std::any_of(Parts.begin(), Parts.end(), [](const auto &Part) {
                  return Part->kind == K::ast_annotation;
                }))
              Valid = false;
            if (Declaration->kind == K::ast_module_decl) {
              if (std::any_of(Parts.begin(), Parts.end(), [](const auto &Part) {
                    return Part->kind == K::ast_annotation;
                  }))
                Valid = false;
              ModuleEnabled = Enabled;
              return false;
            }
            return !Enabled;
          }),
      Declarations.end());
  if (!Valid)
    kerr() << Path << ": error: invalid @cfg annotation\n";
  return Valid;
}
} // namespace

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
#ifdef KELYRA_STDLIB_SOURCE_DIR
  if (Name == BuiltinAnnotationModule)
    return std::filesystem::path(KELYRA_STDLIB_SOURCE_DIR) / RelativePath;
#endif
  return std::nullopt;
}

bool ModuleLoader::Load(const std::filesystem::path &Path, std::string Expected,
                        bool IsEntry) {
  if (Progress)
    kinfo() << "  [parse " << Modules.size() + 1 << "] " << Path.string()
            << '\n';
  auto Buffer = llvm::MemoryBuffer::getFile(Path.string());
  if (!Buffer) {
    kerr() << "cannot read source file: " << Path.string() << '\n';
    return false;
  }

  Modules.emplace_back();
  auto &Module = Modules.back();
  Module.Path = Path.string();
  Module.IsEntry = IsEntry;
  Module.IsExternal = !IsEntry && IsUnderExternalPath(Path);
  Module.Parsed = Lexer.parse((*Buffer)->getBuffer().str(), Module.Path);
  for (const auto &Diagnostic : Module.Parsed.diagnostics) {
    std::ostringstream Message;
    Message << Diagnostic;
    kerr() << Message.str() << '\n';
  }
  if (!Module.Parsed.ok())
    return false;
  bool ModuleEnabled = true;
  if (!ApplyTargetConditions(*Module.Parsed.root, Module.Path, TargetTriple,
                             ModuleEnabled))
    return false;
  if (!ModuleEnabled) {
    kerr() << Module.Path << ": error: module is disabled by @cfg for target\n";
    return false;
  }

  std::string Name;
  for (const auto &Child : Module.Parsed.root->children)
    if (Child->kind == lex::TokenKind::ast_module_decl)
      Name = Child->text;
  if (!Name.empty()) {
    auto ExpectedPath = std::filesystem::path{};
    std::istringstream Parts(Name);
    std::string Part;
    while (std::getline(Parts, Part, '.'))
      ExpectedPath /= Part;
    ExpectedPath += ".kly";
    const auto FullPath = std::filesystem::absolute(Path).lexically_normal();
    auto Actual = FullPath.end();
    auto Expected = ExpectedPath.end();
    bool Matches = true;
    while (Expected != ExpectedPath.begin()) {
      --Expected;
      if (Actual == FullPath.begin()) {
        Matches = false;
        break;
      }
      --Actual;
      if (*Actual != *Expected) {
        Matches = false;
        break;
      }
    }
    if (!Matches)
      kwarn() << Module.Path << ": warning: module '" << Name
              << "' does not match path suffix '" << ExpectedPath.string()
              << "'\n";
  }
  if (!Expected.empty() && Name != Expected) {
    kerr() << Module.Path << ": error: expected module '" << Expected << "'\n";
    return false;
  }
  if (!Name.empty() && !States.emplace(Name, State::Loading).second) {
    kerr() << Module.Path << ": error: cyclic or duplicate module '" << Name
           << "'\n";
    return false;
  }

  for (const auto &Child : Module.Parsed.root->children) {
    if (Child->kind != lex::TokenKind::ast_import)
      continue;
    if (Child->text == "c") {
      if (Child->children.size() != 1) {
        kerr() << Module.Path << ": error: import c requires a header\n";
        return false;
      }
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
        kerr() << Module.Path << ": error: cyclic module import '" << Imported
               << "'\n";
        return false;
      }
      continue;
    }
    const auto Found = FindModule(Imported);
    if (!Found) {
      kerr() << Module.Path << ": error: cannot find module '" << Imported
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
