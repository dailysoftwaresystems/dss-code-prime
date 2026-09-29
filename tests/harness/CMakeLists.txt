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
