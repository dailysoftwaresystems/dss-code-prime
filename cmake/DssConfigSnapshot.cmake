# ── THE RUN'S SHIPPED-CONFIG SNAPSHOT — ONE DECISION FOR THE WHOLE SUITE ─────
#   D-TEST-SHIPPED-CONFIG-EXPOSURE-UNFIXED-OUTSIDE-THE-SUITE-THAT-FLAKED
#
# ★★★ WHAT THIS EXISTS TO STOP. Every ctest entry that resolves shipped config
# used to read the LIVE WORKING TREE: `dss_add_test` pointed `$DSS_CONFIG_ROOT`
# at `${CMAKE_SOURCE_DIR}`, the corpus entries had no override at all and reached
# the same tree through the cwd walk, and every `loadShipped()` in every process
# then re-opened `<repo>/src/dss-config/...` from disk. ✔MEASURED (P31/P32, the
# closed twin `D-TEST-SHIPPED-CONFIG-READ-FROM-A-TREE-ANOTHER-PROCESS-IS-WRITING`):
# none of the three `loadShipped` entry points caches, so a call-site count IS a
# read count — `TargetSchema::loadShipped` 788 sites under `tests/`,
# `ObjectFormatSchema::loadShipped` 294, `GrammarSchema::loadShipped` 152 — and a
# writer-denial census put ONE suite's handle open on `sources/c.lang.json` for
# 67.9% of its wall time. Several workstreams share one checkout, and this
# project's own mutant harnesses rewrite a shipped document IN PLACE while a gate
# runs in the next window. A reader that opens inside that window gets a SHORT
# file and reds on whatever assertion happened to be executing — a failure that
# says nothing about the code under test and costs a lane to re-derive.
#
# ★ SO THE CONTENDED RESOURCE IS SAMPLED ONCE PER RUN, at ONE chokepoint, rather
# than once per `loadShipped()` call in ~840 entries. The `config/snapshot` entry
# below is the fixture SETUP; every entry that reads config declares
# `FIXTURES_REQUIRED` on it and gets `$DSS_CONFIG_ROOT` pointed at the copy.
# The predecessor this SUPERSEDES was `tests/test_support/private_config_root.hpp`
# — the same idea taken OPT-IN PER SUITE, which exactly one of the 150 exposed
# suites ever opted into, and which paid a whole 2.6 MB tree copy per PROCESS.
# Two mechanisms both claiming to own config resolution is drift; that header is
# gone and this is the one owner.
#
# ⚠⚠ THE CONSTRAINT THAT SHAPES EVERYTHING: THE COPY IS TAKEN AT ctest RUN TIME
# AND NEVER AT BUILD TIME. This project proves a config-level test is not vacuous
# by mutating a shipped `.json`, re-running `ctest` WITHOUT rebuilding, and
# observing RED. A build-time copy would serve that re-run stale config and turn
# every such mutant GREEN — hundreds of pins that still look like pins and assert
# nothing. See the head of `DssConfigSnapshotTake.cmake`, which is a `cmake -P`
# script precisely so that nothing about it is baked into a binary, and
# `tests/test_support/test_config_snapshot.cpp`, which reds if the snapshot ever
# stops matching the live tree file for file.
#
# ⚠ A SNAPSHOT THAT CANNOT BE MADE IS A LOUD REFUSAL, never a fall-back. The take
# script's every failure arm is a `FATAL_ERROR`, so `config/snapshot` fails, so
# ctest refuses to run every entry that requires the fixture. It must never leave
# `$DSS_CONFIG_ROOT` naming a directory that is not there: `findShippedConfig`'s
# set-but-miss arm falls THROUGH to the cwd walk and back onto the live tree
# (deliberately — `tests/core/test_config_path_walk.cpp` pins that), and every
# claim this fixture makes would then be false while every test still passed.

set(DSS_CONFIG_SNAPSHOT_ROOT "${CMAKE_BINARY_DIR}/dss-config-snapshot")
set(DSS_CONFIG_SNAPSHOT_FIXTURE "DssConfigSnapshot")
set(DSS_CONFIG_SNAPSHOT_TAKE_SCRIPT "${CMAKE_CURRENT_LIST_DIR}/DssConfigSnapshotTake.cmake")

# Register the once-per-run setup entry. Call ONCE, from the root
# `CMakeLists.txt`, BEFORE the directories whose entries consult the snapshot —
# `set_property(TEST ...)` in `dss_use_config_snapshot` can only name an entry
# that already exists, and the fixture entry must exist before anything requires
# it by name.
function(dss_config_snapshot_init)
    get_property(_dss_already GLOBAL PROPERTY DSS_CONFIG_SNAPSHOT_REGISTERED)
    if(_dss_already)
        message(FATAL_ERROR
            "dss_config_snapshot_init() was called twice. There is exactly one "
            "snapshot per ctest run and exactly one entry that takes it; a second "
            "registration would give the run two writers of one directory.")
    endif()

    add_test(NAME config/snapshot
             COMMAND ${CMAKE_COMMAND}
                     "-DDSS_SNAPSHOT_SOURCE=${CMAKE_SOURCE_DIR}"
                     "-DDSS_SNAPSHOT_DEST=${DSS_CONFIG_SNAPSHOT_ROOT}"
                     -P "${DSS_CONFIG_SNAPSHOT_TAKE_SCRIPT}")

    # ★ `FIXTURES_SETUP`, which is what makes this survive `-R`. ctest adds a
    # fixture's setup entries to the execution set automatically whenever a
    # SELECTED entry requires that fixture, so a scoped `ctest -R '^core/'` takes
    # the snapshot too and an operator never has to know this entry exists.
    # It is also what makes the refusal loud: when a setup entry fails, ctest
    # does not run the entries requiring the fixture and reports them as failures
    # rather than quietly passing them against whatever tree they find.
    set_tests_properties(config/snapshot PROPERTIES
        FIXTURES_SETUP "${DSS_CONFIG_SNAPSHOT_FIXTURE}")

    set_property(GLOBAL PROPERTY DSS_CONFIG_SNAPSHOT_REGISTERED ON)
endfunction()

# Point ONE ctest entry at the run's snapshot.
#
# ⚠ CALL THIS BEFORE `dss_strict_arm_verdicts` on any entry that gets both:
# that helper READS the entry's existing `ENVIRONMENT` and re-sets it, so a call
# in the other order drops whatever this added.
#
# ⚠ It APPENDS to `ENVIRONMENT`, so an entry must not ALSO set `DSS_CONFIG_ROOT`
# by hand — two entries for one variable make the effective value a question
# about ctest's internal ordering rather than about this file.
function(dss_use_config_snapshot test_name)
    get_property(_dss_registered GLOBAL PROPERTY DSS_CONFIG_SNAPSHOT_REGISTERED)
    if(NOT _dss_registered)
        message(FATAL_ERROR
            "dss_use_config_snapshot(${test_name}): dss_config_snapshot_init() has "
            "not run yet, so the `config/snapshot` fixture entry does not exist and "
            "this entry would require a fixture nothing provides. Call "
            "dss_config_snapshot_init() from the root CMakeLists.txt before the "
            "add_subdirectory() calls that register test entries.")
    endif()

    set_property(TEST ${test_name} APPEND PROPERTY
                 ENVIRONMENT "DSS_CONFIG_ROOT=${DSS_CONFIG_SNAPSHOT_ROOT}")
    set_property(TEST ${test_name} APPEND PROPERTY
                 FIXTURES_REQUIRED "${DSS_CONFIG_SNAPSHOT_FIXTURE}")
endfunction()
