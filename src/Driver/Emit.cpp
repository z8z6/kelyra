#include "Command.h"

#include "CodeGen/IRGen.h"
#include "Support/Log.h"
#include "Support/Option.h"

#include "mlir/IR/MLIRContext.h"
#include "mlir/IR/Verifier.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/raw_ostream.h"

#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace kelyra;

int driver::Emit(const ModuleLoader &Loader, const sema::Sema &Analysis) {
  const auto &Modules = Loader.GetModules();
  std::vector<const lex::Node *> Asts;
  std::set<const lex::Node *> External;
  for (const auto &Module : Modules) {
    Asts.push_back(Module.Parsed.root.get());
    if (Module.IsExternal)
      External.insert(Module.Parsed.root.get());
  }
  mlir::MLIRContext Context;
  if (Option::Progress)
    kinfo() << "  [codegen] Generating native code\n";
  codegen::IRGen Generator(Context, Analysis, Option::SafeLevel,
                           Option::OptLevel == llvm::CodeGenOptLevel::None);
  Generator.SetExternalModules(External);
  auto Module = Generator.Generate(Asts);
  std::string CWrapperSource;
  for (const auto &Wrapper : Analysis.GetCWrappers())
    CWrapperSource += Wrapper.Source;
  if (mlir::failed(mlir::verify(*Module)))
    return 1;
  if (Option::Progress && (Option::EmitObject || Option::EmitExecutable)) {
    for (const auto &Source : Option::CSources)
      kinfo() << "  [C source] " << Source << '\n';
    for (const auto &Input : Option::LinkInputs)
      kinfo() << "  [link input] " << Input << '\n';
    kinfo() << (Option::EmitExecutable ? "  [compile/link] " : "  [object] ")
            << Option::OutputFile << '\n';
  }
  if (Option::EmitMlir) {
    Module->print(llvm::outs());
    llvm::outs() << '\n';
    return 0;
  }
  const auto Runtime = Option::Runtime == "freestanding"
                           ? codegen::RuntimeMode::Freestanding
                           : codegen::RuntimeMode::Host;
  auto Error = [&]() -> llvm::Error {
    if (Option::EmitObject)
      return codegen::EmitObject(*Module, Option::OutputFile, Option::OptLevel,
                                 CWrapperSource, Option::CArguments,
                                 Option::CSources, Option::Target, Runtime);
    std::vector<std::string> LinkSources(Option::CSources.begin(),
                                         Option::CSources.end());
    LinkSources.insert(LinkSources.end(), Option::LinkInputs.begin(),
                       Option::LinkInputs.end());
    return codegen::EmitExecutable(
        *Module, Option::OutputFile, Option::OptLevel, LinkSources,
        Option::CArguments, CWrapperSource, Runtime, Option::Target);
  }();
  if (Error) {
    kerr() << Option::InputFile
           << ": error: " << llvm::toString(std::move(Error)) << '\n';
    return 1;
  }
  return 0;
}
