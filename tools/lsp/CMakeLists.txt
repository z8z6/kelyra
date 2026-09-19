add_executable(kelyra-ls main.cpp)
target_include_directories(kelyra-ls SYSTEM PRIVATE
  "${PROJECT_SOURCE_DIR}/third_party/llvm-project/llvm/include"
  "${PROJECT_BINARY_DIR}/llvm/include"
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
