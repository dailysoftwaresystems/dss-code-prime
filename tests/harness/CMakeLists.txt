# The GATE on the sqlite harness DRIVER (.harness-config/runner/actions/real-examples/c/sqlite/).
#
# The driver takes hours, so ctest does not run it. When it was a shell + PowerShell
# pair run by hand, nothing in ctest had an opinion about it, which is how both
# halves came to key their leg set on the host, against the project's
# build-ANY-target-inside-ANY-host requirement. The leg DECISION now lives in one
# declared catalogue + one host-independent resolver, and the one Python driver
# (2026-09-21) reads it; this entry holds that architecture, and the driver's
# source, to account on every gate run. The driver's own Step 0 runs as
# `harness/sqlite_driver_selftest` (root CMakeLists.txt).
dss_add_test(NAME harness/test_sqlite_harness_legs
             SOURCES test_sqlite_harness_legs.cpp)
# The catalogue and the resolver's plan are JSON, as are the example manifests
# this test cross-checks the launcher vocabulary against — same dependency the
# two corpus runners take for the same reason.
target_link_libraries(dss_harness_test_sqlite_harness_legs
                      PRIVATE nlohmann_json::nlohmann_json)

# THE NINJA DEPENDENCY-RECORD CHECK'S OWN SELF-TEST, ONE CASE PER ARM — the ctest entry
# `ninja_deps_selftest_guard`. `.harness-config/runner/actions/check-ninja-deps/
# check-ninja-deps.py --self-test` proves the check's rules on text and on throwaway
# trees and prints one verdict line per arm; this binary RUNS it — by the command line
# the top-level CMakeLists declares — and reads each arm's verdict into a case of its
# own. It re-implements none of them. It keeps the entry's name because it IS that
# entry: until 2026-10-10 the program was started bare by ctest, one verdict for every
# arm and no test binary, so no mutation of the program could be an arm of the mutation
# sweep, which starts a test binary whole and judges the exact set of cases that go red.
# ⓘ The name ends in `guard` and the entry carries NO `repo-guard` label: that label is
# given to the top directory's entries. This one proves a program's rules on the host's
# own paths, which every leg has its own of, so it runs on every leg.
dss_add_test(NAME ninja_deps_selftest_guard
             SOURCES test_ninja_deps_selftest.cpp)
# The opt-in (top-level CMakeLists, `dss_use_script_selftest`): the generated header that
# holds the command line, and the program's files declared as inputs of this target's
# object — no compiler reads a Python program, so without that declaration no object
# depended on either.
dss_use_script_selftest(dss_ninja_deps_selftest_guard ninja_deps)
