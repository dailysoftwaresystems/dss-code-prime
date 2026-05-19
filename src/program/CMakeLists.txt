add_library(program OBJECT
    program.cpp
)

target_include_directories(program PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

# See src/core/CMakeLists.txt for the rationale — DSS_BUILDING_DLL must be
# visible at compile time on every OBJECT lib whose .o files end up inside
# dss-code-prime-lib (SHARED).
target_compile_definitions(program PRIVATE DSS_SHARED_BUILD DSS_BUILDING_DLL)

target_link_libraries(program PUBLIC core gen)
