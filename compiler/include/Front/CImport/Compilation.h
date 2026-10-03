#pragma once
#include "Front/CImport/CImporter.h"
#include "Front/Module/Module.h"
#include <optional>
namespace kelyra::cimport {
bool ValidateCompilation();
std::optional<ImportResult> ImportCHeaders(const ModuleLoader &Loader);
bool PrepareDefinitions(const ModuleLoader &Loader, const ImportResult &Declarations,
                        std::optional<std::string> &Source);
int EmitDefinitions(const std::string &Source);
} // namespace kelyra::cimport
