#!/usr/bin/env pwsh
# real-examples/c/sqlite/build-and-test.ps1
# ─────────────────────────────────────────────────────────────────────────────
# SQLite UNIT-CORPUS harness for DSS Code Prime — full-source (no amalgamation),
# TARGET-KEYED, MULTI-LEG. The PowerShell twin of build-and-test.sh.
#
# Clone the repo and run ONE command to prove DSS Code Prime builds SQLite from
# its REAL sources (no amalgamation) into the Tcl `testfixture` and runs SQLite's
# own `.test` unit corpus on whichever legs this host can execute.
#
# ★★ THE LEG SET IS A PROPERTY OF THE HARNESS, NOT OF THIS MACHINE ★★
# (D-HARNESS-CROSS-HOST-ANY-TARGET item 2; user hard requirement 2026-07-25:
# "build ANY target inside ANY host — this MUST work".)
# Until TF-C114 this driver opened with `if (-not $IsWindows) { Die ... }` over a
# single hardcoded `$Spec = 'x86_64:pe64-x86_64-windows-exec'`, so it answered
# "which targets does this harness build?" with "whatever this machine is" — the
# exact inverse of the requirement. It now reads the HOST-FREE leg catalogue
# (legs.json) through the ONE shared resolver (harness_legs.py, also consumed by
# build-and-test.sh), gets the SAME five legs on every host, and ATTEMPTS THE
# BUILD OF EVERY ONE OF THEM. The only legitimate host question — "can this host
# EXECUTE this artifact?" — is answered by the resolver's `run.mode`/`run.verdict`
# from the leg's own `runOn` + `launchers` declaration, never by `$IsWindows`.
# Every declared leg reaches a NAMED verdict from the closed vocabulary in
# tests/test_support/arm_verdict_ledger.hpp; silence about a leg is a harness bug.
#
# The pipeline mirrors the .sh:
#
#   1. identify the host (os/arch), locate this HOST's POSIX toolchain + python3,
#      confirm online, then RESOLVE THE LEG PLAN from the catalogue
#   2. use the dss-code-prime checkout AS-IS on its CURRENT branch (never
#      switched/pulled — a probe tests the working tree exactly as it is)
#   3+4. VIA THIS HOST'S POSIX TOOLCHAIN (WSL on a Windows host, the native
#      shell elsewhere): clone-or-update sqlite/sqlite, configure, and derive the
#      FULL-SOURCE testfixture recipe from the canonical
#      `make -n testfixture USE_AMALGAMATION=0` (the ~185-TU list, the -D
#      defines, the sqlite -I dirs) exactly as the .sh does — then STAGE the
#      sqlite sources + the generated derived sources + the real tcl8.6/zlib
#      HEADERS where the compiler can see them, and emit the recipe in paths the
#      manifest can use. (SQLite's build is autotools + tclsh — inherently Unix;
#      the sources are portable C, so EVERY leg's compile reuses the SAME TU set.)
#      The reference gcc build that produces those derived sources is also
#      PRESERVED, as a ONE-SIDED attribution oracle: it is a Linux ELF, so it can
#      EXONERATE a corpus failure (it fails too => upstream) but never convict
#      (it passes => inconclusive on a Windows/CRT difference). Step 9 prints it.
#   5. locate (or build) a dss-code-prime binary (Release preferred)
#   6. PER LEG, resolve the tcl + zlib LIBRARIES that leg's fixture links against
#      from the leg's OWN declared `libraries.provider`. For the pe64 leg
#      (`search-paths`) `--resolve-library` reads a DLL's EXPORT table (`.edata`)
#      — so it points at real DLLs (tcl86.dll + zlib1.dll), NOT an import .lib;
#      git-for-Windows ships both with the full public API; override with
#      $env:TCL_DLL/$env:ZLIB_DLL. A leg whose DECLARED inputs are not on this
#      machine records `skipped-build-input-missing` — LOUDLY, naming every name
#      and path it searched — and the run continues with the other legs.
#   7. PER LEG, generate a `.dss-project.json` (language c-subset / profile cli /
#      the leg's target spec / artifactName testfixture / the 185 TUs as absolute
#      `sources` / the sqlite+tcl+zlib include dirs / the recipe defines / that
#      leg's two libraries as resolveLibraries) and build it:
#        dss-code-prime --project <manifest> --config=release --output <out>/<leg>
#      → <out>/<leg>/<format>/testfixture[.exe]   (the `.exe` suffix is DERIVED
#      from the leg's object format, never hardcoded)
#   8. PER LEG THAT THIS HOST CAN EXECUTE (run.mode native | launched), run
#      SQLite's `.test` UNIT CORPUS through the dss-built fixture
#      (DSS_TIER: veryquick[default] | quick | full | all), parse
#      "N errors out of M tests", classify failures against the documented
#      non-DSS confounds. GREEN = every failure is a known confound. A leg this
#      host cannot execute was still BUILT, and says so beside its skip verdict.
#      The corpus runs the ORIGINAL, 100% sqlite suite: nothing is omitted by
#      default. A fixture ABORT does not end the run — it is detected, reported,
#      and RESUMED past through sqlite's own `--start=` / SQLITE_TEST_PATTERN_LIST
#      hooks so every remaining unit still reaches a verdict, while the abort
#      itself stays on the record as a failure (see "THE CORPUS RESUME ENGINE").
#      DSS_TIER_EXCLUDES remains only as an operator escape hatch (QUICKTEST_OMIT);
#      it defaults to EMPTY and any use is reported as a coverage reduction.
#   9. summarise: a LEDGER LINE naming every verdict class and counting EVERY
#      declared leg (the idiom of ArmVerdictLedger::renderCountsLine()), one
#      detail line per non-verified leg, and exit non-zero on any `poisoned` leg
#      or genuine unit failure. STRUCTURAL skips are reported, never fatal;
#      ENVIRONMENTAL skips warn by default and are FATAL under
#      DSS_STRICT_ARM_VERDICTS=1.
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
#                      DSS_TIER_EXCLUDES  DSS_MAX_RESUMES
#                      DSS_BRANCH  DSS_COMMIT
#                      DSS_SEGMENT_STALL  DSS_SEGMENT_TIMEOUT
#                      TCL_DLL  ZLIB_DLL  TCL_LIBRARY
#                      DSS_LEGS  DSS_STRICT_ARM_VERDICTS
#
# DSS_LEGS    — comma-separated leg LABELS to restrict this invocation to (fast
#               iteration; parity with build-and-test.sh). Unset = every declared
#               leg. A label that matches nothing DIES with the declared list, and
#               every leg it filters OUT is reported as REMOVED COVERAGE — in the
#               Step-9 summary as well as up front, so a filtered run can never be
#               read as the full leg set.
# DSS_STRICT_ARM_VERDICTS — 1|true|TRUE|yes turns an ENVIRONMENTAL skip (a
#               declared build input or a declared launcher absent from THIS
#               machine) into a hard failure. Mirrors readStrictArmVerdicts() in
#               tests/test_support/arm_verdict_ledger.hpp, INCLUDING its
#               malformed-value handling: an unrecognised value is neither "on"
#               nor "off", it is a LOUD refusal to start (read at Step 1, so a
#               typo costs a second rather than a multi-hour corpus run).
#
# DSS_BRANCH  — the branch you INTEND to test. Unset (default) = whatever the
#               checkout is on. Set, and Step 2 ASSERTS it and DIES on a mismatch.
# DSS_COMMIT  — the commit you INTEND to test (full or abbreviated). Unset = no
#               assertion. ⚠ An ASSERTION, never a checkout instruction: this
#               driver NEVER moves our repo (a bare-sha checkout would leave a
#               detached HEAD, contradicting "the checkout AS-IS").
# ★ BOTH ARE PARITY WITH build-and-test.sh:105-121 / :780-799, and the parity is
# load-bearing rather than cosmetic. A CI template or /loop driver that exports
# DSS_COMMIT=<sha> for BOTH legs used to get an assertion on Linux and SILENCE
# here: Windows ignored the variable, built whatever was in the tree, and printed
# a green verdict a reader would take as commit-pinned BECAUSE THE SIBLING LEG WAS.
# A capability in one driver and not the other is a silent harness bug.

$ErrorActionPreference = 'Stop'
# WSL emits UTF-8. Without this, every non-ASCII character in a message coming back
# from the Linux side (the shared-clone lock's operator guidance, for one) is decoded
# with the OEM code page and reaches the console as `?`.
try { [Console]::OutputEncoding = [System.Text.Encoding]::UTF8 } catch {}

# ── logging / fail-loud (mirrors the .sh's step/info/pass/warn/die) ──────────
function Step($m) { Write-Host "`n== $m ==" -ForegroundColor Blue }
function Info($m) { Write-Host "   $m" }
function Pass($m) { Write-Host " [OK] $m" -ForegroundColor Green }
function Warn($m) { Write-Host " [!]  $m" -ForegroundColor Yellow }
function Die($m)  { Write-Host " [X] ERROR: $m" -ForegroundColor Red; exit 1 }

# ── config (override via environment) ────────────────────────────────────────
# This script lives at real-examples/c/sqlite/, so repo root = ../../../.
# Forward slashes, and [IO.Path]::Combine for the work tree: both spell the SAME
# string on Windows (Combine uses the platform separator) and both are the only
# spellings that survive on a POSIX host, where a literal `\` is an ordinary
# filename character rather than a separator.
$RepoRoot     = (Resolve-Path (Join-Path $PSScriptRoot '../../..')).Path
$Jobs         = if ($env:DSS_JOBS) { $env:DSS_JOBS } else { [Environment]::ProcessorCount }
$SqliteWslDir = if ($env:SQLITE_WSL_DIR) { $env:SQLITE_WSL_DIR } else { '$HOME/src/sqlite' }
# ★ NOT a target directory and NOT a claim about the host: `windows` names THIS
# DRIVER's work tree (the .sh owns a sibling), kept as-is so an operator's paths
# and every log reference stay valid. The per-LEG outputs live under out/<label>/.
$Work         = [System.IO.Path]::Combine($RepoRoot, 'build', 'real-examples', 'c', 'sqlite', 'windows')
# ★★ THERE IS NO `$Spec` / `$Fmt` ANY MORE, AND THAT ABSENCE IS THE POINT.
# They were a single hardcoded pe64 target, which made "which target does this
# harness build?" a question this file answered instead of the catalogue. The
# leg set now comes from legs.json via harness_legs.py (Step 1b, `$Legs`), and
# every target-shaped value below is read off the LEG being processed.
$LegsPy       = Join-Path $PSScriptRoot 'harness_legs.py'
# The catalogue itself, named explicitly rather than left to the resolver's
# default: stage-zinc.py takes it too, and both must be reading the SAME file.
$LegsJson     = Join-Path $PSScriptRoot 'legs.json'
# DSS_TIER: which unit-corpus tier — veryquick (default) | quick | full | all.
$Tier         = if ($env:DSS_TIER) { $env:DSS_TIER } else { 'veryquick' }
# DSS_CONFIG: RELEASE by default (load-bearing — the corpus must exercise the
# full optimizer to catch release-only miscompiles; a debug fixture would run
# green while masking a release bug, and far slower).
$Config       = if ($env:DSS_CONFIG) { $env:DSS_CONFIG } else { 'release' }
# DSS_CONFOUNDS: space-separated .NET-regex patterns for KNOWN non-DSS unit
# failures (a failing test matching any is not counted against green). Same set
# as the .sh: WAL set-lock wall-clock timing, an UPSTREAM zipfile test-isolation
# leak, the recover-fault OOM-oracle class.
# zipfile-25.0 — MECHANISM PROVEN 2026-07-26 (the old "error-text env diff" note
# was WRONG): symlink.test:163 `file mkdir x` is never cleaned up and symlink
# sorts before zipfile, so a DIRECTORY named `x` is present in the shared testdir
# when zipfile-25.0 asserts that `zipfile('x')` fails with "cannot open file: x".
# fopen() on a directory SUCCEEDS, so it fails in fread() instead. Proven by a
# 4-case probe in ONE process varying only the filesystem. Compiler-independent.
# See D-SQLITE-ZIPFILE25-SYMLINK-TESTDIR-LEAK.
# ★ ASYMMETRY, DELIBERATELY NOT "FIXED": the .sh also carries `^date-2\.4c$` and
# this list does not. Adding it here would suppress a failure that has never been
# observed on pe64 — a confound must be EARNED per platform, not copied across.
# Tracked by D-SQLITE-CONFOUND-LIST-DRIVER-ASYMMETRY.
# ⚠ PER-DRIVER SCOPE, recorded 2026-08-01 (TF-C108): build-and-test.sh now carries a
# MEASURED matched control for '^busy2-' and '^recoverfault' (DSS and a gcc-built
# reference testfixture ran full.test concurrently; recoverfault failed on the SAME
# FOUR NAMES in both, busy2 on different members of the family in each). That control
# was earned on LINUX and does NOT transfer here — it says nothing about pe64, whose
# own 'full' tier has so far reported 0 errors out of 979,736, i.e. these patterns
# currently suppress NOTHING on this driver. Should a busy2/recoverfault failure ever
# appear on pe64, it must be re-earned with a pe64 control (the reference testfixture
# this driver already builds), not inherited from the Linux row.
#
# ★★ THE LIST IS PER LEG, AND EVERY ENTRY IN IT WAS EARNED ON pe64. That is the
# whole reason `Get-LegConfounds` exists instead of one global `$Confounds`: the
# rule "a confound must be EARNED per platform, not copied across" is exactly as
# true between two LEGS of this driver as it is between the two drivers. This
# driver now attempts five legs; handing the pe64 list to a macho or elf leg would
# silently excuse, on a platform where nothing has ever been measured, failures
# that would be the first evidence of a real bug there. So a leg that is not
# pe64-x86_64 starts with an EMPTY list and says so out loud when it runs.
# DSS_CONFOUNDS overrides deliberately apply to EVERY leg — an operator naming a
# pattern is stating intent, not inheriting one — and that too is announced.
$PeEarnedConfounds = @('^walsetlk-', '^walsetlk\.', '^walsetlk_recover-', '^busy2-', '^zipfile-25\.0$', '^recoverfault')
$ConfoundsOverride = if ($env:DSS_CONFOUNDS) { @($env:DSS_CONFOUNDS -split '\s+' | Where-Object { $_ }) } else { $null }
function Get-LegConfounds($legLabel) {
  if ($null -ne $ConfoundsOverride) { return $ConfoundsOverride }
  if ($legLabel -eq 'pe64-x86_64')  { return $PeEarnedConfounds }
  return @()
}
# DSS_TIER_EXCLUDES: space-separated regexes naming .test FILES to drop from the
# tier. Delivered through SQLite's OWN upstream hook — the QUICKTEST_OMIT env var
# read by test/permutations.test (~line 152): a COMMA-separated list of Tcl regexes
# matched against each test file's tail name and subtracted from `$allquicktests`,
# the set every permutation in all.test is derived from EXCEPT `full` (which uses
# `$alltests`). So an excluded file still runs ONCE, under `full`, and is dropped
# from the derived permutations.
#
# ★★ DEFAULT EMPTY — ALWAYS, ON EVERY TIER. The requirement is that the ORIGINAL,
# 100% sqlite test suite runs, unmodified and with nothing omitted. The mechanism
# survives only as an explicit operator escape hatch; nothing is excluded unless a
# human sets DSS_TIER_EXCLUDES, and any use is loudly reported as the coverage
# reduction it is (echoed before the run, carried into $unitVerdict, printed in
# Step 9), so no result can ever be read as "the whole corpus ran".
#
# An excluded file is NOT how the harness survives an aborting unit — that is the
# RESUME ENGINE's job (Step 8): an abort is detected, reported, and resumed past
# via sqlite's own `--start=` / SQLITE_TEST_PATTERN_LIST hooks, so every remaining
# unit still reaches a verdict while the abort itself stays on the record as a
# FAILURE. Excluding a file would delete coverage; resuming preserves it.
$TierExcludes = if ($null -ne $env:DSS_TIER_EXCLUDES) { @($env:DSS_TIER_EXCLUDES -split '\s+' | Where-Object { $_ }) } `
                else { @() }
# DSS_MAX_RESUMES: hard bound on how many times Step 8 may re-invoke the fixture
# after an abort. Exceeded → the harness STOPS and says so; it never loops, and it
# never masks a fixture that aborts on everything.
$MaxResumes   = if ($env:DSS_MAX_RESUMES) { [int]$env:DSS_MAX_RESUMES } else { 10 }
# DSS_SEGMENT_STALL: seconds a segment may produce NO log output before the harness
# declares it HUNG, kills it, and resumes past it. A stall bound (not a wall-clock
# bound) is the right shape here: the tiers differ by orders of magnitude — `all`
# runs ~2.5 h — but output is continuous within any of them, so a silent fixture is
# the signal. MEASURED headroom: the slowest single test FILE in a real `all` run is
# sort4.test at 306 s, and the fixture prints a line per TEST, not per file, so real
# output gaps are far shorter still. 1800 s is ~6x the slowest file.
$SegStall     = if ($env:DSS_SEGMENT_STALL) { [int]$env:DSS_SEGMENT_STALL } else { 1800 }
# DSS_SEGMENT_TIMEOUT: optional ABSOLUTE per-segment wall-clock cap in seconds.
# 0 = disabled (the default) — the stall bound above is the one that generalises;
# an absolute cap has to be re-tuned for every tier.
$SegCap       = if ($env:DSS_SEGMENT_TIMEOUT) { [int]$env:DSS_SEGMENT_TIMEOUT } else { 0 }
# DSS_KILL_SETTLE: seconds to wait, after killing a hung segment, for the OS to
# actually release its file handles before the next segment starts. MEASURED: with
# no settle the next segment dies at tester.tcl's startup `reset_db` with
# `error deleting "test.db": permission denied` — the harness manufacturing its own
# next failure out of the previous kill.
$KillSettle   = if ($env:DSS_KILL_SETTLE) { [int]$env:DSS_KILL_SETTLE } else { 20 }
# ★ THE resolve-library BINARIES ARE PER LEG AND ARE RESOLVED IN STEP 6 from the
# leg's OWN declared `libraries.provider` / `tclNames` / `zNames` / `searchPaths`.
# There is deliberately no `$TclDll`/`$ZlibDll` global any more: one pair of
# globals silently WAS the pe64 leg's answer, so a second leg would have linked
# against Windows DLLs. $env:TCL_DLL / $env:ZLIB_DLL survive as explicit operator
# overrides for the `search-paths` provider (today: exactly the pe64 leg) — see
# Resolve-LegLibraries.
#
# Tcl runtime script library (init.tcl …) — the fixture needs it at RUN time, so
# this is a fact about the machine EXECUTING a leg, not about any target. It is
# applied only to a leg this host actually runs. The default is the
# git-for-Windows tree that ships the tcl86.dll the pe64 leg links; override with
# $env:TCL_LIBRARY on any other host.
$TclLibrary   = if ($env:TCL_LIBRARY) { $env:TCL_LIBRARY } else { 'C:\Program Files\Git\mingw64\lib\tcl8.6' }

$Stage        = Join-Path $Work 'stage'          # the staged sqlite tree + headers
# PER-LEG output root: <Work>/out/<label>/… . Nothing is written to `out/` itself,
# so one leg's wipe can never reach a sibling's just-built artifact.
$OutRoot      = Join-Path $Work 'out'
$GenPy        = Join-Path $PSScriptRoot 'gen-pe64-manifest.py'

# Convert C:\path → /mnt/c/path for a WSL callee (a backslash Windows path through
# `wsl wslpath` gets its backslashes stripped — do it manually, as the .sh
# companion does). On a POSIX host there is nothing to convert: the path this
# driver holds IS the path the shell will see, so this is the identity. Which of
# the two applies is decided ONCE, in Step 1, from the host's identity — the one
# question a host is allowed to answer.
function ToShellPath($p) {
  $full = (Resolve-Path -LiteralPath $p).Path
  if (-not $script:HostNeedsWsl) { return $full }
  '/mnt/' + $full.Substring(0,1).ToLowerInvariant() + ($full.Substring(2) -replace '\\','/')
}

# Run ONE command line through THIS HOST's POSIX toolchain — the same channel
# Step 3+4 derives the recipe through, chosen by the same Step-1 decision.
# Returns @{ Rc; Out }; the rc is taken DIRECTLY off the native call.
function Invoke-PosixCommand($bashLine) {
  # stderr merged INSIDE bash, never with PowerShell's `2>&1`: PowerShell wraps a
  # native command's stderr in ErrorRecords, an EMPTY stderr line then stringifies
  # to "System.Management.Automation.RemoteException", and non-ASCII bypasses the
  # console encoding and arrives as `?`. Same reasoning as the derive invocation.
  if ($script:HostNeedsWsl) { $out = @(& wsl.exe bash -l -c "$bashLine 2>&1") }
  else                      { $out = @(& bash      -l -c "$bashLine 2>&1") }
  return @{ Rc = $LASTEXITCODE; Out = $out }
}

# ── THE STAGED-SOURCE COHERENCE GATE ─────────────────────────────────────────
# D-HARNESS-SQLITE-STAGED-TREE-MIXED-VINTAGE. MEASURED 2026-08-04.
#
# The harness reuses ONE sqlite build dir and `git pull --rebase`es the checkout
# under it every run. `make` refreshes only prerequisites of the requested target,
# and `testfixture USE_AMALGAMATION=0` makes sqlite3.c / shell.c / tclsqlite3.c /
# tsrc/ prerequisites of NOTHING — orphans that never refresh while every file
# around them marches forward. The staged tree accumulated FIVE upstream vintages
# (2026-06-25 → 2026-08-04); a cross-built sqlite3 out of it compiled and linked
# cleanly and then exited 1 at startup on shell.c's sqlite3_sourceid() guard.
# check-source-coherence.sh compares SQLITE_SOURCE_ID across every staged artifact
# that carries one and fails loud, naming the divergent files and both ids.
#
# ★ IT IS A PRECONDITION ON A SHARED INPUT, NOT A PER-LEG VERDICT — do not
# "improve" it into a `skipped-*` on one leg. ONE stage feeds EVERY leg, so an
# incoherent stage poisons the whole run: it is a run-wide `Die` BEFORE any leg is
# built. A per-leg skip would report four other legs as meaningful results
# obtained from an input that is known to be wrong.
#
# ★ IT IS UNSKIPPABLE, AND THERE IS NO OPT-OUT VARIABLE. It is deliberately NOT
# behind DSS_SKIP_SELFTEST: that flag governs the driver's self-tests, and this is
# a gate on the RUN's input. Its only prerequisite is a POSIX shell, which is a
# STRICT SUBSET of what this driver already requires to derive the recipe at all
# (make -n + tclsh + gcc, Step 3+4) — so a host that can reach this line can
# always run the checker, and there is no honest skip path to invent for it.
function Assert-StagedSourceCoherence($label) {
  $chk = Join-Path $PSScriptRoot 'check-source-coherence.sh'
  if (-not (Test-Path $chk)) {
    Die "staged-source coherence gate missing: $chk`n      It is what stops the harness compiling a tree that silently accumulated several upstream vintages (D-HARNESS-SQLITE-STAGED-TREE-MIXED-VINTAGE). Running without it is how a binary links clean and then exits 1 on sqlite's own sourceid guard."
  }
  $chkP   = ToShellPath $chk
  $stageP = ToShellPath $Stage
  # The STAGED tree is what DSS compiles, so it is what is asserted — the sqlite
  # build dir is only its source. `--checkout` additionally pins the stage to the
  # checkout's own manifest.uuid: ONE coherent state is necessary but not
  # sufficient, and this says WHICH state it must be.
  $dirs = @("$stageP/sqlite/bld", "$stageP/sqlite/src")
  $line = "bash '$chkP' --checkout `"$SqliteWslDir`" --label '$label' " + (($dirs | ForEach-Object { "'$_'" }) -join ' ')
  $r = Invoke-PosixCommand $line
  foreach ($ln in $r.Out) { Info "      $ln" }
  if ($r.Rc -ne 0) {
    Die @"
STAGED SQLITE SOURCES ARE INCOHERENT (check-source-coherence.sh rc=$($r.Rc)) — refusing to compile.
      label : $label
      dirs  : $($dirs -join '  ')
      Every artifact carrying a SQLITE_SOURCE_ID must agree, and agree with the checkout.
      They do not, so this tree is several upstream vintages at once and anything built from
      it is unattributable. D-HARNESS-SQLITE-STAGED-TREE-MIXED-VINTAGE — the report above
      names the divergent files and both ids.
      ★ RE-STAGING WILL NOT FIX IT. Step 3+4 wipes and re-stages on every run, but it copies
      from $SqliteWslDir/bld-dss, and THAT is where the orphans live: sqlite3.c / shell.c /
      tclsqlite3.c / tsrc/ are prerequisites of no target this harness ever asks make for, so
      they never refresh. Regenerate them from the current source state, in the build dir:
          make sqlite3.c shell.c tclsqlite3.c
      (hand-copying one file fixes the symptom you noticed and leaves the ones you did not.)
"@
  }
  Pass "staged sqlite sources coherent — $label"
}

# ── Step 0 — SELF-TEST the driver's own late-stage logic ─────────────────────
# python3 is discovered HERE rather than in Step 1 because Step 0's second
# self-test (the leg resolver) needs it. Same two-name ladder as before —
# `python3`, then `python` — and Step 1 still reports which one was found, so
# nothing about the discovery itself changed.
$python3 = Get-Command python3 -ErrorAction SilentlyContinue
if (-not $python3) { $python3 = Get-Command python -ErrorAction SilentlyContinue }
if (-not $python3) { Die "python3 not found on PATH — needed to resolve the leg plan (harness_legs.py) and to generate each leg's .dss-project.json manifest." }
# >>> dss:selftest >>>
# Mirrors build-and-test.sh. The classifier runs at the very END of a run, so a
# defect there is invisible until the build and the whole corpus have already been
# paid for — a top-level `local` in the .sh classifier aborted a COMPLETED 13-hour
# arm64 run at exactly that point (D-HARNESS-TEST-SCOPE-FIDELITY). Syntax checks
# cannot catch that class; only executing the code can.
# So the driver REFUSES TO START if its own end-of-run logic is broken. This reuses
# test-confound-scope.ps1, which EXTRACTS the shipped classifier and runs it — no
# duplicated logic to drift. Set DSS_SKIP_SELFTEST=1 to bypass (not recommended).
$selfTest = Join-Path $PSScriptRoot 'test-confound-scope.ps1'
if ($env:DSS_SKIP_SELFTEST -eq '1') {
  Warn 'driver self-test SKIPPED (DSS_SKIP_SELFTEST=1) — a late-stage defect will not surface until the end of the run.'
} elseif (-not (Test-Path $selfTest)) {
  Die "driver self-test missing: $selfTest`n      This guard is what stops a defect in the END-OF-RUN classifier from costing you the entire run."
} else {
  $stOut = & pwsh -NoProfile -File $selfTest 2>&1
  if ($LASTEXITCODE -ne 0) {
    $stOut | ForEach-Object { "      $_" } | Write-Host
    Die "DRIVER SELF-TEST FAILED — refusing to start.`n      The end-of-run classifier is broken, so this run would execute the whole corpus (hours) and then abort while classifying."
  }
  # ★ PARITY with build-and-test.sh: the SKIP COUNT is part of the result, not a
  # footnote, and an UNPARSEABLE summary is a failure rather than a blank. The old
  # form indexed .Matches.Groups[1] unconditionally, so a self-test that stopped
  # printing `passed=` rendered "OK ( assertions)" -- a pass reported over a result
  # nobody could read, which is the very defect class this guard exists to catch.
  $sm = ($stOut | Select-String -Pattern '^passed=(\d+) failed=(\d+) skipped=(\d+)$' | Select-Object -Last 1)
  if (-not $sm) {
    $stOut | ForEach-Object { "      $_" } | Write-Host
    Die @"
driver self-test exited 0 but printed no readable summary line.
      Expected a final 'passed=N failed=N skipped=N'. Its assertions may well have passed,
      but a self-test whose RESULT cannot be read proves nothing and this guard will not
      report OK over it. Either test-confound-scope.ps1's summary format changed (update
      this parse with it) or it died before printing the line.
"@
  }
  $n = $sm.Matches[0].Groups[1].Value
  $nSkip = $sm.Matches[0].Groups[3].Value
  if ($nSkip -eq '0') {
    Info "driver self-test: OK ($n assertions, 0 skipped)"
  } else {
    Warn "driver self-test: OK ($n assertions) — but $nSkip assertion(s) SKIPPED on this host (unmet prerequisite, normally 'no git on PATH'). That part of the late-stage logic is UNPROVEN for this run: $selfTest"
  }
}
# ── the LEG RESOLVER's own self-test, same refuse-to-start discipline ────────
# The resolver decides WHICH TARGETS THIS RUN BUILDS. A defect there does not
# announce itself: a leg simply fails to appear, the run looks orderly, and the
# missing coverage is discovered — if ever — long after the hours are spent. That
# is the identical failure shape the classifier guard above exists for, so it gets
# the identical treatment, including the DSS_SKIP_SELFTEST=1 escape hatch.
# Its load-bearing assertion is HOST-INVARIANCE OF THE BUILD SET: nine synthetic
# hosts × three launcher-availability worlds must all yield the same legs in the
# same order. That property IS D-HARNESS-CROSS-HOST-ANY-TARGET item (2), executable.
if ($env:DSS_SKIP_SELFTEST -eq '1') {
  Warn 'leg-resolver self-test SKIPPED (DSS_SKIP_SELFTEST=1) — a defect in the leg plan will not surface until a leg is silently missing from the results.'
} elseif (-not (Test-Path $LegsPy)) {
  Die "leg resolver missing: $LegsPy`n      It is the ONE host-independent answer to 'which targets does this harness build?', shared with build-and-test.sh. Without it this driver has no leg set at all."
} else {
  $ltOut = & $python3.Source $LegsPy '--self-test' 2>&1
  if ($LASTEXITCODE -ne 0) {
    $ltOut | ForEach-Object { "      $_" } | Write-Host
    Die "LEG-RESOLVER SELF-TEST FAILED — refusing to start.`n      The leg plan is what decides which targets this run builds; a broken resolver silently deletes coverage."
  }
  # Same discipline as the classifier parse above: an UNREADABLE result is a
  # failure, never an "OK" printed over numbers nobody can see.
  $lm = ($ltOut | Select-String -Pattern '^passed=(\d+) failed=(\d+)$' | Select-Object -Last 1)
  if (-not $lm) {
    $ltOut | ForEach-Object { "      $_" } | Write-Host
    Die @"
leg-resolver self-test exited 0 but printed no readable summary line.
      Expected a final 'passed=N failed=0' from harness_legs.py --self-test. Either that
      output format changed (update this parse with it) or it died before printing the line.
"@
  }
  Info "leg-resolver self-test: OK ($($lm.Matches[0].Groups[1].Value) assertions)"
}
# <<< dss:selftest <<<

# ── Step 1 — host identity, this HOST's POSIX toolchain, online ──────────────
# ★★ WHAT USED TO BE HERE, AND WHY IT IS GONE ★★
#     if (-not $IsWindows -and $PSVersionTable.PSVersion.Major -ge 6) {
#       Die "this harness targets Windows (the pe64 leg); ..."
#     }
# That gate CONFLATED HOST WITH TARGET. pe64 was the only target this driver
# could build, and no other target could be built FROM Windows — the precise
# inversion of "build ANY target inside ANY host". It is deleted, not relaxed.
#
# HOST IDENTITY IS NOW USED FOR EXACTLY TWO THINGS, AND NEVER FOR A THIRD:
#   (1) to answer "can this host EXECUTE this leg's artifact?" — and even that is
#       not answered here: it is answered by the resolver, from the leg's own
#       `runOn` + `launchers`, and arrives as run.mode / run.verdict.
#   (2) to locate this HOST's toolchains (the POSIX shell below, python3 above).
# It is NEVER used to decide which legs exist. If you find yourself adding an
# `if ($HostOs -eq …)` that changes the LEG SET, you have re-locked the harness.
Step '1/9  Host identity + this host''s POSIX toolchain (online)'
function Get-HostOsCanonical {
  # PowerShell 7 exposes $IsWindows/$IsLinux/$IsMacOS. Windows PowerShell 5.1
  # PREDATES them: they are simply UNDEFINED there, so `-not $IsWindows` is $true
  # on the one platform 5.1 can possibly be running on. (That is not a hypothetical
  # — it is why the deleted gate above needed its `-and PSVersion.Major -ge 6`
  # clause to avoid killing every 5.1 run.) 5.1 is Windows-only, so the version IS
  # the answer there and the automatic variables are never consulted.
  if ($PSVersionTable.PSVersion.Major -lt 6) { return 'windows' }
  if ($IsWindows) { return 'windows' }
  if ($IsLinux)   { return 'linux' }
  if ($IsMacOS)   { return 'darwin' }
  return 'unknown'
}
function Get-HostArchCanonical {
  # Spellings match the shipped *.target.json `name`s and currentHostArch() in
  # tests/test_support/arm_verdict_ledger.hpp, so a spec's target prefix compares
  # directly. OSArchitecture (not process architecture): an x86 pwsh on an x64 box
  # can still EXECUTE an x64 artifact, and execution is the question.
  $raw = ''
  try { $raw = [System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture.ToString() } catch { $raw = '' }
  if (-not $raw) { $raw = "$env:PROCESSOR_ARCHITECTURE" }   # 5.1 / older .NET fallback
  $low = "$raw".Trim().ToLowerInvariant()
  if ($low -match '^(x64|amd64|x86_64)$')  { return 'x86_64' }
  if ($low -match '^(arm64|aarch64)$')     { return 'arm64' }
  # UNKNOWN IS REPORTED, NOT GUESSED. The resolver still returns the full leg set
  # for an unrecognised host (its self-test pins that), so an unknown arch costs
  # execution, never coverage.
  return $(if ($low) { $low } else { 'unknown' })
}
$HostOs   = Get-HostOsCanonical
$HostArch = Get-HostArchCanonical
# The ONE host-keyed switch in this driver, and it is a BUILD-HOST TOOLCHAIN fact,
# not a target fact: deriving the sqlite recipe needs a POSIX shell + make + tclsh
# (sqlite's build is autotools + tclsh). On a Windows host that comes from WSL; on
# a Linux/macOS host it is native. Which target gets built is unaffected either way.
$script:HostNeedsWsl = ($HostOs -eq 'windows')
if ($script:HostNeedsWsl) {
  $wsl = Get-Command wsl.exe -ErrorAction SilentlyContinue
  if (-not $wsl) { Die "no POSIX toolchain for this host: wsl.exe not found.`n      This is WHERE THIS HOST FINDS ITS POSIX TOOLCHAIN, not a statement about any target — deriving the sqlite recipe needs a POSIX shell + make + tclsh (sqlite's build is autotools + tclsh).`n      On a Windows host that toolchain is WSL: install WSL + a Debian/Ubuntu distro." }
  $probe = & wsl.exe bash -c 'echo posix-ok' 2>&1
  if ($LASTEXITCODE -ne 0 -or "$probe".Trim() -ne 'posix-ok') { Die "this host's POSIX toolchain (WSL) is present but a bash round-trip failed (got: '$probe')." }
  $PosixToolchain = "WSL ($($wsl.Source))"
} else {
  $bashCmd = Get-Command bash -ErrorAction SilentlyContinue
  if (-not $bashCmd) { Die "no POSIX toolchain for this host: 'bash' is not on PATH.`n      This is WHERE THIS HOST FINDS ITS POSIX TOOLCHAIN, not a statement about any target — deriving the sqlite recipe needs a POSIX shell + make + tclsh (sqlite's build is autotools + tclsh).`n      On a $HostOs host that toolchain is native: install bash (and git gcc make ar tclsh, which Step 3+4 checks for by name)." }
  $probe = & $bashCmd.Source -c 'echo posix-ok' 2>&1
  if ($LASTEXITCODE -ne 0 -or "$probe".Trim() -ne 'posix-ok') { Die "this host's POSIX toolchain (native bash at $($bashCmd.Source)) is present but a round-trip failed (got: '$probe')." }
  $PosixToolchain = "native bash ($($bashCmd.Source))"
  # ⚠ HONESTY, STATED AT THE TOP OF THE RUN RATHER THAN DISCOVERED AT THE BOTTOM:
  # the native-POSIX branch of Step 3+4 has never been EXECUTED. It is written and
  # it fails loud at every checkpoint (the derive script is `set -Eeuo pipefail`
  # and every stage is verified), but "written and loud" is not "measured", and
  # this driver does not print numbers it has not earned.
  Warn "this driver's native-POSIX recipe derivation is UNPROVEN — it has never been executed on a $HostOs host. It fails loud rather than silently, but treat a first run here as a bring-up, and prefer build-and-test.sh, which is the measured driver on a POSIX host."
}
# DSS_STRICT_ARM_VERDICTS is read HERE, not at Step 9, so a typo costs a second
# instead of a multi-hour corpus run. Mirrors readStrictArmVerdicts() in
# tests/test_support/arm_verdict_ledger.hpp EXACTLY, including the third state:
# an unrecognised value is neither on nor off. Folding it into `off` is how a
# stale `=0` or a typo'd `=ture` quietly disables a gate.
$StrictVerdicts = $false
if ($null -ne $env:DSS_STRICT_ARM_VERDICTS) {
  if     ($env:DSS_STRICT_ARM_VERDICTS -cin @('1','true','TRUE','yes'))       { $StrictVerdicts = $true }
  elseif ($env:DSS_STRICT_ARM_VERDICTS -cin @('','0','false','FALSE','no'))   { $StrictVerdicts = $false }
  else {
    Die @"
DSS_STRICT_ARM_VERDICTS='$($env:DSS_STRICT_ARM_VERDICTS)' is neither on nor off.
      Recognised (case-sensitive, same set as readStrictArmVerdicts() in
      tests/test_support/arm_verdict_ledger.hpp):  ON: 1 true TRUE yes   OFF: (empty) 0 false FALSE no
      An unrecognised value is refused rather than treated as OFF — a typo must never
      silently disable the gate it was typed to enable.
"@
  }
}
try {
  $null = Invoke-WebRequest -Uri 'https://github.com' -Method Head -TimeoutSec 20 -UseBasicParsing
} catch { Die "offline — cannot reach https://github.com ($($_.Exception.Message))." }
Info "host: $HostOs/$HostArch   ($([Environment]::OSVersion.VersionString))"
Info "posix toolchain: $PosixToolchain   python3: $($python3.Source)"
Info "tier: $Tier   config: $Config   strict-verdicts: $(if ($StrictVerdicts) { 'ON (environmental skips are FATAL)' } else { 'off (environmental skips WARN)' })"
Pass "host identified + POSIX toolchain reachable + online"

# ── Step 1b — the LEG PLAN, from the host-free catalogue ─────────────────────
# ★ THE LEG SET COMES FROM legs.json THROUGH harness_legs.py — the SAME resolver
# build-and-test.sh consumes, so the two drivers cannot disagree about which
# targets exist. The host identity is PASSED IN explicitly (rather than letting
# the resolver detect it) for one reason: it puts this driver's own canonical
# answer in the log, where a disagreement between the two detections is visible
# instead of silent.
Step '1b/9  Resolve the leg plan (harness_legs.py --plan)'
$planRc = 0
$planOut = @()
try {
  # rc taken DIRECTLY off the call — $LASTEXITCODE on the very next statement,
  # never after a pipe (a pipe reports the PIPE's status). The try/catch is not
  # decoration: PowerShell 7.3+ can make a nonzero-exiting native command THROW
  # while $ErrorActionPreference is 'Stop', which would abort with a stack trace
  # instead of the diagnostic below.
  $planOut = @(& $python3.Source $LegsPy '--plan' '--host-os' $HostOs '--host-arch' $HostArch '--format' 'json' 2>&1)
  $planRc  = $LASTEXITCODE
} catch {
  $planRc  = if ($LASTEXITCODE) { $LASTEXITCODE } else { 1 }
  $planOut = @("$_")
}
if ($planRc -ne 0) {
  Die "leg resolution FAILED (rc=$planRc) — this driver has no leg set and will not guess one:`n$(($planOut | ForEach-Object { "      $_" }) -join "`n")"
}
$planText = ($planOut | ForEach-Object { "$_" }) -join "`n"
$plan = $null
try { $plan = $planText | ConvertFrom-Json } catch {
  Die "leg resolver returned output that is not JSON:`n      $($_.Exception.Message)`n$(($planOut | Select-Object -First 20 | ForEach-Object { "      $_" }) -join "`n")"
}
$AllLegs = @($plan.legs)
if ($AllLegs.Count -eq 0) { Die "leg resolver returned a plan with NO legs — refusing to run a harness that builds nothing." }
if ($plan.host.os -ne $HostOs -or $plan.host.arch -ne $HostArch) {
  # Not fatal — the resolver canonicalises, and it is the authority on its own
  # spellings — but a silent disagreement between the two host detections is
  # exactly the kind of drift that makes a run's verdict unreadable.
  Warn "the resolver echoed host $($plan.host.os)/$($plan.host.arch) for the $HostOs/$HostArch this driver detected — canonicalisation, or a real disagreement. The resolver's answer is the one used."
}
# DSS_LEGS — a FILTER for fast iteration, never a redefinition of the leg set.
# Parity with build-and-test.sh. An unmatched label DIES (a typo must not quietly
# run a different set), and every leg it removes is reported as the coverage
# reduction it is — here AND in the Step-9 summary.
$LegFilterRaw = if ($env:DSS_LEGS) { @($env:DSS_LEGS -split '[,\s]+' | Where-Object { $_ }) } else { @() }
$FilteredOut = @()
$Legs = $AllLegs
if ($LegFilterRaw.Count) {
  $declared = @($AllLegs | ForEach-Object { $_.label })
  $unknown  = @($LegFilterRaw | Where-Object { $declared -notcontains $_ })
  if ($unknown.Count) {
    Die "DSS_LEGS names $($unknown.Count) leg(s) this harness does not declare: $($unknown -join ' ')`n      Declared legs: $($declared -join ' ')`n      A filter that matches nothing would silently run a different set than you asked for."
  }
  $Legs        = @($AllLegs | Where-Object { $LegFilterRaw -contains $_.label })
  $FilteredOut = @($AllLegs | Where-Object { $LegFilterRaw -notcontains $_.label } | ForEach-Object { $_.label })
}
Info "catalogue declares $($AllLegs.Count) leg(s) — the SAME set on every host:"
$SelectedLabels = @($Legs | ForEach-Object { $_.label })
foreach ($lg in $AllLegs) {
  # by LABEL, not by object identity: a PSCustomObject compares by reference, so
  # `-contains $lg` would quietly answer "no" the day this list is rebuilt.
  $sel = if ($SelectedLabels -contains $lg.label) { ' ' } else { '-' }
  $runTxt = switch ($lg.run.mode) {
    'native'   { 'run: NATIVE' }
    'launched' { "run: LAUNCHED via '$($lg.run.launcher -join ' ')'" }
    default    { "run: skip [$($lg.run.verdict)]" }
  }
  Info "  [$sel] $($lg.label.PadRight(15)) $($lg.spec.PadRight(34)) build: ATTEMPTED   $runTxt"
}
if ($FilteredOut.Count) {
  Warn "DSS_LEGS REMOVED COVERAGE — $($FilteredOut.Count) declared leg(s) will NOT be built or reported on this run: $($FilteredOut -join ' ')"
  Warn "      this run therefore covers $($Legs.Count) of $($AllLegs.Count) declared legs; the Step-9 summary says so too."
}
Pass "leg plan resolved — $($Legs.Count) leg(s) selected of $($AllLegs.Count) declared"

# ── Step 2 — dss-code-prime (current checkout, untouched) ────────────────────
# >>> dss:src-provenance >>>
# ★ THIS REGION IS EXTRACTED AND EXECUTED by test-confound-scope.ps1 (the Step-0
# self-test) against throwaway git repos — the same discipline the .sh's self-test
# applies to its Step-2 gate: run the SHIPPED code, never a re-implementation that
# would stay green while this rotted. The sentinel comments are a CONTRACT with
# that file; rename or move one and the self-test fails with "could not locate",
# which this driver treats as refuse-to-start.
Step "2/9  Use dss-code-prime at $RepoRoot (current checkout, untouched)"
$DssBranchWant = if ($env:DSS_BRANCH) { $env:DSS_BRANCH } else { '' }
$DssCommitWant = if ($env:DSS_COMMIT) { $env:DSS_COMMIT } else { '' }
# One line of git output, or '' — never $null, never an array, never a throw.
# The try/catch is not decoration: under PowerShell 7.4+ the (now stable)
# $PSNativeCommandUseErrorActionPreference makes a nonzero-exiting NATIVE command
# throw while $ErrorActionPreference is 'Stop', so an unresolvable rev-parse would
# abort the driver instead of yielding the UNKNOWN this block exists to print.
# MEASURED here: pwsh 7.5.2 with that preference $false — precisely why it must not
# be relied on. `("$o").Trim()` also collapses git's single output line out of the
# ARRAY PowerShell may hand back, so downstream `-ne` compares strings rather than
# silently doing array filtering.
function Get-GitLine($dir, [string[]]$gitArgs) {
  try { $o = (& git -C $dir @gitArgs 2>$null) } catch { return '' }
  if ($null -eq $o) { return '' }
  return ("$o").Trim()
}
# ★ TEST FOR .git BEFORE CALLING git AT ALL. `git -C <dir>` WALKS UPWARD: aimed at a
# directory that is not a checkout it answers with an ENCLOSING repository's HEAD, so
# the verdict would cite a real but WRONG commit — strictly worse than UNKNOWN,
# because it reads as verified. `Test-Path` with no -PathType on purpose: a
# `git worktree` (and a submodule) keeps .git as a FILE, and a directory-only test
# would call such a checkout "not a repo" — the same choice the .sh makes with `-e`
# (build-and-test.sh:537).
$dssIsRepo = Test-Path -LiteralPath (Join-Path $RepoRoot '.git')
$dssHead = ''; $dssHeadLong = ''; $dssBranch = ''
if ($dssIsRepo) {
  $dssHead     = Get-GitLine $RepoRoot @('rev-parse','--short','HEAD')
  $dssHeadLong = Get-GitLine $RepoRoot @('rev-parse','HEAD')
  $dssBranch   = Get-GitLine $RepoRoot @('rev-parse','--abbrev-ref','HEAD')
}
# PARITY with build-and-test.sh, which grew this in the same cycle
# (D-HARNESS-SH-SRC-DIR-GIT-REQUIRED-VS-RSYNC-GATE item 3, and the preferred fix in
# its measured sibling D-HARNESS-WSL-TREE-PROVENANCE-UNVERIFIABLE). A verdict line
# naming a commit is a CITATION later cycles quote as "the compiler that ran this",
# and it lied in two ways:
#   * an EMPTY field -- `& git ... 2>$null` yields $null when git fails, and
#     "compiler : $DssBin @ " then reads as fine rather than as unknown;
#   * a hash the SOURCES do not match. Mid-cycle this checkout normally carries
#     uncommitted work, so the built compiler is NOT that commit and the verdict
#     should say so rather than assert it.
# Reported, NEVER fatal: gating uncommitted work is what these drivers are for.
# ── WHAT IS PORTED FROM THE .sh's STEP-2, AND WHAT IS DELIBERATELY NOT ───────
# NOT PORTED — the CLONE GATE. Its shapes (a) and (c) are about $SRC_DIR being
# absent, or populated-but-not-a-checkout, and about the harness then CLONING our
# repo onto the default branch. $RepoRoot is derived from $PSScriptRoot, so this
# driver always runs on the checkout it lives in and never clones dss-code-prime at
# all -- neither shape can occur, and inventing a gate for them would be ceremony.
# Nor does the .sh's stale-.git hybrid exist here: the measured instance of that was
# the rsync'd WSL tree, and this driver reads the real Windows checkout (verified in
# that same session: the .ps1 reported correctly while the .sh did not).
# PORTED, because nothing about them is host-shaped: the UNKNOWN(...)-never-empty
# fields, the .git guard, the DETACHED-HEAD mapping, the divergence count, and the
# DSS_BRANCH / DSS_COMMIT ASSERTIONS below. Those last two were the silent gap this
# comment previously said nothing about -- see the env-var block at the top of this
# file for the failure it produced.
if     (-not $dssIsRepo) { $dssHead   = "UNKNOWN(no .git under $RepoRoot)" }
elseif (-not $dssHead)   { $dssHead   = "UNKNOWN(rev-parse HEAD failed in $RepoRoot)" }
# A DETACHED HEAD makes `rev-parse --abbrev-ref HEAD` print the literal string
# "HEAD", which reads as a branch NAMED HEAD and would make the DSS_BRANCH mismatch
# message below nonsense ("but the tree is on 'HEAD'"). Mapped, as the .sh does at
# build-and-test.sh:549.
if     (-not $dssBranch)       { $dssBranch = 'UNKNOWN' }
elseif ($dssBranch -eq 'HEAD') { $dssBranch = 'DETACHED-HEAD' }
$dssDivergeNote = ''
if (-not $dssIsRepo) {
  # Same reason as above: `git status` in a non-checkout reports the ENCLOSING
  # repository's dirt. Not measuring is honest; measuring the wrong tree is not.
  $dssDivergeNote = " (divergence from HEAD UNVERIFIED -- no .git under $RepoRoot)"
} else {
  try {
    # --no-optional-locks: a plain `status` refreshes and REWRITES .git/index, and this
    # step's whole promise is "current checkout, untouched". Untracked files COUNT: a
    # source file HEAD never knew about is divergence exactly as much as an edit is.
    $porcelain = @(& git -C $RepoRoot --no-optional-locks status --porcelain 2>$null | Where-Object { $_ -ne '' })
    if ($LASTEXITCODE -ne 0) {
      $dssDivergeNote = ' (divergence from HEAD UNVERIFIED -- git status failed)'
    } elseif ($porcelain.Count -gt 0) {
      $dssDivergeNote = " (+$($porcelain.Count) file(s) differ from HEAD -- the sources built are NOT exactly this commit)"
    }
  } catch {
    # A throwing git (old git rejecting --no-optional-locks, or PowerShell's native
    # -command error preference) must never cost a run -- it costs the COUNT, and the
    # note says which.
    $dssDivergeNote = ' (divergence from HEAD UNVERIFIED -- git status could not run)'
  }
}
Info "  at $dssHead on $dssBranch$dssDivergeNote"
# ── the DECLARED ref, ASSERTED against the checkout ──────────────────────────
# PARITY with build-and-test.sh:777-799. Intent the operator STATES beats intent
# inferred from the filesystem. Both unset (the default) leaves behaviour exactly
# as it was: use the checkout as it is.
# ⚠ These are ASSERTIONS, never checkout instructions — the .sh made that choice
# deliberately and this driver holds to it: checking out a bare sha leaves a
# DETACHED HEAD, which contradicts the "current checkout, untouched" contract that
# is the whole reason a pre-commit probe can gate uncommitted work.
if ($DssBranchWant -and $dssBranch -ne $DssBranchWant) {
  Die @"
DSS_BRANCH='$DssBranchWant' but $RepoRoot is on '$dssBranch'.
      Refusing to spend a multi-hour corpus run on a branch you did not ask for.
      Check the branch out yourself — this harness NEVER switches our own repo, so
      that a probe tests the working tree exactly as it is — or drop DSS_BRANCH.
"@
}
if ($DssCommitWant) {
  if (-not $dssIsRepo) {
    Die "DSS_COMMIT='$DssCommitWant' cannot be verified: $RepoRoot is not a git checkout (no .git entry)."
  }
  # `--verify … ^{commit}` both resolves an abbreviation and proves the object is a
  # commit that EXISTS here; a typo or an unfetched sha fails loud instead of
  # silently comparing two strings that were never going to match.
  $dssCommitFull = Get-GitLine $RepoRoot @('rev-parse','--verify','--quiet',"$DssCommitWant^{commit}")
  if (-not $dssCommitFull) {
    Die "DSS_COMMIT='$DssCommitWant' does not resolve to a commit in $RepoRoot — fetch it, or fix the value."
  }
  if (-not $dssHeadLong) {
    Die "DSS_COMMIT='$DssCommitWant' cannot be verified: rev-parse HEAD failed in $RepoRoot."
  }
  if ($dssCommitFull -ne $dssHeadLong) {
    Die @"
DSS_COMMIT='$DssCommitWant' ($dssCommitFull) but $RepoRoot is at $dssHeadLong.
      The run would have validated the checkout, not the commit you named.
"@
  }
  Info "  DSS_COMMIT verified: HEAD is $dssCommitFull"
}
# The divergence is REPORTED, never fatal — a dirty tree is the NORMAL shape of a
# pre-commit probe, and refusing it would delete this driver's main use. It is
# WARNED about (not merely noted) so a commit-pinned run cannot read as byte-pinned:
# DSS_COMMIT proves which COMMIT is checked out, never that the sources match it.
if ($dssDivergeNote) { Warn "  $RepoRoot$dssDivergeNote" }
Pass "dss-code-prime checkout ready"
# <<< dss:src-provenance <<<

# ── Step 2b — take the RUN LOCK on the work tree ─────────────────────────────
# MEASURED FAILURE (2026-07-26) this exists to prevent: two harness invocations
# shared $Work. Invocation B's Step 3+4 deleted and re-staged $Stage — the test
# corpus invocation A's fixture was still sourcing .test files from, mid-run — and
# B's Step 7 then died with "Access to the path '…\testfixture.exe' is denied"
# because A's fixture was still EXECUTING that binary. The evidence is unambiguous:
# A's run/ dir and its process both dated 09:49:23, and A's corpus.log was still
# growing (79 MB) at 11:01. So the harness must be single-instance per work tree,
# and silently corrupting a live 2.5 h run is the worse half of that bug.
#
# The lock is LIVENESS-BASED, so a crashed run never wedges the next one: it records
# the owning PID + that process's start time, and a later invocation steals it (and
# SAYS so) when the owner is gone. Correctness never depends on a release.
# >>> dss:run-lock >>>
New-Item -ItemType Directory -Force -Path $Work | Out-Null
$LockDir  = Join-Path $Work '.harness-lock'
$LockFile = Join-Path $LockDir 'owner.txt'
$LockStolen = ''
function Get-LockOwner {
  if (-not (Test-Path $LockFile)) { return $null }
  $t = (Get-Content -Raw -LiteralPath $LockFile -ErrorAction SilentlyContinue)
  if (-not $t) { return $null }
  $parts = $t.Trim() -split '\|'
  if ($parts.Count -lt 2) { return $null }
  return @{ Pid = [int]$parts[0]; Start = $parts[1]; Text = $t.Trim() }
}
function Test-LockOwnerAlive($owner) {
  if (-not $owner) { return $false }
  $p = Get-Process -Id $owner.Pid -ErrorAction SilentlyContinue
  if (-not $p) { return $false }
  # PID reuse guard: the live process must ALSO have the recorded start time.
  try { return ($p.StartTime.Ticks.ToString() -eq $owner.Start) } catch { return $false }
}
$selfStart = (Get-Process -Id $PID).StartTime.Ticks.ToString()
for ($try = 0; $try -lt 2; $try++) {
  try { [void][System.IO.Directory]::CreateDirectory($Work) } catch {}
  $created = $false
  try { $null = New-Item -ItemType Directory -Path $LockDir -ErrorAction Stop; $created = $true } catch { $created = $false }
  if (-not $created) {
    $owner = Get-LockOwner
    if (Test-LockOwnerAlive $owner) {
      Die @"
another dss sqlite harness run is ALREADY ACTIVE on this work tree.
      work tree : $Work
      owner PID : $($owner.Pid)  (still running)
      Two invocations here corrupt each other: Step 3+4 re-stages the .test corpus the
      other run's fixture is sourcing from, and Step 7 cannot delete a testfixture.exe
      that is still executing. Wait for it, or point this run elsewhere by overriding
      the work tree (this script derives it from the repo root).
      If you are certain that PID is dead, remove: $LockDir
"@
    }
    # stale (owner gone, or the PID was reused by something else) — steal + REPORT
    $LockStolen = if ($owner) { "PID $($owner.Pid)" } else { 'an unreadable lock' }
    Remove-Item -Recurse -Force $LockDir -ErrorAction SilentlyContinue
    continue
  }
  break
}
Set-Content -LiteralPath $LockFile -Value "$PID|$selfStart|$(Get-Date -Format o)" -Encoding ascii
if ($LockStolen) { Warn "took over a STALE run lock left by $LockStolen (that run died without releasing it) — reported in the verdict." }
Info "run lock: $LockDir (pid $PID)"
# <<< dss:run-lock <<<

# ── Step 3+4 — derive the full-source recipe + stage (this host's POSIX shell) ─
# ★ TARGET-FREE BY CONSTRUCTION, AND THAT IS WHY IT RUNS ONCE FOR ALL FIVE LEGS.
# What this step produces — the TU list, the -I dirs, the -D defines, the staged
# sqlite sources + tcl/zlib headers — is sqlite's own portable C. Nothing here is
# keyed on a target; the per-target decisions (the recipe TRANSFORM, the stack
# reserve, the resolve-library binaries) are the LEG's, applied in Steps 6/7.
Step "3+4/9  Derive full-source testfixture recipe + stage sources/headers ($(if ($script:HostNeedsWsl) { 'WSL' } else { 'native POSIX' }))"
New-Item -ItemType Directory -Force -Path $Work | Out-Null
# NOTE the stage is NOT wiped here. The wipe happens inside the derive script BELOW,
# after the shared-clone lock is held — otherwise a run that is about to be refused
# has already destroyed its own staged corpus on the way to the refusal.
New-Item -ItemType Directory -Force -Path $Stage | Out-Null
$StageShell = ToShellPath $Stage
# The derivation reproduces the .sh's Step-4 recipe logic (make -n
# testfixture USE_AMALGAMATION=0 → TUs + defines + -I dirs, with the
# libsqlite3.a core recovered via `ar t`), then STAGES the sqlite sources +
# generated derived sources + real tcl8.6/zlib headers into the driver-visible
# $Stage dir and writes the recipe as three files IN THE PATHS THIS DRIVER USES:
#   $Stage/tus.txt  $Stage/includes.base.txt  $Stage/defines.txt
# plus $Stage/zinc-src/{zlib.h,zconf.h} — the deriving host's copy, VERBATIM. The
# per-TARGET zlib header dirs ($Stage/zinc/<recipeTransform>/) and the per-stage
# include lists ($Stage/includes.<recipeTransform>.txt) are written just below,
# by stage-zinc.py, from the guards each leg declares in legs.json.
# The shell-side `win()` below performs that last translation: `wslpath -m` when
# the POSIX shell is WSL and the driver is on the Windows side of a boundary,
# and the identity when the shell and the driver share one filesystem.
$deriveScript = @'
set -Eeuo pipefail
__CLONE_LOCK_REGION__
for t in git gcc make ar tclsh; do
  command -v "$t" >/dev/null 2>&1 || {
    echo "MISSING tool: $t — install the recipe toolchain, e.g.:" >&2
    echo "    sudo apt-get install -y git build-essential tcl tcl-dev zlib1g-dev" >&2
    exit 1; }
done
# win <path> -> the same file spelled the way the DRIVER addresses it.
# `wslpath -m` (the forward-slash Windows form the manifest wants) when this shell
# is WSL and the driver is on the Windows side of that boundary; the IDENTITY when
# the shell and the driver share one filesystem. The body is substituted by the
# PowerShell side from the ONE host decision that also chose this shell — so there
# is no second place where a host could be mis-read, and `wslpath` (which does not
# exist outside WSL) is never named on a host that lacks it.
# ★ Defined HERE, before first use: bash resolves a function at CALL time, and the
# reference-oracle block below calls this long before the staging section does.
win() { __WIN_PATH_BODY__; }
DIR="__SQLITE_WSL_DIR__"
STAGE="__STAGE_WSL__"
# ── SHARED-CLONE WRITE LOCK ─────────────────────────────────────────────────
# THIS is the mutating window: the fetch/checkout/pull below rewrites the very
# .test files a build-and-test.sh corpus run sources LIVE out of this same clone
# for hours. Held only for staging — once the tree is copied to $STAGE the .ps1
# touches the clone no more, so the lock is released when this script ends.
trap dss_clone_lock_release EXIT
dss_clone_lock_write "$DIR" "build-and-test.ps1 pe64 staging (fetch/pull + stage copy)"
echo "CLONE-LOCK=WRITE $DSS_CLONE_LOCK_DIR"
# Only now is anything destroyed: a run that gets refused above still has its
# previous stage intact.
rm -rf "$STAGE"; mkdir -p "$STAGE"
# clone-or-update sqlite (external dependency — DOES pull).
#
# ★ THIS BLOCK USED TO SWALLOW ITS OWN FAILURES. It read
#     git -C "$DIR" fetch --all --prune --quiet            || true
#     git -C "$DIR" pull  --rebase --quiet    2>/dev/null  || true
#   so a failed update (offline, detached HEAD, dirty tree, rebase conflict) was
#   discarded ALONG WITH the stderr explaining it, the run continued on a STALE
#   checkout, and Step 9 still printed an authoritative-looking `sqlite @ <sha>`.
#   That is a silent-WRONG-PROVENANCE defect, not merely a stale-corpus one: the
#   verdict names a version the run did not actually get. Under the
#   `set -Eeuo pipefail` at the top of this script a bare failure now ABORTS.
#
# ★ THE DEFAULT-BRANCH CHECKOUT IS LOAD-BEARING, and mirrors the .sh's
#   clone_or_update (incl. its `default_branch` helper — sqlite's default is
#   `master`/`trunk`, never `main`, so it must be RESOLVED, never assumed).
#   The two drivers SHARE this clone ($HOME/src/sqlite). Without the checkout the
#   .ps1 pulls whatever branch — or detached HEAD — the .sh last left behind, so
#   the WINDOWS leg's corpus version silently depended on the LINUX leg's
#   execution history. Resolving locally via symbolic-ref keeps the common path
#   OFFLINE-clean; `remote show origin` is only the fallback.
if [ -d "$DIR/.git" ]; then
  git -C "$DIR" fetch --all --prune --quiet
  SQLITE_BRANCH="$(git -C "$DIR" symbolic-ref --short refs/remotes/origin/HEAD 2>/dev/null || true)"
  SQLITE_BRANCH="${SQLITE_BRANCH#origin/}"
  if [ -z "$SQLITE_BRANCH" ]; then
    SQLITE_BRANCH="$(git -C "$DIR" remote show origin | sed -n 's/.*HEAD branch: //p')"
  fi
  [ -n "$SQLITE_BRANCH" ] || { echo "could not resolve sqlite's default branch in $DIR" >&2; exit 1; }
  git -C "$DIR" checkout --quiet "$SQLITE_BRANCH"
  git -C "$DIR" pull --rebase --quiet
elif [ ! -e "$DIR/configure" ]; then
  mkdir -p "$(dirname "$DIR")"
  git clone --quiet https://github.com/sqlite/sqlite.git "$DIR"
fi
[ -x "$DIR/configure" ] || { echo "no ./configure in $DIR — not a SQLite checkout" >&2; exit 1; }
BLD="$DIR/bld-dss"; mkdir -p "$BLD"
( cd "$BLD" && "$DIR/configure" >/dev/null 2>&1 )

# ── the -L that TCL_LIBS needs but does not carry (Tcl 9 externalised tommath) ─
# WHY THIS EXISTS — do NOT "simplify" the -L away:
# Tcl 8.6 BUNDLED libtommath inside libtcl. Tcl 9 does NOT, so a Tcl-9
# tclConfig.sh declares it as an EXTERNAL dependency in TCL_LIBS (MEASURED on the
# macOS leg this cycle: TCL_LIBS=' -lz -lpthread -framework CoreFoundation
# -ltommath'), and main.mk's testfixture rule passes $TCL_LIBS through VERBATIM —
# with no `-L` to go with it. The reference build then dies with
# `ld: library 'tommath' not found` while every .o still compiles fine, i.e. the
# ATTRIBUTION ORACLE silently disappears (D-SQLITE-GCC-REFERENCE-FIXTURE-AS-ORACLE).
#
# ★ ON THIS DRIVER THE DEFECT IS LATENT, NOT LIVE — and that is precisely why it is
#   repaired here rather than "when it bites". The WSL distro this harness pins is
#   Debian/Ubuntu with Tcl 8.6, where libtommath is bundled and nothing is missing.
#   The day that distro's default Tcl becomes 9.x the reference build stops linking
#   WITH NO WARNING — a `do-release-upgrade` is the entire trigger. A defect whose
#   activation condition is someone else's release schedule is still a defect.
#
# Mirrors build-and-test.sh's DESIGN, not its syntax:
#   · PROBE-GATED. Every `-l<name>` in TCL_LIBS is LINK-PROBED first; only an
#     UNRESOLVABLE one earns a `-L`. On every WSL distro today every probe passes,
#     NOTHING is added, and both the configure invocation and the link line stay
#     byte-identical to their pre-change form. It is a strict NO-OP on Tcl 8.6.
#   · the probe uses the MAKEFILE'S OWN `CC`, read back out of the Makefile
#     configure just wrote — never a bare `gcc` off PATH. The probe must be the
#     compiler that will actually perform the link, or it lies about what links.
#   · WHICH tclConfig.sh matters, and only configure knows: it is the one configure
#     recorded as TCL_CONFIG_SH — literally the file the link's `.tclenv.sh`
#     sources. So DERIVE IT AFTER configure, off the Makefile, instead of
#     re-guessing the selection and risking a different answer.
#   · the DIRECTORY is DERIVED, never hardcoded. The .sh's chain is
#     pkg-config → `brew --prefix` → its LIB_ROOTS because it also runs on macOS.
#     This is the LINUX chain, the same design against the authorities that exist
#     inside a WSL rootfs: pkg-config's own libdir (the library's authoritative
#     self-description) → ldconfig's cache (the dynamic linker's OWN answer, which
#     is what makes a multiarch dir like /usr/lib/x86_64-linux-gnu findable at all)
#     → the compiler's own `-print-search-dirs` library list. `brew --prefix` is
#     deliberately NOT ported: it exists for the macOS keg layout, has no bearing
#     on a Debian/Ubuntu rootfs, and would be machinery that can never fire.
mk_var() {                      # mk_var <makefile> <NAME> -> its value (first defn, trimmed)
  sed -n "s/^$2[[:space:]]*=[[:space:]]*//p" "$1" 2>/dev/null \
    | sed 's/[[:space:]]*$//' | sed -n '1p' || true
}
PROBE_CC=(cc)                   # replaced with the Makefile's own CC a few lines down
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
dir_holds_lib() {               # dir_holds_lib <dir> <name> -> 0 iff lib<name>.* is there
  local d="$1" n="$2" f
  [ -n "$d" ] && [ -d "$d" ] || return 1
  # an unmatched glob stays LITERAL and `-e` then rejects it — no nullglob needed.
  for f in "$d/lib$n".so "$d/lib$n".so.* "$d/lib$n".a; do
    [ -e "$f" ] && return 0
  done
  return 1
}
ldconfig_bin() {                # ldconfig lives in /sbin, which Debian leaves OFF a non-root PATH
  local c
  for c in ldconfig /sbin/ldconfig /usr/sbin/ldconfig; do
    command -v "$c" >/dev/null 2>&1 && { printf '%s' "$c"; return 0; }
  done
  return 0
}
libdir_for() {                  # libdir_for <name> -> a dir holding lib<name>.* (or "")
  local n="$1" d ldc
  if command -v pkg-config >/dev/null 2>&1; then
    for d in "$(pkg-config --variable=libdir "lib$n" 2>/dev/null || true)" \
             "$(pkg-config --variable=libdir "$n"    2>/dev/null || true)"; do
      dir_holds_lib "$d" "$n" && { printf '%s' "$d"; return 0; }
    done
  fi
  ldc="$(ldconfig_bin)"
  if [ -n "$ldc" ]; then
    while IFS= read -r d; do
      dir_holds_lib "$d" "$n" && { printf '%s' "$d"; return 0; }
    done < <("$ldc" -p 2>/dev/null | sed -n "s|.*=> \(/.*\)/lib$n\.so.*|\1|p" | sort -u)
  fi
  while IFS= read -r d; do
    dir_holds_lib "$d" "$n" && { printf '%s' "$d"; return 0; }
  done < <( { "${PROBE_CC[@]}" -print-search-dirs 2>/dev/null || true; } \
            | sed -n 's/^libraries: *=*//p' | tr ':' '\n' | sed 's|//*$||' | sort -u )
  return 0
}
# ★ every diagnostic below goes to STDERR under a MARKER prefix. Both halves are
#   load-bearing, for DIFFERENT reasons:
#     (a) stdout IS tcl_libs_ldflags's return value — an info line leaking into it
#         would be spliced onto the front of REF_LDFLAGS and handed to configure
#         as part of the flag;
#     (b) the PowerShell side only PRINTS $deriveOut when the derivation FAILS, so
#         on a SUCCESSFUL run an unmarked note is swallowed whole. The marker is
#         what lets the .ps1 re-emit these as Info/Warn — see the REF-LINK-* loop
#         there. Rename one end and the operator silently stops being told.
ref_note() { echo "REF-LINK-NOTE=$*" >&2; }
ref_warn() { echo "REF-LINK-WARN=$*" >&2; }
tcl_libs_ldflags() {            # tcl_libs_ldflags <tclConfig.sh> -> "-Ldir …" | ""
  local cfg="$1" libs tok name dir out=""
  local -a toks=()
  [ -n "$cfg" ] && [ -f "$cfg" ] || return 0
  libs="$( . "$cfg" >/dev/null 2>&1; printf '%s' "${TCL_LIBS:-}" )" || return 0
  # `read -ra` splits on IFS and does NOT glob — safer than `for tok in $libs`.
  read -r -a toks <<< "$libs" || true
  [ ${#toks[@]} -gt 0 ] || return 0
  for tok in "${toks[@]}"; do
    case "$tok" in -l?*) name="${tok#-l}" ;; *) continue ;; esac   # skips -framework X etc.
    probe_link_l -l"$name" && continue        # already resolvable → add NOTHING
    dir="$(libdir_for "$name")"
    if [ -z "$dir" ]; then
      ref_warn "tclConfig.sh declares -l$name in TCL_LIBS, which ${PROBE_CC[*]} cannot resolve and neither pkg-config, ldconfig, nor the compiler's own search dirs can locate. The reference testfixture -- the ATTRIBUTION ORACLE -- will NOT link. Install lib$name-dev inside the WSL distro."
      continue
    fi
    if probe_link_l -L"$dir" -l"$name"; then
      case " $out " in *" -L$dir "*) ;; *) out="${out:+$out }-L$dir" ;; esac
      ref_note "-l$name is not on a default search path -- adding -L$dir"
    else
      ref_warn "lib$name was found under $dir, yet ${PROBE_CC[*]} still cannot link -l$name against it (wrong architecture?). The reference testfixture will NOT link."
    fi
  done
  printf '%s' "$out"
}
_mk_cc="$(mk_var "$BLD/Makefile" CC)"
[ -n "$_mk_cc" ] && read -r -a PROBE_CC <<< "$_mk_cc"      # handles `ccache gcc`
REF_LDFLAGS="$(tcl_libs_ldflags "$(mk_var "$BLD/Makefile" TCL_CONFIG_SH)")"
if [ -n "$REF_LDFLAGS" ]; then
  # The mechanism is sqlite's OWN documented client knob: `LDFLAGS=…` handed to
  # CONFIGURE (not to make) lands in the Makefile as `LDFLAGS.configure`
  # (Makefile.in: `LDFLAGS.configure = @LDFLAGS@`), which main.mk folds into
  # $(LDFLAGS.libsqlite3) and therefore onto the testfixture link line. Chosen over
  # exporting LIBRARY_PATH for one `make` because it BAKES THE FIX INTO THE TREE:
  # the oracle exists to be USED, and a human re-running `make testfixture` by hand
  # inside the WSL clone to attribute a failure must get a link too, not just this
  # script. Also chosen over passing `LDFLAGS.configure=…` to make, which main.mk
  # explicitly says not to rely on.
  # The re-configure fires ONLY when a -L is genuinely missing, so a host that needs
  # none — every WSL distro today — still configures EXACTLY ONCE.
  # (no "reference link" prefix in the text — the PowerShell side already adds one)
  ref_note "re-running configure with LDFLAGS=$REF_LDFLAGS to supply the missing search path"
  ( cd "$BLD" && "$DIR/configure" "LDFLAGS=$REF_LDFLAGS" >/dev/null 2>&1 )
  # FAIL LOUD if it did not land. A silent miss — say sqlite reshapes the @LDFLAGS@
  # substitution upstream — would put us straight back to an oracle that quietly is
  # not there, which is the exact failure mode this block repairs.
  if grep -qF -- "$REF_LDFLAGS" "$BLD/Makefile"; then
    ref_note "LDFLAGS.configure now carries $REF_LDFLAGS"
  else
    ref_warn "configure did NOT carry LDFLAGS='$REF_LDFLAGS' into $BLD/Makefile -- the reference testfixture will almost certainly fail to link; sqlite's LDFLAGS.configure / @LDFLAGS@ substitution may have changed shape upstream."
  fi
fi

# ── the reference build, and the PRESERVED oracle ────────────────────────────
# The build generates every derived .c (parse.c/opcodes.c/ctime.c/tclsqlite-ex.c/
# fts5.c…) + libsqlite3.a, which the DSS TU set needs — AND, when it links, it
# produces the ATTRIBUTION ORACLE. A link miss stays TOLERATED exactly as before
# (the byproducts are still harvested and the run continues); what changes is that
# the binary is no longer thrown away and the log is no longer sent to /dev/null.
#
# ★ THE `rm -f "$BLD/testfixture"` BELOW IS LOAD-BEARING — `make -n` only PRINTS a
#   target's recipe when the target is MISSING. Harvesting that one cc/link line is
#   the ENTIRE reason this step exists (TU list + -D defines + -I dirs), so dropping
#   the `rm` empties $RECIPE and the <150-TU / <18-define floor below kills the run.
#   The copy and the `rm` are a PAIR: the binary is moved OUT of the make target's
#   path *precisely so* the delete can happen without destroying the oracle. The
#   copy is not a make target and not a prerequisite of one, so `make -n` still sees
#   `testfixture` missing and still prints the recipe.
#
# ★ WHAT THIS ORACLE IS, AND — just as important — WHAT IT IS NOT.
#   It is a LINUX/gcc ELF fixture built inside WSL. The leg under test is a native
#   Windows pe64 binary linking tcl86.dll + the MS CRT. So unlike the .sh's, this
#   oracle is ONE-SIDED and must be read that way:
#     · oracle FAILS the same .test  → strong exoneration. The failure is upstream
#       or test-suite, not DSS codegen. Every confound this driver already carries
#       (walsetlk wall-clock, the zipfile-25.0 testdir leak, recoverfault's OOM
#       oracle) is exactly this shape and would have been settled by it.
#     · oracle PASSES                → INCONCLUSIVE. It does NOT convict DSS: a
#       Windows/CRT platform difference passes on Linux by construction. The
#       fpconv1-2.0 anchor at the top of this file is the proof — it was settled by
#       a gcc build linking msvcrt.dll, i.e. a WINDOWS reference, which this is not.
#   That asymmetry is why the verdict line spells it out instead of just printing a
#   path. An oracle whose limits are not stated is a trap, not evidence.
#
# It is preserved into $STAGE — this harness's OWN output tree, NOT the sqlite
# checkout — so sqlite's `make clean`/`distclean` can never take it, and it is
# Windows-visible so a human triaging on the Windows side can see it exists. There
# is no counterpart to the .sh's "clear the previous run's copy BEFORE building":
# `rm -rf "$STAGE"` at the top of this script already guarantees the stronger
# property (past that line the preserved path holds THIS run's binary or nothing),
# and sqlite is pulled on every run, so a stale oracle would attribute against
# sources that are no longer the ones under test — strictly worse than none.
REF_KEEP="$STAGE/reference-testfixture"
REF_LOG="$STAGE/reference-build.log"
if ( cd "$BLD" && make -s testfixture USE_AMALGAMATION=0 -j"$(nproc 2>/dev/null || echo 4)" ) > "$REF_LOG" 2>&1; then
  # Plain `cp` + a best-effort `chmod +x`, NOT the .sh's `cp -p`. $STAGE is a
  # DrvFs (/mnt/c) path: `cp -p` there can fail on the ownership/timestamp
  # preservation it cannot honour and report non-zero for a copy that in fact
  # landed — which would make this announce a MISSING oracle that is sitting right
  # there. The copy is the load-bearing half; DrvFs already presents files as 0777
  # by default, so the chmod only matters when the mount carries real metadata.
  if cp "$BLD/testfixture" "$REF_KEEP"; then
    chmod +x "$REF_KEEP" 2>/dev/null || true
    echo "REF-ORACLE=$REF_KEEP"
    echo "REF-ORACLE-WIN=$(win "$REF_KEEP")"
  else
    echo "REF-ORACLE-MISS=reference testfixture LINKED but could NOT be preserved to $REF_KEEP -- it is about to be deleted to expose the recipe, so no oracle survives this run. Check permissions / free space."
  fi
else
  echo "REF-ORACLE-MISS=reference gcc testfixture did not fully link (tolerated -- byproducts + recipe still harvested). Log KEPT at $(win "$REF_LOG") -- READ IT.${REF_LDFLAGS:+ NOTE: link-path repair WAS in effect (LDFLAGS=$REF_LDFLAGS), so this is a DIFFERENT miss.}"
fi
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
mkdir -p "$STAGE/sqlite/bld" "$STAGE/tclinc" "$STAGE/zinc-src" "$STAGE/test"
cp -r "$DIR/src" "$STAGE/sqlite/src"
cp -r "$DIR/ext" "$STAGE/sqlite/ext"
cp "$BLD"/*.c "$STAGE/sqlite/bld/" 2>/dev/null || true
cp "$BLD"/*.h "$STAGE/sqlite/bld/" 2>/dev/null || true
# the .test corpus + its tcl harness (tester.tcl …) — testfixture.exe runs these
cp -r "$DIR/test/." "$STAGE/test/" 2>/dev/null || true
# real tcl8.6 headers (parsed agnostically — NO descriptor, D-FFI-SHIPPED-LIBS-OS-ONLY)
# ★ KNOWN, STILL LATENT — NOT repaired by this cycle's Tcl-9 work above, and left
#   here on purpose rather than silently half-fixed. Two 8.6-shaped assumptions:
#     · this picks its tclConfig.sh with `find /usr/lib | head -1` rather than the
#       TCL_CONFIG_SH the Makefile records — so on a distro carrying BOTH 8.6 and 9
#       the headers staged for the pe64 compile can come from a DIFFERENT Tcl than
#       the one configure selected for the reference link;
#     · the fallback below globs `-path '*tcl8*'`, which cannot match a Tcl 9 tree
#       at all, so the "tcl.h not found" hard-fail below fires on a 9-only distro.
#   Both are the SAME Tcl-9 story as the -L repair above and both fire on the same
#   trigger (the WSL distro's default Tcl moving to 9.x). They are reported, not
#   patched, because fixing them changes which headers the pe64 leg is COMPILED
#   against — a behaviour change that needs its own measured run, not a drive-by.
TCLH="$( . "$(find /usr/lib -name tclConfig.sh 2>/dev/null | head -1)" >/dev/null 2>&1; printf '%s' "${TCL_INCLUDE_SPEC#-I}" )"
[ -f "$TCLH/tcl.h" ] || TCLH="$(dirname "$(find /usr/include -name tcl.h -path '*tcl8*' 2>/dev/null | head -1)")"
[ -f "$TCLH/tcl.h" ] || { echo "tcl.h not found in WSL — apt-get install tcl-dev" >&2; exit 1; }
cp "$TCLH"/*.h "$STAGE/tclinc/"
# zlib headers (staged privately so they never shadow anything on -I)
#
# ★★ THE DERIVING HOST'S COPY, STAGED VERBATIM AND CONFIGURED FOR NOBODY.
# (D-HARNESS-SQLITE-STAGE-ZCONF-IS-PE-SHAPED, closed TF-C115.)
# This zconf.h has been through the deriving host's ./configure, which rewrote
# `#if 1 /* was set to #if 1 by ./configure */ #define Z_HAVE_UNISTD_H` — a
# measurement of THIS MACHINE, exactly like the recipe's HAVE_*/Z_HAVE_* defines
# that `recipeTransform` exists to drop. So it is copied to `zinc-src/` UNTOUCHED
# and no leg ever includes it: the PowerShell side runs stage-zinc.py, which
# writes ONE zinc/<recipeTransform>/ per declared stage with that stage's guards
# applied, and each leg's include list points at its own.
#
# What used to be here was a `perl -0777 -pi` that flipped Z_HAVE_UNISTD_H to
# `#if 0` IN PLACE — correct for the pe64 leg, wrong for every other one, and
# with a single shared zinc/ there was nowhere for the other four legs' header to
# go. The .ps1 therefore refused them (`poisoned`) rather than compile them
# against a header configured for a different target, and the refusal was right:
# MEASURED TF-C115, that flip is NOT type-neutral off Linux. On darwin `off_t` is
# `long long`, so the guard decides whether zlib's z_off_t is `long long` (guard
# on, matching the real libz.dylib) or `long` (guard off) — same width, DIFFERENT
# TYPE, which is exactly what D-LANG-TYPE-IDENTITY-VOCABULARY forbids. On the pe
# target the un-flipped header fails loud instead (`error[F001D] got unistd.h`).
ZH="$(find /usr/include -maxdepth 3 -name zlib.h 2>/dev/null | head -1)"
[ -f "$ZH" ] || { echo "zlib.h not found in WSL — apt-get install zlib1g-dev" >&2; exit 1; }
[ -f "$(dirname "$ZH")/zconf.h" ] || { echo "zconf.h not found beside $ZH — the pair is what ./configure rewrites; a zlib.h without it cannot be staged for any target" >&2; exit 1; }
mkdir -p "$STAGE/zinc-src"
cp "$ZH" "$(dirname "$ZH")/zconf.h" "$STAGE/zinc-src/"
echo "ZLIB-SRC=$(win "$STAGE/zinc-src")"

# remap a shell-side path under $DIR or the header dirs → its STAGED location; the
# `win()` defined at the top of this script then spells it the way the DRIVER does.
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
# includes.base.txt: the sqlite -I dirs (remapped) + $BLD + tcl. NOT zlib: that
# dir is PER TARGET, so the PowerShell side appends each stage's own zinc/ to
# this base and writes one includes.<stage>.txt per stage. The base is
# leg-independent — everything in it is sqlite's own portable C.
: > "$STAGE/includes.base.txt"
{ echo "$BLD"; for d in "${SQLITE_INCS[@]}"; do echo "$d"; done; } | while read -r d; do
  [ -n "$d" ] || continue; s="$(remap "$d")"; [ -d "$s" ] && win "$s" >> "$STAGE/includes.base.txt"
done
win "$STAGE/tclinc" >> "$STAGE/includes.base.txt"
# defines.txt
printf '%s\n' "${RECIPE_DEFS[@]}" > "$STAGE/defines.txt"
# the staged test dir (Windows path) for the corpus run
win "$STAGE/test" > "$STAGE/testdir.win.txt"

echo "RECIPE-TUS=$(wc -l < "$STAGE/tus.txt")"
echo "RECIPE-DEFS=$(wc -l < "$STAGE/defines.txt")"
echo "RECIPE-INCS=$(wc -l < "$STAGE/includes.base.txt")"
# SQLITE-HEAD is a PROVENANCE FIELD: Step 9 prints it as `sqlite : <dir> @ <sha>`.
# The bare `$(git rev-parse …)` this used to be printed an EMPTY field whenever git
# failed or $DIR was not a checkout -- "sqlite : /home/…/src/sqlite @" reads as fine
# rather than as unknown, which is exactly the shape build-and-test.sh closed by
# routing every provenance read through git_head_short (never empty, always says WHY
# it is unknown). `set -e` never sees a substitution that fails inside an argument,
# so nothing else would have caught it. Same UNKNOWN(<why>) vocabulary here.
if [ -e "$DIR/.git" ]; then
  # `-e`, not `-d`: a worktree/submodule checkout keeps .git as a FILE.
  SQLITE_HEAD="$(git -C "$DIR" rev-parse --short HEAD 2>/dev/null)" || SQLITE_HEAD=""
  [ -n "$SQLITE_HEAD" ] || SQLITE_HEAD="UNKNOWN(rev-parse HEAD failed in $DIR)"
else
  # git -C WALKS UPWARD out of a non-checkout, so without this guard the field could
  # name an enclosing repository's commit -- a confident wrong citation.
  SQLITE_HEAD="UNKNOWN(no .git under $DIR)"
fi
echo "SQLITE-HEAD=$SQLITE_HEAD"
echo "CLONE-LOCK-NOTES=$DSS_CLONE_NOTES"
'@
# The shared-clone lock is EXTRACTED from build-and-test.sh rather than copied, so
# the two drivers run literally the same lock code against the same key and cannot
# drift apart. Both see this clone as the same WSL path, so the keys agree.
$shHarness = Join-Path $PSScriptRoot 'build-and-test.sh'
if (-not (Test-Path $shHarness)) { Die "build-and-test.sh not found next to this script ($shHarness) — it is the single source of the shared-clone lock." }
$shLines = [System.IO.File]::ReadAllLines($shHarness)
$lockStart = -1; $lockEnd = -1
for ($i = 0; $i -lt $shLines.Count; $i++) {
  if ($lockStart -lt 0 -and $shLines[$i] -match '>>> dss:clone-lock >>>') { $lockStart = $i }
  elseif ($lockStart -ge 0 -and $shLines[$i] -match '<<< dss:clone-lock <<<') { $lockEnd = $i; break }
}
if ($lockStart -lt 0 -or $lockEnd -lt 0) { Die "could not find the 'dss:clone-lock' region in $shHarness (start=$lockStart end=$lockEnd) — the shared-clone lock cannot be injected, and running without it lets this staging step rewrite .test files under a live corpus run." }
$cloneLockRegion = ($shLines[($lockStart + 1)..($lockEnd - 1)]) -join "`n"
# `win()`'s body — the ONLY host-shaped line in the derive script, substituted from
# the SAME Step-1 decision that chose the shell. `wslpath` is not even NAMED on a
# host that has no WSL.
$winPathBody = if ($script:HostNeedsWsl) { 'wslpath -m "$1"' } else { 'printf ''%s\n'' "$1"' }
$deriveScript = $deriveScript.Replace('__CLONE_LOCK_REGION__', $cloneLockRegion).Replace('__SQLITE_WSL_DIR__', $SqliteWslDir).Replace('__STAGE_WSL__', $StageShell).Replace('__WIN_PATH_BODY__', $winPathBody) -replace "`r`n", "`n"
$tmpSh = Join-Path $Work 'derive.sh'
# UTF-8 WITHOUT a BOM. `-Encoding ascii` (what this used to be) replaces every
# non-ASCII character with `?` — which silently mangled the injected shared-clone
# lock's operator-facing message. A BOM, meanwhile, would break bash on line 1.
[System.IO.File]::WriteAllText($tmpSh, $deriveScript, (New-Object System.Text.UTF8Encoding($false)))
# Merge stderr into stdout INSIDE bash, not with PowerShell's `2>&1`. PowerShell wraps
# a native command's stderr in ErrorRecords, and an EMPTY stderr line then stringifies
# to "System.Management.Automation.RemoteException" while non-ASCII bypasses the
# console encoding and arrives as `?` — mangling exactly the operator-facing text the
# shared-clone lock prints. On stdout it is plain UTF-8 and survives intact.
# ★ The `bash -l -c "bash '<path>' 2>&1"` shape is preserved EXACTLY on the WSL
# host — a login shell is what puts the recipe toolchain on PATH there. On a native
# POSIX host the driver already IS inside a shell session, so the script is run
# directly and only the `2>&1` merge is reproduced (with PowerShell's own redirect,
# which is safe here because the same merge is happening one process out).
if ($script:HostNeedsWsl) {
  $deriveOut = @(& wsl.exe bash -l -c "bash '$(ToShellPath $tmpSh)' 2>&1")
} else {
  $deriveOut = @(& bash -l "$(ToShellPath $tmpSh)" 2>&1)
}
if ($LASTEXITCODE -ne 0) {
  if (($deriveOut -join "`n") -match 'DSS-CLONE-LOCK-BLOCKED') {
    Die "the shared sqlite clone is LOCKED by another dss harness run:`n$(($deriveOut | Where-Object { $_ -notmatch 'DSS-CLONE-LOCK-BLOCKED' -and $_.Trim() }) -join "`n")"
  }
  Die "WSL recipe derivation failed:`n$($deriveOut -join "`n")"
}
function Marker($k) { ($deriveOut | Select-String -Pattern "^$k=(.+)$" | Select-Object -Last 1).Matches[0].Groups[1].Value }
$nTus = Marker 'RECIPE-TUS'; $nDefs = Marker 'RECIPE-DEFS'; $nIncs = Marker 'RECIPE-INCS'
$sqliteHead = Marker 'SQLITE-HEAD'
# stale clone locks stolen during staging ride into the verdict like any other
# hygiene event — a theft is never silent.
$CloneLockNotes = @()
$m = ($deriveOut | Select-String -Pattern '^CLONE-LOCK-NOTES=(.+)$' | Select-Object -Last 1)
if ($m) { $CloneLockNotes += "shared-clone lock: $($m.Matches[0].Groups[1].Value.TrimEnd('; ',' '))" }
foreach ($n in $CloneLockNotes) { Warn $n }
Info "clone lock: WRITE taken + released for the staging window (the .ps1 touches the clone only here)"
# ── the reference link repair + the ATTRIBUTION ORACLE (both live on the WSL side) ─
# $deriveOut is only PRINTED when the derivation FAILS, so everything the Tcl-9
# link repair and the oracle preservation have to say would otherwise vanish on a
# SUCCESSFUL run — which is exactly how an oracle goes missing with nobody noticing
# (D-SQLITE-GCC-REFERENCE-FIXTURE-AS-ORACLE). Re-emit the markers as ordinary
# Info/Warn lines. The marker NAMES are a CONTRACT with the ref_note/ref_warn/
# REF-ORACLE* emitters in the WSL script above: rename one end only and the
# operator silently stops being told anything.
foreach ($ln in $deriveOut) {
  if     ($ln -match '^REF-LINK-NOTE=(.*)$') { Info "reference link: $($Matches[1])" }
  elseif ($ln -match '^REF-LINK-WARN=(.*)$') { Warn "reference link: $($Matches[1])" }
}
# Deliberately NOT read through `Marker`: that helper indexes .Matches[0]
# unconditionally and throws when its marker is absent, and an ABSENT oracle is a
# normal, TOLERATED outcome here (the byproducts + recipe are what the run needs).
# This is the same null-safe shape the clone-lock notes above use.
$RefOracle = ''; $RefOracleWin = ''; $RefOracleMiss = ''
$om = ($deriveOut | Select-String -Pattern '^REF-ORACLE=(.+)$'      | Select-Object -Last 1)
if ($om) { $RefOracle     = $om.Matches[0].Groups[1].Value.Trim() }
$om = ($deriveOut | Select-String -Pattern '^REF-ORACLE-WIN=(.+)$'  | Select-Object -Last 1)
if ($om) { $RefOracleWin  = $om.Matches[0].Groups[1].Value.Trim() }
$om = ($deriveOut | Select-String -Pattern '^REF-ORACLE-MISS=(.+)$' | Select-Object -Last 1)
if ($om) { $RefOracleMiss = $om.Matches[0].Groups[1].Value.Trim() }
if ($RefOracle) {
  Info "oracle: reference gcc testfixture preserved -> $RefOracleWin"
} else {
  if (-not $RefOracleMiss) { $RefOracleMiss = 'the WSL derivation emitted no oracle marker at all (REF-ORACLE/REF-ORACLE-MISS) — the reference-build block above may have been edited out of the derive script.' }
  Warn "oracle: ABSENT -- $RefOracleMiss"
  Warn "        a corpus failure on this leg therefore cannot be EXONERATED against a non-DSS build."
}
if (-not (Test-Path (Join-Path $Stage 'tus.txt'))) { Die "recipe derivation produced no tus.txt:`n$($deriveOut -join "`n")" }

# ── PER-TARGET zlib headers + PER-LEG include lists ──────────────────────────
# ★ D-HARNESS-SQLITE-STAGE-ZCONF-IS-PE-SHAPED, closed. The staged zconf.h carries
# the DERIVING host's ./configure probe results; each leg declares what ITS target's
# answers are (legs.json `build.zconfGuards`) and stage-zinc.py — the SAME tool
# build-and-test.sh calls — writes one zinc/<recipeTransform>/ per declared stage.
# Nothing here knows what a guard means or which target has <unistd.h>: this loop
# joins a KEY the resolver derived onto a directory. That is the whole difference
# between "target-keyed" and "a branch in a driver".
#
# A stage that cannot be produced is NAMED, and every leg that would have used it
# is poisoned in Step 7 — there is deliberately no fallback to a sibling stage's
# copy, because that fallback is precisely the defect this closes.
$ZincSrc  = Join-Path $Stage 'zinc-src'
$ZincRoot = Join-Path $Stage 'zinc'
foreach ($h in @('zlib.h', 'zconf.h')) {
  if (-not (Test-Path (Join-Path $ZincSrc $h))) { Die "the derivation did not stage $h into $ZincSrc — every leg parses this one header, so there is no leg to build on any host. See the derive output above." }
}
$StageZincPy = Join-Path $PSScriptRoot 'stage-zinc.py'
if (-not (Test-Path $StageZincPy)) { Die "stage-zinc.py not found next to this script ($StageZincPy) — it is the single implementation of per-target zlib header staging, shared with build-and-test.sh." }
$zincOut = & $python3.Source $StageZincPy `
             '--zlib-h'  (Join-Path $ZincSrc 'zlib.h') `
             '--zconf-h' (Join-Path $ZincSrc 'zconf.h') `
             '--dest'    $ZincRoot `
             '--catalogue' $LegsJson 2>&1
$zincRc = $LASTEXITCODE
# label -> the include-list file that leg's manifest must use. A leg absent from
# this map lost its header stage and is poisoned in Step 7.
$LegIncludes = @{}
$StageDirs   = @{}
foreach ($ln in $zincOut) {
  if ($ln -match '^ZINC-STAGE-OK=([^|]*)\|([^|]*)\|([^|]*)\|(.*)$') {
    $k = $Matches[1]; $StageDirs[$k] = $Matches[2]
    Info "zinc stage '$k' -> $($Matches[2])   [$($Matches[3])]"
    if ($Matches[4]) { Info "      note: $($Matches[4])" }
  } elseif ($ln -match '^ZINC-STAGE-FAIL=([^|]*)\|(.*)$') {
    Warn "zinc stage '$($Matches[1])' COULD NOT BE PRODUCED — $($Matches[2])"
  } elseif ($ln -match '^ZINC-STAGES=(.*)$') {
    Info "zinc stages: $($Matches[1]) produced"
  } else { Info "      $ln" }
}
if ($StageDirs.Count -eq 0) { Die "stage-zinc.py produced NO per-target zlib header dir (rc=$zincRc):`n$($zincOut -join "`n")" }
$IncBase = Get-Content -LiteralPath (Join-Path $Stage 'includes.base.txt')
foreach ($k in $StageDirs.Keys) {
  $f = Join-Path $Stage "includes.$k.txt"
  # utf8NoBOM, NOT ascii: these lines are PATHS, and `-Encoding ascii` replaces
  # every non-ASCII character with `?` — a user profile with an accent in it
  # would silently become an include dir that does not exist. gen-pe64-manifest.py
  # reads this file as UTF-8, and a BOM would corrupt its first entry.
  Set-Content -LiteralPath $f -Value (@($IncBase) + @(($StageDirs[$k] -replace '\\','/'))) -Encoding utf8NoBOM
}
foreach ($lg in $AllLegs) {
  $k = "$($lg.build.headerStageKey)"
  if ($StageDirs.ContainsKey($k)) { $LegIncludes[$lg.label] = (Join-Path $Stage "includes.$k.txt") }
}
# FIRST GATE ON THE SHARED INPUT — the moment the stage exists, before anything is
# read out of it. See Assert-StagedSourceCoherence for why this is a run-wide Die
# and not a per-leg verdict, and why it has no opt-out.
Assert-StagedSourceCoherence 'staged sqlite (Step 4)'
Pass "recipe: $nTus TUs, $nDefs defines, $nIncs include dirs (sqlite @ $sqliteHead) staged under $Stage"

# ── Step 5 — locate (or build) the dss-code-prime compiler ───────────────────
# Picks the NEWEST existing binary (build-rel Release / build MSVC / build-dbg
# Ninja-Debug) — newest-wins deliberately avoids a STALE Release binary that
# predates a project-config field (the exact trap that fails loud below). With
# no binary present (a fresh clone) it configures + builds Release into build-rel.
# ★ THE COMPILER IS A HOST BINARY AND HAS NOTHING TO DO WITH THE LEG SET: ONE
# dss-code-prime emits every one of the five targets, selected from config. That
# is why there is one Step 5 and not one per leg.
Step '5/9  Locate / build dss-code-prime (newest existing, else build Release)'
function Find-Dss {
  # Both spellings on every host: the executable suffix is a fact about the
  # machine this compiler RUNS on, and probing for a name that cannot exist here
  # costs one Test-Path.
  $names = @('dss-code-prime.exe', 'dss-code-prime')
  $dirs  = @(
    [System.IO.Path]::Combine($RepoRoot, 'build-rel', 'bin', 'dss'),
    [System.IO.Path]::Combine($RepoRoot, 'build', 'bin', 'dss', 'Release'),
    [System.IO.Path]::Combine($RepoRoot, 'build', 'bin', 'dss'),
    [System.IO.Path]::Combine($RepoRoot, 'build-dbg', 'bin', 'dss')
  )
  $cands = @()
  foreach ($d in $dirs) { foreach ($n in $names) { $cands += (Join-Path $d $n) } }
  $cands = $cands | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } |
             Sort-Object { (Get-Item -LiteralPath $_).LastWriteTime } -Descending
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

# ── Step 6 — PER-LEG build inputs (each leg's OWN tcl + z resolve-libraries) ──
# ★ WHAT CHANGED AND WHY. This used to be two globals — $TclDll/$ZlibDll — that
# silently WERE the pe64 leg's answer: a second leg would have linked against
# Windows DLLs. Each leg now resolves its own (tcl, z) pair from ITS OWN declared
# `libraries.provider` / `tclNames` / `zNames` / `searchPaths`.
#
# ★ A MISSING INPUT IS A NAMED, LOUD, NON-FATAL VERDICT — NOT A DEAD RUN AND NOT
# A SILENT SKIP. `skipped-build-input-missing` exists in the closed vocabulary
# (tests/test_support/arm_verdict_ledger.hpp) for exactly this: "the manifest
# asked for something and the machine could not supply it". It names every name
# and every path searched, so the operator can fix it, and the OTHER legs still
# run. What it must never be is invisible: the run-level rule in Step 9 turns
# ZERO VERIFIED LEGS + at least one ENVIRONMENTAL skip into a hard failure, which
# is how the old "pe64 DLL missing ⇒ Die" loudness survives this generalisation.
Step '6/9  Resolve each leg''s DECLARED build inputs (tcl + z)'

# Candidate roots for the `host-system` provider. legs.json declares NO
# searchPaths for it (its own lint forbids them there), so the roots come from the
# driver — and they are CANDIDATES in exactly the sense legs.json's
# $noHostKeyingComment means: every root is tried on every host, a hit is used, a
# miss costs one Test-Path. No root is skipped because of the host we are on.
# $env:DSS_HOST_LIBDIR is the operator's explicit escape hatch, searched first.
function Get-HostSystemLibRoots {
  $roots = New-Object 'System.Collections.Generic.List[string]'
  foreach ($v in @($env:DSS_HOST_LIBDIR, $env:LD_LIBRARY_PATH, $env:DYLD_LIBRARY_PATH, $env:DYLD_FALLBACK_LIBRARY_PATH)) {
    if ($v) { foreach ($p in ("$v" -split '[:;]')) { if ($p) { [void]$roots.Add($p) } } }
  }
  foreach ($p in @('/usr/lib', '/usr/lib64', '/lib', '/lib64', '/usr/local/lib',
                   '/usr/lib/x86_64-linux-gnu', '/usr/lib/aarch64-linux-gnu',
                   '/usr/local/opt/tcl-tk/lib', '/opt/homebrew/lib', '/opt/local/lib')) {
    [void]$roots.Add($p)
  }
  return $roots
}
# First DECLARED name present under any candidate root. ROOT-major: the path order
# a catalogue declares is a preference order (an operator's ${env:DSS_PE_LIBDIR}
# is listed ahead of the git-for-Windows tree precisely so it wins), and the name
# order breaks ties within one directory.
function Find-DeclaredLib($names, $roots) {
  foreach ($r in $roots) {
    if (-not $r) { continue }
    foreach ($n in $names) {
      if (-not $n) { continue }
      $c = Join-Path $r $n
      if (Test-Path -LiteralPath $c -PathType Leaf) { return (Resolve-Path -LiteralPath $c).Path }
    }
  }
  return ''
}
# Resolve ONE leg's declared (tcl, z) pair. Returns @{ Ok; Tcl; Z; Detail }.
# Ok=$false is ALWAYS `skipped-build-input-missing` with a Detail that names the
# provider, every candidate NAME and every candidate PATH — a reader must be able
# to act on it without reading this script.
function Resolve-LegLibraries($leg) {
  $libs     = $leg.build.libraries
  $provider = "$($libs.provider)"
  $tclNames = @($libs.tclNames)
  $zNames   = @($libs.zNames)
  switch ($provider) {
    'search-paths' {
      # $env:TCL_DLL / $env:ZLIB_DLL — the operator's EXPLICIT override, preserved
      # verbatim for the provider the pe64 leg declares. An override that points at
      # nothing is a HARD error, not a skip: stated intent that cannot be honoured
      # must never be quietly downgraded (this is the one pre-TF-C114 Die shape that
      # is kept exactly as it was).
      $tcl = ''; $z = ''
      if ($env:TCL_DLL) {
        if (-not (Test-Path -LiteralPath $env:TCL_DLL -PathType Leaf)) { Die "`$env:TCL_DLL='$($env:TCL_DLL)' does not name a file — fix it or unset it (leg '$($leg.label)')." }
        $tcl = (Resolve-Path -LiteralPath $env:TCL_DLL).Path
      }
      if ($env:ZLIB_DLL) {
        if (-not (Test-Path -LiteralPath $env:ZLIB_DLL -PathType Leaf)) { Die "`$env:ZLIB_DLL='$($env:ZLIB_DLL)' does not name a file — fix it or unset it (leg '$($leg.label)')." }
        $z = (Resolve-Path -LiteralPath $env:ZLIB_DLL).Path
      }
      # The resolver already expanded every ${env:…} and DROPPED the candidates
      # whose variable is unset, so what arrives here is real directories only.
      $roots = @($libs.searchPaths) | Select-Object -Unique
      if (-not $tcl) { $tcl = Find-DeclaredLib $tclNames $roots }
      if (-not $z)   { $z   = Find-DeclaredLib $zNames   $roots }
      if ($tcl -and $z) { return @{ Ok = $true; Tcl = $tcl; Z = $z; Detail = "provider 'search-paths'" } }
      $missing = @(); if (-not $tcl) { $missing += "tcl ($($tclNames -join ' | '))" }; if (-not $z) { $missing += "z ($($zNames -join ' | '))" }
      return @{ Ok = $false; Tcl = $tcl; Z = $z; Detail = "provider 'search-paths' found no $($missing -join ' and no ') under any declared search path [$($roots -join ' ; ')] — install them, add a path via `$env:DSS_PE_LIBDIR, or point `$env:TCL_DLL/`$env:ZLIB_DLL straight at them" }
    }
    'host-system' {
      $roots = @(Get-HostSystemLibRoots)
      $tcl = Find-DeclaredLib $tclNames $roots
      $z   = Find-DeclaredLib $zNames   $roots
      if ($tcl -and $z) { return @{ Ok = $true; Tcl = $tcl; Z = $z; Detail = "provider 'host-system'" } }
      $missing = @(); if (-not $tcl) { $missing += "tcl ($($tclNames -join ' | '))" }; if (-not $z) { $missing += "z ($($zNames -join ' | '))" }
      return @{ Ok = $false; Tcl = $tcl; Z = $z; Detail = "provider 'host-system' found no $($missing -join ' and no ') under any candidate root [$($roots -join ' ; ')] — this machine has no copy of that target's tcl/zlib runtime; put one anywhere and name the directory in `$env:DSS_HOST_LIBDIR" }
    }
    default {
      # A provider this driver has NO implementation for. Named out loud rather
      # than crashed on or silently passed: the leg is declared, it is real, and
      # the gap is THIS DRIVER's — say which provider, so the next cycle knows
      # exactly what to write.
      return @{ Ok = $false; Tcl = ''; Z = ''; Detail = "library provider '$provider' is NOT IMPLEMENTED by build-and-test.ps1 (implemented: search-paths, host-system). The leg is declared and its build would be attempted; this driver simply cannot obtain its declared inputs (tcl: $($tclNames -join ' | ') / z: $($zNames -join ' | ')). build-and-test.sh is where 'ubuntu-ports-arm64' is implemented today." }
    }
  }
}

# ── Can this driver express THIS leg's manifest correctly? ───────────────────
# ONE thing is keyed on the leg's DECLARED recipeTransform:
#
#   THE GENERATOR. gen-pe64-manifest.py once applied the windows self-config
#   transform (drop the deriving host's HAVE_/Z_HAVE_/_HAVE_SQLITE_CONFIG_H
#   probes, add SQLITE_OS_WIN=1) and an 8 MiB stackReserve UNCONDITIONALLY.
#   It has since grown --recipe-transform/--stack-reserve, so both are now the
#   LEG's to declare. This still PROBES for those arguments rather than
#   assuming them: the two files land in different cycles, and a driver that
#   assumed the newer generator would silently apply the WINDOWS transform to
#   a non-Windows target — a cross-compile category error — against the older
#   one. The probe makes the blocker appear and disappear on its own.
#
# ★ THE SECOND BLOCKER IS GONE, AND THAT IS THE POINT OF TF-C115. It read: "the
# SHARED staged zconf.h has Z_HAVE_UNISTD_H forced to '#if 0' for the pe target
# and there is ONE zinc/ for all legs", so every non-`windows-selfconfig` leg was
# refused. Step 3+4 now stages one zinc/ PER recipeTransform (stage-zinc.py) and
# each leg's manifest gets its OWN include list, so there is nothing left to
# refuse. The blocker was NOT relaxed to get a green — the need for it was
# removed. The ABI question it flagged as UNMEASURED is now MEASURED, and the
# refusal was right: on darwin the guard flips z_off_t between `long long` and
# `long` (same width, different type), and on pe the un-flipped header fails
# loud with `error[F001D] got unistd.h`. Only on Linux LP64 was it benign.
#
# A leg that trips the remaining blocker — or whose header stage could not be
# produced at all — records `poisoned` WITH THE REASON. It is not built wrong and
# reported green, and it is not quietly dropped.
function Get-GenCapabilities {
  $h = ''
  try { $h = (& $python3.Source $GenPy '--help' 2>&1) -join "`n" } catch { $h = '' }
  return @{
    RecipeTransform = ($h -match '--recipe-transform')
    StackReserve    = ($h -match '--stack-reserve')
  }
}
function Test-LegManifestBlockers($leg, $gen) {
  $blockers = @()
  $t = "$($leg.build.recipeTransform)"
  if ($t -ne 'windows-selfconfig') {
    if (-not ($gen.RecipeTransform -and $gen.StackReserve)) {
      $have = @(); if ($gen.RecipeTransform) { $have += '--recipe-transform' }; if ($gen.StackReserve) { $have += '--stack-reserve' }
      $blockers += "this leg declares recipeTransform='$t' + stackReserveBytes=$($leg.build.stackReserveBytes), but $([System.IO.Path]::GetFileName($GenPy)) applies 'windows-selfconfig' + an 8 MiB reserve UNCONDITIONALLY and exposes $(if ($have.Count) { "only $($have -join ' ')" } else { 'neither --recipe-transform nor --stack-reserve' }). Generating this leg's manifest with the generator as it stands would silently apply the WINDOWS transform to a non-Windows target — a cross-compile category error, so it is refused"
    }
  }
  # The leg's OWN staged zlib header. Absent = its zinc/<recipeTransform>/ could
  # not be produced (stage-zinc.py said so, loudly, in Step 3+4). There is NO
  # fallback to a sibling stage's copy: that fallback is the defect.
  if (-not $LegIncludes.ContainsKey($leg.label)) {
    $blockers += "this leg's staged zlib header dir (zinc/$($leg.build.headerStageKey)/, from its declared zconfGuards) was NOT produced, so there is no include list this leg could correctly be compiled with. See the ZINC-STAGE-FAIL line in Step 3+4. Compiling it against another stage's zinc/ is exactly D-HARNESS-SQLITE-STAGE-ZCONF-IS-PE-SHAPED and is refused"
  }
  return $blockers
}
$GenCaps = Get-GenCapabilities
Info "manifest generator: $([System.IO.Path]::GetFileName($GenPy))  --recipe-transform:$(if ($GenCaps.RecipeTransform) { 'YES' } else { 'no' })  --stack-reserve:$(if ($GenCaps.StackReserve) { 'YES' } else { 'no' })"

# ── the ledger: EVERY declared leg gets exactly one row, from here on ────────
# Same shape as ArmVerdictRecord (example/spec/arm/verdict/detail); Step 9 renders
# it in ArmVerdictLedger::renderCountsLine()'s idiom. A leg that never reaches a
# later step keeps the verdict written here, so no leg can end the run unnamed.
$LegLedger = @{}
$LegOrder  = @()
function Set-LegVerdict($label, $verdict, $detail) {
  if (-not $script:LegLedger.ContainsKey($label)) { $script:LegLedger[$label] = @{ Label = $label; Spec = ''; Verdict = ''; Detail = ''; Built = $false; UnitVerdict = ''; UnitFail = $false } }
  $script:LegLedger[$label].Verdict = $verdict
  $script:LegLedger[$label].Detail  = $detail
}
foreach ($lg in $AllLegs) {
  $LegOrder += $lg.label
  $LegLedger[$lg.label] = @{ Label = $lg.label; Spec = $lg.spec; Verdict = ''; Detail = ''; Built = $false; UnitVerdict = ''; UnitFail = $false }
}
# A leg the operator filtered out is NOT verified and must not read as one. It is
# a HARNESS-class non-verification — the runner did not select it — which is the
# meaning `not-selected-by-runner` already carries in the closed vocabulary.
foreach ($lbl in $FilteredOut) {
  Set-LegVerdict $lbl 'not-selected-by-runner' "removed from this run by DSS_LEGS='$($env:DSS_LEGS)' — DECLARED coverage that was not exercised"
}

# Resolve every SELECTED leg's inputs up front, so the operator learns about a
# missing input in the first minute rather than after the first leg's build.
$LegLibs = @{}
foreach ($lg in $Legs) {
  $r = Resolve-LegLibraries $lg
  if ($r.Ok) {
    $LegLibs[$lg.label] = $r
    Info "[$($lg.label)] tcl : $($r.Tcl)"
    Info "[$($lg.label)] z   : $($r.Z)"
  } else {
    Set-LegVerdict $lg.label 'skipped-build-input-missing' $r.Detail
    Warn "[$($lg.label)] BUILD INPUT MISSING — this leg will NOT be built on this machine."
    Warn "      $($r.Detail)"
  }
}
$BuildableLegs = @($Legs | Where-Object { $LegLibs.ContainsKey($_.label) })
if (-not (Test-Path $TclLibrary)) { Warn "Tcl script library not at $TclLibrary — a leg this host RUNS needs it (set `$env:TCL_LIBRARY)." }
Pass "build inputs resolved for $($BuildableLegs.Count) of $($Legs.Count) selected leg(s)"

# ─────────────────────────────────────────────────────────────────────────────
# THE CORPUS RESUME ENGINE — an abort is a RECOVERABLE, REPORTED outcome
# ─────────────────────────────────────────────────────────────────────────────
# The harness exists so that EVERY sqlite unit reaches a verdict. Before this
# engine, a fixture that ABORTED mid-suite (a Tcl `error` out of a test body, a
# hard crash) produced one line — "fixture did not complete the suite (crash?)" —
# and every test FILE behind the abort point was silently never run. One bad unit
# cost the other thousand: two ~2.5h `all` runs died at the same file and taught
# us nothing about the ~7 permutations queued behind it.
#
# DETECT  a segment aborted iff its log has no "N errors out of M tests" summary
#         line. That is the STRUCTURAL fact; the engine is never keyed on a test
#         name or iteration index (the OOM-injection abort point is a function of
#         process-global allocation history — the `all` tier dies at
#         ...oom-persistent.143, the 6s bounded repro at .134).
# LOCATE  from the log: the last COMPLETED file ("Time: <file> N ms"), the
#         permutation ("run_tests <name>" / "run_test_suite <name>" in the Tcl
#         traceback, or the tier's sole suite), and the ABORTING file (resolved
#         from the last emitted test name against the real corpus file list —
#         the traceback's own `(file "…")` frame is NOT usable: Tcl truncates it
#         to ~200 chars, so a long staged path degrades it to `…/test/sw...`).
# RESUME  in a NEW PROCESS (required, not merely convenient: the leaked handle
#         behind this abort class is held for the life of the fixture process),
#         using SQLITE'S OWN upstream hooks — no hand-rolled Tcl runner, nothing
#         written into the sqlite clone or the staged tree:
#           · SQLITE_TEST_PATTERN_LIST (permutations.test ~1175) — a glob list
#             intersected with the permutation's own -files, so passing "every
#             corpus basename after the abort point" selects exactly the
#             permutation's remaining files without the harness ever needing to
#             know that file set.
#           · --start=<permutation>: (tester.tcl ~444 / slave_test_file ~2395) —
#             re-runs THE ORIGINAL tier script, skipping every permutation before
#             the named one, so every `ifcapable`/platform guard in all.test is
#             evaluated by sqlite exactly as in a normal run.
#         So one abort yields at most two segments: the rest of the aborting
#         permutation, then the tier continued from the next permutation.
# BOUND   $MaxResumes (DSS_MAX_RESUMES, default 10). The resume boundary is forced
#         to advance every time, so an aborting file can never be re-entered.
# REPORT  the UNION across segments — total tests, total errors, EVERY abort with
#         its permutation + file, the resume count, and every unit NOT reached.
#         An abort is itself a FAILURE line: resuming never makes it disappear,
#         and a run with aborts is NEVER green.
#
# GRANULARITY (stated because it is a real, reported loss): resume restarts at the
# next FILE. The remainder of the aborting file — the fault-injection iterations
# after the one that died — is NOT run, and is reported as such per abort. sqlite
# exposes no finer restart point than (permutation, file), so a finer resume would
# mean hand-rolling a runner.
#
# >>> dss:corpus-engine >>>  (region mirrored in build-and-test.sh; the verifier
# extracts it from this file by these sentinels, so keep them on their own lines)

# sqlite's own $alltests: every `.test` basename in the corpus dir MINUS the driver
# scripts it excludes by name (all.test / permutations.test / …), ORDINAL-sorted —
# the same order run_tests uses (`lsort $options(-files)`, default -ascii). The
# exclusion list is read as DATA out of permutations.test's own
# `set alltests [test_set $alltests -exclude { … }]` block, never hard-coded; a
# parse miss only widens the list, and the list is used as a SUPERSET filter, so
# the worst case is a wasted resume stepping over a non-unit.
function Get-CorpusFiles($testdir) {
  $skip = @{}
  $perms = Join-Path $testdir 'permutations.test'
  if (Test-Path $perms) {
    $inBlock = $false; $found = $false
    foreach ($line in [System.IO.File]::ReadLines($perms)) {
      if (-not $inBlock) {
        if ($line -match '^\s*set\s+alltests\s+\[test_set\s+\$alltests\s+-exclude\s+\{') { $inBlock = $true; $found = $true }
        continue
      }
      foreach ($w in ($line -split '[\s{}\]]+')) { if ($w -match '\.test$') { $skip[$w] = $true } }
      if ($line -match '\}\s*\]') { break }
    }
    if (-not $found) { Warn "could not read sqlite's \$alltests exclude list from $perms — using the raw *.test glob (a superset; still correct)." }
  }
  $lst = New-Object 'System.Collections.Generic.List[string]'
  Get-ChildItem -LiteralPath $testdir -Filter '*.test' -File |
    ForEach-Object { if (-not $skip.ContainsKey($_.Name)) { $lst.Add($_.Name) } }
  $lst.Sort([System.StringComparer]::Ordinal)
  return $lst
}

# The tier script's permutation sequence, in order, read as DATA from sqlite's own
# `run_test_suite <name>` lines (all.test names 27; veryquick/quick/full name one).
function Get-TierPermutations($tierFile) {
  $names = New-Object 'System.Collections.Generic.List[string]'
  foreach ($line in [System.IO.File]::ReadLines($tierFile)) {
    $m = [regex]::Match($line, '(?<![\w-])run_test_suite\s+([A-Za-z_][A-Za-z0-9_]*)')
    if ($m.Success) { $names.Add($m.Groups[1].Value) }
  }
  return $names
}

# The TEST-NAME PREFIX each suite stamps onto its test names, read as DATA out of
# permutations.test — mirroring tier_prefixes() in build-and-test.sh. Necessary
# because the prefix is NOT derivable from the suite name: permutations.test:39
# defaults it to "<name>." but :220 declares `mmap -prefix "mm-"`, and
# quick/full/threads declare "" (none). The pe64 `all` run emits exactly the
# "mm-…" shape, so without this every ^-anchored confound pattern misses those
# names and a documented confound is reported as a GENUINE failure
# (D-SQLITE-CONFOUND-PERMUTATION-PREFIX). Longest-first so a longer prefix wins
# over one that is merely its head.
function Get-TierPrefixes($permutationsFile) {
  $out = New-Object 'System.Collections.Generic.List[string]'
  if (-not (Test-Path $permutationsFile)) { return $out }
  foreach ($line in [System.IO.File]::ReadLines($permutationsFile)) {
    $mn = [regex]::Match($line, 'test_suite\s+"([^"]*)"')
    if (-not $mn.Success) { continue }
    $mp = [regex]::Match($line, '-prefix\s+"([^"]*)"')
    $p  = if ($mp.Success) { $mp.Groups[1].Value } else { $mn.Groups[1].Value + '.' }
    if ($p -ne '' -and -not $out.Contains($p)) { $out.Add($p) }
  }
  return ($out | Sort-Object -Property Length -Descending)
}

# Single streaming pass over one segment log. (These logs reach 150 MB / 3.6M
# lines — a per-pattern Get-Content sweep would cost minutes per pattern.)
function Read-CorpusSegment($logPath) {
  $r = @{
    Summary = ''; Tests = 0; Errors = 0; FailNames = @{}; Completed = New-Object 'System.Collections.Generic.List[string]'
    LastTest = ''; Permutation = ''; GaveUp = $false; OkLines = 0; FailMarkers = 0
  }
  $reSummary = [regex]'(\d+) errors? out of (\d+) tests'
  $reTime    = [regex]'^Time: (\S+) \d+ ms$'
  $reTest    = [regex]'^(\S+)\.\.\.'
  $reBang    = [regex]'^! (\S+) (expected|got):'
  # NOTE the leading '!': finalize_testing emits `!Failures on these tests: …`
  # (tester.tcl ~1304, `output2 -nonewline "!Failures on these tests:"`).
  $reFails   = [regex]'^!?Failures on these tests:\s*(.+)$'
  $rePerm    = [regex]'^"?(?:run_test_suite|run_tests)\s+([A-Za-z_][A-Za-z0-9_]*)'
  foreach ($line in [System.IO.File]::ReadLines($logPath)) {
    if ($line.Length -eq 0) { continue }
    # Per-test tally. An ABORTED segment never prints a summary, so these counts are
    # the ONLY record of the work it did — see the derivation note at the union.
    if ($line.EndsWith(' Ok', [System.StringComparison]::Ordinal)) { $r.OkLines++ }
    $c = $line[0]
    if ($c -eq 'T') { $m = $reTime.Match($line);  if ($m.Success) { $r.Completed.Add($m.Groups[1].Value); continue } }
    if ($c -eq '!') {
      $m = $reFails.Match($line)
      if ($m.Success) { foreach ($n in ($m.Groups[1].Value -split '\s+')) { if ($n) { $r.FailNames[$n] = $true } }; continue }
      $m = $reBang.Match($line)
      if ($m.Success) {
        $r.FailNames[$m.Groups[1].Value] = $true
        # one `expected:` per FAILED test (`got:` is its partner line) — the failure
        # tally that pairs with OkLines to reconstitute sqlite's own count.
        if ($m.Groups[2].Value -eq 'expected') { $r.FailMarkers++ }
        continue
      }
    }
    if ($c -eq '*' -and $line.StartsWith('*** Giving up')) { $r.GaveUp = $true; continue }
    $m = $reSummary.Match($line)
    if ($m.Success) { $r.Summary = $m.Value; $r.Errors = [int]$m.Groups[1].Value; $r.Tests = [int]$m.Groups[2].Value; continue }
    $m = $rePerm.Match($line.TrimStart()); if ($m.Success) { $r.Permutation = $m.Groups[1].Value; continue }
    $m = $reTest.Match($line); if ($m.Success) { $r.LastTest = $m.Groups[1].Value }
  }
  return $r
}

# Which corpus FILE was the fixture inside when it died? The last test it emitted
# names it: pick the corpus stem that occurs RIGHTMOST in that test name on
# delimiter boundaries (rightmost, then longest). `inmemory_journal.swarmvtabfault
# -1.1-oom-persistent.143` -> swarmvtabfault.test (not swarmvtab.test: the 'f'
# after it is not a delimiter; not the leading permutation token: it is left of it).
function Resolve-AbortFile($lastTest, $corpusFiles) {
  if (-not $lastTest) { return '' }
  $bestFile = ''; $bestIdx = -1; $bestLen = -1
  foreach ($f in $corpusFiles) {
    $stem = $f.Substring(0, $f.Length - 5)   # strip '.test'
    $i = $lastTest.Length
    while ($true) {
      $i = $lastTest.LastIndexOf($stem, [Math]::Min($i, $lastTest.Length - 1), [System.StringComparison]::Ordinal)
      if ($i -lt 0) { break }
      $before = if ($i -eq 0) { '.' } else { [string]$lastTest[$i - 1] }
      $afterI = $i + $stem.Length
      $after  = if ($afterI -ge $lastTest.Length) { '.' } else { [string]$lastTest[$afterI] }
      if (($before -eq '.' -or $before -eq '-') -and ($after -eq '.' -or $after -eq '-')) {
        if ($i -gt $bestIdx -or ($i -eq $bestIdx -and $stem.Length -gt $bestLen)) { $bestIdx = $i; $bestLen = $stem.Length; $bestFile = $f }
        break
      }
      if ($i -eq 0) { break }
      $i = $i - 1
    }
  }
  return $bestFile
}

# Every corpus basename ordinally AFTER $boundary — the SQLITE_TEST_PATTERN_LIST
# superset that sqlite intersects with the permutation's own -files.
function Get-FilesAfter($corpusFiles, $boundary) {
  $out = New-Object 'System.Collections.Generic.List[string]'
  foreach ($f in $corpusFiles) { if ([string]::CompareOrdinal($f, $boundary) -gt 0) { $out.Add($f) } }
  return $out
}

# ── process hygiene ──────────────────────────────────────────────────────────
# Scoped to OUR EXACT fixture binary path — never to the image name. A developer's
# own testfixture.exe, or one belonging to a different checkout, is never touched.
# This is only safe because the run lock guarantees we are the sole invocation on
# this work tree: any process still running THIS path is, by construction, a
# leftover of a run that is already over.
function Get-OurFixtureProcesses($fixturePath) {
  $hits = New-Object 'System.Collections.Generic.List[object]'
  if (-not $fixturePath) { return $hits }
  $want = [System.IO.Path]::GetFullPath($fixturePath)
  foreach ($p in (Get-Process -Name ([System.IO.Path]::GetFileNameWithoutExtension($fixturePath)) -ErrorAction SilentlyContinue)) {
    $path = $null
    try { $path = $p.Path } catch { $path = $null }   # access denied on foreign processes
    if ($path -and ([System.IO.Path]::GetFullPath($path) -eq $want)) { $hits.Add($p) }
  }
  return $hits
}
# Kill every leftover of OUR fixture and return one report line per kill (never
# silent — a killed process is a fact the verdict has to carry).
function Stop-OurFixtures($fixturePath, $why) {
  $killed = New-Object 'System.Collections.Generic.List[string]'
  foreach ($p in (Get-OurFixtureProcesses $fixturePath)) {
    $desc = "pid $($p.Id) (started $($p.StartTime.ToString('s')))"
    try { $p.Kill(); [void]$p.WaitForExit(15000); $killed.Add("$why — killed $desc") }
    catch { $killed.Add("$why — FAILED to kill ${desc}: $($_.Exception.Message)") }
  }
  return $killed
}

# Run ONE fixture segment: stdin at EOF, stdout+stderr to $logPath, killed if it
# stalls (no log growth for $stall s) or exceeds an absolute cap.
#
# stdin is closed deliberately. It is NOT the fix for anything observed — the
# "aborted fixture drops into the Tcl REPL and blocks on stdin" theory was TESTED
# and REFUTED: tclsqlite.c's TCLSH_MAIN evaluates its mainloop script, takes the
# `source $argv0` branch whenever $argv is non-empty, and on error prints errorInfo
# and `return 1`; the `while {![eof stdin]}` REPL is reachable only with NO script
# argument. Measured: with stdin an open pipe that is never written and never
# closed, an aborted fixture still exits rc=1 in 8.8 s. Closing stdin is kept as
# cheap hardening (tester.tcl's own `--pause` does read stdin) — not as a cure.
#
# $launcher is the leg's DECLARED launcher argv (legs.json `launchers`, resolved
# per host by harness_legs.py — qemu for a cross-arch host, Wine for a cross-OS
# one), EMPTY for a native leg. When it is non-empty the OS process is the
# LAUNCHER's and the fixture is its first argument. Two consequences are stated
# rather than discovered:
#   · the stall/cap bounds still apply, because they watch the LOG, not the
#     process — a launcher that hangs looks exactly like a fixture that hangs;
#   · `Get-OurFixtureProcesses` matches on OUR EXACT fixture path, so under a
#     launcher it finds nothing and the settle loop below degrades to its 2 s
#     floor. That is a real limitation of the process sweep, not of the run, and
#     it is reported at the call sites rather than papered over: killing by image
#     name would be the workaround, and it would kill a developer's own qemu.
function Invoke-Fixture($exe, $argv, $workdir, $logPath, $errPath, $stall, $cap, $launcher) {
  $emptyIn = Join-Path ([System.IO.Path]::GetDirectoryName($logPath)) '.stdin-eof'
  Set-Content -LiteralPath $emptyIn -Value '' -NoNewline -Encoding ascii
  $launcher = @($launcher | Where-Object { $_ })
  if ($launcher.Count) {
    $procExe  = $launcher[0]
    $procArgs = @($launcher | Select-Object -Skip 1) + @($exe) + @($argv)
  } else {
    $procExe  = $exe
    $procArgs = @($argv)
  }
  $sp = @{
    FilePath = $procExe; ArgumentList = $procArgs; WorkingDirectory = $workdir
    RedirectStandardOutput = $logPath; RedirectStandardError = $errPath
    RedirectStandardInput = $emptyIn; NoNewWindow = $true; PassThru = $true
  }
  $p = Start-Process @sp
  $t0 = Get-Date; $lastLen = -1L; $lastGrow = Get-Date; $killReason = ''
  while (-not $p.HasExited) {
    Start-Sleep -Seconds 5
    if ($p.HasExited) { break }
    $len = 0L
    try { $len = (Get-Item -LiteralPath $logPath -ErrorAction SilentlyContinue).Length } catch { $len = $lastLen }
    if ($len -ne $lastLen) { $lastLen = $len; $lastGrow = Get-Date }
    elseif ($stall -gt 0 -and ((Get-Date) - $lastGrow).TotalSeconds -ge $stall) {
      $killReason = "produced no output for $stall s (DSS_SEGMENT_STALL)"; break
    }
    if ($cap -gt 0 -and ((Get-Date) - $t0).TotalSeconds -ge $cap) {
      $killReason = "exceeded the absolute cap of $cap s (DSS_SEGMENT_TIMEOUT)"; break
    }
  }
  if ($killReason) {
    try { $p.Kill($true) } catch { try { $p.Kill() } catch {} }   # $true = whole tree
    [void]$p.WaitForExit(30000)
    # SETTLE. MEASURED (on the .sh twin): without this the very next segment dies at
    # tester.tcl's startup `reset_db` with `error deleting "test.db": permission
    # denied` — the killed fixture's handle is still open, so the harness
    # manufactures its own next failure and burns the resume budget on it. Bounded,
    # and only on this path.
    $s = 0
    while ($s -lt $KillSettle -and (Get-OurFixtureProcesses $exe).Count -gt 0) { Start-Sleep -Seconds 1; $s++ }
    Start-Sleep -Seconds 2
  }
  $p.WaitForExit()
  # stderr is appended to the segment log: the Tcl traceback the parser reads is
  # written there, and it is emitted once, at the very end — the same place it sits
  # in a merged stream. A clean segment writes nothing to stderr, so its log is
  # unchanged from what this harness has always produced.
  if ((Test-Path $errPath) -and (Get-Item $errPath).Length -gt 0) {
    Add-Content -LiteralPath $logPath -Value (Get-Content -Raw -LiteralPath $errPath)
  }
  Remove-Item -LiteralPath $emptyIn -ErrorAction SilentlyContinue
  return @{ Rc = $p.ExitCode; KillReason = $killReason; Seconds = ((Get-Date) - $t0).TotalSeconds }
}
# <<< dss:corpus-engine <<<

# ── Step 7 — PER LEG: generate the manifest + build the testfixture ──────────
# ★ EVERY BUILDABLE LEG IS BUILT, ON EVERY HOST. Whether this machine can EXECUTE
# the result is a separate question, asked in Step 8. A leg that this host can
# never run is still compiled and still linked, because that is the capability
# under test: TF-C113 proved the mach-o arm64 leg by BUILDING it on a Windows box
# and shipping the binary to a Mac.
# ★ A BUILD FAILURE IS `poisoned` FOR THAT LEG AND DOES NOT KILL THE RUN. The old
# single-leg `Die` here was correct when there was one leg; with five it would let
# one broken target delete four legs' worth of evidence — the same "one bad unit
# must never cost the other thousand" rule the resume engine exists for. The
# poisoned verdict is loud, it is in the Step-9 ledger, and it exits non-zero.
Step "7/9  Build each leg's full-source testfixture (dss-code-prime --project, $Config)"
# LAST GATE BEFORE HOURS OF COMPILING — re-assert the shared stage's coherence.
# See the call site at the end of Step 3+4 for why this exists; it is repeated
# here because everything between the two is the last chance to notice that the
# input changed vintage before the expensive part begins.
Assert-StagedSourceCoherence 'staged sqlite (pre-compile)'
$BuiltLegs = @()
foreach ($leg in $BuildableLegs) {
  $lbl  = $leg.label
  $fmt  = $leg.format
  # The executable suffix is DERIVED FROM THE OBJECT FORMAT, never hardcoded and
  # never taken from the host: a pe artifact is `.exe` whether it was produced on
  # Windows, Linux or a Mac; an elf or mach-o one has no suffix on any of them.
  $sfx  = if ($fmt -like 'pe*') { '.exe' } else { '' }
  $legOut   = Join-Path $OutRoot $lbl
  $fixture  = Join-Path (Join-Path $legOut $fmt) "testfixture$sfx"
  $manifest = Join-Path $Work "testfixture.$lbl.dss-project.json"
  $LegLedger[$lbl].Fmt      = $fmt
  $LegLedger[$lbl].OutDir   = $legOut
  $LegLedger[$lbl].Fixture  = $fixture

  # ── can this driver express the leg's manifest at all? ──
  $blockers = Test-LegManifestBlockers $leg $GenCaps
  if ($blockers.Count) {
    $why = ($blockers -join '  ALSO: ')
    Set-LegVerdict $lbl 'poisoned' "this driver cannot express this leg's manifest correctly: $why"
    Warn "[$lbl] POISONED — refusing to build a manifest this driver cannot express correctly:"
    foreach ($b in $blockers) { Warn "      $b" }
    continue
  }

# >>> dss:preflight >>>
  # PRE-FLIGHT HYGIENE — FIRST thing for this leg, before anything is generated or
  # deleted. The out-dir wipe further down cannot remove a testfixture that is
  # still executing, and that is exactly how a leftover fixture from a dead run turns
  # into "Access to the path … is denied" — an error that looks nothing like its
  # cause. We hold the run lock, so anything still running OUR fixture path is a
  # leftover by construction.
  #
  # PER LEG, against THIS leg's own path: a sibling leg's fixture is a different
  # file and must never be swept by this one.
  #
  # NOTE the helper functions this calls are defined ABOVE, in the dss:corpus-engine
  # region hoisted before this step. PowerShell binds function names in EXECUTION
  # order, so a helper defined later in the file simply does not exist here — that is
  # a real, measured failure ("The term 'Stop-OurFixtures' is not recognized"), not a
  # style point. Keep the region above this line.
  $LegLedger[$lbl].PreflightKills = @(Stop-OurFixtures $fixture 'pre-flight')
  foreach ($k in $LegLedger[$lbl].PreflightKills) { Warn "[$lbl] LEFTOVER FIXTURE: $k" }
# <<< dss:preflight <<<
  $extraDefineArgs = @()
  # The leg supplies target, libraries and — when the generator can take them —
  # its own recipe transform + stack reserve. When it cannot (Test-LegManifestBlockers
  # has already refused any leg that would need them to differ), the call is
  # BYTE-FOR-BYTE the pe64 invocation this driver has always made.
  # ★ THIS LEG'S OWN INCLUDE LIST — the base recipe dirs plus the zinc/ staged for
  # ITS recipeTransform. Not one shared includes.txt: that is what made every leg
  # compile against the pe leg's zlib header.
  $genArgs = @(
    $GenPy,
    '--tus',      (Join-Path $Stage 'tus.txt'),
    '--includes', $LegIncludes[$lbl],
    '--defines',  (Join-Path $Stage 'defines.txt'),
    '--target',   $leg.spec,
    '--resolve-library', $LegLibs[$lbl].Tcl,
    '--resolve-library', $LegLibs[$lbl].Z,
    '--artifact-name', 'testfixture'
  )
  if ($GenCaps.RecipeTransform) { $genArgs += @('--recipe-transform', "$($leg.build.recipeTransform)") }
  if ($GenCaps.StackReserve)    { $genArgs += @('--stack-reserve',    "$($leg.build.stackReserveBytes)") }
  $genArgs = $genArgs + $extraDefineArgs + @('--output', $manifest)
  $genOut = & $python3.Source @genArgs 2>&1
  if ($LASTEXITCODE -ne 0) {
    Set-LegVerdict $lbl 'poisoned' "manifest generation failed: $($genOut -join ' / ')"
    Warn "[$lbl] POISONED — manifest generation failed:`n$(($genOut | ForEach-Object { "      $_" }) -join "`n")"
    continue
  }
  Info "[$lbl] manifest -> $manifest ($genOut)"
  Info "[$lbl] zlib headers: $($StageDirs[""$($leg.build.headerStageKey)""])  [$(($leg.build.zconfGuards.PSObject.Properties | Sort-Object Name | ForEach-Object { "$($_.Name)=$(if ($_.Value) { 1 } else { 0 })" }) -join ' ')]"

  # SCOPED TO THIS LEG. The wipe used to take the whole of out/; with five legs
  # that would destroy a sibling's just-built artifact. The guard is not ceremony:
  # a $legOut that ever came out empty would make this `Remove-Item -Recurse
  # -Force` of the wrong thing, and the label comes from a JSON document.
  if ((-not $legOut) -or (-not $lbl) -or ($legOut -eq $OutRoot)) { Die "internal: refusing to wipe '$legOut' for leg '$lbl' — it is not a per-leg directory under $OutRoot." }
  if (Test-Path $legOut) { Remove-Item -Recurse -Force $legOut }
  New-Item -ItemType Directory -Force -Path $legOut | Out-Null
  $clog = Join-Path $legOut 'compile.log'
  # dss-code-prime returns exit 0 even on FATAL errors → judge from `error[` + the binary.
  & $DssBin --project $manifest --config="$Config" --output $legOut --time *>&1 |
    Tee-Object -FilePath $clog | Out-Null
  $errCount = (Select-String -Path $clog -Pattern 'error\[' -AllMatches).Count
  $ctime = (Get-Content $clog | Select-String -Pattern 'compile time (\S+)' | Select-Object -Last 1)
  $ctimeSuffix = if ($ctime) { "  ($($ctime.Matches[0].Value))" } else { '' }
  if ($errCount -gt 0 -or -not (Test-Path $fixture)) {
    Get-Content $clog | Select-String -Pattern 'error\[' | Select-Object -First 5 | ForEach-Object { Info "      $($_.Line)" }
    Set-LegVerdict $lbl 'poisoned' "build FAILED$ctimeSuffix — $errCount error[...] diagnostic(s); no testfixture$sfx. See $clog"
    Warn "[$lbl] BUILD FAILED$ctimeSuffix — $errCount error[...] diagnostic(s); no testfixture$sfx. See $clog"
    continue
  }
  $LegLedger[$lbl].Built = $true
  $LegLedger[$lbl].CompileLog = $clog
  $BuiltLegs += $leg
  Pass "[$lbl] testfixture -> $fixture$ctimeSuffix"
}
Info "built $($BuiltLegs.Count) of $($BuildableLegs.Count) buildable leg(s); $($AllLegs.Count) declared"


# ── Step 8 — PER LEG THIS HOST CAN EXECUTE: run the .test UNIT CORPUS ────────
# ★ THE ONE LEGITIMATE HOST QUESTION, AND IT IS NOT ASKED HERE. Whether a leg
# runs is `run.mode` off the resolved plan — `native`, `launched` (through the
# leg's DECLARED launcher), or `skip` with a NAMED verdict from the closed
# vocabulary. There is no `$IsWindows` in this step and there must never be one.
# A `skip` leg WAS BUILT, and Step 9 says so beside its verdict: "built, not run
# here" is a completely different fact from "not built".
Step "8/9  Run SQLite unit corpus ($Tier.test) on every runnable leg + classify"
# Shared across legs — the staged corpus is one tree and one tier, whichever leg
# is executing it.
$StagedTestDir = (Get-Content -Raw (Join-Path $Stage 'testdir.win.txt')).Trim()
$TestFile = if ($env:DSS_TEST_FILE) { $env:DSS_TEST_FILE } else { Join-Path $StagedTestDir "$Tier.test" }
if (-not (Test-Path $TestFile)) { Die "test file not found: $TestFile (tier '$Tier')." }
# Tier exclusions — announced BEFORE the run so the reduction is on the record even
# if the fixture never reaches a summary (see $TierExcludes above).
if ($TierExcludes.Count) {
  Warn "tier EXCLUSIONS active — this run is NOT full-corpus coverage on ANY leg"
  Info "      QUICKTEST_OMIT=$($TierExcludes -join ',')  (sqlite's own hook, test/permutations.test)"
  Info "      drops these file(s) from every `$allquicktests-derived permutation (still run under 'full'):"
  Info "        $($TierExcludes -join ' ')  (operator-set DSS_TIER_EXCLUDES — the default is EMPTY: the 100% corpus)"
}
$CorpusFiles = Get-CorpusFiles $StagedTestDir
$TierPerms   = Get-TierPermutations $TestFile
$TierPrefixes = Get-TierPrefixes (Join-Path $StagedTestDir 'permutations.test')

# A leg that BUILT but that this host cannot execute keeps the resolver's verdict —
# and the fact that it was BUILT is recorded with it. This is the whole point of
# the cycle: the build happened on a host that can never run the artifact.
foreach ($leg in $BuiltLegs) {
  if ($leg.run.mode -ne 'skip') { continue }
  Set-LegVerdict $leg.label "$($leg.run.verdict)" "BUILT OK ($($LegLedger[$leg.label].Fixture)) but NOT RUN on this host — $($leg.run.detail)"
  Info "[$($leg.label)] built, NOT run here [$($leg.run.verdict)]: $($leg.run.detail)"
}
$RunnableLegs = @($BuiltLegs | Where-Object { $_.run.mode -eq 'native' -or $_.run.mode -eq 'launched' })
if ($RunnableLegs.Count -eq 0) { Info "no built leg can be EXECUTED on this host — every one of them has a named skip verdict above." }

foreach ($leg in $RunnableLegs) {
# ★★ THE BODY OF THIS LOOP IS DELIBERATELY NOT INDENTED. Two reasons, both
# load-bearing: (1) test-confound-scope.ps1 EXTRACTS the shipped classifier below
# by matching `^\$real = @\(\)…` at COLUMN 0 and executes it at script scope —
# indenting it would break the Step-0 self-test, which is a refuse-to-start;
# (2) TF-C114 wrapped ~300 lines of already-proven corpus logic in this loop, and
# re-indenting every one of them would have buried the actual change in a
# whitespace diff on the one leg that must not regress. PowerShell attaches no
# meaning to indentation, so this costs nothing but a reader's first glance.
$LegTag      = $leg.label
$legRec      = $LegLedger[$LegTag]
$fixture     = $legRec.Fixture
$legOut      = $legRec.OutDir
$LegRunMode  = $leg.run.mode
$legLauncher = @($leg.run.launcher)
# CONFOUNDS ARE PER LEG AND EVERY ONE OF THEM WAS EARNED SOMEWHERE. See
# Get-LegConfounds: a non-pe64 leg starts EMPTY rather than inheriting a list
# measured on a platform it is not.
$Confounds   = @(Get-LegConfounds $LegTag)
Step "8/9  [$LegTag] $($leg.spec) — $Tier.test ($LegRunMode$(if ($legLauncher.Count) { ": $($legLauncher -join ' ')" }))"
if ($Confounds.Count) {
  Info "[$LegTag] confound patterns in force ($($Confounds.Count)): $($Confounds -join ' ')$(if ($null -ne $ConfoundsOverride) { '   [operator DSS_CONFOUNDS — applied to EVERY leg]' } else { '   [EARNED on this leg]' })"
} else {
  Info "[$LegTag] NO confound patterns: nothing has ever been measured as a non-DSS confound on this leg, and a confound must be EARNED per platform, never copied from a sibling leg. Every failure here counts."
}
# The leg's OWN library directories go on PATH so its fixture can load them at
# run time; TCL_LIBRARY points the Tcl runtime at its script library.
$runEnvPath = (Split-Path $LegLibs[$LegTag].Tcl) + [System.IO.Path]::PathSeparator + (Split-Path $LegLibs[$LegTag].Z) + [System.IO.Path]::PathSeparator + $env:PATH
$rundir = Join-Path $legOut 'run'; if (Test-Path $rundir) { Remove-Item -Recurse -Force $rundir }
New-Item -ItemType Directory -Force -Path $rundir | Out-Null
$runlog = Join-Path $legOut 'corpus.log'
$Ledger = Join-Path $legOut 'corpus-units.txt'

# >>> dss:corpus-loop >>>
# Segment queue. Segment 0 is EXACTLY today's invocation (`fixture <tier>.test`)
# so a run with no abort is bit-for-bit the run it always was; resume segments are
# only ever appended by an abort.
$segments   = @(@{ Kind = 'tier'; Args = @($TestFile); Patterns = @(); Label = "$Tier.test"; Perm = '' })
$results    = @()          # one Read-CorpusSegment record per segment actually run
$aborts     = @()          # one record per abort — these NEVER disappear from the verdict
$notReached = @()          # units we can prove were never given a chance
$hygiene    = @()          # leftover/killed fixture processes — never silent
foreach ($k in $legRec.PreflightKills) { $hygiene += $k }
if ($LockStolen) { $hygiene += "took over a STALE run lock left by $LockStolen" }
foreach ($n in $CloneLockNotes) { $hygiene += $n }
if ($legLauncher.Count) {
  # Stated in the ledger, not just in a comment: under a launcher the OS process
  # is the launcher's, so the fixture-path process sweep below can see nothing.
  $hygiene += "leg runs under launcher '$($legLauncher -join ' ')' — the leftover-fixture sweep matches OUR EXACT fixture PATH and therefore cannot see a launcher-hosted process; process hygiene for this leg is UNVERIFIED"
}
$resumes    = 0
$lastBoundary = ''
$oldPath = $env:PATH; $oldTclLib = $env:TCL_LIBRARY
$oldOmit = $env:QUICKTEST_OMIT; $oldPatterns = $env:SQLITE_TEST_PATTERN_LIST
# The leg's DECLARED launcher environment (legs.json `launchers[].env`, e.g.
# QEMU_LD_PREFIX). Snapshotted so it is restored exactly, including "was unset".
# ★ `Where-Object { $_ }` IS LOAD-BEARING, MEASURED TF-C115 — NOT defensive noise.
# Every leg the resolver plans carries `run.env`, and for a NATIVE run (or a
# launcher declaring `"env": {}`) it is an EMPTY PSCustomObject. An empty object
# is TRUTHY in PowerShell, so the `if` fires; `.PSObject.Properties.Name` over
# zero properties yields a single $null, and `@($null)` is an array of ONE null.
# The next line then evaluates `$oldLegEnv[$null]` and PowerShell throws
# "Index operation failed; the array index evaluated to null" — killing Step 8
# outright for the ONE leg a Windows host can execute. MEASURED: a full driver run
# on 2026-08-04 built both legs and died HERE, at the first line of the corpus
# step, with two good testfixtures already on disk.
$legEnvNames = @(); if ($leg.run.env) { $legEnvNames = @($leg.run.env.PSObject.Properties.Name | Where-Object { $_ }) }
$oldLegEnv = @{}
foreach ($en in $legEnvNames) { $oldLegEnv[$en] = [Environment]::GetEnvironmentVariable($en) }
$si = 0
while ($si -lt $segments.Count) {
  $seg = $segments[$si]
  $log = if ($si -eq 0) { $runlog } else { Join-Path $legOut "corpus.resume$si.log" }
  if ($si -eq 0) { Info "[$LegTag] running $($seg.Label) via $([System.IO.Path]::GetFileName($fixture)) …" }
  else {
    Info "[$LegTag] segment $($si + 1): $($seg.Label)$(if ($seg.Patterns.Count) { "  (SQLITE_TEST_PATTERN_LIST: $($seg.Patterns.Count) candidate file(s))" })"
  }
  try {
    $env:PATH = $runEnvPath; if (Test-Path $TclLibrary) { $env:TCL_LIBRARY = $TclLibrary }
    if ($TierExcludes.Count) { $env:QUICKTEST_OMIT = ($TierExcludes -join ',') }
    # The leg's DECLARED launcher environment (QEMU_LD_PREFIX and friends). Set
    # for the CHILD by setting it here and restoring in the finally — Start-Process
    # inherits this process's block, and there is no per-invocation environment
    # parameter that also preserves the redirections this call needs.
    foreach ($en in $legEnvNames) { [Environment]::SetEnvironmentVariable($en, "$($leg.run.env.$en)") }
    # SQLITE_TEST_PATTERN_LIST is a Tcl LIST of globs; corpus basenames are
    # bare words, so a space join is a valid list.
    if ($seg.Patterns.Count) { $env:SQLITE_TEST_PATTERN_LIST = ($seg.Patterns -join ' ') } else { $env:SQLITE_TEST_PATTERN_LIST = $null }
    $run = Invoke-Fixture $fixture @($seg.Args) $rundir $log "$log.stderr" $SegStall $SegCap $legLauncher
    $segRc = $run.Rc
  } finally {
    $env:PATH = $oldPath; $env:TCL_LIBRARY = $oldTclLib
    $env:QUICKTEST_OMIT = $oldOmit; $env:SQLITE_TEST_PATTERN_LIST = $oldPatterns
    foreach ($en in $legEnvNames) { [Environment]::SetEnvironmentVariable($en, $oldLegEnv[$en]) }
  }
  if ($run.KillReason) { Warn "[$LegTag] segment $($si + 1) HUNG — killed: $($run.KillReason)"; $hygiene += "segment $($si + 1) TIMED OUT and was killed — $($run.KillReason)" }
  # POST-SEGMENT HYGIENE — a segment that spawned or left a fixture behind must not
  # carry its file handles into the next one (the abort class this engine exists for
  # is a leaked handle), nor outlive the run.
  foreach ($k in (Stop-OurFixtures $fixture "after segment $($si + 1)")) {
    Warn "[$LegTag] LEFTOVER FIXTURE: $k"; $hygiene += $k
  }
  $res = Read-CorpusSegment $log
  $res.Log = $log; $res.Rc = $segRc; $res.Label = $seg.Label; $res.Kind = $seg.Kind
  $res.KillReason = $run.KillReason
  $results += $res
  $si++
  if ($res.Summary) {
    # A completed segment can still have stopped early: `*** Giving up...` is
    # tester.tcl hitting --maxerror (default 1000) and finalising. It DOES print a
    # summary, so it would otherwise read as a full run. Say so instead.
    if ($res.GaveUp) {
      $lastF = if ($res.Completed.Count) { $res.Completed[$res.Completed.Count - 1] } else { '(none)' }
      Warn "[$LegTag] segment $si stopped EARLY at the --maxerror cap (`*** Giving up...`) — this is NOT full coverage"
      $notReached += "every file after $lastF in '$($seg.Label)' — the fixture hit its --maxerror cap and finalised early (raise it with --maxerror=N)"
    }
    continue
  }

  # ── ABORT ──────────────────────────────────────────────────────────────────
  $lastDone = if ($res.Completed.Count) { $res.Completed[$res.Completed.Count - 1] } else { '' }
  $perm     = $res.Permutation
  if (-not $perm -and $seg.Perm) { $perm = $seg.Perm }
  if (-not $perm -and $TierPerms.Count -eq 1) { $perm = $TierPerms[0] }
  # A KILLED segment (stall/cap) has NO Tcl traceback at all — and that is precisely
  # when resume matters most, so the traceback cannot be the only source. Fall back
  # to the permutation PREFIX sqlite stamps on every test name (`<perm>.<test>`,
  # run_tests -prefix), then — for an initial tier segment that never showed ANY
  # later permutation's prefix — to the tier's first permutation (all.test's first
  # suite, `full`, is the one declared with -prefix "").
  $permInferred = ''
  if (-not $perm -and $res.LastTest) {
    foreach ($p in $TierPerms) {
      if ($res.LastTest.StartsWith("$p.", [System.StringComparison]::Ordinal)) { $perm = $p; $permInferred = 'from the test-name prefix'; break }
    }
  }
  if (-not $perm -and $res.LastTest -and $seg.Kind -eq 'tier' -and -not $seg.Perm -and $TierPerms.Count -gt 0) {
    $perm = $TierPerms[0]
    $permInferred = "INFERRED — no permutation prefix ever appeared, so the run never left '$($TierPerms[0])', the tier's first suite"
  }
  $abortFile = Resolve-AbortFile $res.LastTest $CorpusFiles
  # The boundary must STRICTLY advance every resume, or an aborting file could be
  # re-entered forever. If the aborting file could not be named (or is not past the
  # last completed one), fall back to the last completed file and then force the
  # boundary one corpus entry forward.
  $boundary = $abortFile
  if (-not $boundary -or [string]::CompareOrdinal($boundary, $lastDone) -le 0) { $boundary = $lastDone }
  $forced = $false
  if ([string]::CompareOrdinal($boundary, $lastBoundary) -le 0) {
    $forced = $true
    $fwd = Get-FilesAfter $CorpusFiles $lastBoundary
    if ($fwd.Count -eq 0) { $boundary = '' } else { $boundary = $fwd[0] }
  }
  $aborts += @{
    Segment = $si; Perm = $perm; File = $abortFile; LastDone = $lastDone
    LastTest = $res.LastTest; Rc = $segRc; Log = $log; Boundary = $boundary
    KillReason = $run.KillReason
  }
  $how = if ($run.KillReason) { "was KILLED after it $($run.KillReason)" } else { "exited rc=$segRc with NO summary line" }
  Warn "[$LegTag] ABORT #$($aborts.Count) — segment $si ('$($seg.Label)') $how"
  Info  "        permutation        : $(if ($perm) { $perm } else { '(UNDETERMINED)' })$(if ($permInferred) { "   [$permInferred]" })"
  Info  "        last file completed: $(if ($lastDone) { $lastDone } else { '(none)' })"
  Info  "        died inside file   : $(if ($abortFile) { $abortFile } else { '(unresolved)' })   last test: $(if ($res.LastTest) { $res.LastTest } else { '(none)' })"
  # The unit that died NEVER goes unreported — named when we can name it, described
  # by what we do know when we cannot. Silence about a unit is the defect.
  if ($abortFile) {
    $notReached += "the REMAINDER of $abortFile under permutation '$(if ($perm) { $perm } else { '?' })' (aborted at $($res.LastTest))"
  } else {
    $what = if ($forced) { "the resume boundary was FORCED to $(if ($boundary) { $boundary } else { 'the end of the corpus' }), so that one file may have been skipped without a verdict" }
            else { "the next segment resumes from $(if ($boundary) { $boundary } else { 'the end of the corpus' }) and will RE-ATTEMPT it" }
    $notReached += "the UNNAMED file that aborted under permutation '$(if ($perm) { $perm } else { '?' })' after $(if ($lastDone) { $lastDone } else { 'the start of the permutation' }) — the log named no resolvable corpus file (last test: $(if ($res.LastTest) { $res.LastTest } else { 'none' })); $what"
  }
  Get-Content $log -Tail 6 | ForEach-Object { Info "      $_" }

  if (-not $boundary) { Warn "[$LegTag] the abort is at the END of the corpus file list — nothing left to resume."; continue }
  if (-not $perm) {
    Warn "[$LegTag] CANNOT RESUME — the aborting permutation could not be determined from the log."
    $notReached += "every unit after $boundary — no resume was possible (permutation undetermined; see $log)"
    continue
  }
  $permIdx = $TierPerms.IndexOf($perm)
  if ($resumes -ge $MaxResumes) {
    Warn "[$LegTag] RESUME BUDGET EXHAUSTED ($MaxResumes) — stopping. Raise DSS_MAX_RESUMES to go further."
    $notReached += "every unit after $boundary in '$perm'" + $(if ($permIdx -ge 0 -and $permIdx -lt $TierPerms.Count - 1) { " and every permutation after '$perm' ($($TierPerms[($permIdx + 1)..($TierPerms.Count - 1)] -join ' '))" } else { '' }) + " — resume budget ($MaxResumes) exhausted"
    continue
  }
  # (a) the rest of the aborting permutation, via sqlite's own file-selection hook.
  $after = Get-FilesAfter $CorpusFiles $boundary
  $resumes++; $lastBoundary = $boundary
  Info  "        -> resume $resumes/$($MaxResumes): permutations.test $perm, corpus files after $boundary"
  $tail = @(@{ Kind = 'perm'; Args = @((Join-Path $StagedTestDir 'permutations.test'), $perm)
               Patterns = $after; Perm = $perm; Label = "permutations.test $perm (after $boundary)" })
  # (b) the tier continued from the NEXT permutation — the ORIGINAL tier script, so
  # every ifcapable/platform guard is evaluated by sqlite exactly as in a full run.
  if ($seg.Kind -ne 'perm') {
    if ($permIdx -lt 0) {
      Warn "[$LegTag] permutation '$perm' is not named by $([System.IO.Path]::GetFileName($TestFile)) — cannot continue the tier past it."
      $notReached += "every permutation after '$perm' in $([System.IO.Path]::GetFileName($TestFile)) — '$perm' is not one of its run_test_suite entries"
    } elseif ($permIdx -lt $TierPerms.Count - 1) {
      $next = $TierPerms[$permIdx + 1]
      $tail += @{ Kind = 'tier'; Args = @($TestFile, "--start=${next}:"); Patterns = @(); Perm = $next
                  Label = "$([System.IO.Path]::GetFileName($TestFile)) --start=${next}:" }
      Info  "        -> then: $([System.IO.Path]::GetFileName($TestFile)) --start=${next}:  (permutations $next..$($TierPerms[$TierPerms.Count - 1]))"
    }
  }
  $segments = @($segments[0..($si - 1)]) + $tail + @(if ($si -lt $segments.Count) { $segments[$si..($segments.Count - 1)] })
}

# ── union the segments + classify ────────────────────────────────────────────
# TWO SOURCES, COUNTED SEPARATELY AND NAMED. A segment that ABORTED prints no
# summary line — but it is not an empty segment: the real `all` run's two aborted
# segments between them executed 4.1M passing tests and one genuine failure
# (mm-backup4-3.3). Summing only the summary lines under-reported that run by ~98%
# and would have hidden any regression inside those segments from the totals.
#
# DERIVATION for an aborted segment, from its per-test lines:
#     tests  = (lines ending " Ok") + (`! <name> expected:` lines) + 1
#     errors = (`! <name> expected:` lines)
# The +1 is STRUCTURAL, not a fudge: finalize_testing reports `[incr_ntest]`
# (tester.tcl ~1273), and incr_ntest INCREMENTS THEN RETURNS, so sqlite's own figure
# is always one more than the tests actually run. Every do_test increments that
# counter and prints exactly one of the two line kinds above.
# CALIBRATED: exact (delta 0) against all four real logs that DO carry a summary —
# 46860, 25452, 1 from the `all` run's resumed segments, and 59055 from an unrelated
# 1000-failure run that also hit `*** Giving up`. It is re-checked EVERY run below,
# so it can never quietly drift out of agreement with sqlite's arithmetic.
$sumTests = 0; $sumErrors = 0; $nSummarised = 0
$derTests = 0; $derErrors = 0; $nDerived = 0
$filesDone = 0; $failNames = @(); $calibration = @()
foreach ($r in $results) {
  $filesDone += $r.Completed.Count
  $failNames += $r.FailNames.Keys        # names ALWAYS flow, summary or not
  $r.DerivedTests  = $r.OkLines + $r.FailMarkers + 1
  $r.DerivedErrors = $r.FailMarkers
  if ($r.Summary) {
    $sumTests += $r.Tests; $sumErrors += $r.Errors; $nSummarised++
    # self-calibration: on a segment where sqlite DID report, the derivation must
    # agree. A mismatch means the derived figures for aborted segments are off by a
    # comparable margin, and that gets said out loud rather than assumed away.
    if ($r.DerivedTests -ne $r.Tests) { $calibration += "$($r.Label): sqlite says $($r.Tests) tests, the per-test derivation says $($r.DerivedTests) (delta $($r.DerivedTests - $r.Tests))" }
  } else {
    $derTests += $r.DerivedTests; $derErrors += $r.DerivedErrors; $nDerived++
  }
}
$failNames = @($failNames | Select-Object -Unique)
$totalTests = $sumTests + $derTests
$totalErrors = $sumErrors + $derErrors
function Format-Count($n) { return ([long]$n).ToString('#,##0', [System.Globalization.CultureInfo]::InvariantCulture) }
# For a single clean segment the summary text is the fixture's own, byte for byte.
$summaryText = if ($results.Count -eq 1 -and $results[0].Summary) { $results[0].Summary }
               else { "$totalErrors errors out of $(Format-Count $totalTests) tests (union of $($results.Count) segment(s))" }
# Where the union came from — stated so nobody has to reverse-engineer it.
$derivationText = ''
if ($nDerived -gt 0) {
  $derivationText = "$nSummarised segment summary/summaries: $(Format-Count $sumTests) test(s), $sumErrors error(s) · $nDerived ABORTED segment(s): $(Format-Count $derTests) test(s), $derErrors error(s) counted from per-test lines (' Ok' + '! <name> expected:' + 1 — an aborted segment prints no summary)"
  if ($calibration.Count) {
    $derivationText += "  [!! the derivation DISAGREES with sqlite on $($calibration.Count) segment(s) that did report — treat the aborted-segment figures as APPROXIMATE: $($calibration -join '; ')]"
  }
}
$real = @(); $confound = @(); $scopedExcused = @()
foreach ($t in $failNames) {
  $isc = $false
  # Scoped confounds, mirroring build-and-test.sh: `native:<re>` / `emulated:<re>`
  # (bare = every leg). Parsed rather than ignored: without this, a scoped pattern
  # would be treated as the literal regex "emulated:^writecrash-", match nothing,
  # and SILENTLY excuse nothing while appearing configured
  # (D-SQLITE-CONFOUND-LIST-DRIVER-ASYMMETRY).
  # ★ THE SCOPE IS NOW A FACT ABOUT THE LEG, NOT ABOUT THE DRIVER. Until TF-C114
  # this line was the constant 'native', because the driver had exactly one leg
  # and it ran its binary directly. A leg whose resolved run.mode is `launched`
  # executes through a DECLARED launcher (qemu / Wine / `arch -x86_64`), which is
  # precisely what `emulated:` means in the .sh — so it is read off the leg. The
  # DEFAULT is still 'native', which is also what test-confound-scope.ps1 gets
  # when it executes this block with no leg in scope.
  $legMode = if ($LegRunMode -eq 'launched') { 'emulated' } else { 'native' }
  # Strip the suite's declared test-name prefix before matching, so an ^-anchored
  # pattern still recognises `mm-zipfile-25.0` in the `all` tier as the same
  # confound it recognises as `zipfile-25.0` in `full`.
  $tBare = $t
  foreach ($pfx in $TierPrefixes) {
    if ($t.StartsWith($pfx, [System.StringComparison]::Ordinal)) { $tBare = $t.Substring($pfx.Length); break }
  }
  foreach ($p in $Confounds) {
    $pScope = ''; $pRx = $p
    if ($p -like 'native:*')        { $pScope = 'native';   $pRx = $p.Substring(7) }
    elseif ($p -like 'emulated:*')  { $pScope = 'emulated'; $pRx = $p.Substring(9) }
    if ($pScope -and $pScope -ne $legMode) { continue }
    if ($t -match $pRx -or $tBare -match $pRx) { $isc = $true; if ($pScope) { $scopedExcused += $t }; break }
  }
  if ($isc) { $confound += $t } else { $real += $t }
}
if ($scopedExcused.Count) {
  Warn "[$LegTag] $($scopedExcused.Count) failure(s) excused ONLY because this leg runs '$legMode': $($scopedExcused -join ' ')"
}
# Per-unit ledger — every file that reached a verdict, every abort, every gap.
$led = New-Object 'System.Collections.Generic.List[string]'
$led.Add("sqlite unit ledger — tier '$Tier', $($results.Count) segment(s), $resumes resume(s)")
$led.Add("union: $summaryText")
if ($derivationText) { $led.Add("   derived from: $derivationText") }
foreach ($r in $results) {
  $led.Add("")
  $led.Add("== segment: $($r.Label)   rc=$($r.Rc)   $(if ($r.Summary) { $r.Summary } else { 'ABORTED (no summary line)' })")
  $led.Add("   log: $($r.Log)")
  # counts + WHERE THEY CAME FROM, per segment
  if ($r.Summary) {
    $led.Add("   tests: $(Format-Count $r.Tests) / errors: $($r.Errors)   [source: sqlite's own summary line; per-test derivation independently gives $(Format-Count $r.DerivedTests)]")
  } else {
    $led.Add("   tests: $(Format-Count $r.DerivedTests) / errors: $($r.DerivedErrors)   [source: DERIVED from per-test lines — $(Format-Count $r.OkLines) ' Ok' + $($r.FailMarkers) '! expected:' + 1; this segment aborted and printed no summary]")
  }
  $led.Add("   files completed ($($r.Completed.Count)): $($r.Completed -join ' ')")
  if ($r.FailNames.Count) { $led.Add("   failing test(s) seen here ($($r.FailNames.Count)): $(($r.FailNames.Keys | Sort-Object) -join ' ')") }
}
if ($calibration.Count) { $led.Add(""); $led.Add("== derivation calibration MISMATCH =="); foreach ($c in $calibration) { $led.Add("   $c") } }
if ($aborts.Count)     { $led.Add(""); $led.Add("== aborts =="); foreach ($a in $aborts) { $led.Add("   segment $($a.Segment): permutation '$(if ($a.Perm) { $a.Perm } else { '?' })' file '$(if ($a.File) { $a.File } else { '?' })' after test '$($a.LastTest)' ($(if ($a.KillReason) { "KILLED: $($a.KillReason)" } else { "rc=$($a.Rc)" })) -> $($a.Log)") } }
if ($notReached.Count) { $led.Add(""); $led.Add("== NOT REACHED (no verdict) =="); foreach ($n in $notReached) { $led.Add("   $n") } }
if ($hygiene.Count)    { $led.Add(""); $led.Add("== process hygiene =="); foreach ($h in $hygiene) { $led.Add("   $h") } }
if ($TierExcludes.Count) { $led.Add(""); $led.Add("== EXCLUDED by operator (DSS_TIER_EXCLUDES -> QUICKTEST_OMIT) =="); $led.Add("   $($TierExcludes -join ' ')") }
Set-Content -LiteralPath $Ledger -Value $led

$unitVerdict = ''; $unitFail = $false
if ($aborts.Count) {
  # An abort is itself a FAILURE. Resuming recovers the units behind it; it never
  # makes the abort disappear, and a run with aborts is NEVER green.
  $where = @(); foreach ($a in $aborts) { $where += "$(if ($a.Perm) { $a.Perm } else { '?' })/$(if ($a.File) { $a.File } else { '?' })" }
  $unitVerdict = "FAIL: $($aborts.Count) fixture ABORT(s) [$($where -join ' ')]; recovered by $resumes resume(s); union: $summaryText"
  if ($derivationText) { $unitVerdict += " [$derivationText]" }
  if ($real.Count)      { $unitVerdict += "; $($real.Count) genuine unit failure(s): $($real -join ' ')" }
  if ($notReached.Count) { $unitVerdict += "; $($notReached.Count) unit group(s) NOT REACHED — see $Ledger" }
  $unitFail = $true
  Warn "[$LegTag] corpus FAIL — $($aborts.Count) abort(s): $($where -join ' ')"
  Info "      union across $($results.Count) segment(s): $summaryText; $filesDone test file(s) completed"
  if ($derivationText) { Info "        derived from: $derivationText" }
  if ($real.Count) { Info "      $($real.Count) GENUINE DSS failure(s): $($real -join ' ')" }
  foreach ($n in $notReached) { Warn "      NOT REACHED: $n" }
  Info "      per-unit ledger: $Ledger"
} elseif (-not $results[0].Summary) {
  $unitVerdict = "FAIL: fixture did not complete the suite (crash?) — see $runlog"; $unitFail = $true
  Warn "[$LegTag] corpus FAIL — no summary line (fixture crashed mid-suite); tail:"
  Get-Content $runlog -Tail 6 | ForEach-Object { Info "      $_" }
} elseif ($totalErrors -gt 0 -and $failNames.Count -eq 0) {
  $unitVerdict = "FAIL: $totalErrors error(s) but no failure markers ('Failures on these tests:' / '! <name>') to classify — see $runlog"; $unitFail = $true
  Warn "[$LegTag] corpus FAIL — $summaryText (unclassifiable — no failure markers)"
} elseif ($real.Count -eq 0) {
  $unitVerdict = "PASS ($summaryText$(if ($confound.Count) { "; $($confound.Count) known confound(s): $($confound -join ' ')" }))"
  Pass "[$LegTag] corpus GREEN — $summaryText$(if ($confound.Count) { "; all $($confound.Count) failure(s) are known non-DSS confounds: $($confound -join ' ')" })"
} else {
  $unitVerdict = "FAIL: $($real.Count) genuine unit failure(s): $($real -join ' ')"; $unitFail = $true
  Warn "[$LegTag] corpus FAIL — $summaryText; $($real.Count) GENUINE DSS failure(s): $($real -join ' ')"
  if ($confound.Count) { Info "      (+$($confound.Count) known confound(s) ignored: $($confound -join ' '))" }
}
# A killed zombie / stolen stale lock is a fact about THIS run, not a footnote — it
# rides on the verdict even when the corpus itself came back clean.
if ($hygiene.Count) {
  $unitVerdict += "  [PROCESS HYGIENE: $($hygiene.Count) event(s) — $($hygiene -join '; ')]"
  foreach ($h in $hygiene) { Warn "[$LegTag] HYGIENE: $h" }
}
# A NOT-REACHED unit is a coverage hole even when nothing failed — never silent.
if ($notReached.Count -and -not $aborts.Count) {
  $unitVerdict += "  [NOT FULL COVERAGE: $($notReached.Count) unit group(s) NOT REACHED — see $Ledger]"
  $unitFail = $true
  foreach ($n in $notReached) { Warn "[$LegTag] NOT REACHED: $n" }
}
# The exclusion rides along on EVERY verdict — pass and fail alike — so a GREEN
# line can never be read as "the whole corpus ran".
if ($TierExcludes.Count) {
  $unitVerdict += "  [NOT FULL COVERAGE: $($TierExcludes.Count) file pattern(s) EXCLUDED from the $Tier tier via QUICKTEST_OMIT -- $($TierExcludes -join ' ')]"
  Warn "[$LegTag] the verdict above covers a REDUCED corpus: $($TierExcludes -join ' ') excluded from every `$allquicktests-derived permutation."
}
# <<< dss:corpus-loop <<<

# ── this leg's row in the ledger ─────────────────────────────────────────────
# The VERDICT is `ran`: the leg produced an assertion, which is exactly what the
# closed vocabulary means by verified. Whether that assertion PASSED is a
# separate fact ($unitFail), carried separately and folded into the exit code by
# Step 9 — the same separation ArmVerdictLedger draws between a verdict and a
# test result. A leg that ran and failed is `1 verified … 0 poisoned` plus a
# non-zero exit, not a vanished leg.
$legRec.Verdict     = 'ran'
$legRec.Detail      = "$LegRunMode$(if ($legLauncher.Count) { " via '$($legLauncher -join ' ')'" }) — $unitVerdict"
$legRec.UnitVerdict = $unitVerdict
$legRec.UnitFail    = $unitFail
$legRec.Ledger      = $Ledger
# The per-leg detail Step 9 reprints. Only populated when there is something to
# say, so a clean single-segment run leaves the summary byte-identical to what it
# always was.
$rep = New-Object 'System.Collections.Generic.List[string]'
if ($results.Count -gt 1 -or $aborts.Count -or $notReached.Count -or $hygiene.Count) {
  [void]$rep.Add("segments : $($results.Count) ($resumes resume(s) of max $MaxResumes)   $filesDone test file(s) completed   ledger: $Ledger")
  if ($derivationText) { [void]$rep.Add("counts   : $derivationText") }
  foreach ($a in $aborts) { [void]$rep.Add("aborted  : permutation '$(if ($a.Perm) { $a.Perm } else { '?' })' file '$(if ($a.File) { $a.File } else { '?' })'$(if ($a.KillReason) { " [KILLED: $($a.KillReason)]" }) — its remaining cases did NOT run  ($($a.Log))") }
  foreach ($n in $notReached) { [void]$rep.Add("NOT RUN  : $n") }
  foreach ($h in $hygiene)    { [void]$rep.Add("hygiene  : $h") }
}
$legRec.Report = $rep
}
# ── end of the per-leg run loop ──────────────────────────────────────────────

# ── Step 9 — results ─────────────────────────────────────────────────────────
Step '9/9  Results'
# $dssDivergeNote is the Step-2 measurement, reprinted verbatim so the opening
# banner and the closing verdict cannot disagree about the same run.
Info "compiler : $DssBin @ $dssHead$dssDivergeNote"
Info "sqlite   : $SqliteWslDir @ $sqliteHead   (staged: $Stage)"
Info "recipe   : $nTus TUs, $nDefs defines"
# The ATTRIBUTION ORACLE, surfaced where a human triaging a failure will actually
# see it. Its ABSENCE is printed too, and loudly: a missing oracle is the difference
# between attributing a corpus failure and arguing about it, so it must never be
# silent. Step 3+4 preserves this copy OUT of the make target's path — see "the
# reference build, and the PRESERVED oracle" there.
# ★ THE ONE-SIDEDNESS IS PART OF THE VERDICT, not a footnote. This fixture is a
#   LINUX/gcc ELF built by the deriving host's own toolchain. Against a leg of a
#   DIFFERENT platform (pe64, mach-o) it can EXONERATE (it fails too → upstream,
#   not DSS) but it can NEVER convict (it passes → could still be a platform/CRT
#   difference; fpconv1-2.0 is the standing example). Do not "tidy" that sentence
#   away — a reader who takes an oracle pass as proof of a DSS bug will chase a
#   defect that is not there.
#   ⚠ AND NOTE THE ASYMMETRY BETWEEN LEGS, which is new as of TF-C114: for an
#   elf64 Linux leg this same binary is very nearly a MATCHED control (same OS,
#   same object format, differing only in the compiler), i.e. a much stronger
#   instrument than it is for pe64 or mach-o. Neither claim has been MEASURED on
#   this driver, so the line below states what the oracle IS and leaves the
#   strength of the inference to the leg being triaged.
if ($RefOracle) {
  Info "oracle   : $RefOracleWin"
  Info "             LINUX/gcc reference testfixture (ELF, built by the deriving host). Run it on"
  Info "             the same .test to EXONERATE dss:   $(if ($script:HostNeedsWsl) { "wsl.exe -- $RefOracle" } else { $RefOracle }) <staged .test>"
  Info "             It fails too => upstream/test-suite. It passes => INCONCLUSIVE for a leg of a"
  Info "             different platform (pe64 / mach-o); never proof of a dss bug on its own."
} else {
  Warn "oracle   : ABSENT — no reference fixture survived this run, so a corpus failure"
  Warn "             cannot be attributed to DSS vs upstream. $RefOracleMiss"
}
Info "tier     : $Tier.test   outputs: $Work"
Info "excluded : $(if ($TierExcludes.Count) { "$($TierExcludes -join ' ')   (operator DSS_TIER_EXCLUDES -> QUICKTEST_OMIT; dropped from every `$allquicktests-derived permutation, still run under 'full')" } else { '(none — the full tier ran)' })"

# ── the LEG LEDGER — every DECLARED leg, one row, a NAMED verdict ────────────
# ★ THE ACCOUNTING IS THE DELIVERABLE. This is
# ArmVerdictLedger::renderCountsLine()'s idiom, in its vocabulary, for the same
# reason it exists there: "N skipped" collapses four different facts into one and
# turns a gate back into a log. Every class is named, every declared leg is
# counted, and if the classes do not SUM to the declared total the line says so
# ABOUT ITSELF rather than quietly printing a breakdown that does not add up.
$vRan = 0; $vExpect = 0; $vByRunOn = 0; $vNoEmu = 0; $vEmuMissing = 0
$vInputMissing = 0; $vNotSelected = 0; $vPoisoned = 0; $vUnclassified = @()
foreach ($lbl in $LegOrder) {
  switch ("$($LegLedger[$lbl].Verdict)") {
    'ran'                          { $vRan++ }
    'expect-error-asserted'        { $vExpect++ }
    'skipped-by-runOn'             { $vByRunOn++ }
    'skipped-no-emulator-declared' { $vNoEmu++ }
    'skipped-emulator-missing'     { $vEmuMissing++ }
    'skipped-build-input-missing'  { $vInputMissing++ }
    'not-selected-by-runner'       { $vNotSelected++ }
    'poisoned'                     { $vPoisoned++ }
    default                        { $vUnclassified += $lbl }
  }
}
$verified   = $vRan + $vExpect
$structural = $vByRunOn + $vNoEmu
$environmental = $vEmuMissing + $vInputMissing
$skipped    = $structural + $environmental + $vNotSelected
$accounted  = $verified + $skipped + $vPoisoned
$countsLine = "$verified verified ($vRan ran, $vExpect expect-error), $skipped skipped [structural: $vByRunOn by-runOn, $vNoEmu no-emulator-declared; environmental: $vEmuMissing emulator-missing, $vInputMissing build-input-missing; harness: $vNotSelected not-selected], $vPoisoned poisoned  (of $($AllLegs.Count) declared legs)"
if ($accounted -ne $AllLegs.Count) {
  # The runtime twin of renderCountsLine()'s accounting-hole branch: a breakdown
  # that does not sum to its own denominator reads like full accounting and is
  # worse than none. It announces itself here rather than waiting to be noticed.
  $countsLine += "  ★ LEDGER ACCOUNTING HOLE: $accounted of $($AllLegs.Count) legs fall in a reported class — the rest belong to NO class and have VANISHED from this line$(if ($vUnclassified.Count) { " (unclassified: $($vUnclassified -join ' '))" }) (D-TEST-CROSS-ARCH-SKIP-YIELDS-NO-VERDICT)"
}
Info "legs     : $countsLine"
# One detail line per NON-VERIFIED leg, with its reason. Verified legs are omitted
# from THIS list — they are already accounted for above and reported in full below.
foreach ($lbl in $LegOrder) {
  $r = $LegLedger[$lbl]
  if ($r.Verdict -eq 'ran' -or $r.Verdict -eq 'expect-error-asserted') { continue }
  Info "  [$($r.Verdict)] $lbl spec=$($r.Spec)$(if ($r.Built) { ' (BUILT)' }) — $($r.Detail)"
}
# …and the full report for every leg that DID run.
foreach ($lbl in $LegOrder) {
  $r = $LegLedger[$lbl]
  if ($r.Verdict -ne 'ran') { continue }
  foreach ($ln in @($r.Report)) { Info "  [$lbl] $ln" }
  Info "$($lbl) ($($r.Spec)): compiled   units: $($r.UnitVerdict)"
}
if ($FilteredOut.Count) {
  Warn "coverage : DSS_LEGS restricted this run to $($Legs.Count) of $($AllLegs.Count) declared legs — NOT EXERCISED: $($FilteredOut -join ' ')"
}
# Release the run lock. Correctness does NOT depend on this — the lock is
# liveness-based, so a run that dies here just leaves one the next invocation
# steals and reports. Releasing simply keeps that report quiet when it should be.
Remove-Item -Recurse -Force $LockDir -ErrorAction SilentlyContinue

# ── the exit code ────────────────────────────────────────────────────────────
$failReasons = @()
if ($vPoisoned -gt 0) {
  $failReasons += "$vPoisoned leg(s) POISONED: $(@($LegOrder | Where-Object { $LegLedger[$_].Verdict -eq 'poisoned' }) -join ' ')"
}
$unitFailLegs = @($LegOrder | Where-Object { $LegLedger[$_].UnitFail })
if ($unitFailLegs.Count) { $failReasons += "$($unitFailLegs.Count) leg(s) with genuine unit failures: $($unitFailLegs -join ' ')" }
if ($accounted -ne $AllLegs.Count) { $failReasons += "the leg ledger does not account for every declared leg ($accounted of $($AllLegs.Count))" }
# STRUCTURAL skips are reported, NEVER fatal — nothing about this machine could
# change them. ENVIRONMENTAL skips warn by default and are FATAL under
# DSS_STRICT_ARM_VERDICTS=1, exactly as arm_verdict_ledger.hpp specifies: a
# developer without the target's tcl runtime must still get a usable run, while
# the gate opts in to demanding every declared input be present.
$envSkipLegs = @($LegOrder | Where-Object { $LegLedger[$_].Verdict -eq 'skipped-emulator-missing' -or $LegLedger[$_].Verdict -eq 'skipped-build-input-missing' })
if ($envSkipLegs.Count) {
  if ($StrictVerdicts) {
    $failReasons += "DSS_STRICT_ARM_VERDICTS=1 and $($envSkipLegs.Count) leg(s) were skipped for an ENVIRONMENTAL reason: $($envSkipLegs -join ' ')"
  } else {
    Warn "$($envSkipLegs.Count) leg(s) skipped for an ENVIRONMENTAL reason (this machine could supply what the catalogue declared, and did not): $($envSkipLegs -join ' ')"
    Warn "      set DSS_STRICT_ARM_VERDICTS=1 to make that a hard failure. It is NOT fatal by default so a developer missing one target's runtime still gets a usable run."
  }
}
# ★ ZERO VERIFIED + AT LEAST ONE ENVIRONMENTAL SKIP IS A FAILURE, and this rule is
# what carries the pre-TF-C114 loudness across the generalisation. Before this
# cycle a missing tcl86.dll was a hard `Die`; with five legs and a per-leg
# `skipped-build-input-missing` it would otherwise have become "0 verified, 5
# skipped, exit 0" — a broken machine reporting success, which is precisely the
# "449 of 557 silently skipped and counted as passes" defect arm_verdict_ledger.hpp
# was written for. The environmental clause is what keeps it honest in the other
# direction: on a host where every non-verification is STRUCTURAL (a riscv64 Linux
# box, say) nothing about the machine could have done better, the legs whose
# inputs WERE present are still built, and that is not a failure.
if ($verified -eq 0) {
  if ($environmental -gt 0) {
    $failReasons += "NO declared leg reached a VERIFIED verdict, and $environmental non-verification(s) are ENVIRONMENTAL — this machine could have produced evidence and did not. A run that proves nothing must not exit 0"
  } else {
    Warn "no declared leg reached a VERIFIED verdict on this host — every non-verification is STRUCTURAL (nothing about this machine could change it). Builds were still attempted; this run proves nothing about EXECUTION on any target."
  }
}
if ($failReasons.Count) {
  Write-Host ""
  foreach ($f in $failReasons) { Write-Host " [X] $f" -ForegroundColor Red }
  Write-Host " [X] sqlite harness FAILED — $countsLine" -ForegroundColor Red
  exit 1
}
Pass "sqlite harness GREEN — $countsLine"
Pass "$verified leg(s) compiled the full-source testfixture + ran the $Tier unit corpus GREEN"
exit 0
