---
name: dss-plan-sweep
description: >
  Comprehensive plan-staleness reconciliation for DSS Code Prime — sweep EVERY plan in .plans/
  (+ the deferred-anchor registry, README, sibling skills) and reconcile every status, count,
  description, anchor, and cross-reference against the CURRENT observable implementation
  (git + ctest + src/ + the anchor guard). The third sibling: dss-cycle IMPLEMENTS, dss-audit
  JUDGES code, dss-plan-sweep RECONCILES plan-truth. It FIXES — markdown only, never src/ —
  everything reconcilable to observable fact (drifted counts, already-pushed "pending" markers,
  duplicate/contradictory rows, broken refs, numbering drift, a status that contradicts another
  plan surface). For doneness JUDGMENTS (striking an anchor closed, rewriting a description to
  match new code) it fixes only on unambiguous evidence, else FLAGS. Its guarantee: it leaves
  NO staleness behind — every divergence is fixed-with-evidence or explicitly flagged, never
  silently ignored, never over-claimed. Reconciles to truth, never to opinion; never deletes a
  historical/log row (the audit trail is load-bearing).
user-invocable: true
argument-hint: "[optional: specific plan file(s) to sweep; default = all plans]"
---

# DSS Code Prime — Plan Staleness Sweep

The repo's plans are a living contract: `.plans/NN-name - {ok,tbd}.md`, the
[`_deferred-anchor-registry.md`](../../../.plans/_deferred-anchor-registry.md), `README.md`, and
the sibling skills. Over a burst of cycles they drift — a count moves, a "pending push" gets
pushed, an anchor is struck in one plan but left open in another, a `⏳ planned` lands, a
description outlives the code it described. This skill is the **comprehensive sweep that makes
every plan tell the truth again** — and *leaves none of that drift behind*.

It is the strategic deep-clean counterpart to two tactical habits that already exist:

- **`dss-cycle` §C step 8** updates the plans *in the same commit as the code*, per cycle (tactical,
  keeps tidy).
- **`dss-audit` §I** fixes trivial staleness it *happens* to trip over (incidental).
- **`dss-plan-sweep`** (this skill) sweeps *all* plans *systematically* and reconciles *everything*
  (the formalization of the repo's existing "Plans staleness sweep" commits).

Authoritative inputs:
- **The plans:** every file in [`.plans/`](../../../.plans/) — numbered plans `00`–`22`, `08.x`
  sub-plans, `_deferred-anchor-registry.md`, `v2-gap-catalog`, `ZZ-final-goal.md`.
- **Observable truth:** `git` (push state, commit hashes, history), `ctest` (the real suite count),
  `src/` (what is actually implemented), `tools/check-anchor-registry.{ps1,sh}` (src↔registry).
- **Conventions:** the `dss-code-prime` skill (the `.plans/` system, §9 conventions). It wins on
  any conflict.

---

## A. The mandate & the cardinal discipline

1. **Leave no staleness behind.** Every divergence the sweep encounters ends in one of exactly two
   states: **fixed** (reconciled to observable fact, in the commit) or **flagged** (surfaced as an
   explicit decision in the report, because it needs a doneness judgment). A divergence that is
   *neither* — silently passed over — is a failure of the sweep itself. §H is the completeness proof.
2. **Reconcile to truth, never to opinion.** The plan text is corrected to match what `git` / `ctest`
   / `src/` / the anchor guard *actually show* — never to what the sweep *thinks* should be true. If
   the truth is unknown without a doneness judgment, that is a **flag** (§B), not a guess.
3. **Never over-claim a closure.** Striking an anchor `✅ CLOSED` or rewriting a description to "match
   the new code" asserts the work *fully covers the anchor's stated scope*. The sweep does that ONLY
   on unambiguous evidence (§B); otherwise it FLAGS. Marking partially-done work closed is the
   `dss-audit` §E #8 sin — the sweep must not commit it in the name of tidiness.
4. **Never rewrite history.** Historical log rows, the cycle-log, prior ctest-count progressions, and
   closed-anchor audit rows are **immutable** — the trail is load-bearing (same rule as the registry's
   "never delete a row"). The sweep reconciles *current / headline* status only.
5. **Markdown only.** The sweep edits `.plans/**` and skill `*.md`. It never touches `src/`, tests, or
   `*.json`. It does not build or run a cycle.

---

## B. The two kinds of staleness — fix vs flag

**FIX (mechanically reconcilable to observable fact):** the bulk of staleness. The truth is
determinable without judging whether work is "done":
- a headline ctest count that ≠ the actual `ctest` run;
- a `pending push` / `commit-pending` marker for a commit `git` shows is already pushed;
- a duplicate or self-contradictory row (e.g. the same tier listed twice with different statuses);
- a broken cross-reference / dead relative link / a `§N` pointer to a section that moved;
- numbering drift (a `§3` "OPT4" label colliding with `§0`'s as-built OPT4 — relabel to the live
  surface, per the plan's own stated convention);
- a status that contradicts *another plan surface* for the same item (§F);
- a filename status mismatch where it is unambiguous (but see §G — renames are usually a flag).

**FLAG (needs a doneness judgment — fix only on unambiguous evidence):**
- striking an anchor `✅ CLOSED`;
- rewriting a description because "the code changed";
- flipping a `⏳ planned` / `WIP` to done, or a plan filename `- tbd.md → - ok.md`.

The test for **"unambiguous evidence"** (the only bar that lets a judgment item be *fixed* rather than
flagged): the closing work is **present in `src/`** AND **covered by a passing test** AND the anchor's
**full stated scope** is satisfied (read the anchor text — partial coverage is a partial close, not a
strike). If all three hold and the suite is green, fix it and cite the commit. If *any* is uncertain
→ **flag** it: "looks closed (evidence: …) but scope-match unconfirmed — verify and strike." The
flag is how the sweep leaves nothing behind without over-claiming.

---

## C. The staleness taxonomy — the sweep checklist

For each class: the **tell**, the **source of truth** to reconcile against, and **fix|flag**.

| # | Class | Tell | Source of truth | Action |
|---|---|---|---|---|
| 1 | **Headline status marker** | `✅`/`⏳`/`🟦`/`WIP`/`DONE` on a current item | git + src + ctest | **fix** if it contradicts observable state *and* needs no scope-judgment; else **flag** |
| 2 | **Counts** (ctest / anchors / examples) | "N/N ctest", "M anchors", "K examples" in a *current/headline* line | actual `ctest` run; anchor guard count; `examples/` listing | **fix** the headline; **never** touch historical progression rows (§A.4) |
| 3 | **Commit / push state** | `pending push`, `commit-pending`, `pending-push`, `(commit-pending)` | `git log` / `git rev-list … origin/<b>` | **fix** to the real hash / strike "pending" once pushed |
| 4 | **Anchor closure consistency** | an anchor `✅ CLOSED` in one plan but listed open / as a live blocker elsewhere; or cited in `src/` but unstruck | the anchor guard + grep across all plans | **fix** the inconsistency *toward the evidenced state*; if the evidenced state itself is a doneness judgment → **flag** |
| 5 | **Cross-references** | dead relative link, `§N` ref to a moved section, ref to a renamed/closed plan file | the target file/section exists? | **fix** |
| 6 | **Numbering drift** | a sub-plan label (`§3 OPT-N`) diverging from the as-built `§0`/`§0.1` numbering | the plan's own stated "live numbering surface" note | **fix** per the plan's convention; if no convention is stated → **flag** (don't invent one) |
| 7 | **Duplicate / contradictory rows** | same item twice with different statuses (e.g. the OPT6 "DONE" + "planned" pair) | the evidenced state | **fix** (collapse to the true row); preserve any legitimately-distinct cycle rows |
| 8 | **Temporal-provenance prose** | `Next = X` / `⏳ planned` / "future" / "as of <date>" that is now past or done | git history (did X land?) | **fix** the forward-pointer if X is unambiguously done; else **flag** |
| 9 | **Description drift** | prose describing an implementation that has since changed (e.g. "dominators land in OPT1" when they landed OPT4) | `src/` + git history | **flag** unless the correction is unambiguous and scope-complete (then **fix**) |
| 10 | **Filename status** (`- ok.md` / `- tbd.md`) | a `- tbd.md` whose work is fully done, or a `- ok.md` reopened | git + src + the plan's own §0 | **flag** (rename touches every cross-ref + is a doneness judgment — §G) |

The four **plan surfaces for the same fact must agree** (§F): a status in plan-00 §0, in §0.1, in the
sub-plan's §0/§3.1, and in the registry are *one truth seen four ways* — reconcile them together, not
in isolation.

---

## D. The sweep — the steps

### Step 0 — Orient & baseline
- `git branch --show-current`, `git log --oneline -10`, `git status -s`, remote sync.
- **If the tree is dirty / a loop is mid-cycle on the plans → do NOT sweep those files** (§G
  no-commingling). Sweep only the quiescent plans, or wait. Note the skipped files in the report.
- Establish the live facts the sweep reconciles to: run `ctest --test-dir build --output-on-failure`
  (the real suite count), `tools/check-anchor-registry.{ps1,sh}` (src↔registry), and note the pushed
  HEAD (`git rev-list --left-right --count origin/<branch>...HEAD`).

### Step 1 — Inventory
- List **every** file under `.plans/` plus `README.md` and the sibling skills. The sweep covers all of
  them; partial coverage is not a sweep. (An explicit argument narrows scope to named files — say so
  in the report.)

### Step 2 — Per-class scan (§C)
- Walk the taxonomy across the inventory. Useful starting greps: `pending push|commit-pending`;
  the headline `[0-9]+/[0-9]+ ctest` count; `✅|⏳|🟦|WIP|TBD|Next =|⏳ planned`; each closed anchor's
  id (to check for open/blocker co-occurrence elsewhere); relative-link targets.
- For every hit, classify **fix | flag** per §B and record it (located: file:line).

### Step 3 — Reconcile
- **Fix** the mechanically-verifiable divergences, editing the plan text to the observable truth.
- **Flag** the doneness-judgment items (unless the §B unambiguous-evidence bar is met). A flag is a
  report entry, never a silent skip.
- Reconcile the four surfaces (§F) *together* so the fix in one is reflected in all.

### Step 4 — Verification pass (the completeness proof — §H)
- **Re-run the scans.** After fixing, every mechanical class must come back clean: the headline count
  now equals `ctest`; no `pending push` remains for a pushed commit; the anchor guard is OK; no closed
  anchor co-occurs with an open/blocker mention; links resolve. A divergence still present that is not
  on the flag list = the sweep is not done.

### Step 5 — Report & commit
- Emit the reconciliation report (§I): every plan swept, every fix, every flag.
- Commit markdown-only: `docs(plans): staleness sweep — <scope/date>`; body lists fixes + flags; the
  `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>` trailer. Push.

---

## E. Sources of truth — what is authoritative for each fact

The sweep **never invents** a value; it reads it from the authority and reconciles the plan to it.

| Fact | Authority |
|---|---|
| Current ctest count | a fresh `ctest --test-dir build --output-on-failure` |
| Push / commit state | `git log`, `git rev-list … origin/<branch>` |
| Anchor exists / is cited in src | `tools/check-anchor-registry.{ps1,sh}` + grep `src/` |
| Anchor is *closed* | code present + test green + full anchor scope covered (§B); else a flag |
| What an implementation actually does | `src/` + the cycle that landed it (git) |
| Plan-file lifecycle (`- ok`/`- tbd`) | the plan's own §0 status + git (flag the rename) |
| Cross-ref target exists | the referenced file/section |

If an authority is unavailable (e.g. CI legs the sweep can't run locally, or a closure that needs a
scope read it can't fully make) → the item is **flagged unverified**, never fixed on a guess.

---

## F. The four-surface consistency invariant

For any tier/anchor, these must all agree — they are one truth seen four ways:

1. **Plan-00 §0** status table (the project-wide headline).
2. **Plan-00 §0.1** stepper (next-up / done by block).
3. The **owning sub-plan** §0 / §3.1 row.
4. The **`_deferred-anchor-registry.md`** row (for anchors).

When the sweep fixes one, it reconciles all four in the same pass — a fix that leaves the other three
stale just relocates the drift. The plan *filename* `- ok.md`/`- tbd.md` is a fifth surface (flag, §G).

---

## G. Authority & boundaries

- **Markdown only:** `.plans/**` + skill `*.md`. **Never** `src/`, tests, or `*.json`. Never a build,
  never a cycle.
- **Never an over-claim:** no closure strike / description rewrite on judgment alone — only on the §B
  unambiguous-evidence bar; otherwise flag.
- **Never rewrite history:** log rows, cycle-log, count progressions, closed-anchor rows are immutable
  (§A.4). Reconcile current/headline status only.
- **No commingling with a live loop:** if `dss-cycle` / a `/loop` is *actively editing the plans* this
  moment, do not write those files — **flag the staleness in the report instead** (avoid the race; the
  no-commingling-with-in-flight-work rule wins over the convenience of the fix). Sweep the quiescent
  files; note the skipped ones.
- **Filename renames are a flag, not an auto-fix:** changing `- tbd.md → - ok.md` is both a doneness
  judgment *and* a multi-file operation (every cross-ref + git mv). Surface it as a recommendation with
  the evidence; let the implementer/user execute it through a cycle.
- **When unsure whether something is "mechanical" or "a judgment call" → it is a judgment call:** flag
  it, do not fix it. (Same tie-breaker as `dss-audit` §I.)

---

## H. The completeness guarantee — "leave no staleness behind"

This is the property that distinguishes a *sweep* from a spot-fix. It is proven, not asserted:

1. **Coverage:** every file in the inventory (§D.1) was scanned for every taxonomy class (§C). The
   report names them — an unswept plan is a hole in the guarantee.
2. **Resolution:** every divergence found is in exactly one of {fixed, flagged}. The report's fix-list
   and flag-list together account for *all* of them. A divergence in neither list is a defect of the
   sweep, not acceptable output.
3. **Verification (§D.4):** the mechanical classes re-scan clean after the fixes — the headline count
   equals `ctest`, no live "pending push", anchor guard OK, no closed-anchor/open-mention collision,
   links resolve. If a re-scan still shows a mechanical divergence, the sweep is not finished.
4. **The flags are the honest remainder:** "leave nothing behind" does **not** mean "fix everything" —
   it means *nothing is silently ignored*. A correctly-flagged doneness judgment is a *resolved* item
   (handed to the right decision-maker), not leftover staleness.

---

## I. The report & commit

**Report shape:**
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

**Commit:** `docs(plans): staleness sweep — <scope/date>`, markdown only, push. If the sweep produced
*only* flags (nothing mechanically fixable), there is no commit — just the report (and the flags go to
the user / `dss-cycle`).

---

## J. Quick reference

| Need | Command / path |
|---|---|
| Real suite count | `ctest --test-dir build --output-on-failure` |
| Anchor guard (src↔registry) | `tools/check-anchor-registry.ps1` (or `.sh`) |
| Push state | `git rev-list --left-right --count origin/<branch>...HEAD` |
| All plans | `.plans/` (numbered `00`–`22`, `08.x`, registry, `v2-gap-catalog`, `ZZ-final-goal`) |
| Anchor registry | `.plans/_deferred-anchor-registry.md` |
| The four surfaces | plan-00 §0, plan-00 §0.1, sub-plan §0/§3.1, registry (§F) |
| Sibling: per-cycle plan update | the `dss-cycle` skill §C step 8 |
| Sibling: incidental plan hygiene | the `dss-audit` skill §I |
| `.plans/` system + conventions | the `dss-code-prime` skill |

**The sweep's creed:** *reconcile to truth, never to opinion; fix what the evidence proves, flag what
needs a judgment, rewrite no history — and leave nothing silently behind. A plan that disagrees with
the code is a bug in the plan; a sweep that ends with an unaccounted-for divergence is a bug in the
sweep.*
