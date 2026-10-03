cmake_minimum_required(VERSION 3.20)
project(Kelyra LANGUAGES C CXX)

get_filename_component(KELYRA_WORKSPACE_ROOT
  "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)

set(KELYRA_STDLIB_SOURCE_DIR "${PROJECT_SOURCE_DIR}/../kstd/src" CACHE PATH
  "Kelyra standard library source directory")

if(NOT DEFINED KELYRA_LLVM_SOURCE_DIR)
  get_filename_component(KELYRA_LLVM_SOURCE_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/../kelyra-llvm" ABSOLUTE)
endif()
if(NOT DEFINED KELYRA_LLVM_BINARY_DIR)
  set(KELYRA_LLVM_BINARY_DIR "${CMAKE_CURRENT_BINARY_DIR}/llvm")
endif()

if(NOT DEFINED CMAKE_CXX_STANDARD OR CMAKE_CXX_STANDARD LESS 20)
  set(CMAKE_CXX_STANDARD 20)
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 添加 llvm 和 mlir 依赖
if(NOT EXISTS "${KELYRA_LLVM_SOURCE_DIR}/llvm/CMakeLists.txt")
  message(FATAL_ERROR "Kelyra LLVM fork is missing. Initialize kelyra-llvm.")
endif()

set(LLVM_ENABLE_PROJECTS "mlir;clang" CACHE STRING "LLVM projects to build")
if(NOT "mlir" IN_LIST LLVM_ENABLE_PROJECTS OR NOT "clang" IN_LIST LLVM_ENABLE_PROJECTS)
  message(FATAL_ERROR "Kelyra requires mlir and clang in LLVM_ENABLE_PROJECTS")
endif()
set(LLVM_TARGETS_TO_BUILD "Native;DirectX;SPIRV" CACHE STRING "LLVM target backends")
set(LLVM_INCLUDE_TESTS OFF CACHE BOOL "Build LLVM tests")
set(LLVM_INCLUDE_BENCHMARKS OFF CACHE BOOL "Build LLVM benchmarks")
set(MLIR_INCLUDE_TESTS OFF CACHE BOOL "Build MLIR tests")

# Only build the LLVM/MLIR dependencies needed by Kelyra by default.
if(NOT TARGET LLVMCore)
  add_subdirectory("${KELYRA_LLVM_SOURCE_DIR}/llvm"
    "${KELYRA_LLVM_BINARY_DIR}" EXCLUDE_FROM_ALL)
endif()

if(MSVC)
  add_compile_options(/EHsc)
endif()

add_subdirectory(src/IR)
add_subdirectory(src/Front)
add_subdirectory(src/CodeGen)
add_subdirectory(src/Shader)

file(GLOB KELYRA_ENTRY_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
)
set(KELYRA_COMPILER_SOURCES)
foreach(Module IN ITEMS Support Front/Module Front/Expansion)
  file(GLOB_RECURSE ModuleSources CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/${Module}/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/${Module}/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/${Module}/*.h"
  )
  list(APPEND KELYRA_COMPILER_SOURCES ${ModuleSources})
endforeach()
add_executable(kelyra ${KELYRA_ENTRY_SOURCES} ${KELYRA_COMPILER_SOURCES})
target_compile_definitions(kelyra PRIVATE
  KELYRA_STDLIB_SOURCE_DIR="${KELYRA_STDLIB_SOURCE_DIR}")
target_include_directories(kelyra PRIVATE "${PROJECT_BINARY_DIR}/generated")
target_link_libraries(kelyra PRIVATE
  kelyra_codegen
  kelyra_shader
  kelyra_cimport
  kelyra_frontend
  kelyra_ir
  kelyra_sema
)
if(TARGET clang)
  add_dependencies(kelyra clang)
endif()
if(NOT LLVM_ENABLE_RTTI)
  if(MSVC)
    target_compile_options(kelyra PRIVATE /GR-)
  else()
    target_compile_options(kelyra PRIVATE -fno-rtti)
  endif()
endif()
set_target_properties(kelyra PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

add_subdirectory(tools/format)
add_subdirectory(tools/lsp)

include(CTest)
if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
