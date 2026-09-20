add_library(kelyra_sema Annotation.cpp Class.cpp Function.cpp Expression.cpp Reflection.cpp Sema.cpp
  Statement.cpp)
target_include_directories(kelyra_sema PUBLIC "${PROJECT_SOURCE_DIR}/include")
target_link_libraries(kelyra_sema PUBLIC kelyra_frontend)
