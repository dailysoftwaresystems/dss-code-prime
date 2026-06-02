cmake_minimum_required(VERSION 3.28)

# ── Integration Tests ──────────────────────────────────────────
#
# Invokes the compiled `dss-code-prime` executable as a subprocess
# and validates the user-facing CLI surface end-to-end. Cross-
# platform (Windows / Linux / macOS) — the runner's per-host
# spec selection lives at runtime, not in cmake. Always against
# the CURRENT host platform (user invariant 2026-06-02).
#
# The runner ALSO drives the curated `examples/` corpus through
# the CLI path (parallel to the in-process exercise via
# `tests/examples/examples_runner`). The two paths share the
# same source files + expected.json manifests; this one stresses
# argv parsing + filesystem I/O + the `--output` routing logic
# the in-process path can't reach.

enable_testing()

add_executable(integrated_tests runner.cpp)
target_link_libraries(integrated_tests
    PRIVATE
        nlohmann_json::nlohmann_json
)
target_include_directories(integrated_tests
    PRIVATE
        ${CMAKE_SOURCE_DIR}/tests/test_support
)
set_target_properties(integrated_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin/dss
)

# ctest entry — passes the compiler binary path + the examples
# root so the runner is repo-relocation-safe.
add_test(
    NAME integrated_tests
    COMMAND integrated_tests
            $<TARGET_FILE:dss-code-prime>
            ${CMAKE_SOURCE_DIR}/examples
)
