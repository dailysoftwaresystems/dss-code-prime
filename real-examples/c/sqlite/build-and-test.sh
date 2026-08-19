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
#      (`host-system` | `ubuntu-ports-arm64` | `search-paths` | `pinned-archive`).
#      A leg whose pair cannot be resolved on this machine records
#      `skipped-build-input-missing` NAMING what was searched — the run continues,
#      and the other legs are unaffected. `pinned-archive` is the GENERAL form:
#      the leg declares digest-pinned third-party archives and the members to take,
#      harness_legs.py `--acquire` fetches/verifies/extracts/slices them once for
#      both drivers, and Step 7 stages the result BESIDE the artefact because such
#      a library is a stand-in whose declared runtime identity is `@loader_path/…`.
#   7. build the full-source `testfixture` with dss-code-prime, once PER LEG,
#      from a generated `.dss-project.json` manifest (dss --project mode). Every
#      declared leg is attempted, on every host. Each leg's manifest declares
#      c-subset / cli / the leg's <targetName>:<formatName> target / the ~185 TUs
#      (absolute `sources`) / the sqlite+tcl+zlib include dirs / the recipe defines
#      (transformed per the leg's declared `recipeTransform`) / the leg's own
#      resolveLibraries / its declared `stackReserve`; the build routes the binary
#      to <out>/<leg>/<formatName>/, and the COMPILER says what it named it there
#      (`dss-code-prime: artifact <spec> <path>` — see `dss_bh_reported_artifact`; the
#      suffix is the object format's business, not this driver's). ONE manifest
#      generator (gen-pe64-manifest.py) serves this driver and the .ps1.
#   8. stage each leg's run dir — including the loadext extension the corpus
#      dlopen()s (`libtestloadext.so`, or `testloadext.dll` on a Windows target:
#      the name is DECLARED per leg), EMITTED BY DSS ITSELF for the leg's declared
#      `sharedLibFormat`, so no host needs a cross-compiler for any leg; where a
#      VERIFIED target compiler happens to exist it also builds a CONTROL copy
#      beside it, and DSS_LOADEXT_HELPER=reference stages that one instead —
#      then run
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
#                      DSS_ALLOW_NONRELEASE_COMPILER
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
CLI_SMOKE="$SCRIPT_DIR/cli-smoke.py"
# The deferred-anchor registry, consulted AT the point a leg fails
# (D-PROCESS-CHECK-THE-REGISTRY-FOR-A-MATCHED-CONTROL-BEFORE-COMMISSIONING-ONE).
# NOT required to exist: a checkout without .plans still runs, it just gets one
# line saying the lookup found nothing to read.
ANCHOR_REGISTRY="$SCRIPT_DIR/../../../.plans/_deferred-anchor-registry.md"
# ── THE SHARED CORE ──────────────────────────────────────────────────────────
# base-harness.sh holds the recipe derivation, the artifact read-back and the
# per-(leg, artifact) verdict ledger — the decisions this driver and
# build-and-test.ps1 BOTH make, which used to be written out once per driver and
# had already drifted three measured ways (its header names them). The .ps1
# reaches the same file: its recipe derivation has always run in a POSIX shell,
# and that shell now sources this.
#
# Sourced HERE, at the top, and NOT guarded by an `if`: a driver whose shared
# core is missing must not limp on with a private copy of half of it. That is
# how the drift started.
BASE_HARNESS="$SCRIPT_DIR/base-harness.sh"
[[ -r "$BASE_HARNESS" ]] || {
  printf ' ✗ ERROR: the shared harness core is missing: %s\n' "$BASE_HARNESS" >&2
  printf '      It carries the recipe derivation, the artifact read-back and the verdict\n' >&2
  printf '      ledger that this driver and build-and-test.ps1 SHARE. Restore it rather\n' >&2
  printf '      than reintroducing a private copy — three hand-kept copies of one decision\n' >&2
  printf '      is what it was extracted to end.\n' >&2
  exit 1
}
# shellcheck source=base-harness.sh
. "$BASE_HARNESS"
# The contract version this driver was written against. A stale core would
# otherwise present itself as a MISSING CAPABILITY — silently skipping the CLI,
# say — which is the failure class the shared core exists to make impossible.
[[ "${DSS_BASE_HARNESS_VERSION:-0}" -ge 1 ]] || {
  printf ' ✗ ERROR: %s is version %s; this driver needs >= 1.\n' \
         "$BASE_HARNESS" "${DSS_BASE_HARNESS_VERSION:-<unset>}" >&2
  exit 1
}
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
# failures — a failing test matching any is not counted against green.
#
# ★★★ THE DEFAULT SET IS NOT IN THIS FILE ANY MORE, AND THAT IS THE WHOLE FIX.
# [D-HARNESS-CONFOUND-LEDGER-IS-PER-DRIVER-NOT-PER-LEG,
#  D-HARNESS-SQLITE-CONFOUNDS-NOT-DECLARED-PER-LEG,
#  D-SQLITE-CONFOUND-LIST-DRIVER-ASYMMETRY.]
#
# What stood here was ONE global default list applied to EVERY leg this driver
# runs — the two elf legs, the two macho legs on a Mac, pe64 under Wine — while
# build-and-test.ps1 returned a six-pattern list for `pe64-x86_64` and an EMPTY
# one for everything else. Two drivers, two ledgers, neither keyed on the thing
# that actually decides the answer. ✔MEASURED consequence: the SAME elf64-x86_64
# artefact's `zipfile-25.0` was a "known non-DSS confound" under this driver and
# a "genuine failure" under the .ps1, in the same project on the same day. The
# genuine-failure count is what every verdict here rests on, so no two legs' were
# comparable.
#
# ⇒ THE EARNED SET IS DECLARED PER LEG, in legs.json `confounds`, and each
# pattern carries the leg + host + date + mechanism that earned it plus the
# anchor holding the long form. The prose that used to live in this comment block
# — the walsetlk/busy2/recoverfault matched controls, the zipfile symlink leak,
# the macOS dyld string, the qemu abort artefact — moved there WITH its pattern,
# which is the point: evidence beside the claim it supports, in the one file both
# drivers read. harness_legs.py's lint REFUSES a pattern with no provenance, a
# duplicate, one that does not compile as a regex, and a leg that omits the key
# entirely — `[]` ("nothing has ever been earned here; every failure counts") is
# a claim a catalogue has to make out loud.
#
# ⓘ WHAT THIS VARIABLE STILL DOES, unchanged: an operator OVERRIDE, which
# deliberately applies to EVERY leg — naming a pattern on the command line is
# stating intent for this run, not inheriting one — and which is announced as
# such, per leg, so a reader of the log can never mistake it for the earned set.
# Its grammar is unchanged too: bare `<re>` = however the leg runs, and the
# `native:<re>` / `emulated:<re>` scope prefixes, which the catalogue spells as a
# `scope` FIELD and harness_legs.py renders back into this same wire form.
#
# ⛔ THE 55 DrvFs FAILURES ARE NOT CANDIDATES FOR THIS LIST. A launched leg whose
# run directory lands on a filesystem that only approximates POSIX semantics
# fails ~60 units under a GCC reference too — but the mechanism is OURS and it is
# fixed, by declaration, in legs.json `launchers[].runFilesystem`. A confound row
# would have laundered a harness misconfiguration into "expected", using the very
# mechanism this list exists to keep honest. D-HARNESS-WSL-LAUNCHED-LEG-RUNDIR-IS-DRVFS.
DSS_CONFOUNDS="${DSS_CONFOUNDS:-}"
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
# DSS_ALLOW_NONRELEASE_COMPILER: proceed with a dss-code-prime whose own tree does
# NOT say Release, instead of refusing at Step 5. It silences nothing — the run
# then states the actual build type on the Step-5 banner and again in the Step-9
# verdict, so a log from such a run cannot later be quoted as a controlled
# measurement. PARITY with build-and-test.ps1, where the same variable governs the
# same gate; see Step 5 for the defect that put a gate there at all.
# Same three-state parse and the same Step-1 validation as the variable above.
DSS_ALLOW_NONRELEASE_COMPILER="${DSS_ALLOW_NONRELEASE_COMPILER:-}"

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
           LEG_RUN_MODE=() LEG_RUN_FIDELITY=() LEG_RUN_SAME_ISA=() \
           LEG_RUN_VERDICT=() LEG_RUN_DETAIL=() \
           LEG_LAUNCH=() LEG_LAUNCH_ENV=() \
           LEG_PATH_TRANSLATION=() LEG_PATH_TRANSLATOR=() LEG_ENV_TRANSFER=() \
           LEG_RUN_FILESYSTEM=() LEG_CONFOUNDS=() LEG_ABORT_CONFOUNDS=() \
           LEG_RUN_LAUNCH=() \
           LEG_CONFOUND_GATING=() LEG_CONFOUND_REPORT=() \
           LEG_CONFOUND_DECLARED=() \
           LEG_RECIPE_TRANSFORM=() LEG_HEADER_STAGE_KEY=() LEG_ZCONF_GUARDS=() \
           LEG_CONFIG_STAGE_KEY=() LEG_CONFIGURE_ANSWERS=() \
           LEG_STACK_RESERVE=() LEG_SHARED_FLAGS=() LEG_LOADEXT_NAME=() \
           LEG_SHARED_LIB_FORMAT=() LEG_SHARED_LIB_SPEC=() \
           LEG_CC_CANDIDATES=() LEG_CC_PKG=() \
           LEG_LIB_PROVIDER=() LEG_LIB_TCL_NAMES=() LEG_LIB_Z_NAMES=() LEG_LIB_PATHS=() \
           LEG_LIB_TCL_IMPORT_NAME=() LEG_LIB_Z_IMPORT_NAME=()
# Resolved by this driver: the leg's chosen target compiler, its (tcl, z) pair, and
# its VERDICT — one name from the closed vocabulary in
# tests/test_support/arm_verdict_ledger.hpp, with a reason. Empty verdict = "still
# in flight"; Step 9 refuses to let any declared leg end that way.
declare -A LEG_CC=() LEG_CC_MACHINE=() LEG_TCL_LIB=() LEG_Z_LIB=() LEG_VERDICT=() LEG_VERDICT_DETAIL=()
# EVERY leg that resolved a libtcl, INCLUDING one whose zlib did not — the Tcl
# header/library coherence check (Step 6) is about a version skew, and a leg that
# found its Tcl can witness one whether or not it is buildable. LEG_TCL_LIB above
# is deliberately narrower: it is the FIXTURE's precondition and needs both.
declare -A LEG_TCL_LIB_ANY=()
# For a leg whose libraries were ACQUIRED (`pinned-archive`): where they landed,
# and the "<as>\t<path>" lines the resolver reported. Kept because the artefact is
# not finished when the link is — an acquired library is a STAND-IN whose declared
# runtime identity is `@loader_path/<name>`, so a copy of it has to be staged
# BESIDE the binary at Step 7 or the artefact fails at load time on the target
# machine. Empty for every other provider.
declare -A LEG_ACQ_DIR=() LEG_ACQ_LIBS=()
# ── THE RUNTIME DATA an acquired library needs, which its CODE does not carry ──
# [D-HARNESS-ACQUIRED-TCL-DYLIB-HAS-NO-SCRIPT-LIBRARY]
#
# ✔MEASURED on the operator's Mac at 11e97e0e: the macho64-arm64 fixture BUILT
# (189 TUs, zero diagnostics) and ran `select1.test` correctly — "0 errors out of
# 192 tests", no leaks — and then the TIER driver died instantly with
#     Can't find a usable init.tcl in the following directories: /opt/local/lib/tcl8.6 …
#     (procedure "tclInit" line 61)  invoked from within "interp create tinterp"
# because `permutations.test` runs every unit in a FRESH SLAVE INTERPRETER, and
# `interp create` re-enters `tclInit`, which needs Tcl's SCRIPT LIBRARY. The
# acquired MacPorts `libtcl8.6.dylib` bakes in `/opt/local/lib/tcl8.6` — MacPorts'
# own prefix — and only the dylib was ever downloaded.
#
# ★ THE GENERAL LESSON: acquisition obtains a library's CODE but not the RUNTIME
# DATA that library requires. Nothing at build time can see it — the link
# succeeds, the binary runs, and it dies only on the code path that touches the
# data. ICU and tzdata are the next two.
#
# This driver's half is to CONSUME what acquisition staged and point the leg's run
# environment at it. WHICH directory that is, and staging it, belong to the shared
# resolver — see $ACQ_SCRIPT_LIBRARY_KEY below for the exact contract read.
declare -A LEG_TCL_SCRIPT_DIR=()
# THE CONTRACT FIELD, NAMED ONCE — ✔READ FROM harness_legs.py, NOT GUESSED.
# `acquisition_record()` returns a TOP-LEVEL `scriptLibraryDir`, derived by
# `_script_library_dir()` from the ONE `dataDirs` entry whose `role` is
# `tclScriptLibrary` (`DATA_DIR_ROLES`, singled out there precisely because "a
# driver has to point TCL_LIBRARY at it"). It is on the SUCCESS and FAILURE
# returns alike — one record type, so "a field cannot be forgotten on the branch
# nobody exercises". This driver reads the resolved field and never re-derives it
# from `dataDirs`, so a rename is a one-line change HERE and in the .ps1.
ACQ_SCRIPT_LIBRARY_KEY="scriptLibraryDir"
# LEG_DECLARED = every leg the catalogue declares (the ledger's denominator).
# LEG_ORDER    = the subset this invocation actually processes (DSS_LEGS filter).
declare -a LEG_ORDER=() LEG_DECLARED=()
# Legs whose corpus ran but left a DECLARED capability's witness file inert.
# Run-wide rather than per-leg: the capability set is a property of the one
# staged tree every leg compiles, so a gap on any leg indicts the stage.
declare -a CAPABILITY_GAPS=()

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
  local raw="${#lines[@]}"
  # D-HARNESS-DIVERGENCE-COUNT-INFLATED-BY-CRLF: the raw count OVERSTATED divergence
  # by ~40x on a Windows→Linux synced tree (✔MEASURED on the VPS: "2420 file(s)
  # differ" against a true modified set of ~60; of the first 60, 24 were CR-only).
  # A Windows working tree carries CRLF and no autocrlf normalization applies on the
  # Linux side, so `status --porcelain` reports every synced file as modified.
  # ★ WHY IT IS NOT COSMETIC: this warning exists to stop someone attributing a
  # result to a commit it was not built from. A figure wrong by 40x trains the reader
  # to ignore it, which disables the warning exactly when it is telling the truth.
  # ⛔ NOT fixed by lowering the severity — the severity is right, the arithmetic was
  # wrong. TRACKED files are re-counted ignoring a CR at end-of-line; UNTRACKED files
  # are counted as-is, because a source file a stale HEAD never knew about is real
  # divergence and is precisely what the rsync gate leaves behind.
  # ⚠ The porcelain listing stays the ENUMERATION. Only the TRACKED-modification
  # lines are re-tested; untracked entries are passed through exactly as `status`
  # reported them. Rebuilding the whole count from `diff` + `ls-files` was tried and
  # REJECTED: `status` reports an untracked DIRECTORY as one entry while `ls-files`
  # expands it to every file inside, so that version changed the figure on trees with
  # no CRLF at all (✔MEASURED on this repo: 210 → 214). A fix for a Windows→Linux
  # artefact must be INERT on every other tree, or it is a second defect.
  local semantic_tracked=""
  semantic_tracked="$(git -C "$1" --no-optional-locks diff --ignore-cr-at-eol --name-only HEAD 2>/dev/null)" \
    || semantic_tracked="$(git -C "$1" diff --ignore-cr-at-eol --name-only HEAD 2>/dev/null)" \
    || { printf '%d' "$raw"; return 0; }
  local semantic=0 line="" path="" xy=""
  for line in "${lines[@]}"; do
    [[ -n "$line" ]] || continue
    xy="${line:0:2}"
    path="${line:3}"
    if [[ "$xy" == '??' ]]; then
      semantic=$(( semantic + 1 ))          # untracked: real divergence, always counted
      continue
    fi
    # A rename reads `R  old -> new`; the destination is what a CR test can match.
    [[ "$path" != *' -> '* ]] || path="${path##* -> }"
    path="${path%\"}"; path="${path#\"}"
    if printf '%s\n' "$semantic_tracked" | grep -qxF -- "$path"; then
      semantic=$(( semantic + 1 ))
    fi
  done
  # ⚠ The excluded count is written to a FILE, not to a global: every caller invokes
  # this inside `$( )`, which is a SUBSHELL, so an assignment here would be discarded
  # and the note would silently read zero — a fix that reports "0 excluded" while
  # excluding 2360 is worse than the inflated count it replaced. The path is optional;
  # a caller that does not set it simply gets no side-channel.
  if [[ -n "${GIT_TREE_DIVERGENCE_CRONLY_FILE:-}" ]]; then
    printf '%d' "$(( raw > semantic ? raw - semantic : 0 ))" \
      > "$GIT_TREE_DIVERGENCE_CRONLY_FILE" 2>/dev/null || true
  fi
  printf '%d' "$semantic"
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
GIT_TREE_DIVERGENCE_CRONLY=0
read_src_provenance() {         # read_src_provenance <dir>
  SRC_HEAD="$(git_head_short "$1")"
  SRC_HEAD_LONG="$(git -C "$1" rev-parse HEAD 2>/dev/null)" || SRC_HEAD_LONG=""
  SRC_BRANCH="$(git_head_branch "$1")"
  GIT_TREE_DIVERGENCE_CRONLY=0
  GIT_TREE_DIVERGENCE_CRONLY_FILE="$(mktemp 2>/dev/null || printf '')"
  SRC_DIVERGE="$(git_tree_divergence "$1")"
  if [[ -n "$GIT_TREE_DIVERGENCE_CRONLY_FILE" && -s "$GIT_TREE_DIVERGENCE_CRONLY_FILE" ]]; then
    GIT_TREE_DIVERGENCE_CRONLY="$(cat "$GIT_TREE_DIVERGENCE_CRONLY_FILE" 2>/dev/null || printf '0')"
  fi
  [[ -z "$GIT_TREE_DIVERGENCE_CRONLY_FILE" ]] || rm -f "$GIT_TREE_DIVERGENCE_CRONLY_FILE"
  GIT_TREE_DIVERGENCE_CRONLY_FILE=""
  local cr_note=""
  # Stated explicitly, per the row: a reader comparing this against a raw
  # `git status` must be told why the two disagree, or the smaller number reads as
  # a bug in the harness rather than as the more honest figure.
  [[ "${GIT_TREE_DIVERGENCE_CRONLY:-0}" == "0" ]] \
    || cr_note="; $GIT_TREE_DIVERGENCE_CRONLY CR-only difference(s) excluded as non-semantic"
  if [[ -z "$SRC_DIVERGE" ]]; then
    SRC_DIVERGE_NOTE=" (divergence from HEAD UNVERIFIED — git status failed in $1)"
  elif [[ "$SRC_DIVERGE" != "0" ]]; then
    SRC_DIVERGE_NOTE=" (+$SRC_DIVERGE file(s) differ from HEAD — the sources built are NOT exactly this commit$cr_note)"
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
# ★ A LIST, NOT A SINGLE FILE. Each entry EXTRACTS shipped logic and executes it,
# and each emits the same `passed=N failed=N skipped=N` summary parsed below — so
# adding a fourth costs one array entry instead of a second copy of this careful
# rc/skip/refuse block. A self-test that exists but is never RUN is documentation,
# which is how a guarded behaviour comes to look guarded without being tested.
#   test-confound-scope.sh    the end-of-run confound classifier + the Step-2 gate
#   test-driver-contracts.sh  the LEG CONTRACTS — the not-run recorder, the shared
#                             run decision, the Step-8 gate sequence, parse_segment,
#                             the precondition discriminator (driven over real
#                             ZERO-BYTE segment logs, so "one resume, not ten" is
#                             asserted rather than described — D-HARNESS-
#                             PRECONDITION-DISCRIMINATOR-BLIND-TO-A-SILENT-CRASH),
#                             acq_field and the target-keyed loader variable, each
#                             with its red-on-disable mutation asserted to have
#                             LANDED.
#   test-mirror-regions.sh    the `dss:` REGIONS — every region declared with who
#                             verifies it (a claimed verifier that does not read
#                             the region is a LOUD failure), and for a region
#                             declared MIRRORED the symbol pairing plus
#                             DIFFERENTIAL EXECUTION of both drivers' copies on
#                             byte-identical input. It found a live divergence on
#                             its first complete run: the .ps1 recorded only the
#                             MATCHED SUBSTRING of sqlite's summary line where
#                             this driver records the whole line.
#                             D-HARNESS-CORPUS-ENGINE-MIRROR-CLAIMS-A-VERIFIER-THAT-DOES-NOT-EXIST
declare -a _SELFTESTS=("$SCRIPT_DIR/test-confound-scope.sh" "$SCRIPT_DIR/test-driver-contracts.sh"
                       "$SCRIPT_DIR/test-mirror-regions.sh")
for _selftest in "${_SELFTESTS[@]}"; do
if [[ "${DSS_SKIP_SELFTEST:-0}" == "1" ]]; then
  warn "driver self-test ${_selftest##*/} SKIPPED (DSS_SKIP_SELFTEST=1) — a late-stage defect will not surface until the end of the run."
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
    # blocks (26 of 64 assertions — the provenance helpers and the whole Step-2
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
      info "driver self-test ${_selftest##*/}: OK ($_st_pass assertions, 0 skipped)"
    else
      # ★★ THE SELF-TEST'S OWN WORDS, NEVER THIS DRIVER'S GUESS AT THEM.
      # ANCHOR, ONE LINE, DO NOT WRAP:
      # D-HARNESS-SELFTEST-SKIP-REPORTED-UNDER-ANOTHER-SUITE-S-REASON
      # This branch used to append "(an unmet prerequisite, normally 'no git on
      # PATH' at Step 0)". That is true of test-confound-scope.sh and FALSE of
      # test-mirror-regions.sh, whose skips mean "this host has no pwsh, so the
      # .ps1 arm of every mirrored region went unexecuted and twin parity is
      # REDUCED" — a materially different loss, reported for two months under a
      # sibling suite's cause. ✔MEASURED 2026-08-11 on macOS and on the arm64
      # VPS, identically: `OK (275 assertions) — but 9 assertion(s) SKIPPED`,
      # with git present on both. A driver that narrates a reason it did not
      # measure is the same defect class as an oracle line printed for a leg
      # that has none. Each self-test now states its OWN reason and this driver
      # RELAYS it: the `SKIP`/`skip` lines the suites already emit, plus any
      # classification block a suite prints ahead of its summary.
      warn "driver self-test ${_selftest##*/}: OK ($_st_pass assertions) — but $_st_skip assertion(s) SKIPPED on this host."
      # BSD-portable sed: one `-e` per expression, no `;`-chained scripts. The
      # range stops AT the summary line and the summary is then dropped, so this
      # relays the explanation without reprinting the count above it.
      printf '%s\n' "$_st_out" \
        | sed -n -e '/^[[:space:]]*[Ss][Kk][Ii][Pp][[:space:]]/p' \
                 -e '/COVERAGE REDUCED/,/^passed=/p' \
        | sed -e '/^passed=/d' -e 's/^/      /' >&2
      warn "      Those assertions are UNPROVEN for this run. Re-run the suite by hand on a host
      that has the missing prerequisite if you want the full battery: $_selftest"
    fi
  else
    printf '%s\n' "$_st_out" | sed 's/^/      /' >&2
    die "DRIVER SELF-TEST FAILED ($_selftest) — refusing to start.
      Late-stage driver logic is broken, so this run would execute the whole corpus
      (hours) and then abort while classifying — or would classify a leg's outcome
      wrongly and report it. Fix the driver first; the output above names the
      failing assertion."
  fi
fi
done

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
# ★★ AND THE THIRD QUESTION, WHICH THE FIRST TWO DO NOT ASK: does the VALUE still
# mean the same thing once it has crossed? For a variable holding a path it does
# not [D-HARNESS-PS1-TCL-LIBRARY-NOT-FORWARDED-ACROSS-THE-WSL-BOUNDARY]. So the
# names arrive here in THREE declared groups, and the resolver REFUSES any name
# it has no declared kind for — a path-valued variable added to the plain group
# by a later edit is a LOUD refusal, not a raw forward.
#   $3 = the count of NAMESPACE-NEUTRAL names, then that many names
#   then the count of DRIVER-PATH names, then that many names
#   then the remaining args: names the CATALOGUE declared for this launcher
launch_env_carrier() {         # launch_env_carrier <envverb> <pathverb> <current> <nplain> <plain...> <npath> <path...> <declared...>
  local verb="$1" pathverb="$2" current="$3"; shift 3
  [[ -n "$verb" && "$verb" != "inherit" ]] || return 0
  local -a call=(--env-transfer "$verb" --path-translation "${pathverb:-none}"
                 --carrier-current "$current")
  local n i nplain npath carried=0
  nplain="$1"; shift
  for ((i = 0; i < nplain; i++)); do
    n="$1"; shift
    [[ -n "$n" && -n "${!n:-}" ]] || continue   # THE FILTER — see the note above
    call+=(--forward "$n"); carried=1
  done
  npath="$1"; shift
  for ((i = 0; i < npath; i++)); do
    n="$1"; shift
    [[ -n "$n" && -n "${!n:-}" ]] || continue   # THE FILTER — same rule
    # ONE token with the `=` form: a path may contain spaces, and the resolver
    # splits on the FIRST `=` only.
    call+=("--forward-path=$n=${!n}"); carried=1
  done
  for n in "$@"; do
    [[ -n "$n" && -n "${!n:-}" ]] || continue   # THE FILTER — same rule
    call+=(--forward-declared "$n"); carried=1
  done
  # Nothing SET means nothing to carry, and carrying nothing is the correct
  # answer — not an empty carrier that manufactures empty variables.
  [[ "$carried" == 1 ]] || return 0
  local out rc
  if out="$(python3 "$LEG_RESOLVER" --catalogue "$LEG_CATALOGUE" "${call[@]}" 2>&1)"; then rc=0; else rc=$?; fi
  [[ $rc -eq 0 ]] || die "could not resolve the launcher's environment transfer (envTransfer '$verb', rc=$rc):
      ${out:-<no diagnostic>}
      Without it the launched fixture runs with an EMPTY run environment, which does not
      fail — it silently changes what the corpus does."
  printf '%s\n' "$out"
}
# ── THE REGISTRY, AT THE POINT OF FAILURE ────────────────────────────────────
# D-PROCESS-CHECK-THE-REGISTRY-FOR-A-MATCHED-CONTROL-BEFORE-COMMISSIONING-ONE.
#
# ✔MEASURED (TF-C123): a 2x2 attribution was commissioned from scratch for 57
# unit failures whose IDENTICAL experiment and IDENTICAL verdict were already in
# the registry from seven cycles earlier, and the un-cited row let three false
# statements reach a commit. The row was findable; LOOKING is the part you have
# to remember. So the harness looks, here, and prints what it found beside the
# failure it just reported.
#
# ⚠ FAIL-SOFT BY CONSTRUCTION, because this runs on a failure path and must never
# become one: the resolver mode always exits 0, and this function swallows even
# that. A run that already failed must not also lose its report.
# ⚠ A POINTER, NEVER A VERDICT — a matched row means someone has looked at
# something with this name before, not that this failure is explained.
registry_controls_for() {      # registry_controls_for <leg> <failing test name...>
  local leg="$1"; shift
  local -a call=(--registry-controls "$ANCHOR_REGISTRY" --for-leg "$leg") t
  local n=0
  for t in "$@"; do
    [[ -n "$t" ]] || continue
    # Bounded: a leg can fail with hundreds of names and this is a pointer, not
    # a search engine. The resolver says how many rows it held back, so a
    # truncated lookup never reads as an exhaustive one.
    [[ $n -lt 12 ]] || break
    call+=(--for-test "$t"); n=$((n + 1))
  done
  local out
  # `|| true` deliberately: python3 absent, unreadable registry, anything —
  # none of it may add a failure to a leg that has already failed.
  out="$(python3 "$LEG_RESOLVER" "${call[@]}" 2>&1 || true)"
  [[ -n "$out" ]] || return 0
  info "      ── registry rows naming this leg / these tests (a POINTER, not a verdict — read before commissioning an experiment):"
  printf '%s\n' "$out" | sed 's/^/      /'
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

# ── THE LAUNCHER'S FILESYSTEM ────────────────────────────────────────────────
# D-HARNESS-WSL-LAUNCHED-LEG-RUNDIR-IS-DRVFS. Twin of Get-LegRunDirPlan /
# Invoke-RunDirArgv in build-and-test.ps1, capability-paired for the same reason
# the path-namespace pair above is.
#
# THE THIRD NAMESPACE. The two above make a launched leg's argv and environment
# correct; neither says a word about the FILESYSTEM the launched fixture writes
# its databases onto — and this corpus is a database engine's, so that is the
# property it tests hardest. ✔MEASURED on the .ps1 twin's Windows host: /mnt/c is
# mounted 9p/drvfs with NO `metadata` option, so `chmod 644` reads back as 777 and
# `chmod 400` as 555 — the entire POSIX mode synthesised from ONE Windows
# attribute; /tmp (ext4) answers 644 and 400. A 2x2 matched control ({DSS, gcc
# reference} x {DrvFs, ext4}) reproduced all 60 failures under GCC on DrvFs and
# made every one VANISH on ext4. ⛔ They are NOT confounds: the mechanism is ours
# and it is fixed here, by declaration, not excused.
#
# ★ THIS DRIVER SPELLS NO MECHANISM — no `--cd`, no `/tmp`, no `mkdir`, no `cp`.
# It reads the leg's DECLARED verb and asks the resolver, exactly as launch_path
# asks it about `wslpath`.
leg_run_dir_plan() {           # leg_run_dir_plan <leg> <driver-rundir>  -> JSON
  local leg="$1" driver_run_dir="$2" out rc
  # rc DIRECTLY off python3, never after a pipe.
  if out="$(python3 "$LEG_RESOLVER" --catalogue "$LEG_CATALOGUE" \
              --run-dir-plan "$leg" --host-os "$HOST_OS" --host-arch "$HOST_ARCH" \
              --driver-run-dir "$driver_run_dir" --format json 2>&1)"; then rc=0; else rc=$?; fi
  [[ $rc -eq 0 && -n "${out//[[:space:]]/}" ]] || die "[$leg] could not resolve this leg's RUN DIRECTORY (harness_legs.py --run-dir-plan, rc=$rc):
      ${out:-<no diagnostic>}
      Which filesystem a launched leg runs on is DECLARED (legs.json \`launchers[].runFilesystem\`),
      never assumed — and the assumption is what put a Linux sqlite corpus onto DrvFs."
  printf '%s\n' "$out"
}
# One field out of that JSON. A LIST field (the argv prefixes, the launcher argv)
# comes back shlex-quoted and space-joined so the caller `eval`s it into an array
# — the same transport `emit_sh` uses for LEG_LAUNCH, and for the same reason: a
# word with a space in it must survive.
run_dir_field() {              # run_dir_field <json> <key>  -> stdout
  local json="$1" key="$2" out rc
  if out="$(printf '%s' "$json" | python3 -c '
import json, shlex, sys
blob = json.load(sys.stdin)
key = sys.argv[1]
if key not in blob:
    sys.stderr.write("no such run-dir-plan field: %s (have: %s)\n"
                     % (key, ", ".join(sorted(blob))))
    raise SystemExit(3)
v = blob[key]
sys.stdout.write(" ".join(shlex.quote(x) for x in v) if isinstance(v, list) else str(v))
' "$key" 2>&1)"; then rc=0; else rc=$?; fi
  [[ $rc -eq 0 ]] || die "the run-dir plan does not carry the field this driver reads ('$key', rc=$rc):
      ${out:-<no diagnostic>}
      That is a contract break between harness_legs.py and this driver, not a property of this host."
  printf '%s' "$out"
}
# Run one of the resolver's argv PREFIXES. An EMPTY prefix means the launcher
# shares this driver's filesystem and the caller does it natively — that is what
# `runFilesystem: driver` MEANS, so empty is a real answer and not a missing one,
# and this returns 0 for it.
#
# ★ IT RETURNS A VERDICT, IT DOES NOT `die` — deliberately, not defensively. This
# driver attempts five legs; a run directory that could not be prepared costs THAT
# leg its corpus and must not delete four other legs' worth of evidence. It is
# the same rule stage_loadext_extension learnt the expensive way on 2026-08-05,
# when a `die` in a staging function ended a run in which two legs had already
# reported green over 331,351 and 331,355 units.
# ⛔ AND THERE IS NO FALLBACK TO THIS DRIVER'S OWN DIRECTORY. Falling back would
# put the corpus straight back onto the filesystem the declaration exists to keep
# it off, silently, which is worse than not running the leg.
RUN_DIR_WHY=""
run_dir_argv() {               # run_dir_argv <leg> <what> <quoted-prefix> <arg...>  -> 0 | 1
  local leg="$1" what="$2" prefix="$3"; shift 3
  RUN_DIR_WHY=""
  [[ -n "${prefix//[[:space:]]/}" ]] || return 0
  local -a argv=()
  eval "argv=($prefix)"
  argv+=("$@")
  local out rc
  if out="$("${argv[@]}" 2>&1)"; then rc=0; else rc=$?; fi
  [[ $rc -eq 0 ]] && return 0
  RUN_DIR_WHY="could not $what in the launcher's own filesystem — \`${argv[*]}\` exited $rc: ${out:-<no diagnostic>}"
  return 1
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

# ── THE SINGLE RUN-DECISION, SHARED BY BOTH ARTIFACTS ────────────────────────
# The sqlite3 CLI smoke gate (Step 7c) and the unit corpus (Step 8) ask the SAME
# question — "may this host EXECUTE this leg?" — and the answer is `run.mode` off
# the RESOLVED plan, never `if [[ $HOST_OS ]]`. It is a FUNCTION, and defined HERE
# beside the plan it reads, so the two call sites cannot drift into two different
# answers: that drift is what D-HARNESS-UNITS-SKIP-A-LEG-WHOSE-LAUNCHER-IT-SAYS-IS-
# AVAILABLE is about, and the fix its registry row asked for is one resolver, both
# call sites.
#
# ⚠ NOTE FOR THE NEXT READER, because the obvious reading of that row is WRONG and
# cost this cycle its first hour: the two call sites had ALREADY agreed on this
# question — both tested `run.mode == skip` and nothing else. What actually
# differed is that Step 8 carried a SECOND, unrelated gate (an absent CONTROL
# compiler) which Step 7c never had. See the note where that gate used to be.
leg_run_is_skipped() {         # leg_run_is_skipped <leg>  -> 0 when NOT runnable here
  # ⚠ TWO `local` STATEMENTS, NOT ONE. ✔MEASURED (bash 5.x): `local a="$1"
  # b="${M[$a]:-}"` expands EVERY right-hand side BEFORE performing ANY of the
  # assignments, so the second one reads an unset `a` and dies `a: unbound
  # variable` under `set -u`. The same trap `corpus_files` above documents — and
  # it was caught here by an executable pin, not by reading.
  local _leg="$1"
  local _mode="${LEG_RUN_MODE[$_leg]:-}"
  case "$_mode" in
    skip)   return 0 ;;
    native) return 1 ;;
    launched)
      # A `launched` leg with no launcher argv is the contradiction this anchor is
      # named after wearing its other face: the plan says "runnable" and hands the
      # driver nothing to run it with. The resolver already refuses an EMPTY
      # declared command, so this asserts the invariant SURVIVED transport through
      # `--format sh` + `eval` rather than trusting that it did.
      [[ -n "${LEG_LAUNCH[$_leg]:-}" ]] || die "[$_leg] the resolved plan says run mode 'launched' but carries an EMPTY launcher argv.
      A leg cannot be both runnable and unrunnable. That is a transport defect between
      $(basename "$LEG_RESOLVER") and this driver, not a property of this machine."
      return 1 ;;
    *) die "[$_leg] has an unknown run mode '${_mode:-<empty>}' — the resolver and this driver disagree about the vocabulary." ;;
  esac
}

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

# >>> dss:run-fidelity-select >>>
# DSS_RUN_FIDELITY: restrict this run to legs whose artefact executes at a given
# FIDELITY. [D-HARNESS-RUN-FIDELITY-IS-COMPUTED-BUT-NEITHER-RECORDED-NOR-SELECTABLE]
#
# ★ WHY THIS IS NOT DSS_LEGS WITH EXTRA STEPS. `DSS_LEGS=elf64-arm64` names a
# LEG; the operator then has to know, per host, whether that leg reaches real
# hardware here — which is exactly the host-keyed reasoning this harness exists to
# remove from operators' heads. `DSS_RUN_FIDELITY=native,foreign-kernel` names the
# EVIDENCE wanted and lets the resolver work out which legs supply it on THIS box,
# so the same command means the same thing on the Mac, the VPS and this desktop.
#
# ★★ IT SELECTS THE RUN, NEVER THE BUILD. "CAN THIS HOST BUILD TARGET T" is always
# yes and is not a host question (this module's header); only execution is
# host-limited. A deselected leg is therefore still BUILT and still reported — it
# is ledgered `not-selected-by-runner`, the same class DSS_LEGS uses, with a detail
# naming the fidelity it actually has. Silence about a leg is a harness bug.
#
# ⚠ VALIDATED HERE, AT THE DOOR, not at first use: a typo'd fidelity would
# otherwise match nothing and silently run zero legs, which reads exactly like a
# host that can execute nothing.
if [[ -n "${DSS_RUN_FIDELITY:-}" ]]; then
  _fid_known="$(python3 "$LEG_RESOLVER" --run-fidelities)" \
    || die "could not read the run-fidelity vocabulary from $(basename "$LEG_RESOLVER")"
  declare -a _fid_want=() _fid_keep=() _fid_drop=()
  # Split on comma OR whitespace, exactly as DSS_LEGS-style filters are typed.
  IFS=', ' read -r -a _fid_want <<< "${DSS_RUN_FIDELITY}"
  for _f in "${_fid_want[@]}"; do
    [[ -z "$_f" ]] && continue
    grep -qx -- "$_f" <<< "$_fid_known" \
      || die "DSS_RUN_FIDELITY names '$_f', which is not a run fidelity this harness declares.
      Known: $(tr '\n' ' ' <<< "$_fid_known")
      A value nothing matches would silently select ZERO legs, which reads exactly
      like a host that can execute nothing [D-HARNESS-RUN-FIDELITY-IS-COMPUTED-BUT-NEITHER-RECORDED-NOR-SELECTABLE]."
  done
  for _l in "${LEG_ORDER[@]}"; do
    _lf="${LEG_RUN_FIDELITY[$_l]:-}"
    if [[ -n "$_lf" ]] && grep -qx -- "$_lf" <<< "$(printf '%s\n' "${_fid_want[@]}")"; then
      _fid_keep+=("$_l")
    else
      _fid_drop+=("$_l")
    fi
  done
  [[ ${#_fid_keep[@]} -gt 0 ]] || \
    die "DSS_RUN_FIDELITY='${DSS_RUN_FIDELITY}' selected NO leg on this host.
      Per-leg fidelity here: $(for _l in "${LEG_ORDER[@]}"; do printf '%s=%s ' "$_l" "${LEG_RUN_FIDELITY[$_l]:-<never runs>}"; done)
      A run that covers nothing must stop, not report a clean sweep of zero legs."
  LEG_ORDER=("${_fid_keep[@]}")
  if [[ ${#_fid_drop[@]} -gt 0 ]]; then
    warn "DSS_RUN_FIDELITY='${DSS_RUN_FIDELITY}' DESELECTED ${#_fid_drop[@]} leg(s) — this run does NOT cover them:"
    for _l in "${_fid_drop[@]}"; do
      LEG_VERDICT["$_l"]="not-selected-by-runner"
      LEG_VERDICT_DETAIL["$_l"]="deselected by DSS_RUN_FIDELITY='${DSS_RUN_FIDELITY}' — this leg's run fidelity on this host is '${LEG_RUN_FIDELITY[$_l]:-<never runs here>}', so ${LEG_SPEC[$_l]} was NOT built and NOT verified by this run"
      warn "      $_l (${LEG_SPEC[$_l]}) — fidelity '${LEG_RUN_FIDELITY[$_l]:-<never runs here>}', not built, not verified"
    done
  fi
fi
# <<< dss:run-fidelity-select <<<
info "legs selected: ${LEG_ORDER[*]}   tier: $DSS_TIER"
for _l in "${LEG_ORDER[@]}"; do
  info "   $_l: run mode '${LEG_RUN_MODE[$_l]:-<unset>}', fidelity '${LEG_RUN_FIDELITY[$_l]:-<never runs here>}'"
done

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

# Same three states, same refusal of a value that is neither, and validated here
# rather than at Step 5 — Step 5 sits AFTER the sqlite clone + configure + stage,
# so a typo found there costs the staging run instead of a second.
ALLOW_NONRELEASE_DSS=0
case "${DSS_ALLOW_NONRELEASE_COMPILER}" in
  1|true|TRUE|yes)        ALLOW_NONRELEASE_DSS=1 ;;
  ''|0|false|FALSE|no)    ALLOW_NONRELEASE_DSS=0 ;;
  *) die "DSS_ALLOW_NONRELEASE_COMPILER='${DSS_ALLOW_NONRELEASE_COMPILER}' is not a value this harness recognises.
      Accepted: 1 / true / TRUE / yes  (allow)  ·  0 / false / FALSE / no / empty (default: REFUSE).
      Refused rather than read as 'off' even though 'off' is the SAFE direction here — a flag
      whose typo means something is a flag nobody can read back out of a log. Same rule, same
      spellings, as DSS_STRICT_ARM_VERDICTS directly above." ;;
esac

# ── WHAT EACH LAUNCHER NEEDS BEYOND ITS OWN argv[0] ─────────────────────────
# >>> dss:launcher-prereq >>>
# ★★ THE PLAN SAYS `launched` BECAUSE argv[0] RESOLVED, AND THAT IS A MUCH WEAKER
# FACT THAN IT READS AS. For the arm64 leg on a Windows host argv[0] is `wsl.exe`
# — present on every machine with WSL — while the program that actually executes
# the artefact is `qemu-aarch64` INSIDE the distro, which `shutil.which('wsl.exe')`
# has never asked about on any host. ✔MEASURED: that leg passed every gate this
# harness had on a box with no qemu, every unit exited 255 with NO diagnostic, and
# fourteen of them were charged to DSS — the harness accusing the compiler of a
# defect in the machine it was running on.
#
# `--check-launcher` EXECUTES the leg's DECLARED prerequisite rows (legs.json
# `launchers[].requires`) in the LAUNCHER's own namespace and answers rc 0 met /
# 3 unmet / 2 catalogue defect. This driver only classifies the answer; it never
# decides what a launcher needs and never probes anything itself.
#
# ★ THE OUTCOME IS `skipped-launcher-prerequisite-missing`, the closed
# vocabulary's ENVIRONMENTAL sibling of `skipped-emulator-missing` — announced by
# default, FATAL under DSS_STRICT_ARM_VERDICTS=1 through the SAME Step-9 ENV_SKIPS
# list, and STILL BUILT. This gate is only ever about EXECUTION on this machine.
#
# ⓘ WHY HERE. It runs at Step 1, before Steps 7b/7c/8, so an operator learns in
# the first minute rather than after an hour of compiling — and so the CLI smoke
# gate and the unit corpus, which both ask `leg_run_is_skipped`, get the answer
# from one place. `--artifact` (the 4-D PT_INTERP/DT_NEEDED cross-check) is NOT
# passed: nothing is built yet, and asking for it here would mean either lying
# about the artefact or moving the check to where it is too late to be cheap.
#
# ⚠ `launcher_prereq_rows` is a SEPARATE function on purpose: the report must be
# printed with its `provides`, `why` AND `install`. A diagnostic without the
# remedy is one nobody acts on — this project has a standing example in the
# QEMU_LD_PREFIX note, which lived for months as an operational workaround rather
# than as a checked prerequisite with an apt line beside it.
launcher_prereq_rows() {       # launcher_prereq_rows <json>  -> formatted lines
  # THE ROWS THE CATALOGUE DECLARED, not a summary of them. `provides` says what
  # the missing thing is FOR, `why` says on what evidence it is declared, and
  # `install` is the one line the operator actually needs.
  printf '%s' "$1" | python3 -c '
import json, sys
# ★★ THE REPORT MUST SURVIVE ITS OWN CHARACTERS. ✔MEASURED 2026-08-08 against a
# REAL --check-launcher report: legs.json writes its evidence with a ✔ in it,
# harness_legs.py escapes it (json.dumps is ensure_ascii), json.loads turns it
# back into U+2714 — and sys.stdout then encodes with the LOCALE codec, which on
# a cp1252 host raises UnicodeEncodeError MID-REPORT. Two rows printed, the rest
# lost, and the caller`s `|| true` swallowed the traceback: a remedy list silently
# truncated to whatever fitted the codepage. An environment default nobody
# declared, which is this project`s recurring instrument failure. `backslashreplace`
# keeps the platform encoding and makes NO character able to end the report.
try:
    sys.stdout.reconfigure(errors="backslashreplace")
except (AttributeError, ValueError):
    pass
raw = sys.stdin.read()
try:
    rep = json.loads(raw)
except ValueError as exc:
    # SAID OUT LOUD rather than swallowed: a report this driver cannot read is a
    # contract break with harness_legs.py, not an empty list of missing rows.
    sys.stdout.write("the --check-launcher report is not JSON (%s): %s\n"
                     % (exc, raw[:400]))
    raise SystemExit(0)
for row in rep.get("missing", []):
    sys.stdout.write("MISSING [%s] %s\n" % (row.get("kind", "?"), row.get("path", "?")))
    sys.stdout.write("      provides: %s\n" % (row.get("provides") or "<not declared>",))
    sys.stdout.write("      why     : %s\n" % (row.get("why") or "<not declared>",))
    sys.stdout.write("      install : %s\n" % (row.get("install") or "<not declared>",))
    probe = row.get("probe") or []
    if probe:
        sys.stdout.write("      probed  : %s\n" % (" ".join(probe),))
for u in rep.get("uncovered", []):
    sys.stdout.write("UNCOVERED %s\n" % (u,))
'
}
LAUNCHER_PREREQ_JSON=""
LAUNCHER_PREREQ_ERR=""
apply_launcher_prereq_gate() { # apply_launcher_prereq_gate <leg>
                               #   -> 0 met (or not launched) | 1 unmet | 2 unreadable
  local leg="$1" errf out rc rows n _row
  LAUNCHER_PREREQ_JSON=""; LAUNCHER_PREREQ_ERR=""
  # A leg this host runs NATIVELY has no launcher, and a leg the plan already
  # skips has already been named. Neither is this gate's business.
  [[ "${LEG_RUN_MODE[$leg]:-}" == "launched" ]] || return 0
  errf="$(mktemp)" || die "could not create a temp file for the launcher-prerequisite check's stderr."
  # rc DIRECTLY off python3, never after a pipe, and stderr to its own file so a
  # diagnostic can never be parsed as the report.
  if out="$(python3 "$LEG_RESOLVER" --catalogue "$LEG_CATALOGUE" \
              --check-launcher "$leg" --host-os "$HOST_OS" --host-arch "$HOST_ARCH" 2>"$errf")"; then
    rc=0
  else
    rc=$?
  fi
  LAUNCHER_PREREQ_JSON="$out"
  LAUNCHER_PREREQ_ERR="$(cat "$errf" 2>/dev/null || true)"; rm -f "$errf"
  case "$rc" in
    0) info "[$leg] launcher '${LEG_LAUNCH[$leg]:-<none declared>}': every DECLARED prerequisite is present on this machine"
       return 0 ;;
    3) rows="$(launcher_prereq_rows "$LAUNCHER_PREREQ_JSON" 2>&1 || true)"
       n="$(printf '%s\n' "$rows" | grep -c '^MISSING ' || true)"
       warn "[$leg] LAUNCHER PREREQUISITE MISSING — this host HAS '${LEG_LAUNCH[$leg]:-<none declared>}', and does NOT have everything that launcher DECLARES it needs."
       while IFS= read -r _row; do [[ -z "$_row" ]] || warn "      $_row"; done <<< "$rows"
       warn "      This leg is STILL BUILT. Its sqlite3 CLI smoke gate and its ENTIRE unit corpus are NOT run on this machine —"
       warn "      running them would exercise a launcher that cannot start the artefact, and every failure would be charged to the compiler."
       # DOWNGRADED TO `skip`, with the answer the machine gave. The plan resolved
       # `launched` from a fact that turned out to be too weak; this is the same
       # class of statement `skipped-emulator-missing` already makes, one probe
       # deeper. Both artifacts read it through `leg_run_is_skipped`.
       LEG_RUN_MODE["$leg"]="skip"
       LEG_RUN_VERDICT["$leg"]="skipped-launcher-prerequisite-missing"
       LEG_RUN_DETAIL["$leg"]="the DECLARED launcher '${LEG_LAUNCH[$leg]:-<none declared>}' is present on this host but ${n:-0} of its DECLARED prerequisite(s) are not — see the rows above for what each one provides and how to install it"
       LEG_VERDICT["$leg"]="skipped-launcher-prerequisite-missing"
       LEG_VERDICT_DETAIL["$leg"]="${LEG_RUN_DETAIL[$leg]}"
       return 1 ;;
    *) # rc 2 is the resolver's own FATAL (a catalogue/usage defect); anything
       # else is an outcome this driver has no arm for. NEVER assumed benign: an
       # unreadable answer is not evidence that the launcher works, and running
       # the corpus on that assumption is how a launch failure becomes a
       # compiler accusation. `poisoned` is the closed vocabulary's FAILURE
       # class — it reds the run and the run CONTINUES to every other leg.
       warn "[$leg] the launcher-prerequisite check exited $rc, which this driver does not recognise as a verdict class."
       warn "      ${LAUNCHER_PREREQ_ERR:-<no diagnostic on stderr>}"
       warn "      Treating it as a FAILURE rather than assuming the launcher is fine — an unreadable outcome is not evidence."
       LEG_RUN_MODE["$leg"]="skip"
       LEG_RUN_VERDICT["$leg"]="poisoned"
       LEG_RUN_DETAIL["$leg"]="harness_legs.py --check-launcher exited $rc for this leg (${LAUNCHER_PREREQ_ERR:-<no diagnostic>}), so whether its launcher can start the artefact is UNKNOWN on this machine"
       LEG_VERDICT["$leg"]="poisoned"
       LEG_VERDICT_DETAIL["$leg"]="${LEG_RUN_DETAIL[$leg]}"
       return 2 ;;
  esac
}
LAUNCHER_PREREQ_UNMET=0
for _l in "${LEG_ORDER[@]}"; do
  # ⚠ `|| _rc=$?`, NEVER `apply_launcher_prereq_gate "$_l"; _rc=$?`. Under
  # `set -Eeuo pipefail` the bare form exits the script ON the non-zero, BEFORE
  # the assignment runs, so every unmet leg would kill the run instead of being
  # recorded — the same rule this file states at :3561 and at the smoke gate.
  _plrc=0; apply_launcher_prereq_gate "$_l" || _plrc=$?
  [[ "$_plrc" -eq 0 ]] || LAUNCHER_PREREQ_UNMET=$((LAUNCHER_PREREQ_UNMET + 1))
done
[[ "$LAUNCHER_PREREQ_UNMET" -eq 0 ]] || \
  warn "$LAUNCHER_PREREQ_UNMET leg(s) will NOT be executed on this machine because a DECLARED launcher prerequisite is absent. They are still BUILT, and Step 9 names each one."
# <<< dss:launcher-prereq <<<

ensure_cmd curl curl
curl -fsS --max-time 20 -o /dev/null https://github.com || die "offline — cannot reach https://github.com."
pass "$HOST_OS/$HOST_ARCH host is online"
ensure_cmd git git
# HOST toolchain, for the harness's OWN work (deriving the recipe, building the
# reference oracle) — NOT the per-leg target compilers, which are the OPTIONAL
# CONTROL arm resolved from each leg's declared `targetCc` candidates in Step 6.
# ⚠ This block is the harness's own remaining host dependence and it is a
# different question from the one D-HARNESS-CROSS-HOST-ANY-TARGET asks: `make` +
# a host `cc` derive the RECIPE from upstream's build system and build the gcc
# reference oracle the confound classifier compares against. Neither produces an
# artefact for any leg's target.
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
# ── WHICH SQLITE THIS RUN IS ACTUALLY TESTING ────────────────────────────────
# DECLARED in legs.json `stageBuild`, resolved by harness_legs.py, consumed
# IDENTICALLY here and in build-and-test.ps1's derive script. Not spelled out in
# either driver, because a capability enabled in one and not the other is this
# project's canonical silent harness bug — and it had already happened one level
# down, between the two TARGETS of a single run: the sqlite3 CLI recipe carried
# -DSQLITE_ENABLE_FTS4 -DSQLITE_ENABLE_RTREE while the testfixture recipe built
# from the same tree carried neither.
#
# ★ WHY IT MATTERS AT ALL — ✔MEASURED on the elf64-x86_64 corpus.log of
#   2026-08-06: 362 of the 1,241 files the run called COMPLETED asserted
#   NOTHING. They emitted only the two teardown lines and returned at their
#   first `ifcapable` gate, while DSS had already compiled the very code they
#   would have tested (fts5.c was one of the 189 TUs, preprocessed to nothing).
#   D-HARNESS-CORPUS-FILES-COMPLETE-WITHOUT-ASSERTING-BECAUSE-CAPABILITIES-ARE-OFF.
#
# ★ TWO MECHANISMS, because upstream offers two and they are not interchangeable
#   — ✔MEASURED, and the measurement is the reason the second one exists:
#     · `configure --enable-all …` lands in the Makefile's OPT_FEATURE_FLAGS and
#       reaches every compile line.
#     · a raw define has NO configure flag. `OPTS=…` handed to configure lands
#       in the Makefile and NEVER on a testfixture compile line; `make
#       OPTIONS=-DSQLITE_ENABLE_STAT4` reached 106 of them. main.mk documents
#       exactly this: `OPT_FEATURE_FLAGS = … $(OPTIONS)`.
STAGE_CONFIGURE_FLAGS=""; STAGE_MAKE_OPTIONS=""
STAGE_REQUIRED_DEFINES=""; STAGE_WITNESSES=""
# ★ stderr TO ITS OWN FILE, exactly as the leg plan does a few hundred lines up:
#   this output is EVAL'd, so a diagnostic merged into it with 2>&1 would be
#   executed as shell. `--format sh` emits ONLY variable assignments — asserted
#   by the resolver's own self-test, which ran at Step 0b — and that guarantee is
#   worth nothing if stderr is allowed to join the stream. rc is taken DIRECTLY
#   off python3, never after a pipe.
_sb_err="$(mktemp)" || die "could not create a temp file for the stage-build resolver's stderr."
if _sb_out="$(python3 "$LEG_RESOLVER" --catalogue "$LEG_CATALOGUE" \
                --stage-build --format sh 2>"$_sb_err")"; then _sb_rc=0; else _sb_rc=$?; fi
if [[ $_sb_rc -ne 0 ]]; then
  _sb_msg="$(cat "$_sb_err" 2>/dev/null || true)"; rm -f "$_sb_err"
  die "could not resolve the sqlite stage build configuration
      (harness_legs.py --stage-build, rc=$_sb_rc):
$(printf '%s\n' "$_sb_msg" | sed 's/^/      /')
      This is the ONE declaration of which extensions the corpus tests. Proceeding without it
      would configure a tree with fts5/fts3/rtree/session OFF and then report every one of
      their ~270 test files as 'completed' — having asserted nothing."
fi
rm -f "$_sb_err"
eval "$_sb_out"
STAGE_CONFIGURE_FLAGS="${DSS_STAGE_CONFIGURE_FLAGS:-}"
STAGE_MAKE_OPTIONS="${DSS_STAGE_MAKE_OPTIONS:-}"
STAGE_REQUIRED_DEFINES="${DSS_STAGE_REQUIRED_DEFINES:-}"
STAGE_WITNESSES="${DSS_STAGE_WITNESSES:-}"
[[ -n "$STAGE_CONFIGURE_FLAGS" && -n "$STAGE_REQUIRED_DEFINES" ]] || die \
  "harness_legs.py --stage-build --format sh exited 0 but did not set the variables this driver reads.
      That is a contract break between the resolver and this driver, not a property of this host."
# Word-split DELIBERATELY: the resolver has already refused any value carrying
# whitespace or quoting, so each word is exactly one flag.
# shellcheck disable=SC2206
CONFIGURE_ARGS+=($STAGE_CONFIGURE_FLAGS)
info "configure: stage capabilities — $STAGE_CONFIGURE_FLAGS${STAGE_MAKE_OPTIONS:+   make OPTIONS=$STAGE_MAKE_OPTIONS}"
# $BLD is REUSED between runs, and re-running configure rewrites the Makefile but
# NOT the object timestamps — `make` cannot see that a header PATH changed. So a
# tclsqlite.o built under a previous run's Tcl survives and gets linked against
# THIS run's libtcl: a silently mis-built REFERENCE fixture, i.e. a corrupt
# attribution oracle (the one thing worse than no oracle). Stamp the Tcl identity
# and refuse to build on top of a different one. A MISSING stamp means "no
# information" and never fires, so every existing tree — every Linux tree today —
# is untouched until the Tcl actually changes under it. The harness does NOT wipe
# it itself: $BLD lives inside the sqlite clone, which this driver never destroys.
# ★★ WIDENED FROM "THE TCL BEHIND $BLD" TO "THE BUILD CONFIGURATION BEHIND $BLD",
#    and turned from a `die` into a REMEDIATION, because the capability set above
#    made both changes necessary in the same stroke:
#      · A capability flag changes far more than a header path. `configure`
#        rewrites OPT_FEATURE_FLAGS, but every existing .o is NEWER than its .c,
#        so make skips them — and the fixture links from a MIX of FTS5-on and
#        FTS5-off objects. It is also not only objects: main.mk runs `lemon
#        $(OPT_FEATURE_FLAGS)` and `mkkeywordhash $(OPT_FEATURE_FLAGS)`, so
#        parse.c and keywordhash.h are themselves configuration-dependent.
#      · A `die` here would halt FOUR HOSTS on a change this driver made
#        deliberately, each needing a hand `rm -rf`. The stamp exists to stop
#        stale objects, and wiping them IS the fix — announcing it and doing it
#        beats naming it and stopping.
# ★ AND AN ABSENT STAMP NOW FIRES. The old stamp deliberately did not: "no
#   information" was the right reading when it was added retroactively to trees
#   whose Tcl had not moved. For a CONFIGURATION stamp the same silence is the
#   dangerous case — a tree we cannot prove was built with this configuration is
#   exactly the tree that links a mixed fixture — so unknown provenance rebuilds.
#   That is also what carries every existing bld-dss across this change without
#   anyone being told to delete anything.
STAGE_STAMP="$BLD/.dss-stage-identity"
STAGE_STAMP_LEGACY="$BLD/.dss-tcl-identity"
STAGE_STAMP_NOW="tclsh=$(echo 'puts $tcl_version' | tclsh 2>/dev/null || true) configure=${CONFIGURE_ARGS[*]:-<default>} options=${STAGE_MAKE_OPTIONS:-<none>}"
STAGE_STAMP_WAS="$(cat "$STAGE_STAMP" 2>/dev/null || true)"
if [[ "$STAGE_STAMP_WAS" != "$STAGE_STAMP_NOW" ]]; then
  # LOOK AT THE TARGET BEFORE DESTROYING IT.
  # ⚠ BE HONEST ABOUT WHICH OF THESE CAN ACTUALLY VETO. The first two hold by
  #   construction — `BLD="$SQLITE_DIR/bld-dss"` is this file's only assignment
  #   to it and `mkdir -p "$BLD"` ran a few lines above — so they document the
  #   invariant and would catch a future edit that broke it, but they are not
  #   what protects a directory today. The MARKS check is the one that can say
  #   no, and it is the one that matters: `SQLITE_DIR` is an operator-settable
  #   knob (the clone-lock diagnostic advises setting it), so `$BLD` really can
  #   point at a `bld-dss` this harness did not build. It is also wiped only
  #   while this driver holds the clone WRITE lock, so no concurrent driver can
  #   be reading it.
  _wipe_ok=1
  [[ "$BLD" == "$SQLITE_DIR/bld-dss" ]] || _wipe_ok=0
  [[ -d "$BLD" ]] || _wipe_ok=0
  [[ -f "$BLD/Makefile" || -f "$STAGE_STAMP" || -f "$STAGE_STAMP_LEGACY" || -z "$(ls -A "$BLD" 2>/dev/null)" ]] || _wipe_ok=0
  if [[ $_wipe_ok -ne 1 ]]; then
    die "the build configuration behind $BLD changed, and that build directory could not be
      identified as this driver's own — refusing to delete it.
      was: ${STAGE_STAMP_WAS:-<no stamp: provenance unknown>}
      now: $STAGE_STAMP_NOW
      Expected \$SQLITE_DIR/bld-dss carrying a Makefile or a previous stamp. Remove it by hand
      once you have confirmed what it is:  rm -rf '$BLD'   then re-run."
  fi
  warn "the build configuration behind $BLD changed — REBUILDING IT FROM SCRATCH."
  warn "      was: ${STAGE_STAMP_WAS:-<no stamp: this tree predates the configuration stamp>}"
  warn "      now: $STAGE_STAMP_NOW"
  warn "      Re-running configure rewrites the Makefile but not the .o timestamps, so make would"
  warn "      SKIP every object and link a fixture from a MIXTURE of the two configurations — and"
  warn "      parse.c / keywordhash.h are generated with OPT_FEATURE_FLAGS too, so the mixture"
  warn "      would reach the parser itself. Wiping is the only honest way to change it."
  rm -rf "$BLD"
  mkdir -p "$BLD"
fi
# ★ THE BARE-CONFIGURE FALLBACK IS GONE, and its removal is the point rather than
#   tidying. It existed when CONFIGURE_ARGS was optional (Tcl pinning only). Now
#   the array ALWAYS carries the declared capability flags — the driver dies
#   above if it cannot resolve them — so an empty array can only mean the
#   resolution silently produced nothing, and the old `else` branch answered that
#   by configuring a sqlite with fts5/fts3/rtree/session OFF and continuing. A
#   fallback that is unreachable today is still the thing that runs on the day
#   the reasoning above stops holding. Found by this cycle's own parity pin
#   ("no ./configure invocation is left BARE"), not by review.
[[ ${#CONFIGURE_ARGS[@]} -gt 0 ]] || die "INTERNAL: CONFIGURE_ARGS is empty at the configure step.
      It must always carry legs.json's declared stageBuild.configureFlags by this point. Configuring
      without them would build a sqlite with the extensions OFF and the corpus would then report
      every one of their test files as 'completed' having asserted nothing."
( cd "$BLD" && "$SQLITE_DIR/configure" "${CONFIGURE_ARGS[@]}" >/dev/null )
printf '%s\n' "$STAGE_STAMP_NOW" > "$STAGE_STAMP"
# The legacy Tcl-only stamp is removed rather than left behind: two stamps in
# one directory is an invitation for a later reader to check the stale one.
rm -f "$STAGE_STAMP_LEGACY"
# ── DID THE CAPABILITIES ACTUALLY TAKE? ──────────────────────────────────────
# Asked of the MAKEFILE configure just wrote, before anything is compiled. A
# configure flag can be accepted and do nothing — ✔MEASURED: `--memsys3` exits 0
# and emits no define, because MEMSYS5 wins — and upstream can rename or retire a
# flag at any pull. Either way the corpus would go on reporting files as
# 'completed' while they assert nothing, which is unfalsifiable from the outside.
# The `$(OPTIONS)` defines are checked separately, on the derived recipe, because
# they never appear in OPT_FEATURE_FLAGS at all: they arrive on the make line.
_optflags="$(mk_var "$BLD/Makefile" OPT_FEATURE_FLAGS)"
_missing_cfg=""
for _d in $STAGE_REQUIRED_DEFINES; do
  case " $STAGE_MAKE_OPTIONS " in *" -D$_d "*) continue ;; esac
  case " $_optflags " in *" -D$_d "*|*" -D$_d="*) ;; *) _missing_cfg="$_missing_cfg $_d" ;; esac
done
if [[ -n "$_missing_cfg" ]]; then
  die "configure accepted its flags and did NOT produce the defines they exist for.
      missing from OPT_FEATURE_FLAGS:$_missing_cfg
      flags passed: ${CONFIGURE_ARGS[*]:-<none>}
      OPT_FEATURE_FLAGS: ${_optflags:-<empty>}
      A flag can be accepted and do nothing (measured: --memsys3), and upstream can rename or
      retire one at any pull. Without this stop the run would build a library WITHOUT these
      capabilities and then report every one of their test files as 'completed' — having asserted
      nothing at all [D-HARNESS-CORPUS-FILES-COMPLETE-WITHOUT-ASSERTING-BECAUSE-CAPABILITIES-ARE-OFF]."
fi
info "configure: all ${STAGE_REQUIRED_DEFINES// /, } accounted for"
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
# ── REGENERATE THE AMALGAMATION ORPHANS — A PRECONDITION THE CLI CREATES ─────
# D-HARNESS-SQLITE-STAGED-TREE-MIXED-VINTAGE, and this is a NEW instance of it
# that arrived with the sqlite3 CLI leg. MEASURED 2026-08-05 on the live tree.
#
# The gate at the end of this step enforces TWO id classes. The identity class
# (SQLITE_SOURCE_ID) was already kept honest, because everything carrying one is
# a prerequisite of `testfixture`. The FTS5 class was not, and asking make for
# the CLI is what exposed it:
#
#   `make sqlite3d` has fts5.o among its prerequisites, so make regenerates
#   `fts5.c` (via ext/fts5/tool/mkfts5c.tcl) and `sqlite3.h` at the CURRENT
#   checkout vintage — while `sqlite3.c`, `tclsqlite3.c` and `tsrc/fts5.c`, which
#   are prerequisites of NOTHING this harness asks for, keep the fts5 stamp they
#   were generated with. ✔MEASURED: fts5.c at 2026-08-05 00:41 carrying 6bdfff7d
#   beside three files at 2026-08-04 18:25 carrying bdc841de — "2 DIFFERENT ids
#   across 4 file(s)", and the coherence gate correctly went red.
#
# ★ THIS IS NOT PATCHING THE STAGED TREE. Nothing here edits an upstream source.
# It asks upstream's OWN build system to regenerate its OWN derived files from
# ONE source state — literally the command check-source-coherence.sh prints when
# it fails. Hand-copying one file WOULD be the workaround: it fixes the symptom
# you noticed and leaves the ones you did not.
#
# ★ IT RUNS BEFORE BOTH REFERENCE BUILDS, and the order is measured, not assumed.
# Once these are current, `make sqlite3d` and `make testfixture` regenerate
# nothing further: ✔MEASURED 2026-08-05, the gate stayed green across both
# subsequent builds. Running it afterwards instead would leave a window in which
# the reference binaries were built from a tree the gate had not yet certified.
#
# TOLERATED, NOT FATAL — deliberately. The run-wide `die` belongs to the GATE at
# the end of this step, which asserts the OUTCOME. Failing here instead would
# swap a precise "these files disagree, and here are their ids" for a vague "a
# make invocation exited non-zero", and this is a repair attempt, not the
# verdict on whether the repair was needed or worked.
info "regenerating the amalgamation orphans (sqlite3.c / shell.c / tclsqlite3.c) so the stage is ONE vintage"
if ( cd "$BLD" && make sqlite3.c shell.c tclsqlite3.c "OPTIONS=$STAGE_MAKE_OPTIONS" ) > "$OUT_DIR/amalgamation-regen.log" 2>&1; then
  info "      amalgamation regenerated (log: $OUT_DIR/amalgamation-regen.log)"
else
  warn "regenerating the amalgamation orphans FAILED (tolerated here — the coherence gate"
  warn "      at the end of this step is what renders the verdict). Log: $OUT_DIR/amalgamation-regen.log"
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
if ( cd "$BLD" && make -s testfixture USE_AMALGAMATION=0 "OPTIONS=$STAGE_MAKE_OPTIONS" -j"$JOBS" ) > "$REF_BUILD_LOG" 2>&1; then
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
#
# ★ `--require-cli` IS NOW PASSED, and it was not before. The gate has always had
# the flag (check-source-coherence.sh:78, :192-208) and NOTHING called it with
# one, so the assertion it implements — that sqlite3.c, shell.c and sqlite3.h are
# all present, and that shell.c's QUOTED `#include "sqlite3.h"` can only resolve
# beside it — was shipped inert. It matters now for a concrete reason: this
# harness BUILDS the CLI, and shell.c's startup guard compares
# `sqlite3_sourceid()` against the SQLITE_SOURCE_ID it was compiled with. A
# mismatched pair produces a binary that COMPILES CLEAN, LINKS CLEAN and then
# prints "SQLite header and source version mismatch" and exit(1) — which from the
# outside is indistinguishable from a miscompile.
"$SRC_COHERENCE" --checkout "$SQLITE_DIR" --require-cli --label "staged sqlite (Step 4)" "$BLD" \
  || die "staged sqlite tree is INCOHERENT — refusing to build (D-HARNESS-SQLITE-STAGED-TREE-MIXED-VINTAGE)"

# ── the SECOND attribution oracle: a gcc-built sqlite3 CLI ───────────────────
# ★ A MATCHED CONTROL — AND WHAT IT DOES AND DOES NOT HOLD CONSTANT, STATED.
# This is upstream's OWN `sqlite3d` target (main.mk:2185,
# `sqlite3d$(T.exe): shell.c $(LIBOBJS0)`): the SAME translation units from the
# SAME staged tree, built by gcc through upstream's own unmodified make rule.
# That is what an oracle needs — a second implementation compiling the same
# sources — and it is why a failure both binaries share is not DSS's.
#
# ★ WHAT IS **NOT** IDENTICAL, AND SAYING SO IS THE POINT. It is NOT true that
# "the only variable is the compiler", and the earlier wording here said exactly
# that. ✔MEASURED 2026-08-05 (base-harness.sh's dss_bh_recipe_token_span carries
# the measurement): upstream compiles the library objects with 8 defines
# INCLUDING `SQLITE_CORE` and compiles shell.c, on the link line, with 18 that do
# NOT include it (main.mk:2160-2166 explains the split). The gcc reference gets
# that SPLIT, because it is plain `make -s sqlite3d`. DSS builds ONE program from
# ONE `defines` array, so it necessarily gets the UNION of the two sets. So the
# controlled variables are: the sources, the tree, and the include dirs. The
# uncontrolled ones are: the compiler (the thing under test) and shell.c's view
# of `SQLITE_CORE` (a consequence of one-program-one-define-set). A difference
# that lands on that seam is a real result the oracle cannot attribute, and it
# has to be recognised rather than argued away — which needs it written down.
#
# It exists because the unit corpus cannot see shell.c at ALL: the corpus runs
# through `testfixture`, a Tcl interpreter linking the sqlite LIBRARY, so every
# CLI-only surface (argv, the dot-commands, the .dump writer, the startup guard)
# is covered by nothing until the smoke gate runs [D-SQLITE-CLI-BUILT-ON-NO-LEG].
# Without this reference a smoke failure could not be attributed, and cli-smoke.py
# charges an unattributable failure to DSS rather than waving it through.
#
# SAME COPY-THEN-DELETE PAIR as the fixture above, for the same reason: `make -n`
# only prints a recipe for a MISSING target, and the CLI recipe is harvested a few
# lines down. The copy is not a make target, so the delete still exposes it.
# A link miss is TOLERATED (the CLI legs still build; they just cannot be
# exonerated) and is stated out loud rather than left to silence.
REF_CLI_KEEP="$OUT_DIR/reference-sqlite3"
REF_CLI=""
rm -f "$REF_CLI_KEEP"
REF_CLI_LOG="$OUT_DIR/reference-cli-build.log"
info "building the reference gcc sqlite3 CLI (upstream's own 'sqlite3d' target)"
if ( cd "$BLD" && make -s sqlite3d "OPTIONS=$STAGE_MAKE_OPTIONS" -j"$JOBS" ) > "$REF_CLI_LOG" 2>&1 && [[ -x "$BLD/sqlite3d" ]]; then
  if cp -p "$BLD/sqlite3d" "$REF_CLI_KEEP"; then
    REF_CLI="$REF_CLI_KEEP"
    info "reference gcc sqlite3 CLI built + preserved -> $REF_CLI  (the CLI ATTRIBUTION ORACLE)"
  else
    warn "the reference sqlite3 CLI LINKED but could NOT be preserved to $REF_CLI_KEEP — it is"
    warn "      about to be deleted to expose its recipe, so no CLI oracle survives this run."
  fi
else
  warn "the reference gcc sqlite3 CLI did not build (tolerated — the CLI legs still build)."
  warn "      Log KEPT at $REF_CLI_LOG — READ IT. Without it NO smoke failure on any leg can be"
  warn "      EXONERATED, and cli-smoke.py charges an unattributable failure to DSS by design."
fi
rm -f "$BLD/sqlite3d"

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
# The archive the CORE sources have to be recovered from: DSS cannot consume a gcc
# `.a`, so every member's `.c` is found again by name. Resolved BEFORE the
# derivation because both derivations (this one and the CLI's below) pass it.
AR="$BLD/libsqlite3.a"; [[ -f "$BLD/.libs/libsqlite3.a" ]] && AR="$BLD/.libs/libsqlite3.a"
FIXTURE_TUS_FILE="$OUT_DIR/tus.base.txt"
FIXTURE_DEFS_FILE="$OUT_DIR/defines.base.txt"
FIXTURE_INCS_FILE="$OUT_DIR/recipe-includes.base.txt"
# ── THE FIXTURE RECIPE, DERIVED BY THE SHARED CORE ───────────────────────────
# ★ THE SAME FUNCTION THE CLI DERIVATION BELOW CALLS, AND THE SAME ONE
# build-and-test.ps1's derive script calls. This block used to be a hand-written
# copy of dss_bh_emit_recipe: its own `sed -e ':a' …` join, its own -D/-I greps,
# its own span/archive TU loops and its own dedup. Extracting the core and then
# leaving the FIXTURE path on the private copy would have been the worst of both
# — one decision with a shared implementation that half the callers do not use,
# which is exactly the arrangement that manufactured the three measured drifts
# base-harness.sh's header lists. Everything this call passes is what the FIXTURE
# needs and the CLI does not:
#   · --make-var USE_AMALGAMATION=0   upstream's own switch for the full-source
#                                     fixture (the amalgamation is banned here).
#   · --prereq-mode link-line         select ONLY the `-o testfixture` line.
#   · --always-make 1                 add `-B`, so the derive is DETERMINISTIC.
#   · --token-scope recipe            compile lines UNION the link line; required
#                                     with `-B`, whose output also prints the
#                                     jimsh/lemon bootstrap and its foreign -D.
#   · --archive-from-span 0           take EVERY archive member; the fixture links
#                                     the whole library, not a named object list.
#
# ★★ THOSE FIRST THREE CHANGED 2026-08-06 (TF-C126), AND THE COMMENT THEY REPLACE
# IS WHY. It read: "`make -n testfixture` runs with every prerequisite already
# built, so the recipe IS essentially the one link command" and "there is no
# bootstrap in this recipe to keep foreign -D out of". Both were ASSUMPTIONS about
# the state of $BLD, neither was asserted, and ✔MEASURED 2026-08-06 both are FALSE
# on a tree whose bootstrap is not current: the derive harvested `tool/lemon.c`,
# `tool/lempar.c` and `tool/mksourceid.c` as target TUs and every one of the five
# legs failed to compile. `make -n` describes WHAT REMAINS TO BE DONE, not what the
# build is — so $BLD is an INPUT to this derivation, and it was an unexamined one.
# ⚠ THE SAME DEFECT WAS ALREADY FOUND AND FIXED FOR THE CLI ON 2026-08-05 (see
# base-harness.sh's --always-make note: "a build that succeeded or failed depending
# on the state of a build directory nobody thought of as an input"). The CLI moved
# to link-line/-B; THIS call site was left on the unhardened path, and the two
# derivations' own numbers are the matched control — across two runs hours apart
# the CLI held at 103 TUs while the fixture drifted 189 -> 192.
# FLOORS 150/18: ~189 TUs and ~20 defines. ⚠ The floor comment used to say "~192",
# and 192 was the CONTAMINATED count (189 + the three tool sources above) — the
# figure was taken from a poisoned derive on 2026-08-05 and had been the documented
# normal ever since. High enough that losing the archive recovery (which leaves
# ~90) is an immediate named stop, low enough not to red on upstream adding or
# dropping a source file.
#
# ★ D-HARNESS-SH-TU-DEDUP-DEPENDS-ON-BASH-HASH-ORDER is closed HERE as a
# consequence: the surviving path for two same-basename spellings is
# dss_bh_dedup_by_basename's answer, over a SORTED set, rather than bash's
# internal hash order over `"${!TU[@]}"`. ✔MEASURED 2026-08-05 before adopting
# it: byte-identical on this recipe (192 TUs, zero duplicate basenames), so the
# latent defect closes without moving today's TU set.
if _fixture_summary="$(dss_bh_emit_recipe \
      --build-dir "$BLD" --make-target testfixture --recipe-file "$RECIPE" \
      --make-var USE_AMALGAMATION=0 \
      --make-var "OPTIONS=$STAGE_MAKE_OPTIONS" \
      --prereq-mode link-line --always-make 1 --token-scope recipe \
      --archive "$AR" --archive-from-span 0 \
      --search-root "$SQLITE_DIR/src" --search-root "$SQLITE_DIR/ext" --search-root "$BLD" \
      --min-tus 150 --min-defines 18 \
      --out-tus "$FIXTURE_TUS_FILE" --out-defines "$FIXTURE_DEFS_FILE" \
      --out-includes "$FIXTURE_INCS_FILE")"; then
  mapfile -t TUS         < "$FIXTURE_TUS_FILE"
  mapfile -t RECIPE_DEFS < "$FIXTURE_DEFS_FILE"
  mapfile -t SQLITE_INCS < "$FIXTURE_INCS_FILE"
  pass "recipe: $_fixture_summary"
else
  die "the testfixture recipe derivation FAILED — see $RECIPE and the diagnostic above.
      A short parse does not error on its own: it yields a smaller TU set that compiles,
      links, and fails much later looking like a codegen bug. That is what the floors and
      the drop ledger turn into this stop."
fi

# ── the sqlite3 CLI recipe — ITS OWN ARRAYS, never the fixture's map ─────────
# ★ SEPARATE ARRAYS, DELIBERATELY. The CLI's TU set is derived into CLI_TUS /
# CLI_DEFS / CLI_INCS and the fixture's `TU` map is not touched. Merging them
# would be the concrete trigger for the dedup defect closed just above, and the
# two sets are genuinely different programs: ✔MEASURED 2026-08-05, the CLI set is
# a STRICT SUBSET of the fixture's plus exactly one file (shell.c), and the
# fixture carries 90 TUs — the Tcl test harness — that must never reach the CLI.
#
# ★ WHY `sqlite3d` AND NOT `sqlite3`. main.mk:216-219 says `sqlite3$(T.exe)`
# REQUIRES the amalgamation and IGNORES USE_AMALGAMATION, so `make -n sqlite3
# USE_AMALGAMATION=0` derives an AMALGAMATION build — against this project's
# "full upstream source, never the amalgamation" rule
# [D-SQLITE-CLI-UPSTREAM-TARGET-IS-AMALGAMATION-ONLY]. `sqlite3d` is upstream's
# own full-source CLI: `sqlite3d$(T.exe): shell.c $(LIBOBJS0)` (main.mk:2185).
#
# ★ WHY `link-line` MODE. sqlite3d's prerequisites are `.o` files, so a
# whole-blob scrape finds only shell.c when the objects are current — and when
# they are STALE it finds far too much: ✔MEASURED 2026-08-05, a whole-blob derive
# absorbs `tool/lemon.c`, `tool/lempar.c` and `tool/mksourceid.c`, which are
# BUILD-HOST tools (`lempar.c` is not even standalone C — it is lemon's parser
# template). Link-line mode reads upstream's own declared prerequisite list and
# recovers each `.o` through the archive, giving 103 TUs in both object states.
#
# ★ WHY `--always-make` AND `--token-scope recipe`. `make -n` prints only what it
# WOULD do, so with the 102 objects already current it prints ONE line — the
# link — and the -D set read off it OMITS `SQLITE_CORE` (upstream compiles the
# library and shell.c with different define sets; main.mk:2160-2166). ✔MEASURED
# 2026-08-05, that is not cosmetic: ext/icu/icu.c:31-33 is
# `#if !defined(SQLITE_CORE) || defined(SQLITE_ENABLE_ICU) …`, so a file that
# should compile to nothing instead demanded <unicode/*.h> and the CLI died with
# four `error[F001A]`. The build's success depended on the freshness of a build
# directory nobody thought of as an input. `-B` makes make print everything (a
# DRY RUN still — `-n` is in force), and `--token-scope recipe` reads the -D/-I
# off the compile lines UNION the link line, so the jimsh/lemon bootstrap's
# foreign `JIM_COMPAT` / `HAVE_REALPATH` / `_FILE_OFFSET_BITS=64` stay out.
#
# FLOORS: 100 TUs / 18 defines. Honest for a ~103-TU program — high enough that
# losing the archive recovery (which would leave 1) or the link line (0) is an
# immediate named stop, low enough not to red on upstream adding or dropping one
# source file or one flag. The regression that actually matters is caught by NAME
# below, not by the count: a count with zero headroom reds on an unrelated
# upstream edit, and a count alone cannot say WHICH define went missing.
CLI_RECIPE="$OUT_DIR/sqlite3-cli-recipe.txt"
CLI_TUS_FILE="$OUT_DIR/cli-tus.txt"
CLI_DEFS_FILE="$OUT_DIR/cli-defines.txt"
CLI_INCS_FILE="$OUT_DIR/cli-includes.base.txt"
if _cli_summary="$(dss_bh_emit_recipe \
      --build-dir "$BLD" --make-target sqlite3d --recipe-file "$CLI_RECIPE" \
      --make-var "OPTIONS=$STAGE_MAKE_OPTIONS" \
      --prereq-mode link-line --always-make 1 --token-scope recipe \
      --archive "$AR" --archive-from-span 1 \
      --search-root "$SQLITE_DIR/src" --search-root "$SQLITE_DIR/ext" --search-root "$BLD" \
      --min-tus 100 --min-defines 18 \
      --out-tus "$CLI_TUS_FILE" --out-defines "$CLI_DEFS_FILE" --out-includes "$CLI_INCS_FILE")"; then
  mapfile -t CLI_TUS  < "$CLI_TUS_FILE"
  mapfile -t CLI_DEFS < "$CLI_DEFS_FILE"
  mapfile -t CLI_INCS < "$CLI_INCS_FILE"
  pass "cli recipe: $_cli_summary"
else
  die "the sqlite3 CLI recipe derivation FAILED — see $CLI_RECIPE and the diagnostic above.
      This is NOT skippable: a CLI leg that silently does not build is exactly the
      'a capability in one driver and not the other' failure this work closed."
fi
# The CLI needs SHELL.C, and a derivation that lost it would still clear the TU
# floor on the 102 library sources alone — building a library with no `main` and
# failing much later at the entry trampoline. Asserted by name rather than by
# count, because the count cannot see WHICH file went missing.
printf '%s\n' "${CLI_TUS[@]}" | grep -qE '/shell\.c$' \
  || die "the CLI TU set has no shell.c — it is the CLI's only entry point (sqlite3.c has no main).
      Derived from: $CLI_RECIPE"
# ★ SQLITE_CORE, ASSERTED BY NAME. This is the define whose absence is SILENT in
# every count: with 18 perfectly plausible defines and no SQLITE_CORE,
# ext/icu/icu.c:31-33 (`#if !defined(SQLITE_CORE) || defined(SQLITE_ENABLE_ICU) …`)
# stops being a no-op and pulls in <unicode/utypes.h>, and the run dies three
# minutes later with four `error[F001A] got unicode/*.h` that look like a missing
# system dependency rather than a derivation bug. ✔MEASURED 2026-08-05 — that is
# exactly how this was found. It goes missing whenever the -D tokens are read
# from the LINK line alone (upstream compiles the library with SQLITE_CORE and
# shell.c without it), so this assertion is the guard on `--token-scope recipe`
# still doing its job.
printf '%s\n' "${CLI_DEFS[@]}" | grep -qx 'SQLITE_CORE' \
  || die "the CLI define set has no SQLITE_CORE (${#CLI_DEFS[@]} defines derived).
      Without it ext/icu/icu.c stops compiling to nothing and demands <unicode/*.h>,
      which fails as 'error[F001A] got unicode/utypes.h' — a derivation bug wearing a
      missing-dependency costume. It is contributed by the library COMPILE lines, so
      this means the -D tokens were read from the link line alone: check that
      --always-make and --token-scope recipe survived. Derived from: $CLI_RECIPE"

# ── THE DECLARED CAPABILITIES REACHED **BOTH** DERIVED RECIPES ───────────────
# ★ THIS IS THE GUARD FOR THE DEFECT THAT MOTIVATED THE WHOLE stageBuild BLOCK,
#   and it is asserted on BOTH targets because the defect WAS the asymmetry:
#   ✔MEASURED 2026-08-06, the CLI recipe carried -DSQLITE_ENABLE_FTS4 and
#   -DSQLITE_ENABLE_RTREE while the testfixture recipe, derived from the SAME
#   configured tree in the SAME run, carried neither. "Which extensions exist"
#   had two answers inside one run, and nothing anywhere said so.
# ★ AND IT IS THE ONLY CHECK THAT CAN SEE THE `$(OPTIONS)` DEFINES AT ALL. They
#   never appear in OPT_FEATURE_FLAGS — they arrive on the make command line — so
#   the post-configure Makefile check above skips them by construction. If
#   `--make-var OPTIONS=…` is ever dropped from a call site, or a driver spells
#   the value itself and gets it wrong, this is what fires.
_assert_recipe_capabilities() {          # <label> <recipe file> <define>…
  local label="$1" recipe="$2"; shift 2
  local missing="" d
  for d in $STAGE_REQUIRED_DEFINES; do
    # `NAME` OR `NAME=VALUE`. dss_bh_recipe_defines emits a valued define as
    # `NAME=VALUE`, and the post-configure check above already accepts `-DNAME=`.
    # Matching only the bare name here would mean upstream spelling one
    # `-DSQLITE_ENABLE_FTS5=1` passes that check and dies on this one with
    # "MISSING declared capabilities" — a loud stop pointing at the wrong thing,
    # which costs more than the silence it replaced.
    printf '%s\n' "$@" | grep -qE "^${d}(=|$)" || missing="$missing $d"
  done
  [[ -z "$missing" ]] || die "the $label recipe is MISSING declared capabilities:$missing
      declared (legs.json stageBuild.requiredDefines): $STAGE_REQUIRED_DEFINES
      make OPTIONS passed:                            ${STAGE_MAKE_OPTIONS:-<none>}
      derived from:                                   $recipe
      Every one of these was asked for by name. A missing one means the library DSS is about to
      build does not have that capability, while this run would go on to report every one of its
      test files as 'completed' — with nothing asserted in any of them. The fixture and the CLI
      are checked SEPARATELY on purpose: they are derived from two different make targets, and
      the two disagreeing is the exact defect this exists to catch."
}
_assert_recipe_capabilities "testfixture" "$RECIPE"     "${RECIPE_DEFS[@]}"
_assert_recipe_capabilities "sqlite3 CLI" "$CLI_RECIPE" "${CLI_DEFS[@]}"
pass "capabilities: both recipes carry all ${STAGE_REQUIRED_DEFINES// /, }"

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
# ⚠ BUILD INTO A NAMED SUBDIRECTORY, NEVER INTO `build/` ITSELF. Operator rule
# 2026-08-17: `build/` is the single ROOT and every build tree is a subdirectory
# of it. Configuring into the root would make the root simultaneously a container
# and a build tree — the exact ambiguity the rule exists to remove.
_dss_bdir="$SRC_DIR/build/rel"
( cd "$SRC_DIR" && cmake -B "$_dss_bdir" -DCMAKE_BUILD_TYPE=Release && cmake --build "$_dss_bdir" -j"$JOBS" )
# ★★ SEARCH THE TREE WE JUST BUILT, NOT THE WHOLE ROOT. This was
# `find "$SRC_DIR/build" … -print -quit`, taking the FIRST match — unambiguous
# only while `build/` held exactly one build tree. The one-root layout makes that
# ambiguous BY CONSTRUCTION: ✔MEASURED 2026-08-17 on the Mac, `build/bin` and
# `build/mac/bin` BOTH matched and `-print -quit` resolved it by filesystem
# enumeration order. It happened to pick the tree this step rebuilds; nothing
# guaranteed it. Same class as a pin sampling `front()` of a discovered set.
DSS_BIN="$(find "$_dss_bdir" -type f -name dss-code-prime -perm -u+x -print -quit 2>/dev/null)"
# Widen to the whole root ONLY if the tree we built has none, and SAY SO — a
# silent widening is how the ambiguity creeps back.
if [[ -z "$DSS_BIN" ]]; then
  DSS_BIN="$(find "$SRC_DIR/build" -type f -name dss-code-prime -perm -u+x -print -quit 2>/dev/null)"
  [[ -n "$DSS_BIN" ]] && warn "no dss-code-prime under $_dss_bdir; widened to a root-wide search and took $DSS_BIN"
fi
[[ -n "$DSS_BIN" && -x "$DSS_BIN" ]] || die "dss-code-prime binary not found under $_dss_bdir (nor anywhere under $SRC_DIR/build)."

# ★★ THE COMPILER'S OWN BUILD TYPE IS READ FROM THE TREE THAT PRODUCED IT, AND
# PRINTED NEXT TO ITS PATH — even here, where the two lines above are what set it.
# PARITY with build-and-test.ps1's Step 5, and the parity is the entire point:
# that driver used to take the NEWEST binary under any root with no regard for how
# it was built (on a developer box, always `build/dbg`), while this one builds
# `-DCMAKE_BUILD_TYPE=Release` unconditionally. TWO DRIVERS, ONE CONTRACT, TWO
# ANSWERS — and neither log ever named the variable, so the difference was
# published as a property of the HOST
# (D-PERF-WINDOWS-HOST-COMPILES-8X-SLOWER-THAN-LINUX, ~8x) when it was -O0 against
# -O3 (~2.1x once controlled). This side was RIGHT and still said nothing, which
# is exactly what made the other side's silence unnoticeable.
# ⚠ AND THIS SIDE IS NOT UNCONDITIONALLY RIGHT EITHER: the widened search above
# reaches the WHOLE of $SRC_DIR/build, which holds other people's trees (build/dbg
# is one), so what this step ends up holding is not guaranteed to be what the
# cmake line asked for. The answer is READ, never inferred from the invocation
# that precedes it and never from the directory's name.
# Echoes TYPE on line 1 and PROVENANCE on line 2 — two lines rather than a
# delimiter because both halves are free text that can contain anything.
dss_build_type() {                      # <binary>
  local bin="$1" dir="" cache="" btype="" cfgs="" gen="" rel="" part="" c="" hit=""
  local -a parts=() cfglist=()
  dir="$(cd "$(dirname "$bin")" 2>/dev/null && pwd)" || dir=""
  # Walk UP to the build tree: the nearest ancestor holding a CMakeCache.txt.
  while [[ -n "$dir" ]]; do
    if [[ -f "$dir/CMakeCache.txt" ]]; then cache="$dir/CMakeCache.txt"; break; fi
    [[ "$dir" == "/" ]] && break
    dir="$(dirname "$dir")"
  done
  if [[ -z "$cache" ]]; then
    printf '%s\n' '<unknown>' "NO CMakeCache.txt in any ancestor of $bin — nothing on this machine states how it was built"
    return 0
  fi
  btype="$(sed -n 's/^CMAKE_BUILD_TYPE:[^=]*=//p'          "$cache" | sed -n 1p)"
  cfgs="$( sed -n 's/^CMAKE_CONFIGURATION_TYPES:[^=]*=//p' "$cache" | sed -n 1p)"
  gen="$(  sed -n 's/^CMAKE_GENERATOR:[^=]*=//p'           "$cache" | sed -n 1p)"
  if [[ -n "$cfgs" ]]; then
    # MULTI-config (Xcode is the one that reaches this on a POSIX host). The cache
    # CANNOT answer: one tree holds every config and CMAKE_BUILD_TYPE is an entry
    # the generator IGNORES, so `-DCMAKE_BUILD_TYPE=Release` + `--build --config
    # Debug` leaves a cache saying Release over a Debug binary. What the generator
    # DOES state is the config it built, written into the OUTPUT PATH — so the
    # answer is the path component naming one of the configs the cache DECLARES.
    # Two facts the tree states about itself, cross-checked; a component matching
    # nothing declared is not an answer.
    rel="${bin#"$dir"/}"
    IFS='/' read -r -a parts   <<< "$rel"
    IFS=';' read -r -a cfglist <<< "$cfgs"
    for part in "${parts[@]}"; do
      for c in "${cfglist[@]}"; do
        # Case-INSENSITIVE, matching the .ps1 twin (-contains) and
        # profile-compile-support.py (.lower()): CMake uppercases the build
        # type to look up CMAKE_<LANG>_FLAGS_<CFG>, so `release` selects the
        # identical flags as `Release` and a lowercase spelling is not a
        # different answer.
        if [[ -n "$c" && "${part,,}" == "${c,,}" ]]; then hit="$part"; fi   # DEEPEST wins
      done
    done
    if [[ -n "$hit" ]]; then
      printf '%s\n' "$hit" "the multi-config generator's own output subdirectory '$hit', cross-checked against CMAKE_CONFIGURATION_TYPES=$cfgs in $cache (generator '$gen')"
    else
      printf '%s\n' '<unknown>' "$cache is a MULTI-CONFIG tree (generator '$gen' declares CMAKE_CONFIGURATION_TYPES=$cfgs) and no component of '$rel' names one of them"
    fi
  elif [[ -n "$btype" ]]; then
    printf '%s\n' "$btype" "CMAKE_BUILD_TYPE in $cache (single-config generator '$gen')"
  else
    # EMPTY is an answer, not a gap: it is CMake's default of no optimisation
    # flags at all. Rounding it up to Release is the whole class of defect here.
    printf '%s\n' '<none>' "$cache declares NO CMAKE_BUILD_TYPE (single-config generator '$gen') — CMake's default of no optimisation flags"
  fi
}
_dss_bt="$(dss_build_type "$DSS_BIN")"
DSS_BUILD_TYPE="$(printf '%s\n' "$_dss_bt"    | sed -n 1p)"
DSS_BUILD_TYPE_SRC="$(printf '%s\n' "$_dss_bt" | sed -n 2p)"
DSS_BUILD_TYPE_NOTE="  (compiler build type: $DSS_BUILD_TYPE)"
info "compiler  : $DSS_BIN"
info "build type: $DSS_BUILD_TYPE"
info "  read from: $DSS_BUILD_TYPE_SRC"
# Case-insensitive on purpose: CMake uppercases the build type to look up
# CMAKE_<LANG>_FLAGS_<CFG>, so `-DCMAKE_BUILD_TYPE=release` selects the identical
# flags as `Release` and refusing it would be this check failing on a spelling.
if [[ "${DSS_BUILD_TYPE,,}" != "release" ]]; then
  if [[ "$ALLOW_NONRELEASE_DSS" -eq 0 ]]; then
    die "this run would be timed against a NON-RELEASE compiler, and it REFUSES rather than proceed quietly.
      compiler   : $DSS_BIN
      build type : $DSS_BUILD_TYPE
      read from  : $DSS_BUILD_TYPE_SRC
      A Debug dss-code-prime is -g, no -O and no NDEBUG: it compiles the same program correctly
      and takes several times as long, and that difference lands in whatever this run is read for.
      The .ps1 twin holds the identical gate, so a number from either driver is comparable only
      because both of them state this.
      Fix the tree ($_dss_bdir) — cmake -B '$_dss_bdir' -DCMAKE_BUILD_TYPE=Release — or set
      DSS_ALLOW_NONRELEASE_COMPILER=1 to proceed with THIS binary, with every report line saying so."
  fi
  DSS_BUILD_TYPE_NOTE="  (compiler build type: $DSS_BUILD_TYPE — NOT Release, DSS_ALLOW_NONRELEASE_COMPILER=1)"
  warn "DSS_ALLOW_NONRELEASE_COMPILER=1 — proceeding with a $DSS_BUILD_TYPE compiler. TIMINGS FROM THIS RUN ARE NOT COMPARABLE with build-and-test.ps1 or with any other run of this driver, which always times a Release compiler."
fi
pass "dss-code-prime built: $DSS_BIN  ($DSS_BUILD_TYPE)"

# ── Step 6 — stage third-party headers + obtain per-leg libs ─────────────────
step "6/9  Third-party headers (parsed agnostically) + per-leg tcl/zlib libraries"
# Headers are leg-INDEPENDENT as far as ABI goes: DSS parses the host tcl/zlib
# headers agnostically (ABI is irrelevant at parse). The tcl headers sit in a
# per-version private subdir (safe on -I); zlib.h sits directly in a system
# include dir (would shadow the OS descriptors) → stage a private copy of just
# zlib.h + zconf.h.
# ⚠ THAT SENTENCE USED TO STOP AT "leg-INDEPENDENT", AND IT IS ONLY HALF TRUE —
# which is why the defect below survived for months
# [D-HARNESS-TCL-HEADER-IS-HOST-CHOSEN-WHILE-EVERY-LEG-LIBRARY-IS-PINNED].
# True of ABI. FALSE of API SURFACE: the header VERSION selects WHICH SYMBOLS the
# fixture REFERENCES (sqlite's tclsqlite.c gates live code on
# TCL_MAJOR_VERSION>8), and that is a LINK-time, PER-LEG fact. The Tcl chosen
# here is the ONE Tcl input this harness still takes from the HOST while every
# leg's library is pinned by its own TARGET-keyed provider — so after the
# per-leg libraries are resolved below, the two are compared leg by leg and a
# skew REFUSES the run.
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
# ONE PER ARTIFACT: the fixture and the CLI are different programs with different
# declared inputs (the CLI must not see the staged Tcl headers), so a single
# shared list would hand one of them an input it never asked for.
declare -A LEG_INC_FILE=() LEG_CLI_INC_FILE=() LEG_ZINC_DIR=() LEG_CFG_DIR=()
declare -A ZINC_STAGE_DIR=() CFG_STAGE_DIR=()
# ★★ AND THE SAME STORY FOR SQLITE'S OWN ./configure HEADER
# (D-HARNESS-MACHO-LEG-INHERITS-THE-DERIVING-LINUX-HOSTS-CONFIGURE-PROBES).
# `configure` ran HERE, on the deriving host, and wrote $BLD/sqlite_cfg.h with THIS
# machine's probe answers. The recipe carries `_HAVE_SQLITE_CONFIG_H` — the one
# host-probe define left on the command line — which makes sqliteInt.h
# `#include "sqlite_cfg.h"`, and $BLD is on every leg's include list. So every leg
# inherited this box's answers: MEASURED, that is how the macho64-arm64 CLI came to
# fail on `off64_t`/`pread64`/`pwrite64` (HAVE_PREAD64+HAVE_PWRITE64 -> os_unix.c's
# USE_PREAD64 -> macro casts typed with a type Darwin does not have).
# Each leg DECLARES its target's answers (legs.json `build.configureAnswers`) and
# the same stage-zinc.py writes one cfg/<targetOs>/sqlite_cfg.h per declared stage —
# the deriving host's answers for the ~49 rows that do not vary, the leg's own for
# the three that do. Step 7 puts that dir AHEAD of $BLD on the leg's include list,
# so the leg's own header wins the quote-include search and $BLD still supplies the
# generated sqlite3.h/parse.h/opcodes.h it is there for.
CFG_ROOT="$BLD/cfg"; mkdir -p "$CFG_ROOT"
# The DERIVING host's generated header. Fatal for the whole run if absent, exactly
# like zlib.h above and for the same reason: it is the SOURCE every stage is
# rewritten from, so without it no leg on any host has a configure header at all.
[[ -f "$BLD/sqlite_cfg.h" ]] || die "the generated sqlite_cfg.h is not in $BLD.
      sqlite's ./configure writes it there and the recipe's _HAVE_SQLITE_CONFIG_H makes every TU
      include it; each leg is staged its OWN copy from it (build.configureAnswers), so its absence
      is fatal for the ENTIRE run rather than for one leg."
# rc DIRECTLY off python3 (never through a pipe) — the output is captured first
# and parsed after, so a FAIL line is still read on a non-zero rc.
ZINC_OUT="$(python3 "$SCRIPT_DIR/stage-zinc.py" --zlib-h "$ZINC_SRC/zlib.h" \
              --zconf-h "$ZINC_SRC/$(basename "$ZCH")" --dest "$ZINC_ROOT" \
              --sqlite-cfg-h "$BLD/sqlite_cfg.h" --cfg-dest "$CFG_ROOT" \
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
    CFG-STAGE-OK=*)
      _rest="${_zl#CFG-STAGE-OK=}"; _k="${_rest%%|*}"; _rest="${_rest#*|}"
      _dir="${_rest%%|*}"; _rest="${_rest#*|}"; _ans="${_rest%%|*}"; _note="${_rest#*|}"
      CFG_STAGE_DIR["$_k"]="$_dir"
      info "sqlite config stage '$_k' -> $_dir   [$_ans]"
      [[ -z "$_note" ]] || info "      note: $_note" ;;
    CFG-STAGE-FAIL=*)
      _rest="${_zl#CFG-STAGE-FAIL=}"
      warn "sqlite config stage '${_rest%%|*}' COULD NOT BE PRODUCED — ${_rest#*|}" ;;
    CFG-STAGES=*) info "sqlite config stages: ${_zl#CFG-STAGES=} produced" ;;
    *) [[ -z "$_zl" ]] || info "      $_zl" ;;
  esac
done <<< "$ZINC_OUT"
[[ ${#ZINC_STAGE_DIR[@]} -gt 0 ]] || die "stage-zinc.py produced NO per-target zlib header dir:
$ZINC_OUT"
[[ ${#CFG_STAGE_DIR[@]} -gt 0 ]] || die "stage-zinc.py produced NO per-target sqlite_cfg.h.
      Every leg would then fall back to the DERIVING host's copy on the \$BLD include dir, which is
      exactly D-HARNESS-MACHO-LEG-INHERITS-THE-DERIVING-LINUX-HOSTS-CONFIGURE-PROBES:
$ZINC_OUT"
# ── ★★ AND NOW REMOVE THE DERIVING HOST'S COPY, because "the staged dir is FIRST
# on the include list" DOES NOT COVER EVERY TU.
#
# ⛔ THE EXCEPTION, ✔MEASURED 2026-08-05 (TF-C121). A quote include searches the
# INCLUDING FILE'S OWN DIRECTORY *BEFORE* THE INCLUDE LIST IS CONSULTED AT ALL
# (src/core/types/include_path_resolve.hpp: `resolveIncludePath` — "try the
# including file's own directory FIRST, then each of `includeDirs`", C 6.10.2p3).
# The position argument is therefore sound only for an includer that has no
# `sqlite_cfg.h` beside it. `sqliteInt.h` lives in sqlite/src/ and does not — but
# `$BLD/ctime.c` DOES, and it is TU #1 in BOTH tus.txt and cli-tus.txt, i.e. the
# fixture and the CLI, on every leg. It opens with:
#     #if defined(_HAVE_SQLITE_CONFIG_H) && !defined(SQLITECONFIG_H)
#     #include "sqlite_cfg.h"
#     #define SQLITECONFIG_H 1
# so it took the DERIVING host's header out of its own directory (HAVE_MALLOC_H,
# HAVE_PREAD64, HAVE_PWRITE64 — all three of the answers the staging exists to
# correct) and then SHADOWED sqliteInt.h's guarded include for the rest of that
# TU. Latent only because ctime.c consumes HAVE_ISNAN alone, a row on which every
# target agrees; os_unix.c (which is where the off64_t/pread64/pwrite64 damage
# actually lands) has no sibling header and got the staged one correctly, which is
# why the "6 errors -> 0" result held and the exception stayed invisible.
#
# ★ THIS IS NOT "PATCHING THE STAGED TREE" — and the distinction is worth stating
# because that is a HARD rule here (the corpus must be unmodified upstream sqlite).
# `sqlite_cfg.h` is not upstream source: it is a GENERATED ARTEFACT OF THE
# HARNESS'S OWN `./configure` RUN thirty lines up, in a build dir this harness
# created. Removing it removes THIS HARNESS'S OWN OUTPUT, and its content is not
# discarded — stage-zinc.py has just read it (above) and rewritten it into one
# per-target copy per stage.
# ★ AND IT COSTS NOTHING TO REGENERATE, ✔MEASURED: upstream's own Makefile carries
# `sqlite_cfg.h: $(AS_AUTO_DEF)` -> `$(AS_AUTORECONFIG)`, so a human re-running
# `make testfixture` by hand in $BLD (the workflow the LDFLAGS note at Step 4
# contemplates) regenerates it automatically and BYTE-IDENTICALLY — measured md5
# 24c26a7425fea820274d20945244915f before and after, `Makefile is unchanged`. This
# driver re-runs `./configure` unconditionally at the top of every run in any case.
# With it gone the self-directory lookup MISSES on every TU on every leg, the
# include list wins uniformly, and the SQLITECONFIG_H shadowing becomes harmless
# (ctime.c now defines it having read the RIGHT header). test-confound-scope.sh
# asserts the removal is still here.
rm -f "$BLD/sqlite_cfg.h"
info "removed the deriving host's $BLD/sqlite_cfg.h — ${#CFG_STAGE_DIR[@]} per-target copy/copies replace it; a quote include searches the includer's OWN dir first, so leaving it there let \$BLD/ctime.c (TU #1) read this machine's answers ahead of the whole include list"
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

# ── `pinned-archive` — the GENERAL, DECLARED library-acquisition route ───────
# [D-HARNESS-LIBRARY-ACQUISITION-BUILT-FOR-ONE-LEG-IN-ONE-DRIVER]
# `ensure_arm64_libs` above is a whole acquisition mechanism — fetch, extract,
# stage — that exists for ONE leg, in ONE driver, with the archive layout welded
# into this file. `pinned-archive` is the same idea DECLARED: the archives, their
# PINNED sha256, the members to take and the runtime identity to record all live
# in $LEG_CATALOGUE, and the fetch/verify/extract/slice is implemented ONCE in
# $LEG_RESOLVER (`--acquire`) so this driver and the .ps1 acquire identically
# instead of each growing its own. That is what makes "any leg builds on any
# host" true rather than aspirational: the macho legs used to declare
# `host-system`, i.e. "hope this box has a Darwin libtcl", which meant they built
# nowhere but a Mac.
#
# NOTHING HERE IS KEYED ON THE HOST. The provider is a property of the LEG.
#
# acquire_leg_libs <leg> — the resolver's acquisition report, as JSON, on stdout.
# Diagnostics go to STDERR (the caller redirects them to a log), so stdout is the
# report and nothing else. Called from a COMMAND SUBSTITUTION, which is a
# subshell — the same isolation `ensure_arm64_libs` gets from `( … )` and for the
# same reason: a leg whose archive cannot be fetched, whose digest does not
# match, or whose declared member is absent costs THAT LEG a
# `skipped-build-input-missing`, never the other four. rc is taken DIRECTLY off
# the substitution, never through a pipe.
#
# No `--cache-root`: WHERE the cache lives is a host fact the resolver already
# decides (`cache_root()`: $DSS_HARNESS_CACHE_ROOT, else ~/.cache/dss-code-prime),
# and a second opinion here is exactly the fork this route exists to delete.
acquire_leg_libs() {            # acquire_leg_libs <leg>  -> acquisition JSON
  local leg="$1"
  ensure_cmd python3 python3
  python3 "$LEG_RESOLVER" --catalogue "$LEG_CATALOGUE" --acquire "$leg"
}
# acq_field <json> cacheDir    -> the one string
# acq_field <json> libraries   -> one "<as>\t<path>" line per ACQUIRED library
# A READER, never a second opinion: every value it returns was decided by
# $LEG_RESOLVER. Each driver parses the report in its own language (the .ps1 uses
# ConvertFrom-Json); what neither driver is allowed to do is re-derive a value.
# ⚠ TAB-separated and read with `IFS=$'\t'`, because a cache path may contain
# spaces — the same reason `LEG_LIB_PATHS` is newline-separated.
#              <json> scriptLibraryDir -> the staged Tcl script library, or ""
# ★ `optional:<key>` IS A SEPARATE MODE FROM THE PLAIN READ, DELIBERATELY. A plain
# `report[key]` must KeyError on a field the contract guarantees — a driver that
# shrugs at a missing `cacheDir` would resolve libraries out of nowhere. But
# `scriptLibraryDir` is legitimately "" for a leg whose acquired library needs no
# runtime data, so it is read with a default and the CALLER decides what "" means.
# [D-HARNESS-ACQUIRED-TCL-DYLIB-HAS-NO-SCRIPT-LIBRARY]
acq_field() {                   # acq_field <json> <cacheDir|libraries|optional:<key>>
  python3 -c 'import json, sys
# ★ NO NEWLINE TRANSLATION. ✔MEASURED 2026-08-06 on Windows: Python opens stdout
# in TEXT MODE and writes \r\n for every \n, so a path read back through `$( )`
# keeps a trailing CR — `$(dirname "/cache/libtcl.dylib\r")` and every later
# comparison then work on a name no file has. Harmless on the POSIX hosts this
# driver usually runs on, which is exactly why it would surface first on the ONE
# host nobody had tried.
sys.stdout.reconfigure(newline="")
report = json.load(sys.stdin)
what = sys.argv[1]
if what == "libraries":
    for lib in report["libraries"]:
        sys.stdout.write("%s\t%s\n" % (lib["as"], lib["path"]))
elif what.startswith("optional:"):
    sys.stdout.write("%s\n" % (report.get(what.split(":", 1)[1], "") or ""))
else:
    sys.stdout.write("%s\n" % report[what])' "$2" <<< "$1"
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
      # D-HARNESS-SEARCH-PATHS-SKIP-REASON-IS-MALFORMED: the `}` in the fallback
      # text MUST be escaped. An unescaped one CLOSES the `${_paths[*]:-…}`
      # expansion early, so bash reads ` unset>}` as literal trailing text.
      # ✔MEASURED both ways — non-empty array: `a b unset>}` (garbage appended to
      # a correct list); empty array: `<none declared or all ${env:... unset>}`
      # (the fallback itself truncated mid-word). `\}` renders both correctly.
      searched="provider 'search-paths'; tcl names tried: ${_tnames[*]:-<none>}; zlib names tried: ${_znames[*]:-<none>}; paths searched: ${_paths[*]:-<none declared or all \${env:...\} unset>}"
      ;;
    pinned-archive)
      # DECLARED archives, PINNED digests, materialised by $LEG_RESOLVER — see
      # `acquire_leg_libs`. COMMAND SUBSTITUTION IS A SUBSHELL, so a failed fetch,
      # a checksum mismatch, an absent member or a missing architecture slice
      # costs THIS LEG and no other; rc is taken DIRECTLY off the `if`, never
      # through a pipe. The resolver's own (loud) diagnostic is kept, verbatim, in
      # a per-leg log and named in the skip reason — this driver never
      # paraphrases it and never falls back to "whatever else is on this machine".
      _acq_log="$OUT_DIR/acquire-$leg.log"; _acq_stage="--acquire"
      if _acq_json="$(acquire_leg_libs "$leg" 2>"$_acq_log")"; then _acq_rc=0; else _acq_rc=$?; fi
      declare -a _tnames=(); read -r -a _tnames <<< "${LEG_LIB_TCL_NAMES[$leg]}"
      declare -a _znames=(); read -r -a _znames <<< "${LEG_LIB_Z_NAMES[$leg]}"
      _acq_dir=""; _acq_libs=""
      if [[ "$_acq_rc" -eq 0 ]]; then
        # Reading the report can itself fail (a truncated or reshaped report), and
        # that must not read as "no library on this machine" — so its rc is taken
        # too, and it demotes the leg exactly like a failed acquisition.
        if _acq_dir="$(acq_field "$_acq_json" cacheDir)" \
           && _acq_libs="$(acq_field "$_acq_json" libraries)" \
           && [[ -n "$_acq_dir" && -n "$_acq_libs" ]]; then
          LEG_ACQ_DIR["$leg"]="$_acq_dir"; LEG_ACQ_LIBS["$leg"]="$_acq_libs"
          # ── THE SCRIPT LIBRARY THE ACQUIRED Tcl CANNOT RUN WITHOUT ──────────
          # [D-HARNESS-ACQUIRED-TCL-DYLIB-HAS-NO-SCRIPT-LIBRARY] — see the
          # LEG_TCL_SCRIPT_DIR note at the top of this file for the measurement.
          # Selected by ROLE out of the acquisition report; `run_leg` points
          # TCL_LIBRARY at it, leg-scoped, for THIS leg's child only.
          # ⚠ ABSENT IS REPORTED, NEVER ASSUMED BENIGN. A missing script library
          # does not fail the build and does not fail a directly-named .test file
          # — the MAIN interpreter is already initialised by the time one is
          # sourced, so `tclInit` is never re-entered. It kills the TIER, and only
          # the tier, because permutations.test runs every unit in a fresh SLAVE
          # interpreter. That asymmetry is exactly why this was invisible until a
          # tier ran, and why the absence is said OUT LOUD here rather than
          # discovered as an unnamed abort 11 resumes later.
          _acq_tcldir="$(acq_field "$_acq_json" "optional:$ACQ_SCRIPT_LIBRARY_KEY")" || _acq_tcldir=""
          if [[ -n "$_acq_tcldir" ]]; then
            LEG_TCL_SCRIPT_DIR["$leg"]="$_acq_tcldir"
            info "[$leg] Tcl script library (acquired): $_acq_tcldir  — TCL_LIBRARY is pointed here for THIS leg's children only"
          else
            warn "[$leg] the acquisition report stages NO Tcl script library (report field '$ACQ_SCRIPT_LIBRARY_KEY' is empty)."
            warn "      An acquired libtcl bakes in ITS PACKAGER'S script-library path, which does not exist on this"
            warn "      machine. Individual .test files will still run; the TIER will abort at the first"
            warn "      \`interp create\` with \"Can't find a usable init.tcl\" and NO unit will get a verdict."
            warn "      D-HARNESS-ACQUIRED-TCL-DYLIB-HAS-NO-SCRIPT-LIBRARY — the fix belongs in the leg's"
            warn "      \`pinned-archive\` declaration + $(basename "$LEG_RESOLVER"), not here."
          fi
          # The (tcl, z) pair is picked out of the acquired cache with the leg's
          # OWN declared names, by the same helper the other providers use — the
          # driver never decides which acquired file is the Tcl one from its shape.
          tcl_lib="$(find_first_in "$_acq_dir" -- ${_tnames[@]+"${_tnames[@]}"})"
          z_lib="$(find_first_in "$_acq_dir" -- ${_znames[@]+"${_znames[@]}"})"
        else
          # NAMED SEPARATELY from a failed acquisition: `--acquire` returned 0 and
          # the archives really are on disk, so a skip reason that blamed the
          # download would send the next reader to the wrong place.
          _acq_rc=$?; _acq_dir=""; _acq_stage="reading the --acquire report"
          warn "[$leg] the acquisition report from $(basename "$LEG_RESOLVER") could not be read (rc=$_acq_rc)"
          printf '%s\n' "$_acq_json" | sed 's/^/      /' >&2
        fi
      else
        [[ ! -s "$_acq_log" ]] || sed 's/^/      /' "$_acq_log" >&2
      fi
      searched="provider 'pinned-archive' ($_acq_stage rc=$_acq_rc); tcl names tried: ${_tnames[*]:-<none>}; zlib names tried: ${_znames[*]:-<none>}; acquired under: ${_acq_dir:-<nothing acquired — see $_acq_log>}"
      ;;
    *)
      die "[$leg] declares library provider '$provider', which this driver does not implement.
      Known: host-system | ubuntu-ports-arm64 | search-paths | pinned-archive (see $LEG_CATALOGUE and
      LIBRARY_PROVIDERS in $LEG_RESOLVER). A provider the driver silently ignored would
      resolve to an empty library pair and read as a missing input — so it fails loud here."
      ;;
  esac
  [[ -z "$tcl_lib" ]] || LEG_TCL_LIB_ANY["$leg"]="$tcl_lib"
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

# ── THE FOURTH Tcl COHERENCE CHECK — AND THE ONLY PER-LEG ONE ────────────────
# [D-HARNESS-TCL-HEADER-IS-HOST-CHOSEN-WHILE-EVERY-LEG-LIBRARY-IS-PINNED]
#
# The three checks above — `PINNED Tcl skew after Step 4` (interpreter-vs-
# staging), `Tcl staging is INCOHERENT` (header-vs-tclConfig) and `RECIPE/STAGING
# Tcl MISMATCH` (recipe-vs-staging), named by their diagnostics rather than by
# line numbers that go stale — are ALL HOST-SCOPED: they hold this machine's Tcl
# installation to account against itself. Not one of them compares the staged
# header against the library a LEG WILL ACTUALLY LINK — and that is the
# comparison that matters, because the header above is the ONE Tcl input this
# harness still takes from the HOST while every leg's library is pinned by its
# TARGET-keyed provider.
#
# ✔MEASURED 2026-08-06, first native macOS run: a 9.0 header (that host's default
# Homebrew tcl-tk) over the pinned 8.6 libraries produced four K_SymbolUndefined
# — Tcl_GetBool, Tcl_GetBoolFromObj, Tcl_GetBytesFromObj, Tcl_GetChild — because
# sqlite's tclsqlite.c gates live code on TCL_MAJOR_VERSION>8. On Linux the host
# tclsh is 8.6, so header and library had agreed BY ACCIDENT OF THE HOST.
#
# ★ FATAL, NEVER A WARN. A warn here builds a binary that links clean and then
# misbehaves, which is the exact class this harness exists to prevent. A leg
# whose library version cannot be MEASURED is the one soft outcome, and it is a
# loud warning naming that leg — never a silent pass.
#
# ★ THE COMPARISON LIVES IN $LEG_RESOLVER, NOT HERE — the same argument
# --translate-path and --resolve-library-argv already make. build-and-test.ps1
# calls the identical verb, so this capability cannot exist in one driver and
# not the other [D-HARNESS-LIBRARY-ACQUISITION-BUILT-FOR-ONE-LEG-IN-ONE-DRIVER].
# It reads the library's BYTES (export table + self-declared identity), never the
# file name: `libtcl8.6.so` is what somebody CALLED the file, and a name being
# trusted is what this whole anchor is about.
declare -a _tclcoh=(); _tclcoh_n=0
for leg in "${LEG_ORDER[@]}"; do
  # LEG_TCL_LIB_ANY, not LEG_TCL_LIB: a leg whose zlib is missing is not BUILT,
  # but its libtcl is still evidence about the run's one host-chosen input, and
  # a skew it witnesses is a skew every other leg has too.
  [[ -n "${LEG_TCL_LIB_ANY[$leg]:-}" ]] || continue
  _tclcoh+=(--leg-tcl-library "$leg=${LEG_TCL_LIB_ANY[$leg]}"); _tclcoh_n=$((_tclcoh_n + 1))
done
if [[ "$_tclcoh_n" -gt 0 ]]; then
  # rc DIRECTLY off the command substitution, never after a pipe. stderr is
  # merged because the refusal IS the diagnostic and losing it would leave the
  # operator with a bare exit code.
  if _tclcoh_out="$(python3 "$LEG_RESOLVER" --catalogue "$LEG_CATALOGUE" \
                      --tcl-coherence --staged-tcl-header "$TCL_INC/tcl.h" \
                      "${_tclcoh[@]}" 2>&1)"; then _tclcoh_rc=0; else _tclcoh_rc=$?; fi
  if [[ "$_tclcoh_rc" -ne 0 ]]; then
    die "Tcl HEADER/LIBRARY COHERENCE FAILED (rc=$_tclcoh_rc) — refusing to build.
$(printf '%s\n' "$_tclcoh_out" | sed 's/^/      /')
      The staged headers come from THIS HOST ($TCL_INC); every leg's library comes from its own
      declared provider. This driver will not compile a fixture against one Tcl and link another."
  fi
  printf '%s\n' "$_tclcoh_out" | sed 's/^/      /'
  info "tcl coherence: the staged $TCL_VER headers match the libtcl of all $_tclcoh_n resolved leg(s) — measured from each library's OWN bytes, not its file name"
fi

# ── each leg's TARGET C compiler — THE CONTROL ARM, NOT A REQUIREMENT ────────
#
# ★★★ READ THIS BEFORE THE PARAGRAPHS BELOW IT, WHICH DESCRIBE A RULE THAT IS
# STILL TRUE AND A ROLE THAT HAS CHANGED. Since 2026-08-05 the loadext helper
# extension is built by DSS ITSELF, for the leg's declared `sharedLibFormat`, on
# whatever host this driver is running on — so NO leg's run depends on this
# machine owning a third-party cross-compiler any more.
# Operator: "why do we need mingw? since dss code prime should not have
# dependencies?" / "I installed mingw in wsl, but we should NOT depend on a tool".
# [D-HARNESS-CROSS-HOST-ANY-TARGET]
#
# WHAT THIS BLOCK STILL DOES, AND WHY IT IS WORTH KEEPING. A verified target
# compiler is now the CONTROL arm: where one exists, the same source is ALSO
# built with it beside the DSS artefact, and `DSS_LOADEXT_HELPER=reference`
# STAGES it instead so the corpus becomes a differential. That control matters
# precisely because DSS is now testing DSS — the fixture and the helper both
# compile sqlite3ext.h, so a shared defect could cancel and read as green.
#
# ⇒ THE THREE THINGS THAT CHANGED HERE, each of them a de-host-locking:
#   1. A leg with NO verified compiler is no longer `skipped-build-input-missing`.
#      It records nothing at all — its helper comes from DSS and its corpus runs.
#      That single change is what un-skips pe64 on a Linux/WSL box without mingw,
#      the arm64 leg without gcc-aarch64-linux-gnu, and both macho legs anywhere.
#   2. `pkg_install` is no longer attempted UNASKED. Installing a cross-compiler
#      to satisfy a CONTROL would mutate the machine for an arm that is optional
#      by construction; it is attempted only when the operator has explicitly
#      selected `DSS_LOADEXT_HELPER=reference`.
#   3. The resolution is best-effort. Its failure is INFORMATION (printed, and
#      carried into the leg's cross-check line), never a verdict.
#
# The RULE the resolver applies is unchanged and still load-bearing: a candidate
# is accepted only if `<cc> -dumpmachine` names this leg's own target arch AND
# OS. A control built by the wrong-target compiler would be a worthless control.
# ⚠ The old `${CC:-cc}` override is deliberately NOT carried over: with five
# legs, one `$CC` cannot say WHICH leg's target compiler it means, and applying it
# to a cross leg is exactly the wrong-arch helper this anchor
# (D-HARNESS-ARM64-LEG-HOST-ARCH-HELPER-SO) exists to prevent. `$CC` still governs
# PROBE_CC, which is a genuine host question.
#
# ★★ "FIRST PRESENT WINS" WAS THE DEFECT, AND IT IS NOT WHAT THIS DOES ANY MORE.
# D-HARNESS-LOADEXT-HELPER-TARGET-BLINDNESS-NOW-ABORTS-THE-RUN. ✔MEASURED
# 2026-08-05, a full run on a WSL/Ubuntu x86_64 host: legs.json declared pe64's
# candidates as ["x86_64-w64-mingw32-gcc", "gcc"], the mingw cross-compiler was
# absent, and "first present wins" therefore picked the plain LINUX `gcc` to build
# a WINDOWS leg's shared object — legitimately, because the catalogue had listed
# it. The guard 30 lines below (and the one in stage_loadext_extension) forbid
# exactly that fallback in so many words; neither fired, because they were
# watching for a fallback the CONFIG had already licensed. The run died in
# /usr/bin/ld ("relocation R_X86_64_PC32 against symbol `sqlite3_api` ...
# recompile with -fPIC") after two legs had gone green, and pe64's units never ran.
#
# THE RESOLVER NOW ASKS THE COMPILER. `harness_legs.py --resolve-target-cc <leg>`
# accepts a candidate only when `<cc> -dumpmachine` names this leg's own target
# arch AND OS, both derived from the leg's declared `spec`. It lives in the PYTHON
# and not here for the reason base-harness.sh's header gives: a decision both
# drivers need is written once, in the file they already share, not twice in two
# shells. ✔MEASURED after the fix — WSL: pe64 REFUSED ("targets OS 'linux' (triple
# 'x86_64-linux-gnu'); this leg needs 'windows'"), elf64-x86_64 -> cc, elf64-arm64
# -> aarch64-linux-gnu-gcc; Windows: pe64 -> x86_64-w64-mingw32-gcc.
LEG_CC_WHY=""
resolve_leg_target_cc() {       # resolve_leg_target_cc <leg>  -> 0 + LEG_CC set
  local leg="$1" _err _out _rc
  # stderr to its OWN file: it is the candidate ladder, and mixing it into the
  # captured stdout would put prose where the driver reads a compiler name.
  #
  # ★ A PER-LEG REFUSAL, NOT A `die` (TF-C120). This function is called from
  # inside the per-leg loop below, so an `exit` here would cost the OTHER legs
  # their verdicts over one leg's temp file — structurally the same shape
  # D-HARNESS-LOADEXT-HELPER-TARGET-BLINDNESS-NOW-ABORTS-THE-RUN was opened to
  # end, and against the standing rule that the harness must SURVIVE everything
  # (one bad unit must never cost us the other thousand; every unit gets a
  # verdict). Low trigger probability — but the shape, not the odds, is what
  # that anchor is about, and the new self-test does not cover this arm.
  # It reuses this function's OWN refusal mechanism, the same one both failure
  # exits below use — set LEG_CC_WHY, leave LEG_CC[$leg] unset, return non-zero —
  # so the caller reports "no CONTROL compiler here" and CARRIES ON.
  # ⚠ CORRECTED 2026-08-06 (D-HARNESS-UNITS-SKIP-A-LEG-WHOSE-LAUNCHER-IT-SAYS-IS-
  # AVAILABLE): this note used to say the caller "records
  # `skipped-build-input-missing` for THIS leg and CONTINUES" via
  # `leg_marks_missing`. That STOPPED BEING TRUE when the control became optional
  # — the caller's branch now records NO verdict in either direction, by design —
  # and a comment asserting a verdict that is not set is how the empty `not run
  # []` token survived review. An absent control costs the leg NOTHING now: the
  # loadext helper comes from DSS, and Step 8 runs the corpus regardless.
  # rc 5 only to sit apart from the resolver's own rc and from the `return 4`
  # below; every caller tests truthiness, never the value.
  # The `if` (never `_err="$(mktemp)"; rc=$?`) keeps errexit out of it — the
  # assignment-then-`$?` shape would EXIT before the rc could be read, the same
  # trap the resolver call below documents.
  if ! _err="$(mktemp)"; then
    LEG_CC_WHY="mktemp could not create a temp file for the target-cc probe's stderr (TMPDIR='${TMPDIR:-<unset>}'), so the candidate ladder was never captured and no compiler could be resolved for this leg"
    unset "LEG_CC[$leg]"
    return 5
  fi
  # rc DIRECTLY off python3, never after a pipe, and the `if` keeps errexit out
  # of it — the `_out=$(...); rc=$?` shape would EXIT here on the refusal this
  # function exists to report.
  # ★ CITED BY PREDICATE, NOT BY LINE. This comment used to point at `:3690`
  # (the trap that made a whole classifier dead code) and `:3561` (the correct
  # idiom); ✔MEASURED 2026-08-05 (TF-C120), BOTH were already wrong before the
  # guard above shifted them further — `:3561` was `else` and `:3690` an
  # unrelated subshell comment even at HEAD. Grep the spellings instead:
  #   · the trap, written out in full — grep `is LOAD-BEARING, not style`
  #     (the CLI build's `|| _rc=$?` note: a plain `x="$(fn)"; _rc=$?` under
  #     `set -Eeuo pipefail` + ERR trap exits before the rc is ever read).
  #   · what it COST — grep `DEAD CODE` (the per-leg CLI classifier, which the
  #     early exit skipped entirely for every failing leg).
  #   · the correct idiom is the `if`/`else _rc=$?` two lines below, and the
  #     leg plan's `_plan_rc` capture.
  if _out="$(python3 "$LEG_RESOLVER" --catalogue "$LEG_CATALOGUE" \
                --resolve-target-cc "$leg" 2>"$_err")"; then _rc=0; else _rc=$?; fi
  # Flattened to one line: it becomes a ledger DETAIL, which Step 9 prints per leg.
  LEG_CC_WHY="$(tr '\n' ' ' < "$_err" 2>/dev/null || true)"; rm -f "$_err"
  [[ "$_rc" -eq 0 ]] || return "$_rc"
  # `<cc>\t<triple>` — TAB-separated for the same reason acq_field is: neither
  # field may be re-derived here, and a compiler PATH can contain spaces.
  LEG_CC["$leg"]="${_out%%$'\t'*}"
  LEG_CC_MACHINE["$leg"]="${_out#*$'\t'}"
  [[ -n "${LEG_CC[$leg]}" ]] || {
    LEG_CC_WHY="the resolver exited 0 but named no compiler (output: '$_out')"
    unset "LEG_CC[$leg]"; return 4
  }
  return 0
}
# WHICH ARM STAGES THE HELPER. Resolved ONCE, here, by the shared resolver so
# both drivers read the operator's choice the same way and an unrecognised value
# is refused by name rather than silently read as the default (which would report
# a control that never ran). `--helper-builder ''` means "take $DSS_LOADEXT_HELPER
# or the default"; the resolver owns both the vocabulary and the precedence.
# `|| die`, never `X="$(...)"; rc=$?` — under `set -Eeuo pipefail` the assignment
# form EXITS before the rc can be read, which is the trap that once shipped a
# whole verdict classifier as dead code. The resolver's own refusal (rc 2, with
# the full vocabulary and the reason) has already gone to stderr.
LOADEXT_BUILDER="$(python3 "$LEG_RESOLVER" --catalogue "$LEG_CATALOGUE" --loadext-builder)" \
  || die "DSS_LOADEXT_HELPER='${DSS_LOADEXT_HELPER:-}' is not a builder this harness implements — see the refusal above."
if [[ "$LOADEXT_BUILDER" == "reference" ]]; then
  warn "DSS_LOADEXT_HELPER=reference — the loadext helper will be STAGED from each leg's"
  warn "      VERIFIED target C compiler instead of from DSS. This is the CONTROL arm: it makes"
  warn "      the corpus itself the differential for 'is a loadext-* red the fixture or the helper?',"
  warn "      and it re-introduces a host dependency ON PURPOSE — a leg with no such compiler here"
  warn "      records skipped-build-input-missing (environmental; the default would have run it)."
fi
for leg in "${LEG_ORDER[@]}"; do
  # A leg this host cannot RUN needs no helper extension — not even a control.
  [[ "${LEG_RUN_MODE[$leg]}" != "skip" ]] || continue
  # A leg with no libraries is not built, so there is no run to stage for it.
  [[ -n "${LEG_TCL_LIB[$leg]:-}" ]] || continue
  # ★ THE INSTALL IS ONLY ATTEMPTED FOR AN ARM THE OPERATOR ASKED FOR. Installing
  # a cross-compiler to satisfy the optional control would mutate this machine for
  # something that is optional by construction — and it is exactly the "we should
  # NOT depend on a tool" the primary path now removes.
  if ! resolve_leg_target_cc "$leg" \
     && [[ "$LOADEXT_BUILDER" == "reference" && -n "${LEG_CC_PKG[$leg]}" ]]; then
    warn "[$leg] DSS_LOADEXT_HELPER=reference and no candidate compiler on PATH targets ${LEG_SPEC[$leg]} — trying to install ${LEG_CC_PKG[$leg]}"
    info "      ${LEG_CC_WHY}"
    # Subshell: a package manager that cannot supply it costs this leg its RUN,
    # never the whole harness.
    ( pkg_install "${LEG_CC_PKG[$leg]}" ) || true
    hash -r    # this shell's own command hash; the resolver spawns fresh and
               # reads $PATH itself, so this is for the rest of THIS driver.
    resolve_leg_target_cc "$leg" || true
  fi
  # ★★ NO VERDICT IS RECORDED HERE ANY MORE, IN EITHER DIRECTION. This branch used
  # to call `leg_marks_missing` — the single line that made a leg's ~330,000 units
  # depend on this host owning a cross-compiler for that leg's target. The helper
  # now comes from DSS (Step 8), so an absent control costs the leg NOTHING; it is
  # reported as the fact it is. The verdict for the operator-selected `reference`
  # arm is decided at Step 8, by the shared resolver, from the report it returns —
  # in ONE place, with the reason attached [D-HARNESS-CROSS-HOST-ANY-TARGET].
  if [[ -z "${LEG_CC[$leg]:-}" ]]; then
    # `:-` on a key the resolver ALWAYS emits: under `set -u` an absent key aborts
    # the driver, and this branch is already reporting a degraded state — it must
    # not be the thing that ends the run. (The lint at Step 0 requires
    # `sharedLibFormat` on every leg, so the fallback text should be unreachable.)
    info "[$leg] no CONTROL compiler here (tried ${LEG_CC_CANDIDATES[$leg]:-<none declared>}) — the loadext helper will be built by DSS for ${LEG_SHARED_LIB_SPEC[$leg]:-<no sharedLibFormat declared>}, which needs nothing from this machine."
    [[ -z "${LEG_CC_WHY// /}" ]] || info "      the resolver's ladder: ${LEG_CC_WHY}"
    continue
  fi
  info "[$leg] control cc: ${LEG_CC[$leg]} — it reports '${LEG_CC_MACHINE[$leg]}', which is ${LEG_SPEC[$leg]}'s arch+OS (asked, not assumed)"
  [[ -z "${LEG_CC_WHY// /}" ]] || info "      candidates passed over: ${LEG_CC_WHY}"
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
# >>> dss:corpus-engine >>>  (region mirrored in build-and-test.ps1)
# ⚠ CORRECTED 2026-08-06: this header used to say "the verifier extracts it from
# this file by these sentinels". THERE IS NO SUCH VERIFIER. ✔MEASURED — the only
# consumers of any `dss:` sentinel in this repository are test-confound-scope.sh
# (`src-provenance`, `src-clone`, `src-gate`, `loadext-stage`, `loadext-verdict`,
# `confound-supply`), test-confound-scope.ps1 (`src-provenance`,
# `loadext-stage-ps1`) and test-driver-contracts.sh (`verdict-vocabulary`);
# `corpus-engine` is read by NOTHING. The sentinels are still worth keeping — they
# mark the paired region for a reader and for the verifier that should exist — but
# a comment asserting a guard that is not there is the exact defect this project
# keeps paying for: an instrument credited with an observation it never made.
# ⇒ nothing about this region is enforced today; the two copies can diverge
# silently, and the confound classifier's SUPPLY (which lives outside it) did
# exactly that for months. Keep the sentinels on their own lines.

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
#   B <path>  the SOURCE FILE a Tcl traceback blames, once per traceback
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
    # ── THE FIRST DIAGNOSTIC LINE ────────────────────────────────────────────
    # [D-HARNESS-ACQUIRED-TCL-DYLIB-HAS-NO-SCRIPT-LIBRARY, second half.]
    # (NB no apostrophes in this block either: it lives inside a single-quoted awk
    # program, and one apostrophe ENDS that program mid-rule. Measured the hard way
    # while writing this very rule.)
    # MEASURED: the harness reported "the UNNAMED file that aborted ... the log
    # named no resolvable corpus file (last test: none)" ELEVEN TIMES while the
    # FIRST LINE of the captured log read "Can not find a usable init.tcl in the
    # following directories: /opt/local/lib/tcl8.6 ..." (verbatim, with the
    # contraction restored, in the PRECONDITION FAILURE branch below). It had the
    # diagnosis in hand and did not surface it.
    # `A` is the first non-blank line that is NOT the fixture doing its job — not a
    # per-test line, not a `Time:` line, not an ` Ok`, not a summary. It is
    # CONSULTED ONLY on the zero-progress abort path below, so it can never
    # mislabel a healthy segment; captured here because this is the ONE streaming
    # pass over a log that can reach 150 MB / 3.6M lines.
    # FIRST, before every `next` in this program — a rule placed lower would never
    # see a line an earlier rule consumed.
    { if (diag == "" && $0 ~ /[^ \t]/ \
          && $0 !~ /^Time: / && $0 !~ / Ok$/ && $0 !~ /^[^ \t]+\.\.\./ \
          && $0 !~ /[0-9]+ errors? out of [0-9]+ tests/) {
        diag = $0
        gsub(/\t/, " ", diag)                 # the fact file is TAB-separated
        if (length(diag) > 400) diag = substr(diag, 1, 400) " …[truncated]"
      } }
    # ── THE FILE THE TCL TRACEBACK BLAMES ────────────────────────────────────
    # [D-HARNESS-ABORT-FILE-NAMED-ONLY-BY-THE-TRACEBACK.]
    # ✔MEASURED 2026-08-06, pe64-x86_64 under the wine launcher, and it cost a
    # unit its verdict TWICE IN ONE RUN in two different ways:
    #   · corpus.log completed `Time: symlink.test 26 ms` and then died inside
    #     symlink2.test. The last test NAME it had emitted was
    #     `symlink.test-sharedcachesetting`, so the name-based resolver below
    #     answered symlink.test — A FILE THAT HAD ALREADY COMPLETED AND BEEN
    #     COUNTED — and the run reported "the REMAINDER of symlink.test" as not
    #     run while symlink2.test, the file that actually died, went unnamed.
    #   · the resume segment then re-entered symlink2.test, died at its line 48
    #     BEFORE `do_test` printed a single `name...` line, and with T empty the
    #     resolver had no input at all: "the UNNAMED file that aborted ... (last
    #     test: none)", boundary FORCED, symlink2.test skipped without a verdict.
    # In BOTH logs the file was named, in plain text, by the Tcl traceback:
    #     (file "Z:/home/rafael/src/sqlite/test/symlink2.test" line 48)
    # Nothing read it. The last test NAME is a PROXY for the aborting file; the
    # traceback is the fixture SAYING it, so B is preferred over T at the use
    # site and T stays as the fallback for a KILLED segment, which has no
    # traceback at all.
    # ★ ONE B PER TRACEBACK, and it is the INNERMOST frame: Tcl prints errorInfo
    # innermost-first, so the first `(file …)` of a block is the unit that died
    # and the later ones are permutations.test / veryquick.test driving it. The
    # flag is cleared by any line of NORMAL fixture output (a `Time:`, an ` Ok`,
    # a `name...`), so a block emits exactly one frame and a later traceback
    # emits its own. `fact B` returns the LAST, i.e. the innermost frame of the
    # last traceback — the one the process died in.
    # NOTE no `next`: this rule is purely ADDITIVE and must not consume a line
    # any existing rule below would otherwise have counted.
    { if ($0 ~ /^Time: / || $0 ~ / Ok$/ || $0 ~ /^[^ \t]+\.\.\./) tbseen = 0 }
    match($0, /\(file "[^"]*" line [0-9]+\)/) {
      if (!tbseen) {
        tbseen = 1
        blame = substr($0, RSTART, RLENGTH)
        sub(/^\(file "/, "", blame); sub(/" line [0-9]+\)$/, "", blame)
        gsub(/\t/, " ", blame)                # the fact file is TAB-separated
        if (blame != "") print "B\t" blame
      } }
    # ── COMPLETED IS NOT THE SAME AS COVERED ────────────────────────────────
    # `pend` counts the result lines this file has emitted that are not the two
    # the harness emits for EVERY file (`<f>.test-closeallfiles... Ok` and
    # `<f>.test-sharedcachesetting... Ok`). A file whose pend is 0 at its
    # `Time:` line ran nothing: it returned at an `ifcapable` gate.
    # ✔MEASURED 2026-08-06 before this existed: 362 of 1,241 files, 29% of a
    # corpus reported entirely as "completed".
    # ⚠ The exclusion is BY THE FULL TEARDOWN NAME SHAPE, not by a suffix: these
    #   lines end in `...` with NO space before it, so $1 is
    #   `fts5aa.test-closeallfiles...` — an anchored `-closeallfiles$` match
    #   silently never fires, which is exactly how a first cut of this counter
    #   reported 0 inert files and agreed with nothing.
    /^Time: / { if (NF==4 && $4=="ms") {
        print "F\t" $2; nf++; lastdone=$2
        if (pend == 0) { print "I\t" $2; ni++ }
        pend = 0
        next } }
    /^\*\*\* Giving up/ { gaveup=1; next }
    /^!?Failures on these tests:/ {
      line=$0; sub(/^!?Failures on these tests:[ \t]*/, "", line);
      n=split(line, a, /[ \t]+/); for (i=1;i<=n;i++) if (a[i]!="") print "X\t" a[i]; next }
    / Ok$/                     { ok++
                                 if ($1 !~ /\.test-closeallfiles\.\.\.$/ &&
                                     $1 !~ /\.test-sharedcachesetting\.\.\.$/) pend++ }
    # one `expected:` per FAILED test (`got:` is its partner line) — the failure
    # tally that pairs with `ok` to reconstitute the count sqlite itself reports,
    # for a segment that aborted before printing a summary.
    # (NB no apostrophes in this block: it lives inside a single-quoted awk program.)
    /^! [^ ]+ expected:/       { fx++; pend++ }
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
          if (diag!="")    print "A\t" diag
          print "N\t" nf+0; print "D\t" lastdone
          print "M\t" ni+0
          print "K\t" ok+0; print "Q\t" fx+0 }
  ' "$1" > "$2"
}
fact() { LC_ALL=C awk -F'\t' -v k="$1" '$1==k{v=$2} END{print v}' "$2"; }
facts() { LC_ALL=C awk -F'\t' -v k="$1" '$1==k{print $2}' "$2"; }
# ── THE ZERO-PROGRESS SIGNATURE ─────────────────────────────────────────────
# [D-HARNESS-PRECONDITION-DISCRIMINATOR-BLIND-TO-A-SILENT-CRASH.]
#
# What must CHANGE between two consecutive segments for another resume to be
# worth attempting. The PRECONDITION FAILURE branch below compares this answer
# against the previous zero-file segment's; empty means "keep resuming".
#
# ✔MEASURED 2026-08-10, one Windows run, two legs, same commit, same root cause —
# and this is the A/B that motivated the helper:
#   · elf64-arm64 ran under qemu, which PRINTS `qemu: uncaught target signal 11
#     (Segmentation fault) - core dumped`. The discriminator fired on the second
#     segment and the remaining resume budget was NOT spent.
#   · elf64-x86_64 ran natively and died SILENTLY: every corpus*.log was 0 bytes
#     and every facts file held only N/D/M/K/Q — no `A` fact at all. The old
#     condition also required `-n "$s_diag"`, so it could never be satisfied: the
#     leg burned all 10 resumes and reported `11 fixture ABORT(s)` with every
#     abort unnameable and 12 unit groups NOT REACHED.
# THE LEG WHOSE CRASH TALKS WAS HANDLED; THE LEG WHOSE CRASH IS SILENT WAS NOT.
# A silent crash is strictly worse than a talking one, and the `-n` conjunct had
# quietly assumed its own best case (a fixture whose first line says what is
# wrong). So a segment that produced NOTHING gets a SENTINEL, which compares
# equal to the next such segment exactly as two identical diagnostics do.
#
# ★ "NOTHING" IS THE PARSE FINDING NOTHING, NOT MERELY THE DIAGNOSTIC BEING
# EMPTY, and the distinction is what keeps the resilience rule intact. `A` is
# also empty for a segment that ran TESTS without completing a FILE (its ` Ok`
# lines and `name...` lines are fixture output, which the A rule excludes by
# design) — that segment made progress, its resume boundary advances off the T
# fact, and it must stay on the ordinary resume path. So the sentinel is returned
# only when the diagnostic, the ` Ok` tally, the failure tally and the last test
# name are ALL empty: the fixture said nothing whatsoever.
zero_progress_signature() { # zero_progress_signature <diag> <ok-lines> <fail-markers> <last-test>
  if [[ -n "${1:-}" ]]; then printf '%s' "$1"; return 0; fi
  if [[ "${2:-0}" -eq 0 && "${3:-0}" -eq 0 && -z "${4:-}" ]]; then
    printf '%s' '<SILENT: the fixture produced no diagnostic, no test result and no test name>'
  fi
}
# Which corpus FILE was the fixture inside when it died? Two things can name it,
# and this resolver takes EITHER: a qualified test NAME (the T fact) or the SOURCE
# PATH a Tcl traceback blames (the B fact). Pick the corpus stem occurring
# RIGHTMOST in it on delimiter boundaries (rightmost, then longest).
# `inmemory_journal.swarmvtabfault-1.1-oom-persistent.143` -> swarmvtabfault.test
# (not swarmvtab.test: the 'f' after it is not a delimiter; not the leading
# permutation token: it is left of it).
#
# ★★ THE DIRECTORY PREFIX IS DISCARDED FIRST, ON EITHER SEPARATOR, AND THAT IS
# WHAT MAKES THIS NAMESPACE-AGNOSTIC. The corpus list this matches against is a
# list of BASENAMES — corpus_files() built it with `${f##*/}` — so the question
# "which corpus file is this" is a question about the last path component and
# nothing else. Whose namespace the prefix belongs to is then IRRELEVANT, which
# is the property we want: the same log carries BOTH
#     (file "Z:/home/rafael/src/sqlite/test/symlink2.test" line 48)     <- launcher
#     (file "/home/rafael/src/sqlite/test/veryquick.test" line 16)      <- driver
# because wine spells a path it resolved itself on its own Z: drive while the
# driver-supplied argv comes back verbatim. NEITHER resolved before this change
# (✔MEASURED: both answered EMPTY, so this is not a wine quirk — a plain POSIX
# path did not resolve either, because `/` is not one of the `.`/`-` delimiters
# the stem matcher accepts).
# ⇒ NO path TRANSLATION is performed and none is declared. A `pathTranslation`
# verb would be the wrong instrument twice over: it is defined for the ARGV
# direction (how a launcher must be HANDED a path), and no single verb could be
# right for a log that carries two namespaces at once. Reducing to the granularity
# the corpus list is already in answers both without knowing either.
#
# ★ THE NAME ARRIVES THROUGH THE ENVIRONMENT, NOT `awk -v`. ✔MEASURED on this
# host, gawk 5.3.2: `awk -v n='Z:\home\rafael\test'` yields `Z:homeafael<TAB>est`
# — -v runs ESCAPE PROCESSING over its value, so `\h` collapses and `\t` becomes a
# tab. That was harmless while this only ever saw test names; the moment it can
# be handed a PATH it silently corrupts the input. ENVIRON is byte-exact (measured
# in the same probe), and it is the same reason our_fixture_pids uses it.
resolve_abort_file() {         # resolve_abort_file <name-or-path> <corpus-list-file>
  [[ -n "$1" ]] || return 0
  DSS_ABORT_NAME="$1" LC_ALL=C awk '
    BEGIN { name = ENVIRON["DSS_ABORT_NAME"]
            sub(/^.*\//, "", name); sub(/^.*\\/, "", name) }
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
# The FIRST basename after <boundary>, done inside awk rather than `files_after | head -1`.
# ★ MEASURED 2026-08-06 (TF-C124): that pipeline returns **141 (SIGPIPE)** whenever there is
#   more than one match — `head` closes the pipe, and under this driver's `set -Eeuo pipefail`
#   the non-zero pipeline status is FATAL. It killed a real run inside the resume path, i.e.
#   inside the very code whose job is to SURVIVE an abort.
# ⚠ THE SHAPE IS WHY IT HID SO LONG: it fails on the NORMAL case (matches exist) and returns
#   0 on the EMPTY one (no matches). So it could only fire once a leg actually needed a
#   SECOND resume — every earlier run either resumed once or not at all.
# ⇒ `exit` after the first hit: no pipe, so no SIGPIPE, and the intent is stated rather than
#   implied by a downstream `head`.
first_file_after() { LC_ALL=C awk -v b="$1" '$0 > b { print; exit }' "$2"; }
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
# ── THE RUNTIME LOADER'S SEARCH VARIABLE, PER TARGET ────────────────────────
# [D-HARNESS-RUN-ENV-LD-LIBRARY-PATH-INERT-ON-DARWIN, and the second site the
#  D-HARNESS-ACQUIRE-ERGONOMIC-GAPS row (c) names — one place, target-keyed, as
#  that row asked.]
#
# There is no such thing as "the" loader search variable. It is a property of the
# TARGET's loader, and the leg declares its target: ld.so reads LD_LIBRARY_PATH,
# dyld reads DYLD_LIBRARY_PATH and IGNORES the ELF one, and the Windows loader
# reads neither.
#
# THE OS COMES FROM THE RESOLVED PLAN, NOT FROM A SPLIT DONE HERE.
# `LEG_CONFIG_STAGE_KEY` is `spec_target_os(spec)` — harness_legs.py's
# `configure_stage_key` docstring: "the staged-configure-header directory name for
# this leg: its TARGET OS", and it RAISES rather than defaulting, so `--plan`
# cannot deliver an unknown one. The format token is CROSS-CHECKED against it
# rather than used as the source: same declare-then-cross-check discipline as
# zconfGuards. ⚠ The name is about configure staging, so this reuse is worth
# stating: the value is the target OS, but the RIGHT long-term home is a declared
# `LEG_TARGET_OS` (better: a declared loader-variable name) emitted by the shared
# resolver, so neither driver holds this table.
leg_loader_path_var() {        # leg_loader_path_var <leg> -> the env var NAME, or ""
  local _leg="$1"
  local _os="${LEG_CONFIG_STAGE_KEY[$_leg]:-}"
  local _fmt="${LEG_FORMAT[$_leg]:-}"
  # `<container><bits>-<arch>-<os>-<kind>`: the OS is the second-to-last token.
  local _fmt_os=""
  case "$_fmt" in
    *-*-*) _fmt_os="${_fmt%-*}"; _fmt_os="${_fmt_os##*-}" ;;
  esac
  [[ -n "$_os" ]] || die "[$_leg] the resolved plan carries no target OS (LEG_CONFIG_STAGE_KEY is empty).
      The runtime loader's search variable is a property of the TARGET; choosing one without
      knowing the target is how LD_LIBRARY_PATH came to be exported for a Darwin leg that
      cannot read it (D-HARNESS-RUN-ENV-LD-LIBRARY-PATH-INERT-ON-DARWIN)."
  [[ "$_os" == "$_fmt_os" ]] || die "[$_leg] the resolved plan disagrees with itself about this leg's target OS:
      configStageKey says '$_os', the object format '$_fmt' says '$_fmt_os'.
      One of them decides which loader variable this leg's libraries are exported under, and a
      run that guesses which would silently export a variable the target's loader ignores."
  case "$_os" in
    linux)   printf '%s' 'LD_LIBRARY_PATH' ;;
    darwin)  printf '%s' 'DYLD_LIBRARY_PATH' ;;
    windows)
      # A DECLARED EMPTY, NOT A FALL-THROUGH. The Windows loader has no
      # environment search variable of this kind — it searches the EXECUTABLE'S
      # OWN DIRECTORY first, which is exactly where Step 7 stages this leg's
      # acquired DLLs, so nothing is missing. ⚠ And PATH is specifically the wrong
      # answer here: this function's caller runs inside the subshell that is about
      # to `exec` the LAUNCHER (`wine`), and a PATH mutated with staged library
      # directories — or with the `;` a Windows PATH needs — would break the
      # launcher's OWN lookup before the fixture ever started. The .ps1 twin, which
      # runs a pe64 leg NATIVELY and resolves its launcher differently, does use
      # PATH; that difference is a property of the two run paths, not a drift.
      printf '%s' '' ;;
    *) die "[$_leg] target OS '$_os' has no declared runtime-loader search variable in this driver.
      Known: linux (LD_LIBRARY_PATH) | darwin (DYLD_LIBRARY_PATH) | windows (none — the loader
      searches the executable's directory). A new target OS must DECLARE its answer; defaulting
      to the ELF spelling is the defect this function exists to end." ;;
  esac
}
run_leg() {                    # run_leg <leg> <bin> <args...>  — REPLACES this shell
  local leg="$1" bin="$2"; shift 2
  # THE LAUNCHER IS DECLARED, NOT INFERRED. `LEG_LAUNCH` is the catalogue's
  # launcher argv, shlex-quoted and space-joined by the resolver, so it may be
  # MULTI-WORD (`arch -x86_64`) and may contain spaces inside a word; `eval` on the
  # resolver's own quoting is the only correct way to split it back into an argv.
  # `LEG_LAUNCH_ENV` is the same for `NAME='value'` pairs — it is what carries
  # QEMU_LD_PREFIX for the arm64 leg, which used to be a hardcoded $QEMU_SYSROOT
  # here and is now a property of the leg that needs it.
  # ★ AND `LEG_RUN_LAUNCH` WINS WHEN IT IS SET: it is the SAME declared argv with
  # the run filesystem's working-directory option already spliced in by the
  # resolver (`--run-dir-plan`), which is how a launched leg is told to start in
  # ITS OWN filesystem instead of over a compatibility mount. Empty on every leg
  # whose launcher shares this driver's filesystem, so those spawn exactly as
  # before. [D-HARNESS-WSL-LAUNCHED-LEG-RUNDIR-IS-DRVFS]
  local -a launch=() envs=()
  eval "launch=(${LEG_RUN_LAUNCH[$leg]:-${LEG_LAUNCH[$leg]}})"
  eval "envs=(${LEG_LAUNCH_ENV[$leg]})"
  [[ ${#envs[@]} -eq 0 ]] || export "${envs[@]}"
  # The leg's OWN library directories, for the runtime loader. Only for a leg whose
  # libraries the harness STAGED (`ubuntu-ports-arm64`, `search-paths`,
  # `pinned-archive`): a `host-system` leg's libraries are, by definition, already
  # where this machine's loader looks, and prepending a system dir would be a
  # change with no purpose. Keyed on the leg's DECLARED provider, so a future
  # staged-library leg inherits it.
  if [[ "${LEG_LIB_PROVIDER[$leg]}" != "host-system" ]]; then
    local d1 d2 libpath lvar
    d1="$(dirname "${LEG_TCL_LIB[$leg]}")"; d2="$(dirname "${LEG_Z_LIB[$leg]}")"
    libpath="$d1"; [[ "$d2" == "$d1" ]] || libpath="$d1:$d2"
    # ★★ THE VARIABLE NAME IS A PROPERTY OF THE TARGET, NOT A CONSTANT.
    # [D-HARNESS-RUN-ENV-LD-LIBRARY-PATH-INERT-ON-DARWIN] — its trigger has FIRED.
    # This line was `export LD_LIBRARY_PATH=…` for every leg. **dyld IGNORES
    # LD_LIBRARY_PATH**; on Darwin the variable is DYLD_LIBRARY_PATH. The row
    # recorded it as inert only because both macho legs were `provider:
    # host-system` and never reached this code — they are `pinned-archive` now, so
    # it is LIVE, and it is the same target-keyed principle as the rest of the
    # catalogue. ✔VERIFIED here rather than taken on trust: legs.json declares
    # `"provider": "pinned-archive"` for BOTH macho legs at this commit, which is
    # precisely the trigger condition the row named.
    lvar="$(leg_loader_path_var "$leg")"
    if [[ -n "$lvar" ]]; then
      local cur="${!lvar:-}"
      [[ -z "$cur" ]] || libpath="$libpath:$cur"
      export "$lvar=$libpath"
    fi
  fi
  # ── THE ACQUIRED Tcl's SCRIPT LIBRARY, LEG-SCOPED ──────────────────────────
  # [D-HARNESS-ACQUIRED-TCL-DYLIB-HAS-NO-SCRIPT-LIBRARY]. Exported HERE and
  # nowhere else: this function is the last thing a segment's BACKGROUND SUBSHELL
  # does before `exec`, so the assignment reaches this leg's child and CANNOT leak
  # into the driver, into the reference fixture, or into another leg — which is
  # the same isolation the launcher env and the loader path above rely on. Never a
  # global export, and never keyed on the host: a leg whose Tcl was acquired needs
  # this on every host that runs it, and a leg whose Tcl is the host's own must
  # NOT have it (the host's tclsh already resolves its own).
  [[ -z "${LEG_TCL_SCRIPT_DIR[$leg]:-}" ]] || export TCL_LIBRARY="${LEG_TCL_SCRIPT_DIR[$leg]}"
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
  # SAME CHOKE POINT, SAME REASON, for the loader search variable: `run_leg` asks
  # `leg_loader_path_var` for it, and that function DIES on a target OS it has no
  # declared answer for — but by then it is inside the background subshell above,
  # where a `die` exits only that subshell, lands in the segment log, and reads as
  # a failing test instead of as the harness refusing to start. Asking here, in the
  # foreground, makes the refusal a refusal. The answer is discarded; only its
  # ability to EXIST is being asserted.
  [[ "${LEG_LIB_PROVIDER[$leg]}" == "host-system" ]] || leg_loader_path_var "$leg" >/dev/null
  # `trap - ERR; set +e` inside the subshell is load-bearing. The old form was a
  # `|| segrc=$?` list, which suppresses errexit and the ERR trap; a BACKGROUND job
  # does not, so this subshell would inherit them (set -E) and the harness-level
  # `die` would fire on the fixture's own non-zero exit — writing a bogus
  # " [X] ERROR: failed at line …" INTO the segment log (stderr is redirected there)
  # and masking the real exit status. A failing test is data here, not an error.
  # `$leg_run_cd` is $rundir whenever the launcher shares this driver's filesystem
  # — i.e. on every POSIX host, where this line is byte-for-byte what it always
  # was. When it does not, the child's real working directory comes from the
  # launcher's own spliced option and this `cd` only keeps the SUBSHELL somewhere
  # that exists. [D-HARNESS-WSL-LAUNCHED-LEG-RUNDIR-IS-DRVFS]
  ( trap - ERR; set +e; cd "${leg_run_cd:-$rundir}" && run_leg "$leg" "$launch_bin" "$@" ) > "$log" 2>&1 < /dev/null &
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
# ── THIS LEG'S OWN ATTRIBUTION ORACLE ────────────────────────────────────────
# [D-HARNESS-PE64-HAS-NO-SAME-PLATFORM-ORACLE] The run reference is built by the
# DERIVING host's gcc, so it belongs to ONE platform and is an oracle for the
# legs of that platform only. A leg of any other target gets its own, compiled
# from ITS OWN manifest by the compiler `--resolve-target-cc` has already proven
# targets it — and when this host owns no such compiler the leg reports NO
# ORACLE, in those terms, instead of inheriting a line about a binary it cannot
# run. Empty means "none for this leg", which Step 9 prints as such.
declare -A LEG_ORACLE=() LEG_ORACLE_CC=() LEG_ORACLE_TRIPLE=()
# ★★ THE ORACLE'S *STATUS*, KEPT ON EVERY OUTCOME — the fact the old code threw
# away. [D-HARNESS-BUILD-FAILURE-HAS-NO-PER-TU-ATTRIBUTION] `build-failed` is not
# the same event as `no-reference-compiler`: the first means THE CONTROL RAN AND
# AGREED WITH US, the second means there is no control at all. Collapsing them to
# an empty LEG_ORACLE made a leg whose reference rejects the same TUs dss rejects
# print "NO ORACLE", which reads as *the control is missing*. Only `built` and
# `build-failed` let `--attribute-build` grant anything, and it is handed this
# value verbatim rather than re-deriving it from whether a log file exists — a
# log left behind by a previous run must never buy an amnesty this run did not.
declare -A LEG_ORACLE_STATUS=() LEG_ORACLE_LOG=() LEG_BUILD_ATTRIBUTION=()
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
#
# The distinct (zinc stage, config stage) PAIRS the declared legs actually need —
# built from the legs, not as a cross product of the two families: 2 zinc stages x
# 3 config stages would write 6 include lists for the 3 combinations that exist,
# and a list nothing uses is a file a reader has to rule out.
declare -A LEG_STAGE_PAIRS=()
for _l in "${LEG_DECLARED[@]}"; do
  _k="${LEG_HEADER_STAGE_KEY[$_l]:-}"; _c="${LEG_CONFIG_STAGE_KEY[$_l]:-}"
  [[ -n "$_k" && -n "${ZINC_STAGE_DIR[$_k]:-}" ]] || continue
  [[ -n "$_c" && -n "${CFG_STAGE_DIR[$_c]:-}" ]] || continue
  LEG_STAGE_PAIRS["$_k|$_c"]=1
done
#
# ★★ THE STAGED sqlite_cfg.h DIR IS THE SECOND THING ON THIS LIST THAT IS NOT LEG-
# INDEPENDENT, ON A DIFFERENT KEY — which is why the lists below are written per
# PAIR (zinc stage, config stage) rather than per zinc stage.
# The two families are keyed differently ON PURPOSE: the zlib header follows the
# `recipeTransform` and the sqlite configure header follows the TARGET OS, and four
# legs share `recipeTransform: "none"` while being TWO different configure targets
# (two Linux, two Darwin). One key could not tell them apart, and a leg handed the
# other family's header is the whole defect
# (D-HARNESS-MACHO-LEG-INHERITS-THE-DERIVING-LINUX-HOSTS-CONFIGURE-PROBES).
# ★ THE cfg/ DIR GOES FIRST — but position is only HALF the mechanism, and on its
# own it was NOT ENOUGH. Quote includes search the INCLUDING FILE'S OWN DIRECTORY
# *BEFORE* THIS LIST IS CONSULTED AT ALL (src/core/types/include_path_resolve.hpp,
# C 6.10.2p3), so no list position can outrank a `sqlite_cfg.h` sitting beside the
# includer — which is exactly the case for `$BLD/ctime.c`, TU #1 of both artefacts.
# The OTHER half is the `rm -f "$BLD/sqlite_cfg.h"` at Step 6, which deletes the
# deriving host's copy so that self-directory lookup misses everywhere; the full
# measurement is in the comment there. With it gone, `sqliteInt.h` (in src/, no
# sibling header) and `ctime.c` (in $BLD, sibling now removed) resolve through the
# SAME list, and the first entry that has the header wins.
# First is still the only placement that is correct no matter how the rest of the
# list is later reordered, and it can shadow nothing: the staged dir holds exactly
# one file, `sqlite_cfg.h`. (The .ps1 puts its staged bld dir at the FRONT of its
# base list, so "just before $BLD" would have been two different rules in two
# drivers — which is how they drift.)
for _pair in "${!LEG_STAGE_PAIRS[@]}"; do
  _k="${_pair%%|*}"; _c="${_pair#*|}"
  printf '%s\n' "${CFG_STAGE_DIR[$_c]}" "${INC_DIRS_HEAD[@]}" "${ZINC_STAGE_DIR[$_k]}" \
    "${INC_DIRS_TAIL[@]}" > "$OUT_DIR/recipe-includes.$_k.$_c.txt"
done
# THE CLI'S OWN INCLUDE LIST, from the CLI's OWN recipe — one per header stage,
# exactly like the fixture's above and for the same D-HARNESS-SQLITE-STAGE-ZCONF-IS-PE-SHAPED
# reason (each leg compiles against the zlib header staged for ITS target).
#
# ★ IT IS NOT THE FIXTURE'S LIST, and the difference is real rather than tidy:
# $INC_DIRS_HEAD carries $THIRD_PARTY_INCS, i.e. the staged TCL headers, which the
# CLI has no business seeing. shell.c does not use Tcl at all — ✔MEASURED
# 2026-08-05: zero `tcl.h` / `Tcl_` references in the generated shell.c — so
# handing it tcl on -I would be an undeclared input that could only ever shadow
# something. ${CLI_INCS[@]} is what `make -n sqlite3d` itself asked for (the six
# sqlite src/ext dirs); the leg's zinc/ supplies <zlib.h>, which shell.c DOES
# include; $INC_DIRS_TAIL supplies $BLD (the generated sqlite3.h) and, on a Mac,
# the SDK dir that must stay LAST so it cannot shadow the staged zlib header.
for _pair in "${!LEG_STAGE_PAIRS[@]}"; do
  _k="${_pair%%|*}"; _c="${_pair#*|}"
  printf '%s\n' "${CFG_STAGE_DIR[$_c]}" "${CLI_INCS[@]}" "${ZINC_STAGE_DIR[$_k]}" \
    "${INC_DIRS_TAIL[@]}" > "$OUT_DIR/cli-includes.$_k.$_c.txt"
done
for _l in "${LEG_DECLARED[@]}"; do
  _k="${LEG_HEADER_STAGE_KEY[$_l]:-}"; _c="${LEG_CONFIG_STAGE_KEY[$_l]:-}"
  # BOTH staged headers or NEITHER: a leg with one of the two is a leg compiled
  # against somebody else's answers for the other, and the Step-7/7b blockers
  # below poison it by name rather than letting it build. `continue` here leaves
  # LEG_INC_FILE unset, which is what those blockers test.
  [[ -n "$_k" && -n "${ZINC_STAGE_DIR[$_k]:-}" ]] || continue
  [[ -n "$_c" && -n "${CFG_STAGE_DIR[$_c]:-}" ]] || continue
  LEG_INC_FILE["$_l"]="$OUT_DIR/recipe-includes.$_k.$_c.txt"
  LEG_CLI_INC_FILE["$_l"]="$OUT_DIR/cli-includes.$_k.$_c.txt"
  LEG_ZINC_DIR["$_l"]="${ZINC_STAGE_DIR[$_k]}"
  LEG_CFG_DIR["$_l"]="${CFG_STAGE_DIR[$_c]}"
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
#   · <library argv>      its resolved (tcl, z) pair — see `leg_resolve_library_argv`.
#                         Passed through as TOKENS built by $LEG_RESOLVER, not
#                         spelled here, because the flag carries an optional
#                         runtime-identity override this file must not know about.
#   · --recipe-transform  `none` | `windows-selfconfig`  (build.recipeTransform)
#   · --stack-reserve     bytes; 0 omits the key           (build.stackReserveBytes)
#   · --includes          THIS LEG's include list — the one carrying the zlib
#                         header staged for its target (build.zconfGuards, via
#                         build.headerStageKey). Not one shared file: that is
#                         D-HARNESS-SQLITE-STAGE-ZCONF-IS-PE-SHAPED.
# rc is taken DIRECTLY off python3 by the caller's `if`, never through a pipe.
generate_manifest() {           # generate_manifest <leg> <out-manifest> <library-argv>...
  local leg="$1" out="$2"; shift 2
  dss_bh_generate_manifest "$MANIFEST_GEN" "$out" testfixture "${LEG_SPEC[$leg]}" \
    "$RECIPE_TUS_FILE" "${LEG_INC_FILE[$leg]}" "$RECIPE_DEFS_FILE" \
    "${LEG_RECIPE_TRANSFORM[$leg]}" "${LEG_STACK_RESERVE[$leg]}" "$@"
}
# generate_cli_manifest <leg> <out-manifest> <library-argv>… — the SECOND artifact.
#
# ★ A SECOND MANIFEST IS NOT A STYLE CHOICE. The manifest schema allows exactly
# ONE artifact: `artifactName` is a SCALAR (src/program/project_config.cpp:372-390),
# so two artifacts need two manifests and two `--project` invocations. There is no
# multi-artifact form to reach for.
#
# Four things differ from the fixture's manifest, and every one of them is a
# property of the PROGRAM rather than of the leg:
#   · --tus            the CLI's own 103 (shell.c + the 102 library TUs)
#   · --includes       the CLI's own list — no staged Tcl headers
#   · --artifact-name  sqlite3
#   · the library argv the CALLER passes: ZLIB ONLY, no Tcl.
# Everything leg-shaped — the target spec, the recipe transform, the stack
# reserve — is read from the SAME leg declaration the fixture uses, because those
# are facts about the TARGET and the CLI is built for the same one.
generate_cli_manifest() {       # generate_cli_manifest <leg> <out-manifest> <library-argv>...
  local leg="$1" out="$2"; shift 2
  dss_bh_generate_manifest "$MANIFEST_GEN" "$out" sqlite3 "${LEG_SPEC[$leg]}" \
    "$CLI_TUS_FILE" "${LEG_CLI_INC_FILE[$leg]}" "$CLI_DEFS_FILE" \
    "${LEG_RECIPE_TRANSFORM[$leg]}" "${LEG_STACK_RESERVE[$leg]}" "$@"
}
# leg_resolve_library_argv <leg> — the argv that hands DSS this leg's two resolved
# libraries, ONE TOKEN PER LINE, on stdout. Diagnostics to stderr; rc is the
# resolver's own.
#
# ★★ THE FLAG IS SPELLED IN $LEG_RESOLVER, NOT HERE, and that is the whole point.
# A resolved library may carry a DECLARED RUNTIME IDENTITY (`LEG_LIB_*_IMPORT_NAME`,
# from the leg's `importName`): an ACQUIRED library is a STAND-IN — we read its
# export surface on this host, but the target machine loads its OWN copy — so the
# identity recorded in the artefact (DT_NEEDED / LC_LOAD_DYLIB / PE import name)
# must be the one the leg declares, not the packager's embedded install name.
# ✔MEASURED 2026-08-04: the MacPorts dylibs carry `LC_ID_DYLIB = /opt/local/lib/…`,
# so a naive link produces a Mach-O that demands MacPorts on the target Mac and
# dies in dyld — a LOAD failure, not a build error, and one this host cannot
# observe. A driver that spelled the override itself would be a capability that
# exists in one driver and not the other, which is precisely
# D-HARNESS-LIBRARY-ACQUISITION-BUILT-FOR-ONE-LEG-IN-ONE-DRIVER.
#
# `--dss` makes the resolver PROBE this run's compiler for the override and REFUSE
# (non-zero, with a diagnostic) if it cannot record the identity. Dropping the
# override silently is not on the table: it would link clean here and fail at load
# there. A leg with NO declared identity gets `--resolve-library <path>` — byte
# for byte what every leg passed before this route existed.
#
# ⚠ These tokens go to $MANIFEST_GEN, not to $DSS_BIN: in this driver EVERY leg is
# built through a `--project` manifest, and the generator's `--resolve-library`
# deliberately mirrors the DSS CLI's own `<path>[=<import-name>]` so it is a
# pass-through of the same vocabulary (gen-pe64-manifest.py `resolve_library_entry`
# -> the manifest's `resolveLibraries`). There is no second, argv-only path to keep
# in step.
leg_resolve_library_argv() {    # leg_resolve_library_argv <leg>  -> one token per line
  local leg="$1"
  python3 "$LEG_RESOLVER" --catalogue "$LEG_CATALOGUE" \
      --resolve-library-argv "${LEG_TCL_LIB[$leg]}" \
      --import-name "${LEG_LIB_TCL_IMPORT_NAME[$leg]}" --dss "$DSS_BIN" \
    && leg_resolve_z_library_argv "$leg"
}
# leg_resolve_z_library_argv <leg> — the ZLIB HALF ONLY, for the sqlite3 CLI.
#
# ★ THE CLI DOES NOT LINK TCL, AND MUST NOT DECLARE THAT IT DOES. shell.c has no
# Tcl in it (✔MEASURED 2026-08-05: zero `tcl.h` / `Tcl_` references in the
# generated shell.c), so listing the Tcl library in its manifest would record a
# runtime dependency the program never uses — a DT_NEEDED / import that makes the
# binary refuse to load on a machine without Tcl, for no reason at all.
#
# ★ IT DOES LINK ZLIB, and that correction is MEASURED rather than assumed. The
# recipe carries `SQLITE_HAVE_ZLIB=1` (it survives the windows-selfconfig
# transform by design — that transform drops only a leading HAVE_/Z_HAVE_), and
# under it shell.c reaches a live `#include <zlib.h>` at two places, so the
# generated CLI really does call into zlib. `-lz` is on upstream's own sqlite3d
# link line for exactly this reason.
leg_resolve_z_library_argv() {  # leg_resolve_z_library_argv <leg> -> one token per line
  local leg="$1"
  python3 "$LEG_RESOLVER" --catalogue "$LEG_CATALOGUE" \
      --resolve-library-argv "${LEG_Z_LIB[$leg]}" \
      --import-name "${LEG_LIB_Z_IMPORT_NAME[$leg]}" --dss "$DSS_BIN"
}
# >>> dss:artifact-report >>>
# ── WHAT DID THE COMPILER ACTUALLY WRITE? ASK IT, DO NOT GUESS. ──────────────
# ★ ANCHOR, ONE LINE, DO NOT WRAP: D-HARNESS-FIXTURE-PATH-ASSUMES-THE-POSIX-ARTIFACT-SPELLING
#
# This driver used to answer that question itself, with `bin="$outd/$fmt/testfixture"`
# — a name assembled from the artifact's base and NO suffix. DSS names a PE executable
# `testfixture.exe`, so on the one host where this driver builds the pe64 leg (a POSIX
# host cross-building for Windows) the binary was there, the compile log held ZERO
# `error[`, and the leg was still recorded as a build FAILURE and marked POISONED.
# ✔MEASURED 2026-08-04 on WSL x86_64: `PE32+ executable (console) x86-64, for MS
# Windows, 8 sections`, 5,387,264 bytes — thrown away by its own instrument. Note
# WHERE the defect hid: only a cross-host build can reach it, which is precisely the
# case this harness exists to measure.
#
# ★ AND THE READER LIVES IN THE SHARED CORE, NOT HERE. This driver used to carry
# its own `dss_reported_artifact` whose rule was "take the LAST match", and the
# .ps1 carried a `Get-ReportedArtifact` implementing the same rule — so extracting
# `dss_bh_reported_artifact` took the copy count from two to FOUR while the two
# survivors still implemented the rule the shared one refuses. "Last wins"
# silently assumes ONE artifact per (log, spec), which stopped being true the
# moment this harness built a second artifact per leg: it would hand back a
# SIBLING's binary with no diagnostic. Both private copies are gone; the readers
# here and in build-and-test.ps1 are now the shared pair
# (`dss_bh_reported_artifact` / `Get-DssReportedArtifact`), which REFUSE an
# ambiguous log instead of guessing. Likewise the compile-time suffix, which was
# a third private one-liner and is now `dss_bh_compile_time_suffix`.
#
# The suffix is not this file's business and never was. `TargetSpec::outputExtension`
# (src/program/target_spec.cpp) derives it from the CLOSED object-format enum; a copy
# here would be a second table to keep in step, and that is not hypothetical — the
# .ps1 sibling carried the other copy, matched on a format-NAME prefix, and the two
# disagreed. So the compiler now REPORTS every artifact it commits, one line each, on
# stderr:
#
#     dss-code-prime: artifact <targetSpec> <absolute path>
#
# and the shared core reads it. The target spec is a single token BY CONSTRUCTION (DSS
# refuses whitespace in either half of a spec), so the path is the whole REMAINDER of
# the line and an output directory containing a space survives intact.
#
# ★ ABSENCE IS A REAL ANSWER HERE, NOT AN ERROR TO PAPER OVER. A build that wrote
# nothing reports nothing; rc 1 is what makes the caller's "0 error[ but no artefact"
# branch fire on exactly that case, which is a diagnostic worth keeping.
# <<< dss:artifact-report <<<
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
  # environment: one of its two staged header dirs could not be produced (a
  # ZINC-STAGE-FAIL or a CFG-STAGE-FAIL in Step 6). `poisoned`, named, and NO
  # fallback to a sibling stage's copy — that fallback IS
  # D-HARNESS-SQLITE-STAGE-ZCONF-IS-PE-SHAPED / D-HARNESS-MACHO-LEG-INHERITS-THE-
  # DERIVING-LINUX-HOSTS-CONFIGURE-PROBES.
  if [[ -z "${LEG_INC_FILE[$leg]:-}" ]]; then
    LEG_VERDICT["$leg"]='poisoned'
    LEG_VERDICT_DETAIL["$leg"]="this leg has no include list: its staged zlib header dir 'zinc/${LEG_HEADER_STAGE_KEY[$leg]:-?}' (declared zconfGuards: ${LEG_ZCONF_GUARDS[$leg]:-none}) and/or its staged sqlite config dir 'cfg/${LEG_CONFIG_STAGE_KEY[$leg]:-?}' (declared configureAnswers: ${LEG_CONFIGURE_ANSWERS[$leg]:-none}) was NOT produced — see the ZINC-STAGE-FAIL / CFG-STAGE-FAIL line in Step 6. Compiling it against another target's zlib header, or against the DERIVING host's sqlite_cfg.h, is refused."
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
  # ★ THE SWEEP IS SCOPED TO THIS LEG'S ARTEFACT DIRECTORY, not to a file name —
  # and it has to be, because at this point the artefact HAS NO NAME YET. The name
  # is whatever the compiler decides to write (see `dss_bh_reported_artifact`), and
  # this driver is not entitled to guess it; the sweep used to pass a constructed
  # `…/testfixture`, which is the same guess that poisoned the pe64 leg. The
  # matcher is a substring test over each process's argv, so a directory is a
  # strictly WIDER and strictly more honest needle: "anything still executing out
  # of the directory I am about to overwrite". We hold the run lock, so anything
  # matching is a leftover by construction.
  preflight_out="$(stop_our_fixtures "$outd/$fmt/" 'pre-flight')" \
    || die "[$leg] the pre-flight fixture sweep FAILED — refusing to build over a possibly-running fixture."
  while IFS= read -r k; do
    [[ -z "$k" ]] || { warn "[$leg] LEFTOVER FIXTURE: $k"; PREFLIGHT_KILLS+=("$k"); }
  done <<< "$preflight_out"
  # <<< dss:preflight <<<
  info "[$leg] $spec — ${#TUS[@]} TUs → testfixture (resolve: $(basename "${LEG_TCL_LIB[$leg]}"), $(basename "${LEG_Z_LIB[$leg]}"); transform: ${LEG_RECIPE_TRANSFORM[$leg]}; stackReserve: ${LEG_STACK_RESERVE[$leg]})"
  info "[$leg] zlib headers: ${LEG_ZINC_DIR[$leg]}  [${LEG_ZCONF_GUARDS[$leg]}]"
  # STATED PER LEG, beside the zlib line and for the same reason: a build log must
  # be able to answer "which machine's ./configure answers was this compiled
  # against?" without anyone re-deriving it from a key.
  info "[$leg] sqlite config header: ${LEG_CFG_DIR[$leg]}  [${LEG_CONFIGURE_ANSWERS[$leg]}]"
  # THE LIBRARY ARGV, BUILT BY THE RESOLVER (see `leg_resolve_library_argv`).
  # `mapfile` because the tokens are NEWLINE-separated and a token may contain
  # spaces (a cache path) or `=` (the identity override) — word-splitting them
  # would corrupt exactly the paths this harness works hardest to keep intact.
  # rc DIRECTLY off the substitution; stderr to a per-leg log so the resolver's own
  # refusal is quoted verbatim and never paraphrased.
  _argv_log="$outd/resolve-library-argv.log"
  declare -a _lib_argv=()
  if _argv_raw="$(leg_resolve_library_argv "$leg" 2>"$_argv_log")" && [[ -n "$_argv_raw" ]]; then
    mapfile -t _lib_argv <<< "$_argv_raw"
  else
    # `poisoned`, NOT a skip. The resolver refuses when a leg DECLARES a runtime
    # identity this compiler cannot record — and the only alternative is to build
    # an artefact that links clean here and dies in the target's loader, which is
    # the one failure this host cannot observe. A defect, so it reads as one.
    # The resolver's own words, flattened to one line for the ledger. A SILENT
    # refusal would be the worst outcome here, so an empty log is itself named.
    _argv_msg="$(tr '\n' ' ' < "$_argv_log" 2>/dev/null || true)"
    [[ -n "${_argv_msg// /}" ]] || _argv_msg="<the resolver refused with no diagnostic on stderr — see $_argv_log>"
    COMPILE_FAILS=$((COMPILE_FAILS + 1))
    LEG_VERDICT["$leg"]="poisoned"
    LEG_VERDICT_DETAIL["$leg"]="the DSS argv for this leg's resolved libraries could not be built (declared runtime identities: tcl='${LEG_LIB_TCL_IMPORT_NAME[$leg]:-<none>}', z='${LEG_LIB_Z_IMPORT_NAME[$leg]:-<none>}') — $_argv_msg"
    warn "[$leg] POISONED — ${LEG_VERDICT_DETAIL[$leg]}"
    continue
  fi
  # rc DIRECTLY off the generator (the `if` also keeps errexit out of it). It emits
  # two lines — the transform summary and the counts — so both are surfaced.
  if counts="$(generate_manifest "$leg" "$manifest" "${_lib_argv[@]}")"; then
    while IFS= read -r _cl; do [[ -z "$_cl" ]] || info "[$leg] manifest: $_cl"; done <<< "$counts"
    info "[$leg] manifest → $manifest"
  else
    printf '%s\n' "$counts" | sed 's/^/      /' >&2
    die "[$leg] manifest generation FAILED ($MANIFEST_GEN) — see above.
      The generator also asserts that every TU EXISTS on disk, so a staged-tree miss
      lands here rather than mid-compile."
  fi
  # ── THE SAME-PLATFORM ATTRIBUTION ORACLE, FROM THE SAME MANIFEST ──────────
  # ★ ONE DECLARATION, TWO COMPILERS — which is what an oracle IS. The manifest
  # DSS is about to consume is handed straight to this leg's VERIFIED target
  # compiler, so the TU set, the include roots, the defines and the resolved
  # libraries cannot drift between the subject and its control. It is also the
  # only shape that works cross-target: upstream's autotools Makefile is
  # configured for the DERIVING host and cannot emit a foreign-target fixture,
  # which is exactly why pe64 had no oracle for two months
  # [D-HARNESS-PE64-HAS-NO-SAME-PLATFORM-ORACLE].
  # ★ NEVER FATAL, AND NEVER SILENT. rc 4 = this host owns no compiler for this
  # leg; rc 3 = one exists and the build failed. Both leave the leg with NO
  # ORACLE and Step 9 says so by name — the run continues, because a missing
  # control is a reporting fact, not a reason to throw away the corpus.
  _orc=0
  _oracle_json="$(python3 "$LEG_RESOLVER" --catalogue "$LEG_CATALOGUE" \
      --build-reference-oracle "$leg" --manifest "$manifest" \
      --oracle-dir "$outd" --oracle-log "$outd/reference-oracle.log" \
      2>"$outd/reference-oracle.stderr")" || _orc=$?
  # THE STATUS IS RECORDED ON EVERY ARM, from the resolver's own JSON — never
  # inferred from the rc here, which would be this driver re-deriving a fact the
  # resolver already stated. [D-HARNESS-BUILD-FAILURE-HAS-NO-PER-TU-ATTRIBUTION]
  LEG_ORACLE_STATUS["$leg"]="$(printf '%s' "$_oracle_json" | sed -n -e 's/.*"status": "\([^"]*\)".*/\1/p')"
  LEG_ORACLE_LOG["$leg"]="$outd/reference-oracle.log"
  if [[ "$_orc" -eq 0 ]]; then
    LEG_ORACLE["$leg"]="$(printf '%s' "$_oracle_json"  | sed -n -e 's/.*"path": "\([^"]*\)".*/\1/p')"
    LEG_ORACLE_CC["$leg"]="$(printf '%s' "$_oracle_json" | sed -n -e 's/.*"cc": "\([^"]*\)".*/\1/p')"
    LEG_ORACLE_TRIPLE["$leg"]="$(printf '%s' "$_oracle_json" | sed -n -e 's/.*"triple": "\([^"]*\)".*/\1/p')"
    info "[$leg] same-platform ORACLE built by ${LEG_ORACLE_CC[$leg]} (${LEG_ORACLE_TRIPLE[$leg]}) → ${LEG_ORACLE[$leg]}"
  else
    LEG_ORACLE["$leg"]=""
    # ★ AND THE TWO NON-ZERO ARMS ARE NOT ONE EVENT. rc 3 means the control RAN
    # and could not build the very sources dss is about to be handed — which is
    # EVIDENCE, and the whole input to the per-TU attribution below. rc 4 means
    # this host owns no compiler for this leg, which is evidence about nothing.
    if [[ "$_orc" -eq 3 ]]; then
      info "[$leg] the same-platform ORACLE also FAILED to build these sources — its diagnostics are the CONTROL for this leg's build (${LEG_ORACLE_LOG[$leg]})"
    fi
    # The resolver's own words, never this driver's paraphrase of them.
    while IFS= read -r _ol; do [[ -z "$_ol" ]] || warn "[$leg] oracle: $_ol"; done \
      < "$outd/reference-oracle.stderr"
  fi
  # ── THE BUILD, THROUGH THE SHARED CORE ────────────────────────────────────
  # ★ THE SAME FUNCTION THE CLI LOOP CALLS. This block used to run the compiler
  # itself and read the artefact back with a PRIVATE `dss_reported_artifact`
  # whose rule was "take the LAST match" — the very rule
  # dss_bh_reported_artifact refuses, because with two artifacts per leg it can
  # hand a caller its SIBLING's binary with no diagnostic. Extracting the core
  # and leaving the FIXTURE on the private copy left the count at four copies of
  # one decision, two of them still implementing the unsafe rule.
  # rc: 0 built · 1 no artefact reported · 2 AMBIGUOUS · 3 diagnostics · 4 the
  # build REPORTED an artefact that is not on disk. `|| _rc=$?` is load-bearing
  # under `set -Eeuo pipefail`: see the identical note in the CLI loop below.
  _rc=0
  bin="$(dss_bh_build_artifact "$DSS_BIN" "$manifest" "$DSS_CONFIG" "$outd" "$log" "$spec")" || _rc=$?
  # THE ONE EXTRA QUESTION THIS LOOP ASKS, and it is asked HERE rather than in the
  # shared core on purpose: Step 8 EXECS this file, so the POSIX exec bit has to be
  # set. That is not a target-agnostic property (a staticlib leg's artefact is not
  # executable) and it is not askable on a Windows host at all, which is why the
  # core stops at "does it exist" and the caller that intends to exec adds this.
  if [[ $_rc -eq 0 && ! -x "$bin" ]]; then _rc=5; fi
  if [[ $_rc -ne 0 ]]; then
    COMPILE_FAILS=$((COMPILE_FAILS + 1))
    # `poisoned` — the ledger's FAILURE class, and it DISPLACES whatever this leg
    # was carrying: a build that produced no artifact is the strongest thing that
    # can be said about it, and a failure must never read as a skip.
    LEG_VERDICT["$leg"]="poisoned"
    LEG_VERDICT_DETAIL["$leg"]="the fixture did not build for ${LEG_SPEC[$leg]} — see $log"
    if [[ $_rc -eq 3 ]]; then
      warn "[$leg] build FAILED$(dss_bh_compile_time_suffix "$log") — first diagnostics ($log):"
      { grep -m3 -E 'error\[' "$log" || head -3 "$log"; } 2>/dev/null | sed 's/^/      /'
      # >>> dss:build-attribution >>>
      # ★★★ WHOSE FAILURE IS THIS? Asked ONLY on rc 3 — the diagnostics arm — because
      # it is the only one where there is anything to compare: the other arms produced
      # no diagnostics at all. [D-HARNESS-BUILD-FAILURE-HAS-NO-PER-TU-ATTRIBUTION]
      # The DECISION is the resolver's, once, for both drivers; this block only
      # prints what it returned and records it. `|| _arc=$?` is load-bearing under
      # errexit for the same reason it is on the build call above.
      _arc=0
      _attr="$(python3 "$LEG_RESOLVER" --catalogue "$LEG_CATALOGUE" \
          --attribute-build "$leg" --compile-log "$log" \
          --oracle-log "${LEG_ORACLE_LOG[$leg]:-}" \
          --oracle-status "${LEG_ORACLE_STATUS[$leg]:-}" \
          --manifest "$manifest" 2>"$outd/build-attribution.stderr")" || _arc=$?
      if [[ $_arc -eq 0 || $_arc -eq 3 ]]; then
        # ONE ACCOUNT, COMPOSED BY THE RESOLVER — the same transport the confound
        # report uses, and the reason the two drivers cannot tell two stories about
        # one build. Every TU is printed, upstream ones included: an upstream TU that
        # vanished from the log would be indistinguishable from one that compiled.
        while IFS= read -r _al; do [[ -z "$_al" ]] || info "$_al"; done \
          <<< "$(printf '%s' "$_attr" | python3 -c 'import json,sys; print("\n".join(json.load(sys.stdin)["report"]))' 2>/dev/null)"
        LEG_BUILD_ATTRIBUTION["$leg"]="$(printf '%s' "$_attr" | python3 -c 'import json,sys; d=json.load(sys.stdin); print("%d of %d rejected TU(s) charged to DSS%s" % (len(d["chargedToDss"]), len(d["tus"]), "" if not d["chargedToDss"] else ": " + " ".join(t.rsplit("/",1)[-1] for t in d["chargedToDss"])))' 2>/dev/null)"
        [[ -n "${LEG_BUILD_ATTRIBUTION[$leg]}" ]] \
          && LEG_VERDICT_DETAIL["$leg"]="the fixture did not build for ${LEG_SPEC[$leg]} — ${LEG_BUILD_ATTRIBUTION[$leg]}.  See $log"
      else
        # A REFUSAL IS NOT AN ATTRIBUTION. The resolver could not decide, so nothing
        # is excused and the leg keeps the un-attributed detail it already has.
        while IFS= read -r _al; do [[ -z "$_al" ]] || warn "[$leg] attribution: $_al"; done \
          < "$outd/build-attribution.stderr"
        warn "[$leg] build attribution UNAVAILABLE (rc $_arc) — every diagnostic stays charged to dss, which is the safe direction"
      fi
      # <<< dss:build-attribution <<<
    elif [[ $_rc -eq 2 ]]; then
      warn "[$leg] build FAILED$(dss_bh_compile_time_suffix "$log") — the build log reports MORE THAN ONE artefact for $spec; see the diagnostic above and $log"
    elif [[ $_rc -eq 1 ]]; then
      # ★ THE GENUINE CASE THE OLD MESSAGE WAS TRYING TO DESCRIBE, and it now says
      # what it really means: the build emitted no diagnostics AND never claimed to
      # have written anything for this leg's target. That is a compiler that
      # returned quietly without producing an artefact — a defect worth a loud
      # verdict, and no longer reachable by merely mis-spelling a file name.
      warn "[$leg] build FAILED$(dss_bh_compile_time_suffix "$log") — 0 error[ and the build reported NO artefact for $spec (expected a 'dss-code-prime: artifact $spec <path>' line in $log)"
    elif [[ $_rc -eq 4 ]]; then
      warn "[$leg] build FAILED$(dss_bh_compile_time_suffix "$log") — 0 error[ but the artefact the build REPORTED is not there: $bin"
    else
      warn "[$leg] build FAILED$(dss_bh_compile_time_suffix "$log") — 0 error[ but the artefact the build REPORTED is not an executable file: $bin"
    fi
  else
    # ── STAGE THE ACQUIRED LIBRARIES BESIDE THE ARTEFACT ──────────────────────
    # ★ THE LINK IS NOT THE END OF THE BUILD FOR AN ACQUIRING LEG. The runtime
    # identity such a leg declares is `@loader_path/<name>` — TRUE BY
    # CONSTRUCTION only if the library sits in the directory holding the
    # EXECUTABLE. dyld resolves `@loader_path` against that directory (not the
    # cwd), and the fixture is exec'd in place from `<outd>/<fmt>/` with its cwd
    # set to the run dir, so `<outd>/<fmt>/` is where the copies belong. Without
    # them the artefact is a load failure waiting to happen on a machine this
    # host cannot observe — the same class of silent breakage the identity
    # override exists to prevent, re-introduced one step later.
    # FROM THE DECLARATION, never a name list in this file: the `as` and `path`
    # of every library `--acquire` reported, so a leg that declares a third
    # archive member gets it staged with no edit here. Empty for every
    # non-acquiring provider, whose libraries the target machine already has.
    _stage_dir="$(dirname "$bin")"; _stage_bad=""
    while IFS=$'\t' read -r _as _src; do
      [[ -n "$_as" && -n "$_src" ]] || continue
      if cp -p "$_src" "$_stage_dir/$_as"; then
        info "[$leg] staged beside the artefact: $_as  (from $_src)"
      else
        _stage_bad="$_stage_bad $_as"
        warn "[$leg] could NOT stage '$_as' beside the artefact: $_src -> $_stage_dir/$_as"
      fi
    done <<< "${LEG_ACQ_LIBS[$leg]:-}"
    if [[ -n "$_stage_bad" ]]; then
      # Per-leg, and `poisoned` rather than a skip: the compile succeeded, so this
      # is a defect in the artefact we produced, not an absent input. Refusing to
      # register the fixture is the point — an incomplete artefact must not be
      # handed to Step 8 or shipped as a verified leg.
      COMPILE_FAILS=$((COMPILE_FAILS + 1))
      LEG_VERDICT["$leg"]="poisoned"
      LEG_VERDICT_DETAIL["$leg"]="the fixture built for ${LEG_SPEC[$leg]}, but its ACQUIRED librar(y/ies)$_stage_bad could not be staged into $_stage_dir. The artefact records '@loader_path/<name>' for them, so without the copies it fails in the target's loader — see the warnings above."
      warn "[$leg] POISONED — ${LEG_VERDICT_DETAIL[$leg]}"
      continue
    fi
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
    pass "[$leg] testfixture -> $bin$(dss_bh_compile_time_suffix "$log")"
  fi
done

# ── Step 7b — build the sqlite3 CLI for EVERY declared leg ───────────────────
# ★ A SEPARATE LOOP, AND THAT IS THE POINT. The CLI's buildability is not the
# fixture's: it needs ZLIB and does not need TCL, so a leg whose Tcl could not be
# resolved on this host — which stops the fixture dead at the loop above — can
# still produce a perfectly good sqlite3. Nesting this inside the fixture loop
# would have inherited the fixture's `continue`s and silently made the CLI
# unbuildable for a reason that has nothing to do with it.
#
# ★ THE BUILD IS ATTEMPTED FOR EVERY DECLARED LEG, ON EVERY HOST. There is no
# host test in this loop and there must never be one — the same rule the fixture
# loop states, for the same reason: whether this machine can EXECUTE the result
# is a different question, asked by the smoke gate in Step 7c. A leg this host
# can never run is still compiled and still linked, because that is the
# capability under test.
step "7b/9  Build the sqlite3 CLI (dss-code-prime --project), per leg"
declare -A CLI_BIN=() CLI_OK=()
CLI_FAILS=0
for leg in "${LEG_ORDER[@]}"; do
  spec="${LEG_SPEC[$leg]}"; fmt="${LEG_FORMAT[$leg]}"
  # ★ ITS OWN OUTPUT DIRECTORY, AND ITS OWN COMPILE LOG. `<outd>/cli/` rather
  # than `<outd>/`, so the project driver's per-format subdir lands at
  # `<outd>/cli/<fmt>/sqlite3` and CANNOT collide with `<outd>/<fmt>/testfixture`.
  # The separate log is the STRUCTURAL half of the artifact-reader fix: two
  # artifacts for the SAME target spec in ONE log is exactly the ambiguity that
  # made "take the LAST match" unsafe, and giving each build its own log means
  # the ambiguity never arises. dss_bh_reported_artifact fails loud if it ever
  # does anyway.
  outd="$OUT_DIR/$leg/cli"; log="$outd/compile.log"
  manifest="$outd/$leg.sqlite3.dss-project.json"
  mkdir -p "$outd"
  # The ONE thing that can stop a leg here is a DECLARED BUILD INPUT this machine
  # could not find: DSS reads --resolve-library binaries at COMPILE time, so
  # without zlib there is nothing to compile against. An OBSERVED absence with a
  # named verdict — not an inference from what kind of box this is.
  if [[ -z "${LEG_Z_LIB[$leg]:-}" ]]; then
    CLI_FAILS=$((CLI_FAILS + 1))
    dss_bh_set_verdict "$leg" sqlite3 'skipped-build-input-missing' \
      "no zlib could be resolved for this leg on this host, and the CLI links zlib (SQLITE_HAVE_ZLIB=1 reaches a live '#include <zlib.h>' in shell.c) — see Step 6."
    warn "[$leg] CLI build NOT ATTEMPTED [skipped-build-input-missing] — $(dss_bh_get_detail "$leg" sqlite3)"
    continue
  fi
  if [[ -z "${LEG_CLI_INC_FILE[$leg]:-}" ]]; then
    CLI_FAILS=$((CLI_FAILS + 1))
    dss_bh_set_verdict "$leg" sqlite3 'poisoned' \
      "this leg has no CLI include list: its staged zlib header dir 'zinc/${LEG_HEADER_STAGE_KEY[$leg]:-?}' and/or its staged sqlite config dir 'cfg/${LEG_CONFIG_STAGE_KEY[$leg]:-?}' (declared configureAnswers: ${LEG_CONFIGURE_ANSWERS[$leg]:-none}) was NOT produced — see the ZINC-STAGE-FAIL / CFG-STAGE-FAIL line in Step 6. Compiling it against another target's zlib header, or against the DERIVING host's sqlite_cfg.h, is refused (D-HARNESS-SQLITE-STAGE-ZCONF-IS-PE-SHAPED / D-HARNESS-MACHO-LEG-INHERITS-THE-DERIVING-LINUX-HOSTS-CONFIGURE-PROBES)."
    warn "[$leg] CLI POISONED — $(dss_bh_get_detail "$leg" sqlite3)"
    continue
  fi
  # Sweep this leg's CLI artefact DIRECTORY, not a file name: at this point the
  # artefact has no name yet (the compiler decides it), and guessing one is the
  # defect D-HARNESS-FIXTURE-PATH-ASSUMES-THE-POSIX-ARTIFACT-SPELLING names.
  preflight_out="$(stop_our_fixtures "$outd/$fmt/" 'cli pre-flight')" \
    || die "[$leg] the pre-flight sweep for the CLI artefact dir FAILED — refusing to build over a possibly-running binary."
  while IFS= read -r k; do
    [[ -z "$k" ]] || { warn "[$leg] LEFTOVER CLI PROCESS: $k"; PREFLIGHT_KILLS+=("$k"); }
  done <<< "$preflight_out"

  declare -a _cli_lib_argv=()
  _cli_argv_log="$outd/resolve-library-argv.log"
  if _argv_raw="$(leg_resolve_z_library_argv "$leg" 2>"$_cli_argv_log")" && [[ -n "$_argv_raw" ]]; then
    mapfile -t _cli_lib_argv <<< "$_argv_raw"
  else
    _argv_msg="$(tr '\n' ' ' < "$_cli_argv_log" 2>/dev/null || true)"
    [[ -n "${_argv_msg// /}" ]] || _argv_msg="<the resolver refused with no diagnostic on stderr — see $_cli_argv_log>"
    CLI_FAILS=$((CLI_FAILS + 1))
    dss_bh_set_verdict "$leg" sqlite3 'poisoned' \
      "the DSS argv for this leg's resolved zlib could not be built (declared runtime identity: '${LEG_LIB_Z_IMPORT_NAME[$leg]:-<none>}') — $_argv_msg"
    warn "[$leg] CLI POISONED — $(dss_bh_get_detail "$leg" sqlite3)"
    continue
  fi
  info "[$leg] $spec — ${#CLI_TUS[@]} TUs → sqlite3 (resolve: $(basename "${LEG_Z_LIB[$leg]}"); transform: ${LEG_RECIPE_TRANSFORM[$leg]})"
  if counts="$(generate_cli_manifest "$leg" "$manifest" "${_cli_lib_argv[@]}")"; then
    while IFS= read -r _cl; do [[ -z "$_cl" ]] || info "[$leg] cli manifest: $_cl"; done <<< "$counts"
  else
    printf '%s\n' "$counts" | sed 's/^/      /' >&2
    die "[$leg] CLI manifest generation FAILED ($MANIFEST_GEN) — see above."
  fi
  # rc: 0 built · 1 no artefact reported · 2 AMBIGUOUS · 3 diagnostics. Judged
  # from `error[` plus the build's own artefact report, never from the process
  # exit status — dss-code-prime returns 0 even on fatal errors.
  # `|| _rc=$?` is LOAD-BEARING, not style. This file runs under `set -Eeuo
  # pipefail` with an ERR trap, and a plain `bin="$(fn)"; _rc=$?` lets the
  # assignment's non-zero status trip errexit BEFORE the next line runs — the
  # harness then dies with "failed at line N (command: return 3)" instead of
  # rendering this leg's verdict, turning a per-leg build failure into a
  # whole-run abort. MEASURED here on the first real run. The `|| …` list form
  # suppresses errexit for the assignment, which is exactly how the fixture
  # loop's `|| bin=''` above has always handled the same hazard.
  _rc=0
  bin="$(dss_bh_build_artifact "$DSS_BIN" "$manifest" "$DSS_CONFIG" "$outd" "$log" "$spec")" || _rc=$?
  if [[ $_rc -ne 0 ]]; then
    CLI_FAILS=$((CLI_FAILS + 1))
    case "$_rc" in
      3) _why="$(grep -m3 -E 'error\[' "$log" | tr '\n' ' ')" ;;
      2) _why="the build log reports MORE THAN ONE artefact for $spec — see the diagnostic above and $log" ;;
      4) _why="0 error[ but the artefact the build REPORTED is not there: $bin" ;;
      *) _why="0 error[ and the build reported NO artefact for $spec (expected a 'dss-code-prime: artifact $spec <path>' line in $log)" ;;
    esac
    dss_bh_set_verdict "$leg" sqlite3 'poisoned' "the sqlite3 CLI did not build for $spec — $_why  See $log"
    warn "[$leg] CLI build FAILED$(dss_bh_compile_time_suffix "$log") — $_why"
    continue
  fi
  # The acquired libraries go BESIDE this artefact too. A leg that ACQUIRED its
  # zlib records `@loader_path/<name>` for it, which is a claim about the
  # directory holding THE EXECUTABLE — and the CLI's directory is not the
  # fixture's, so staging beside the fixture does not make it true here.
  _stage_dir="$(dirname "$bin")"; _stage_bad=""
  while IFS=$'\t' read -r _as _src; do
    [[ -n "$_as" && -n "$_src" ]] || continue
    cp -p "$_src" "$_stage_dir/$_as" || _stage_bad="$_stage_bad $_as"
  done <<< "${LEG_ACQ_LIBS[$leg]:-}"
  if [[ -n "$_stage_bad" ]]; then
    CLI_FAILS=$((CLI_FAILS + 1))
    dss_bh_set_verdict "$leg" sqlite3 'poisoned' \
      "the CLI built for $spec, but its ACQUIRED librar(y/ies)$_stage_bad could not be staged into $_stage_dir. The artefact records '@loader_path/<name>' for them, so without the copies it fails in the target's loader."
    warn "[$leg] CLI POISONED — $(dss_bh_get_detail "$leg" sqlite3)"
    continue
  fi
  # Same macOS fresh-inode install the fixture gets, and for the same measured
  # reason (D-HARNESS-MACOS-PROVENANCE-KILLS-OVERWRITTEN-FIXTURE): the smoke gate
  # execs this file ~30 times, so an inode carrying a permanent exec DENY would
  # turn every assertion into a 137 with no output. A HOST fact about the box
  # that will exec the file, not about the target that produced it.
  if [[ "$HOST_OS" == "darwin" ]]; then
    fresh_ino="$(fixture_fresh_inode "$bin")" || die "[$leg] could not install the sqlite3 CLI on a FRESH INODE at $bin (rc=$?) — D-HARNESS-MACOS-PROVENANCE-KILLS-OVERWRITTEN-FIXTURE."
    info "[$leg] fresh-inode install: $bin now inode $fresh_ino"
  fi
  CLI_BIN["$leg"]="$bin"; CLI_OK["$leg"]=1
  dss_bh_set_verdict "$leg" sqlite3 'built' "sqlite3 -> $bin"
  pass "[$leg] sqlite3 -> $bin$(dss_bh_compile_time_suffix "$log")"
done
info "sqlite3 CLI: built on $(( ${#LEG_ORDER[@]} - CLI_FAILS )) of ${#LEG_ORDER[@]} processed leg(s)"

# ── Step 7c — the sqlite3 CLI SMOKE GATE, per leg ────────────────────────────
# ★ WHY THIS EXISTS AT ALL. The unit corpus in Step 8 runs through `testfixture`
# — a Tcl interpreter linking the sqlite LIBRARY — and NEVER executes shell.c. So
# argv handling, the dot-commands, the `.dump` writer and the startup version
# guard are covered by NOTHING without this [D-SQLITE-CLI-BUILT-ON-NO-LEG].
#
# ★ EVERY LEG GETS A VERDICT, OR A LOUD SKIP WITH A REASON. A leg that built the
# CLI but cannot execute it here (run.mode `skip` — a cross target with no
# launcher on this host) is recorded as `built-not-run-here`, which is a
# completely different fact from "not built" and is printed as such in Step 9.
# Silence about a leg is a harness bug.
step "7c/9  sqlite3 CLI smoke gate (14 assertions, attributed against gcc)"
[[ -f "$CLI_SMOKE" ]] || die "the CLI smoke gate is missing: $CLI_SMOKE"
# The expectation comes from the STAGED tree's OWN header — the very file these
# binaries were compiled against — never from a literal in this driver. A
# hardcoded "3.54.0" silently stops testing anything the day upstream bumps.
CLI_EXPECT_VERSION="$(sed -n 's/^#define SQLITE_VERSION  *"\(.*\)".*/\1/p' "$BLD/sqlite3.h" | head -1)"
CLI_EXPECT_SOURCE_ID="$(sed -n 's/^#define SQLITE_SOURCE_ID  *"\(.*\)".*/\1/p' "$BLD/sqlite3.h" | head -1)"
[[ -n "$CLI_EXPECT_VERSION" && -n "$CLI_EXPECT_SOURCE_ID" ]] \
  || die "could not read SQLITE_VERSION / SQLITE_SOURCE_ID out of $BLD/sqlite3.h.
      They are what the smoke gate compares the built CLI's --version against; without
      them the gate would be asserting nothing, which must never pass quietly."
info "expecting version '$CLI_EXPECT_VERSION' / source id '$CLI_EXPECT_SOURCE_ID' (from $BLD/sqlite3.h)"
[[ -n "$REF_CLI" ]] || warn "no gcc reference CLI — every smoke failure this run is UNATTRIBUTABLE and is charged to DSS by design."
# >>> dss:smoke-targets >>>
# ── WHAT EACH BINARY ACTUALLY IS, READ OUT OF ITS OWN HEADER ────────────────
# ★ `--identify-binary` prints `<arch>\t<container>\t<targetOs>` read from the
# ELF e_machine/EI_OSABI, the PE Machine field or the Mach-O cputype — no external
# tool, and rc 3 with a NAMED diagnostic on bytes it cannot identify. A DEFAULT
# here would be the worst possible kind: the caller is deciding whether a binary
# that would not run is this compiler's fault.
# ⚠ THE ANSWER COMES BACK IN A GLOBAL, NOT ON STDOUT, AND THAT IS NOT STYLE. A
# caller writing `t="$(identify_binary_triple x)"` runs the function in a SUBSHELL,
# so its diagnostic assignment would be discarded and the failure path would have
# nothing to print — the "a failing substitution leaves an empty field" trap this
# driver already records for its leg plan, wearing the diagnostic's face.
IDENTIFY_TRIPLE=""
IDENTIFY_WHY=""
identify_binary_triple() {     # identify_binary_triple <path>  -> 0 (IDENTIFY_TRIPLE) | 1 (IDENTIFY_WHY)
  local path="$1" errf out rc
  IDENTIFY_TRIPLE=""; IDENTIFY_WHY=""
  errf="$(mktemp)" || die "could not create a temp file for --identify-binary's stderr."
  # rc DIRECTLY, never after a pipe.
  if out="$(python3 "$LEG_RESOLVER" --catalogue "$LEG_CATALOGUE" --identify-binary "$path" 2>"$errf")"; then
    rc=0
  else
    rc=$?
  fi
  IDENTIFY_WHY="$(cat "$errf" 2>/dev/null || true)"; rm -f "$errf"
  if [[ "$rc" -ne 0 ]]; then
    IDENTIFY_WHY="could not identify $path (rc=$rc): ${IDENTIFY_WHY:-<no diagnostic on stderr>}"
    return 1
  fi
  # tab-separated -> the colon triple cli-smoke.py parses. NEVER fabricated: an
  # unreadable header returns 1 above and the caller says so out loud.
  IDENTIFY_TRIPLE="$(printf '%s' "$out" | tr '\t' ':' | tr -d '\r\n')"
  [[ -n "$IDENTIFY_TRIPLE" ]] || { IDENTIFY_WHY="harness_legs.py --identify-binary $path exited 0 and printed NOTHING — a contract break, not a property of the file."; return 1; }
  return 0
}
# ── AND HOW THIS HOST RUNS A BINARY OF THAT TARGET ──────────────────────────
# ★★ THE HOST-IDENTITY BRANCH THAT USED TO PICK THE REFERENCE'S LAUNCHER IS GONE.
# Its .ps1 twin read `if ($script:HostNeedsWsl) { --reference-launcher=wsl.exe … }`
# and THAT is why the oracle was unmatched: on a Windows host it ran the reference
# host-native x86_64 while DSS ran arm64 under qemu, then charged every difference
# to DSS. This driver had the same bug in its LATENT form — it passed NO reference
# launcher at all, which is correct only for as long as every host that owns a
# reference happens to run it natively (it stops being true on the arm64 VPS).
# The launcher now comes from the leg catalogue, keyed on the reference's OWN
# MEASURED target, through the same resolver that answers for every other leg.
LAUNCHER_FOR_ARGV=""
LAUNCHER_FOR_WHY=""
launcher_argv_for_target() {   # launcher_argv_for_target <triple>  -> 0 | the resolver's rc
  local target="$1" errf out rc
  LAUNCHER_FOR_ARGV=""; LAUNCHER_FOR_WHY=""
  errf="$(mktemp)" || die "could not create a temp file for --launcher-for-target's stderr."
  if out="$(python3 "$LEG_RESOLVER" --catalogue "$LEG_CATALOGUE" \
              --launcher-for-target "$target" --host-os "$HOST_OS" --host-arch "$HOST_ARCH" 2>"$errf")"; then
    rc=0
  else
    rc=$?
  fi
  # The REASON is always on stderr, on every outcome — including the one where
  # stdout is deliberately EMPTY because the target runs natively here. The argv
  # is shlex-quoted EXACTLY as a plan's LEG_LAUNCH, so it is `eval`'d into an
  # array by the caller and never word-split.
  LAUNCHER_FOR_WHY="$(cat "$errf" 2>/dev/null || true)"; rm -f "$errf"
  LAUNCHER_FOR_ARGV="$out"
  return "$rc"
}
# The reference is ONE binary and it is the same for every leg, so it is measured
# ONCE. A reference this host cannot identify or cannot execute is DROPPED, loudly:
# cli-smoke.py's CONTROL_ABSENT is an honest state, and running a control that
# never starts is the false-ACQUITTAL half of the same defect family
# (D-HARNESS-ATTRIBUTION-ORACLE-EXONERATES-VIA-A-REFERENCE-THAT-NEVER-RAN).
REF_CLI_TARGET=""
declare -a REF_CLI_LAUNCH=()
if [[ -n "$REF_CLI" ]]; then
  _idrc=0; identify_binary_triple "$REF_CLI" || _idrc=$?
  if [[ "$_idrc" -ne 0 ]]; then
    warn "the gcc reference CLI could not be IDENTIFIED — $IDENTIFY_WHY"
    warn "      It is DROPPED for this run rather than passed with a guessed target: an unattributable"
    warn "      smoke failure is an honest outcome, a fabricated control triple is not."
    REF_CLI=""
  else
    REF_CLI_TARGET="$IDENTIFY_TRIPLE"
    _lrc=0; launcher_argv_for_target "$REF_CLI_TARGET" || _lrc=$?
    case "$_lrc" in
      0) eval "REF_CLI_LAUNCH=(${LAUNCHER_FOR_ARGV})"
         info "reference CLI target: $REF_CLI_TARGET (MEASURED from its own header) — $LAUNCHER_FOR_WHY"
         [[ ${#REF_CLI_LAUNCH[@]} -eq 0 ]] || info "reference CLI launcher: ${REF_CLI_LAUNCH[*]}  (DECLARED by the catalogue for that target on this host, never inferred from the host's identity)" ;;
      3) warn "this host cannot EXECUTE the gcc reference CLI ($REF_CLI_TARGET) — $LAUNCHER_FOR_WHY"
         warn "      The reference is DROPPED: a control that cannot start would fail all fourteen assertions for one"
         warn "      reason and EXONERATE every DSS failure on every leg against a binary that never executed."
         REF_CLI="" ;;
      *) warn "harness_legs.py --launcher-for-target '$REF_CLI_TARGET' exited $_lrc — ${LAUNCHER_FOR_WHY:-<no diagnostic>}"
         warn "      That triple came from --identify-binary, so a malformed one is OUR defect, not this machine's."
         warn "      The reference is DROPPED rather than run with an unknown launcher."
         REF_CLI="" ;;
    esac
  fi
fi
# <<< dss:smoke-targets <<<
declare -A CLI_SMOKE_VERDICT=()
CLI_SMOKE_FAILS=0
for leg in "${LEG_ORDER[@]}"; do
  if [[ "${CLI_OK[$leg]:-0}" != "1" ]]; then
    CLI_SMOKE_VERDICT["$leg"]="not run [$(dss_bh_get_verdict "$leg" sqlite3)] — $(dss_bh_get_detail "$leg" sqlite3)"
    continue                                  # already counted + warned in Step 7b
  fi
  # THE ONE LEGITIMATE HOST QUESTION, and it is `run.mode` off the RESOLVED plan
  # — never `if [[ $HOST_OS ]]`. `native` runs it directly, `launched` runs it
  # through the leg's DECLARED launcher, `skip` records a named verdict. Asked
  # through the SHARED predicate, which is the same call Step 8 makes.
  if leg_run_is_skipped "$leg"; then
    CLI_SMOKE_VERDICT["$leg"]="built, NOT RUN here [${LEG_RUN_VERDICT[$leg]}] — ${LEG_RUN_DETAIL[$leg]}"
    warn "[$leg] CLI smoke SKIPPED — built at ${CLI_BIN[$leg]} but this host cannot execute it: ${LEG_RUN_DETAIL[$leg]}"
    continue
  fi
  # ★ WHAT THIS LEG'S CLI ACTUALLY IS, MEASURED FROM ITS OWN HEADER — never
  # assumed from the leg's name and never from the spec, which is the DECLARED
  # side. cli-smoke.py compares the two and reports a leg that built the WRONG
  # TARGET as its own non-verdict; that comparison is worth nothing if this driver
  # feeds it the declaration twice.
  _cidrc=0; identify_binary_triple "${CLI_BIN[$leg]}" || _cidrc=$?
  if [[ "$_cidrc" -ne 0 ]]; then
    CLI_SMOKE_FAILS=$((CLI_SMOKE_FAILS + 1))
    CLI_SMOKE_VERDICT["$leg"]="FAIL — the built CLI could not be IDENTIFIED (${CLI_BIN[$leg]}); no smoke verdict was taken"
    warn "[$leg] CLI smoke NOT RUN — $IDENTIFY_WHY"
    warn "      This is RED and it is NOT charged to the compiler: the gate needs the subject's MEASURED target and this"
    warn "      driver will not fabricate one. Counted as a failure so the run cannot exit 0 over a leg it never asserted about."
    continue
  fi
  _cli_target="$IDENTIFY_TRIPLE"
  _smoke_dir="$OUT_DIR/$leg/cli-smoke"
  rm -rf "$_smoke_dir"; mkdir -p "$_smoke_dir"
  declare -a _smoke_argv=("$CLI_SMOKE" --cli "${CLI_BIN[$leg]}"
                          --expect-version "$CLI_EXPECT_VERSION"
                          --expect-source-id "$CLI_EXPECT_SOURCE_ID"
                          --leg-spec "${LEG_SPEC[$leg]}"
                          --cli-target "$_cli_target"
                          --workdir "$_smoke_dir" --label "$leg"
                          --json "$_smoke_dir/result.json")
  # THE LAUNCHER IS DECLARED, NOT INFERRED — and it is `eval`'d, not word-split.
  # LEG_LAUNCH is the catalogue's launcher argv SHLEX-QUOTED and space-joined by
  # the resolver, so it can be multi-word (`arch -x86_64`) AND a single word can
  # contain spaces. `for t in ${LEG_LAUNCH[...]}` would shred exactly that case;
  # `eval` on the resolver's own quoting is the only correct split, which is why
  # run_leg does the same thing. Empty for a native leg.
  declare -a _smoke_launch=()
  eval "_smoke_launch=(${LEG_LAUNCH[$leg]:-})"
  # ★ `--launcher=<tok>`, NOT `--launcher <tok>` — a launcher TOKEN may itself begin
  # with a dash, and argparse then refuses it ("expected one argument") instead of
  # taking it as the value. ✔MEASURED 2026-08-05 (TF-C121) in the .ps1 twin, whose
  # `--reference-launcher -e` killed the pe64 CLI smoke gate before a single
  # assertion ran — and the caller then classified that argv defect as
  # `smoke: FAIL — CHARGED TO DSS`, accusing the compiler of a bug in the harness's
  # own command line. THIS line has the identical shape and is latent only because
  # every token it has ever seen starts with a letter (`wine`, `qemu-aarch64`,
  # `qemu-x86_64`, `wsl.exe`). It is not hypothetical: legs.json declares
  # `arch -x86_64` for macho64-x86_64 on a darwin/arm64 host, second token `-x86_64`
  # — i.e. it would fire the first time anyone runs that leg on the operator's Mac.
  # Fixed in BOTH drivers in one change; a fix in one and not the other is this
  # project's canonical silent harness bug.
  # (D-HARNESS-DASH-LEADING-LAUNCHER-TOKEN-MISPARSED-AS-AN-OPTION)
  for _t in "${_smoke_launch[@]}"; do _smoke_argv+=("--launcher=$_t"); done
  # ★★ THE REFERENCE'S LAUNCHER IS RESOLVED FROM ITS MEASURED TARGET, NOT FROM
  # THIS HOST'S IDENTITY. The line that used to stand here said "the reference is
  # a LOCAL gcc build, so it always runs natively — no launcher", and passed none.
  # That is only true while every host that owns a reference happens to be able to
  # execute it directly; it is FALSE on a Windows host (its reference is a Linux
  # ELF reached through `wsl.exe -e`) and it becomes false the moment an arm64 host
  # is asked about an x86_64 leg. Its .ps1 twin patched exactly that with
  # `if ($script:HostNeedsWsl)`, a hardcoded host branch, and THAT is why the
  # oracle was unmatched: the reference ran host-native x86_64 while DSS ran arm64
  # under qemu, and every difference was charged to DSS.
  # Both are replaced by ONE question asked of the catalogue —
  # `--launcher-for-target <the reference's MEASURED triple>` — resolved once,
  # above. `--reference-target` is that same MEASURED triple, which is what lets
  # cli-smoke.py refuse to exonerate anything against a control aimed elsewhere.
  # ★ `=` FORM for every launcher token, same rule and same anchor as `--launcher`
  # above (D-HARNESS-DASH-LEADING-LAUNCHER-TOKEN-MISPARSED-AS-AN-OPTION): this is
  # the very option whose SPACE form killed the pe64 gate before one assertion ran.
  if [[ -n "$REF_CLI" ]]; then
    _smoke_argv+=(--reference "$REF_CLI" --reference-target "$REF_CLI_TARGET")
    for _t in ${REF_CLI_LAUNCH[@]+"${REF_CLI_LAUNCH[@]}"}; do _smoke_argv+=("--reference-launcher=$_t"); done
  fi
  # ★ THE LEG'S RUNTIME ENVIRONMENT, APPLIED IN A SUBSHELL. Two things are
  # load-bearing here and both are DECLARED by the leg rather than known to this
  # file:
  #   · LEG_LAUNCH_ENV — for the arm64 leg this is QEMU_LD_PREFIX. Without it
  #     qemu cannot find the guest loader and EVERY exec dies at exit 255, which
  #     would read as 14 DSS failures on a binary that is completely fine.
  #   · the TARGET's loader search variable — a leg whose libraries the harness
  #     STAGED (any provider but `host-system`) needs its own zlib on the loader
  #     path; a `host-system` leg's libraries are already where this machine's
  #     loader looks. ★ THE NAME IS `leg_loader_path_var`'s ANSWER, not a
  #     constant: this was a hardcoded LD_LIBRARY_PATH, which dyld IGNORES — the
  #     SECOND site of D-HARNESS-RUN-ENV-LD-LIBRARY-PATH-INERT-ON-DARWIN, recorded
  #     as item (c) of D-HARNESS-ACQUIRE-ERGONOMIC-GAPS with the instruction to
  #     fix both together so the choice is made in ONE place, target-keyed.
  # The subshell is what keeps both out of the parent: leaking QEMU_LD_PREFIX or
  # a foreign loader path into the rest of the run would silently change how
  # every later leg's processes resolve libraries.
  # ★ `|| _srcc=$?` below is LOAD-BEARING, not style — the same rule this file
  # already states at :3561. Under `set -Eeuo pipefail` a bare `cmd; rc=$?` exits
  # the script ON the non-zero, BEFORE the assignment runs. With that form the
  # whole classification below is DEAD CODE for every failing leg: verdicts 1
  # (charged to DSS) and 3 (the gcc reference fails identically) can never be
  # reached, and one leg's failed smoke ABORTS THE ENTIRE RUN — measured TF-C119,
  # where it killed the run at step 7c and the unit corpus at step 8 never ran.
  # A leg that cannot smoke must yield a LOUD VERDICT, never silence and never a
  # dead sibling: the harness survives everything.
  _srcc=0
  # Resolved HERE, in the foreground, for the reason the subshell body restates.
  _smoke_lvar=""
  [[ "${LEG_LIB_PROVIDER[$leg]}" == "host-system" ]] || _smoke_lvar="$(leg_loader_path_var "$leg")"
  (
    declare -a _envs=()
    eval "_envs=(${LEG_LAUNCH_ENV[$leg]:-})"
    [[ ${#_envs[@]} -eq 0 ]] || export "${_envs[@]}"
    if [[ "${LEG_LIB_PROVIDER[$leg]}" != "host-system" ]]; then
      # $_smoke_lvar was resolved in the FOREGROUND, above: `leg_loader_path_var`
      # dies on an undeclared target OS, and a `die` in THIS subshell would be
      # captured as smoke output and classified as a CLI failure charged to DSS —
      # the harness accusing the compiler of a bug in the harness.
      _lvar="$_smoke_lvar"
      if [[ -n "$_lvar" ]]; then
        _lpath="$(dirname "${LEG_Z_LIB[$leg]}")"
        _lcur="${!_lvar:-}"; [[ -z "$_lcur" ]] || _lpath="$_lpath:$_lcur"
        export "$_lvar=$_lpath"
      fi
    fi
    # The acquired Tcl's script library, same leg-scope, same subshell. The CLI
    # does not embed Tcl — but cli-smoke.py is the arm that would report a
    # Tcl-shaped failure as a DSS defect, so the leg's declared run environment is
    # applied here in full rather than in part.
    [[ -z "${LEG_TCL_SCRIPT_DIR[$leg]:-}" ]] || export TCL_LIBRARY="${LEG_TCL_SCRIPT_DIR[$leg]}"
    python3 "${_smoke_argv[@]}"
  ) > "$_smoke_dir/smoke.log" 2>&1 || _srcc=$?
  sed 's/^/      /' "$_smoke_dir/smoke.log"
  # ★★ EVERY rc THE GATE CAN RETURN HAS ITS OWN ARM — `*)` IS THE LAST RESORT, NOT
  # THE DEFAULT VERDICT (D-HARNESS-CLI-SMOKE-CHARGES-A-LAUNCH-FAILURE-TO-THE-COMPILER).
  # Until TF-C136 this case had arms for 0 and 3 only, so EVERY other rc — including
  # a gate that explicitly declined to attribute, and an argv defect of our own — fell
  # into `*)` and printed as an accusation against the compiler. ✔MEASURED: 14 rows of
  # "CHARGED TO DSS" over an elf64-arm64 binary that never launched, because qemu could
  # not find the guest loader. A default arm that names a culprit is a default arm that
  # will eventually name the wrong one; the fix is to enumerate, and to make the
  # remaining `*)` say "unknown rc" rather than "DSS".
  case "$_srcc" in
    0) CLI_SMOKE_VERDICT["$leg"]="PASS (14/14)"
       pass "[$leg] CLI smoke: 14/14" ;;
    1) # ★ THE ARM THE ENUMERATION LEFT OUT, AND IT IS THE ACCUSATION ITSELF.
       # rc 1 is the gate's "CHARGED TO DSS" — a MATCHED control passed where the
       # subject failed. Until this line it had no arm and fell into `*)`, which
       # prints "an rc the driver does not understand is a driver defect. NOT
       # charged to DSS": a genuine, matched, attributed compiler failure reported
       # as a harness defect and quietly exonerated. That is the FALSE-ACQUITTAL
       # direction — the one that HIDES a real bug — and it was introduced by the
       # very change that removed the false-accusation default. Enumerating the
       # rc table means enumerating ALL of it.
       CLI_SMOKE_FAILS=$((CLI_SMOKE_FAILS + 1))
       CLI_SMOKE_VERDICT["$leg"]="FAIL — CHARGED TO DSS (a MATCHED gcc control passes the assertions this leg fails); see $_smoke_dir/result.json"
       warn "[$leg] CLI smoke RED and CHARGED TO DSS — the reference targets this leg's own target, it launched, and it passes what this binary fails." ;;
    3) CLI_SMOKE_FAILS=$((CLI_SMOKE_FAILS + 1))
       CLI_SMOKE_VERDICT["$leg"]="FAIL — NOT DSS (the gcc reference fails identically); see $_smoke_dir/result.json"
       warn "[$leg] CLI smoke RED, but DSS is NOT implicated — the gcc reference fails the same assertions." ;;
    4) CLI_SMOKE_FAILS=$((CLI_SMOKE_FAILS + 1))
       CLI_SMOKE_VERDICT["$leg"]="FAIL — NOT A VERDICT (unattributable); see $_smoke_dir/result.json"
       # RED and counted, deliberately: an unattributable run is a FAILURE, never a
       # warning. What changed is WHO it names — `dssImplicated` is null, not true.
       warn "[$leg] CLI smoke RED, but this run is NOT A VERDICT about generated code — the subject never launched and/or there was no MATCHED control (the reference targets a different arch/format than this leg). See $_smoke_dir/result.json 'controlState' + 'subjectLaunched'." ;;
    2) CLI_SMOKE_FAILS=$((CLI_SMOKE_FAILS + 1))
       CLI_SMOKE_VERDICT["$leg"]="FAIL — HARNESS ARGV DEFECT (the gate rejected its own arguments); see $_smoke_dir/smoke.log"
       warn "[$leg] CLI smoke could not run: the gate REJECTED THE ARGUMENTS THIS DRIVER PASSED IT. That is our defect, not the compiler's — see $_smoke_dir/smoke.log." ;;
    *) CLI_SMOKE_FAILS=$((CLI_SMOKE_FAILS + 1))
       CLI_SMOKE_VERDICT["$leg"]="FAIL — UNKNOWN rc=$_srcc from the smoke gate; see $_smoke_dir/result.json"
       warn "[$leg] CLI smoke returned rc=$_srcc, which this driver has no arm for. NOT charged to DSS — an rc the driver does not understand is a driver defect. Add an arm here and to the .ps1 twin." ;;
  esac
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
# >>> dss:confound-supply >>>
# THE CONFOUND SUPPLY, PER LEG, FROM THE LEG'S OWN DECLARATION.
# [D-HARNESS-CONFOUND-LEDGER-IS-PER-DRIVER-NOT-PER-LEG.]
#
# What stood here was `read -r -a CONFOUND_PATTERNS <<< "$DSS_CONFOUNDS"` — ONE
# array, built ONCE, before the leg loop, applied to EVERY leg. It is now a
# function called INSIDE the loop, because the answer is a property of the leg.
#
# ★ THE MATCHER WAS ALWAYS TESTED AND THE SUPPLY NEVER WAS, WHICH IS WHY THE
# DEFECT LIVED SO LONG: test-confound-scope.{sh,ps1} assign the pattern array
# directly and then exercise the classifier, so both drivers' matching was pinned
# in detail while the question "where did that array COME FROM" was asked by
# nothing at all. Both self-tests now extract and run THIS function too.
#
# ⚠ `eval` INTO AN ARRAY, exactly like LEG_LAUNCH: the resolver emits
# shlex-quoted words so that a pattern containing a space, a `$` or a backslash
# survives. `for p in ${LEG_CONFOUNDS[...]}` would shred it.
#
# ★★★ AND THE CALL SITE MUST BE A PLAIN ASSIGNMENT FIRST, NEVER A SUBSTITUTION
# INSIDE `eval`. [D-HARNESS-CONFOUND-SUPPLY-REFUSAL-DIES-IN-A-SUBSHELL.]
# `die` is `exit 1`, which exits the SUBSHELL a command substitution runs in, and
# under `set -Eeuo pipefail` bash does NOT propagate a failed substitution inside
# a NON-ASSIGNMENT command. ✔MEASURED end to end with this shipped region and the
# production call site `eval "CONFOUND_PATTERNS=($(leg_confound_patterns "$leg"))"`:
# the refusal printed, then `REACHED THE NEXT STATEMENT. CONFOUND_PATTERNS size=0`,
# then the script COMPLETED with rc=0 — i.e. the whole corpus would have run with
# an EMPTY confound list instead of stopping. A simple `v="$(…)"` DOES propagate
# the status (measured: rc 1, nothing after it runs), which is the same rule this
# driver already records at the SEGQ site for the same reason. The pin that holds
# it is test-driver-contracts.sh's "the refusal STOPS THE DRIVER" case, which
# drives the real call-site shape rather than capturing the substitution's rc.
# ⚠ AND AN UNSET ENTRY IS FATAL, NOT EMPTY: harness_legs.py REFUSES to plan a leg
# that does not declare `confounds`, so the array being absent here means the
# plan this driver eval'd is not the plan that file produces. Substituting an
# empty list would report every failure on the leg as a DSS defect on the
# strength of a transport bug.
leg_confound_patterns() {   # leg_confound_patterns <leg>  -> shlex-quoted words
  local leg="$1"
  if [[ -n "$DSS_CONFOUNDS" ]]; then
    # THE OPERATOR OVERRIDE, and it deliberately applies to EVERY leg: naming a
    # pattern on the command line is stating intent for this run, not inheriting
    # one. Re-quoted through the same transport so both paths produce one shape.
    local _p; local -a _out=()
    for _p in $DSS_CONFOUNDS; do _out+=("$(printf '%q' "$_p")"); done
    printf '%s' "${_out[*]}"
    return 0
  fi
  [[ -n "${LEG_CONFOUNDS[$leg]+set}" ]] || die "[$leg] the resolved leg plan carries NO LEG_CONFOUNDS entry.
      harness_legs.py refuses to plan a leg that does not declare \`confounds\`, so this is a transport
      defect between the resolver and this driver — NOT a leg with nothing earned. Treating it as an
      empty list would silently report every failure on this leg as a DSS defect.
      [D-HARNESS-CONFOUND-LEDGER-IS-PER-DRIVER-NOT-PER-LEG]"
  # ★★ AND WHETHER A MACHINE MEASUREMENT BACKS THAT LIST. A row whose mechanism is
  # a property of THIS MACHINE (the stepping CLOCK_REALTIME) declares
  # `requires: [<probe>]` and is honoured only where the probe MEASURES the defect
  # as PRESENT. An `unprobed` plan is FAIL-SAFE — every conditional row was
  # dropped, so nothing is excused on evidence nobody gathered — but it is NOT fit
  # to run a corpus on: the excusals it withholds would read as compiler
  # regressions, which is the mirror of the defect the gate exists to remove. So
  # the refusal is LOUD and names the flag that produced it.
  # [D-HARNESS-CONFOUND-SCOPE-IS-A-RUN-MODE-NOT-A-HOST]
  # ⚠ THE TWO UNUSABLE GATINGS FAIL FOR OPPOSITE REASONS, so the refusal names
  # both rather than asserting one. Saying "so every conditional row is INACTIVE"
  # is TRUE of `unprobed` and FALSE of `injected` — where the rows ARE honoured,
  # from a file that may describe another machine — and a refusal that misstates
  # what happened is the same defect as a caveat that contradicts the decision
  # beside it. See anchor, ONE LINE, DO NOT WRAP:
  # D-HARNESS-PROBE-VERDICTS-FLAG-INJECTS-AN-UNVALIDATED-PRESENT
  [[ "${LEG_CONFOUND_GATING[$leg]:-}" == 'probed' ]] || die "[$leg] the resolved leg plan says confoundGating='${LEG_CONFOUND_GATING[$leg]:-<unset>}', not 'probed'.
      A conditional confound row (\`requires: [<environment probe>]\`) is honoured ONLY where the named
      probe MEASURED its defect as PRESENT on THIS machine, and this plan carries no such measurement.
      'unprobed' — nothing was measured, so every conditional row is INACTIVE. Safe, and not usable:
        the withheld excusals surface as GENUINE reds and read as compiler regressions.
        Resolve the plan WITHOUT \`--environment-probes skip\` so harness_legs.py measures.
      'injected' — the verdicts were READ FROM A FILE (\`--probe-verdicts\`), so conditional rows ARE
        honoured, on evidence gathered somewhere this driver cannot vouch for. A verdict captured on
        another box would excuse a real miscompile HERE, in silence. Drop the flag and let it measure.
      [D-HARNESS-CONFOUND-SCOPE-IS-A-RUN-MODE-NOT-A-HOST]"
  printf '%s' "${LEG_CONFOUNDS[$leg]}"
}
# <<< dss:confound-supply <<<

# >>> dss:confound-report >>>
# WHY A FAILURE WAS EXCUSED, PRINTED — NOT MERELY DECIDED.
# [D-HARNESS-CONFOUND-SCOPE-IS-A-RUN-MODE-NOT-A-HOST.]
#
# ★★ `earnedOn` FAILED BECAUSE IT IS PROSE NOTHING READS, AND A PROBE RESULT
# NOBODY SEES IS THE SAME FAILURE WITH EXTRA STEPS. So every run states, per leg:
# which environment probes ran, each verdict WITH ITS MEASURED EVIDENCE, and which
# confound rows are consequently ACTIVE vs INACTIVE. A run whose report cannot say
# why a failure was excused has not earned the exclusion.
#
# ⚠ THE LINES ARE GENERATED BY harness_legs.py AND PRINTED VERBATIM HERE. Neither
# driver composes them: two drivers each writing their own account of the same
# decision is how the ledger came to have two answers in the first place, and it is
# what the differential battery's `confound-report` case exists to keep true. This
# function's whole job is transport + the LOUD refusal of an empty report, which is
# the one thing a caller could otherwise not tell from "nothing to say".
print_confound_report() {   # print_confound_report <leg> <report-text>
  local _leg="$1" _report="$2" _line
  if [[ -z "${_report//[[:space:]]/}" ]]; then
    die "[$_leg] the resolved leg plan carries an EMPTY confound report.
      harness_legs.py emits at least one line for every leg — the rows that are ACTIVE, and one line
      per INACTIVE row saying which probe withheld it. An empty report means the account of WHY a
      failure was excused did not arrive, and an unexplained exclusion is not an earned one.
      [D-HARNESS-CONFOUND-SCOPE-IS-A-RUN-MODE-NOT-A-HOST]"
  fi
  while IFS= read -r _line; do
    # ⚠ TRAILING CR STRIPPED, AND THE TWIN DOES THE SAME. The report arrives as one
    # newline-joined scalar, and on a host where it was assembled with CRLF every
    # line would otherwise carry a CR into the log and mangle the tag.
    # [D-HARNESS-MIRROR-CR-CLAIM-IS-VACUOUS.] `Write-ConfoundReport` has always done
    # `$line.TrimEnd("`r")`; this half did NOT, and the differential battery could
    # not see it because its normaliser strips CR from BOTH arms BEFORE comparing.
    # ✔MEASURED: the raw bytes differed, the post-normalise lines were identical.
    # The battery now checks each arm's RAW output for a CR that is not its own line
    # terminator, so this line is load-bearing and its removal reds.
    _line="${_line%$'\r'}"
    [[ -z "$_line" ]] && continue
    info "$_line"
  done <<< "$_report"
}
# <<< dss:confound-report <<<
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
declare -A LEG_SEGMENTS=() LEG_RESUMES=() LEG_FILESDONE=() LEG_FILESINERT=() LEG_LEDGER=() LEG_ABORTS=() LEG_NOTREACHED=() LEG_HYGIENE=()
# The subset of LEG_ABORTS that an EARNED `matches: abort-file` row excused —
# reported, never charged. [D-HARNESS-ABORT-HAS-NO-EARNED-CONFOUND-VOCABULARY]
declare -A LEG_ABORTS_EARNED=()
UNIT_FAILS=0

# ── THE ONE PLACE A LEG'S UNIT CORPUS IS RECORDED AS "NOT RUN" ───────────────
# ★ AN EMPTY SKIP TOKEN IS NOW IMPOSSIBLE BY CONSTRUCTION, NOT BY REVIEW
# (D-HARNESS-UNITS-SKIP-A-LEG-WHOSE-LAUNCHER-IT-SAYS-IS-AVAILABLE).
#
# ✔MEASURED on the operator's Mac at 11e97e0e:
#     macho64-x86_64 (x86_64:macho64-x86_64-darwin-exec): compiled   units: not run []
#       — host darwin/arm64 cannot run x86_64:macho64-x86_64-darwin-exec natively;
#         declared launcher 'arch -x86_64' is available
# A whole leg's corpus was skipped with NO classified reason, in the same sentence
# that said the thing it needed was present. A not-run carrying no class cannot be
# counted as structural / environmental / harness, so the unit ledger can lose an
# entire leg while still LOOKING complete — and "every unit gets a verdict; silence
# about a unit is a harness bug" is the rule that forbids exactly that.
#
# Every `not run` this driver writes goes through THIS function, and it REFUSES to
# write one without a token from the CLOSED vocabulary. That is the by-construction
# half: there is no second place to add a fifth unguarded assignment.
#
# THE VOCABULARY IS READ FROM THE SHARED RESOLVER, never spelled here. A
# driver-local copy of a closed vocabulary is how the two drivers drift, which is
# the standing defect class this file family keeps re-learning
# (D-HARNESS-LIBRARY-ACQUISITION-BUILT-FOR-ONE-LEG-IN-ONE-DRIVER).
# rc DIRECTLY off the `if`, never `x="$(...)"; rc=$?` — under `set -Eeuo pipefail`
# the assignment form exits before the rc can be read.
# stderr is NOT swallowed: if this refuses, the resolver's own diagnostic is the
# thing worth reading, and a `2>/dev/null` here would leave the die below asserting
# a failure it could not describe.
# >>> dss:verdict-vocabulary >>>
# (EXTRACTED AND EXECUTED by test-driver-contracts.sh, so the pin exercises THIS
#  read — CR handling included — instead of a clean literal list of its own. A pin
#  that stubs the vocabulary cannot see the defect the stub papers over; that is
#  exactly how the CRLF bug below survived its first pin.)
if _vocab_out="$(python3 "$LEG_RESOLVER" --catalogue "$LEG_CATALOGUE" --verdict-vocabulary)"; then
  _vocab_rc=0
else
  _vocab_rc=$?
fi
declare -a UNIT_SKIP_VOCAB=()
if [[ "$_vocab_rc" -eq 0 ]]; then
  # A herestring, NOT a pipe and NOT a process substitution: `printf | grep -q`
  # can return 141 (SIGPIPE on the writer) under `pipefail` even on a MATCH, and
  # `mapfile < <(cmd)` swallows the command's rc entirely. Both are ways to make a
  # membership test lie, which is the last thing a guard against silence may do.
  #
  # ★★ THE TRAILING CR IS STRIPPED, AND IT IS NOT DEFENSIVE NOISE.
  # ✔MEASURED 2026-08-06 on Windows/Git Bash: `python3 … --verdict-vocabulary | od -c`
  # emits `r a n \r \n` — Python opens stdout in TEXT MODE and translates \n to
  # \r\n on Windows. `read -r` strips the \n and KEEPS the \r, so every token would
  # be stored as `ran\r`, `poisoned\r`, … and `unit_verdict_token_known` would then
  # reject EVERY LEGITIMATE TOKEN — turning each correctly-classified not-run into a
  # bogus "HARNESS DEFECT", marking those legs `poisoned`, and failing the run. The
  # guard against silence would have become a guard that screams at nothing.
  # Same hazard `parse_segment` documents for a CRLF segment log, same remedy. The
  # .ps1 twin is safe by construction: it reads the tokens through `.Trim()`.
  while IFS= read -r _v; do
    _v="${_v%$'\r'}"
    [[ -z "${_v//[[:space:]]/}" ]] || UNIT_SKIP_VOCAB+=("$_v")
  done <<< "$_vocab_out"
fi
[[ ${#UNIT_SKIP_VOCAB[@]} -gt 0 ]] || die "the leg resolver could not state the CLOSED verdict vocabulary (rc=$_vocab_rc).
      command: python3 $LEG_RESOLVER --verdict-vocabulary
      Without it this driver cannot tell a classified skip from an unclassified one, and an
      unclassified skip is precisely how a leg's entire corpus vanishes from the ledger while
      the summary still reads as full coverage. Refusing to run rather than guess the list."
# <<< dss:verdict-vocabulary <<<
unit_verdict_token_known() {   # unit_verdict_token_known <token>
  local _t
  for _t in "${UNIT_SKIP_VOCAB[@]}"; do [[ "$_t" == "$1" ]] && return 0; done
  return 1
}
# Legs whose not-run could not be classified. Counted and named so the run says so
# out loud AND cannot exit 0 — a harness defect that only warns is a harness defect
# that ships.
UNIT_UNCLASSIFIED=0
declare -a UNIT_UNCLASSIFIED_LEGS=()
unit_not_run() {               # unit_not_run <leg> <token> <detail>
  local leg="$1" token="$2" detail="$3" why=""
  [[ -n "$detail" ]] || detail="<no reason recorded>"
  if [[ -z "$token" ]]; then
    why="this driver recorded a NOT-RUN with an EMPTY verdict token"
  elif ! unit_verdict_token_known "$token"; then
    why="this driver recorded a NOT-RUN with the token '$token', which is OUTSIDE the closed vocabulary (${UNIT_SKIP_VOCAB[*]})"
  fi
  if [[ -n "$why" ]]; then
    UNIT_UNCLASSIFIED=$((UNIT_UNCLASSIFIED + 1)); UNIT_UNCLASSIFIED_LEGS+=("$leg")
    warn "[$leg] HARNESS DEFECT — $why."
    warn "      This leg's ENTIRE unit corpus did not run and the run cannot say under which class."
    warn "      what it did say  : $detail"
    warn "      resolved run plan: mode='${LEG_RUN_MODE[$leg]:-<unset>}'${LEG_LAUNCH[$leg]:+, declared launcher '${LEG_LAUNCH[$leg]}'}"
    # `poisoned` — the closed vocabulary's name for "no artifact was exercised and
    # the reason is OURS". Recording it keeps the leg inside the Step-9 ledger (so
    # it can never ALSO become an accounting hole) and keeps the run from exiting
    # 0. The run still CONTINUES to every other leg: the harness must survive its
    # own defects, not hide them.
    LEG_VERDICT["$leg"]="poisoned"
    LEG_VERDICT_DETAIL["$leg"]="HARNESS DEFECT: $why. $detail"
    UNIT_VERDICT["$leg"]="not run [poisoned] — HARNESS DEFECT: $why. $detail"
    return 0
  fi
  UNIT_VERDICT["$leg"]="not run [$token] — $detail"
}
# Legs whose corpus could not be run because their loadext helper could not be
# STAGED. Its own counter, deliberately: it is neither a fixture compile failure
# (COMPILE_FAILS — the fixture built fine) nor a unit failure (UNIT_FAILS — no
# unit ran), and folding it into either would print a Step-9 line that names the
# wrong thing. It is REPORTED in Step 9 and it REDS the run
# (D-HARNESS-LOADEXT-HELPER-TARGET-BLINDNESS-NOW-ABORTS-THE-RUN).
STAGE_FAILS=0
# tester.tcl's cmdlinearg(testdir) default: the fixture `file mkdir`s this subdir of
# its CWD and cd's into it before any .test body runs, so a test's relative
# `./libtestloadext.so` (`./testloadext.dll` on a Windows Tcl) resolves HERE. The
# harness passes no --testdir override.
SQLITE_TESTDIR_SUBDIR="testdir"
# ⓘ `leg_shared_flags()` USED TO LIVE HERE and is deliberately GONE, not moved to
# a second place. `build.sharedLibFlags` is still declared per leg and still used
# — but only by the CONTROL arm, whose argv harness_legs.py now assembles
# (`loadext_helper_reference_argv`). The primary arm is DSS, which takes an object
# format and not a flag list. A wrapper here with no caller is dead config that
# reads as configuration, and this project has paid for that shape before; the
# declaration's one consumer is now the one file both drivers share.
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
# ★★ THE FILE NAME IS A TARGET FACT, NOT A CONSTANT. It was `libtestloadext.so`
# for all five legs until 2026-08-05. ✔MEASURED from the staged upstream tree,
# test/loadext.test:26-29: the name is chosen from `tcl_platform(platform)` —
# `./testloadext.dll` on a Windows Tcl, `./libtestloadext.so` everywhere else —
# and the `tcl_platform(os) eq Darwin` branch three lines down changes only the
# compiler FLAGS, never the name. So the pe64 fixture looked for a name this
# driver never wrote, `[file exists …]` was FALSE, and it fell through to exactly
# the hardcoded-`gcc` self-build the pre-staging exists to prevent. Each leg now
# DECLARES the name (`build.loadExtHelperName`, cross-checked against the target
# OS by harness_legs.py's lint) and the driver reads it.
#
# ★★★ AND A FAILURE HERE IS A PER-LEG VERDICT, NOT THE END OF THE RUN.
# D-HARNESS-LOADEXT-HELPER-TARGET-BLINDNESS-NOW-ABORTS-THE-RUN. This function used
# to `die` on every failure path, and on 2026-08-05 it did: two legs had already
# gone GREEN (331,351 and 331,355 units) when the pe64 helper failed to link, and
# the `die` took the whole run with it — no Step 9, no ledger, no verdict for the
# two legs that had passed and none for the two that had not been reached. That
# violates the standing rule that the harness SURVIVES everything: "name the file,
# resume AFTER it, report the union. One bad unit must never cost us the other
# thousand." So every failure path below RETURNS, the caller records `poisoned`
# (the ledger's FAILURE class, from the closed vocabulary in
# tests/test_support/arm_verdict_ledger.hpp) and CONTINUES to the next leg, and
# Step 9 both prints the reason and refuses to exit 0.
#
# ⚠ WHY `poisoned` AND NOT A SKIP, since the fixture itself built fine: without a
# target-correct helper this leg's loadext-* units cannot be trusted, and the
# alternative — run the corpus anyway — hands the fixture back to loadext.test's
# own `exec gcc`, which is the wrong-arch helper this anchor is named after. A
# skip would also be WRONG in the other direction: it reads as "nothing to see
# here" and, being environmental, would leave the run green outside strict mode.
# `poisoned` says the artefact's result is not trustworthy, which is the true
# statement, and it is REPORTED rather than silent.
#
# ⚠ AND WHY THE WHOLE LEG, not just its 16 loadext units: this driver has no way
# to give a single upstream unit its own verdict without EXCLUDING it, and
# curating the corpus to get green is forbidden. The cost is stated plainly in the
# verdict detail so a reader knows what this run did not cover.
#
# ★★★ WHO BUILDS IT, SINCE 2026-08-05: DSS ITSELF, FOR THE LEG'S DECLARED
# `sharedLibFormat` — so this function no longer needs ANY third-party compiler and
# no leg's corpus depends on one being installed here [D-HARNESS-CROSS-HOST-ANY-
# TARGET]. The leg's verified target compiler is now the optional CONTROL arm
# (Step 6), built beside the primary where it exists and STAGED only when the
# operator asks (DSS_LOADEXT_HELPER=reference). A leg with neither used to record
# `skipped-build-input-missing` and skip ~330,000 units; it now simply runs.
# What must STILL never happen is a fallback to a compiler that targets something
# else — that is the defect above, and it is invisible in the results. The
# resolver refuses it in both directions: a candidate must PROVE its target with
# `-dumpmachine`, and a failed DSS build is `poisoned` rather than quietly rebuilt
# by the other arm.
# >>> dss:loadext-stage >>>
# EXTRACTED AND EXECUTED by test-confound-scope.sh, at top level, under this
# driver's exact shell options — the same treatment the confound classifier and
# the src-provenance gate get, and for the same reason: a re-implementation in the
# test would stay green while the shipped function broke. The battery there
# asserts the target-keyed NAME, every RETURN path, and — the red-on-disable that
# matters most — that this block contains no `die`.
#
# ★★★ THE BUILD ITSELF LIVES IN harness_legs.py, NOT HERE, AND THAT IS THE POINT.
# This function is now a THIN CALL: it hands the resolver the paths only this
# driver knows and turns ONE report into ONE verdict. Everything that decides —
# which compiler, which object format, whether the artefact is actually a
# loadable shared library, whether a control was possible — is in the file both
# drivers already hard-require, so build-and-test.ps1 gets the identical
# capability from the identical code. That is what stops the pair-gap this
# project keeps paying for (a capability in one driver and not the other is a
# silent harness bug), and it is why the .ps1 can finally stage a helper at all:
# it never could, because it had no way to build one for a target its own host
# has no compiler for [D-HARNESS-PS1-STAGES-NO-LOADEXT-HELPER-COVERAGE-IS-
# UNDECLARED].
#
# TWO FAILURE CLASSES, NOT ONE — this is why the return codes are 1 and 2:
#   1  poisoned                    a REAL failure. The primary build produced no
#                                  loadable library. The run must not exit 0.
#   2  skipped-build-input-missing ENVIRONMENTAL. The operator asked for the
#                                  CONTROL arm (DSS_LOADEXT_HELPER=reference) and
#                                  this machine has no verified target compiler.
#                                  Nothing is wrong; the default would have run.
# Folding these into one code is what would let an operator-selected control arm
# red a run in which nothing is broken — the resolver names the class, and this
# function only translates it.

# loadext_field <json> <key> -> the one string, flattened to a single line.
# Same shape and the same reason as acq_field above: the report is JSON, python3
# is already a hard requirement of this driver, and a bash-side regex over JSON
# is how a driver comes to read a truncated reason and print it as the whole one.
# ★ INSIDE the extracted block ON PURPOSE. test-confound-scope.sh runs the shipped
# staging function verbatim; a reader function defined outside would have to be
# RE-IMPLEMENTED in the test, and a re-implementation is exactly what lets the
# test stay green while the shipped code breaks.
# A key that is absent (or output that is not JSON at all — the resolver's own
# FATAL line) returns non-zero rather than an empty string, so the caller's `||`
# branch can say so instead of printing a confident blank.
loadext_field() {              # loadext_field <json> <key>
  python3 -c 'import json, sys
try:
    report = json.load(sys.stdin)
except ValueError:
    sys.exit(3)
value = report.get(sys.argv[1])
if value is None:
    sys.exit(4)
sys.stdout.write(" ".join(str(value).split()))' "$2" <<< "$1"
}
STAGE_WHY=""
STAGE_CROSSCHECK=""
STAGE_STAGED=""
stage_loadext_extension() {    # stage_loadext_extension <leg> <rundir> -> 0 | 1 | 2
  local leg="$1" rundir="$2" _json _rc=0 _klass
  local name="${LEG_LOADEXT_NAME[$leg]:-}"
  local dstdir="$rundir/$SQLITE_TESTDIR_SUBDIR"
  local work="$OUT_DIR/$leg/loadext-helper"
  STAGE_WHY=""; STAGE_CROSSCHECK=""; STAGE_STAGED=""
  mkdir -p "$dstdir" "$work" || {
    STAGE_WHY="could not create the run's testdir ($dstdir) or the helper's work dir ($work) — check free space and permissions."
    return 1
  }
  # rc DIRECTLY off python3, never after a pipe, and the `if` keeps errexit out of
  # it — `_json="$(…)"; _rc=$?` would EXIT on rc 3/4 before the class could be
  # read, which is the trap that once shipped an entire verdict classifier as
  # DEAD CODE. stderr joins stdout: the resolver puts its FATAL line there and
  # this function must be able to quote it.
  #
  # ★ `--reference-cc "${LEG_CC[$leg]:-}"` IS ALLOWED TO BE EMPTY, and that is the
  # whole de-host-locking. Empty = "no verified target compiler on this machine",
  # which costs the leg its CONTROL and nothing else. It is never a guess: it is
  # exactly what `--resolve-target-cc` accepted at Step 6, or nothing.
  if _json="$(python3 "$LEG_RESOLVER" --catalogue "$LEG_CATALOGUE" \
                --build-loadext-helper "$leg" \
                --helper-builder "$LOADEXT_BUILDER" \
                --dss "$DSS_BIN" \
                --sqlite-src "$SQLITE_DIR/src" --sqlite-bld "$BLD" \
                --dest-dir "$dstdir" --work-dir "$work" \
                --dss-config "$DSS_CONFIG" \
                --reference-cc "${LEG_CC[$leg]:-}" \
                --reference-machine "${LEG_CC_MACHINE[$leg]:-}" 2>&1)"; then _rc=0; else _rc=$?; fi
  # The report is on stdout on EVERY outcome — a driver needs the detail most when
  # it failed — so the fields are read the same way either way. Flattened to one
  # line each: they become ledger DETAILs, which Step 9 prints per leg.
  _klass="$(loadext_field "$_json" verdictClass)" || _klass="?"
  STAGE_WHY="$(loadext_field "$_json" detail)" || STAGE_WHY="the helper build reported nothing this driver could read; raw output: $(printf '%s' "$_json" | tr '\n' ' ')"
  STAGE_CROSSCHECK="$(loadext_field "$_json" crossCheck)" || STAGE_CROSSCHECK=""
  # The FILE the resolver actually wrote, recorded so the caller can carry it into
  # a launcher's own filesystem when the corpus does not run in this one.
  # [D-HARNESS-WSL-LAUNCHED-LEG-RUNDIR-IS-DRVFS]
  STAGE_STAGED="$(loadext_field "$_json" staged)" || STAGE_STAGED=""
  case "$_rc" in
    0) info "[$leg] loadext helper -> $STAGE_STAGED — $STAGE_WHY"
       [[ -z "$STAGE_CROSSCHECK" ]] || info "      $STAGE_CROSSCHECK"
       return 0 ;;
    4) return 2 ;;                        # skipped-build-input-missing
    3) return 1 ;;                        # poisoned
    *) # rc 2 = the resolver's own FATAL (a catalogue/usage defect), or anything
       # else. A class this driver does not recognise is POISONED, never assumed
       # benign: an unreadable outcome is not evidence that the helper is there.
       STAGE_WHY="the helper build exited $_rc, which this driver does not recognise as a verdict class (${_klass}). Treating it as a failure rather than assuming the helper was staged. Raw: $(printf '%s' "$_json" | tr '\n' ' ' | cut -c1-600)"
       return 1 ;;
  esac
}
# <<< dss:loadext-stage <<<
for leg in "${LEG_ORDER[@]}"; do
  # ── the three ways a leg does not reach the corpus, each already NAMED ──────
  # None of them is a host test: they are the recorded OUTCOMES of Step 6 and
  # Step 7. The verdict is left exactly as those steps set it — this loop never
  # invents one, and never silently drops a leg.
  if [[ -z "${LEG_TCL_LIB[$leg]:-}" ]]; then
    unit_not_run "$leg" "${LEG_VERDICT[$leg]}" "${LEG_VERDICT_DETAIL[$leg]}"
    continue                                   # already warned at Step 6/7
  fi
  if [[ "${COMPILE_OK[$leg]:-0}" != "1" ]]; then
    unit_not_run "$leg" "${LEG_VERDICT[$leg]}" "step 7 did not produce a fixture"
    warn "[$leg] corpus skipped — step 7 did not compile the fixture"; continue
  fi
  if leg_run_is_skipped "$leg"; then
    # ★ BUILT, and the build result is REPORTED — this is the whole point of the
    # split. The artifact for this target exists and is on disk; this machine
    # simply cannot execute it, which the resolver said up front and by name.
    unit_not_run "$leg" "${LEG_VERDICT[$leg]}" "${LEG_VERDICT_DETAIL[$leg]}  (the fixture DID build: ${FIXTURE[$leg]})"
    info "[$leg] fixture built but NOT RUN here [${LEG_RUN_VERDICT[$leg]}]: ${LEG_RUN_DETAIL[$leg]}"
    continue
  fi
  # ★★ THE GATE THAT USED TO BE HERE COST A WHOLE LEG ITS CORPUS, AND IT IS GONE.
  # [D-HARNESS-UNITS-SKIP-A-LEG-WHOSE-LAUNCHER-IT-SAYS-IS-AVAILABLE]
  #
  # It read `if [[ -z "${LEG_CC[$leg]:-}" ]]; then <not run>; continue; fi` — i.e.
  # "run this leg's ~330,000 units only if THIS MACHINE owns a compiler that
  # targets that leg". It is a leftover from before the loadext helper was built
  # by DSS: Step 6 stopped recording any verdict for an absent control compiler
  # ("★★ NO VERDICT IS RECORDED HERE ANY MORE, IN EITHER DIRECTION"), but this
  # gate was not removed with it, so a leg with no control was skipped carrying
  # the EMPTY verdict Step 6 had deliberately stopped setting.
  #
  # ✔MEASURED, and this is the whole diagnosis: on the operator's arm64 Mac,
  # `clang -dumpmachine` reports `arm64-apple-darwin24.6.0`, which the resolver
  # correctly REFUSES for macho64-x86_64 ("targets arch 'arm64' …; this leg needs
  # 'x86_64'"). So macho64-arm64 got a control and ran its corpus, macho64-x86_64
  # got none and ran nothing — on the same machine, from the same build, with its
  # declared `arch -x86_64` launcher present and its CLI smoke passing 14/14
  # THROUGH THAT LAUNCHER in the very same run.
  #
  # ⚠ THE CONTROL IS OPTIONAL BY CONSTRUCTION and nothing below needs it: the one
  # consumer is `stage_loadext_extension`, whose `--reference-cc "${LEG_CC[$leg]:-}"`
  # is DECLARED to accept empty ("IS ALLOWED TO BE EMPTY, and that is the whole
  # de-host-locking"). Requiring it here re-locked the corpus to the host that a
  # cross leg exists to escape [D-HARNESS-CROSS-HOST-ANY-TARGET].
  if [[ -z "${LEG_CC[$leg]:-}" ]]; then
    info "[$leg] no CONTROL compiler on this host — the corpus RUNS anyway (the loadext helper comes from DSS); only the helper's cross-check against a second toolchain is lost."
  fi
  # THE DRIVER-SIDE run directory. It exists on EVERY leg regardless of where the
  # corpus actually runs, because this driver has to be able to WRITE into it:
  # the loadext helper is produced by a process on THIS machine and can only land
  # where this machine can put a file. For a `driver` filesystem it is also where
  # the fixture runs; for a foreign one it is the staging area the resolver's
  # copy argv reads FROM. [D-HARNESS-WSL-LAUNCHED-LEG-RUNDIR-IS-DRVFS]
  bin="${FIXTURE[$leg]}"; rundir="$OUT_DIR/$leg/run"; rm -rf "$rundir"; mkdir -p "$rundir"
  # >>> dss:run-dir >>>
  # WHERE THE CORPUS RUNS, DECLARED — never "wherever this driver happens to put
  # its build tree". A launcher that crosses into another kernel does not write
  # onto this filesystem; it reaches this one through a compatibility mount whose
  # POSIX semantics are approximate, and a database engine's corpus is the single
  # worst thing to run over an approximation of POSIX semantics.
  # ✔MEASURED 2026-08-06 (the .ps1 twin's host): /mnt/c is 9p/drvfs with NO
  # `metadata` option, so `chmod 644` reads back as 777 and `chmod 400` as 555 —
  # every mode bit synthesised from ONE Windows attribute. A 2x2 matched control
  # ({DSS, gcc reference} x {DrvFs, ext4}) reproduced all 60 failures under GCC on
  # DrvFs and made every one of them VANISH on ext4.
  # ★ THIS DRIVER SPELLS NO MECHANISM. The verb is the LAUNCHER's declaration and
  # harness_legs.py answers with the directory, the launcher argv (working-
  # directory option already spliced into the right position) and the argv
  # prefixes that create/clear/populate it. On every POSIX host every declared
  # launcher is `driver`, so `$leg_launch_run` is empty, `$leg_run_cd` is
  # `$rundir` and this leg's spawn is byte-for-byte the one it has always been.
  leg_run_plan="$(leg_run_dir_plan "$leg" "$rundir")"
  leg_run_fs="$(run_dir_field "$leg_run_plan" runFilesystem)"
  leg_launch_run="$(run_dir_field "$leg_run_plan" launcherPath)"
  leg_run_cd="${leg_launch_run:-$rundir}"
  # The launcher argv the fixture is spawned through: the DECLARED command with
  # the working-directory option already spliced in by the resolver, so run_leg
  # never learns where in an argv an option has to go. Identical to LEG_LAUNCH
  # whenever the verb needs no option, which is every launcher on a POSIX host.
  LEG_RUN_LAUNCH["$leg"]="$(run_dir_field "$leg_run_plan" launcher)"
  if [[ -n "$leg_launch_run" ]]; then
    info "[$leg] the launcher writes onto ITS OWN filesystem (runFilesystem '$leg_run_fs') — the corpus runs in $leg_launch_run"
    info "      NOT in $rundir, which that launcher reaches only through a compatibility mount whose POSIX file modes are synthesised from one host attribute. [D-HARNESS-WSL-LAUNCHED-LEG-RUNDIR-IS-DRVFS]"
    # PER-LEG, NEVER THE RUN — and never a silent fallback to $rundir, which is
    # the very filesystem this declaration exists to keep the corpus off.
    # `if …; then :; else` because a plain call under `set -Eeuo pipefail` + the
    # ERR trap would EXIT on the non-zero before the classifier could run.
    _rd_ok=1
    if run_dir_argv "$leg" "clear the run directory $leg_launch_run" \
                    "$(run_dir_field "$leg_run_plan" rmTreeArgv)" "$leg_launch_run"; then :; else _rd_ok=0; fi
    if [[ "$_rd_ok" -eq 1 ]]; then
      if run_dir_argv "$leg" "create the run directory $leg_launch_run" \
                      "$(run_dir_field "$leg_run_plan" mkdirArgv)" "$leg_launch_run/$SQLITE_TESTDIR_SUBDIR"; then :; else _rd_ok=0; fi
    fi
    if [[ "$_rd_ok" -eq 0 ]]; then
      STAGE_FAILS=$((STAGE_FAILS + 1))
      LEG_VERDICT["$leg"]="poisoned"
      LEG_VERDICT_DETAIL["$leg"]="the fixture BUILT (${FIXTURE[$leg]}), but this leg's DECLARED run directory could not be prepared, so its corpus was NOT run and this run covers NONE of its units. $RUN_DIR_WHY"
      unit_not_run "$leg" "poisoned" "run directory preparation FAILED: $RUN_DIR_WHY"
      warn "[$leg] POISONED — could not prepare the declared run directory; this leg's corpus is NOT run, the rest of the run CONTINUES:"
      warn "      $RUN_DIR_WHY"
      continue
    fi
  else
    info "[$leg] the corpus runs in this driver's own filesystem (runFilesystem '$leg_run_fs') — $rundir"
  fi
  # <<< dss:run-dir <<<
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
  # sets them. PATH stays absent on purpose: no translation makes one host's
  # PATH mean anything to a foreign process. HOW they cross belongs to the verb.
  # Empty on every POSIX host, where the child simply inherits, so this leaves
  # the .sh's long-standing behaviour untouched.
  # ★ TCL_LIBRARY IS PRESENT AND IS IN THE SECOND GROUP, NOT THE FIRST
  # [D-HARNESS-PS1-TCL-LIBRARY-NOT-FORWARDED-ACROSS-THE-WSL-BOUNDARY]. Its value
  # is a path in THIS driver's namespace, so it crosses through --forward-path,
  # which the resolver puts through the launcher's DECLARED pathTranslation.
  # Omitting it was the defect the .ps1 was found with; putting it in the plain
  # group would be the quieter one, and the resolver refuses that spelling.
  # Only the carrier's NAME and its prior value are per-LEG; WHICH variables are
  # actually carried is decided per SEGMENT, in the loop below, from what is set
  # at that moment (see launch_env_carrier for why that distinction is the
  # difference between a run and a false green).
  leg_env_verb="${LEG_ENV_TRANSFER[$leg]:-inherit}"
  declare -a LEG_ENV_NAMES=()
  mapfile -t LEG_ENV_NAMES < <(printf '%s\n' ${LEG_LAUNCH_ENV[$leg]} | sed -n 's/^\([A-Za-z_][A-Za-z0-9_]*\)=.*/\1/p')
  declare -a LEG_ENV_FORWARD_PLAIN=(SQLITE_TEST_PATTERN_LIST QUICKTEST_OMIT)
  declare -a LEG_ENV_FORWARD_PATHS=(TCL_LIBRARY)
  declare -a LEG_ENV_FORWARD=("${LEG_ENV_FORWARD_PLAIN[@]}" "${LEG_ENV_FORWARD_PATHS[@]}"
                              ${LEG_ENV_NAMES[@]+"${LEG_ENV_NAMES[@]}"})
  leg_carrier_name="$(launch_env_carrier_name "$leg_env_verb")"
  leg_carrier_old=""
  if [[ -n "$leg_carrier_name" ]]; then
    leg_carrier_old="${!leg_carrier_name:-}"
    info "[$leg] the launcher does NOT inherit this driver's environment (envTransfer '$leg_env_verb') — variables that are SET at spawn time cross via $leg_carrier_name; candidates: ${LEG_ENV_FORWARD[*]}"
  fi
  # ★ RECOVERABLE. A helper that cannot be staged costs THIS leg its corpus and
  # nothing else — the run continues to every remaining leg, Step 9 prints the
  # reason, and $STAGE_FAILS keeps the run from exiting 0. Before 2026-08-05 this
  # call was `stage_loadext_extension "$leg" "$rundir"` against a function that
  # `die`d, so one leg's link error ended a run in which two legs had already
  # gone green (D-HARNESS-LOADEXT-HELPER-TARGET-BLINDNESS-NOW-ABORTS-THE-RUN).
  # >>> dss:loadext-verdict >>>
  # ★ TWO CLASSES, AND THE `if`/`else _stage_rc=$?` SHAPE IS LOAD-BEARING: a plain
  # `stage_loadext_extension …; rc=$?` under `set -Eeuo pipefail` + the ERR trap
  # EXITS on the non-zero BEFORE the assignment, which is how a classifier ships
  # as dead code. Both branches record a NAMED verdict from the closed vocabulary
  # in tests/test_support/arm_verdict_ledger.hpp and CONTINUE to the next leg.
  _stage_rc=0
  if stage_loadext_extension "$leg" "$rundir"; then _stage_rc=0; else _stage_rc=$?; fi
  if [[ "$_stage_rc" -eq 2 ]]; then
    # ENVIRONMENTAL, and it can ONLY happen when the operator asked for the
    # control arm — the default builder needs nothing from this machine. It does
    # NOT feed $STAGE_FAILS: nothing failed, so this must not red a run.
    # `leg_marks_missing` is the established path for exactly this class and it
    # preserves the displaced run verdict in the detail.
    leg_marks_missing "$leg" "its corpus is NOT run (the fixture DID build: ${FIXTURE[$leg]})" \
      "DSS_LOADEXT_HELPER=$LOADEXT_BUILDER was requested and this host cannot provide that arm. $STAGE_WHY"
    unit_not_run "$leg" "skipped-build-input-missing" "$STAGE_WHY"
    continue
  fi
  if [[ "$_stage_rc" -ne 0 ]]; then
    STAGE_FAILS=$((STAGE_FAILS + 1))
    LEG_VERDICT["$leg"]="poisoned"
    LEG_VERDICT_DETAIL["$leg"]="the fixture BUILT (${FIXTURE[$leg]}), but this leg's loadext helper extension ('${LEG_LOADEXT_NAME[$leg]:-<undeclared>}') could not be staged, so its corpus was NOT run and this run covers NONE of its units. $STAGE_WHY"
    unit_not_run "$leg" "poisoned" "loadext helper staging FAILED: $STAGE_WHY"
    warn "[$leg] POISONED — loadext helper staging FAILED; this leg's corpus is NOT run, the rest of the run CONTINUES:"
    warn "      $STAGE_WHY"
    continue
  fi
  # <<< dss:loadext-verdict <<<
  # ★ AND INTO THE FILESYSTEM THE FIXTURE WILL ACTUALLY LOOK IN. The helper is
  # BUILT by a process on THIS machine, so it can only be written where this
  # machine can write; when the corpus runs in the launcher's own filesystem the
  # staged file has to be carried across, through the resolver's DECLARED copy
  # argv. Without this, moving to a native run directory would have FIXED the
  # file-permission families and BROKEN loadext.test — trading one manufactured
  # failure class for another. Inert on every leg that runs where this driver
  # does. [D-HARNESS-WSL-LAUNCHED-LEG-RUNDIR-IS-DRVFS]
  if [[ -n "$leg_launch_run" && -n "$STAGE_STAGED" ]]; then
    _cp_ok=1
    if run_dir_argv "$leg" "copy the loadext helper into $leg_launch_run/$SQLITE_TESTDIR_SUBDIR" \
                    "$(run_dir_field "$leg_run_plan" copyArgv)" \
                    "$(launch_path "${LEG_PATH_TRANSLATION[$leg]:-none}" "$STAGE_STAGED")" \
                    "$leg_launch_run/$SQLITE_TESTDIR_SUBDIR/$(basename "$STAGE_STAGED")"; then :; else _cp_ok=0; fi
    if [[ "$_cp_ok" -eq 0 ]]; then
      STAGE_FAILS=$((STAGE_FAILS + 1))
      LEG_VERDICT["$leg"]="poisoned"
      LEG_VERDICT_DETAIL["$leg"]="the fixture BUILT (${FIXTURE[$leg]}) and its loadext helper was produced, but the helper could not be carried into this leg's DECLARED run directory, so its corpus was NOT run. $RUN_DIR_WHY"
      unit_not_run "$leg" "poisoned" "loadext helper transfer FAILED: $RUN_DIR_WHY"
      warn "[$leg] POISONED — the loadext helper could not reach the run directory; this leg's corpus is NOT run, the rest of the run CONTINUES:"
      warn "      $RUN_DIR_WHY"
      continue
    fi
    info "[$leg] loadext helper carried into the launcher's filesystem -> $leg_launch_run/$SQLITE_TESTDIR_SUBDIR/$(basename "$STAGE_STAGED")"
  fi
  # THE CONFOUNDS FOR THIS LEG — read from the leg's OWN declaration (legs.json
  # `confounds`, resolved by harness_legs.py), which is the same declaration
  # build-and-test.ps1 reads. ONE ledger, both drivers.
  # [D-HARNESS-CONFOUND-LEDGER-IS-PER-DRIVER-NOT-PER-LEG]
  declare -a CONFOUND_PATTERNS=()
  # ★★★ TWO STATEMENTS, AND THE FIRST ONE IS WHY THE REFUSAL WORKS.
  # [D-HARNESS-CONFOUND-SUPPLY-REFUSAL-DIES-IN-A-SUBSHELL.] A plain assignment
  # takes the substitution's exit status as its own, so `die` inside
  # leg_confound_patterns stops this driver. Fused into the `eval` below —
  # `eval "CONFOUND_PATTERNS=($(leg_confound_patterns "$leg"))"` — the refusal
  # printed and the run CONTINUED with an empty list (✔MEASURED, rc 0). Do not
  # re-fuse them, and do not make this `local`: `local v="$(…)"` takes `local`'s
  # own status, which is always 0 (✔MEASURED too).
  CONFOUND_SUPPLY="$(leg_confound_patterns "$leg")"
  eval "CONFOUND_PATTERNS=($CONFOUND_SUPPLY)"
  if [[ ${#CONFOUND_PATTERNS[@]} -gt 0 ]]; then
    info "[$leg] confound patterns in force (${#CONFOUND_PATTERNS[@]}): ${CONFOUND_PATTERNS[*]}$( [[ -n "$DSS_CONFOUNDS" ]] && printf '   [operator DSS_CONFOUNDS — applied to EVERY leg]' || printf '   [EARNED on this leg — legs.json `confounds`, provenance per pattern]' )"
  elif [[ "${LEG_CONFOUND_DECLARED[$leg]:-0}" -gt 0 ]]; then
    # ★★ AN EMPTY SUPPLY IS NOT A CLAIM ABOUT THE CATALOGUE. Three different facts
    # produce an empty array and they must read differently: the catalogue declares
    # none, every declared row was GATED OFF by an environment probe, or the
    # transport failed. The third is now impossible here — leg_confound_patterns
    # `die`s and the plain assignment above propagates it — and the first two are
    # told apart by the resolver's own declared-row count, never inferred from the
    # empty array. [D-HARNESS-CONFOUND-SCOPE-IS-A-RUN-MODE-NOT-A-HOST]
    info "[$leg] NO confound patterns IN FORCE, and this is NOT a catalogue that declares none: ${LEG_CONFOUND_DECLARED[$leg]} row(s) ARE declared for this leg and every one of them was gated OFF for this run — see the per-row account immediately below. Every failure here counts, and a clock-family failure here reads as GENUINE."
  else
    info "[$leg] NO confound patterns: this leg's catalogue entry declares \`confounds: []\` (0 rows declared), i.e. nothing has ever been measured as a non-DSS confound HERE, and a confound must be EARNED per platform, never copied from a sibling leg. Every failure here counts."
  fi
  # ★ AND THE ACCOUNT OF WHY — every probe verdict with its evidence, every row's
  # ACTIVE/INACTIVE decision. Printed on the OPERATOR override path too: an
  # operator's list replaces the earned one, and which earned rows it displaced is
  # exactly what a reader of that run needs to know.
  # [D-HARNESS-CONFOUND-SCOPE-IS-A-RUN-MODE-NOT-A-HOST]
  print_confound_report "$leg" "${LEG_CONFOUND_REPORT[$leg]:-}"
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
  declare -a SEG_LOGS=() SEG_LABELS=() SEG_RCS=() SEG_COUNTS=() ABORTS=() ABORT_LOGS=() ABORT_ROWS=() NOT_REACHED=() HYGIENE=() CALIBRATION=()
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
  seg_i=0; resumes=0; last_boundary=""; total_tests=0; total_errors=0; files_done=0; files_inert=0
  seg_summary=""; all_fails=""
  # The previous segment's ZERO-PROGRESS SIGNATURE (zero_progress_signature: its
  # first diagnostic, or a SENTINEL when it said nothing at all), but ONLY when
  # that segment completed zero files. Empty means "the last segment made
  # progress", which is what keeps a genuine mid-corpus crash on the ordinary
  # resume path. See the PRECONDITION FAILURE branch for why one repetition, and
  # not the first occurrence, is the discriminator. PRECONDITION_FAIL is that
  # signature once detected — the classifier below reads it, so the verdict is
  # decided in one place.
  prev_zero_sig=""; PRECONDITION_FAIL=""
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
    leg_env_carrier_out="$(launch_env_carrier "$leg_env_verb" "$leg_xlate" "$leg_carrier_old" \
                             "${#LEG_ENV_FORWARD_PLAIN[@]}" "${LEG_ENV_FORWARD_PLAIN[@]}" \
                             "${#LEG_ENV_FORWARD_PATHS[@]}" "${LEG_ENV_FORWARD_PATHS[@]}" \
                             ${LEG_ENV_NAMES[@]+"${LEG_ENV_NAMES[@]}"})"
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
    # ★★ SEG_COUNTS IS APPENDED HERE, WITH ITS THREE SIBLINGS, AND OVERWRITTEN LATER —
    # never appended from inside a branch. ✔MEASURED 2026-08-07 on the arm64 VPS: it
    # USED to be appended only in the two branches that compute a count, and the
    # PRECONDITION-FAILURE branch `break`s BEFORE reaching them. So `nseg` (which is
    # `${#SEG_LOGS[@]}`) counted a segment that SEG_COUNTS had no entry for, and the
    # ledger loop below died with `SEG_COUNTS[$k]: unbound variable` under `set -u`.
    # ⇒ **the driver crashed while reporting the failure it had just correctly
    # diagnosed** — the precondition detector worked perfectly, named the cause
    # (`qemu-x86_64: Could not open '/lib64/ld-linux-x86-64.so.2'`), refused to spend
    # the remaining 9 resumes on an unresumable fault, and was then killed by its own
    # reporter. An error path that cannot survive being taken is not an error path.
    # ★ The fix is structural rather than "append it in that branch too": these four
    # arrays are INDEX-PARALLEL, and an invariant maintained by remembering it at
    # every append site is one append site away from breaking again. Appending all
    # four together makes the parallelism true by construction.
    SEG_COUNTS+=("tests: (none counted) / errors: (none counted)   [this segment produced no countable output — see its log]")
    facts_f="$scratch/facts.$seg_i"
    parse_segment "$seglog" "$facts_f"
    s_sum="$(fact S "$facts_f")"; s_perm_log="$(fact P "$facts_f")"
    s_last="$(fact T "$facts_f")"; s_done="$(fact D "$facts_f")"
    s_nf="$(fact N "$facts_f")";   s_gaveup="$(fact G "$facts_f")"
    # `fact` returns the LAST value for a key, so this is the innermost frame of
    # the LAST traceback — see the B rule in parse_segment.
    s_blame="$(fact B "$facts_f")"
    s_ok="$(fact K "$facts_f")";   s_fx="$(fact Q "$facts_f")"
    s_diag="$(fact A "$facts_f")"
    files_done=$((files_done + s_nf))
    # `${…:-0}` is load-bearing, not defensive habit: `$(( x + ))` is a bash
    # SYNTAX ERROR, not a zero, so a segment whose facts file carries no `M` —
    # any log produced by a parse that predates this fact — would abort the
    # whole leg's accounting rather than under-report by one segment.
    _seg_inert="$(fact M "$facts_f")"
    files_inert=$((files_inert + ${_seg_inert:-0}))
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
      SEG_COUNTS[$((${#SEG_COUNTS[@]} - 1))]="tests: $(group_digits "$seg_c") / errors: $seg_e   [source: sqlite's own summary line; per-test derivation independently gives $(group_digits "$seg_derived_tests")]"
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
    # ★★ FIRST: IS THIS AN ABORT AT ALL, OR A PRECONDITION FAILURE?
    # [D-HARNESS-ACQUIRED-TCL-DYLIB-HAS-NO-SCRIPT-LIBRARY, the harness half.]
    #
    # ✔MEASURED on the operator's Mac: the fixture could not initialise a SLAVE
    # interpreter (no Tcl script library), so EVERY segment died before running a
    # single file. The engine reported "the UNNAMED file that aborted under
    # permutation 'veryquick' … the log named no resolvable corpus file (last test:
    # none); the resume boundary was FORCED" ELEVEN TIMES and then "resume budget
    # (10) exhausted". It burned its whole budget on a failure that could never
    # make progress, and never named a cause — while the captured log's FIRST LINE
    # said exactly what was wrong.
    #
    # ★★ THIS DOES NOT WEAKEN THE RESILIENCE RULE, AND THE SEPARATION IS EXACT.
    # A fixture abort/crash remains a RECOVERABLE outcome: named, resumed past,
    # reported in the union — one bad unit must never cost us the other thousand.
    # What is added is a DISTINCT case with two conjuncts that a genuine mid-corpus
    # crash cannot satisfy together:
    #   (1) the segment completed ZERO files, and
    #   (2) its ZERO-PROGRESS SIGNATURE is IDENTICAL to the previous segment's,
    #       which also completed zero files.
    # A real crash on the corpus's next file also completes zero files — but the
    # resume boundary STRICTLY ADVANCES every time, so it dies in a different file
    # with a different diagnostic, and (2) fails. Two identical zero-progress
    # segments in a row mean the fixture never started, which is not something
    # resuming can fix. The FIRST such abort is still resumed exactly as today: one
    # attempt is what distinguishes "could not start" from "crashed at the start".
    #
    # ★★ THE SIGNATURE, NOT THE DIAGNOSTIC — see zero_progress_signature() for the
    # measured A/B. This condition used to read `-n "$s_diag" && "$s_diag" ==
    # "${prev_zero_diag:-}"`, which is unsatisfiable for a fixture that dies
    # WITHOUT WRITING A BYTE: there is no `A` fact, so the `-n` conjunct is false
    # forever and the whole resume budget burns on a startup crash. The helper
    # answers a SENTINEL for a segment that said nothing at all, so silence
    # compares equal to silence — and answers empty for a segment that ran tests
    # without completing a file, which is what keeps that case resumable.
    s_zero_sig="$(zero_progress_signature "$s_diag" "$s_ok" "$s_fx" "$s_last")"
    if [[ "$s_nf" -eq 0 && -n "$s_zero_sig" && "$s_zero_sig" == "${prev_zero_sig:-}" ]]; then
      PRECONDITION_FAIL="$s_zero_sig"
      # The log's SIZE is stated because it is the measured evidence for the
      # silent case, and `head` prints nothing at all for a 0-byte log — a report
      # that goes quiet exactly where the fixture did would look like a reporting
      # bug. `if _sz_raw=…` guards the assignment: under `set -e` a failed command
      # substitution in a BARE assignment kills the run, and this one is on the
      # failure path, where dying would cost the leg the diagnosis it just earned.
      _sz="size unknown"
      if _sz_raw="$(LC_ALL=C wc -c < "$seglog" 2>/dev/null)"; then _sz="${_sz_raw//[[:space:]]/} byte(s)"; fi
      warn "[$leg] PRECONDITION FAILURE — the fixture completed ZERO test files in TWO consecutive segments, both ending IDENTICALLY."
      warn "      This is NOT a resumable fixture crash: nothing the resume engine can do changes it,"
      warn "      so the remaining $((DSS_MAX_RESUMES - resumes)) resume(s) are NOT spent on it."
      warn "      what both segments ended with, verbatim, from $seglog:"
      warn "        $s_zero_sig"
      info "      first lines of that log ($_sz):"
      head -6 "$seglog" 2>/dev/null | sed 's/^/        /'
      NOT_REACHED+=("EVERY unit of the '${s_perm:-$DSS_TIER}' corpus — the fixture never completed a single file. PRECONDITION FAILURE: $s_zero_sig")
      break
    fi
    # Carried to the NEXT segment so the comparison above has something to compare
    # against. Cleared by any segment that made progress, which is what keeps a
    # genuine crash on the resilience path.
    if [[ "$s_nf" -eq 0 ]]; then prev_zero_sig="$s_zero_sig"; else prev_zero_sig=""; fi
    # An aborted segment is NOT an empty segment. It printed no summary, so its work
    # is counted from its per-test lines (see the derivation note at the union) —
    # otherwise the totals silently omit everything it did, and a regression inside
    # it never reaches the headline at all.
    der_tests=$((der_tests + seg_derived_tests)); der_errors=$((der_errors + s_fx)); n_derived=$((n_derived + 1))
    total_tests=$((total_tests + seg_derived_tests)); total_errors=$((total_errors + s_fx))
    SEG_COUNTS[$((${#SEG_COUNTS[@]} - 1))]="tests: $(group_digits "$seg_derived_tests") / errors: $s_fx   [source: DERIVED from per-test lines — $(group_digits "$s_ok") ' Ok' + $s_fx '! expected:' + 1; this segment aborted and printed no summary]"
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
    # THE TRACEBACK FIRST, THE TEST NAME SECOND. B is the fixture SAYING which
    # file it was in; T is a PROXY for it, and a measured-wrong one — the pe64
    # wine run resolved `symlink.test-sharedcachesetting` to symlink.test, a file
    # whose own `Time:` line was already in the same log, while the traceback
    # named symlink2.test. T stays because a KILLED segment prints no traceback at
    # all, which is exactly when resume matters most.
    abort_source=""
    abort_file="$(resolve_abort_file "$s_blame" "$scratch/files.txt")"
    if [[ -n "$abort_file" ]]; then
      abort_source="named by the Tcl traceback ($s_blame)"
    else
      abort_file="$(resolve_abort_file "$s_last" "$scratch/files.txt")"
      [[ -z "$abort_file" ]] || abort_source="INFERRED from the last test name ($s_last)"
    fi
    # The boundary must STRICTLY advance every resume, or an aborting file could be
    # re-entered forever. If the aborting file could not be named (or is not past
    # the last completed one), fall back to the last completed file, then force the
    # boundary one corpus entry forward.
    boundary="$abort_file"; forced=0
    if [[ -z "$boundary" ]] || ! str_gt "$boundary" "$s_done"; then boundary="$s_done"; fi
    if ! str_gt "$boundary" "$last_boundary"; then
      forced=1
      boundary="$(first_file_after "$last_boundary" "$scratch/files.txt")"
    fi
    ABORTS+=("${perm:-?}/${abort_file:-?}")
    # ★ THE ABORT'S LOG TRAVELS WITH ITS NAME. An abort is EXCUSED only when its
    # DIAGNOSTIC matches too, and the diagnostic lives in this segment log: a
    # name alone identifies a LOCATION, and a row keyed on a location forgives
    # every future failure in that file — including one this compiler caused.
    # [D-HARNESS-ABORT-CONFOUND-KEYED-ON-LOCATION-NOT-IDENTITY]
    ABORT_LOGS+=("$seglog")
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
    info "        how it was named   : ${abort_source:-(could not be named — no traceback frame and no resolvable test name)}"
    # The unit that died NEVER goes unreported — named when we can name it,
    # described by what we do know when we cannot. Silence about a unit is the defect.
    if [[ -n "$abort_file" ]]; then
      # "last test emitted", not "aborted at": the two are the same thing only
      # when the file got as far as a do_test. symlink2.test died before its
      # first one, so the last name in that log belonged to the PREVIOUS file —
      # which is exactly the confusion the old wording invited.
      NOT_REACHED+=("the REMAINDER of $abort_file under permutation '${perm:-?}' (${abort_source:-source unrecorded}; last test emitted: ${s_last:-none})")
    else
      if [[ "$forced" == 1 ]]; then
        what="the resume boundary was FORCED to ${boundary:-the end of the corpus}, so that one file may have been skipped without a verdict"
      else
        what="the next segment resumes from ${boundary:-the end of the corpus} and will RE-ATTEMPT it"
      fi
      # The traceback frame, when there was one, goes IN the report even though it
      # did not resolve: "the log named nothing" and "the log named something that
      # is not in this corpus" are different facts and the reader needs the second.
      NOT_REACHED+=("the UNNAMED file that aborted under permutation '${perm:-?}' after ${s_done:-the start of the permutation} — the log named no resolvable corpus file (last test: ${s_last:-none}; traceback frame: ${s_blame:-none}); $what")
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
  # ★ THE INDEX-PARALLEL INVARIANT, ASSERTED RATHER THAN ASSUMED. The four SEG_*
  # arrays are indexed by the same k below. They are appended together now (see the
  # note at the append site), so this cannot fire today — which is exactly when to
  # write it down: the previous breakage produced `SEG_COUNTS[$k]: unbound variable`,
  # a message that names bash's rule instead of the harness's contract and sent a
  # reader to the wrong file. A guard that fires here NAMES the invariant, the leg,
  # and the four lengths, so the next divergence is one line of triage rather than a
  # hunt through a 5,700-line driver. Fail loud beats crash loud.
  if [[ ${#SEG_COUNTS[@]} -ne $nseg || ${#SEG_LABELS[@]} -ne $nseg || ${#SEG_RCS[@]} -ne $nseg ]]; then
    die "[$leg] INTERNAL: the per-segment arrays are not index-parallel — SEG_LOGS=$nseg SEG_LABELS=${#SEG_LABELS[@]} SEG_RCS=${#SEG_RCS[@]} SEG_COUNTS=${#SEG_COUNTS[@]}.
      Every segment must append to ALL FOUR at the single append site; a branch that
      exits the segment loop early (the PRECONDITION-FAILURE break is the one that did
      this) must not be able to skip one. The ledger below indexes all four by the same k."
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
      # ★ PUBLISHED BESIDE "files completed", NEVER INSTEAD OF IT, and published
      #   even when it is zero. A count of files that ASSERTED NOTHING is the one
      #   number that makes "1,241 files completed" honest — without it, a run in
      #   which a third of the corpus returned at its first `ifcapable` gate reads
      #   exactly like a run in which all of it executed.
      k_inert="$(fact M "$scratch/facts.$k")"
      printf '   of those, files that ASSERTED NOTHING (%s): %s\n' \
        "${k_inert:-0}" "$(facts I "$scratch/facts.$k" | tr '\n' ' ')"
      k_fails="$(facts X "$scratch/facts.$k" | LC_ALL=C sort -u | tr '\n' ' ')"
      [[ -z "${k_fails// /}" ]] || printf '   failing test(s) seen here: %s\n' "$k_fails"
    done
    [[ ${#CALIBRATION[@]} -eq 0 ]] || { printf '\n== derivation calibration MISMATCH ==\n'; printf '   %s\n' "${CALIBRATION[@]}"; }
    [[ ${#ABORT_ROWS[@]} -eq 0 ]]  || { printf '\n== aborts ==\n'; printf '   %s\n' "${ABORT_ROWS[@]}"; }
    [[ ${#NOT_REACHED[@]} -eq 0 ]] || { printf '\n== NOT REACHED (no verdict) ==\n'; printf '   %s\n' "${NOT_REACHED[@]}"; }
    [[ ${#HYGIENE[@]} -eq 0 ]]     || { printf '\n== process hygiene ==\n'; printf '   %s\n' "${HYGIENE[@]}"; }
    [[ ${#EXCLUDE_PATTERNS[@]} -eq 0 ]] || { printf '\n== EXCLUDED by operator (DSS_TIER_EXCLUDES -> QUICKTEST_OMIT) ==\n   %s\n' "${EXCLUDE_PATTERNS[*]}"; }
  } > "$ledger"

  # ── DID THE DECLARED CAPABILITIES REACH THE TESTS? ─────────────────────────
  # The recipe assertions in Step 4 proved each define reached the COMPILER.
  # That is not the property the operator asked for. A capability can be
  # compiled in and still never be exercised — which is precisely the state this
  # whole change was written against, where DSS compiled fts5.c on every run and
  # every fts5 test file returned at its gate having asserted nothing.
  # So: each declared witness must have emitted at least one real result. The
  # witnesses were chosen from the MEASURED inert set, so this gate was red
  # before the capability set existed and goes green only by it — the
  # red-on-disable property is a consequence of how they were picked, not a
  # separate experiment that could rot.
  if [[ -n "${STAGE_WITNESSES// /}" && $nseg -gt 0 ]]; then
    _inert_union="$scratch/inert-union.txt"
    : > "$_inert_union"
    for ((k = 0; k < nseg; k++)); do facts I "$scratch/facts.$k" >> "$_inert_union"; done
    _ran_union="$scratch/ran-union.txt"
    : > "$_ran_union"
    for ((k = 0; k < nseg; k++)); do facts F "$scratch/facts.$k" >> "$_ran_union"; done
    _gap=""; _checked=0; _absent=""
    for _w in $STAGE_WITNESSES; do
      _cap="${_w%%=*}"; _file="${_w#*=}"
      # A witness the tier never reached is NOT a capability gap — it is a file
      # outside this run's corpus, and reporting it as a gap would make the
      # instrument lie in exactly the direction it exists to prevent.
      # ★ BUT IT IS NOT SILENT EITHER, and that distinction is the whole lesson
      #   of this cycle. ✔MEASURED 2026-08-07: on the Windows driver the fts5,
      #   rtree and session witnesses were ABSENT FROM THE CORPUS ENTIRELY (the
      #   staged test dir had no sibling `ext/`, so sqlite's own
      #   `glob -nocomplain $testdir/../ext/…` returned nothing), and a gate that
      #   only counted DECLARED witnesses printed "every declared capability
      #   reached the tests" over three families that could not possibly have
      #   run. A count of what was actually CHECKED is what makes the green line
      #   mean something.
      if grep -qx -- "$_file.test" "$_ran_union"; then
        _checked=$((_checked + 1))
        grep -qx -- "$_file.test" "$_inert_union" && _gap="$_gap $_cap($_file)"
      else
        _absent="$_absent $_cap($_file.test)"
      fi
    done
    _declared=0; for _w in $STAGE_WITNESSES; do _declared=$((_declared + 1)); done
    [[ -z "$_absent" ]] || warn "[$leg] $((_declared - _checked)) of $_declared capability witness(es) were NOT IN THIS RUN'S CORPUS, so nothing was proved about them:$_absent
      A witness file that never appears is not a passing witness. Either the tier does not include
      it, or the corpus this leg was handed is missing the directory it lives in."
    if [[ -n "$_gap" ]]; then
      CAPABILITY_GAPS+=("$leg:$_gap")
      warn "[$leg] DECLARED CAPABILITIES DID NOT REACH THE TESTS —$_gap"
      warn "      Each of those files ran to completion and asserted NOTHING: it returned at its"
      warn "      \`ifcapable\` gate. The define reached the compiler (Step 4 proved that), so the"
      warn "      library was built without the capability the flag was supposed to enable, or the"
      warn "      fixture linked objects from an older configuration. Reported at the end of the run."
    else
      # CHECKED-of-DECLARED, never just DECLARED: "7 witnesses" was true of a run
      # in which four of them were never in the corpus at all.
      pass "[$leg] every capability witness that was IN THIS CORPUS reached the tests — $_checked of $_declared declared (witnesses: ${STAGE_WITNESSES//=/ -> })"
    fi
  fi

  # ── EARNED vs UNEARNED ABORTS — THE SAME LEDGER, THE OTHER NAME SPACE ─────
  # ANCHOR, ONE LINE, DO NOT WRAP:
  # D-HARNESS-ABORT-HAS-NO-EARNED-CONFOUND-VOCABULARY
  # A `confounds` row keys on a UNIT name; an abort kills the fixture mid-file, so
  # there IS no unit name and a PROVEN-upstream abort could not be recorded in any
  # form. The rule below ("a run with aborts is NEVER green") therefore charged an
  # environment fault to the compiler even after a matched-CRT control had shown
  # both fixtures failing identically. ★ THE RULE IS NOT REMOVED — it is made
  # CONDITIONAL ON PROVENANCE: an `matches: abort-file` row carries the same
  # mandatory earnedOn/earnedAt/mechanism/anchor as any confound, and an UNEARNED
  # abort still fails the leg exactly as before. The classification is ONE
  # implementation in the resolver (an abort is one short string in both drivers,
  # so there is no transport asymmetry to excuse a second matcher).
  declare -a ABORTS_EARNED=() ABORTS_UNEARNED=() ABORT_PROVENANCE=()
  # By INDEX, so each abort is classified against ITS OWN segment log. Walking
  # the names alone would hand every abort the same (or no) diagnostic, which is
  # the location-keyed excusal this conjunction exists to end.
  # ⚠ PLAIN `"${!ABORTS[@]}"`, NOT the `${arr[@]+"${arr[@]}"}` set -u guard:
  # `${!VAR+word}` is INDIRECT EXPANSION, so the guarded form takes the array's
  # VALUE as a variable NAME and dies with "invalid variable name". ✔MEASURED
  # 2026-08-18 on bash 5.2.21 — and it only ever executes when ABORTS is
  # NON-EMPTY, so it was invisible on every clean run and fired on exactly the
  # run whose summary was needed (WSL host, pe64 leg, 4 Wine aborts), killing
  # step 9/9 after every corpus result had already been produced.
  # The plain form is already set -u-safe on an empty array in bash ≥ 4.4.
  for _ai in "${!ABORTS[@]}"; do
    a="${ABORTS[$_ai]}"
    _acrc=0
    if [[ -z "${LEG_ABORT_CONFOUNDS[$leg]:-}" ]]; then
      # No abort row declared for this leg at all — skip the resolve entirely
      # rather than pay for a probe run to be told what the plan already says.
      _acrc=3; _aprov="this leg declares no \`matches: abort-file\` row"
    else
      _aprov="$(python3 "$LEG_RESOLVER" --catalogue "$LEG_CATALOGUE" \
                  --classify-abort "$leg" --abort "$a" \
                  --abort-log "${ABORT_LOGS[$_ai]:-/nonexistent}" \
                  --host-os "$HOST_OS" --host-arch "$HOST_ARCH" 2>&1)" || _acrc=$?
    fi
    if [[ "$_acrc" -eq 0 ]]; then
      ABORTS_EARNED+=("$a"); ABORT_PROVENANCE+=("$a"$'\x1f'"$_aprov")
    else
      ABORTS_UNEARNED+=("$a")
    fi
  done
  LEG_ABORTS_EARNED["$leg"]="${ABORTS_EARNED[*]-}"
  # ★ STATED, NEVER SILENCED. An earned abort stops COUNTING against DSS; it does
  # not stop being reported. Its provenance and — critically — what it took down
  # with it are printed here, because "we proved it is not ours" is a different
  # claim from "nothing happened".
  for _p in ${ABORT_PROVENANCE[@]+"${ABORT_PROVENANCE[@]}"}; do
    warn "[$leg] ABORT ${_p%%$'\x1f'*} — PROVEN NOT DSS's, and it still cost the rest of that file:"
    while IFS= read -r _pl; do [[ -z "$_pl" ]] || info "        $_pl"; done <<< "${_p#*$'\x1f'}"
  done

  if [[ -n "$PRECONDITION_FAIL" ]]; then
    # ★ FIRST ARM, AND IT CARRIES THE DIAGNOSIS. The old engine would have landed
    # in the ABORT arm below and produced "N fixture ABORT(s) [veryquick/?]" —
    # eleven identical rows naming an unnamed file — while sitting on the actual
    # error. A harness that says "the log named no resolvable corpus file" while
    # holding "Can't find a usable init.tcl" is withholding the diagnosis, and
    # THAT, not the retrying, was the expensive half.
    # [D-HARNESS-ACQUIRED-TCL-DYLIB-HAS-NO-SCRIPT-LIBRARY]
    # ★ "each ENDING THE SAME WAY", not "each dying with the same first
    # diagnostic": PRECONDITION_FAIL now carries a zero_progress_signature, which
    # is the `<SILENT: …>` sentinel for a fixture that wrote nothing at all. A
    # verdict that called that sentinel a "diagnostic" would be describing a line
    # the fixture never printed.
    UNIT_VERDICT["$leg"]="FAIL:PRECONDITION FAILURE — THE FIXTURE NEVER STARTED: it completed ZERO test files in $nseg consecutive segment(s), each ending the same way: $PRECONDITION_FAIL  (this is not a resumable crash; the remaining resume budget was NOT spent on it — see $runlog)"
    UNIT_FAILS=$((UNIT_FAILS + 1))
    warn "[$leg] corpus FAIL — PRECONDITION FAILURE, no unit of this leg's corpus ever ran."
    warn "      $PRECONDITION_FAIL"
    info "      $nseg segment(s), $files_done test file(s) completed ($files_inert of them asserted NOTHING), $resumes of $DSS_MAX_RESUMES resume(s) used."
    info "      per-unit ledger: $ledger"
  elif [[ ${#ABORTS_UNEARNED[@]} -gt 0 ]]; then
    # An UNEARNED abort is itself a FAILURE. Resuming recovers the units behind
    # it; it never makes the abort disappear, and a run with an unproven abort is
    # NEVER green. Only a row that SHOWS ITS WORK buys the exemption — see the
    # classification above.
    v="FAIL:${#ABORTS_UNEARNED[@]} fixture ABORT(s) [${ABORTS_UNEARNED[*]}]; recovered by $resumes resume(s); union: $union_summary"
    [[ -z "$derivation" ]] || v="$v [$derivation]"
    [[ ${#real[@]} -eq 0 ]] || v="$v; ${#real[@]} genuine unit failure(s): ${real[*]}"
    [[ ${#NOT_REACHED[@]} -eq 0 ]] || v="$v; ${#NOT_REACHED[@]} unit group(s) NOT REACHED — see $ledger"
    UNIT_VERDICT["$leg"]="$v"; UNIT_FAILS=$((UNIT_FAILS + 1))
    warn "[$leg] corpus FAIL — ${#ABORTS_UNEARNED[@]} UNEARNED abort(s): ${ABORTS_UNEARNED[*]}${ABORTS_EARNED[*]:+   (plus ${#ABORTS_EARNED[@]} PROVEN-not-DSS abort(s), reported above and NOT charged: ${ABORTS_EARNED[*]})}"
    info "      union across $nseg segment(s): $union_summary; $files_done test file(s) completed ($files_inert of them asserted NOTHING)"
    [[ -z "$derivation" ]] || info "        derived from: $derivation"
    [[ ${#real[@]} -eq 0 ]] || info "      ${#real[@]} UNCLASSIFIED failure(s) — not matched by any earned confound, NOT yet attributed to DSS: ${real[*]}"
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
    warn "[$leg] corpus FAIL — $summary; ${#real[@]} UNCLASSIFIED failure(s) — run each against the gcc reference fixture before charging it to DSS: ${real[*]}"
    [[ ${#confound[@]} -gt 0 ]] && info "      (+${#confound[@]} known confound(s) ignored: ${confound[*]})"
  fi
  # ★ ONE call for EVERY failing branch above, deliberately placed after the
  # chain rather than inside the branch that motivated it: an ABORT and a
  # ZERO-FILES run need the prior control every bit as much as a genuine failure,
  # and a lookup wired into one branch silently does not run for the other four.
  # Keyed on the recorded VERDICT, which is the same fact the .ps1's $unitFail is.
  if [[ "${UNIT_VERDICT[$leg]:-}" == FAIL:* ]]; then
    registry_controls_for "$leg" ${real[@]+"${real[@]}"}
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
  LEG_FILESDONE["$leg"]="$files_done"; LEG_FILESINERT["$leg"]="$files_inert"; LEG_LEDGER["$leg"]="$ledger"
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
printf '   compiler : %s @ %s%s%s\n' "$DSS_BIN" "$SRC_HEAD" "$SRC_DIVERGE_NOTE" "$DSS_BUILD_TYPE_NOTE"
printf '   sqlite   : %s @ %s\n' "$SQLITE_DIR" "$(git_head_short "$SQLITE_DIR")"
printf '   recipe   : %s TUs, %s defines (%s)\n' "${#TUS[@]}" "${#RECIPE_DEFS[@]}" "$RECIPE"
printf '   cli recipe: %s TUs, %s defines (%s)\n' "${#CLI_TUS[@]}" "${#CLI_DEFS[@]}" "$CLI_RECIPE"
# The CLI's own ATTRIBUTION ORACLE, and its absence, on the same terms as the
# fixture's below: without it NO smoke failure can be separated from an upstream
# or environment fault, and cli-smoke.py charges an unattributable failure to DSS.
if [[ -n "${REF_CLI:-}" && -f "${REF_CLI:-}" && -x "${REF_CLI:-}" ]]; then
  # The TU count is the LIVE one, read off the derivation two lines above. It was
  # a hardcoded `103` printed directly beneath the line that prints
  # ${#CLI_TUS[@]} — two numbers for one fact, one of which stops being true the
  # day upstream adds a source file, sitting where a reader compares them.
  printf '   cli oracle: %s  (gcc `make sqlite3d` — the SAME %s TUs from the SAME staged tree; the compiler and the -D split differ, see the note at Step 4)\n' \
    "$REF_CLI" "${#CLI_TUS[@]}"
else
  printf '   cli oracle: %sABSENT%s — no smoke failure this run could be attributed. Log: %s\n' \
    "$C_YLW" "$C_RST" "${REF_CLI_LOG:-$OUT_DIR/reference-cli-build.log}"
fi
# The ATTRIBUTION ORACLE, surfaced where a human triaging a failure will see it.
# Its ABSENCE is printed too, and loudly: a missing oracle is the difference
# between attributing a corpus failure and arguing about it (it is what stalled
# walsetlk_recover), so it must never be silent. Step 4 preserves this copy out of
# the make target's path — see "the PRESERVED oracle" there.
# `-f` as well as `-x`: a DIRECTORY passes `-x`, so `-x` alone could report an
# "oracle" that is not a runnable file. The test asserts what is actually claimed.
# ★★★ ONE ORACLE LINE PER LEG, AND A LEG WITH NO ORACLE SAYS SO.
# ANCHOR, ONE LINE, DO NOT WRAP: D-HARNESS-PE64-HAS-NO-SAME-PLATFORM-ORACLE
# This used to be a SINGLE line for the whole run: `oracle : <path>`, printed
# whenever the reference fixture survived. That reference is built by the
# DERIVING host's gcc, so on every host this project uses it is an ELF Linux
# binary — and the line was therefore printed, unqualified, for pe64 and for the
# two mach-o legs, whose failures it cannot be run against at all. A control
# CLAIMED AND NOT HELD is worse than an absent one: it retires exactly the
# suspicion that would have made someone go and look.
# The verdict is now DERIVED per leg from two MEASURED facts — the reference
# binary's own target, read out of its header, against the leg's declared spec —
# by the shared resolver, so both drivers print the same words.
REF_FIXTURE_TARGET=""
if [[ -n "${REF_FIXTURE:-}" && -f "${REF_FIXTURE:-}" && -x "${REF_FIXTURE:-}" ]]; then
  # MEASURED, never assumed. An unidentifiable reference leaves this empty and
  # every leg then reports NO ORACLE — the honest reading of "we do not know
  # what this binary is", and the one that cannot flatter the run.
  _idrc=0; identify_binary_triple "$REF_FIXTURE" || _idrc=$?
  if [[ "$_idrc" -eq 0 ]]; then
    REF_FIXTURE_TARGET="$IDENTIFY_TRIPLE"
  else
    warn "the reference testfixture could not be IDENTIFIED — $IDENTIFY_WHY"
    warn "      Every leg therefore reports NO ORACLE: a control whose platform is unknown is not a control."
  fi
else
  printf '   oracle   : %sno run reference survived this run%s. Log: %s\n' \
    "$C_YLW" "$C_RST" "${REF_BUILD_LOG:-$BLD/reference-build.log}"
fi
for leg in "${LEG_DECLARED[@]}"; do
  while IFS= read -r _oline; do
    [[ -z "$_oline" ]] || printf '   oracle   : %s\n' "$_oline"
  done < <(python3 "$LEG_RESOLVER" --catalogue "$LEG_CATALOGUE" \
             --oracle-report "$leg" \
             --reference-target "$REF_FIXTURE_TARGET" \
             --reference-path "${REF_FIXTURE:-}" \
             --leg-oracle "${LEG_ORACLE[$leg]:-}" \
             --leg-oracle-cc "${LEG_ORACLE_CC[$leg]:-}" \
             --leg-oracle-triple "${LEG_ORACLE_TRIPLE[$leg]:-}" 2>&1)
  # ★★ AND WHAT THE ORACLE WAS *FOR*, on the same shelf as the oracle line itself.
  # [D-HARNESS-BUILD-FAILURE-HAS-NO-PER-TU-ATTRIBUTION] A build that failed and was
  # ATTRIBUTED, whose attribution appears only in the per-leg build chatter 2,000
  # lines up, is an attribution nobody reads — and the summary is the one part of
  # this run that is always read. Printed only when there IS one: a leg that built
  # has nothing to attribute and a blank line here would read as a missing answer.
  if [[ -n "${LEG_BUILD_ATTRIBUTION[$leg]:-}" ]]; then
    printf '   oracle   : %s build attribution: %s\n' "$leg" "${LEG_BUILD_ATTRIBUTION[$leg]}"
  fi
done
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
      printf '   %-14s segments : %s (%s resume(s) of max %s)   %s test file(s) completed (%s asserted NOTHING)   ledger: %s\n' \
        "$leg" "${LEG_SEGMENTS[$leg]}" "${LEG_RESUMES[$leg]}" "$DSS_MAX_RESUMES" "${LEG_FILESDONE[$leg]}" "${LEG_FILESINERT[$leg]:-0}" "${LEG_LEDGER[$leg]}"
      for a in ${LEG_ABORTS[$leg]:-}; do
        # ★ AN EARNED ABORT IS STILL PRINTED, AND STILL SAYS WHAT IT COST. It is
        # excused from the VERDICT, not from the record: "proven not ours" and
        # "nothing happened" are different claims, and the second one would be a
        # lie about a file whose remaining cases never ran.
        # [D-HARNESS-ABORT-HAS-NO-EARNED-CONFOUND-VOCABULARY]
        if [[ " ${LEG_ABORTS_EARNED[$leg]:-} " == *" $a "* ]]; then
          printf '   %-14s aborted  : %s — PROVEN NOT DSS (earned `matches: abort-file` row); NOT charged, and its remaining cases still did NOT run\n' "$leg" "$a"
        else
          printf '   %-14s aborted  : %s — its remaining cases did NOT run\n' "$leg" "$a"
        fi
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

# ── THE sqlite3 CLI, PER LEG — a SECOND artifact needs a SECOND ledger line ──
# ★ ONE LINE PER DECLARED LEG, ALWAYS. The fixture block above is keyed on the
# fixture's outcome and cannot express "the fixture built and the CLI did not"
# (or the reverse, which is reachable: the CLI needs zlib and NOT Tcl, so a
# host with no Tcl builds a CLI and no fixture). A reader must be able to see
# both artifacts' fate for every leg without inferring either from the other.
printf '   --- sqlite3 CLI (full TU: shell.c + the %s library TUs recovered from %s) ---\n' \
  "$(( ${#CLI_TUS[@]} - 1 ))" "$(basename "$AR")"
for leg in "${LEG_DECLARED[@]}"; do
  _cv="$(dss_bh_get_verdict "$leg" sqlite3)"
  if [[ "${CLI_OK[$leg]:-0}" == "1" ]]; then
    printf '   %-14s (%s): %sbuilt%s   smoke: %s\n' "$leg" "${LEG_SPEC[$leg]}" "$C_GRN" "$C_RST" \
      "${CLI_SMOKE_VERDICT[$leg]:-<NO SMOKE VERDICT>}"
  elif [[ -z "$_cv" ]]; then
    # Not selected by DSS_LEGS — the loop never processed it. Named, never blank:
    # a leg that silently produced no line is the shortfall the ledger exists for.
    # ★ THIS BRANCH IS ONLY HONEST BECAUSE THE HARNESS KNOWS WHICH: `LEG_ORDER`
    # is the SELECTED set, so a leg outside it was filtered out and a leg inside
    # it with no verdict is a HARNESS BUG — which the CLI-ledger guard below
    # turns into a non-zero exit rather than a plausible-looking line.
    if printf '%s\n' "${LEG_ORDER[@]}" | grep -qxF "$leg"; then
      printf '   %-14s (%s): %s★ NO CLI VERDICT%s — this leg WAS selected and the CLI loop still recorded nothing for it. That is a harness bug; see the ledger check below.\n' \
        "$leg" "${LEG_SPEC[$leg]}" "$C_RED" "$C_RST"
    else
      printf '   %-14s (%s): %snot processed%s [not-selected-by-runner] — DSS_LEGS='\''%s'\'' did not select this leg\n' \
        "$leg" "${LEG_SPEC[$leg]}" "$C_YLW" "$C_RST" "${DSS_LEGS:-}"
    fi
  else
    printf '   %-14s (%s): %sNOT BUILT%s [%s] — %s\n' "$leg" "${LEG_SPEC[$leg]}" "$C_YLW" "$C_RST" \
      "$_cv" "$(dss_bh_get_detail "$leg" sqlite3)"
  fi
done
# ── THE CLI VERDICT-COMPLETENESS GUARD — the INSTRUMENT, actually wired in ───
# ★ `dss_bh_assert_verdicts` SHIPPED INERT. It was written with the docstring
# "the inert-instrument guard: a ledger nobody filled in must never read as
# clean" and then had ZERO call sites in either driver — the guard against a
# silent ledger was itself the silent thing. It is the check that would have
# caught the two defects found beside it: a CLI loop that skipped legs it should
# have built, and a run reporting "BUILT on 0 of 5" and exiting 0.
#
# OVER THE SELECTED LEGS, not the declared ones, and the distinction is the whole
# reason the ledger line above can say WHICH: a leg DSS_LEGS filtered out was
# never asked for and has no verdict by design; a leg that WAS selected and
# reached no verdict is a harness bug. Reported here, made FATAL below beside the
# fixture ledger's own hole detector, because a run that cannot say what happened
# to an artifact it declared has not proved what it claims.
CLI_LEDGER_HOLE=0
if [[ ${#LEG_ORDER[@]} -gt 0 ]]; then
  dss_bh_assert_verdicts sqlite3 "${LEG_ORDER[@]}" || CLI_LEDGER_HOLE=1
fi

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
# ★ `skipped-launcher-prerequisite-missing` IS ENVIRONMENTAL, beside
# `skipped-emulator-missing`, and it is here because it was NOT: harness_legs.py
# added the token to the closed vocabulary and this list is a HARDCODED MIRROR, so
# a leg carrying it fell through `${VERDICT_COUNT[$v]+set}` into LEDGER_BOGUS and
# printed as "a verdict OUTSIDE the closed vocabulary" — the ledger accusing the
# resolver of a defect in the ledger. Its .ps1 twin's `switch` did the same thing
# via `$vUnclassified`, which is a "★ LEDGER ACCOUNTING HOLE". Both fixed together;
# a fix in one driver and not the other is this project's canonical silent bug.
declare -a LEDGER_VOCAB=(ran expect-error-asserted skipped-by-runOn
                         skipped-no-emulator-declared skipped-emulator-missing
                         skipped-launcher-prerequisite-missing
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
LEDGER_ENVIRONMENTAL=$(( ${VERDICT_COUNT[skipped-emulator-missing]} + ${VERDICT_COUNT[skipped-launcher-prerequisite-missing]} + ${VERDICT_COUNT[skipped-build-input-missing]} ))
LEDGER_HARNESS=$(( ${VERDICT_COUNT[not-selected-by-runner]} ))
LEDGER_SKIPPED=$(( LEDGER_STRUCTURAL + LEDGER_ENVIRONMENTAL + LEDGER_HARNESS ))
LEDGER_FAILED=$(( ${VERDICT_COUNT[poisoned]} ))
LEDGER_ACCOUNTED=$(( LEDGER_VERIFIED + LEDGER_SKIPPED + LEDGER_FAILED ))
LEDGER_TOTAL=${#LEG_DECLARED[@]}
printf '   verdicts : %d verified (%d ran, %d expect-error), %d skipped [structural: %d by-runOn, %d no-emulator-declared; environmental: %d emulator-missing, %d launcher-prerequisite-missing, %d build-input-missing; harness: %d not-selected], %d poisoned  (of %d declared legs)\n' \
  "$LEDGER_VERIFIED" "${VERDICT_COUNT[ran]}" "${VERDICT_COUNT[expect-error-asserted]}" \
  "$LEDGER_SKIPPED" "${VERDICT_COUNT[skipped-by-runOn]}" "${VERDICT_COUNT[skipped-no-emulator-declared]}" \
  "${VERDICT_COUNT[skipped-emulator-missing]}" "${VERDICT_COUNT[skipped-launcher-prerequisite-missing]}" \
  "${VERDICT_COUNT[skipped-build-input-missing]}" \
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
    skipped-emulator-missing|skipped-launcher-prerequisite-missing|skipped-build-input-missing) ENV_SKIPS+=("$leg") ;;
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
# ★ AN UNCLASSIFIED NOT-RUN IS ITS OWN EXIT, AND IT COMES FIRST. The ledger hole
# below is the LEG-level backstop and would (correctly) have caught this too — but
# it names the wrong thing: "a leg reached no verdict" sends a reader to the leg
# plan, when what actually happened is that a leg's ENTIRE UNIT CORPUS was skipped
# for a reason the driver could not classify. Reporting it separately is the
# difference between a diagnosis and a symptom
# (D-HARNESS-UNITS-SKIP-A-LEG-WHOSE-LAUNCHER-IT-SAYS-IS-AVAILABLE).
if [[ "${UNIT_UNCLASSIFIED:-0}" -gt 0 ]]; then
  printf '\n%s%d leg(s) had their UNIT CORPUS skipped with a verdict token this driver could not classify: %s%s\n' \
    "$C_RED" "$UNIT_UNCLASSIFIED" "${UNIT_UNCLASSIFIED_LEGS[*]}" "$C_RST"
  printf 'Each one is warned above with what the driver DID say and the run mode the resolver\n'
  printf 'planned for it. This is a HARNESS defect, not a compiler result: a not-run that names\n'
  printf 'no class cannot be counted as structural, environmental or harness, so the leg would\n'
  printf 'otherwise vanish from the accounting while the summary still read as coverage.\n'
  printf 'The closed vocabulary is: %s\n' "${UNIT_SKIP_VOCAB[*]}"
  exit 1
fi
if [[ "$LEDGER_HOLE" -eq 1 ]]; then
  printf '\n%sTHE LEDGER DOES NOT ADD UP — a declared leg reached no named verdict.%s\n' "$C_RED" "$C_RST"
  printf 'That is a HARNESS defect, not a compiler result: whatever this run proved, it did\n'
  printf 'not prove it about every leg it claimed to cover. Refusing to exit 0.\n'
  exit 1
fi
# The SECOND artifact's ledger, on exactly the same terms. Two artifacts per leg
# means two ways to lose a leg silently, and a hole in either one is the same
# defect: the run cannot say what happened to something it declared.
if [[ "${CLI_LEDGER_HOLE:-0}" -eq 1 ]]; then
  printf '\n%sTHE sqlite3 CLI LEDGER DOES NOT ADD UP — a SELECTED leg reached no CLI verdict.%s\n' "$C_RED" "$C_RST"
  printf 'The leg(s) are named in the base-harness diagnostic above. This is a HARNESS defect:\n'
  printf 'the CLI build loop must reach a named verdict for every leg the runner selected —\n'
  printf 'built, poisoned, or skipped-build-input-missing. Refusing to exit 0.\n'
  exit 1
fi
if [[ "$COMPILE_FAILS" -gt 0 ]]; then
  printf '\n%s%d leg(s) failed to compile the testfixture — inspect the compile.log diagnostics.%s\n' "$C_RED" "$COMPILE_FAILS" "$C_RST"
  exit 1
fi
# ★ A DEGRADED LEG STILL REDS THE RUN. The staging failure is RECOVERABLE — the
# run reached every other leg and every one of them has a verdict — but recovering
# from it is not the same as it not having happened: the leg's entire corpus went
# unrun, so the run must not be able to end 0 and read as coverage
# (D-HARNESS-LOADEXT-HELPER-TARGET-BLINDNESS-NOW-ABORTS-THE-RUN). Its own branch,
# and its own sentence, because "failed to compile the testfixture" above would be
# false: those fixtures built.
if [[ "${STAGE_FAILS:-0}" -gt 0 ]]; then
  printf '\n%s%d leg(s) BUILT their testfixture but could not stage the loadext helper the corpus dlopen()s — their units did NOT run.%s\n' "$C_RED" "$STAGE_FAILS" "$C_RST"
  printf 'Each one is named [poisoned] above with the exact reason and its %s/<leg>/loadext-helper.log.\n' "$OUT_DIR"
  printf 'The run CONTINUED past it and every other leg reached a verdict — that is the recovery, not an excuse.\n'
  exit 1
fi
# ★ THE CLI IS PART OF THE RUN'S VERDICT, NOT AN EXTRA. A CLI that failed to
# build, or whose smoke gate went red, exits non-zero exactly as a fixture
# failure does — otherwise "we can run all sqlite3 CLI in any host" would be a
# claim nothing enforces, and a silently-unbuilt CLI is the shortfall this whole
# step exists to make impossible. The two counters are kept apart so the message
# says WHICH half broke.
if [[ "${CLI_FAILS:-0}" -gt 0 ]]; then
  printf '\n%s%d leg(s) did not produce a sqlite3 CLI — each one'\''s reason is on its CLI ledger line above.%s\n' "$C_RED" "$CLI_FAILS" "$C_RST"
  printf 'Where a compile was actually attempted, the diagnostics are in %s/<leg>/cli/compile.log.\n' "$OUT_DIR"
  exit 1
fi
if [[ "${CLI_SMOKE_FAILS:-0}" -gt 0 ]]; then
  printf '\n%s%d leg(s) failed the sqlite3 CLI smoke gate — inspect %s/<leg>/cli-smoke/smoke.log.%s\n' "$C_RED" "$CLI_SMOKE_FAILS" "$OUT_DIR" "$C_RST"
  printf 'Each leg'\''s line above says whether it was CHARGED TO DSS or exonerated against the\n'
  printf 'gcc reference. Exonerated is still red: it names WHO is at fault, not that it passed.\n'
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
# ★ THE POISONED COUNT IS READ FROM THE LEDGER, NOT SPELLED `0`. It was a literal
# until 2026-08-05, which was true only because every site that set `poisoned` also
# incremented a counter with its own `exit 1` above — a correctness that lived in
# another function and could be broken by adding one verdict site (this cycle added
# one). A closing line that hardcodes its own denominator is the same defect class
# as the ledger accounting hole it sits next to.
# ★ A CAPABILITY GAP FAILS THE RUN, and it fails it HERE rather than mid-leg so
#   the other legs still produce their verdicts — one stage defect must not cost
#   us four legs' results. It is a HARNESS failure, never a DSS one: the compiler
#   built what it was handed, and what it was handed was wrong.
if [[ ${#CAPABILITY_GAPS[@]} -gt 0 ]]; then
  for _g in "${CAPABILITY_GAPS[@]}"; do warn "capability gap — ${_g%%:*}:${_g#*:}"; done
  die "the run built a sqlite that does NOT have capabilities this harness declares.
      Every gap above is a test file that completed and asserted nothing, for a capability
      legs.json stageBuild names explicitly. The corpus totals above are therefore an
      OVERSTATEMENT of coverage: those files are counted as completed.
      [D-HARNESS-CORPUS-FILES-COMPLETE-WITHOUT-ASSERTING-BECAUSE-CAPABILITIES-ARE-OFF]"
fi
pass "$LEDGER_VERIFIED of $LEDGER_TOTAL declared leg(s) VERIFIED: compiled the full-source testfixture + ran the $DSS_TIER unit corpus GREEN — SQLite units pass with dss-code-prime.  ($LEDGER_SKIPPED skipped: $LEDGER_STRUCTURAL structural, $LEDGER_ENVIRONMENTAL environmental, $LEDGER_HARNESS harness — each named above; $LEDGER_FAILED poisoned)"
# The CLI's own closing claim, BOUNDED the same way — it names how many legs
# built it and how many actually EXECUTED the gate, because "built" and "ran the
# 14 assertions" are different facts and a cross leg with no launcher on this
# host legitimately reaches only the first.
_cli_built=0; _cli_smoked=0
for leg in "${LEG_DECLARED[@]}"; do
  [[ "${CLI_OK[$leg]:-0}" == "1" ]] && _cli_built=$((_cli_built + 1))
  [[ "${CLI_SMOKE_VERDICT[$leg]:-}" == PASS* ]] && _cli_smoked=$((_cli_smoked + 1))
done
# ★ "THE REST" IS COUNTED OFF THE BUILT LEGS, NOT OFF THE DECLARED ONES. It used
# to read $LEDGER_TOTAL − $_cli_smoked and describe that as "built but not
# executable on this host", which silently absorbed every leg that was never
# built at all — contradicted by this driver's own CLI ledger three lines
# earlier. A leg that did not build is not a leg this host could not run.
pass "sqlite3 CLI: BUILT on $_cli_built of $LEDGER_TOTAL declared leg(s) from ${#CLI_TUS[@]} full-source TUs; the 14-assertion smoke gate passed on $_cli_smoked (of the $_cli_built built, $(( _cli_built - _cli_smoked )) were NOT executed here — each named above; the $(( LEDGER_TOTAL - _cli_built )) that did not build are named there too)"
