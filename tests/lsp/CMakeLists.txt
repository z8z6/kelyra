find_package(Python3 COMPONENTS Interpreter REQUIRED)

add_test(
  NAME kelyra.lsp
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/lsp_test.py"
          "$<TARGET_FILE:kelyra-ls>"
)
