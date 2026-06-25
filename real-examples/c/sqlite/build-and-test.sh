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
#   3. clone-or-update  sqlite/sqlite    into  ~/dss-build
#   4. amalgamate SQLite -> sqlite3.c    (autotools: `make sqlite3.c`, needs tclsh)
#   5. build dss-code-prime              (its default CMake-4 Release build)
#   6. compile sqlite3.c with dss-code-prime for windows + linux + macos
#                                        -> ~/dss-outputs/c/sqlite/<os>/
#   7. test the linux output with a SQLite smoke unit (when it runs)
#   8. summarise results + exit non-zero if any expected step failed
#
# DESIGN: every step is idempotent and FAIL-LOUD. The compiler is young, so step 6
# is the current FRONTIER — sqlite3.c (~9 MB of dense C) will surface unsupported
# constructs and the harness reports exactly where it stopped, per target. Re-run
# it as the compiler matures; the green frontier advances down the steps.
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
SQLITE_DIR="${SQLITE_DIR:-$HOME/dss-build/sqlite}"
OUT_DIR="${OUT_DIR:-$HOME/dss-outputs/c/sqlite}"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"
LANGUAGE="c-subset"
MIN_CMAKE_MAJOR=4

# os-label = <targetName>:<formatName>  (the 3 deliverable OSes; --target is repeatable)
declare -a TARGETS=(
  "windows=x86_64:pe64-x86_64-windows-exec"
  "linux=x86_64:elf64-x86_64-linux-exec"
  "macos=arm64:macho64-arm64-darwin-exec"
)

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

# ── Step 3 — sqlite -> ~/dss-build ───────────────────────────────────────────
step "3/8  Fetch sqlite/sqlite -> $SQLITE_DIR (default branch)"
clone_or_update "$SQLITE_REPO_URL" "$SQLITE_DIR" ""
pass "sqlite ready"

# ── Step 4 — amalgamate -> sqlite3.c (autotools; needs tclsh) ────────────────
step "4/8  Amalgamate SQLite (autotools: make sqlite3.c)"
command -v tclsh >/dev/null 2>&1 || apt_install tcl tcl-dev     # mksqlite3c.tcl needs tclsh 8.6+
BLD="$SQLITE_DIR/bld-dss"
mkdir -p "$BLD"
( cd "$BLD" && "$SQLITE_DIR/configure" >/dev/null && make -s sqlite3.c )
AMALGAMATION="$BLD/sqlite3.c"
[[ -f "$AMALGAMATION" ]] || die "amalgamation not produced at $AMALGAMATION"
pass "amalgamation: $AMALGAMATION ($(wc -l < "$AMALGAMATION") lines, $(du -h "$AMALGAMATION" | cut -f1))"

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

# ── Step 6 — compile sqlite3.c with dss-code-prime, per target OS ────────────
step "6/8  Compile SQLite with dss-code-prime (windows + linux + macos)"
mkdir -p "$OUT_DIR"
declare -A COMPILED                 # label -> 1 on success
COMPILE_FAILS=0
for entry in "${TARGETS[@]}"; do
  label="${entry%%=*}"; spec="${entry#*=}"
  outd="$OUT_DIR/$label"; mkdir -p "$outd"
  log="$outd/compile.log"
  info "[$label] $spec"
  # The compile is a PROBE: a non-zero exit is DATA (the frontier), not a script
  # abort. Running it as an `if` condition exempts it from `set -e` AND the ERR
  # trap, so the loop reports every target instead of dying on the first failure.
  if "$DSS_BIN" --compile "$AMALGAMATION" --language "$LANGUAGE" --target "$spec" --output "$outd" >"$log" 2>&1; then
    COMPILED["$label"]=1
    pass "[$label] compiled -> $outd"
  else
    rc=$?
    COMPILE_FAILS=$((COMPILE_FAILS + 1))
    warn "[$label] compile failed (exit $rc) — first diagnostics from $log:"
    { grep -m3 -iE 'error|unsupported|fatal| D-|S0[0-9]|H0[0-9]|K_|not (yet )?supported' "$log" || head -3 "$log"; } 2>/dev/null | sed 's/^/      /'
  fi
done

# ── Step 7 — test the linux output with a SQLite smoke unit ──────────────────
step "7/8  Test SQLite (linux output)"
SMOKE_RESULT="skipped"
if [[ "${COMPILED[linux]:-0}" == "1" ]]; then
  linux_bin="$(find "$OUT_DIR/linux" -maxdepth 1 -type f -perm -u+x -print -quit 2>/dev/null)"
  if [[ -n "$linux_bin" ]]; then
    # sqlite3.c is the LIBRARY (no main). A real unit needs the CLI driver (shell.c)
    # + SQLite's TCL test suite — added once the amalgamation compiles. The smoke
    # unit here is "the dss-built artifact runs": run it and capture the outcome.
    if "$linux_bin" </dev/null >/dev/null 2>&1; then
      SMOKE_RESULT="ran (exit 0)"; pass "linux artifact ran"
    else
      src=$?; SMOKE_RESULT="ran (exit $src)"
      warn "linux artifact exited $src (expected until shell.c + the SQLite test suite are wired)"
    fi
  else
    SMOKE_RESULT="no runnable artifact emitted"; warn "$SMOKE_RESULT"
  fi
else
  warn "linux compile did not succeed (step 6) — nothing to test yet"
  info "next frontier: make sqlite3.c compile, then add shell.c (the CLI) + SQLite's TCL units ('make test')"
fi

# ── Step 8 — results ─────────────────────────────────────────────────────────
step "8/8  Results"
printf '   compiler : %s @ %s\n' "$DSS_BIN" "$(git -C "$SRC_DIR" rev-parse --short HEAD)"
printf '   sqlite   : %s @ %s\n' "$AMALGAMATION" "$(git -C "$SQLITE_DIR" rev-parse --short HEAD)"
printf '   outputs  : %s\n' "$OUT_DIR"
for entry in "${TARGETS[@]}"; do
  label="${entry%%=*}"
  if [[ "${COMPILED[$label]:-0}" == "1" ]]; then printf '   %-8s : %scompiled%s\n' "$label" "$C_GRN" "$C_RST"
  else printf '   %-8s : %sFAILED%s (see %s/%s/compile.log)\n' "$label" "$C_RED" "$C_RST" "$OUT_DIR" "$label"; fi
done
printf '   smoke    : %s\n' "$SMOKE_RESULT"

if [[ "$COMPILE_FAILS" -gt 0 ]]; then
  printf '\n%s%d/%d target(s) did not compile — SQLite-readiness frontier is at step 6.%s\n' "$C_YLW" "$COMPILE_FAILS" "${#TARGETS[@]}" "$C_RST"
  printf '%sInspect the per-target compile.log diagnostics to pick the next compiler feature to land.%s\n' "$C_YLW" "$C_RST"
  exit 1
fi
pass "all targets compiled — SQLite builds with dss-code-prime"
