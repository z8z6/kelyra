file(GLOB_RECURSE KELYRA_CIMPORT_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/*.h"
  "${PROJECT_SOURCE_DIR}/include/Front/CImport/*.h"
)
add_library(kelyra_cimport ${KELYRA_CIMPORT_SOURCES})
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
  if(MSVC)
    target_compile_options(kelyra_cimport PRIVATE /GR-)
  else()
    target_compile_options(kelyra_cimport PRIVATE -fno-rtti)
  endif()
endif()
