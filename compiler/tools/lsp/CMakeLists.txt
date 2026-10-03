file(GLOB_RECURSE KELYRA_LS_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/*.h"
)
add_executable(kelyra-ls ${KELYRA_LS_SOURCES})
target_compile_definitions(kelyra-ls PRIVATE
  KELYRA_STDLIB_SOURCE_DIR="${KELYRA_STDLIB_SOURCE_DIR}")
target_include_directories(kelyra-ls SYSTEM PRIVATE
  "${KELYRA_LLVM_SOURCE_DIR}/llvm/include"
  "${KELYRA_LLVM_BINARY_DIR}/include"
)
target_link_libraries(kelyra-ls PRIVATE
  kelyra_frontend
  LLVMSupportLSP
)
if(NOT LLVM_ENABLE_RTTI)
  if(MSVC)
    target_compile_options(kelyra-ls PRIVATE /GR-)
  else()
    target_compile_options(kelyra-ls PRIVATE -fno-rtti)
  endif()
endif()
set_target_properties(kelyra-ls PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
