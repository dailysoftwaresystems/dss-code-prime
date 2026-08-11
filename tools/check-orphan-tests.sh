#!/usr/bin/env bash
# check-orphan-tests.sh — CI guard for D-TEST-UNIT-TIER-HAS-NO-ORPHAN-SOURCE-GUARD.
#
# Contract: every `*.cpp` under the tests root MUST either be registered by a
# STRUCTURAL `dss_add_test(NAME ... SOURCES ...)` call in some
# `tests/**/CMakeLists.txt`, or appear in the ALLOWLIST below with a reason whose
# CMakeLists reference THIS GUARD VERIFIES.
#
# ── WHAT AN ORPHAN TEST IS, AND WHY IT IS WORSE THAN A MISSING TEST ──────────
# An orphan is a `tests/**/*.cpp` that no `CMakeLists.txt` names. It compiles
# NOWHERE. No target links it, no ctest entry runs it, not one of its assertions
# ever executes. And it is INVISIBLE from every direction anyone normally looks:
# the file is present in the tree, it is committed, it reads like a test, `grep`
# finds its assertions, a reviewer reading the diff sees a test being added, and
# the suite goes green — because the suite never had anything to say about it.
# A MISSING test at least looks missing. This is absent coverage wearing the
# appearance of coverage, which is the strictly more dangerous of the two.
#
# ★ THIS IS THE FOURTH VACUOUS-TEST INSTANCE FOUND IN A SINGLE CYCLE (2026-08-10).
# The other three: an op-count floor asserting >= 8 against a live 12; a macho
# example that passed for four cycles while reading a narrow `char**` through an
# `unsigned short**`; a red-on-disable arm asserting a rejection that does not
# happen. Every one of those was a test that RAN and asserted nothing useful.
# This class is the same defect one step earlier — a test that does not even run.
# ✔MEASURED 2026-08-10, before this guard existed: NOTHING in this repository
# checked for it. `tools/` held only the two anchor-registry twins, the
# line-endings twins, the landing-log tool + its test, ssh helpers and a config.
# The only `file(GLOB_RECURSE)` under `tests/` is in `tests/examples/CMakeLists.txt`
# and it discovers example MANIFESTS, not unit-test sources. No `.github/workflows`
# job asserts an expected ctest entry count. An orphan could have sat in this tree
# indefinitely.
#
# ── WHY THE PARSE IS STRUCTURAL AND NOT A GREP ───────────────────────────────
# ★★ A LOOSE REGEX OVER THE CMakeLists TEXT GIVES THE WRONG ANSWER, and it gives
# it in the direction that greens the guard. ✔MEASURED 2026-08-10 on this tree:
# `grep -o dss_add_test` reports **240** mentions and a naive scan for `.cpp`
# strings reports **234** unique paths — against **230** files on disk and
# **228** real registrations. The surplus mentions are the function DEFINITION
# (`function(dss_add_test)`), the usage docblock above it, and prose comments.
# A guard built on those numbers would count a COMMENT as coverage, which is the
# instrument error this repo keeps paying for. So:
#   · `#` comments are stripped QUOTE-AWARE before anything is matched;
#   · `dss_add_test` is recognised only as a whole word followed by `(`, and its
#     arguments are read to the MATCHING `)` — so the 43 MULTI-LINE calls parse
#     exactly like the 185 single-line ones (✔MEASURED: 185 + 43 = 228);
#   · arguments are tokenised the way CMake tokenises them (whitespace, quotes,
#     and `(`/`)` as separators), then walked as `NAME` / `SOURCES` keywords —
#     the same two the real helper declares via `cmake_parse_arguments`
#     (tests/CMakeLists.txt:6).
# ✔MEASURED token shapes over all 228 registrations: 217 plain same-directory,
# 11 subdirectory-relative, ZERO absolute, ZERO containing `..`, ZERO quoted,
# ZERO carrying an unexpanded `${...}`. The resolver nevertheless implements
# `${CMAKE_CURRENT_SOURCE_DIR}`, `.`/`..` collapsing and absolute-path
# detection, and FAILS LOUD on anything it cannot resolve — a token this guard
# silently dropped would manufacture a phantom orphan, and a token it silently
# accepted could hide a real one.
#
# ── THE SCAN IS OF THE FILESYSTEM, NOT OF GIT, AND THAT IS DELIBERATE ────────
# An unwired test is unwired the moment it is WRITTEN, which is well before it is
# staged — and "before commit" is the only moment a guard can prevent rather than
# diagnose. `tools/check-line-endings.sh` reached the same conclusion the hard way
# and had to grow its Check E for exactly this tier. Consequence, stated rather
# than hidden: transient output under `tests/` would be scanned too.
# ✔MEASURED 2026-08-10 — `git status --porcelain --ignored tests/` lists ZERO
# ignored and ZERO untracked paths, and `ScratchDir(Location::InsideRepo)` roots
# at `<cwd>/test-scratch` where cwd is the build directory or the repo root,
# never `tests/`. If build or run output ever does land here, MOVE THE OUTPUT.
# Do not add an ignore list: an exception list is the mechanism this guard's own
# allowlist is fenced against below.
#
# ── EXIT CODES ───────────────────────────────────────────────────────────────
#   0  every source is registered or allowlisted
#   1  ORPHAN — a source is registered nowhere
#   2  SCAN COLLAPSED — missing root, a per-dimension floor, a token the parser
#      could not resolve, or a FAILED SELF-TEST (a partial parse, and a guard
#      that cannot demonstrate failure, both clear nobody)
#   3  STALE ALLOWLIST — an exemption that no longer describes reality
# All classes are reported in ONE run; the exit code carries the highest
# precedence one (2 > 1 > 3), because a collapsed scan makes the other verdicts
# untrustworthy and an orphan is the contract while a rotted exemption is hygiene.
#
# ── USAGE ────────────────────────────────────────────────────────────────────
#   check-orphan-tests.sh              scan <repo>/tests AND run the self-test
#   check-orphan-tests.sh <root>       scan <root> only (no self-test)
# The self-test runs whenever no root is given, which is the form ctest invokes,
# so the ctest-driven run ALWAYS proves the guard can fail. There is no flag to
# switch it off.
#
# Cross-platform: this is the bash variant for Linux/macOS; `check-orphan-tests.ps1`
# is the Windows twin and MUST stay behaviourally identical
# (D-GATE-SCRIPT-PS1-PAIRING-UNCHECKED — pairing by EXISTENCE is not pairing by
# BEHAVIOUR). Every line either twin prints is ASCII-ONLY and carries NO absolute
# path, deliberately: the pair is verified by DIFFING their output byte-for-byte,
# and neither a console encoding nor a path separator may be able to fake a
# disagreement. POSIX awk only — no gawk extensions, and the awk program body is
# ASCII even in its comments — because the macOS leg may run an old awk under an
# unknown locale.
set -uo pipefail

SCRIPT_PATH="${BASH_SOURCE[0]}"
SCRIPT_DIR="$(cd "$(dirname "${SCRIPT_PATH}")" && pwd)"
SCRIPT_ABS="${SCRIPT_DIR}/$(basename "${SCRIPT_PATH}")"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

# ── FAIL-CLOSED PER-DIMENSION FLOORS ────────────────────────────────────────
# ★★ THREE floors, one per dimension, because a collapse in ANY ONE of them
# produces a clean-looking pass. Lose the source enumeration and there is nothing
# to accuse; lose the CMakeLists enumeration and NOTHING is registered so every
# source is an orphan (loud, at least); lose only the PARSE and the registered
# set shrinks silently while both enumerations still look healthy — that last one
# is the shape that greens a guard while it checks nothing.
# ⚠ THIS REPOSITORY HAS ALREADY SHIPPED EXACTLY THAT BUG:
# `D-GATE-ANCHOR-GUARD-FAILS-OPEN-ON-MISSING-ROOT` — the anchor guard reported
# `OK (1 src anchors all resolve to plans)` while scanning nothing at all, the
# "1" being `echo "" | wc -l`. A guard that reports success over what it never
# read is the worst defect a guard can have.
# ★ AND THE FLOORS ARE NOT THE ASSERTION. The assertion is the PROPERTY — every
# source is registered or allowlisted — which holds at any tree size. An exact
# pinned orphan count of 0 is deliberately NOT used as the check: a pin like that
# is satisfied by a scan that found nothing to count.
# Values sit far below the live figures (✔MEASURED 2026-08-10: 230 sources,
# 21 CMakeLists, 228 registrations) so ordinary churn never trips them. They
# catch COLLAPSE, not drift. If one fires, fix the scan; never lower the floor.
SOURCE_FLOOR=150
CMAKELISTS_FLOOR=12
REGISTRATION_FLOOR=150

# ── THE ALLOWLIST ───────────────────────────────────────────────────────────
# Row format, `|`-separated:  <source>|<CMakeLists that references it>|<reason>
# Both paths are relative to the TESTS ROOT. Reasons carry no `|` and no tab.
#
# ★★ AN EXEMPTION LIST THAT CAN ROT IS THE "GUARD WEAKENED EVERY TIME IT FIRES"
# PATTERN, which this repository has anchored TWICE
# (`D-TEST-PE64-CONFOUND-PIN-WEAKENED-BY-ITS-OWN-SUBJECT`). So every entry is
# MACHINE-CHECKED on every run, three ways, and any failure is exit 3:
#   (a) the source still EXISTS under the tests root — an entry naming a deleted
#       file is dead weight that reads like live coverage policy;
#   (b) the source is NOT registered by `dss_add_test` — the moment the real
#       mechanism covers it, the exemption is a false statement and must go;
#   (c) the CMakeLists named in column 2 still STRUCTURALLY references the
#       source. This is what turns the "reason" from PROSE into a CHECKED
#       CLAIM: if `examples_runner.cpp` stops being an `add_executable` source,
#       the entry stops being true and this guard says so, instead of quietly
#       exempting a file that is now genuinely orphaned.
# ⚠ (c) is verified by tokenising column 2's file the same structural way the
# registration parse works — comments stripped, `${CMAKE_CURRENT_SOURCE_DIR}`
# expanded, paths resolved against that file's own directory. A mention in a
# COMMENT does not satisfy it.
# ★ BOTH TWINS MUST CARRY BYTE-IDENTICAL ROWS. Two entries today, and every fact
# in each was confirmed verbatim against the tree.
ORPHAN_ALLOWLIST='
examples/examples_runner.cpp|examples/CMakeLists.txt|NOT a gtest suite. It is the ONE example-corpus runner binary, built by add_executable(dss_examples_runner examples_runner.cpp) at examples/CMakeLists.txt:18 and driven per-example by the ctest entries that same file generates from the expected.json glob. Registering it via dss_add_test would make it a test in its own right, which it is not.
test_support/pch_stub.cpp|CMakeLists.txt|NOT a test. A translation unit that exists only so add_library(dss_test_pch STATIC ...) at CMakeLists.txt:89 has something to compile, which is what lets target_precompile_headers PRODUCE the one shared test PCH every dss_add_test target consumes via REUSE_FROM. It contains no assertions and defines no symbol worth running.
'

# ── ROOT SELECTION ──────────────────────────────────────────────────────────
# ★ The label printed is `tests` for the default root and the ARGUMENT VERBATIM
# for a scoped run — never a resolved absolute path. That is what lets the two
# twins' whole output be compared byte-for-byte on the same subject: `pwd` in
# Git Bash answers `/c/...` where pwsh answers `C:\...`, and that difference
# would show up as a "disagreement" that is really just two shells describing
# the same directory.
ROOT_ARG="${1:-}"
if [[ -n "${ROOT_ARG}" ]]; then
    TESTS_ROOT_IN="${ROOT_ARG}"
    ROOT_LABEL="${ROOT_ARG}"
    RUN_SELFTEST=0
else
    TESTS_ROOT_IN="${REPO_ROOT}/tests"
    ROOT_LABEL="tests"
    RUN_SELFTEST=1
fi

echo "orphan-tests: root=${ROOT_LABEL}"

if [[ ! -d "${TESTS_ROOT_IN}" ]]; then
    echo "orphan-tests: FAIL - the tests root does not exist. Refusing to report a pass on a scan of nothing."
    echo "orphan-tests: FAIL - 0 sources / 0 CMakeLists / 0 registrations from 0 calls / 0 allowlisted: 0 orphan(s), 0 stale allowlist entries."
    exit 2
fi
TESTS_ROOT="$(cd "${TESTS_ROOT_IN}" && pwd)"

# ── ONE cumulative temp registry and ONE trap ───────────────────────────────
# ★ Set ONCE. Reassigning `trap ... EXIT` per temp file REPLACES the handler
# rather than adding to it, so the last assignment silently stops cleaning up
# what the earlier ones covered — a leak the sibling anchor guard shipped once,
# on every failing run. `${_tmps[@]+"${_tmps[@]}"}` is the bash-3.2-safe
# expansion (macOS ships 3.2, where `set -u` makes a plain empty `"${arr[@]}"`
# fatal). Each site registers itself on the SAME LINE, never through a helper:
# command substitution runs in a SUBSHELL, so a `_t="$(_mktmp)"` helper appends
# to the subshell's copy of the array and the parent's stays empty.
_tmps=()
_tmpdirs=()
trap 'rm -f ${_tmps[@]+"${_tmps[@]}"}; rm -rf ${_tmpdirs[@]+"${_tmpdirs[@]}"}' EXIT

# ════════════════════════════════════════════════════════════════════════════
# THE PARSER. Kept in ONE awk program fed a file list, so the `.ps1` twin has a
# single algorithm to mirror rather than a scattering of pipeline stages.
# Emits tab-separated records on stdout:
#   R <resolved source>  <registration NAME>  <cmakelists>   a registration
#   T <cmakelists>  <resolved source>           any STRUCTURAL .cpp token in that
#                                               file (used only to verify an
#                                               allowlist reference site)
#   E <message>                                 fail-loud: the parse is incomplete
#   C <n>                                       dss_add_test calls parsed
# Every path is relative to the tests root, forward-slashed, so the records are
# host- and shell-independent.
# ════════════════════════════════════════════════════════════════════════════
ORPHAN_AWK='
# Literal (non-regex) replace-all. Used instead of gsub so that no part of this
# parser depends on how a given awk escapes ${ } in a dynamic regex - the tokens
# it rewrites are CMake variable references, which are nothing but metacharacters.
function replaceAll(s, from, to,   out, p) {
    out = ""
    while ((p = index(s, from)) > 0) {
        out = out substr(s, 1, p - 1) to
        s = substr(s, p + length(from))
    }
    return out s
}
# Quote-aware "#" comment strip for ONE line, carrying quote state across lines in
# the file-global inq. A "#" inside a double-quoted CMake argument is CONTENT, not
# a comment. MEASURED 2026-08-10: zero such cases and zero multi-line quoted
# strings exist under tests/ today - this is written correctly anyway, because
# "correct only while an incidental property holds" is how the next reader
# inherits a silent mis-parse.
function strip(line,   out, rest, q, h) {
    out = ""; rest = line
    while (rest != "") {
        if (inq == 0) {
            q = index(rest, "\"")
            h = index(rest, "#")
            if (h > 0 && (q == 0 || h < q)) return out substr(rest, 1, h - 1)
            if (q > 0) { out = out substr(rest, 1, q); rest = substr(rest, q + 1); inq = 1; continue }
            return out rest
        }
        q = index(rest, "\"")
        if (q > 0) { out = out substr(rest, 1, q); rest = substr(rest, q + 1); inq = 0; continue }
        return out rest
    }
    return out
}
function dirOf(p,   d) {
    if (index(p, "/") == 0) return ""
    d = p
    sub(/\/[^\/]*$/, "", d)
    return d
}
# Join dir + token and collapse "." / "..". Returns "" if the path escapes the
# tests root, which is a fail-loud condition and never a silent skip.
function normPath(dir, tok,   full, n, parts, i, k, stack, out) {
    if (dir == "") full = tok; else full = dir "/" tok
    n = split(full, parts, "/")
    k = 0
    for (i = 1; i <= n; i++) {
        if (parts[i] == "" || parts[i] == ".") continue
        if (parts[i] == "..") { if (k == 0) return ""; k--; continue }
        k++; stack[k] = parts[i]
    }
    out = ""
    for (i = 1; i <= k; i++) out = (out == "" ? stack[i] : out "/" stack[i])
    return out
}
# Resolve one CMake source token to a tests-root-relative path. quiet suppresses
# the E record: the T pass sets it, because an unresolvable token there can only
# ever cost an allowlist entry its confirmation - i.e. it fails CLOSED (exit 3)
# rather than exempting anything, and reporting it twice would just be noise.
function resolveTok(fname, dir, tok, quiet,   t, p) {
    t = replaceAll(tok, "\"", "")
    t = replaceAll(t, "${CMAKE_CURRENT_SOURCE_DIR}", ".")
    if (index(t, "${") > 0) {
        if (!quiet) print "E\t" fname ": SOURCES token " tok " carries a CMake variable this guard cannot resolve, so the parse is INCOMPLETE. Teach BOTH twins to expand it, or use a literal path; do not let a token go unread."
        return ""
    }
    if (substr(t, 1, 1) == "/" || t ~ /^[A-Za-z]:/) {
        if (!quiet) print "E\t" fname ": SOURCES token " tok " is an ABSOLUTE path, so it cannot be placed in the tests-root-relative scan set."
        return ""
    }
    p = normPath(dir, t)
    if (p == "") {
        if (!quiet) print "E\t" fname ": SOURCES token " tok " resolves OUTSIDE the tests root, so this guard cannot account for it."
        return ""
    }
    return p
}
function parseCall(fname, dir, argtext,   n, toks, i, mode, name, nsrc, t, p) {
    argtext = replaceAll(argtext, "\t", " ")
    argtext = replaceAll(argtext, "\n", " ")
    n = split(argtext, toks, "[ ]+")
    mode = ""; name = ""; nsrc = 0
    for (i = 1; i <= n; i++) {
        t = toks[i]
        if (t == "") continue
        # NAME and SOURCES are the only keywords dss_add_test declares
        # (cmake_parse_arguments at tests/CMakeLists.txt:10), so anything after
        # SOURCES that is not NAME is a source. Mirroring the real helper is the
        # point: a parser that invents keywords would silently drop sources.
        if (t == "NAME" || t == "SOURCES") { mode = t; continue }
        if (mode == "NAME") { name = t; mode = ""; continue }
        if (mode == "SOURCES") {
            p = resolveTok(fname, dir, t, 0)
            if (p != "") { print "R\t" p "\t" name "\t" fname; nsrc++ }
            continue
        }
    }
    calls++
    if (name == "") print "E\t" fname ": a dss_add_test call declares no NAME."
    if (nsrc == 0) print "E\t" fname ": dss_add_test(NAME " name ") registers no resolvable source."
}
function proc(fname, text,   dir, rest, s, l, prevc, i, n, depth, c, argtext, tmp, toks2, t, p) {
    dir = dirOf(fname)
    rest = text
    while (1) {
        if (match(rest, /dss_add_test[ \t]*\(/) == 0) break
        s = RSTART; l = RLENGTH
        prevc = (s > 1) ? substr(rest, s - 1, 1) : ""
        # Whole-word only. Without this, an identifier ENDING in dss_add_test
        # would be read as a call. (function(dss_add_test) is already excluded by
        # the required "(" after the name.)
        if (prevc ~ /[A-Za-z0-9_]/) { rest = substr(rest, s + l); continue }
        i = s + l
        depth = 1
        n = length(rest)
        while (i <= n) {
            c = substr(rest, i, 1)
            if (c == "(") depth++
            else if (c == ")") { depth--; if (depth == 0) break }
            i++
        }
        if (depth != 0) {
            print "E\t" fname ": an unterminated dss_add_test( call - the parse cannot be trusted."
            break
        }
        argtext = substr(rest, s + l, i - (s + l))
        parseCall(fname, dir, argtext)
        rest = substr(rest, i + 1)
    }
    # Every STRUCTURAL .cpp token in the file, for allowlist reference-site
    # verification. "(" and ")" are CMake token separators, so they are split on
    # here too - without that, add_executable(r examples_runner.cpp) yields the
    # token examples_runner.cpp) and the reference is missed. MEASURED: an earlier
    # draft of this scan did exactly that and reported both allowlisted files as
    # referenced NOWHERE, which would have made both rows look stale.
    tmp = replaceAll(text, "\t", " ")
    tmp = replaceAll(tmp, "\n", " ")
    tmp = replaceAll(tmp, "(", " ")
    tmp = replaceAll(tmp, ")", " ")
    n = split(tmp, toks2, "[ ]+")
    for (i = 1; i <= n; i++) {
        t = replaceAll(toks2[i], "\"", "")
        if (length(t) < 5) continue
        if (substr(t, length(t) - 3) != ".cpp") continue
        p = resolveTok(fname, dir, t, 1)
        if (p != "") print "T\t" fname "\t" p
    }
}
# ONE-FILE LOOKBEHIND. POSIX awk has no ENDFILE, so each file is accumulated and
# flushed at the NEXT file boundary AND in END. Without the boundary flush the
# LAST file would be the only one processed; without the END flush it would be
# the only one skipped.
FNR == 1 { if (have) proc(pf, buf); pf = FILENAME; buf = ""; inq = 0; have = 1 }
{ line = $0; sub(/\r$/, "", line); buf = buf strip(line) "\n" }
END {
    if (have) proc(pf, buf)
    print "C\t" (calls + 0)
}
'

# The WITNESS SELECTOR for the self-test, kept beside the parser it must agree
# with. It emits ONE line: <cmakelists>\t<line no>\t<token>\t<resolved path> for
# the first single-line `dss_add_test` whose sole SOURCES token is a plain
# same-directory name AND occurs EXACTLY ONCE in that file's raw text.
# ★ Uniqueness is a SELECTION CRITERION, not a separate assertion, so the two can
# never drift apart: if no such registration exists the self-test REFUSES to run
# the arm rather than mutating something ambiguous.
WITNESS_AWK='
function countLit(s, needle,   n, p) { n = 0; while ((p = index(s, needle)) > 0) { n++; s = substr(s, p + length(needle)) } return n }
function flush(   i, j, s, inner, n, t, mode, name, src, nsrc, d) {
    if (found) return
    for (i = 1; i <= nl; i++) {
        s = L[i]
        sub(/^[ \t]+/, "", s); sub(/[ \t\r]+$/, "", s)
        if (index(s, "dss_add_test(") != 1) continue
        if (substr(s, length(s)) != ")") continue
        if (countLit(s, "(") != 1 || countLit(s, ")") != 1) continue
        inner = substr(s, length("dss_add_test(") + 1, length(s) - length("dss_add_test(") - 1)
        n = split(inner, t, "[ ]+")
        mode = ""; name = ""; nsrc = 0; src = ""
        for (j = 1; j <= n; j++) {
            if (t[j] == "") continue
            if (t[j] == "NAME" || t[j] == "SOURCES") { mode = t[j]; continue }
            if (mode == "NAME") { name = t[j]; mode = ""; continue }
            if (mode == "SOURCES") { src = t[j]; nsrc++ }
        }
        if (nsrc != 1) continue
        if (index(src, "/") > 0) continue
        if (index(src, "$") > 0) continue
        if (countLit(buf, src) != 1) continue
        d = pf
        if (index(d, "/") == 0) d = ""; else sub(/\/[^\/]*$/, "", d)
        print pf "\t" i "\t" src "\t" (d == "" ? src : d "/" src)
        found = 1
        return
    }
}
FNR == 1 { if (have) flush(); pf = FILENAME; nl = 0; buf = ""; have = 1 }
{ nl++; L[nl] = $0; buf = buf $0 "\n" }
END { if (have) flush() }
'

# ════════════════════════════════════════════════════════════════════════════
# THE SCAN. Everything below runs with the tests root as cwd, so `find .` yields
# root-relative paths directly and no path arithmetic is needed anywhere.
# ════════════════════════════════════════════════════════════════════════════
cd "${TESTS_ROOT}" || exit 2

_src_list="$(mktemp)"; _tmps+=("${_src_list}")
_cml_list="$(mktemp)"; _tmps+=("${_cml_list}")
_parse="$(mktemp)";    _tmps+=("${_parse}")

# LC_ALL=C makes the order BYTE-ordinal, which is what the `.ps1` twin sorts by.
# A culture-aware collation would order `test_support/` differently and the two
# twins' output would differ for no real reason.
find . -type f -name '*.cpp'          -print | sed 's|^\./||' | LC_ALL=C sort > "${_src_list}"
find . -type f -name 'CMakeLists.txt' -print | sed 's|^\./||' | LC_ALL=C sort > "${_cml_list}"
_n_src="$(grep -c . "${_src_list}" || true)"
_n_cml="$(grep -c . "${_cml_list}" || true)"

_cml_files=()
while IFS= read -r _f; do [[ -n "${_f}" ]] && _cml_files+=("${_f}"); done < "${_cml_list}"

_collapsed=0
_collapse_msgs=""
# ★ The empty case is handled HERE rather than left to a floor further down.
# Under `set -u` an empty `"${arr[@]}"` is a FATAL unbound-variable on bash 3.2,
# which is the bash macOS ships - i.e. exactly the platform this `.sh` exists to
# serve. It would still be red, but red with a bash error instead of the sentence
# that says what happened, and "the guard died" reads to most people as "the
# guard is broken", not "the scan collapsed".
if [[ "${#_cml_files[@]}" -eq 0 ]]; then
    _collapse_msgs+="orphan-tests: FAIL - found 0 CMakeLists.txt under the tests root, below its floor of ${CMAKELISTS_FLOOR}."$'\n'
    _collapse_msgs+="  Nothing was parsed, so NOTHING is registered. Refusing to report a verdict at all; fix the scan, do not lower the floor."$'\n'
    _collapsed=1
else
    # rc DIRECTLY, never after a pipe: awk's own status is the only thing that
    # can distinguish "parsed cleanly" from "died on file 3 of 21".
    awk "${ORPHAN_AWK}" "${_cml_files[@]}" > "${_parse}"
    _awk_rc=$?
    if [[ "${_awk_rc}" -ne 0 ]]; then
        _collapse_msgs+="orphan-tests: FAIL - the CMakeLists parser exited ${_awk_rc}. A partial parse can only UNDER-count registrations, so it can never be trusted to clear a source. Refusing to report a verdict."$'\n'
        _collapsed=1
    fi
fi

_reg_list="$(mktemp)"; _tmps+=("${_reg_list}")
_tok_list="$(mktemp)"; _tmps+=("${_tok_list}")
_err_list="$(mktemp)"; _tmps+=("${_err_list}")
awk -F'\t' '$1 == "R" { print $2 }'            "${_parse}" | LC_ALL=C sort -u > "${_reg_list}"
awk -F'\t' '$1 == "T" { print $2 "\t" $3 }'    "${_parse}" | LC_ALL=C sort -u > "${_tok_list}"
awk -F'\t' '$1 == "E" { print $2 }'            "${_parse}"                    > "${_err_list}"
_n_reg_calls="$(awk -F'\t' '$1 == "C" { print $2 }' "${_parse}")"
[[ -n "${_n_reg_calls}" ]] || _n_reg_calls=0
_n_reg="$(grep -c . "${_reg_list}" || true)"
_n_err="$(grep -c . "${_err_list}" || true)"

# ── FLOORS, one per dimension ───────────────────────────────────────────────
if [[ "${_n_src}" -lt "${SOURCE_FLOOR}" ]]; then
    _collapse_msgs+="orphan-tests: FAIL - the source scan found only ${_n_src} *.cpp under the tests root, below its floor of ${SOURCE_FLOOR}."$'\n'
    _collapse_msgs+="  This does NOT mean every test is wired - it means the SCAN COLLAPSED. Refusing to report a pass; fix the scan, do not lower the floor."$'\n'
    _collapsed=1
fi
if [[ "${_n_cml}" -lt "${CMAKELISTS_FLOOR}" ]]; then
    _collapse_msgs+="orphan-tests: FAIL - the CMakeLists scan found only ${_n_cml} files, below its floor of ${CMAKELISTS_FLOOR}."$'\n'
    _collapse_msgs+="  A collapsed CMakeLists scan registers nothing, so it cannot clear a single source. Refusing to report a pass; fix the scan, do not lower the floor."$'\n'
    _collapsed=1
fi
if [[ "${_n_reg}" -lt "${REGISTRATION_FLOOR}" ]]; then
    _collapse_msgs+="orphan-tests: FAIL - the parser resolved only ${_n_reg} distinct registered sources, below its floor of ${REGISTRATION_FLOOR}."$'\n'
    _collapse_msgs+="  THIS IS THE DIMENSION THAT FAILS QUIETEST: both enumerations can look healthy while the PARSE has stopped matching, and every clear then evaporates. Refusing to report a pass; fix the parser, do not lower the floor."$'\n'
    _collapsed=1
fi
if [[ "${_n_err}" -gt 0 ]]; then
    _collapse_msgs+="orphan-tests: FAIL - ${_n_err} CMakeLists token(s) could not be resolved, so the parse is INCOMPLETE:"$'\n'
    while IFS= read -r _e; do
        [[ -z "${_e}" ]] && continue
        _collapse_msgs+="    ${_e}"$'\n'
    done < "${_err_list}"
    _collapsed=1
fi

# ── PHANTOM REGISTRATIONS: registered, but no such file ─────────────────────
# ★★ THE OTHER DIRECTION OF THE SAME PROPERTY, and it was MEASURED MISSING.
# ✔MEASURED 2026-08-10 by sabotage: with quote-aware comment stripping REMOVED
# from the parser, this guard stayed fully GREEN — the usage docblock in
# tests/CMakeLists.txt contains a commented `dss_add_test(NAME core/test_strong_ids
# SOURCES test_strong_ids.cpp)`, which then parsed as a real registration for a
# path that does not exist. Nothing noticed, because the guard only ever asked
# "is every FILE registered?" and never "does every REGISTRATION name a file?".
# A registered path that resolves to nothing clears nobody, so a registered set
# full of fiction is a census that cannot be trusted - hence the collapse class,
# not the orphan class. CMake itself errors on a missing source at configure time,
# so this should be unreachable in practice; the value is that it makes the
# comment-stripping mechanism GUARDED instead of merely present.
_phantom="$(mktemp)"; _tmps+=("${_phantom}")
LC_ALL=C comm -23 "${_reg_list}" "${_src_list}" > "${_phantom}"
_n_phantom="$(grep -c . "${_phantom}" || true)"
if [[ "${_n_phantom}" -gt 0 ]]; then
    _collapse_msgs+="orphan-tests: FAIL - ${_n_phantom} registration(s) name a source that does NOT exist under the tests root:"$'\n'
    _pi=0
    while IFS= read -r _p; do
        [[ -z "${_p}" ]] && continue
        _pi=$(( _pi + 1 ))
        if [[ "${_pi}" -le 20 ]]; then
            _collapse_msgs+="    PHANTOM: ${_p}"$'\n'
        fi
    done < "${_phantom}"
    if [[ "${_n_phantom}" -gt 20 ]]; then
        _collapse_msgs+="    ... and $(( _n_phantom - 20 )) more."$'\n'
    fi
    _collapse_msgs+="  A registration pointing at nothing clears nobody, so the registered set is partly fiction and this guard refuses to clear anything from it. Either the source was renamed without its CMakeLists, or the parser is reading text that is not a registration at all."$'\n'
    _collapsed=1
fi

# ── ALLOWLIST VALIDATION ────────────────────────────────────────────────────
_allow_rows="$(mktemp)"; _tmps+=("${_allow_rows}")
_allow_list="$(mktemp)"; _tmps+=("${_allow_list}")
printf '%s\n' "${ORPHAN_ALLOWLIST}" | awk -F'|' 'NF >= 3 { print }'          > "${_allow_rows}"
awk -F'|' '{ print $1 }' "${_allow_rows}" | LC_ALL=C sort -u                 > "${_allow_list}"
_n_allow="$(grep -c . "${_allow_list}" || true)"

_n_stale=0
_stale_msgs=""
while IFS='|' read -r _ap _asite _areason; do
    [[ -z "${_ap:-}" || -z "${_asite:-}" ]] && continue
    if ! grep -qxF -- "${_ap}" "${_src_list}"; then
        _stale_msgs+="  STALE: the allowlist names '${_ap}', which does NOT exist under the tests root."$'\n'
        _stale_msgs+="         An exemption for a deleted file reads like live coverage policy and protects nothing. Delete the row."$'\n'
        _n_stale=$(( _n_stale + 1 )); continue
    fi
    if grep -qxF -- "${_ap}" "${_reg_list}"; then
        _stale_msgs+="  STALE: the allowlist exempts '${_ap}', but dss_add_test now REGISTERS it."$'\n'
        _stale_msgs+="         The real mechanism covers it, so the exemption is a false statement about this tree. Delete the row."$'\n'
        _n_stale=$(( _n_stale + 1 )); continue
    fi
    if ! grep -qxF -- "${_asite}" "${_cml_list}"; then
        _stale_msgs+="  STALE: the allowlist says '${_ap}' is referenced by '${_asite}', which does not exist."$'\n'
        _n_stale=$(( _n_stale + 1 )); continue
    fi
    if ! grep -qxF -- "${_asite}$(printf '\t')${_ap}" "${_tok_list}"; then
        _stale_msgs+="  STALE: '${_asite}' no longer STRUCTURALLY references '${_ap}'."$'\n'
        _stale_msgs+="         That reference IS the reason the row exists, so the file may now be genuinely orphaned. Re-check it, then either register it or correct the row - do not widen the exemption."$'\n'
        _n_stale=$(( _n_stale + 1 ))
    fi
done < "${_allow_rows}"

# ── THE PROPERTY: every source is registered or allowlisted ─────────────────
_cleared="$(mktemp)"; _tmps+=("${_cleared}")
_orphans="$(mktemp)"; _tmps+=("${_orphans}")
cat "${_reg_list}" "${_allow_list}" | grep . | LC_ALL=C sort -u > "${_cleared}"
LC_ALL=C comm -23 "${_src_list}" "${_cleared}" > "${_orphans}"
_n_orphan="$(grep -c . "${_orphans}" || true)"

# ════════════════════════════════════════════════════════════════════════════
# REPORT — every failure class in ONE run. A guard that aborts after the first
# problem makes the reader re-run it N times to learn N things, and the
# remediation text never prints at all.
# ════════════════════════════════════════════════════════════════════════════
[[ -z "${_collapse_msgs}" ]] || printf '%s' "${_collapse_msgs}"

if [[ "${_n_orphan}" -gt 0 ]]; then
    echo "orphan-tests: FAIL - ${_n_orphan} test source(s) under the tests root are named by NO CMakeLists.txt."
    echo "They compile nowhere and no ctest entry runs them. Every assertion inside them is dead"
    echo "text. This is ABSENT COVERAGE wearing the appearance of coverage: the file is there, it"
    echo "reads like a test, the diff that added it looked like added coverage, and the suite is"
    echo "green because it never had anything to say about them."
    echo ""
    while IFS= read -r _o; do
        [[ -z "${_o}" ]] && continue
        echo "  ORPHAN: ${_o}"
    done < "${_orphans}"
    echo ""
    echo "Fix: either"
    echo "  (a) REGISTER it - add dss_add_test(NAME <dir>/<stem> SOURCES <file>) to the"
    echo "      CMakeLists.txt of its own directory, then RUN it. A registration that compiles"
    echo "      is not yet a test that asserts; check that it fails when it should."
    echo "  (b) DELETE it, if it was superseded and nobody noticed because nothing ran it."
    echo "  (c) if it is genuinely NOT a test source (a runner main, a PCH stub), add a row to"
    echo "      the ALLOWLIST in BOTH tools/check-orphan-tests.sh and"
    echo "      tools/check-orphan-tests.ps1, naming the reason AND the CMakeLists that"
    echo "      references it. The guard verifies that reference on every run, so the exemption"
    echo "      cannot quietly stop being true."
    echo "Do NOT widen this guard to make a red go away. An orphan is the one defect here that"
    echo "costs nothing to fix and everything to leave in place."
fi

if [[ "${_n_stale}" -gt 0 ]]; then
    echo "orphan-tests: FAIL - the ALLOWLIST no longer describes this tree:"
    echo ""
    printf '%s' "${_stale_msgs}"
    echo "An exemption list that can rot is the pattern this repository has anchored twice: a"
    echo "guard weakened every time it fires ends up asserting nothing. Fix the ROW."
fi

if [[ "${_collapsed}" -ne 0 || "${_n_orphan}" -gt 0 || "${_n_stale}" -gt 0 ]]; then
    _verdict="FAIL"
else
    _verdict="OK"
fi
echo "orphan-tests: ${_verdict} - ${_n_src} sources / ${_n_cml} CMakeLists / ${_n_reg} registrations from ${_n_reg_calls} calls / ${_n_allow} allowlisted: ${_n_orphan} orphan(s), ${_n_stale} stale allowlist entries."

_final_rc=0
if [[ "${_n_stale}" -gt 0 ]]; then _final_rc=3; fi
if [[ "${_n_orphan}" -gt 0 ]]; then _final_rc=1; fi
if [[ "${_collapsed}" -ne 0 ]]; then _final_rc=2; fi

# ════════════════════════════════════════════════════════════════════════════
# RED-ON-DISABLE SELF-TEST — the guard PROVES it can fail, on every ctest run.
#
# ★★ EXERCISE THE FAILURE ARM, DO NOT READ IT. `D-TEST-NONFATAL-GUARD-DEGRADES-
# TO-A-VACUOUS-PASS` and `D-CENSUS-INSTRUMENT-UNGUARDED-BY-CTEST` are both in this
# repository's registry because an instrument nobody executed was believed. So the
# self-test lives INSIDE the guard rather than beside it: it is impossible to run
# this check without also proving it reds, and there is exactly ONE thing to wire
# into ctest.
#
# THE SUBJECT is a MIRROR in a per-run temp directory: the CMakeLists copied
# byte-for-byte, and every `*.cpp` created EMPTY. That is a faithful subject
# precisely because this guard never opens a `.cpp` — it only needs the file to
# exist — and the mirror's own GREEN CONTROL arm is what PROVES the faithfulness
# instead of asserting it (it compares the mirror's census against the live one).
# Per-run temp, never a fixed path: `D-TEST-FIXED-SCRATCH-PATH-POPULATION` is a
# whole anchor family about constant scratch paths colliding under `ctest -j`.
#
# EACH ARM RE-INVOKES THE SCRIPT AS A SUBPROCESS and reads its EXIT CODE. An
# in-process helper would be cheaper and would NOT test the contract ctest
# actually consumes — and "printed failed=0 and exited 2" is a defect this repo
# carried for weeks, so the exit code is part of what has to be proven.
#
# FAIL-CLOSED RULES, all ENFORCED rather than described:
#   · the GREEN CONTROL runs first; a red arm on an unfaithful subject proves
#     nothing, and this is the arm that catches that;
#   · the witness is UNIQUE in the subject — it is SELECTED by that property, so
#     the selector and the assertion are the same code and cannot drift;
#   · the mutant differs by BYTES, verified with `cmp` against a pristine copy —
#     never by a line count, and stronger than a hash since it is the full
#     comparison a hash approximates;
#   · the witness is absent from the mutant BY THE SAME MATCHER THE GUARD USES:
#     the assertion is that the guard ITSELF printed `  ORPHAN: <witness>`;
#   · the mutant STILL PARSES, asserted numerically off the guard's own summary —
#     registrations must drop by EXACTLY ONE and the source count must not move,
#     which is what distinguishes a surgical mutation from a broken file;
#   · every restore is verified with `cmp` against the pristine copy, so a later
#     arm cannot silently run against an earlier arm's mutant. (A lane in this
#     repo restored files without `touch`, ninja skipped the rebuild, and every
#     arm silently linked the previous mutant. Only RUNNING it found that.)
#   · NOTHING naming the mutation is written inside the mutated tree.
# ════════════════════════════════════════════════════════════════════════════
if [[ "${RUN_SELFTEST}" -eq 0 ]]; then
    exit "${_final_rc}"
fi

_st_fail=0
_st_rc=0
_st_file="$(mktemp)"; _tmps+=("${_st_file}")
_run_guard() {
    # ★★ THE OUTPUT GOES TO A FILE AND EVERY LATER READ IS A FILE READ, NEVER A
    # PIPE, AND THAT IS A MEASURED CORRECTION rather than a style choice.
    # ✔MEASURED 2026-08-10: the first draft asserted with
    # `printf '%s\n' "$out" | grep -qF ...` and under `set -o pipefail` that is a
    # LATENT FLAKE — `grep -q` exits on its FIRST match, `printf` then dies of
    # SIGPIPE, and pipefail hands the pipeline 141, so a SATISFIED assertion reads
    # as a FAILED one. It reproduced in a sibling harness on some inputs and not
    # others purely on buffering, i.e. exactly the intermittent shape that gets
    # dismissed as noise. `rc DIRECTLY, never after a pipe` applies to a guard's
    # own self-checks too.
    # Plain assignment, never `local`: with `local v="$(cmd)"` the `$?` you read
    # is the `local` builtin's status, not the command's.
    bash "${SCRIPT_ABS}" "$1" > "${_st_file}" 2>&1
    _st_rc=$?
}
# Read a census number off the guard's own summary line BY LABEL, never by
# position. ★ An index-counted field was the first draft and it was already wrong
# by one (it read the stale count where the orphan count belongs), which is the
# COUNT-over-CONTENT mistake in miniature: a label cannot silently shift.
_st_num() {
    awk -v want="$1" '
        /^orphan-tests: (OK|FAIL) - / {
            n = split($0, w, /[ ]+/)
            for (i = 2; i <= n; i++) if (w[i] == want && w[i-1] ~ /^[0-9]+$/) { print w[i-1]; exit }
        }' "${_st_file}"
}
_st_expect() {  # $1 = arm label, $2 = expected rc, $3 = trailing note
    if [[ "${_st_rc}" -ne "$2" ]]; then
        echo "orphan-tests: SELF-TEST FAIL - arm $1 exited ${_st_rc}, expected $2. This guard CANNOT be"
        echo "  trusted: an arm built to red did not red, so a green run of it says nothing."
        sed 's/^/    | /' "${_st_file}"
        _st_fail=1
        return 1
    fi
    echo "orphan-tests: self-test arm $1 rc=$2 as expected$3"
    return 0
}
_st_says() {    # $1 = arm label, $2 = fixed substring the arm's message MUST carry
    if ! grep -qF -- "$2" "${_st_file}"; then
        echo "orphan-tests: SELF-TEST FAIL - arm $1 exited as expected but its message never said"
        echo "  '$2', so the red does not tell the reader what actually happened."
        _st_fail=1
        return 1
    fi
    return 0
}
# ★★ ABSENCE, asserted. This is what makes the three FLOOR arms below isolate one
# floor each instead of merely "being red". ✔MEASURED 2026-08-10 by sabotage: with
# all three floors forced to 0, an earlier arm set stayed fully GREEN — the empty
# root still reds for a floor-INDEPENDENT reason (zero CMakeLists parsed), and the
# substring `below its floor of 0` was then satisfied by a DIFFERENT floor's
# message than the one the arm claimed to be testing. A red arm that cannot say
# WHICH mechanism produced the red is not testing that mechanism.
_st_silent() {  # $1 = arm label, $2 = fixed substring that MUST NOT appear
    if grep -qF -- "$2" "${_st_file}"; then
        echo "orphan-tests: SELF-TEST FAIL - arm $1 also emitted '$2', so this arm is NOT isolating the"
        echo "  one mechanism it claims to test and could pass on the strength of another."
        _st_fail=1
        return 1
    fi
    return 0
}
_st_census() {  # $1 = arm label, then want-sources want-cmakelists want-registrations want-orphans
    _c_src="$(_st_num sources)"; _c_cml="$(_st_num CMakeLists)"
    _c_reg="$(_st_num registrations)"; _c_orp="$(_st_num 'orphan(s),')"
    if [[ "${_c_src}" != "$2" || "${_c_cml}" != "$3" || "${_c_reg}" != "$4" || "${_c_orp}" != "$5" ]]; then
        echo "orphan-tests: SELF-TEST FAIL - arm $1 was not SURGICAL. Got ${_c_src}/${_c_cml}/${_c_reg} sources/CMakeLists/registrations"
        echo "  and ${_c_orp} orphan(s); wanted $2/$3/$4 and $5. Either the mutant stopped parsing or it"
        echo "  changed more than the one thing this arm claims, and then the red means something else."
        _st_fail=1
        return 1
    fi
    return 0
}

_MIRROR="$(mktemp -d)";   _tmpdirs+=("${_MIRROR}")
_PRISTINE="$(mktemp -d)"; _tmpdirs+=("${_PRISTINE}")
_EMPTY="$(mktemp -d)";    _tmpdirs+=("${_EMPTY}")

# Build the mirror: directories, then every source as an EMPTY file, then the
# CMakeLists byte-for-byte (plus a pristine second copy to restore from and to
# `cmp` against). `tr | xargs -0`, not `xargs -d`: `-d` is a GNU extension the
# macOS leg does not have.
# ⓘ `_PRISTINE` deliberately holds the CMakeLists ONLY, no `*.cpp`. That makes it
# both the restore source AND, at zero extra cost, the perfectly isolated subject
# for the source-floor arm (21 CMakeLists, 228 registrations, 0 sources).
( cd "${_MIRROR}" && sed -n 's|/[^/]*$||p' "${_src_list}" "${_cml_list}" | LC_ALL=C sort -u \
    | tr '\n' '\0' | xargs -0 mkdir -p -- )
( cd "${_PRISTINE}" && sed -n 's|/[^/]*$||p' "${_cml_list}" | LC_ALL=C sort -u \
    | tr '\n' '\0' | xargs -0 mkdir -p -- )
( cd "${_MIRROR}" && tr '\n' '\0' < "${_src_list}" | xargs -0 touch -- )
while IFS= read -r _c; do
    [[ -z "${_c}" ]] && continue
    cp -- "${TESTS_ROOT}/${_c}" "${_MIRROR}/${_c}"
    cp -- "${TESTS_ROOT}/${_c}" "${_PRISTINE}/${_c}"
done < "${_cml_list}"

# ── ARM 0 — GREEN CONTROL. Without this, every red below is worthless.
_run_guard "${_MIRROR}"
if _st_expect "0 GREEN-CONTROL" 0 " (mirror is a faithful subject)"; then
    _st_census "0 GREEN-CONTROL" "${_n_src}" "${_n_cml}" "${_n_reg}" 0 || true
fi

# ── ARM 1 — THE ORPHAN. Delete ONE whole single-line registration; the source it
# named is then wired nowhere.
_w="$(cd "${_MIRROR}" && awk "${WITNESS_AWK}" "${_cml_files[@]}")"
_w_file="$(printf '%s' "${_w}" | cut -f1)"
_w_line="$(printf '%s' "${_w}" | cut -f2)"
_w_tok="$(printf '%s' "${_w}" | cut -f3)"
_w_path="$(printf '%s' "${_w}" | cut -f4)"
if [[ -z "${_w_file}" || -z "${_w_line}" || -z "${_w_path}" ]]; then
    echo "orphan-tests: SELF-TEST FAIL - no witness registration could be selected (it must be a"
    echo "  single-line dss_add_test whose sole source token occurs EXACTLY ONCE in its file)."
    echo "  Without a unique witness the mutation would be ambiguous, so the arm is REFUSED"
    echo "  rather than run weakly."
    _st_fail=1
else
    awk -v n="${_w_line}" 'FNR != n' "${_MIRROR}/${_w_file}" > "${_MIRROR}/${_w_file}.mut" \
        && mv "${_MIRROR}/${_w_file}.mut" "${_MIRROR}/${_w_file}"
    if cmp -s "${_PRISTINE}/${_w_file}" "${_MIRROR}/${_w_file}"; then
        echo "orphan-tests: SELF-TEST FAIL - the mutation changed NO bytes of the witness file. An arm"
        echo "  that did not mutate anything cannot prove a red."
        _st_fail=1
    else
        _run_guard "${_MIRROR}"
        if _st_expect "1 ORPHAN" 1 " (witness ${_w_path})"; then
            _st_says "1 ORPHAN" "  ORPHAN: ${_w_path}" || true
            _st_census "1 ORPHAN" "${_n_src}" "${_n_cml}" "$(( _n_reg - 1 ))" 1 || true
        fi
        # ── ARM 1b — A COMMENT IS NOT COVERAGE. ★★ ADDED BECAUSE IT WAS MEASURED
        # MISSING: sabotaging the parser's quote-aware comment stripping left the
        # whole arm set GREEN, i.e. the one mechanism that separates 228 real
        # registrations from 240 textual mentions was completely unexercised. This
        # arm re-adds the deleted registration AS A COMMENT and requires the guard
        # to still call the witness an orphan. If comments ever start counting, a
        # `#`-prefixed line becomes coverage and this arm is the only thing that
        # notices. (The phantom-registration check above catches the same sabotage
        # by an independent route; two mechanisms, deliberately.)
        printf '%s\n' "# dss_add_test(NAME $(dirname "${_w_path}")/${_w_tok%.cpp} SOURCES ${_w_tok})" >> "${_MIRROR}/${_w_file}"
        _run_guard "${_MIRROR}"
        if _st_expect "1b COMMENT-IS-NOT-COVERAGE" 1 " (witness still orphaned)"; then
            _st_says "1b COMMENT-IS-NOT-COVERAGE" "  ORPHAN: ${_w_path}" || true
            _st_census "1b COMMENT-IS-NOT-COVERAGE" "${_n_src}" "${_n_cml}" "$(( _n_reg - 1 ))" 1 || true
        fi
    fi
    cp -- "${_PRISTINE}/${_w_file}" "${_MIRROR}/${_w_file}"
    cmp -s "${_PRISTINE}/${_w_file}" "${_MIRROR}/${_w_file}" \
        || { echo "orphan-tests: SELF-TEST FAIL - restoring the witness file did not reproduce the pristine bytes; later arms would run against arm 1's mutant."; _st_fail=1; }
fi

# ── ARM 2 / 3 — COLLAPSE, the two whole-root cases: a root with nothing in it,
# then a root that is not there at all. Both red for reasons INDEPENDENT of the
# three floors, which is exactly why they are not sufficient on their own and arms
# 4-6 below exist.
_run_guard "${_EMPTY}"
if _st_expect "2 COLLAPSE-EMPTY-ROOT" 2 ""; then
    _st_says "2 COLLAPSE-EMPTY-ROOT" "found 0 CMakeLists.txt under the tests root" || true
fi
_run_guard "${_EMPTY}/definitely-not-here"
if _st_expect "3 COLLAPSE-MISSING-ROOT" 2 ""; then
    _st_says "3 COLLAPSE-MISSING-ROOT" "the tests root does not exist" || true
fi

# ════════════════════════════════════════════════════════════════════════════
# ARMS 4-6 — ONE ARM PER FLOOR, EACH ISOLATED BY ASSERTED ABSENCE.
# ★★ THESE EXIST BECAUSE THE FLOORS WERE MEASURED UNGUARDED. ✔MEASURED
# 2026-08-10 by sabotage: forcing all three floors to 0 left the whole arm set
# GREEN. The reason is instructive and general — arms 2 and 3 red for
# floor-independent reasons, so they cover the floors' TERRITORY without
# exercising the floors' MECHANISM. That is the same mistake the anchor guard's
# per-root floors were corrected for: "a control matched to the easy case is not
# evidence for the hard one".
# Each arm therefore drives ONE dimension below its floor while keeping the other
# two comfortably above, and asserts BOTH that its own floor message appears AND
# that the neighbouring floor messages do not.
# ════════════════════════════════════════════════════════════════════════════

# ── ARM 4 — SOURCE floor. Subject: the CMakeLists-only pristine tree. 0 sources,
# 21 CMakeLists, 228 registrations, so only the source floor can speak.
_run_guard "${_PRISTINE}"
if _st_expect "4 COLLAPSE-SOURCE-FLOOR" 2 ""; then
    _st_says   "4 COLLAPSE-SOURCE-FLOOR" "the source scan found only 0 *.cpp"                || true
    _st_silent "4 COLLAPSE-SOURCE-FLOOR" "the CMakeLists scan found only"                    || true
    _st_silent "4 COLLAPSE-SOURCE-FLOOR" "the parser resolved only"                          || true
fi

# ── ARM 5 — CMAKELISTS floor. Rename all but two CMakeLists out of the way so the
# enumeration falls below its floor while all 230 sources stay in place.
# `mv X X.stashed` rather than moving files into a side directory: nothing has to
# be re-created, `find -name CMakeLists.txt` stops seeing them, and the restore is
# the same rename backwards.
_stashed=()
_keep=2
_idx=0
while IFS= read -r _c; do
    [[ -z "${_c}" ]] && continue
    _idx=$(( _idx + 1 ))
    if [[ "${_idx}" -le "${_keep}" ]]; then continue; fi
    mv -- "${_MIRROR}/${_c}" "${_MIRROR}/${_c}.stashed" && _stashed+=("${_c}")
done < "${_cml_list}"
_run_guard "${_MIRROR}"
if _st_expect "5 COLLAPSE-CMAKELISTS-FLOOR" 2 ""; then
    _st_says   "5 COLLAPSE-CMAKELISTS-FLOOR" "the CMakeLists scan found only ${_keep} files" || true
    _st_silent "5 COLLAPSE-CMAKELISTS-FLOOR" "the source scan found only"                    || true
fi
for _c in ${_stashed[@]+"${_stashed[@]}"}; do
    mv -- "${_MIRROR}/${_c}.stashed" "${_MIRROR}/${_c}"
done

# ── ARM 6 — REGISTRATION floor: THE DIMENSION THAT FAILS QUIETEST. Both
# enumerations stay healthy (230 sources, >= 12 CMakeLists) while the PARSE stops
# resolving enough registrations. Achieved by stashing the registration-densest
# CMakeLists, selected from the guard's OWN parse output rather than by guessing
# which files are big.
_dense="$(awk -F'\t' '$1 == "R" { c[$4]++ } END { for (f in c) print c[f] "\t" f }' "${_parse}" | LC_ALL=C sort -rn)"
_stashed=()
_left="${_n_reg}"
_cml_left="${_n_cml}"
while IFS=$(printf '\t') read -r _cnt _cf; do
    [[ -z "${_cf:-}" ]] && continue
    [[ "${_left}" -lt "${REGISTRATION_FLOOR}" ]] && break
    [[ "$(( _cml_left - 1 ))" -lt "${CMAKELISTS_FLOOR}" ]] && break
    mv -- "${_MIRROR}/${_cf}" "${_MIRROR}/${_cf}.stashed" \
        && { _stashed+=("${_cf}"); _left=$(( _left - _cnt )); _cml_left=$(( _cml_left - 1 )); }
done <<< "${_dense}"
if [[ "${_left}" -ge "${REGISTRATION_FLOOR}" ]]; then
    echo "orphan-tests: SELF-TEST FAIL - arm 6 could not drive the registration count below its floor"
    echo "  (${_left} left, floor ${REGISTRATION_FLOOR}) while keeping >= ${CMAKELISTS_FLOOR} CMakeLists. The arm is REFUSED"
    echo "  rather than run in a shape that proves something else."
    _st_fail=1
else
    _run_guard "${_MIRROR}"
    if _st_expect "6 COLLAPSE-REGISTRATION-FLOOR" 2 ""; then
        _st_says   "6 COLLAPSE-REGISTRATION-FLOOR" "the parser resolved only"                || true
        _st_silent "6 COLLAPSE-REGISTRATION-FLOOR" "the source scan found only"              || true
        _st_silent "6 COLLAPSE-REGISTRATION-FLOOR" "the CMakeLists scan found only"          || true
    fi
fi
for _c in ${_stashed[@]+"${_stashed[@]}"}; do
    mv -- "${_MIRROR}/${_c}.stashed" "${_MIRROR}/${_c}"
done
# Prove the stash/unstash round trip left the subject byte-identical. Without this
# every arm after 6 would be running against a tree that merely LOOKS restored.
while IFS= read -r _c; do
    [[ -z "${_c}" ]] && continue
    cmp -s "${_PRISTINE}/${_c}" "${_MIRROR}/${_c}" \
        || { echo "orphan-tests: SELF-TEST FAIL - after the floor arms, a mirror CMakeLists no longer matches its pristine copy; later arms would run against a mutated subject."; _st_fail=1; break; }
done < "${_cml_list}"

# The first allowlist row drives arms 7-9. Everything about them is derived from
# the row, so reordering or replacing the allowlist cannot leave a stale literal
# behind in the self-test.
_a1_path="$(awk -F'|' 'NR == 1 { print $1 }' "${_allow_rows}")"
_a1_site="$(awk -F'|' 'NR == 1 { print $2 }' "${_allow_rows}")"
_a1_base="$(basename "${_a1_path}")"
_a1_dir="$(dirname "${_a1_path}")"
_a1_stem="${_a1_base%.cpp}"

# ── ARM 7 — STALE ALLOWLIST: the named file is gone.
rm -f -- "${_MIRROR}/${_a1_path}"
_run_guard "${_MIRROR}"
if _st_expect "7 STALE-ALLOW-FILE-GONE" 3 ""; then
    _st_says "7 STALE-ALLOW-FILE-GONE" "does NOT exist under the tests root" || true
    _st_census "7 STALE-ALLOW-FILE-GONE" "$(( _n_src - 1 ))" "${_n_cml}" "${_n_reg}" 0 || true
fi
touch -- "${_MIRROR}/${_a1_path}"

# ── ARM 8 — STALE ALLOWLIST: the file is NOW registered, so the exemption is a
# lie. The line added is an ordinary-looking registration; nothing in it names the
# mutation, so the guard cannot key off a marker instead of the real property.
printf '%s\n' "dss_add_test(NAME ${_a1_dir}/${_a1_stem} SOURCES ${_a1_base})" >> "${_MIRROR}/${_a1_site}"
_run_guard "${_MIRROR}"
if _st_expect "8 STALE-ALLOW-NOW-REGISTERED" 3 ""; then
    _st_says "8 STALE-ALLOW-NOW-REGISTERED" "dss_add_test now REGISTERS it" || true
    _st_census "8 STALE-ALLOW-NOW-REGISTERED" "${_n_src}" "${_n_cml}" "$(( _n_reg + 1 ))" 0 || true
fi
cp -- "${_PRISTINE}/${_a1_site}" "${_MIRROR}/${_a1_site}"
cmp -s "${_PRISTINE}/${_a1_site}" "${_MIRROR}/${_a1_site}" \
    || { echo "orphan-tests: SELF-TEST FAIL - restoring the allowlist reference site after arm 8 did not reproduce the pristine bytes."; _st_fail=1; }

# ── ARM 9 — STALE ALLOWLIST: the reference site stops referencing the file. THIS
# is the arm that makes the allowlist REASON a checked claim rather than prose.
awk -v s="${_a1_base}" 'index($0, s) == 0' "${_PRISTINE}/${_a1_site}" > "${_MIRROR}/${_a1_site}"
if cmp -s "${_PRISTINE}/${_a1_site}" "${_MIRROR}/${_a1_site}"; then
    echo "orphan-tests: SELF-TEST FAIL - arm 9 removed no line, so the reference site still references"
    echo "  the allowlisted source and the arm would pass for the wrong reason."
    _st_fail=1
else
    _run_guard "${_MIRROR}"
    if _st_expect "9 STALE-ALLOW-SITE-DROPPED-REFERENCE" 3 ""; then
        _st_says "9 STALE-ALLOW-SITE-DROPPED-REFERENCE" "no longer STRUCTURALLY references" || true
        _st_census "9 STALE-ALLOW-SITE-DROPPED-REFERENCE" "${_n_src}" "${_n_cml}" "${_n_reg}" 0 || true
    fi
fi
cp -- "${_PRISTINE}/${_a1_site}" "${_MIRROR}/${_a1_site}"

# ── ARM 10 — GREEN AFTER RESTORE. Every mutation undone, the subject is back.
_run_guard "${_MIRROR}"
if _st_expect "10 GREEN-AFTER-RESTORE" 0 " (all mutations undone)"; then
    _st_census "10 GREEN-AFTER-RESTORE" "${_n_src}" "${_n_cml}" "${_n_reg}" 0 || true
fi

if [[ "${_st_fail}" -ne 0 ]]; then
    echo "orphan-tests: SELF-TEST FAILED. Treat the verdict above as UNPROVEN: this guard has not"
    echo "  demonstrated that it can fail, which is the only thing that makes a green mean"
    echo "  anything. Fix the guard before trusting its census."
    exit 2
fi
echo "orphan-tests: self-test OK - 12 arms exercised (2 green controls, orphan, comment-is-not-coverage, 2 whole-root collapse, 3 per-floor collapse, 3 stale-allowlist); this guard is PROVEN able to fail."
exit "${_final_rc}"
