#include "Command.h"
#include "Driver/Generics.h"

#include "Support/Log.h"
#include "Support/Option.h"

using namespace kelyra;

bool driver::ValidateOptions() {
  if (Option::Progress && LogLevelOpt.getNumOccurrences() == 0)
    LogLevelOpt = LogLevel::Info;
  if (Option::Runtime != "host" && Option::Runtime != "freestanding") {
    kerr() << "--runtime must be 'host' or 'freestanding'\n";
    return false;
  }
  if (Option::Runtime == "freestanding" && !Option::EmitExecutable &&
      !Option::EmitObject) {
    kerr() << "--runtime=freestanding requires --emit-exe or --emit-obj\n";
    return false;
  }
  return true;
}

int driver::Run() {
  if (!ValidateOptions())
    return 2;
  auto Loader = LoadModules();
  if (!Loader)
    return 1;
  auto Declarations = ImportCHeaders(*Loader);
  if (!Declarations)
    return 1;
  if (auto Result = GenerateCDefinitionsIfRequested(*Loader, *Declarations))
    return *Result;
  DumpAstIfRequested(*Loader);
  if (!WantsCompilation())
    return 0;
  if (!ExpandGenerics(Loader->GetMutableModules()))
    return 1;
  sema::Sema Analysis;
  if (!Analyze(*Loader, *Declarations, Analysis))
    return 1;
  if (!Option::EmitMlir && !Option::EmitObject && !Option::EmitExecutable)
    return 0;
  return Emit(*Loader, Analysis);
}
