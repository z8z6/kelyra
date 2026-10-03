#include "Support/Option.h"

#include "mlir/IR/BuiltinOps.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Support/Program.h"
#include "llvm/TargetParser/Triple.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <vector>

using namespace kelyra;

namespace {
llvm::cl::list<std::string> LinkPath{"link-path",
                                     llvm::cl::desc("Library linking path"),
                                     llvm::cl::ZeroOrMore,
                                     llvm::cl::cat(Option::KelyraCategory)};
llvm::cl::list<std::string> LinkLib{"link",
                                    llvm::cl::desc("Library linked"),
                                    llvm::cl::ZeroOrMore,
                                    llvm::cl::cat(Option::KelyraCategory)};
} // namespace

namespace kelyra::codegen::detail {
bool IsFreestandingRuntime();
llvm::Error EmitObject(mlir::ModuleOp Module, llvm::ArrayRef<std::string> CWrapperSources,
                       llvm::StringRef ObjectPath);

llvm::Error EmitExecutable(mlir::ModuleOp Module, llvm::ArrayRef<std::string> CWrapperSources,
                           llvm::ArrayRef<std::string> Libraries) {
  const llvm::StringRef OutputPath = Option::OutputFile.getValue();
  const llvm::Triple Triple(Option::Target.getValue());
  const bool Freestanding = IsFreestandingRuntime();
  std::vector<std::string> LinkSources;
  std::vector<std::string> LibraryNames(Libraries.begin(), Libraries.end());
  for (const auto &Input : LinkLib) {
    const std::filesystem::path Path(Input);
    if (Path.has_parent_path() || Path.extension() == ".a" || Path.extension() == ".o" ||
        Path.extension() == ".obj")
      LinkSources.push_back(Input);
    else
      LibraryNames.push_back(Input);
  }
  const bool Linux = Triple.getArch() == llvm::Triple::x86_64 && Triple.isOSLinux();
  const bool Windows = Triple.getArch() == llvm::Triple::x86_64 && Triple.isOSWindows();
  if (Freestanding && !Linux && !Windows)
    return llvm::createStringError(
        "freestanding runtime supports only Linux x86-64 and Windows x86-64 "
        "(target: %s)",
        Triple.str().c_str());

  llvm::SmallString<128> ObjectPath;
  if (auto ErrorCode = llvm::sys::fs::createTemporaryFile("kelyra", "o", ObjectPath))
    return llvm::createStringError(ErrorCode, "cannot create temporary object");
  llvm::FileRemover RemoveObject(ObjectPath);
  if (auto Error = EmitObject(Module, CWrapperSources, ObjectPath))
    return Error;

  // The target option always has a default, so the linker driver must support --target.
  auto Linker = llvm::sys::findProgramByName("clang");
  if (!Linker)
    return llvm::createStringError(Linker.getError(), "cannot find C compiler");
  const std::string TargetArgument = "--target=" + Triple.str();
  llvm::SmallString<128> StartObject;
  std::unique_ptr<llvm::FileRemover> RemoveStart;
  if (Freestanding) {
    const llvm::Twine Source = llvm::Twine(KELYRA_RUNTIME_DIR) +
                               (Linux ? "/linux-x86_64/start.S" : "/windows-x86_64/start.S");
    const std::string StartSource = Source.str();
    if (!llvm::sys::fs::exists(StartSource))
      return llvm::createStringError("missing freestanding startup file: %s", StartSource.c_str());
    if (auto ErrorCode = llvm::sys::fs::createTemporaryFile("kelyra-start", "o", StartObject))
      return llvm::createStringError(ErrorCode, "cannot create startup object");
    RemoveStart = std::make_unique<llvm::FileRemover>(StartObject);
    llvm::SmallVector<llvm::StringRef> Compile{*Linker, "-c", StartSource, "-o", StartObject};
    Compile.push_back(TargetArgument);
    std::string Message;
    if (llvm::sys::ExecuteAndWait(*Linker, Compile, std::nullopt, {}, 0, 0, &Message) != 0)
      return llvm::createStringError("failed to compile freestanding startup object: %s",
                                     Message.empty() ? "Clang exited with a non-zero status"
                                                     : Message.c_str());
  }
  llvm::SmallString<128> TemporaryOutput;
  llvm::sys::fs::createUniquePath(OutputPath + ".tmp-%%%%%%%%", TemporaryOutput, false);
  llvm::FileRemover RemoveOutput(TemporaryOutput);
  llvm::SmallVector<llvm::StringRef> Arguments{*Linker, ObjectPath};
  if (Windows)
    Arguments.push_back("-Wl,/OPT:REF");
  Arguments.push_back(TargetArgument);
  if (Freestanding) {
    Arguments.push_back(StartObject);
    Arguments.push_back("-nostdlib");
    if (Linux)
      Arguments.append({"-static", "-Wl,-e,_start"});
    else if (Triple.isWindowsMSVCEnvironment())
      Arguments.append({"-Wl,/entry:mainCRTStartup", "-lkernel32"});
    else
      Arguments.append({"-Wl,-e,mainCRTStartup", "-lkernel32"});
  }
  for (const auto &Source : LinkSources)
    Arguments.push_back(Source);
  std::vector<std::string> LibraryArguments;
  for (const auto &Library : LibraryNames) {
    const auto Name = llvm::StringRef(Library);
    if (Name.ends_with(".lib") || Name.ends_with(".dll"))
      LibraryArguments.push_back("-l" + Name.drop_back(4).str());
    else
      LibraryArguments.push_back("-l" + Library);
  }
  for (const auto &Library : LibraryArguments)
    Arguments.push_back(Library);
  for (const auto &Argument : Option::ClangArgs)
    Arguments.push_back(Argument);
  std::vector<std::string> SearchArguments;
  for (const auto &Path : LinkPath)
    SearchArguments.push_back("-L" + Path);
  for (const auto &Argument : SearchArguments)
    Arguments.push_back(Argument);
  Arguments.append({"-o", TemporaryOutput});
  std::string Message;
  const int Status =
      llvm::sys::ExecuteAndWait(*Linker, Arguments, std::nullopt, {}, 0, 0, &Message);
  if (Status != 0)
    return llvm::createStringError("%slinker failed%s: %s",
                                   Freestanding ? "freestanding " : "",
                                   Freestanding
                                       ? " (check unresolved libc/CRT/compiler-runtime symbols and "
                                         "explicitly supplied libraries)"
                                       : "",
                                   Message.empty() ? "non-zero exit status" : Message.c_str());
  if (auto ErrorCode = llvm::sys::fs::rename(TemporaryOutput, OutputPath))
    return llvm::createStringError(ErrorCode, "cannot write executable");
  RemoveOutput.releaseFile();
  return llvm::Error::success();
}

} // namespace kelyra::codegen::detail
