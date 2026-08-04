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
# D-TEST-RUN-HARNESS-DRAIN-AFTER-EXIT-DEADLOCKS. `runBinary` is the ONE spawn
# chokepoint both corpus harnesses use, and its capture path used to drain the
# pipe only after the child exited — a deadlock for any child bigger than the
# pipe buffer, reported as if the CHILD had hung. This pin spawns the test
# executable as its own 200 KB payload generator, so it needs no external tool
# and clears both the Windows (~4 KiB) and Linux (64 KiB) buffers.
# ★ It defines its own `main` (to intercept the payload flag before gtest parses
# argv), which is why it must stay a SEPARATE target from its siblings above.
dss_add_test(NAME test_support/test_run_binary_capture
             SOURCES test_run_binary_capture.cpp)
