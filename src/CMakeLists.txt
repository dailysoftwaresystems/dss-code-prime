add_subdirectory(core)
add_subdirectory(program)

# ── Shared library (DLL / .so / .dylib) ──
# All compiler functionality in a single shared library for embedding in other projects.
add_library(dss-code-prime-lib SHARED $<TARGET_OBJECTS:core> $<TARGET_OBJECTS:program>)
target_include_directories(dss-code-prime-lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(dss-code-prime-lib
    PUBLIC  DSS_SHARED_BUILD
    PRIVATE DSS_BUILDING_DLL
)
set_target_properties(dss-code-prime-lib PROPERTIES
    OUTPUT_NAME "dss-code-prime"
)

# ── Executable (CLI) ──
# Thin wrapper that delegates to Program::run().
add_executable(dss-code-prime main.cpp)
target_link_libraries(dss-code-prime PRIVATE dss-code-prime-lib)
