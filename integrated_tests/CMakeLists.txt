cmake_minimum_required(VERSION 3.28)

# ── Integration Tests ──
# Invokes the compiled dss-code-prime executable with specific inputs and
# validates outputs: file existence, size, PE structure, directory layout.
# Tests are organized by target platform under target/<os>/.

enable_testing()

# Path to the compiler executable
set(DSS_COMPILER $<TARGET_FILE:dss-code-prime>)

# ─────────────────────────────────────────────────────────────────────────────
# Target: Windows — validates Windows PE output
# ─────────────────────────────────────────────────────────────────────────────

add_executable(integrated_tests_windows target/windows/integrated_tests.cpp)
set_target_properties(integrated_tests_windows PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin/dss
)

add_test(
    NAME integrated_tests_windows
    COMMAND integrated_tests_windows ${DSS_COMPILER}
)
