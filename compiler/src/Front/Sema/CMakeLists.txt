file(GLOB_RECURSE KELYRA_SEMA_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/*.h"
  "${PROJECT_SOURCE_DIR}/include/Front/Sema/*.h"
  "${CMAKE_CURRENT_SOURCE_DIR}/*.in"
)
add_library(kelyra_sema ${KELYRA_SEMA_SOURCES})
file(READ "${KELYRA_STDLIB_SOURCE_DIR}/std/annotation/annotation.kly"
  KELYRA_BUILTIN_ANNOTATIONS_SOURCE)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${KELYRA_STDLIB_SOURCE_DIR}/std/annotation/annotation.kly")
configure_file(BuiltinAnnotations.h.in
  "${PROJECT_BINARY_DIR}/generated/BuiltinAnnotations.h" @ONLY)
file(READ "${KELYRA_STDLIB_SOURCE_DIR}/std/meta/meta.kly"
  KELYRA_BUILTIN_META_SOURCE)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${KELYRA_STDLIB_SOURCE_DIR}/std/meta/meta.kly")
configure_file(BuiltinMeta.h.in
  "${PROJECT_BINARY_DIR}/generated/BuiltinMeta.h" @ONLY)
file(READ "${KELYRA_STDLIB_SOURCE_DIR}/std/c/c.kly"
  KELYRA_BUILTIN_C_TYPES_SOURCE)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${KELYRA_STDLIB_SOURCE_DIR}/std/c/c.kly")
configure_file(BuiltinCTypes.h.in
  "${PROJECT_BINARY_DIR}/generated/BuiltinCTypes.h" @ONLY)
target_include_directories(kelyra_sema PRIVATE "${PROJECT_BINARY_DIR}/generated")
target_include_directories(kelyra_sema SYSTEM PRIVATE
  "${KELYRA_LLVM_SOURCE_DIR}/llvm/include"
  "${KELYRA_LLVM_BINARY_DIR}/include"
  "${KELYRA_LLVM_SOURCE_DIR}/clang/include"
  "${KELYRA_LLVM_BINARY_DIR}/tools/clang/include")
target_include_directories(kelyra_sema PUBLIC "${PROJECT_SOURCE_DIR}/include")
target_link_libraries(kelyra_sema PUBLIC kelyra_frontend PRIVATE LLVMSupport clangBasic)
