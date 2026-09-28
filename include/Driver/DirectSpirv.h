#pragma once

#include "mlir/IR/BuiltinOps.h"
#include "llvm/ADT/StringRef.h"

#include <string>

namespace kelyra::driver {

// Lowers a verified Kelyra Shader IR module to the MLIR SPIR-V dialect and
// serializes it directly to a SPIR-V binary. No GLSL compiler is involved.
bool EmitDirectSpirv(mlir::ModuleOp Module, llvm::StringRef Output,
                     std::string &Error);

} // namespace kelyra::driver
