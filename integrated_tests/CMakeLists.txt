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
# Same repo root the `tests/` targets bake in (tests/CMakeLists.txt,
# dss_add_test), so `tests/test_support/repo_root.hpp` resolves here too.
target_compile_definitions(integrated_tests PRIVATE
    DSS_TEST_REPO_ROOT="${CMAKE_SOURCE_DIR}")
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

# ── The SPAWNED CLI needs this in its ENVIRONMENT, not just baked ──────────
# This runner's whole job is to invoke `dss-code-prime` as a SUBPROCESS, and
# that subprocess resolves shipped config through its own
# `core/types/config_path_walk.cpp`, which reads $DSS_CONFIG_ROOT and otherwise
# walks up from ITS cwd. An out-of-tree build gives it a cwd with no
# src/dss-config/ in any ancestor, and until now nothing in this directory set
# the variable at all — `grep DSS_CONFIG_ROOT integrated_tests/` returned
# nothing. MEASURED consequence at a3af1320, and it is the exact silent-harness
# -bug shape the note below warns about: the IN-PROCESS corpus runner
# (tests/examples/examples_runner) PASSED while this CLI-subprocess sibling
# failed all 561 manifests with error[C_InvalidLanguageName] /
# error[D_SchemaLoadFailed]. A compile definition cannot fix this one — the
# value has to be in the environment for the child process to inherit it.
# (CTest ENVIRONMENT is a ';'-separated list; a repo path containing a literal
# ';' would mis-split — impossible on normal Windows/WSL paths.)
set_tests_properties(integrated_tests PROPERTIES
    ENVIRONMENT "DSS_CONFIG_ROOT=${CMAKE_SOURCE_DIR}")

# D-TEST-CROSS-ARCH-SKIP-YIELDS-NO-VERDICT: the same strict-arm-verdict wiring
# the in-process corpus entries get. The capability MUST land on both harnesses
# together — one runner enforcing an arm's verdict while its sibling shrugs is a
# silent harness bug, which is the class this whole change closes.
dss_strict_arm_verdicts(integrated_tests)
