add_executable(kelyra-format main.cpp)
target_link_libraries(kelyra-format PRIVATE kelyra_frontend)
set_target_properties(kelyra-format PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
