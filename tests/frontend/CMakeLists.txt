add_executable(kelyra_frontend_tests frontend_test.cpp)
target_link_libraries(kelyra_frontend_tests PRIVATE
  kelyra_frontend
  GTest::gtest_main
)
target_compile_definitions(kelyra_frontend_tests PRIVATE
  KELYRA_TEST_ROOT="${PROJECT_SOURCE_DIR}/tests"
)
gtest_discover_tests(kelyra_frontend_tests)
