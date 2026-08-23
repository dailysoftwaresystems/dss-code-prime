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
    runtime_object_cache.cpp
)

target_include_directories(program PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

# See src/core/CMakeLists.txt for the rationale — DSS_BUILDING_DLL must be
# visible at compile time on every OBJECT lib whose .o files end up inside
# dss-code-prime-lib (SHARED).
target_compile_definitions(program PRIVATE DSS_SHARED_BUILD DSS_BUILDING_DLL)

# The same repo-root VERSION forward `core` and `link` do, for the same reason
# and from the same single source of truth (`DSS_VERSION`, the stripped repo-root
# VERSION file read by the top-level CMakeLists).
#
# ⚠ THIS IS *NOT* A FALLBACK FOR THE RUNTIME OBJECT CACHE'S COMPILER-IDENTITY
# TERM, and an earlier draft of these two files made it one. That draft is
# recorded in `runtime_object_cache.cpp` at the build-stamp include: a version
# string does not move when codegen moves, so falling back to it would let two
# compilers built from different source at one VERSION share a cache entry and
# serve the older one's archive to the newer — a reachable stale object, the one
# failure the whole mechanism exists to prevent. A backed-out
# `dss_use_build_stamp(program)` edge must be a hard `#error` naming that edge,
# never a quietly weaker key. [[D-RUNTIME-DSS-SHIPS-NO-IMPLEMENTATION-HALF]]
#
# ⓘ NO `src/program/` SOURCE READS IT TODAY — ✔MEASURED 2026-08-17: nothing here
# names `DSS_PROJECT_VERSION`, and nothing here reaches
# `core/types/predefined_macro_json.hpp`, which is the `#error` that makes the
# forward mandatory for `core` and `link`. It is kept per the note at
# `dss_use_build_stamp(program)` below, so a `program` source that legitimately
# wants the product version has it; it is simply unreachable from the cache key.
target_compile_definitions(program PRIVATE DSS_PROJECT_VERSION="${DSS_VERSION}")

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
# ── THE BUILD-STAMP EDGE (the `compiler` term of the runtime cache key) ──
# `dss_use_build_stamp` is defined in the top-level CMakeLists and wires BOTH
# halves at once: the generated header's include path and the ordering edge onto
# `dss_build_stamp_generate`. Without it `runtime_object_cache.cpp` does not
# compile — it `#include`s `program/dss_build_stamp.hpp` UNCONDITIONALLY, which
# resolves through the `..` include directory above and then includes its
# GENERATED half out of the build tree.
#
# ⚠⚠ THIS EDGE IS TARGET-WIDE; THE INCLUSION MUST NOT BE. The generated header
# is rewritten whenever the working tree's content changes, so every TU that
# `#include`s it recompiles on every dirty edit — and each one of those drags the
# shared library through a relink. `runtime_object_cache.cpp` is the ONE
# translation unit in this library that may include it; the other eight must not,
# and adding a second includer is a review-stop rather than a preference. The
# edge cannot be scoped narrower than the target, so this sentence is the only
# thing standing between one recompile and nine.
dss_use_build_stamp(program)

# ⓘ THE `DSS_PROJECT_VERSION` FORWARD ABOVE IS NOT A FALLBACK FOR THIS EDGE.
# `runtime_object_cache.cpp` includes `dss_build_stamp.hpp` UNCONDITIONALLY and
# has no `#else` arm, so backing this line out is a hard `#error` naming
# `dss_use_build_stamp(<target>)` — which is the intended and correct failure.
# ✔RED-ON-DISABLE: comment out the call and the build fails at the stamp header's
# own `#error`, not at a missing generated file. The forward stays because `core`
# and `link` make the same one from the same source of truth and a `program`
# source may legitimately want the version for an unrelated purpose; it is simply
# not reachable from the cache key.

# NO nlohmann/json EDGE, and that is now correct rather than an omission.
# `project_config.cpp` was the only source here that included it, and it moved
# to `core/types/` on 2026-08-13
# (D-LSP-PROJECT-CONFIG-LIVES-ABOVE-ITS-CONSUMERS) — `core` already links
# nlohmann PRIVATE for its own config-document readers. Verified by
# `grep -rn '#include <nlohmann' src/program/` returning nothing. Re-add the
# edge (PRIVATE) the day a `program` source parses JSON directly again; the
# include path does NOT arrive transitively from `core`.
