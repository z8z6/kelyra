#include "Command.h"

#include "Lexer/Lexer.h"
#include "Support/Log.h"
#include "Support/Option.h"

#include <iostream>
#include <sstream>
#include <vector>

using namespace kelyra;

void driver::DumpAstIfRequested(const ModuleLoader &Loader) {
  if (Option::LexDumpAst)
    std::cout << lex::Lexer().dumpAst(*Loader.GetModules().front().Parsed.root)
              << '\n';
}

bool driver::WantsCompilation() {
  return Option::LexVerify || Option::EmitMlir || Option::EmitObject ||
         Option::EmitExecutable;
}

bool driver::Analyze(const ModuleLoader &Loader,
                     const cimport::ImportResult &Declarations,
                     sema::Sema &Analysis) {
  const auto &Modules = Loader.GetModules();
  std::vector<sema::ModuleInput> Inputs;
  for (const auto &Module : Modules)
    Inputs.push_back(
        {Module.Parsed.root.get(), Module.IsEntry, Module.IsExternal});
  if (Option::Progress)
    kinfo() << "  [check] " << Modules.size() << " Kelyra module(s)\n";
  const bool Valid =
      Analysis.CheckModules(Inputs, Declarations.Functions,
                            Declarations.Types) &&
      (!(Option::EmitExecutable ||
         (Option::EmitObject && Option::Runtime == "freestanding")) ||
       Analysis.CheckEntrypoint(*Modules.front().Parsed.root));
  for (const auto &Warning : Analysis.GetWarnings())
    kwarn() << Warning.Loc.File << ':' << Warning.Loc.Line << ':'
            << Warning.Loc.Column << ": warning: " << Warning.Message << '\n';
  if (!Valid)
    for (const auto &Diagnostic : Analysis.GetDiagnostics()) {
      std::ostringstream Message;
      Message << Diagnostic;
      kerr() << Message.str() << '\n';
    }
  return Valid;
}
