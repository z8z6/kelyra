add_library(kelyra_codegen Class.cpp Expression.cpp IRGen.cpp Statement.cpp)
get_directory_property(KELYRA_LLVM_NATIVE_ARCH
  DIRECTORY "${PROJECT_SOURCE_DIR}/third_party/llvm-project/llvm"
  DEFINITION LLVM_NATIVE_ARCH
)
target_include_directories(kelyra_codegen PUBLIC "${PROJECT_SOURCE_DIR}/include")
target_include_directories(kelyra_codegen SYSTEM PUBLIC
  "${PROJECT_SOURCE_DIR}/third_party/llvm-project/llvm/include"
  "${PROJECT_BINARY_DIR}/llvm/include"
  "${PROJECT_SOURCE_DIR}/third_party/llvm-project/mlir/include"
  "${PROJECT_BINARY_DIR}/llvm/tools/mlir/include"
)
target_link_libraries(kelyra_codegen PUBLIC
  LLVM${KELYRA_LLVM_NATIVE_ARCH}AsmParser
  LLVM${KELYRA_LLVM_NATIVE_ARCH}CodeGen
  LLVM${KELYRA_LLVM_NATIVE_ARCH}Desc
  LLVM${KELYRA_LLVM_NATIVE_ARCH}Info
  LLVMPasses
  kelyra_frontend
  kelyra_ir
  kelyra_sema
  MLIRArithDialect
  MLIRArithToLLVM
  MLIRBuiltinToLLVMIRTranslation
  MLIRControlFlowDialect
  MLIRControlFlowToLLVM
  MLIRFuncDialect
  MLIRFuncToLLVM
  MLIRIR
  MLIRLLVMDialect
  MLIRLLVMIRTransforms
  MLIRLLVMToLLVMIRTranslation
  MLIRPass
  MLIRReconcileUnrealizedCasts
  MLIRTargetLLVMIRExport
)
if(NOT LLVM_ENABLE_RTTI)
  target_compile_options(kelyra_codegen PRIVATE -fno-rtti)
endif()
