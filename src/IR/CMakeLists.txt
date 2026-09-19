list(APPEND CMAKE_MODULE_PATH
  "${PROJECT_SOURCE_DIR}/third_party/llvm-project/llvm/cmake/modules"
  "${PROJECT_SOURCE_DIR}/third_party/llvm-project/mlir/cmake/modules"
)
include(AddMLIR)

set(LLVM_TARGET_DEFINITIONS Kelyra.td)
set(KELYRA_TABLEGEN_INCLUDES
  "${PROJECT_SOURCE_DIR}/third_party/llvm-project/mlir/include"
  "${PROJECT_SOURCE_DIR}/third_party/llvm-project/llvm/include"
)
mlir_tablegen(KelyraDialect.h.inc -gen-dialect-decls -dialect=kelyra
  EXTRA_INCLUDES ${KELYRA_TABLEGEN_INCLUDES})
mlir_tablegen(KelyraDialect.cpp.inc -gen-dialect-defs -dialect=kelyra
  EXTRA_INCLUDES ${KELYRA_TABLEGEN_INCLUDES})
mlir_tablegen(KelyraTypes.h.inc -gen-typedef-decls
  EXTRA_INCLUDES ${KELYRA_TABLEGEN_INCLUDES})
mlir_tablegen(KelyraTypes.cpp.inc -gen-typedef-defs
  EXTRA_INCLUDES ${KELYRA_TABLEGEN_INCLUDES})
mlir_tablegen(KelyraOps.h.inc -gen-op-decls
  EXTRA_INCLUDES ${KELYRA_TABLEGEN_INCLUDES})
mlir_tablegen(KelyraOps.cpp.inc -gen-op-defs
  EXTRA_INCLUDES ${KELYRA_TABLEGEN_INCLUDES})
add_public_tablegen_target(KelyraOpsIncGen)

add_library(kelyra_ir Kelyra.cpp)
add_dependencies(kelyra_ir KelyraOpsIncGen)
if(NOT LLVM_ENABLE_RTTI)
  if(MSVC)
    target_compile_options(kelyra_ir PRIVATE /GR-)
  else()
    target_compile_options(kelyra_ir PRIVATE -fno-rtti)
  endif()
endif()
target_include_directories(kelyra_ir PUBLIC
  "${PROJECT_SOURCE_DIR}/include"
  "${CMAKE_CURRENT_BINARY_DIR}"
)
target_include_directories(kelyra_ir SYSTEM PUBLIC
  "${PROJECT_SOURCE_DIR}/third_party/llvm-project/llvm/include"
  "${PROJECT_BINARY_DIR}/llvm/include"
  "${PROJECT_SOURCE_DIR}/third_party/llvm-project/mlir/include"
  "${PROJECT_BINARY_DIR}/llvm/tools/mlir/include"
)
target_link_libraries(kelyra_ir PUBLIC
  MLIRIR
  MLIRInferTypeOpInterface
  MLIRSideEffectInterfaces
)
