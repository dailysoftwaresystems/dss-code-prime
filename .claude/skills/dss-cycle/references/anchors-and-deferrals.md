# Deferral & anchor pinning discipline + quick reference

## Contents
- F. Deferral & anchor pinning discipline — 0. the admissibility gate · 1. explain it · 2. pin it in the
  best home (the registry files, the six-cell schema, the writer) · 2b. closing is a move · 3. state its
  priority · 4. cite, don't orphan
- G. Quick reference — commands and paths; the write-by-default warning; the loop's own creed

This file numbers the steps the older way; the crosswalk to `SKILL.md`'s 0–12 is at the top of
`workflow-steps.md`.

AMENDED 2026-09-21 by "you do everything. I'm not your babysitter." — where this file sends a FORK to the user as a §B decision, a meaning fork or a documented-behaviour change included, or a new engine mechanism at the plan's design audit, `SKILL.md`'s step 4, the agent now decides it by measurement and the references' own documentation, writes the rationale into the row and reports it veto-able. The gates' three ESCAPE HATCHES stay the operator's — a deferral, §A.7 clause c; carrying a net-open rise, the balance gate's escalation; growing a ratchet baseline, `UNCOVERED_BASELINE` and its kind — because standing orders govern all three: close, do not file; no follow-ups; a ratchet only comes down. Each is a PAUSE with one crisp question, never an agent decision (see SKILL.md, the decision gate)

## F. Deferral & anchor pinning discipline

Every deferral is explicit, located, and prioritized — never silent. **But first: most
candidate deferrals are not admissible. The default is that the work lands THIS cycle** — a
deferral is the rare exception that must earn its place, not the convenient way to end a cycle.

0. **The admissibility gate — is this even a deferral?** Before anything is pinned, the
   candidate must clear a strict bar, or it is *not a deferral at all* — it is this cycle's
   work. A deferral is admissible **only** when ALL THREE hold:
   - **(a) Blocked *now*, by a *named* prerequisite.** The work genuinely cannot complete this
     cycle because one specific, nameable thing is absent: an unbuilt substrate it depends on,
     an **unfired trigger** (§D), or a **§B decision** the user has not made. You must be able to
     state the exact missing prerequisite *and* what event unblocks it. "Needs more design",
     "needs a refactor first", "is complex/large" name no blocker — that is the hard part itself
     (§A.3), and the hard part lands now.
   - **(b) Deferring is the *best long-term* call, not a convenience.** Landing it now would
     NOT force a workaround or a speculative build (§A.2). If it can land cleanly now, it must —
     a deferral you *could* have closed this cycle is a silent slice.
   - **(c) The reason survives the negative test.** "It was getting big", "the cycle is full",
     "I'll circle back", "phase 1 / phase 2", "polish later", "follow-up", and "the natural next
     step" are **inadmissible** — each is the hard part being sliced (§A.3). Scope size and
     tedium are never blockers.

   Fail ANY of (a)–(c) → **it is not a deferral; do it this cycle.** If it genuinely cannot land
   now yet you cannot point to a clean *named* blocker, that uncertainty is itself a **§B
   decision gate** — bring the options to the user, never quietly defer. **When in doubt, do the
   work.** Only a candidate that clears all three proceeds to the pinning steps (1–4) below.

1. **Explain it.** One clear paragraph: what is deferred and *why* (the real blocker or the
   missing trigger) — the *why* is the admissible reason from step 0, restated.
2. **Pin it in the best home.** Every anchor → the deferred-anchor registry, the only home a row can
   have, because the door writes nothing else (since 2026-09-25 a plan's §3.1 table and plan-00 **§0.2**
   are no longer homes; ✔MEASURED that day, the plans held no open row the program counted),
   which is **two documents** — one WORKING list (`-production.md`) and one ARCHIVE (`-done.md`) —
   and the choice is not cosmetic:
   - `.plans/_deferred-anchor-registry-production.md` — a still-open defect **a user of the
     compiler could hit**, in the shipped binary or in the config it reads. ★ This is the file
     the burndown works from, ALWAYS (operator, 2026-08-25).
   - `.plans/_deferred-anchor-registry-done.md` — the ARCHIVE (operator, 2026-09-01). Every CLOSED
     row, in one table. **Nothing here is work, and you never file INTO it directly** — a row
     arrives by being closed.
   - ⚠ **There is no harness registry.** It retired on 2026-09-16 with the move to `DssHarness`:
     a defect in the harness is that tool's to fix, and a defect in this repository's own build
     wiring, tests or plans is a production row like any other. The rule it carried has not
     changed — fix a harness defect the moment you FACE it, never later.
   ⚠ A row's bucket follows the **DEFECT**, never the instrument that found it. `D-CONFIG-*` and
   `D-DIAG-*` are PRODUCTION deliberately — a config document IS the compiler's behaviour here,
   and a diagnostic IS its output to a user.

   **The schema is SIX cells:** `| Anchor | Priority | Status | Trigger | Closing work | Cross-refs |`
   — `Priority` is `P0`..`P5`, `Status` is `✅ CLOSED` / `🟠 OPEN` / `⏳ GATED` /
   `🔵 DISCLOSED`, and `--status` or a `--status-file` holds one of those cells or its bare word
   (`closed`, `open`, `gated`, `disclosed`); `dssharness write-anchor` refuses anything else, the
   retired `🔵 🟠 OPEN (DISCLOSED)` included. The last one is **OPEN WORK** whose debt PRE-DATES this cycle: it
   counts in every total and is exempt only from the balance gate's net-increase refusal, so
   writing up a defect you merely FOUND is not punished like shipping a new deferral. The claim
   is checkable against the base ref — never use it for a defect this cycle introduced.
   ⚠⚠ **DO NOT HAND-WRITE THE ROW.** Use the writer, which takes the FIELDS:

       dssharness write-anchor D-<AREA>-<NAME> \
            --priority P1 --status open --trigger '...' --closing '...' --cross-refs '...'

   It WRITES by default; `--anchor-dry-run` shows the row and where it would be filed without writing,
   and every cell has a `--<cell>-file` form (`--trigger-file`, …) for a long cell. `--insert` and
   `--apply` do not exist, and a command carrying either exits 10.

   A hand-typed row can WRAP the anchor id (invisible to every grep, and it MINTS a false id),
   carry an unescaped `|` (silently adds a column, shifting the status into the closing work), or
   miss a cell. None of those is expressible through the writer. Omit `--priority` and it is
   seeded from the burndown sieve and PRINTED, so you see what it chose.

2b. **CLOSING IS A MOVE, NOT AN EDIT** (operator, 2026-09-01: *"always delete a done item and put
   into `_deferred-anchor-registry-done.md` once finished"*):

       dssharness set-anchor D-<AREA>-<NAME> --status closed --closing '...'

   `set-anchor` patches only the fields you name, preserves the rest byte-for-byte, deletes the row
   from its working registry and appends it to the archive's matching table. Reopening (`--status
   open`) moves it back. `check-anchor-balance` fails the tree for a closed row left in a working
   registry, for an open row filed in the archive, and for a `Status` column that contradicts the
   verdict leading its own `Trigger` prose.
3. **State its priority explicitly** in the row's `Priority` cell — `P0`..`P5`, the bands defined in
   `registry-and-priority.md`, a declaration `burndown-queue` seeds and then reads. A row waiting on a
   named trigger carries `⏳ GATED` in its `Status` cell: do not build until the trigger fires. Work that
   blocks this cycle closes inside it (Step 2 or before push), and work due this cycle runs at its end, or
   earlier if it becomes a blocker — neither leaves the cycle open-ended.
4. **Cite, don't orphan.** Any `D-*` referenced in `src/` must resolve to a registry row — the only
   home a row can have — or the anchor guards fail the gate (§C step 6): `dssharness check-anchor-citations`
   resolves a cited id only to a row of the two registries, while `check-anchor-registry` still accepts an
   id found anywhere in `.plans/`.

---

## G. Quick reference

| Need | Command / path |
|---|---|
| Build | `dssharness build --legs <leg>` |
| Full test suite | `dssharness test --legs windows-x86_64-debug --json --time` — it builds first; the round gate is `dssharness test --legs gate` |
| Anchor guard | `dssharness run check-anchor-registry` (ctest `anchor_registry_guard`) |
| Resolve every cited id to a registry row | `dssharness check-anchor-citations --current-tree` |
| **Handoff — read at Step 0, rewritten at Step 8.1** | `.plans/_handoff.md` — ①where we are ②where we need to get ③priorities ④concurrent branches/PRs (rebase surface) ⑤timeline (accumulates) |
| Open PRs / rebase surface | `gh pr list --state open` · `gh pr view <n> --json files` (Step 8.2) |
| Priority spine | `.plans/00-compiler-implementation-plan - tbd.md` §0.1 |
| Deferral registry — WORKING (what is LEFT) | `.plans/_deferred-anchor-registry-production.md` — the only working registry since 2026-09-16 |
| Deferral registry — ARCHIVE (closed; never read to ORIENT) | `.plans/_deferred-anchor-registry-done.md` |
| Read ONE anchor, in full | `dssharness read-anchor <ANCHOR>` — one command on every host |
| List anchors — name, priority, status | `dssharness read-anchors --pending [--band P0]` [→ the listing ends with its count, so `--open --band <P>` gives a band's](registry-and-priority.md) |
| Write a NEW row from fields | `dssharness write-anchor <ANCHOR> --trigger '...' --closing '...'` — ⚠ it WRITES; `--anchor-dry-run` is how you look first |
| Change a row — **closing MOVES it to the archive** | `dssharness set-anchor <ANCHOR> --status closed --closing '...'` — ⚠ it WRITES |
| Apply a lane's rows directory, as one batch | `dssharness run rows --manual-step stage --input rows=<lane rows> --input staged=<staged> --input only=<ID>,...` (or `only=@<file>`, one id a line) `--input new=<ID>,...` (the ids the batch may CREATE: a row no registry holds that `new` does not name is refused as a typo), then `--manual-step check --input staged=<staged>` (read-only: each supplied cell SAME, RESPACED, KEPT, FILLED or LOST, status and priority SAME or CHANGED, with a word diff of each LOST cell; every id a row newly cites must be a row of a registry or of the batch, because no guard reads the registries' own citations, `check-anchor-citations` included), then `--manual-step apply --input staged=<staged>` (`--input acceptLost=<ID>:<cell>,...` for each LOST cell read) — rehearsed in a throwaway repository first, all or nothing, every row read back; a failure restores both registries unless another writer changed them meanwhile, which it says |
| Lint every row a reader cannot key on | `dssharness read-anchors --lint` |

⛔⛔ **THE DEFAULT INVERTED WHEN THE DOOR MOVED, AND A COPIED IDIOM NOW WRITES.** The retired
`.harness-config/runner/actions/anchors/*-anchor.{sh,ps1}` twins DRY-RAN unless given `--apply`; `dssharness write-anchor`
and `set-anchor` **WRITE unless given `--anchor-dry-run`**. So the one habit that used to be safe —
leaving `--apply` off to see what would happen — now lands the change. ✔MEASURED 2026-09-17:
`dssharness set-anchor <ID> --priority P2 --anchor-dry-run` answers *"dry run: … would be updated in
the pending registry; nothing was written"*, exit 0, and `git status` is clean afterwards.
ⓘ `--production` is gone with the twins: the registry a row lands in is decided by its STATUS, and
`--pending` / `--done` narrow a LISTING. Every field also has a `--<field>-file` form, which is how
a multi-line cell reaches the tool without a shell quoting it.

| Anchor balance gate | `dssharness check-anchor-balance --base <cycle-start-sha>` |
| Per-cycle plan | `/feature-dev:feature-dev` (Step 3) |
| Plan-lock design audit | independent `dss-audit` lens on the plan, pre-build (Step 3.5) |
| Per-cycle review (+ re-review the fold) | `/pr-review-toolkit:review-pr` ×N to a fixed point (Step 5) |
| Pre-commit self-audit | independent `dss-audit` lens on the built cycle (Step 8.5) — findings loop back to Step 4 |
| Conventions + strict tests | the `dss-code-prime` skill (§7, §9, §13) |

**The loop's own creed:** it holds itself to the same fail-loud, no-workaround, agnostic
standard it enforces on the code. When in doubt — about a definition, a design, or whether
the bar is met — it **pauses and asks** rather than guessing.

---
