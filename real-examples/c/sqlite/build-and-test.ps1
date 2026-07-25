#!/usr/bin/env pwsh
# real-examples/c/sqlite/build-and-test.ps1
# ─────────────────────────────────────────────────────────────────────────────
# SQLite UNIT-CORPUS harness for DSS Code Prime — Windows pe64, full-source
# (no amalgamation). The pe64 mirror of build-and-test.sh.
#
# Clone the repo and run ONE command to prove DSS Code Prime builds SQLite from
# its REAL sources (no amalgamation) into the Tcl `testfixture.exe` and runs
# SQLite's own `.test` unit corpus on a native Windows pe64 binary.
#
# The pipeline mirrors the .sh, adapted to a Windows host + the single pe64 leg:
#
#   1. verify the host is Windows + WSL is available + online
#   2. use the dss-code-prime checkout AS-IS on its CURRENT branch (never
#      switched/pulled — a probe tests the working tree exactly as it is)
#   3+4. VIA WSL: clone-or-update sqlite/sqlite, configure, and derive the
#      FULL-SOURCE testfixture recipe from the canonical
#      `make -n testfixture USE_AMALGAMATION=0` (the ~185-TU list, the -D
#      defines, the sqlite -I dirs) exactly as the .sh does — then STAGE the
#      sqlite sources + the generated derived sources + the real tcl8.6/zlib
#      HEADERS onto a Windows path, and emit the recipe in Windows paths.
#      (SQLite's build is autotools + tclsh — inherently Unix; the sources are
#      portable C, so the pe64 compile reuses the SAME TU set.)
#   5. locate (or build) a Windows dss-code-prime.exe (Release preferred)
#   6. resolve the pe64 tcl + zlib LIBRARIES the fixture links against: for pe64
#      `--resolve-library` reads a DLL's EXPORT table (`.edata`) — so this points
#      at real DLLs (tcl86.dll + zlib1.dll), NOT an import .lib. git-for-Windows
#      ships both with the full public API; override with $env:TCL_DLL/$env:ZLIB_DLL.
#   7. generate a `.dss-project.json` (language c-subset / profile cli / target
#      x86_64:pe64-x86_64-windows-exec / artifactName testfixture / the 185 TUs
#      as absolute `sources` / the sqlite+tcl+zlib include dirs / the recipe
#      defines / the two DLLs as resolveLibraries) and build it:
#        dss-code-prime --project <manifest> --config=release --output <out>
#      → <out>/pe64-x86_64-windows-exec/testfixture.exe
#   8. run SQLite's `.test` UNIT CORPUS through the dss-built fixture
#      (DSS_TIER: veryquick[default] | quick | full | all), parse
#      "N errors out of M tests", classify failures against the documented
#      non-DSS confounds. GREEN = every failure is a known confound.
#   9. summarise + exit non-zero on any genuine failure / compile miss / crash.
#
# DESIGN: every step is idempotent and FAIL-LOUD. dss-code-prime exits 0 even on
# fatal compile errors, so step 7 reads success from the DIAGNOSTICS (no `error[`
# line) + the emitted binary, never $LASTEXITCODE.
#
# ─────────────────────────────────────────────────────────────────────────────
# ★★ pe64-testfixture — front-end + FFI closure RESOLVED (2026-07-25) ★★
#
# The pe64 full-source testfixture BUILDS + LINKS + RUNS the veryquick unit corpus
# via the pe64 target, DSS parsing the REAL tcl8.6 headers agnostically, the pe64
# `--resolve-library` path (tcl86.dll/zlib1.dll export tables), and the `--project`
# manifest. THREE Windows-only front-end gaps (each `_WIN32`/`SQLITE_OS_WIN`-gated,
# so the Linux/macOS arc never compiled them) were closed as real compiler cycles —
# documented below for the record:
#
#   B1  [config, trivial]  tcl.h / tclDecls.h — the legacy single-underscore
#       MSVC alias `_declspec` is not neutralized. Under DSS's pe profile
#       (_MSC_VER=1943, __GNUC__ undefined) tcl.h picks
#       `#define TCL_NORETURN _declspec(noreturn)` (tcl.h ~line 159); DSS
#       neutralizes `__declspec` (double underscore, c-subset.lang.json:67) but
#       NOT `_declspec`, so it leaks as raw tokens and derails the parser at the
#       first EXTERN declaration. FIX: add a `_declspec` pe predefine mirroring
#       `__declspec` in src/dss-config/sources/c-subset.lang.json:
#         { "name": "_declspec", "kind": "constant", "value": "",
#           "params": ["x"], "availableObjectFormats": ["pe"] }
#
#   B2  [parser, small]  src/test1.c:9346 — `extern LONG volatile sqlite3_os_type;`
#       (inside `#if SQLITE_OS_WIN`; LONG is windows.json's `i32 "long"`). DSS's
#       C parser accepts a type-qualifier AFTER a builtin type-specifier
#       (`int volatile x`) and BEFORE a typedef-name (`volatile LONG x`), but NOT
#       a trailing qualifier on a typedef-name (`LONG volatile x`). Minimal repro
#       (target-agnostic, reproduces on elf64): `typedef long LONG; extern LONG
#       volatile d;`  FIX (grammar): allow a type-qualifier to follow a
#       typedef-name type-specifier (C 6.7.1 — specifiers/qualifiers, any order).
#
#   B3  [FFI surface, multi-cycle]  With B1+B2 cleared the 185-TU compile advances
#       to a body of ~27 unresolved Windows/CRT/math symbols (fail-loud S0001,
#       + cascading S000D member-access errors) referenced by the Windows-gated
#       and Unix-oriented test sources — the pe64 analogue of the Linux arc's
#       per-cycle OS-symbol additions (isnan/pread64/…). The worklist:
#         · kernel32 (test1.c Win section): CreateEvent OpenEvent SetEvent
#           LockFile UnlockFile + EVENT_MODIFY_STATE
#         · msvcrt CRT (test1.c/test_quota.c): _set_abort_behavior _CALL_REPORTFAULT
#           _commit _chsize_s _stati64
#         · CRT low-level I/O by POSIX name (test_fs.c): open close read fstat —
#           test_fs.c:72 gates <unistd.h> on `!_WIN32 || __MSVCRT__`; DSS defines
#           _WIN32 but not __MSVCRT__, so they need <io.h>/<sys/stat.h> pe mappings
#         · math (func.c): log2 trunc acosh asinh atanh   · time (date.c): localtime_r
#       These need new/extended pe shipped-lib descriptors (windows.json / io.json
#       / math.json / time.json …). NOTE a recipe-provenance nuance: this recipe
#       is derived from a LINUX `make -n testfixture`, so some surface reflects
#       Unix-oriented test helpers; a native Windows `nmake -f Makefile.msc`
#       testfixture recipe would compile a somewhat different TU set. The core
#       Win32 (events/locking) + msvcrt surface is genuinely needed either way.
#
# ★ B3 was closed by an AGNOSTIC recipe auto-config (gen-pe64-manifest.py drops the
# Linux host-feature probes `HAVE_*`/`config.h` + adds `SQLITE_OS_WIN`, so SQLite
# self-configures its Windows build → the Unix symbols evaporate), the C99 math +
# `strftime` sourced from ucrtbase.dll (legacy msvcrt is C89), and the genuinely-
# Windows kernel32/msvcrt symbols added to the pe shipped-lib descriptors.
#
# ★ KNOWN remaining pe64 issue (anchored, NOT a DSS bug): `fpconv1-2.0` — legacy
#   msvcrt.dll's `sprintf` renders `%e` to ~17 significant digits while sqlite's own
#   dtoa is full-precision, and fpconv1-2.0 compares the two (a GCC build linking
#   msvcrt.dll fails IDENTICALLY; DSS's codegen was disasm-verified correct). It is
#   resolved by the pe->UCRT migration's Phase 3 (printf-family -> ucrtbase,
#   full-precision dtoa) — D-FFI-PE-CRT-UCRT-MIGRATION.
# ─────────────────────────────────────────────────────────────────────────────
#
# Overridable via env: SQLITE_WSL_DIR  DSS_JOBS  DSS_BIN  SKIP_DSS_BUILD
#                      DSS_TIER  DSS_CONFIG  DSS_TEST_FILE  DSS_CONFOUNDS
#                      TCL_DLL  ZLIB_DLL  TCL_LIBRARY

$ErrorActionPreference = 'Stop'

# ── logging / fail-loud (mirrors the .sh's step/info/pass/warn/die) ──────────
function Step($m) { Write-Host "`n== $m ==" -ForegroundColor Blue }
function Info($m) { Write-Host "   $m" }
function Pass($m) { Write-Host " [OK] $m" -ForegroundColor Green }
function Warn($m) { Write-Host " [!]  $m" -ForegroundColor Yellow }
function Die($m)  { Write-Host " [X] ERROR: $m" -ForegroundColor Red; exit 1 }

# ── config (override via environment) ────────────────────────────────────────
# This script lives at real-examples/c/sqlite/, so repo root = ../../../.
$RepoRoot     = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$Jobs         = if ($env:DSS_JOBS) { $env:DSS_JOBS } else { [Environment]::ProcessorCount }
$SqliteWslDir = if ($env:SQLITE_WSL_DIR) { $env:SQLITE_WSL_DIR } else { '$HOME/src/sqlite' }
$Work         = Join-Path $RepoRoot 'build\real-examples\c\sqlite\windows'
$Spec         = 'x86_64:pe64-x86_64-windows-exec'
$Fmt          = ($Spec -split ':')[1]
# DSS_TIER: which unit-corpus tier — veryquick (default) | quick | full | all.
$Tier         = if ($env:DSS_TIER) { $env:DSS_TIER } else { 'veryquick' }
# DSS_CONFIG: RELEASE by default (load-bearing — the corpus must exercise the
# full optimizer to catch release-only miscompiles; a debug fixture would run
# green while masking a release bug, and far slower).
$Config       = if ($env:DSS_CONFIG) { $env:DSS_CONFIG } else { 'release' }
# DSS_CONFOUNDS: space-separated .NET-regex patterns for KNOWN non-DSS unit
# failures (a failing test matching any is not counted against green). Same set
# as the .sh: WAL set-lock wall-clock timing, a zipfile error-text env diff,
# the recover-fault OOM-oracle class.
$Confounds    = if ($env:DSS_CONFOUNDS) { $env:DSS_CONFOUNDS -split '\s+' } `
                else { @('^walsetlk-', '^walsetlk\.', '^busy2-', '^zipfile-25\.0$', '^recoverfault') }
# pe64 resolve-library targets: DLLs with export tables (NOT import .libs).
# git-for-Windows ships a full Tcl 8.6 + zlib runtime; override if you have a
# dedicated Tcl dev kit (Magicsplat/ActiveTcl).
$GitMingwBin  = 'C:\Program Files\Git\mingw64\bin'
$TclDll       = if ($env:TCL_DLL)  { $env:TCL_DLL }  else { Join-Path $GitMingwBin 'tcl86.dll' }
$ZlibDll      = if ($env:ZLIB_DLL) { $env:ZLIB_DLL } else { Join-Path $GitMingwBin 'zlib1.dll' }
# Tcl runtime script library (init.tcl …) — testfixture needs it at RUN time.
$TclLibrary   = if ($env:TCL_LIBRARY) { $env:TCL_LIBRARY } else { 'C:\Program Files\Git\mingw64\lib\tcl8.6' }

$Stage        = Join-Path $Work 'stage'          # the staged sqlite tree + headers
$OutDir       = Join-Path $Work 'out'
$Manifest     = Join-Path $Work 'testfixture.pe64.dss-project.json'
$GenPy        = Join-Path $PSScriptRoot 'gen-pe64-manifest.py'

# Convert C:\path → /mnt/c/path (a backslash Windows path through `wsl wslpath`
# gets its backslashes stripped — do it manually, as the .sh companion does).
function ToWslPath($p) {
  $full = (Resolve-Path -LiteralPath $p).Path
  '/mnt/' + $full.Substring(0,1).ToLowerInvariant() + ($full.Substring(2) -replace '\\','/')
}

# ── Step 1 — host is Windows + WSL + online ──────────────────────────────────
Step '1/9  Host check (Windows + WSL, online)'
if (-not $IsWindows -and $PSVersionTable.PSVersion.Major -ge 6) {
  Die "this harness targets Windows (the pe64 leg); use build-and-test.sh on Linux/WSL/macOS."
}
$wsl = Get-Command wsl.exe -ErrorAction SilentlyContinue
if (-not $wsl) { Die "wsl.exe not found — WSL is required to derive the sqlite recipe (autotools + tclsh). Install WSL + a Debian/Ubuntu distro." }
$probe = & wsl.exe bash -c 'echo wsl-ok' 2>&1
if ($LASTEXITCODE -ne 0 -or "$probe".Trim() -ne 'wsl-ok') { Die "WSL is present but a bash round-trip failed (got: '$probe')." }
$python3 = Get-Command python3 -ErrorAction SilentlyContinue
if (-not $python3) { $python3 = Get-Command python -ErrorAction SilentlyContinue }
if (-not $python3) { Die "python3 not found on PATH — needed to generate the .dss-project.json manifest." }
try {
  $null = Invoke-WebRequest -Uri 'https://github.com' -Method Head -TimeoutSec 20 -UseBasicParsing
} catch { Die "offline — cannot reach https://github.com ($($_.Exception.Message))." }
Info "host: Windows ($([Environment]::OSVersion.Version))   leg: $Spec   tier: $Tier   config: $Config"
Pass "Windows host + WSL online"

# ── Step 2 — dss-code-prime (current checkout, untouched) ────────────────────
Step "2/9  Use dss-code-prime at $RepoRoot (current checkout, untouched)"
$dssHead = (& git -C $RepoRoot rev-parse --short HEAD 2>$null)
$dssBranch = (& git -C $RepoRoot rev-parse --abbrev-ref HEAD 2>$null)
Info "  at $dssHead on $dssBranch"
Pass "dss-code-prime checkout ready"

# ── Step 3+4 — derive the full-source recipe + stage to Windows (VIA WSL) ────
Step '3+4/9  Derive full-source testfixture recipe + stage sources/headers (WSL)'
New-Item -ItemType Directory -Force -Path $Work | Out-Null
if (Test-Path $Stage) { Remove-Item -Recurse -Force $Stage }
New-Item -ItemType Directory -Force -Path $Stage | Out-Null
$StageWsl = ToWslPath $Stage
# The WSL derivation reproduces the .sh's Step-4 recipe logic (make -n
# testfixture USE_AMALGAMATION=0 → TUs + defines + -I dirs, with the
# libsqlite3.a core recovered via `ar t`), then STAGES the sqlite sources +
# generated derived sources + real tcl8.6/zlib headers onto the Windows-visible
# $Stage dir and writes the recipe as three files IN WINDOWS PATHS:
#   $Stage/tus.txt  $Stage/includes.txt  $Stage/defines.txt
# (via `wslpath -m`, the forward-slash Windows form the manifest wants).
$deriveScript = @'
set -Eeuo pipefail
for t in git gcc make ar tclsh; do
  command -v "$t" >/dev/null 2>&1 || {
    echo "MISSING tool: $t — install the recipe toolchain, e.g.:" >&2
    echo "    sudo apt-get install -y git build-essential tcl tcl-dev zlib1g-dev" >&2
    exit 1; }
done
DIR="__SQLITE_WSL_DIR__"
STAGE="__STAGE_WSL__"
# clone-or-update sqlite (external dependency — DOES pull)
if [ -d "$DIR/.git" ]; then
  git -C "$DIR" fetch --all --prune --quiet || true
  git -C "$DIR" pull --rebase --quiet 2>/dev/null || true
elif [ ! -e "$DIR/configure" ]; then
  mkdir -p "$(dirname "$DIR")"
  git clone --quiet https://github.com/sqlite/sqlite.git "$DIR"
fi
[ -x "$DIR/configure" ] || { echo "no ./configure in $DIR — not a SQLite checkout" >&2; exit 1; }
BLD="$DIR/bld-dss"; mkdir -p "$BLD"
( cd "$BLD" && "$DIR/configure" >/dev/null 2>&1 )
# best-effort reference build → generates every derived .c (parse.c/opcodes.c/
# ctime.c/tclsqlite-ex.c/fts5.c…) + libsqlite3.a (a Tcl-link miss is tolerated —
# we only harvest byproducts + the recipe, not gcc's binary).
( cd "$BLD" && make -s testfixture USE_AMALGAMATION=0 -j"$(nproc 2>/dev/null || echo 4)" >/dev/null 2>&1 ) || true
rm -f "$BLD/testfixture"
RECIPE="$STAGE/testfixture-recipe.txt"
( cd "$BLD" && make -n testfixture USE_AMALGAMATION=0 ) > "$RECIPE" 2>&1 || true
BLOB="$(sed ':a;N;$!ba;s/\\\n/ /g' "$RECIPE" | tr '\t' ' ')"
# defines (-DNAME[=VALUE]) — strip make's literal "" and -D
mapfile -t RECIPE_DEFS < <(printf '%s\n' "$BLOB" | grep -oE '\-D[A-Za-z0-9_]+(=[^ ]*)?' | sed 's/^-D//; s/"//g' | sort -u)
# sqlite -I dirs (drop the bare `.`; it is $BLD, added explicitly below)
mapfile -t SQLITE_INCS < <(printf '%s\n' "$BLOB" | grep -oE '\-I ?[^ ]+' | sed 's/^-I *//' | grep -v '^\.$' | sort -u)
# TU set (1): every .c the recipe names (absolute, or relative → $BLD)
declare -A TU=()
while IFS= read -r c; do
  [ -n "$c" ] || continue
  if   [ -f "$c" ];      then TU["$c"]=1
  elif [ -f "$BLD/$c" ]; then TU["$BLD/$c"]=1
  fi
done < <(printf '%s\n' "$BLOB" | tr ' ' '\n' | grep -E '\.c$' | sort -u)
# TU set (2): the core sources inside libsqlite3.a (DSS can't consume the .a)
AR="$BLD/libsqlite3.a"; [ -f "$BLD/.libs/libsqlite3.a" ] && AR="$BLD/.libs/libsqlite3.a"
if [ -f "$AR" ]; then
  while read -r obj; do
    base="${obj%.o}"
    hit="$(find "$DIR/src" "$DIR/ext" "$BLD" -name "$base.c" 2>/dev/null | grep -v '/tsrc/' | head -1)"
    [ -z "$hit" ] && hit="$(find "$DIR/src" "$DIR/ext" "$BLD" -name "$base.c" 2>/dev/null | head -1)"
    [ -n "$hit" ] && TU["$hit"]=1
  done < <(ar t "$AR" 2>/dev/null | grep '\.o$')
fi
# de-alias generated .c that exist under both bld/ and bld/tsrc/ (dedup by basename)
declare -A SEEN=(); declare -a TUS=()
for f in $(printf '%s\n' "${!TU[@]}" | sort); do
  b="$(basename "$f")"; [ -n "${SEEN[$b]:-}" ] && continue; SEEN[$b]="$f"; TUS+=("$f")
done
[ ${#TUS[@]} -ge 150 ]        || { echo "recipe yielded only ${#TUS[@]} TUs (<150) — see $RECIPE" >&2; exit 1; }
[ ${#RECIPE_DEFS[@]} -ge 18 ] || { echo "recipe yielded only ${#RECIPE_DEFS[@]} defines (<18)" >&2; exit 1; }

# ── stage: sqlite sources + generated derived sources + tcl8.6/zlib headers ──
mkdir -p "$STAGE/sqlite/bld" "$STAGE/tclinc" "$STAGE/zinc" "$STAGE/test"
cp -r "$DIR/src" "$STAGE/sqlite/src"
cp -r "$DIR/ext" "$STAGE/sqlite/ext"
cp "$BLD"/*.c "$STAGE/sqlite/bld/" 2>/dev/null || true
cp "$BLD"/*.h "$STAGE/sqlite/bld/" 2>/dev/null || true
# the .test corpus + its tcl harness (tester.tcl …) — testfixture.exe runs these
cp -r "$DIR/test/." "$STAGE/test/" 2>/dev/null || true
# real tcl8.6 headers (parsed agnostically — NO descriptor, D-FFI-SHIPPED-LIBS-OS-ONLY)
TCLH="$( . "$(find /usr/lib -name tclConfig.sh 2>/dev/null | head -1)" >/dev/null 2>&1; printf '%s' "${TCL_INCLUDE_SPEC#-I}" )"
[ -f "$TCLH/tcl.h" ] || TCLH="$(dirname "$(find /usr/include -name tcl.h -path '*tcl8*' 2>/dev/null | head -1)")"
[ -f "$TCLH/tcl.h" ] || { echo "tcl.h not found in WSL — apt-get install tcl-dev" >&2; exit 1; }
cp "$TCLH"/*.h "$STAGE/tclinc/"
# zlib headers (staged privately so they never shadow anything on -I)
ZH="$(find /usr/include -maxdepth 3 -name zlib.h 2>/dev/null | head -1)"
[ -f "$ZH" ] || { echo "zlib.h not found in WSL — apt-get install zlib1g-dev" >&2; exit 1; }
cp "$ZH" "$STAGE/zinc/"; cp "$(dirname "$ZH")/zconf.h" "$STAGE/zinc/" 2>/dev/null || true
# un-configure the Linux ./configure edit for the pe TARGET: the staged zconf.h has
# `#if 1 → #define Z_HAVE_UNISTD_H` (a ./configure host-probe result ~line 444) which
# forces `#include <unistd.h>` — absent on windows (F001D). Flip ONLY that guard to
# `#if 0` in the STAGED copy (target-appropriate header staging — the zconf.h analog of
# gen-pe64-manifest.py's HAVE_/Z_HAVE_ host-probe drop). The sibling Z_HAVE_STDARG_H
# `#if 1` block is deliberately left intact (<stdarg.h> exists on pe).
perl -0777 -pi -e 's{#if 1(\s*/\* was set to #if 1 by \./configure \*/\s*\n#  define Z_HAVE_UNISTD_H)}{#if 0$1}' "$STAGE/zinc/zconf.h"

# remap a WSL path under $DIR or the header dirs → its STAGED location, then → a
# forward-slash Windows path (wslpath -m).
win() { wslpath -m "$1"; }
remap() {
  local p="$1"
  case "$p" in
    "$DIR"/src/*)  echo "$STAGE/sqlite/src/${p#"$DIR"/src/}" ;;
    "$DIR"/ext/*)  echo "$STAGE/sqlite/ext/${p#"$DIR"/ext/}" ;;
    "$BLD")        echo "$STAGE/sqlite/bld" ;;                    # the -I build dir itself → staged bld
    "$BLD"/*)      echo "$STAGE/sqlite/bld/$(basename "$p")" ;;   # a generated TU under it
    "$DIR"/*)      echo "$STAGE/sqlite/${p#"$DIR"/}" ;;
    "$TCLH")       echo "$STAGE/tclinc" ;;
    "$TCLH"/*)     echo "$STAGE/tclinc/${p#"$TCLH"/}" ;;
    *)             echo "$p" ;;   # already staged / third-party
  esac
}
# tus.txt (Windows paths)
: > "$STAGE/tus.txt"
for f in "${TUS[@]}"; do s="$(remap "$f")"; [ -f "$s" ] && win "$s" >> "$STAGE/tus.txt"; done
# includes.txt: the sqlite -I dirs (remapped) + $BLD + tcl + zlib
: > "$STAGE/includes.txt"
{ echo "$BLD"; for d in "${SQLITE_INCS[@]}"; do echo "$d"; done; } | while read -r d; do
  [ -n "$d" ] || continue; s="$(remap "$d")"; [ -d "$s" ] && win "$s" >> "$STAGE/includes.txt"
done
win "$STAGE/tclinc" >> "$STAGE/includes.txt"
win "$STAGE/zinc"   >> "$STAGE/includes.txt"
# defines.txt
printf '%s\n' "${RECIPE_DEFS[@]}" > "$STAGE/defines.txt"
# the staged test dir (Windows path) for the corpus run
win "$STAGE/test" > "$STAGE/testdir.win.txt"

echo "RECIPE-TUS=$(wc -l < "$STAGE/tus.txt")"
echo "RECIPE-DEFS=$(wc -l < "$STAGE/defines.txt")"
echo "RECIPE-INCS=$(wc -l < "$STAGE/includes.txt")"
echo "SQLITE-HEAD=$(git -C "$DIR" rev-parse --short HEAD 2>/dev/null)"
'@
$deriveScript = $deriveScript.Replace('__SQLITE_WSL_DIR__', $SqliteWslDir).Replace('__STAGE_WSL__', $StageWsl) -replace "`r`n", "`n"
$tmpSh = Join-Path $Work 'derive.sh'
Set-Content -LiteralPath $tmpSh -Value $deriveScript -NoNewline -Encoding ascii
$deriveOut = & wsl.exe bash -l (ToWslPath $tmpSh) 2>&1
if ($LASTEXITCODE -ne 0) { Die "WSL recipe derivation failed:`n$($deriveOut -join "`n")" }
function Marker($k) { ($deriveOut | Select-String -Pattern "^$k=(.+)$" | Select-Object -Last 1).Matches[0].Groups[1].Value }
$nTus = Marker 'RECIPE-TUS'; $nDefs = Marker 'RECIPE-DEFS'; $nIncs = Marker 'RECIPE-INCS'
$sqliteHead = Marker 'SQLITE-HEAD'
if (-not (Test-Path "$Stage\tus.txt")) { Die "recipe derivation produced no tus.txt:`n$($deriveOut -join "`n")" }
Pass "recipe: $nTus TUs, $nDefs defines, $nIncs include dirs (sqlite @ $sqliteHead) staged under $Stage"

# ── Step 5 — locate (or build) a Windows dss-code-prime.exe ──────────────────
# Picks the NEWEST existing binary (build-rel Release / build MSVC / build-dbg
# Ninja-Debug) — newest-wins deliberately avoids a STALE Release binary that
# predates a project-config field (the exact trap that fails loud below). With
# no binary present (a fresh clone) it configures + builds Release into build-rel.
Step '5/9  Locate / build dss-code-prime (newest existing, else build Release)'
function Find-Dss {
  $cands = @(
    (Join-Path $RepoRoot 'build-rel\bin\dss\dss-code-prime.exe'),
    (Join-Path $RepoRoot 'build\bin\dss\Release\dss-code-prime.exe'),
    (Join-Path $RepoRoot 'build\bin\dss\dss-code-prime.exe'),
    (Join-Path $RepoRoot 'build-dbg\bin\dss\dss-code-prime.exe')
  ) | Where-Object { Test-Path $_ } | Sort-Object { (Get-Item $_).LastWriteTime } -Descending
  return ($cands | Select-Object -First 1)
}
if ($env:DSS_BIN -and (Test-Path $env:DSS_BIN)) {
  $DssBin = (Resolve-Path $env:DSS_BIN).Path
  Info "using `$env:DSS_BIN — $DssBin"
} elseif ($env:SKIP_DSS_BUILD -eq '1') {
  $DssBin = Find-Dss
  if (-not $DssBin) { Die "SKIP_DSS_BUILD=1 but no dss-code-prime.exe found under build-rel/build/build-dbg." }
  Info "SKIP_DSS_BUILD=1 — reusing $DssBin"
} else {
  $DssBin = Find-Dss
  if (-not $DssBin) {
    Info "no existing binary — configuring + building Release (build-rel)"
    $bdir = Join-Path $RepoRoot 'build-rel'
    if (-not (Test-Path $bdir)) { & cmake -S $RepoRoot -B $bdir -DCMAKE_BUILD_TYPE=Release; if ($LASTEXITCODE -ne 0) { Die "cmake configure failed" } }
    & cmake --build $bdir --config Release --target dss-code-prime -j $Jobs
    if ($LASTEXITCODE -ne 0) { Die "dss-code-prime build failed" }
    $DssBin = Find-Dss
  }
}
if (-not $DssBin -or -not (Test-Path $DssBin)) { Die "dss-code-prime.exe not found." }
$dssAge = (Get-Item $DssBin).LastWriteTime
Info "compiler: $DssBin  (built $dssAge)"
Warn "if the build below fails with a stale-manifest-field error (e.g. unknown 'artifactName'), this binary predates the project-config extensions — rebuild it (delete build-rel or set SKIP_DSS_BUILD off)."
Pass "dss-code-prime located"

# ── Step 6 — pe64 tcl + zlib libraries (resolve-library = DLL export tables) ──
Step '6/9  Resolve-library DLLs (tcl + zlib export surfaces) for pe64'
if (-not (Test-Path $TclDll))  { Die "Tcl DLL not found: $TclDll — install git-for-Windows (ships tcl86.dll) or a Tcl dev kit, or set \$env:TCL_DLL." }
if (-not (Test-Path $ZlibDll)) { Die "zlib DLL not found: $ZlibDll — set \$env:ZLIB_DLL to a zlib1.dll." }
Info "tcl : $TclDll"
Info "zlib: $ZlibDll"
if (-not (Test-Path $TclLibrary)) { Warn "Tcl script library not at $TclLibrary — the corpus RUN needs it (set \$env:TCL_LIBRARY)." }
Pass "pe64 resolve-library DLLs ready"

# ── Step 7 — generate the manifest + build the testfixture (dss --project) ───
Step "7/9  Build the full-source testfixture (dss-code-prime --project, $Config)"
$extraDefineArgs = @()
$genArgs = @(
  $GenPy,
  '--tus',      (Join-Path $Stage 'tus.txt'),
  '--includes', (Join-Path $Stage 'includes.txt'),
  '--defines',  (Join-Path $Stage 'defines.txt'),
  '--target',   $Spec,
  '--resolve-library', $TclDll,
  '--resolve-library', $ZlibDll,
  '--artifact-name', 'testfixture'
) + $extraDefineArgs + @('--output', $Manifest)
$genOut = & $python3.Source @genArgs 2>&1
if ($LASTEXITCODE -ne 0) { Die "manifest generation failed:`n$($genOut -join "`n")" }
Info "manifest -> $Manifest ($genOut)"

if (Test-Path $OutDir) { Remove-Item -Recurse -Force $OutDir }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$log = Join-Path $Work 'compile.log'
# dss-code-prime returns exit 0 even on FATAL errors → judge from `error[` + the binary.
& $DssBin --project $Manifest --config="$Config" --output $OutDir --time *>&1 |
  Tee-Object -FilePath $log | Out-Null
$errCount = (Select-String -Path $log -Pattern 'error\[' -AllMatches).Count
$fixture  = Join-Path $OutDir "$Fmt\testfixture.exe"
$ctime = (Get-Content $log | Select-String -Pattern 'compile time (\S+)' | Select-Object -Last 1)
$ctimeSuffix = if ($ctime) { "  ($($ctime.Matches[0].Value))" } else { '' }
if ($errCount -gt 0 -or -not (Test-Path $fixture)) {
  Get-Content $log | Select-String -Pattern 'error\[' | Select-Object -First 5 | ForEach-Object { Info "      $($_.Line)" }
  Die "pe64 build FAILED$ctimeSuffix — $errCount error[...] diagnostic(s); no testfixture.exe. See $log"
}
Pass "testfixture -> $fixture$ctimeSuffix"

# ── Step 8 — run the .test UNIT CORPUS through the fixture ────────────────────
Step "8/9  Run SQLite unit corpus ($Tier.test) + classify failures"
$StagedTestDir = (Get-Content -Raw (Join-Path $Stage 'testdir.win.txt')).Trim()
$TestFile = if ($env:DSS_TEST_FILE) { $env:DSS_TEST_FILE } else { Join-Path $StagedTestDir "$Tier.test" }
if (-not (Test-Path $TestFile)) { Die "test file not found: $TestFile (tier '$Tier')." }
# tcl86.dll + zlib1.dll must be loadable at runtime (put them on PATH); TCL_LIBRARY
# points the Tcl runtime at its script library.
$runEnvPath = (Split-Path $TclDll) + ';' + (Split-Path $ZlibDll) + ';' + $env:PATH
$rundir = Join-Path $Work 'run'; if (Test-Path $rundir) { Remove-Item -Recurse -Force $rundir }
New-Item -ItemType Directory -Force -Path $rundir | Out-Null
$runlog = Join-Path $Work 'corpus.log'
Info "[pe64] running $Tier.test via $([System.IO.Path]::GetFileName($fixture)) …"
$oldPath = $env:PATH; $oldTclLib = $env:TCL_LIBRARY
Push-Location $rundir
try {
  $env:PATH = $runEnvPath; if (Test-Path $TclLibrary) { $env:TCL_LIBRARY = $TclLibrary }
  & $fixture $TestFile *>&1 | Tee-Object -FilePath $runlog | Out-Null
} finally { $env:PATH = $oldPath; $env:TCL_LIBRARY = $oldTclLib; Pop-Location }

$summary  = (Get-Content $runlog | Select-String -Pattern '(\d+) errors? out of (\d+) tests' | Select-Object -Last 1)
# The testfixture reports failures two ways: the canonical "Failures on these
# tests: …" summary AND inline per-failure markers (`! <name> expected:` /
# `! <name> got:`) — many .test files emit ONLY the latter (mirrors the .sh).
# Union both sources + dedup so a run that prints only the markers still classifies.
$faillist = (Get-Content $runlog | Select-String -Pattern '^Failures on these tests:\s*(.+)$' | Select-Object -Last 1)
$failNames = @()
if ($faillist) { $failNames += ($faillist.Matches[0].Groups[1].Value -split '\s+' | Where-Object { $_ }) }
$failNames += (Get-Content $runlog |
  Select-String -Pattern '^! ([A-Za-z0-9_.:-]+) (expected|got):' |
  ForEach-Object { $_.Matches[0].Groups[1].Value })
$failNames = @($failNames | Select-Object -Unique)
$unitVerdict = ''; $unitFail = $false
if (-not $summary) {
  $unitVerdict = "FAIL: fixture did not complete the suite (crash?) — see $runlog"; $unitFail = $true
  Warn "[pe64] corpus FAIL — no summary line (fixture crashed mid-suite); tail:"
  Get-Content $runlog -Tail 6 | ForEach-Object { Info "      $_" }
} else {
  $nerr = [int]$summary.Matches[0].Groups[1].Value
  $fails = $failNames
  $real = @(); $confound = @()
  foreach ($t in $fails) {
    $isc = $false
    foreach ($p in $Confounds) { if ($t -match $p) { $isc = $true; break } }
    if ($isc) { $confound += $t } else { $real += $t }
  }
  if ($nerr -gt 0 -and $fails.Count -eq 0) {
    $unitVerdict = "FAIL: $nerr error(s) but no failure markers ('Failures on these tests:' / '! <name>') to classify — see $runlog"; $unitFail = $true
    Warn "[pe64] corpus FAIL — $($summary.Matches[0].Value) (unclassifiable — no failure markers)"
  } elseif ($real.Count -eq 0) {
    $unitVerdict = "PASS ($($summary.Matches[0].Value)$(if ($confound.Count) { "; $($confound.Count) known confound(s): $($confound -join ' ')" }))"
    Pass "[pe64] corpus GREEN — $($summary.Matches[0].Value)$(if ($confound.Count) { "; all $($confound.Count) failure(s) are known non-DSS confounds: $($confound -join ' ')" })"
  } else {
    $unitVerdict = "FAIL: $($real.Count) genuine unit failure(s): $($real -join ' ')"; $unitFail = $true
    Warn "[pe64] corpus FAIL — $($summary.Matches[0].Value); $($real.Count) GENUINE DSS failure(s): $($real -join ' ')"
    if ($confound.Count) { Info "      (+$($confound.Count) known confound(s) ignored: $($confound -join ' '))" }
  }
}

# ── Step 9 — results ─────────────────────────────────────────────────────────
Step '9/9  Results'
Info "compiler : $DssBin @ $dssHead"
Info "sqlite   : $SqliteWslDir @ $sqliteHead   (staged: $Stage)"
Info "recipe   : $nTus TUs, $nDefs defines"
Info "tier     : $Tier.test   outputs: $Work"
Info "pe64 ($Spec): compiled   units: $unitVerdict"
if ($unitFail) { Write-Host "`n [X] pe64 leg had genuine unit failures (non-confound) — the corpus is not green." -ForegroundColor Red; exit 1 }
Pass "pe64 leg compiled the full-source testfixture + ran the $Tier unit corpus GREEN"
exit 0
