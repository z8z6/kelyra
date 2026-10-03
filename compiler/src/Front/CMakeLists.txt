file(GLOB_RECURSE KELYRA_FRONTEND_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/*.h"
  "${PROJECT_SOURCE_DIR}/include/Front/*.h"
  "${PROJECT_SOURCE_DIR}/include/Front/*.def"
)
# CImport and Sema have separate targets; Module and Expansion belong to the executable.
list(FILTER KELYRA_FRONTEND_SOURCES EXCLUDE REGEX "/(CImport|Module|Expansion|Sema)/")
add_library(kelyra_frontend ${KELYRA_FRONTEND_SOURCES})
target_include_directories(kelyra_frontend PUBLIC "${PROJECT_SOURCE_DIR}/include")

target_include_directories(kelyra_frontend SYSTEM PRIVATE
  "${KELYRA_LLVM_SOURCE_DIR}/llvm/include"
  "${KELYRA_LLVM_BINARY_DIR}/include")
target_link_libraries(kelyra_frontend PRIVATE LLVMSupport)

add_subdirectory(Sema)
add_subdirectory(CImport)
