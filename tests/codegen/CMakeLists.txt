add_executable(kelyra_codegen_tests codegen_test.cpp)
target_link_libraries(kelyra_codegen_tests PRIVATE
  kelyra_codegen
  kelyra_sema
  MLIRIR
  GTest::gtest_main
)
gtest_discover_tests(kelyra_codegen_tests)
