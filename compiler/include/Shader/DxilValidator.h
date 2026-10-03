#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace kelyra::shader {

// Validates LLVM's DXContainer and fills the required DXIL validator digest.
// This does not compile HLSL or generate shader instructions.
bool ValidateAndSignDxil(llvm::StringRef Path, std::string &Error);

} // namespace kelyra::shader
