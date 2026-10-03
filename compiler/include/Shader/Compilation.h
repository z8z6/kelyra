#pragma once
#include "Front/Module/Module.h"
#include "Front/Sema/Sema.h"
namespace kelyra::shader {
bool ValidateOutput(bool ExclusiveAction = false);
// Compile every annotated source function; external KMI modules are not owned here.
int Emit(const ModuleLoader &Loader, const sema::Sema &Analysis, bool NativeOutput);
} // namespace kelyra::shader
