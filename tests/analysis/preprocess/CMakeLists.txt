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

# [[D-PERF-PP-IF-REMATERIALIZES-THE-WHOLE-SYNTH-BUFFER-PER-EVALUATION]] +
# [[D-PERF-PP-EVERY-INCLUDE-RE-READS-AND-RE-TOKENIZES-THE-SAME-HEADER]]: the two
# REWORK defects, pinned by the SHAPE of the cost curve rather than by a
# wall-clock threshold. A separate binary rather than more cases in
# test_preprocessor.cpp because every case here is a RATIO of two timed
# preprocess runs taken back to back in one process, and that only holds if the
# process is not also being timed against unrelated fixtures.
dss_add_test(NAME analysis/preprocess/test_preprocess_no_rework
             SOURCES test_preprocess_no_rework.cpp)
target_include_directories(dss_analysis_preprocess_test_preprocess_no_rework
                           PRIVATE ${CMAKE_SOURCE_DIR}/tests)

# [[D-PP-REMAP-ORIGIN-OFFSET-UNVALIDATED]]: a diagnostic whose SUBJECT is a
# macro-expansion PRODUCT token must point at the EXPANSION SITE, not at the
# offset an extrapolation through the line map produced (and not at the
# end-of-file the renderer's clamp then made of it). A separate binary rather
# than more cases in test_preprocessor.cpp because every case here asserts on
# RENDERED diagnostic text through the full UnitBuilder -> reporter -> format
# path, while that file's fixtures inspect a `preprocess()` token stream
# directly; the two need different fixtures and different failure messages.
dss_add_test(NAME analysis/preprocess/test_macro_expansion_diagnostic_position
             SOURCES test_macro_expansion_diagnostic_position.cpp)
target_include_directories(
    dss_analysis_preprocess_test_macro_expansion_diagnostic_position
    PRIVATE ${CMAKE_SOURCE_DIR}/tests)

# [[D-PP-PASTE-REJECTS-A-VALID-PREPROCESSING-NUMBER]] +
# [[D-PP-PASTE-PRODUCT-IS-RE-READ-AS-THE-PASTE-OPERATOR]]: `##` validity is a
# question about PREPROCESSING tokens (C 6.10.3.3p3 + 6.4.8), so the scanner
# answers it and the preprocessor asks. A separate binary rather than more cases
# in test_preprocessor.cpp because half the arms here drive the TOKENIZER
# directly in a named phase, which that file never does, and because each arm
# pairs an accept with the refusal that must still follow it.
dss_add_test(NAME analysis/preprocess/test_paste_preprocessing_token
             SOURCES test_paste_preprocessing_token.cpp)
