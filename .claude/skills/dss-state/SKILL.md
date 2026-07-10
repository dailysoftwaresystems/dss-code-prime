---
name: dss-state
description: Measure the current state of the DSS Code Prime compiler. Runs a 104-probe C-feature battery through the real dss-code-prime CLI and reports every % axis — empirical C coverage, plan-23 full-C arc %, SQLite-readiness %, test suite, deferred-anchor closure, cross-target emit/run matrix — plus a velocity-based ETA for "compiles SQLite" and the full C23 arc. Use when asked how far along the compiler is, for a progress/state/status report, % complete, an ETA for full C or SQLite, or to re-measure after a /dss-cycle.
---

Computes a live state-of-the-compiler report by **driving the real CLI**: every probe in
[probes.mjs](.claude/skills/dss-state/probes.mjs) is compiled by `dss-code-prime --compile`
for the host target and the produced binary is **executed** — so the percentages measure the
whole pipeline (parse → semantic → IR → codegen → link → runtime), not the grammar. Static
axes (plan-23 phase board, anchor registry, ctest log, git) are parsed from the repo.

All paths below are relative to the repo root.

## Prerequisites

- Node ≥ 18 (verified with v22.12) and git on PATH — nothing else; the driver has zero deps.
- A built CLI. The driver auto-picks the **newest** of `build/bin/dss/{Debug,Release}/`,
  `build-rel/bin/dss/`, `build-dbg/bin/dss/`. To (re)build the MSVC Debug one:

```powershell
cmake --build build --config Debug --target dss-code-prime
```

- Optional (Windows): WSL gives the ELF runtime witnesses. This box has `qemu-aarch64` 8.2.2
  and the `/usr/aarch64-linux-gnu` sysroot inside WSL, which the arm64 leg needs (see Gotchas).

## Run (agent path)

```powershell
node .claude/skills/dss-state/driver.mjs
```

Full report to stdout in ~20–60 s (battery itself is ~2–7 s; WSL legs dominate). Variants:

```powershell
node .claude/skills/dss-state/driver.mjs --quick                      # skip WSL → ~10 s
node .claude/skills/dss-state/driver.mjs --ctest                      # live ctest run instead of the cached log (+ ~2 min)
node .claude/skills/dss-state/driver.mjs --json out.json --md out.md  # also write machine/markdown copies
```

| flag | effect |
|---|---|
| `--quick` / `--no-wsl` | skip WSL/qemu runtime legs (matrix shows emit-only) |
| `--ctest` | run the full ctest suite live (default: parse the cached `build/Testing/Temporary/LastTest.log`, timestamped in the report) |
| `--json <p>` / `--md <p>` | write the structured dump / the markdown report |
| `--cli <exe>` | use a specific compiler binary instead of newest-found |
| `--jobs N` | probe concurrency (default min(8, cores)) |
| `--keep` | keep the temp probe dirs (`%TEMP%/dss-state-<pid>/`) for inspection |
| `--strict` | exit 1 if any miscompile/crash/toolfail was detected |

## Reading the report

- **🔴 MISCOMPILES** is the highest-priority section: rc=0 compile but WRONG runtime result —
  the silent-miscompile class this repo treats as critical. (The battery caught a real one on
  its first run: `ptr_swap_through` — see Gotchas.)
- **Funnel**: each rejected probe is classified by the first letter of its diagnostic codes
  (`P_*` parse, `S_*` semantic, `H/I_*` IR, `L/A/E_*` backend) — so you see *where* each
  feature dies, not just that it fails.
- **ETA model**: remaining-cycle estimates per FC phase live in `PHASE_CYCLE_EST`
  (driver.mjs, calibrated on FC1–FC4 actuals); velocity = completed-phase cycles / calendar
  days since `PLAN_ARC_START`, with a `SUSTAIN_FACTOR` haircut for the steady scenario.
  SQLite milestone = cheapest of two routes (Win64-PE vs SysV-ELF prerequisite phase sets).
  All three scenario dates + the math are printed; treat them as extrapolations.
- If plan-23 gains/renames FC phases the driver **fails loud** (`PLAN DRIFT` row / fatal) —
  update `PHASE_CYCLE_EST` and the `SQLITE_PATHS` sets to match.

## Extending the battery

Add probes to [probes.mjs](.claude/skills/dss-state/probes.mjs) — `{ id, cat, expect, src }`
(+ optional `stdout`, multi-TU `files`). Style rules are at the top of that file; the two that
matter: declare-then-assign (init-at-decl has its own probe), and never use Bool-arithmetic
(`(a<b)*20` hits a known semantic gap) — accumulate via `if`. New probes need no driver change.

## Test

```powershell
node .claude/skills/dss-state/driver.mjs --quick
```

Expect: a `battery of 104` funnel line, the category table, and exit 0. (`--strict` also
exits 0 since 2026-06-12 — the `ptr_swap_through` miscompile is FIXED, see below.)

## Gotchas

- **Artifacts are named after the FIRST source file's stem** (`--output` dir + `main.exe`/`main`).
  Every probe writes its source as `main.c` for this reason; a `canonical.c` once produced
  `canonical.exe` and the matrix read 0/5 emit. Keep battery sources named `main.c`.
- **The exe is a thin main over `dss-code-prime.dll`** — rebuilds usually relink only the DLL,
  so the staleness check compares the newest of exe+DLL against on-disk `src/**/*.{cpp,hpp,…}`
  (commit timestamps skew by minutes; `.json` configs are runtime-loaded and never stale it).
- **DSS ELF executables are dynamic** (every program imports libc `exit`), so bare
  `qemu-aarch64` fails with `Could not open '/lib/ld-linux-aarch64.so.1'`. The driver passes
  `-L /usr/aarch64-linux-gnu`; without qemu or that sysroot in WSL the leg reports
  `skipped (wsl-qemu runner unavailable)` — install qemu-user + an aarch64 sysroot
  (`gcc-aarch64-linux-gnu` provides it) to light it up.
- **Per-format features flip green per HOST (TLS arc example).** The battery compiles probes
  for the HOST-NATIVE target — pe64 on Windows, elf64 on Linux. A feature landed per-format
  (thread_local: ELF x86_64 in TLS C1; PE lands C3; Mach-O C4) therefore shows
  `c11_thread_local` GREEN on a Linux host but 0x8015-RED on the Windows host until its C3
  leg lands — the red is CORRECT (fail-loud on the un-landed leg), not a regression. Judge
  such probes on the leg that owns them.
- **Git velocity is squash-blind**: PRs squash to one main commit, hiding per-cycle commits.
  The plan board is the primary velocity source; the `^v0.0.2` commit count is shown only as
  a floor.
- **Multiple build dirs — the ctest axis follows the CLI's dir, not `build/`.** This box has
  both an MSVC `build/` (often STALE/broken — `semantic_analyzer.cpp` overflows COFF sections
  without `/bigobj`) and a healthy Ninja `build-dbg/`. The driver auto-picks the newest CLI
  (usually `build-dbg/`) and now derives the ctest dir + `LastTest.log` from that same path
  (`buildInfoOf()`), so a single-config Ninja dir gets no `-C`. Before this fix the test axis
  hardcoded `build/` and reported garbage (a `2/2` partial log; a `145/448` from the broken
  MSVC dir) while the real suite was `448/448` in `build-dbg`. To rebuild the active one:
  `cmake --build build-dbg --target dss-code-prime` (Ninja, single-config — no `--config`).
- **The `ptr_swap_through` miscompile is FIXED (2026-06-12)**: the probe's exit-34 was NOT a
  pointer bug — the swap compiled perfectly; `return x - y + 4;` parsed RIGHT-associative
  (`x - (y + 4)`) because same-precedence infix chains nested rightward in the Pratt walker
  (`D-PARSE-INFIX-ASSOCIATIVITY-STRUCTURAL` ✅, registry). The probe now passes, MISCOMPILES
  is empty, and `--strict` exits 0. Lesson for probe triage: a miscompile probe's NAME frames
  a hypothesis (pointers) — the disassembly, not the name, localizes the defect.
- **PowerShell quirk while probing by hand**: piping the CLI through `Select-Object -First N`
  can kill it before it exits — `$LASTEXITCODE` comes back empty. The driver is unaffected
  (Node captures properly); just don't trust `rc=` after a truncated pipe.

## Troubleshooting

- **`fatal: no dss-code-prime binary found`** — build one:
  `cmake --build build --config Debug --target dss-code-prime` (MSVC multi-config: the
  `--config Debug` matters; plain `cmake --build build` builds no config).
- **`fatal: plan-23 parse found only N FC rows`** — plan-23's §0.1 table format changed;
  fix `parsePlan23()` + recalibrate `PHASE_CYCLE_EST`.
- **Matrix row `WRONG exit …`** — that's a real cross-target regression, not a harness issue;
  the same canonical program just ran correctly elsewhere.
- **Test-suite axis says `no data`** — no `build/Testing/Temporary/LastTest.log` yet; run with
  `--ctest` once.
