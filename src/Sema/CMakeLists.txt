add_library(kelyra_sema Sema.cpp)
target_include_directories(kelyra_sema PUBLIC "${PROJECT_SOURCE_DIR}/include")
target_link_libraries(kelyra_sema PUBLIC kelyra_frontend)
