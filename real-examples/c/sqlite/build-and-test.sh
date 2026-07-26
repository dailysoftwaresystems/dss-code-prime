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
#   5. build dss-code-prime              (its default CMake-4 Release build)
#   6. stage the third-party HEADERS DSS parses agnostically (real tcl8.6 + zlib,
#      NO descriptor — D-FFI-SHIPPED-LIBS-OS-ONLY) and obtain the per-leg LIBS the
#      fixture links + runs against (host libtcl/libz; arm64 via ports .deb extract)
#   7. build the full-source `testfixture` with dss-code-prime, once PER LEG,
#      from a generated `.dss-project.json` manifest (dss --project mode):
#        - host  → the native ELF/Mach-O target (runs directly)
#        - arm64 → elf64-aarch64 (Linux x86_64 host only; runs under qemu-aarch64)
#      each leg's manifest declares c-subset / cli / the leg's
#      <targetName>:<formatName> target / the ~185 TUs (absolute `sources`) / the
#      sqlite+tcl8.6+zlib include dirs / the recipe defines / the leg's
#      libtcl8.6.so + libz.so.1 (resolveLibraries); the build routes the binary
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
#                      ARM64_LIBDIR
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
# a zipfile error-message-text env diff; the recover-fault OOM-oracle class).
# (date-2.4c is gcc-EXONERATED — a GCC-built testfixture fails it identically
# [expected NULL, got a date], so it is a sqlite date-format/version/env diff, not
# a DSS miscompile. NOTE: fpconv1-2.0 is DELIBERATELY NOT a confound — a debug DSS
# fixture + a GCC fixture both pass it while a release DSS fixture fails all 500k
# values, so it is a GENUINE release-optimizer fp miscompile [D-OPT-SQLITE-FPCONV1-
# RELEASE-FP-MISCOMPILE] the harness MUST flag red until fixed.)
DSS_CONFOUNDS="${DSS_CONFOUNDS:-^walsetlk- ^walsetlk\. ^busy2- ^zipfile-25\.0$ ^recoverfault ^date-2\.4c$}"
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
clone_or_update "$SQLITE_REPO_URL" "$SQLITE_DIR" ""
[[ -f "$SQLITE_DIR/test/$DSS_TIER.test" || -n "${DSS_TEST_FILE:-}" ]] || \
  die "tier '$DSS_TIER' has no $SQLITE_DIR/test/$DSS_TIER.test (expected veryquick|quick|full|all)."
pass "sqlite ready"

# ── Step 4 — configure + derive the full-source testfixture recipe ───────────
step "4/9  Derive the full-source testfixture recipe (make -n testfixture)"
# mksqlite3c.tcl + the fixture link need tclsh 8.6+ and the Tcl dev files
# (tclConfig.sh — configure detects Tcl through it). apt: tcl (8.6+) + tcl-dev.
ensure_tclsh() {
  local ver=""
  command -v tclsh >/dev/null 2>&1 && ver="$(echo 'puts $tcl_version' | tclsh 2>/dev/null || true)"
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
else
  command -v dpkg >/dev/null 2>&1 && dpkg -s tcl-dev >/dev/null 2>&1 || pkg_install tcl-dev tcl-tk
  command -v dpkg >/dev/null 2>&1 && dpkg -s zlib1g-dev >/dev/null 2>&1 || pkg_install zlib1g-dev zlib
fi
BLD="$SQLITE_DIR/bld-dss"
mkdir -p "$BLD"
( cd "$BLD" && "$SQLITE_DIR/configure" >/dev/null )
# Build the reference fixture BEST-EFFORT: it generates every derived .c
# (parse.c/opcodes.c/ctime.c/tclsqlite-ex.c/fts5.c…) + libsqlite3.a, which the DSS
# TU set needs. A link-stage miss (e.g. a Tcl lib quirk) is tolerated — we only
# harvest the byproducts + the recipe, not gcc's binary.
info "building the reference testfixture (generates derived sources + libsqlite3.a)"
( cd "$BLD" && make -s testfixture USE_AMALGAMATION=0 -j"$JOBS" >/dev/null 2>&1 ) || \
  warn "reference gcc testfixture did not fully link (tolerated — harvesting generated sources + recipe)"
# Emit the recipe: with testfixture removed but its prereqs built, `make -n` prints
# the single testfixture cc/link command (the source of TUs + defines + -I dirs).
rm -f "$BLD/testfixture"
RECIPE="$OUT_DIR/testfixture-recipe.txt"
mkdir -p "$OUT_DIR"
( cd "$BLD" && make -n testfixture USE_AMALGAMATION=0 ) > "$RECIPE" 2>&1 || true
# `make -n testfixture` is essentially ONE cc command (the fixture link); join its
# backslash-continuations, then extract each token-type from the whole blob.
BLOB="$(sed ':a;N;$!ba;s/\\\n/ /g' "$RECIPE" | tr '\t' ' ')"
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
# Headers are leg-INDEPENDENT: DSS parses the host tcl8.6/zlib headers agnostically
# (ABI is irrelevant at parse). tcl8.6 headers sit in a private subdir (safe on -I);
# zlib.h sits directly in /usr/include (would shadow the OS descriptors) → stage a
# private copy of just zlib.h + zconf.h.
THIRD_PARTY_INCS=()
# Portable multi-root find. A hardcoded start-dir list mixes Linux + macOS paths
# (e.g. `/opt/homebrew/*`, `/usr/lib64`); `find` EXITS NON-ZERO on a start dir
# that does not exist on this host, which under `set -Eeuo pipefail` would abort
# the whole harness. Restrict the search to the roots that EXIST (a genuinely
# absent header/lib is still caught by the explicit `… || die` checks below).
# Usage: find_in <dir>... -- <find-expr>...   (stderr suppressed).
find_in() {
  local -a roots=()
  while [[ $# -gt 0 && "$1" != "--" ]]; do [[ -d "$1" ]] && roots+=("$1"); shift; done
  [[ "${1:-}" == "--" ]] && shift
  [[ ${#roots[@]} -gt 0 ]] || return 0
  find "${roots[@]}" "$@" 2>/dev/null
}
tcl_header_dir() {
  local d=""
  local cfg; cfg="$(find_in /usr/lib /usr/lib64 /usr/local/lib /opt/homebrew/lib -- -name tclConfig.sh | head -1)"
  [[ -n "$cfg" ]] && d="$( . "$cfg" >/dev/null 2>&1; printf '%s' "${TCL_INCLUDE_SPEC#-I}" )"
  [[ -n "$d" && -f "$d/tcl.h" ]] || d="$(dirname "$(find_in /usr/include /usr/local/include /opt/homebrew/include -- -name tcl.h -path '*tcl8*' | head -1)")"
  [[ -f "$d/tcl.h" ]] || d="$(dirname "$(find_in /usr/include /usr/local/include /opt/homebrew/include -- -name tcl.h | head -1)")"
  printf '%s' "$d"
}
TCL_INC="$(tcl_header_dir)"
[[ -f "$TCL_INC/tcl.h" ]] || die "tcl.h not found — install tcl-dev / tcl8.6-dev (or brew tcl-tk)."
ZINC="$BLD/zinc"; mkdir -p "$ZINC"
ZH="$(find_in /usr/include /usr/local/include /opt/homebrew/include -- -maxdepth 3 -name zlib.h | head -1)"
[[ -n "$ZH" ]] || die "zlib.h not found — install zlib1g-dev (or brew zlib)."
cp -f "$ZH" "$ZINC/"
for zc in "$(dirname "$ZH")/zconf.h" $(find_in /usr/include -- -maxdepth 3 -name zconf.h); do
  [[ -f "$zc" ]] && { cp -f "$zc" "$ZINC/"; break; }
done
THIRD_PARTY_INCS=("$TCL_INC" "$ZINC")
info "tcl headers: $TCL_INC   zlib headers: $ZINC (staged)"

# host libraries (the native leg links + runs against these)
find_first() { local n; for n in "$@"; do local h; h="$(find_in /usr/lib /lib /usr/local/lib /opt/homebrew/lib -- -name "$n" | head -1)"; [[ -n "$h" ]] && { printf '%s' "$h"; return; }; done; }
HOST_TCL_LIB="$(find_first 'libtcl8.6.so' 'libtcl8.6.so.0' 'libtcl8.6.dylib')"
HOST_Z_LIB="$(find_first 'libz.so' 'libz.so.1' 'libz.dylib')"
[[ -n "$HOST_TCL_LIB" ]] || die "host libtcl8.6 not found (install tcl-dev / brew tcl-tk)."
[[ -n "$HOST_Z_LIB"   ]] || die "host libz not found (install zlib1g-dev / brew zlib)."
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

# ── Step 7 — build the full-source testfixture with dss-code-prime, per leg ──
step "7/9  Build the full-source testfixture (dss-code-prime --project), per leg"
declare -A FIXTURE=()          # leg -> binary path (on success)
declare -A COMPILE_OK=()
COMPILE_FAILS=0
# the per-leg manifest is emitted as clean JSON by python3 (a harness dep now).
ensure_cmd python3 python3
# The leg-INDEPENDENT include-dir set: the sqlite recipe dirs + the staged
# third-party tcl8.6/zlib headers + the build tree ($BLD, which resolves the
# recipe's relative `-I.`). These dirs, the TU list (${TUS[@]}) and the recipe
# defines (${RECIPE_DEFS[@]}, already `-D`-stripped) are the SAME inputs the
# per-file CLI fed; here they populate a `.dss-project.json` manifest instead.
declare -a INC_DIRS=()
for d in "${SQLITE_INCS[@]}" "${THIRD_PARTY_INCS[@]}" "$BLD"; do INC_DIRS+=("$d"); done

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
for leg in "${LEG_ORDER[@]}"; do
  spec="${LEG_SPEC[$leg]}"; fmt="${spec##*:}"; outd="$OUT_DIR/$leg"; log="$outd/compile.log"
  manifest="$outd/$leg.dss-project.json"
  mkdir -p "$outd"
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
    FIXTURE["$leg"]="$bin"; COMPILE_OK["$leg"]=1
    pass "[$leg] testfixture -> $bin$(compile_time_suffix "$log")"
  fi
done

# ── Step 8 — run the .test UNIT CORPUS through each leg's fixture ─────────────
step "8/9  Run SQLite unit corpus ($DSS_TIER.test) on each leg + classify failures"
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
# S is the WHOLE line ("0 errors out of 9 tests on <host> …"), which is what this
# harness has always printed — E/C carry the numbers so nothing has to re-parse it.
# NOTE the leading '!' on the canonical failure list: finalize_testing emits
# `!Failures on these tests: …` (tester.tcl ~1304), which a `^Failures` pattern
# silently never matches.
parse_segment() {              # parse_segment <log> <out-facts>
  LC_ALL=C awk '
    /^Time: / { if (NF==4 && $4=="ms") { print "F\t" $2; nf++; lastdone=$2; next } }
    /^\*\*\* Giving up/ { gaveup=1; next }
    /^!?Failures on these tests:/ {
      line=$0; sub(/^!?Failures on these tests:[ \t]*/, "", line);
      n=split(line, a, /[ \t]+/); for (i=1;i<=n;i++) if (a[i]!="") print "X\t" a[i]; next }
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
          print "N\t" nf+0; print "D\t" lastdone }
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

  # >>> dss:corpus-loop >>>
  # Segment queue, one record per fixture invocation, fields separated by US
  # (\x1f):  kind | perm | label | patternfile | arg1 | arg2
  # Segment 0 is EXACTLY today's invocation (`fixture <tier>.test`) so a run with
  # no abort is bit-for-bit the run it always was; resume segments are only ever
  # spliced in by an abort.
  US=$'\x1f'
  SEGQ=("tier${US}${US}$DSS_TIER.test${US}${US}$TEST_FILE${US}")
  declare -a SEG_LOGS=() SEG_LABELS=() SEG_RCS=() ABORTS=() ABORT_ROWS=() NOT_REACHED=() HYGIENE=()
  # PRE-FLIGHT HYGIENE — a leftover fixture from a dead run holds file handles (the
  # abort class this engine exists for IS a leaked handle) and can make this run
  # fail for a reason this run did not cause. We hold the run lock, so anything
  # still running OUR fixture path is a leftover.
  while IFS= read -r k; do
    [[ -z "$k" ]] || { warn "[$leg] LEFTOVER FIXTURE: $k"; HYGIENE+=("$k"); }
  done < <(stop_our_fixtures "$bin" 'pre-flight')
  [[ -z "$LOCK_STOLEN" ]] || HYGIENE+=("took over a STALE run lock left by PID $LOCK_STOLEN")
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
    while IFS= read -r k; do
      [[ -z "$k" ]] || { warn "[$leg] LEFTOVER FIXTURE: $k"; HYGIENE+=("$k"); }
    done < <(stop_our_fixtures "$bin" "after segment $((seg_i + 1))")
    SEG_LOGS+=("$seglog"); SEG_LABELS+=("$s_label"); SEG_RCS+=("$segrc")
    facts_f="$scratch/facts.$seg_i"
    parse_segment "$seglog" "$facts_f"
    s_sum="$(fact S "$facts_f")"; s_perm_log="$(fact P "$facts_f")"
    s_last="$(fact T "$facts_f")"; s_done="$(fact D "$facts_f")"
    s_nf="$(fact N "$facts_f")";   s_gaveup="$(fact G "$facts_f")"
    files_done=$((files_done + s_nf))
    all_fails="$all_fails $(facts X "$facts_f" | tr '\n' ' ')"
    seg_i=$((seg_i + 1))
    if [[ -n "$s_sum" ]]; then
      seg_summary="$s_sum"
      total_errors=$((total_errors + $(fact E "$facts_f")))
      total_tests=$((total_tests + $(fact C "$facts_f")))
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
  # For a single clean segment the summary text is the fixture's own, byte for byte.
  union_summary="$total_errors errors out of $total_tests tests (union of $nseg segment(s))"
  if [[ $nseg -eq 1 ]]; then summary="$seg_summary"; else summary="$union_summary"; fi
  declare -a real=() confound=()
  for t in $faillist; do
    is_c=0
    for p in "${CONFOUND_PATTERNS[@]}"; do [[ "$t" =~ $p ]] && { is_c=1; break; }; done
    if [[ "$is_c" == 1 ]]; then confound+=("$t"); else real+=("$t"); fi
  done
  # Per-unit ledger — every file that reached a verdict, every abort, every gap.
  { printf "sqlite unit ledger — leg '%s', tier '%s', %s segment(s), %s resume(s)\n" "$leg" "$DSS_TIER" "$nseg" "$resumes"
    for ((k = 0; k < nseg; k++)); do
      k_sum="$(fact S "$scratch/facts.$k")"
      printf '\n== segment: %s   rc=%s   %s\n   log: %s\n' \
        "${SEG_LABELS[$k]}" "${SEG_RCS[$k]}" "${k_sum:-ABORTED (no summary line)}" "${SEG_LOGS[$k]}"
      printf '   files completed (%s): %s\n' "$(fact N "$scratch/facts.$k")" "$(facts F "$scratch/facts.$k" | tr '\n' ' ')"
    done
    [[ ${#ABORT_ROWS[@]} -eq 0 ]]  || { printf '\n== aborts ==\n'; printf '   %s\n' "${ABORT_ROWS[@]}"; }
    [[ ${#NOT_REACHED[@]} -eq 0 ]] || { printf '\n== NOT REACHED (no verdict) ==\n'; printf '   %s\n' "${NOT_REACHED[@]}"; }
    [[ ${#HYGIENE[@]} -eq 0 ]]     || { printf '\n== process hygiene ==\n'; printf '   %s\n' "${HYGIENE[@]}"; }
    [[ ${#EXCLUDE_PATTERNS[@]} -eq 0 ]] || { printf '\n== EXCLUDED by operator (DSS_TIER_EXCLUDES -> QUICKTEST_OMIT) ==\n   %s\n' "${EXCLUDE_PATTERNS[*]}"; }
  } > "$ledger"

  if [[ ${#ABORTS[@]} -gt 0 ]]; then
    # An abort is itself a FAILURE. Resuming recovers the units behind it; it never
    # makes the abort disappear, and a run with aborts is NEVER green.
    v="FAIL:${#ABORTS[@]} fixture ABORT(s) [${ABORTS[*]}]; recovered by $resumes resume(s); union: $union_summary"
    [[ ${#real[@]} -eq 0 ]] || v="$v; ${#real[@]} genuine unit failure(s): ${real[*]}"
    [[ ${#NOT_REACHED[@]} -eq 0 ]] || v="$v; ${#NOT_REACHED[@]} unit group(s) NOT REACHED — see $ledger"
    UNIT_VERDICT["$leg"]="$v"; UNIT_FAILS=$((UNIT_FAILS + 1))
    warn "[$leg] corpus FAIL — ${#ABORTS[@]} abort(s): ${ABORTS[*]}"
    info "      union across $nseg segment(s): $union_summary; $files_done test file(s) completed"
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
  unset real confound ABORTS ABORT_ROWS NOT_REACHED HYGIENE SEG_LOGS SEG_LABELS SEG_RCS TIER_PERMS
done

# ── Step 9 — results ─────────────────────────────────────────────────────────
step "9/9  Results"
printf '   compiler : %s @ %s\n' "$DSS_BIN" "$(git -C "$SRC_DIR" rev-parse --short HEAD)"
printf '   sqlite   : %s @ %s\n' "$SQLITE_DIR" "$(git -C "$SQLITE_DIR" rev-parse --short HEAD)"
printf '   recipe   : %s TUs, %s defines (%s)\n' "${#TUS[@]}" "${#RECIPE_DEFS[@]}" "$RECIPE"
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
