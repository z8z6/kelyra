cmake_minimum_required(VERSION 3.20)
project(Kelp VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(kelp src/main.cpp)
set_target_properties(kelp PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
if(MSVC)
  target_compile_options(kelp PRIVATE /W4 /EHsc)
else()
  target_compile_options(kelp PRIVATE -Wall -Wextra -Wpedantic)
endif()
target_compile_definitions(kelp PRIVATE KELP_VERSION="${PROJECT_VERSION}")
install(TARGETS kelp RUNTIME DESTINATION bin)

include(CTest)
if(BUILD_TESTING)
  if(WIN32)
    add_test(NAME kelp.basic
      COMMAND powershell -NoProfile -ExecutionPolicy Bypass -File
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/basic.ps1" $<TARGET_FILE:kelp>)
  else()
    add_test(NAME kelp.basic
      COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/tests/basic.sh" $<TARGET_FILE:kelp>)
  endif()
endif()
