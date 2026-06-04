---
name: dss-cycle
description: >
  Advance the DSS Code Prime compiler by exactly ONE development cycle: pick the next
  priority from the plan-00 §0.1 stepper, clear its blockers first, plan it with
  /feature-dev:feature-dev, implement the best long-term solution, run
  /pr-review-toolkit:review-pr, pin every deferral with a priority, cross-plan update,
  commit and push. One cycle per invocation — meant to be driven by /loop for continuous
  autonomous progress. PAUSES and asks the user on any pending definition, architectural
  fork, gated anchor, or hard-stop boundary; never guesses, never workarounds, never
  breaks source (language) / target (processor) / linker (object-format) agnosticism.
user-invocable: true
argument-hint: "[optional: a specific §0.1 step or D-anchor to target this cycle]"
---

# DSS Code Prime — Development Cycle Loop

This skill executes **exactly one cycle per invocation, then stops**. A cycle takes the
project one priority forward and ends at `git push` (or at a paused decision gate). It is
the codification of the autonomous-cycle discipline proven across the 10-series cycles.

The authoritative artifacts this skill reads and writes every cycle:

- **Stepper (priority spine):** [`.plans/00-compiler-implementation-plan - tbd.md`](../../../.plans/00-compiler-implementation-plan%20-%20tbd.md) — §0.1 **"Stepper — next-up by block"**.
- **Deferred-anchor registry (blockers + backlog):** [`.plans/_deferred-anchor-registry.md`](../../../.plans/_deferred-anchor-registry.md).
- **Repo conventions + strict-test posture:** the `dss-code-prime` skill (§7 testing, §9 conventions, §13 contribution checklist). Read it; it wins on any conflict with this skill.

---

## A. The bar — NON-NEGOTIABLE (re-read every cycle)

These hold for every line of code, every test, every commit. A cycle that cannot meet the
bar **stops and reports** — it never pushes a partial or a workaround.

1. **Source / target / linker agnostic.** No identity branch in shared substrate — never
   `if (schema.name() == "...")`, `if (arch == "...")`, `if (format == "...")`. Vocabulary
   is config-driven (`.lang.json` / `.target.json` / `.format.json`); the engine walks a
   closed verb set, never a language/CPU/format identity. This is a hard veto: if the only
   way you see forward is an identity branch, that is a **decision gate** (§B), not a cycle.
2. **Best long-term solution, no workarounds.** Implement the complete clean solution now.
   "Tight slice" / "just for this case" / "TODO later" is forbidden. A real blocker is
   named and pinned (§F) — never silently deferred.
3. **No follow-ups for the hard part.** The difficult core of the priority is implemented
   **this cycle** — never sliced off as a "follow-up", "next cycle", "phase 2", or "polish
   later" when no real blocker stops it from landing now. "It is hard", "the cycle is
   getting big", or "I'll circle back" is **not** a blocker. A later cycle is legitimate
   ONLY for work behind a genuine named blocker or an unfired trigger, pinned per §F/§D. If
   the hard part truly cannot land now, that is a **decision gate** (§B): bring the options,
   do not quietly defer.
4. **Fail loud.** Every unsupported construct emits a real diagnostic, never a silent
   miscompile or a swallowed error. Follow the `*Fatal` + `X_*`/`D-*` patterns already in
   `src/`.
5. **Strict-assertion tests.** Every new test asserts the strongest provable property
   (exact counts, full-sequence/byte equality, `static_assert`, death-test message match).
   See the `dss-code-prime` skill §7. A test that still passes when the implementation is
   silently broken is not strict enough.
6. **The full commit gate.** The operational checklist (with commands) is **Step 6 (§C)** —
   the single source of truth for the gate items. **Any red the cycle cannot self-repair →
   STOP and report. Do not push.** Self-repair = a mechanical fix obvious from the failure
   (missing include, stale assertion, fold-induced break); if the red implies a design choice
   or reveals a real blocker, it is a **§B gate**, not a repair.

---

## B. The pause-and-ask gate — the most important behavioral rule

The loop is autonomous for **execution** but escalates **decisions** to the user. When any
of the following appears, **PAUSE the loop and ask the user — do not assume a default, do
not guess, do not hallucinate, do not pivot to other work on your own:**

- **A pending definition or ambiguity** — any requirement, behavior, naming, scope, or
  schema shape that is underspecified or undecided. Surface it as a question and wait.
- **An architectural fork** — more than one defensible long-term design. A fork is *real*
  only if you can state ≥2 concrete, defensible long-term designs (step 2 below). If you
  cannot articulate a genuine second option, there is no fork — the hard part lands this
  cycle (§A.3); never invent a fork to escape the work.
- **A gated anchor** whose trigger has not fired, or a correctness-critical anchor whose
  negative miscompile-pin cannot be constructed (§D).
- **A hard-stop boundary** (§D).

**How to present a decision (always this shape):**

1. State the problem and why it blocks the cycle, in one or two sentences.
2. Give 2–4 candidate **long-term** solutions. Each one must be **no-workaround** and
   **source/target/linker agnostic** — if a candidate breaks agnosticism or is a shortcut,
   say so explicitly and explain why it is still listed (usually: to be rejected).
3. For each: the trade-off (what it costs, what it buys, what it forecloses).
4. **Recommend** the best long-term agnostic option and say why.
5. Ask the user to choose. If you are missing a fact needed to decide, ask for the fact —
   never invent it. When the user answers, capture the decision + rationale in the owning
   plan (or plan-00 §0.2) this cycle so it is not re-litigated next invocation; if the
   resolution defers work, pin it with a §F priority.

The loop resumes only after the user answers. While paused, do not start a different cycle.

---

## C. The cycle — ten steps

### Step 0 — Orient
- Check `git status` + current branch + the last commit subject. A `… WIP` cycle in flight
  means **this cycle finishes it** (it is the priority).
- Read §0.1 of plan 00 and skim `_deferred-anchor-registry.md` for open anchors.
- Establish the baseline: `cmake --build build` then `ctest --test-dir build --output-on-failure`.
  Baseline must be green before new work (unless the WIP is the thing being repaired). A red
  baseline with no WIP-repair context is itself a **§B gate** — present it; do not silently
  "fix it" (scope creep) and do not proceed on red.

### Step 1 — Pick the next priority
- From **§0.1 (next-up by block)**, take the next *real* priority (the next eligible step,
  reading top-to-bottom). An explicit argument to this skill overrides the auto-pick — but the
  override is still subject to the bar (§A), the pause-and-ask gate (§B), and the hard-stop /
  gated-anchor checks (§D); a supplied target is never a license to skip them.
- **If §0.1 has no eligible step**, analyze the deferred-anchor registry with
  `/feature-dev:feature-dev`. **Only anchors that are unconditional or whose trigger has
  already fired are eligible for promotion** — a trigger-gated anchor with an unfired trigger
  (§D) is NOT promotable, even if it is the highest-leverage item. Prioritize the *eligible*
  anchors by leverage, **promote the chosen one into §0.1** as a new row, then pick it.
  (Refilling the stepper from deferrals is the sanctioned "stepper is dry" path.) If the only
  forward work is a trigger-gated anchor whose trigger has not fired → **§B gate**.
- If picking surfaces a pending definition → **§B gate**.

### Step 2 — Clear blockers FIRST
- Determine the priority's blockers from **both** the §0.1 "Blocked by" column **and** the
  anchor registry (prerequisite `D-*` anchors for that area) **and** any "requires
  deferrals" note in the plan.
- Address the blockers before the priority itself. A blocker for the current task runs
  **before the cycle ends** — and earlier if it gates everything else. Highest-priority
  blocker first.
- If a blocker is itself gated / a hard stop / a fork → **§B gate**.

### Step 3 — Plan with feature-dev
- Run `/feature-dev:feature-dev` on the priority to produce the execution plan (understand →
  design → build sequence). Keep its TodoWrite list as the cycle's working plan.
- If the plan exposes an architectural fork or a pending definition → **§B gate**.

### Step 4 — Implement
- Build the **best long-term, agnostic** solution (§A). Extend config vocabulary, never
  branch the engine on identity.
- Tests are strict (§A.5). Diagnostics fail loud (§A.4).
- Any new `D-*` anchor cited in `src/` MUST be registered in the same commit (§F, Step 8) —
  the anchor guard enforces it.

### Step 5 — Review & fold
- Run **`/pr-review-toolkit:review-pr`** on the cycle's diff.
- Also run the standing inter-task checks: an **agnosticism verification** pass (no
  hardcoded language/CPU/format in shared substrate) and a **CI-hazard screen** for
  GCC-vs-MSVC portability (gtest `ASSERT_*` in non-void helpers; PCH-masked missing
  includes; UTF-8 / string-literal portability). Local green ≠ CI green.
- Fold every FOLD-NOW finding. Then rebuild + re-run the full ctest.

### Step 6 — Fail-loud gate
This is the canonical gate checklist (§A.6 is its one-line statement). Verify every item:
- `cmake --build build` clean (no link errors).
- `ctest --test-dir build --output-on-failure` 100%, including the new tests.
- anchor-registry guard OK: `tools/check-anchor-registry.ps1` (or `.sh`).
- agnosticism scan clean (no hardcoded language/CPU/format in shared substrate).
- CI-hazard screen clean (from Step 5): no GCC-vs-MSVC portability traps. Local green ≠ CI green.
- review folded clean.

**Any red the cycle cannot self-repair → STOP and report the blocker. Do not push broken.**
Better to wake the user to "stopped at step N, here is the blocker" than to push something
subtly wrong.

### Step 7 — Pin deferrals (bookkeeping sweep)
Sweep every deferral discovered this cycle and pin it per §F. This is the record-keeping pass:
high-priority and blocker-now deferrals of the *current* task were already actioned (Step 2 or
before push) — they are never left open-ended.

### Step 8 — Cross-plan update
Keep the plans honest in the **same commit** as the code:
- Update plan 00 §0 status table + §0.1 stepper row (flip status, update ctest count).
- Update the owning sub-plan: flip the §0 status row AND stamp the §3.1 deferred-items row
  (status flip in §0; `✅ CLOSED` stamp in §3.1 — update both, not one).
- In `_deferred-anchor-registry.md`: mark closed anchors `✅ CLOSED <date>` with the commit;
  **never delete a row** (the audit trail is load-bearing); add new anchors.
- Record the cycle in the running cycle-log (memory entry per the established convention).
- Update the `dss-code-prime` skill if a convention changed.

### Step 9 — Commit & push
- Commit using the repo cycle convention: subject `Cycle <id>: <concise summary>` (use
  `Cycle <id> WIP: …` only if the cycle legitimately pauses mid-task at a §B gate). Body
  lists anchors closed/opened + test delta. End with the repo's standard Co-Authored-By
  trailer (currently `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>`).
- **Push immediately** to the current feature branch (push-after-every-commit: it starts CI
  while context is hot, so GCC issues surface on the next cycle, not days later).
- Stay on the current feature branch; do not cut a new branch per cycle unless the user asks.

### Step 10 — Report & end
Emit a one-line cycle summary: priority closed, anchors touched, test delta, commit hash.
**The invocation ends here.** Under `/loop`, the next invocation begins the next cycle with
fresh context.

---

## D. Hard stops & gated anchors (always route through §B)

- **OPT7 / inlining — roadmap `G-406` (plan 07); cross-CU sub-anchor `D-OPT7-1` (plan 22).**
  First inter-procedural pass; touches linkage / DCE / cross-CU legality. Plan 07 marks it a
  **"HARD STOP boundary … supervised cycle when opened"** — so always halt and present a §B
  decision brief; never open autonomously.
- **Trigger-gated anchors** (e.g. `D-OPT-MEMORYSSA-CLOBBER-WALK`,
  `D-OPT4-1-NON-LINEAR-MARKER-MERGE`). A trigger-gated anchor is **NOT a TODO** — it means
  "do not build until the trigger fires" (real-input failure, 3rd consumer, targeted
  backend). If its trigger has not fired, **skip it and report "trigger not fired"** — do
  not close it because it is next in a backlog. Backlog ordering is sequencing guidance, not
  a closure license.
- **Correctness-critical anchors** (silent-miscompile class, e.g.
  `D-OPT6-LICM-TRAP-SAFE-HOIST`). The closing cycle MUST ship a **negative miscompile-pin**:
  a program that breaks (e.g. traps via div-by-zero) iff the transform mis-fires under a
  constructed input. If the pin cannot be constructed this cycle, **STOP** and bring a §B
  brief — do not ship on review alone.

---

## E. Stop-command handling

If the user issues a stop while a cycle is running: **finish the current cycle's full flow
through Step 9 (review → gate → cross-plan update → commit → push)**, then halt. Do not begin
a new cycle. Two situations the stop does **not** override:
- **Cannot reach a clean gate** (red build/test the cycle can't self-repair): stop at the gate
  and report — do not push broken.
- **Already paused at an unanswered §B decision**: you cannot fabricate a resolution to force
  the flow to completion. Commit + push only the WIP-so-far **if** a WIP commit is legitimate
  (Step 9 — the pause is a real §B gate), re-present the decision brief, and halt awaiting the
  answer.
The stop tightens the loop to a close; it never lowers the bar.

---

## F. Deferral & anchor pinning discipline

Every deferral is explicit, located, and prioritized — never silent.

1. **Explain it.** One clear paragraph: what is deferred and *why* (the real blocker or the
   missing trigger).
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
| Anchor guard | `tools/check-anchor-registry.ps1` (or `tools/check-anchor-registry.sh`) |
| Priority spine | `.plans/00-compiler-implementation-plan - tbd.md` §0.1 |
| Deferral registry | `.plans/_deferred-anchor-registry.md` |
| Per-cycle plan | `/feature-dev:feature-dev` (Step 3) |
| Per-cycle review | `/pr-review-toolkit:review-pr` (Step 5) |
| Conventions + strict tests | the `dss-code-prime` skill (§7, §9, §13) |

**The loop's own creed:** it holds itself to the same fail-loud, no-workaround, agnostic
standard it enforces on the code. When in doubt — about a definition, a design, or whether
the bar is met — it **pauses and asks** rather than guessing.
