cmake_minimum_required(VERSION 3.20)
project(Kelyra LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17 CACHE STRING "C++ standard")
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/llvm-project/llvm/CMakeLists.txt")
  message(FATAL_ERROR "LLVM submodule is missing. Run: git submodule update --init --recursive")
endif()

set(LLVM_ENABLE_PROJECTS "mlir" CACHE STRING "LLVM projects to build")
if(NOT "mlir" IN_LIST LLVM_ENABLE_PROJECTS)
  message(FATAL_ERROR "Kelyra requires mlir in LLVM_ENABLE_PROJECTS")
endif()
set(LLVM_TARGETS_TO_BUILD "Native" CACHE STRING "LLVM target backends")
set(LLVM_INCLUDE_TESTS OFF CACHE BOOL "Build LLVM tests")
set(LLVM_INCLUDE_BENCHMARKS OFF CACHE BOOL "Build LLVM benchmarks")
set(MLIR_INCLUDE_TESTS OFF CACHE BOOL "Build MLIR tests")

# Only build the LLVM/MLIR dependencies needed by Kelyra by default.
add_subdirectory(third_party/llvm-project/llvm llvm EXCLUDE_FROM_ALL)

add_executable(kelyra src/main.cpp)
target_include_directories(kelyra SYSTEM PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/third_party/llvm-project/llvm/include"
  "${CMAKE_CURRENT_BINARY_DIR}/llvm/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/third_party/llvm-project/mlir/include"
  "${CMAKE_CURRENT_BINARY_DIR}/llvm/tools/mlir/include"
)
target_link_libraries(kelyra PRIVATE MLIRIR LLVMSupport)
set_target_properties(kelyra PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/bin"
)

include(CTest)
if(BUILD_TESTING)
  add_test(NAME kelyra-smoke COMMAND kelyra)
  set_tests_properties(kelyra-smoke PROPERTIES PASS_REGULAR_EXPRESSION "module")
endif()
