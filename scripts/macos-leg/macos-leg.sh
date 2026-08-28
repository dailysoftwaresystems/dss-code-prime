#!/usr/bin/env bash
# PURPOSE: run a DSS gate leg on the operator's macOS host -- push the tree, build clean, run ctest.
#
# ★★★ WHY THIS EXISTS. macOS is the ONE target with no off-Mac emulator: nothing on
# Windows or Linux runs a Mach-O. Every other leg is verifiable where it is built
# (pe64 natively, elf64 under WSL, arm64 under qemu). Until now the Mac was reachable
# only as a shell (`scripts/ssh-macos`), so a macOS LEG meant retyping a push, a
# configure, a build and a ctest by hand -- the exact shape that produced
# `scripts/wsl-leg` after three near-identical scratch scripts in one session.
#
# ★★ IT USES THE OPERATOR'S OWN CHECKOUT, BY EXPLICIT AUTHORISATION, AND THE
# DESTRUCTIVE HALF IS OPT-IN RATHER THAN A DEFAULT.
# ✔MEASURED 2026-08-25: `~/src/dss-code-prime` sat at cycle **P5c** with 2449 tracked
# modifications and 152 untracked files. The standing order forbids cleaning or
# resetting anything on that personal machine, so this originally pushed into its own
# directory. The operator then authorised it in terms -- *"if you want you can reset
# --hard ~/src/dss-code-prime and use that one in macos"* -- which removes a duplicate
# ~3 GB tree and lets the BASE arrive over git instead of over a 2696-file tar.
# ⚠ `--reset-to <commit>` is therefore NAMED AND OPT-IN. A driver that resets someone's
# checkout as a silent default is a driver that eventually resets the wrong one.
#
# ⚠⚠ AND `reset --hard` IS NOT ENOUGH ON ITS OWN -- IT RESTORES TRACKED FILES AND
# REMOVES NO UNTRACKED ONE. ✔MEASURED on the very first adoption: 46 untracked entries
# survived the reset, among them `examples/c-subset/` and
# `tests/corpus/diagnostics/c-subset/` -- trees from BEFORE the c-subset -> c rename of
# 2026-08-24. The examples runner GLOBS `examples/<lang>/*`, so a stale `c-subset` tree
# is picked up and run against a config that no longer describes it: a false red at
# best, a green over the wrong corpus at worst. This reports the untracked count after
# every reset for exactly that reason -- a leg tree's staleness lives in what git
# stopped tracking, not in what it tracks.
#
# ★ TRANSPORT is `ssh-macos.sh --push` (tar over ssh), because ✔MEASURED there is no
# local `rsync` in Git Bash -- the primary Windows shell for this repo -- so
# `--rsync` cannot run from the host that drives the gate.
#
# ⚠ BUILDS CLEAN, DELIBERATELY. tar preserves mtimes exactly as `rsync -a` does, so a
# pushed source whose mtime lands behind an existing build output makes ninja skip it
# (the defect `scripts/wsl-leg` records as D-SYNC-RSYNC-PRESERVED-MTIME-DEFEATS-THE-REBUILD).
# Building clean costs minutes and removes the whole class.
#
# ⚠ `command -v` LIES over non-interactive ssh on this host -- probe the FILESYSTEM.
# That is why the toolchain is located by absolute path below and not by asking the shell.
#
# ★★★ EVERY RUN CARRIES A TOKEN, AND THE WITNESS IS MATCHED ON THAT TOKEN.
# ✔MEASURED 2026-08-25 (D-SCRIPT-MACOS-LEG-WITNESS-CAN-BE-ANOTHER-RUN-S-EXIT-CODE):
# two legs ran against this Mac at once and BOTH tee'd to `${TMPDIR}/macos-leg.out`.
# `tee` truncates on open but each writer keeps its OWN offset, so the killed leg's
# `REMOTE_CTEST_RC=143` (SIGTERM) sat at ~2 MB while the live leg wrote from ~0 -- and
# the witness was `grep -o 'REMOTE_CTEST_RC=[0-9]*' | tail -1`, which reads the LAST
# match in the file. The live leg would have been called FAILED.
# ★ The direction that costs something is the other one: a stale `=0` outliving a live
# failure is a FALSE GREEN by the identical mechanism, and nothing would have said so.
# ⇒ the log path is per-run, and the witness is `REMOTE_CTEST_RC[<run>]=`. A foreign
# run's line is now unmatchable rather than merely unlikely to be last.
#
# ★★ AND THE TOKEN ONLY MAKES THE COLLISION VISIBLE -- THE LOCK IS WHAT PREVENTS IT.
# The same measurement: the second leg's `rm -rf build/dbg` ran underneath the first
# leg's LIVE ctest, which then spent 2 h 34 m walking a tree that was being deleted and
# rebuilt under it. A gate result taken from a shared build tree is not attributable to
# anything, which is precisely when it matters least and costs most. The remote half
# now takes a lock on the build tree and REFUSES rather than destroying it; a lock whose
# owning pid is gone is stale and may be taken, so a killed leg cannot wedge the carriage.
# ⓘ A hand-rolled `ssh <host> ctest ...` still bypasses the lock -- that is what leg #1
# was. The token is the defence that survives an invocation this script never saw.
#
# ★★ PARALLEL ctest, WHICH THIS LEG LOST BY BEING WRITTEN AFTER THE FIX.
# `D-SCRIPT-REMOTE-LEG-CTEST-TAKES-THE-REMOTE-SERIAL-DEFAULT` closed exactly this in
# `scripts/remote-leg/remote-leg.sh`: ssh forwards NO environment, so a driver-side
# `CTEST_PARALLEL_LEVEL` never arrives and the remote default is SERIAL. `macos-leg` was
# written on 2026-08-25, after that close, and passed no `-j` at all. The level is 4 by
# operator ruling (see the jobs block); the row's core-count PROBE is deliberately not
# copied here, because how many cores exist is not the same question as how many a guest
# process may take on someone's personal machine.
#
# ★★ ccache, BECAUSE THE CLEAN BUILD IS CORRECT AND ONLY ITS COST IS THE PROBLEM.
# `rm -rf build/dbg` below is deliberate and stays: tar preserves mtimes, so a pushed
# source can land BEHIND an existing object and ninja will silently skip it
# (D-SYNC-RSYNC-PRESERVED-MTIME-DEFEATS-THE-REBUILD). ✔MEASURED 2026-08-25 with the
# host held awake: that clean build is 841 targets / 504 compile edges and **179 s**
# of a **17.4 min** leg (13 s configure + 179 s build + 849 s ctest).
# ⚠ THIS LINE READ "~22 min of a ~35 min leg" AND THAT NUMBER WAS A NAP. It was taken
# on runs where the Mac slept through most of the build
# ([[D-SCRIPT-MACOS-LEG-RAN-WHILE-THE-HOST-SLEPT-AND-CHARGED-IT-TO-THE-TESTS]]);
# the same build measured 166 s, then 1553 s, then 179 s once `caffeinate` made
# sleeping impossible. ★ The 166 s reading was RIGHT and was disowned for a while
# because a broken measurement contradicted it -- when two measurements of one
# operation disagree by 9x, ask WHY before trusting the newer one. The fix is not
# to start trusting
# mtimes -- it is to make the rebuild decision CONTENT-ADDRESSED, which is what a
# compiler cache is. ⓘ ccache is ABSENT on this Mac as measured, so this stays inert and
# says what to type rather than installing software on the operator's machine: a driver
# that silently mutates a personal box is the same class of mistake as one that resets it.
#
# Usage:
#   scripts/macos-leg/macos-leg.sh                      # push CWD, clean build, full ctest
#   scripts/macos-leg/macos-leg.sh --src <dir>          # push <dir> instead of CWD
#   scripts/macos-leg/macos-leg.sh -R '<regex>'         # scope the ctest
#   scripts/macos-leg/macos-leg.sh -j <n>               # ctest parallelism (default 6)
#   scripts/macos-leg/macos-leg.sh --guards             # ALSO run the repo guards (off by
#                                                       #   default: they check the TREE, and
#                                                       #   the root host already did)
#   scripts/macos-leg/macos-leg.sh --no-push            # reuse what is already on the Mac
#   scripts/macos-leg/macos-leg.sh --reset-to <commit>  # DESTRUCTIVE: reset the remote checkout first
#   scripts/macos-leg/macos-leg.sh --mode build         # clean build and STOP -- no ctest
#   scripts/macos-leg/macos-leg.sh --tree <name> --build-type <T>   # build/<name>, default build/dbg + Debug
#
# ⚠ `--mode`, `--tree` and `--build-type` ARRIVED IN P35 AND `--help` DENIED THEIR
# EXISTENCE UNTIL P36. `--help` prints the contiguous comment block at the top of this
# file and stops at the first non-comment line, so the rationale written beside those
# flags DOWN IN THE ARGUMENT LOOP was unreachable from the interface -- the same class
# of drift that the `sed -n '2,40p'` note below records, arriving by a different route.
# A flag the tool accepts and its own `--help` does not mention is a flag nobody will
# type.
#
# ⚠ A FLAG THE SELECTED MODE NEVER READS IS REFUSED, NOT IGNORED: `-R` and `--guards`
# under `--mode build` (which runs no ctest) exit non-zero and say so, and a `-j` that
# is not a positive integer is refused rather than silently replaced by the default.
set -uo pipefail

SRC="$(pwd)"
FILTER=""
JOBS=""
GUARDS="${DSS_LEG_GUARDS:-0}"
DO_PUSH=1
# ★ The token identifies THIS invocation to the witness. pid alone is not enough -- pids
# are reused, and two legs minutes apart on one host can collide.
LEG_RUN="$$-$(date +%s)"
LEG_OUT="${TMPDIR:-/tmp}/macos-leg-${LEG_RUN}.out"
DST="${DSS_MACOS_LEG_DIR:-~/src/dss-code-prime}"
RESET_TO=""
# Defaults chosen so every pre-existing invocation behaves EXACTLY as before.
MODE="full"
TREE="dbg"
BUILD_TYPE="Debug"
CARRIAGE="scripts/ssh-macos/ssh-macos.sh"

die() { printf '\n[X] macos-leg: %s\n' "$*" >&2; exit 1; }
say() { printf '\n=== %s ===\n' "$*"; }

# Which flags the CALLER actually typed, as opposed to which ones hold a default or
# an inherited environment value. The refusals below can only be honest about "this
# mode never reads that" if they can tell a supplied value from an inherited one --
# `--guards` in particular, whose default is `$DSS_LEG_GUARDS`.
GAVE_FILTER=0; GAVE_GUARDS=0

while [ $# -gt 0 ]; do
    case "$1" in
        --src)     SRC="${2:?--src needs a value}"; shift 2 ;;
        -R)        FILTER="${2:?-R needs a value}"; GAVE_FILTER=1; shift 2 ;;
        -j)        JOBS="${2:?-j needs a value}"; shift 2 ;;
        --guards)  GUARDS=1; GAVE_GUARDS=1; shift ;;
        --no-push) DO_PUSH=0; shift ;;
        # DESTRUCTIVE, and therefore never a default: `git reset --hard` on the remote
        # checkout before the push. Tracked files return to <commit>; untracked ones do
        # NOT, and the count is reported below.
        --reset-to) RESET_TO="${2:?--reset-to needs a commit}"; shift 2 ;;
        --dst)     DST="${2:?--dst needs a value}"; shift 2 ;;
        # `build` mode: clean configure + build, then STOP. It exists so a BENCHMARK
        # can get a RELEASE driver onto this host without paying for a suite -- and
        # because the reading it then takes is only valid on a QUIET host, so running
        # tests in the same invocation would corrupt the measurement the build was for.
        # Defaults leave `full` byte-for-byte as it was: build/dbg, Debug.
        # D-SCRIPT-MACOS-LEG-HAS-NO-BUILD-ONLY-MODE-SO-NO-LEG-CAN-BE-BENCHMARKED
        --mode)       MODE="${2:?--mode needs a value}"; shift 2 ;;
        --tree)       TREE="${2:?--tree needs a value}"; shift 2 ;;
        --build-type) BUILD_TYPE="${2:?--build-type needs a value}"; shift 2 ;;
        # ⚠ This printed a FIXED LINE RANGE (`sed -n '2,40p'`) and the usage block had
        # already drifted below it, so `--help` showed the rationale and NOT the usage.
        # The contiguous comment block cannot drift out of date the way a range does.
        -h|--help) awk 'NR>1 && /^#/ {sub(/^# ?/,""); print; next} NR>1 {exit}' "$0"; exit 0 ;;
        # An unknown flag is a REFUSAL, never a shrug: silently ignoring one is how a
        # leg runs a different thing than the operator asked for and still reports green.
        *) die "unknown argument '$1' (try --help)" ;;
    esac
done

# An unknown MODE is a REFUSAL for the same reason an unknown FLAG is, and the
# argument loop above already says so in a comment. A closed set is only closed
# where something checks it.
case "$MODE" in
    full|build) ;;
    *) die "unknown --mode '$MODE' (expected: full, build)" ;;
esac

# ── A KNOWN FLAG THE SELECTED MODE NEVER READS IS ALSO A REFUSAL ────────────
# ★★★ SAME ARGUMENT THE ARGUMENT LOOP ALREADY MAKES ABOUT THE UNKNOWN FLAG:
# "silently ignoring one is how a leg runs a different thing than the operator asked
# for and still reports green." A KNOWN flag this mode never reads is the identical
# failure wearing a legal name, and it is the wider hole -- nothing is unknown, so
# nothing was ever going to refuse it.
# ✔MEASURED 2026-08-25 (cycle P36) by RUNNING the pre-edit file: `--mode build -R
# 'harness/.*'` was accepted without a word and went straight on to the carriage, and
# `--mode build --guards` did the same. Neither could have taken effect: `LEG_FILTER`
# and `LEG_GUARDS` are read ONLY by the `CTEST_ARGS` block, which the build mode
# returns before reaching. A `-R` is typed precisely to narrow a run, so swallowing it
# is the one response that must never be silent.
# ★ `--guards` is checked on the FLAG and not on `$GUARDS`, because `$GUARDS` also
# carries `$DSS_LEG_GUARDS` -- a standing environment switch that means "run them
# wherever they run" and must not turn an unrelated build into a refusal.
if [ "$MODE" = "build" ] && [ "$GAVE_FILTER" = "1" ]; then
    die "--mode build never reads '-R' (this mode runs no ctest, so there is nothing for a filter to scope). Refused rather than ignored: a flag that is silently dropped is how a leg runs a different thing than the operator asked for and still reports green."
fi
if [ "$MODE" = "build" ] && [ "$GAVE_GUARDS" = "1" ]; then
    die "--mode build never reads '--guards' (the repo guards are ctest entries and this mode runs no ctest). Refused rather than ignored: a flag that is silently dropped is how a leg runs a different thing than the operator asked for and still reports green."
fi

# ── AND A `-j` THAT IS NOT A JOB COUNT IS A REFUSAL, NOT A SHRUG ────────────
# ★★ THE COERCION EXISTS AND IT IS ON THE WRONG SIDE OF THE WIRE. The remote body
# maps `''|0|*[!0-9]*` to the default, which is the RIGHT answer for a value that was
# never sent and the WRONG one for a value the operator typed. ✔MEASURED on the
# pre-edit file: `-j four` was accepted without a word and went on to the carriage,
# where the remote `case` would have replaced it with the default and said nothing.
# A leg that quietly picks its own parallelism answers a different question than the
# one asked, and the timings are what a `-j` is usually typed for.
# ★ Validated HERE, where the value is typed. The remote fallback stays as it is,
# because it now only has to cover "nothing arrived".
case "${JOBS:-}" in
    '') ;;
    0|*[!0-9]*) die "-j '$JOBS' is not a positive job count. Refused rather than silently replaced by the remote default." ;;
esac

[ -f "$CARRIAGE" ] || die "carriage not found at $CARRIAGE (run from the repo root)"

if [ -n "$RESET_TO" ]; then
    say "remote fetch + reset --hard $RESET_TO"
    # `$DST` unquoted on the remote side so a leading `~` expands there.
    bash "$CARRIAGE" "cd $DST && git fetch --all --prune -q && git cat-file -e ${RESET_TO}^{commit} && git reset --hard -q $RESET_TO && echo RESET_HEAD=\$(git rev-parse --short HEAD) && echo RESET_UNTRACKED=\$(git status --porcelain --untracked-files=all | grep -c '^??')" \
      > "${TMPDIR:-/tmp}/macos-leg-reset.out" 2>&1
    grep -E 'RESET_HEAD=|RESET_UNTRACKED=' "${TMPDIR:-/tmp}/macos-leg-reset.out" || {
        cat "${TMPDIR:-/tmp}/macos-leg-reset.out" >&2
        die "reset did not report a HEAD -- refusing to build on an unknown tree"
    }
    _untracked=$(sed -n 's/^RESET_UNTRACKED=//p' "${TMPDIR:-/tmp}/macos-leg-reset.out" | tail -1)
    if [ "${_untracked:-0}" != "0" ]; then
        echo "⚠ $_untracked untracked file(s) survived the reset -- reset --hard removes NONE of them."
        echo "  A stale tree that git stopped tracking is still visible to every glob in the suite."
    fi
fi

# ── the leg repository is a CLONE, put on the tree under test ────────────────
# Operator ruling 2026-08-26: every leg host keeps its own clone (`~/src/dss-code-prime`
# here), the leg CHECKS ITS BRANCH before working in it, and the leg cleans up after
# itself. One owner for all three hosts: `scripts/leg-tree/`.
# ★ The script text is INLINED rather than assumed present on the far side -- the Mac's
# checkout can predate this file, and a bootstrap that needs the thing it bootstraps is
# not a bootstrap. `"$(cat …)"` is what makes that safe: command-substitution output is
# NOT re-expanded, so the script's own `$` and quotes arrive verbatim while the three
# values written explicitly expand here, once, under this shell's control.
# ⚠ THIS RUNS EVEN WHEN `--no-push` IS GIVEN, and deliberately: `--no-push` means "do
# not replace the files", not "do not know which commit they belong to".
leg_tree_remote() {
    _ltr_verb="$1"; shift
    _ltr_args=""
    for _a in "$@"; do _ltr_args="$_ltr_args '$_a'"; done
    bash "$CARRIAGE" "$(cat "$SRC/scripts/leg-tree/leg-tree.sh")
leg_tree_${_ltr_verb}${_ltr_args}"
}
LEG_BRANCH=$(git -C "$SRC" rev-parse --abbrev-ref HEAD 2>/dev/null || echo '')
LEG_SHA=$(git -C "$SRC" rev-parse HEAD 2>/dev/null || echo '')
[ -n "$LEG_BRANCH" ] || die "cannot read this checkout's branch -- the host's clone is put on the DRIVER's branch, so a driver that cannot name its own has nothing to ask for"
say "leg-tree prepare $DST -> $LEG_BRANCH @ $LEG_SHA"
leg_tree_remote prepare "$DST" "$LEG_BRANCH" "$LEG_SHA" || die "leg-tree could not prepare $DST"
# ★ Restore on EVERY exit path, `die` included: a leg that dies half way leaves the
# dirtiest tree of all, which is exactly when the next leg most needs a clean one.
trap 'leg_tree_remote restore "$DST" "$LEG_SHA" 2>&1 | tail -2 || true' EXIT

if [ "$DO_PUSH" = "1" ]; then
    say "push $SRC -> $DST"
    # --prune, because a LEG's contract is "test THIS tree". Without it the Mac keeps
    # every file this repo has ever deleted, and the gate measures a tree that exists
    # nowhere -- ✔MEASURED 2026-08-25, and it reddened plan_citations_guard on macOS while
    # the identical guard was rc=0 locally.
    bash "$CARRIAGE" --push --prune "$SRC" "$DST" || die "push failed"
fi

say "remote clean configure + build + ctest"
# The remote half goes over STDIN. A remote script fed this way never has to be
# quoted into a `-c` string, which is the trap that once turned a variable into
# `rsync -a --delete / /`.
#
# ⚠⚠ THIS COMMENT PREVIOUSLY CLAIMED THE OPPOSITE OF THE TRUTH AND THE CLAIM CITED A
# MEASUREMENT, WHICH IS WHY IT SURVIVED. It read: "✔MEASURED 2026-08-25 that stdin
# survives byte-exact to this host (1000 random bytes, identical md5) -- the
# 2026-08-18 note claiming the login profile eats stdin is expired." The 2026-08-18
# note was RIGHT and this line retired it on the strength of an experiment that did
# not test the thing that breaks: 1000 random bytes measured DATA crossing the wire,
# while what actually crosses here is A SHELL READING ITS SCRIPT FROM STDIN, and the
# consumer was never the login profile at all.
#
# ✔MEASURED 2026-08-25 (cycle P36, lane B): `ssh-macos.sh` resolved the `.local` name
# by shelling out to `powershell.exe` INSIDE A COMMAND SUBSTITUTION, which inherits
# this pipeline's stdin and DRANK THE REMOTE BODY before `ssh` ever ran. Same host,
# same payload, only the resolution path changing:
#     hostname form : printf '12345678\n' | ssh-macos.sh "wc -c"  ->  0
#     IP form       : DSS_MACOS_HOST=<ip> ...                     ->  9
# So `bash -s` received ZERO BYTES and EVERY macOS leg taken through the `.local`
# name ended in "no witness came back ... UNKNOWN". Fail-closed, so no false green
# was ever produced -- but a leg that cannot run is not a leg that passed.
# ⇒ FIXED at the source: the resolver helpers in `ssh-macos.sh` now read `</dev/null`.
# ★ THE LESSON WORTH KEEPING: a "✔MEASURED" tag is only as good as the question the
# experiment asked. Retiring a warning needs an experiment that reproduces the
# WARNED-ABOUT shape, not merely an adjacent one that happens to pass.
REMOTE_BODY=$(cat <<'REMOTE_EOF'
set -uo pipefail

# ★★★ EVERY TERMINAL STATE EMITS A WITNESS, NOT JUST THE TWO SUCCESSFUL ONES.
# ⚠ SEVEN of this body's exits emitted no witness at all -- a missing checkout, an
# unmakeable log directory, a held lock, a missing cmake, a missing ninja, a failed
# configure and a failed build -- so every one of them reached the operator as the SAME
# sentence, "no witness came back, this run's real status is UNKNOWN", while the real
# diagnosis sat further up a several-hundred-line transcript. (The wipe check below
# would have been an eighth; it is new in this commit and was born on this channel.)
# ✔MEASURED 2026-08-25 (cycle P36) on the real host, one arm of the seven: a `--dst`
# that does not exist there reported "no REMOTE_BUILD_RC witness came back ... UNKNOWN"
# before and "macOS build leg FAILED (rc=1)" after, with `[X] remote: /tmp/… missing`
# visible in both transcripts and read by neither verdict.
# ★ The verdict was never WRONG -- the driver refuses either way, which is the half
# that matters -- but the CAUSE was always misnamed, and a refusal's only job is to be
# acted on.
# ★★ IT IS ALSO THIS FILE'S OWN STATED RULE, applied to the states it had not reached:
# see the `build`-mode block below -- "a watcher must observe EVERY terminal state on
# the channel it polls". Eight of them were invisible on that channel.
# ⓘ THE KEY IS MODE-CHOSEN, exactly as the driver's poll is: a `build` run can never
# produce a REMOTE_CTEST_RC, so a failure that emitted one would be unmatchable and
# would land back on the anonymous refusal this block exists to end.
WITNESS_KEY=REMOTE_CTEST_RC
if [ "${LEG_MODE:-full}" = "build" ]; then WITNESS_KEY=REMOTE_BUILD_RC; fi
remote_fail() {   # <rc> <what went wrong>
    _rf_rc="$1"; shift
    echo "[X] remote: $*"
    echo "$WITNESS_KEY[$LEG_RUN]=$_rf_rc"
    exit "$_rf_rc"
}

cd "$LEG" || remote_fail 1 "$LEG missing"

# ★★★ HOLD THE MACHINE AWAKE FOR EXACTLY AS LONG AS THIS LEG LIVES.
# ⚠ ✔MEASURED 2026-08-25 (cycle P34) from `pmset -g log` DURING a running leg: this
# host entered Sleep and DarkWake over and over, seconds apart, while the build and
# ctest were mid-flight -- "Entering Sleep state due to 'Maintenance Sleep'" and
# "'Dark Wake Thermal Emergency'". An ssh session holds NO power assertion, so macOS
# scores the machine idle however busy the leg is.
#
# ★★ WHAT THAT DOES TO A MEASUREMENT, WHICH IS WORSE THAN WHAT IT DOES TO THE CLOCK:
# frozen processes stop, the wall clock does not, so ctest attributes the sleep to
# whichever tests were in flight. ✔OBSERVED: `tokenizer/test_source_reader` reported
# **728.98 sec** in the leg and runs in **0.004 s** when invoked directly -- and two
# neighbours reported 729.69 s and 733.98 s, all three finishing the instant the host
# woke. Same cause on the build: 166 s in one run and 1553 s in the next, same graph,
# same -j, same host. Every one of those numbers is a sleep duration wearing a test's
# name, and a slowness hunt reading them is chasing an artifact.
# D-SCRIPT-MACOS-LEG-RAN-WHILE-THE-HOST-SLEPT-AND-CHARGED-IT-TO-THE-TESTS
#
# ★ `-w $$` ties the assertion to THIS shell: it is released when the leg exits, by
# any path, with no trap to forget and nothing left holding the Mac awake afterwards.
# `-dimsu` covers display, idle, disk and system sleep. Absolute path because
# `command -v` is unreliable over non-interactive ssh on this host.
if [ -x /usr/bin/caffeinate ]; then
    /usr/bin/caffeinate -dimsu -w $$ &
    echo "awake : caffeinate -dimsu held for pid $$ (host may not sleep during this leg)"
else
    echo "! /usr/bin/caffeinate ABSENT -- this host may SLEEP mid-leg and charge the"
    echo "  stall to whatever test was in flight. Timings from this run are suspect."
fi

# ★ Artifacts live under build/, which is gitignored AND excluded from the push. The
# previous names sat in the checkout ROOT as untracked files, where `--reset-to` counts
# them as staleness -- a leg's own logs should not look like a dirty tree.
LOGDIR="build/macos-leg/$LEG_RUN"
mkdir -p "$LOGDIR" || remote_fail 1 "cannot create $LOGDIR"
echo "logs  : $LEG/$LOGDIR"

# ★★ MUTUAL EXCLUSION ON THE BUILD TREE. Two legs sharing one build/dbg do not merely
# run slowly -- the second one's `rm -rf` deletes the tree the first is testing, and
# NEITHER verdict is attributable afterwards. Refuse; never destroy.
# ⚠ THE REFUSAL USED TO NAME `build/dbg` UNCONDITIONALLY, AND SINCE THE `build` MODE
# LANDED THAT IS OFTEN THE WRONG TREE. The lock is ONE lock over the whole `build/`
# directory (deliberately: it is coarser than the trees it protects, and coarse is the
# safe direction for a refusal), but a message that names a directory neither leg is
# touching sends the reader to the wrong place -- and a refusal's only job is to be
# acted on. It names the ACTUAL trees now, both the owner's and this run's, which is
# why the lock records one.
TREEDIR="build/$LEG_TREE"
LOCK="build/.macos-leg.lock"
if [ -e "$LOCK" ]; then
    _owner=$(sed -n 's/^pid=//p' "$LOCK" | head -1)
    if [ -n "$_owner" ] && kill -0 "$_owner" 2>/dev/null; then
        echo "[X] remote: another macOS leg owns $LEG/build"
        echo "    owner pid=$_owner run=$(sed -n 's/^run=//p' "$LOCK" | head -1) mode=$(sed -n 's/^mode=//p' "$LOCK" | head -1) tree=$(sed -n 's/^tree=//p' "$LOCK" | head -1)"
        remote_fail 4 "refusing to start -- that would rm -rf $LEG/$TREEDIR underneath a live leg"
    fi
    # A leg killed mid-run must not wedge the carriage forever: the owner is gone, so
    # the lock describes nothing. Say so rather than silently reclaiming it.
    echo "! stale lock (pid ${_owner:-?} is gone) -- taking it"
    rm -f "$LOCK"
fi
printf 'pid=%s\nrun=%s\nmode=%s\ntree=%s\n' "$$" "$LEG_RUN" "${LEG_MODE:-full}" "$TREEDIR" > "$LOCK"
trap 'rm -f "$LOCK"' EXIT INT TERM

# Absolute paths: `command -v` is unreliable over non-interactive ssh here.
CMAKE=""
for c in /opt/homebrew/bin/cmake /usr/local/bin/cmake; do [ -x "$c" ] && CMAKE="$c" && break; done
NINJA=""
for n in /opt/homebrew/bin/ninja /usr/local/bin/ninja; do [ -x "$n" ] && NINJA="$n" && break; done
[ -n "$CMAKE" ] || remote_fail 1 "no cmake found on the filesystem"
[ -n "$NINJA" ] || remote_fail 1 "no ninja found on the filesystem"
echo "cmake : $CMAKE ($("$CMAKE" --version | head -1))"
echo "ninja : $NINJA ($("$NINJA" --version))"
echo "cc    : $(/usr/bin/cc --version | head -1)"

# ★★★ THE DEFAULT IS 4, BY OPERATOR RULING 2026-08-25: *"default for testing is -j4 to
# not use 100% of the machine on our tests"*. ⚠ The first draft of this fix probed the
# remote core count -- the precedent `remote-leg.sh` set -- and would have claimed all TEN
# of this Mac's cores. A gate leg is a GUEST on a personal machine, not its owner, and the
# probe answered the wrong question: not "how many cores exist" but "how many may I take".
# Amended same-day from 4 to 6. `-j <n>` still overrides for a box that can spare more.
JOBS="${LEG_JOBS:-}"
case "${JOBS:-}" in
    ''|0|*[!0-9]*) JOBS="${DSS_JOBS:-6}" ;;
esac
echo "jobs  : $JOBS"

# ★★ A CONTENT-ADDRESSED REBUILD DECISION, WHICH IS WHAT THE CLEAN BUILD BELOW NEEDS.
# Inert when absent, and it says what to type -- it does not install anything.
CCACHE=""
for c in /opt/homebrew/bin/ccache /usr/local/bin/ccache; do [ -x "$c" ] && CCACHE="$c" && break; done
CACHE_ARGS=""
if [ -n "$CCACHE" ]; then
    echo "ccache: $CCACHE ($("$CCACHE" --version | head -1))"
    CACHE_ARGS="-DCMAKE_C_COMPILER_LAUNCHER=$CCACHE -DCMAKE_CXX_COMPILER_LAUNCHER=$CCACHE"
else
    echo "ccache: ABSENT -- every leg recompiles all 504 TUs from scratch (~179 s measured, host awake)."
    echo "        The clean build is DELIBERATE and stays: tar preserves mtimes, so an"
    echo "        incremental ninja here can silently skip a pushed source. ccache removes"
    echo "        the COST without trusting an mtime, because it keys on CONTENT."
    echo "        One line, on the Mac:  brew install ccache"
fi

_t=$(date +%s)
_phase() { echo "PHASE $1 $(( $(date +%s) - _t ))s"; _t=$(date +%s); }

# (TREEDIR is set above, beside the lock that names it)
rm -rf "$TREEDIR"
# ★★ AND THE WIPE IS CHECKED, BECAUSE "CLEAN" IS THE ONLY THING THIS BUILD PROMISES.
# `set -e` is deliberately off here, so a `rm -rf` that failed fell straight through to
# a configure over the SURVIVING tree, and the leg then ran an INCREMENTAL build under
# a heading that says clean -- the preserved-mtime class this file's header spends a
# paragraph refusing to hope around. The postcondition is the DIRECTORY, not the rc.
if [ -e "$TREEDIR" ]; then
    remote_fail 1 "could not remove $TREEDIR, so this build would be INCREMENTAL under a heading that says CLEAN"
fi
# shellcheck disable=SC2086
"$CMAKE" -S . -B "$TREEDIR" -G Ninja \
    -DCMAKE_BUILD_TYPE="$LEG_BUILD_TYPE" \
    -DCMAKE_MAKE_PROGRAM="$NINJA" $CACHE_ARGS > "$LOGDIR/configure.log" 2>&1
rc=$?
if [ $rc -ne 0 ]; then tail -25 "$LOGDIR/configure.log"; remote_fail "$rc" "configure failed (rc=$rc, log $LOGDIR/configure.log)"; fi
echo "configure OK ($TREEDIR, $LEG_BUILD_TYPE)"; _phase configure

"$CMAKE" --build "$TREEDIR" --parallel "$JOBS" > "$LOGDIR/build.log" 2>&1
rc=$?
if [ $rc -ne 0 ]; then grep -iE 'error' "$LOGDIR/build.log" | head -25; remote_fail "$rc" "build failed (rc=$rc, log $LOGDIR/build.log)"; fi
echo "build OK: $(tail -1 "$LOGDIR/build.log")"; _phase build

# ── build mode: STOP HERE, and emit its OWN witness ─────────────────────────
# ★★ A DIFFERENT TERMINAL STATE NEEDS A DIFFERENT WITNESS. The driver polls for
# `REMOTE_CTEST_RC[<run>]`; a mode that never runs ctest would never emit one, and
# the driver would correctly refuse a run that actually succeeded. So this mode
# emits `REMOTE_BUILD_RC[<run>]` and the driver picks the name by mode -- the
# standing rule that a watcher must observe every terminal state on the channel it
# polls, applied to a channel that just grew a second state.
if [ "${LEG_MODE:-full}" = "build" ]; then
    if [ ! -x "$TREEDIR/bin/dss/dsscp" ]; then
        remote_fail 1 "build reported success but produced no driver at $TREEDIR/bin/dss/dsscp"
    fi
    ls -la "$TREEDIR/bin/dss/dsscp"
    echo "no tests were run in this mode, and it does not claim otherwise"
    echo "REMOTE_BUILD_RC[$LEG_RUN]=0"
    exit 0
fi

CTEST_ARGS="--test-dir $TREEDIR --output-on-failure -j $JOBS"
[ -n "${LEG_FILTER:-}" ] && CTEST_ARGS="$CTEST_ARGS -R ${LEG_FILTER}"
# ★★ THE REPO GUARDS ARE SKIPPED HERE, by operator ruling: this is an INDIRECT leg. They
# check the SOURCE TREE, the tree is byte-identical to the root host's, and on macOS they
# are the most expensive entries in the suite. `--guards` (or DSS_LEG_GUARDS=1) restores them.
[ "${LEG_GUARDS:-0}" = "1" ] || CTEST_ARGS="$CTEST_ARGS -LE repo-guard"
# shellcheck disable=SC2086
"${CMAKE%cmake}ctest" $CTEST_ARGS > "$LOGDIR/ctest.log" 2>&1
rc=$?
tail -25 "$LOGDIR/ctest.log"
_phase ctest
# ★ The witness carries THIS run's token, so a concurrent or killed leg's line cannot
# satisfy it. See the header for the measurement that made this necessary.
echo "REMOTE_CTEST_RC[$LEG_RUN]=$rc"
exit $rc
REMOTE_EOF
)
# ⚠ THE PREAMBLE IS BUILT WITH `printf`, NOT WITH `${var/pat/repl}`. ✔MEASURED
# 2026-08-25: the pattern-replacement form emitted a bare `$FILTER` into the remote
# script and `set -u` there killed it with "FILTER: unbound variable" -- the
# replacement text in `${var/pat/repl}` does NOT undergo quote removal, so the single
# quotes meant to protect it survived into the payload and changed what it meant.
# ★ The leg did NOT report green on that: the witness check below refused it, which is
# the whole reason the witness exists.
# `LEG` is emitted UNQUOTED so a leading `~` still expands on the remote side, exactly
# as `ssh-macos --push` requires and for the same reason.
printf 'LEG=%s\nLEG_FILTER=%s\nLEG_RUN=%s\nLEG_JOBS=%s\nLEG_GUARDS=%s\nLEG_MODE=%s\nLEG_TREE=%s\nLEG_BUILD_TYPE=%s\n%s\n' \
    "$DST" "'$FILTER'" "'$LEG_RUN'" "'$JOBS'" "'$GUARDS'" \
    "'$MODE'" "'$TREE'" "'$BUILD_TYPE'" "$REMOTE_BODY" \
  | bash "$CARRIAGE" 'bash -s' 2>&1 | tee "$LEG_OUT"
# ⚠ The pipeline's status is `tee`'s, NEVER the leg's. ✔MEASURED at the P33 fold: piping
# a gate through `tail` reported rc=0 while ctest had failed rc=8. The witness below is
# the authority, exactly as `scripts/run-gate` emits one for the same reason.
# ★ The witness NAME is chosen by mode, because the two modes have different
# terminal states and a watcher must observe the one its run can actually reach.
if [ "$MODE" = "build" ]; then WITNESS_KEY="REMOTE_BUILD_RC"; else WITNESS_KEY="REMOTE_CTEST_RC"; fi
WITNESS=$(grep -oE "$WITNESS_KEY\[$LEG_RUN\]=[0-9]+" "$LEG_OUT" | tail -1)
[ -n "$WITNESS" ] || die "no $WITNESS_KEY[$LEG_RUN] witness came back -- this run's real status is UNKNOWN, which is not a pass (log: $LEG_OUT)"
RC=${WITNESS##*=}
grep -E '^PHASE ' "$LEG_OUT"
if [ "$RC" != "0" ]; then die "macOS $MODE leg FAILED (rc=$RC, log $LEG_OUT)"; fi
if [ "$MODE" = "build" ]; then
    say "macOS build OK (run $LEG_RUN, build/$TREE, $BUILD_TYPE) -- no tests were run"
else
    say "macOS leg OK (run $LEG_RUN)"
fi
