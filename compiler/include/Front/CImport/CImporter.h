#pragma once

#include "Front/Sema/CDeclarations.h"

#include <string>
#include <vector>

namespace kelyra::cimport {
struct ImportResult : sema::CDeclarations {
  std::vector<std::string> Diagnostics;

  bool Ok() const { return Diagnostics.empty(); }
};

ImportResult ImportHeaders(const std::vector<std::string> &Headers,
                           const std::vector<std::string> &Arguments = {},
                           bool CaptureHeaders = false);
std::string GenerateDefinitions(const ImportResult &Declarations,
                                const std::vector<std::string> &Headers,
                                const std::string &ModuleName);
} // namespace kelyra::cimport
