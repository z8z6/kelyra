#pragma once

#include "Driver/ModuleLoader.h"

#include <deque>

namespace kelyra {
// Materialize the explicitly requested generic classes and functions before
// semantic analysis. The generated declarations remain in their source module.
bool ExpandGenerics(std::deque<SourceModule> &Modules);
} // namespace kelyra
