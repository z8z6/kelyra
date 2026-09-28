#include "Command.h"

#include "Support/Option.h"

using namespace kelyra;

std::unique_ptr<ModuleLoader> driver::LoadModules() {
  auto Loader =
      std::make_unique<ModuleLoader>(Option::Progress, Option::Target);
  for (const auto &Path : Option::ModulePaths)
    Loader->AddModulePath(Path);
  for (const auto &Path : Option::ExternalPaths)
    Loader->AddExternalPath(Path);
  if (!Loader->LoadEntry(Option::InputFile))
    return nullptr;
  return Loader;
}
