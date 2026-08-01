#!/usr/bin/env bash
#
# real-examples/c/sqlite/build-and-test.sh
# ─────────────────────────────────────────────────────────────────────────────
# SQLite UNIT-CORPUS harness for DSS Code Prime — full-source, no amalgamation.
#
# Clone the repo and run ONE command to prove DSS Code Prime builds SQLite from
# its REAL sources (no amalgamation) into the Tcl `testfixture` and runs SQLite's
# own `.test` unit corpus GREEN — on the native host AND on arm64 under qemu.
#
# Windows companion: build-and-test.ps1 does the same for the pe64 leg.
#
# The pipeline, end-to-end on a Linux (or WSL) host — macOS runs the native leg:
#
#   1. verify the host is Linux / WSL / macOS and online
#   2. use the dss-code-prime checkout at ~/src AS-IS on its CURRENT branch —
#      NEVER switched or pulled (a probe tests the working tree exactly as it is);
#      an ABSENT dir is freshly cloned (default branch)
#   3. clone-or-update  sqlite/sqlite    into  ~/src
#   4. configure SQLite + derive the FULL-SOURCE `testfixture` recipe from the
#      canonical `make -n testfixture USE_AMALGAMATION=0` — the exact TU list
#      (every src/*.c + ext/**/*.c + generated parse.c/opcodes.c/ctime.c/… that
#      the reference build compiles), the -D defines, and the sqlite -I dirs.
#      DSS compiles the WHOLE source set (it cannot consume gcc's libsqlite3.a),
#      so the core sources inside that .a are recovered via `ar t`.
#      The reference fixture built along the way is KEPT, as the ATTRIBUTION
#      ORACLE, at <OUT_DIR>/reference-testfixture — copied out of the build tree
#      because deriving the recipe requires DELETING the make target. Step 9
#      prints its path, or says ABSENT when this run produced none.
#   5. build dss-code-prime              (its default CMake-4 Release build)
#   6. stage the third-party HEADERS DSS parses agnostically (the host's REAL tcl
#      headers — whatever version it has — + zlib, NO descriptor — D-FFI-SHIPPED-
#      LIBS-OS-ONLY) and obtain the per-leg LIBS the fixture links + runs against
#      (host libtcl/libz; arm64 via ports .deb extract)
#   7. build the full-source `testfixture` with dss-code-prime, once PER LEG,
#      from a generated `.dss-project.json` manifest (dss --project mode):
#        - host  → the native ELF/Mach-O target (runs directly)
#        - arm64 → elf64-aarch64 (Linux x86_64 host only; runs under qemu-aarch64)
#      each leg's manifest declares c-subset / cli / the leg's
#      <targetName>:<formatName> target / the ~185 TUs (absolute `sources`) / the
#      sqlite+tcl+zlib include dirs / the recipe defines / the leg's host libtcl +
#      libz (resolveLibraries — the arm64 cross leg keeps its own fixed
#      libtcl8.6.so.0 + libz.so.1 from the ports .deb); the build routes the binary
#      to <out>/<leg>/<formatName>/testfixture.
#   8. stage each leg's run dir — including the `libtestloadext.so` extension the
#      loadext corpus dlopen()s, compiled by THE LEG'S TARGET compiler — then run
#      SQLite's `.test` UNIT CORPUS through the dss-built fixture on every
#      runnable leg (DSS_TIER: veryquick[default] | quick | full | all), parse
#      "N errors out of M tests", and classify each failing test against the
#      documented non-DSS confounds (WAL set-lock wall-clock timing, an env
#      error-message text diff, the OOM-oracle recover faults). GREEN = every
#      failure is a known confound (0 genuine DSS miscompiles).
#      The corpus runs the ORIGINAL, 100% sqlite suite: nothing is omitted by
#      default. A fixture ABORT does not end the leg — it is detected, reported,
#      and RESUMED past through sqlite's own `--start=` / SQLITE_TEST_PATTERN_LIST
#      hooks so every remaining unit still reaches a verdict, while the abort
#      itself stays on the record as a failure (see "THE CORPUS RESUME ENGINE").
#   9. summarise + exit non-zero if any leg has a GENUINE (non-confound) unit
#      failure, a compile miss, a fixture abort, or a unit that never ran.
#
# DESIGN: every step is idempotent and FAIL-LOUD. dss-code-prime exits 0 even on
# fatal compile errors, so step 7 reads success from the DIAGNOSTICS (no `error[`
# line) + the emitted binary, never `$?` (probe a6b65f8b).
#
# Overridable via env: DSS_REPO_URL SQLITE_REPO_URL SRC_DIR SQLITE_DIR OUT_DIR
#                      JOBS  DSS_TIER  DSS_LEGS  DSS_CONFOUNDS  DSS_TIER_EXCLUDES
#                      DSS_MAX_RESUMES  DSS_SEGMENT_STALL  DSS_SEGMENT_TIMEOUT
#                      ARM64_LIBDIR  DSS_TCL_VERSION
# ─────────────────────────────────────────────────────────────────────────────
set -Eeuo pipefail

# ── bash 4+ required (associative arrays / `declare -A`) — macOS ships 3.2 ─────
if [ -z "${BASH_VERSINFO:-}" ] || [ "${BASH_VERSINFO[0]:-0}" -lt 4 ]; then
  for _newer_bash in /opt/homebrew/bin/bash /usr/local/bin/bash "$(command -v bash 2>/dev/null || true)"; do
    if [ -n "$_newer_bash" ] && [ -x "$_newer_bash" ] && "$_newer_bash" -c '[ "${BASH_VERSINFO[0]}" -ge 4 ]' 2>/dev/null; then
      exec "$_newer_bash" "$0" "$@"
    fi
  done
  echo "ERROR: this harness needs bash 4+ (found ${BASH_VERSION:-unknown}); on macOS run: brew install bash" >&2
  exit 1
fi

# ── config (override via environment) ────────────────────────────────────────
# dss-code-prime is ALWAYS used at its CURRENT branch — the harness never switches
# or pulls our own repo. SQLite DOES clone-or-pull (external dependency).
DSS_REPO_URL="${DSS_REPO_URL:-git@github.com:dailysoftwaresystems/dss-code-prime.git}"
SQLITE_REPO_URL="${SQLITE_REPO_URL:-git@github.com:sqlite/sqlite.git}"
SRC_DIR="${SRC_DIR:-$HOME/src/dss-code-prime}"
SQLITE_DIR="${SQLITE_DIR:-$HOME/src/sqlite}"
OUT_DIR="${OUT_DIR:-$SRC_DIR/build/real-examples/c/sqlite}"
JOBS="${JOBS:-$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)}"
LANGUAGE="c-subset"
MIN_CMAKE_MAJOR=4
# DSS_TCL_VERSION: OPTIONAL pin for the Tcl the fixture is staged against (e.g.
# "8.6", "9.0"). UNSET — the default — means VERSION-AGNOSTIC: Step 6 discovers
# whatever Tcl the host actually has, from that installation's OWN tclConfig.sh.
# Nothing pins a Tcl version any more: commit 355572d (Cycle TF-C65,
# D-FFI-SHIPPED-LIBS-OS-ONLY) deleted every THIRD-PARTY shipped-lib descriptor, so
# DSS carries no tcl.json that could disagree with the host — it PARSES whatever
# tcl.h Step 6 stages — and sqlite supports Tcl 9 natively (`#if
# TCL_MAJOR_VERSION==9` in src/tclsqlite.c). Set this only to exercise one
# specific Tcl ABI; a pin that is not installed FAILS LOUD in Step 6 rather than
# silently falling back to another version (which would link and then misbehave).
DSS_TCL_VERSION="${DSS_TCL_VERSION:-}"
# DSS_TIER: which unit-corpus tier to run — veryquick (default, ~331k tests, ~6min
# native) | quick | full | all. Bigger tiers take far longer (all.test under qemu is
# hours). DSS_TEST_FILE overrides with a single .test path (fast plumbing check).
DSS_TIER="${DSS_TIER:-veryquick}"
# DSS_CONFIG: the compiler pipeline the testfixture is built with — RELEASE by
# default. This is load-bearing, not just speed: release-only optimizer bugs
# (regalloc/LICM) are exactly what the corpus must catch, so the fixture MUST
# exercise the full optimizer; a debug fixture would run the corpus green while
# masking a release miscompile (and run far slower). Override to `debug` only to
# isolate whether a corpus failure is optimizer-induced.
DSS_CONFIG="${DSS_CONFIG:-release}"
# DSS_CONFOUNDS: ERE patterns (space/newline-separated) for KNOWN non-DSS unit
# failures — a failing test matching any is not counted against green. Defaults
# to the documented set (WAL set-lock wall-clock timing on a fast/uncontended box;
# a zipfile UPSTREAM TEST-ISOLATION LEAK; the recover-fault OOM-oracle class).
# (zipfile-25.0 — MECHANISM PROVEN 2026-07-26, superseding the earlier and WRONG
# "error-message-text env diff" description. symlink.test:163 does `file mkdir x`
# and never removes it; symlink sorts BEFORE zipfile, so by the time zipfile.test
# runs in the SHARED testdir a DIRECTORY named `x` exists. zipfile-25.0 asserts
# `zipfile('x')` fails with "cannot open file: x" — but on Linux fopen() on a
# directory SUCCEEDS, so it fails later in fread() instead. A 4-case probe in ONE
# process, varying only the filesystem, gives: x absent -> "cannot open file: x"
# (expected); x = empty file -> success; x = DIRECTORY -> "error in fread()", the
# corpus string byte-for-byte. One binary, three outcomes => cannot be codegen.
# See D-SQLITE-ZIPFILE25-SYMLINK-TESTDIR-LEAK.)
# (date-2.4c is gcc-EXONERATED — a GCC-built testfixture fails it identically
# [expected NULL, got a date], so it is a sqlite date-format/version/env diff, not
# a DSS miscompile. NOTE: fpconv1-2.0 is DELIBERATELY NOT a confound — a debug DSS
# fixture + a GCC fixture both pass it while a release DSS fixture fails all 500k
# values, so it is a GENUINE release-optimizer fp miscompile [D-OPT-SQLITE-FPCONV1-
# RELEASE-FP-MISCOMPILE] the harness MUST flag red until fixed.)
# A pattern may be SCOPED by execution mode: `emulated:<re>` / `native:<re>`; bare
# means every leg. `emulated:^writecrash-` is excused ONLY on a leg that needs a
# runner (qemu), because the emulator injects "qemu: uncaught target signal 6" into
# the output sqlite's crash harness compares — PROVEN by a gcc-built aarch64
# abort() emitting that exact string, no DSS involved. The native host leg passes
# all 988 writecrash assertions, so this must NEVER be excused there.
# ^walsetlk_recover- is a SEPARATE pattern on purpose: `^walsetlk-` must NOT sweep
# it in by family resemblance (different test FILE), so it is excused only by its
# own row, earned by its own control. EVIDENCE: the GCC-built full-source reference
# testfixture fails it IDENTICALLY — 6 runs each, same machine, same sqlite tree,
# both linking the same libtcl8.6.so: gcc 4 pass / 2 fail (tm -35334590, -35331430),
# dss 3 pass / 3 fail (tm -35335818, +36338628, -35333129). Same intermittency, same
# ~35.3 s magnitude, both signs. See D-SQLITE-WALSETLK-RECOVER-NEGATIVE-ELAPSED.
# (loadext-2.1 / loadext-2.2 — ADDED 2026-07-31 (TF-C103), the first confounds this
# harness EARNED using its own reference oracle, and the reason the oracle was
# repaired the cycle before. They are the only two non-confound failures in the whole
# `full` tier: 2 out of 1,061,550 tests. MECHANISM, MEASURED and not inferred:
# loadext.test:64-65 hardcodes the macOS dyld error string as
# `{dlopen.%s, 10.: .*image.*found.*}`, and macOS 26.5.2 (build 25F84) changed that
# string in TWO independent ways — the dlopen flags now print in HEX (`0x000A`) where
# the regex wants DECIMAL (`10`), and the phrase is now `(no such file)` inside a
# `tried: <path>, <path>, …` enumeration where the regex wants `image not found`.
# EVIDENCE — this is exoneration, not resemblance: the reference cc-built full-source
# testfixture, which contains no DSS-compiled code at all, fails the SAME TWO tests on
# the SAME machine and sqlite tree — `2 errors out of 52 tests`, `!Failures on these
# tests: loadext-2.1 loadext-2.2` — and emits the identical new-format string. A
# compiler cannot change what dyld prints. ⚠ SCOPED DELIBERATELY NARROW: anchored to
# these two test NAMES, NOT to `^loadext-`, so a real DSS defect anywhere else in
# loadext.test still fails loud — the extension-loading path is exactly where a
# codegen or linkage defect would plausibly show up. Revisit when sqlite updates the
# regex upstream or the macOS string changes again. See
# D-SQLITE-LOADEXT-MACOS26-DLERROR-FORMAT.)
# (^busy2- and ^recoverfault — CONTROL RECORDED 2026-08-01 (TF-C108). Until this
# date these were the only two patterns here carrying a DESCRIPTION but no MEASURED
# matched control, i.e. they were asserted rather than earned. They are now earned,
# by one experiment: the DSS-built and the gcc-built REFERENCE testfixture ran
# `full.test` CONCURRENTLY on the same box and the same sqlite tree (23 min; DSS 12
# errors / 1,061,995, reference 8 errors / 1,062,222). ^recoverfault gets the
# strongest possible control — the SAME FOUR NAMES failed in BOTH fixtures
# (recoverfault-{1,2}-oom-{persistent,transient}.{459,460}), and a compiler cannot
# change which allocation an OOM oracle stops at. ^busy2- is earned as a CLASS: both
# fixtures failed a busy2 wall-clock assertion, DIFFERENT members each (DSS
# busy2-2.2.5, reference busy2-1.2.3) — which is the signature of an intermittent
# environment effect, not of codegen. A SECOND, INDEPENDENT datum points the same
# way: in the 2026-07-31 `full` run the SAME name, busy2-2.1.3, failed on the host
# x86_64 leg AND on the arm64-under-qemu leg — two different code generators and two
# different instruction sets producing the identical timing failure in one run. The same run also re-validated ^walsetlk- (DSS
# 2.1.3/2.1.12/2.1.14/2.2.6/2.2.8, reference 2.1.8), ^walsetlk_recover- (BOTH negative
# and near-identical: DSS -33,932,622 us, reference -33,923,905 us) and ^zipfile-25.0
# (both). ⓘ WHY THE WHOLE FAMILY MOVES TOGETHER: every one of these assertions takes
# its number from Tcl's `time`, which lives entirely outside DSS-compiled code —
# VERIFIED, not assumed: ldd resolves libtcl8.6.so to the SAME
# /lib/x86_64-linux-gnu/libtcl8.6.so for both fixtures, and `nm -D --defined-only`
# finds Tcl_GetTime defined in NEITHER fixture while that .so exports it; and
# sqlite's own unixSleep
# (src/os_unix.c) calls nanosleep(&sp, NULL) with no EINTR retry while
# sqliteDefaultBusyCallback credits the NOMINAL delay from its totals[] table, so a
# caught signal shortens the real wait with the handler none the wiser.
# ★★ ROOT CAUSE, MEASURED THE SAME DAY — THE CLOCK ON THIS BOX IS BROKEN, AND IT IS
# NOT A COMPILER PROBLEM. CLOCK_REALTIME oscillates between two fixed values ~34.47 s
# apart, flipping every ~5 s. Two independent instruments: a shell sampler (791
# intervals) saw 49 steps, max +34.4495 / min -34.4496 s; a single-process probe
# calling clock_gettime(CLOCK_REALTIME) and clock_gettime(CLOCK_MONOTONIC) back to
# back at 20 Hz (2,382 samples) saw 48 steps, with (realtime - monotonic) — a
# CONSTANT unless realtime is stepped — taking exactly two values, SPREAD 35.164 s.
# The magnitude is the smoking gun: EVERY walsetlk_recover-1.3 negative elapsed ever
# recorded (-33,932,622 / -33,923,905 / -33,969,790 / -35,334,590 / -35,331,430 /
# -35,335,818 us) falls inside that spread. A test that reads the clock either side
# of a wait and straddles a flip mismeasures it by +/-34.5 s. This is why the whole
# family moves together and why the gcc reference suffers it identically. Tracked by
# D-ENV-WSL2-CLOCK-REALTIME-STEPS-34S; when the clock is fixed, several of the
# patterns below may simply evaporate — re-evaluate then and NARROW this list.
# ⚠ SCOPE NOTE:
# ^busy2- stays a whole-FILE pattern, which is broader than the per-NAME standard set
# by loadext-2.1/2.2; narrowing it needs per-name controls that do not exist yet. And
# misc7-7.0, which fails by the SAME mechanism, is deliberately NOT added here —
# family resemblance is not a control. See D-SQLITE-MISC7-BUSYTIMEOUT-DELAY-WINDOW-
# GENUINE and D-SQLITE-WALSETLK-RECOVER-NEGATIVE-ELAPSED.)
DSS_CONFOUNDS="${DSS_CONFOUNDS:-^walsetlk- ^walsetlk\. ^walsetlk_recover- ^busy2- ^zipfile-25\.0$ ^recoverfault ^date-2\.4c$ ^loadext-2\.1$ ^loadext-2\.2$ emulated:^writecrash-}"
# DSS_TIER_EXCLUDES: space-separated regexes naming .test FILES to drop from the
# tier. Delivered through SQLite's OWN upstream hook — the QUICKTEST_OMIT env var
# read by test/permutations.test (~line 152): a COMMA-separated list of Tcl regexes
# matched against each test file's tail name and subtracted from `$allquicktests`,
# the set every permutation in all.test is derived from EXCEPT `full` (which uses
# `$alltests`, so an excluded file still runs ONCE there). A confound EXPLAINS a
# failing test; an exclusion REMOVES a file from the run — a real coverage
# reduction, so it is echoed before the run and appended to every leg's verdict in
# Step 9.
#
# ★★ DEFAULT EMPTY — ALWAYS, ON EVERY TIER AND EVERY LEG. The requirement is that
# the ORIGINAL, 100% sqlite test suite runs, unmodified and with nothing omitted.
# The mechanism survives only as an operator escape hatch. An excluded file is NOT
# how the harness survives an aborting unit — that is the RESUME ENGINE's job
# (Step 8): an abort is detected, reported, and resumed past via sqlite's own
# `--start=` / SQLITE_TEST_PATTERN_LIST hooks, so every remaining unit still
# reaches a verdict while the abort itself stays on the record as a FAILURE.
# Excluding a file deletes coverage; resuming preserves it.
DSS_TIER_EXCLUDES="${DSS_TIER_EXCLUDES:-}"
# DSS_MAX_RESUMES: hard bound on how many times Step 8 may re-invoke a leg's
# fixture after an abort. Exceeded → the harness STOPS and says so; it never
# loops, and never masks a fixture that aborts on everything.
DSS_MAX_RESUMES="${DSS_MAX_RESUMES:-10}"
# DSS_SEGMENT_STALL: seconds a segment may produce NO log output before the harness
# declares it HUNG, kills it, and resumes past it. A stall bound (not a wall-clock
# bound) is the right shape here: the tiers differ by orders of magnitude — `all`
# runs ~2.5 h — but output is continuous within any of them, so a silent fixture is
# the signal. MEASURED headroom: the slowest single test FILE in a real `all` run is
# sort4.test at 306 s, and the fixture prints a line per TEST, not per file, so real
# output gaps are far shorter still. 1800 s is ~6x the slowest file.
DSS_SEGMENT_STALL="${DSS_SEGMENT_STALL:-1800}"
# DSS_SEGMENT_TIMEOUT: optional ABSOLUTE per-segment wall-clock cap in seconds.
# 0 = disabled (the default) — the stall bound above is the one that generalises;
# an absolute cap has to be re-tuned for every tier.
DSS_SEGMENT_TIMEOUT="${DSS_SEGMENT_TIMEOUT:-0}"
# DSS_KILL_SETTLE: seconds to wait, after killing a hung segment, for the OS to
# actually release its file handles before the next segment starts. MEASURED: with
# no settle the next segment dies at tester.tcl's startup `reset_db` with
# `error deleting "test.db": permission denied` — the harness manufacturing its own
# next failure out of the previous kill.
DSS_KILL_SETTLE="${DSS_KILL_SETTLE:-20}"

# ── host identification (OS + arch) ──────────────────────────────────────────
HOST_OS=""
case "$(uname -s)" in
  Linux)  HOST_OS="linux"  ;;
  Darwin) HOST_OS="macos"  ;;
esac
HOST_ARCH=""
case "$(uname -m)" in
  arm64|aarch64) HOST_ARCH="arm64"  ;;
  x86_64|amd64)  HOST_ARCH="x86_64" ;;
esac
# the host's native target spec — a full `<targetName>:<formatName>` pair (what
# both the CLI --target and a project manifest's targets[] require), keyed by
# (OS, arch). targetName ∈ {x86_64, arm64} (a shipped .target.json); formatName
# is the shipped exec object-format for that arch+OS.
host_target_spec() {
  case "$HOST_OS/$HOST_ARCH" in
    linux/x86_64) echo "x86_64:elf64-x86_64-linux-exec"   ;;
    linux/arm64)  echo "arm64:elf64-aarch64-linux-exec"   ;;
    macos/arm64)  echo "arm64:macho64-arm64-darwin-exec"  ;;
    macos/x86_64) echo "x86_64:macho64-x86_64-darwin-exec";;
  esac
}

# ── LEGS: label -> "spec|runner-prefix|libsrc|cc|cc-pkg" ─────────────────────
# The "host" leg is always the native target (runs directly). On a Linux x86_64
# host the "arm64" cross leg is added: elf64-aarch64 compiled here + RUN under
# user-mode qemu-aarch64 (QEMU_LD_PREFIX → the aarch64 sysroot; LD_LIBRARY_PATH →
# the staged arm64 tcl/zlib). libsrc selects which tcl/zlib libraries the leg's
# fixture links + runs against ("host" | "arm64"). cc is the leg's TARGET C
# compiler — a per-leg target fact exactly like the runner prefix and libsrc, used
# to build the corpus's dlopen()ed helper extension FOR THE LEG (step 8); cc-pkg is
# the apt package providing it (empty = already ensured by step 1).
QEMU_SYSROOT="${QEMU_SYSROOT:-/usr/aarch64-linux-gnu}"
declare -A LEG_SPEC=() LEG_PREFIX=() LEG_LIBSRC=() LEG_CC=() LEG_CC_PKG=()
declare -a LEG_ORDER=()
add_leg() {                    # add_leg <label> <spec> <runner-prefix> <libsrc> <cc> [<cc-apt-pkg>]
  LEG_ORDER+=("$1"); LEG_SPEC["$1"]="$2"; LEG_PREFIX["$1"]="$3"; LEG_LIBSRC["$1"]="$4"
  LEG_CC["$1"]="$5"; LEG_CC_PKG["$1"]="${6:-}"
}
_hspec="$(host_target_spec)"
[[ -n "$_hspec" ]] && add_leg "host" "$_hspec" "" "host" "${CC:-cc}"
if [[ "$HOST_OS" == "linux" && "$HOST_ARCH" == "x86_64" ]]; then
  add_leg "arm64" "arm64:elf64-aarch64-linux-exec" "qemu-aarch64" "arm64" \
          "aarch64-linux-gnu-gcc" "gcc-aarch64-linux-gnu"
fi
# DSS_LEGS: comma-separated filter (e.g. DSS_LEGS=host) for fast iteration.
if [[ -n "${DSS_LEGS:-}" ]]; then
  declare -a _filtered=()
  for _l in "${LEG_ORDER[@]}"; do
    case ",${DSS_LEGS}," in *",${_l},"*) _filtered+=("$_l");; esac
  done
  [[ ${#_filtered[@]} -gt 0 ]] || { echo "DSS_LEGS='${DSS_LEGS}' matched no leg (have: ${LEG_ORDER[*]})" >&2; exit 2; }
  LEG_ORDER=("${_filtered[@]}")
fi

# ─────────────────────────────────────────────────────────────────────────────
# SHARED-CLONE READER/WRITER LOCK
# ─────────────────────────────────────────────────────────────────────────────
# The output-tree lock (Step 2b) is not enough, because the two drivers SHARE one
# sqlite checkout and consume it differently:
#   · build-and-test.sh  runs the .test corpus DIRECTLY out of the clone, for the
#     whole run — measured: a qemu-aarch64 fixture 2 h 14 m into full.test with
#     /home/rafael/src/sqlite/test/full.test open.
#   · build-and-test.ps1 fetch/checkout/pull --rebases that same clone during
#     staging, then copies it to a Windows stage.
# Their OUTPUT trees differ, so both output locks are free and nothing stops the
# .ps1 from rewriting .test files under the live arm64 fixture — silently, which is
# the same failure class as the one the output lock already closes.
#
# MODEL — reader/writer, because the access patterns genuinely differ:
#   WRITE  short, mutating: the .ps1's whole staging step; the .sh's Steps 3-7
#          (clone_or_update pulls, configure/make generate sources, Step 6 writes
#          $BLD/zinc INSIDE the clone).
#   READ   long, read-only: the .sh's Step 8 corpus run (hours). The .sh takes the
#          write lock first and DOWNGRADES to a read marker before Step 8, so the
#          hours-long window blocks a mutator without blocking another reader.
#
# A live holder FAILS LOUD; it does not block. Deliberate: these runs are unattended
# for hours, and silently waiting 2.5 h is a worse outcome than a refusal that names
# the holder and its age. Staleness is liveness-based (PID + start marker), so a
# crashed run never wedges the next one — its lock is stolen and the theft REPORTED.
#
# The lock state lives OUTSIDE the clone (never write into a checkout the harness
# also pulls), keyed on the clone's real path so both drivers derive the same key.
#
# >>> dss:clone-lock >>>  (self-contained on purpose: build-and-test.ps1 EXTRACTS
# this region by these sentinels and injects it into its WSL staging script, so both
# drivers run literally the same lock code and cannot drift. Use only printf/exit
# here — the .sh's step/info/warn/die helpers do not exist in that context.)
DSS_CLONE_LOCK_DIR=""; DSS_CLONE_ROLE=""; DSS_CLONE_NOTES=""
dss_clone_lock_key() {         # dss_clone_lock_key <clone-path>
  local p; p="$(cd "$1" 2>/dev/null && pwd -P)" || p="$1"
  printf '%s/dss-code-prime/clone-locks/%s' "${XDG_CACHE_HOME:-$HOME/.cache}" \
    "$(printf '%s' "$p" | tr -c 'A-Za-z0-9._-' '_')"
}
dss_proc_marker() { ps -p "$1" -o lstart= 2>/dev/null | tr -s ' ' || true; }
dss_holder_alive() {           # dss_holder_alive <owner-file>
  [[ -f "$1" ]] || return 1
  local pid mark
  pid="$(sed -n '1p' "$1" 2>/dev/null || true)"
  mark="$(sed -n '2p' "$1" 2>/dev/null || true)"
  [[ -n "$pid" ]] && kill -0 "$pid" 2>/dev/null && [[ "$(dss_proc_marker "$pid")" == "$mark" ]]
}
dss_holder_desc() {            # dss_holder_desc <owner-file>  — names it AND its age
  local pid since what now age
  pid="$(sed -n '1p' "$1" 2>/dev/null || true)"
  since="$(sed -n '3p' "$1" 2>/dev/null || true)"
  what="$(sed -n '4p' "$1" 2>/dev/null || true)"
  now="$(date +%s)"
  case "$since" in ''|*[!0-9]*) since="$now" ;; esac   # a corrupt marker must not abort the run
  age=$(( now - since ))
  printf 'pid %s — %s — holding for %dh%02dm' "${pid:-?}" "${what:-unknown}" "$((age / 3600))" "$(((age % 3600) / 60))"
}
dss_write_owner() {            # dss_write_owner <what> <owner-file>
  printf '%s\n%s\n%s\n%s\n' "$$" "$(dss_proc_marker $$)" "$(date +%s)" "$1" > "$2"
}
dss_clone_lock_fail() {        # dss_clone_lock_fail <headline> <clone> <holder-desc>
  printf 'DSS-CLONE-LOCK-BLOCKED\n' >&2
  printf '\n [X] ERROR: %s\n' "$1" >&2
  printf '      clone  : %s\n' "$2" >&2
  printf '      held by: %s\n' "$3" >&2
  printf '      Both drivers share this checkout: build-and-test.sh runs the .test corpus\n' >&2
  printf '      DIRECTLY out of it for its whole run, while build-and-test.ps1 fetch/pull/\n' >&2
  printf '      checkouts it during staging. Running both at once rewrites .test files under\n' >&2
  printf '      a live fixture — silently, and the corrupted run still reports a verdict.\n' >&2
  printf '      Wait for the holder above, or point this run at a different checkout:\n' >&2
  printf '        .sh   SQLITE_DIR=/path/to/another/sqlite\n' >&2
  printf '        .ps1  $env:SQLITE_WSL_DIR=/path/to/another/sqlite\n' >&2
  exit 3
}
dss_clone_lock_write() {       # dss_clone_lock_write <clone-path> <what>
  local ld w r live="" tries=0
  ld="$(dss_clone_lock_key "$1")"; w="$ld/w.lock"
  mkdir -p "$ld/readers"
  while ! mkdir "$w" 2>/dev/null; do
    dss_holder_alive "$w/owner" && \
      dss_clone_lock_fail "another dss harness run is MUTATING this sqlite clone" "$1" "$(dss_holder_desc "$w/owner")"
    DSS_CLONE_NOTES="${DSS_CLONE_NOTES}stole a STALE clone WRITE lock (its holder is gone); "
    rm -rf "$w"
    tries=$((tries + 1))
    [[ $tries -lt 3 ]] || dss_clone_lock_fail "could not take the clone write lock after 3 attempts" "$1" "see $w"
  done
  dss_write_owner "$2" "$w/owner"
  # drain readers: a long corpus run holds the clone read-only for hours
  for r in "$ld/readers"/*.reader; do
    [[ -e "$r" ]] || continue
    if dss_holder_alive "$r"; then live="$live$(dss_holder_desc "$r"); "
    else DSS_CLONE_NOTES="${DSS_CLONE_NOTES}removed a STALE clone READ marker (${r##*/}); "; rm -f "$r"; fi
  done
  if [[ -n "$live" ]]; then
    rm -rf "$w"
    dss_clone_lock_fail "this sqlite clone is being READ by a corpus run in progress" "$1" "$live"
  fi
  DSS_CLONE_LOCK_DIR="$ld"; DSS_CLONE_ROLE=write
}
dss_clone_lock_read() {        # dss_clone_lock_read <clone-path> <what>  (may downgrade)
  local ld w; ld="$(dss_clone_lock_key "$1")"; w="$ld/w.lock"
  mkdir -p "$ld/readers"
  if [[ "$DSS_CLONE_ROLE" != write ]]; then
    dss_holder_alive "$w/owner" && \
      dss_clone_lock_fail "another dss harness run is MUTATING this sqlite clone" "$1" "$(dss_holder_desc "$w/owner")"
    [[ ! -d "$w" ]] || { DSS_CLONE_NOTES="${DSS_CLONE_NOTES}stole a STALE clone WRITE lock (its holder is gone); "; rm -rf "$w"; }
  fi
  dss_write_owner "$2" "$ld/readers/$$.reader"
  if [[ "$DSS_CLONE_ROLE" == write ]]; then
    rm -rf "$w"                                  # downgrade: marker first, then release
  elif dss_holder_alive "$w/owner"; then         # a writer won the race — back out
    rm -f "$ld/readers/$$.reader"
    dss_clone_lock_fail "a mutating run took this sqlite clone first" "$1" "$(dss_holder_desc "$w/owner")"
  fi
  DSS_CLONE_LOCK_DIR="$ld"; DSS_CLONE_ROLE=read
}
dss_clone_lock_release() {
  [[ -n "$DSS_CLONE_LOCK_DIR" ]] || return 0
  case "$DSS_CLONE_ROLE" in
    write) rm -rf "$DSS_CLONE_LOCK_DIR/w.lock" ;;
    read)  rm -f  "$DSS_CLONE_LOCK_DIR/readers/$$.reader" ;;
  esac
  DSS_CLONE_ROLE=""
}
# <<< dss:clone-lock <<<

# ── logging / fail-loud ──────────────────────────────────────────────────────
if [[ -t 1 ]]; then
  C_RST=$'\033[0m'; C_RED=$'\033[31m'; C_GRN=$'\033[32m'; C_YLW=$'\033[33m'; C_BLU=$'\033[34;1m'
else
  C_RST=; C_RED=; C_GRN=; C_YLW=; C_BLU=
fi
step()  { printf '\n%s== %s ==%s\n' "$C_BLU" "$*" "$C_RST"; }
info()  { printf '   %s\n' "$*"; }
pass()  { printf '%s ✓ %s%s\n' "$C_GRN" "$*" "$C_RST"; }
warn()  { printf '%s ! %s%s\n' "$C_YLW" "$*" "$C_RST"; }
die()   { printf '%s ✗ ERROR: %s%s\n' "$C_RED" "$*" "$C_RST" >&2; exit 1; }
trap 'die "failed at line $LINENO (command: $BASH_COMMAND)"' ERR

# ── package install helpers (apt on Linux/WSL, Homebrew on macOS) ─────────────
SUDO=""; [[ "$(id -u)" -eq 0 ]] || SUDO="sudo"
APT_UPDATED=0
pkg_install() {                 # pkg_install <apt-pkg> [<brew-pkg=apt-pkg>]
  local apt_pkg="$1" brew_pkg="${2:-$1}"
  if [[ "$HOST_OS" == "macos" ]]; then
    command -v brew >/dev/null 2>&1 || die "Homebrew not found — install from https://brew.sh, then re-run (needed for: $brew_pkg)."
    info "installing (brew): $brew_pkg"
    brew list "$brew_pkg" >/dev/null 2>&1 || brew install "$brew_pkg"
  else
    command -v apt-get >/dev/null 2>&1 || die "apt-get not found — this harness targets Debian/Ubuntu/WSL + macOS. Install manually: $apt_pkg"
    if [[ "$APT_UPDATED" -eq 0 ]]; then $SUDO apt-get update -y >/dev/null; APT_UPDATED=1; fi
    info "installing (apt): $apt_pkg"
    $SUDO DEBIAN_FRONTEND=noninteractive apt-get install -y "$apt_pkg" >/dev/null
  fi
}
ensure_cmd() {                  # ensure_cmd <command> <apt-pkg> [<brew-pkg>]
  command -v "$1" >/dev/null 2>&1 || pkg_install "$2" "${3:-$2}"
}

default_branch() {              # origin/HEAD (sqlite's may be master/trunk, not main)
  local r=""
  r="$(git -C "$1" symbolic-ref --short refs/remotes/origin/HEAD 2>/dev/null)" || true
  printf '%s' "${r#origin/}"
}
clone_or_update() {             # clone_or_update <url> <dir> <wanted-branch-or-empty>
  local url="$1" dir="$2" want="${3:-}"
  if [[ -d "$dir/.git" ]]; then
    info "updating $(basename "$dir") in $dir"
    git -C "$dir" fetch --all --prune --quiet
    local branch="${want:-$(default_branch "$dir")}"
    [[ -n "$branch" ]] || branch="$(git -C "$dir" remote show origin | sed -n 's/.*HEAD branch: //p')"
    git -C "$dir" checkout --quiet "$branch"
    git -C "$dir" pull --rebase --quiet
  else
    info "cloning $url -> $dir"
    mkdir -p "$(dirname "$dir")"
    git clone --quiet "$url" "$dir"
    [[ -z "$want" ]] || git -C "$dir" checkout --quiet "$want"
  fi
  info "  at $(git -C "$dir" rev-parse --short HEAD) on $(git -C "$dir" rev-parse --abbrev-ref HEAD)"
}

# ── Step 0 — SELF-TEST the driver's own late-stage logic ─────────────────────
# >>> dss:selftest >>>
# WHY THIS EXISTS, AND WHY IT RUNS BEFORE ANYTHING EXPENSIVE.
# The classifier runs at the very END of a run — after the build and after hours
# of corpus. A defect there is invisible until everything else has already been
# paid for. That is not hypothetical: a top-level `local` in the classifier aborted
# a COMPLETED 13-hour arm64 run at the classification step, with the whole corpus
# already executed (D-HARNESS-TEST-SCOPE-FIDELITY). `bash -n` cannot catch it —
# a top-level `local` is syntactically valid and only fails at runtime.
#
# So the driver now REFUSES TO START if its own end-of-run logic is broken. The
# check reuses test-confound-scope.sh, which EXTRACTS the shipped classifier and
# executes it at top level under these same shell options — no duplicated logic to
# drift, and it is already demonstrated red-on-disable. Cost: well under a second.
#
# This matters most on a NEW HOST (macOS, a fresh Linux box), where the first run
# is the one you least want to lose. A portability defect in the late-stage code
# surfaces here, in seconds, instead of after a multi-hour corpus.
# Set DSS_SKIP_SELFTEST=1 to bypass (not recommended; say why in the log).
_selftest="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/test-confound-scope.sh"
if [[ "${DSS_SKIP_SELFTEST:-0}" == "1" ]]; then
  warn "driver self-test SKIPPED (DSS_SKIP_SELFTEST=1) — a late-stage defect will not surface until the end of the run."
elif [[ ! -f "$_selftest" ]]; then
  die "driver self-test missing: $_selftest
      This guard is what stops a defect in the END-OF-RUN classifier from costing
      you the entire run. Restore the file, or set DSS_SKIP_SELFTEST=1 knowing that
      a classifier fault will surface only after the corpus has finished."
else
  # ★ "$BASH", never a bare `bash` (D-HARNESS-SELFTEST-BSD-SED-PORTABILITY).
  # A bare `bash` is resolved through PATH, which on macOS is /bin/bash 3.2 —
  # NOT the bash 4+ this driver just re-exec'd itself into (line 67). The
  # self-test needs 4+ (`declare -A`), so under 3.2 it died at its first
  # associative array having run ZERO assertions — and still exited 0, so this
  # guard reported "OK" while proving nothing (an INERT guard, worse than a
  # failing one). "$BASH" is the absolute path of the interpreter actually
  # running, so the self-test always gets the same validated shell. Correct on
  # Linux too, where it simply resolves to the same bash that is already running.
  if _st_out="$("$BASH" "$_selftest" 2>&1)"; then
    info "driver self-test: OK ($(printf '%s\n' "$_st_out" | sed -n 's/^passed=\([0-9]*\).*/\1/p') assertions)"
  else
    printf '%s\n' "$_st_out" | sed 's/^/      /' >&2
    die "DRIVER SELF-TEST FAILED — refusing to start.
      The end-of-run classifier is broken, so this run would execute the whole corpus
      (hours) and then abort while classifying. Fix the driver first; the output above
      names the failing assertion."
  fi
fi
# <<< dss:selftest <<<

# ── Step 1 — host supported + online ─────────────────────────────────────────
step "1/9  Host check (Linux / WSL / macOS, online)"
[[ -n "$HOST_OS"   ]] || die "unsupported host OS — uname -s = '$(uname -s)' (need Linux/WSL or macOS/Darwin)."
[[ -n "$HOST_ARCH" ]] || die "unsupported host arch — uname -m = '$(uname -m)' (need arm64/aarch64 or x86_64)."
[[ ${#LEG_ORDER[@]} -gt 0 ]] || die "no runnable leg for this host ($HOST_OS/$HOST_ARCH)."
if [[ "$HOST_OS" == "macos" ]]; then
  info "host: macOS ($HOST_ARCH, $(uname -r))"
elif grep -qiE 'microsoft|wsl' /proc/version 2>/dev/null; then
  info "host: WSL ($HOST_ARCH, $(uname -r))"
else
  info "host: native Linux ($HOST_ARCH, $(uname -r))"
fi
info "legs: ${LEG_ORDER[*]}   tier: $DSS_TIER"
ensure_cmd curl curl
curl -fsS --max-time 20 -o /dev/null https://github.com || die "offline — cannot reach https://github.com."
pass "$HOST_OS/$HOST_ARCH host is online"
ensure_cmd git git
if [[ "$HOST_OS" == "macos" ]]; then
  command -v cc >/dev/null 2>&1 || die "no C compiler (cc) — run 'xcode-select --install'."
  command -v make >/dev/null 2>&1 || die "no 'make' — run 'xcode-select --install'."
else
  ensure_cmd gcc build-essential
  ensure_cmd make build-essential
  ensure_cmd ar binutils
fi

# ── Step 2 — dss-code-prime (current checkout, untouched) ────────────────────
if [[ -d "$SRC_DIR/.git" ]]; then
  step "2/9  Use dss-code-prime at $SRC_DIR (current checkout, untouched)"
  info "  at $(git -C "$SRC_DIR" rev-parse --short HEAD) on $(git -C "$SRC_DIR" rev-parse --abbrev-ref HEAD)"
else
  step "2/9  Clone dss-code-prime -> $SRC_DIR (absent — fresh clone, default branch)"
  clone_or_update "$DSS_REPO_URL" "$SRC_DIR" ""
fi
pass "dss-code-prime ready"

# ── Step 2b — take the RUN LOCK on the output tree ───────────────────────────
# MEASURED FAILURE (2026-07-26, on the pe64 twin of this harness) this exists to
# prevent: two harness invocations shared one output tree. Invocation B deleted and
# re-staged the .test corpus invocation A's fixture was still sourcing files from,
# mid-run, and B then died trying to delete a testfixture binary A was still
# EXECUTING. The evidence was unambiguous: A's run dir and its process both dated
# 09:49:23, and A's corpus log was still growing (79 MB) at 11:01. So the harness
# must be single-instance per output tree — and silently corrupting a live 2.5 h
# run is the worse half of that bug.
#
# The lock is LIVENESS-BASED, so a crashed run never wedges the next one: it records
# the owning PID + that process's start marker, and a later invocation steals it
# (and SAYS so) when the owner is gone. Correctness never depends on a release.
# >>> dss:run-lock >>>
mkdir -p "$OUT_DIR"
LOCK_DIR="$OUT_DIR/.harness-lock"
LOCK_FILE="$LOCK_DIR/owner.txt"
LOCK_STOLEN=""
proc_start_marker() {          # a PID's start time — the PID-reuse guard
  ps -p "$1" -o lstart= 2>/dev/null | tr -s ' ' || true
}
self_marker="$(proc_start_marker $$)"
for _try in 1 2; do
  # `mkdir` is the portable atomic test-and-set; -p would defeat it.
  if mkdir "$LOCK_DIR" 2>/dev/null; then break; fi
  owner_pid=""; owner_mark=""
  if [[ -f "$LOCK_FILE" ]]; then
    owner_pid="$(sed -n '1p' "$LOCK_FILE" 2>/dev/null || true)"
    owner_mark="$(sed -n '2p' "$LOCK_FILE" 2>/dev/null || true)"
  fi
  if [[ -n "$owner_pid" ]] && kill -0 "$owner_pid" 2>/dev/null \
     && [[ "$(proc_start_marker "$owner_pid")" == "$owner_mark" ]]; then
    die "another dss sqlite harness run is ALREADY ACTIVE on this output tree.
      output tree : $OUT_DIR
      owner PID   : $owner_pid  (still running)
      Two invocations here corrupt each other: one re-stages the .test corpus the
      other run's fixture is sourcing from, and neither can replace a testfixture
      binary that is still executing. Wait for it, or set OUT_DIR elsewhere.
      If you are certain that PID is dead, remove: $LOCK_DIR"
  fi
  LOCK_STOLEN="${owner_pid:-an unreadable lock}"
  rm -rf "$LOCK_DIR"
done
printf '%s\n%s\n%s\n' "$$" "$self_marker" "$(date -Is 2>/dev/null || date)" > "$LOCK_FILE"
[[ -z "$LOCK_STOLEN" ]] || warn "took over a STALE run lock left by PID $LOCK_STOLEN (that run died without releasing it) — reported in the verdict."
info "run lock: $LOCK_DIR (pid $$)"
# <<< dss:run-lock <<<

# ── Step 3 — sqlite ──────────────────────────────────────────────────────────
step "3/9  Fetch sqlite/sqlite -> $SQLITE_DIR (default branch)"
# Take the clone WRITE lock before the first thing that mutates it (the pull), and
# release it however this script ends. Steps 3-7 all mutate: clone_or_update pulls,
# configure/make generate sources, Step 6 writes $BLD/zinc inside the checkout.
trap dss_clone_lock_release EXIT
dss_clone_lock_write "$SQLITE_DIR" "$(basename "$0") staging/build — tier $DSS_TIER, legs ${LEG_ORDER[*]}"
info "clone lock: WRITE on $SQLITE_DIR  ($DSS_CLONE_LOCK_DIR)"
clone_or_update "$SQLITE_REPO_URL" "$SQLITE_DIR" ""
[[ -f "$SQLITE_DIR/test/$DSS_TIER.test" || -n "${DSS_TEST_FILE:-}" ]] || \
  die "tier '$DSS_TIER' has no $SQLITE_DIR/test/$DSS_TIER.test (expected veryquick|quick|full|all)."
pass "sqlite ready"

# ─────────────────────────────────────────────────────────────────────────────
# SHARED THIRD-PARTY DISCOVERY (roots + the Tcl inventory)
# ─────────────────────────────────────────────────────────────────────────────
# This block sits ABOVE Step 4 on purpose. It used to live inside Step 6, but the
# INTERPRETER is part of the Tcl choice, not a separate decision: sqlite's
# `configure`, `make -n` and mksqlite3c.tcl all run under the `tclsh` Step 4 puts
# on PATH, and the recipe they emit bakes THAT Tcl's `-I` dirs in. Selecting the
# headers/library in Step 6 from a different Tcl than Step 4's interpreter builds
# the fixture against two Tcls at once. So Step 4 and Step 6 now share ONE
# inventory and ONE selector.
#
# Portable multi-root find. A hardcoded start-dir list mixes Linux + macOS paths
# (e.g. `/opt/homebrew/*`, `/usr/lib64`); `find` EXITS NON-ZERO on a start dir
# that does not exist on this host, which under `set -Eeuo pipefail` would abort
# the whole harness. Restrict the search to the roots that EXIST (a genuinely
# absent header/lib is still caught by the explicit `… || die` checks later).
# Usage: find_in <dir>... -- <find-expr>...   (stderr suppressed).
find_in() {
  local -a roots=()
  while [[ $# -gt 0 && "$1" != "--" ]]; do [[ -d "$1" ]] && roots+=("$1"); shift; done
  [[ "${1:-}" == "--" ]] && shift
  [[ ${#roots[@]} -gt 0 ]] || return 0
  find "${roots[@]}" "$@" 2>/dev/null
}
# macOS keeps NONE of this material where Linux does, so the root lists below get
# two macOS-only sources APPENDED. Both are pure additions: `xcrun` and `brew` do
# not exist on Linux, so the helpers print nothing there, and `find_in` then drops
# the empty candidate — every root list stays byte-identical to its pre-macOS form
# on a Linux host, and the legacy roots keep their original PRECEDENCE by staying
# first.
#   · the Xcode SDK. macOS has NO /usr/include AT ALL; the system C headers —
#     zlib.h/zconf.h among them — live under `xcrun --show-sdk-path`/usr/include.
#     Only the SDK's INCLUDE dir is taken: its lib/ holds `.tbd` TEXT stubs, not
#     Mach-O, and a .tbd handed to DSS as a --resolve-library cannot be read.
#   · KEG-ONLY Homebrew prefixes. A keg-only formula is deliberately NOT symlinked
#     into /opt/homebrew/{include,lib,bin}, so it is invisible to the plain roots —
#     tcl-tk (9.x), tcl-tk@8 (8.6) and zlib are all keg-only. `brew --prefix <f>`
#     prints the WOULD-BE prefix even when <f> is not installed, which is exactly
#     why these are only CANDIDATE roots that find_in must still filter.
brew_prefix() { command -v brew  >/dev/null 2>&1 && brew --prefix "$1" 2>/dev/null || true; }
sdk_prefix()  { command -v xcrun >/dev/null 2>&1 && xcrun --show-sdk-path 2>/dev/null || true; }
declare -a EXTRA_INC_ROOTS=() EXTRA_LIB_ROOTS=() EXTRA_BIN_ROOTS=()
if [[ "$HOST_OS" == "macos" ]]; then
  for _f in zlib tcl-tk tcl-tk@8; do
    _p="$(brew_prefix "$_f")"
    [[ -n "$_p" ]] && { EXTRA_INC_ROOTS+=("$_p/include"); EXTRA_LIB_ROOTS+=("$_p/lib"); EXTRA_BIN_ROOTS+=("$_p/bin"); }
  done
  _sdk="$(sdk_prefix)"
  [[ -n "$_sdk" ]] && EXTRA_INC_ROOTS+=("$_sdk/usr/include")
fi
# `${arr[@]:-}` — an EMPTY array must expand to one empty string, not trip `set -u`
# (bash ≤4.3 treats a bare `${arr[@]}` on an empty array as unbound). find_in drops
# the empty element like any other non-directory. On Linux all three ARE empty.
declare -a INC_ROOTS=(/usr/include /usr/local/include /opt/homebrew/include "${EXTRA_INC_ROOTS[@]:-}")
declare -a LIB_ROOTS=(/usr/lib /lib /usr/local/lib /opt/homebrew/lib             "${EXTRA_LIB_ROOTS[@]:-}")
declare -a CFG_ROOTS=(/usr/lib /usr/lib64 /usr/local/lib /opt/homebrew/lib       "${EXTRA_LIB_ROOTS[@]:-}")

# ── the Tcl inventory, VERSION-AGNOSTIC, straight from tclConfig.sh ──────────
# The harness used to hardcode 8.6 everywhere. That was a harness-only fiction:
# TF-C65 (commit 355572d, D-FFI-SHIPPED-LIBS-OS-ONLY) deleted every third-party
# shipped-lib descriptor, so DSS holds no tcl.json to disagree with the host and
# simply PARSES the tcl.h Step 6 stages; and sqlite supports Tcl 9 natively. The
# pin bought nothing and hard-failed any host whose Tcl is 9.x (Homebrew's
# `tcl-tk` is 9.0 today). tclConfig.sh is Tcl's OWN authoritative, version-neutral
# description of an installation — TCL_VERSION, TCL_INCLUDE_SPEC (`-I<headerdir>`),
# TCL_LIB_FILE (the runtime library's file NAME) and TCL_EXEC_PREFIX (where its
# bin/ is) — so interpreter, header dir AND library are all DERIVED from it rather
# than guessed from hardcoded names.
# Emits "<version> <path>" per installation, `sort -u`'d so the candidate set (and
# therefore the pick) is deterministic instead of filesystem-order-dependent.
tcl_configs() {
  local cfg ver
  while IFS= read -r cfg; do
    ver="$( . "$cfg" >/dev/null 2>&1; printf '%s' "${TCL_VERSION:-}" )" || ver=""
    [[ -n "$ver" ]] && printf '%s %s\n' "$ver" "$cfg"
  done < <(find_in "${CFG_ROOTS[@]}" -- -name tclConfig.sh | sort -u)
  return 0
}
tcl_cfg_for() {                 # tcl_cfg_for <version> -> its tclConfig.sh (or "")
  local line
  while IFS= read -r line; do
    [[ -n "$line" ]] || continue
    [[ "${line%% *}" == "$1" ]] && { printf '%s' "${line#* }"; return; }
  done <<< "$TCL_CFGS"
  return 0
}
tclsh_version() { echo 'puts $tcl_version' | tclsh 2>/dev/null || true; }
# The `#define TCL_VERSION "x.y"` a HEADER declares. POSIX BRE only (`\(…\)` and
# `[[:space:]][[:space:]]*` — never GNU `\+`), and it tolerates Tcl 9's indented
# `#   define` as well as 8.6's `#define`. Verified byte-identical under BSD
# /usr/bin/sed and GNU gsed.
tcl_h_version() {               # tcl_h_version <path/to/tcl.h> -> "8.6" | "9.0" | ""
  sed -n 's/^#[[:space:]]*define[[:space:]][[:space:]]*TCL_VERSION[[:space:]][[:space:]]*"\([0-9][0-9.]*\)".*/\1/p' "$1" 2>/dev/null | sed -n '1p'
}
TCL_CFGS="$(tcl_configs)"

# ── the -L that TCL_LIBS needs but does not carry (Tcl 9 externalised tommath) ─
# WHY THIS EXISTS — do NOT "simplify" the -L away:
# Tcl 8.6 BUNDLED libtommath inside libtcl. Tcl 9 does NOT, so a Tcl-9
# tclConfig.sh declares it as an EXTERNAL dependency in TCL_LIBS
# (`TCL_LIBS=' -lz  -lpthread -framework CoreFoundation  -ltommath'`), and
# main.mk's testfixture rule passes `$$TCL_LIBS` through VERBATIM — with no `-L`
# to go with it. On macOS the Homebrew lib dir is NOT a default linker search
# path, so the moment the host's default Tcl became 9.x the REFERENCE fixture
# stopped linking (`ld: library 'tommath' not found`) while every .o still
# compiled fine — i.e. the ATTRIBUTION ORACLE silently disappeared
# (D-SQLITE-GCC-REFERENCE-FIXTURE-AS-ORACLE; its absence stalled walsetlk_recover).
#
# Written GENERICALLY — it repairs whatever `-l` TCL_LIBS names, not `tommath` by
# name — and PROBE-GATED, so it is a strict NO-OP wherever the toolchain already
# resolves the library. On every Linux host today Debian's libtommath.so sits in
# a default multiarch dir, every probe passes, NOTHING is added, and both the
# configure invocation and the link line stay byte-identical to their pre-macOS
# form.
#   · the PROBE links with the SAME compiler the sqlite Makefile links with
#     (Step 4 reads it out of the generated Makefile's own `CC =`), NEVER a bare
#     `clang`: on a host carrying the Android NDK ahead of /usr/bin on PATH,
#     `clang` IS the NDK's and dies on `-lSystem`, which would make every probe
#     lie. The probe's exit status is captured DIRECTLY off the compiler.
#   · the DIRECTORY is DERIVED, never hardcoded — Homebrew's prefix is
#     /usr/local on Intel and /opt/homebrew on Apple Silicon, and CI/Linux differ
#     again. Precedence: pkg-config's own `libdir` (the library's authoritative
#     self-description) → the Homebrew keg for lib<name>/<name> → the LIB_ROOTS
#     this harness already discovers (which carry both Homebrew layouts).
declare -a PROBE_CC=("${CC:-cc}")   # Step 4 replaces this with the Makefile's CC
probe_link_l() {                # probe_link_l <-L…/-l… args> -> 0 iff the LINK succeeds
  local tmp rc
  tmp="$(mktemp -d)" || return 1
  printf 'int main(void){return 0;}\n' > "$tmp/probe.c"
  # status taken DIRECTLY off the compiler — never through a pipe, which would
  # report the pipe's status instead. The if/else keeps `set -e` out of it.
  if "${PROBE_CC[@]}" "$tmp/probe.c" "$@" -o "$tmp/probe.bin" >/dev/null 2>&1
  then rc=0; else rc=$?; fi
  rm -rf "$tmp"
  return "$rc"
}
pkgcfg_libdir() {               # pkgcfg_libdir <module> -> its libdir (or "")
  command -v pkg-config >/dev/null 2>&1 && pkg-config --variable=libdir "$1" 2>/dev/null || true
}
dir_holds_lib() {               # dir_holds_lib <dir> <name> -> 0 iff lib<name>.* is there
  local d="$1" n="$2" f
  [[ -n "$d" && -d "$d" ]] || return 1
  # an unmatched glob stays LITERAL and `-e` then rejects it — no nullglob needed.
  for f in "$d/lib$n".dylib "$d/lib$n".so "$d/lib$n".so.* "$d/lib$n".tbd "$d/lib$n".a; do
    [[ -e "$f" ]] && return 0
  done
  return 1
}
libdir_for() {                  # libdir_for <name> -> a dir holding lib<name>.* (or "")
  local n="$1" d p
  for d in "$(pkgcfg_libdir "lib$n")" "$(pkgcfg_libdir "$n")"; do
    dir_holds_lib "$d" "$n" && { printf '%s' "$d"; return; }
  done
  # `brew --prefix <f>` prints the WOULD-BE prefix even when <f> is absent, and
  # prints NOTHING when the formula does not exist — hence the emptiness guard.
  for p in "$(brew_prefix "lib$n")" "$(brew_prefix "$n")"; do
    [[ -n "$p" ]] || continue
    dir_holds_lib "$p/lib" "$n" && { printf '%s' "$p/lib"; return; }
  done
  for d in "${LIB_ROOTS[@]}"; do
    dir_holds_lib "$d" "$n" && { printf '%s' "$d"; return; }
  done
  return 0
}
mk_var() {                      # mk_var <makefile> <NAME> -> its value (first defn, trimmed)
  # `NAME = value`, as configure writes it. POSIX BRE only — no `-r`, no `\+`,
  # no `-i`: byte-identical under BSD /usr/bin/sed and GNU sed.
  sed -n "s/^$2[[:space:]]*=[[:space:]]*//p" "$1" 2>/dev/null \
    | sed 's/[[:space:]]*$//' | sed -n '1p' || true
}
tcl_libs_ldflags() {            # tcl_libs_ldflags <tclConfig.sh> -> "-Ldir …" | ""
  local cfg="$1" libs tok name dir out=""
  local -a toks=()
  [[ -n "$cfg" && -f "$cfg" ]] || return 0
  libs="$( . "$cfg" >/dev/null 2>&1; printf '%s' "${TCL_LIBS:-}" )" || return 0
  # `read -ra` splits on IFS and does NOT glob — safer than `for tok in $libs`.
  read -r -a toks <<< "$libs" || true
  [[ ${#toks[@]} -gt 0 ]] || return 0
  for tok in "${toks[@]}"; do
    case "$tok" in -l?*) name="${tok#-l}" ;; *) continue ;; esac   # skips -framework X etc.
    probe_link_l -l"$name" && continue        # already resolvable → add NOTHING
    dir="$(libdir_for "$name")"
    # ★ every diagnostic here goes to STDERR: stdout IS this function's return
    #   value, and a `info`/`warn` line leaking into it would be spliced onto the
    #   front of REF_LDFLAGS and handed to configure as part of the flag.
    if [[ -z "$dir" ]]; then
      warn "reference link: tclConfig.sh declares -l$name in TCL_LIBS, which ${PROBE_CC[*]} cannot" >&2
      warn "      resolve and neither pkg-config, brew, nor ${#LIB_ROOTS[@]} library roots can locate." >&2
      warn "      The reference testfixture — the ATTRIBUTION ORACLE — will NOT link. Install lib$name." >&2
      continue
    fi
    if probe_link_l -L"$dir" -l"$name"; then
      case " $out " in *" -L$dir "*) ;; *) out="${out:+$out }-L$dir" ;; esac
      info "reference link: -l$name is not on a default search path — adding -L$dir" >&2
    else
      warn "reference link: lib$name was found under $dir, yet ${PROBE_CC[*]} still cannot link" >&2
      warn "      -l$name against it (wrong architecture?). The reference testfixture will NOT link." >&2
    fi
  done
  printf '%s' "$out"
}

# ── Step 4 — configure + derive the full-source testfixture recipe ───────────
step "4/9  Derive the full-source testfixture recipe (make -n testfixture)"
# mksqlite3c.tcl + the fixture link need tclsh 8.6+ and the Tcl dev files
# (tclConfig.sh — configure detects Tcl through it). apt: tcl (8.6+) + tcl-dev.
# An interpreter REPORTING <version>, most-authoritative candidate first: the bin/
# of the very installation whose tclConfig.sh declares that version (same tree as
# its headers + library), then a version-suffixed `tclshN.M` on PATH (the Debian
# shape), then each keg bin/ (the Homebrew shape), then the plain PATH `tclsh`.
# Every candidate is CONFIRMED by asking it `puts $tcl_version` — a file NAME is
# not proof of a version, and picking on the name is how a 9.0 interpreter ends up
# generating sources for an 8.6 build.
tclsh_bin_for() {               # tclsh_bin_for <version> -> path (or "")
  local want="$1" cfg="" pfx="" c v r
  local -a cands=()
  cfg="$(tcl_cfg_for "$want")"
  if [[ -n "$cfg" ]]; then
    pfx="$( . "$cfg" >/dev/null 2>&1; printf '%s' "${TCL_EXEC_PREFIX:-}" )" || pfx=""
    [[ -n "$pfx" ]] && cands+=("$pfx/bin/tclsh$want" "$pfx/bin/tclsh")
    # …and the keg layout, where tclConfig.sh sits in <prefix>/lib beside bin/.
    cands+=("$(dirname "$(dirname "$cfg")")/bin/tclsh$want" "$(dirname "$(dirname "$cfg")")/bin/tclsh")
  fi
  cands+=("$(command -v "tclsh$want" 2>/dev/null || true)")
  for r in "${EXTRA_BIN_ROOTS[@]:-}"; do
    [[ -n "$r" ]] && cands+=("$r/tclsh$want" "$r/tclsh")
  done
  cands+=("$(command -v tclsh 2>/dev/null || true)")
  for c in "${cands[@]}"; do
    [[ -n "$c" && -x "$c" ]] || continue
    v="$(echo 'puts $tcl_version' | "$c" 2>/dev/null || true)"
    [[ "$v" == "$want" ]] && { printf '%s' "$c"; return; }
  done
  return 0
}
# Put the pinned interpreter on PATH under the PLAIN name. Prepending the found
# binary's own directory is NOT enough: that directory's `tclsh` may be a
# different version (Debian's alternatives symlink; a prefix holding both), and
# everything downstream invokes the unversioned name. A one-line exec shim in the
# harness's OWN output tree makes `tclsh` resolve to the pinned interpreter on any
# layout, touching neither the host nor the sqlite clone. `exec "<abs path>"`
# keeps argv[0] absolute, so Tcl still finds its own init.tcl next to the REAL
# installation rather than beside the shim.
tcl_pin_path() {                # tcl_pin_path <interpreter>
  local bin="$1" dir="$OUT_DIR/tcl-pin"
  mkdir -p "$dir"
  printf '#!/bin/sh\nexec "%s" "$@"\n' "$bin" > "$dir/tclsh"
  chmod +x "$dir/tclsh"
  export PATH="$dir:$PATH"
  hash -r
}
TCL_PIN_SH=""; TCL_PIN_CFG=""
ensure_tclsh() {
  local ver=""
  command -v tclsh >/dev/null 2>&1 && ver="$(echo 'puts $tcl_version' | tclsh 2>/dev/null || true)"
  # ── PINNED (DSS_TCL_VERSION set): the interpreter is PART of the pin ───────
  # Not "≥ 8.6" — EXACTLY the pinned version. `configure`, `make -n` and
  # mksqlite3c.tcl all run under this tclsh and bake ITS `-I` dirs into the
  # recipe, so continuing on a different Tcl would silently stage the fixture
  # against two of them. Nothing is installed on this path: an absent pin is an
  # operator decision to make, so it FAILS LOUD naming everything searched.
  if [[ -n "$DSS_TCL_VERSION" ]]; then
    TCL_PIN_CFG="$(tcl_cfg_for "$DSS_TCL_VERSION")"
    if [[ "$ver" != "$DSS_TCL_VERSION" ]]; then
      TCL_PIN_SH="$(tclsh_bin_for "$DSS_TCL_VERSION")"
      [[ -n "$TCL_PIN_SH" ]] || die "DSS_TCL_VERSION=$DSS_TCL_VERSION is pinned, but NO tclsh reporting $DSS_TCL_VERSION was found.
      tclsh on PATH  : $(command -v tclsh 2>/dev/null || echo none) (reports ${ver:-none})
      tclConfig.sh   : $(printf '%s' "${TCL_CFGS:-<none>}" | tr '\n' ';')
      searched       : each config's TCL_EXEC_PREFIX/bin + <prefix>/bin, 'tclsh$DSS_TCL_VERSION' on
                       PATH, ${EXTRA_BIN_ROOTS[*]:-<no keg bin roots>}, then PATH 'tclsh'
      Install it (apt: tcl$DSS_TCL_VERSION — brew: 'tcl-tk@8' for 8.6 / 'tcl-tk' for 9.x, both
      KEG-ONLY), or unset DSS_TCL_VERSION. NOT continuing on a different Tcl: sqlite's configure,
      make -n and mksqlite3c.tcl all run under this interpreter and bake ITS -I dirs into the
      recipe, so a mismatched tclsh silently builds the fixture against two different Tcls."
      tcl_pin_path "$TCL_PIN_SH"
      ver="$(echo 'puts $tcl_version' | tclsh 2>/dev/null || true)"
    else
      TCL_PIN_SH="$(command -v tclsh)"
    fi
    [[ "$ver" == "$DSS_TCL_VERSION" ]] || die "tclsh is STILL $ver after pinning to $DSS_TCL_VERSION (shim: $OUT_DIR/tcl-pin) — refusing to continue."
    info "tclsh $ver ($(command -v tclsh)) — PINNED by DSS_TCL_VERSION"
    return
  fi
  # ── UNPINNED: the original ≥ 8.6 floor, unchanged ─────────────────────────
  if [[ -z "$ver" ]] || ! awk "BEGIN{exit !(${ver:-0}+0 >= 8.6)}"; then
    [[ -n "$ver" ]] && info "tclsh ${ver} is < 8.6 — installing a newer tcl"
    pkg_install tcl tcl-tk
    if [[ "$HOST_OS" == "macos" ]]; then
      local tclbin; tclbin="$(brew --prefix tcl-tk 2>/dev/null)/bin"
      [[ -d "$tclbin" ]] && export PATH="$tclbin:$PATH"
    fi
    hash -r
  fi
  command -v tclsh >/dev/null 2>&1 || die "tclsh not found after install."
  ver="$(echo 'puts $tcl_version' | tclsh 2>/dev/null || true)"
  awk "BEGIN{exit !(${ver:-0}+0 >= 8.6)}" || die "tclsh ${ver:-none} is < 8.6."
  info "tclsh $ver ($(command -v tclsh))"
}
ensure_tclsh
# Tcl dev files (headers + tclConfig.sh) — configure needs them to emit the recipe.
if [[ "$HOST_OS" == "macos" ]]; then
  pkg_install tcl tcl-tk
  # zlib too — parity with the Linux branch below, and NOT optional here. macOS
  # ships NO libz a program can OPEN: /usr/lib/libz.1.dylib exists only inside the
  # dyld shared cache, /usr/lib/libz.1.2.12.dylib is a dangling symlink to it, and
  # the SDK carries .tbd TEXT stubs. Step 6 must hand DSS a REAL library binary to
  # read (--resolve-library), so Homebrew's zlib is a hard prerequisite; it is
  # keg-only, which is why Step 6 searches `brew --prefix zlib` explicitly.
  pkg_install zlib1g-dev zlib
else
  command -v dpkg >/dev/null 2>&1 && dpkg -s tcl-dev >/dev/null 2>&1 || pkg_install tcl-dev tcl-tk
  command -v dpkg >/dev/null 2>&1 && dpkg -s zlib1g-dev >/dev/null 2>&1 || pkg_install zlib1g-dev zlib
fi
BLD="$SQLITE_DIR/bld-dss"
mkdir -p "$BLD"
# Carry the pin INTO sqlite's own Tcl detection. Without this, `configure` picks a
# Tcl by its own search and can write a DIFFERENT version's `-I` into the Makefile
# — which `make -n` then hands us as SQLITE_INCS, and those dirs come FIRST in the
# Step-7 include list, so they would silently WIN over the Tcl Step 6 staged.
# sqlite's autosetup exposes exactly the two knobs needed (autosetup/
# sqlite-config.tcl: `with-tcl:DIR` = the directory holding tclConfig.sh,
# `with-tclsh:PATH` = the interpreter used for tclConfig detection AND all
# TCL-based code generation, which trumps --with-tcl). Passing both states the
# same installation twice, deliberately: whichever one autosetup honours, it is
# the pinned one. The Step-6 recipe coherence check is the backstop if it is not.
# UNPINNED — the default, and every Linux run today — takes the `else` branch,
# which is byte-identical to what this line always was.
declare -a CONFIGURE_ARGS=()
if [[ -n "$DSS_TCL_VERSION" ]]; then
  [[ -n "$TCL_PIN_SH"  ]] && CONFIGURE_ARGS+=("--with-tclsh=$TCL_PIN_SH")
  [[ -n "$TCL_PIN_CFG" ]] && CONFIGURE_ARGS+=("--with-tcl=$(dirname "$TCL_PIN_CFG")")
  info "configure: pinning sqlite's Tcl detection — ${CONFIGURE_ARGS[*]:-<none resolvable>}"
fi
# $BLD is REUSED between runs, and re-running configure rewrites the Makefile but
# NOT the object timestamps — `make` cannot see that a header PATH changed. So a
# tclsqlite.o built under a previous run's Tcl survives and gets linked against
# THIS run's libtcl: a silently mis-built REFERENCE fixture, i.e. a corrupt
# attribution oracle (the one thing worse than no oracle). Stamp the Tcl identity
# and refuse to build on top of a different one. A MISSING stamp means "no
# information" and never fires, so every existing tree — every Linux tree today —
# is untouched until the Tcl actually changes under it. The harness does NOT wipe
# it itself: $BLD lives inside the sqlite clone, which this driver never destroys.
TCL_STAMP="$BLD/.dss-tcl-identity"
TCL_STAMP_NOW="tclsh=$(echo 'puts $tcl_version' | tclsh 2>/dev/null || true) configure=${CONFIGURE_ARGS[*]:-<default>}"
if [[ -f "$TCL_STAMP" && "$(cat "$TCL_STAMP" 2>/dev/null || true)" != "$TCL_STAMP_NOW" ]]; then
  die "the Tcl behind $BLD CHANGED since it was built.
      was: $(cat "$TCL_STAMP" 2>/dev/null || true)
      now: $TCL_STAMP_NOW
      Re-running configure rewrites the Makefile but not the .o timestamps, so a stale tclsqlite.o
      compiled against the PREVIOUS Tcl's headers would link against this run's libtcl and quietly
      corrupt the reference (gcc) fixture — the ORACLE the whole failure-attribution story rests on.
      Rebuild the tree from scratch:  rm -rf '$BLD'   then re-run."
fi
if [[ ${#CONFIGURE_ARGS[@]} -gt 0 ]]; then
  ( cd "$BLD" && "$SQLITE_DIR/configure" "${CONFIGURE_ARGS[@]}" >/dev/null )
else
  ( cd "$BLD" && "$SQLITE_DIR/configure" >/dev/null )
fi
printf '%s\n' "$TCL_STAMP_NOW" > "$TCL_STAMP"
# ── repair the reference link line: the -L that TCL_LIBS omits ───────────────
# WHICH tclConfig.sh matters, and only configure knows: it is the one configure
# just wrote into the Makefile as TCL_CONFIG_SH — literally the file the link's
# `.tclenv.sh` sources. So DERIVE IT AFTER configure, off the Makefile itself,
# rather than re-guessing the selection here and risking a different answer.
# Same reason PROBE_CC comes from the Makefile's own `CC =`: the probe must be
# the compiler that will actually do the link, never a bare `clang` off PATH.
_mk_cc="$(mk_var "$BLD/Makefile" CC)"
[[ -n "$_mk_cc" ]] && read -r -a PROBE_CC <<< "$_mk_cc"      # handles `ccache gcc`
REF_LDFLAGS="$(tcl_libs_ldflags "$(mk_var "$BLD/Makefile" TCL_CONFIG_SH)")"
if [[ -n "$REF_LDFLAGS" ]]; then
  # The mechanism is sqlite's OWN documented client knob: `LDFLAGS=…` handed to
  # configure lands in the Makefile as `LDFLAGS.configure` (Makefile.in:
  # `LDFLAGS.configure = @LDFLAGS@`), which main.mk folds into
  # $(LDFLAGS.libsqlite3) and therefore onto the testfixture link line
  # (main.mk: `testfixture$(T.exe): … $(LDFLAGS.libsqlite3)`).
  # Chosen over exporting LIBRARY_PATH for one `make` — both link, MEASURED —
  # because it BAKES THE FIX INTO THE TREE: the oracle exists to be USED, and a
  # human re-running `make testfixture` by hand in $BLD to attribute a failure
  # must get a link too, not just this script. Also chosen over passing
  # `LDFLAGS.configure=…` to make, which main.mk explicitly says not to rely on.
  # The re-configure happens ONLY when a -L is genuinely missing, so a host that
  # needs none — every Linux host today — still configures EXACTLY ONCE.
  info "configure: reference link needs $REF_LDFLAGS — re-running configure with LDFLAGS"
  # Appended AFTER the Tcl identity stamp is written, deliberately: the stamp
  # guards against a stale .o compiled under a DIFFERENT Tcl's headers, and a
  # library SEARCH PATH cannot affect a .o. Folding it into the stamp would
  # false-alarm ("the Tcl behind $BLD CHANGED") on every existing tree.
  CONFIGURE_ARGS+=("LDFLAGS=$REF_LDFLAGS")
  ( cd "$BLD" && "$SQLITE_DIR/configure" "${CONFIGURE_ARGS[@]}" >/dev/null )
  # FAIL LOUD if it did not land. A silent miss — say sqlite reshapes the
  # @LDFLAGS@ substitution upstream — would put us straight back to an oracle
  # that quietly is not there, which is the exact failure mode this repairs.
  if grep -qF -- "$REF_LDFLAGS" "$BLD/Makefile"; then
    info "configure: LDFLAGS.configure now carries $REF_LDFLAGS"
  else
    warn "configure did NOT carry LDFLAGS='$REF_LDFLAGS' into $BLD/Makefile."
    warn "      The reference testfixture will almost certainly fail to link — sqlite's"
    warn "      LDFLAGS.configure / @LDFLAGS@ substitution may have changed shape upstream."
  fi
fi
# Build the reference fixture. It generates every derived .c
# (parse.c/opcodes.c/ctime.c/tclsqlite-ex.c/fts5.c…) + libsqlite3.a, which the DSS
# TU set needs — AND, when it links, it is the ORACLE that decides whether a corpus
# failure is ours or upstream's. A link miss stays tolerated (the byproducts are
# still harvested), but the log is KEPT, not sent to /dev/null: it was discarded for
# a long time, which is why nobody noticed the build had started succeeding — and a
# missing oracle is what stalled the walsetlk_recover attribution
# (D-SQLITE-GCC-REFERENCE-FIXTURE-AS-ORACLE).
info "building the reference testfixture (generates derived sources + libsqlite3.a)"
REF_BUILD_LOG="$BLD/reference-build.log"
# ── the PRESERVED oracle — why the copy exists AND why the `rm` below must stay ─
# These two lines are a PAIR. Deleting either one re-creates a defect the other
# repairs, so do NOT "simplify" one away without the other:
#
#   · the `rm -f "$BLD/testfixture"` further down is LOAD-BEARING. `make -n` only
#     PRINTS a target's recipe when the target is MISSING — against an up-to-date
#     tree it prints nothing harvestable. Harvesting that one cc/link line is the
#     ENTIRE reason Step 4 exists (it is where the TU list, the -D defines and the
#     -I dirs come from), so removing the `rm` empties $RECIPE and Step 4 dies on
#     its own <150-TU / <18-define floor.
#
#   · but that same linked binary is the ATTRIBUTION ORACLE — the reference build
#     that decides whether a corpus failure is DSS's fault or upstream's. The
#     harness used to build it, ANNOUNCE it as an oracle, and then delete it ten
#     lines later; $REF_FIXTURE was never read again ANYWHERE in the script, so
#     after a full run the oracle did not exist on disk at all. (Second half of
#     D-SQLITE-GCC-REFERENCE-FIXTURE-AS-ORACLE: the Tcl-9 `-L` repair above made
#     the oracle LINK again, this makes it SURVIVE. Both are needed to have one.)
#
# So the binary is copied OUT of the make target's path BEFORE the target is
# deleted. The copy is not a make target and not a prerequisite of one, so
# `make -n` still sees `testfixture` missing and still prints the recipe.
# It lands in $OUT_DIR — this harness's OWN output tree, NOT the sqlite checkout —
# so sqlite's `make clean`/`distclean` can never take it, and Step 9 prints the
# path so a human triaging a failure knows the oracle is there and where.
REF_FIXTURE_KEEP="$OUT_DIR/reference-testfixture"
mkdir -p "$OUT_DIR"
# Clear any copy from a PREVIOUS run BEFORE building: sqlite is pulled/updated on
# every run, so a stale oracle would attribute against sources that are no longer
# the ones under test — strictly worse than no oracle at all. Past this line the
# preserved path holds THIS run's binary or nothing.
rm -f "$REF_FIXTURE_KEEP"
if ( cd "$BLD" && make -s testfixture USE_AMALGAMATION=0 -j"$JOBS" ) > "$REF_BUILD_LOG" 2>&1; then
  # `cp -p` — POSIX, not a GNU-only long option; -p keeps the exec bit so the
  # copy is runnable. FAIL LOUD on a copy miss: announcing an oracle that the
  # very next line deletes is the exact failure mode this block exists to end.
  if cp -p "$BLD/testfixture" "$REF_FIXTURE_KEEP"; then
    REF_FIXTURE="$REF_FIXTURE_KEEP"
    info "reference gcc testfixture built + preserved -> $REF_FIXTURE  (usable as an ATTRIBUTION ORACLE)"
  else
    REF_FIXTURE=""
    warn "reference testfixture LINKED but could NOT be preserved to $REF_FIXTURE_KEEP —"
    warn "      it is about to be deleted to expose the recipe, so no oracle will survive this"
    warn "      run. Check permissions / free space on $OUT_DIR."
  fi
else
  REF_FIXTURE=""
  warn "reference gcc testfixture did not fully link (tolerated — harvesting generated sources + recipe)"
  warn "      log kept: $REF_BUILD_LOG — READ IT. A working reference is what lets a corpus"
  warn "      failure be ATTRIBUTED instead of argued about; its absence stalled walsetlk_recover."
  [[ -n "$REF_LDFLAGS" ]] && \
  warn "      link-path repair WAS in effect (LDFLAGS=$REF_LDFLAGS) — so this is a DIFFERENT miss."
fi
# Emit the recipe: with testfixture removed but its prereqs built, `make -n` prints
# the single testfixture cc/link command (the source of TUs + defines + -I dirs).
# ★ THIS `rm` IS LOAD-BEARING — see "the PRESERVED oracle" above. The binary was
#   copied to $REF_FIXTURE_KEEP a few lines up *precisely so* this delete can
#   happen without destroying the attribution oracle. If you want to keep the
#   fixture around, copy it (as above); do NOT drop this line — without a MISSING
#   target `make -n` prints no recipe and Step 4 has nothing to harvest.
rm -f "$BLD/testfixture"
RECIPE="$OUT_DIR/testfixture-recipe.txt"
mkdir -p "$OUT_DIR"
( cd "$BLD" && make -n testfixture USE_AMALGAMATION=0 ) > "$RECIPE" 2>&1 || true
# `make -n testfixture` is essentially ONE cc command (the fixture link); join its
# backslash-continuations, then extract each token-type from the whole blob.
# ★ ONE `-e` PER LABEL — never `sed ':a;N;$!ba;…'`
# (D-HARNESS-SELFTEST-BSD-SED-PORTABILITY). GNU sed lets a `:label` be
# terminated by `;`, BSD/macOS sed does NOT: it swallows the rest of the script
# as part of the LABEL NAME and dies `unused label 'a;N;$!ba;…'`, emitting only
# the first line — so the backslash-continuation join SILENTLY DID NOT HAPPEN
# and $BLOB was whatever sed managed before erroring. Splitting the script into
# separate -e arguments makes the label end at the argument boundary, which is
# the portable form: MEASURED byte-identical output (8093 B on this recipe) from
# BSD sed and GNU sed, and identical to what the Linux legs were already getting.
BLOB="$(sed -e ':a' -e 'N;$!ba' -e 's/\\\n/ /g' "$RECIPE" | tr '\t' ' ')"
# defines: -DNAME[=VALUE]  →  DSS `--define NAME[=VALUE]` (strip make's literal "" so
# SQLITE_PRIVATE="" becomes an EMPTY value, and drop bare shell quoting).
mapfile -t RECIPE_DEFS < <(printf '%s\n' "$BLOB" | grep -oE '\-D[A-Za-z0-9_]+(=[^ ]*)?' | sed 's/^-D//; s/"//g' | sort -u)
# sqlite include dirs off the recipe (ext/**, src, .).
mapfile -t SQLITE_INCS < <(printf '%s\n' "$BLOB" | grep -oE '\-I ?[^ ]+' | sed 's/^-I *//' | grep -v '^\.$' | sort -u)
# TU set (1): every .c the recipe names directly (the test/ext harness sources +
# the generated testfixture entry). Most tokens are ABSOLUTE ($(TOP)/…); the few
# the recipe names RELATIVELY (ctime.c/fts5.c/parse.c/tclsqlite-ex.c) live in the
# build dir $BLD (make -n's CWD), so an unrooted token is resolved against $BLD
# too. Without this the generated `tclsqlite-ex.c` — which DEFINES the Tcl
# `Sqlite3_Init` the fixture links against and is NOT a libsqlite3.a member (so
# TU-set-2 can't recover it) — is silently dropped and the link fails on an
# undefined `Sqlite3_Init`.
declare -A TU=()
while IFS= read -r c; do
  [[ -n "$c" ]] || continue
  if   [[ -f "$c"      ]]; then TU["$c"]=1
  elif [[ -f "$BLD/$c" ]]; then TU["$BLD/$c"]=1
  fi
done < <(printf '%s\n' "$BLOB" | tr ' ' '\n' | grep -E '\.c$' | sort -u)
# TU set (2): the CORE sources compiled into libsqlite3.a — DSS can't consume the
# gcc archive, so recover each member's .c (members live in src/, ext/**, or bld/).
AR="$BLD/libsqlite3.a"; [[ -f "$BLD/.libs/libsqlite3.a" ]] && AR="$BLD/.libs/libsqlite3.a"
declare -A TU_BASENAME=()      # basename -> path (dedup generated .c aliased in bld/ & bld/tsrc/)
if [[ -f "$AR" ]]; then
  while read -r obj; do
    base="${obj%.o}"
    hit="$(find "$SQLITE_DIR/src" "$SQLITE_DIR/ext" "$BLD" -name "$base.c" 2>/dev/null | grep -v '/tsrc/' | head -1)"
    [[ -z "$hit" ]] && hit="$(find "$SQLITE_DIR/src" "$SQLITE_DIR/ext" "$BLD" -name "$base.c" 2>/dev/null | head -1)"
    [[ -n "$hit" ]] && TU["$hit"]=1
  done < <(ar t "$AR" 2>/dev/null | grep '\.o$')
fi
# de-alias generated .c that exist under both bld/ and bld/tsrc/ (same file, two paths)
declare -A TU_FINAL=()
for f in "${!TU[@]}"; do
  b="$(basename "$f")"
  if [[ -n "${TU_BASENAME[$b]:-}" ]]; then continue; fi
  TU_BASENAME["$b"]="$f"; TU_FINAL["$f"]=1
done
mapfile -t TUS < <(printf '%s\n' "${!TU_FINAL[@]}" | sort)
[[ ${#TUS[@]} -ge 150 ]]       || die "recipe derivation yielded only ${#TUS[@]} TUs (<150) — recipe parse broke; see $RECIPE"
[[ ${#RECIPE_DEFS[@]} -ge 18 ]] || die "recipe derivation yielded only ${#RECIPE_DEFS[@]} defines (<18) — recipe parse broke; see $RECIPE"
pass "recipe: ${#TUS[@]} TUs, ${#RECIPE_DEFS[@]} defines, ${#SQLITE_INCS[@]} sqlite -I dirs"

# ── Step 5 — build dss-code-prime (CMake-4 Release) ──────────────────────────
step "5/9  Build dss-code-prime (CMake ${MIN_CMAKE_MAJOR}+ Release)"
cmake_major() { cmake --version 2>/dev/null | sed -n '1s/.*version \([0-9]*\).*/\1/p'; }
ensure_cmake() {
  local major; major="$(cmake_major)"
  if [[ -n "$major" && "$major" -ge "$MIN_CMAKE_MAJOR" ]]; then info "cmake: $(cmake --version | sed -n '1p')"; return; fi
  if [[ "$HOST_OS" == "macos" ]]; then
    warn "CMake ${MIN_CMAKE_MAJOR}+ required (found: ${major:-none}) — installing via Homebrew"
    pkg_install cmake cmake; hash -r; major="$(cmake_major)"
    [[ -n "$major" && "$major" -ge "$MIN_CMAKE_MAJOR" ]] || die "CMake ${MIN_CMAKE_MAJOR}+ install failed via brew."
    info "cmake: $(cmake --version | sed -n '1p') (Homebrew)"; return
  fi
  warn "CMake ${MIN_CMAKE_MAJOR}+ required (found: ${major:-none}) — installing the official Kitware Linux binary"
  ensure_cmd tar tar
  local arch json ver url dest
  arch="$(uname -m)"
  json="$(curl -fsSL https://api.github.com/repos/Kitware/CMake/releases/latest)"
  ver="$(printf '%s\n' "$json" | sed -n 's/.*"tag_name": *"v\([0-9.]*\)".*/\1/p')"; ver="${ver%%$'\n'*}"
  [[ -n "$ver" ]] || die "could not resolve the latest CMake version from the GitHub API."
  url="https://github.com/Kitware/CMake/releases/download/v${ver}/cmake-${ver}-linux-${arch}.tar.gz"
  dest="$HOME/.local/cmake-${ver}"
  info "downloading CMake ${ver} ($arch) from Kitware"
  rm -rf "$dest"; mkdir -p "$dest"
  curl -fsSL "$url" | tar -xz --strip-components=1 -C "$dest"
  export PATH="$dest/bin:$PATH"; hash -r; major="$(cmake_major)"
  [[ -n "$major" && "$major" -ge "$MIN_CMAKE_MAJOR" ]] || die "CMake ${MIN_CMAKE_MAJOR}+ install failed."
  info "cmake: $(cmake --version | sed -n '1p') (Kitware $ver)"
}
ensure_cmake
( cd "$SRC_DIR" && cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j"$JOBS" )
DSS_BIN="$(find "$SRC_DIR/build" -type f -name dss-code-prime -perm -u+x -print -quit 2>/dev/null)"
[[ -n "$DSS_BIN" && -x "$DSS_BIN" ]] || die "dss-code-prime binary not found under $SRC_DIR/build."
pass "dss-code-prime built: $DSS_BIN"

# ── Step 6 — stage third-party headers + obtain per-leg libs ─────────────────
step "6/9  Third-party headers (parsed agnostically) + per-leg tcl/zlib libraries"
# Headers are leg-INDEPENDENT: DSS parses the host tcl/zlib headers agnostically
# (ABI is irrelevant at parse). The tcl headers sit in a per-version private subdir
# (safe on -I); zlib.h sits directly in a system include dir (would shadow the OS
# descriptors) → stage a private copy of just zlib.h + zconf.h.
THIRD_PARTY_INCS=()
# find_in / the *_ROOTS lists / tcl_configs / tcl_cfg_for all live in the SHARED
# THIRD-PARTY DISCOVERY block above Step 4 — Step 4's `ensure_tclsh` needs the very
# same inventory to honour DSS_TCL_VERSION, and one selector shared by both steps
# is what keeps the INTERPRETER, the headers and the library a single Tcl.
# RE-TAKE the inventory here: Step 4 may have installed a Tcl since it was first
# computed (the unpinned path runs `pkg_install tcl tcl-tk`).
TCL_CFGS="$(tcl_configs)"
# Pick ONE installation, deterministically:
#   1. DSS_TCL_VERSION set → EXACTLY that TCL_VERSION, or DIE. A pin that silently
#      fell back to another ABI would be worse than no pin at all.
#   2. otherwise → the installation matching the `tclsh` ensure_tclsh validated
#      onto PATH. That interpreter is the one sqlite's configure and mksqlite3c.tcl
#      ALREADY ran under (Step 4), so matching it keeps generator, headers and
#      runtime library one single Tcl.
#   3. otherwise → the highest version present. `sort -t. -k1,1nr -k2,2nr` (numeric
#      major, then minor) — NOT `sort -V`, which is a GNU extension absent from
#      POSIX and unreliable under a BSD userland.
TCL_VER=""; TCL_CFG=""; TCLSH_VER="$(tclsh_version)"
if [[ -n "$DSS_TCL_VERSION" ]]; then
  TCL_VER="$DSS_TCL_VERSION"; TCL_CFG="$(tcl_cfg_for "$TCL_VER")"
  [[ -n "$TCL_CFG" ]] || die "DSS_TCL_VERSION=$TCL_VER is pinned, but no Tcl $TCL_VER is installed.
      tclConfig.sh found: $(printf '%s' "${TCL_CFGS:-<none>}" | tr '\n' ';')
      roots searched   : ${CFG_ROOTS[*]}
      Install it (apt: tcl${TCL_VER}-dev — brew: 'tcl-tk' for 9.x, 'tcl-tk@8' for 8.6; both are
      KEG-ONLY, which is why their own prefixes are searched), or unset DSS_TCL_VERSION to take
      whatever Tcl this host has."
  info "tcl: PINNED to $TCL_VER by DSS_TCL_VERSION"
else
  [[ -n "$TCLSH_VER" ]] && TCL_CFG="$(tcl_cfg_for "$TCLSH_VER")"
  if [[ -n "$TCL_CFG" ]]; then
    TCL_VER="$TCLSH_VER"
  elif [[ -n "$TCL_CFGS" ]]; then
    _hi="$(printf '%s\n' "$TCL_CFGS" | sort -t. -k1,1nr -k2,2nr | sed -n '1p')"
    TCL_VER="${_hi%% *}"; TCL_CFG="${_hi#* }"
    warn "no tclConfig.sh matches the tclsh on PATH (${TCLSH_VER:-none}) — falling back to the highest installed Tcl ($TCL_VER)."
  fi
fi
# Interpreter-vs-staging agreement. WITH a pin this is an ASSERTION, not advice:
# Step 4's ensure_tclsh already put a tclsh of exactly $DSS_TCL_VERSION on PATH and
# died if it could not, so a disagreement here means something regressed between
# the two steps — fatal. WITHOUT a pin it is a genuine host inconsistency the
# operator may not care about (nothing forced the interpreter), so it stays a warn
# that names the remedy.
if [[ -n "$TCL_VER" && -n "$TCLSH_VER" && "$TCL_VER" != "$TCLSH_VER" ]]; then
  if [[ -n "$DSS_TCL_VERSION" ]]; then
    die "PINNED Tcl skew after Step 4: staging $TCL_VER but tclsh on PATH reports $TCLSH_VER
      (pin=$DSS_TCL_VERSION, interpreter=$(command -v tclsh)). ensure_tclsh guarantees these agree,
      so this is a harness regression, not a host problem. Refusing to build against two Tcls."
  fi
  warn "tcl SKEW: staging headers+library for $TCL_VER while tclsh on PATH is $TCLSH_VER — the sources
      Step 4 generated came from the latter. Set DSS_TCL_VERSION=$TCL_VER to pin BOTH end-to-end."
fi
# The chosen Tcl's header dir: TCL_INCLUDE_SPEC out of ITS OWN tclConfig.sh (the
# authoritative answer — it names the private per-version subdir). Two weaker
# fallbacks cover an installation whose config is missing or omits the spec: a
# tcl.h under a version-named directory, then any tcl.h at all. The version glob
# replaces the old hardcoded `*tcl8*`; on Debian, `*tcl8.6*` still selects
# /usr/include/tcl8.6 exactly as before.
tcl_header_dir() {              # tcl_header_dir <tclConfig.sh|""> <version|"">
  local cfg="$1" ver="$2" d="" spec=""
  if [[ -n "$cfg" ]]; then
    spec="$( . "$cfg" >/dev/null 2>&1; printf '%s' "${TCL_INCLUDE_SPEC:-}" )" || spec=""
    d="${spec#-I}"
  fi
  [[ -n "$d" && -f "$d/tcl.h" ]] || d="$(dirname "$(find_in "${INC_ROOTS[@]}" -- -name tcl.h -path "*tcl${ver:-[0-9]}*" | sed -n '1p')")"
  [[ -f "$d/tcl.h" ]] || d="$(dirname "$(find_in "${INC_ROOTS[@]}" -- -name tcl.h | sed -n '1p')")"
  printf '%s' "$d"
}
TCL_INC="$(tcl_header_dir "$TCL_CFG" "$TCL_VER")"
[[ -f "$TCL_INC/tcl.h" ]] || die "tcl.h not found — install the Tcl DEV files (apt: tcl-dev / tcl8.6-dev; brew: tcl-tk).
      roots searched: ${INC_ROOTS[*]}"
# Cross-check the header we are about to stage against the installation we chose.
# A tcl.h from one Tcl beside a libtcl from another COMPILES AND LINKS and then
# misbehaves at run time — precisely the class this harness must never ship
# silently.
TCL_INC_VER="$(tcl_h_version "$TCL_INC/tcl.h")"
[[ -z "$TCL_VER" || -z "$TCL_INC_VER" || "$TCL_VER" == "$TCL_INC_VER" ]] || \
  die "Tcl staging is INCOHERENT: tclConfig.sh reports $TCL_VER ($TCL_CFG) but $TCL_INC/tcl.h reports $TCL_INC_VER.
      A fixture built against one Tcl's headers and another's library links clean and then fails at
      run time. Pin one with DSS_TCL_VERSION, or remove the stray installation."
[[ -n "$TCL_VER" ]] || TCL_VER="$TCL_INC_VER"
# RECIPE COHERENCE — the third and last place a Tcl can enter the build. The `-I`
# dirs harvested from `make -n testfixture` (SQLITE_INCS, Step 4) carry whatever
# Tcl `configure` detected. Step 7 puts those dirs BEFORE the staged
# THIRD_PARTY_INCS in the include list, so a tcl.h sitting in one of them WINS
# over everything decided above — silently compiling the fixture against a Tcl
# whose library it never links. That is the exact failure DSS_TCL_VERSION exists
# to prevent, so it is FATAL, not a warning. Only dirs that actually contain a
# tcl.h are judged; an unreadable/version-less header is skipped rather than
# guessed at.
for _d in "${SQLITE_INCS[@]:-}"; do
  [[ -n "$_d" && -f "$_d/tcl.h" ]] || continue
  _rv="$(tcl_h_version "$_d/tcl.h")"
  [[ -z "$_rv" || "$_rv" == "$TCL_VER" ]] || die "RECIPE/STAGING Tcl MISMATCH — the fixture would compile against TWO Tcls.
      recipe -I dir : $_d  (tcl.h reports $_rv)
      staged Tcl    : $TCL_VER  ($TCL_CFG)
      The recipe dir comes FIRST in the Step-7 include list, so its headers would WIN over the
      staged ones while the fixture links $TCL_VER's library.
      'configure' chose that dir; it follows the tclsh on PATH ($(command -v tclsh), $TCLSH_VER).
      Fix: run with DSS_TCL_VERSION=$_rv (stage what the recipe uses) or DSS_TCL_VERSION=$TCL_VER
      (which also passes --with-tclsh/--with-tcl to configure so the recipe follows the pin), then
      delete $BLD so configure re-runs."
done
ZINC="$BLD/zinc"; mkdir -p "$ZINC"
ZH="$(find_in "${INC_ROOTS[@]}" -- -maxdepth 3 -name zlib.h | sed -n '1p')"
[[ -n "$ZH" ]] || die "zlib.h not found — install zlib1g-dev (or 'brew install zlib').
      roots searched: ${INC_ROOTS[*]}"
cp -f "$ZH" "$ZINC/"
# zconf.h beside zlib.h first (they are a matched pair); the sweep is the fallback.
# Deliberately unquoted — this word-splits the find results into loop items.
for zc in "$(dirname "$ZH")/zconf.h" $(find_in "${INC_ROOTS[@]}" -- -maxdepth 3 -name zconf.h); do
  [[ -f "$zc" ]] && { cp -f "$zc" "$ZINC/"; break; }
done
THIRD_PARTY_INCS=("$TCL_INC" "$ZINC")
info "tcl $TCL_VER headers: $TCL_INC   zlib headers: $ZINC (staged from $ZH)"

# ── host libraries (the native leg links + runs against these) ───────────────
# WHAT THESE ARE FOR — both flow through leg_tcl_lib/leg_z_lib into the per-leg
# `.dss-project.json` "resolveLibraries" (Step 7), i.e. DSS `--resolve-library`.
# DSS OPENS AND READS each one AT COMPILE TIME to harvest its export table (the
# FF1 binary reader) and fails loud `F_FileOpenFailed` on a path it cannot open
# (src/program/compile_pipeline.cpp:305). They are NOT `-l` flags for a system
# linker and NOT the gcc reference link (that uses its own configure-derived
# flags) — so each must be a REAL, READABLE library binary. The runtime dependency
# the linker then records comes from the binary's OWN embedded identity (ELF
# DT_SONAME / Mach-O LC_ID_DYLIB install name — D-FF1-READER-SONAME,
# src/ffi/ingest.cpp:356), which is why a Homebrew dylib carrying an absolute
# install name resolves at run time from wherever the keg lives.
# WHY READABILITY IS TESTED, NOT MERE EXISTENCE — on macOS the only libz left in
# /usr/lib is `libz.1.2.12.dylib`, a symlink to `libz.1.dylib`, which is not a file
# at all: it exists solely inside the dyld shared cache. `find` prints that
# dangling link happily and DSS then cannot open it, killing the build. `-f`/`-r`
# FOLLOW symlinks, which is exactly the test wanted; `find -type f` would inspect
# the LINK itself and still accept the dangling one. Checking every match (not just
# `head -1`) is what lets a later, real candidate win — and it drops the old
# `find | head -1` SIGPIPE exposure under `pipefail`.
find_first() {                  # find_first <name>... -> first match that is a readable file
  local n h
  for n in "$@"; do
    [[ -n "$n" ]] || continue
    while IFS= read -r h; do
      [[ -f "$h" && -r "$h" ]] && { printf '%s' "$h"; return; }
    done < <(find_in "${LIB_ROOTS[@]}" -- -name "$n")
  done
  return 0
}
# Candidate library FILE NAMES for the chosen Tcl, from tclConfig.sh's TCL_LIB_FILE
# (which is exactly that: `libtcl8.6.so` on Debian, `libtcl9.0.dylib` on Homebrew)
# instead of a hardcoded list. `<file>.0` covers a host shipping only the versioned
# ELF runtime object; the TCL_VERSION-derived triple covers a config that omits
# TCL_LIB_FILE. On Debian the first candidate is `libtcl8.6.so` — the same name,
# and the same file, the pre-macOS hardcoded list resolved to.
tcl_lib_names() {
  local f=""
  if [[ -n "$TCL_CFG" ]]; then
    f="$( . "$TCL_CFG" >/dev/null 2>&1; printf '%s' "${TCL_LIB_FILE:-}" )" || f=""
  fi
  [[ -n "$f" ]] && printf '%s\n%s.0\n' "$f" "$f"
  [[ -n "$TCL_VER" ]] && printf 'libtcl%s.so\nlibtcl%s.so.0\nlibtcl%s.dylib\n' "$TCL_VER" "$TCL_VER" "$TCL_VER"
  return 0
}
declare -a TCL_LIB_NAMES=(); mapfile -t TCL_LIB_NAMES < <(tcl_lib_names)
HOST_TCL_LIB="$(find_first "${TCL_LIB_NAMES[@]:-}")"
# zlib: the two Linux names FIRST (so a Linux host resolves exactly as before),
# then the macOS ones. Homebrew's keg ships libz.<ver>.dylib plus libz.dylib and
# libz.1.dylib symlinks onto it, all readable.
HOST_Z_LIB="$(find_first 'libz.so' 'libz.so.1' 'libz.dylib' 'libz.1.dylib')"
[[ -n "$HOST_TCL_LIB" ]] || die "host libtcl ${TCL_VER:-<version unknown>} not found — install the Tcl runtime + dev files
      (apt: tcl-dev / tcl${TCL_VER}-dev; brew: 'tcl-tk' for 9.x or 'tcl-tk@8' for 8.6 — KEG-ONLY, so
      its own prefix is searched). names tried: ${TCL_LIB_NAMES[*]:-<none>}
      roots searched: ${LIB_ROOTS[*]}"
[[ -n "$HOST_Z_LIB"   ]] || die "host libz not found — install zlib1g-dev (linux) / 'brew install zlib' (macOS).
      macOS ships NO OPENABLE libz: /usr/lib/libz.1.dylib exists only inside the dyld shared cache,
      /usr/lib/libz.1.2.12.dylib is a dangling symlink to it, and the SDK carries .tbd TEXT stubs —
      none can be read, and DSS must read the export table (--resolve-library). Homebrew's keg-only
      zlib provides a real dylib under \$(brew --prefix zlib)/lib.
      roots searched: ${LIB_ROOTS[*]}"
info "host libs: $HOST_TCL_LIB  +  $HOST_Z_LIB"

# arm64 libraries (only if an arm64 leg is selected) — Ubuntu ports .deb extract,
# NO apt-source surgery: resolve the exact .deb from the ports Packages index, then
# dpkg-deb -x and harvest the runtime .so. qemu resolves libc/libm from the sysroot.
ARM64_LIBDIR="${ARM64_LIBDIR:-$HOME/.cache/dss-code-prime/arm64libs}"
ensure_arm64_libs() {
  if [[ -e "$ARM64_LIBDIR/libtcl8.6.so.0" && -e "$ARM64_LIBDIR/libz.so.1" ]]; then
    info "arm64 libs cached: $ARM64_LIBDIR ($(ls "$ARM64_LIBDIR" | tr '\n' ' '))"; return
  fi
  ensure_cmd curl curl; ensure_cmd dpkg-deb dpkg; ensure_cmd gzip gzip; ensure_cmd qemu-aarch64 qemu-user
  mkdir -p "$ARM64_LIBDIR"
  local work; work="$(mktemp -d)"
  local codename; codename="$( . /etc/os-release 2>/dev/null; echo "${VERSION_CODENAME:-noble}" )"
  local baseurl="http://ports.ubuntu.com/ubuntu-ports"
  local idx="$work/Packages"
  info "arm64 libs: fetching ports index ($codename/main)"
  curl -fsSL "$baseurl/dists/$codename/main/binary-arm64/Packages.gz" | gzip -d > "$idx" || die "cannot fetch ports arm64 Packages index for '$codename'."
  local pkg rel
  for pkg in libtcl8.6 zlib1g libtommath1; do
    rel="$(awk -v p="$pkg" '$1=="Package:"{c=$2} $1=="Filename:"&&c==p{print $2; exit}' "$idx")"
    [[ -n "$rel" ]] || { warn "arm64 $pkg not in ports index (skipping — may be optional)"; continue; }
    info "  downloading $pkg:arm64"
    curl -fsSL "$baseurl/$rel" -o "$work/$pkg.deb" || die "download failed: $pkg ($baseurl/$rel)"
    dpkg-deb -x "$work/$pkg.deb" "$work/root"
  done
  find "$work/root" \( -name 'libtcl8.6.so*' -o -name 'libz.so*' -o -name 'libtommath.so*' \) \
    -exec cp -Pa {} "$ARM64_LIBDIR/" \; 2>/dev/null || true
  # ensure the DT_NEEDED soname link exists (tcl bakes libtcl8.6.so.0)
  if [[ ! -e "$ARM64_LIBDIR/libtcl8.6.so.0" ]]; then
    local rt; rt="$(find "$ARM64_LIBDIR" -name 'libtcl8.6.so*' | head -1)"
    [[ -n "$rt" ]] && ln -sf "$(basename "$rt")" "$ARM64_LIBDIR/libtcl8.6.so.0"
  fi
  # a plain `.so` alias for --resolve-library introspection
  [[ -e "$ARM64_LIBDIR/libtcl8.6.so" ]] || ln -sf "$(basename "$(find "$ARM64_LIBDIR" -name 'libtcl8.6.so.0' | head -1)")" "$ARM64_LIBDIR/libtcl8.6.so" 2>/dev/null || true
  rm -rf "$work"
  [[ -e "$ARM64_LIBDIR/libtcl8.6.so.0" ]] || die "arm64 libtcl8.6 not obtained under $ARM64_LIBDIR."
  [[ -e "$ARM64_LIBDIR/libz.so.1"      ]] || die "arm64 libz not obtained under $ARM64_LIBDIR."
  info "arm64 libs staged: $(ls "$ARM64_LIBDIR" | tr '\n' ' ')"
}
for leg in "${LEG_ORDER[@]}"; do
  [[ "${LEG_LIBSRC[$leg]}" == "arm64" ]] && { ensure_arm64_libs; break; }
done
# resolve each leg's (tcl,z) library pair for --resolve-library + runtime.
leg_tcl_lib() { case "${LEG_LIBSRC[$1]}" in arm64) find_first_in "$ARM64_LIBDIR" libtcl8.6.so libtcl8.6.so.0 ;; *) printf '%s' "$HOST_TCL_LIB" ;; esac; }
leg_z_lib()   { case "${LEG_LIBSRC[$1]}" in arm64) find_first_in "$ARM64_LIBDIR" libz.so.1 libz.so ;;          *) printf '%s' "$HOST_Z_LIB"   ;; esac; }
find_first_in() { local d="$1"; shift; local n; for n in "$@"; do [[ -e "$d/$n" ]] && { printf '%s' "$d/$n"; return; }; done; }
pass "headers + libraries ready for: ${LEG_ORDER[*]}"

# ─────────────────────────────────────────────────────────────────────────────
# THE CORPUS RESUME ENGINE — an abort is a RECOVERABLE, REPORTED outcome
# ─────────────────────────────────────────────────────────────────────────────
# The harness exists so that EVERY sqlite unit reaches a verdict. Before this
# engine, a fixture that ABORTED mid-suite (a Tcl `error` out of a test body, a
# hard crash) produced one line — "fixture did not complete the suite (crash?)" —
# and every test FILE behind the abort point was silently never run. One bad unit
# cost the other thousand.
#
# DETECT  a segment aborted iff its log has no "N errors out of M tests" summary
#         line. That is the STRUCTURAL fact; the engine is never keyed on a test
#         name or iteration index (an OOM-injection abort point is a function of
#         process-global allocation history and moves between runs).
# LOCATE  from the log: the last COMPLETED file ("Time: <file> N ms"), the
#         permutation ("run_tests <name>" / "run_test_suite <name>" in the Tcl
#         traceback, or the tier's sole suite), and the ABORTING file (resolved
#         from the last emitted test name against the real corpus file list —
#         the traceback's own `(file "…")` frame is NOT usable: Tcl truncates it
#         to ~200 chars, so a long path degrades it to `…/test/sw...`).
# RESUME  in a NEW PROCESS (required, not merely convenient: a leaked handle is
#         held for the life of the fixture process), using SQLITE'S OWN upstream
#         hooks — no hand-rolled Tcl runner, and NOTHING is ever written into the
#         sqlite clone (the .sh runs the corpus straight out of it):
#           · SQLITE_TEST_PATTERN_LIST (permutations.test ~1175) — a glob list
#             intersected with the permutation's own -files, so passing "every
#             corpus basename after the abort point" selects exactly the
#             permutation's remaining files without the harness ever needing to
#             know that file set.
#           · --start=<permutation>: (tester.tcl ~444 / slave_test_file ~2395) —
#             re-runs THE ORIGINAL tier script, skipping every permutation before
#             the named one, so every `ifcapable`/platform guard in all.test is
#             evaluated by sqlite exactly as in a normal run.
# BOUND   $DSS_MAX_RESUMES. The resume boundary is forced to advance every time,
#         so an aborting file can never be re-entered.
# REPORT  the UNION across segments — total tests, total errors, EVERY abort with
#         its permutation + file, the resume count, and every unit NOT reached.
#         An abort is itself a FAILURE line: resuming never makes it disappear,
#         and a run with aborts is NEVER green.
#
# GRANULARITY (stated because it is a real, reported loss): resume restarts at the
# next FILE. The remainder of the aborting file — the fault-injection iterations
# after the one that died — is NOT run, and is reported as such per abort. sqlite
# exposes no finer restart point than (permutation, file).
#
# >>> dss:corpus-engine >>>  (region mirrored in build-and-test.ps1; the verifier
# extracts it from this file by these sentinels, so keep them on their own lines)

# sqlite's own $alltests: every `.test` basename in the corpus dir MINUS the driver
# scripts it excludes by name (all.test / permutations.test / …), byte-sorted — the
# same order run_tests uses (`lsort $options(-files)`, default -ascii). The
# exclusion list is read as DATA out of permutations.test's own
# `set alltests [test_set $alltests -exclude { … }]` block, never hard-coded; a
# parse miss only widens the list, and the list is used as a SUPERSET filter, so
# the worst case is a wasted resume stepping over a non-unit.
corpus_files() {               # corpus_files <testdir>
  # NOTE two statements deliberately: bash expands ALL words of a `local` command
  # before it performs any of its assignments, so `local d="$1" skip="$d/…"` reads
  # an unset `d` (fatal under `set -u`).
  local d="$1" f
  local skip="$d/permutations.test"
  { for f in "$d"/*.test; do [[ -e "$f" ]] || continue; printf '%s\n' "${f##*/}"; done; } \
  | LC_ALL=C awk -v skipfile="$skip" '
      BEGIN {
        while ((getline line < skipfile) > 0) {
          if (!inb) { if (line ~ /^[ \t]*set[ \t]+alltests[ \t]+\[test_set[ \t]+\$alltests[ \t]+-exclude[ \t]*\{/) inb=1; continue }
          n=split(line, w, /[ \t{}\]]+/); for (i=1;i<=n;i++) if (w[i] ~ /\.test$/) skip[w[i]]=1
          if (line ~ /\}[ \t]*\]/) break
        } }
      !($0 in skip)' \
  | LC_ALL=C sort
}
# The tier script's permutation sequence, in order, read as DATA from sqlite's own
# `run_test_suite <name>` lines (all.test names 27; veryquick/quick/full name one).
# The TEST-NAME PREFIX each permutation stamps onto its test names, read as DATA
# out of sqlite's own permutations.test — the same discipline as the file/permutation
# readers above, and necessary because the prefix is NOT derivable from the name:
# permutations.test:39 defaults it to "<name>." but :220 declares `mmap -prefix "mm-"`,
# and veryquick/quick/full/threads/valgrind declare `-prefix ""` (no prefix at all).
# So `memsubsys1.walsetlk-2.2.6` and `mm-backup4-3.3` are BOTH qualified names, with
# different separators, and neither can be recovered by guessing from the suite name.
# Emits one non-empty prefix per line, longest first so that a longer prefix wins over
# a shorter one that happens to be its head.
tier_prefixes() {              # tier_prefixes <permutations.test>
  LC_ALL=C awk '
    /^[ \t]*test_suite[ \t]+"/ {
      # name = the first quoted field after test_suite
      line = $0
      if (match(line, /test_suite[ \t]+"[^"]*"/)) {
        nm = substr(line, RSTART, RLENGTH); sub(/^test_suite[ \t]+"/, "", nm); sub(/"$/, "", nm)
        if (match(line, /-prefix[ \t]+"[^"]*"/)) {
          p = substr(line, RSTART, RLENGTH); sub(/^-prefix[ \t]+"/, "", p); sub(/"$/, "", p)
        } else {
          p = nm "."          # permutations.test:39 — the documented default
        }
        if (p != "") print length(p) "\t" p
      }
    }' "$1" | LC_ALL=C sort -rn -k1,1 | cut -f2- | LC_ALL=C awk '!seen[$0]++'
}

tier_permutations() {          # tier_permutations <tierfile>
  LC_ALL=C awk 'match($0, /run_test_suite[ \t]+[A-Za-z_][A-Za-z0-9_]*/) {
      s = substr($0, RSTART, RLENGTH); sub(/^run_test_suite[ \t]+/, "", s); print s }' "$1"
}
# ONE streaming pass over a segment log -> a small tab-separated fact file.
# (These logs reach 150 MB / 3.6M lines; a grep per pattern costs minutes each.)
#   F <file>  a completed test FILE      S <text>  the summary LINE, verbatim
#   X <name>  a failing test name        P <name>  the permutation in the traceback
#   T <name>  the last test emitted      G         `*** Giving up` (--maxerror cap)
#   N <n>     completed file count       D <file>  the LAST completed file
#   E <n>     errors      C <n>          tests     (parsed out of the summary line)
#   K <n>     lines ending " Ok"         Q <n>     `! <name> expected:` lines
# K and Q are the per-test tally: for a segment that ABORTED they are the ONLY
# record of the work it did (no summary line exists) — see the union's derivation.
# S is the WHOLE line ("0 errors out of 9 tests on <host> …"), which is what this
# harness has always printed — E/C carry the numbers so nothing has to re-parse it.
# NOTE the leading '!' on the canonical failure list: finalize_testing emits
# `!Failures on these tests: …` (tester.tcl ~1304), which a `^Failures` pattern
# silently never matches.
parse_segment() {              # parse_segment <log> <out-facts>
  LC_ALL=C awk '
    # Strip a trailing CR first. A segment log can be CRLF (a fixture running on
    # Windows), and a POSIX awk keeps that CR — so `$4=="ms"` and `/ Ok$/` would
    # both silently match NOTHING and every count would come back zero. MSYS awk
    # strips it and hides the problem, which is precisely how this stayed invisible.
    { sub(/\r$/, "") }
    /^Time: / { if (NF==4 && $4=="ms") { print "F\t" $2; nf++; lastdone=$2; next } }
    /^\*\*\* Giving up/ { gaveup=1; next }
    /^!?Failures on these tests:/ {
      line=$0; sub(/^!?Failures on these tests:[ \t]*/, "", line);
      n=split(line, a, /[ \t]+/); for (i=1;i<=n;i++) if (a[i]!="") print "X\t" a[i]; next }
    / Ok$/                     { ok++ }
    # one `expected:` per FAILED test (`got:` is its partner line) — the failure
    # tally that pairs with `ok` to reconstitute the count sqlite itself reports,
    # for a segment that aborted before printing a summary.
    # (NB no apostrophes in this block: it lives inside a single-quoted awk program.)
    /^! [^ ]+ expected:/       { fx++ }
    /^! [^ ]+ (expected|got):/ { print "X\t" $2; next }
    match($0, /[0-9]+ errors? out of [0-9]+ tests/) {
      summary=$0; split(substr($0, RSTART, RLENGTH), q, / /); nerr=q[1]; ntest=q[5]; next }
    # A traceback frame is quoted, and the closing quote can abut the name
    # (`"run_test_suite inmemory_journal"`) — take the name by MATCH, never by
    # whitespace split, or the permutation carries a trailing `"`.
    { line=$0; sub(/^[ \t]*"?/, "", line);
      if (match(line, /^(run_test_suite|run_tests)[ \t]+[A-Za-z_][A-Za-z0-9_]*/)) {
        s=substr(line, RSTART, RLENGTH); sub(/^[A-Za-z_]+[ \t]+/, "", s); perm=s; next } }
    /^[^ \t]+\.\.\./ { split($0, c, /\.\.\./); lasttest=c[1] }
    END { if (summary!="") { print "S\t" summary; print "E\t" nerr+0; print "C\t" ntest+0 }
          if (perm!="")    print "P\t" perm
          if (lasttest!="")print "T\t" lasttest
          if (gaveup)      print "G\t1"
          print "N\t" nf+0; print "D\t" lastdone
          print "K\t" ok+0; print "Q\t" fx+0 }
  ' "$1" > "$2"
}
fact() { LC_ALL=C awk -F'\t' -v k="$1" '$1==k{v=$2} END{print v}' "$2"; }
facts() { LC_ALL=C awk -F'\t' -v k="$1" '$1==k{print $2}' "$2"; }
# Which corpus FILE was the fixture inside when it died? The last test it emitted
# names it: pick the corpus stem occurring RIGHTMOST in that test name on delimiter
# boundaries (rightmost, then longest). `inmemory_journal.swarmvtabfault-1.1-oom-
# persistent.143` -> swarmvtabfault.test (not swarmvtab.test: the 'f' after it is
# not a delimiter; not the leading permutation token: it is left of it).
resolve_abort_file() {         # resolve_abort_file <lasttest> <corpus-list-file>
  [[ -n "$1" ]] || return 0
  LC_ALL=C awk -v name="$1" '
    { f=$0; stem=f; sub(/\.test$/,"",stem); L=length(stem)
      off=0; s=name; best=0
      while (1) {
        p=index(s, stem); if (p==0) break
        idx=off+p
        before=(idx==1) ? "." : substr(name, idx-1, 1)
        after =(idx+L>length(name)) ? "." : substr(name, idx+L, 1)
        if ((before=="."||before=="-") && (after=="."||after=="-")) best=idx
        off=idx; s=substr(name, idx+1)
      }
      if (best>0 && (best>bi || (best==bi && L>bl))) { bi=best; bl=L; bf=f } }
    END { if (bf!="") print bf }' "$2"
}
# Every corpus basename byte-wise AFTER <boundary> — the SQLITE_TEST_PATTERN_LIST
# superset sqlite intersects with the permutation's own -files.
files_after() { LC_ALL=C awk -v b="$1" '$0 > b' "$2"; }
# thousands separators, locale-free (a 4.2M headline is unreadable without them)
group_digits() {
  LC_ALL=C awk -v n="$1" 'BEGIN{ s=sprintf("%d", n); out=""
    while (length(s) > 3) { out = "," substr(s, length(s)-2) out; s = substr(s, 1, length(s)-3) }
    print s out }'
}
str_gt()      { [[ "$(LC_ALL=C awk -v a="$1" -v b="$2" 'BEGIN{print (a>b) ? 1 : 0}')" == 1 ]]; }

# ── process hygiene ──────────────────────────────────────────────────────────
# Scoped to OUR EXACT fixture binary path — never to the image name. A developer's
# own testfixture, or one from a different checkout, is never touched. This is only
# safe because the run lock guarantees we are the sole invocation on this output
# tree: any process still running THIS path is, by construction, a leftover of a run
# that is already over. Matching on the full argv covers the cross legs too, where
# the fixture is an argument of qemu-aarch64 rather than the image itself.
# `ps -eo pid=,args=` is POSIX and works on Linux/WSL/macOS — the hosts this script
# targets. If it ever does NOT, leftover detection is UNAVAILABLE, and that has to be
# LOUD and on the record: silently finding nothing is the exact defect class this
# whole layer exists to remove. Probed once.
PS_ENUM_OK=""
ps_enum_available() {
  if [[ -z "$PS_ENUM_OK" ]]; then
    if LC_ALL=C ps -eo pid=,args= >/dev/null 2>&1; then PS_ENUM_OK=yes; else
      PS_ENUM_OK=no
      # >&2 is load-bearing: this probe runs inside our_fixture_pids, whose STDOUT is
      # consumed as a PID list. A warning on stdout would be parsed as PIDs to kill.
      warn "this host's ps(1) cannot enumerate processes (\`ps -eo pid=,args=\` failed):" >&2
      warn "  LEFTOVER-FIXTURE DETECTION IS UNAVAILABLE for this run — a stray fixture from a" >&2
      warn "  dead run will not be found or killed. Reported in the verdict, never assumed clean." >&2
    fi
  fi
  [[ "$PS_ENUM_OK" == yes ]]
}
our_fixture_pids() {           # our_fixture_pids <fixture-abs-path>
  [[ -n "${1:-}" ]] || return 0
  ps_enum_available || return 0
  # The path goes through the ENVIRONMENT, never `awk -v`. With -v the path is in
  # awk's OWN argv, so the concurrently-running `ps` snapshot contains it and the
  # matcher reports its own helper as a leftover fixture — a self-match that had
  # this function "find" a process it then tried to kill. (MEASURED.)
  LC_ALL=C ps -eo pid=,args= 2>/dev/null \
    | DSS_WANT="$1" DSS_SELF="$$" LC_ALL=C awk '
        BEGIN { want=ENVIRON["DSS_WANT"]; self=ENVIRON["DSS_SELF"] }
        { pid=$1; sub(/^[ \t]*[0-9]+[ \t]+/, ""); if (pid != self && index($0, want)) print pid }' || true
}
# Kill every leftover of OUR fixture and echo one report line per kill (never
# silent — a killed process is a fact the verdict has to carry).
stop_our_fixtures() {          # stop_our_fixtures <fixture-abs-path> <why>
  local p
  for p in $(our_fixture_pids "${1:-}"); do
    # This function KILLS. Never act on a token that is not a plain PID: a stray
    # line on the enumerator's stdout must not become a kill target.
    [[ "$p" =~ ^[0-9]+$ ]] || { printf '%s — REFUSED to kill non-numeric target %q (enumerator emitted junk)\n' "$2" "$p"; continue; }
    kill -TERM "$p" 2>/dev/null || true
    local n=0
    while kill -0 "$p" 2>/dev/null && [[ $n -lt 15 ]]; do sleep 1; n=$((n + 1)); done
    if kill -0 "$p" 2>/dev/null; then
      kill -KILL "$p" 2>/dev/null || true
      printf '%s — killed (SIGKILL) pid %s\n' "$2" "$p"
    else
      printf '%s — killed pid %s\n' "$2" "$p"
    fi
  done
}

# Run ONE fixture segment: stdin at EOF, stdout+stderr merged to <log>, killed if it
# stalls (no log growth for DSS_SEGMENT_STALL s) or exceeds an absolute cap. Sets
# SEG_RC and SEG_KILL_REASON.
#
# stdin is </dev/null deliberately. It is NOT the fix for anything observed — the
# "aborted fixture drops into the Tcl REPL and blocks on stdin" theory was TESTED
# and REFUTED: tclsqlite.c's TCLSH_MAIN evaluates its mainloop script, takes the
# `source $argv0` branch whenever $argv is non-empty, and on error prints errorInfo
# and `return 1`; the `while {![eof stdin]}` REPL is reachable only with NO script
# argument. Measured: with stdin an open pipe that is never written and never
# closed, an aborted fixture still exits rc=1 in 8.8 s. Closing stdin is kept as
# cheap hardening (tester.tcl's own `--pause` does read stdin) — not as a cure.
# The per-leg launcher. `exec` is load-bearing, not a micro-optimisation: run_leg is
# always the last command of run_fixture_segment's background subshell, so exec makes
# the FIXTURE (or qemu) that subshell's OWN process. Without it the fixture is a
# GRANDCHILD and the timeout's `kill $child` reaps only the shell — leaving the hung
# fixture alive, holding its file handles, and defeating the point of the timeout.
# It lives inside this region precisely because that contract is load-bearing here.
run_leg() {                    # run_leg <leg> <bin> <args...>  — REPLACES this shell
  local leg="$1" bin="$2"; shift 2
  local pfx="${LEG_PREFIX[$leg]}"
  if [[ -z "$pfx" ]]; then
    exec "$bin" "$@" 2>&1
  else
    export QEMU_LD_PREFIX="$QEMU_SYSROOT" LD_LIBRARY_PATH="$ARM64_LIBDIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
    exec "$pfx" "$bin" "$@" 2>&1
  fi
}

run_fixture_segment() {        # run_fixture_segment <leg> <bin> <log> <args...>
  local leg="$1" bin="$2" log="$3"; shift 3
  SEG_RC=0; SEG_KILL_REASON=""
  # `trap - ERR; set +e` inside the subshell is load-bearing. The old form was a
  # `|| segrc=$?` list, which suppresses errexit and the ERR trap; a BACKGROUND job
  # does not, so this subshell would inherit them (set -E) and the harness-level
  # `die` would fire on the fixture's own non-zero exit — writing a bogus
  # " [X] ERROR: failed at line …" INTO the segment log (stderr is redirected there)
  # and masking the real exit status. A failing test is data here, not an error.
  ( trap - ERR; set +e; cd "$rundir" && run_leg "$leg" "$bin" "$@" ) > "$log" 2>&1 < /dev/null &
  local child=$!
  local last_len=-1 last_grow t0
  t0="$(date +%s)"; local last_grow_t="$t0"
  while kill -0 "$child" 2>/dev/null; do
    sleep 5
    kill -0 "$child" 2>/dev/null || break
    local len now
    len="$(wc -c < "$log" 2>/dev/null || echo "$last_len")"
    now="$(date +%s)"
    if [[ "$len" != "$last_len" ]]; then last_len="$len"; last_grow_t="$now"
    elif [[ "$DSS_SEGMENT_STALL" -gt 0 && $((now - last_grow_t)) -ge "$DSS_SEGMENT_STALL" ]]; then
      SEG_KILL_REASON="produced no output for ${DSS_SEGMENT_STALL}s (DSS_SEGMENT_STALL)"; break
    fi
    if [[ "$DSS_SEGMENT_TIMEOUT" -gt 0 && $((now - t0)) -ge "$DSS_SEGMENT_TIMEOUT" ]]; then
      SEG_KILL_REASON="exceeded the absolute cap of ${DSS_SEGMENT_TIMEOUT}s (DSS_SEGMENT_TIMEOUT)"; break
    fi
  done
  if [[ -n "$SEG_KILL_REASON" ]]; then
    kill -TERM "$child" 2>/dev/null || true; sleep 2
    kill -KILL "$child" 2>/dev/null || true
    # Sweep by path as well: on a host where `exec` cannot replace the shell with a
    # native binary, the fixture is a grandchild the child-kill never reaches.
    stop_our_fixtures "$bin" "segment timeout" >/dev/null || true
    # SETTLE. MEASURED: without this the very next segment dies at tester.tcl's
    # startup `reset_db` with `error deleting "test.db": permission denied` — the
    # killed fixture's handle is still open, so the harness manufactures its own
    # next failure and burns the resume budget on it. Bounded, and only on this path.
    local s=0
    while [[ $s -lt $DSS_KILL_SETTLE ]]; do
      [[ -n "$(our_fixture_pids "$bin")" ]] || break
      sleep 1; s=$((s + 1))
    done
    sleep 2
  fi
  wait "$child" 2>/dev/null || SEG_RC=$?
}
# <<< dss:corpus-engine <<<

# ── Step 7 — build the full-source testfixture with dss-code-prime, per leg ──
step "7/9  Build the full-source testfixture (dss-code-prime --project), per leg"
declare -A FIXTURE=()          # leg -> binary path (on success)
declare -A COMPILE_OK=()
COMPILE_FAILS=0
# the per-leg manifest is emitted as clean JSON by python3 (a harness dep now).
ensure_cmd python3 python3
# The leg-INDEPENDENT include-dir set: the sqlite recipe dirs + the staged
# third-party tcl/zlib headers (whatever Tcl Step 6 discovered — the harness pins
# no version) + the build tree ($BLD, which resolves the
# recipe's relative `-I.`). These dirs, the TU list (${TUS[@]}) and the recipe
# defines (${RECIPE_DEFS[@]}, already `-D`-stripped) are the SAME inputs the
# per-file CLI fed; here they populate a `.dss-project.json` manifest instead.
declare -a INC_DIRS=()
for d in "${SQLITE_INCS[@]}" "${THIRD_PARTY_INCS[@]}" "$BLD"; do INC_DIRS+=("$d"); done
# ★ macOS: the Xcode SDK include dir goes on the path LAST
# (D-CSUBSET-DARWIN-PLATFORM-MACROS follow-on). Now that a macho target
# predefines __APPLE__, the Darwin-guarded arms of sqlite compile for the first
# time and reach genuinely Apple-only SDK headers — <TargetConditionals.h>,
# <sys/sysctl.h>, <sys/param.h>, <sys/mount.h>, <sys/file.h>, <malloc/malloc.h>.
# None ships a DSS descriptor, so `D-INCLUDE-ANGLE-SOURCE-FALLBACK` parses the
# REAL SDK header — but only if its directory is on the include path. A native
# clang gets this dir implicitly via -isysroot; DSS is told explicitly.
# Ordered LAST so sqlite's own headers and the staged third-party dirs still win,
# and because the angle resolver consults SHIPPED DESCRIPTORS FIRST, adding this
# dir cannot shadow the OS descriptors (<stdio.h>/<unistd.h>/… keep resolving to
# stdio.json/unistd.json; only descriptor-less Apple headers fall through to it).
# Linux/Windows no-op: gated on HOST_OS, and `sdk_prefix` is empty without xcrun.
if [[ "$HOST_OS" == "macos" ]]; then
  _sdk_inc="$(sdk_prefix)"
  [[ -n "$_sdk_inc" && -d "$_sdk_inc/usr/include" ]] && INC_DIRS+=("$_sdk_inc/usr/include")
fi

# generate_manifest <spec> <tcl-lib> <z-lib> <out-manifest> — write a project
# manifest reproducing the recipe: language c-subset, profile cli, ONE target
# (the leg's <targetName>:<formatName> spec), artifactName testfixture, the full
# TU set as ABSOLUTE `sources`, the `includes` dirs, the `defines` (a leading
# `-D` stripped defensively; an empty SQLITE_PRIVATE= value preserved), and the
# leg's (tcl, z) libraries as `resolveLibraries`. The arrays reach python3 via
# the ENVIRONMENT (newline-joined) — never argv — so 185 paths can't overflow
# ARG_MAX or trip quoting. Echoes the array counts for the build log.
generate_manifest() {
  local spec="$1" tcl_lib="$2" z_lib="$3" out="$4"
  MANIFEST_SPEC="$spec" MANIFEST_TCL="$tcl_lib" MANIFEST_Z="$z_lib" \
  MANIFEST_TUS="$(printf '%s\n' "${TUS[@]}")" \
  MANIFEST_INCS="$(printf '%s\n' "${INC_DIRS[@]}")" \
  MANIFEST_DEFS="$(printf '%s\n' "${RECIPE_DEFS[@]}")" \
  python3 - "$out" <<'PY'
import json, os, sys
out = sys.argv[1]
def lines(name):
    return [x for x in os.environ.get(name, "").splitlines() if x]
def strip_d(d):
    return d[2:] if d.startswith("-D") else d   # RECIPE_DEFS is pre-stripped; defensive
manifest = {
    "language":         "c-subset",
    "artifactProfile":  "cli",
    "targets":          [os.environ["MANIFEST_SPEC"]],
    "artifactName":     "testfixture",
    "sources":          lines("MANIFEST_TUS"),
    "includes":         lines("MANIFEST_INCS"),
    "defines":          [strip_d(d) for d in lines("MANIFEST_DEFS")],
    "resolveLibraries": [os.environ["MANIFEST_TCL"], os.environ["MANIFEST_Z"]],
}
with open(out, "w", encoding="utf-8") as f:
    json.dump(manifest, f, indent=2)
    f.write("\n")
print("sources=%d includes=%d defines=%d" % (
    len(manifest["sources"]), len(manifest["includes"]), len(manifest["defines"])))
PY
}
compile_time_suffix() { local t; t="$(grep -oE 'compile time [^[:space:]]+' "$1" 2>/dev/null | tail -1)" || true; [[ -n "$t" ]] && printf '  (%s)' "$t" || true; }
# >>> dss:fresh-inode >>>
# ── FRESH-INODE INSTALL (macOS only) ─────────────────────────────────────────
# ★ ANCHOR, ONE LINE, DO NOT WRAP: D-HARNESS-MACOS-PROVENANCE-KILLS-OVERWRITTEN-FIXTURE
#
# WHY THE EXEC'D FILE MUST BE A NEW INODE, AND WHY AN IN-PLACE REBUILD IS NOT ONE.
# dss-code-prime writes the fixture with an O_TRUNC open (src/link/writer.cpp:
# `std::ofstream out(path, … | std::ios::trunc)`), so a rebuild replaces the
# CONTENT of the SAME inode the previous run exec'd thousands of times. On macOS 26
# that inode can pick up a PERMANENT exec DENY: every exec is SIGKILLed (137)
# before one byte of output, the kernel logging
#     proc N: load code signature error 2 for file "testfixture"
#     (AppleSystemPolicy) ASP: Security policy would not allow process: N, <path>
# A full tier run died exactly there — FAIL:11 fixture ABORT(s), a ZERO-BYTE
# corpus.log, 12 unit groups NOT REACHED after the 10-resume budget was spent.
#
# MEASURED on the live soured artifact (2026-07-31, macOS 26.5.2 / arm64):
#   · exec at the soured path                     → 137, 137, 137, no output
#   · byte-identical copy at ANY other path        → 0, 0, 0
#         ⇒ NOT the bytes, NOT the Mach-O layout, NOT the ad-hoc signature, and
#           NOT the shared `com.dss.macho_arm64_exit` identifier.
#   · HARD LINK to the soured inode (DIFFERENT path, SAME inode) → 137, 137, 137
#         ⇒ the verdict is bound to the INODE, not to the path string.
#   · overwriting the soured path IN PLACE with known-good bytes → STILL 137, 137,
#     137.  ⇒ THE HARNESS-FATAL PART: a rebuild cannot clear it, so once the path
#           sours every later run is dead on arrival.
#   · renaming a fresh temp file over that same path → 0, 0, 0, while the old inode,
#     kept alive by the hard link, stayed dead.
# `codesign -f -s -` appearing to "repair" a soured binary is the SAME effect, not
# a different one: codesign writes a temp file and RENAMES it (inode measured to
# change across a re-sign), so it was always the new inode doing the work.
#
# Hence: do NOT re-sign, do NOT strip xattrs (the soured file and a running copy
# carry a BYTE-IDENTICAL com.apple.provenance), do NOT touch AMFI. The artifact is
# already valid — the copy control proves it. What must change is that the file
# Step 8 EXECS is an inode that has never been exec'd under different content.
# The PATH stays stable (the runner, the resume engine, the pre-flight sweep and
# the Step-9 verdict all read `<outd>/<fmt>/testfixture`); only the inode behind it
# is swapped: copy to a sibling temp name, then rename over. `mv` inside ONE
# directory is rename(2) on the same filesystem, so the path ends up owning the
# temp file's brand-new inode and the compiler-truncated one is unlinked.
# The inode is RE-READ afterwards and a non-change is FATAL — a later
# "simplification" back to an in-place overwrite fails loudly here instead of
# silently restoring the 137s at the end of a multi-hour corpus.
# Linux/WSL: gated on HOST_OS at the call site — there is no AppleSystemPolicy and
# no such per-inode verdict, and `stat -f` is BSD-only, so nothing runs there.
#
# `cp -p`, not a plain `cp` + `chmod 755`: writer.cpp sets the fixture's mode by
# ADDING 0111 on top of whatever the umask left (D-OUTPUT-EXEC-BIT), so hardcoding
# 755 here would WIDEN the permissions of anyone running under a tighter umask.
# -p reproduces the compiler's mode (and mtime) exactly; BSD cp treats an
# unpreservable uid/gid as non-fatal, so -p cannot turn a good build into a
# spurious abort. The `-x` assert then proves the installed file is still
# executable rather than assuming it.
fixture_fresh_inode() {
  local p="$1" tmp old new
  old="$(/usr/bin/stat -f '%i' "$p")" || return 1
  tmp="$p.freshinode.$$"
  rm -f "$tmp"
  cp -p "$p" "$tmp" && [[ -x "$tmp" ]] && mv -f "$tmp" "$p" || { rm -f "$tmp"; return 1; }
  new="$(/usr/bin/stat -f '%i' "$p")" || return 1
  # The whole point of this function. If the inode did not change, the rename did
  # not happen and Step 8 would exec the very inode that carries the DENY.
  [[ "$new" != "$old" ]] || return 2
  printf '%s\n' "$new"
}
# <<< dss:fresh-inode <<<
declare -a PREFLIGHT_KILLS=()
for leg in "${LEG_ORDER[@]}"; do
  spec="${LEG_SPEC[$leg]}"; fmt="${spec##*:}"; outd="$OUT_DIR/$leg"; log="$outd/compile.log"
  manifest="$outd/$leg.dss-project.json"
  mkdir -p "$outd"
  # >>> dss:preflight >>>
  # PRE-FLIGHT HYGIENE — FIRST thing this leg does, mirroring the .ps1. The hazard
  # differs by platform but is the same shape: the compiler below WRITES this leg's
  # testfixture, and on POSIX writing an executable that a leftover process is still
  # running fails with ETXTBSY ("Text file busy"), while on Windows the equivalent
  # output wipe fails with "access denied". We hold the run lock, so anything still
  # running THIS leg's fixture path is a leftover by construction.
  # Command substitution, NOT `done < <(...)`. A process substitution runs in a
  # SUBSHELL, so a failure inside it (or a `die`) exits only that subshell and the
  # harness sails on with the sweep silently not done — MEASURED while verifying
  # this very step. `$(...)` propagates the status, so a broken sweep is loud.
  preflight_out="$(stop_our_fixtures "$outd/$fmt/testfixture" 'pre-flight')" \
    || die "[$leg] the pre-flight fixture sweep FAILED — refusing to build over a possibly-running fixture."
  while IFS= read -r k; do
    [[ -z "$k" ]] || { warn "[$leg] LEFTOVER FIXTURE: $k"; PREFLIGHT_KILLS+=("$k"); }
  done <<< "$preflight_out"
  # <<< dss:preflight <<<
  tcl_lib="$(leg_tcl_lib "$leg")"; z_lib="$(leg_z_lib "$leg")"
  [[ -n "$tcl_lib" && -n "$z_lib" ]] || die "[$leg] could not resolve tcl/zlib libraries."
  info "[$leg] $spec — ${#TUS[@]} TUs → testfixture (resolve: $(basename "$tcl_lib"), $(basename "$z_lib"))"
  counts="$(generate_manifest "$spec" "$tcl_lib" "$z_lib" "$manifest")" \
    || die "[$leg] manifest generation failed (python3) — see above."
  info "[$leg] manifest → $manifest ($counts)"
  # A project build routes each target to <output>/<formatName>/<artifactName>.
  # dss-code-prime returns EXIT 0 even on fatal errors → judge from `error[` + the binary.
  "$DSS_BIN" --project "$manifest" --config="$DSS_CONFIG" --output "$outd" --time >"$log" 2>&1 || true
  bin="$outd/$fmt/testfixture"
  if grep -qE 'error\[' "$log" || [[ ! -x "$bin" ]]; then
    COMPILE_FAILS=$((COMPILE_FAILS + 1))
    if grep -qE 'error\[' "$log"; then
      warn "[$leg] build FAILED$(compile_time_suffix "$log") — first diagnostics ($log):"
      { grep -m3 -E 'error\[' "$log" || head -3 "$log"; } 2>/dev/null | sed 's/^/      /'
    else
      warn "[$leg] build FAILED$(compile_time_suffix "$log") — 0 error[ but no executable at $bin"
    fi
  else
    # >>> dss:fresh-inode-install >>>
    # Give the just-built fixture a BRAND-NEW inode before anyone execs it —
    # D-HARNESS-MACOS-PROVENANCE-KILLS-OVERWRITTEN-FIXTURE (rationale + the
    # measurements at `fixture_fresh_inode`, above). SUCCESS BRANCH ONLY: on a
    # failed build the path is left exactly as the compiler left it, so the
    # `error[` / `-x "$bin"` verdict above keeps its current meaning. Fail-loud:
    # a copy/rename that does not land, or that lands on the SAME inode, is a
    # hard stop — continuing would exec the inode that carries the DENY and burn
    # the whole corpus on 137s.
    if [[ "$HOST_OS" == "macos" ]]; then
      fresh_ino="$(fixture_fresh_inode "$bin")" || die "[$leg] could not install the fixture on a FRESH INODE at $bin (rc=$?).
      D-HARNESS-MACOS-PROVENANCE-KILLS-OVERWRITTEN-FIXTURE: macOS pins a permanent
      exec DENY to the INODE, and an in-place rebuild inherits it — the fixture
      would be SIGKILLed (137) on every unit with no output at all. Refusing to
      run the corpus against a fixture that may still be sitting on the old inode."
      info "[$leg] fresh-inode install: $bin now inode $fresh_ino"
    fi
    # <<< dss:fresh-inode-install <<<
    FIXTURE["$leg"]="$bin"; COMPILE_OK["$leg"]=1
    pass "[$leg] testfixture -> $bin$(compile_time_suffix "$log")"
  fi
done

# ── Step 8 — run the .test UNIT CORPUS through each leg's fixture ─────────────
step "8/9  Run SQLite unit corpus ($DSS_TIER.test) on each leg + classify failures"
# DOWNGRADE the clone lock: everything from here is READ-ONLY on the checkout (the
# fixture sources .test files straight out of it) but it lasts for HOURS. A read
# marker blocks a mutating run without blocking a second corpus run.
dss_clone_lock_read "$SQLITE_DIR" "$(basename "$0") corpus run — tier $DSS_TIER, legs ${LEG_ORDER[*]}"
info "clone lock: downgraded to READ for the corpus run ($DSS_CLONE_LOCK_DIR)"
TEST_FILE="${DSS_TEST_FILE:-$SQLITE_DIR/test/$DSS_TIER.test}"
[[ -f "$TEST_FILE" ]] || die "test file not found: $TEST_FILE"
# The corpus directory the tier script lives in — where permutations.test and the
# ~1.3k .test units are. READ-ONLY to this harness: nothing is ever written here.
TESTDIR_SRC="$(cd "$(dirname "$TEST_FILE")" && pwd)"
read -r -a CONFOUND_PATTERNS <<< "$DSS_CONFOUNDS"
# Tier exclusions (see DSS_TIER_EXCLUDES above) — announced BEFORE the run so the
# reduction is on the record even if a leg never reaches a summary line, and
# carried into every leg's Step-9 verdict via $EXCL_NOTE.
read -r -a EXCLUDE_PATTERNS <<< "$DSS_TIER_EXCLUDES"
EXCL_NOTE=""
if [[ ${#EXCLUDE_PATTERNS[@]} -gt 0 ]]; then
  QUICKTEST_OMIT="$(IFS=,; printf '%s' "${EXCLUDE_PATTERNS[*]}")"; export QUICKTEST_OMIT
  EXCL_NOTE="  [NOT FULL COVERAGE: ${#EXCLUDE_PATTERNS[@]} file pattern(s) EXCLUDED from the $DSS_TIER tier via QUICKTEST_OMIT -- ${EXCLUDE_PATTERNS[*]}]"
  warn "tier EXCLUSIONS active — this is NOT full-corpus coverage"
  info "      QUICKTEST_OMIT=$QUICKTEST_OMIT  (sqlite's own hook, test/permutations.test)"
  info "      drops these file(s) from every \$allquicktests-derived permutation (still run under 'full'): ${EXCLUDE_PATTERNS[*]}"
fi

declare -A UNIT_VERDICT=()     # leg -> PASS / FAIL:<reasons> / skipped
# per-leg resume-engine bookkeeping, surfaced in Step 9
declare -A LEG_SEGMENTS=() LEG_RESUMES=() LEG_FILESDONE=() LEG_LEDGER=() LEG_ABORTS=() LEG_NOTREACHED=() LEG_HYGIENE=()
UNIT_FAILS=0
# tester.tcl's cmdlinearg(testdir) default: the fixture `file mkdir`s this subdir of
# its CWD and cd's into it before any .test body runs, so a test's relative
# `./libtestloadext.so` resolves HERE. The harness passes no --testdir override.
SQLITE_TESTDIR_SUBDIR="testdir"
# How a shared object is produced — decided by the leg's TARGET object format, never
# by the host's.
leg_shared_flags() { case "${LEG_SPEC[$1]##*:}" in macho64-*) printf '%s' '-dynamiclib';; *) printf '%s' '-shared -fPIC';; esac; }
# [D-HARNESS-ARM64-LEG-HOST-ARCH-HELPER-SO] Stage the helper shared object the
# loadext corpus dlopen()s, built for THE LEG'S TARGET.
#
# sqlite's test/loadext.test builds this helper itself, from src/test_loadext.c with
# a HARDCODED `gcc`, but only `if {![file exists $testextension]}`. On a cross leg
# that self-build is silently WRONG: qemu-aarch64 passes the guest's `exec gcc`
# through to the HOST kernel, so the aarch64 fixture is handed a HOST x86-64 shared
# object. Every dlopen() then fails, the extension never registers half(), and all
# 16 loadext-* rows report `[1 {no such function: half}]` — a harness artefact that
# reads exactly like a genuine DSS miscompile. Pre-staging a target-correct helper
# makes `[file exists …]` true, so loadext.test never shells out to the wrong
# compiler. Include dirs come from the sqlite TREE ($SQLITE_DIR/src for
# sqlite3ext.h, $BLD for the generated sqlite3.h) — loadext.test's own `-I. -I..`
# find neither from the run dir and silently fall through to a SYSTEM sqlite3.h of
# an unrelated version.
#
# FAIL-LOUD: an unobtainable leg compiler DIES. Falling back to the host compiler is
# precisely the defect above, and it would be invisible in the results.
stage_loadext_extension() {    # stage_loadext_extension <leg> <rundir>
  local leg="$1" rundir="$2"
  local cc="${LEG_CC[$leg]}" pkg="${LEG_CC_PKG[$leg]:-}"
  local src="$SQLITE_DIR/src/test_loadext.c"
  local dst="$rundir/$SQLITE_TESTDIR_SUBDIR/libtestloadext.so"
  [[ -f "$src" ]] || die "[$leg] sqlite extension source not found: $src"
  if ! command -v "$cc" >/dev/null 2>&1 && [[ -n "$pkg" ]]; then
    warn "[$leg] target C compiler '$cc' not found — installing $pkg"
    pkg_install "$pkg"
    hash -r
  fi
  command -v "$cc" >/dev/null 2>&1 || die \
"[$leg] target C compiler '$cc' not found${pkg:+ (apt: $pkg)} — it builds this leg's libtestloadext.so, the extension the loadext corpus dlopen()s.
      NOT falling back to the host compiler: sqlite's loadext.test would then build a HOST-arch extension the $leg fixture cannot load, and every
      loadext-* test would false-red as a genuine DSS failure [D-HARNESS-ARM64-LEG-HOST-ARCH-HELPER-SO]."
  mkdir -p "$(dirname "$dst")"
  # leg_shared_flags is a deliberate word-split flag list.
  "$cc" $(leg_shared_flags "$leg") -I"$SQLITE_DIR/src" -I"$BLD" -o "$dst" "$src" \
    || die "[$leg] could not build the loadext helper extension: $cc $(leg_shared_flags "$leg") -o $dst $src"
  info "[$leg] loadext helper -> $dst (built by $cc)"
}
for leg in "${LEG_ORDER[@]}"; do
  if [[ "${COMPILE_OK[$leg]:-0}" != "1" ]]; then
    UNIT_VERDICT["$leg"]="skipped (compile failed)"; warn "[$leg] corpus skipped — step 7 did not compile the fixture"; continue
  fi
  bin="${FIXTURE[$leg]}"; rundir="$OUT_DIR/$leg/run"; rm -rf "$rundir"; mkdir -p "$rundir"
  stage_loadext_extension "$leg" "$rundir"
  runlog="$OUT_DIR/$leg/corpus.log"
  ledger="$OUT_DIR/$leg/corpus-units.txt"
  # scratch lives in the leg's OUT dir — NEVER in the sqlite clone (the .sh runs the
  # corpus straight out of it, and a stray file there breaks the next `git pull
  # --rebase` and poisons the .ps1's staged copy of the same tree).
  scratch="$OUT_DIR/$leg/.corpus"; rm -rf "$scratch"; mkdir -p "$scratch"
  corpus_files "$TESTDIR_SRC" > "$scratch/files.txt"
  tier_permutations "$TEST_FILE" > "$scratch/perms.txt"
  declare -a TIER_PERMS=(); mapfile -t TIER_PERMS < "$scratch/perms.txt"
  tier_prefixes "$TESTDIR_SRC/permutations.test" > "$scratch/prefixes.txt"
  declare -a TIER_PREFIXES=(); mapfile -t TIER_PREFIXES < "$scratch/prefixes.txt"

  # >>> dss:corpus-loop >>>
  # Segment queue, one record per fixture invocation, fields separated by US
  # (\x1f):  kind | perm | label | patternfile | arg1 | arg2
  # Segment 0 is EXACTLY today's invocation (`fixture <tier>.test`) so a run with
  # no abort is bit-for-bit the run it always was; resume segments are only ever
  # spliced in by an abort.
  US=$'\x1f'
  SEGQ=("tier${US}${US}$DSS_TIER.test${US}${US}$TEST_FILE${US}")
  declare -a SEG_LOGS=() SEG_LABELS=() SEG_RCS=() SEG_COUNTS=() ABORTS=() ABORT_ROWS=() NOT_REACHED=() HYGIENE=() CALIBRATION=()
  sum_tests=0; sum_errors=0; n_summarised=0; der_tests=0; der_errors=0; n_derived=0
  # Carry Step 7's pre-flight kills into this leg's hygiene record, then sweep again:
  # a leftover fixture holds file handles (the abort class this engine exists for IS
  # a leaked handle) and can make this run fail for a reason it did not cause.
  for k in ${PREFLIGHT_KILLS[@]+"${PREFLIGHT_KILLS[@]}"}; do HYGIENE+=("$k"); done
  sweep_out="$(stop_our_fixtures "$bin" 'pre-corpus')" \
    || die "[$leg] the pre-corpus fixture sweep FAILED — refusing to start the corpus with a possible leftover holding handles."
  while IFS= read -r k; do
    [[ -z "$k" ]] || { warn "[$leg] LEFTOVER FIXTURE: $k"; HYGIENE+=("$k"); }
  done <<< "$sweep_out"
  [[ -z "$LOCK_STOLEN" ]] || HYGIENE+=("took over a STALE run lock left by PID $LOCK_STOLEN")
  [[ -z "${DSS_CLONE_NOTES:-}" ]] || HYGIENE+=("shared-clone lock: ${DSS_CLONE_NOTES%%; }")
  ps_enum_available || HYGIENE+=("leftover-fixture detection UNAVAILABLE on this host (ps(1) cannot enumerate) — a stray fixture would go unnoticed")
  seg_i=0; resumes=0; last_boundary=""; total_tests=0; total_errors=0; files_done=0
  seg_summary=""; all_fails=""
  while [[ $seg_i -lt ${#SEGQ[@]} ]]; do
    IFS="$US" read -r s_kind s_perm s_label s_patfile s_arg1 s_arg2 <<< "${SEGQ[$seg_i]}"
    if [[ $seg_i -eq 0 ]]; then
      seglog="$runlog"
      info "[$leg] running $DSS_TIER.test$( [[ -n "${LEG_PREFIX[$leg]}" ]] && printf ' (under %s)' "${LEG_PREFIX[$leg]}" )…"
    else
      seglog="$OUT_DIR/$leg/corpus.resume$seg_i.log"
      info "[$leg] segment $((seg_i + 1)): $s_label$( [[ -n "$s_patfile" ]] && printf '  (SQLITE_TEST_PATTERN_LIST: %s candidate file(s))' "$(wc -l < "$s_patfile")" )"
    fi
    declare -a seg_argv=("$s_arg1"); [[ -n "$s_arg2" ]] && seg_argv+=("$s_arg2")
    # SQLITE_TEST_PATTERN_LIST is a Tcl LIST of globs; corpus basenames are bare
    # words, so a whitespace join is a valid list.
    if [[ -n "$s_patfile" ]]; then SQLITE_TEST_PATTERN_LIST="$(tr '\n' ' ' < "$s_patfile")"; export SQLITE_TEST_PATTERN_LIST
    else unset SQLITE_TEST_PATTERN_LIST; fi
    run_fixture_segment "$leg" "$bin" "$seglog" "${seg_argv[@]}"
    segrc="$SEG_RC"
    unset SQLITE_TEST_PATTERN_LIST
    if [[ -n "$SEG_KILL_REASON" ]]; then
      warn "[$leg] segment $((seg_i + 1)) HUNG — killed: $SEG_KILL_REASON"
      HYGIENE+=("segment $((seg_i + 1)) TIMED OUT and was killed — $SEG_KILL_REASON")
    fi
    # POST-SEGMENT HYGIENE — a segment must not carry its file handles into the
    # next one, nor outlive the run.
    post_out="$(stop_our_fixtures "$bin" "after segment $((seg_i + 1))")" \
      || die "[$leg] the post-segment fixture sweep FAILED — a leftover could poison the next segment."
    while IFS= read -r k; do
      [[ -z "$k" ]] || { warn "[$leg] LEFTOVER FIXTURE: $k"; HYGIENE+=("$k"); }
    done <<< "$post_out"
    SEG_LOGS+=("$seglog"); SEG_LABELS+=("$s_label"); SEG_RCS+=("$segrc")
    facts_f="$scratch/facts.$seg_i"
    parse_segment "$seglog" "$facts_f"
    s_sum="$(fact S "$facts_f")"; s_perm_log="$(fact P "$facts_f")"
    s_last="$(fact T "$facts_f")"; s_done="$(fact D "$facts_f")"
    s_nf="$(fact N "$facts_f")";   s_gaveup="$(fact G "$facts_f")"
    s_ok="$(fact K "$facts_f")";   s_fx="$(fact Q "$facts_f")"
    files_done=$((files_done + s_nf))
    # failing NAMES always flow into the classifier, summary or not (VERIFIED on the
    # real `all` run: mm-backup4-3.3 came from an ABORTED segment and was still
    # reported as a genuine failure — it was only the COUNTS that dropped it).
    all_fails="$all_fails $(facts X "$facts_f" | tr '\n' ' ')"
    seg_derived_tests=$((s_ok + s_fx + 1))
    seg_i=$((seg_i + 1))
    if [[ -n "$s_sum" ]]; then
      seg_summary="$s_sum"
      seg_e="$(fact E "$facts_f")"; seg_c="$(fact C "$facts_f")"
      sum_errors=$((sum_errors + seg_e)); sum_tests=$((sum_tests + seg_c)); n_summarised=$((n_summarised + 1))
      SEG_COUNTS+=("tests: $(group_digits "$seg_c") / errors: $seg_e   [source: sqlite's own summary line; per-test derivation independently gives $(group_digits "$seg_derived_tests")]")
      # self-calibration: where sqlite DID report, the derivation must agree. A
      # mismatch means the aborted-segment figures are off by a comparable margin,
      # and that is said out loud rather than assumed away.
      [[ "$seg_derived_tests" -eq "$seg_c" ]] \
        || CALIBRATION+=("$s_label: sqlite says $seg_c tests, the per-test derivation says $seg_derived_tests (delta $((seg_derived_tests - seg_c)))")
      total_errors=$((total_errors + seg_e))
      total_tests=$((total_tests + seg_c))
      # A completed segment can still have stopped EARLY: `*** Giving up...` is
      # tester.tcl hitting --maxerror (default 1000) and finalising. It DOES print a
      # summary, so it would otherwise read as a full run. Say so instead.
      if [[ -n "$s_gaveup" ]]; then
        warn "[$leg] segment $seg_i stopped EARLY at the --maxerror cap ('*** Giving up...') — this is NOT full coverage"
        NOT_REACHED+=("every file after ${s_done:-(none)} in '$s_label' — the fixture hit its --maxerror cap and finalised early (raise it with --maxerror=N)")
      fi
      continue
    fi

    # ── ABORT ────────────────────────────────────────────────────────────────
    # An aborted segment is NOT an empty segment. It printed no summary, so its work
    # is counted from its per-test lines (see the derivation note at the union) —
    # otherwise the totals silently omit everything it did, and a regression inside
    # it never reaches the headline at all.
    der_tests=$((der_tests + seg_derived_tests)); der_errors=$((der_errors + s_fx)); n_derived=$((n_derived + 1))
    total_tests=$((total_tests + seg_derived_tests)); total_errors=$((total_errors + s_fx))
    SEG_COUNTS+=("tests: $(group_digits "$seg_derived_tests") / errors: $s_fx   [source: DERIVED from per-test lines — $(group_digits "$s_ok") ' Ok' + $s_fx '! expected:' + 1; this segment aborted and printed no summary]")
    perm="$s_perm_log"
    [[ -n "$perm" ]] || perm="$s_perm"
    [[ -n "$perm" || ${#TIER_PERMS[@]} -ne 1 ]] || perm="${TIER_PERMS[0]}"
    # A KILLED segment (stall/cap) has NO Tcl traceback at all — and that is
    # precisely when resume matters most, so the traceback cannot be the only
    # source. Fall back to the permutation PREFIX sqlite stamps on every test name
    # (`<perm>.<test>`, run_tests -prefix), then — for an initial tier segment that
    # never showed ANY later permutation's prefix — to the tier's first permutation
    # (all.test's first suite, `full`, is the one declared with -prefix "").
    perm_inferred=""
    if [[ -z "$perm" && -n "$s_last" ]]; then
      for p in ${TIER_PERMS[@]+"${TIER_PERMS[@]}"}; do
        if [[ "$s_last" == "$p."* ]]; then perm="$p"; perm_inferred="from the test-name prefix"; break; fi
      done
    fi
    if [[ -z "$perm" && -n "$s_last" && "$s_kind" == "tier" && -z "$s_perm" && ${#TIER_PERMS[@]} -gt 0 ]]; then
      perm="${TIER_PERMS[0]}"
      perm_inferred="INFERRED — no permutation prefix ever appeared, so the run never left '${TIER_PERMS[0]}', the tier's first suite"
    fi
    abort_file="$(resolve_abort_file "$s_last" "$scratch/files.txt")"
    # The boundary must STRICTLY advance every resume, or an aborting file could be
    # re-entered forever. If the aborting file could not be named (or is not past
    # the last completed one), fall back to the last completed file, then force the
    # boundary one corpus entry forward.
    boundary="$abort_file"; forced=0
    if [[ -z "$boundary" ]] || ! str_gt "$boundary" "$s_done"; then boundary="$s_done"; fi
    if ! str_gt "$boundary" "$last_boundary"; then
      forced=1
      boundary="$(files_after "$last_boundary" "$scratch/files.txt" | head -1)"
    fi
    ABORTS+=("${perm:-?}/${abort_file:-?}")
    if [[ -n "$SEG_KILL_REASON" ]]; then how="KILLED: $SEG_KILL_REASON"; else how="rc=$segrc"; fi
    ABORT_ROWS+=("segment $seg_i: permutation '${perm:-?}' file '${abort_file:-?}' after test '${s_last:-?}' ($how) -> $seglog")
    if [[ -n "$SEG_KILL_REASON" ]]; then
      warn "[$leg] ABORT #${#ABORTS[@]} — segment $seg_i ('$s_label') was KILLED after it $SEG_KILL_REASON"
    else
      warn "[$leg] ABORT #${#ABORTS[@]} — segment $seg_i ('$s_label') exited rc=$segrc with NO summary line"
    fi
    info "        permutation        : ${perm:-(UNDETERMINED)}${perm_inferred:+   [$perm_inferred]}"
    info "        last file completed: ${s_done:-(none)}"
    info "        died inside file   : ${abort_file:-(unresolved)}   last test: ${s_last:-(none)}"
    # The unit that died NEVER goes unreported — named when we can name it,
    # described by what we do know when we cannot. Silence about a unit is the defect.
    if [[ -n "$abort_file" ]]; then
      NOT_REACHED+=("the REMAINDER of $abort_file under permutation '${perm:-?}' (aborted at ${s_last:-?})")
    else
      if [[ "$forced" == 1 ]]; then
        what="the resume boundary was FORCED to ${boundary:-the end of the corpus}, so that one file may have been skipped without a verdict"
      else
        what="the next segment resumes from ${boundary:-the end of the corpus} and will RE-ATTEMPT it"
      fi
      NOT_REACHED+=("the UNNAMED file that aborted under permutation '${perm:-?}' after ${s_done:-the start of the permutation} — the log named no resolvable corpus file (last test: ${s_last:-none}); $what")
    fi
    tail -6 "$seglog" 2>/dev/null | sed 's/^/      /'

    if [[ -z "$boundary" ]]; then
      warn "[$leg] the abort is at the END of the corpus file list — nothing left to resume."; continue
    fi
    if [[ -z "$perm" ]]; then
      warn "[$leg] CANNOT RESUME — the aborting permutation could not be determined from the log."
      NOT_REACHED+=("every unit after $boundary — no resume was possible (permutation undetermined; see $seglog)"); continue
    fi
    perm_idx=-1
    for ((k = 0; k < ${#TIER_PERMS[@]}; k++)); do [[ "${TIER_PERMS[$k]}" == "$perm" ]] && { perm_idx=$k; break; }; done
    if [[ $resumes -ge $DSS_MAX_RESUMES ]]; then
      warn "[$leg] RESUME BUDGET EXHAUSTED ($DSS_MAX_RESUMES) — stopping. Raise DSS_MAX_RESUMES to go further."
      rest=""
      [[ $perm_idx -ge 0 && $perm_idx -lt $((${#TIER_PERMS[@]} - 1)) ]] && rest=" and every permutation after '$perm' (${TIER_PERMS[*]:$((perm_idx + 1))})"
      NOT_REACHED+=("every unit after $boundary in '$perm'$rest — resume budget ($DSS_MAX_RESUMES) exhausted"); continue
    fi
    # (a) the rest of the aborting permutation, via sqlite's own file-selection hook.
    resumes=$((resumes + 1)); last_boundary="$boundary"
    patfile="$scratch/after.$resumes"
    files_after "$boundary" "$scratch/files.txt" > "$patfile"
    info "        -> resume $resumes/$DSS_MAX_RESUMES: permutations.test $perm, corpus files after $boundary"
    declare -a TAIL_SEGS=("perm${US}${perm}${US}permutations.test $perm (after $boundary)${US}${patfile}${US}${TESTDIR_SRC}/permutations.test${US}${perm}")
    # (b) the tier continued from the NEXT permutation — the ORIGINAL tier script,
    # so every ifcapable/platform guard is evaluated by sqlite exactly as always.
    if [[ "$s_kind" != "perm" ]]; then
      if [[ $perm_idx -lt 0 ]]; then
        warn "[$leg] permutation '$perm' is not named by ${TEST_FILE##*/} — cannot continue the tier past it."
        NOT_REACHED+=("every permutation after '$perm' in ${TEST_FILE##*/} — '$perm' is not one of its run_test_suite entries")
      elif [[ $perm_idx -lt $((${#TIER_PERMS[@]} - 1)) ]]; then
        nextperm="${TIER_PERMS[$((perm_idx + 1))]}"
        TAIL_SEGS+=("tier${US}${nextperm}${US}${TEST_FILE##*/} --start=${nextperm}:${US}${US}${TEST_FILE}${US}--start=${nextperm}:")
        info "        -> then: ${TEST_FILE##*/} --start=${nextperm}:  (permutations $nextperm..${TIER_PERMS[-1]})"
      fi
    fi
    SEGQ=("${SEGQ[@]:0:$seg_i}" "${TAIL_SEGS[@]}" "${SEGQ[@]:$seg_i}")
    unset TAIL_SEGS
  done

  # ── union the segments + classify ──────────────────────────────────────────
  nseg="${#SEG_LOGS[@]}"
  faillist="$(printf '%s\n' $all_fails | LC_ALL=C sort -u | tr '\n' ' ')"
  # TWO SOURCES, COUNTED SEPARATELY AND NAMED. A segment that ABORTED prints no
  # summary line — but it is not an empty segment: the real `all` run's two aborted
  # segments between them ran 4.1M passing tests and one genuine failure. Summing
  # only the summary lines under-reported that run by ~98% and would have hidden a
  # regression inside those segments from the totals entirely.
  #
  # DERIVATION for an aborted segment, from its per-test lines:
  #     tests  = (lines ending " Ok") + (`! <name> expected:` lines) + 1
  #     errors = (`! <name> expected:` lines)
  # The +1 is STRUCTURAL, not a fudge: finalize_testing reports `[incr_ntest]`
  # (tester.tcl ~1273) and incr_ntest INCREMENTS THEN RETURNS, so sqlite's own figure
  # is always one more than the tests actually run. Every do_test increments that
  # counter and prints exactly one of the two line kinds above.
  # CALIBRATED: exact (delta 0) against all four real logs that DO carry a summary,
  # and re-checked EVERY run above, so it cannot quietly drift from sqlite's own
  # arithmetic without saying so.
  # For a single clean segment the summary text is the fixture's own, byte for byte.
  union_summary="$total_errors errors out of $(group_digits "$total_tests") tests (union of $nseg segment(s))"
  if [[ $nseg -eq 1 ]]; then summary="$seg_summary"; else summary="$union_summary"; fi
  derivation=""
  if [[ "$n_derived" -gt 0 ]]; then
    derivation="$n_summarised segment summary/summaries: $(group_digits "$sum_tests") test(s), $sum_errors error(s) · $n_derived ABORTED segment(s): $(group_digits "$der_tests") test(s), $der_errors error(s) counted from per-test lines (' Ok' + '! <name> expected:' + 1 — an aborted segment prints no summary)"
    [[ ${#CALIBRATION[@]} -eq 0 ]] \
      || derivation="$derivation  [!! the derivation DISAGREES with sqlite on ${#CALIBRATION[@]} segment(s) that did report — treat the aborted-segment figures as APPROXIMATE: ${CALIBRATION[*]}]"
  fi
  # A confound may be SCOPED to an execution mode: `native:<re>` or `emulated:<re>`
  # (bare `<re>` = every leg). The mode is derived from whether the leg needs a
  # runner prefix to execute its binaries — a per-leg TARGET fact, never a host or
  # arch identity test, so a future emulated target inherits this for free.
  # WHY SCOPES EXIST: `writecrash-` fails ONLY under qemu, because the emulator
  # writes "qemu: uncaught target signal 6" into the child's captured output that
  # sqlite's crash harness compares (D-SQLITE-ARM64-WRITECRASH-QEMU-ABORT-ARTIFACT,
  # proven with a gcc-built aarch64 abort()). The native host leg passes all 988 of
  # those assertions, so a BARE `^writecrash-` would suppress a future genuine host
  # regression. An unscoped confound is exactly the silent-classification fault
  # refused for date-2.4c (D-SQLITE-CONFOUND-LIST-DRIVER-ASYMMETRY).
  # NOT `local`: this block runs at TOP LEVEL (the nearest function,
  # stage_loadext_extension, closes at ~1241), so `local` is a fatal
  # "can only be used in a function" — it aborted a completed 13 h arm64 run at the
  # classification step, AFTER the whole corpus had been executed.
  leg_mode='native'
  [[ -n "${LEG_PREFIX[$leg]:-}" ]] && leg_mode='emulated'
  declare -a real=() confound=() scoped_excused=()
  for t in $faillist; do
    is_c=0; by_scope=0
    # In the `all` tier sqlite QUALIFIES every test name with its permutation
    # (`memsubsys1.walsetlk-2.2.6`), while veryquick/quick/full report the bare name
    # (`walsetlk-2.2.6`). Every confound pattern is ^-anchored, so against a
    # qualified name it matches NOTHING and a long-documented confound is reported
    # as a GENUINE failure. That is the mirror of a silent excusal — it indicts a
    # benign test loudly instead of excusing a real one quietly — and it is just as
    # wrong about where the compiler stands. Measured on the first Linux `all` run:
    # 30 of 37 distinct failures were prefixed (D-SQLITE-CONFOUND-PERMUTATION-PREFIX).
    # The prefix is stripped ONLY when it is a prefix sqlite ITSELF DECLARES, read by
    # tier_prefixes() from permutations.test's own `-prefix` values — never guessed
    # from the suite name, because it is NOT derivable from it: the default is
    # "<name>." (:39) but `mmap` declares "mm-" (:220) and quick/full/threads declare
    # "" (none at all). Guessing `^<ident>\.` would have silently missed every
    # `mm-…` name — the very ones the pe64 `all` run produces. TIER_PREFIXES is sorted
    # longest-first so a longer prefix wins over one that is its head.
    t_bare="$t"
    for _pfx in ${TIER_PREFIXES[@]+"${TIER_PREFIXES[@]}"}; do
      [[ "$t" == "$_pfx"* ]] && { t_bare="${t#"$_pfx"}"; break; }
    done
    for p in "${CONFOUND_PATTERNS[@]}"; do
      p_scope=''; p_rx="$p"
      case "$p" in
        native:*)   p_scope='native';   p_rx="${p#native:}"   ;;
        emulated:*) p_scope='emulated'; p_rx="${p#emulated:}" ;;
      esac
      # A scoped pattern is INERT on any other mode — it must not excuse there.
      [[ -n "$p_scope" && "$p_scope" != "$leg_mode" ]] && continue
      # Match the name as reported AND with its permutation qualifier removed: the
      # confound is a property of the TEST, and running it under a permutation does
      # not change which upstream behaviour it is sensitive to.
      [[ "$t" =~ $p_rx || "$t_bare" =~ $p_rx ]] && { is_c=1; [[ -n "$p_scope" ]] && by_scope=1; break; }
    done
    if [[ "$is_c" == 1 ]]; then
      confound+=("$t")
      # Named separately in the verdict: an excusal that depends on HOW the leg runs
      # is a coverage statement, not a clean pass. Silence about it would be a
      # harness bug.
      [[ "$by_scope" == 1 ]] && scoped_excused+=("$t")
    else
      real+=("$t")
    fi
  done
  if [[ ${#scoped_excused[@]} -gt 0 ]]; then
    warn "[$leg] ${#scoped_excused[@]} failure(s) excused ONLY because this leg runs '$leg_mode': ${scoped_excused[*]}"
    warn "      these are NOT evidence of correctness on a native run of this target — and a crash-simulation"
    warn "      abort can TRUNCATE the rest of its .test file, so coverage there is partial."
  fi
  # Per-unit ledger — every file that reached a verdict, every abort, every gap.
  { printf "sqlite unit ledger — leg '%s', tier '%s', %s segment(s), %s resume(s)\n" "$leg" "$DSS_TIER" "$nseg" "$resumes"
    printf 'union: %s\n' "$summary"
    [[ -z "$derivation" ]] || printf '   derived from: %s\n' "$derivation"
    for ((k = 0; k < nseg; k++)); do
      k_sum="$(fact S "$scratch/facts.$k")"
      printf '\n== segment: %s   rc=%s   %s\n   log: %s\n' \
        "${SEG_LABELS[$k]}" "${SEG_RCS[$k]}" "${k_sum:-ABORTED (no summary line)}" "${SEG_LOGS[$k]}"
      printf '   %s\n' "${SEG_COUNTS[$k]}"
      printf '   files completed (%s): %s\n' "$(fact N "$scratch/facts.$k")" "$(facts F "$scratch/facts.$k" | tr '\n' ' ')"
      k_fails="$(facts X "$scratch/facts.$k" | LC_ALL=C sort -u | tr '\n' ' ')"
      [[ -z "${k_fails// /}" ]] || printf '   failing test(s) seen here: %s\n' "$k_fails"
    done
    [[ ${#CALIBRATION[@]} -eq 0 ]] || { printf '\n== derivation calibration MISMATCH ==\n'; printf '   %s\n' "${CALIBRATION[@]}"; }
    [[ ${#ABORT_ROWS[@]} -eq 0 ]]  || { printf '\n== aborts ==\n'; printf '   %s\n' "${ABORT_ROWS[@]}"; }
    [[ ${#NOT_REACHED[@]} -eq 0 ]] || { printf '\n== NOT REACHED (no verdict) ==\n'; printf '   %s\n' "${NOT_REACHED[@]}"; }
    [[ ${#HYGIENE[@]} -eq 0 ]]     || { printf '\n== process hygiene ==\n'; printf '   %s\n' "${HYGIENE[@]}"; }
    [[ ${#EXCLUDE_PATTERNS[@]} -eq 0 ]] || { printf '\n== EXCLUDED by operator (DSS_TIER_EXCLUDES -> QUICKTEST_OMIT) ==\n   %s\n' "${EXCLUDE_PATTERNS[*]}"; }
  } > "$ledger"

  if [[ ${#ABORTS[@]} -gt 0 ]]; then
    # An abort is itself a FAILURE. Resuming recovers the units behind it; it never
    # makes the abort disappear, and a run with aborts is NEVER green.
    v="FAIL:${#ABORTS[@]} fixture ABORT(s) [${ABORTS[*]}]; recovered by $resumes resume(s); union: $union_summary"
    [[ -z "$derivation" ]] || v="$v [$derivation]"
    [[ ${#real[@]} -eq 0 ]] || v="$v; ${#real[@]} genuine unit failure(s): ${real[*]}"
    [[ ${#NOT_REACHED[@]} -eq 0 ]] || v="$v; ${#NOT_REACHED[@]} unit group(s) NOT REACHED — see $ledger"
    UNIT_VERDICT["$leg"]="$v"; UNIT_FAILS=$((UNIT_FAILS + 1))
    warn "[$leg] corpus FAIL — ${#ABORTS[@]} abort(s): ${ABORTS[*]}"
    info "      union across $nseg segment(s): $union_summary; $files_done test file(s) completed"
    [[ -z "$derivation" ]] || info "        derived from: $derivation"
    [[ ${#real[@]} -eq 0 ]] || info "      ${#real[@]} GENUINE DSS failure(s): ${real[*]}"
    for n in ${NOT_REACHED[@]+"${NOT_REACHED[@]}"}; do warn "      NOT REACHED: $n"; done
    info "      per-unit ledger: $ledger"
  elif [[ -z "$summary" ]]; then
    UNIT_VERDICT["$leg"]="FAIL:fixture did not complete the suite (crash?) — see $runlog"
    UNIT_FAILS=$((UNIT_FAILS + 1)); warn "[$leg] corpus FAIL — no summary line (fixture crashed mid-suite); tail:"
    tail -4 "$runlog" 2>/dev/null | sed 's/^/      /'
  elif [[ "$total_errors" -gt 0 && -z "${faillist// /}" ]]; then
    # conservative: errors reported but no classifiable failure list -> RED.
    UNIT_VERDICT["$leg"]="FAIL:$total_errors error(s) but no failure markers ('Failures on these tests:' / '! <name>') to classify — see $runlog"
    UNIT_FAILS=$((UNIT_FAILS + 1)); warn "[$leg] corpus FAIL — $summary (unclassifiable — no failure markers)"
  elif [[ ${#real[@]} -eq 0 ]]; then
    if [[ ${#confound[@]} -eq 0 ]]; then
      UNIT_VERDICT["$leg"]="PASS ($summary)"
      pass "[$leg] corpus GREEN — $summary"
    else
      UNIT_VERDICT["$leg"]="PASS ($summary; ${#confound[@]} known non-DSS confound(s): ${confound[*]})"
      pass "[$leg] corpus GREEN — $summary; all ${#confound[@]} failure(s) are known non-DSS confounds: ${confound[*]}"
    fi
  else
    UNIT_VERDICT["$leg"]="FAIL:${#real[@]} genuine unit failure(s): ${real[*]}"
    UNIT_FAILS=$((UNIT_FAILS + 1))
    warn "[$leg] corpus FAIL — $summary; ${#real[@]} GENUINE DSS failure(s): ${real[*]}"
    [[ ${#confound[@]} -gt 0 ]] && info "      (+${#confound[@]} known confound(s) ignored: ${confound[*]})"
  fi
  # A killed zombie / stolen stale lock is a fact about THIS run, not a footnote —
  # it rides on the verdict even when the corpus itself came back clean.
  if [[ ${#HYGIENE[@]} -gt 0 ]]; then
    UNIT_VERDICT["$leg"]="${UNIT_VERDICT[$leg]}  [PROCESS HYGIENE: ${#HYGIENE[@]} event(s) — ${HYGIENE[*]}]"
    for h in "${HYGIENE[@]}"; do warn "[$leg] HYGIENE: $h"; done
  fi
  # A NOT-REACHED unit is a coverage hole even when nothing failed — never silent.
  if [[ ${#NOT_REACHED[@]} -gt 0 && ${#ABORTS[@]} -eq 0 ]]; then
    UNIT_VERDICT["$leg"]="${UNIT_VERDICT[$leg]}  [NOT FULL COVERAGE: ${#NOT_REACHED[@]} unit group(s) NOT REACHED — see $ledger]"
    UNIT_FAILS=$((UNIT_FAILS + 1))
    for n in "${NOT_REACHED[@]}"; do warn "[$leg] NOT REACHED: $n"; done
  fi
  LEG_SEGMENTS["$leg"]="$nseg"; LEG_RESUMES["$leg"]="$resumes"
  LEG_FILESDONE["$leg"]="$files_done"; LEG_LEDGER["$leg"]="$ledger"
  LEG_ABORTS["$leg"]="${ABORTS[*]-}"; LEG_NOTREACHED["$leg"]="$(printf '%s\n' ${NOT_REACHED[@]+"${NOT_REACHED[@]}"})"
  LEG_HYGIENE["$leg"]="$(printf '%s\n' ${HYGIENE[@]+"${HYGIENE[@]}"})"
  # <<< dss:corpus-loop <<<
  unset real confound ABORTS ABORT_ROWS NOT_REACHED HYGIENE CALIBRATION SEG_LOGS SEG_LABELS SEG_RCS SEG_COUNTS TIER_PERMS TIER_PREFIXES
done

# ── Step 9 — results ─────────────────────────────────────────────────────────
step "9/9  Results"
printf '   compiler : %s @ %s\n' "$DSS_BIN" "$(git -C "$SRC_DIR" rev-parse --short HEAD)"
printf '   sqlite   : %s @ %s\n' "$SQLITE_DIR" "$(git -C "$SQLITE_DIR" rev-parse --short HEAD)"
printf '   recipe   : %s TUs, %s defines (%s)\n' "${#TUS[@]}" "${#RECIPE_DEFS[@]}" "$RECIPE"
# The ATTRIBUTION ORACLE, surfaced where a human triaging a failure will see it.
# Its ABSENCE is printed too, and loudly: a missing oracle is the difference
# between attributing a corpus failure and arguing about it (it is what stalled
# walsetlk_recover), so it must never be silent. Step 4 preserves this copy out of
# the make target's path — see "the PRESERVED oracle" there.
# `-f` as well as `-x`: a DIRECTORY passes `-x`, so `-x` alone could report an
# "oracle" that is not a runnable file. The test asserts what is actually claimed.
if [[ -n "${REF_FIXTURE:-}" && -f "${REF_FIXTURE:-}" && -x "${REF_FIXTURE:-}" ]]; then
  printf '   oracle   : %s\n' "$REF_FIXTURE"
  printf '              reference cc testfixture — run it on the same .test to ATTRIBUTE a failure\n'
else
  printf '   oracle   : %sABSENT%s — no reference fixture survived this run, so a corpus failure\n' "$C_YLW" "$C_RST"
  printf '              CANNOT be attributed to DSS vs upstream. Log: %s\n' "${REF_BUILD_LOG:-$BLD/reference-build.log}"
fi
printf '   tier     : %s.test   outputs: %s\n' "$DSS_TIER" "$OUT_DIR"
if [[ ${#EXCLUDE_PATTERNS[@]} -gt 0 ]]; then
  printf '   excluded : %s   (operator DSS_TIER_EXCLUDES -> QUICKTEST_OMIT; dropped from every $allquicktests-derived permutation, still run under '\''full'\'')\n' "${EXCLUDE_PATTERNS[*]}"
else
  printf '   excluded : (none — the full tier ran)\n'
fi
for leg in "${LEG_ORDER[@]}"; do
  spec="${LEG_SPEC[$leg]}"
  if [[ "${COMPILE_OK[$leg]:-0}" == "1" ]]; then
    # Only printed when there is something to say: a clean single-segment run
    # leaves this block byte-identical to what it always was.
    if [[ "${LEG_SEGMENTS[$leg]:-1}" -gt 1 || -n "${LEG_ABORTS[$leg]:-}" || -n "${LEG_NOTREACHED[$leg]:-}" || -n "${LEG_HYGIENE[$leg]:-}" ]]; then
      printf '   %-6s segments : %s (%s resume(s) of max %s)   %s test file(s) completed   ledger: %s\n' \
        "$leg" "${LEG_SEGMENTS[$leg]}" "${LEG_RESUMES[$leg]}" "$DSS_MAX_RESUMES" "${LEG_FILESDONE[$leg]}" "${LEG_LEDGER[$leg]}"
      for a in ${LEG_ABORTS[$leg]:-}; do
        printf '   %-6s aborted  : %s — its remaining cases did NOT run\n' "$leg" "$a"
      done
      while IFS= read -r n; do [[ -z "$n" ]] || printf '   %-6s NOT RUN  : %s\n' "$leg" "$n"; done <<< "${LEG_NOTREACHED[$leg]:-}"
      while IFS= read -r h; do [[ -z "$h" ]] || printf '   %-6s hygiene  : %s\n' "$leg" "$h"; done <<< "${LEG_HYGIENE[$leg]:-}"
    fi
    # $EXCL_NOTE rides along on EVERY verdict — pass and fail alike — so a GREEN
    # line can never be read as "the whole corpus ran".
    printf '   %-6s (%s): %scompiled%s   units: %s%s\n' "$leg" "$spec" "$C_GRN" "$C_RST" "${UNIT_VERDICT[$leg]:--}" "$EXCL_NOTE"
  else
    printf '   %-6s (%s): %sCOMPILE FAILED%s   see %s/%s/compile.log\n' "$leg" "$spec" "$C_RED" "$C_RST" "$OUT_DIR" "$leg"
  fi
done
# Release the run lock. Correctness does NOT depend on this — the lock is
# liveness-based, so a run that dies here just leaves one the next invocation steals
# and reports. Releasing simply keeps that report quiet when it should be.
rm -rf "$LOCK_DIR"
if [[ "$COMPILE_FAILS" -gt 0 ]]; then
  printf '\n%s%d leg(s) failed to compile the testfixture — inspect the compile.log diagnostics.%s\n' "$C_RED" "$COMPILE_FAILS" "$C_RST"
  exit 1
fi
if [[ "$UNIT_FAILS" -gt 0 ]]; then
  printf '\n%s%d leg(s) had genuine unit failures (non-confound) — the corpus is not green.%s\n' "$C_RED" "$UNIT_FAILS" "$C_RST"
  exit 1
fi
pass "every leg compiled the full-source testfixture + ran the $DSS_TIER unit corpus GREEN — SQLite units pass with dss-code-prime"
