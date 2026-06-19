dss_add_test(NAME analysis/preprocess/test_preprocessor
             SOURCES test_preprocessor.cpp)

# Reach the shared test-support headers (core/e2e_harness etc.) from this
# sibling test directory, mirroring analysis/compilation_unit. dss_add_test
# only puts src/ on the include path. (Do NOT add the repo root -- its VERSION
# file shadows <version> on case-insensitive filesystems.)
target_include_directories(dss_analysis_preprocess_test_preprocessor
                           PRIVATE ${CMAKE_SOURCE_DIR}/tests)
