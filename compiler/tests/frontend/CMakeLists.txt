file(GLOB_RECURSE KELYRA_FRONTEND_TESTS_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/*.h"
)
add_executable(kelyra_frontend_tests ${KELYRA_FRONTEND_TESTS_SOURCES})
target_link_libraries(kelyra_frontend_tests PRIVATE
  kelyra_frontend
  GTest::gtest_main
)
target_compile_definitions(kelyra_frontend_tests PRIVATE
  KELYRA_TEST_ROOT="${PROJECT_SOURCE_DIR}/tests"
)
gtest_discover_tests(kelyra_frontend_tests)
