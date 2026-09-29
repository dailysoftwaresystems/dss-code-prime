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

# [[D-PP-PRAGMA-RECOGNIZED-SEMANTICS]]: `#pragma once` HAS AN EFFECT -- the
# include machinery keeps a TU-wide once-registry and SKIPS a repeat splice of
# the same FILE IDENTITY. A separate binary rather than more cases in
# test_preprocessor.cpp because every arm here needs REAL FILES ON DISK reached
# through several spellings (`./h.h`, `sub/../h.h`, a hard link, a symlink) and
# an InsideRepo ScratchDir made the process cwd -- neither of which that file's
# in-memory buffer fixtures can express.
dss_add_test(NAME analysis/preprocess/test_pragma_once_include_dedup
             SOURCES test_pragma_once_include_dedup.cpp)
target_include_directories(
    dss_analysis_preprocess_test_pragma_once_include_dedup
    PRIVATE ${CMAKE_SOURCE_DIR}/tests)

# [[D-PP-PRAGMA-RECOGNIZED-SEMANTICS]]: `#pragma STDC` (C23 6.10.8) is
# RECOGNIZED, ACCEPTED, and per-form JUDGED -- six of the nine standard forms
# are states DSS satisfies (silent), three are states it does not (accepted,
# with the divergence NAMED in an unsuppressable warning), and anything else
# stays loud. A separate binary rather than more cases in test_preprocessor.cpp
# because every arm here reads diagnostic SEVERITY -- the same code carries the
# loud refusal and the accepted-with-notice warning, so presence alone cannot
# tell the shipped behaviour from the defect it replaced -- and because the
# nine forms are driven as a parameterised suite.
dss_add_test(NAME analysis/preprocess/test_pragma_stdc_float_state
             SOURCES test_pragma_stdc_float_state.cpp)
target_include_directories(
    dss_analysis_preprocess_test_pragma_stdc_float_state
    PRIVATE ${CMAKE_SOURCE_DIR}/tests)

# [[D-PP-SKIPPED-CONDITIONAL-GROUP-VALIDATED-AS-A-PHASE-7-NUMBER]]: the text of a
# SKIPPED conditional group is divided into preprocessing tokens and NOT
# otherwise processed (C 6.10.1p6), so no phase-7 conversion judgement applies to
# it -- and the pp-number tail's exponent-letter set is the UNION of the decimal
# and per-prefix declarations. A separate binary rather than more cases in
# test_preprocessor.cpp because every arm here PAIRS an accept inside a skipped
# group with the refusal that must still follow the same spelling when it is
# live, and because two of them assert the diagnostic-tier classification
# directly rather than through a preprocess run.
dss_add_test(NAME analysis/preprocess/test_skipped_group_not_otherwise_processed
             SOURCES test_skipped_group_not_otherwise_processed.cpp)

# D-TOK-STRING-STYLE-MULTILINE-IS-NEVER-READ + D-PP-CONVERSION-DIAGNOSTIC-FIRES-ON-A-TOKEN-NEVER-CONVERTED:
# a character constant, string literal or header name ENDS AT ITS LINE (the
# tokenizer honours `stringStyle.multiline`), and a conversion diagnostic is
# delivered only for a token phase 7 converts. Tokenizer and preprocessor pins
# in one binary because the second defect was hidden behind the first.
dss_add_test(NAME analysis/preprocess/test_literal_ends_at_its_line
             SOURCES test_literal_ends_at_its_line.cpp)

# [[D-PP-SINGLE-PASS-INCLUDE-RESOLUTION]]: the include PRE-SCAN's macro state IS
# the authoritative `MacroExpander`, hosted as an oracle — so the guard that
# decides whether a quote-`#include` is spliced is evaluated by the same engine
# that will re-evaluate it authoritatively, and a function-like macro in that
# guard no longer turns legal C into a hard refusal. A separate binary rather
# than more cases in test_preprocessor.cpp because every arm here needs a REAL
# HEADER ON DISK reached through an InsideRepo ScratchDir made the process cwd —
# a quote-include resolution is not observable any other way — while that file's
# fixtures preprocess in-memory buffers with no include path at all. Same split,
# same reason, as test_pragma_once_include_dedup beside it.
dss_add_test(NAME analysis/preprocess/test_prescan_macro_oracle
             SOURCES test_prescan_macro_oracle.cpp)
target_include_directories(
    dss_analysis_preprocess_test_prescan_macro_oracle
    PRIVATE ${CMAKE_SOURCE_DIR}/tests)

# [[D-CSUBSET-COMPILER-FEATURE-QUERY-OPERATORS]]: the feature-query operator
# family (`__has_attribute` / `__has_builtin` / `__has_feature` /
# `__has_extension`, against the three C23 conditional-inclusion operators that
# ARE implemented) pinned as ONE enumeration — visibility tracks implementation,
# the portable `#ifndef` shim still installs, an UNDECLARED capability answers 0
# rather than 1, the `#define` refusal covers exactly the C23 6.10.10p2 names,
# and an unshimmed query fails LOUD instead of answering 0 in silence. A separate
# binary rather than more cases in test_preprocessor.cpp because those five are
# halves of a single invariant whose JOINT reading is the guard — split across a
# 10k-line file, a later cycle can satisfy one and break another unnoticed — and
# because the pre-scan arm needs a real header on disk reached through an
# InsideRepo ScratchDir made the process cwd, which that file's in-memory
# fixtures cannot express. Same split, same reason, as test_prescan_macro_oracle.
dss_add_test(NAME analysis/preprocess/test_feature_query_operator_family
             SOURCES test_feature_query_operator_family.cpp)
target_include_directories(
    dss_analysis_preprocess_test_feature_query_operator_family
    PRIVATE ${CMAKE_SOURCE_DIR}/tests)

# P68 round 8 (D-C-SIZEOF-LONG-DOUBLE-NOT-PREDEFINED, then
# D-C-SIZEOF-PREDEFINED-MACRO-FAMILY-MISSING): `__SIZEOF_LONG_DOUBLE__`, the C
# language's `type-size` row, realized on every (target x format) pair and
# pinned against the size `sizeof(long double)` has there — derived from the
# grammar's own `long double` row, never a table of numbers — and absent where
# the format refuses the type or no pair was named. A separate binary rather
# than more cases in test_preprocessor.cpp because it is a sweep over every
# SHIPPED document on disk, which that file's in-memory fixtures deliberately
# do not load.
dss_add_test(NAME analysis/preprocess/test_sizeof_long_double_macro
             SOURCES test_sizeof_long_double_macro.cpp)

# P68 round 8 (D-C-SIZEOF-PREDEFINED-MACRO-FAMILY-MISSING): the whole
# `__SIZEOF_*__` family — every member a `type-size` row of the language, the
# targets' platform ABI typedefs behind `wchar_t`/`wint_t`, and a translation
# unit of `_Static_assert(sizeof(T) == __SIZEOF_T__)` COMPILED FOR EVERY REAL
# PAIR (through `applyTargetFormatPair` and `analyzeForTargetFormat`, the
# driver's own two halves), so DSS's preprocessor and DSS's `sizeof` must give
# one answer. Separate from the `long double` pin above because that one sweeps
# the merge over every pair, plausible or not, while this one compiles C.
dss_add_test(NAME analysis/preprocess/test_sizeof_macro_family
             SOURCES test_sizeof_macro_family.cpp)

# P68 round 8 (D-C-BITINT-MAXWIDTH-MACRO-RESTATES-THE-MODEL-BOUND): a predefined
# macro whose value is a limit of DSS's own model — `__BITINT_MAXWIDTH__`, named
# by `kind: model-limit` and written by the loader from `kBitIntMaxWidth`. Pins
# the document (no number), the loaded value, the width check reading the same
# number through the driver's two halves, and the loader's refusals. Reads the
# shipped config, so it MUST run through ctest (the config snapshot).
dss_add_test(NAME analysis/preprocess/test_model_limit_macros
             SOURCES test_model_limit_macros.cpp)
target_link_libraries(dss_analysis_preprocess_test_model_limit_macros
    PRIVATE nlohmann_json::nlohmann_json)

# P68 round 9 (D-C-LIMITS-H-DEFINES-NINE-OF-THE-STANDARD-MACROS): `<limits.h>`
# realized per (target, format) pair from the lattice — every one of the 32 names
# with the TYPE and VALUE gcc/clang (per arch), mingw/MSVC (pe) and Darwin give it,
# plus its `#if` arms, COMPILED FOR EVERY REAL PAIR through the driver's two halves;
# the `<stdint.h>`/`NULL`/`clock_t` identities the typed shipped-constant splice
# decides; `#undef` removing a shipped constant like any macro; and the splice's
# refusal of a declared type no literal of the language has. Reads the shipped
# config, so it runs through ctest (the config snapshot).
dss_add_test(NAME analysis/preprocess/test_limits_h_lattice
             SOURCES test_limits_h_lattice.cpp)
target_include_directories(dss_analysis_preprocess_test_limits_h_lattice
                           PRIVATE ${CMAKE_SOURCE_DIR}/tests)

# P68 round 9 (D-C-WCHAR-T-IS-SIGNED-ON-ARM64-LINUX): `wchar_t` — the `<stddef.h>`
# typedef and the type of `L'…'`/`L"…"` — from ONE source, the target's platform
# ABI typedef table, on every real (target, format) pair; every shipped target
# declares it; and a stripped custom target that does not REACHES
# S_AbiTypedefUndeclared at the literal. Reads the shipped config, so it runs
# through ctest (the config snapshot).
dss_add_test(NAME analysis/preprocess/test_wchar_t_abi_typedef
             SOURCES test_wchar_t_abi_typedef.cpp)

# P68 round 9 (D-CONFIG-SYS-STAT-OVER-DECLARES-S-ISLNK-ON-PE, the coordinator's
# ruling (a), and the <sys/stat.h> family found incomplete while measuring it): on
# every real pair `<sys/stat.h>` defines exactly the mode names — with their values
# — the platform's own reference defines (glibc, Darwin, mingw-w64 ∪ MSVC), and on
# pe `<direct.h>`/`<dirent.h>` pull only what the Windows references pull. Reads the
# shipped config, so it runs through ctest (the config snapshot).
dss_add_test(NAME analysis/preprocess/test_platform_header_surface
             SOURCES test_platform_header_surface.cpp)

# P68 round 9 (D-C-PREFIXED-CHARACTER-CONSTANT-IS-NOT-A-CONSTANT-EXPRESSION +
# D-PP-IF-NARROW-CHARACTER-CONSTANT-IGNORES-PLAIN-CHAR-SIGNEDNESS): `L'…'`, `u'…'`,
# `U'…'` and `u8'…'` are integer constant expressions in every ICE position and in
# `#if`, and a character constant reads in `#if` as signed iff its element type is —
# on every real pair, through the driver's own front halves and the CST→HIR tier.
# Reads the shipped config, so it runs through ctest (the config snapshot).
dss_add_test(NAME analysis/preprocess/test_prefixed_char_constant
             SOURCES test_prefixed_char_constant.cpp)

# P68 round 9 (lane `lm`): the GNU integer-type predefined-macro families —
# `__*_TYPE__` (`type-name`), `__*_MAX__`/`__*_MIN__`/`__*_WIDTH__` (`type-limit`),
# `__*_C(c)`/`__*_C_SUFFIX__` (`type-suffix`) — against every reference's answer on
# every real pair, the one-fact rule (each `__X_TYPE__` IS the shipped typedef),
# the merge's inputs, and the loader's refusals. Reads the shipped config, so it
# runs through ctest (the config snapshot).
dss_add_test(NAME analysis/preprocess/test_type_derived_predefines
             SOURCES test_type_derived_predefines.cpp)

# P68 round 9 (D-C-WCHAR-T-IS-SIGNED-ON-ARM64-LINUX, the preprocessor face): the
# `type-unsigned` predefine kind — `__CHAR_UNSIGNED__`, `__WCHAR_UNSIGNED__` and
# `__WINT_UNSIGNED__` are defined on exactly the real pairs where gcc/clang `-dM`
# define them, which is exactly where the type is unsigned; the merge realizes them
# from the pair's facts alone; and the loader refuses every malformed row. Reads
# the shipped config, so it runs through ctest (the config snapshot).
dss_add_test(NAME analysis/preprocess/test_type_unsigned_predefines
             SOURCES test_type_unsigned_predefines.cpp)

# P68 round 9 (D-FFI-STDINT-LIMIT-MACROS): `<stdint.h>`'s whole C23 7.22.2 / 7.22.3
# limit-and-width family — every `_MIN`/`_MAX`/`_WIDTH` with the TYPE and VALUE the
# references give it on each platform (the promoted type of the platform's own
# typedef), its `#if` value and sign, the `SIG_ATOMIC_*` rows present exactly where
# `<signal.h>` is, `__STDC_VERSION_STDINT_H__`, and each macro equal to its GNU
# predefined twin (`SIZE_MAX == __SIZE_MAX__`) — COMPILED FOR EVERY REAL PAIR through
# the driver's two halves. Reads the shipped config, so it runs through ctest (the
# config snapshot).
dss_add_test(NAME analysis/preprocess/test_stdint_limit_macros
             SOURCES test_stdint_limit_macros.cpp)
target_include_directories(dss_analysis_preprocess_test_stdint_limit_macros
                           PRIVATE ${CMAKE_SOURCE_DIR}/tests)

# P68 (lane lm, #2 — D-C-LINUX-IDENTITY-PREDEFINES-MISSING): the operating-system and
# object-format identity predefines on every real pair — exactly the names the platform's
# references predefine in their ISO modes (the Linux six on ELF, `__APPLE__`/`__MACH__` on
# Mach-O, `_WIN32`/`_WIN64` on pe), never the GNU-mode `linux`/`unix`, and on the ELF pairs
# the surface the Linux claims imply. Reads the shipped config, so it runs through ctest
# (the config snapshot).
dss_add_test(NAME analysis/preprocess/test_os_identity_predefines
             SOURCES test_os_identity_predefines.cpp)
target_include_directories(dss_analysis_preprocess_test_os_identity_predefines
                           PRIVATE ${CMAKE_SOURCE_DIR}/tests)
