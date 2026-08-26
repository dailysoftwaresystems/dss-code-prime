#!/usr/bin/env bash
# PURPOSE: run a DSS gate leg inside WSL -- sync the Windows checkout, build clean, and run ctest through run-gate.
#
# ★★★ WHY THIS IS A SCRIPT AND NOT A PIPELINE SOMEONE RETYPES. The three-leg gate
# runs a WSL x86_64 leg before every commit, and until 2026-08-19 the way to run
# it was to type an rsync, a cmake, and a ctest into a scratch file each time.
# ✔MEASURED that cycle: THREE near-identical scratch scripts were written in a
# single session (a full leg, a guards-only pass, an arm64-strict pass), each one
# re-deriving the same excludes and the same build layout. They are the same tool
# at three scopes, which is what the `--mode` flag below is.
#
# ⚠ AND EACH RETYPING RE-OPENS THE SAME EDGE CASES, all of them measured in this
# repository's own record:
#   * an UNANCHORED rsync exclude (`build*` rather than `/build*`) once silently
#     skipped `src/program/build_scripts.cpp`, and a gate leg was configured
#     against a tree missing a changed `.cpp`;
#   * `rsync -a` PRESERVES MTIMES, so a /mnt/c source whose mtime lands behind an
#     existing build output makes ninja skip the changed file
#     (D-SYNC-RSYNC-PRESERVED-MTIME-DEFEATS-THE-REBUILD). This script builds
#     CLEAN rather than hoping around that;
#   * a `wsl.exe bash -c` carrying a variable once expanded to
#     `rsync -a --delete / /`, ran for 70 minutes, and reported exit 0. Nothing
#     here interpolates into a remote `-c` string.
#
# ★ WHY THERE IS NO `.ps1` SIBLING, deliberately. This script's body runs INSIDE
# a WSL distro, where PowerShell is not the shell; a `.ps1` twin would have
# nothing to be a twin OF. The Windows-side half is a single invocation with no
# logic in it:
#
#     wsl.exe -e bash scripts/wsl-leg/wsl-leg.sh [--mode <m>] [-R <regex>]
#
# (`wsl.exe -e`, never `wsl.exe bash -c` — see the third edge case above.)
#
# MODES
#   full      (default) rsync -> CLEAN configure+build -> full ctest via run-gate
#   guards    run the repo guards over the WINDOWS checkout at /mnt/c, no sync,
#             no build. Cheap, and it is what proves a `.sh` guard's repo-root
#             derivation works under a real POSIX shell.
#   arm-strict  CLEAN configure+build with -DDSS_STRICT_ARM_VERDICTS=ON into
#             build/arm-strict, then ctest over `-R` (required in this mode:
#             the whole point is a SCOPED run, since strict verdicts emulate
#             every x86_64 example one at a time).
#   build     rsync -> CLEAN configure+build into build/<--tree> at
#             --build-type, and STOP. No ctest, and it says so on the way out.
#             For a BENCHMARK leg, which needs a binary on this host and whose
#             reading is only valid on a QUIET host -- so running a suite in the
#             same invocation would corrupt the measurement the build was for.
#             It is also the only mode that can produce a RELEASE driver here.
#
# FLAGS
#   --tree <name>        build/<name>; `build` mode only (default: bench)
#   --build-type <T>     Debug|Release|...; not read by `guards` (default: Debug)
#   -R <regex>           ctest filter; required by arm-strict, not read by
#                        `guards` or `build`
#   --src / --dst        override either end of the sync; `--dst` is not read by
#                        `guards`
#
# ⚠ "NOT READ BY <mode>" MEANS REFUSED, NOT IGNORED. Passing a flag the selected
# mode never reads exits non-zero and says which mode and why. Until 2026-08-25 each
# of those was silently dropped -- `--mode full --tree bench` built and tested
# `build/dbg` and reported OK, which is the unknown-flag failure the argument loop
# already refuses, wearing a known flag's name.
#
# ⚠ ONE LEG PER `--dst` AT A TIME. The sync is `rsync --delete` over the whole
# destination tree, so a second leg started against the same `--dst` would rewrite
# the tree a live one is testing. A lock under `<dst>/build/` makes that a REFUSAL
# (exit 4) rather than a pair of unattributable verdicts; a lane that wants to run
# concurrently passes its own `--dst`.
#
# Usage:
#   wsl.exe -e bash scripts/wsl-leg/wsl-leg.sh
#   wsl.exe -e bash scripts/wsl-leg/wsl-leg.sh --mode guards
#   wsl.exe -e bash scripts/wsl-leg/wsl-leg.sh --mode arm-strict -R 'harness/.*'
set -uo pipefail

MODE="full"
FILTER=""
# Defaults chosen so every pre-existing invocation behaves EXACTLY as before:
# `full` and `arm-strict` set their own tree and have always built Debug.
TREE="bench"
BUILD_TYPE="Debug"
SRC="${DSS_WIN_CHECKOUT:-/mnt/c/Source/DailySoftware/dss-code-prime}"
DST="${DSS_WSL_CHECKOUT:-$HOME/src/dss-code-prime}"

die() { printf '\n[X] wsl-leg: %s\n' "$*" >&2; exit 1; }
say() { printf '\n=== %s ===\n' "$*"; }

# Which flags the CALLER actually typed, as opposed to which ones hold a default.
# The refusal below can only be honest about "this mode never reads that" if it can
# tell a supplied value from an inherited one.
GAVE_TREE=0; GAVE_BUILD_TYPE=0; GAVE_FILTER=0; GAVE_DST=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --mode)   MODE="${2:?--mode needs a value}"; shift 2 ;;
        -R)       FILTER="${2:?-R needs a value}"; GAVE_FILTER=1; shift 2 ;;
        --tree)       TREE="${2:?--tree needs a value}"; GAVE_TREE=1; shift 2 ;;
        --build-type) BUILD_TYPE="${2:?--build-type needs a value}"; GAVE_BUILD_TYPE=1; shift 2 ;;
        --src)    SRC="${2:?}"; shift 2 ;;
        --dst)    DST="${2:?}"; GAVE_DST=1; shift 2 ;;
        -h|--help)
            awk 'NR==1 && /^#!/ {next} /^# PURPOSE:/ {next} /^#/ || /^[[:space:]]*$/ {print; next} {exit}' "$0"
            exit 0 ;;
        # An unknown flag is a REFUSAL, never a shrug: silently ignoring one is
        # how a leg runs a different thing than the operator asked for and still
        # reports green.
        *) die "unknown argument '$1' (try --help)" ;;
    esac
done

# An unknown MODE is a REFUSAL for the same reason an unknown FLAG is: until
# 2026-08-25 anything unrecognised fell through to the `full` branch, so a typo
# ran a full clean build and a full ctest while the operator believed a narrow
# mode had run -- green, and answering a different question than the one asked.
case "$MODE" in
    full|guards|arm-strict|build) ;;
    *) die "unknown --mode '$MODE' (expected: full, guards, arm-strict, build)" ;;
esac

# ── A KNOWN FLAG THE SELECTED MODE NEVER READS IS ALSO A REFUSAL ────────────
# ★★★ THE ARGUMENT LOOP ABOVE ALREADY STATES THE PRINCIPLE, ABOUT THE UNKNOWN FLAG:
# "silently ignoring one is how a leg runs a different thing than the operator asked
# for and still reports green." A KNOWN flag that THIS MODE never reads is the
# IDENTICAL failure with a better spelling, and it was the wider hole of the two --
# nothing is unknown, nothing is refused, and the operator is answered about a
# different thing than they asked about.
# ✔MEASURED 2026-08-25 (cycle P36) by RUNNING the pre-edit file: `--mode full --tree
# bench` announced and configured **build/dbg** -- the named tree silently dropped,
# because `BUILD` is hardcoded per mode below and `$TREE` is read only by `build`.
# `--mode build -R 'harness/.*'` was accepted without a word and configured
# build/bench, and this mode returns before the ctest block below ever runs, so the
# filter could not have scoped anything. A `-R` is typed precisely to narrow a run.
# ★ REFUSED RATHER THAN HONOURED for `--tree`: `full` and `arm-strict` have FIXED
# build directories on purpose (`build/dbg`, `build/arm-strict`) and every plan that
# cites a WSL gate result cites those names, so making them configurable would change
# what the pre-commit gate means. Refusing costs an operator one retype; honouring
# would cost the next reader the ability to trust a cited tree name.
refuse_inert() {   # <flag> <why this mode cannot honour it>
    die "--mode $MODE never reads '$1' ($2). Refused rather than ignored: a flag that is silently dropped is how a leg runs a different thing than the operator asked for and still reports green."
}
if [[ $GAVE_TREE -eq 1 && "$MODE" != "build" ]]; then
    refuse_inert '--tree' "this mode's build directory is fixed -- full builds build/dbg, arm-strict builds build/arm-strict, guards builds nothing"
fi
if [[ $GAVE_BUILD_TYPE -eq 1 && "$MODE" == "guards" ]]; then
    refuse_inert '--build-type' 'guards runs no configure and no build'
fi
if [[ $GAVE_FILTER -eq 1 && ( "$MODE" == "guards" || "$MODE" == "build" ) ]]; then
    refuse_inert '-R' 'this mode runs no ctest, so there is nothing for a filter to scope'
fi
if [[ $GAVE_DST -eq 1 && "$MODE" == "guards" ]]; then
    refuse_inert '--dst' 'guards runs over the Windows checkout at $SRC and never syncs'
fi
# ★ THE MIRROR CASE -- A MANDATORY ARGUMENT THAT IS MISSING -- IS REFUSED HERE TOO,
# AND IT USED TO BE REFUSED FORTY-ODD SECONDS LATE. The wording is unchanged; only the
# POSITION moved, from where `BUILD` is chosen (after a full `rsync --delete` of the
# tree and after the lock is taken) to here, beside the other argument checks. An
# argument error is knowable before the first byte moves, and a refusal that arrives
# after the expensive part has run teaches an operator to distrust the cheap part.
if [[ "$MODE" == "arm-strict" && -z "$FILTER" ]]; then
    die "--mode arm-strict requires -R <regex>: strict verdicts emulate every x86_64 example one at a time, so an unscoped run costs hours"
fi

[[ -d "$SRC" ]] || die "the Windows checkout is not visible at $SRC"

# The arm64 leg's examples need the sysroot prefix or ~450 of them false-red at
# exit 255. Exported for every mode: harmless where nothing reads it.
export QEMU_LD_PREFIX="${QEMU_LD_PREFIX:-/usr/aarch64-linux-gnu}"

# ── guards: no sync, no build, straight over the Windows checkout ────────────
if [[ "$MODE" == "guards" ]]; then
    cd "$SRC" || die "cannot enter $SRC"
    say "repo guards under a real POSIX shell ($(pwd))"
    rc=0
    for g in check-anchor-registry check-line-endings check-orphan-tests; do
        printf -- '--- %s\n' "$g"
        bash "scripts/$g/$g.sh" 2>&1 | tail -2
        s=${PIPESTATUS[0]}; [[ $s -eq 0 ]] || { rc=$s; echo "    RED rc=$s"; }
    done
    # ⚠ THIS LIST IS HAND-KEPT, AND A GUARD MISSING FROM IT IS A GUARD THIS MODE
    # CANNOT RUN. `check-shell-portability` was added 2026-08-22 and belongs here
    # more than most: its whole subject is what a POSIX shell does differently, and
    # this mode is the one that runs the guards under a real POSIX shell.
    for g in check-scripts-index check-plan-citations check-anchor-balance check-shell-portability; do
        printf -- '--- %s\n' "$g"
        python3 "scripts/$g/$g.py" 2>&1 | tail -2
        s=${PIPESTATUS[0]}; [[ $s -eq 0 ]] || { rc=$s; echo "    RED rc=$s"; }
    done
    [[ $rc -eq 0 ]] || die "a guard went red (rc=$rc)"
    say "guards OK"
    exit 0
fi

# ── sync ────────────────────────────────────────────────────────────────────
say "rsync $SRC -> $DST (excludes ANCHORED)"
mkdir -p "$DST" || die "cannot create $DST"

# ── MUTUAL EXCLUSION ON THE DESTINATION TREE ────────────────────────────────
# ★★★ THE macOS CARRIAGE HAS REFUSED THIS SINCE P34 AND THIS ONE NEVER HAS, WHICH IS
# THE WRONG WAY ROUND. ✔The measurement that bought macOS its lock
# (D-SCRIPT-MACOS-LEG-WITNESS-CAN-BE-ANOTHER-RUN-S-EXIT-CODE) was two legs on one
# host: the second one's `rm -rf build/dbg` ran underneath the first one's LIVE
# ctest, which then spent 2 h 34 m walking a tree being deleted and rebuilt under it,
# and NEITHER verdict was attributable afterwards. This repository runs up to FOUR
# PARALLEL LANES by standing order and this is the leg that runs before every commit,
# so the collision is likelier here, not rarer.
# ⚠ AND THE BLAST RADIUS IS WIDER ON THIS CARRIAGE: the `rsync --delete` below
# rewrites the whole SOURCE TREE, not just a build directory. A `build`-mode leg into
# build/bench and a `full` leg into build/dbg have different build trees and still
# destroy each other through the sync -- so the lock is on `$DST`, the shared thing,
# and not on `$BUILD`.
# ★ The lock lives under `build/`, which is EXCLUDED from the rsync above, so a sync
# cannot delete the lock that is protecting it from that sync.
# ★ A lock whose owning pid is GONE describes nothing and is taken, so a killed leg
# cannot wedge the carriage -- but it says so rather than reclaiming it silently.
mkdir -p "$DST/build" || die "cannot create $DST/build"
LOCK="$DST/build/.wsl-leg.lock"
if [[ -e "$LOCK" ]]; then
    LOCK_OWNER=$(sed -n 's/^pid=//p' "$LOCK" | head -1)
    if [[ -n "$LOCK_OWNER" ]] && kill -0 "$LOCK_OWNER" 2>/dev/null; then
        printf '\n[X] wsl-leg: another WSL leg owns %s\n' "$DST" >&2
        printf '    owner pid=%s run=%s mode=%s\n' "$LOCK_OWNER" \
            "$(sed -n 's/^run=//p' "$LOCK" | head -1)" \
            "$(sed -n 's/^mode=//p' "$LOCK" | head -1)" >&2
        printf '    Refusing -- starting here would rsync --delete over a live leg and rm -rf its build tree.\n' >&2
        printf '    Use --dst <other-dir> for a lane of your own, or wait for that run to finish.\n' >&2
        exit 4
    fi
    printf '! stale lock (pid %s is gone) -- taking it\n' "${LOCK_OWNER:-?}"
    rm -f "$LOCK"
fi
# ★ pid alone is not an identity -- pids are reused. The run token is what a reader
# matches against a transcript.
LEG_RUN="$$-$(date +%s)"
printf 'pid=%s\nrun=%s\nmode=%s\n' "$$" "$LEG_RUN" "$MODE" > "$LOCK" \
    || die "cannot write the leg lock at $LOCK"
# Armed only AFTER the lock is ours: a trap set earlier would delete somebody else's.
trap 'rm -f "$LOCK"' EXIT INT TERM
printf 'lock : %s (run %s)\n' "$LOCK" "$LEG_RUN"

# ★★★ AGENT WORKTREES NEVER TRAVEL TO A GATE HOST. Operator ruling 2026-08-25
# (cycle P34) required this on BOTH carriages and the macOS/VPS side got it; THIS
# carriage was missed, and the omission was ✔MEASURED on 2026-08-25 (cycle P35)
# before it was fixed: this distro held **9,661 worktree files out of 34,831**,
# i.e. 28% of the tree under test was somebody's uncommitted lane. A worktree
# carries its own `examples/` corpus and its own `src/`, so a gate host holding
# one is not a mirror of the tree the leg reports on.
# D-SCRIPT-WSL-LEG-RSYNCS-AGENT-WORKTREES-ONTO-THE-GATE-HOST
# ⚠ rsync does NOT delete an EXCLUDED path, so adding this line does not clean a
# distro that already holds one -- that needs an explicit removal, once.
rsync -a --delete \
    --exclude='/build' --exclude='/build-*' --exclude='/target' \
    --exclude='/.dss-deps' --exclude='/scratchpad' --exclude='/Testing' \
    --exclude='/test-scratch' --exclude='/.claude/worktrees' \
    "$SRC/" "$DST/" || die "rsync failed"
cd "$DST" || die "cannot enter $DST"
# ── THE ATTRIBUTION LINES, AND WHY THEY MAY NOT INVENT A CLEAN TREE ─────────
# ★★ `git status --porcelain 2>/dev/null | wc -l` COUNTS ZERO WHEN GIT FAILED, so a
# distro with no git, an unreadable index, or a `--dst` that is not a repository all
# printed `dirty: 0 path(s)` -- byte-identical to the reading a genuinely pristine
# checkout produces. These two lines are this leg's ONLY record of WHICH tree it
# measured, and a record whose failure mode is indistinguishable from the good news
# is not a record. (`head:` had the milder form of the same bug: `(no git)` is at
# least visibly not a commit, but it read as a note rather than as a defect.)
# ⓘ Reported, NOT fatal: a tree that cannot name its HEAD can still be built and
# tested, and the leg's verdict about the SOURCE is still worth having. What must not
# happen is the leg claiming an attribution it does not have.
_head=$(git log --oneline -1 2>/dev/null) \
    || _head='UNKNOWN -- git could not name a HEAD here, so this leg cannot say which commit it measured'
[[ -n "$_head" ]] || _head='UNKNOWN -- git named no HEAD here'
printf 'head : %s\n' "$_head"
_status=$(git status --porcelain 2>/dev/null)
_status_rc=$?
if [[ $_status_rc -ne 0 ]]; then
    printf 'dirty: UNKNOWN -- git status exited %s, so a clean tree and a broken git read alike\n' "$_status_rc"
elif [[ -z "$_status" ]]; then
    printf 'dirty: 0 path(s)\n'
else
    printf 'dirty: %s path(s)\n' "$(printf '%s\n' "$_status" | wc -l)"
fi

# ── build ───────────────────────────────────────────────────────────────────
# CLEAN, always. See the mtime note in the header: an incremental build over an
# rsynced tree can silently skip the very file the leg exists to exercise.
if [[ "$MODE" == "arm-strict" ]]; then
    BUILD="build/arm-strict"
    CONFIGURE_EXTRA="-DDSS_STRICT_ARM_VERDICTS=ON"
    # (the `-R` requirement is enforced with the other argument checks, above)
elif [[ "$MODE" == "build" ]]; then
    BUILD="build/$TREE"
    CONFIGURE_EXTRA=""
else
    BUILD="build/dbg"
    CONFIGURE_EXTRA=""
fi

# ★★ ccache — THE CLEAN BUILD IS CORRECT AND ONLY ITS COST WAS EVER THE
# PROBLEM. The `rm -rf "$BUILD"` below stays (see the mtime note in the header);
# ccache removes the cost WITHOUT trusting an mtime, because it keys on CONTENT
# and its object cache survives the wipe. Inert when absent, and it INSTALLS
# NOTHING -- it prints the one line to type.
# D-SCRIPT-REMOTE-LEG-REBUILT-FROM-SCRATCH-WITH-AN-INSTALLED-CCACHE-UNUSED
CACHE_ARGS=""
CCACHE=$(command -v ccache 2>/dev/null || true)
if [ -n "$CCACHE" ]; then
    printf 'ccache : %s\n' "$CCACHE"
    CACHE_ARGS="-DCMAKE_C_COMPILER_LAUNCHER=$CCACHE"
    CACHE_ARGS="$CACHE_ARGS -DCMAKE_CXX_COMPILER_LAUNCHER=$CCACHE"
else
    printf 'ccache : ABSENT -- this leg recompiles every TU from scratch.\n'
    printf '         One line in WSL:  sudo apt-get install -y ccache\n'
fi

say "clean configure + build ($BUILD, $BUILD_TYPE${CONFIGURE_EXTRA:+, $CONFIGURE_EXTRA})"
rm -rf "$BUILD"
# ★★ AND THE WIPE IS CHECKED, BECAUSE "CLEAN" IS THE ONLY THING THIS BUILD PROMISES.
# `set -e` is deliberately OFF in this file, so a `rm -rf` that failed (a busy
# directory, a permission, a mount gone read-only) fell straight through to a
# configure over the SURVIVING tree, and the leg then ran an INCREMENTAL build under
# a heading that says CLEAN -- which is exactly the preserved-mtime class the header
# spends ten lines refusing to hope around. The postcondition is the DIRECTORY, not
# the rc: `rm -rf` can also be defeated by something recreating the path.
if [[ -e "$BUILD" ]]; then
    die "could not remove $BUILD, so this build would be INCREMENTAL under a heading that says CLEAN (see D-SYNC-RSYNC-PRESERVED-MTIME-DEFEATS-THE-REBUILD in the header)"
fi
cmake -S . -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE="$BUILD_TYPE" -DDSS_BUILD_TESTS=ON \
      $CONFIGURE_EXTRA $CACHE_ARGS > /tmp/wsl-leg-configure.log 2>&1 \
    || { tail -25 /tmp/wsl-leg-configure.log; die "configure failed"; }
# ★★★ OPERATOR RULING 2026-08-25: "never use all CPUS, the idea is to keep build + tests + run always at 4 cpus", AMENDED same-day to "make it 6 cores, not 4, everywhere".  (a bare `cmake --build` means ninja's all-cores default)
cmake --build "$BUILD" --parallel "${DSS_JOBS:-6}" > /tmp/wsl-leg-build.log 2>&1 \
    || { tail -30 /tmp/wsl-leg-build.log; die "build failed"; }
printf 'build: %s\n' "$(tail -1 /tmp/wsl-leg-build.log)"

# ── build: STOP HERE, deliberately ──────────────────────────────────────────
# A mode that syncs and builds and does NOT test. It exists because a
# BENCHMARK leg needs a compiler binary on this host and must not pay for a
# full ctest to get one -- and because the reading it then takes is only valid
# on a QUIET host, so bundling a suite into the same invocation would corrupt
# the very measurement the build was for. It is also the only mode that can
# produce a RELEASE binary here: every other mode is Debug by design, because
# the gate wants assertions on and a benchmark wants them off.
if [[ "$MODE" == "build" ]]; then
    # ★★★ THE DRIVER IS THE WHOLE DELIVERABLE OF THIS MODE, SO ITS ABSENCE IS A
    # FAILURE AND NOT A FOOTNOTE. Until 2026-08-25 this block PRINTED the sentence
    # "NOT FOUND -- the build reported success but produced no driver" and then said
    # `WSL build OK` and exited **0**. Its macOS twin, added in the SAME COMMIT for
    # the SAME anchor pair, has always `exit 1`d on exactly this condition
    # (`macos-leg.sh`, the `LEG_MODE=build` block) -- and the lax half was the one
    # that runs on the host used before every commit.
    # ★★ WHAT AN `exit 0` HERE COSTS, which is more than a missing line of output:
    # this mode exists to hand a RELEASE driver to a benchmark, so a caller that
    # believes it succeeded either measures a STALE binary left by an earlier run
    # -- reporting a number about a tree this leg did not build -- or dies much
    # later with a message about something else entirely. A missing witness and a
    # failed build must not look alike.
    # D-SCRIPT-WSL-LEG-BUILD-MODE-REPORTS-OK-AFTER-PRINTING-THAT-IT-BUILT-NO-DRIVER
    DRIVER="$BUILD/bin/dss/dsscp"
    if [[ ! -x "$DRIVER" ]]; then
        printf 'binary: NOT FOUND at %s\n' "$DRIVER"
        die "the build reported success but produced no driver at $DRIVER -- this mode's only deliverable is that binary, so there is nothing to hand a benchmark"
    fi
    ls -la "$DRIVER"
    say "WSL build OK ($BUILD, $BUILD_TYPE) -- no tests were run, and this mode never claims otherwise"
    exit 0
fi

# ── test, through run-gate so a silent no-run cannot report success ──────────
# ★★ THE REPO GUARDS ARE SKIPPED, by operator ruling 2026-08-25: WSL is an INDIRECT leg.
# They check the SOURCE TREE and this tree was rsynced FROM the root host, which already
# checked it -- ✔MEASURED 18 entries / 159.1 s of pure repetition. `DSS_LEG_GUARDS=1`
# restores them.
# ⓘ WHAT THIS COSTS, said out loud: CMakeLists dispatches on WIN32, so the root host runs
# the `.ps1` guards and this leg used to be the only place the `.sh` twins ran. They are now
# exercised in CI only. The twins HAVE silently diverged before.
GUARD_SKIP=""
[[ "${DSS_LEG_GUARDS:-0}" == "1" ]] || GUARD_SKIP="-LE repo-guard"
say "ctest${FILTER:+ (-R $FILTER)}${GUARD_SKIP:+ (guards skipped)}"
# shellcheck disable=SC2086
if [[ -n "$FILTER" ]]; then
    bash scripts/run-gate/run-gate.sh /tmp/wsl-leg-ctest.log 'tests passed' \
        ctest --test-dir "$BUILD" --output-on-failure -R "$FILTER" $GUARD_SKIP
else
    bash scripts/run-gate/run-gate.sh /tmp/wsl-leg-ctest.log '100% tests passed' \
        ctest --test-dir "$BUILD" --output-on-failure $GUARD_SKIP
fi
rc=$?
grep -E "tests passed|tests failed|The following tests FAILED" -A20 /tmp/wsl-leg-ctest.log | tail -25
[[ $rc -eq 0 ]] || die "ctest leg failed (rc=$rc, log /tmp/wsl-leg-ctest.log)"
say "WSL leg OK"
