#include "Front/Module/Module.h"
#include "Front/Expansion/TargetConditions.h"
#include "Front/Module/ModuleInterface.h"
#include "Support/Log.h"
#include "Support/Option.h"

#include "llvm/Support/CommandLine.h"
#include "llvm/Support/MemoryBuffer.h"

#include <algorithm>
#include <set>
#include <sstream>
#include <string_view>
#include <utility>

using namespace kelyra;

namespace {
llvm::cl::list<std::string> SearchPath{"module-search-path",
                                       llvm::cl::desc("Source module search path"),
                                       llvm::cl::ZeroOrMore,
                                       llvm::cl::cat(Option::KelyraCategory)};
llvm::cl::list<std::string> InterfacePath{
    "module-interface-path",
    llvm::cl::desc("Kelyra .kmi file or directory containing .kmi files"),
    llvm::cl::ZeroOrMore,
    llvm::cl::cat(Option::KelyraCategory)};
llvm::cl::opt<std::string> LibraryRoot{"library-root",
                                       llvm::cl::desc("Source root of a library compilation"),
                                       llvm::cl::cat(Option::KelyraCategory)};

void SetFile(lex::Node &Node, std::string_view File) {
  Node.Loc.File = File;
  for (auto &Child : Node.children)
    SetFile(*Child, File);
}

std::filesystem::path AbsolutePath(const std::filesystem::path &Path) {
  return std::filesystem::absolute(Path).lexically_normal().make_preferred();
}

std::filesystem::path ModulePath(const std::string &Name) {
  std::string Relative = Name;
  std::replace(Relative.begin(), Relative.end(), '.', '/');
  return Relative + ".kly";
}

std::optional<std::filesystem::path> FindSource(const std::filesystem::path &Root,
                                                const std::string &Name) {
  const auto Relative = ModulePath(Name);
  const auto Directory = Relative.parent_path() / Relative.stem() / Relative.filename();
  for (const auto &Candidate : {Root / Relative, Root / Directory})
    if (std::filesystem::is_regular_file(Candidate))
      return AbsolutePath(Candidate);
  return std::nullopt;
}

bool HasSuffix(const std::filesystem::path &Path, const std::filesystem::path &Suffix) {
  auto Actual = Path.end();
  auto Expected = Suffix.end();
  while (Expected != Suffix.begin()) {
    if (Actual == Path.begin() || *--Actual != *--Expected)
      return false;
  }
  return true;
}

} // namespace

void Module::SetPath(const std::filesystem::path &NewPath, std::string_view DiagnosticPath) {
  const auto LocationPath = DiagnosticPath.empty() ? NewPath.string() : std::string(DiagnosticPath);
  Path = AbsolutePath(NewPath);
  SourcePath = std::make_shared<std::string>(LocationPath);
  Lex.File = *SourcePath;
  if (Lex.root)
    SetFile(*Lex.root, Lex.File);
}

bool ModuleLoader::Load() {
  // Reset derived indexes before their owning storage.
  Entry = nullptr;
  CompilationModules.clear();
  States.clear();
  ByName.clear();
  CHeaders.clear();
  Modules.clear();
  LibraryDeclarations = {};
  ModuleSearchPath.clear();
  Library = !LibraryRoot.empty();
  try {
    Root = LibraryRoot.empty() ? AbsolutePath(Option::InputFile.getValue()).parent_path()
                               : AbsolutePath(LibraryRoot.getValue());
    for (const auto &Path : SearchPath)
      ModuleSearchPath.push_back(AbsolutePath(Path));
    if (!LoadLibrary() || !LoadInput() || !LoadImported("std.annotation"))
      return false;
    return !Library || LoadLibrarySources(Root);
  } catch (const std::filesystem::filesystem_error &Error) {
    kerr() << "cannot load modules: " << Error.what() << '\n';
    return false;
  }
}

Module *ModuleLoader::Insert(Module NewModule) {
  if (!NewModule.Name.empty()) {
    const auto Existing = ByName.find(NewModule.Name);
    if (Existing != ByName.end()) {
      kerr() << "duplicate module '" << NewModule.Name << "': " << Existing->second->Path.string()
             << " and " << NewModule.Path.string() << '\n';
      return nullptr;
    }
  }
  const auto Path = NewModule.Path;
  const auto [It, Added] = Modules.try_emplace(Path, std::move(NewModule));
  if (!Added) {
    kerr() << "module path already loaded: " << It->second.Path.string() << '\n';
    return nullptr;
  }
  if (!It->second.Name.empty())
    ByName.emplace(It->second.Name, &It->second);
  return &It->second;
}

bool ModuleLoader::LoadLibrary() {
  std::set<std::filesystem::path> KMIs;
  for (const auto &Argument : InterfacePath) {
    const auto Path = AbsolutePath(Argument);
    if (std::filesystem::is_regular_file(Path) && ModuleInterface::isKMI(Path)) {
      KMIs.emplace(Path);
    } else if (std::filesystem::is_directory(Path)) {
      for (const auto &File : std::filesystem::recursive_directory_iterator(Path))
        if (File.is_regular_file() && ModuleInterface::isKMI(File.path()))
          KMIs.emplace(AbsolutePath(File.path()));
    } else {
      kerr() << "expected a .kmi file or interface directory: " << Path.string() << '\n';
      return false;
    }
  }
  for (const auto &Path : KMIs) {
    ModuleInterface Interface(Path);
    if (!Interface.Load()) {
      kerr() << "cannot load module interface: " << Path.string() << '\n';
      return false;
    }
    if (!LibraryDeclarations.Merge(Interface.Declarations))
      return false;
    for (auto &Module : Interface.Modules) {
      Module.SetPath(Path / ModulePath(Module.Name), Module.GetSourcePath());
      Module.IsExternal = true;
      Module.IsEntry = false;
      if (!Insert(std::move(Module)))
        return false;
    }
  }
  return true;
}

bool ModuleLoader::LoadInput() {
  return LoadSource(AbsolutePath(Option::InputFile.getValue()), {}, true);
}

bool ModuleLoader::LoadLibrarySources(const std::filesystem::path &Path) {
  if (!std::filesystem::is_directory(Path)) {
    kerr() << "cannot read library source directory: " << Path.string() << '\n';
    return false;
  }
  std::vector<std::filesystem::path> Files;
  for (const auto &File : std::filesystem::recursive_directory_iterator(Path))
    if (File.is_regular_file() && File.path().extension() == ".kly")
      Files.push_back(AbsolutePath(File.path()));
  std::sort(Files.begin(), Files.end());
  for (const auto &File : Files)
    if (!LoadSource(File, {}, false, true))
      return false;
  return true;
}

std::optional<std::filesystem::path> ModuleLoader::FindModule(const std::string &Name,
                                                              bool LocalOnly) const {
  if (auto Path = FindSource(Root, Name))
    return Path;
  if (LocalOnly)
    return std::nullopt;
  for (const auto &SearchPath : ModuleSearchPath)
    if (auto Path = FindSource(SearchPath, Name))
      return Path;
#ifdef KELYRA_STDLIB_SOURCE_DIR
  if (Name.starts_with("std."))
    return FindSource(KELYRA_STDLIB_SOURCE_DIR, Name);
#endif
  return std::nullopt;
}

bool ModuleLoader::LoadSource(const std::filesystem::path &Path, const std::string &Expected,
                              bool IsEntry, bool SkipDisabled) {
  Module Source;
  Source.SetPath(Path);
  if (const auto Existing = Modules.find(Source.Path); Existing != Modules.end()) {
    if (Existing->second.IsExternal || (!Expected.empty() && Existing->second.Name != Expected)) {
      kerr() << "conflicting source module: " << Source.Path.string() << '\n';
      return false;
    }
    return Activate(Existing->second);
  }
  const auto Buffer = llvm::MemoryBuffer::getFile(Source.GetSourcePath());
  if (!Buffer) {
    kerr() << "cannot read source file: " << Source.Path.string() << ": "
           << Buffer.getError().message() << '\n';
    return false;
  }
  Source.Lex = Parser.parse((*Buffer)->getBuffer().str(), Source.GetSourcePath());
  for (const auto &Diagnostic : Source.Lex.diagnostics) {
    std::ostringstream Message;
    Message << Diagnostic;
    kerr() << Message.str() << '\n';
  }
  if (!Source.Lex.ok())
    return false;
  bool Enabled = true;
  if (!ApplyTargetConditions(
          *Source.Lex.root, Source.GetSourcePath(), Option::Target.getValue(), Enabled))
    return false;
  if (!Enabled) {
    if (SkipDisabled)
      return true;
    kerr() << Source.Path.string() << ": module is disabled by @cfg for target\n";
    return false;
  }
  for (const auto &Child : Source.Lex.root->children)
    if (Child->kind == lex::NodeKind::ast_module_decl)
      Source.Name = Child->text;
  if (!Expected.empty() && Source.Name != Expected) {
    kerr() << Source.Path.string() << ": expected module '" << Expected << "'\n";
    return false;
  }
  if (!Source.Name.empty()) {
    const auto Relative = ModulePath(Source.Name);
    const auto Directory = Relative.parent_path() / Relative.stem() / Relative.filename();
    if (!HasSuffix(Source.Path, Relative) && !HasSuffix(Source.Path, Directory))
      kwarn() << Source.Path.string() << ": module '" << Source.Name
              << "' does not match path suffix '" << Relative.string() << "'\n";
  }
  Source.IsEntry = IsEntry;
  auto *Loaded = Insert(std::move(Source));
  if (!Loaded)
    return false;
  if (IsEntry)
    Entry = Loaded;
  return Activate(*Loaded);
}

bool ModuleLoader::LoadImported(const std::string &Name) {
  // Local source cannot silently override an explicitly supplied interface.
  if (const auto Path = FindModule(Name, true))
    return LoadSource(*Path, Name);
  if (const auto Known = ByName.find(Name); Known != ByName.end())
    return Activate(*Known->second);
  if (const auto Path = FindModule(Name))
    return LoadSource(*Path, Name);
  kerr() << "cannot find module '" << Name << "'\n";
  return false;
}

bool ModuleLoader::Activate(Module &Module) {
  const auto [It, Added] = States.emplace(&Module, State::Loading);
  if (!Added) {
    if (It->second == State::Loaded)
      return true;
    kerr() << Module.Path.string() << ": cyclic module import '" << Module.Name << "'\n";
    return false;
  }
  CompilationModules.push_back(&Module);
  if (!ResolveImports(Module))
    return false;
  // Resolving imports may rehash States; do not retain the iterator.
  States.at(&Module) = State::Loaded;
  return true;
}

bool ModuleLoader::ResolveImports(Module &Module) {
  for (const auto &Child : Module.Lex.root->children) {
    if (Child->kind != lex::NodeKind::ast_import)
      continue;
    if (Child->text != "c") {
      if (!LoadImported(Child->text))
        return false;
      Module.Imports.push_back(ByName.at(Child->text));
      continue;
    }
    if (Module.IsExternal || Child->children.empty())
      continue;
    if (Child->children.size() != 1) {
      kerr() << Module.Path.string() << ": invalid C header import\n";
      return false;
    }
    const auto Header =
        AbsolutePath(Module.Path.parent_path() / Child->children.front()->text).string();
    if (std::find(CHeaders.begin(), CHeaders.end(), Header) == CHeaders.end())
      CHeaders.push_back(Header);
  }
  return true;
}
