# ═══ THE COMPILER'S BUILD STAMP ══════════════════════════════════════════════
#
# Run at BUILD time (`cmake -P`) by the `dss_build_stamp_generate` target in the
# top-level CMakeLists, which also documents the wiring and DECLARES the inputs
# this file reads (below). This file computes ONE string — `DSS_BUILD_STAMP` —
# and writes it into a generated header, but only when its VALUE has changed.
# Run with `-DDSS_STAMP_MODE=selftest` it proves its own properties instead
# (the ctest entry `build/build_stamp_identity`; see THE SELF-TEST at the end).
#
# ── WHY A STAMP AT ALL, AND WHY NOT THE THREE OBVIOUS ALTERNATIVES ───────────
# The runtime object cache keys a compiled artifact on its inputs, and THE
# COMPILER IS AN INPUT: the same source, target and config compiled by a
# different codegen must not select the same cache entry. So the key needs an
# identity for the compiler itself. Each of the obvious identities is refused
# for a MEASURED reason, not a stylistic one:
#
#   * the compiler image's CONTENT HASH — infeasible. ✔MEASURED 2026-08-17: the
#     debug shared library is 419,277,885 bytes, and `integrated_tests` spawns
#     the CLI ~450 times, so this would be hashed 450 times per gate run.
#   * (path, size, mtime) — install-UNSTABLE. Every one of those three terms
#     changes when the compiler is copied to a user's machine, so a SHIPPED
#     cache (`dist/release/`) would miss on every install and be worthless,
#     which is the whole reason the cache exists.
#   * `DSS_PROJECT_VERSION` alone — insufficient. It does not move when codegen
#     does; a whole release's worth of backend changes share one version string,
#     and every one of them would collide on the same key.
#
# The stamp is what is left: cheap to read (it is a string literal in the
# binary), stable across an INSTALL (nothing about the file's location is in
# it), and it MOVES when the source the compiler was built from moves.
#
# ── THE ASYMMETRY THAT DECIDES EVERY JUDGEMENT CALL BELOW ────────────────────
# Over-invalidation costs one recompile of one small runtime unit — and, in the
# BUILD, a new header, so the compiler library and every executable linking it
# relink. Under-invalidation links an artifact compiled by a DIFFERENT compiler
# and the failure is silent. So every uncertain case degrades toward "differ".
#
# ── WHAT THE COMPILER IS BUILT FROM: THE DECLARED INPUTS ─────────────────────
# The stamp must move when the compiler can, and only then: the top-level
# CMakeLists DECLARES what the compiler is built from, and both ways of naming
# the tree below read exactly that, no more.
#   DSS_STAMP_INPUTS       paths read WHOLE: every file under a directory, or
#                          the file itself (`src`, `cmake`, the top-level
#                          `CMakeLists.txt`, `VERSION` — the compiler's sources
#                          and its config documents, and the scripts that build
#                          it)
#   DSS_STAMP_RECORDS      FILES under a path read whole that build NOTHING —
#                          the harness records kept beside the build's modules
#                          (`cmake/`'s deletion inventory and test-budget
#                          checker) — left out, each by its own path. One that
#                          names no file there, by that exact spelling, is
#                          refused. An UNDECLARED file beside them is read: a new
#                          file there degrades toward "differ" (the asymmetry
#                          above), never toward a stamp that misses a build input
#                          — which is why `cmake` is not a script directory
#   DSS_STAMP_SCRIPT_DIRS  directories of which only the CMake scripts count
#                          (`CMakeLists.txt`, `*.cmake`): the test trees, which
#                          one configure reads beside the compiler's and from
#                          which a script COULD reach a product target, while
#                          their sources build only tests
# All three are comma-separated paths relative to DSS_STAMP_SOURCE_DIR (the
# records may be empty). Nothing else of the TREE names the compiler: a test's
# source, a plan, a skill, a harness program or a document is never compiled
# into it, and the runtime cache's key carries the config documents it loads as
# a term of its own.
#
# ── AND WHAT IT IS BUILT AGAINST: A DEPENDENCY, FETCHED OR HANDED ────────────
# A dependency the build FETCHES needs no term: the top-level CMakeLists pins
# its version, and that file is an input. A dependency the configure is HANDED
# (CMake's `FETCHCONTENT_SOURCE_DIR_<NAME>`: use these sources, fetch nothing)
# is held by no pin — its bytes are whatever stands in the directory — so the
# stamp names it by them:
#   DSS_STAMP_HANDED_DEPENDENCIES   comma-separated `<NAME>=<absolute directory>`,
#                          as `dss_build_stamp_handed_dependencies` reads CMake's
#                          own record of the hand-overs; EMPTY where the configure
#                          was handed none, and the value is then exactly what it
#                          is without this term
# Every file under each directory enters one sorted list as its dependency's
# name, its path below that directory and the SHA-256 of its bytes; the list's
# digest is the `.deps` component. The directory's own location is NOT in it —
# the same bytes handed from another path stamp the same value — and a clone's
# own `.git` there is not read: git's bookkeeping moves when no source does.
# Read on every build, as the inputs are: a handed directory can change between
# two builds with no configure between them.
# ✔MEASURED 2026-10-10 why the term exists: DssHarness builds each mutation
# worker with the dependency sources the leg's own build fetched, HANDED to the
# worker's configure from inside the worker. Until then the stamp named no
# dependency at all on the strength of the pin, which a hand-over steps around:
# a compiler built against other bytes of a dependency carried the pinned
# build's stamp, and the runtime object cache could not tell the two apart.
# ✔MEASURED 2026-10-06 (cycle P69, lane `hm`) why the inputs are declared, in
# two directions. On a work tree the dirt was the WHOLE tree's, so an edit to
# any file — a registry row, a skill, a harness program — moved the stamp and
# relinked 550 targets (82-88 s of a DssHarness run's overhead, against 10.6 s
# for a rebuild after no edit). And without git the value was
# `<VERSION>+nogit<UTC timestamp>.<serial>`, which moved on EVERY build by
# design — while DssHarness builds every leg on another machine from a copy
# with no `.git`, so each of those builds relinked the same 550 targets with
# nothing changed (the WSL copy's serial 4, 5, 6 over three builds, the last
# after a sync that wrote no file) and its runtime cache opened a new root
# every time. The same day's re-review found the records read too: `cmake` read
# whole put the deletion inventory and the test-budget checker into the
# compiler's identity, so an edit of either moved the stamp.
#
# ── INPUTS (passed with -D; see the target that invokes this) ────────────────
#   DSS_STAMP_VERSION     the repo-root VERSION file's contents (the top-level
#                         CMakeLists already reads it into DSS_VERSION)
#   DSS_STAMP_SOURCE_DIR  the repo root — the working directory git is asked about
#   DSS_STAMP_OUTPUT      the generated header to write
#   DSS_STAMP_INPUTS, DSS_STAMP_RECORDS, DSS_STAMP_SCRIPT_DIRS   the
#                         declaration (above)
#   DSS_STAMP_HANDED_DEPENDENCIES   the dependencies the configure was handed
#                         (above); must be passed, EMPTY where there is none
#   DSS_STAMP_GIT         path to git, or EMPTY when configure found none
#   DSS_STAMP_MODE        `stamp` (the default) or `selftest`
#   DSS_STAMP_SCRATCH     the self-test's scratch directory (selftest only)
#   DSS_STAMP_GENERATOR, DSS_STAMP_MAKE_PROGRAM   the build's generator and its
#                         make program, which the self-test configures its
#                         declaration fixtures with (selftest only)
#
# ── THE VALUE ────────────────────────────────────────────────────────────────
#   <VERSION>+g<12 hex>                     a work tree whose inputs match HEAD
#   <VERSION>+g<12 hex>.dirty<16 hex>       …plus a digest of the inputs' DIRT
#   <VERSION>+src<16 hex>                   no git, or no work tree: a digest of
#                                           the inputs' CONTENT
#   <any of the three>.deps<16 hex>         …plus a digest of the CONTENT of the
#                                           dependencies the configure was handed
#
# No component ever contains whitespace, so the whole stamp is a single token —
# the property `tests/program/test_build_stamp.cpp` pins, because this string
# ends up inside a cache FILENAME.

cmake_minimum_required(VERSION 4.0)

if(NOT DEFINED DSS_STAMP_MODE OR DSS_STAMP_MODE STREQUAL "")
    set(DSS_STAMP_MODE "stamp")
endif()
if(NOT DSS_STAMP_MODE STREQUAL "stamp" AND NOT DSS_STAMP_MODE STREQUAL "selftest")
    message(FATAL_ERROR "DssBuildStamp: DSS_STAMP_MODE is '${DSS_STAMP_MODE}', which is neither `stamp` nor "
                        "`selftest`.")
endif()

# The declaration, as lists of tree-relative paths. Refused, never defaulted: a
# default here would be a second statement of what the compiler is built from.
# The records may be EMPTY (no record), but must be passed.
function(_dss_stamp_read_inputs out_inputs out_records out_script_dirs)
    foreach(_dss_required IN ITEMS DSS_STAMP_INPUTS DSS_STAMP_SCRIPT_DIRS)
        if(NOT DEFINED ${_dss_required} OR "${${_dss_required}}" STREQUAL "")
            message(FATAL_ERROR
                "DssBuildStamp: ${_dss_required} was not passed — the top-level CMakeLists DECLARES what the "
                "compiler is built from, and this script reads nothing it was not told.")
        endif()
    endforeach()
    if(NOT DEFINED DSS_STAMP_RECORDS)
        message(FATAL_ERROR
            "DssBuildStamp: DSS_STAMP_RECORDS was not passed — the top-level CMakeLists DECLARES the records its "
            "inputs hold (empty when they hold none), and this script reads nothing it was not told.")
    endif()
    string(REPLACE "," ";" _dss_in "${DSS_STAMP_INPUTS}")
    string(REPLACE "," ";" _dss_rc "${DSS_STAMP_RECORDS}")
    string(REPLACE "," ";" _dss_sd "${DSS_STAMP_SCRIPT_DIRS}")
    set(${out_inputs} "${_dss_in}" PARENT_SCOPE)
    set(${out_records} "${_dss_rc}" PARENT_SCOPE)
    set(${out_script_dirs} "${_dss_sd}" PARENT_SCOPE)
endfunction()

if(DSS_STAMP_MODE STREQUAL "stamp")

foreach(_dss_required IN ITEMS DSS_STAMP_VERSION DSS_STAMP_SOURCE_DIR DSS_STAMP_OUTPUT)
    if(NOT DEFINED ${_dss_required} OR "${${_dss_required}}" STREQUAL "")
        message(FATAL_ERROR
            "DssBuildStamp: ${_dss_required} was not passed. This script is "
            "invoked by the `dss_build_stamp_generate` target in the top-level "
            "CMakeLists; run it from there, never by hand.")
    endif()
endforeach()
# DSS_STAMP_GIT is deliberately NOT in that loop: "configure found no git" is a
# legitimate state with a defined behaviour below, not a caller error.
# DSS_STAMP_HANDED_DEPENDENCIES must be PASSED and may be empty: "handed none"
# is a statement, and a run that was told nothing would name no dependency.
if(NOT DEFINED DSS_STAMP_HANDED_DEPENDENCIES)
    message(FATAL_ERROR
        "DssBuildStamp: DSS_STAMP_HANDED_DEPENDENCIES was not passed — the top-level CMakeLists says which "
        "dependencies its configure was handed (empty when it was handed none), and a stamp computed without "
        "being told would name none of them.")
endif()
_dss_stamp_read_inputs(_dss_inputs _dss_records _dss_script_dirs)
# Every declared input must EXIST: a missing one is a declaration that names
# nothing, and a stamp computed without it would stop moving with it.
foreach(_dss_in IN LISTS _dss_inputs _dss_script_dirs)
    if(NOT EXISTS "${DSS_STAMP_SOURCE_DIR}/${_dss_in}")
        message(FATAL_ERROR
            "DssBuildStamp: the declared input '${_dss_in}' does not exist under '${DSS_STAMP_SOURCE_DIR}'. "
            "Correct DSS_BUILD_STAMP_INPUTS / DSS_BUILD_STAMP_SCRIPT_DIRS in the top-level CMakeLists.")
    endif()
endforeach()
# Every declared RECORD must be a FILE that a directory read whole holds, spelled
# as the directory spells it (a directory's own listing, so a case the file
# system forgives is still refused): a record that names nothing would exclude
# nothing while it claimed to, and one outside the inputs is no exclusion at all.
foreach(_dss_r IN LISTS _dss_records)
    set(_dss_under FALSE)
    foreach(_dss_in IN LISTS _dss_inputs)
        string(LENGTH "${_dss_in}/" _dss_len)
        string(SUBSTRING "${_dss_r}" 0 ${_dss_len} _dss_head)
        if(_dss_head STREQUAL "${_dss_in}/" AND IS_DIRECTORY "${DSS_STAMP_SOURCE_DIR}/${_dss_in}")
            set(_dss_under TRUE)
        endif()
    endforeach()
    get_filename_component(_dss_rdir "${_dss_r}" DIRECTORY)
    file(GLOB _dss_listed LIST_DIRECTORIES false RELATIVE "${DSS_STAMP_SOURCE_DIR}"
         "${DSS_STAMP_SOURCE_DIR}/${_dss_rdir}/*")
    list(FIND _dss_listed "${_dss_r}" _dss_at)
    if(NOT _dss_under OR _dss_at LESS 0)
        message(FATAL_ERROR
            "DssBuildStamp: the declared record '${_dss_r}' is not a file, by that spelling, under a directory "
            "DSS_BUILD_STAMP_INPUTS reads whole (${DSS_STAMP_INPUTS}). Correct DSS_BUILD_STAMP_RECORDS in the "
            "top-level CMakeLists.")
    endif()
endforeach()

set(_dss_stamp "${DSS_STAMP_VERSION}")
set(_dss_have_git FALSE)

# ★ THE EXECUTABLE EXISTING IS NOT THE SAME QUESTION AS THIS BEING A WORK TREE.
# A source tarball unpacked on a machine with git installed has git and no
# `.git`, and an EXISTS check alone would then report success while `rev-parse`
# quietly failed and the stamp silently lost its commit component. So the
# verdict comes from the COMMAND'S RESULT, and the executable check is only
# there to avoid spawning something that is not on disk.
if(DSS_STAMP_GIT AND EXISTS "${DSS_STAMP_GIT}")
    execute_process(
        COMMAND "${DSS_STAMP_GIT}" rev-parse --short=12 HEAD
        WORKING_DIRECTORY "${DSS_STAMP_SOURCE_DIR}"
        RESULT_VARIABLE  _dss_rc
        OUTPUT_VARIABLE  _dss_commit
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    if(_dss_rc EQUAL 0 AND NOT "${_dss_commit}" STREQUAL "")
        set(_dss_have_git TRUE)
        string(APPEND _dss_stamp "+g${_dss_commit}")
    endif()
endif()

if(_dss_have_git)
    # ── THE DIRTY COMPONENT ──────────────────────────────────────────────────
    # A commit id identifies the tree only when the tree MATCHES it. Every
    # developer build and every mid-cycle gate runs against a modified tree, so
    # without this component two DIFFERENT working trees at the same HEAD would
    # share a stamp — and the second one would serve the first one's artifacts.
    # That is the failure this whole file exists to make impossible. The dirt is
    # the DECLARED INPUTS' dirt (git pathspecs below): a tree dirty only outside
    # them builds the same compiler, and says so.
    #
    # Captured to FILES rather than to CMake variables, for two measured
    # reasons: `git diff HEAD` is 1,440,179 bytes on this tree today (so a
    # `string(SHA256 ...)` would carry the whole diff through a CMake string),
    # and a diff of a binary fixture contains NUL bytes, which do not survive an
    # `execute_process` OUTPUT_VARIABLE round trip. `file(SHA256 …)` reads the
    # bytes as bytes.
    #
    # ⓘ WHAT THE PAIR COVERS, STATED HONESTLY. `status --porcelain` reports
    # untracked paths by NAME; `diff HEAD` reports tracked changes by CONTENT.
    # So editing an untracked file WITHOUT renaming it does not move the stamp.
    # The exposure is small by construction — a new source file is only compiled
    # once a (tracked) CMakeLists names it, which moves the diff — but it is a
    # real gap and belongs in the record rather than in a claim of totality.
    set(_dss_pathspecs "")
    foreach(_dss_in IN LISTS _dss_inputs)
        list(APPEND _dss_pathspecs "${_dss_in}")
    endforeach()
    foreach(_dss_sd IN LISTS _dss_script_dirs)
        list(APPEND _dss_pathspecs ":(glob)${_dss_sd}/**/CMakeLists.txt" ":(glob)${_dss_sd}/**/*.cmake")
    endforeach()
    foreach(_dss_r IN LISTS _dss_records)
        list(APPEND _dss_pathspecs ":(exclude,literal)${_dss_r}")   # a record: under an input, building nothing
    endforeach()
    set(_dss_status_probe "${DSS_STAMP_OUTPUT}.status-probe")
    set(_dss_diff_probe   "${DSS_STAMP_OUTPUT}.diff-probe")
    get_filename_component(_dss_out_dir "${DSS_STAMP_OUTPUT}" DIRECTORY)
    file(MAKE_DIRECTORY "${_dss_out_dir}")

    execute_process(
        COMMAND "${DSS_STAMP_GIT}" status --porcelain -- ${_dss_pathspecs}
        WORKING_DIRECTORY "${DSS_STAMP_SOURCE_DIR}"
        OUTPUT_FILE "${_dss_status_probe}"
        ERROR_QUIET)
    execute_process(
        COMMAND "${DSS_STAMP_GIT}" diff HEAD -- ${_dss_pathspecs}
        WORKING_DIRECTORY "${DSS_STAMP_SOURCE_DIR}"
        OUTPUT_FILE "${_dss_diff_probe}"
        ERROR_QUIET)

    file(SIZE "${_dss_status_probe}" _dss_status_size)
    file(SIZE "${_dss_diff_probe}"   _dss_diff_size)
    if(_dss_status_size GREATER 0 OR _dss_diff_size GREATER 0)
        # Two digests, then a digest OF the pair — never a concatenation of the
        # two byte streams, which would let a boundary shift between them
        # produce the same input for two different trees.
        file(SHA256 "${_dss_status_probe}" _dss_status_digest)
        file(SHA256 "${_dss_diff_probe}"   _dss_diff_digest)
        string(SHA256 _dss_dirty "${_dss_status_digest}:${_dss_diff_digest}")
        string(SUBSTRING "${_dss_dirty}" 0 16 _dss_dirty)
        string(APPEND _dss_stamp ".dirty${_dss_dirty}")
    endif()

    file(REMOVE "${_dss_status_probe}" "${_dss_diff_probe}")
else()
    # ── NO GIT / NO WORK TREE: THE INPUTS' CONTENT ───────────────────────────
    # With no commit to name, the files themselves are the identity: every file
    # of the declared inputs, by its path and the SHA-256 of its bytes, in one
    # sorted list, digested. It holds while nothing changes and moves when any
    # input file's bytes, name or presence does — stronger than the work-tree
    # pair, which sees an untracked file only by name. The path is part of each
    # entry, so a rename moves it, and the list is sorted, so the order a
    # directory happens to be read in never does.
    # ⓘ The cost, ✔MEASURED 2026-10-06 when this replaced the always-differing
    # value: the declared inputs are 669 files, 28.1 MB (`src/` holds 635 of
    # them), and the self-test's real-tree arm stamps them without git in 169 ms
    # on the Windows leg, the `cmake -P` start included — it prints the figure on
    # every leg it runs on. The old value relinked 550 targets on every build of
    # a copy.
    set(_dss_files "")
    foreach(_dss_in IN LISTS _dss_inputs)
        if(IS_DIRECTORY "${DSS_STAMP_SOURCE_DIR}/${_dss_in}")
            file(GLOB_RECURSE _dss_found LIST_DIRECTORIES false RELATIVE "${DSS_STAMP_SOURCE_DIR}"
                 "${DSS_STAMP_SOURCE_DIR}/${_dss_in}/*")
            list(APPEND _dss_files ${_dss_found})
        else()
            list(APPEND _dss_files "${_dss_in}")
        endif()
    endforeach()
    foreach(_dss_sd IN LISTS _dss_script_dirs)
        file(GLOB_RECURSE _dss_found LIST_DIRECTORIES false RELATIVE "${DSS_STAMP_SOURCE_DIR}"
             "${DSS_STAMP_SOURCE_DIR}/${_dss_sd}/CMakeLists.txt" "${DSS_STAMP_SOURCE_DIR}/${_dss_sd}/*.cmake")
        list(APPEND _dss_files ${_dss_found})
    endforeach()
    list(REMOVE_DUPLICATES _dss_files)
    if(_dss_records)
        list(REMOVE_ITEM _dss_files ${_dss_records})   # each one checked above to be a file the inputs list
    endif()
    list(SORT _dss_files)
    set(_dss_manifest "")
    foreach(_dss_f IN LISTS _dss_files)
        file(SHA256 "${DSS_STAMP_SOURCE_DIR}/${_dss_f}" _dss_h)
        string(APPEND _dss_manifest "${_dss_f}\t${_dss_h}\n")
    endforeach()
    string(SHA256 _dss_content "${_dss_manifest}")
    string(SUBSTRING "${_dss_content}" 0 16 _dss_content)
    string(APPEND _dss_stamp "+src${_dss_content}")
endif()

# ── THE HANDED DEPENDENCIES' CONTENT ────────────────────────────────────────
# One sorted list over every dependency the configure was handed: the
# dependency's NAME alone on a line (so one handed with no file still differs
# from none handed, and the same bytes under another name are another
# dependency), then each of its files as a tab, its path below the directory,
# a tab and the SHA-256 of its bytes. Never the directory's own path: where a
# dependency was handed FROM is no part of what the compiler was built against.
string(REPLACE "," ";" _dss_handed "${DSS_STAMP_HANDED_DEPENDENCIES}")
if(_dss_handed)
    list(SORT _dss_handed)
    set(_dss_manifest "")
    foreach(_dss_pair IN LISTS _dss_handed)
        if(NOT _dss_pair MATCHES "^([^=]+)=(.+)$")
            message(FATAL_ERROR
                "DssBuildStamp: the handed dependency '${_dss_pair}' is not `<NAME>=<directory>`. "
                "DSS_STAMP_HANDED_DEPENDENCIES is what `dss_build_stamp_handed_dependencies` answers in the "
                "top-level CMakeLists; pass that, unchanged.")
        endif()
        set(_dss_dep_name "${CMAKE_MATCH_1}")
        set(_dss_dep_dir "${CMAKE_MATCH_2}")
        if(NOT IS_DIRECTORY "${_dss_dep_dir}")
            message(FATAL_ERROR
                "DssBuildStamp: the dependency '${_dss_dep_name}' was handed from '${_dss_dep_dir}', which is no "
                "directory: a hand-over that names nothing, and a stamp computed without it would not move with "
                "the bytes the compiler is built against. Configure again with the directory that holds them.")
        endif()
        file(GLOB_RECURSE _dss_found LIST_DIRECTORIES false RELATIVE "${_dss_dep_dir}" "${_dss_dep_dir}/*")
        list(FILTER _dss_found EXCLUDE REGEX "^\\.git(/|$)")   # a clone's own bookkeeping, never a source
        list(SORT _dss_found)
        string(APPEND _dss_manifest "${_dss_dep_name}\n")
        foreach(_dss_f IN LISTS _dss_found)
            file(SHA256 "${_dss_dep_dir}/${_dss_f}" _dss_h)
            string(APPEND _dss_manifest "\t${_dss_f}\t${_dss_h}\n")
        endforeach()
    endforeach()
    string(SHA256 _dss_deps "${_dss_manifest}")
    string(SUBSTRING "${_dss_deps}" 0 16 _dss_deps)
    string(APPEND _dss_stamp ".deps${_dss_deps}")
endif()

# ── WRITE ONLY ON CHANGE ─────────────────────────────────────────────────────
# `dss_build_stamp_generate` is an always-out-of-date target, so this script
# runs on EVERY build. Writing unconditionally would touch the header every
# time, and every TU that includes it would recompile every time — turning a
# cache-correctness mechanism into a permanent build-time tax. The compare is
# over the FULL generated text, not over the stamp alone, so a change to the
# banner below is picked up too. (A VALUE that moves on every build defeats
# this compare from the other side; the no-git branch had one until 2026-10-06.)
#
# The content is assembled from explicit `\n` escapes rather than from a
# multi-line literal, so the generated bytes are LF regardless of the line
# endings this script file itself happens to be checked out with.
string(CONCAT _dss_content
    "// GENERATED AT BUILD TIME by cmake/DssBuildStamp.cmake — DO NOT EDIT,\n"
    "// DO NOT COMMIT. Include `program/dss_build_stamp.hpp` instead; it is the\n"
    "// checked-in face of this file and the place the contract is documented.\n"
    "#pragma once\n"
    "\n"
    "#define DSS_BUILD_STAMP \"${_dss_stamp}\"\n")

set(_dss_previous "")
if(EXISTS "${DSS_STAMP_OUTPUT}")
    file(READ "${DSS_STAMP_OUTPUT}" _dss_previous)
endif()
if(NOT "${_dss_previous}" STREQUAL "${_dss_content}")
    get_filename_component(_dss_out_dir "${DSS_STAMP_OUTPUT}" DIRECTORY)
    file(MAKE_DIRECTORY "${_dss_out_dir}")
    file(WRITE "${DSS_STAMP_OUTPUT}" "${_dss_content}")
    message(STATUS "dss: build stamp -> ${_dss_stamp}")
endif()
return()
endif()

# ═══ THE SELF-TEST (`-DDSS_STAMP_MODE=selftest`, ctest `build/build_stamp_identity`) ═══
# Builds throwaway trees under DSS_STAMP_SCRATCH and runs THIS file in `stamp`
# mode over them, as the build does, reading back the header each run writes.
# The fixtures' declaration is fixed (`src,cmake,CMakeLists.txt,VERSION` read
# whole but for the records `cmake/record.md,cmake/check.py`, `tests` for its
# CMake scripts): those arms prove the MECHANISM. One more arm stamps the REAL
# tree (DSS_STAMP_SOURCE_DIR) without git, under the tree's own declaration
# (DSS_STAMP_INPUTS, DSS_STAMP_RECORDS, DSS_STAMP_SCRIPT_DIRS), and prints how
# long that took. Eleven hand the fixtures a dependency's sources and read the
# `.deps` component. Eleven more configure fixture PROJECTS against
# `cmake/DssBuildStampDeclaration.cmake`, the configure-time check that every
# directory a configure processes is declared -- seven of them over a
# dependency's sources the configure is handed. Every git command it runs,
# and every stamp run, has git's repository variables removed, so an exported
# GIT_DIR cannot point either at another repository. Each arm prints its
# verdict; the run fails on any red arm and on an arm count other than
# _DSS_SELFTEST_ARMS.
set(_DSS_SELFTEST_ARMS 44)
foreach(_dss_required IN ITEMS DSS_STAMP_SCRATCH DSS_STAMP_SOURCE_DIR)
    if(NOT DEFINED ${_dss_required} OR "${${_dss_required}}" STREQUAL "")
        message(FATAL_ERROR "DssBuildStamp selftest: ${_dss_required} was not passed.")
    endif()
endforeach()
_dss_stamp_read_inputs(_dss_real_inputs _dss_real_records _dss_real_scripts)
string(REPLACE ";" "," _dss_real_inputs "${_dss_real_inputs}")
string(REPLACE ";" "," _dss_real_records "${_dss_real_records}")
string(REPLACE ";" "," _dss_real_scripts "${_dss_real_scripts}")
set(_dss_t_inputs "src,cmake,CMakeLists.txt,VERSION")
set(_dss_t_records "cmake/record.md,cmake/check.py")
set(_dss_t_scripts "tests")
set(_dss_t_unset --unset=GIT_DIR --unset=GIT_WORK_TREE --unset=GIT_INDEX_FILE --unset=GIT_OBJECT_DIRECTORY
                 --unset=GIT_ALTERNATE_OBJECT_DIRECTORIES --unset=GIT_COMMON_DIR --unset=GIT_NAMESPACE)
set(_dss_t_ran 0)
set(_dss_t_failed 0)

function(_dss_t_arm name ok detail)
    math(EXPR _n "${_dss_t_ran} + 1")
    set(_dss_t_ran ${_n} PARENT_SCOPE)
    if(ok)
        message(STATUS "dss-build-stamp selftest: ok      ${name}")
    else()
        math(EXPR _f "${_dss_t_failed} + 1")
        set(_dss_t_failed ${_f} PARENT_SCOPE)
        message(STATUS "dss-build-stamp selftest: FAILED  ${name} -- ${detail}")
    endif()
endfunction()

function(_dss_t_write path text)
    get_filename_component(_d "${path}" DIRECTORY)
    file(MAKE_DIRECTORY "${_d}")
    file(WRITE "${path}" "${text}")
endfunction()

# Runs THIS file in stamp mode over `tree`, writing `header` -> the stamp it
# wrote (or ""), the exit code, and what it printed. `git` is a git path, or ""
# for no git; `inputs`, `records` and `scripts` are the declaration it is handed.
# ONE argument after `out_text` is the handed dependencies (`<NAME>=<directory>`,
# comma-separated); without it the run is handed none, which it is still TOLD.
function(_dss_t_stamp_into tree header git inputs records scripts out_value out_rc out_text)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env ${_dss_t_unset}
                "${CMAKE_COMMAND}" -DDSS_STAMP_MODE=stamp -DDSS_STAMP_VERSION=9.9.9
                "-DDSS_STAMP_SOURCE_DIR=${tree}" "-DDSS_STAMP_OUTPUT=${header}"
                "-DDSS_STAMP_GIT=${git}" "-DDSS_STAMP_INPUTS=${inputs}" "-DDSS_STAMP_RECORDS=${records}"
                "-DDSS_STAMP_SCRIPT_DIRS=${scripts}" "-DDSS_STAMP_HANDED_DEPENDENCIES=${ARGN}"
                -P "${CMAKE_CURRENT_FUNCTION_LIST_FILE}"
        RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
    set(_value "")
    if(EXISTS "${header}")
        file(READ "${header}" _h)
        if(_h MATCHES "#define DSS_BUILD_STAMP \"([^\"]*)\"")
            set(_value "${CMAKE_MATCH_1}")
        endif()
    endif()
    set(${out_value} "${_value}" PARENT_SCOPE)
    set(${out_rc} "${_rc}" PARENT_SCOPE)
    set(${out_text} "${_out}${_err}" PARENT_SCOPE)
endfunction()

# The fixture form: the header beside the tree, the fixtures' own declaration.
function(_dss_t_stamp tree git inputs out_value out_rc out_text)
    _dss_t_stamp_into("${tree}" "${tree}-out/stamp.hpp" "${git}" "${inputs}" "${_dss_t_records}" "${_dss_t_scripts}"
                      _v _rc _t ${ARGN})
    set(${out_value} "${_v}" PARENT_SCOPE)
    set(${out_rc} "${_rc}" PARENT_SCOPE)
    set(${out_text} "${_t}" PARENT_SCOPE)
endfunction()

function(_dss_t_git tree)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env ${_dss_t_unset}
                "${DSS_STAMP_GIT}" -c user.email=selftest@build-stamp.invalid -c user.name=build-stamp-selftest
                -c commit.gpgsign=false -c core.autocrlf=false -c core.hooksPath= ${ARGN}
        WORKING_DIRECTORY "${tree}" RESULT_VARIABLE _rc OUTPUT_QUIET ERROR_VARIABLE _err)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "DssBuildStamp selftest: `git ${ARGN}` failed in its fixture (rc ${_rc}): ${_err}")
    endif()
endfunction()

function(_dss_t_fixture tree)
    file(REMOVE_RECURSE "${tree}" "${tree}-out")
    _dss_t_write("${tree}/src/a.cpp" "int a;\n")
    _dss_t_write("${tree}/src/sub/b.hpp" "#pragma once\n")
    _dss_t_write("${tree}/cmake/x.cmake" "set(X 1)\n")
    _dss_t_write("${tree}/cmake/record.md" "a harness record\n")
    _dss_t_write("${tree}/cmake/check.py" "x = 1\n")
    _dss_t_write("${tree}/CMakeLists.txt" "project(p)\n")
    _dss_t_write("${tree}/VERSION" "9.9.9\n")
    _dss_t_write("${tree}/tests/t.cpp" "int t;\n")
    _dss_t_write("${tree}/tests/sub/CMakeLists.txt" "add_test(NAME t COMMAND t)\n")
    _dss_t_write("${tree}/docs/d.md" "doc\n")
    _dss_t_write("${tree}/.plans/p.md" "plan\n")
    _dss_t_write("${tree}/.harness-config/h.py" "x = 1\n")
endfunction()

set(_dss_hex16 "[0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f]")
set(_dss_hex12 "[0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f]")

# ── no git ──
set(_t "${DSS_STAMP_SCRATCH}/nogit")
_dss_t_fixture("${_t}")
_dss_t_stamp("${_t}" "" "${_dss_t_inputs}" _v1 _rc1 _o1)
_dss_t_stamp("${_t}" "" "${_dss_t_inputs}" _v2 _rc2 _o2)
set(_ok FALSE)
if(_rc1 EQUAL 0 AND _rc2 EQUAL 0 AND NOT _v1 STREQUAL "" AND _v1 STREQUAL _v2 AND NOT _o2 MATCHES "build stamp ->")
    set(_ok TRUE)
endif()
_dss_t_arm("no git: an unchanged tree stamps the same value twice, and the second run leaves the header alone"
           ${_ok} "'${_v1}' then '${_v2}' (rc ${_rc1}/${_rc2}); second run printed: ${_o2}")
set(_ok FALSE)
if(_v1 MATCHES "^9\\.9\\.9\\+src${_dss_hex16}$")
    set(_ok TRUE)
endif()
_dss_t_arm("no git: the value is <VERSION>+src<16 hex>, one token" ${_ok} "'${_v1}'")
file(APPEND "${_t}/src/a.cpp" "int a2;\n")
_dss_t_stamp("${_t}" "" "${_dss_t_inputs}" _v3 _rc _o)
set(_ok FALSE)
if(_rc EQUAL 0 AND NOT _v3 STREQUAL _v1 AND _v3 MATCHES "^9\\.9\\.9\\+src${_dss_hex16}$")
    set(_ok TRUE)
endif()
_dss_t_arm("no git: a byte added to a compiler source moves it" ${_ok} "'${_v1}' -> '${_v3}' (rc ${_rc})")
_dss_t_write("${_t}/src/sub/c.hpp" "#pragma once\n")
_dss_t_stamp("${_t}" "" "${_dss_t_inputs}" _v4 _rc _o)
set(_ok FALSE)
if(_rc EQUAL 0 AND NOT _v4 STREQUAL _v3)
    set(_ok TRUE)
endif()
_dss_t_arm("no git: a new file among the compiler sources moves it" ${_ok} "'${_v3}' -> '${_v4}'")
file(RENAME "${_t}/src/sub/c.hpp" "${_t}/src/sub/d.hpp")
_dss_t_stamp("${_t}" "" "${_dss_t_inputs}" _v5 _rc _o)
set(_ok FALSE)
if(_rc EQUAL 0 AND NOT _v5 STREQUAL _v4)
    set(_ok TRUE)
endif()
_dss_t_arm("no git: a compiler source renamed, its bytes unchanged, moves it" ${_ok} "'${_v4}' -> '${_v5}'")
file(APPEND "${_t}/tests/t.cpp" "int t2;\n")
file(APPEND "${_t}/docs/d.md" "more\n")
file(APPEND "${_t}/.plans/p.md" "more\n")
file(APPEND "${_t}/.harness-config/h.py" "y = 2\n")
_dss_t_write("${_t}/docs/new.md" "new\n")
_dss_t_stamp("${_t}" "" "${_dss_t_inputs}" _v6 _rc _o)
set(_ok FALSE)
if(_rc EQUAL 0 AND _v6 STREQUAL _v5)
    set(_ok TRUE)
endif()
_dss_t_arm("no git: a test's source, a document, a plan and a harness file leave it" ${_ok} "'${_v5}' -> '${_v6}'")
# The records beside the build's modules (2026-10-06, the P69 re-review's MINOR 1: `cmake` read whole read them).
file(APPEND "${_t}/cmake/record.md" "more\n")
file(APPEND "${_t}/cmake/check.py" "y = 2\n")
_dss_t_stamp("${_t}" "" "${_dss_t_inputs}" _v6r _rc _o)
set(_ok FALSE)
if(_rc EQUAL 0 AND _v6r STREQUAL _v6)
    set(_ok TRUE)
endif()
_dss_t_arm("no git: a declared record and checker under a whole input leave it" ${_ok} "'${_v6}' -> '${_v6r}'")
_dss_t_write("${_t}/cmake/new.h.in" "#define N 1\n")
_dss_t_stamp("${_t}" "" "${_dss_t_inputs}" _v6m _rc _o)
set(_ok FALSE)
if(_rc EQUAL 0 AND NOT _v6m STREQUAL _v6r)
    set(_ok TRUE)
endif()
_dss_t_arm("no git: an UNDECLARED file beside them is read, and moves it" ${_ok} "'${_v6r}' -> '${_v6m}'")
file(APPEND "${_t}/tests/sub/CMakeLists.txt" "# moved\n")
_dss_t_stamp("${_t}" "" "${_dss_t_inputs}" _v7 _rc _o)
set(_ok FALSE)
if(_rc EQUAL 0 AND NOT _v7 STREQUAL _v6m)
    set(_ok TRUE)
endif()
_dss_t_arm("no git: a CMake script under a test directory moves it" ${_ok} "'${_v6m}' -> '${_v7}'")
file(WRITE "${_t}/VERSION" "9.9.9\n\n")
_dss_t_stamp("${_t}" "" "${_dss_t_inputs}" _v8 _rc _o)
set(_ok FALSE)
if(_rc EQUAL 0 AND NOT _v8 STREQUAL _v7)
    set(_ok TRUE)
endif()
_dss_t_arm("no git: the VERSION file is an input" ${_ok} "'${_v7}' -> '${_v8}'")

# ── git ──
if(NOT DSS_STAMP_GIT OR NOT EXISTS "${DSS_STAMP_GIT}")
    message(FATAL_ERROR "DssBuildStamp selftest: no git ('${DSS_STAMP_GIT}'), so the work-tree arms cannot run -- "
                        "a named failure, never a skip: every leg this repository builds on has git.")
endif()
set(_t "${DSS_STAMP_SCRATCH}/git")
_dss_t_fixture("${_t}")
_dss_t_git("${_t}" init -q)
_dss_t_git("${_t}" add -A)
_dss_t_git("${_t}" commit -q -m fixture)
execute_process(COMMAND "${CMAKE_COMMAND}" -E env ${_dss_t_unset} "${DSS_STAMP_GIT}" rev-parse --short=12 HEAD
                WORKING_DIRECTORY "${_t}" OUTPUT_VARIABLE _head OUTPUT_STRIP_TRAILING_WHITESPACE)
_dss_t_stamp("${_t}" "${DSS_STAMP_GIT}" "${_dss_t_inputs}" _g1 _rc _o)
set(_ok FALSE)
if(_rc EQUAL 0 AND _g1 STREQUAL "9.9.9+g${_head}" AND _g1 MATCHES "^9\\.9\\.9\\+g${_dss_hex12}$")
    set(_ok TRUE)
endif()
_dss_t_arm("git: a tree whose inputs match HEAD stamps <VERSION>+g<12 hex>" ${_ok}
           "'${_g1}', HEAD '${_head}' (rc ${_rc}): ${_o}")
file(APPEND "${_t}/tests/t.cpp" "int t2;\n")
file(APPEND "${_t}/docs/d.md" "more\n")
file(APPEND "${_t}/.plans/p.md" "more\n")
file(APPEND "${_t}/.harness-config/h.py" "y = 2\n")
_dss_t_write("${_t}/docs/new.md" "new\n")
_dss_t_stamp("${_t}" "${DSS_STAMP_GIT}" "${_dss_t_inputs}" _g2 _rc _o)
set(_ok FALSE)
if(_rc EQUAL 0 AND _g2 STREQUAL _g1)
    set(_ok TRUE)
endif()
_dss_t_arm("git: dirt outside the inputs (a test's source, documents, a plan, a harness file) leaves it clean" ${_ok}
           "'${_g1}' -> '${_g2}'")
file(APPEND "${_t}/cmake/record.md" "more\n")
file(APPEND "${_t}/cmake/check.py" "y = 2\n")
_dss_t_stamp("${_t}" "${DSS_STAMP_GIT}" "${_dss_t_inputs}" _g2r _rc _o)
set(_ok FALSE)
if(_rc EQUAL 0 AND _g2r STREQUAL _g1)
    set(_ok TRUE)
endif()
_dss_t_arm("git: an edited declared record and checker leave it clean" ${_ok} "'${_g1}' -> '${_g2r}'")
_dss_t_write("${_t}/cmake/new.h.in" "#define N 1\n")
_dss_t_stamp("${_t}" "${DSS_STAMP_GIT}" "${_dss_t_inputs}" _g2m _rc _o)
set(_ok FALSE)
if(_rc EQUAL 0 AND _g2m MATCHES "^9\\.9\\.9\\+g${_head}\\.dirty${_dss_hex16}$")
    set(_ok TRUE)
endif()
_dss_t_arm("git: an UNDECLARED file beside them makes it dirty" ${_ok} "'${_g2m}'")
file(REMOVE "${_t}/cmake/new.h.in")   # gone again: the next arm's dirt is its own
file(APPEND "${_t}/src/a.cpp" "int a2;\n")
_dss_t_stamp("${_t}" "${DSS_STAMP_GIT}" "${_dss_t_inputs}" _g3 _rc _o)
set(_ok FALSE)
if(_rc EQUAL 0 AND _g3 MATCHES "^9\\.9\\.9\\+g${_head}\\.dirty${_dss_hex16}$")
    set(_ok TRUE)
endif()
_dss_t_arm("git: an edited compiler source makes it .dirty<16 hex>" ${_ok} "'${_g3}'")
file(WRITE "${_t}/src/a.cpp" "int a;\n")
_dss_t_write("${_t}/src/new.cpp" "int n;\n")
_dss_t_stamp("${_t}" "${DSS_STAMP_GIT}" "${_dss_t_inputs}" _g4 _rc _o)
set(_ok FALSE)
if(_rc EQUAL 0 AND _g4 MATCHES "^9\\.9\\.9\\+g${_head}\\.dirty${_dss_hex16}$" AND NOT _g4 STREQUAL _g3)
    set(_ok TRUE)
endif()
_dss_t_arm("git: an untracked file among the compiler sources makes it dirty" ${_ok} "'${_g3}' -> '${_g4}'")
file(REMOVE "${_t}/src/new.cpp")
file(APPEND "${_t}/tests/sub/CMakeLists.txt" "# moved\n")
_dss_t_stamp("${_t}" "${DSS_STAMP_GIT}" "${_dss_t_inputs}" _g5 _rc _o)
set(_ok FALSE)
if(_rc EQUAL 0 AND _g5 MATCHES "^9\\.9\\.9\\+g${_head}\\.dirty${_dss_hex16}$")
    set(_ok TRUE)
endif()
_dss_t_arm("git: a CMake script under a test directory makes it dirty" ${_ok} "'${_g5}'")

# ── the dependencies a configure was handed ──
# Two directories of the SAME bytes at different paths (`dep-a`, `dep-b`) and the fixtures above: `nogit` as it
# stands after its arms (value `_v8`) and `git` (value `_g5`, dirty).
set(_dep "${DSS_STAMP_SCRATCH}/dep-a")
set(_dep_b "${DSS_STAMP_SCRATCH}/elsewhere/dep-b")
file(REMOVE_RECURSE "${_dep}" "${_dep_b}")
foreach(_d IN ITEMS "${_dep}" "${_dep_b}")
    _dss_t_write("${_d}/include/dep.hpp" "#pragma once\n")
    _dss_t_write("${_d}/CMakeLists.txt" "# the dependency's own script\n")
endforeach()
_dss_t_stamp("${DSS_STAMP_SCRATCH}/nogit" "" "${_dss_t_inputs}" _h1 _rc1 _o "DEPX=${_dep}")
_dss_t_stamp("${DSS_STAMP_SCRATCH}/nogit" "" "${_dss_t_inputs}" _h2 _rc2 _o2 "DEPX=${_dep}")
set(_ok FALSE)
if(_rc1 EQUAL 0 AND _rc2 EQUAL 0 AND _h1 STREQUAL _h2 AND NOT _o2 MATCHES "build stamp ->"
   AND _h1 MATCHES "^9\\.9\\.9\\+src${_dss_hex16}\\.deps${_dss_hex16}$")
    string(REGEX REPLACE "\\.deps${_dss_hex16}$" "" _h1_head "${_h1}")
    if(_h1_head STREQUAL _v8)
        set(_ok TRUE)
    endif()
endif()
_dss_t_arm("handed: a handed dependency adds .deps<16 hex> to the value the tree has without it, the same twice"
           ${_ok} "'${_v8}' -> '${_h1}' then '${_h2}' (rc ${_rc1}/${_rc2})")
_dss_t_stamp("${DSS_STAMP_SCRATCH}/nogit" "" "${_dss_t_inputs}" _h3 _rc _o "")
set(_ok FALSE)
if(_rc EQUAL 0 AND _h3 STREQUAL _v8)
    set(_ok TRUE)
endif()
_dss_t_arm("handed: a configure handed nothing stamps the value with no .deps component" ${_ok}
           "'${_h3}', expected '${_v8}' (rc ${_rc})")
_dss_t_stamp("${DSS_STAMP_SCRATCH}/nogit" "" "${_dss_t_inputs}" _h4 _rc _o "DEPX=${_dep_b}")
set(_ok FALSE)
if(_rc EQUAL 0 AND _h4 STREQUAL _h1)
    set(_ok TRUE)
endif()
_dss_t_arm("handed: the same bytes handed from another directory stamp the same value" ${_ok}
           "'${_h1}' from dep-a, '${_h4}' from elsewhere/dep-b (rc ${_rc})")
_dss_t_write("${_dep}/.git/HEAD" "ref: refs/heads/main\n")
_dss_t_write("${_dep}/.git/objects/aa/bb" "an object\n")
_dss_t_stamp("${DSS_STAMP_SCRATCH}/nogit" "" "${_dss_t_inputs}" _h5 _rc _o "DEPX=${_dep}")
set(_ok FALSE)
if(_rc EQUAL 0 AND _h5 STREQUAL _h1)
    set(_ok TRUE)
endif()
_dss_t_arm("handed: a clone's own .git under the handed directory leaves it" ${_ok} "'${_h1}' -> '${_h5}' (rc ${_rc})")
file(APPEND "${_dep}/include/dep.hpp" "#define DEP 2\n")
_dss_t_stamp("${DSS_STAMP_SCRATCH}/nogit" "" "${_dss_t_inputs}" _h6 _rc _o "DEPX=${_dep}")
set(_ok FALSE)
if(_rc EQUAL 0 AND NOT _h6 STREQUAL _h1 AND _h6 MATCHES "^9\\.9\\.9\\+src${_dss_hex16}\\.deps${_dss_hex16}$")
    string(REGEX REPLACE "\\.deps${_dss_hex16}$" "" _h6_head "${_h6}")
    if(_h6_head STREQUAL _v8)
        set(_ok TRUE)
    endif()
endif()
_dss_t_arm("handed: a byte added to a handed dependency's source moves the .deps component, and only it" ${_ok}
           "'${_h1}' -> '${_h6}' (rc ${_rc})")
file(RENAME "${_dep}/include/dep.hpp" "${_dep}/include/dep_renamed.hpp")
_dss_t_stamp("${DSS_STAMP_SCRATCH}/nogit" "" "${_dss_t_inputs}" _h10 _rc _o "DEPX=${_dep}")
set(_ok FALSE)
if(_rc EQUAL 0 AND NOT _h10 STREQUAL _h6 AND _h10 MATCHES "\\.deps${_dss_hex16}$")
    set(_ok TRUE)
endif()
_dss_t_arm("handed: the same bytes under another file name stamp another value" ${_ok}
           "'${_h6}' -> '${_h10}' (rc ${_rc})")
_dss_t_stamp("${DSS_STAMP_SCRATCH}/nogit" "" "${_dss_t_inputs}" _h7 _rc _o "DEPY=${_dep_b}")
set(_ok FALSE)
if(_rc EQUAL 0 AND NOT _h7 STREQUAL _h4 AND _h7 MATCHES "\\.deps${_dss_hex16}$")
    set(_ok TRUE)
endif()
_dss_t_arm("handed: the same bytes handed as ANOTHER dependency stamp another value" ${_ok}
           "'${_h4}' as DEPX, '${_h7}' as DEPY (rc ${_rc})")
_dss_t_stamp("${DSS_STAMP_SCRATCH}/git" "${DSS_STAMP_GIT}" "${_dss_t_inputs}" _h8 _rc _o "DEPX=${_dep_b}")
set(_ok FALSE)
if(_rc EQUAL 0 AND _h8 MATCHES "^9\\.9\\.9\\+g${_head}\\.dirty${_dss_hex16}\\.deps${_dss_hex16}$")
    string(REGEX REPLACE "\\.deps${_dss_hex16}$" "" _h8_head "${_h8}")
    string(REGEX REPLACE "^.*\\.deps" "" _h8_tail "${_h8}")
    string(REGEX REPLACE "^.*\\.deps" "" _h4_tail "${_h4}")
    if(_h8_head STREQUAL _g5 AND _h8_tail STREQUAL _h4_tail)
        set(_ok TRUE)
    endif()
endif()
_dss_t_arm("handed: a work tree carries the same .deps component after its own commit and dirt" ${_ok}
           "'${_g5}' -> '${_h8}'; without git the component is '${_h4}' (rc ${_rc})")
_dss_t_stamp("${DSS_STAMP_SCRATCH}/nogit" "" "${_dss_t_inputs}" _h9 _rc _o "DEPX=${DSS_STAMP_SCRATCH}/no-such-dep")
string(REGEX REPLACE "[ \t\r\n]+" " " _o "${_o}")
set(_ok FALSE)
if(NOT _rc EQUAL 0 AND _o MATCHES "the dependency 'DEPX' was handed from '[^']*no-such-dep', which is no directory")
    set(_ok TRUE)
endif()
_dss_t_arm("handed: a hand-over that names no directory is refused, by the dependency's name" ${_ok} "rc ${_rc}: ${_o}")
_dss_t_stamp("${DSS_STAMP_SCRATCH}/nogit" "" "${_dss_t_inputs}" _h11 _rc _o "${_dep_b}")
string(REGEX REPLACE "[ \t\r\n]+" " " _o "${_o}")
set(_ok FALSE)
if(NOT _rc EQUAL 0 AND _o MATCHES "the handed dependency '[^']*dep-b' is not `<NAME>=<directory>`")
    set(_ok TRUE)
endif()
_dss_t_arm("handed: a hand-over that is no NAME=directory pair is refused as written" ${_ok} "rc ${_rc}: ${_o}")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env ${_dss_t_unset}
            "${CMAKE_COMMAND}" -DDSS_STAMP_MODE=stamp -DDSS_STAMP_VERSION=9.9.9
            "-DDSS_STAMP_SOURCE_DIR=${DSS_STAMP_SCRATCH}/nogit" "-DDSS_STAMP_OUTPUT=${DSS_STAMP_SCRATCH}/nogit-out/untold.hpp"
            "-DDSS_STAMP_GIT=" "-DDSS_STAMP_INPUTS=${_dss_t_inputs}" "-DDSS_STAMP_RECORDS=${_dss_t_records}"
            "-DDSS_STAMP_SCRIPT_DIRS=${_dss_t_scripts}" -P "${CMAKE_CURRENT_LIST_FILE}"
    RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
string(REGEX REPLACE "[ \t\r\n]+" " " _o "${_out}${_err}")
set(_ok FALSE)
if(NOT _rc EQUAL 0 AND _o MATCHES "DSS_STAMP_HANDED_DEPENDENCIES was not passed"
   AND NOT EXISTS "${DSS_STAMP_SCRATCH}/nogit-out/untold.hpp")
    set(_ok TRUE)
endif()
_dss_t_arm("handed: a run not told what its configure was handed is refused, and writes no header" ${_ok}
           "rc ${_rc}: ${_o}")

# ── the real tree, under its own declaration, without git ──
file(REMOVE_RECURSE "${DSS_STAMP_SCRATCH}/real-out")
string(TIMESTAMP _dss_t0 "%s%f" UTC)
_dss_t_stamp_into("${DSS_STAMP_SOURCE_DIR}" "${DSS_STAMP_SCRATCH}/real-out/stamp.hpp" "" "${_dss_real_inputs}"
                  "${_dss_real_records}" "${_dss_real_scripts}" _rv _rc _o)
string(TIMESTAMP _dss_t1 "%s%f" UTC)
math(EXPR _dss_ms "(${_dss_t1} - ${_dss_t0}) / 1000")
set(_ok FALSE)
if(_rc EQUAL 0 AND _rv MATCHES "^9\\.9\\.9\\+src${_dss_hex16}$")
    set(_ok TRUE)
endif()
_dss_t_arm("the real tree's declared inputs all exist and stamp it without git" ${_ok} "'${_rv}' (rc ${_rc}): ${_o}")
message(STATUS "dss-build-stamp selftest: measured: the real tree's no-git stamp took ${_dss_ms} ms "
               "(inputs ${_dss_real_inputs}; CMake scripts of ${_dss_real_scripts})")

# ── the declaration, checked at configure (cmake/DssBuildStampDeclaration.cmake) ──
# Fixture PROJECTS, configured with the build's own generator: each declares
# `src,cmake,CMakeLists.txt` read whole and `tests` for its scripts, as the
# top-level CMakeLists declares its own, and adds directories around them.
if(NOT DEFINED DSS_STAMP_GENERATOR OR DSS_STAMP_GENERATOR STREQUAL "")
    message(FATAL_ERROR "DssBuildStamp selftest: DSS_STAMP_GENERATOR was not passed, so the declaration arms cannot "
                        "configure their fixtures -- a named failure, never a skip.")
endif()
# Arguments after `out_text` are handed to the fixture's configure as they are (a `-D` cache entry). The
# fixture asks which dependencies it was handed into `HD`, as the top-level CMakeLists does, and hands the check
# the variable `_dss_t_handed_var` names (`HD` unless an arm says otherwise).
set(_dss_t_handed_var "HD")
function(_dss_t_configure case top_extra src_extra out_rc out_text)
    set(_root "${DSS_STAMP_SCRATCH}/decl-${case}")
    file(REMOVE_RECURSE "${_root}")
    string(CONCAT _top
        "cmake_minimum_required(VERSION 4.0)\nproject(declfixture NONE)\n"
        "include(\"${CMAKE_CURRENT_FUNCTION_LIST_DIR}/DssBuildStampDeclaration.cmake\")\n"
        "set(IN \"src,cmake,CMakeLists.txt\")\nset(SD \"tests\")\n"
        "dss_build_stamp_handed_dependencies(HD)\n"
        "cmake_language(DEFER CALL dss_build_stamp_check_declaration IN SD ${_dss_t_handed_var})\n"
        "add_subdirectory(src)\nadd_subdirectory(tests)\n${top_extra}")
    _dss_t_write("${_root}/tree/CMakeLists.txt" "${_top}")
    _dss_t_write("${_root}/tree/src/CMakeLists.txt" "add_subdirectory(sub)\n${src_extra}")
    _dss_t_write("${_root}/tree/src/sub/CMakeLists.txt" "# a declared input's subdirectory\n")
    _dss_t_write("${_root}/tree/tests/CMakeLists.txt" "# a declared script directory\n")
    _dss_t_write("${_root}/tree/tools/CMakeLists.txt" "# under no declaration\n")
    _dss_t_write("${_root}/dep/CMakeLists.txt" "# outside the source tree, as a fetched dependency is\n")
    # a dependency's sources held INSIDE the tree, as a copy built from handed sources holds them
    _dss_t_write("${_root}/tree/vendor/depx/CMakeLists.txt" "add_subdirectory(inner)\n")
    _dss_t_write("${_root}/tree/vendor/depx/inner/CMakeLists.txt" "# a directory of the dependency's own\n")
    set(_gen -G "${DSS_STAMP_GENERATOR}")
    if(DSS_STAMP_MAKE_PROGRAM)
        list(APPEND _gen "-DCMAKE_MAKE_PROGRAM=${DSS_STAMP_MAKE_PROGRAM}")
    endif()
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -S "${_root}/tree" -B "${_root}/build" ${_gen} ${ARGN}
        RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
    # CMake wraps an error message at ~80 columns, so a refusal is matched on its
    # text with every run of whitespace folded to one space.
    string(REGEX REPLACE "[ \t\r\n]+" " " _text "${_out}${_err}")
    set(${out_rc} "${_rc}" PARENT_SCOPE)
    set(${out_text} "${_text}" PARENT_SCOPE)
endfunction()
_dss_t_configure(ok "" "" _rc _o)
set(_ok FALSE)
if(_rc EQUAL 0)
    set(_ok TRUE)
endif()
_dss_t_arm("configure: every directory it processes lies under the declaration, so it passes" ${_ok}
           "rc ${_rc}: ${_o}")
_dss_t_configure(outside "add_subdirectory(tools)\n" "" _rc _o)
set(_ok FALSE)
if(NOT _rc EQUAL 0 AND _o MATCHES "configure processed tools, which lies under no declared input")
    set(_ok TRUE)
endif()
_dss_t_arm("configure: a directory under no declaration is refused, by its name" ${_ok} "rc ${_rc}: ${_o}")
_dss_t_configure(nested "" "add_subdirectory(../tools tools)\n" _rc _o)
set(_ok FALSE)
if(NOT _rc EQUAL 0 AND _o MATCHES "configure processed tools, which lies under no declared input")
    set(_ok TRUE)
endif()
_dss_t_arm("configure: one added from inside a declared directory is refused too -- the walk descends" ${_ok}
           "rc ${_rc}: ${_o}")
_dss_t_configure(dep "add_subdirectory(\"\${CMAKE_SOURCE_DIR}/../dep\" dep)\n" "" _rc _o)
set(_ok FALSE)
if(_rc EQUAL 0)
    set(_ok TRUE)
endif()
_dss_t_arm("configure: a directory outside the source tree, where a fetched dependency lives, is not judged" ${_ok}
           "rc ${_rc}: ${_o}")
# A dependency the configure is HANDED (CMake's own `FETCHCONTENT_SOURCE_DIR_<NAME>`), held inside the source tree:
# FetchContent itself adds the directory, and never fetches -- the repository it declares answers nowhere.
set(_dss_t_fetch
    "include(FetchContent)\nFetchContent_Declare(depx GIT_REPOSITORY \"https://invalid.invalid/depx.git\" GIT_TAG 0)\n")
string(APPEND _dss_t_fetch "FetchContent_MakeAvailable(depx)\n")
set(_dss_t_handed "-DFETCHCONTENT_SOURCE_DIR_DEPX=${DSS_STAMP_SCRATCH}/decl-handed/tree/vendor/depx")
_dss_t_configure(handed "${_dss_t_fetch}" "" _rc _o "${_dss_t_handed}")
set(_ok FALSE)
if(_rc EQUAL 0)
    set(_ok TRUE)
endif()
_dss_t_arm("configure: a dependency's sources it was handed, held inside the source tree, are not judged" ${_ok}
           "rc ${_rc}: ${_o}")
_dss_t_configure(nothanded "add_subdirectory(vendor/depx)\n" "" _rc _o)
set(_ok FALSE)
if(NOT _rc EQUAL 0 AND _o MATCHES "configure processed vendor/depx, vendor/depx/inner, which lies under no declared input")
    set(_ok TRUE)
endif()
_dss_t_arm("configure: the same directories with no hand-over are refused, each by its name" ${_ok} "rc ${_rc}: ${_o}")
_dss_t_configure(emptyhandover "add_subdirectory(tools)\n" "" _rc _o "-DFETCHCONTENT_SOURCE_DIR_DEPX=")
set(_ok FALSE)
if(NOT _rc EQUAL 0 AND _o MATCHES "configure processed tools, which lies under no declared input")
    set(_ok TRUE)
endif()
_dss_t_arm("configure: an EMPTY hand-over entry hands nothing over, so an undeclared directory is still refused" ${_ok}
           "rc ${_rc}: ${_o}")
set(_dss_t_handed "-DFETCHCONTENT_SOURCE_DIR_DEPX=${DSS_STAMP_SCRATCH}/decl-handedbeside/tree/vendor/depx")
_dss_t_configure(handedbeside "${_dss_t_fetch}add_subdirectory(tools)\n" "" _rc _o "${_dss_t_handed}")
set(_ok FALSE)
if(NOT _rc EQUAL 0 AND _o MATCHES "configure processed tools, which lies under no declared input")
    set(_ok TRUE)
endif()
_dss_t_arm("configure: a handed dependency excuses its own directories only -- one beside it is refused, alone" ${_ok}
           "rc ${_rc}: ${_o}")
# A script that records a hand-over ITSELF, after the stamp's wiring read them: the walk would excuse the
# directory and the stamp would not carry its bytes.
string(CONCAT _dss_t_late
    "set(FETCHCONTENT_SOURCE_DIR_DEPX \"\${CMAKE_SOURCE_DIR}/vendor/depx\" CACHE PATH \"\" FORCE)\n"
    "add_subdirectory(vendor/depx)\n")
_dss_t_configure(latehandover "${_dss_t_late}" "" _rc _o)
set(_ok FALSE)
if(NOT _rc EQUAL 0 AND _o MATCHES "ends with the handed dependencies 'DEPX=[^']*/vendor/depx'"
   AND _o MATCHES "the build stamp was wired with ''")
    set(_ok TRUE)
endif()
_dss_t_arm("configure: a hand-over recorded after the stamp was wired is refused, by the dependency's name" ${_ok}
           "rc ${_rc}: ${_o}")
set(_dss_t_handed_var "NEVER_ASKED")
_dss_t_configure(neverasked "" "" _rc _o)
set(_dss_t_handed_var "HD")
set(_ok FALSE)
if(NOT _rc EQUAL 0 AND _o MATCHES "`NEVER_ASKED` is not set")
    set(_ok TRUE)
endif()
_dss_t_arm("configure: a check not told what the stamp was wired with is refused, naming the variable" ${_ok}
           "rc ${_rc}: ${_o}")
_dss_t_configure(separator "" "" _rc _o "-DFETCHCONTENT_SOURCE_DIR_DEPX=${DSS_STAMP_SCRATCH}/decl-separator/de,px")
set(_ok FALSE)
if(NOT _rc EQUAL 0 AND _o MATCHES "the dependency 'DEPX' was handed from '[^']*de,px', and that holds a")
    set(_ok TRUE)
endif()
_dss_t_arm("configure: a hand-over whose path holds a separator of the stamp's list is refused, by name" ${_ok}
           "rc ${_rc}: ${_o}")

# ── refusals ──
# Asked of the WORK TREE on purpose: git takes a pathspec that matches nothing
# without a word, so there only the existence check can refuse the declaration.
_dss_t_stamp("${_t}" "${DSS_STAMP_GIT}" "${_dss_t_inputs},no-such-input" _r1 _rc _o)
set(_ok FALSE)
if(NOT _rc EQUAL 0 AND _o MATCHES "no-such-input")
    set(_ok TRUE)
endif()
_dss_t_arm("a declared input that names nothing is refused, by its name" ${_ok} "rc ${_rc}: ${_o}")
_dss_t_stamp("${_t}" "" "" _r2 _rc _o)
set(_ok FALSE)
if(NOT _rc EQUAL 0 AND _o MATCHES "DSS_STAMP_INPUTS was not passed")
    set(_ok TRUE)
endif()
_dss_t_arm("an inputs declaration that was not passed is refused" ${_ok} "rc ${_rc}: ${_o}")
# (CMake wraps an error message, so each refusal is matched with its whitespace folded, as the configure arms are.)
_dss_t_stamp_into("${_t}" "${_t}-out/stamp.hpp" "" "${_dss_t_inputs}" "cmake/no-such-record.md" "${_dss_t_scripts}"
                  _r3 _rc _o)
string(REGEX REPLACE "[ \t\r\n]+" " " _o "${_o}")
set(_ok FALSE)
if(NOT _rc EQUAL 0 AND _o MATCHES "declared record 'cmake/no-such-record.md' is not a file")
    set(_ok TRUE)
endif()
_dss_t_arm("a declared record that names no file is refused, by its name" ${_ok} "rc ${_rc}: ${_o}")
_dss_t_stamp_into("${_t}" "${_t}-out/stamp.hpp" "" "${_dss_t_inputs}" "tests/t.cpp" "${_dss_t_scripts}" _r4 _rc _o)
string(REGEX REPLACE "[ \t\r\n]+" " " _o "${_o}")
set(_ok FALSE)
if(NOT _rc EQUAL 0 AND _o MATCHES "declared record 'tests/t.cpp' is not a file, by that spelling, under a directory")
    set(_ok TRUE)
endif()
_dss_t_arm("a record outside every directory read whole is refused" ${_ok} "rc ${_rc}: ${_o}")

file(REMOVE_RECURSE "${DSS_STAMP_SCRATCH}")
if(NOT _dss_t_ran EQUAL _DSS_SELFTEST_ARMS)
    message(FATAL_ERROR "dss-build-stamp selftest: ARM COUNT ${_dss_t_ran}, expected ${_DSS_SELFTEST_ARMS} -- "
                        "_DSS_SELFTEST_ARMS is the ratchet")
endif()
if(_dss_t_failed GREATER 0)
    message(FATAL_ERROR "dss-build-stamp selftest: ${_dss_t_ran} arm(s), ${_dss_t_failed} failed")
endif()
message(STATUS "dss-build-stamp selftest: ${_dss_t_ran} arm(s), 0 failed")
