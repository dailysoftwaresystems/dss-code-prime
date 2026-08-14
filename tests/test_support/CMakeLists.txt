# Direct unit tests for the test_support substrate.
# (Header is consumed by every test target via the dss_add_test
# include-path wiring in tests/CMakeLists.txt; this directory carries
# self-tests for the substrate itself.)

dss_add_test(NAME test_support/test_scratch_dir SOURCES test_scratch_dir.cpp)
# D-TEST-SCOPED-ENV-DUPLICATED-THREE-WAYS. `scoped_env.hpp` is the ONE env
# override, hoisted from five hand-copied versions none of which was ever
# exercised directly. Its job is RESTORING state, so a wrong restore fails in a
# LATER, unrelated test in the same binary — the symptom lands arbitrarily far
# from the cause. The pin that matters most is "absent restores to ABSENT, never
# to empty": `getenv` returns nullptr vs "", and the construct-to-clear callers
# exist precisely to tell those apart.
dss_add_test(NAME test_support/test_scoped_env SOURCES test_scoped_env.cpp)
# The ONE test-side repo-root resolver. This ordinary entry runs the
# precedence/validation suite; the `BakedRootOnly.*` suite in the same binary is
# the out-of-tree regression guard and is EXCLUDED here, because it asserts
# preconditions (no DSS_CONFIG_ROOT, no repo ancestry above cwd) that are false
# under normal dss_add_test wiring. Its own entry lives at the bottom of
# tests/CMakeLists.txt, where the stripped environment is set up.
dss_add_test(NAME test_support/test_repo_root SOURCES test_repo_root.cpp)
set_property(TEST test_support/test_repo_root APPEND PROPERTY
             ENVIRONMENT "GTEST_FILTER=-BakedRootOnly.*")
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
# D-TEST-RUN-HARNESS-DEADLINE-COUNTS-HOST-SUSPEND. The same chokepoint's
# DEADLINE: it used to be spent on `steady_clock`, which on Darwin counts host
# suspend, so a napping laptop was billed to the child. Pins which clock is
# selected (against a clock id read from the kernel here) and that the deadline
# still kills a hanging child, its budget stated in AWAKE seconds.
# ★ Its `main` spawns THIS executable as its own hanging child, so — like its
# sibling above — it must stay a SEPARATE target.
dss_add_test(NAME test_support/test_run_binary_deadline_clock
             SOURCES test_run_binary_deadline_clock.cpp)
