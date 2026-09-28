add_executable(kelyra-ls main.cpp)
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
  target_compile_options(kelyra-ls PRIVATE -fno-rtti)
endif()
set_target_properties(kelyra-ls PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
