add_library(program OBJECT
    program.cpp
)

target_include_directories(program PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(program PUBLIC core gen)
