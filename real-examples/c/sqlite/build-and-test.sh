#!/usr/bin/env bash
#
# real-examples/c/sqlite/build-and-test.sh
# ─────────────────────────────────────────────────────────────────────────────
# SQLite UNIT-CORPUS harness for DSS Code Prime — full-source, no amalgamation.
#
# Clone the repo and run ONE command to prove DSS Code Prime builds SQLite from
# its REAL sources (no amalgamation) into the Tcl `testfixture` and runs SQLite's
# own `.test` unit corpus GREEN — for EVERY DECLARED TARGET, on whatever host you
# happen to be standing on.
#
# ★★ TARGET-KEYED, NEVER HOST-KEYED (D-HARNESS-CROSS-HOST-ANY-TARGET item 2; user
# HARD REQUIREMENT 2026-07-25: "build ANY target inside ANY host — this MUST work").
# This driver used to derive its leg list from `uname`: the "host" leg was whatever
# the machine was, and an arm64 leg appeared ONLY when the host happened to be
# Linux/x86_64 — so pe64 and mach-o were unreachable from it on every host. That
# conflated HOST with TARGET, which is the inverse of the requirement.
#   · WHICH LEGS EXIST is now declared, host-free, in `legs.json` and resolved by
#     `harness_legs.py` — the SAME five legs on Linux, macOS, an arm64 VPS or
#     Windows/WSL. BUILDING is attempted for every one of them, unconditionally.
#   · THE ONLY LEGITIMATE HOST QUESTION is "can this host EXECUTE this artifact",
#     and it is answered by the catalogue's own `runOn` + `launchers` (a launcher
#     is qemu for a cross-ARCH host, Wine for a cross-OS one), never by a
#     `uname`/`$HOST_OS` branch in leg selection.
#   · EVERY declared leg reaches a NAMED verdict from the closed vocabulary in
#     tests/test_support/arm_verdict_ledger.hpp — Step 9 prints the ledger line.
# The remaining `$HOST_OS` tests in this file are all about the BUILD HOST'S OWN
# toolchain/filesystem (brew vs apt, the Xcode SDK, macOS's per-inode exec deny);
# each one says so at its site. None of them decides which target gets built.
#
# Windows companion: build-and-test.ps1 (its own de-host-locking is a separate item).
#
# The pipeline, end-to-end:
#
#   1. IDENTIFY the host (Linux / WSL / macOS, online), then RESOLVE the declared
#      leg set from legs.json through harness_legs.py — one host-independent
#      resolver shared with build-and-test.ps1
#   2. use the dss-code-prime checkout at ~/src AS-IS on its CURRENT branch —
#      NEVER switched or pulled (a probe tests the working tree exactly as it is).
#      An ABSENT (or non-checkout) dir is a REFUSAL, never a silent clone of the
#      default branch: DSS_ALLOW_FRESH_CLONE=1 opts in and DSS_BRANCH says which
#      branch. DSS_BRANCH/DSS_COMMIT, when set, are VERIFIED against the checkout,
#      and every provenance line carries how far the working tree diverges from the
#      HEAD it names (D-HARNESS-SH-SRC-DIR-GIT-REQUIRED-VS-RSYNC-GATE)
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
#      LIBS-OS-ONLY; portable C, shared by EVERY leg) and resolve EACH LEG'S OWN
#      (tcl, z) library pair from the provider its catalogue entry declares
#      (`host-system` | `ubuntu-ports-arm64` | `search-paths`). A leg whose pair
#      cannot be resolved on this machine records `skipped-build-input-missing`
#      NAMING what was searched — the run continues, and the other legs are
#      unaffected.
#   7. build the full-source `testfixture` with dss-code-prime, once PER LEG,
#      from a generated `.dss-project.json` manifest (dss --project mode). Every
#      declared leg is attempted, on every host. Each leg's manifest declares
#      c-subset / cli / the leg's <targetName>:<formatName> target / the ~185 TUs
#      (absolute `sources`) / the sqlite+tcl+zlib include dirs / the recipe defines
#      (transformed per the leg's declared `recipeTransform`) / the leg's own
#      resolveLibraries / its declared `stackReserve`; the build routes the binary
#      to <out>/<leg>/<formatName>/testfixture. ONE manifest generator
#      (gen-pe64-manifest.py) serves this driver and the .ps1.
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
#   9. summarise — including a LEDGER LINE that names every verdict class and
#      counts every DECLARED leg — and exit non-zero if any leg has a GENUINE
#      (non-confound) unit failure, a compile miss, a fixture abort, or a unit that
#      never ran. Structural skips are reported, never fatal; ENVIRONMENTAL skips
#      warn by default and become FATAL under DSS_STRICT_ARM_VERDICTS=1.
#
# DESIGN: every step is idempotent and FAIL-LOUD. dss-code-prime exits 0 even on
# fatal compile errors, so step 7 reads success from the DIAGNOSTICS (no `error[`
# line) + the emitted binary, never `$?` (probe a6b65f8b).
#
# Overridable via env: DSS_REPO_URL SQLITE_REPO_URL SRC_DIR SQLITE_DIR OUT_DIR
#                      JOBS  DSS_TIER  DSS_LEGS  DSS_CONFOUNDS  DSS_TIER_EXCLUDES
#                      DSS_MAX_RESUMES  DSS_SEGMENT_STALL  DSS_SEGMENT_TIMEOUT
#                      ARM64_LIBDIR  DSS_TCL_VERSION  DSS_STRICT_ARM_VERDICTS
#                      DSS_BRANCH  DSS_COMMIT  DSS_ALLOW_FRESH_CLONE
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
# ── WHICH dss-code-prime IS UNDER TEST — declared, not inferred ───────────────
# D-HARNESS-SH-SRC-DIR-GIT-REQUIRED-VS-RSYNC-GATE. Step 2 used to decide this from
# ONE filesystem fact — "does $SRC_DIR/.git exist?" — and a corpus run is hours
# long, so an answer that is merely PLAUSIBLE is worse here than almost anywhere
# else in the project: the result gets quoted later as evidence about a commit.
#
# DSS_BRANCH  — the branch you INTEND to test. Unset (default) = whatever the
#               checkout is on, exactly as before. Set, it is ASSERTED against the
#               checkout (die on mismatch) and is also the branch a fresh clone
#               checks out — clone_or_update has always taken a wanted-branch third
#               argument; Step 2 simply never passed one.
# DSS_COMMIT  — the commit you INTEND to test (full or abbreviated). Unset = no
#               assertion. Set, it must RESOLVE in $SRC_DIR *and* equal HEAD.
#               ⚠ It pins HEAD, not the working tree — an uncommitted edit still
#               satisfies it, which is why the divergence count below is reported
#               alongside rather than folded into this check.
# DSS_ALLOW_FRESH_CLONE — 1 permits Step 2 to CLONE when $SRC_DIR is absent.
#               Default 0 = refuse. A silent clone lands on the repo's DEFAULT
#               branch (main), so on a clean machine the harness would spend hours
#               validating a compiler nobody asked for and then print that commit
#               as the verdict's provenance.
DSS_BRANCH="${DSS_BRANCH:-}"
DSS_COMMIT="${DSS_COMMIT:-}"
DSS_ALLOW_FRESH_CLONE="${DSS_ALLOW_FRESH_CLONE:-0}"
# The checkout THIS SCRIPT lives in, offered as the suggestion when SRC_DIR points
# nowhere — the newcomer who clones the repo and runs the script is shape (a)'s
# most likely victim, and "$SRC_DIR does not exist" is only half an answer.
# ⚠ Deliberately NOT the default for SRC_DIR: that has always been
# $HOME/src/dss-code-prime, and silently changing WHICH TREE gets built (on this
# workstation, the WSL gate tree vs a /mnt/c one) would be the same class of
# surprise this gate exists to remove. Empty unless it really is a checkout, so a
# script copied out of the repo suggests nothing rather than something wrong.
SELF_REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." 2>/dev/null && pwd)" || SELF_REPO=""
[[ -n "$SELF_REPO" && -e "$SELF_REPO/.git" ]] || SELF_REPO=""
# The directory THIS driver ships in — home of its three siblings: the leg
# catalogue (legs.json), the leg resolver (harness_legs.py) and the driver
# self-test (test-confound-scope.sh). Resolved ONCE so every consumer below reads
# the same copy that was shipped beside this script.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LEG_CATALOGUE="$SCRIPT_DIR/legs.json"
LEG_RESOLVER="$SCRIPT_DIR/harness_legs.py"
MANIFEST_GEN="$SCRIPT_DIR/gen-pe64-manifest.py"
SRC_COHERENCE="$SCRIPT_DIR/check-source-coherence.sh"
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
# DSS_STRICT_ARM_VERDICTS: promote every ENVIRONMENTAL skip
# (`skipped-emulator-missing`, `skipped-build-input-missing`) from a loud warning
# to a HARD FAILURE. Same variable, same accepted spellings and the same
# malformed-value discipline as the two examples-corpus runners — see
# `readStrictArmVerdicts` in tests/test_support/arm_verdict_ledger.hpp: a stale
# `=0` must never quietly disable a gate, and a typo'd `=ture` must never quietly
# disable it either, so an unrecognised value is refused rather than read as off.
# Default (unset) = warn: a developer without qemu/Wine/a Darwin libtcl must still
# get a usable run out of this harness; a GATE opts in.
# Parsed and VALIDATED at Step 1 (cheap) rather than at Step 9 (hours later) —
# an unreadable gate setting must stop the run before it costs anything.
DSS_STRICT_ARM_VERDICTS="${DSS_STRICT_ARM_VERDICTS:-}"

# ── host identification (OS + arch) ──────────────────────────────────────────
# ★ IDENTITY ONLY. These two variables answer "what machine am I standing on",
# which is a legitimate question — the harness has to know whether to call apt or
# brew, and where this OS keeps its headers. They must NEVER decide which TARGET
# gets built: that comes from legs.json (see the banner). Every surviving use of
# $HOST_OS below is annotated with which host fact it is about.
#
# The spellings are the CORPUS's, not this script's invention: `linux` / `darwin` /
# `windows` are what examples/**/expected.json uses for `runOn`, what
# `currentHostOs()` returns in tests/test_support/arm_verdict_ledger.hpp, and what
# harness_legs.py's OS_ALIASES canonicalise to. This driver said `macos` for years,
# which meant the one string that had to match across the resolver boundary was the
# one string spelled differently on each side.
# WSL reports `Linux` from `uname -s` and IS linux for every purpose here (same
# toolchain, same ELF targets, same filesystem semantics) — it needs no arm of its
# own; Step 1 merely NAMES it in the banner so a reader knows which box this was.
HOST_OS=""
case "$(uname -s)" in
  Linux)  HOST_OS="linux"  ;;   # WSL lands here too, and that is correct
  Darwin) HOST_OS="darwin" ;;
esac
HOST_ARCH=""
case "$(uname -m)" in
  arm64|aarch64) HOST_ARCH="arm64"  ;;
  x86_64|amd64)  HOST_ARCH="x86_64" ;;
esac
# ── LEGS ─────────────────────────────────────────────────────────────────────
# ★ THE LEG SET IS NOT DECIDED HERE, AND NOT BY THIS MACHINE.
# It is DECLARED, host-free, in `legs.json` and resolved by `harness_legs.py`
# (D-HARNESS-CROSS-HOST-ANY-TARGET item 2). All this file does is `declare -A` the
# arrays the resolver's `--format sh` emitter fills and `eval` its output — see
# Step 1, which is where the plan is taken, once the host has been IDENTIFIED.
#
# WHAT WAS HERE BEFORE, so a future reader does not "restore" it: a `host` leg
# built from `host_target_spec()` (a uname→spec table) plus an `arm64` leg added
# only `if [[ "$HOST_OS" == linux && "$HOST_ARCH" == x86_64 ]]`. Under that shape
# pe64 and mach-o could not be reached from this driver at all, and the arm64 leg
# vanished on an arm64 host — the leg set WAS the machine. The hardcoded specs are
# gone with it: tests/harness/test_sqlite_harness_legs.cpp fails this driver for
# naming any target spec the catalogue does not declare.
#
# Per-leg arrays, keyed by leg LABEL. The first block is filled verbatim by the
# resolver (its names are a contract — see `emit_sh` in harness_legs.py); the
# second is filled by this driver as the run proceeds.
declare -A LEG_SPEC=() LEG_FORMAT=() LEG_ARCH=() \
           LEG_RUN_MODE=() LEG_RUN_VERDICT=() LEG_RUN_DETAIL=() \
           LEG_LAUNCH=() LEG_LAUNCH_ENV=() \
           LEG_PATH_TRANSLATION=() LEG_PATH_TRANSLATOR=() LEG_ENV_TRANSFER=() \
           LEG_RECIPE_TRANSFORM=() LEG_HEADER_STAGE_KEY=() LEG_ZCONF_GUARDS=() \
           LEG_STACK_RESERVE=() LEG_SHARED_FLAGS=() \
           LEG_CC_CANDIDATES=() LEG_CC_PKG=() \
           LEG_LIB_PROVIDER=() LEG_LIB_TCL_NAMES=() LEG_LIB_Z_NAMES=() LEG_LIB_PATHS=()
# Resolved by this driver: the leg's chosen target compiler, its (tcl, z) pair, and
# its VERDICT — one name from the closed vocabulary in
# tests/test_support/arm_verdict_ledger.hpp, with a reason. Empty verdict = "still
# in flight"; Step 9 refuses to let any declared leg end that way.
declare -A LEG_CC=() LEG_TCL_LIB=() LEG_Z_LIB=() LEG_VERDICT=() LEG_VERDICT_DETAIL=()
# LEG_DECLARED = every leg the catalogue declares (the ledger's denominator).
# LEG_ORDER    = the subset this invocation actually processes (DSS_LEGS filter).
declare -a LEG_ORDER=() LEG_DECLARED=()

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
  # HOST fact, not a target fact: which PACKAGE MANAGER this machine has.
  local apt_pkg="$1" brew_pkg="${2:-$1}"
  if [[ "$HOST_OS" == "darwin" ]]; then
    command -v brew >/dev/null 2>&1 || die "Homebrew not found — install from https://brew.sh, then re-run (needed for: $brew_pkg)."
    info "installing (brew): $brew_pkg"
    brew list "$brew_pkg" >/dev/null 2>&1 || brew install "$brew_pkg"
  else
    # ★ The host identity rides along: this helper can be reached at Step 0, i.e.
    # BEFORE Step 1's "unsupported host OS" die, so on an unidentified host the
    # apt branch is where the operator first learns anything. Saying WHICH host it
    # could not identify turns a confusing "no apt-get" into the real answer.
    command -v apt-get >/dev/null 2>&1 || die "apt-get not found — this harness targets Debian/Ubuntu/WSL + macOS
      (host OS identified as: ${HOST_OS:-UNIDENTIFIED — uname -s = '$(uname -s)'}). Install manually: $apt_pkg"
    if [[ "$APT_UPDATED" -eq 0 ]]; then $SUDO apt-get update -y >/dev/null; APT_UPDATED=1; fi
    info "installing (apt): $apt_pkg"
    $SUDO DEBIAN_FRONTEND=noninteractive apt-get install -y "$apt_pkg" >/dev/null
  fi
}
ensure_cmd() {                  # ensure_cmd <command> <apt-pkg> [<brew-pkg>]
  command -v "$1" >/dev/null 2>&1 || pkg_install "$2" "${3:-$2}"
}

# ── checkout provenance — REPORTED, never inferred ───────────────────────────
# >>> dss:src-provenance >>>
# WHY THIS EXISTS — D-HARNESS-SH-SRC-DIR-GIT-REQUIRED-VS-RSYNC-GATE (and its
# measured sibling D-HARNESS-WSL-TREE-PROVENANCE-UNVERIFIABLE).
# A verdict line that names a commit is a CITATION: later cycles quote it as "the
# compiler that ran the corpus". This driver emitted two kinds of false citation.
#   · It printed `git rev-parse HEAD` for $SRC_DIR unconditionally. The WSL ctest
#     gate rsyncs the working tree with `--exclude=/.git` (deliberately — the rsync
#     exists to gate UNCOMMITTED work that no commit can represent), so a STALE
#     .git survives beside CURRENT sources and HEAD names a commit that was never
#     built. MEASURED: a run reported 5093341d while the tree was fb555bc0 + two
#     days of newer work. The RESULT was valid; only the LABEL lied.
#   · Those rev-parses sat inside `printf`/`info` ARGUMENTS, where a failing
#     command substitution does NOT trip `set -e`. The field simply came out EMPTY,
#     which a human skims straight past as "fine".
# ★ A stale-.git tree and a legitimate uncommitted-work probe are INDISTINGUISHABLE
# from inside the tree — both are "sources that differ from HEAD". So the honest fix
# is to REPORT the divergence and let the reader judge. Guessing which one it is
# would only trade a confident wrong label for a confident wrong story, and hard-
# failing on a dirty tree would break the pre-commit probe this harness exists for.
#
# Every function here returns a NON-EMPTY, self-describing string on every path,
# so it is safe to call from inside `printf` arguments.
git_head_short() {              # git_head_short <dir> -> short sha | UNKNOWN(<why>)
  local d="$1" sha=""
  # `-e`: a worktree/submodule checkout has .git as a FILE (see clone_or_update).
  [[ -e "$d/.git" ]] || { printf 'UNKNOWN(no .git under %s)' "$d"; return 0; }
  sha="$(git -C "$d" rev-parse --short HEAD 2>/dev/null)" || sha=""
  [[ -n "$sha" ]] || { printf 'UNKNOWN(rev-parse HEAD failed in %s)' "$d"; return 0; }
  printf '%s' "$sha"
}
git_head_branch() {             # git_head_branch <dir> -> branch | DETACHED-HEAD | UNKNOWN
  local b=""
  b="$(git -C "$1" rev-parse --abbrev-ref HEAD 2>/dev/null)" || b=""
  [[ -n "$b" ]] || { printf 'UNKNOWN'; return 0; }
  # A detached HEAD prints the literal string "HEAD" here, which reads like a branch
  # named HEAD and would make a DSS_BRANCH mismatch message nonsense.
  [[ "$b" != "HEAD" ]] || { printf 'DETACHED-HEAD'; return 0; }
  printf '%s' "$b"
}
git_tree_divergence() {         # git_tree_divergence <dir> -> N | "" when uncomputable
  local out=""
  # `--no-optional-locks` because this is a READ of a checkout the harness promises
  # not to touch ("current checkout, untouched") and a plain `status` refreshes and
  # REWRITES .git/index. Git < 2.14 rejects the flag, so fall back rather than lose
  # the count on an old host. ★ rc is captured DIRECTLY off git, never through a
  # pipe — a `| wc -l` would report wc's success as git's.
  out="$(git -C "$1" --no-optional-locks status --porcelain 2>/dev/null)" \
    || out="$(git -C "$1" status --porcelain 2>/dev/null)" \
    || { printf ''; return 0; }
  # `status --porcelain` honours .gitignore, so this harness's own output tree
  # ($SRC_DIR/build/…, and the repo ignores build/ + build-*/) cannot inflate the
  # count. It DOES list untracked files, which is exactly right: a source file a
  # stale HEAD never knew about is divergence, and it is precisely what the rsync
  # gate leaves behind. "" (uncomputable) is kept DISTINCT from "0" (clean) —
  # collapsing them would report an unmeasurable tree as pristine.
  [[ -n "$out" ]] || { printf '0'; return 0; }
  local -a lines=(); mapfile -t lines <<< "$out"
  printf '%d' "${#lines[@]}"
}
dir_has_entries() {             # dir_has_entries <dir> -> true if it holds ANY entry
  # Shape (c) of the anchor: a populated directory that is NOT a checkout — exactly
  # what `rsync --exclude=/.git` produces. `git clone` refuses such a target, so
  # without this the situation surfaced as a git error from inside a helper.
  # Dotfiles count: an rsync'd tree can plausibly hold only hidden entries, and
  # "empty except for .github" is still not clonable-into.
  local e
  for e in "$1"/* "$1"/.[!.]* "$1"/..?*; do
    if [[ -e "$e" || -L "$e" ]]; then return 0; fi
  done
  return 1
}
# The provenance of $SRC_DIR, read ONCE (Step 2) and reused verbatim by the Step-9
# summary — so the opening banner and the closing verdict cannot drift apart, and
# the `status` walk (which re-hashes every file whose mtime the rsync changed) is
# paid once, up front, instead of at the end of a multi-hour run.
SRC_HEAD=""; SRC_HEAD_LONG=""; SRC_BRANCH=""; SRC_DIVERGE=""; SRC_DIVERGE_NOTE=""
read_src_provenance() {         # read_src_provenance <dir>
  SRC_HEAD="$(git_head_short "$1")"
  SRC_HEAD_LONG="$(git -C "$1" rev-parse HEAD 2>/dev/null)" || SRC_HEAD_LONG=""
  SRC_BRANCH="$(git_head_branch "$1")"
  SRC_DIVERGE="$(git_tree_divergence "$1")"
  if [[ -z "$SRC_DIVERGE" ]]; then
    SRC_DIVERGE_NOTE=" (divergence from HEAD UNVERIFIED — git status failed in $1)"
  elif [[ "$SRC_DIVERGE" != "0" ]]; then
    SRC_DIVERGE_NOTE=" (+$SRC_DIVERGE file(s) differ from HEAD — the sources built are NOT exactly this commit)"
  else
    SRC_DIVERGE_NOTE=""
  fi
}
# <<< dss:src-provenance <<<

# >>> dss:src-clone >>>  (extracted by test-confound-scope.sh together with the
# provenance block above and the Step-2 gate below — the three are one decision)
default_branch() {              # origin/HEAD (sqlite's may be master/trunk, not main)
  local r=""
  r="$(git -C "$1" symbolic-ref --short refs/remotes/origin/HEAD 2>/dev/null)" || true
  printf '%s' "${r#origin/}"
}
clone_or_update() {             # clone_or_update <url> <dir> <wanted-branch-or-empty>
  local url="$1" dir="$2" want="${3:-}"
  # `-e`, not `-d`: in a `git worktree` (and in a submodule) `.git` is a FILE that
  # points at the real gitdir. Under `-d` such a checkout looked like "not a repo",
  # so this helper tried to CLONE over a populated directory — git refuses, and the
  # ERR trap then reported a git error instead of the situation. `-d` implies `-e`,
  # so the only behaviour that changes is the one that used to fail.
  if [[ -e "$dir/.git" ]]; then
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
  # Through the guarded helpers: a bare `$(git rev-parse …)` inside an argument list
  # prints an EMPTY field when it fails, and `set -e` never sees it.
  info "  at $(git_head_short "$dir") on $(git_head_branch "$dir")"
}
# <<< dss:src-clone <<<

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
# drift, and it is already demonstrated red-on-disable. It also covers the Step-2
# source gate + provenance helpers, which it exercises against throwaway git repos
# (D-HARNESS-SH-SRC-DIR-GIT-REQUIRED-VS-RSYNC-GATE). Cost: MEASURED 0.20 s for 46
# assertions on Linux (WSL, bash 5.2.21, git 2.43) — up from 22 ms / 20 assertions
# before the gate was covered, and still nothing against a run measured in hours.
#
# This matters most on a NEW HOST (macOS, a fresh Linux box), where the first run
# is the one you least want to lose. A portability defect in the late-stage code
# surfaces here, in seconds, instead of after a multi-hour corpus.
# Set DSS_SKIP_SELFTEST=1 to bypass (not recommended; say why in the log).
_selftest="$SCRIPT_DIR/test-confound-scope.sh"
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
    # ★ The SKIP COUNT is part of the result, not a footnote. The self-test runs at
    # Step 0 and `ensure_cmd git` is Step 1, so on a git-less host its two git-gated
    # blocks (26 of 46 assertions — the provenance helpers and the whole Step-2
    # source gate) SKIP. The old form printed only `passed=`, so that host read
    # "driver self-test: OK (20 assertions)" — the exact number that at HEAD meant
    # the ENTIRE battery — over 43% of it. A partial run must never READ as a full
    # one, so a nonzero skip count is a WARN naming what went unproven.
    # BSD-portable sed: one `-e` per expression, no `;`-chained scripts.
    _st_pass="$(printf '%s\n' "$_st_out" | sed -n -e 's/^passed=\([0-9][0-9]*\) .*/\1/p')"
    _st_skip="$(printf '%s\n' "$_st_out" | sed -n -e 's/^passed=[0-9][0-9]* failed=[0-9][0-9]* skipped=\([0-9][0-9]*\)$/\1/p')"
    if [[ -z "$_st_pass" || -z "$_st_skip" ]]; then
      printf '%s\n' "$_st_out" | sed 's/^/      /' >&2
      die "driver self-test exited 0 but printed no readable summary line.
      Expected a final 'passed=N failed=N skipped=N'; parsed passed=[${_st_pass:-<none>}] skipped=[${_st_skip:-<none>}].
      Its assertions may well have passed — but a self-test whose RESULT cannot be read
      proves nothing, and this guard will not report OK over an unparseable one. Either
      test-confound-scope.sh's summary format changed (update this parse with it) or it
      died before printing it."
    fi
    if [[ "$_st_skip" == "0" ]]; then
      info "driver self-test: OK ($_st_pass assertions, 0 skipped)"
    else
      warn "driver self-test: OK ($_st_pass assertions) — but $_st_skip assertion(s) SKIPPED on this host
      (an unmet prerequisite, normally 'no git on PATH' at Step 0, before Step 1 installs it).
      That part of the late-stage logic is UNPROVEN for this run — re-run the self-test by hand
      once git is present if you want the full battery: $_selftest"
    fi
  else
    printf '%s\n' "$_st_out" | sed 's/^/      /' >&2
    die "DRIVER SELF-TEST FAILED — refusing to start.
      The end-of-run classifier is broken, so this run would execute the whole corpus
      (hours) and then abort while classifying. Fix the driver first; the output above
      names the failing assertion."
  fi
fi

# ── Step 0b — SELF-TEST the LEG PLAN (same refuse-to-start discipline) ───────
# The leg plan decides WHICH TARGETS this run is about. A defect there does not
# announce itself: it produces a SHORTER leg list, and a shorter leg list looks
# exactly like a successful run of fewer things. That is the whole failure this
# cycle exists to end, so the resolver's own battery — host-invariance of the build
# set across nine simulated hosts, the closed verdict vocabulary, the run oracle,
# and the assertions-only shape of the `sh` emitter this driver is about to `eval`
# — runs HERE, in well under a second, beside the classifier self-test.
# (The gate also runs it: tests/harness/test_sqlite_harness_legs.cpp. That covers
# the repo; this covers the machine the operator is actually on, which may be a
# host the gate never sees.)
# Same DSS_SKIP_SELFTEST=1 escape hatch, same "a self-test whose result cannot be
# READ proves nothing" rule as above.
if [[ "${DSS_SKIP_SELFTEST:-0}" == "1" ]]; then
  warn "leg-plan self-test SKIPPED (DSS_SKIP_SELFTEST=1) — a broken leg plan will not surface until a leg silently fails to appear."
else
  [[ -f "$LEG_RESOLVER"  ]] || die "leg resolver missing: $LEG_RESOLVER
      This file, with legs.json beside it, IS the answer to 'which targets does this
      harness build?'. Without it the driver has nothing to read and the only thing
      left to key on is the host — the exact defect D-HARNESS-CROSS-HOST-ANY-TARGET
      closed. Restore it; there is no fallback, by design."
  [[ -f "$LEG_CATALOGUE" ]] || die "leg catalogue missing: $LEG_CATALOGUE (see $LEG_RESOLVER)."
  [[ -f "$MANIFEST_GEN"  ]] || die "manifest generator missing: $MANIFEST_GEN
      Both drivers emit their .dss-project.json through this ONE file (Step 7). It is
      checked here rather than at Step 7 so a missing generator costs a second, not the
      whole recipe derivation and compiler build that precede it."
  # python3 is needed HERE now — far earlier than the old `ensure_cmd python3` at
  # Step 7 — because both the leg plan and every manifest are python. It is a hard
  # dependency of this harness either way (build-and-test.ps1 dies without it too),
  # so ensuring it at Step 0 only moves the cost forward, never adds one.
  ensure_cmd python3 python3
  if _lt_out="$(python3 "$LEG_RESOLVER" --catalogue "$LEG_CATALOGUE" --self-test 2>&1)"; then
    _lt_pass="$(printf '%s\n' "$_lt_out" | sed -n -e 's/^passed=\([0-9][0-9]*\) failed=0$/\1/p')"
    if [[ -z "$_lt_pass" ]]; then
      printf '%s\n' "$_lt_out" | sed 's/^/      /' >&2
      die "leg-plan self-test exited 0 but printed no readable 'passed=N failed=0' line.
      A self-test whose RESULT cannot be read proves nothing, and this guard will not
      report OK over an unparseable one."
    fi
    info "leg-plan self-test: OK ($_lt_pass assertions)"
  else
    printf '%s\n' "$_lt_out" | sed 's/^/      /' >&2
    die "LEG-PLAN SELF-TEST FAILED — refusing to start.
      The leg resolver or the catalogue it reads is broken, so this run would build a
      leg set nobody declared — and a MISSING leg is invisible in a green summary.
      Fix $LEG_RESOLVER / $LEG_CATALOGUE first; the output above names the assertion."
  fi
  # The catalogue's own defects are host-independent and cheap to find, so they are
  # found here rather than by whichever host happens to trip over one.
  if _ll_out="$(python3 "$LEG_RESOLVER" --catalogue "$LEG_CATALOGUE" --lint 2>&1)"; then
    info "leg catalogue: lints clean ($LEG_CATALOGUE)"
  else
    printf '%s\n' "$_ll_out" | sed 's/^/      /' >&2
    die "THE LEG CATALOGUE DOES NOT LINT — refusing to start. See $LEG_CATALOGUE."
  fi
fi
# <<< dss:selftest <<<

# ── Step 1 — IDENTIFY the host, then RESOLVE the declared legs ───────────────
step "1/9  Host identification + leg plan (from legs.json), online"
# ★ HOST IDENTIFICATION FAILING IS GENUINELY FATAL, and it is the ONE place a
# `uname` answer may stop the run: everything below — which package manager, where
# the headers are, whether the fresh-inode install applies — is a statement about
# THIS MACHINE, and a machine the driver cannot name is one it cannot make those
# statements about. This is NOT the old "no leg for this host": the leg set is
# host-free now and identical everywhere (see the LEGS block above).
[[ -n "$HOST_OS"   ]] || die "unsupported host OS — uname -s = '$(uname -s)' (need Linux/WSL or macOS/Darwin).
      This is HOST IDENTIFICATION failing, not a target restriction: the five legs in
      $LEG_CATALOGUE are the same on every host. What this driver cannot do on an
      unrecognised OS is drive its package manager or find its headers.
      A Windows host runs the pe64 leg through build-and-test.ps1 (or runs THIS script
      inside WSL, which reports Linux and is treated as linux)."
[[ -n "$HOST_ARCH" ]] || die "unsupported host arch — uname -m = '$(uname -m)' (need arm64/aarch64 or x86_64)."
# The BUILD HOST's own flavour, for the banner only — WSL is linux for every
# decision this driver makes, and is named merely so a log says which box it was.
if [[ "$HOST_OS" == "darwin" ]]; then
  info "host: macOS ($HOST_ARCH, $(uname -r))"
elif grep -qiE 'microsoft|wsl' /proc/version 2>/dev/null; then
  info "host: WSL ($HOST_ARCH, $(uname -r))   [linux, for every purpose here]"
else
  info "host: native Linux ($HOST_ARCH, $(uname -r))"
fi

# ── THE LAUNCHER'S PATH NAMESPACE ────────────────────────────────────────────
# D-HARNESS-NO-WSL-LAUNCHER-FOR-ELF-ON-WINDOWS. Twin of Convert-LaunchPath /
# Assert-LaunchArgsTranslated in build-and-test.ps1 — capability-paired on
# purpose, because a translation that exists in one driver and not the other is
# the silent harness bug this project keeps re-paying for.
#
# A launcher need not address files the way the driver spawning it does. Wine on
# Linux does (`wine /home/me/x.exe`); `wsl.exe` does not — the driver holds a
# drive-letter path and the callee needs `/mnt/c/...`. AND THE FAILURE IS NOT A
# PATH ERROR: the callee opens a RELATIVE file of that name, misses, and the run
# reads as a broken binary.
#
# ★ NEITHER FUNCTION KNOWS WHAT `wslpath` IS. The verb is the LAUNCHER's
# declaration (legs.json `pathTranslation`, arriving as LEG_PATH_TRANSLATION) and
# the resolver performs it, so the translator is named in exactly one file.
launch_path() {                # launch_path <verb> <path>  -> stdout
  local verb="$1" p="$2" out rc
  if [[ -z "$verb" || "$verb" == "none" ]]; then printf '%s\n' "$p"; return 0; fi
  # rc DIRECTLY off python3, never after a pipe.
  if out="$(python3 "$LEG_RESOLVER" --catalogue "$LEG_CATALOGUE" \
              --path-translation "$verb" --translate-path "$p" 2>&1)"; then rc=0; else rc=$?; fi
  [[ $rc -eq 0 && -n "${out//[[:space:]]/}" ]] || die "could not translate '$p' into the launcher's path namespace (pathTranslation '$verb', rc=$rc):
      ${out:-<no diagnostic>}
      The leg's DECLARED launcher cannot be handed a path at all, so this run stops here
      rather than spawn it with one its callee would silently read as a relative filename."
  printf '%s\n' "${out%%$'\n'*}"
}
# The net under "translate at construction": every argument, at the ONE place the
# child is spawned. A future segment kind that adds a path-valued argument and
# forgets to translate it is refused BY NAME instead of failing hours in looking
# like a fixture bug.
# ★ `--assert-translated=` uses the `=` form deliberately: a real fixture argv
# carries `--start=full:`, which the space form would be parsed as an option.
#
# ── THE LAUNCHER'S ENVIRONMENT NAMESPACE ─────────────────────────────────────
# Twin of Resolve-LaunchEnvCarrier in build-and-test.ps1. The second half of the
# same fact, found by MEASURING the first: a launcher in another OS namespace
# does not inherit this driver's environment any more than it understands its
# paths. ✔MEASURED 2026-08-04: a wsl.exe-launched fixture saw
# SQLITE_TEST_PATTERN_LIST as EMPTY, so the corpus RESUME ENGINE — which selects
# its files through exactly that variable — silently re-ran the corpus from the
# beginning instead of from the abort point.
#
# Prints the `NAME=VALUE` assignments the caller must EXPORT, one per line, and
# nothing at all for a verb whose child inherits (every launcher this catalogue
# declares on a POSIX host). The carrier's NAME and the separator its list uses
# belong to the VERB, so both come from the resolver.
# ★★ ONLY A VARIABLE THAT IS ACTUALLY SET MAY BE CARRIED, AND THAT IS NOT
# TIDINESS — IT IS THE DIFFERENCE BETWEEN A RUN AND A FALSE GREEN. MEASURED
# 2026-08-04 on the .ps1 side: naming an UNSET variable in the carrier
# materialised it in the other namespace as EMPTY-BUT-EXISTING, sqlite's
# permutations.test asks `info exists ::env(SQLITE_TEST_PATTERN_LIST)`, and an
# empty-but-existing value is an EMPTY FILE LIST rather than "no filter" — the
# tier selected ZERO files and the driver called it GREEN. Hence the `-v` guard
# below, and hence this is called PER SEGMENT.
launch_env_carrier_name() {    # launch_env_carrier_name <verb>  -> stdout
  local verb="$1" vocab rc carrier
  [[ -n "$verb" && "$verb" != "inherit" ]] || return 0
  if vocab="$(python3 "$LEG_RESOLVER" --catalogue "$LEG_CATALOGUE" --env-transfers 2>&1)"; then rc=0; else rc=$?; fi
  [[ $rc -eq 0 ]] || die "could not read the environment-transfer vocabulary (rc=$rc):
      ${vocab:-<no diagnostic>}"
  carrier="$(printf '%s\n' "$vocab" | awk -F'\t' -v v="$verb" '$1 == v { print $2 }')"
  [[ -n "$carrier" ]] || die "envTransfer '$verb' declares no carrier variable, yet it is not 'inherit'.
      The resolver and this driver disagree about the vocabulary."
  printf '%s\n' "$carrier"
}
launch_env_carrier() {         # launch_env_carrier <verb> <current> <name...>
  local verb="$1" current="$2"; shift 2
  [[ -n "$verb" && "$verb" != "inherit" ]] || return 0
  local -a call=(--env-transfer "$verb" --carrier-current "$current") n
  for n in "$@"; do
    [[ -n "$n" ]] || continue
    [[ -n "${!n:-}" ]] || continue     # THE FILTER — see the note above
    call+=(--forward "$n")
  done
  # Nothing SET means nothing to carry, and carrying nothing is the correct
  # answer — not an empty carrier that manufactures empty variables.
  [[ " ${call[*]} " == *" --forward "* ]] || return 0
  local out rc
  if out="$(python3 "$LEG_RESOLVER" --catalogue "$LEG_CATALOGUE" "${call[@]}" 2>&1)"; then rc=0; else rc=$?; fi
  [[ $rc -eq 0 ]] || die "could not resolve the launcher's environment transfer (envTransfer '$verb', rc=$rc):
      ${out:-<no diagnostic>}
      Without it the launched fixture runs with an EMPTY run environment, which does not
      fail — it silently changes what the corpus does."
  printf '%s\n' "$out"
}
assert_launch_args_translated() {   # assert_launch_args_translated <verb> <arg...>
  local verb="$1"; shift
  [[ -n "$verb" && "$verb" != "none" ]] || return 0
  local -a call=(--path-translation "$verb") a
  for a in "$@"; do call+=("--assert-translated=$a"); done
  local out rc
  if out="$(python3 "$LEG_RESOLVER" --catalogue "$LEG_CATALOGUE" "${call[@]}" 2>&1)"; then rc=0; else rc=$?; fi
  [[ $rc -eq 0 ]] || die "REFUSING to spawn the leg's launcher — an argument is still in THIS driver's path namespace, not the launcher's:
      ${out:-<no diagnostic>}"
}

# ── THE LEG PLAN ─────────────────────────────────────────────────────────────
# ONE call, one resolver, shared with build-and-test.ps1. The host is an INPUT to
# the plan (it decides run mode), never a filter on it: every host gets the same
# legs, and `--format sh` emits ONLY variable assignments (asserted by the
# resolver's own self-test, which ran at Step 0b) so this `eval` cannot execute a
# command even if the catalogue were hostile.
# ★ rc is captured DIRECTLY off python3 — never after a pipe, and never from
# inside a `printf` argument, where a failing substitution leaves an EMPTY field
# that `set -e` does not see (D-HARNESS-SH-SRC-DIR-GIT-REQUIRED-VS-RSYNC-GATE
# item 5). stderr goes to its own file so a diagnostic can never be eval'd.
_plan_err="$(mktemp)" || die "could not create a temp file for the leg plan's stderr."
if LEG_PLAN_SH="$(python3 "$LEG_RESOLVER" --catalogue "$LEG_CATALOGUE" \
                    --plan --host-os "$HOST_OS" --host-arch "$HOST_ARCH" \
                    --format sh 2>"$_plan_err")"; then
  _plan_rc=0
else
  _plan_rc=$?
fi
_plan_msg="$(cat "$_plan_err" 2>/dev/null || true)"; rm -f "$_plan_err"
[[ "$_plan_rc" -eq 0 ]] || die "the leg resolver FAILED (rc=$_plan_rc) — refusing to guess a leg set.
      command: python3 $LEG_RESOLVER --plan --host-os $HOST_OS --host-arch $HOST_ARCH --format sh
      ${_plan_msg:-<no diagnostic on stderr>}"
[[ -n "${LEG_PLAN_SH//[[:space:]]/}" ]] || die "the leg resolver produced EMPTY output (rc=0).
      An empty plan would leave this run with ZERO legs and a summary that says nothing
      failed — the silent-shortfall shape this whole mechanism exists to prevent.
      ${_plan_msg:-<no diagnostic on stderr>}"
eval "$LEG_PLAN_SH"
[[ ${#LEG_ORDER[@]} -gt 0 ]] || die "the leg plan parsed but declares ZERO legs — see $LEG_CATALOGUE."
LEG_DECLARED=("${LEG_ORDER[@]}")
# Seed each leg's verdict from the plan: a leg the resolver already knows cannot be
# EXECUTED here carries its named skip from this moment on, and it is STILL BUILT.
for _l in "${LEG_DECLARED[@]}"; do
  LEG_VERDICT["$_l"]="${LEG_RUN_VERDICT[$_l]}"
  LEG_VERDICT_DETAIL["$_l"]="${LEG_RUN_DETAIL[$_l]}"
done
info "legs declared by $(basename "$LEG_CATALOGUE") (the SAME set on every host): ${LEG_DECLARED[*]}"
for _l in "${LEG_DECLARED[@]}"; do
  case "${LEG_RUN_MODE[$_l]}" in
    native)   info "  $_l  ${LEG_SPEC[$_l]}  — build + run NATIVELY here" ;;
    launched) info "  $_l  ${LEG_SPEC[$_l]}  — build here, run under '${LEG_LAUNCH[$_l]}'$( [[ "${LEG_PATH_TRANSLATION[$_l]}" != "none" ]] && printf ' [paths -> %s]' "${LEG_PATH_TRANSLATION[$_l]}" )" ;;
    skip)     info "  $_l  ${LEG_SPEC[$_l]}  — build here; NOT runnable on this host [${LEG_RUN_VERDICT[$_l]}]: ${LEG_RUN_DETAIL[$_l]}" ;;
    *)        die  "leg '$_l' has an unknown run mode '${LEG_RUN_MODE[$_l]}' — the resolver and this driver disagree about the vocabulary." ;;
  esac
done

# DSS_LEGS: comma-separated filter (e.g. DSS_LEGS=elf64-x86_64) for fast iteration.
# It never adds a leg and never changes a verdict class — a filtered-out leg is
# ledgered as `not-selected-by-runner`, the ledger's own name for "the harness, not
# the machine, chose not to do this". It is reported and never fatal, but the COST
# is spelled out: that leg's coverage is simply gone from this run.
if [[ -n "${DSS_LEGS:-}" ]]; then
  declare -a _filtered=() _dropped=()
  for _l in "${LEG_ORDER[@]}"; do
    case ",${DSS_LEGS}," in
      *",${_l},"*) _filtered+=("$_l") ;;
      *)           _dropped+=("$_l")  ;;
    esac
  done
  [[ ${#_filtered[@]} -gt 0 ]] || \
    die "DSS_LEGS='${DSS_LEGS}' matched no declared leg (have: ${LEG_DECLARED[*]})."
  LEG_ORDER=("${_filtered[@]}")
  if [[ ${#_dropped[@]} -gt 0 ]]; then
    warn "DSS_LEGS='${DSS_LEGS}' DESELECTED ${#_dropped[@]} declared leg(s) — this run does NOT cover them:"
    for _l in "${_dropped[@]}"; do
      LEG_VERDICT["$_l"]="not-selected-by-runner"
      LEG_VERDICT_DETAIL["$_l"]="deselected by DSS_LEGS='${DSS_LEGS}' — ${LEG_SPEC[$_l]} was NOT built and NOT verified by this run"
      warn "      $_l (${LEG_SPEC[$_l]}) — not built, not verified"
    done
  fi
fi
info "legs selected: ${LEG_ORDER[*]}   tier: $DSS_TIER"

# ── strict mode, parsed and VALIDATED now (never at Step 9) ──────────────────
# Mirrors `readStrictArmVerdicts` in tests/test_support/arm_verdict_ledger.hpp,
# accepted spellings and all. A value it does not recognise is REFUSED here rather
# than read as "off": a typo'd `=ture` that silently disables the gate is exactly
# the failure the C++ twin's `malformed` flag exists to make unsayable — and
# refusing at Step 1 costs a second, while discovering it at Step 9 costs the run.
STRICT_VERDICTS=0
case "${DSS_STRICT_ARM_VERDICTS}" in
  1|true|TRUE|yes)        STRICT_VERDICTS=1 ;;
  ''|0|false|FALSE|no)    STRICT_VERDICTS=0 ;;
  *) die "DSS_STRICT_ARM_VERDICTS='${DSS_STRICT_ARM_VERDICTS}' is not a value this harness recognises.
      Accepted: 1 / true / TRUE / yes  (strict)  ·  0 / false / FALSE / no / empty (default).
      An unrecognised value is REFUSED, never read as 'off' — a gate that a typo can
      silently disable is worse than no gate. Same rule as readStrictArmVerdicts() in
      tests/test_support/arm_verdict_ledger.hpp, which the corpus runners share." ;;
esac
[[ "$STRICT_VERDICTS" -eq 0 ]] || \
  warn "DSS_STRICT_ARM_VERDICTS=1 — every ENVIRONMENTAL skip (a missing launcher, a missing declared build input) will FAIL this run."

ensure_cmd curl curl
curl -fsS --max-time 20 -o /dev/null https://github.com || die "offline — cannot reach https://github.com."
pass "$HOST_OS/$HOST_ARCH host is online"
ensure_cmd git git
# HOST toolchain, for the harness's OWN work (deriving the recipe, building the
# reference oracle) — NOT the per-leg target compilers, which come from each leg's
# declared `targetCc` candidates in Step 6.
if [[ "$HOST_OS" == "darwin" ]]; then
  command -v cc >/dev/null 2>&1 || die "no C compiler (cc) — run 'xcode-select --install'."
  command -v make >/dev/null 2>&1 || die "no 'make' — run 'xcode-select --install'."
else
  ensure_cmd gcc build-essential
  ensure_cmd make build-essential
  ensure_cmd ar binutils
fi

# ── Step 2 — dss-code-prime (current checkout, VERIFIED, untouched) ──────────
# WHAT THIS GATE REPLACED, AND WHY IT IS WORTH THE LINES
# (D-HARNESS-SH-SRC-DIR-GIT-REQUIRED-VS-RSYNC-GATE). The old form decided the whole
# question — WHICH COMPILER a multi-hour corpus run is about to validate — from one
# filesystem fact, `[[ -d $SRC_DIR/.git ]]`, and had exactly two outcomes: use it, or
# clone with an EMPTY wanted-branch. Three shapes came out of that:
#   (a) $SRC_DIR ABSENT (any clean machine) -> silent clone, and because the third
#       argument was "" the clone landed on the repo's DEFAULT branch, main. Hours
#       spent on a compiler nobody asked for, Step 9 printing that commit as the
#       provenance of the verdict, and one easily-missed banner as the only signal.
#   (b) a STALE .git beside CURRENT sources — precisely what the WSL ctest gate's
#       `rsync --exclude=/.git` leaves behind. The run is VALID and its LABEL lies.
#   (c) $SRC_DIR populated but NOT a checkout — the same rsync, onto a machine that
#       never had the .git. `git clone` refuses a non-empty target, so the harness
#       died inside a helper with a git error that names none of this.
# Now: (a) is a refusal that says what to type, (b) is self-labelling, (c) names the
# scenario. ⚠ NONE of this rsyncs .git — it is 959 MB, and the rsync exists to gate
# UNCOMMITTED work that no commit can represent.
#
# ★ This region is EXTRACTED and executed by test-confound-scope.sh (the Step-0
# self-test) against real throwaway repos, at top level under these same shell
# options — so all four shapes are exercised in under a second before any run
# starts, rather than discovered on the one machine that hits them.
# >>> dss:src-gate >>>
if [[ -e "$SRC_DIR/.git" ]]; then
  # `-e`, not `-d` — a `git worktree` checkout keeps .git as a FILE, and under `-d`
  # such a tree fell through to shape (c) and was refused for no reason.
  step "2/9  Use dss-code-prime at $SRC_DIR (current checkout, untouched)"
elif dir_has_entries "$SRC_DIR"; then
  die "$SRC_DIR exists and is NOT a git checkout (no .git entry).
      That is the tree the WSL ctest gate produces: it rsyncs the working tree with
      --exclude=/.git, so you get real sources and no repository. The harness will
      NOT clone over it — git clone refuses a non-empty directory anyway, which is
      how this used to surface (an opaque git error from inside a helper).
      Point SRC_DIR at a real checkout:
        SRC_DIR=${SELF_REPO:-/path/to/dss-code-prime} $0
      or, if this tree is disposable, remove it and opt in to a clone:
        DSS_ALLOW_FRESH_CLONE=1 DSS_BRANCH=<branch> $0"
elif [[ "$DSS_ALLOW_FRESH_CLONE" == "1" ]]; then
  step "2/9  Clone dss-code-prime -> $SRC_DIR (DSS_ALLOW_FRESH_CLONE=1, branch: ${DSS_BRANCH:-<repo default>})"
  # ★ The third argument is the entire fix for shape (a): clone_or_update has always
  # accepted a wanted branch and honoured it (see its `else` arm) — this call site
  # passed "" and let default_branch() resolve origin/HEAD to main.
  clone_or_update "$DSS_REPO_URL" "$SRC_DIR" "$DSS_BRANCH"
else
  die "no dss-code-prime checkout at $SRC_DIR, and this harness will NOT clone one silently.
      A fresh clone takes $DSS_REPO_URL at its DEFAULT branch (main) unless told
      otherwise — so an unattended multi-hour corpus run would validate a compiler
      that is not the branch you are working on, and Step 9 would print that commit
      as if it were the one under test. Say which you want:
        SRC_DIR=${SELF_REPO:-/path/to/your/checkout} $0   # use a checkout you have
        DSS_ALLOW_FRESH_CLONE=1 DSS_BRANCH=<branch> $0    # clone, and NAME the branch
      ⚠ SRC_DIR defaults to \$HOME/src/dss-code-prime, which is NOT necessarily the
      tree this script was run from — that mismatch is the usual way a run lands here.
      (DSS_ALLOW_FRESH_CLONE=1 without DSS_BRANCH does clone the default branch —
      the point is that you asked for it, not that it is forbidden.)"
fi
# One read, one banner, on every path — including the fresh clone, whose DSS_COMMIT
# must be checked too. $SRC_HEAD / $SRC_DIVERGE_NOTE are what Step 9 reprints.
read_src_provenance "$SRC_DIR"
info "  at $SRC_HEAD on $SRC_BRANCH$SRC_DIVERGE_NOTE"
# ── the DECLARED ref, ASSERTED against the checkout ──────────────────────────
# Intent the operator states beats intent inferred from the filesystem. Both unset
# (the default) leaves behaviour exactly as it was: use the checkout as it is.
if [[ -n "$DSS_BRANCH" && "$SRC_BRANCH" != "$DSS_BRANCH" ]]; then
  die "DSS_BRANCH='$DSS_BRANCH' but $SRC_DIR is on '$SRC_BRANCH'.
      Refusing to spend a multi-hour corpus run on a branch you did not ask for.
      Check the branch out yourself — this harness NEVER switches our own repo, so
      that a probe tests the working tree exactly as it is — or drop DSS_BRANCH."
fi
if [[ -n "$DSS_COMMIT" ]]; then
  # `--verify … ^{commit}` both resolves an abbreviation and proves the object is a
  # commit that EXISTS here; a typo or an unfetched sha fails loud instead of
  # silently comparing two strings that were never going to match.
  DSS_COMMIT_FULL="$(git -C "$SRC_DIR" rev-parse --verify --quiet "${DSS_COMMIT}^{commit}" 2>/dev/null)" || DSS_COMMIT_FULL=""
  [[ -n "$DSS_COMMIT_FULL" ]] || \
    die "DSS_COMMIT='$DSS_COMMIT' does not resolve to a commit in $SRC_DIR — fetch it, or fix the value."
  [[ -n "$SRC_HEAD_LONG" ]] || \
    die "DSS_COMMIT='$DSS_COMMIT' cannot be verified: rev-parse HEAD failed in $SRC_DIR."
  [[ "$DSS_COMMIT_FULL" == "$SRC_HEAD_LONG" ]] || \
    die "DSS_COMMIT='$DSS_COMMIT' ($DSS_COMMIT_FULL) but $SRC_DIR is at $SRC_HEAD_LONG.
      The run would have validated the checkout, not the commit you named."
  info "  DSS_COMMIT verified: HEAD is $DSS_COMMIT_FULL"
fi
# The divergence is REPORTED, never fatal — a dirty tree is the NORMAL shape of a
# pre-commit probe, and refusing it would delete the harness's main use.
if [[ -z "$SRC_DIVERGE" ]]; then
  warn "could not measure how far $SRC_DIR diverges from HEAD (git status failed) — the commit in the Step-9 verdict is UNVERIFIED."
elif [[ "$SRC_DIVERGE" != "0" ]]; then
  warn "$SRC_DIVERGE file(s) in $SRC_DIR differ from HEAD ($SRC_HEAD) — the compiler this run builds is NOT that commit.
      Not an error, and never a blocker. Two situations produce it: uncommitted work
      you are deliberately gating, and a STALE .git left beside fresh sources by the
      ctest gate's rsync. They are INDISTINGUISHABLE from inside the tree, so the
      count rides along on the Step-9 verdict rather than the harness guessing."
fi
pass "dss-code-prime ready"
# <<< dss:src-gate <<<

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
# HOST fact, not a target fact: where THIS MACHINE keeps its headers and libraries.
# Homebrew kegs and the Xcode SDK only exist on a Mac, so probing for them anywhere
# else is wasted work — and `xcrun`/`brew` are absent there anyway, which is why the
# helpers above already degrade to "" without this guard. Nothing here selects a leg.
if [[ "$HOST_OS" == "darwin" ]]; then
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
    # HOST fact: Homebrew's tcl-tk is KEG-ONLY, so its bin/ is not on PATH after
    # an install. Nothing to do with which leg is being built.
    if [[ "$HOST_OS" == "darwin" ]]; then
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
# HOST fact: which package manager provides them (and what macOS does NOT ship).
if [[ "$HOST_OS" == "darwin" ]]; then
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

# ── STAGED-TREE COHERENCE — a PRECONDITION ON A SHARED INPUT ─────────────────
# D-HARNESS-SQLITE-STAGED-TREE-MIXED-VINTAGE. $BLD is REUSED across runs while
# Step 3 `git pull --rebase`es the checkout underneath it, and `make` refreshes
# only the prerequisites of the target we ask for (`testfixture
# USE_AMALGAMATION=0`) — under which sqlite3.c / shell.c / tclsqlite3.c / tsrc/
# are prerequisites of NOTHING. Those orphans sit there looking current while
# everything around them marches forward, and the staged tree quietly accumulates
# several upstream vintages. MEASURED consequence: a cross-built sqlite3 compiled
# and linked cleanly, then exited 1 at startup on shell.c's own
# `sqlite3_sourceid()` vs `SQLITE_SOURCE_ID` guard.
#
# ★ THIS IS NOT A PER-LEG VERDICT, AND MUST NOT BECOME ONE. Every leg compiles
# the SAME staged tree, so an incoherent stage does not disadvantage one target —
# it invalidates the input all five share. Recording it as `skipped-*` on one leg
# would let the other four build from a tree already known to be inconsistent and
# then report their results as if they meant something. It is a run-wide `die`,
# raised BEFORE any leg is built.
#
# ★ AND IT IS UNSKIPPABLE — no DSS_SKIP_SELFTEST, no opt-out variable. The defect
# is host-independent and silent, and a check that only runs when someone
# remembers it fails exactly the way a host-locked driver does. Its only
# prerequisite is a POSIX shell, which is a strict subset of what this driver
# already requires, so there is no environment in which skipping it would be the
# honest thing to do — hence no skip path and no verdict name for one.
[[ -x "$SRC_COHERENCE" ]] || die "the staged-source coherence gate is missing or not executable: $SRC_COHERENCE
      It is what stops a REUSED $BLD from mixing upstream vintages behind your back
      (D-HARNESS-SQLITE-STAGED-TREE-MIXED-VINTAGE). There is no skip path for it by
      design; restore the file rather than routing around it."
"$SRC_COHERENCE" --checkout "$SQLITE_DIR" --label "staged sqlite (Step 4)" "$BLD" \
  || die "staged sqlite tree is INCOHERENT — refusing to build (D-HARNESS-SQLITE-STAGED-TREE-MIXED-VINTAGE)"

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
  # HOST fact: how to OBTAIN cmake on this machine (brew vs the Kitware tarball,
  # whose asset names are linux-only). cmake builds the COMPILER, not a leg.
  if [[ "$HOST_OS" == "darwin" ]]; then
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
# ★ FATAL FOR THE WHOLE RUN, deliberately — unlike a per-leg library miss, which is
# that leg's `skipped-build-input-missing`. The staged HEADERS are LEG-INDEPENDENT:
# DSS parses this one portable C header for EVERY leg (ABI is irrelevant at parse
# time — D-FFI-SHIPPED-LIBS-OS-ONLY), so with no tcl.h anywhere on the machine there
# is no leg — not one of the five, on any host — that could be compiled at all.
# A per-leg skip would just say the same thing five times.
[[ -f "$TCL_INC/tcl.h" ]] || die "tcl.h not found — install the Tcl DEV files (apt: tcl-dev / tcl8.6-dev; brew: tcl-tk).
      This is fatal for the ENTIRE run, not for one leg: every leg parses this same header.
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
# ── the zlib headers — ONE STAGED DIRECTORY PER TARGET CONFIGURATION ─────────
# ★ D-HARNESS-SQLITE-STAGE-ZCONF-IS-PE-SHAPED (closed TF-C115).
# The deriving host's zconf.h has been through ITS ./configure, which rewrote
#     #if 1  /* was set to #if 1 by ./configure */
#     #  define Z_HAVE_UNISTD_H
# That `#if 1` is a measurement of THIS MACHINE — the header-level twin of the
# recipe's HAVE_*/Z_HAVE_* defines that `recipeTransform` already exists to drop.
# So the pair is copied to `zinc-src/` UNTOUCHED and NO leg includes it: each leg
# DECLARES its target's answer for each guard (legs.json `build.zconfGuards`) and
# stage-zinc.py — the same tool build-and-test.ps1 calls — writes one
# zinc/<recipeTransform>/ per declared stage. Step 7 gives each leg the include
# list carrying ITS OWN stage, so no leg parses another target's zlib header.
#
# ★ THIS DRIVER'S HALF OF THE DEFECT WAS THE MIRROR IMAGE, AND IT WAS SILENT.
# The .ps1 flipped the guard for pe and said out loud that the shared stage was
# therefore pe-shaped; this driver never flipped it at all, so once TF-C114 gave
# it the pe64 leg, that leg was staged a POSIX-configured zconf.h. MEASURED
# TF-C115: that combination does not miscompile, it fails loud —
# `error[F001D] got unistd.h` — so the pe64 leg was unbuildable from here. Both
# halves are closed by the same per-target staging.
ZINC_SRC="$BLD/zinc-src"; ZINC_ROOT="$BLD/zinc"; mkdir -p "$ZINC_SRC" "$ZINC_ROOT"
# ZINC_ROOT lives INSIDE the sqlite checkout and survives between runs, so a tree
# staged by the pre-TF-C115 driver still has `zinc/zlib.h` + `zinc/zconf.h` sitting
# at its top level — the single pe-shaped header this cycle removed. Nothing puts
# that directory on an include path any more (only `zinc/<stage>/` goes there), but
# check-source-coherence.sh walks $BLD at `-maxdepth 2` and would still SEE them.
# Delete them: a stale artefact of the exact layout that was just replaced is the
# last thing a reader of this tree should find.
rm -f "$ZINC_ROOT"/*.h
ZH="$(find_in "${INC_ROOTS[@]}" -- -maxdepth 3 -name zlib.h | sed -n '1p')"
# Fatal for the whole run for the same reason as tcl.h above: one portable header,
# parsed by every leg.
[[ -n "$ZH" ]] || die "zlib.h not found — install zlib1g-dev (or 'brew install zlib').
      This is fatal for the ENTIRE run, not for one leg: every leg parses this same header.
      roots searched: ${INC_ROOTS[*]}"
cp -f "$ZH" "$ZINC_SRC/"
# zconf.h beside zlib.h first (they are a matched pair); the sweep is the fallback.
# Deliberately unquoted — this word-splits the find results into loop items.
ZCH=""
for zc in "$(dirname "$ZH")/zconf.h" $(find_in "${INC_ROOTS[@]}" -- -maxdepth 3 -name zconf.h); do
  [[ -f "$zc" ]] && { cp -f "$zc" "$ZINC_SRC/"; ZCH="$zc"; break; }
done
# zconf.h is where every guard lives, so "zlib.h without it" is not a degraded
# stage, it is no stage — and it used to be tolerated silently (`|| true`).
[[ -n "$ZCH" ]] || die "zconf.h not found beside $ZH nor anywhere under ${INC_ROOTS[*]}.
      zlib.h includes it and every ./configure guard lives in it; without it no leg's
      zlib header can be configured for its target."
# label -> the include-list file that leg's manifest must use; populated below.
declare -A LEG_INC_FILE=() LEG_ZINC_DIR=()
declare -A ZINC_STAGE_DIR=()
# rc DIRECTLY off python3 (never through a pipe) — the output is captured first
# and parsed after, so a FAIL line is still read on a non-zero rc.
ZINC_OUT="$(python3 "$SCRIPT_DIR/stage-zinc.py" --zlib-h "$ZINC_SRC/zlib.h" \
              --zconf-h "$ZINC_SRC/$(basename "$ZCH")" --dest "$ZINC_ROOT" \
              --catalogue "$LEG_CATALOGUE" 2>&1)" || true
while IFS= read -r _zl; do
  case "$_zl" in
    ZINC-STAGE-OK=*)
      _rest="${_zl#ZINC-STAGE-OK=}"; _k="${_rest%%|*}"; _rest="${_rest#*|}"
      _dir="${_rest%%|*}"; _rest="${_rest#*|}"; _guards="${_rest%%|*}"; _note="${_rest#*|}"
      ZINC_STAGE_DIR["$_k"]="$_dir"
      info "zinc stage '$_k' -> $_dir   [$_guards]"
      [[ -z "$_note" ]] || info "      note: $_note" ;;
    ZINC-STAGE-FAIL=*)
      _rest="${_zl#ZINC-STAGE-FAIL=}"
      warn "zinc stage '${_rest%%|*}' COULD NOT BE PRODUCED — ${_rest#*|}" ;;
    ZINC-STAGES=*) info "zinc stages: ${_zl#ZINC-STAGES=} produced" ;;
    *) [[ -z "$_zl" ]] || info "      $_zl" ;;
  esac
done <<< "$ZINC_OUT"
[[ ${#ZINC_STAGE_DIR[@]} -gt 0 ]] || die "stage-zinc.py produced NO per-target zlib header dir:
$ZINC_OUT"
THIRD_PARTY_INCS=("$TCL_INC")
info "tcl $TCL_VER headers: $TCL_INC   zlib source headers: $ZINC_SRC (from $ZH) -> ${#ZINC_STAGE_DIR[@]} per-target stage(s) under $ZINC_ROOT"

# ── PER-LEG libraries — each leg resolves its OWN (tcl, z) pair ──────────────
# WHAT THESE ARE FOR — they flow into the per-leg `.dss-project.json`
# "resolveLibraries" (Step 7), i.e. DSS `--resolve-library`.
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
find_first_in() {               # find_first_in <dir>... -- <name>... -> first readable hit
  local -a dirs=()
  while [[ $# -gt 0 && "$1" != "--" ]]; do [[ -n "$1" ]] && dirs+=("$1"); shift; done
  [[ "${1:-}" == "--" ]] && shift
  local d n
  for d in ${dirs[@]+"${dirs[@]}"}; do
    [[ -d "$d" ]] || continue
    for n in "$@"; do
      [[ -n "$n" && -f "$d/$n" && -r "$d/$n" ]] && { printf '%s' "$d/$n"; return; }
    done
  done
  return 0
}
# The OBJECT-FORMAT FAMILY a library FILE NAME belongs to. Used to keep the
# tclConfig-derived candidates below from leaking across formats — see
# `host_system_tcl_names`. Keyed on the NAME, never on the host: a `.dylib` is a
# Mach-O library wherever it is sitting.
lib_name_family() {             # lib_name_family <file-name> -> dll | dylib | so | ""
  case "$1" in
    *.dll)          printf 'dll'   ;;
    *.dylib)        printf 'dylib' ;;
    *.so|*.so.*)    printf 'so'    ;;
    *)              printf ''      ;;
  esac
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
# The name list a `host-system` leg searches for, in PRECEDENCE order:
#   1. the DERIVED names — this installation's own TCL_LIB_FILE, filtered to the
#      OBJECT-FORMAT FAMILIES the leg itself declares. Derivation must come first
#      or the harness loses its version-agnosticism (TF-C65: nothing pins a Tcl
#      version any more, and a host whose Tcl is 8.7 or 9.1 matches no fixed list);
#      the family filter is what stops a Linux host from handing its `libtcl8.6.so`
#      to the macho64 legs, whose declared names are all `.dylib`. That filter is
#      keyed on the LEG'S OWN DECLARATION, not on the host.
#   2. the leg's DECLARED tclNames, verbatim, as the backstop for a host with no
#      tclConfig.sh at all.
# `!seen[$0]++` because the derived list and the declared list legitimately
# OVERLAP (a Debian host derives `libtcl8.6.so`, which the elf leg also declares).
# A duplicate candidate would search twice and, more to the point, print twice in
# the "names tried" of a skip diagnostic — which reads as a mistake.
host_system_tcl_names() {       # host_system_tcl_names <leg>  -> one candidate per line
  local leg="$1" want n fam
  local -a declared=()
  read -r -a declared <<< "${LEG_LIB_TCL_NAMES[$leg]}"
  { for n in ${TCL_LIB_NAMES[@]+"${TCL_LIB_NAMES[@]}"}; do
      [[ -n "$n" ]] || continue
      fam="$(lib_name_family "$n")"
      for want in ${declared[@]+"${declared[@]}"}; do
        if [[ "$fam" == "$(lib_name_family "$want")" ]]; then printf '%s\n' "$n"; break; fi
      done
    done
    printf '%s\n' ${declared[@]+"${declared[@]}"}
  } | LC_ALL=C awk 'NF && !seen[$0]++'
}

# arm64 libraries for the `ubuntu-ports-arm64` provider — Ubuntu ports .deb
# extract, NO apt-source surgery: resolve the exact .deb from the ports Packages
# index, then dpkg-deb -x and harvest the runtime .so. qemu resolves libc/libm from
# the sysroot.
# ★ NOT FATAL TO THE RUN. It used to `die`, which meant a host that cannot reach
# ports.ubuntu.com (or has no dpkg-deb) lost EVERY leg over one leg's input. It is
# now called in a SUBSHELL so a `die`/ERR inside it exits only that subshell: the
# caller sees rc != 0, records `skipped-build-input-missing` for THAT leg, and the
# other four carry on. The diagnostics still print — loudly — they just stop being
# the whole run's obituary.
ARM64_LIBDIR="${ARM64_LIBDIR:-$HOME/.cache/dss-code-prime/arm64libs}"
ensure_arm64_libs() {
  if [[ -e "$ARM64_LIBDIR/libtcl8.6.so.0" && -e "$ARM64_LIBDIR/libz.so.1" ]]; then
    info "arm64 libs cached: $ARM64_LIBDIR ($(ls "$ARM64_LIBDIR" | tr '\n' ' '))"; return
  fi
  ensure_cmd curl curl; ensure_cmd dpkg-deb dpkg; ensure_cmd gzip gzip
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

# ── resolve EVERY declared leg's build inputs ────────────────────────────────
# ★ ONE VERDICT PER LEG, AND THE RUN CONTINUES. A leg whose DECLARED inputs are
# absent from this machine is recorded `skipped-build-input-missing` — an
# ENVIRONMENTAL skip in the ledger's vocabulary: not a compile failure (nothing was
# miscompiled) and not structural (another machine, same catalogue, builds it
# fine). It warns by default and REDS under DSS_STRICT_ARM_VERDICTS=1.
#
# PRECEDENCE, stated because two verdicts can apply at once: a leg that is BOTH
# un-runnable here (say a macho leg on Linux) and missing its build inputs records
# the BUILD-INPUT verdict. The build is what the driver actually attempted and the
# earlier fact; the run verdict it displaces is preserved verbatim in the detail
# string, so nothing is lost — and it is the honest headline, because "this box has
# no Darwin libtcl" is the reason the artifact does not exist at all, which is a
# strictly bigger statement than "and it could not have run it either".
leg_marks_missing() {           # leg_marks_missing <leg> <what-is-lost> <why>
  # The DISPLACED run verdict rides along in the detail, so promoting the build
  # fact to the headline never erases the run fact.
  local extra=""
  [[ -z "${LEG_RUN_VERDICT[$1]}" ]] || extra="  [and this host could not RUN it either: ${LEG_RUN_VERDICT[$1]}]"
  LEG_VERDICT["$1"]="skipped-build-input-missing"
  LEG_VERDICT_DETAIL["$1"]="$3$extra"
  warn "[$1] BUILD INPUT MISSING — $2: $3"
}
for leg in "${LEG_ORDER[@]}"; do
  provider="${LEG_LIB_PROVIDER[$leg]}"
  tcl_lib=""; z_lib=""; searched=""
  case "$provider" in
    host-system)
      # This machine's OWN tcl/zlib, discovered the version-agnostic way.
      declare -a _tnames=(); mapfile -t _tnames < <(host_system_tcl_names "$leg")
      declare -a _znames=(); read -r -a _znames <<< "${LEG_LIB_Z_NAMES[$leg]}"
      tcl_lib="$(find_first ${_tnames[@]+"${_tnames[@]}"})"
      z_lib="$(find_first ${_znames[@]+"${_znames[@]}"})"
      searched="provider 'host-system'; tcl names tried: ${_tnames[*]:-<none>}; zlib names tried: ${_znames[*]:-<none>}; roots: ${LIB_ROOTS[*]}"
      ;;
    ubuntu-ports-arm64)
      # SUBSHELL on purpose — see ensure_arm64_libs. rc is taken DIRECTLY off the
      # subshell, never through a pipe.
      if ( ensure_arm64_libs ); then _ports_rc=0; else _ports_rc=$?; fi
      declare -a _tnames=(); read -r -a _tnames <<< "${LEG_LIB_TCL_NAMES[$leg]}"
      declare -a _znames=(); read -r -a _znames <<< "${LEG_LIB_Z_NAMES[$leg]}"
      if [[ "$_ports_rc" -eq 0 ]]; then
        tcl_lib="$(find_first_in "$ARM64_LIBDIR" -- ${_tnames[@]+"${_tnames[@]}"})"
        z_lib="$(find_first_in "$ARM64_LIBDIR" -- ${_znames[@]+"${_znames[@]}"})"
      fi
      searched="provider 'ubuntu-ports-arm64' (rc=$_ports_rc); tcl names tried: ${_tnames[*]:-<none>}; zlib names tried: ${_znames[*]:-<none>}; staged under: $ARM64_LIBDIR"
      ;;
    search-paths)
      # Declared candidate DIRECTORIES, tried on EVERY host: a hit is used, a miss
      # costs nothing. NEWLINE-separated, because a path may contain spaces.
      declare -a _paths=(); mapfile -t _paths < <(printf '%s' "${LEG_LIB_PATHS[$leg]}")
      declare -a _tnames=(); read -r -a _tnames <<< "${LEG_LIB_TCL_NAMES[$leg]}"
      declare -a _znames=(); read -r -a _znames <<< "${LEG_LIB_Z_NAMES[$leg]}"
      tcl_lib="$(find_first_in ${_paths[@]+"${_paths[@]}"} -- ${_tnames[@]+"${_tnames[@]}"})"
      z_lib="$(find_first_in ${_paths[@]+"${_paths[@]}"} -- ${_znames[@]+"${_znames[@]}"})"
      searched="provider 'search-paths'; tcl names tried: ${_tnames[*]:-<none>}; zlib names tried: ${_znames[*]:-<none>}; paths searched: ${_paths[*]:-<none declared or all \${env:...} unset>}"
      ;;
    *)
      die "[$leg] declares library provider '$provider', which this driver does not implement.
      Known: host-system | ubuntu-ports-arm64 | search-paths (see $LEG_CATALOGUE and
      LIBRARY_PROVIDERS in $LEG_RESOLVER). A provider the driver silently ignored would
      resolve to an empty library pair and read as a missing input — so it fails loud here."
      ;;
  esac
  if [[ -z "$tcl_lib" || -z "$z_lib" ]]; then
    _lost="libtcl and libz"
    [[ -n "$tcl_lib" ]] && _lost="libz"
    [[ -n "$z_lib"   ]] && _lost="libtcl"
    leg_marks_missing "$leg" "this leg is NOT built on this host" \
      "no $_lost for ${LEG_SPEC[$leg]} on this machine — $searched"
    continue
  fi
  LEG_TCL_LIB["$leg"]="$tcl_lib"; LEG_Z_LIB["$leg"]="$z_lib"
  info "[$leg] libs ($provider): $tcl_lib  +  $z_lib"
done

# ── each leg's TARGET C compiler (the loadext helper extension) ──────────────
# WHY IT IS RESOLVED HERE AND NOT AT STEP 8, and why a missing one does NOT stop
# the build: the compiler builds `libtestloadext.so`, the extension the loadext
# corpus dlopen()s — a RUN input, not a build input for the fixture itself. So:
#   · a leg that WILL run needs it. Absent (and un-installable) ⇒
#     `skipped-build-input-missing`, decided NOW, at Step 6, rather than after a
#     ~50 s..8 min compile and a staging step — the leg is still BUILT (that is
#     unconditional), it just will not be run.
#   · a leg that will NOT run here does not need it at all, and failing it for a
#     cross-compiler it would never invoke would manufacture an environmental skip
#     out of nothing — on a Linux box that is both macho legs, every run.
# The candidates are the leg's own declared `targetCc.candidates`, first present
# wins. ⚠ The old `${CC:-cc}` override is deliberately NOT carried over: with five
# legs, one `$CC` cannot say WHICH leg's target compiler it means, and applying it
# to a cross leg is exactly the wrong-arch helper this anchor
# (D-HARNESS-ARM64-LEG-HOST-ARCH-HELPER-SO) exists to prevent. `$CC` still governs
# PROBE_CC, which is a genuine host question.
for leg in "${LEG_ORDER[@]}"; do
  # A leg this host cannot RUN needs no helper extension — do not manufacture an
  # environmental skip out of a cross-compiler it would never invoke.
  [[ "${LEG_RUN_MODE[$leg]}" != "skip" ]] || continue
  # A leg with no libraries is not built, so there is no run to stage for it.
  [[ -n "${LEG_TCL_LIB[$leg]:-}" ]] || continue
  declare -a _ccs=(); read -r -a _ccs <<< "${LEG_CC_CANDIDATES[$leg]}"
  cc=""
  for c in ${_ccs[@]+"${_ccs[@]}"}; do
    command -v "$c" >/dev/null 2>&1 && { cc="$c"; break; }
  done
  if [[ -z "$cc" && -n "${LEG_CC_PKG[$leg]}" ]]; then
    warn "[$leg] no target C compiler on PATH (${_ccs[*]:-<none declared>}) — trying to install ${LEG_CC_PKG[$leg]}"
    # Subshell: a package manager that cannot supply it costs this leg its RUN,
    # never the whole harness.
    ( pkg_install "${LEG_CC_PKG[$leg]}" ) || true
    hash -r
    for c in ${_ccs[@]+"${_ccs[@]}"}; do
      command -v "$c" >/dev/null 2>&1 && { cc="$c"; break; }
    done
  fi
  if [[ -z "$cc" ]]; then
    leg_marks_missing "$leg" "it is still BUILT, but this host cannot RUN its corpus" \
      "no target C compiler for ${LEG_SPEC[$leg]} — tried ${_ccs[*]:-<none declared>}${LEG_CC_PKG[$leg]:+ (apt: ${LEG_CC_PKG[$leg]})}. It builds this leg's libtestloadext.so, the extension the loadext corpus dlopen()s; NOT falling back to the host compiler, because a HOST-arch extension the $leg fixture cannot load would false-red every loadext-* test as a genuine DSS failure [D-HARNESS-ARM64-LEG-HOST-ARCH-HELPER-SO]"
    continue
  fi
  LEG_CC["$leg"]="$cc"
  info "[$leg] target cc: $cc"
done

_ready=0
for leg in "${LEG_ORDER[@]}"; do [[ -n "${LEG_TCL_LIB[$leg]:-}" ]] && _ready=$((_ready + 1)); done
[[ "$_ready" -gt 0 ]] || die "NOT ONE of the ${#LEG_ORDER[@]} selected leg(s) could resolve its declared (tcl, z) libraries on this host.
      Every leg is reported above with the exact names and roots it searched. There is nothing
      left to build, so this is fatal rather than a run that would report five skips and exit 0."
pass "headers staged + build inputs resolved for $_ready of ${#LEG_ORDER[@]} selected leg(s)"

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
#
# ★ <bin> ARRIVES ALREADY SPELLED IN THE LAUNCHER'S PATH NAMESPACE (legs.json
# `pathTranslation`), translated ONCE per leg by the caller rather than once per
# segment, and so does every path-valued argument. run_fixture_segment ASSERTS
# that before it forks — see the comment there for why not here.
run_leg() {                    # run_leg <leg> <bin> <args...>  — REPLACES this shell
  local leg="$1" bin="$2"; shift 2
  # THE LAUNCHER IS DECLARED, NOT INFERRED. `LEG_LAUNCH` is the catalogue's
  # launcher argv, shlex-quoted and space-joined by the resolver, so it may be
  # MULTI-WORD (`arch -x86_64`) and may contain spaces inside a word; `eval` on the
  # resolver's own quoting is the only correct way to split it back into an argv.
  # `LEG_LAUNCH_ENV` is the same for `NAME='value'` pairs — it is what carries
  # QEMU_LD_PREFIX for the arm64 leg, which used to be a hardcoded $QEMU_SYSROOT
  # here and is now a property of the leg that needs it.
  local -a launch=() envs=()
  eval "launch=(${LEG_LAUNCH[$leg]})"
  eval "envs=(${LEG_LAUNCH_ENV[$leg]})"
  [[ ${#envs[@]} -eq 0 ]] || export "${envs[@]}"
  # The leg's OWN library directories, for the runtime loader. Only for a leg whose
  # libraries the harness STAGED (`ubuntu-ports-arm64`, `search-paths`): a
  # `host-system` leg's libraries are, by definition, already where this machine's
  # loader looks, and prepending a system dir would be a change with no purpose.
  # Keyed on the leg's DECLARED provider, so a future staged-library leg inherits it.
  if [[ "${LEG_LIB_PROVIDER[$leg]}" != "host-system" ]]; then
    local d1 d2 libpath
    d1="$(dirname "${LEG_TCL_LIB[$leg]}")"; d2="$(dirname "${LEG_Z_LIB[$leg]}")"
    libpath="$d1"; [[ "$d2" == "$d1" ]] || libpath="$d1:$d2"
    export LD_LIBRARY_PATH="$libpath${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
  fi
  if [[ ${#launch[@]} -eq 0 ]]; then
    exec "$bin" "$@" 2>&1
  else
    exec "${launch[@]}" "$bin" "$@" 2>&1
  fi
}

# <bin> is the fixture as THIS DRIVER addresses it — the spelling `ps` reports and
# every log line names, so it is what the leftover-fixture sweeps below match on.
# <launch_bin> is the same file spelled in the LAUNCHER's namespace, which is what
# the child actually receives. They are identical for `pathTranslation: none`.
run_fixture_segment() {        # run_fixture_segment <leg> <bin> <launch_bin> <log> <args...>
  local leg="$1" bin="$2" launch_bin="$3" log="$4"; shift 4
  SEG_RC=0; SEG_KILL_REASON=""
  # THE CHOKE POINT for the launcher's path namespace, and it is HERE rather than
  # in run_leg on purpose: run_leg is the last command of a BACKGROUND subshell
  # that has already done `trap - ERR; set +e`, so a `die` inside it would exit
  # that subshell into the segment log and be read as a failing test rather than
  # as the harness refusing to start. In the foreground it stops the run.
  [[ "${LEG_RUN_MODE[$leg]}" != "launched" ]] \
    || assert_launch_args_translated "${LEG_PATH_TRANSLATION[$leg]:-none}" "$launch_bin" "$@"
  # `trap - ERR; set +e` inside the subshell is load-bearing. The old form was a
  # `|| segrc=$?` list, which suppresses errexit and the ERR trap; a BACKGROUND job
  # does not, so this subshell would inherit them (set -E) and the harness-level
  # `die` would fire on the fixture's own non-zero exit — writing a bogus
  # " [X] ERROR: failed at line …" INTO the segment log (stderr is redirected there)
  # and masking the real exit status. A failing test is data here, not an error.
  ( trap - ERR; set +e; cd "$rundir" && run_leg "$leg" "$launch_bin" "$@" ) > "$log" 2>&1 < /dev/null &
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
# python3 was already ensured at Step 0b (the leg plan needs it far earlier than
# the manifest does). Re-asserted here because this is where the manifest is
# actually generated, and `ensure_cmd` on a present command costs nothing.
ensure_cmd python3 python3
# The leg-INDEPENDENT include-dir set: the sqlite recipe dirs + the staged
# third-party tcl/zlib headers (whatever Tcl Step 6 discovered — the harness pins
# no version) + the build tree ($BLD, which resolves the
# recipe's relative `-I.`). These dirs, the TU list (${TUS[@]}) and the recipe
# defines (${RECIPE_DEFS[@]}, already `-D`-stripped) are the SAME inputs the
# per-file CLI fed; here they populate a `.dss-project.json` manifest instead.
# ★ SPLIT IN TWO SO THE PER-TARGET zinc/ CAN GO WHERE THE SHARED ONE USED TO.
# HEAD ends where the staged third-party headers end; the leg's own zlib dir is
# spliced in there and TAIL follows. Appending it to the end instead would put it
# AFTER the macOS SDK include dir below — and the SDK also ships a zlib.h, so on a
# Mac host the leg would silently take Apple's copy instead of its own staged one.
declare -a INC_DIRS_HEAD=() INC_DIRS_TAIL=()
for d in "${SQLITE_INCS[@]}" "${THIRD_PARTY_INCS[@]}"; do INC_DIRS_HEAD+=("$d"); done
INC_DIRS_TAIL+=("$BLD")
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
# Linux/Windows no-op: `sdk_prefix` is empty without xcrun. HOST fact: only a Mac
# HAS an Xcode SDK, and this is about where THIS MACHINE keeps Apple's headers —
# note that it applies to EVERY leg built here, not just the macho ones.
if [[ "$HOST_OS" == "darwin" ]]; then
  _sdk_inc="$(sdk_prefix)"
  [[ -n "$_sdk_inc" && -d "$_sdk_inc/usr/include" ]] && INC_DIRS_TAIL+=("$_sdk_inc/usr/include")
fi

# ── the recipe arrays, staged as files for the shared manifest generator ─────
# The TU list and the recipe defines are LEG-INDEPENDENT, so they are written once
# and every leg's manifest is generated from the same two files.
# FILES, not argv: ~185 absolute paths would overflow a Windows command line, which
# is why gen-pe64-manifest.py took this shape in the first place.
#
# ★ THE INCLUDE LIST IS *NOT* LEG-INDEPENDENT, and pretending it was is
# D-HARNESS-SQLITE-STAGE-ZCONF-IS-PE-SHAPED. Its last entry is the staged zlib
# header dir, which is configured for ONE target family. So one list is written
# PER HEADER STAGE — base dirs + that stage's zinc/ — and each leg is handed the
# one carrying its own. The stage KEY comes from the resolver
# (`LEG_HEADER_STAGE_KEY`, derived from the leg's declared recipeTransform); this
# driver only joins it onto a path.
RECIPE_TUS_FILE="$OUT_DIR/recipe-tus.txt"
RECIPE_DEFS_FILE="$OUT_DIR/recipe-defines.txt"
printf '%s\n' "${TUS[@]}"         > "$RECIPE_TUS_FILE"
printf '%s\n' "${RECIPE_DEFS[@]}" > "$RECIPE_DEFS_FILE"
for _k in "${!ZINC_STAGE_DIR[@]}"; do
  printf '%s\n' "${INC_DIRS_HEAD[@]}" "${ZINC_STAGE_DIR[$_k]}" "${INC_DIRS_TAIL[@]}" \
    > "$OUT_DIR/recipe-includes.$_k.txt"
done
for _l in "${LEG_DECLARED[@]}"; do
  _k="${LEG_HEADER_STAGE_KEY[$_l]:-}"
  [[ -n "$_k" && -n "${ZINC_STAGE_DIR[$_k]:-}" ]] || continue
  LEG_INC_FILE["$_l"]="$OUT_DIR/recipe-includes.$_k.txt"
  LEG_ZINC_DIR["$_l"]="${ZINC_STAGE_DIR[$_k]}"
done

# generate_manifest <leg> <out-manifest> — write this leg's project manifest.
#
# ★ ONE GENERATOR, BOTH DRIVERS. This used to be a second, inline python heredoc
# that emitted the same JSON as gen-pe64-manifest.py — two implementations of one
# decision, kept in step by hand. They had already diverged: the .py applies the
# recipe transform and emits `stackReserve`, the heredoc did neither, and the .py
# additionally asserts every source EXISTS. Calling the .py here deletes the fork
# and gives the .sh both capabilities. Everything leg-specific is an ARGUMENT, read
# from the leg's own declaration in legs.json:
#   · --target            the leg's <targetName>:<formatName> spec
#   · --resolve-library   its resolved (tcl, z) pair
#   · --recipe-transform  `none` | `windows-selfconfig`  (build.recipeTransform)
#   · --stack-reserve     bytes; 0 omits the key           (build.stackReserveBytes)
#   · --includes          THIS LEG's include list — the one carrying the zlib
#                         header staged for its target (build.zconfGuards, via
#                         build.headerStageKey). Not one shared file: that is
#                         D-HARNESS-SQLITE-STAGE-ZCONF-IS-PE-SHAPED.
# rc is taken DIRECTLY off python3 by the caller's `if`, never through a pipe.
generate_manifest() {
  local leg="$1" out="$2"
  python3 "$MANIFEST_GEN" \
    --tus       "$RECIPE_TUS_FILE" \
    --includes  "${LEG_INC_FILE[$leg]}" \
    --defines   "$RECIPE_DEFS_FILE" \
    --target    "${LEG_SPEC[$leg]}" \
    --resolve-library "${LEG_TCL_LIB[$leg]}" \
    --resolve-library "${LEG_Z_LIB[$leg]}" \
    --artifact-name   testfixture \
    --recipe-transform "${LEG_RECIPE_TRANSFORM[$leg]}" \
    --stack-reserve    "${LEG_STACK_RESERVE[$leg]}" \
    --output    "$out"
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
# ── STAGED-TREE COHERENCE, RE-ASSERTED IMMEDIATELY BEFORE THE FIRST LEG ──────
# The same run-wide precondition as at the end of Step 4, checked AGAIN here and
# for a reason: Steps 5-6 sit between the two, and Step 6 writes inside the
# checkout ($BLD/zinc) while Step 5 can take many minutes — long enough for a
# concurrent `git pull` or a hand-run `make` in that tree to move it. Re-reading a
# precondition immediately before the work it guards costs a second and is the
# difference between "the tree was coherent at some point earlier" and "the tree
# these five legs are about to compile is coherent". Same shape, same reasoning:
# a shared-input precondition, a run-wide `die`, no skip path.
"$SRC_COHERENCE" --checkout "$SQLITE_DIR" --label "staged sqlite (Step 7, pre-build)" "$BLD" \
  || die "staged sqlite tree is INCOHERENT — refusing to build (D-HARNESS-SQLITE-STAGED-TREE-MIXED-VINTAGE)"

declare -a PREFLIGHT_KILLS=()
for leg in "${LEG_ORDER[@]}"; do
  # ★ THE BUILD IS ATTEMPTED FOR EVERY DECLARED LEG, ON EVERY HOST — there is no
  # host test in this loop, and there must never be one. The ONE thing that can
  # stop a leg here is a DECLARED BUILD INPUT that Step 6 could not find on this
  # machine (DSS reads each --resolve-library binary at compile time, so without
  # them there is nothing to compile against). That is an OBSERVED absence with a
  # named verdict, already recorded and already warned about — not an inference
  # from what kind of box this is.
  if [[ -z "${LEG_TCL_LIB[$leg]:-}" || -z "${LEG_Z_LIB[$leg]:-}" ]]; then
    warn "[$leg] build NOT ATTEMPTED [${LEG_VERDICT[$leg]}] — ${LEG_VERDICT_DETAIL[$leg]}"
    continue
  fi
  # THE OTHER thing that can stop a leg here, and it is a DEFECT rather than an
  # environment: its own staged zlib header dir could not be produced (a
  # ZINC-STAGE-FAIL in Step 6). `poisoned`, named, and NO fallback to a sibling
  # stage's zinc/ — that fallback is D-HARNESS-SQLITE-STAGE-ZCONF-IS-PE-SHAPED.
  if [[ -z "${LEG_INC_FILE[$leg]:-}" ]]; then
    LEG_VERDICT["$leg"]='poisoned'
    LEG_VERDICT_DETAIL["$leg"]="the zlib header dir for this leg's stage 'zinc/${LEG_HEADER_STAGE_KEY[$leg]:-?}' (its declared zconfGuards: ${LEG_ZCONF_GUARDS[$leg]:-none}) was NOT produced — see the ZINC-STAGE-FAIL line in Step 6. Compiling it against another target's zlib header is refused."
    COMPILE_FAILS=$((COMPILE_FAILS + 1))
    warn "[$leg] POISONED — ${LEG_VERDICT_DETAIL[$leg]}"
    continue
  fi
  spec="${LEG_SPEC[$leg]}"; fmt="${LEG_FORMAT[$leg]}"; outd="$OUT_DIR/$leg"; log="$outd/compile.log"
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
  info "[$leg] $spec — ${#TUS[@]} TUs → testfixture (resolve: $(basename "${LEG_TCL_LIB[$leg]}"), $(basename "${LEG_Z_LIB[$leg]}"); transform: ${LEG_RECIPE_TRANSFORM[$leg]}; stackReserve: ${LEG_STACK_RESERVE[$leg]})"
  info "[$leg] zlib headers: ${LEG_ZINC_DIR[$leg]}  [${LEG_ZCONF_GUARDS[$leg]}]"
  # rc DIRECTLY off the generator (the `if` also keeps errexit out of it). It emits
  # two lines — the transform summary and the counts — so both are surfaced.
  if counts="$(generate_manifest "$leg" "$manifest")"; then
    while IFS= read -r _cl; do [[ -z "$_cl" ]] || info "[$leg] manifest: $_cl"; done <<< "$counts"
    info "[$leg] manifest → $manifest"
  else
    printf '%s\n' "$counts" | sed 's/^/      /' >&2
    die "[$leg] manifest generation FAILED ($MANIFEST_GEN) — see above.
      The generator also asserts that every TU EXISTS on disk, so a staged-tree miss
      lands here rather than mid-compile."
  fi
  # A project build routes each target to <output>/<formatName>/<artifactName>.
  # dss-code-prime returns EXIT 0 even on fatal errors → judge from `error[` + the binary.
  "$DSS_BIN" --project "$manifest" --config="$DSS_CONFIG" --output "$outd" --time >"$log" 2>&1 || true
  bin="$outd/$fmt/testfixture"
  if grep -qE 'error\[' "$log" || [[ ! -x "$bin" ]]; then
    COMPILE_FAILS=$((COMPILE_FAILS + 1))
    # `poisoned` — the ledger's FAILURE class, and it DISPLACES whatever this leg
    # was carrying: a build that produced no artifact is the strongest thing that
    # can be said about it, and a failure must never read as a skip.
    LEG_VERDICT["$leg"]="poisoned"
    LEG_VERDICT_DETAIL["$leg"]="the fixture did not build for ${LEG_SPEC[$leg]} — see $log"
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
    # HOST fact, and the comment above says why in detail: AppleSystemPolicy pins
    # the exec DENY to an inode on THIS MACHINE's filesystem. It is about the box
    # that will exec the file, not about which target produced it — every leg built
    # on a Mac goes through it.
    if [[ "$HOST_OS" == "darwin" ]]; then
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
# How a shared object is produced — DECLARED by the leg (`build.sharedLibFlags` in
# legs.json), not pattern-matched off its format name here. This used to be a
# `case "${LEG_SPEC[$1]##*:}" in macho64-*)` in this file, i.e. a second, private
# opinion about object formats sitting a long way from the catalogue that already
# has one; a new format would have had to be taught to both.
leg_shared_flags() { printf '%s' "${LEG_SHARED_FLAGS[$1]}"; }
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
# FAIL-LOUD: the compiler was RESOLVED at Step 6, from the leg's own declared
# `targetCc.candidates`, and a leg that could not resolve one never reaches this
# function (it carries `skipped-build-input-missing` and its corpus is not run). So
# by the time we are here the compiler is known to exist, and the only failure left
# is the COMPILE ITSELF — which dies, loudly. What must never happen is a fallback
# to the host compiler: that is precisely the defect above, and it is invisible in
# the results.
stage_loadext_extension() {    # stage_loadext_extension <leg> <rundir>
  local leg="$1" rundir="$2"
  local cc="${LEG_CC[$leg]}"
  local src="$SQLITE_DIR/src/test_loadext.c"
  local dst="$rundir/$SQLITE_TESTDIR_SUBDIR/libtestloadext.so"
  [[ -f "$src" ]] || die "[$leg] sqlite extension source not found: $src"
  command -v "$cc" >/dev/null 2>&1 || die \
"[$leg] target C compiler '$cc' resolved at Step 6 is no longer on PATH — it builds this leg's libtestloadext.so, the extension the loadext corpus dlopen()s.
      NOT falling back to the host compiler: sqlite's loadext.test would then build a HOST-arch extension the $leg fixture cannot load, and every
      loadext-* test would false-red as a genuine DSS failure [D-HARNESS-ARM64-LEG-HOST-ARCH-HELPER-SO]."
  mkdir -p "$(dirname "$dst")"
  # leg_shared_flags is a deliberate word-split flag list.
  "$cc" $(leg_shared_flags "$leg") -I"$SQLITE_DIR/src" -I"$BLD" -o "$dst" "$src" \
    || die "[$leg] could not build the loadext helper extension: $cc $(leg_shared_flags "$leg") -o $dst $src"
  info "[$leg] loadext helper -> $dst (built by $cc)"
}
for leg in "${LEG_ORDER[@]}"; do
  # ── the three ways a leg does not reach the corpus, each already NAMED ──────
  # None of them is a host test: they are the recorded OUTCOMES of Step 6 and
  # Step 7. The verdict is left exactly as those steps set it — this loop never
  # invents one, and never silently drops a leg.
  if [[ -z "${LEG_TCL_LIB[$leg]:-}" ]]; then
    UNIT_VERDICT["$leg"]="not run [${LEG_VERDICT[$leg]}] — ${LEG_VERDICT_DETAIL[$leg]}"
    continue                                   # already warned at Step 6/7
  fi
  if [[ "${COMPILE_OK[$leg]:-0}" != "1" ]]; then
    UNIT_VERDICT["$leg"]="not run [${LEG_VERDICT[$leg]}] — step 7 did not produce a fixture"
    warn "[$leg] corpus skipped — step 7 did not compile the fixture"; continue
  fi
  if [[ "${LEG_RUN_MODE[$leg]}" == "skip" ]]; then
    # ★ BUILT, and the build result is REPORTED — this is the whole point of the
    # split. The artifact for this target exists and is on disk; this machine
    # simply cannot execute it, which the resolver said up front and by name.
    UNIT_VERDICT["$leg"]="not run [${LEG_VERDICT[$leg]}] — ${LEG_VERDICT_DETAIL[$leg]}  (the fixture DID build: ${FIXTURE[$leg]})"
    info "[$leg] fixture built but NOT RUN here [${LEG_RUN_VERDICT[$leg]}]: ${LEG_RUN_DETAIL[$leg]}"
    continue
  fi
  if [[ -z "${LEG_CC[$leg]:-}" ]]; then
    UNIT_VERDICT["$leg"]="not run [${LEG_VERDICT[$leg]}] — ${LEG_VERDICT_DETAIL[$leg]}  (the fixture DID build: ${FIXTURE[$leg]})"
    continue                                   # already warned at Step 6
  fi
  bin="${FIXTURE[$leg]}"; rundir="$OUT_DIR/$leg/run"; rm -rf "$rundir"; mkdir -p "$rundir"
  # The launcher's DECLARED path namespace, and this leg's fixture spelled in it.
  # Translated ONCE per leg; the only other translation site is a segment's FIRST
  # argument (the .test script), below. `$bin` itself keeps this driver's spelling
  # because the process sweep and every log line address the file the way we do.
  leg_xlate="${LEG_PATH_TRANSLATION[$leg]:-none}"
  launch_bin="$(launch_path "$leg_xlate" "$bin")"
  if [[ "$leg_xlate" != "none" ]]; then
    info "[$leg] the launcher addresses files in ANOTHER namespace (pathTranslation '$leg_xlate' via '${LEG_PATH_TRANSLATOR[$leg]}') — fixture $bin -> $launch_bin"
  fi
  # ── the launcher's ENVIRONMENT namespace ───────────────────────────────────
  # WHICH variables may cross is THIS DRIVER's knowledge — it is the one that
  # sets them — and the rule is NAMESPACE-NEUTRAL VALUES ONLY: PATH and
  # TCL_LIBRARY are deliberately absent because both hold HOST paths. HOW they
  # cross belongs to the verb. Empty on every POSIX host, where the child simply
  # inherits, so this leaves the .sh's long-standing behaviour untouched.
  # Only the carrier's NAME and its prior value are per-LEG; WHICH variables are
  # actually carried is decided per SEGMENT, in the loop below, from what is set
  # at that moment (see launch_env_carrier for why that distinction is the
  # difference between a run and a false green).
  leg_env_verb="${LEG_ENV_TRANSFER[$leg]:-inherit}"
  declare -a LEG_ENV_NAMES=()
  mapfile -t LEG_ENV_NAMES < <(printf '%s\n' ${LEG_LAUNCH_ENV[$leg]} | sed -n 's/^\([A-Za-z_][A-Za-z0-9_]*\)=.*/\1/p')
  declare -a LEG_ENV_FORWARD=(SQLITE_TEST_PATTERN_LIST QUICKTEST_OMIT
                              ${LEG_ENV_NAMES[@]+"${LEG_ENV_NAMES[@]}"})
  leg_carrier_name="$(launch_env_carrier_name "$leg_env_verb")"
  leg_carrier_old=""
  if [[ -n "$leg_carrier_name" ]]; then
    leg_carrier_old="${!leg_carrier_name:-}"
    info "[$leg] the launcher does NOT inherit this driver's environment (envTransfer '$leg_env_verb') — variables that are SET at spawn time cross via $leg_carrier_name; candidates: ${LEG_ENV_FORWARD[*]}"
  fi
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
  # ★ THE INVARIANT THIS QUEUE RESTS ON, spelled once: `arg1` is ALWAYS the .test
  # SCRIPT the fixture sources (its Tcl `$argv0`) and is therefore always a PATH,
  # so it is stored ALREADY TRANSLATED into the launcher's namespace; `arg2` is a
  # bare Tcl word (a permutation name) or a tester.tcl flag (`--start=<perm>:`)
  # and is never a path. run_fixture_segment's assertion is what catches a future
  # segment kind that breaks the invariant.
  # ★ THE TRANSLATED PATHS ARE COMPUTED INTO SIMPLE ASSIGNMENTS FIRST, never
  # embedded as `$(launch_path …)` inside the array element. `launch_path` fails
  # by calling `die`, and `die` inside a command substitution exits the SUBSHELL:
  # in `SEGQ=("…$(launch_path …)…")` that leaves an EMPTY field which `set -e`
  # does not see, so a refusal would become a silently-relative script path — the
  # exact "a failing substitution leaves an empty field" trap this driver already
  # records for its leg plan. A simple `v="$(…)"` DOES propagate the status.
  LAUNCH_TIER_SCRIPT="$(launch_path "$leg_xlate" "$TEST_FILE")"
  LAUNCH_PERM_SCRIPT="$(launch_path "$leg_xlate" "$TESTDIR_SRC/permutations.test")"
  US=$'\x1f'
  SEGQ=("tier${US}${US}$DSS_TIER.test${US}${US}${LAUNCH_TIER_SCRIPT}${US}")
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
      info "[$leg] running $DSS_TIER.test$( [[ -n "${LEG_LAUNCH[$leg]}" ]] && printf ' (under the declared launcher: %s%s)' "${LEG_LAUNCH[$leg]}" "$( [[ "$leg_xlate" != "none" ]] && printf ', paths -> %s' "$leg_xlate" )" )…"
    else
      seglog="$OUT_DIR/$leg/corpus.resume$seg_i.log"
      info "[$leg] segment $((seg_i + 1)): $s_label$( [[ -n "$s_patfile" ]] && printf '  (SQLITE_TEST_PATTERN_LIST: %s candidate file(s))' "$(wc -l < "$s_patfile")" )"
    fi
    declare -a seg_argv=("$s_arg1"); [[ -n "$s_arg2" ]] && seg_argv+=("$s_arg2")
    # SQLITE_TEST_PATTERN_LIST is a Tcl LIST of globs; corpus basenames are bare
    # words, so a whitespace join is a valid list.
    if [[ -n "$s_patfile" ]]; then SQLITE_TEST_PATTERN_LIST="$(tr '\n' ' ' < "$s_patfile")"; export SQLITE_TEST_PATTERN_LIST
    else unset SQLITE_TEST_PATTERN_LIST; fi
    # LAST, because it reads the variables set just above: the launcher's declared
    # environment TRANSFER, resolved PER SEGMENT from what is actually SET right
    # now. Empty on every host whose launcher inherits, which is all of them here.
    # ★ A SIMPLE ASSIGNMENT, then a split — NOT `mapfile < <(launch_env_carrier …)`.
    # The helper fails by calling `die`, and `die` inside a PROCESS SUBSTITUTION
    # exits only that subshell: mapfile would read zero lines, `set -e` would see
    # nothing, and the segment would run with an un-forwarded environment — the
    # very silence this mechanism exists to end. Same trap, same remedy, as the
    # translated segment paths above.
    declare -a LEG_ENV_CARRIER=()
    leg_env_carrier_out="$(launch_env_carrier "$leg_env_verb" "$leg_carrier_old" \
                             ${LEG_ENV_FORWARD[@]+"${LEG_ENV_FORWARD[@]}"})"
    if [[ -n "$leg_env_carrier_out" ]]; then
      mapfile -t LEG_ENV_CARRIER <<< "$leg_env_carrier_out"
      export "${LEG_ENV_CARRIER[@]}"
    elif [[ -n "$leg_carrier_name" ]]; then
      # Nothing to carry THIS segment: the carrier must not keep a previous
      # segment's list, or an unset variable is manufactured as empty.
      if [[ -n "$leg_carrier_old" ]]; then export "$leg_carrier_name=$leg_carrier_old"
      else unset "$leg_carrier_name"; fi
    fi
    run_fixture_segment "$leg" "$bin" "$launch_bin" "$seglog" "${seg_argv[@]}"
    segrc="$SEG_RC"
    unset SQLITE_TEST_PATTERN_LIST
    if [[ -n "$leg_carrier_name" ]]; then
      if [[ -n "$leg_carrier_old" ]]; then export "$leg_carrier_name=$leg_carrier_old"
      else unset "$leg_carrier_name"; fi
    fi
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
    declare -a TAIL_SEGS=("perm${US}${perm}${US}permutations.test $perm (after $boundary)${US}${patfile}${US}${LAUNCH_PERM_SCRIPT}${US}${perm}")
    # (b) the tier continued from the NEXT permutation — the ORIGINAL tier script,
    # so every ifcapable/platform guard is evaluated by sqlite exactly as always.
    if [[ "$s_kind" != "perm" ]]; then
      if [[ $perm_idx -lt 0 ]]; then
        warn "[$leg] permutation '$perm' is not named by ${TEST_FILE##*/} — cannot continue the tier past it."
        NOT_REACHED+=("every permutation after '$perm' in ${TEST_FILE##*/} — '$perm' is not one of its run_test_suite entries")
      elif [[ $perm_idx -lt $((${#TIER_PERMS[@]} - 1)) ]]; then
        nextperm="${TIER_PERMS[$((perm_idx + 1))]}"
        TAIL_SEGS+=("tier${US}${nextperm}${US}${TEST_FILE##*/} --start=${nextperm}:${US}${US}${LAUNCH_TIER_SCRIPT}${US}--start=${nextperm}:")
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
  # (bare `<re>` = every leg). The mode is the RESOLVER'S OWN run mode for this leg
  # — `native` (this host executes the artifact directly) vs `launched` (it goes
  # through a declared launcher: qemu for a cross-arch host, Wine for a cross-OS
  # one). It used to be inferred from "is LEG_PREFIX non-empty", which was the same
  # fact read out of a variable this driver maintained by hand. Still never a host
  # or arch identity test, so a future launched target inherits the scoping free.
  # The SCOPE NAMES stay `native:`/`emulated:` — they are the DSS_CONFOUNDS
  # vocabulary an operator types, and renaming them would silently un-excuse every
  # `emulated:` pattern in the shipped default list.
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
  [[ "${LEG_RUN_MODE[$leg]:-native}" == 'launched' ]] && leg_mode='emulated'
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
  elif [[ "$files_done" -eq 0 ]]; then
    # ★★ A RUN THAT COMPLETED ZERO TEST FILES IS NOT GREEN, WHATEVER ITS SUMMARY
    # LINE SAYS. Twin of the same branch in build-and-test.ps1, added with it.
    # MEASURED 2026-08-04 (TF-C116) on the .ps1 side: a tier that selected no
    # files still made tester.tcl finalise and print `0 errors out of 1 tests`,
    # and the driver reported "corpus GREEN" beside "0 test file(s) completed".
    # The floor is structural — a summary line is not proof that a suite ran.
    UNIT_VERDICT["$leg"]="FAIL:the fixture completed ZERO test files yet printed a summary ($summary) — a suite that ran nothing is not a pass; see $runlog"
    UNIT_FAILS=$((UNIT_FAILS + 1))
    warn "[$leg] corpus FAIL — 0 test file(s) completed, though the fixture printed '$summary'."
    info "      A tier that selects no files still finalises and reports a summary. That is not a run."
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
  # ★ THE LEDGER VERDICT for a leg that reached the corpus: `ran` — the ledger's
  # VERIFIED class, "this arm produced an assertion". It is `ran` whether the
  # corpus came back GREEN or RED: a failing assertion is still an assertion, and
  # the redness is carried by $UNIT_FAILS and $UNIT_VERDICT — exactly the split the
  # C++ corpus runners make (a gtest failure does not turn an arm's verdict from
  # Ran into something else). `poisoned` stays reserved for "no artifact, never
  # spawned", so a reader can always tell "we tested it and it failed" apart from
  # "we never tested it".
  LEG_VERDICT["$leg"]="ran"
  LEG_VERDICT_DETAIL["$leg"]="${UNIT_VERDICT[$leg]:-<no unit verdict recorded>}"
  unset real confound ABORTS ABORT_ROWS NOT_REACHED HYGIENE CALIBRATION SEG_LOGS SEG_LABELS SEG_RCS SEG_COUNTS TIER_PERMS TIER_PREFIXES
done

# ── Step 9 — results ─────────────────────────────────────────────────────────
step "9/9  Results"
# PROVENANCE — the one line later cycles quote as "the compiler that ran this".
# D-HARNESS-SH-SRC-DIR-GIT-REQUIRED-VS-RSYNC-GATE, items (3) and (5):
#   · the sha is the string Step 2 already read and VERIFIED, not a fresh rev-parse.
#     A command substitution that fails inside printf's ARGUMENTS does not trip
#     `set -e`; the old form printed an EMPTY field, which reads as "clean". Reusing
#     Step 2's value also makes it impossible for the banner and the verdict to
#     disagree about the same run.
#   · $SRC_DIVERGE_NOTE states how far the sources that were BUILT sit from that
#     commit, so a stale-.git run (or an uncommitted-work probe) labels itself
#     instead of quietly asserting a hash it cannot stand behind.
# sqlite gets no divergence count on purpose: Steps 3-6 run configure/make INSIDE
# that clone and generate sources there, so its working tree always differs from
# HEAD by construction and a number would carry no information.
printf '   compiler : %s @ %s%s\n' "$DSS_BIN" "$SRC_HEAD" "$SRC_DIVERGE_NOTE"
printf '   sqlite   : %s @ %s\n' "$SQLITE_DIR" "$(git_head_short "$SQLITE_DIR")"
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
for leg in "${LEG_DECLARED[@]}"; do
  spec="${LEG_SPEC[$leg]}"
  if [[ "${COMPILE_OK[$leg]:-0}" == "1" ]]; then
    # Only printed when there is something to say: a clean single-segment run
    # leaves this block byte-identical to what it always was.
    if [[ "${LEG_SEGMENTS[$leg]:-1}" -gt 1 || -n "${LEG_ABORTS[$leg]:-}" || -n "${LEG_NOTREACHED[$leg]:-}" || -n "${LEG_HYGIENE[$leg]:-}" ]]; then
      printf '   %-14s segments : %s (%s resume(s) of max %s)   %s test file(s) completed   ledger: %s\n' \
        "$leg" "${LEG_SEGMENTS[$leg]}" "${LEG_RESUMES[$leg]}" "$DSS_MAX_RESUMES" "${LEG_FILESDONE[$leg]}" "${LEG_LEDGER[$leg]}"
      for a in ${LEG_ABORTS[$leg]:-}; do
        printf '   %-14s aborted  : %s — its remaining cases did NOT run\n' "$leg" "$a"
      done
      while IFS= read -r n; do [[ -z "$n" ]] || printf '   %-14s NOT RUN  : %s\n' "$leg" "$n"; done <<< "${LEG_NOTREACHED[$leg]:-}"
      while IFS= read -r h; do [[ -z "$h" ]] || printf '   %-14s hygiene  : %s\n' "$leg" "$h"; done <<< "${LEG_HYGIENE[$leg]:-}"
    fi
    # $EXCL_NOTE rides along on EVERY verdict — pass and fail alike — so a GREEN
    # line can never be read as "the whole corpus ran".
    printf '   %-14s (%s): %scompiled%s   units: %s%s\n' "$leg" "$spec" "$C_GRN" "$C_RST" "${UNIT_VERDICT[$leg]:--}" "$EXCL_NOTE"
  elif [[ "${LEG_VERDICT[$leg]:-}" == "poisoned" ]]; then
    printf '   %-14s (%s): %sCOMPILE FAILED%s   see %s/%s/compile.log\n' "$leg" "$spec" "$C_RED" "$C_RST" "$OUT_DIR" "$leg"
  else
    # ★ NOT BUILT — and it says WHY, by name. This line is the difference between
    # the old driver and this one: a leg that this host could not build or run used
    # to be ABSENT from the leg list entirely, so the summary said nothing at all
    # about it and a reader had no way to know it was ever declared.
    printf '   %-14s (%s): %sNOT BUILT%s [%s] — %s\n' "$leg" "$spec" "$C_YLW" "$C_RST" \
      "${LEG_VERDICT[$leg]:-<NO VERDICT>}" "${LEG_VERDICT_DETAIL[$leg]:-<no reason recorded>}"
  fi
done

# ─────────────────────────────────────────────────────────────────────────────
# THE VERDICT LEDGER — every DECLARED leg, one named verdict, counts that SUM
# ─────────────────────────────────────────────────────────────────────────────
# Same shape and the same words as `ArmVerdictLedger::renderCountsLine()` in
# tests/test_support/arm_verdict_ledger.hpp, deliberately: the sqlite harness does
# not get private words for "did not run", and a reader greps ONE vocabulary across
# this driver and the two examples-corpus runners.
#
# WHY EVERY CLASS IS NAMED even when its count is 0 (this harness never produces
# `expect-error-asserted`, and only DSS_LEGS produces `not-selected-by-runner`): a
# bare "N skipped" re-creates exactly the conflation the ledger exists to end, and
# a class that appears only when non-zero cannot be grepped for reliably.
declare -A VERDICT_COUNT=()
declare -a LEDGER_VOCAB=(ran expect-error-asserted skipped-by-runOn
                         skipped-no-emulator-declared skipped-emulator-missing
                         skipped-build-input-missing not-selected-by-runner poisoned)
for v in "${LEDGER_VOCAB[@]}"; do VERDICT_COUNT["$v"]=0; done
declare -a LEDGER_UNNAMED=() LEDGER_BOGUS=()
for leg in "${LEG_DECLARED[@]}"; do
  v="${LEG_VERDICT[$leg]:-}"
  if [[ -z "$v" ]]; then LEDGER_UNNAMED+=("$leg"); continue; fi
  if [[ -z "${VERDICT_COUNT[$v]+set}" ]]; then LEDGER_BOGUS+=("$leg=$v"); continue; fi
  # ★ `${VERDICT_COUNT[...]}` — a full parameter expansion INSIDE the arithmetic,
  # never a bare `VERDICT_COUNT[key]`. MEASURED (bash 5.2.37): while the array is
  # ASSOCIATIVE both forms are correct — bash takes an associative subscript as a
  # literal string, so the hyphens in `expect-error-asserted` are safe. But that
  # correctness rests entirely on the `declare -A` above still being in force: on
  # an INDEXED array the same bare subscript is evaluated as an EXPRESSION and dies
  # `expect: unbound variable` under `set -u`. Expanding first hands the evaluator a
  # plain integer and makes the reading independent of a declaration 20 lines away.
  VERDICT_COUNT["$v"]=$(( ${VERDICT_COUNT[$v]} + 1 ))
done
LEDGER_VERIFIED=$(( ${VERDICT_COUNT[ran]} + ${VERDICT_COUNT[expect-error-asserted]} ))
LEDGER_STRUCTURAL=$(( ${VERDICT_COUNT[skipped-by-runOn]} + ${VERDICT_COUNT[skipped-no-emulator-declared]} ))
LEDGER_ENVIRONMENTAL=$(( ${VERDICT_COUNT[skipped-emulator-missing]} + ${VERDICT_COUNT[skipped-build-input-missing]} ))
LEDGER_HARNESS=$(( ${VERDICT_COUNT[not-selected-by-runner]} ))
LEDGER_SKIPPED=$(( LEDGER_STRUCTURAL + LEDGER_ENVIRONMENTAL + LEDGER_HARNESS ))
LEDGER_FAILED=$(( ${VERDICT_COUNT[poisoned]} ))
LEDGER_ACCOUNTED=$(( LEDGER_VERIFIED + LEDGER_SKIPPED + LEDGER_FAILED ))
LEDGER_TOTAL=${#LEG_DECLARED[@]}
printf '   verdicts : %d verified (%d ran, %d expect-error), %d skipped [structural: %d by-runOn, %d no-emulator-declared; environmental: %d emulator-missing, %d build-input-missing; harness: %d not-selected], %d poisoned  (of %d declared legs)\n' \
  "$LEDGER_VERIFIED" "${VERDICT_COUNT[ran]}" "${VERDICT_COUNT[expect-error-asserted]}" \
  "$LEDGER_SKIPPED" "${VERDICT_COUNT[skipped-by-runOn]}" "${VERDICT_COUNT[skipped-no-emulator-declared]}" \
  "${VERDICT_COUNT[skipped-emulator-missing]}" "${VERDICT_COUNT[skipped-build-input-missing]}" \
  "${VERDICT_COUNT[not-selected-by-runner]}" "$LEDGER_FAILED" "$LEDGER_TOTAL"
# FAIL LOUD IN THE SUMMARY LINE ITSELF. A breakdown that does not sum to its own
# denominator is worse than no breakdown: it READS like full accounting. Same
# precedent, same words, as renderCountsLine()'s hole detector — and here it is
# also FATAL, because a leg with no verdict is precisely the silent shortfall this
# whole cycle removed, and a run that ends 0 while one leg vanished would restore it.
LEDGER_HOLE=0
if [[ "$LEDGER_ACCOUNTED" -ne "$LEDGER_TOTAL" ]]; then
  LEDGER_HOLE=1
  printf '   %s★ LEDGER ACCOUNTING HOLE: %d of %d declared legs fall in a reported class — the rest belong to NO class and have VANISHED from the line above%s\n' \
    "$C_RED" "$LEDGER_ACCOUNTED" "$LEDGER_TOTAL" "$C_RST"
  [[ ${#LEDGER_UNNAMED[@]} -eq 0 ]] || printf '     with NO verdict at all : %s\n' "${LEDGER_UNNAMED[*]}"
  [[ ${#LEDGER_BOGUS[@]}   -eq 0 ]] || printf '     with a verdict OUTSIDE the closed vocabulary: %s\n' "${LEDGER_BOGUS[*]}"
  printf '     the closed vocabulary is: %s  (tests/test_support/arm_verdict_ledger.hpp)\n' "${LEDGER_VOCAB[*]}"
fi
# One line per NON-verified leg, with its reason. Verified legs are omitted: they
# are already fully reported by the per-leg block above.
for leg in "${LEG_DECLARED[@]}"; do
  v="${LEG_VERDICT[$leg]:-<NO VERDICT>}"
  case "$v" in ran|expect-error-asserted) continue ;; esac
  printf '   [%s] %s spec=%s — %s\n' "$v" "$leg" "${LEG_SPEC[$leg]}" "${LEG_VERDICT_DETAIL[$leg]:-<no reason recorded>}"
done
# ENVIRONMENTAL skips: a loud warning by default, FATAL under
# DSS_STRICT_ARM_VERDICTS=1 — the same variable and the same semantics as the two
# examples-corpus runners. STRUCTURAL skips are never fatal and are not listed here:
# nothing about this machine can change them.
declare -a ENV_SKIPS=()
for leg in "${LEG_DECLARED[@]}"; do
  case "${LEG_VERDICT[$leg]:-}" in
    skipped-emulator-missing|skipped-build-input-missing) ENV_SKIPS+=("$leg") ;;
  esac
done
if [[ ${#ENV_SKIPS[@]} -gt 0 ]]; then
  if [[ "$STRICT_VERDICTS" -eq 1 ]]; then
    printf '\n   %s✗ %d ENVIRONMENTAL skip(s) and DSS_STRICT_ARM_VERDICTS=1 — this run is RED: %s%s\n' \
      "$C_RED" "${#ENV_SKIPS[@]}" "${ENV_SKIPS[*]}" "$C_RST"
    printf '     Each one is a DECLARED input this machine could not supply (a launcher, a\n'
    printf '     resolve-library binary, a target compiler) — not a compiler defect, and not a\n'
    printf '     property of the catalogue. Supply them, or drop DSS_STRICT_ARM_VERDICTS.\n'
  else
    warn "${#ENV_SKIPS[@]} leg(s) were skipped for an ENVIRONMENTAL reason — this machine could not supply a DECLARED input: ${ENV_SKIPS[*]}"
    warn "      Those targets are NOT covered by this run. Set DSS_STRICT_ARM_VERDICTS=1 to make it a hard failure."
  fi
fi
# Release the run lock. Correctness does NOT depend on this — the lock is
# liveness-based, so a run that dies here just leaves one the next invocation steals
# and reports. Releasing simply keeps that report quiet when it should be.
rm -rf "$LOCK_DIR"
if [[ "$LEDGER_HOLE" -eq 1 ]]; then
  printf '\n%sTHE LEDGER DOES NOT ADD UP — a declared leg reached no named verdict.%s\n' "$C_RED" "$C_RST"
  printf 'That is a HARNESS defect, not a compiler result: whatever this run proved, it did\n'
  printf 'not prove it about every leg it claimed to cover. Refusing to exit 0.\n'
  exit 1
fi
if [[ "$COMPILE_FAILS" -gt 0 ]]; then
  printf '\n%s%d leg(s) failed to compile the testfixture — inspect the compile.log diagnostics.%s\n' "$C_RED" "$COMPILE_FAILS" "$C_RST"
  exit 1
fi
if [[ "$UNIT_FAILS" -gt 0 ]]; then
  printf '\n%s%d leg(s) had genuine unit failures (non-confound) — the corpus is not green.%s\n' "$C_RED" "$UNIT_FAILS" "$C_RST"
  exit 1
fi
if [[ ${#ENV_SKIPS[@]} -gt 0 && "$STRICT_VERDICTS" -eq 1 ]]; then exit 1; fi
# ★ THE CLOSING CLAIM IS BOUNDED BY THE LEDGER. It used to read "every leg
# compiled ... GREEN", which on the old driver meant "every leg this host happened
# to produce" — a sentence that got smaller as the host got less capable, without
# ever saying so. It now names what was verified out of what was DECLARED.
pass "$LEDGER_VERIFIED of $LEDGER_TOTAL declared leg(s) VERIFIED: compiled the full-source testfixture + ran the $DSS_TIER unit corpus GREEN — SQLite units pass with dss-code-prime.  ($LEDGER_SKIPPED skipped: $LEDGER_STRUCTURAL structural, $LEDGER_ENVIRONMENTAL environmental, $LEDGER_HARNESS harness — each named above; 0 poisoned)"
