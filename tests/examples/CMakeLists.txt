# `examples/` ctest harness — registers ONE ctest entry per example
# discovered under <repo>/examples/<lang>/<name>/expected.json. Each
# entry spawns `dss_examples_runner` with the example directory as
# argv[1]; the runner drives the in-process compile + spawn + strict
# assertion sequence (see examples_runner.cpp).
#
# Discovery is at cmake-configure time via file(GLOB_RECURSE). New
# examples need a cmake reconfigure (`cmake .`) before ctest sees
# them — the trade-off is zero runtime discovery cost AND ctest -j
# parallelism across examples.
#
# Working directory invariant: the runner uses `ScratchDir(InsideRepo)
# + useAsCwd()`. ScratchDir walks UP from cwd looking for
# `src/dss-config/`, so the test process MUST start in (or under)
# the repo tree. CMake's WORKING_DIRECTORY is set to ${CMAKE_SOURCE_DIR}
# below to guarantee this on every host.

add_executable(dss_examples_runner examples_runner.cpp)
target_link_libraries(dss_examples_runner
    PRIVATE
        dsscp-lib
        GTest::gtest
        GTest::gtest_main
        nlohmann_json::nlohmann_json
)
target_include_directories(dss_examples_runner
    PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/tests/test_support
)

# Reuse the shared test PCH (defined in tests/CMakeLists.txt). The extra
# nlohmann_json include dir on this target is additive and does not affect the
# precompiled STL/gtest set.
#
# ★ GUARDED 2026-08-03 (TF-C112), D-BUILD-PCH-REUSE-FROM-UNGUARDED-IN-DEBUG — the
# SECOND of the two unguarded REUSE_FROM sites (the other is tests/CMakeLists.txt).
# This comment previously ended "No-op in Debug (PCH disabled)", which was FALSE and
# is the reason both sites survived review: a CONSUMER `REUSE_FROM` call is NOT a
# no-op when CMAKE_DISABLE_PRECOMPILE_HEADERS is set — it warns, once per emission,
# per target. Only the PRODUCER form (`target_precompile_headers(... PRIVATE ...)`,
# src/CMakeLists.txt) is genuinely silent, and that asymmetry is what made the
# claim look true. MEASURED on this tree: the two sites together emitted 633
# CMake warnings per Debug configure, and they were 633 of 633 — i.e. this single
# message WAS the entire warning channel, so any real CMake warning was invisible.
# Guard on the same variable the sibling dss_pch_reproducible() opens with, so the
# two cannot drift apart.
if(NOT CMAKE_DISABLE_PRECOMPILE_HEADERS)
    target_precompile_headers(dss_examples_runner REUSE_FROM dss_test_pch)
endif()
dss_pch_reproducible(dss_examples_runner)  # consumer must match producer's PCH flags

# ★ THE -D THAT MAKES THE LINE ABOVE TRUE. This target is declared with a bare
# `add_executable`, so it never passed through `dss_add_test`, which is where
# every OTHER test binary picks up `DSS_TEST_REPO_ROOT` (tests/CMakeLists.txt).
# It nonetheless REUSEs `dss_test_pch`, and the PRODUCER *is* built with that
# definition — so the consumer's macro set differed from the producer's, which
# is exactly the invariant the line above claims to be enforcing and the one
# tests/CMakeLists.txt states outright: "GCC invalidates a precompiled header
# whose consumer has a different -D set". ⚠ MEASURED on MSVC 19.51: a single
# `warning C4651: '/DDSS_TEST_REPO_ROOT=...' specified for precompiled header
# but not for current compile` — the ONLY warning in an 831-test Debug build,
# which is how it was noticed at all.
#
# Not cosmetic, and the consequence differs per compiler, which is why the
# warning was worth reading rather than filtering. `repo_root.hpp` resolves the
# repo root through three candidates and (2) is `#ifdef DSS_TEST_REPO_ROOT`;
# compiled without it, THIS binary silently loses candidate (2) and falls to
# the directory walk — while every other test binary keeps it. The baked root
# exists precisely so an out-of-tree build resolves config with NO environment
# (see the `regression/out_of_tree_baked_root` arm), so the one runner that
# spawns the whole example corpus was the one binary that could not use it.
target_compile_definitions(dss_examples_runner PRIVATE
    DSS_TEST_REPO_ROOT="${CMAKE_SOURCE_DIR}")

# ★★ THE GLOB MOVED TO THE ROOT `CMakeLists.txt` AND THIS FILE NOW READS IT.
# D-TEST-INTEGRATED-RUNNER-WALKS-EVERY-EXAMPLE-IN-ONE-THREAD gave the CLI-
# subprocess sibling per-example ctest entries too, so BOTH harnesses derive
# their entry set from the corpus. Two independent globs would let them
# enumerate different corpora while both stayed green — the
# [[D-EXAMPLES-RUNNER-TWO-RUNNERS-MUST-AGREE]] divergence in its most basic
# form. One glob, hoisted, read here and in `integrated_tests/CMakeLists.txt`;
# the root also REFUSES a collapsed result, which this file could not do alone
# because a collapse looks identical to a small corpus from inside one consumer.
set(EXAMPLE_MANIFESTS ${DSS_EXAMPLE_MANIFESTS})

# D-TEST-MANIFEST-ARM64-ARM-WITHOUT-EMULATOR: the corpus emulator lint is a
# CORPUS-wide question with one corpus-wide answer, so it is ONE entry, not 611
# (✔RE-MEASURED 2026-08-20; this line said 558, which was true when it was
# written and has been wrong by 53 since - a raw count of a growing corpus is a
# citation that rots in place, so read the glob, never this number).
# The per-example entries exclude it; the lint entry selects it and receives the
# corpus ROOT as argv[1] instead of an example dir. The filter goes AFTER the
# directory argument so argv[1] keeps its meaning in both shapes.
# ★★★ A `--gtest_filter` THAT MATCHES NOTHING EXITS **0**, AND THAT IS THE HOLE
# THE DEFAULT-IN LOOP BELOW DOES NOT CLOSE BY ITSELF. ✔MEASURED 2026-08-20 on the
# vendored googletest: `--gtest_filter=ExamplesCorpusLint.NoSuchTest` prints
# `[  PASSED  ] 0 tests.` plus a WARNING and returns **rc=0**. So a typo in a
# GTest name — in this file, in a rename, anywhere — buys a permanently GREEN
# ctest entry that asserts nothing, which is precisely the vacuous-coverage class
# `D-TEST-EXAMPLES-CORPUS-LINT-ENTRY-SELECTS-ONE-TEST-NOT-THE-SUITE` was opened
# for. The pair list makes the STRUCTURE safe; this makes the STRING safe.
#
# ⚠ It is a ctest property and not a gtest flag on purpose: this googletest has
# `--gtest_fail_if_no_test_linked` (nothing linked at all) but NO flag for
# "the filter selected none", so the only signal the runner emits is that
# warning line — and reading the tool's own words is this repository's rule for
# a witness. It costs nothing on a healthy entry and applies to every shape
# registered here: per-example, dedicated lint, and the catch-all (whose
# negatives could in principle grow to eat it whole).
function(dss_refuse_a_vacuous_gtest_selection test_name)
    set_tests_properties(${test_name} PROPERTIES
        FAIL_REGULAR_EXPRESSION "did not match any test")
endfunction()

set(EXAMPLES_LINT_SUITE "ExamplesCorpusLint")

# ★ THE ENTRY IS NAMED FOR THE SUITE, NOT FOR ONE OF ITS TESTS — renamed from
# `examples/manifest-emulator-lint` in AP6 (2026-08-14) because the name had
# started DENYING what the entry runs. The `--gtest_filter` below selects the
# whole `ExamplesCorpusLint` suite, and that suite has grown past the emulator
# question: it now also carries the recursive-staging pin
# (`StagesNestedSubdirectoriesWithContentIntact`) and the single-definition-site
# pin (`StagingPrimitiveLivesOnlyInTheSharedHeader`), neither of which has
# anything to do with an emulator. A reader triaging a red
# `examples/manifest-emulator-lint` would have gone looking at manifests and
# qemu for a failure in a filesystem walk.
#
# ★★★ D-TEST-EXAMPLES-CORPUS-LINT-ENTRY-SELECTS-ONE-TEST-NOT-THE-SUITE — AND THE
# PARAGRAPH ABOVE IS WHY THIS WAS NOT NOTICED FOR THIRTEEN CYCLES. It says the
# filter selects the WHOLE suite. ✔MEASURED 2026-08-20: it named exactly ONE
# test, so of the **8** lints defined that day **2** were selected and **6** were
# selected by NO ctest entry at all — the per-example entries exclude
# `${EXAMPLES_LINT_SUITE}.*` wholesale, so nothing else could pick them up.
# (Three of the six had been dark since P5b, when the suite held 5; the other
# three were written by the cycle that found this.) They compiled, they linked, `--gtest_filter`
# could still run them by hand, and the gate never executed one of their
# assertions. ✔ROOT-CAUSED to `2fc87192` (cycle P5b, 2026-08-15), which split
# the census onto its own entry and, in the same edit, narrowed this filter from
# `${EXAMPLES_LINT_SUITE}.*` to a single test name — with THIS comment left
# saying otherwise. The intent was only to give the census its own NAME; the
# side effect was that every lint not explicitly named stopped running.
#
# ★★ SO THE REGISTRATION IS NOW DEFAULT-IN, AND THAT IS THE WHOLE FIX. A lint
# runs unless something deliberately subtracts it, and the subtraction and the
# dedicated entry that replaces it are produced by the SAME loop over
# `EXAMPLES_LINT_NAMED_ENTRIES` below — so they CANNOT drift apart the way a
# hand-written filter and a hand-written `add_test` did. ⚠ Do NOT "simplify"
# this back into two literal filters: the literal form is what failed, and it
# failed silently, in the direction that greens the gate.
# ⚠ THE OLD NAME IS CITED AS LANDED EVIDENCE by the CLOSED registry row
# `D-TEST-MANIFEST-ARM64-ARM-WITHOUT-EMULATOR` above. That citation was
# ANNOTATED with this rename rather than rewritten — a closed row records what
# was true when it closed, and silently editing its evidence to match today's
# tree is how an audit trail stops being one. Anything else naming this entry
# must move WITH it; ✔MEASURED at the rename, the repo-wide citations were this
# file, `tests/examples/examples_runner.cpp` (x2) and that one registry row.
# ⚠ ✔RE-MEASURED 2026-08-20: the set has GROWN — this file names the entry at 4
# sites and `examples_runner.cpp` at 4, plus the registry. The figure above is
# history, correct for its date; the number a future renamer must act on is the
# one they measure themselves. Do not read a dated count as a current one.

foreach(MANIFEST ${EXAMPLE_MANIFESTS})
    get_filename_component(EXAMPLE_DIR ${MANIFEST} DIRECTORY)
    get_filename_component(EXAMPLE_NAME ${EXAMPLE_DIR} NAME)
    get_filename_component(LANG_DIR ${EXAMPLE_DIR} DIRECTORY)
    get_filename_component(LANG_NAME ${LANG_DIR} NAME)
    set(TEST_NAME "examples/${LANG_NAME}/${EXAMPLE_NAME}")
    add_test(NAME ${TEST_NAME}
             COMMAND dss_examples_runner ${EXAMPLE_DIR}
                     --gtest_filter=-${EXAMPLES_LINT_SUITE}.*
             WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    # D-TEST-SHIPPED-CONFIG-EXPOSURE-UNFIXED-OUTSIDE-THE-SUITE-THAT-FLAKED.
    # ★ These entries carried NO `DSS_CONFIG_ROOT` at all: the runner's
    # `ScratchDir(InsideRepo) + useAsCwd()` put the cwd under the repo, so every
    # compile in every one of them resolved shipped config by the ancestor walk
    # onto the LIVE tree — which makes this loop the single largest population of
    # live-tree readers in the repository, and the one a `dss_add_test`-only fix
    # would have left untouched. They now read the run's snapshot like everything
    # else. ⚠ BEFORE `dss_strict_arm_verdicts`, which re-sets `ENVIRONMENT` from
    # what it reads and would drop this if called first.
    dss_use_config_snapshot(${TEST_NAME})
    dss_strict_arm_verdicts(${TEST_NAME})
    dss_refuse_a_vacuous_gtest_selection(${TEST_NAME})
endforeach()

# D-CSUBSET-INLINE-FUNCTION-NO-EXTERNAL-DEFINITION-EMITTED: the carve-out census
# gets its OWN entry rather than riding the general lint entry's. Both are
# corpus-wide and both live in ${EXAMPLES_LINT_SUITE}; the census gets its own
# NAME so a census failure is reported as one, instead of arriving under the
# generic `examples/corpus-lints` and being investigated as a manifest problem.
# ⚠ THIS PARAGRAPH USED TO ARGUE AGAINST `${EXAMPLES_LINT_SUITE}.*` ITSELF, on
# the ground that a census failure would surface under the name
# "manifest-emulator-lint". ✔That hazard was retired by the AP6 rename above —
# no entry has carried that name since — and the argument survived it, which is
# how the catch-all ended up narrowed to a single test name. The catch-all IS
# `${EXAMPLES_LINT_SUITE}.*` again, minus exactly what the loop below moves onto
# its own entry. The per-example entries' exclusion filter is still the whole
# suite, so nothing runs twice.
#
# ⇒ ONE PAIR PER LINT THAT EARNS ITS OWN CTEST NAME: "<test>|<ctest entry name>".
# Add a pair here ONLY to give a lint a more findable name than the catch-all —
# never to remove it from the gate, which this list structurally cannot do.
set(EXAMPLES_LINT_NAMED_ENTRIES
    "OnlyDeclaredExamplesMayDivergeFromTheirBaseline|examples/optimization-observable-census")

set(_dss_lint_excluded "")
foreach(_pair ${EXAMPLES_LINT_NAMED_ENTRIES})
    string(REPLACE "|" ";" _parts "${_pair}")
    list(LENGTH _parts _n)
    if(NOT _n EQUAL 2)
        message(FATAL_ERROR
            "EXAMPLES_LINT_NAMED_ENTRIES entry '${_pair}' must be "
            "\"<GTestName>|<ctest entry name>\"")
    endif()
    list(GET _parts 0 _lint_test)
    list(GET _parts 1 _lint_entry)
    add_test(NAME "${_lint_entry}"
             COMMAND dss_examples_runner ${CMAKE_SOURCE_DIR}/examples
                     --gtest_filter=${EXAMPLES_LINT_SUITE}.${_lint_test}
             WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    dss_use_config_snapshot("${_lint_entry}")
    dss_refuse_a_vacuous_gtest_selection("${_lint_entry}")
    # The SAME iteration that gave this lint an entry takes it out of the
    # catch-all — the two halves of "moved, not dropped" are one statement pair.
    if(_dss_lint_excluded STREQUAL "")
        set(_dss_lint_excluded "${EXAMPLES_LINT_SUITE}.${_lint_test}")
    else()
        string(APPEND _dss_lint_excluded ":${EXAMPLES_LINT_SUITE}.${_lint_test}")
    endif()
endforeach()

# gtest filter grammar: `positive[:positive...][-negative[:negative...]]`, so the
# minus sign appears at most ONCE however many lints are subtracted.
if(_dss_lint_excluded STREQUAL "")
    set(_dss_lint_catchall "${EXAMPLES_LINT_SUITE}.*")
else()
    set(_dss_lint_catchall "${EXAMPLES_LINT_SUITE}.*-${_dss_lint_excluded}")
endif()
add_test(NAME "examples/corpus-lints"
         COMMAND dss_examples_runner ${CMAKE_SOURCE_DIR}/examples
                 --gtest_filter=${_dss_lint_catchall}
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
dss_use_config_snapshot("examples/corpus-lints")
dss_refuse_a_vacuous_gtest_selection("examples/corpus-lints")
