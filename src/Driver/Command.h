#pragma once

#include "CImport/CImporter.h"
#include "Driver/ModuleLoader.h"
#include "Sema/Sema.h"

#include <memory>
#include <optional>

namespace kelyra::driver {
bool ValidateOptions();
std::unique_ptr<ModuleLoader> LoadModules();
std::optional<cimport::ImportResult> ImportCHeaders(const ModuleLoader &Loader);
std::optional<int>
GenerateCDefinitionsIfRequested(const ModuleLoader &Loader,
                                const cimport::ImportResult &Declarations);
void DumpAstIfRequested(const ModuleLoader &Loader);
void DumpClassLayouts(const sema::Sema &Analysis);
bool WantsCompilation();
bool Analyze(const ModuleLoader &Loader,
             const cimport::ImportResult &Declarations, sema::Sema &Analysis);
int Emit(const ModuleLoader &Loader, const sema::Sema &Analysis);
int Run();
} // namespace kelyra::driver
