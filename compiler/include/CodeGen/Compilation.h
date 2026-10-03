#pragma once
#include "Front/Module/Module.h"
#include "Front/Sema/Sema.h"
#include <optional>
namespace kelyra::codegen {
// Interpreted requirements of the native stage; command-line options remain private.
struct CompilationPlan {
  bool NativeOutput = false;
  bool RequiresEntrypoint = false;
};
std::optional<CompilationPlan> PrepareCompilation(const ModuleLoader &Loader,
                                                  bool ExclusiveAction = false);
int Emit(const ModuleLoader &Loader, const sema::Sema &Analysis);
} // namespace kelyra::codegen
