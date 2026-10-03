#pragma once

#include "Front/AST/AST.h"
#include <string>

namespace kelyra {
// Apply module/declaration cfg annotations before resolving imports.
bool ApplyTargetConditions(lex::Node &Root, const std::string &Path,
                           const std::string &TargetTriple, bool &ModuleEnabled);
} // namespace kelyra
