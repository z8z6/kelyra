add_library(kelyra_cimport CImporter.cpp)
target_include_directories(kelyra_cimport PUBLIC "${PROJECT_SOURCE_DIR}/include")
target_include_directories(kelyra_cimport SYSTEM PRIVATE
  "${PROJECT_SOURCE_DIR}/third_party/llvm-project/llvm/include"
  "${PROJECT_BINARY_DIR}/llvm/include"
  "${PROJECT_SOURCE_DIR}/third_party/llvm-project/clang/include"
  "${PROJECT_BINARY_DIR}/llvm/tools/clang/include"
)
target_link_libraries(kelyra_cimport PUBLIC kelyra_sema PRIVATE
  clangTooling
  LLVMSupport
)
if(NOT LLVM_ENABLE_RTTI)
  target_compile_options(kelyra_cimport PRIVATE -fno-rtti)
endif()
