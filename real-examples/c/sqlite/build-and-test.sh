#!/usr/bin/env bash
#
# real-examples/c/sqlite/build-and-test.sh
# ─────────────────────────────────────────────────────────────────────────────
# SQLite-readiness probe harness for DSS Code Prime.
#
# Drives the full "can DSS Code Prime build SQLite?" pipeline end-to-end on a
# Linux or WSL host:
#
#   1. verify the host is Linux / WSL and online
#   2. clone-or-update  dss-code-prime   into  ~/src
#   3. clone-or-update  sqlite/sqlite    into  ~/src
#   4. amalgamate SQLite -> sqlite3.c    (autotools: `make sqlite3.c`, needs tclsh)
#   5. build dss-code-prime              (its default CMake-4 Release build)
#   6. compile with dss-code-prime for windows + macos + linux + linux-arm64:
#        - RUNNABLE targets (both Linux legs) → 2-TU sqlite3.c + shell.c = a real
#          `sqlite3` CLI binary
#        - COMPILE-ONLY targets (windows/macos) → the amalgamation alone (no runner)
#                                        -> ~/src/dss-code-prime/build/real-examples/c/sqlite/<label>/
#   7. test each runnable CLI with a SQLite smoke unit (SELECT sum → 42 +
#        --version → 3.54.0) against EXPECTED output: x86_64-linux NATIVE +
#        arm64-linux under qemu-aarch64 (QEMU_LD_PREFIX = the aarch64 sysroot)
#   8. summarise results + exit non-zero if an expected step failed for the
#        sqlite-RUN-green goal: a RUNNABLE-target compile miss OR a runnable-target
#        smoke miss. Compile-only (windows/macos) misses at known per-OS frontiers
#        (os_win F001D / macOS layout) are REPORTED as warnings, not fatal.
#
# DESIGN: every step is idempotent and FAIL-LOUD. The compiler is young, so step 6
# is the current FRONTIER — sqlite3.c + shell.c (dense C) will surface unsupported
# constructs and the harness reports exactly where it stopped, per target. Re-run
# it as the compiler matures; the green frontier advances down the steps. NOTE:
# dss-code-prime exits 0 even on fatal compile errors, so step 6 reads success from
# the DIAGNOSTICS (no `error[` line), not the exit code (probe a6b65f8b).
#
# NOTE on amalgamation: the canonical `sqlite/sqlite` repo is AUTOTOOLS, not CMake
# (there is no root CMakeLists.txt — the CMake "amalgamation" projects are third-
# party wrappers). The amalgamation is produced by `./configure && make sqlite3.c`
# (requires a `tclsh` 8.6+ interpreter), which is what this harness uses.
#
# Overridable via env: DSS_REPO_URL DSS_BRANCH SQLITE_REPO_URL SRC_DIR SQLITE_DIR
#                      OUT_DIR JOBS  (see the config block below).
# ─────────────────────────────────────────────────────────────────────────────
set -Eeuo pipefail

# ── config (override via environment) ───────────────────────────────────────
DSS_REPO_URL="${DSS_REPO_URL:-git@github.com:dailysoftwaresystems/dss-code-prime.git}"
DSS_BRANCH="${DSS_BRANCH:-main}"                 # compiler is built from main per spec; override to probe a feature branch
SQLITE_REPO_URL="${SQLITE_REPO_URL:-git@github.com:sqlite/sqlite.git}"
SRC_DIR="${SRC_DIR:-$HOME/src/dss-code-prime}"
SQLITE_DIR="${SQLITE_DIR:-$HOME/src/sqlite}"
OUT_DIR="${OUT_DIR:-$SRC_DIR/build/real-examples/c/sqlite}"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"
LANGUAGE="c-subset"
MIN_CMAKE_MAJOR=4

# os-label = <targetName>:<formatName>  (the deliverable OSes; --target is repeatable).
# Two Linux legs (x86_64 native + arm64 under qemu) are the RUNNABLE targets — they
# compile the real `sqlite3` CLI (sqlite3.c + shell.c, 2-TU) and run the smoke unit
# (step 7). windows/macos stay COMPILE-ONLY (no runner on this Linux CI host): the
# amalgamation-only single-TU compile is the deliverable-surface witness there.
declare -a TARGETS=(
  "windows=x86_64:pe64-x86_64-windows-exec"
  "macos=arm64:macho64-arm64-darwin-exec"
  "linux=x86_64:elf64-x86_64-linux-exec"
  "linux-arm64=arm64:elf64-aarch64-linux-exec"
)
# RUNNERS: the label -> run-command PREFIX for every target that produces a runnable
# binary on THIS host. A native x86_64-linux artifact runs directly (empty prefix);
# an arm64-linux ELF runs under user-mode qemu (QEMU_LD_PREFIX points its dynamic
# loader + libc at the aarch64 sysroot). A label ABSENT from RUNNERS is compile-only
# (windows/macos) — step 6 compiles it single-TU (amalgamation, no main) and step 7
# skips it (no runner). This associative map is the SINGLE source of "what is
# runnable and how" — 2-TU compilation (step 6) and the smoke (step 7) both key on it.
declare -A RUNNERS=(
  ["linux"]=""
  ["linux-arm64"]="qemu-aarch64"
)
# QEMU_SYSROOT: the aarch64 sysroot qemu resolves the ELF interpreter + shared libs
# against (Debian/Ubuntu cross package `libc6-arm64-cross` installs it here).
QEMU_SYSROOT="${QEMU_SYSROOT:-/usr/aarch64-linux-gnu}"

# DSS_OS: optional comma-separated target-label filter for FAST iteration (e.g.
# DSS_OS=linux compiles only the x86_64-linux leg — faster while ONE target is the
# active frontier). Default (unset) = every deliverable target; the FINAL green run
# leaves DSS_OS unset to verify windows + macos + both Linux legs together.
if [[ -n "${DSS_OS:-}" ]]; then
  declare -a _dss_filtered=()
  for _t in "${TARGETS[@]}"; do
    case ",${DSS_OS}," in *",${_t%%=*},"*) _dss_filtered+=("$_t");; esac
  done
  if [[ ${#_dss_filtered[@]} -eq 0 ]]; then
    echo "DSS_OS='${DSS_OS}' matched no target label (windows|macos|linux|linux-arm64)" >&2
    exit 2
  fi
  TARGETS=("${_dss_filtered[@]}")
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

# ── package install helpers (Debian/Ubuntu/WSL) ──────────────────────────────
SUDO=""; [[ "$(id -u)" -eq 0 ]] || SUDO="sudo"
APT_UPDATED=0
apt_install() {                 # apt_install <pkg>...
  command -v apt-get >/dev/null 2>&1 || die "apt-get not found — this harness targets Debian/Ubuntu/WSL. Install missing tools manually: $*"
  if [[ "$APT_UPDATED" -eq 0 ]]; then $SUDO apt-get update -y >/dev/null; APT_UPDATED=1; fi
  info "installing: $*"
  $SUDO DEBIAN_FRONTEND=noninteractive apt-get install -y "$@" >/dev/null
}
ensure_cmd() {                  # ensure_cmd <command> <apt-pkg>
  command -v "$1" >/dev/null 2>&1 || apt_install "$2"
}

# resolve a repo's default branch (origin/HEAD) — sqlite's may be master/trunk, not main
default_branch() {
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

# ── Step 1 — host is Linux/WSL and online ────────────────────────────────────
step "1/8  Host check (Linux / WSL, online)"
[[ "$(uname -s)" == "Linux" ]] || die "requires Linux or WSL — uname -s = '$(uname -s)'."
if grep -qiE 'microsoft|wsl' /proc/version 2>/dev/null; then info "host: WSL ($(uname -r))"; else info "host: native Linux ($(uname -r))"; fi
ensure_cmd curl curl
curl -fsS --max-time 20 -o /dev/null https://github.com || die "offline — cannot reach https://github.com."
pass "Linux/WSL host is online"

# baseline build prerequisites (git for clones; gcc+make for SQLite's configure; tcl for the amalgamation)
ensure_cmd git git
ensure_cmd gcc build-essential
ensure_cmd make build-essential

# ── Step 2 — dss-code-prime -> ~/src ─────────────────────────────────────────
step "2/8  Fetch dss-code-prime -> $SRC_DIR (branch: $DSS_BRANCH)"
clone_or_update "$DSS_REPO_URL" "$SRC_DIR" "$DSS_BRANCH"
pass "dss-code-prime ready"

# ── Step 3 — sqlite -> ~/src ─────────────────────────────────────────────────
step "3/8  Fetch sqlite/sqlite -> $SQLITE_DIR (default branch)"
clone_or_update "$SQLITE_REPO_URL" "$SQLITE_DIR" ""
pass "sqlite ready"

# ── Step 4 — amalgamate -> sqlite3.c + shell.c (autotools; needs tclsh) ───────
step "4/8  Amalgamate SQLite (autotools: make sqlite3.c shell.c)"
command -v tclsh >/dev/null 2>&1 || apt_install tcl tcl-dev     # mksqlite3c.tcl needs tclsh 8.6+
BLD="$SQLITE_DIR/bld-dss"
mkdir -p "$BLD"
# sqlite3.c is the amalgamated LIBRARY (no main); shell.c is the CLI DRIVER (its
# `main` opens a DB + runs SQL) — the runnable `sqlite3` binary is the two linked
# together. `make sqlite3.c shell.c` produces BOTH in the autotools build dir.
( cd "$BLD" && "$SQLITE_DIR/configure" >/dev/null && make -s sqlite3.c shell.c )
AMALGAMATION="$BLD/sqlite3.c"
SHELL_C="$BLD/shell.c"
[[ -f "$AMALGAMATION" ]] || die "amalgamation not produced at $AMALGAMATION"
[[ -f "$SHELL_C" ]] || die "CLI driver shell.c not produced at $SHELL_C (needed for the runnable 2-TU sqlite3 binary)"
pass "amalgamation: $AMALGAMATION ($(wc -l < "$AMALGAMATION") lines, $(du -h "$AMALGAMATION" | cut -f1))"
pass "CLI driver : $SHELL_C ($(wc -l < "$SHELL_C") lines)"

# ── Step 5 — build dss-code-prime (its default CMake-4 Release build) ────────
step "5/8  Build dss-code-prime (CMake ${MIN_CMAKE_MAJOR}+ Release)"
cmake_major() { cmake --version 2>/dev/null | sed -n '1s/.*version \([0-9]*\).*/\1/p'; }
ensure_cmake() {                # the project requires CMake >= 4.0 (root CMakeLists: cmake_minimum_required(VERSION 4.0))
  local major; major="$(cmake_major)"
  if [[ -n "$major" && "$major" -ge "$MIN_CMAKE_MAJOR" ]]; then info "cmake: $(cmake --version | sed -n '1p')"; return; fi
  warn "CMake ${MIN_CMAKE_MAJOR}+ required (found: ${major:-none}) — installing the official Kitware Linux binary"
  ensure_cmd tar tar
  local arch json ver url dest
  arch="$(uname -m)"            # x86_64 | aarch64 — matches Kitware's release asset names
  json="$(curl -fsSL https://api.github.com/repos/Kitware/CMake/releases/latest)"
  ver="$(printf '%s\n' "$json" | sed -n 's/.*"tag_name": *"v\([0-9.]*\)".*/\1/p')"; ver="${ver%%$'\n'*}"
  [[ -n "$ver" ]] || die "could not resolve the latest CMake version from the GitHub API."
  url="https://github.com/Kitware/CMake/releases/download/v${ver}/cmake-${ver}-linux-${arch}.tar.gz"
  dest="$HOME/.local/cmake-${ver}"
  info "downloading CMake ${ver} ($arch) from Kitware"
  rm -rf "$dest"; mkdir -p "$dest"          # clean re-extract (idempotent on re-run)
  curl -fsSL "$url" | tar -xz --strip-components=1 -C "$dest"
  export PATH="$dest/bin:$PATH"; hash -r
  major="$(cmake_major)"
  [[ -n "$major" && "$major" -ge "$MIN_CMAKE_MAJOR" ]] || \
    die "CMake ${MIN_CMAKE_MAJOR}+ install failed (got '${major:-none}'). Install a 4.x build from https://github.com/Kitware/CMake/releases and re-run."
  info "cmake: $(cmake --version | sed -n '1p') (Kitware $ver)"
}
ensure_cmake
( cd "$SRC_DIR" && cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j"$JOBS" )
DSS_BIN="$(find "$SRC_DIR/build" -type f -name dss-code-prime -perm -u+x -print -quit 2>/dev/null)"
[[ -n "$DSS_BIN" && -x "$DSS_BIN" ]] || die "dss-code-prime binary not found under $SRC_DIR/build after the build."
pass "dss-code-prime built: $DSS_BIN"

# ── Step 6 — compile sqlite3.c (+ shell.c for runnable targets) per target OS ─
step "6/8  Compile SQLite with dss-code-prime (windows + macos + linux + linux-arm64)"
mkdir -p "$OUT_DIR"
declare -A COMPILED                 # label -> 1 on success
COMPILE_FAILS=0                     # total compile misses (all targets — reporting)
RUNNABLE_COMPILE_FAILS=0           # compile misses on RUNNABLE targets only (fatal)
# Surface the COMPILER's own `--time` line ("dss-code-prime: compile time <dur>",
# captured in the log) as a "(compile time <dur>)" suffix — the timing is done by
# dss-code-prime; the harness only passes --time and echoes the result.
compile_time_suffix() {
  local t=""
  t="$(grep -oE 'compile time [^[:space:]]+' "$1" 2>/dev/null)" || true
  t="${t##*$'\n'}"                  # last match (no pipe → no SIGPIPE under pipefail)
  [[ -n "$t" ]] && printf '  (%s)' "$t" || true
}
for entry in "${TARGETS[@]}"; do
  label="${entry%%=*}"; spec="${entry#*=}"
  outd="$OUT_DIR/$label"; mkdir -p "$outd"
  log="$outd/compile.log"
  # A RUNNABLE target (in RUNNERS) compiles the real CLI: 2-TU sqlite3.c + shell.c
  # (sqlite3.c FIRST — the multi-TU merge requires the library CU ahead of the
  # driver). A COMPILE-ONLY target (windows/macos, absent from RUNNERS) compiles
  # the amalgamation ALONE (single-TU) — its deliverable surface, no runner here.
  declare -a units=("$AMALGAMATION")
  local_kind="amalgamation only (compile-only)"
  if [[ -n "${RUNNERS[$label]+set}" ]]; then
    units=("$AMALGAMATION" "$SHELL_C")   # sqlite3.c FIRST, then the CLI driver
    local_kind="sqlite3.c + shell.c (2-TU → runnable sqlite3)"
  fi
  info "[$label] $spec — $local_kind"
  # ⚠ GOTCHA (probe a6b65f8b): dss-code-prime returns EXIT 0 even on FATAL compile
  # errors. So success CANNOT be read from `$?` — it is read from the DIAGNOSTICS.
  # A clean compile emits NO `error[CODE]:` lines; any `error[` in the merged
  # stdout+stderr log means the frontier stopped this target. Run the compile
  # unconditionally (its rc is unreliable either way), then classify by grep.
  # `--time` asks the compiler to self-report its wall-clock (surfaced below).
  # c105 (the MSVC-profile flip): the pe target passes sqlite's OWN sanctioned
  # build knob — SQLITE_OMIT_SEH (DSS's _MSC_VER mask does not implement the
  # MSVC SEH __try/__except language extension; MinGW sqlite ships SEH-less the
  # same way). Build configuration, not compiler behavior: exactly the -D flag
  # an MSVC build script would pass. (SQLITE_DISABLE_INTRINSIC was DROPPED at
  # c113: DSS resolves <intrin.h> via the pe-gated shippedLibs/intrin.json
  # descriptor and lowers __umulh + _ReadWriteBarrier as builtin intrinsics.)
  declare -a defines=()
  if [[ "$spec" == *"pe64"* ]]; then
    defines+=(--define SQLITE_OMIT_SEH=1)
  fi
  "$DSS_BIN" --compile "${units[@]}" --language "$LANGUAGE" --target "$spec" --output "$outd" --time "${defines[@]}" >"$log" 2>&1 || true
  if grep -qE 'error\[' "$log"; then
    COMPILE_FAILS=$((COMPILE_FAILS + 1))
    # A RUNNABLE target's compile miss is a FATAL regression of the sqlite-RUN-green
    # goal; a COMPILE-ONLY target's miss is DATA at a known per-OS frontier (e.g.
    # windows os_win F001D, macOS layout gaps) — reported, but not fatal to the run.
    if [[ -n "${RUNNERS[$label]+set}" ]]; then
      RUNNABLE_COMPILE_FAILS=$((RUNNABLE_COMPILE_FAILS + 1))
    fi
    warn "[$label] compile FAILED$(compile_time_suffix "$log") — first diagnostics from $log:"
    { grep -m3 -E 'error\[' "$log" || head -3 "$log"; } 2>/dev/null | sed 's/^/      /'
  else
    COMPILED["$label"]=1
    pass "[$label] compiled -> $outd$(compile_time_suffix "$log")"
  fi
done

# ── Step 7 — test the runnable sqlite3 CLIs against EXPECTED output ───────────
step "7/8  Test SQLite (smoke: SELECT sum → 42, --version → 3.54.0)"
# The smoke unit is a REAL SQL round-trip through the dss-built `sqlite3` CLI:
#   CREATE a table, INSERT 1 and 41, SELECT sum(x) → must print 42 (an
#   in-memory DB exercises the parser, VDBE, b-tree, and aggregation), AND
#   `--version` must print SQLite's version string 3.54.0. Each runnable target
#   (x86_64-linux NATIVE + arm64-linux under qemu) runs BOTH against the EXPECTED
#   text — a wrong number, a crash (no output), or a version mismatch is a fail.
SQL_SMOKE='CREATE TABLE t(x);
INSERT INTO t VALUES(1);
INSERT INTO t VALUES(41);
SELECT sum(x) FROM t;'
EXPECT_SUM="42"
EXPECT_VERSION_PREFIX="3.54.0"    # the CLI prints "3.54.0 <date> <hash>"; match the version token
declare -A SMOKE                  # label -> PASS / FAIL:<reason> / skipped
SMOKE_FAILS=0
# Resolve a target's run wrapper: native (empty prefix) runs the ELF directly;
# arm64 runs under qemu-aarch64 with QEMU_LD_PREFIX → the aarch64 sysroot (so the
# dynamic loader + libc.so.6 resolve). The examples runner merges child stderr into
# stdout; we do the same (2>&1) so a crash's diagnostics ride the captured output.
run_target() {                    # run_target <label> <binary> <args...>  (stdin forwarded)
  local label="$1" bin="$2"; shift 2
  local pfx="${RUNNERS[$label]}"
  if [[ -z "$pfx" ]]; then
    "$bin" "$@" 2>&1
  else
    QEMU_LD_PREFIX="$QEMU_SYSROOT" "$pfx" "$bin" "$@" 2>&1
  fi
}
for entry in "${TARGETS[@]}"; do
  label="${entry%%=*}"
  # Only RUNNABLE targets are smoke-tested; compile-only ones (windows/macos) skip.
  [[ -n "${RUNNERS[$label]+set}" ]] || continue
  if [[ "${COMPILED[$label]:-0}" != "1" ]]; then
    SMOKE["$label"]="skipped (compile failed)"; warn "[$label] smoke skipped — step 6 did not compile it"
    continue
  fi
  bin="$(find "$OUT_DIR/$label" -maxdepth 1 -type f -perm -u+x -print -quit 2>/dev/null)"
  if [[ -z "$bin" ]]; then
    SMOKE["$label"]="FAIL:no runnable artifact emitted"; SMOKE_FAILS=$((SMOKE_FAILS + 1))
    warn "[$label] smoke FAIL — no runnable artifact under $OUT_DIR/$label"
    continue
  fi
  # (1) SQL round-trip: pipe the smoke SQL on stdin, expect exactly "42".
  sql_out="$(printf '%s\n' "$SQL_SMOKE" | run_target "$label" "$bin" 2>&1 || true)"
  # (2) version: `--version` prints the version string; match the 3.54.0 token.
  ver_out="$(run_target "$label" "$bin" --version </dev/null 2>&1 || true)"
  reasons=""
  if [[ "$(printf '%s' "$sql_out" | tr -d '[:space:]')" != "$EXPECT_SUM" ]]; then
    reasons="SELECT sum(x)!=$EXPECT_SUM (got: $(printf '%s' "$sql_out" | head -c 120 | tr '\n' ' '))"
  fi
  case "$ver_out" in
    "$EXPECT_VERSION_PREFIX"*) ;;   # ok — version line leads with 3.54.0
    *) reasons="${reasons:+$reasons; }--version!=$EXPECT_VERSION_PREFIX* (got: $(printf '%s' "$ver_out" | head -c 120 | tr '\n' ' '))" ;;
  esac
  if [[ -z "$reasons" ]]; then
    SMOKE["$label"]="PASS"
    pass "[$label] smoke PASS — SELECT sum → 42, --version → $EXPECT_VERSION_PREFIX"
  else
    SMOKE["$label"]="FAIL:$reasons"; SMOKE_FAILS=$((SMOKE_FAILS + 1))
    warn "[$label] smoke FAIL — $reasons"
  fi
done

# ── Step 8 — results ─────────────────────────────────────────────────────────
step "8/8  Results"
printf '   compiler : %s @ %s\n' "$DSS_BIN" "$(git -C "$SRC_DIR" rev-parse --short HEAD)"
printf '   sqlite   : %s @ %s\n' "$AMALGAMATION" "$(git -C "$SQLITE_DIR" rev-parse --short HEAD)"
printf '   outputs  : %s\n' "$OUT_DIR"
for entry in "${TARGETS[@]}"; do
  label="${entry%%=*}"
  compiled="no"; [[ "${COMPILED[$label]:-0}" == "1" ]] && compiled="yes"
  runnable="compile-only"; [[ -n "${RUNNERS[$label]+set}" ]] && runnable="runnable"
  smoke="${SMOKE[$label]:--}"       # "-" for compile-only targets (no smoke)
  if [[ "$compiled" == "yes" ]]; then
    printf '   %-11s : %scompiled%s (%s)  smoke: %s\n' "$label" "$C_GRN" "$C_RST" "$runnable" "$smoke"
  else
    printf '   %-11s : %sFAILED%s (%s)  see %s/%s/compile.log\n' "$label" "$C_RED" "$C_RST" "$runnable" "$OUT_DIR" "$label"
  fi
done

# Compile-only targets (windows/macos) sit at KNOWN per-OS frontiers (windows os_win
# F001D; macOS layout gaps) — a miss there is DATA, surfaced as a WARNING, never
# fatal (else the harness could never go green while those frontiers stand, masking
# the Linux RUN-green goal). Report them but do not exit on them.
compile_only_fails=$((COMPILE_FAILS - RUNNABLE_COMPILE_FAILS))
if [[ "$compile_only_fails" -gt 0 ]]; then
  printf '\n%s%d compile-only target(s) did not compile (known per-OS frontier — windows os_win / macOS).%s\n' "$C_YLW" "$compile_only_fails" "$C_RST"
  printf '%sReported, not fatal: these are tracked separately from the sqlite-RUN-green goal.%s\n' "$C_YLW" "$C_RST"
fi

# Exit non-zero if an EXPECTED step failed for the sqlite-RUN-green goal: a RUNNABLE
# target's compile miss (step 6) OR a runnable target's smoke miss (step 7). These
# are the real regressions — the two Linux legs must build the CLI AND run the SQL.
if [[ "$RUNNABLE_COMPILE_FAILS" -gt 0 ]]; then
  printf '\n%s%d RUNNABLE target(s) did not compile — the sqlite-RUN-green frontier is at step 6.%s\n' "$C_RED" "$RUNNABLE_COMPILE_FAILS" "$C_RST"
  printf '%sInspect the per-target compile.log diagnostics to pick the next compiler feature to land.%s\n' "$C_RED" "$C_RST"
  exit 1
fi
if [[ "$SMOKE_FAILS" -gt 0 ]]; then
  printf '\n%s%d runnable target(s) compiled but FAILED the SQL smoke — the RUN frontier.%s\n' "$C_RED" "$SMOKE_FAILS" "$C_RST"
  printf '%sInspect the smoke reasons above (a wrong sum / a crash / a version mismatch).%s\n' "$C_RED" "$C_RST"
  exit 1
fi
pass "every RUNNABLE target compiled (2-TU) + passed the SQL smoke — SQLite builds AND runs with dss-code-prime"
