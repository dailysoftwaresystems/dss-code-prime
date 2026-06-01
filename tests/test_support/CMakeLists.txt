# Direct unit tests for the test_support substrate.
# (Header is consumed by every test target via the dss_add_test
# include-path wiring in tests/CMakeLists.txt; this directory carries
# self-tests for the substrate itself.)

dss_add_test(NAME test_support/test_scratch_dir SOURCES test_scratch_dir.cpp)
