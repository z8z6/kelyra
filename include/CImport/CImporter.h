#pragma once

#include "Sema/Sema.h"

#include <string>
#include <vector>

namespace kelyra::cimport {
struct ImportResult {
  std::vector<sema::ExternalFunction> Functions;
  std::vector<sema::ExternalType> Types;
  std::vector<std::string> Diagnostics;

  bool Ok() const { return Diagnostics.empty(); }
};

ImportResult ImportHeaders(const std::vector<std::string> &Headers,
                           const std::vector<std::string> &Arguments = {});
} // namespace kelyra::cimport
