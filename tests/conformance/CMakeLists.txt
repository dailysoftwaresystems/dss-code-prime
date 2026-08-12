# conformance/ — the DIFFERENTIAL accept/reject oracle.
# D-CONF-REFERENCE-DIFFERENTIAL-ORACLE
#
# Compiles a probe corpus with the real DSS CLI *and* with every reference C
# compiler present on the host, then DIFFS the accept/reject verdicts in BOTH
# directions:
#   A — a reference compiler accepts and DSS rejects  (a conformance gap)
#   B — DSS accepts and NO reference compiler accepts (an invented extension)
#
# Direction B had never been checked before this target existed; the operator
# found an instance by eye. This is the mechanism that makes the class
# self-catching.

dss_add_test(NAME conformance/test_reference_conformance
             SOURCES test_reference_conformance.cpp)

# It REUSES tests/core/native_c_probe.hpp rather than growing a second copy of
# host-compiler discovery, shell quoting, output capture and the inert-oracle
# ledger. That header's own docblock is a post-mortem of what happened the last
# time this repo had two copies of exactly this logic, so the include path is
# added here instead of the code being duplicated.
target_include_directories(dss_conformance_test_reference_conformance
    PRIVATE ${CMAKE_SOURCE_DIR}/tests/core)

# ★ THE SUBJECT IS THE CLI, SO ITS PATH IS BAKED AND ITS BUILD IS A DEPENDENCY.
#
# A pin on "what DSS accepts" that drives the library in-process is not a pin on
# what users run: it skips argv parsing, the shipped-config resolver, and the
# per-target pipeline selection. `$<TARGET_FILE:dss-code-prime>` resolves to the
# real executable (with `.exe` on Windows) at generate time, so the test needs no
# search heuristic and cannot pick up a stale binary from another build tree.
#
# `add_dependencies` is NOT optional here. `dss_add_test` links the LIBRARY, so
# without this line a fresh `ctest` could run this entry against a CLI that was
# never built — which fails as "the DSS CLI produced no verdict", i.e. it looks
# like a harness bug rather than a missing build step.
target_compile_definitions(dss_conformance_test_reference_conformance
    PRIVATE DSS_CLI_PATH="$<TARGET_FILE:dss-code-prime>")
add_dependencies(dss_conformance_test_reference_conformance dss-code-prime)

# Cost, MEASURED on this host (Windows, Debug, 5 oracles, 76 probes): the DSS
# half is ~130 ms per invocation x 76 probes x 2 fixed targets, and each oracle
# is ONE batch process rather than 76 (which is why the cl.exe leg does not pay
# the vcvars64 cost per probe). The ctest default timeout is generous enough on
# every leg measured; a leg that adds a slow WSL-bridged oracle pays one process
# launch, not one per probe.
set_tests_properties(conformance/test_reference_conformance PROPERTIES
    TIMEOUT 900)
