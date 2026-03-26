add_library(gen OBJECT
    link/targets/windows/target_windows_x86_64.cpp
)

target_include_directories(gen PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(gen PUBLIC core)
