cmake_minimum_required(VERSION 3.20)
project(KelyraWorkspace LANGUAGES C CXX)

set(KELYRA_LLVM_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/kelyra-llvm")
set(KELYRA_LLVM_BINARY_DIR "${CMAKE_BINARY_DIR}/llvm")
set(KELYRA_STDLIB_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/kstd/src" CACHE PATH
    "Kelyra standard library source directory")

if(NOT EXISTS "${KELYRA_LLVM_SOURCE_DIR}/llvm/CMakeLists.txt")
  message(FATAL_ERROR "Kelyra LLVM fork is missing. Initialize kelyra-llvm.")
endif()

set(LLVM_ENABLE_PROJECTS "mlir;clang" CACHE STRING "LLVM projects to build")
set(LLVM_TARGETS_TO_BUILD "Native;DirectX;SPIRV" CACHE STRING "LLVM target backends")
set(LLVM_INCLUDE_TESTS OFF CACHE BOOL "Build LLVM tests")
set(LLVM_INCLUDE_BENCHMARKS OFF CACHE BOOL "Build LLVM benchmarks")
set(MLIR_INCLUDE_TESTS OFF CACHE BOOL "Build MLIR tests")

include(CTest)
add_subdirectory(kelyra-llvm/llvm llvm EXCLUDE_FROM_ALL)
add_subdirectory(compiler)

option(KELYRA_BUILD_KELP "Build the Kelp package manager" ON)
if(KELYRA_BUILD_KELP)
  add_subdirectory(kelp)
endif()
