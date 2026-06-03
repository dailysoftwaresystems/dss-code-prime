add_library(program OBJECT
    program.cpp
    target_spec.cpp
    compile_pipeline.cpp
    cli_args.cpp
    input_resolver.cpp
    cross_validate_target_format.cpp
)

target_include_directories(program PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

# See src/core/CMakeLists.txt for the rationale — DSS_BUILDING_DLL must be
# visible at compile time on every OBJECT lib whose .o files end up inside
# dss-code-prime-lib (SHARED).
target_compile_definitions(program PRIVATE DSS_SHARED_BUILD DSS_BUILDING_DLL)

# `compile_pipeline.cpp` reaches through the full HIR→write pipeline; its
# transitive includes pull in headers from every IR tier. The OBJECT library
# only needs include-paths exposed — the .o files get linked into
# dss-code-prime-lib by the top-level lib target, which already aggregates
# every backend.
target_link_libraries(program PUBLIC core lsp ffi ffi_abi opt)
