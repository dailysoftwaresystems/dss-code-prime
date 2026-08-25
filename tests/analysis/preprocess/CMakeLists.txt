dss_add_test(NAME analysis/preprocess/test_preprocessor
             SOURCES test_preprocessor.cpp)

# Reach the shared test-support headers (core/e2e_harness etc.) from this
# sibling test directory, mirroring analysis/compilation_unit. dss_add_test
# only puts src/ on the include path. (Do NOT add the repo root -- its VERSION
# file shadows <version> on case-insensitive filesystems.)
target_include_directories(dss_analysis_preprocess_test_preprocessor
                           PRIVATE ${CMAKE_SOURCE_DIR}/tests)

# [[D-PP-BARE-RELATIVE-MAIN-PATH-DEFEATS-THE-INCLUDER-DIRECTORY-SEARCH]]: the
# includer-directory derivation, pinned on the BARE source-name form. A separate
# binary rather than more cases in test_preprocessor.cpp because every test in
# it moves the process cwd (the defect is only observable when the header is IN
# the working directory), and that is a property of the whole file, not of one
# case — keeping it out of the 10k-line neighbour keeps the cwd move from being
# something a future case there acquires by accident.
dss_add_test(NAME analysis/preprocess/test_include_bare_relative_includer_dir
             SOURCES test_include_bare_relative_includer_dir.cpp)
target_include_directories(
    dss_analysis_preprocess_test_include_bare_relative_includer_dir
    PRIVATE ${CMAKE_SOURCE_DIR}/tests)

# [[D-PERF-PP-OFF-GRAMMAR-BODY-RUN-IS-ONE-TOKEN-PER-CODEPOINT]]: the preprocessor
# folds each off-grammar `EmptySpace` body run (a comment) into ONE token, so a
# comment costs one token rather than one per character. A separate binary rather
# than more cases in test_preprocessor.cpp because the pin is a COMPARISON of two
# whole preprocess runs that differ only in a comment's LENGTH, while that file's
# fixtures all assert on the contents of a single run.
dss_add_test(NAME analysis/preprocess/test_comment_body_run_is_one_token
             SOURCES test_comment_body_run_is_one_token.cpp)
