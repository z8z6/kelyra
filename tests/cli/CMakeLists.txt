add_executable(kelyra_cli_tests cli_test.cpp)
target_include_directories(kelyra_cli_tests SYSTEM PRIVATE
  "${PROJECT_SOURCE_DIR}/third_party/llvm-project/llvm/include"
  "${PROJECT_BINARY_DIR}/llvm/include"
)
target_link_libraries(kelyra_cli_tests PRIVATE
  LLVMSupport
  GTest::gtest_main
)
target_compile_definitions(kelyra_cli_tests PRIVATE
  KELYRA_EXECUTABLE="$<TARGET_FILE:kelyra>"
  KELYRA_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
  KELYRA_TEST_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)
add_dependencies(kelyra_cli_tests kelyra)
gtest_discover_tests(kelyra_cli_tests)

find_program(KELYRA_GDB gdb)
find_package(Python3 COMPONENTS Interpreter QUIET)
if(KELYRA_GDB AND Python3_Interpreter_FOUND)
  add_test(NAME kelyra.gdb.locals
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/debug_test.py"
      $<TARGET_FILE:kelyra> "${KELYRA_GDB}")
  set_tests_properties(kelyra.gdb.locals PROPERTIES SKIP_RETURN_CODE 77)
endif()
