file(GLOB_RECURSE KELYRA_IR_TESTS_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/*.h"
)
add_executable(kelyra_ir_tests ${KELYRA_IR_TESTS_SOURCES})
target_link_libraries(kelyra_ir_tests PRIVATE
  kelyra_ir
  MLIRFuncDialect
  MLIRParser
  GTest::gtest_main
)
gtest_discover_tests(kelyra_ir_tests)
