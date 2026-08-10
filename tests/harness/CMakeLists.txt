# The GATE on the sqlite harness DRIVERS (real-examples/c/sqlite/).
#
# Those drivers are shell + PowerShell, run by hand, and take hours — so nothing
# in ctest has ever had an opinion about them, which is how both of them came to
# key their leg set on the host (D-HARNESS-CROSS-HOST-ANY-TARGET item 2). Their
# leg DECISION now lives in one declared catalogue + one host-independent
# resolver, and those are cheap to interrogate: this entry holds the
# de-host-locked architecture to account on every gate run, in about a second.
dss_add_test(NAME harness/test_sqlite_harness_legs
             SOURCES test_sqlite_harness_legs.cpp)
# The catalogue and the resolver's plan are JSON, as are the example manifests
# this test cross-checks the launcher vocabulary against — same dependency the
# two corpus runners take for the same reason.
target_link_libraries(dss_harness_test_sqlite_harness_legs
                      PRIVATE nlohmann_json::nlohmann_json)
