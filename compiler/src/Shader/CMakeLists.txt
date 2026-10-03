file(GLOB_RECURSE KELYRA_SHADER_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/*.h"
  "${PROJECT_SOURCE_DIR}/include/Shader/*.h"
)
add_library(kelyra_shader ${KELYRA_SHADER_SOURCES})
target_include_directories(kelyra_shader PUBLIC "${PROJECT_SOURCE_DIR}/include")
target_link_libraries(kelyra_shader PUBLIC kelyra_ir kelyra_sema PRIVATE
  MLIRFuncDialect MLIRSPIRVDialect MLIRSPIRVSerialization MLIRParser
  LLVMDirectXCodeGen LLVMDirectXDesc LLVMDirectXInfo LLVMDirectXIRPasses)
if(NOT LLVM_ENABLE_RTTI)
  if(MSVC)
    target_compile_options(kelyra_shader PRIVATE /GR-)
  else()
    target_compile_options(kelyra_shader PRIVATE -fno-rtti)
  endif()
endif()
