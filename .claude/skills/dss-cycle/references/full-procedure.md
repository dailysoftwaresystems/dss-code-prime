# The pause gate, the ten steps, hard stops, and stop-handling — full text

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

## C. The cycle — ten steps (+ design-review gate at 3.5 · self-audit gate at 8.5)

### Step 0 — Orient
- **★★★ READ `.plans/_handoff.md` FIRST — it is the previous cycle's statement of where the
  project is, where it is going, and what comes next (Step 8.1).** It is the cheapest orientation
  available and the reason the file exists; a handoff that is written every cycle and read by
  nobody is pure cost. ⚠ Treat it as the previous cycle's *claim*, not as ground truth: it was
  accurate when written, and Step 0's own measurements below outrank it wherever they disagree.
  **If they disagree, say so and correct the handoff this cycle** — a handoff that has silently
  drifted is worse than a missing one, because it is trusted.
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
- **DELEGATE (§C.0).** Run `/feature-dev:feature-dev`, or hand the priority to a `Plan` /
  `feature-dev:code-architect` agent, to produce the execution plan (understand → design →
  build sequence). Keep its TodoWrite list as the cycle's working plan. The orchestrator
  JUDGES the plan; it should not be the one reading the subsystem to write it.
- If the plan exposes an architectural fork or a pending definition → **§B gate**.

### Step 3.5 — Design-audit the plan before lock (the plan-lock gate)
Before any code is built (Step 4), the plan from Step 3 is **judged against the bar by an
independent reviewer** — the `dss-audit` design-review lens on the *plan*, not yet on code. An
agnosticism break, a tight-slice, a speculative build, or a weak-test plan caught here is far
cheaper than after the diff lands. (This is the gate run on the linkage P1+P2 plan, 2026-06-04.)

- **Independence is the point.** Spawn an **independent subagent** that applies the `dss-audit`
  bar to the plan — fresh context, no stake in having authored it, so it cannot rubber-stamp its
  own reasoning. For a **substrate / architectural-fork / new-mechanism** cycle this is *also* a
  §B pause: route the plan + the review's findings to the user (the human-side `dss-audit` pass)
  before resuming. **Scale the rigor:** a trivial mechanical cycle needs only a quick self-check
  against the list below; a new engine mechanism needs the full independent review.
- **What it checks — the bar (§A) + guardrails (§D), applied to a plan:**
  - *Agnosticism (the #1 break point):* every new vocabulary config-driven with a generic engine
    lookup — no planned `if (lang/arch/format == …)` in shared substrate.
  - *Best-long-term / no tight slice:* the complete solution with the hard part landing this
    cycle, not a stub dressed as "phase 1".
  - *Trigger-discipline:* no speculative build of a trigger-gated or consumer-less mechanism (the
    no-workaround violation in the *other* direction).
  - *Fail-loud (planned):* every unsupported construct gets a real diagnostic — never a silent ignore.
  - *Strict-test (planned):* the plan names the *strongest provable* test — red-on-disable for a
    guard, effectiveness assertion for an optimization, a behavioral/differential pin for a
    feature — not just "add a test".
  - *Guardrails:* no OPT7 / hard-stop crossing, no closing a gated anchor with an unfired trigger,
    no correctness-critical close without a constructible negative pin (§D).
  - *Deferral honesty:* every deferral named + pinned (§F); runtime proof that only manifests
    cross-CU / at-link is legitimately gated, not silently dropped.
- **The gate is real, not advisory.** Every finding is **resolved in the plan before locking**; a
  finding that implies an architectural choice escalates to a **§B gate**. Only a plan that clears
  the bar proceeds to Step 4.
- **It does NOT replace the post-build checks.** Step 5 (review & fold), Step 6 (fail-loud gate),
  and the separate post-cycle `dss-audit` of the *built* artifacts all still run in full — the
  built result is re-verified from scratch (green is never clean until re-run). The plan-lock gate
  is an upstream filter, never a substitute, and a blessed plan earns the code no trust until the
  build is independently audited. Judging the *plan* against the bar is not authoring it — the
  post-build auditor's independence on the *code* stays intact.

### Step 4 — Implement
- **DELEGATE, IN PARALLEL (§C.0).** Split the plan by DISJOINT file sets — engine `.cpp/.hpp`
  vs `src/dss-config/**.json` vs `examples/` vs `tests/` — and launch one agent per set IN
  ONE MESSAGE so they run concurrently. Name each agent's owned paths and its forbidden
  paths explicitly. Hand each the §A invariants, the house comment style, and the known
  traps for its area. The orchestrator integrates and verifies; it does not hand-type every
  edit.
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
  includes; UTF-8 / string-literal portability; **brace-init narrowing** — `T{expr}`
  where `expr` is a wider/unsigned type [e.g. `std::int64_t{rng() % 100}`, narrowing
  `unsigned long`→`int64_t`] is a hard ERROR under clang `-Wc++11-narrowing` but GCC/MSVC
  accept it, so the local MSVC+gcc gate misses it — use `static_cast<T>(expr)`). Local
  green ≠ CI green.
- Fold every FOLD-NOW finding. Then rebuild + re-run the full ctest.
- **Re-review the fold.** If folding *changed logic* (anything beyond comments / renames /
  formatting), run a **second `/pr-review-toolkit:review-pr` pass scoped to the fold's diff** — a
  fold can introduce its own bugs (the 2nd-order-fold discipline). Fold-and-re-review until a
  pass yields **no logic-changing FOLD-NOW** (a fixed point). If passes keep surfacing logic
  FOLD-NOWs without converging, that is a **§B signal** — stop and report, do not grind.

### Step 7 — Pin deferrals (bookkeeping sweep)
Sweep every deferral discovered this cycle and pin it per §F. This is the record-keeping pass:
high-priority and blocker-now deferrals of the *current* task were already actioned (Step 2 or
before push) — they are never left open-ended.

### Step 8.5 — Self-audit before lock (the pre-commit audit gate)
Before committing (Step 9), run the **`dss-audit` pass on the complete, gate-passed,
cross-plan-updated cycle** — the rule-lens (`dss-audit` §E) + guardrail enforcement (§F) + the
silent-gap hunt that the mechanical Step 6 gate **cannot** see. (The cycle-12 enum-init miss was
exactly this: 193/193 green + review-passed, yet a real latent rule-break — caught only by an
independent audit.) Running it here catches such a thing **before** anything is pushed.

- **Independence is the point.** Run it as an **independent subagent** applying the `dss-audit`
  bar — fresh context, no stake in having authored the diff, so it cannot rubber-stamp its own
  work. Step 6 already ran the mechanical battery; the value-add here is the *subtle* checks —
  agnosticism's conservative-default forms, prove-don't-assert / red-on-disable **completeness**
  (every site/form of a multi-site contract, not a subset), over-claimed-close, effectiveness-
  masking, and the §D guardrails.
- **On findings → back to Step 4.** Treat the findings as the next work items: return to **Step 4
  (Implement)**, fix them, then re-flow Step 5 (review + re-review) → 6 (gate) → 7 → 8 → 8.5
  (re-audit). Loop until the self-audit is **clean**, then proceed to Step 9 with a cycle that is
  already self-audited. A finding that implies a *design choice* (not a mechanical fix) is a **§B
  gate** — escalate, do not loop on it. Passes that keep finding issues without converging are
  themselves a §B signal.
- **Pre-commit, by design.** Auditing *before* the commit keeps a rule-breaking change off the
  branch entirely — no fix-forward churn, no pushed §F violation — and preserves "one cycle = one
  clean push". **CI legs** are the one thing this gate cannot check (nothing is pushed yet); they
  verify post-push (next cycle's Step 0 baseline, the separate human-run `dss-audit`, or
  `gh run watch`). This in-loop self-audit does **not** replace that external `dss-audit` — that
  stays the independent post-push backstop.

### Step 9 — Commit & push
- **PRECONDITION: `.plans/_handoff.md` was rewritten this cycle (Step 8.1) and is staged in THIS
  commit.** It ships with the work it describes, never in a follow-up — a handoff committed
  separately describes a tree that no longer exists, and the one reader it exists for is the one
  who will not notice.
- Commit using the repo cycle convention: subject `Cycle <id>: <concise summary>` (use
  `Cycle <id> WIP: …` only if the cycle legitimately pauses mid-task at a §B gate). Body
  lists anchors closed/opened + test delta. End with the repo's standard Co-Authored-By
  trailer (currently `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>`).
- **Push immediately** to the current feature branch (push-after-every-commit: it starts CI
  while context is hot, so GCC issues surface on the next cycle, not days later).
- Stay on the current feature branch; do not cut a new branch per cycle unless the user asks.

### Step 10 — Report & end
Emit a one-line cycle summary: priority closed, anchors touched, test delta, commit hash.

**★ The anchor line is MANDATORY and it carries the numbers, not an adjective:**
`anchors: opened N, closed M, net ±K — OPEN was <before>, now <after>`. The Step 6 balance gate
already refuses `after > before`, so this line is the receipt, not the check. "Anchored a few
follow-ups" is not a report; it is the thing the gate exists to make impossible to say.

**★ Name the NEXT priority as the handoff now records it** — one line, matching the top `NEXT`
entry in `.plans/_handoff.md` (Step 8.1). If the report and the handoff disagree about what comes
next, the handoff is the one a future reader will find, so fix the handoff, not the sentence.

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
through Step 9 (review → gate → cross-plan update → self-audit → commit → push)**, then halt. Do not begin
a new cycle. Two situations the stop does **not** override:
- **Cannot reach a clean gate** (red build/test the cycle can't self-repair): stop at the gate
  and report — do not push broken.
- **Already paused at an unanswered §B decision**: you cannot fabricate a resolution to force
  the flow to completion. Commit + push only the WIP-so-far **if** a WIP commit is legitimate
  (Step 9 — the pause is a real §B gate), re-present the decision brief, and halt awaiting the
  answer.
The stop tightens the loop to a close; it never lowers the bar.

---
