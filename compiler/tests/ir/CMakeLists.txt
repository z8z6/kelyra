add_executable(kelyra_ir_tests ir_test.cpp)
target_link_libraries(kelyra_ir_tests PRIVATE
  kelyra_ir
  MLIRFuncDialect
  MLIRParser
  GTest::gtest_main
)
gtest_discover_tests(kelyra_ir_tests)
