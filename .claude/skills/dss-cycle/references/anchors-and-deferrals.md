# Deferral & anchor pinning discipline + quick reference

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
2. **Pin it in the best home.** A feature-area `D-*` anchor → that plan's §3.1 anchor row.
   A project-level known-open item that is not a feature anchor → plan-00 **§0.2** (Deferred &
   Known-Open Items registry). An orphan / cross-cutting anchor → `_deferred-anchor-registry.md`.
   Use the registry schema: `| Anchor | Trigger | Closing work | Cross-refs |`.
3. **State its priority explicitly** in the row — one of: **blocker-now** (must close inside
   this cycle, Step 2 or before push), **high** (run at end of this cycle, or earlier if it
   becomes a blocker), **normal** (backlog), **trigger-gated** (do not build until the named
   trigger fires). High and blocker-now items never leave the cycle open-ended.
4. **Cite, don't orphan.** Any `D-*` referenced in `src/` must resolve to a registry row or
   a plan citation, or the anchor guard fails the gate (§C step 6).

---

## G. Quick reference

| Need | Command / path |
|---|---|
| Build | `cmake --build build` |
| Full test suite | `ctest --test-dir build --output-on-failure` |
| Anchor guard | `scripts/check-anchor-registry/check-anchor-registry.ps1` (or `scripts/check-anchor-registry/check-anchor-registry.sh`) |
| **Handoff — read at Step 0, rewritten at Step 8.1** | `.plans/_handoff.md` — ①where we are ②where we need to get ③priorities ④concurrent branches/PRs (rebase surface) ⑤timeline (accumulates) |
| Open PRs / rebase surface | `gh pr list --state open` · `gh pr view <n> --json files` (Step 8.2) |
| Priority spine | `.plans/00-compiler-implementation-plan - tbd.md` §0.1 |
| Deferral registry | `.plans/_deferred-anchor-registry.md` |
| Anchor balance gate | `python scripts/check-anchor-balance/check-anchor-balance.py` |
| Per-cycle plan | `/feature-dev:feature-dev` (Step 3) |
| Plan-lock design audit | independent `dss-audit` lens on the plan, pre-build (Step 3.5) |
| Per-cycle review (+ re-review the fold) | `/pr-review-toolkit:review-pr` ×N to a fixed point (Step 5) |
| Pre-commit self-audit | independent `dss-audit` lens on the built cycle (Step 8.5) — findings loop back to Step 4 |
| Conventions + strict tests | the `dss-code-prime` skill (§7, §9, §13) |

**The loop's own creed:** it holds itself to the same fail-loud, no-workaround, agnostic
standard it enforces on the code. When in doubt — about a definition, a design, or whether
the bar is met — it **pauses and asks** rather than guessing.

---
