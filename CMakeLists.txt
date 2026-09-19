cmake_minimum_required(VERSION 3.20)
project(Kelyra LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20 CACHE STRING "C++ standard")
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 添加 llvm 和 mlir 依赖
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/llvm-project/llvm/CMakeLists.txt")
  message(FATAL_ERROR "LLVM submodule is missing. Run: git submodule update --init --recursive")
endif()

set(LLVM_ENABLE_PROJECTS "mlir;clang" CACHE STRING "LLVM projects to build")
if(NOT "mlir" IN_LIST LLVM_ENABLE_PROJECTS OR NOT "clang" IN_LIST LLVM_ENABLE_PROJECTS)
  message(FATAL_ERROR "Kelyra requires mlir and clang in LLVM_ENABLE_PROJECTS")
endif()
set(LLVM_TARGETS_TO_BUILD "Native" CACHE STRING "LLVM target backends")
set(LLVM_INCLUDE_TESTS OFF CACHE BOOL "Build LLVM tests")
set(LLVM_INCLUDE_BENCHMARKS OFF CACHE BOOL "Build LLVM benchmarks")
set(MLIR_INCLUDE_TESTS OFF CACHE BOOL "Build MLIR tests")

# Only build the LLVM/MLIR dependencies needed by Kelyra by default.
add_subdirectory(third_party/llvm-project/llvm llvm EXCLUDE_FROM_ALL)

add_subdirectory(src/IR)

add_library(kelyra_frontend
        src/Lexer/Diagnostic.cpp
        src/Lexer/Formatter.cpp
        src/Lexer/Lexer.cpp
        src/Lexer/Token.cpp
        include/Lexer/Diagnostic.h
        include/Lexer/Token.h
)
target_include_directories(kelyra_frontend PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_subdirectory(src/Sema)
add_subdirectory(src/CImport)
add_subdirectory(src/CodeGen)

add_executable(kelyra src/main.cpp)
target_link_libraries(kelyra PRIVATE
  kelyra_codegen
  kelyra_cimport
  kelyra_frontend
  kelyra_ir
  kelyra_sema
  MLIRFuncDialect
)
if(NOT LLVM_ENABLE_RTTI)
  target_compile_options(kelyra PRIVATE -fno-rtti)
endif()
set_target_properties(kelyra PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/bin"
)

add_subdirectory(tools/format)
add_subdirectory(tools/lsp)

include(CTest)
if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
