add_library(program OBJECT
    program.cpp
    target_spec.cpp
    compile_pipeline.cpp
    cli_args.cpp
    input_resolver.cpp
    project_sources.cpp
    cross_validate_target_format.cpp
    cross_validate_language_target.cpp
    build_scripts.cpp
    git_acquire.cpp
    dependency_lockfile.cpp
    dependency_cache.cpp
    dependency_resolver.cpp
    dump_predefined_macros.cpp
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

# MSVC: compile_pipeline.cpp's whole-pipeline include surface exceeds the
# default COFF object section limit (C1128) in Debug builds (tripped when
# FC2 grew compilation_unit.hpp's include chain). /bigobj lifts the format
# cap only — zero codegen/behavior change.
if(MSVC)
    target_compile_options(program PRIVATE /bigobj)
endif()

# ✅ THE nlohmann/json EDGE IS BACK, ON THE TERMS THE PREVIOUS NOTE SET. That
# note read: "`project_config.cpp` was the only source here that included it,
# and it moved to `core/types/` on 2026-08-13
# (D-LSP-PROJECT-CONFIG-LIVES-ABOVE-ITS-CONSUMERS) ... Re-add the edge (PRIVATE)
# the day a `program` source parses JSON directly again; the include path does
# NOT arrive transitively from `core`." That day is AP6: `dependency_lockfile.cpp`
# reads and writes `.dss-deps/dss-lock.json` and `#include <nlohmann/json.hpp>`
# directly.
#
# PRIVATE, and the honest include list is ONE file — `grep -rn
# '^#include <nlohmann' src/program/` is the whole instrument, exactly as
# src/core/CMakeLists.txt keeps its own list honest. `dependency_lockfile.hpp`
# is a public header and deliberately names NO JSON type: the document shape is
# an implementation detail of the .cpp, so no consumer of the lockfile inherits
# the dependency (contrast the sanctioned `_json.hpp` shape in `core`, which is
# for headers whose declared job IS to accept a `nlohmann::json const&`).
target_link_libraries(program PRIVATE nlohmann_json::nlohmann_json)
