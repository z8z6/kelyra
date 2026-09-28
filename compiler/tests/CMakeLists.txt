if(NOT EXISTS "${PROJECT_SOURCE_DIR}/third_party/googletest/CMakeLists.txt")
  message(FATAL_ERROR "GoogleTest submodule is missing. Run: git submodule update --init --recursive")
endif()

set(BUILD_GMOCK OFF CACHE BOOL "Build GoogleMock")
set(INSTALL_GTEST OFF CACHE BOOL "Install GoogleTest")
set(gtest_force_shared_crt ON CACHE BOOL "Use the parent project's Windows runtime")
add_subdirectory(
  "${PROJECT_SOURCE_DIR}/third_party/googletest"
  "${PROJECT_BINARY_DIR}/googletest"
  EXCLUDE_FROM_ALL
)

include(GoogleTest)

add_subdirectory(frontend)
add_subdirectory(sema)
add_subdirectory(ir)
add_subdirectory(codegen)
add_subdirectory(cli)
add_subdirectory(lsp)
