#include "Front/CImport/Compilation.h"

#include "Front/Parser/Parser.h"
#include "Support/Log.h"
#include "Support/Option.h"

#include "llvm/Support/FileSystem.h"
#include "llvm/Support/ToolOutputFile.h"

#include <sstream>
#include <utility>
#include <vector>

using namespace kelyra;

namespace {
llvm::cl::opt<bool> EmitCDefinitions{
    "emit-c-defs",
    llvm::cl::desc("Generate an importable Kelyra wrapper for C headers"),
    llvm::cl::cat(Option::KelyraCategory)};
llvm::cl::opt<std::string> CDefinitionsModule{"c-defs-module",
                                              llvm::cl::desc("Module name for --emit-c-defs"),
                                              llvm::cl::cat(Option::KelyraCategory)};
} // namespace

std::optional<cimport::ImportResult> cimport::ImportCHeaders(const ModuleLoader &Loader) {
  std::vector<std::string> Arguments(Option::ClangArgs.begin(), Option::ClangArgs.end());
  Arguments.push_back("--target=" + Option::Target.getValue());
  auto Declarations = cimport::ImportHeaders(Loader.GetCHeaders(), Arguments, Loader.IsLibrary());
  if (!Declarations.Ok()) {
    for (const auto &Diagnostic : Declarations.Diagnostics)
      kerr() << Option::InputFile << ": error: " << Diagnostic << '\n';
    return std::nullopt;
  }
  if (!Declarations.Merge(Loader.LibraryDeclarations))
    return std::nullopt;
  return Declarations;
}

bool cimport::PrepareDefinitions(const ModuleLoader &Loader, const ImportResult &Declarations,
                                 std::optional<std::string> &Source) {
  Source.reset();
  if (!EmitCDefinitions)
    return true;
  auto Text = cimport::GenerateDefinitions(Declarations, Loader.GetCHeaders(), CDefinitionsModule);
  auto Parsed = lex::Parser().parse(Text, Option::OutputFile);
  if (!Parsed.ok()) {
    for (const auto &Diagnostic : Parsed.diagnostics) {
      std::ostringstream Message;
      Message << Diagnostic;
      kerr() << Message.str() << '\n';
    }
    return false;
  }
  Source = std::move(Text);
  return true;
}

int cimport::EmitDefinitions(const std::string &Source) {
  std::error_code Error;
  llvm::ToolOutputFile Output(Option::OutputFile, Error, llvm::sys::fs::OF_None);
  if (Error) {
    kerr() << "cannot write Kelyra definitions: " << Error.message() << '\n';
    return 1;
  }
  Output.os() << Source;
  Output.keep();
  return 0;
}

bool cimport::ValidateCompilation() {
  if (EmitCDefinitions && (CDefinitionsModule.empty() || Option::OutputFile.empty())) {
    kerr() << "--emit-c-defs requires --c-defs-module and -o\n";
    return false;
  }
  if (!EmitCDefinitions && !CDefinitionsModule.empty()) {
    kerr() << "--c-defs-module requires --emit-c-defs\n";
    return false;
  }
  return true;
}
