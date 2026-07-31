# Tests root. Each module's tests live under tests/<module>/.

# Helper: register a GoogleTest executable, link it against the shared lib,
# and register it with ctest. Usage:
#   dss_add_test(NAME core/test_strong_ids SOURCES test_strong_ids.cpp)
function(dss_add_test)
    set(options)
    set(oneValueArgs NAME)
    set(multiValueArgs SOURCES)
    cmake_parse_arguments(DSS "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    string(REPLACE "/" "_" target "dss_${DSS_NAME}")
    add_executable(${target} ${DSS_SOURCES})
    target_link_libraries(${target} PRIVATE dss-code-prime-lib GTest::gtest GTest::gtest_main)
    target_include_directories(${target}
        PRIVATE
            ${CMAKE_SOURCE_DIR}/src
            ${CMAKE_SOURCE_DIR}/tests/test_support
    )

    # Reuse the shared test PCH (heavy STL + <gtest/gtest.h>). All test
    # targets compile with identical flags (DSS_SHARED_BUILD via the lib's
    # PUBLIC define, the same includes), so the one precompiled header is
    # valid for every one of them. No-op in Debug (CMAKE_DISABLE_PRECOMPILE_
    # HEADERS), so the linux-clang-asan leg builds PCH-free.
    target_precompile_headers(${target} REUSE_FROM dss_test_pch)
    dss_pch_reproducible(${target})  # consumer must match producer's PCH flags

    add_test(NAME ${DSS_NAME} COMMAND ${target})

    # Shipped-config discovery: findShippedConfig() prefers $DSS_CONFIG_ROOT
    # (a directory containing src/dss-config/) over its cwd-walk. Point it at
    # the repo root so every test resolves shipped config regardless of build
    # location — an OUT-OF-TREE build's ctest cwd is a build subdirectory with
    # no src/dss-config/ in its ancestry, which would otherwise null the loader
    # (loadShipped → nullptr → downstream null-deref). In-tree builds already
    # walk to the config; this makes out-of-tree/CI builds work identically.
    # (CTest ENVIRONMENT is a ';'-separated list; a repo path containing a
    # literal ';' would mis-split — impossible on normal Windows/WSL paths.)
    set_tests_properties(${DSS_NAME} PROPERTIES
        ENVIRONMENT "DSS_CONFIG_ROOT=${CMAKE_SOURCE_DIR}")
endfunction()

# Shared PCH producer for the test executables (see DSS_PCH_HEADERS in the
# top-level CMakeLists). The ~166 test TUs are the larger half of the cold
# compile and every one of them includes <gtest/gtest.h> + the heavy STL set;
# precompiling them ONCE here and reusing across all test targets removes that
# reparse. It links dss-code-prime-lib + GTest::gtest purely to inherit their
# usage-requirement flags (the PUBLIC DSS_SHARED_BUILD define, the gtest include
# dir) so the precompiled header matches every consumer's compile line exactly.
# `dss_test_pch` is never linked anywhere; REUSE_FROM wires the build dependency.
add_library(dss_test_pch STATIC ${CMAKE_CURRENT_SOURCE_DIR}/test_support/pch_stub.cpp)
target_link_libraries(dss_test_pch PRIVATE dss-code-prime-lib GTest::gtest)
target_include_directories(dss_test_pch
    PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/tests/test_support
)
target_precompile_headers(dss_test_pch PRIVATE ${DSS_PCH_HEADERS} <gtest/gtest.h>)
dss_pch_reproducible(dss_test_pch)  # producer: timestamp-free PCH → ccache hits

add_subdirectory(core)
add_subdirectory(test_support)
add_subdirectory(tokenizer)
add_subdirectory(analysis)
add_subdirectory(analysis/syntactic)
add_subdirectory(analysis/compilation_unit)
add_subdirectory(analysis/preprocess)
add_subdirectory(analysis/semantic)
add_subdirectory(hir)
add_subdirectory(mir)
add_subdirectory(opt)
add_subdirectory(lir)
add_subdirectory(asm)
add_subdirectory(link)
add_subdirectory(ffi)
add_subdirectory(program)
add_subdirectory(lsp)
add_subdirectory(examples)

# ── TF-C91 (D-TEST-SCHEMA-TEMPORARY-DANGLING-REFERENCE) — the ORDER arm ──
#
# WHY THIS EXISTS. The dangling-reference defect this cycle fixed was a
# heap-use-after-free that read GREEN in every full-suite run and only bit when
# the affected test ran ALONE: whether the freed schema's bytes are still intact
# depends entirely on what allocated/freed before it, i.e. on TEST ORDER. The
# gate ran each suite exactly once, in declaration order, forever — so it had
# structurally zero chance of seeing it, and a Windows developer found it
# instead. `--gtest_shuffle` re-seeds and re-permutes before EVERY repetition
# (MEASURED: seeds 75986, 75987, 75988 on consecutive iterations), so N
# repetitions explore N independent orders per gate run.
#
# STATE ITS SCOPE OUT LOUD — a probabilistic instrument that oversells itself is
# worse than none. This arm is a SAMPLER, not a proof: green means "no order
# among the N sampled orders exposed a failure", never "order-independent". It
# covers ONE suite, the one that carried the defect; extending it to the other
# 206 `dss_add_test` targets is a separate decision with its own gate-cost
# measurement, deliberately not taken here.
#
# AND IT IS THE WEAKER INSTRUMENT — say so rather than let it imply otherwise.
# It cannot see UB that the host allocator declines to punish: on macOS/libc++
# this very defect measured 0/40 even running ALONE, and only ASan caught it
# (40/40, with the freeing stack). The strictly stronger instrument therefore
# already exists — the `linux-clang-asan` leg of pipeline-pr.yml (Debug,
# `-fsanitize=address,undefined`, full ctest under `ASAN_OPTIONS=abort_on_error=1`)
# and MEASURED at 5/5 red on the broken tree. That leg is gated on the PR
# carrying the `Run Pipes` label (pipeline-pr.yml:89), which is why the gate
# never saw this. This arm narrows the hole; it does not close it.
#
# ON FAILURE: the arm prints `Note: Randomizing tests' orders with a seed of
# NNNNN` before each iteration; re-run with `--gtest_random_seed=NNNNN` to
# replay that exact order.
#
# COST, MEASURED on this host (Release, Apple clang, 314 tests):
#   repeat=1 -> 0.31 s   repeat=10 -> 3.00 s
#   repeat=20 -> 6.03 s  repeat=40 -> 12.11 s   (~0.30 s per order, linear)
# N=20 chosen: 20 independent orders for 6.0 s. It is its OWN ctest entry, so
# the existing `analysis/preprocess/test_preprocessor` entry keeps its ~0.7 s and
# ctest's -j overlaps this one with the other 745 entries.
#
# CROSS-PLATFORM: `COMMAND <target>` makes CMake substitute the built
# executable's real path (with `.exe` on Windows), so this runs as-is on macOS,
# Linux and Windows CI. Note it names NO interpreter, and that is the
# surrounding convention followed rather than broken: the sibling script-backed
# entries at ../CMakeLists.txt:228-241 must dispatch `bash` vs
# `powershell -File` precisely because their command is a SCRIPT. This command is
# a binary; there is nothing to interpret.
#
# CONCURRENCY — AND WHY THERE IS DELIBERATELY NO `RUN_SERIAL` HERE
# (D-TEST-INTEGRATED-FIXED-TEMP-PATH-COLLIDES). This arm is a SECOND ctest entry
# on the SAME binary, so an ordinary `ctest -j` overlaps it with the plain entry
# and TWO live processes of that binary run at once. That is by design and must
# stay that way: `RUN_SERIAL`/`RESOURCE_LOCK` would bury the very contention that
# is cheapest to catch right here, and would spend every gate run the 6.0 s this
# arm was budgeted to overlap away.
#
# The obligation that buys the overlap falls on the SUITE, not on ctest: a suite
# registered on this arm must derive every scratch path from a PER-RUN root, and
# never from a constant one. test_preprocessor.cpp meets that at its
# `ppScratchRoot()` chokepoint (see the docblock there, and the shared scheme in
# tests/test_support/scratch_dir.hpp). MEASURED before that root existed: the two
# entries clobbered each other's fixture files and `ctest -j 8` over just this
# pair went red 1 run in 3. Any suite added to this arm inherits the obligation.
function(dss_add_shuffle_arm)
    cmake_parse_arguments(DSS "" "NAME;REPEAT" "" ${ARGN})
    string(REPLACE "/" "_" target "dss_${DSS_NAME}")
    add_test(NAME ${DSS_NAME}_shuffled
             COMMAND ${target} --gtest_shuffle --gtest_repeat=${DSS_REPEAT})
    # Mirrors dss_add_test's ENVIRONMENT (see the note at its set_tests_properties
    # above): findShippedConfig() prefers $DSS_CONFIG_ROOT over its cwd-walk, so
    # an out-of-tree build resolves shipped config identically.
    set_tests_properties(${DSS_NAME}_shuffled PROPERTIES
        ENVIRONMENT "DSS_CONFIG_ROOT=${CMAKE_SOURCE_DIR}")
endfunction()

dss_add_shuffle_arm(NAME analysis/preprocess/test_preprocessor REPEAT 20)
