cmake_minimum_required(VERSION 3.28)

# ── Integration Tests ──────────────────────────────────────────
#
# Invokes the compiled `dss-code-prime` executable as a subprocess
# and validates the user-facing CLI surface end-to-end. Cross-
# platform (Windows / Linux / macOS) — the runner's per-host
# spec selection lives at runtime, not in cmake. Always against
# the CURRENT host platform (user invariant 2026-06-02).
#
# ★★★ AND THE COMPLEMENT OF THAT INVARIANT, WHICH USED TO BE UNSAID HERE:
# because this harness compiles ONLY the host-runnable spec, a manifest's other
# targets are compiled by the IN-PROCESS runner ALONE. That division of labour is
# checked by the `integrated_tests/coverage-boundary` entry at the bottom of this
# file — see
# D-TEST-INTEGRATED-RUNNER-BUILDS-ONLY-THE-HOST-RUNNABLE-SPEC-SO-ONE-RUNNER-SEES-A-CAPABILITY
#
# The runner ALSO drives the curated `examples/` corpus through
# the CLI path (parallel to the in-process exercise via
# `tests/examples/examples_runner`). The two paths share the
# same source files + expected.json manifests; this one stresses
# argv parsing + filesystem I/O + the `--output` routing logic
# the in-process path can't reach.

enable_testing()

add_executable(integrated_tests runner.cpp)
target_link_libraries(integrated_tests
    PRIVATE
        nlohmann_json::nlohmann_json
)
target_include_directories(integrated_tests
    PRIVATE
        ${CMAKE_SOURCE_DIR}/tests/test_support
)
# Same repo root the `tests/` targets bake in (tests/CMakeLists.txt,
# dss_add_test), so `tests/test_support/repo_root.hpp` resolves here too.
target_compile_definitions(integrated_tests PRIVATE
    DSS_TEST_REPO_ROOT="${CMAKE_SOURCE_DIR}")
set_target_properties(integrated_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin/dss
)

# ── ctest entries — D-TEST-INTEGRATED-RUNNER-WALKS-EVERY-EXAMPLE-IN-ONE-THREAD ─
#
# ★★★ ONE ENTRY PER EXAMPLE, PLUS THREE THAT ARE NOT ABOUT ANY ONE EXAMPLE.
# Operator instruction 2026-08-21: *"paralelize integrated_tests + report each
# integrated test item as pass or fail as a sub item of integrated tests unit"*.
# ctest parallelises its own entries and prints one verdict per entry, so the two
# required properties are the same change. The discovery below is the SAME shape
# `tests/examples/CMakeLists.txt` has used since AP6 — a `GLOB_RECURSE` over the
# manifests and a `foreach` — reused deliberately rather than re-invented, so the
# two harnesses cannot drift on WHICH examples exist.
#
# ✔THE COST IS MEASURED (2026-08-21, Windows, debug binary, best of two runs over
# N-example corpus roots): N=1 1.37s, N=2 2.41s, N=4 4.69s, N=8 9.33s ⇒ marginal
# 1.15 s per example and a FIXED per-process overhead of 0.10 s. Over 613
# manifests that is ~61 s of added CPU — about +9% — against roughly a 7×
# wall-clock improvement at `-j 8`.
#
# ⚠ THE NAME IS `integrated_tests/<lang>/<name>` ON PURPOSE. ctest has no test
# hierarchy, so the prefix IS the nesting: `ctest -R '^integrated_tests/'` selects
# the whole unit, and a red entry names the example instead of the aggregate.

# ── the corpus-wide floors, and the fixture that makes them honest ────────────
#
# ★★★ EACH PER-EXAMPLE ENTRY EMITS A CELL; ONE ADJUDICATOR READS THEM.
# An EXISTENCE claim ("the optimizer is not inert somewhere in the corpus") has no
# per-example form — ✔9 of 24 sampled optimized-arm examples emit a byte-identical
# image, which is the CORRECT result where the optimizer has nothing to do. So the
# claim keeps its scope and is judged over the observations the entries already
# produced, at NO rebuild cost.
#
# ⚠ THE SETUP FIXTURE IS LOAD-BEARING, NOT TIDINESS: without it a cell left by a
# PREVIOUS run counts toward this run's population, and a corpus-wide existence
# floor would be satisfied by an observation nobody made today.
set(DSS_INTEGRATED_CELLS "${CMAKE_BINARY_DIR}/integrated-cells")

add_test(NAME integrated_tests/cells-reset
         COMMAND ${CMAKE_COMMAND} -E rm -rf "${DSS_INTEGRATED_CELLS}")
set_tests_properties(integrated_tests/cells-reset PROPERTIES
    FIXTURES_SETUP DssIntegratedCells)

# ⚠ NO CLEANUP FIXTURE THAT DELETES THE CELLS. A red adjudicator is triaged by
# READING the cells, and a cleanup step would delete the evidence between the
# failure and the reader — the same defect the runner's own scratch-root policy
# already avoids by KEEPING a red run's root.

foreach(MANIFEST ${DSS_EXAMPLE_MANIFESTS})
    get_filename_component(EXAMPLE_DIR ${MANIFEST} DIRECTORY)
    get_filename_component(EXAMPLE_NAME ${EXAMPLE_DIR} NAME)
    get_filename_component(LANG_DIR ${EXAMPLE_DIR} DIRECTORY)
    get_filename_component(LANG_NAME ${LANG_DIR} NAME)
    set(TEST_NAME "integrated_tests/${LANG_NAME}/${EXAMPLE_NAME}")
    add_test(
        NAME ${TEST_NAME}
        COMMAND integrated_tests
                $<TARGET_FILE:dss-code-prime>
                ${CMAKE_SOURCE_DIR}/examples
                "--only=${LANG_NAME}/${EXAMPLE_NAME}"
                "--cells=${DSS_INTEGRATED_CELLS}"
    )
    set_tests_properties(${TEST_NAME} PROPERTIES
        ENVIRONMENT "DSS_CONFIG_ROOT=${CMAKE_SOURCE_DIR}"
        FIXTURES_REQUIRED DssIntegratedCells)
    dss_strict_arm_verdicts(${TEST_NAME})
    list(APPEND DSS_INTEGRATED_EXAMPLE_ENTRIES ${TEST_NAME})
endforeach()

# The host-independent CLI surface and the self-test pins: one entry, no walk.
add_test(NAME integrated_tests/cli-surface
         COMMAND integrated_tests
                 $<TARGET_FILE:dss-code-prime>
                 ${CMAKE_SOURCE_DIR}/examples
                 --only=cli)
set_tests_properties(integrated_tests/cli-surface PROPERTIES
    ENVIRONMENT "DSS_CONFIG_ROOT=${CMAKE_SOURCE_DIR}")
dss_strict_arm_verdicts(integrated_tests/cli-surface)

# The adjudicator — ONE entry for every corpus-wide claim. It PARSES every
# manifest (feeding the emulator lint, a genuine parse fact), executes none, and
# judges the existence floors over the cells. ★ A separate `corpus-lints` entry
# survived the first draft of the split owning exactly ONE check; folded in on the
# operator's instruction 2026-08-21, because two entries for one scope makes a
# reader guess which to open when "a corpus-wide thing" reds.
# `SKIP_RETURN_CODE` is what makes a SUBSET run report a
# classified skip instead of a pass — an existence floor over part of the corpus
# is not evidence, and the one thing it must never do is read as green.
add_test(NAME integrated_tests/adjudicate
         COMMAND integrated_tests
                 $<TARGET_FILE:dss-code-prime>
                 ${CMAKE_SOURCE_DIR}/examples
                 --only=adjudicate
                 "--cells=${DSS_INTEGRATED_CELLS}")
# ★★★ `DEPENDS` ON EVERY PER-EXAMPLE ENTRY, AND IT IS NOT OPTIONAL ORDERING.
# ✔MEASURED 2026-08-21, and it is the defect this list exists to fix: with only
# `FIXTURES_REQUIRED`, ctest scheduled the adjudicator at position 611 of the 617
# entries that existed when the defect was measured (616 after `corpus-lints`
# was folded into `adjudicate`) —
# fixtures order a test after the SETUP, not after the tests that happen to share
# the fixture. It therefore read a PARTIAL population, returned its classified
# skip, and the whole unit reported `100% tests passed` with the corpus-wide
# floors asserting NOTHING. ★ A skip that always fires is exactly as vacuous as a
# pass that always fires, and it is harder to notice because it looks deliberate.
#
# ⚠ `DEPENDS` AND NOT A FIXTURE, deliberately: ctest SKIPS a test whose required
# fixture FAILED, so making the 613 entries a fixture would hide the adjudicator
# the moment any single example reddened — and the cell is written even by a
# failing entry precisely so the corpus-wide floors can still be judged. `DEPENDS`
# orders without coupling verdicts.
set_tests_properties(integrated_tests/adjudicate PROPERTIES
    ENVIRONMENT "DSS_CONFIG_ROOT=${CMAKE_SOURCE_DIR}"
    FIXTURES_REQUIRED DssIntegratedCells
    SKIP_RETURN_CODE 77
    DEPENDS "${DSS_INTEGRATED_EXAMPLE_ENTRIES};integrated_tests/cells-reset")

# ── The SPAWNED CLI needs this in its ENVIRONMENT, not just baked ──────────
# This runner's whole job is to invoke `dss-code-prime` as a SUBPROCESS, and
# that subprocess resolves shipped config through its own
# `core/types/config_path_walk.cpp`, which reads $DSS_CONFIG_ROOT and otherwise
# walks up from ITS cwd. An out-of-tree build gives it a cwd with no
# src/dss-config/ in any ancestor, and until now nothing in this directory set
# the variable at all — `grep DSS_CONFIG_ROOT integrated_tests/` returned
# nothing. MEASURED consequence at a3af1320, and it is the exact silent-harness
# -bug shape the note below warns about: the IN-PROCESS corpus runner
# (tests/examples/examples_runner) PASSED while this CLI-subprocess sibling
# failed all 561 manifests with error[C_InvalidLanguageName] /
# error[D_SchemaLoadFailed]. A compile definition cannot fix this one — the
# value has to be in the environment for the child process to inherit it.
# (CTest ENVIRONMENT is a ';'-separated list; a repo path containing a literal
# ';' would mis-split — impossible on normal Windows/WSL paths.)
# (each entry above carries this property; the single `integrated_tests` entry it
# used to be set on no longer exists)

# D-TEST-CROSS-ARCH-SKIP-YIELDS-NO-VERDICT: the same strict-arm-verdict wiring
# the in-process corpus entries get. The capability MUST land on both harnesses
# together — one runner enforcing an arm's verdict while its sibling shrugs is a
# silent harness bug, which is the class this whole change closes.
#
# ⚠ APPLIED TO EXACTLY THE ENTRIES THAT CAN EXECUTE AN ARM: every per-example
# entry in the `foreach`, `cli-surface`, and `coverage-boundary` below (which
# executes one corpus example through this harness). The two OMISSIONS are
# deliberate and named here so a later reader does not "repair" them — a
# `cmake -E rm -rf` (`cells-reset`) and an entry that executes no example
# (`adjudicate`) have no arm whose verdict could be strict. The strictness the
# adjudicator depends on is already carried by the entries that WROTE its cells.
#
# ⚠ ORDER IS LOAD-BEARING WHERE IT IS CALLED: `dss_strict_arm_verdicts` READS the
# entry's existing `ENVIRONMENT` and appends to it, so it must run AFTER the
# `set_tests_properties(... ENVIRONMENT ...)` that carries `DSS_CONFIG_ROOT`.
# Reversing the two would drop the config root and take every config-level
# red-on-disable pin with it.

# ── the coverage boundary between the TWO corpus harnesses ───────────────────
#
# D-TEST-INTEGRATED-RUNNER-BUILDS-ONLY-THE-HOST-RUNNABLE-SPEC-SO-ONE-RUNNER-SEES-A-CAPABILITY
#
# ★★★ THE ONE ENTRY THAT RUNS BOTH HARNESSES AND COMPARES THEM. Every other
# entry in this file measures ONE runner; this one selects a corpus example that
# has both halves of the boundary (a target this host binds, and a target whose
# `runOn` excludes it), drives it through THIS harness, spawns
# `dss_examples_runner` on the SAME example directory, and judges the two
# coverage reports against the five clauses in
# `tests/test_support/coverage_boundary.hpp`.
#
# ⚠ WHY IT EXISTS AT ALL: the project's rule is that a capability change must
# reach BOTH runners, and that rule has a real exception — this harness compiles
# only the target the host can execute, by the user invariant of 2026-06-02
# named at the top of this file. The exception was written down; its COMPLEMENT
# ("and the in-process runner compiles the rest") was not, and nothing checked
# that anybody covered the other half. A cross-target capability could therefore
# lose its only witness with both corpus suites still reporting green.
#
# ⚠ `$<TARGET_FILE:dss_examples_runner>` IS A FORWARD REFERENCE and that is
# fine: `tests/` is added AFTER this directory in the root CMakeLists, and a
# generator expression is resolved at GENERATE time, when both targets exist.
# ⚠ NO `--cells=`, deliberately: this entry executes a corpus example, so it
# WOULD write a cell — and that cell would join the adjudicator's population
# from an entry the population count knows nothing about. `writeCell` no-ops
# when the directory is unset, which is what keeps the two instruments apart.
add_test(NAME integrated_tests/coverage-boundary
         COMMAND integrated_tests
                 $<TARGET_FILE:dss-code-prime>
                 ${CMAKE_SOURCE_DIR}/examples
                 --only=coverage-boundary
                 "--sibling-runner=$<TARGET_FILE:dss_examples_runner>")
set_tests_properties(integrated_tests/coverage-boundary PROPERTIES
    ENVIRONMENT "DSS_CONFIG_ROOT=${CMAKE_SOURCE_DIR}")
# ORDER IS LOAD-BEARING, same as every call above: `dss_strict_arm_verdicts`
# READS the entry's existing ENVIRONMENT and appends to it.
dss_strict_arm_verdicts(integrated_tests/coverage-boundary)
