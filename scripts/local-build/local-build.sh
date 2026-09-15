#!/usr/bin/env bash
# PURPOSE: build dsscp incrementally on this host, and optionally run ctest.
# Local incremental build + test harness for dsscp.
#
# Usage:
#   scripts/local-build/local-build.sh                    # build build/dbg
#   scripts/local-build/local-build.sh --test             # build then run ctest
#   scripts/local-build/local-build.sh --configure        # cmake configure + build
#   scripts/local-build/local-build.sh --clean            # wipe THIS TREE + reconfigure + build
#   scripts/local-build/local-build.sh --tree rel         # operate on build/rel instead
#   scripts/local-build/local-build.sh --tree lane1 --build-type Debug
#
# ⚠ A LANE TREE TAKES --build-type EXACTLY ONCE, ON ITS FIRST CONFIGURE, AND
# REFUSES IT EVER AFTER. Only `dbg` -> Debug and `rel` -> Release are names this
# repository has an established meaning for; any other tree name must be TOLD its
# type the first time (rc 3 otherwise, naming the flag), and passing the flag to a
# tree whose cache already declares one is ALSO rc 3 -- so a caller cannot flip a
# configured tree underneath the artifacts already in it. Both refusals are
# deliberate; what was missing was saying so here.
#   first  run:  --tree <lane> --configure --build-type Debug
#   after that:  --tree <lane>            (and --clean to change the type)
# ✔MEASURED by EXECUTION 2026-08-29, after a cycle brief stated this invocation
# from the usage text WITHOUT running it and cost a lane two failed rounds --
# D-CYCLE-BRIEF-STATED-AN-INVOCATION-ITS-AUTHOR-HAD-NEVER-RUN, one level out: the
# usage text is itself an interface claim, and it was making one it could not keep.
#
# ★★★ `build/` IS A CONTAINER, NOT A BUILD TREE (the one-root layout, operator
# 2026-08-17: one root `build/`, one SUBDIRECTORY per build). This script used to
# treat `build/` itself as the tree -- `cmake -S . -B build`, `cmake --build build`,
# `cd build && ctest` -- and `--clean` was `rm -rf build`, which under that layout
# DELETES EVERY SIBLING TREE AT ONCE: `build/dbg`, `build/rel`, `build/perf` and the
# gate logs beside them. It would also have configured a FOURTH, unnamed tree at
# `build/` alongside the real ones, and then run ctest in the wrong directory.
# ⚠ Losing `build/rel` is not merely slow to recover: the Windows driver picks the
# NEWEST RELEASE build tree, so a deleted-then-stale `build/rel` silently answers
# discovery afterwards. Fixed 2026-08-20. An anchor name must never be wrapped --
# a split name is not greppable, so no tool can find it and the balance guard
# cannot see it. It goes on one line of its own:
#   D-SCRIPT-LOCAL-BUILD-TREATS-THE-BUILD-ROOT-AS-A-BUILD-TREE
#
# Designed to be safe to invoke without approval prompts in agentic workflows --
# read-only on src/, writes only inside build/<tree>.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

run_test=0
configure=0
clean=0
tree=dbg
build_type=""

# ★ The two tree names this repository has an established meaning for. A tree name
# NOT in this table is allowed (lane trees are ordinary), but configuring one for
# the FIRST time then requires an explicit --build-type: silently configuring a
# Debug tree that someone named `rel` is the kind of quiet wrong answer this file
# already shipped once.
default_build_type_for() {
    case "$1" in
        dbg) echo Debug ;;
        rel) echo Release ;;
        *)   echo "" ;;
    esac
}

# ── TOOLCHAIN I/O FAILURE — a distinct outcome from a build failure ─────────
# ✔MEASURED 2026-08-20 (cycle P23,
# D-BUILD-CONCURRENT-LANES-TRIP-A-TOOLCHAIN-HEADER-READ-FAILURE):
# under several concurrent lane builds, g++ failed to READ
# a standard-library header and printed
#     C:/Strawberry/c/include/c++/13.2.0/bits/locale_facets.tcc: Invalid argument
# followed by ~6 CASCADED diagnostics that look exactly like source defects
# ("'__use_cache' is not a class template"). An immediate retry built clean.
#
# ★ The discriminator is SHAPE, not wording: a compiler DIAGNOSTIC carries
#   `file:line:col:` and a severity; an I/O failure carries a bare path, a
#   colon, and an errno string. Combined with the path sitting inside the
#   TOOLCHAIN's own include tree — a directory this repository never edits —
#   that is not a statement about our source at all.
# ⚠ This deliberately does NOT retry. A retry that hides the event stops it
#   being root-caused, and the root cause (AV on-access scanning? handle
#   pressure? filesystem contention?) is unknown and is not guessed at here.
#   The build still FAILS; it just stops lying about whose fault it is.
local_build_toolchain_io_failure() {
    grep -Eqi '(/(usr|opt)/[^ :]*|[A-Za-z]:[\\/][^ :]*)(include|lib[\\/]gcc)[^ :]*:[[:space:]]+(invalid argument|input/output error|permission denied|resource temporarily unavailable|bad file descriptor)[[:space:]]*$|fatal error:[[:space:]]+error[[:space:]]+(writing[[:space:]]+to|closing)[[:space:]]+.+:[[:space:]]+(invalid argument|input/output error|permission denied|resource temporarily unavailable|bad file descriptor|no space left on device)[[:space:]]*$' "$1"
}

local_build_report_io_failure() {
    local log=$1
    echo "local-build.sh: FAIL — TOOLCHAIN I/O FAILURE (READ or WRITE), not a source defect." >&2
    echo "  The compiler could not READ a file inside its OWN include tree, or could" >&2
    echo "  not WRITE its own temporary. Every diagnostic after that line is a CASCADE" >&2
    echo "  and says nothing about this repository's source." >&2
    echo "  ⚠ The WRITE shape names a TEMP path, not an include path - it is the half" >&2
    echo "    this detector was blind to until 2026-08-21." >&2
    echo "  the line that classifies it:" >&2
    grep -Ei '(/(usr|opt)/[^ :]*|[A-Za-z]:[\\/][^ :]*)(include|lib[\\/]gcc)[^ :]*:[[:space:]]+(invalid argument|input/output error|permission denied|resource temporarily unavailable|bad file descriptor)[[:space:]]*$|fatal error:[[:space:]]+error[[:space:]]+(writing[[:space:]]+to|closing)[[:space:]]+.+:[[:space:]]+(invalid argument|input/output error|permission denied|resource temporarily unavailable|bad file descriptor|no space left on device)[[:space:]]*$' "$log" | head -3 | sed 's/^/    /' >&2
    echo "  ⚠ DO NOT act on the errors above it and DO NOT 'fix' the standard library." >&2
    echo "  ⚠ If this happened during a RED-ON-DISABLE arm, that arm measured NOTHING —" >&2
    echo "    re-run it; a restore that fails this way is not evidence about your change." >&2
    echo "  Root cause is UNKNOWN and is not guessed at here (see the anchor). Re-run the" >&2
    echo "  build; if it recurs at the same file, say so in the row rather than retrying." >&2
    echo "  full log: $log" >&2
}

# ★★★ A NON-ZERO BUILD THAT PRINTED NO DIAGNOSTIC AT ALL IS ITS OWN CLASS, AND IT
# IS NOT A SOURCE DEFECT. ✔MEASURED 2026-09-05 (P62 lane `dy`): editing a source
# file while ninja was already running in the same tree cost TWO WHOLE BUILDS.
# Each ended rc=1 with a log that stopped mid-compile and named no error -- at the
# exit code, indistinguishable from a real compile failure, so the lane re-ran, and
# re-ran again. Nothing in the tooling prevented it and nothing in the log explained
# it. The I/O classifier above could not see it either: that one keys on a
# diagnostic of a particular SHAPE, and this class prints no diagnostic to key on.
#
# ★★ DEFINED BY THE COMPLEMENT, never by enumerating the shapes a silent failure
# takes -- the whole point is that there is nothing to enumerate. "A diagnostic was
# printed" is the positive vocabulary; its absence is the class. A shape nobody has
# thought of yet therefore counts as SILENT, which is the direction that fails
# toward looking at the log rather than toward trusting it.
# ⚠ THE COLON (or the MSVC code) IS LOAD-BEARING. This repository ships
# `src/core/error/` and `tests/link/test_artifact_withheld_after_error.cpp`, whose
# names appear in an ordinary ninja PROGRESS line on every single build. Matching a
# bare `error` would see one of those and conclude a diagnostic had been printed --
# blinding the guard in exactly the tree it is meant to protect.
local_build_no_diagnostic() {
    ! grep -Eq '(^|[^[:alnum:]_])[Ee]rror:|fatal error|[Ee]rror [A-Z]+[0-9]{4}|undefined reference' "$1"
}

# Which of OUR sources moved while the build was running. This is a MEASUREMENT of
# the named cause, not a guess at it: the marker's mtime is the instant before
# `cmake --build` was invoked, so anything newer changed underneath ninja.
local_build_sources_touched_since() {
    local marker=$1
    [[ -f "$marker" ]] || return 1
    find src tests -type f -newer "$marker" 2>/dev/null | head -20
}

# ⚠ THREE STATES, NOT TWO, AND THE THIRD IS WHY THIS HELPER EXISTS. "no file is
# newer than the marker" and "there is no marker, so nothing was compared" are
# different facts, and collapsing them makes the report assert a RULING-OUT it
# never performed — the instrument answering an adjacent question, failing toward
# clean. ✔This was live for the length of one self-test run: arm 11 caught it.
local_build_touched_verdict() {
    local marker=$1
    [[ -f "$marker" ]] || { echo unchecked; return; }
    if [[ -n "$(local_build_sources_touched_since "$marker")" ]]; then
        echo moved
    else
        echo clean
    fi
}

local_build_report_no_diagnostic() {
    local log=$1 marker=$2
    echo "local-build.sh: FAIL — THE BUILD EXITED NON-ZERO AND PRINTED NO DIAGNOSTIC." >&2
    echo "  No line in the log matches any shape our toolchains emit for an error." >&2
    echo "  ⚠ THIS IS NOT EVIDENCE OF A SOURCE DEFECT. Do NOT start 'fixing' the last" >&2
    echo "    file the log happened to name — that file is where the log STOPPED, which" >&2
    echo "    is not where anything went wrong." >&2
    if grep -q '^FAILED: ' "$log"; then
        echo "  ninja named a failed target, so a command died without saying why:" >&2
        grep '^FAILED: ' "$log" | head -3 | sed 's/^/    /' >&2
    else
        echo "  ninja named NO failed target — the log simply ends. The build process" >&2
        echo "    itself went away (killed, or its input changed underneath it)." >&2
    fi
    case "$(local_build_touched_verdict "$marker")" in
        moved)
            echo "  ★ MEASURED: these source files CHANGED WHILE THIS BUILD WAS RUNNING —" >&2
            echo "    this is the known cause, and re-running the build is the whole remedy:" >&2
            local_build_sources_touched_since "$marker" | sed 's/^/    /' >&2 ;;
        clean)
            echo "  ★ MEASURED: no file under src/ or tests/ changed while the build ran, so" >&2
            echo "    the edit-under-ninja cause is RULED OUT here. Re-run once; if it recurs" >&2
            echo "    at the same point, say so in the row rather than retrying a third time." >&2 ;;
        *)
            echo "  ⚠ NOT MEASURED: no build-start marker, so nothing was compared and the" >&2
            echo "    edit-under-ninja cause is neither shown nor ruled out. This is what an" >&2
            echo "    absent marker means — it is NOT a clean bill of health." >&2 ;;
    esac
    echo "  ⚠ If this happened during a RED-ON-DISABLE arm, that arm measured NOTHING —" >&2
    echo "    a build that never completed is not evidence about your change." >&2
    echo "  full log: $log" >&2
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --test)      run_test=1 ;;
        --configure) configure=1 ;;
        --clean)     clean=1; configure=1 ;;
        --tree)
            [[ $# -ge 2 ]] || { echo "--tree needs a name" >&2; exit 2; }
            tree="$2"; shift ;;
        --build-type)
            [[ $# -ge 2 ]] || { echo "--build-type needs a value" >&2; exit 2; }
            build_type="$2"; shift ;;
        --self-test)
            # ★ THE ARM IS EXERCISED, NOT READ. This project has shipped a suite
            # printing `failed=0` while exiting 2; a classifier nobody drives is
            # the same bet. Both arms run over fixtures, and the POSITIVE arm
            # asserts the MESSAGE, not merely a non-zero exit.
            # ⚠ THE FIXTURE PATHS ARE ASSEMBLED, NOT WRITTEN LITERALLY. A literal
            # `path:line` in this file is indistinguishable from a CITATION to
            # `scripts/check-plan-citations`, whose ratchet only moves DOWN -- and
            # these are compiler OUTPUT SAMPLES, not claims about any file here.
            # The runtime strings are byte-identical to what gcc prints; only the
            # source spelling differs, and this comment is why.
            st_at=':'
            # A lone backslash inside the double-quoted sample below would be
            # eaten before printf sees it; assembling it keeps the runtime string
            # byte-identical to what gcc prints on a Windows temp path.
            bs=''
            st_dir=$(mktemp -d)
            trap 'rm -rf "$st_dir"' EXIT
            st_fail=0
            printf '%s\n' \
                'C:/Strawberry/c/include/c++/13.2.0/bits/locale_facets.tcc: Invalid argument' \
                "In file included from x.cpp${st_at}1:" \
                "error: '__use_cache' is not a class template" > "$st_dir/read.log"
            printf '%s\n' \
                "src/link/linker.cpp${st_at}120:5: error: no member named q" \
                "ninja: build stopped: subcommand failed." > "$st_dir/real.log"
            printf '%s\n' \
                "/usr/include/c++/13/bits/basic_string.h${st_at}1:1: error: expected unqualified-id" \
                > "$st_dir/diag-in-toolchain.log"
            if local_build_toolchain_io_failure "$st_dir/read.log"; then
                echo "self-test arm 1 READ-FAILURE            classified as expected"
            else
                echo "self-test arm 1 READ-FAILURE            NOT classified — the guard is blind" >&2; st_fail=1
            fi
            if local_build_toolchain_io_failure "$st_dir/real.log"; then
                echo "self-test arm 2 REAL-COMPILE-ERROR      misclassified — it would HIDE a real defect" >&2; st_fail=1
            else
                echo "self-test arm 2 REAL-COMPILE-ERROR      left alone as expected"
            fi
            # The discriminating arm: a genuine DIAGNOSTIC about a toolchain header
            # carries file:line:col and must NOT be classified as an I/O failure.
            if local_build_toolchain_io_failure "$st_dir/diag-in-toolchain.log"; then
                echo "self-test arm 3 DIAGNOSTIC-IN-TOOLCHAIN misclassified — path alone is not the signal" >&2; st_fail=1
            else
                echo "self-test arm 3 DIAGNOSTIC-IN-TOOLCHAIN left alone as expected"
            fi
            # Arms 5 and 6 are the WRITE half. ✔MEASURED 2026-08-21: a lane's build
            # died with `fatal error: error writing to C:\...\ccigZdWt.s: Invalid
            # argument` and exited 1, indistinguishable from a real compile error,
            # because every pattern here required an INCLUDE path.
            printf '%s\n' \
                "cc1plus: fatal error: error writing to C:${bs}Users${bs}x${bs}AppData${bs}Local${bs}Temp${bs}ccigZdWt.s: Invalid argument" \
                "compilation terminated." > "$st_dir/write.log"
            if local_build_toolchain_io_failure "$st_dir/write.log"; then
                echo "self-test arm 5 WRITE-FAILURE           classified as expected"
            else
                echo "self-test arm 5 WRITE-FAILURE           NOT classified — the guard is blind to the write half" >&2; st_fail=1
            fi
            # The discriminating negative for the write half: a source file the
            # compiler MENTIONS while writing is still a real defect.
            printf '%s\n' \
                "src/hir/hir_verifier.cpp${st_at}42:7: error: no member named 'writing'" \
                "ninja: build stopped: subcommand failed." > "$st_dir/write-real.log"
            if local_build_toolchain_io_failure "$st_dir/write-real.log"; then
                echo "self-test arm 6 REAL-ERROR-SAYS-WRITING misclassified — it would HIDE a real defect" >&2; st_fail=1
            else
                echo "self-test arm 6 REAL-ERROR-SAYS-WRITING left alone as expected"
            fi
            msg=$(local_build_report_io_failure "$st_dir/read.log" 2>&1)
            case "$msg" in
                *"TOOLCHAIN I/O FAILURE"*"locale_facets.tcc"*"RED-ON-DISABLE"*)
                    echo "self-test arm 4 MESSAGE                 names the shape, the file and the red-on-disable hazard" ;;
                *)  echo "self-test arm 4 MESSAGE                 incomplete: $msg" >&2; st_fail=1 ;;
            esac
            # Arms 7..11 are the SILENT half — a non-zero build that printed no
            # diagnostic at all. Every negative arm here is a log that DOES carry a
            # diagnostic, in a different vendor's spelling, because the failure mode
            # that matters is this classifier swallowing a REAL defect.
            printf '%s\n' \
                "[412/1180] Building CXX object src/hir/CMakeFiles/dss_hir.dir/hir_builder.cpp.obj" \
                "[413/1180] Building CXX object src/mir/CMakeFiles/dss_mir.dir/hir_to_mir.cpp.obj" \
                > "$st_dir/silent.log"
            if local_build_no_diagnostic "$st_dir/silent.log"; then
                echo "self-test arm 7 SILENT-STOP             classified as expected"
            else
                echo "self-test arm 7 SILENT-STOP             NOT classified — the guard is blind to the silent half" >&2; st_fail=1
            fi
            # ★ THE ARM THIS CLASSIFIER EXISTS TO SURVIVE. These two paths ship in
            # this repository and appear in an ordinary progress line on EVERY build;
            # a bare `error` match would read them as a diagnostic and go blind here.
            printf '%s\n' \
                "[7/9] Building CXX object src/core/CMakeFiles/dss_core.dir/error/reporter.cpp.obj" \
                "[8/9] Building CXX object tests/link/CMakeFiles/x.dir/test_artifact_withheld_after_error.cpp.obj" \
                > "$st_dir/silent-errorpath.log"
            if local_build_no_diagnostic "$st_dir/silent-errorpath.log"; then
                echo "self-test arm 8 SILENT-ON-ERROR-PATH    classified as expected"
            else
                echo "self-test arm 8 SILENT-ON-ERROR-PATH    NOT classified — a FILE NAME fooled it" >&2; st_fail=1
            fi
            if local_build_no_diagnostic "$st_dir/real.log"; then
                echo "self-test arm 9 REAL-COMPILE-ERROR      misclassified — it would HIDE a real defect" >&2; st_fail=1
            else
                echo "self-test arm 9 REAL-COMPILE-ERROR      left alone as expected"
            fi
            # Two more vendors, because "a diagnostic was printed" is the positive
            # vocabulary and a gap in it silences a real defect.
            printf '%s\n' \
                "src${st_at}mir${st_at}x.cpp(42): error C2065: 'q': undeclared identifier" \
                > "$st_dir/msvc.log"
            printf '%s\n' \
                "/usr/bin/ld: x.o: in function \`main':" \
                "x.c:(.text+0x9): undefined reference to \`helper'" > "$st_dir/ldundef.log"
            if local_build_no_diagnostic "$st_dir/msvc.log" || local_build_no_diagnostic "$st_dir/ldundef.log"; then
                echo "self-test arm 10 MSVC-AND-LD-DIAGNOSTIC misclassified — a vendor spelling is missing from the positive set" >&2; st_fail=1
            else
                echo "self-test arm 10 MSVC-AND-LD-DIAGNOSTIC left alone as expected"
            fi
            # The MESSAGE arm: the report must name the hazard AND must not claim a
            # cause it has not measured. With no marker file, the mtime probe cannot
            # run, and the text has to say the cause is ruled out rather than assert it.
            msg=$(local_build_report_no_diagnostic "$st_dir/silent.log" "$st_dir/no-such-marker" 2>&1)
            case "$msg" in
                *"PRINTED NO DIAGNOSTIC"*"NOT EVIDENCE OF A SOURCE DEFECT"*"RED-ON-DISABLE"*)
                    echo "self-test arm 11 MESSAGE                names the class, the mis-fix hazard and the red-on-disable hazard" ;;
                *)  echo "self-test arm 11 MESSAGE                incomplete: $msg" >&2; st_fail=1 ;;
            esac
            # ★★ ARMS 12..14 DRIVE ALL THREE VERDICTS, and the third arm is the
            # reason: with a marker ABSENT nothing was compared, and saying so is a
            # different sentence from "nothing changed". Collapsing the two made this
            # reporter assert a ruling-out it had never performed — live until arm 11
            # printed the text and it could be read. An instrument that answers the
            # adjacent question fails toward CLEAN, every time.
            case "$(local_build_touched_verdict "$st_dir/absent-marker")" in
                unchecked) echo "self-test arm 12 VERDICT-UNCHECKED     no marker reported as NOT MEASURED" ;;
                *)  echo "self-test arm 12 VERDICT-UNCHECKED     a missing marker did not report as unchecked" >&2; st_fail=1 ;;
            esac
            printf '' > "$st_dir/marker"
            case "$(local_build_touched_verdict "$st_dir/marker")" in
                clean) echo "self-test arm 13 VERDICT-CLEAN         nothing newer than the marker reported as clean" ;;
                *)  echo "self-test arm 13 VERDICT-CLEAN         a quiet tree did not report as clean" >&2; st_fail=1 ;;
            esac
            # ⚠ THE POSITIVE VERDICT NEEDS A DETERMINISTIC GAP, AND WRITE-ORDER DOES
            # NOT PROVIDE ONE. ✔MEASURED on this host: a marker and a probe written
            # back-to-back get mtimes equal TO THE NANOSECOND (the Windows clock tick
            # is ~15 ms), and `-newer` is strictly-greater, so the arm read `clean`
            # and the guard looked blind when it was not. The marker is back-dated
            # HERE, in the fixture, for that reason alone.
            # ★★ PRODUCTION DELIBERATELY DOES NOT BACK-DATE, and that asymmetry is the
            # point: a marker aged even one second would flag the file the caller had
            # just edited before starting the build — which is the ordinary workflow —
            # so the report would fire on every failed build and mean nothing. The
            # real cost of strict comparison is a ~15 ms tie window at the very start
            # of a build that runs for minutes; a tie there loses a diagnosis, it
            # never invents one.
            st_probe="tests/.local-build-selftest-probe"
            printf '' > "$st_dir/marker"
            # `-t CCYYMMDDhhmm` is the POSIX spelling and BSD/macOS `touch` accepts
            # it; `-d <string>` is a GNU extension and would fail on the darwin leg.
            # An assertion measured on one leg is a portability claim.
            touch -t 200001010000 "$st_dir/marker"
            printf 'probe\n' > "$st_probe"
            case "$(local_build_touched_verdict "$st_dir/marker")" in
                moved) echo "self-test arm 14 VERDICT-MOVED         a file changed after the marker was seen" ;;
                *)  echo "self-test arm 14 VERDICT-MOVED         a CHANGED FILE WAS MISSED — the measurement is blind" >&2; st_fail=1 ;;
            esac
            rm -f "$st_probe"
            # NOT `[[ ... ]] && echo` — under the `set -e` in force here a false
            # test would abort the script before `exit "$st_fail"` runs. Same exit
            # code either way, but the reader could not tell which path it took.
            if [[ $st_fail -eq 0 ]]; then
                echo "local-build: self-test OK — 14 arms, both directions exercised."
            fi
            exit "$st_fail" ;;
        -h|--help)
            # ★★ THE HELP IS THE HEADER BLOCK ITSELF, EXTRACTED BY SHAPE. It used to be
            # a hard line range, and on 2026-08-19 a `# PURPOSE:` line inserted at the top
            # shifted every line under it -- the window then ran past the header into the
            # argument parser, and re-tuning the number would only have rearmed the same
            # trap for the next edit. A range coupled to line numbers is a claim about
            # the file that nothing rechecks.
            awk 'NR==1 && /^#!/ {next} /^# PURPOSE:/ {next} /^#/ || /^[[:space:]]*$/ {print; next} {exit}' "$0"; exit 0 ;;
        *) echo "unknown flag: $1" >&2; exit 2 ;;
    esac
    shift
done

# ── The tree name must resolve to a DIRECTLY-NESTED child of build/ ──────────
# Everything destructive below is keyed on this, so it is validated by SHAPE
# rather than by trusting the caller: no separators, no `..`, non-empty. A name
# like `../src` or `dbg/../..` would otherwise walk the delete out of the
# container, which is the precise failure this rewrite exists to prevent.
case "$tree" in
    ""|*/*|*\\*|.|..) echo "refusing tree name '$tree': it must be a single directory name directly under build/" >&2; exit 3 ;;
esac
BUILD_DIR="build/$tree"

if [[ "$clean" == 1 ]]; then
    # Belt and braces: the shape check above already guarantees this, and the
    # guarantee is cheap to restate at the one call site that cannot be undone.
    if [[ "$BUILD_DIR" != build/* || "$BUILD_DIR" == "build/" ]]; then
        echo "refusing to remove '$BUILD_DIR': --clean only ever removes ONE tree under build/" >&2
        exit 3
    fi
    rm -rf "$BUILD_DIR"
fi

# ── --build-type is meaningful ONLY when this run configures a tree from scratch ──
# ⚠ THIS CHECK USED TO LIVE INSIDE THE CONFIGURE BLOCK, AND THAT MADE IT A NO-OP on
# the commonest path: with `build.ninja` already present and no --configure, the
# whole block was skipped, so `--build-type Release` on the debug tree exited 0
# having silently ignored the flag. ✔MEASURED 2026-08-20 by EXERCISING the arm
# rather than reading it. It runs after --clean on purpose: a cleaned tree has no
# cache, so the flag IS meaningful there.
if [[ -n "$build_type" && -f "$BUILD_DIR/CMakeCache.txt" ]]; then
    echo "refusing --build-type on the EXISTING tree 'build/$tree': its cache already declares one." >&2
    echo "  Re-run with --clean to reconfigure it from scratch." >&2
    exit 3
fi

if [[ "$configure" == 1 || ! -f "$BUILD_DIR/build.ninja" ]]; then
    args=(-S . -B "$BUILD_DIR" -G Ninja)
    if [[ ! -f "$BUILD_DIR/CMakeCache.txt" ]]; then
        # First configure of this tree: the build type must be KNOWN, not guessed.
        # An existing tree keeps whatever its cache already says, so no type is
        # passed and a caller cannot silently flip a configured tree underneath
        # the artifacts already in it.
        [[ -n "$build_type" ]] || build_type="$(default_build_type_for "$tree")"
        if [[ -z "$build_type" ]]; then
            echo "refusing to configure a NEW tree 'build/$tree' with no build type: pass --build-type <Debug|Release|...>" >&2
            echo "  (only 'dbg' -> Debug and 'rel' -> Release are established names in this repository)" >&2
            exit 3
        fi
        args+=("-DCMAKE_BUILD_TYPE=$build_type")
    fi
    cmake "${args[@]}"
fi

# `tee` so the console still streams while a copy stays scannable; the build's own
# status is read from PIPESTATUS, never from `$?` (which reports the LAST stage --
# ✔the same mis-read that made a gate-arm test look like it passed this cycle).
# ⚠ `set -e` is in force from the top of this file, so the pipeline must be run
# with it OFF or the script aborts BEFORE the classifier can name the failure --
# which would leave exactly the misattribution this code exists to prevent.
# `pipefail` is left alone: it is set globally and the rest of the script relies on it.
build_log="$BUILD_DIR/.local-build-last.log"
# Stamped the instant BEFORE the build starts, so "newer than this" is exactly
# "changed while ninja was running". Written unconditionally: a marker left over
# from a previous run would date the wrong build and turn the one measurement this
# reporter makes into a fabrication.
build_marker="$BUILD_DIR/.local-build-started"
printf '' > "$build_marker"
set +e
# ★★★ OPERATOR RULING 2026-08-25: "never use all CPUS, the idea is to keep build + tests + run always at 4 cpus", AMENDED same-day to "make it 6 cores, not 4, everywhere".
# ⚠ A BARE `cmake --build` HANDS OFF TO NINJA, WHOSE DEFAULT IS ALL CORES. This site
# was wide open while every discussion of parallelism was about ctest.
cmake --build "$BUILD_DIR" --parallel "${DSS_JOBS:-6}" 2>&1 | tee "$build_log"
build_rc=${PIPESTATUS[0]}
set -e
if [[ $build_rc -ne 0 ]] && local_build_toolchain_io_failure "$build_log"; then
    local_build_report_io_failure "$build_log"
    exit 9
fi
# ⚠ ORDER IS DELIBERATE: the I/O classifier runs FIRST because an I/O failure DOES
# print a diagnostic (`fatal error: error writing to ...`), so it is a strictly more
# specific class. Reaching here means the build failed having printed nothing any
# toolchain would call an error. Its OWN exit code, distinct from both 9 and a real
# compiler's rc, so a caller can tell "nothing was measured" from "your code is wrong".
if [[ $build_rc -ne 0 ]] && local_build_no_diagnostic "$build_log"; then
    local_build_report_no_diagnostic "$build_log" "$build_marker"
    exit 10
fi
[[ $build_rc -eq 0 ]] || exit "$build_rc"

if [[ "$run_test" == 1 ]]; then
    # ★★★ OPERATOR RULING 2026-08-25: "never use all CPUS, the idea is to keep build + tests + run always at 4 cpus", AMENDED same-day to "make it 6 cores, not 4, everywhere".
    # Set CTEST_PARALLEL_LEVEL in the environment, or pass -j, to override.
    : "${CTEST_PARALLEL_LEVEL:=6}"
    export CTEST_PARALLEL_LEVEL
    (cd "$BUILD_DIR" && ctest --output-on-failure)
fi
