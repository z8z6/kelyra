add_executable(kelyra_sema_tests sema_test.cpp)
target_link_libraries(kelyra_sema_tests PRIVATE
  kelyra_sema
  GTest::gtest_main
)
target_compile_definitions(kelyra_sema_tests PRIVATE
  KELYRA_TEST_ROOT="${PROJECT_SOURCE_DIR}/tests"
)
gtest_discover_tests(kelyra_sema_tests)
