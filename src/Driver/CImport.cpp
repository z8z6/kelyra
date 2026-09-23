#include "Command.h"

#include "Lexer/Lexer.h"
#include "Support/Log.h"
#include "Support/Option.h"

#include "llvm/Support/FileSystem.h"
#include "llvm/Support/ToolOutputFile.h"

#include <sstream>
#include <vector>

using namespace kelyra;

std::optional<cimport::ImportResult>
driver::ImportCHeaders(const ModuleLoader &Loader) {
  if (Option::Progress)
    for (const auto &Header : Loader.GetCHeaders())
      kinfo() << "  [C header] " << Header << '\n';
  std::vector<std::string> Arguments(Option::CArguments.begin(),
                                     Option::CArguments.end());
  auto Declarations = cimport::ImportHeaders(Loader.GetCHeaders(), Arguments);
  if (!Declarations.Ok()) {
    for (const auto &Diagnostic : Declarations.Diagnostics)
      kerr() << Option::InputFile << ": error: " << Diagnostic << '\n';
    return std::nullopt;
  }
  return Declarations;
}

std::optional<int> driver::GenerateCDefinitionsIfRequested(
    const ModuleLoader &Loader, const cimport::ImportResult &Declarations) {
  if (!Option::EmitCDefinitions)
    return std::nullopt;
  if (Option::EmitMlir || Option::EmitObject || Option::EmitExecutable ||
      Option::LexDumpAst || Option::LexVerify) {
    kerr() << "--emit-c-defs cannot be combined with another action\n";
    return 2;
  }
  if (Option::CDefinitionsModule.empty() ||
      Option::OutputFile.getNumOccurrences() == 0) {
    kerr() << "--emit-c-defs requires --c-defs-module and -o\n";
    return 2;
  }
  auto Source = cimport::GenerateDefinitions(Declarations, Loader.GetCHeaders(),
                                             Option::CDefinitionsModule);
  auto Parsed = lex::Lexer().parse(Source, Option::OutputFile);
  if (!Parsed.ok()) {
    for (const auto &Diagnostic : Parsed.diagnostics) {
      std::ostringstream Message;
      Message << Diagnostic;
      kerr() << Message.str() << '\n';
    }
    return 1;
  }
  std::error_code Error;
  llvm::ToolOutputFile Output(Option::OutputFile, Error,
                              llvm::sys::fs::OF_None);
  if (Error) {
    kerr() << "cannot write Kelyra definitions: " << Error.message() << '\n';
    return 1;
  }
  Output.os() << Source;
  Output.keep();
  return 0;
}
