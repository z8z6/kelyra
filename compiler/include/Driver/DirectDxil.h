#pragma once

#include "mlir/IR/BuiltinOps.h"
#include "llvm/ADT/StringRef.h"
#include <string>

namespace kelyra::driver {

// Lowers the backend-neutral Shader IR to a DXContainer through LLVM's
// DirectX target. No HLSL text or external shader compiler is involved.
bool EmitDirectDxil(mlir::ModuleOp ShaderModule, llvm::StringRef OutputPath,
                    std::string &Error);

} // namespace kelyra::driver
