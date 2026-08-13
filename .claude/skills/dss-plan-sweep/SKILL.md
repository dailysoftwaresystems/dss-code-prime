---
name: dss-plan-sweep
description: >
  Sweep every plan in .plans/ (plus the deferred-anchor registry, README, and sibling skills) and
  reconcile every status, count, anchor, description, and cross-reference against observable truth —
  git, ctest, src/, and the anchor guard. Use this whenever the user asks to sweep, reconcile,
  refresh, or de-stale the plans; says the plans are out of date, drifted, stale, or disagree with
  the code; asks to fix plan counts or statuses after a burst of cycles; or invokes /dss-plan-sweep —
  even if they never say "skill". Edits markdown ONLY — never src/, tests, or JSON — and never builds
  or runs a cycle. NOT for implementing work (use dss-cycle) and NOT for judging code quality or
  auditing an implementation (use dss-audit); this skill judges only whether the PLANS tell the truth.
  Fixes what evidence proves, flags what needs a doneness judgment, never rewrites historical rows.
user-invocable: true
argument-hint: "[optional: specific plan file(s) to sweep; default = all plans]"
---

# DSS Code Prime — Plan Staleness Sweep

Reconcile every plan surface to what `git` / `ctest` / `src/` / the anchor guard actually show, and
leave no divergence silently unaccounted for.

## When to use

- The plans have drifted after a burst of cycles — counts, statuses, or anchors disagree with the code.
- A systematic pass is wanted, not a spot-fix. Partial coverage is not a sweep.
- The user names specific plans to sweep (narrow the scope, and say so in the report).

**Not this skill:** implementing work → `dss-cycle`. Judging code against the bar → `dss-audit`.
Incidental staleness tripped over mid-audit is already `dss-audit` §I's job; this skill is the
systematic counterpart to `dss-cycle` step 8's per-cycle tidying.

## The one decision that runs on every hit: fix or flag

**FIX** when the truth is determinable *without* judging whether work is done — a headline count that
disagrees with `ctest`, a "pending push" for a commit `git` shows pushed, a duplicate row, a dead
link, numbering drift, a status contradicting another surface for the same item.

**FLAG** when it needs a doneness judgment — striking an anchor `✅ CLOSED`, rewriting a description
because "the code changed", flipping `⏳ planned` to done, renaming `- tbd.md → - ok.md`.

A flag may be *fixed* only on unambiguous evidence: the closing work is **present in `src/`** AND
**covered by a passing test** AND the anchor's **full stated scope** is satisfied. If any of the three
is uncertain, flag it — "looks closed (evidence: …), scope-match unconfirmed". **When unsure whether
something is mechanical or a judgment, it is a judgment.**

## Workflow

1. **Orient.** `git branch --show-current`, `git log --oneline -10`, `git status -s`, remote sync.
   If a `/loop` or `dss-cycle` is *actively editing the plans right now*, do not write those files —
   flag their staleness in the report instead and list them as skipped.
2. **Baseline the authorities.** Run `ctest --test-dir build --output-on-failure` for the real suite
   count, `tools/check-anchor-registry.{ps1,sh}` for src↔registry, and
   `git rev-list --left-right --count origin/<branch>...HEAD` for push state.
3. **Inventory.** Every file under `.plans/`, plus `README.md` and the sibling skills. Name them in
   the report — an unswept plan is a hole in the guarantee.
4. **Scan.** Run `python scripts/scan_staleness.py <repo-root>` for the mechanical classes. Walk the
   full taxonomy (see file map) across the inventory for the classes a grep cannot see.
5. **Classify** every hit fix|flag per the rule above, recorded as `file:line`.
6. **Reconcile.** Fix the mechanical divergences. Reconcile all four surfaces for the same fact
   *together* — a fix that leaves the other three stale just relocates the drift.
7. **Verify.** Re-run `scripts/scan_staleness.py`. Every mechanical class must come back clean. A
   divergence still present and not on the flag list means the sweep is not finished.
8. **Report and commit.** Markdown only: `docs(plans): staleness sweep — <scope/date>`, body listing
   fixes and flags, then push. If the sweep produced *only* flags, there is no commit — just the report.

## Output contract

```
# Plan staleness sweep — <date> (<scope: all | named plans>)
Reconciled to: ctest N/N · anchor-guard OK · pushed HEAD <hash>

## Coverage
swept: <every plan / skill file>  ·  skipped (live-loop): <files + why>

## Fixed (reconciled to observable fact)
- <plan:line> — <class §C#> — <was> → <now> (truth: <authority>)

## Flagged (doneness judgment — needs a decision, NOT silently left)
- <plan:line> — <class> — <what looks stale> · <evidence so far> · <who decides / what to verify>

## Verification
re-scan clean: counts ✓ · pending-push ✓ · anchor guard ✓ · cross-plan consistency ✓ · links ✓
```

## File map

- Read `references/taxonomy.md` at step 4 — the ten staleness classes, each with its tell, its source
  of truth, and whether it is fix or flag.
- Read `references/sources-of-truth.md` when deciding what is authoritative for a given fact, or when
  reconciling the four surfaces that must agree.
- Read `references/completeness-guarantee.md` when reporting, or when judging whether the sweep is
  actually finished.
- Run `scripts/scan_staleness.py` at steps 4 and 7 — the mechanical scans, identical both times.

## Failure modes this skill exists to prevent

- **Over-claiming a closure.** Striking an anchor closed asserts the work covers its *full stated
  scope*. Marking partially-done work closed in the name of tidiness is the sin this skill must not
  commit.
- **Rewriting history.** Log rows, the cycle-log, count progressions, and closed-anchor audit rows are
  immutable — the trail is load-bearing. Reconcile current/headline status only.
- **Silently passing over a divergence.** Every one ends fixed or flagged. Neither is a defect of the
  sweep, not acceptable output. "Leave nothing behind" means nothing is ignored, not that everything
  is fixed.
- **Reconciling to opinion.** The plan is corrected to what the authorities *show*, never to what the
  sweep thinks should be true.
- **Straying out of markdown.** `.plans/**` and skill `*.md` only — never `src/`, tests, or `*.json`.
