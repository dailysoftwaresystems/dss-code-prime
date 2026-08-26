#!/usr/bin/env bash
# PURPOSE: run a DSS gate leg on a physical remote host -- push the working tree over a carriage, build clean, and run ctest through run-gate.
#
# ★★★ WHY THIS IS A SCRIPT AND NOT A PIPELINE SOMEONE RETYPES. `wsl-leg.sh` already
# does exactly this shape for ONE carriage, and cycle P23 needed the same shape for
# TWO more (macOS on real Apple Silicon, and the native aarch64 VPS). Typing the
# rsync + clean configure + build + ctest inline, twice, re-opens every edge case
# these scripts exist to hold -- and two of them are documented IN the carriages this
# script calls:
#   * ssh JOINS THE REMAINING ARGUMENTS WITH SPACES and hands the result to the
#     remote shell, so local quoting is GONE on arrival. ✔MEASURED 2026-08-20 on the
#     operator's Mac: `ssh-macos.sh sh -c 'mkdir -p /tmp/x && …'` arrived as
#     `sh -c mkdir -p /tmp/x && …` and the remote `mkdir` printed its usage line -- a
#     failure that looks like a broken carriage and is not one. Every remote command
#     below is therefore built as ONE argument.
#   * a `tar czf - . | <carriage> 'tar xzf -'` transport DIED on that same host
#     because its login profile CONSUMES STDIN. Both carriages now expose
#     `--rsync <args...> <dest>`, which `exec`s rsync directly so `$?` is rsync's own
#     status. This script uses that and never stdin.
#
# ★★★ THE LEG HOST KEEPS A CLONE. Operator ruling 2026-08-26, and it REPLACES the
# arrangement this header used to describe rather than extending it.
# `.git` is still not SYNCED -- ✔MEASURED 2026-08-21 it is **1.4 GB** against a
# **122 MB** working tree, not a sane payload over ssh -- but the remote no longer
# has to live with whatever `.git` it happens to hold. Before the push,
# `leg_tree_prepare` fetches on the host, puts its clone on the DRIVER's branch at
# the DRIVER's commit, and cleans it; after the leg, `leg_tree_restore` returns it
# to pristine. So the remote `git log` now describes the tree under test, and
# `git status` there shows what `git status` shows here.
# ⚠ WHAT THIS RETIRES, stated because the old sentence is still quoted elsewhere:
# *"read the stamp, never the remote `git log`"* was correct when the remote sat at
# `b52784a` / Cycle P5c with 2,501 dirty files. It is no longer the arrangement.
# `.dss-leg-stamp` is still written -- it names the mode and the run, which git
# cannot -- but it is a RECORD now, not a workaround for a lying history.
# See D-HARNESS-ARM64-VPS-CHECKOUT-IS-STALE-AND-ITS-PREBUILT-COMPILER-REFUSES-ITS-OWN-CONFIG.
#
# ★★ AND THE LEG CLEANS UP AFTER ITSELF, which is the other half of the ruling. A
# sync leaves the clone dirty by construction; left that way the next leg starts from
# a tree nobody described. Restore runs on EVERY exit path, `die` included, and
# `git worktree prune` goes with it -- ✔MEASURED 2026-08-26, both remote hosts carried
# a registered, `prunable` `dss-probe-6f4aab73` worktree from a cycle that never
# cleaned up, and it survived a MANUAL cleanup because a stale worktree registration
# lives in `.git/worktrees/` and never appears in `git status`.
#
# ★ WHAT IS WITHHELD IS DERIVED, NOT TYPED (`scripts/carriage-excludes/`). `.secrets/`,
# `/.claude/worktrees` and every build tree are withheld because GIT IGNORES THEM, at
# any depth -- which is the fix for the four hand-written lists that each missed
# something different. `.git` is the one remaining POLICY withhold and is passed with
# `--also`. ⚠ The security property the old list asserted by hand still holds and is
# now structural: `.secrets/` holds the private keys for these very carriages, and
# ✔MEASURED 2026-08-26 both hosts were holding BOTH keys before the derivation landed.
#
# ★★ RUN THIS FROM WSL, FOR EITHER CARRIAGE. ✔MEASURED 2026-08-21, and it corrected
# this header's own first draft: **Git Bash on Windows has no `rsync`**, so the macOS
# carriage's `--rsync` transport dies with `exec: rsync: not found` (rc=127) before
# a single byte moves. The first draft said "run it from Git Bash for macOS", which
# was written from the fact that `ssh-macos.sh` REACHES the host from Git Bash --
# true, and irrelevant to the transport. Reaching a host and pushing a tree to it
# are two capabilities, and only one of them was measured before the sentence was
# written.
# ⚠ A key on a Windows drive is world-readable to WSL and ssh REFUSES it; both
# carriage scripts already stage a private 0600 copy, so nothing is needed here.
#
# ★ WHY THERE IS NO `.ps1` SIBLING, also measured rather than preferred: the reach
# is not symmetric even from PowerShell -- `ssh-arm64-vps` works ONLY from WSL (its
# key exists only there and its own `.ps1` twin is known broken). A PowerShell twin
# of THIS script would ship with one working carriage out of two while presenting
# both, which is worse than not shipping it.
#
# Usage:
#   wsl.exe -e bash scripts/remote-leg/remote-leg.sh --carriage macos
#   wsl.exe -e bash scripts/remote-leg/remote-leg.sh --carriage arm64-vps
#   wsl.exe -e bash scripts/remote-leg/remote-leg.sh --carriage macos --mode sync-only
#   wsl.exe -e bash scripts/remote-leg/remote-leg.sh --carriage macos -R 'link/.*'
# (Inside a WSL shell, drop the `wsl.exe -e` and invoke `bash ...` directly.)
#
# MODES
#   full       (default) push -> CLEAN configure+build -> full ctest via run-gate
#   sync-only  push and stamp, then stop (use to stage a host before a manual probe)
#   test-only  no push, no configure: CLEAN build is skipped and ctest runs as-is
#
# PARALLELISM
#   `ctest -j` defaults to the REMOTE host's own core count, probed over the
#   carriage -- ssh forwards no environment, so `CTEST_PARALLEL_LEVEL` set by
#   `run-gate` / `local-build` does NOT reach here and the remote default is
#   SERIAL. Override with `-j N` or `DSS_REMOTE_LEG_JOBS`. See the block beside
#   the probe and D-SCRIPT-REMOTE-LEG-CTEST-TAKES-THE-REMOTE-SERIAL-DEFAULT.
#
# INTERLOCK
#   One leg per host at a time, enforced by `.dss-leg-lock` at the remote root.
#   A second leg REFUSES and prints the holder's host/pid/UTC stamp. Stopping the
#   local driver does NOT stop the remote work, so a lock left behind by a killed
#   leg is discarded with `--force-lock` -- an operator decision taken with the
#   holder's stamp in front of them, never a timeout.
set -uo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "${SCRIPT_DIR}/../.." && pwd)"
cd "${REPO_ROOT}" || exit 2

CARRIAGE=""
MODE="full"
FILTER=""
JOBS="${DSS_REMOTE_LEG_JOBS:-}"
REMOTE_ENV=""
FORCE_LOCK=0

die() { printf '\n[X] remote-leg: %s\n' "$*" >&2; exit 1; }
say() { printf '\n=== %s ===\n' "$*"; }

# ★ RESOLVED BY EXECUTION, NOT BY `command -v`, which has answered yes for a stub
# on these hosts. `python3` is the name in WSL and on macOS, `python` the one a
# Windows install provides; a carriage that hardcodes one fails on the other with
# a message that reads as a missing dependency rather than as a missing name.
PY=""
for _c in python3 python; do
    if "$_c" -c 'import sys; sys.exit(0)' >/dev/null 2>&1; then PY="$_c"; break; fi
done
[[ -n "$PY" ]] || die "no working python3/python on PATH -- carriage-excludes cannot run"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --carriage) CARRIAGE="${2:?--carriage needs a value}"; shift 2 ;;
        --mode)     MODE="${2:?--mode needs a value}"; shift 2 ;;
        -R)         FILTER="${2:?-R needs a value}"; shift 2 ;;
        -j)         JOBS="${2:?-j needs a value}"; shift 2 ;;
        --force-lock) FORCE_LOCK=1; shift ;;
        -h|--help)
            awk 'NR==1 && /^#!/ {next} /^# PURPOSE:/ {next} /^#/ || /^[[:space:]]*$/ {print; next} {exit}' "$0"
            exit 0 ;;
        # An unknown flag is a REFUSAL, never a shrug: silently ignoring one is how a
        # leg runs a different thing than the operator asked for and still reports green.
        *) die "unknown argument '$1' (try --help)" ;;
    esac
done

# ── the carriage table. Adding a host means adding a row here and nothing else. ──
case "$CARRIAGE" in
    macos)
        CARRIAGE_SH="scripts/ssh-macos/ssh-macos.sh"
        REMOTE_DIR="src/dss-code-prime"
        # ★★ macOS's NON-INTERACTIVE PATH DOES NOT CONTAIN `/opt/homebrew/bin`.
        # ✔MEASURED 2026-08-21: the login profile exports
        # `/usr/bin:/bin:/usr/sbin:/sbin` (plus emsdk), so `cmake` and `ninja` are
        # NOT FOUND over ssh even though both exist at `/opt/homebrew/bin`, verified
        # by FILESYSTEM probe. This is the repo's standing "`command -v` LIES over
        # non-interactive ssh on macOS" trap, hitting the build step instead of a
        # capability check. Prepending is the fix; probing is not, because the
        # probe would lie the same way.
        REMOTE_ENV='export PATH=/opt/homebrew/bin:$PATH; '
        ;;
    arm64-vps)
        CARRIAGE_SH="scripts/ssh-arm64-vps/ssh-arm64-vps.sh"
        REMOTE_DIR="src/Github/dss-code-prime"
        REMOTE_ENV=''
        ;;
    "")  die "--carriage is required (macos | arm64-vps)" ;;
    *)   die "unknown carriage '$CARRIAGE' (macos | arm64-vps)" ;;
esac
[[ -f "$CARRIAGE_SH" ]] || die "carriage script not found: $CARRIAGE_SH"

case "$MODE" in
    full|sync-only|test-only) ;;
    *) die "unknown --mode '$MODE' (full | sync-only | test-only)" ;;
esac

carriage() { bash "$CARRIAGE_SH" "$@"; }

# ── the leg repository is a CLONE (operator ruling 2026-08-26) ───────────────
# Run one `leg-tree` verb on the carriage. The script text is INLINED rather than
# assumed present on the far side: the host's checkout can predate this file, and a
# bootstrap that needs the thing it bootstraps is not a bootstrap.
# ★ `"$(cat …)"` IS WHAT MAKES THIS SAFE, and the distinction is the one this project
# keeps paying for: command-substitution output is NOT re-expanded, so the script's own
# `$`, backticks and quotes reach the remote verbatim, while the few values written
# explicitly below expand locally, once, under this shell's control. Writing the body
# inside the same double quotes would have expanded the SCRIPT's variables here.
# ⓘ The inlined text ends in a dispatch guarded by `${1:-}`; with no argument it takes
# the no-op arm and only the verb appended below runs.
leg_tree_remote() {
    _ltr_verb="$1"; shift
    _ltr_args=""
    for _a in "$@"; do _ltr_args="$_ltr_args '$_a'"; done
    carriage "$(cat "$REPO_ROOT/scripts/leg-tree/leg-tree.sh")
leg_tree_${_ltr_verb}${_ltr_args}"
}

# ── reachability, BEFORE anything expensive ─────────────────────────────────
say "carriage $CARRIAGE ($CARRIAGE_SH)"
remote_uname=$(carriage "uname -sm" 2>&1 | tail -1)
[[ -n "$remote_uname" ]] || die "carriage produced no output -- host unreachable or profile broken"
printf 'remote : %s\n' "$remote_uname"

# ★★★ THE REMOTE'S OWN CORE COUNT, BECAUSE AN `ssh` CHILD INHERITS NOTHING.
# D-SCRIPT-REMOTE-LEG-CTEST-TAKES-THE-REMOTE-SERIAL-DEFAULT. `run-gate` and
# `local-build` both default `CTEST_PARALLEL_LEVEL` to 8, and `run-gate` even
# appends it to `WSLENV` so a `wsl.exe` child sees it -- but **ssh forwards no
# environment at all without `SendEnv`/`AcceptEnv`**, which its own row says in
# so many words. So a leg driven through a carriage took the REMOTE default,
# and the remote default is SERIAL. ✔MEASURED 2026-08-21: both legs walked the
# 922-entry suite ONE TEST AT A TIME, visible in the log as a strict
# `Start N` / `N/922` alternation with no interleaving.
#
# ⚠ THE LEVEL IS THE REMOTE'S, NOT 8. The 8 in `run-gate` is a WORKSTATION
# number with a stated reason -- this project runs the Windows and WSL legs
# CONCURRENTLY on one box on purpose, so 8 leaves headroom. A remote gate host
# has no such co-tenant, and 8 would be wrong in BOTH directions:
# ✔MEASURED, macOS reports **10** and the VPS reports **4**.
#
# ★ `getconf _NPROCESSORS_ONLN` and not `nproc` (GNU-only, absent on macOS) nor
# `sysctl -n hw.ncpu` (BSD-only): ✔MEASURED to answer on BOTH carriages, which
# is the whole reason a carriage table can stay one row per host.
if [[ -z "$JOBS" ]]; then
    # ★★★ OPERATOR RULING 2026-08-25: "never use all CPUS, the idea is to keep build + tests + run always at 4 cpus", AMENDED same-day to "make it 6 cores, not 4, everywhere".
    # ⚠ THE CORE-COUNT PROBE IS GONE, DELIBERATELY. It answered "how many cores exist",
    # which is the wrong question -- the right one is "how many may this process take",
    # and the operator has answered it. Probing also made the level DIFFER per host
    # (macOS 10, VPS 4), so a leg was never comparable to another leg.
    JOBS="${DSS_JOBS:-6}"
fi
printf 'jobs   : %s (ctest -j)\n' "$JOBS"

LOCAL_HEAD=$(git log --oneline -1 2>/dev/null || echo '(no git)')
LOCAL_DIRTY=$(git status --porcelain 2>/dev/null | wc -l | tr -d ' ')
# ★ `LOCAL_HEAD` is a LOG LINE and cannot be handed to git as a revision. The clone
# handling below needs a bare sha and a branch NAME, so they are read as themselves
# rather than sliced out of a human-facing string.
LOCAL_SHA=$(git rev-parse HEAD 2>/dev/null || echo '')
LOCAL_BRANCH=$(git rev-parse --abbrev-ref HEAD 2>/dev/null || echo '')
printf 'local  : %s\n' "$LOCAL_HEAD"
printf 'dirty  : %s path(s)\n' "$LOCAL_DIRTY"
[ -n "$LOCAL_BRANCH" ] || die "cannot read this checkout's branch -- the leg host's clone is put on the DRIVER's branch, so a driver that cannot name its own has nothing to ask for"

# ── the interlock, BEFORE the first byte is written to the remote ───────────
#
# ★★★ TWO LEGS AGAINST ONE HOST SHARE `$REMOTE_DIR/build/dbg`, AND STOPPING THE
# LOCAL DRIVER DOES NOT STOP THE REMOTE WORK. That second clause is what makes
# the collision easy rather than exotic: ✔MEASURED 2026-08-21, a stopped VPS leg
# kept running for ~13 minutes -- its log grew to test 773 of 922 long after the
# local process was gone -- while a replacement leg was already starting, and a
# replacement's first act is `rm -rf build/dbg`. This is
# D-CYCLE-LANE-ISOLATION-STOPS-AT-THE-BUILD-TREE one hop further out: a gate
# result taken from a build tree something else is writing is not attributable to
# anything, which makes it worthless exactly when it matters.
#
# ★ A LOCK AND NOT A TIMEOUT. A leg legitimately runs for hours (✔macOS spent
# 3,317 s inside `integrated_tests` alone), so any timeout is a guess that
# eventually breaks an honest run. The stale case is the OPERATOR'S call, made
# with the lock's own contents -- host, pid and UTC stamp -- in front of them.
# ⓘ EVERY MODE TAKES IT, `test-only` INCLUDED: that mode skips the push and the
# build but still runs ctest in the shared tree, which is the half that matters.
LOCK="$REMOTE_DIR/.dss-leg-lock"
if [[ "$FORCE_LOCK" == "1" ]]; then
    printf '⚠ --force-lock: discarding any lock held on %s\n' "$CARRIAGE"
    carriage "rm -f $LOCK" >/dev/null 2>&1 || true
fi
# `set -o noclobber` in the REMOTE shell makes the test-and-create atomic; a
# `[[ -f ]]` followed by a write would race two legs started together.
lock_body="host=$(hostname 2>/dev/null || echo '?') pid=$$ utc=$(date -u +%Y-%m-%dT%H:%M:%SZ) carriage=$CARRIAGE"
lock_out=$(carriage "set -o noclobber; { printf '%s\n' \"$lock_body\" > $LOCK; } 2>/dev/null && echo LOCK-TAKEN || { echo LOCK-HELD; cat $LOCK 2>/dev/null; }" 2>&1)
case "$lock_out" in
    *LOCK-TAKEN*) : ;;
    *LOCK-HELD*)
        printf '\n[X] remote-leg: %s already holds a leg lock at %s\n' "$CARRIAGE" "$LOCK" >&2
        printf '%s\n' "$lock_out" | grep -E 'host=|pid=|utc=' >&2
        printf 'If that leg is genuinely gone, re-run with --force-lock.\n' >&2
        exit 1 ;;
    # ⚠ NEITHER TOKEN CAME BACK. That is not "the lock is free" -- it is "the
    # probe produced no verdict", and reading silence as a negative is the exact
    # mistake that let the collision above happen in the first place.
    *)  die "could not determine the lock state on $CARRIAGE (no LOCK-TAKEN/LOCK-HELD in the reply). Refusing rather than guessing." ;;
esac
# Released on EVERY exit path, `die` included -- a lock that outlives its holder
# is a lock that reds the next honest run.
# ★★ WHICH MODES TOUCH THE CLONE, AND WHY IT IS NOT "ALL OF THEM".
# The operator ruling is that a leg cleans up after itself -- but two of this script's
# three modes exist precisely to LEAVE something behind, and a restore that ignored
# that would silently destroy the thing the caller asked for:
#   full       prepare + restore. The ordinary gate: stage, measure, leave pristine.
#   sync-only  prepare, NO restore. Its whole contract is "stage this host so I can
#              probe it by hand" -- restoring on exit would delete the staging as the
#              command returned, and the caller would find a clean tree and no
#              explanation.
#   test-only  NEITHER. It runs ctest over whatever is already there, usually what a
#              previous `sync-only` staged. `prepare` resets the working tree, so
#              running it here would test HEAD while reporting on the staged tree --
#              a wrong answer rather than a missing one.
# ★ The general shape: cleanup belongs to the mode that CREATED the mess, never to
# every mode that happens to run afterwards.
case "$MODE" in
    full)      LEG_TREE_PREPARE=1; LEG_TREE_RESTORE=1 ;;
    sync-only) LEG_TREE_PREPARE=1; LEG_TREE_RESTORE=0 ;;
    *)         LEG_TREE_PREPARE=0; LEG_TREE_RESTORE=0 ;;
esac

# ★ RESTORE RUNS ON EVERY EXIT PATH, `die` included, and BEFORE the lock is dropped:
# a leg that dies half way leaves the dirtiest tree of all, and the next leg to take
# this lock must find the clone pristine rather than carrying the wreckage.
trap '[[ "$LEG_TREE_RESTORE" == "1" ]] && { leg_tree_remote restore "$REMOTE_DIR" "$LOCAL_SHA" 2>&1 | tail -2 || true; };
      bash "$CARRIAGE_SH" "rm -f $LOCK" >/dev/null 2>&1 || true' EXIT

# ── put the host's own clone on the tree under test ─────────────────────────
# Operator ruling 2026-08-26: the leg host keeps a CLONE, the leg checks its branch
# before working in it, and the leg cleans up after itself.
if [[ "$LEG_TREE_PREPARE" == "1" ]]; then
    say "leg-tree prepare $CARRIAGE:$REMOTE_DIR -> $LOCAL_BRANCH @ ${LOCAL_SHA}"
    leg_tree_remote prepare "$REMOTE_DIR" "$LOCAL_BRANCH" "$LOCAL_SHA" \
        || die "leg-tree could not prepare $CARRIAGE:$REMOTE_DIR"
else
    printf '\nleg-tree: SKIPPED for --mode %s -- this mode runs over whatever is already\n' "$MODE"
    printf '  on the host, so preparing would test HEAD while reporting on the staged tree.\n'
fi

# ── push ────────────────────────────────────────────────────────────────────
if [[ "$MODE" != "test-only" ]]; then
    say "rsync -> $CARRIAGE:$REMOTE_DIR (excludes DERIVED from git; .git withheld)"
    # ★★★ THE LIST IS DERIVED, NOT TYPED -- see `scripts/carriage-excludes/`. Four
    # carriages each carried their own enumeration until 2026-08-26, and all four
    # spelled `node_modules` ANCHORED at the top level, so a nested one was
    # invisible to every last of them. ✔MEASURED the same day, ON THE HOST THIS
    # CARRIAGE FEEDS: the VPS held `.kilo/` -- **3,671 files, 61 MB, 3,667 of them
    # `node_modules`** -- out of 25,198 files. ⚠ rsync does NOT delete an EXCLUDED
    # path, so the derivation alone would not have cleaned it; that took one
    # explicit removal, and the same is true of the next tree to leak.
    # D-SCRIPT-CARRIAGE-EXCLUDES-ARE-A-HAND-LIST-AND-MISS-NESTED-IGNORED-TREES
    # ⓘ `.secrets` no longer needs naming here -- it is git-ignored, so the
    # derivation withholds it. `.git` stays withheld, but since 2026-08-26 that is
    # no longer a gap being papered over by the leg stamp: `leg_tree_prepare` above
    # has put the host's OWN clone on the driver's branch at the driver's commit, so
    # the remote git now describes the tree under test rather than an old checkout.
    EXCLUDES="$(mktemp)" || die "cannot create the exclude list"
    "$PY" "$REPO_ROOT/scripts/carriage-excludes/carriage-excludes.py" \
        --format rsync --repo "$REPO_ROOT" --also .git --out "$EXCLUDES" \
        || die "carriage-excludes refused (rc=$?) -- refusing to rsync with a list it would not vouch for"
    carriage --rsync --exclude-from="$EXCLUDES" \
        "$REPO_ROOT/" "$REMOTE_DIR" \
        || { rm -f "$EXCLUDES"; die "rsync failed (rc=$?)"; }
    rm -f "$EXCLUDES"

    # The stamp is the ONLY honest description of what is on that host: the remote
    # `.git` was deliberately not synced and still names an unrelated commit.
    stamp="remote-leg $(date -u +%Y-%m-%dT%H:%M:%SZ) carriage=$CARRIAGE mode=$MODE"
    stamp="$stamp local-head=$LOCAL_HEAD local-dirty=$LOCAL_DIRTY"
    carriage "cd $REMOTE_DIR && printf '%s\n' \"$stamp\" > .dss-leg-stamp && cat .dss-leg-stamp" \
        || die "could not write the leg stamp"
    remote_head=$(carriage "cd $REMOTE_DIR && git log --oneline -1 2>/dev/null || echo '(no git)'" 2>&1 | tail -1)
    printf '\n⚠ remote .git still says: %s\n' "$remote_head"
    printf '⚠ that describes the OLD checkout, NOT the files just pushed. Read .dss-leg-stamp.\n'
fi

[[ "$MODE" == "sync-only" ]] && { say "sync-only: done"; exit 0; }

# ── build ───────────────────────────────────────────────────────────────────
# CLEAN, always, for the same reason wsl-leg builds clean: rsync -a PRESERVES
# MTIMES, so an incremental build over a pushed tree can silently skip the very
# file the leg exists to exercise.
BUILD="build/dbg"

# ★★ ccache — THE CLEAN BUILD IS CORRECT AND ONLY ITS COST WAS EVER THE PROBLEM.
# `rm -rf $BUILD` above stays: a synced tree carries the SOURCE's mtimes, so an
# incremental ninja can silently skip the very file the leg exists to exercise
# (D-SYNC-RSYNC-PRESERVED-MTIME-DEFEATS-THE-REBUILD). ccache removes the cost
# WITHOUT trusting an mtime, because it keys on CONTENT — the object cache
# survives `rm -rf $BUILD`, so an unchanged TU is a cache hit rather than a
# recompile, and correctness is unchanged.
#
# ⚠ ✔MEASURED 2026-08-25 (cycle P34): ccache was ALREADY INSTALLED on the arm64
# VPS at /usr/bin/ccache and this script contained ZERO references to it, so
# every leg run rebuilt ~504 translation units on 4 Neoverse-N1 cores from
# scratch. The cache was sitting there unused.
# D-SCRIPT-REMOTE-LEG-REBUILT-FROM-SCRATCH-WITH-AN-INSTALLED-CCACHE-UNUSED
#
# ★ PROBED IN ITS OWN ROUND TRIP rather than inline in the configure command:
# the configure line is already ONE ssh argument with its own quoting rules, and
# a `$(command -v ...)` folded into it would be expanded on the WRONG side. The
# `grep -E "^/"` keeps a login banner from being mistaken for a path.
CACHE_ARGS=""
REMOTE_CCACHE=$(carriage "command -v ccache 2>/dev/null || true" 2>/dev/null | tr -d '\r' | grep -E '^/' | tail -1)
if [[ -n "$REMOTE_CCACHE" ]]; then
    printf 'ccache : %s (on %s)\n' "$REMOTE_CCACHE" "$CARRIAGE"
    CACHE_ARGS=" -DCMAKE_C_COMPILER_LAUNCHER=$REMOTE_CCACHE"
    CACHE_ARGS="$CACHE_ARGS -DCMAKE_CXX_COMPILER_LAUNCHER=$REMOTE_CCACHE"
else
    printf 'ccache : ABSENT on %s -- this leg recompiles every TU from scratch.\n' "$CARRIAGE"
    printf '         One line on that host:  sudo apt-get install -y ccache\n'
fi
if [[ "$MODE" == "full" ]]; then
    say "clean configure + build ($BUILD) on $CARRIAGE"
    # ONE argument: see the ssh-quoting note in the header.
    carriage "${REMOTE_ENV}cd $REMOTE_DIR && rm -rf $BUILD && cmake -S . -B $BUILD -G Ninja -DCMAKE_BUILD_TYPE=Debug -DDSS_BUILD_TESTS=ON$CACHE_ARGS > /tmp/remote-leg-configure.log 2>&1 || { tail -25 /tmp/remote-leg-configure.log; exit 20; }" \
        || die "configure failed on $CARRIAGE (rc=$?)"
    carriage "${REMOTE_ENV}cd $REMOTE_DIR && cmake --build $BUILD --parallel ${DSS_JOBS:-6} > /tmp/remote-leg-build.log 2>&1 || { tail -30 /tmp/remote-leg-build.log; exit 21; }; tail -1 /tmp/remote-leg-build.log" \
        || die "build failed on $CARRIAGE (rc=$?)"
fi

# ── test, through run-gate so a silent no-run cannot report success ──────────
say "ctest on $CARRIAGE${FILTER:+ (-R $FILTER)}"
LOG="build/remote-leg-$CARRIAGE.log"
ctest_cmd="${REMOTE_ENV}cd $REMOTE_DIR && ctest --test-dir $BUILD --output-on-failure"
[[ -n "$JOBS"   ]] && ctest_cmd="$ctest_cmd -j $JOBS"
[[ -n "$FILTER" ]] && ctest_cmd="$ctest_cmd -R '$FILTER'"
# ★★ THE REPO GUARDS ARE SKIPPED, by operator ruling 2026-08-25: every carriage this script
# drives is an INDIRECT leg. A guard checks the SOURCE TREE, and the tree was pushed FROM the
# root host, which already checked it. `DSS_LEG_GUARDS=1` restores them.
[[ "${DSS_LEG_GUARDS:-0}" == "1" ]] || ctest_cmd="$ctest_cmd -LE repo-guard"

# ★ The witness is TOOL-EMITTED (ctest's own summary line), never a string this
# script writes -- run-gate REFUSES a caller-authored witness for that reason.
witness='100% tests passed'
[[ -n "$FILTER" ]] && witness='tests passed'

bash scripts/run-gate/run-gate.sh "$LOG" "$witness" bash "$CARRIAGE_SH" "$ctest_cmd"
rc=$?
grep -E "tests passed|tests failed|The following tests FAILED" -A20 "$LOG" 2>/dev/null | tail -25
[[ $rc -eq 0 ]] || die "$CARRIAGE leg failed (rc=$rc, log $LOG)"

say "$CARRIAGE leg OK"
