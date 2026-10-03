#include "CodeGen/Compilation.h"

#include "CodeGen/IRGen.h"
#include "Support/Log.h"
#include "Support/Option.h"

#include "mlir/IR/MLIRContext.h"
#include "mlir/IR/Verifier.h"
#include "llvm/Object/ArchiveWriter.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/TargetParser/Triple.h"

#include <deque>
#include <filesystem>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace kelyra::codegen::detail {
bool IsDebugInfoEnabled();
bool IsFreestandingRuntime();
llvm::Error EmitObject(mlir::ModuleOp Module, llvm::ArrayRef<std::string> CWrapperSources,
                       llvm::StringRef ObjectPath = {});
llvm::Error EmitExecutable(mlir::ModuleOp Module, llvm::ArrayRef<std::string> CWrapperSources,
                           llvm::ArrayRef<std::string> Libraries);
} // namespace kelyra::codegen::detail

using namespace kelyra;

namespace {
enum class SemaLevel { None, Basic };
llvm::cl::opt<bool> EmitObjectOption{"emit-obj",
                                     llvm::cl::desc("Generate a native object file"),
                                     llvm::cl::cat(Option::KelyraCategory)};
llvm::cl::opt<bool> EmitExecutableOption{"emit-exe",
                                         llvm::cl::desc("Generate a native executable"),
                                         llvm::cl::cat(Option::KelyraCategory)};
llvm::cl::opt<SemaLevel> SamaLevel{
    "safe-level",
    llvm::cl::desc("Semantic check level"),
    llvm::cl::values(clEnumValN(SemaLevel::None, "0", "No semantic check"),
                     clEnumValN(SemaLevel::Basic, "1", "Basic semantic check")),
    llvm::cl::init(SemaLevel::Basic),
    llvm::cl::cat(Option::KelyraCategory)};

int EmitLibrary(const ModuleLoader &Loader, const sema::Sema &Analysis) {
  const auto SourceRoot = Loader.GetSourceRoot();
  std::vector<const lex::Node *> Asts;
  std::vector<const lex::Node *> Owned;
  std::vector<const lex::Node *> ExternalAsts;
  for (const auto *Source : Loader.GetModules()) {
    const auto *Ast = Source->Lex.root.get();
    if (Analysis.IsMetaModule(Source->Name))
      continue;
    Asts.push_back(Ast);
    if (Source->IsExternal) {
      ExternalAsts.push_back(Ast);
      continue;
    }
    const auto Relative =
        std::filesystem::path(Source->Path).lexically_normal().lexically_relative(SourceRoot);
    if (!Relative.empty() && *Relative.begin() != "..")
      Owned.push_back(Ast);
  }
  if (Owned.empty()) {
    kerr() << "library contains no runtime modules\n";
    return 1;
  }

  const llvm::Triple Target(Option::Target.getValue());
  std::vector<llvm::NewArchiveMember> Members;
  std::deque<std::string> MemberNames;
  std::vector<std::unique_ptr<llvm::FileRemover>> TemporaryFiles;
  const auto AddObject = [&](llvm::ArrayRef<std::string> Sources,
                             const lex::Node *Definition) -> llvm::Error {
    llvm::SmallString<128> ObjectPath;
    if (auto Error = llvm::sys::fs::createTemporaryFile(
            "kelyra-library", Target.isOSWindows() ? "obj" : "o", ObjectPath))
      return llvm::createStringError(Error, "cannot create library object");
    TemporaryFiles.push_back(std::make_unique<llvm::FileRemover>(ObjectPath));
    mlir::MLIRContext Context;
    codegen::IRGen Generator(Context,
                             Analysis,
                             static_cast<unsigned>(SamaLevel.getValue()),
                             codegen::detail::IsDebugInfoEnabled());
    std::set<const lex::Node *> External;
    for (const auto *Ast : Asts)
      if (Ast != Definition)
        External.insert(Ast);
    Generator.SetExternalModules(External);
    Generator.SetEmitExternalGenericInstances(Definition == nullptr);
    auto Module = !Definition && ExternalAsts.empty()
                      ? mlir::OwningOpRef<mlir::ModuleOp>(
                            mlir::ModuleOp::create(mlir::UnknownLoc::get(&Context)))
                      : Generator.Generate(Definition ? Asts : ExternalAsts);
    if (mlir::failed(mlir::verify(*Module)))
      return llvm::createStringError("invalid library module IR");
    if (Option::DumpMlir) {
      Module->print(llvm::outs());
      llvm::outs() << '\n';
    }
    if (auto Error = codegen::detail::EmitObject(*Module, Sources, ObjectPath))
      return Error;
    auto Member = llvm::NewArchiveMember::getFile(ObjectPath, true);
    if (!Member)
      return Member.takeError();
    MemberNames.push_back("module" + std::to_string(MemberNames.size()) +
                          (Target.isOSWindows() ? ".obj" : ".o"));
    Member->MemberName = MemberNames.back();
    Members.push_back(std::move(*Member));
    return llvm::Error::success();
  };
  for (const auto *Ast : Owned)
    if (auto Error = AddObject({}, Ast)) {
      kerr() << llvm::toString(std::move(Error)) << '\n';
      return 1;
    }
  const auto WrapperSources = Analysis.GetCWrapperSources();
  if (!WrapperSources.empty() || !ExternalAsts.empty()) {
    if (auto Error = AddObject(WrapperSources, nullptr)) {
      kerr() << llvm::toString(std::move(Error)) << '\n';
      return 1;
    }
  }
  if (auto Error = llvm::writeArchive(Option::OutputFile.getValue(),
                                      Members,
                                      llvm::SymtabWritingMode::NormalSymtab,
                                      Target.isOSWindows() ? llvm::object::Archive::K_COFF
                                                           : llvm::object::Archive::K_GNU,
                                      true,
                                      false)) {
    kerr() << llvm::toString(std::move(Error)) << '\n';
    return 1;
  }
  return 0;
}
} // namespace

int codegen::Emit(const ModuleLoader &Loader, const sema::Sema &Analysis) {
  if (!Option::DumpMlir && !EmitObjectOption && !EmitExecutableOption)
    return 0;
  if (Loader.IsLibrary() && EmitObjectOption)
    return EmitLibrary(Loader, Analysis);
  const auto &Modules = Loader.GetModules();
  std::unordered_set<std::string> RuntimeModules;
  std::vector<const kelyra::Module *> Pending{&Loader.GetEntry()};
  while (!Pending.empty()) {
    const auto *Source = Pending.back();
    Pending.pop_back();
    if (!Source || Analysis.IsMetaModule(Source->Name) ||
        !RuntimeModules.insert(Source->Name).second)
      continue;
    for (const auto &Dependency : Analysis.GetRuntimeDependencies(Source->Name))
      Pending.push_back(Loader.FindLoadedModule(Dependency));
    Pending.insert(Pending.end(), Source->Imports.begin(), Source->Imports.end());
  }
  std::vector<const lex::Node *> Asts;
  std::set<const lex::Node *> External;
  for (const auto *Module : Modules) {
    if (!RuntimeModules.contains(Module->Name))
      continue;
    Asts.push_back(Module->Lex.root.get());
    if (Module->IsExternal)
      External.insert(Module->Lex.root.get());
  }
  mlir::MLIRContext Context;
  codegen::IRGen Generator(Context,
                           Analysis,
                           static_cast<unsigned>(SamaLevel.getValue()),
                           codegen::detail::IsDebugInfoEnabled());
  Generator.SetExternalModules(External);
  auto Module = Generator.Generate(Asts);
  const auto CWrapperSources = Analysis.GetCWrapperSources();
  if (mlir::failed(mlir::verify(*Module)))
    return 1;
  if (Option::DumpMlir) {
    Module->print(llvm::outs());
    llvm::outs() << '\n';
  }
  if (!EmitObjectOption && !EmitExecutableOption)
    return 0;
  auto Error = [&]() -> llvm::Error {
    if (EmitObjectOption)
      return detail::EmitObject(*Module, CWrapperSources);
    const auto Libraries = Analysis.GetLinkLibraries(RuntimeModules);
    const std::vector<std::string> LibraryNames(Libraries.begin(), Libraries.end());
    return detail::EmitExecutable(*Module, CWrapperSources, LibraryNames);
  }();
  if (Error) {
    kerr() << Option::InputFile << ": error: " << llvm::toString(std::move(Error)) << '\n';
    return 1;
  }
  return 0;
}

std::optional<codegen::CompilationPlan> codegen::PrepareCompilation(const ModuleLoader &Loader,
                                                                    bool ExclusiveAction) {
  if (ExclusiveAction && (Option::DumpMlir || EmitObjectOption || EmitExecutableOption)) {
    kerr() << "native compilation cannot be combined with --emit-c-defs\n";
    return std::nullopt;
  }
  if (EmitObjectOption && EmitExecutableOption) {
    kerr() << "--emit-obj and --emit-exe cannot be combined\n";
    return std::nullopt;
  }
  if ((EmitObjectOption || EmitExecutableOption) && Option::OutputFile.empty()) {
    kerr() << "native output requires -o\n";
    return std::nullopt;
  }
  if (Loader.IsLibrary() && EmitExecutableOption) {
    kerr() << "--library-root cannot be combined with --emit-exe\n";
    return std::nullopt;
  }
  CompilationPlan Plan;
  Plan.NativeOutput = EmitObjectOption || EmitExecutableOption;
  Plan.RequiresEntrypoint = EmitExecutableOption || (EmitObjectOption && !Loader.IsLibrary() &&
                                                     detail::IsFreestandingRuntime());
  return Plan;
}
