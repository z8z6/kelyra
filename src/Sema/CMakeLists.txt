add_library(kelyra_sema Annotation.cpp Class.cpp Function.cpp Expression.cpp Reflection.cpp Sema.cpp
  Statement.cpp)
file(READ "${KELYRA_STDLIB_SOURCE_DIR}/std/annotation.kly"
  KELYRA_BUILTIN_ANNOTATIONS_SOURCE)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${KELYRA_STDLIB_SOURCE_DIR}/std/annotation.kly")
configure_file(BuiltinAnnotations.h.in
  "${PROJECT_BINARY_DIR}/generated/BuiltinAnnotations.h" @ONLY)
target_include_directories(kelyra_sema PRIVATE "${PROJECT_BINARY_DIR}/generated")
target_include_directories(kelyra_sema PUBLIC "${PROJECT_SOURCE_DIR}/include")
target_link_libraries(kelyra_sema PUBLIC kelyra_frontend)
