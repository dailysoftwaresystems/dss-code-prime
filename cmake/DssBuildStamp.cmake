# ═══ THE COMPILER'S BUILD STAMP ══════════════════════════════════════════════
#
# Run at BUILD time (`cmake -P`) by the `dss_build_stamp_generate` target in the
# top-level CMakeLists, which also documents the wiring. This file computes ONE
# string — `DSS_BUILD_STAMP` — and writes it into a generated header, but only
# when its VALUE has changed.
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
# Over-invalidation costs one recompile of one small runtime unit.
# Under-invalidation links an artifact compiled by a DIFFERENT compiler and the
# failure is silent. So every uncertain case degrades toward "always differ".
#
# ── INPUTS (passed with -D; see the target that invokes this) ────────────────
#   DSS_STAMP_VERSION     the repo-root VERSION file's contents (the top-level
#                         CMakeLists already reads it into DSS_VERSION)
#   DSS_STAMP_SOURCE_DIR  the repo root — the working directory git is asked about
#   DSS_STAMP_OUTPUT      the generated header to write
#   DSS_STAMP_GIT         path to git, or EMPTY when configure found none
#
# ── THE VALUE ────────────────────────────────────────────────────────────────
#   <VERSION>                                    git present, HEAD clean
#   <VERSION>+g<short-commit>                    …and the commit it was built from
#   <VERSION>+g<short-commit>.dirty<16 hex>      …plus a digest of the DIRT
#   <VERSION>+nogit<UTC timestamp>.<serial>      no git / no work tree
#
# No component ever contains whitespace, so the whole stamp is a single token —
# the property `tests/program/test_build_stamp.cpp` pins, because this string
# ends up inside a cache FILENAME.

cmake_minimum_required(VERSION 4.0)

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
    # That is the failure this whole file exists to make impossible.
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
    set(_dss_status_probe "${DSS_STAMP_OUTPUT}.status-probe")
    set(_dss_diff_probe   "${DSS_STAMP_OUTPUT}.diff-probe")
    get_filename_component(_dss_out_dir "${DSS_STAMP_OUTPUT}" DIRECTORY)
    file(MAKE_DIRECTORY "${_dss_out_dir}")

    execute_process(
        COMMAND "${DSS_STAMP_GIT}" status --porcelain
        WORKING_DIRECTORY "${DSS_STAMP_SOURCE_DIR}"
        OUTPUT_FILE "${_dss_status_probe}"
        ERROR_QUIET)
    execute_process(
        COMMAND "${DSS_STAMP_GIT}" diff HEAD
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
    # ── NO GIT / NO WORK TREE: ALWAYS MISS ───────────────────────────────────
    # Nothing here can identify the source the compiler was built from, so the
    # only honest answer is "assume it changed". A value that always differs
    # makes every lookup a MISS, which costs recompiles and cannot ever serve a
    # wrong artifact — the safe end of the asymmetry stated at the top.
    #
    # ★ A TIMESTAMP ALONE IS NOT ENOUGH, and the reason is the one that makes
    # this class of bug survive review: `string(TIMESTAMP)` has ONE-SECOND
    # resolution, so two builds in the same second collide and the guarantee
    # quietly becomes "usually differs". The serial file removes the tie; it
    # lives beside the output, so wiping the build directory wipes both together
    # and the header is regenerated anyway.
    set(_dss_serial_file "${DSS_STAMP_OUTPUT}.nogit-serial")
    set(_dss_serial 0)
    if(EXISTS "${_dss_serial_file}")
        file(READ "${_dss_serial_file}" _dss_serial)
        string(STRIP "${_dss_serial}" _dss_serial)
        if(NOT _dss_serial MATCHES "^[0-9]+$")
            set(_dss_serial 0)
        endif()
    endif()
    math(EXPR _dss_serial "${_dss_serial} + 1")
    get_filename_component(_dss_out_dir "${DSS_STAMP_OUTPUT}" DIRECTORY)
    file(MAKE_DIRECTORY "${_dss_out_dir}")
    file(WRITE "${_dss_serial_file}" "${_dss_serial}")

    string(TIMESTAMP _dss_now "%Y%m%dT%H%M%SZ" UTC)
    string(APPEND _dss_stamp "+nogit${_dss_now}.${_dss_serial}")
endif()

# ── WRITE ONLY ON CHANGE ─────────────────────────────────────────────────────
# `dss_build_stamp_generate` is an always-out-of-date target, so this script
# runs on EVERY build. Writing unconditionally would touch the header every
# time, and every TU that includes it would recompile every time — turning a
# cache-correctness mechanism into a permanent build-time tax. The compare is
# over the FULL generated text, not over the stamp alone, so a change to the
# banner below is picked up too.
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
