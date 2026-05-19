add_library(gen OBJECT
    link/targets/windows/target_windows_x86_64.cpp
)

target_include_directories(gen PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

# See src/core/CMakeLists.txt for the rationale — DSS_BUILDING_DLL must be
# visible at compile time on every OBJECT lib whose .o files end up inside
# dss-code-prime-lib (SHARED).
target_compile_definitions(gen PRIVATE DSS_SHARED_BUILD DSS_BUILDING_DLL)

target_link_libraries(gen PUBLIC core)
