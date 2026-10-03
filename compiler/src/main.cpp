#include "CodeGen/Compilation.h"
#include "Front/CImport/Compilation.h"
#include "Front/Expansion/Expansion.h"
#include "Front/Module/ModuleInterface.h"
#include "Front/Parser/Parser.h"
#include "Front/Sema/Analysis.h"
#include "Shader/Compilation.h"
#include "Support/Option.h"
#include "llvm/Support/CommandLine.h"

using namespace kelyra;

int main(int Argc, char **Argv) {
  llvm::cl::HideUnrelatedOptions(Option::KelyraCategory);
  if (!llvm::cl::ParseCommandLineOptions(Argc, Argv, "Kelyra compiler\n", &llvm::errs())) {
    llvm::cl::PrintHelpMessage(true, true);
    return 2;
  }
  if (!cimport::ValidateCompilation())
    return 2;
  ModuleLoader Loader;
  if (!Loader.Load())
    return 1;
  auto Declarations = cimport::ImportCHeaders(Loader);
  if (!Declarations)
    return 1;
  std::optional<std::string> Definitions;
  if (!cimport::PrepareDefinitions(Loader, *Declarations, Definitions))
    return 1;
  const bool ExclusiveAction = Definitions.has_value();
  const auto NativePlan = codegen::PrepareCompilation(Loader, ExclusiveAction);
  if (!NativePlan || !shader::ValidateOutput(ExclusiveAction) ||
      !lex::ValidateOutput(ExclusiveAction))
    return 2;
  ModuleInterfaceOutput Interface;
  if (!Interface.Prepare(Loader, *Declarations, ExclusiveAction))
    return 2;
  if (Definitions)
    return cimport::EmitDefinitions(*Definitions);
  Loader.GetEntry().Lex.DumpAstIfRequested();
  if (!ExpandModules(Loader.GetModules()))
    return 1;
  sema::Sema Analysis(Option::Target.getValue());
  if (!sema::Analyze(Loader, *Declarations, Analysis, NativePlan->RequiresEntrypoint))
    return 1;
  if (const auto Result = shader::Emit(Loader, Analysis, NativePlan->NativeOutput))
    return Result;
  if (const auto Result = codegen::Emit(Loader, Analysis))
    return Result;
  return Interface.Publish() ? 0 : 1;
}
