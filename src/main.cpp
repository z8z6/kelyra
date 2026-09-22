#include "CImport/CImporter.h"
#include "CodeGen/IRGen.h"
#include "Driver/ModuleLoader.h"
#include "Lexer/Lexer.h"
#include "Sema/Sema.h"
#include "Support/Option.h"
#include "mlir/IR/MLIRContext.h"
#include "mlir/IR/Verifier.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/raw_ostream.h"
#include <iostream>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace llvm;
using namespace kelyra;

int main(int argc, char **argv) {
  cl::HideUnrelatedOptions(Option::KelyraCategory);
  if (!cl::ParseCommandLineOptions(argc, argv, "Kelyra compiler\n", &errs())) {
    errs() << "usage: kelyra [options] <file>\n";
    return 2;
  }
  const std::string &filename = Option::InputFile.getValue();
  ModuleLoader Loader(Option::Progress);
  for (const auto &ModulePath : Option::ModulePaths)
    Loader.AddModulePath(ModulePath);
  for (const auto &ExternalPath : Option::ExternalPaths)
    Loader.AddExternalPath(ExternalPath);
  if (!Loader.LoadEntry(filename))
    return 1;
  const auto &Modules = Loader.GetModules();
  const auto &Entry = Modules.front();
  std::vector CArguments(Option::CArguments.begin(), Option::CArguments.end());
  std::vector LinkSources(Option::CSources.begin(), Option::CSources.end());
  LinkSources.insert(LinkSources.end(), Option::LinkInputs.begin(),
                     Option::LinkInputs.end());
  if (Option::Progress)
    for (const auto &Header : Loader.GetCHeaders())
      std::cerr << "  [C header] " << Header << '\n';
  auto CDeclarations = cimport::ImportHeaders(Loader.GetCHeaders(), CArguments);
  if (!CDeclarations.Ok()) {
    for (const auto &Diagnostic : CDeclarations.Diagnostics)
      std::cerr << filename << ": error: " << Diagnostic << '\n';
    return 1;
  }
  if (Option::LexDumpAst)
    std::cout << lex::Lexer().dumpAst(*Entry.Parsed.root) << '\n';
  if (Option::EmitMlir || Option::EmitObject || Option::EmitExecutable) {
    sema::Sema analysis;
    std::vector<sema::ModuleInput> Inputs;
    std::vector<const lex::Node *> Asts;
    std::set<const lex::Node *> External;
    for (const auto &Module : Modules) {
      Inputs.push_back({Module.Parsed.root.get(), Module.IsEntry});
      Asts.push_back(Module.Parsed.root.get());
      if (Module.IsExternal)
        External.insert(Module.Parsed.root.get());
    }
    if (Option::Progress)
      std::cerr << "  [check] " << Modules.size() << " Kelyra module(s)\n";
    const bool Valid = analysis.CheckModules(Inputs, CDeclarations.Functions,
                                             CDeclarations.Types) &&
                       (!Option::EmitExecutable ||
                        analysis.CheckEntrypoint(*Entry.Parsed.root));
    if (!Valid) {
      for (const auto &diagnostic : analysis.GetDiagnostics())
        std::cerr << diagnostic << '\n';
      return 1;
    }
    mlir::MLIRContext context;
    if (Option::Progress)
      std::cerr << "  [codegen] Generating native code\n";
    codegen::IRGen generator(context, analysis, Option::SafeLevel,
                             Option::OptLevel == llvm::CodeGenOptLevel::None);
    generator.SetExternalModules(External);
    auto module = generator.Generate(Asts);
    std::string CWrapperSource;
    for (const auto &Wrapper : analysis.GetCWrappers())
      CWrapperSource += Wrapper.Source;
    if (mlir::failed(mlir::verify(*module)))
      return 1;
    if (Option::Progress && (Option::EmitObject || Option::EmitExecutable)) {
      for (const auto &Source : Option::CSources)
        std::cerr << "  [C source] " << Source << '\n';
      for (const auto &Input : Option::LinkInputs)
        std::cerr << "  [link input] " << Input << '\n';
      std::cerr << (Option::EmitExecutable ? "  [compile/link] "
                                           : "  [object] ")
                << Option::OutputFile << '\n';
    }
    if (Option::EmitMlir) {
      module->print(outs());
      outs() << '\n';
    } else if (auto Error =
                   Option::EmitObject
                       ? codegen::EmitObject(*module, Option::OutputFile,
                                             Option::OptLevel, CWrapperSource,
                                             Option::CArguments)
                       : codegen::EmitExecutable(
                             *module, Option::OutputFile, Option::OptLevel,
                             LinkSources, Option::CArguments, CWrapperSource)) {
      errs() << filename << ": error: " << toString(std::move(Error)) << '\n';
      return 1;
    }
  }
  return 0;
}
