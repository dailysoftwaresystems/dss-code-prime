# DSS Code Prime — HANDOFF

> **REWRITTEN at the end of every cycle** (`/dss-cycle` Step 8.1) and **READ FIRST at the start of
> every cycle** (Step 0). §1–§4 are a *replacement* — stale lines are deleted, not appended past.
> **§5 TIMELINE is the sole exception and accumulates.** State is what is true now; the timeline is
> how it got here.
>
> Every claim is labelled ✔**MEASURED** / 📄**DOCUMENTED** / 🧠**INFERRED**. An unlabelled claim here
> is a defect: this file is read by someone with no context, which is exactly when an unmarked
> inference does the most damage.

**Last updated:** 2026-08-13 · **Branch:** `feature/c23-conformance-burndown-2` · **PR #51**
**HEAD:** `730e642a` ✔MEASURED · in sync with origin (0/0)
⚠ HEAD MOVED MID-CYCLE — `730e642a` (the skills refactor) landed on top of `75ca4034` from a
concurrent session in this same repo. It touched only `.claude/`, so the anchor baseline is
unaffected — ✔verified, not assumed.
✅ **STATUS: GATE GREEN ON ALL THREE LEGS — the first cycle in four to manage it.**
✔ Windows ctest **851/851**, 0 failed · ✔ WSL x86_64 ctest **851/851**, 0 failed ·
✔ arm64 under qemu **594/594** with `DSS_STRICT_ARM_VERDICTS=1` and **zero skips**.
✔ Anchor guard OK (**1019** citations, 0 cell violations) · ✔ balance **983 → 983, net 0**.
⚠ **`e42ae5a5`'s commit message says `1018` and that figure is STALE — do not re-quote it.** ✔MEASURED
2026-08-13 after the push: the guard reports **1019** on the committed tree and **989** on its parent
`730e642a`, so 1018 was true of neither endpoint — it was measured mid-cycle and carried into the
message unrefreshed. No verdict moves (the guard printed OK at both points; the legs and the balance
are untouched); a quoted number was wrong, not a result. Anchored on
`D-PLANS-CLOSEOUT-GREEN-FIGURE-LEG-UNQUALIFIED`, which now demands **freshness as well as scope**.
★ The arm64 leg was proven, not assumed: with strict OFF a missing emulator is a WARNING and the suite still passes, so a green run alone would have been a partial run rounded up. The guard itself was exercised — qemu hidden with strict ON ⇒ 4/4 FAIL.

---

## 1. WHERE WE ARE

### The headline capability
**All five legs** — `elf64-{x86_64,arm64}`, `pe64-x86_64`, `macho64-{arm64,x86_64}` — **build and
execute** both the sqlite3 CLI and the unit corpus, zero DSS-attributable failures.
✔MEASURED 2026-08-11 at `0ecec160`, upstream sqlite `d9de6eedbe`: BUILD 5/5; **2 legs proven by
execution** (macho64-arm64 `14/14` + 2 errors / 394,050 units; Rosetta `14/14` + 2 / 394,046).

⚠ **The round trip is NOT proven.** Table 2 is built-on × runs-on — 20 build cells × 2 artifacts —
and only ~5 cells are witnessed **by execution**. "Builds" and "runs" are different claims.

### What this cycle changed (gate green on all three legs)
- **Assembly gained interior labels end-to-end** — a `.s` jump table runs on all three legs, exit
  42, both configs ✔MEASURED, discrimination proven (arm64 slot `#0`→10, `#8`→42, `#16`→20).
  Required **zero** encoder/linker/relocation-kind change: the capability was already built.
- **Two silent pe64 unwind miscompiles closed.** `SizeOfProlog` under-reported by one byte per FPR
  spill (assumed 9, ✔MEASURED 10); every VLA function shipped `FrameRegister = 0` with no
  `UWOP_SET_FPREG`. Root class: the builder re-derived lengths the assembler had already measured.
- **A live cross-language link failure fixed at source** — one label's address taken twice failed
  to link, and ✔MEASURED **C had it identically** (`void *a = &&L; void *b = &&L;`).
- **DWARF CFI representation + encoder** built, witnessed by `readelf --debug-dump=frames`.
- **`.section` / `.space` fill byte** closed with runtime witnesses on both dialects, all 3 legs.
- **Config loaders hardened** — 16 target-loader containers + `numberStyle` gained unknown-key
  gates via one shared rejector.

### What is NOT known / NOT run — read before trusting anything above
- ✅ **The three-cycle Windows-only streak is broken** — all three legs ran and are green (see
  the status block). The two cycles before this one shipped on Windows alone; keeping that from
  becoming the norm is why the arm64 leg was re-run under `DSS_STRICT_ARM_VERDICTS=1` rather than
  accepted on a default that treats a missing emulator as a warning.
- ✔`.eh_frame` **now lands on ELF and Mach-O executables** (this line previously read "zero images").
  Witnessed by **gdb unwinding 4 DSS frames** on a build with no frame pointers, so `.eh_frame` is
  the only thing that could have produced it. **Relocatable ELF `.o` also round-trips** through
  system gcc — 9 frames vs **2** for an `objcopy`-stripped control. COFF `.obj` and Mach-O
  `MH_OBJECT` still carry none: [[D-UNWIND-NO-EH-FRAME-IN-RELOCATABLE-OBJECTS]].
- ⚠⚠ **TWO independent mechanisms found this cycle make a red-on-disable report GREEN over a LIVE
  mutant** — one where the mutant was never COMPILED IN (`ninja -t deps` = `#deps 0`, **10 of 403**
  objects) and one where it was never READ (cwd-walk config resolution). In both, every fail-closed
  clause was satisfied. ⇒ treat any green red-on-disable from this tree as UNPROVEN unless the
  mutant was shown to have been read.

### Instrument health — FIVE broken instruments found this cycle; assume more
- ✔ Anchor guard resolves **truncated** citations by substring: **91 line-wrapped `D-*` names
  across 48 files** pass silently → `D-GATE-ANCHOR-GUARD-RESOLVES-TRUNCATED-CITATIONS-BY-PREFIX`.
- ✔ Registry **line-number** citations rot silently. A stale path fails loudly when grepped; a
  stale line number still resolves, to the wrong code. 14 repointed.
- ✔ **Three counts written from memory this cycle all erred LOW** (ScopedEnv copies said 3, measured
  5; JSON-leaking headers said 2, measured 3; the anchor count twice before).
- ✔ `test_asm_text_to_lir`'s fixture **replaces** `assembly.directives`, so the whole suite was
  green regardless of what the shipped dialect documents declare.
- ✔ The anchor-BALANCE gate could not classify this handoff’s own prose tables and correctly
  REFUSED to report a number rather than skip them. Taught the three header shapes **by shape,
  not by filename** — excluding the file would have created the silent skip the gate exists to stop.
- ✔ A shell `grep -c $'\r'` reported 63 CR-lines in files that Python measured at **zero** — the
  pattern had degenerated and was counting every line. Nearly became a reported finding.

---

## 2. WHERE WE NEED TO GET

| Destination | The named gap |
|---|---|
| **sqlite round trip proven by execution** | ~15 of 20 build cells never *run*. Needs execution legs, not more building. |
| **Unwind info on all 5 formats** | ✔Executables: pe64 + ELF + Mach-O all land, and ELF `.o` round-trips through system gcc. Remaining: COFF `.obj` (effort; MSVC reference captured) and Mach-O `MH_OBJECT` (**blocked** — no clang on this host to measure the reference). |
| **Assembly reaches real `gcc -S` output** | `leaq X(%rip)` unreachable — no target declares `rip`. **OPERATOR DECISION.** |
| **FC18 — `D-DIAG-CORPUS-EVERY-CODE`** | Sole remaining C23 conformance phase. New PR. |
| **Any target inside any host** | `D-HARNESS-CROSS-HOST-ANY-TARGET` stays OPEN — attempting ≠ producing. Blocker: `D-HARNESS-MACHO-LEG-INPUTS-UNOBTAINABLE-OFF-MAC`. |
| **A strict linker** | `D-LINK-EXEC-UNDEFINED-SYMBOL-FAIL-LOUD`: rc=0 on an undefined EXEC symbol → runtime exit-127, not a link error. |

---

## 3. PRIORITIES

1. **`NEXT` — The assembly `.cfi_*` producer.** The 18 `.cfi_*` spellings are still accepted and dropped, so a `.s` compiles, runs correctly, and cannot be unwound. Its blocker (`D-TARGET-NO-DWARF-REGISTER-NUMBERING`) **closed this cycle**, so it is now unblocked work rather than a deferral. ⚠ `cfi_escape` must stay a **REFUSAL**, not an annotation — accepting opaque bytes would re-create this row’s own defect inside its fix.
   ★ **Keep running all three legs.** This cycle broke a three-cycle streak of Windows-only gates; the streak reforms the moment one is skipped “just once”.
2. **`NEXT` — Close the residue this cycle opened.** Standing operator order: a cycle's own opens
   are the next cycle's mandate, not backlog.
3. **`NEXT` — COFF `.obj` unwind tables.** ELF `.o` round-trips through system gcc; COFF is **effort, not knowledge**, and the reference is already captured: `dumpbin /relocations` shows `.pdata` carrying 3 `ADDR32NB` (Begin/End/UnwindInfo) and `.xdata` one, and ★ MSVC uses ORDINARY NAMED SYMBOLS (`$LN3`, `$unwind$<fn>`) not aux section symbols — so the aux-symbol subsystem assumed necessary is avoidable. ⚠ Mach-O `MH_OBJECT` is **blocked**: no clang on this host to measure the reference, and its writer has no `r_extern = 0` path.
4. **`OPERATOR DECISION` ×3** — none are scope calls; each has a measured cost:
   - `D-ASM-RIP-RELATIVE-SPELLING-NEEDS-AN-IP-REGISTER` — declaring `rip` a `gpr` hands the
     instruction pointer to regalloc.
   - `D-ASM-ADDRESS-OPERAND-CANNOT-NAME-AN-UNDEFINED-SYMBOL` — `isData` picks GOT vs PLT and gas
     has no code-vs-data directive, so a default has a **wire-format** consequence.
   - `D-LSP-TARGET-SPEC-SPLITTER-LIVES-ABOVE-ITS-CONSUMERS` — a type **split** across ~25 assertion
     sites. Splitting a type to fix a layer is a design call.
5. **`QUEUED` — the 91 wrapped anchor citations.** Must land atomically with tightening the guard;
   tightening alone turns 91 sites red.
6. **`QUEUED`** — binary rename → `dsscp` · CI + pkg-publish INERT (PR #45) · public repo (PR #37) ·
   the "byte-identical vs GCC" overclaim in `pitch.txt`.

### Two anchors that must NOT be closed — closing them would itself break the bar
- `D-ASM-TARGET-DECLARES-NO-BYTE-ORDER` — no big-endian target exists to key the facet from.
- `D-ASM-COND-ON-TERMINATOR-ARMS-UNWITNESSED` — no shipped target declares `condCodeFromPayload`
  on a return or branch-with-link.

📄 Both trigger-gated. Building either is the speculative build §A.2 forbids *in the other
direction*. Bring as a §B decision; never close one to improve a number.

---

## 4. CONCURRENT BRANCHES / PRs — the rebase-conflict surface

✔MEASURED 2026-08-13 via `gh pr list --state open`. **Other sessions work other branches on this
same repo and cannot see this cycle.** This section is the only channel between them.

| PR | Branch | What it is doing | Last update |
|---|---|---|---|
| **#51** | `feature/c23-conformance-burndown-2` | ⬅ **THIS BRANCH.** Assembly-as-a-language burn-down; DSS Axis + HIR plan rework. | 2026-08-13 13:49Z |
| **#52** | `ap5-build-hooks-and-dependson-surface` | **THE CONCURRENT SESSION.** AP5: build-lifecycle hooks, the `dependsOn` surface, the composition-verb table. **59 files.** | 2026-08-13 14:19Z — *newer than ours* |

### ⚠ Measured overlap with PR #52 — these files are touched by BOTH
Named individually, because "59 files" tells a rebaser nothing:

- `.plans/_deferred-anchor-registry.md` ← **the hottest file in the repo.** Every session edits it
  every cycle. Expect a conflict here on every rebase; resolve by **keeping both sets of rows**
  (never delete a row — the audit trail is load-bearing) and re-running
  `python tools/check-anchor-balance.py` afterwards.
- `.plans/00-compiler-implementation-plan - tbd.md`
- `.plans/06-artifact-profile-plan - tbd.md` ← this cycle repointed `project_config` paths in it
- `src/core/CMakeLists.txt` ← this cycle rewrote the nlohmann convention comment
- `src/core/types/parse_diagnostic.{hpp,cpp}` ← ⚠ **slot-table collision risk, see below**
- `src/link/format/macho.cpp` ← a lane is wiring `.eh_frame` into it *right now*
- `src/link/object_format_schema.hpp` · `src/program/CMakeLists.txt` · `integrated_tests/runner.cpp`

### ⚠ Cross-branch resources that git merges cleanly and WRONGLY
- **Diagnostic-code slots** (`parse_diagnostic.hpp`). This cycle consumed **`K-NEXT-SLOT: 0x8021`**
  (`K_UnwindRuleUnrepresentable`). Two sessions taking the same slot produce a clean merge and two
  diagnostics with one number.
- **Anchor names.** Two sessions minting the same `D-*` name merge cleanly into one duplicated,
  double-counted row. ✔This cycle already reconciled two such duplicate pairs.

### 📄 The mitigation, restated because it is the whole defence
**Stage by explicit path — NEVER `git add -A`** (`D-CYCLE-CANNOT-ASSUME-IT-OWNS-THE-WORKING-TREE`).
A concurrent workstream's edits can be sitting in this very working tree.
📄 **DCO:** every commit needs `Signed-off-by` (`git commit -s`). The ~70 pre-DCO commits on this
branch would red the gate until rebased — operator decision, not a cycle's.

### Dormant branches (no open PR) — do not rebase onto these
`feature/c23-conformance-burndown-1` (2026-08-12, GUI + GPU plans) ·
`feature/sqlite-green-full-57377343437` (2026-08-11) ·
`feature/finish-sqlite-full-green-5366546` (2026-08-10) · ~20 older `feature/0-0-2-p*` branches.
🧠 Retained as history; none is an active workstream.

---

## 5. TIMELINE

*Newest first. Accumulates — new cycles are prepended. Includes cycles that did not go well.*

| Date | Commit | What shipped | Gate |
|---|---|---|---|
| 2026-08-13 (post-push) | — | **Two findings after the cycle closed, neither moving a verdict.** (a) The WSL lane's build watcher span until killed **over a build that had SUCCEEDED** — its producer `tee -a`'d both FAILURE arms into the log but wrote `BUILD OK` to stdout only; this INVERTS `D-HARNESS-WSL-…-WATCHER-CANNOT-TELL`'s remedy, which assumed success was the observable one. (b) `e42ae5a5`'s message quotes **1018** anchor citations; the committed tree measures **1019**, its parent **989** — a mid-cycle figure carried into the message unrefreshed. | gates re-run: guard OK 1019 · balance 983→983 · line-endings OK |
| 2026-08-13 | `e42ae5a5` | **Unwind lands**: DWARF CFI + `.eh_frame` on ELF/Mach-O execs (gdb unwinds 4 DSS frames) and in ELF `.o` (round-tripped through system gcc, 9 frames vs 2 stripped) · 2 silent pe64 unwind miscompiles · interior labels end-to-end · arm64 32-bit bitwise widening + MOVZ W-form · `.section`/`.space` · config key gates · **2 false-green red-on-disable mechanisms found** · handoff created | **Win 851/851 · WSL 851/851 · arm64 594/594 strict** |
| 2026-08-13 | `75ca4034` | asm-anchor burn-down: net −4 anchors; closed 2 silent miscompiles shipped one cycle earlier; a `.s` calls libc and RUNS | Win 838/838 · ⚠ **WSL + arm64 NOT run** |
| 2026-08-13 | `e5b60f6c` | Second assembly dialect (arm64). **Shipped 2 silent miscompiles** — negative scalars lost their sign; `[x29,#-8]` read as scale | Win 831/831 · ⚠ **1 leg of 3** |
| 2026-08-12 | `4969e9e2` | Inline asm P1+P2 — assembly becomes its own source language | — |
| 2026-08-12 | `ca2c6721` | DSS Axis + DSS HIR plan rework | — |
| 2026-08-12 | `60eb8ed8` | **PR #50 merged.** C23 burn-down: silent stringize miscompile, `__VA_OPT__`, GNU spellings, UCRT migration finished | — |
| 2026-08-11 | `0ecec160` | ELF copy relocations **deleted** — name-scoped copy reloc silently emptied glibc's `environ` alias set | 5/5 build · 2 legs by execution |
| 2026-08-10 | `3e86a187` | **PR #48.** pe CRT → UCRT; MIR call-site signature checking | — |
| 2026-08-03 | `f7c378be` | **PR #46.** SQLite compiled from full upstream source, suite green | — |
| 2026-07-20 | `4ccd6c6f` | **PR #47.** Static linking all formats · long double F80/F128 · type identity | — |
| 2026-07-15 | `d0c132c3` | **PR #41.** Cross-toolchain relocatable objects — DSS `.o` links + runs under gcc | — |
| 2026-07-09 | `c7a5377f` | **PR #36.** C23 FC16 + release-optimizer perf arc (>30 min → ~2 min) | — |
| ≤2026-07-08 | — | 🧠 Compressed: C23 FC17/17.5 (`_BitInt`, `thread_local`), C11 `<threads.h>`, arm64 Mach-O, `<stdbit.h>`, Apache-2.0 relicense (PRs #36–#45) | — |
