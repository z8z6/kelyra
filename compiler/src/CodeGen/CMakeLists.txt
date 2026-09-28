add_library(kelyra_codegen Class.cpp Debug.cpp Expression.cpp IRGen.cpp
  Option.cpp RuntimeReflection.cpp Statement.cpp)
get_directory_property(KELYRA_LLVM_NATIVE_ARCH
  DIRECTORY "${KELYRA_LLVM_SOURCE_DIR}/llvm"
  DEFINITION LLVM_NATIVE_ARCH
)
target_include_directories(kelyra_codegen PUBLIC "${PROJECT_SOURCE_DIR}/include")
target_compile_definitions(kelyra_codegen PRIVATE
  KELYRA_RUNTIME_DIR="${PROJECT_SOURCE_DIR}/runtime")
target_include_directories(kelyra_codegen SYSTEM PUBLIC
  "${KELYRA_LLVM_SOURCE_DIR}/llvm/include"
  "${KELYRA_LLVM_BINARY_DIR}/include"
  "${KELYRA_LLVM_SOURCE_DIR}/mlir/include"
  "${KELYRA_LLVM_BINARY_DIR}/tools/mlir/include"
)
target_link_libraries(kelyra_codegen PUBLIC
  LLVM${KELYRA_LLVM_NATIVE_ARCH}AsmParser
  LLVM${KELYRA_LLVM_NATIVE_ARCH}CodeGen
  LLVM${KELYRA_LLVM_NATIVE_ARCH}Desc
  LLVM${KELYRA_LLVM_NATIVE_ARCH}Info
  LLVMPasses
  LLVMIRReader
  LLVMLinker
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
