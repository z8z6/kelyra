file(GLOB_RECURSE KELYRA_CODEGEN_TESTS_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/*.h"
)
add_executable(kelyra_codegen_tests ${KELYRA_CODEGEN_TESTS_SOURCES})
target_link_libraries(kelyra_codegen_tests PRIVATE
  kelyra_codegen
  kelyra_sema
  MLIRIR
  GTest::gtest_main
)
gtest_discover_tests(kelyra_codegen_tests)
