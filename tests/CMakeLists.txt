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
    target_link_libraries(${target} PRIVATE dsscp-lib GTest::gtest GTest::gtest_main)
    target_include_directories(${target}
        PRIVATE
            ${CMAKE_SOURCE_DIR}/src
            ${CMAKE_SOURCE_DIR}/tests/test_support
    )

    # Reuse the shared test PCH (heavy STL + <gtest/gtest.h>). All test
    # targets compile with identical flags (DSS_SHARED_BUILD via the lib's
    # PUBLIC define, the same includes), so the one precompiled header is
    # valid for every one of them.
    #
    # GUARDED, not unconditional (D-BUILD-PCH-REUSE-FROM-UNGUARDED-IN-DEBUG).
    # The Debug canary sets CMAKE_DISABLE_PRECOMPILE_HEADERS (../CMakeLists.txt
    # ~:162) so the linux-clang-asan leg builds PCH-free — but REUSE_FROM is NOT
    # a no-op under it the way a producer call is. CMake still records the
    # property, then complains at generate time that the producer has PCH
    # disabled: MEASURED on a Debug Ninja configure, 633 `CMake Warning ...
    # "PRECOMPILE_HEADERS_REUSE_FROM" ... has set "DISABLE_PRECOMPILE_HEADERS";
    # ignoring` lines (3 per consumer x 211 dss_add_test targets), and they were
    # 633 of the configure's 633 warnings — i.e. the entire warning channel was
    # this one line, which is exactly where a real warning goes unseen.
    #
    # Same predicate as the sibling on the next line: dss_pch_reproducible opens
    # with `if(CMAKE_DISABLE_PRECOMPILE_HEADERS) return()`. Both must key off the
    # one variable, because both are about the same fact ("there is no PCH here");
    # this one reads inverted only because a function body follows it and there is
    # nothing to early-return from mid-helper.
    if(NOT CMAKE_DISABLE_PRECOMPILE_HEADERS)
        target_precompile_headers(${target} REUSE_FROM dss_test_pch)
    endif()
    dss_pch_reproducible(${target})  # consumer must match producer's PCH flags

    # Repo root, baked at CONFIGURE time. This is candidate (2) of
    # `tests/test_support/repo_root.hpp` and the reason an out-of-tree build
    # resolves config with NO environment at all — the ENVIRONMENT property
    # below only reaches a process ctest itself launches, so it cannot help a
    # binary run directly from a shell, and it is exactly what the
    # `regression/out_of_tree_baked_root` arm strips to prove this path works
    # on its own. Must ALSO be on dss_test_pch (see the producer below): GCC
    # invalidates a precompiled header whose consumer has a different -D set,
    # which is why the "identical flags" note above is load-bearing.
    target_compile_definitions(${target} PRIVATE
        DSS_TEST_REPO_ROOT="${CMAKE_SOURCE_DIR}")

    add_test(NAME ${DSS_NAME} COMMAND ${target})

    # Shipped-config discovery: findShippedConfig() — and, since TF-C118, the
    # test-side resolver in tests/test_support/repo_root.hpp — prefers
    # $DSS_CONFIG_ROOT (a directory containing src/dss-config/) over its
    # cwd-walk. Point it at the repo root so every test resolves shipped config
    # regardless of build location — an OUT-OF-TREE build's ctest cwd is a build
    # subdirectory with no src/dss-config/ in its ancestry, which would
    # otherwise null the loader (loadShipped → nullptr → downstream null-deref).
    # In-tree builds already walk to the config; this makes out-of-tree/CI
    # builds work identically.
    # ⚠ SETTING IT WAS NEVER SUFFICIENT. This property has been here since the
    # out-of-tree gap was first noticed, and an out-of-tree ctest still failed
    # 28 entries, because seventeen test-side helpers carried private cwd-walks
    # that never READ the variable. The fix was the shared resolver; this line
    # is one of the three candidates it consults, not the mechanism.
    # (CTest ENVIRONMENT is a ';'-separated list; a repo path containing a
    # literal ';' would mis-split — impossible on normal Windows/WSL paths.)
    set_tests_properties(${DSS_NAME} PROPERTIES
        ENVIRONMENT "DSS_CONFIG_ROOT=${CMAKE_SOURCE_DIR}")
endfunction()

# Shared PCH producer for the test executables (see DSS_PCH_HEADERS in the
# top-level CMakeLists). The ~166 test TUs are the larger half of the cold
# compile and every one of them includes <gtest/gtest.h> + the heavy STL set;
# precompiling them ONCE here and reusing across all test targets removes that
# reparse. It links dsscp-lib + GTest::gtest purely to inherit their
# usage-requirement flags (the PUBLIC DSS_SHARED_BUILD define, the gtest include
# dir) so the precompiled header matches every consumer's compile line exactly.
# `dss_test_pch` is never linked anywhere; REUSE_FROM wires the build dependency.
add_library(dss_test_pch STATIC ${CMAKE_CURRENT_SOURCE_DIR}/test_support/pch_stub.cpp)
target_link_libraries(dss_test_pch PRIVATE dsscp-lib GTest::gtest)
target_include_directories(dss_test_pch
    PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/tests/test_support
)
# Same -D the consumers get from dss_add_test. NOT optional: GCC rejects a
# precompiled header whose consumer's command-line macro set differs from the
# producer's, so omitting it here would turn every test TU's `-Winvalid-pch`
# into a rebuild-from-source (or an error), which is the "identical flags"
# invariant the comment above depends on.
target_compile_definitions(dss_test_pch PRIVATE
    DSS_TEST_REPO_ROOT="${CMAKE_SOURCE_DIR}")
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
add_subdirectory(harness)
# The DIFFERENTIAL accept/reject oracle (D-CONF-REFERENCE-DIFFERENTIAL-ORACLE):
# diffs DSS's accept/reject verdicts against every reference C compiler on the
# host, in BOTH directions — they-accept-we-reject AND we-accept-nobody-accepts.
add_subdirectory(conformance)

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
# carrying the `Run Pipes` label (pipeline-pr.yml), which is why the gate
# never saw this. This arm narrows the hole; it does not close it.
#
# ON FAILURE: the arm prints `Note: Randomizing tests' orders with a seed of
# NNNNN` before each iteration; re-run with `--gtest_random_seed=NNNNN` to
# replay that exact order.
#
# COST, MEASURED on this host (Release, Apple clang, 314 tests):
#   repeat=1 -> 0.31 s   repeat=10 -> 3.00 s
#   repeat=20 -> 6.03 s  repeat=40 -> 12.11 s   (~0.30 s per order, linear)
# N=20 chosen: 20 independent orders for 6.0 s. ⚠ THAT PRICE IS THIS CONFIG'S, NOT
# EVERY CONFIG'S — the same 20 orders cost 598.72 s on the Debug+ASan+UBSan leg,
# ~100x this figure, which is why N is now divided per leg rather than taken as a
# global constant; see the divisor block above dss_add_shuffle_arm. It is its OWN ctest entry, so
# the existing `analysis/preprocess/test_preprocessor` entry keeps its ~0.7 s and
# ctest's -j overlaps this one with the other 745 entries.
#
# CROSS-PLATFORM: `COMMAND <target>` makes CMake substitute the built
# executable's real path (with `.exe` on Windows), so this runs as-is on macOS,
# Linux and Windows CI. Note it names NO interpreter, and that is the
# surrounding convention followed rather than broken: the sibling script-backed
# entries in ../CMakeLists.txt must dispatch `bash` vs
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
# ── THE COST IS PER-LEG, SO THE REPEAT COUNT HAS TO BE TOO
# ── (D-TEST-SHUFFLE-ARM-REPEAT-BUDGET-IS-PER-LEG) ──
#
# Every N below was chosen against a cost that differs by ~28x between the legs
# that run it, so no single global constant can be right on all of them. MEASURED,
# CI run 30815752993, both legs 773 tests under `ctest --parallel 4` on a 4-core
# runner (sum of per-test wall times, from each job's own ctest output):
#
#     leg                    whole suite      these 8 arms
#     linux-gcc-release          301.8 s     117.8 s  (39%)
#     linux-clang-asan        ~7565.0 s*    3311.5 s  (44%)
#     * 7221.3 s measured over the 687 tests that ran + ~344 s projected for the
#       86 that did not, at the ~4 s/test the surrounding examples averaged.
#
# The Release column is what the block above sized N against ("repeat=20 -> 6.03 s",
# Apple clang). The sanitizer column is what the SAME arms actually cost on the leg
# this file calls the stronger instrument. That leg's Test step is budgeted 30 min
# (pipeline-pr.yml), needed ~31.5 min, and so was KILLED at 687/773 — every test it
# reached PASSED. The 86 it never reached were all examples/c, t* through z*,
# so they took ZERO sanitizer coverage, every run, always the same tail.
#
# WHY TRIMMING HERE RAISES TOTAL DETECTION INSTEAD OF LOWERING IT. These arms are
# the weaker instrument and this file already says so (~line 100): the motivating
# defect measured 0/40 by order-shuffling alone and 40/40 under ASan, because ASan
# poisons freed memory and catches the read on ANY order rather than waiting for an
# allocator coincidence. So extra orders buy the least exactly where they cost the
# most. Giving up ~18 sampled orders per arm on that one leg, to hand 86 corpus
# examples back their sanitizer coverage, is a net gain — and of those two numbers
# only the 86 was measured at zero.
#
# SAY WHAT IT COSTS RATHER THAN ONLY WHAT IT BUYS. The scratch-path RACE class
# below is NOT made order-independent by ASan the way the UAF is, so fewer repeats
# there really is fewer chances at those sub-millisecond windows. That coverage
# stays at full strength on the four uninstrumented legs, where 20 orders cost
# 117.8 s all told; only the leg that cannot afford them trims. PROJECTED after
# this change (measured base x new repeat): the 8 arms cost ~598 s — still ~5x the
# release leg's absolute sampling time — and the Test step lands ~20.2 min against
# its 30-min budget.
#
# DERIVED FROM THE FLAGS THE COMPILER ACTUALLY RECEIVES, never from a separate
# bookkeeping variable someone has to remember to set: a parallel source of truth
# is a thing that can drift, and the drift that matters here — a fast leg quietly
# dropping to 2 orders — looks exactly like a healthy run in every log we keep.
#
# TRUTH and POLICY therefore stay SEPARATE variables on purpose.
# DSS_SHUFFLE_SANITIZED is an observation of this build; DSS_SHUFFLE_REPEAT_DIVISOR
# is what we decided to do about it. Collapsing them into one would make the guard
# in the function vacuous — it could then only compare a policy against itself, so
# a divisor forced on an uninstrumented build (exactly how this silently loses
# orders) would sail straight through. Kept apart, that case is a configure-time
# FATAL_ERROR; MEASURED by forcing the divisor to 10 with clean flags, which fails
# the configure with rc=1 rather than quietly registering repeat=2.
string(TOUPPER "${CMAKE_BUILD_TYPE}" _dss_build_type_upper)
set(DSS_SHUFFLE_SANITIZED OFF)
if ("${CMAKE_CXX_FLAGS}" MATCHES "[-/]fsanitize="
        OR "${CMAKE_CXX_FLAGS_${_dss_build_type_upper}}" MATCHES "[-/]fsanitize=")
    set(DSS_SHUFFLE_SANITIZED ON)
endif()

set(DSS_SHUFFLE_REPEAT_DIVISOR 1)
if (DSS_SHUFFLE_SANITIZED)
    set(DSS_SHUFFLE_REPEAT_DIVISOR 10)
endif()
message(STATUS
    "dss: shuffle-arm sanitized=${DSS_SHUFFLE_SANITIZED} "
    "repeat divisor=${DSS_SHUFFLE_REPEAT_DIVISOR}")

function(dss_add_shuffle_arm)
    cmake_parse_arguments(DSS "" "NAME;REPEAT" "" ${ARGN})
    string(REPLACE "/" "_" target "dss_${DSS_NAME}")

    # Integer division, then a floor of 1. An arm sampling ONE order still runs a
    # second concurrent process against the plain entry, which is the whole of what
    # the race class needs; an arm at 0 would simply not exist.
    math(EXPR repeat "${DSS_REPEAT} / ${DSS_SHUFFLE_REPEAT_DIVISOR}")
    if (repeat LESS 1)
        set(repeat 1)
    endif()

    # Cross-check POLICY against the OBSERVED build, not against itself. Reducing
    # the count is legitimate only where ASan detects order-dependent UB without
    # needing the orders; anywhere else it discards sampling that nothing in the
    # gate replaces, and a green run looks exactly the same afterwards.
    if (NOT DSS_SHUFFLE_SANITIZED AND NOT repeat EQUAL DSS_REPEAT)
        message(FATAL_ERROR
            "dss_add_shuffle_arm(${DSS_NAME}): this build is NOT instrumented "
            "(no [-/]fsanitize= in CMAKE_CXX_FLAGS or CMAKE_CXX_FLAGS_"
            "${_dss_build_type_upper}), yet the repeat count was reduced "
            "${DSS_REPEAT} -> ${repeat} by divisor ${DSS_SHUFFLE_REPEAT_DIVISOR}. "
            "That silently drops test orders this leg is the only one sampling.")
    endif()

    add_test(NAME ${DSS_NAME}_shuffled
             COMMAND ${target} --gtest_shuffle --gtest_repeat=${repeat})
    # Mirrors dss_add_test's ENVIRONMENT (see the note at its set_tests_properties
    # above): findShippedConfig() prefers $DSS_CONFIG_ROOT over its cwd-walk, so
    # an out-of-tree build resolves shipped config identically.
    set_tests_properties(${DSS_NAME}_shuffled PROPERTIES
        ENVIRONMENT "DSS_CONFIG_ROOT=${CMAKE_SOURCE_DIR}")
endfunction()

dss_add_shuffle_arm(NAME analysis/preprocess/test_preprocessor REPEAT 20)

# D-TEST-FIXED-SCRATCH-PATH-POPULATION — the suites below all derived a scratch
# path from a CONSTANT and clobbered each other under concurrency; they now derive
# it from ScratchDir's per-run root. These arms are what keep that true. A comment
# saying "do not simplify this back" is not a guard; a second concurrent ctest entry
# on the same binary is, because reverting to a constant path reddens the gate here
# instead of passing quietly. MEASURED pre-fix under exactly this overlap: import
# resolver 6/6 rounds red plus 4 SIGSEGVs, type-name oracle 6/6 red, hir / schema
# cache / c red with DIVERGENT per-instance failure counts. Post-fix all
# green with counts identical between the two instances.
#
# REPEAT is measured, not guessed — single-run wall time on this host was 0.06-0.17 s
# for the five small binaries and 4.13 s for the c one, so 20x and 3x keep the
# whole set near ~23 s of added work spread over six entries that ctest -j overlaps.
# ⚠ THAT ~23 s IS AN UNINSTRUMENTED PRICE. The same eight arms measured 3311.5 s on
# the Debug+ASan+UBSan leg — 44% of that leg's entire budget, and enough to run it
# out of wall clock. The N values here stay the uninstrumented ones; the sanitizer
# leg divides them at the dss_add_shuffle_arm chokepoint.
#
# ONE SUITE IN THE SAME FIX IS DELIBERATELY *NOT* HERE:
#   * program/test_input_resolver — it was already pid-seeded and stayed GREEN pre-fix.
#     It served as the NEGATIVE CONTROL proving the A/B discriminated rather than just
#     producing noise, and it has no constant-path regression to catch.
#
# fc3_width_semantics IS here, but only because a prerequisite was fixed first, and the
# sequence is worth keeping: its two descriptor tests used to assert ONLY negative
# properties (no value, errorCount > 0), which a VANISHED fixture satisfies just as well
# as the malformed content under test. An arm would have been theater — green whether or
# not the per-run root survived. It now fatally asserts the fixture exists on disk
# byte-for-byte AND pins the specific rejection (code plus message; MEASURED, the loader
# reuses F_ShippedLibDescriptorMalformed for at least three distinct rejections, so a
# code-only assertion cannot discriminate them). Proven by a DETERMINISTIC pre/post pair
# that removes the fixture right after the write: pre-change both tests PASS, post-change
# both FAIL. ⚠ Do not "simplify" those existence assertions away — they are what makes
# this arm mean anything.
#
# fc3 and test_compilation_unit are both THINLY exposed: their write-to-read windows are
# sub-millisecond, so a randomized-timing hammer usually misses (MEASURED: one hammer run
# reported 0 fixtures actually deleted across 12,000 test instances — a green that proved
# nothing). That is an argument for MORE repeats, not fewer, which is why they sit at 20
# alongside the suites that redden easily rather than being dialled down.
dss_add_shuffle_arm(NAME analysis/compilation_unit/test_type_name_oracle REPEAT 20)
dss_add_shuffle_arm(NAME analysis/compilation_unit/test_import_resolver REPEAT 20)
dss_add_shuffle_arm(NAME analysis/compilation_unit/test_compilation_unit REPEAT 20)
dss_add_shuffle_arm(NAME hir/test_hir_lowering_multi_lang REPEAT 20)
dss_add_shuffle_arm(NAME lsp/test_schema_cache REPEAT 20)
dss_add_shuffle_arm(NAME analysis/semantic/test_semantic_analyzer_c REPEAT 3)
dss_add_shuffle_arm(NAME analysis/semantic/test_fc3_width_semantics REPEAT 20)

# ── TF-C118 — THE OUT-OF-TREE REGRESSION GUARD ──────────────────────────────
#
# WHAT BROKE. A build directory OUTSIDE the source tree failed 29 of 787 ctest
# entries (MEASURED, Windows/Release, a3af1320; the operator measured the same
# shape as 28/787 on macOS arm64). ~19 test-side helpers each carried a private
# "walk up from cwd looking for src/dss-config" and never read $DSS_CONFIG_ROOT,
# so out-of-tree — where no ancestor of the cwd holds src/dss-config — they all
# missed, and the ones that answered a miss with std::abort() took their whole
# test BINARY down with them. They now share one resolver,
# tests/test_support/repo_root.hpp, whose candidates are:
#   (1) $DSS_CONFIG_ROOT   (2) the CMake-baked DSS_TEST_REPO_ROOT   (3) the walk
#
# WHY AN ARM AND NOT JUST A TEST. Candidates (1) and (3) both work in an
# ordinary in-tree ctest run, so a normal green suite CANNOT observe (2) at all.
# Delete the baked definition and every entry stays green while the out-of-tree
# build silently rots back to 29 red. This arm is the only place where (1) and
# (3) are BOTH unavailable, which is the only condition under which (2) is
# load-bearing enough to fail.
#
# ★ ENVIRONMENT_MODIFICATION, not ENVIRONMENT. dss_add_test sets
# DSS_CONFIG_ROOT on every entry; `ENVIRONMENT` can only overwrite it with
# another value, and any value at all satisfies candidate (1) and makes the arm
# vacuous. `unset:` (CMake 3.22+) is what actually removes it.
#
# ★ THE WORKING DIRECTORY MUST HAVE NO REPO ANCESTRY, and "the build dir" is
# NOT sufficient — for an in-tree build (build-dbg/) the repo root is an
# ancestor, candidate (3) resolves, and the arm proves nothing again. So the
# location is chosen per build layout and then VERIFIED below.
cmake_path(IS_PREFIX CMAKE_SOURCE_DIR "${CMAKE_BINARY_DIR}" NORMALIZE
           _dss_binary_inside_source)
if (_dss_binary_inside_source)
    # In-tree build: anything under the build dir inherits the repo as an
    # ancestor, so step OUT of the repo entirely.
    set(_dss_norepo_cwd "${CMAKE_SOURCE_DIR}/../dss-no-repo-ancestry-cwd")
else()
    set(_dss_norepo_cwd "${CMAKE_BINARY_DIR}/no-repo-ancestry-cwd")
endif()
cmake_path(SET _dss_norepo_cwd NORMALIZE "${_dss_norepo_cwd}")
file(MAKE_DIRECTORY "${_dss_norepo_cwd}")

# VERIFY the property rather than trusting the branch above. A guard whose own
# precondition can rot silently is not a guard; if someone nests a checkout such
# that this directory gains a src/dss-config ancestor, that is a CONFIGURE-time
# error here, not a quietly-vacuous green arm later. (The runtime half of the
# same check lives in BakedRootOnly.PreconditionsHold, which re-verifies from
# inside the running process where ctest's cwd actually applies.)
set(_dss_probe "${_dss_norepo_cwd}")
foreach(_dss_hop RANGE 12)
    if (IS_DIRECTORY "${_dss_probe}/src/dss-config")
        message(FATAL_ERROR
            "dss: the out-of-tree regression arm's working directory "
            "'${_dss_norepo_cwd}' has repo ancestry at '${_dss_probe}' "
            "(it contains src/dss-config). The arm would then resolve via the "
            "cwd ancestor walk and silently stop testing the CMake-baked "
            "DSS_TEST_REPO_ROOT, which is the only thing it exists to test.")
    endif()
    cmake_path(GET _dss_probe PARENT_PATH _dss_parent)
    if (_dss_parent STREQUAL _dss_probe)
        break()
    endif()
    set(_dss_probe "${_dss_parent}")
endforeach()

# ★ ONE entry, not three — and the reason is a MEASURED constraint, not a
# preference. The obvious design is to point this condition at real consumers
# (say analysis/syntactic/test_corpus for corpusRoot() and
# core/test_header_name_matching for configRoot()). Those binaries also call the
# COMPILER-side loader — `GrammarSchema::loadShipped` -> `findShippedConfig`
# (src/core/types/config_path_walk.cpp), whose candidates are only
# $DSS_CONFIG_ROOT and the cwd walk. It has NO baked value ON PURPOSE: baking a
# build machine's repo path into the shipped compiler would be wrong. So
# stripping DSS_CONFIG_ROOT breaks those binaries for a reason that has nothing
# to do with the test-side resolver, and the arm would be red forever.
# MEASURED, from the no-repo cwd with the variable unset:
# `dss_analysis_syntactic_test_corpus` fails at golden_file.hpp — and would go
# on to fail `loadShipped("c")` even with the corpus root resolved.
# So the guard suite is itself the representative consumer: BakedRootOnly drives
# `dss::test_support::findCorpusRoot()` — the exact entry point the twelve
# suites that used to abort() go through — and resolves a real file under both
# corpusRoot() and configRoot(), which is the whole of what this arm can
# honestly claim.
add_test(NAME regression/out_of_tree_baked_root
         COMMAND dss_test_support_test_repo_root --gtest_filter=BakedRootOnly.*
         WORKING_DIRECTORY "${_dss_norepo_cwd}")
set_tests_properties(regression/out_of_tree_baked_root PROPERTIES
    ENVIRONMENT_MODIFICATION "DSS_CONFIG_ROOT=unset:")
