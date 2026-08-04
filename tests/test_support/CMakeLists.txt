# Direct unit tests for the test_support substrate.
# (Header is consumed by every test target via the dss_add_test
# include-path wiring in tests/CMakeLists.txt; this directory carries
# self-tests for the substrate itself.)

dss_add_test(NAME test_support/test_scratch_dir SOURCES test_scratch_dir.cpp)
dss_add_test(NAME test_support/test_host_native_target
             SOURCES test_host_native_target.cpp)
# D-TEST-CROSS-ARCH-SKIP-YIELDS-NO-VERDICT + D-TEST-MANIFEST-ARM64-ARM-WITHOUT-
# EMULATOR. On a windows/x86_64 host the corpus produces ZERO environmental
# skips (every non-Windows arm is excluded by `runOn` first), so without these
# the strict-mode guard would have no witness on that leg at all.
dss_add_test(NAME test_support/test_arm_verdict_ledger
             SOURCES test_arm_verdict_ledger.cpp)
