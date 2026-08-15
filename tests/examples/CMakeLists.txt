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
        dss-code-prime-lib
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
# src/CMakeLists.txt:51) is genuinely silent, and that asymmetry is what made the
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

file(GLOB_RECURSE EXAMPLE_MANIFESTS
    CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/examples/*/*/expected.json")

# D-TEST-MANIFEST-ARM64-ARM-WITHOUT-EMULATOR: the corpus emulator lint is a
# CORPUS-wide question with one corpus-wide answer, so it is ONE entry, not 558.
# The per-example entries exclude it; the lint entry selects it and receives the
# corpus ROOT as argv[1] instead of an example dir. The filter goes AFTER the
# directory argument so argv[1] keeps its meaning in both shapes.
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
# ⚠ THE OLD NAME IS CITED AS LANDED EVIDENCE by the CLOSED registry row
# `D-TEST-MANIFEST-ARM64-ARM-WITHOUT-EMULATOR` above. That citation was
# ANNOTATED with this rename rather than rewritten — a closed row records what
# was true when it closed, and silently editing its evidence to match today's
# tree is how an audit trail stops being one. Anything else naming this entry
# must move WITH it; ✔MEASURED at the rename, the repo-wide citations were this
# file, `tests/examples/examples_runner.cpp` (x2) and that one registry row.

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
    dss_strict_arm_verdicts(${TEST_NAME})
endforeach()

add_test(NAME "examples/corpus-lints"
         COMMAND dss_examples_runner ${CMAKE_SOURCE_DIR}/examples
                 --gtest_filter=${EXAMPLES_LINT_SUITE}.EveryArmAgreesWithItsSiblingsOnTheEmulator
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# D-CSUBSET-INLINE-FUNCTION-NO-EXTERNAL-DEFINITION-EMITTED: the carve-out census
# gets its OWN entry rather than riding the emulator lint's. Both are corpus-wide
# and both live in ${EXAMPLES_LINT_SUITE}, but a `${EXAMPLES_LINT_SUITE}.*` filter
# would report a census failure under the name "manifest-emulator-lint" — a red
# test whose NAME points at the wrong question, which is how a finding gets
# investigated in the wrong place and then re-filed as flaky. The per-example
# entries' exclusion filter is still the whole suite, so nothing runs twice.
add_test(NAME "examples/optimization-observable-census"
         COMMAND dss_examples_runner ${CMAKE_SOURCE_DIR}/examples
                 --gtest_filter=${EXAMPLES_LINT_SUITE}.OnlyDeclaredExamplesMayDivergeFromTheirBaseline
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
