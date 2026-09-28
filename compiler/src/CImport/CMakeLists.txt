add_library(kelyra_cimport CImporter.cpp)
target_include_directories(kelyra_cimport PUBLIC "${PROJECT_SOURCE_DIR}/include")
target_include_directories(kelyra_cimport SYSTEM PRIVATE
  "${KELYRA_LLVM_SOURCE_DIR}/llvm/include"
  "${KELYRA_LLVM_BINARY_DIR}/include"
  "${KELYRA_LLVM_SOURCE_DIR}/clang/include"
  "${KELYRA_LLVM_BINARY_DIR}/tools/clang/include"
)
target_link_libraries(kelyra_cimport PUBLIC kelyra_sema PRIVATE
  clangTooling
  LLVMSupport
)
if(NOT LLVM_ENABLE_RTTI)
  target_compile_options(kelyra_cimport PRIVATE -fno-rtti)
endif()
