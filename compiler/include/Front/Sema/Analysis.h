#pragma once
#include "Front/CImport/CImporter.h"
#include "Front/Module/Module.h"
#include "Front/Sema/Sema.h"
namespace kelyra::sema {
bool Analyze(const ModuleLoader &Loader, const cimport::ImportResult &Declarations, Sema &Analysis,
             bool RequireEntrypoint);
} // namespace kelyra::sema
