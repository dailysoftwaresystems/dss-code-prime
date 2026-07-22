---
name: dss-cycle
description: >
  Advance the DSS Code Prime compiler by exactly ONE development cycle: pick the next
  priority from the plan-00 §0.1 stepper, clear its blockers first, plan it with
  /feature-dev:feature-dev, design-audit the plan against the bar before locking it,
  implement the best long-term solution, run /pr-review-toolkit:review-pr (and re-review the
  fold if it changed logic), pin every deferral with a priority, cross-plan update, self-audit
  the cycle against the bar before locking, then commit and push. One cycle per invocation — meant to be driven by /loop for continuous
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
   - **Aggregate op-count pins go INERT when a shared helper emits the same op elsewhere.** An
     `EXPECT_GE(And, N)` / op-count assertion is a worthless guard if a branchless-select (or any
     composition helper) already emits that op — deleting the guarded instance leaves the count
     satisfied and the test green (the FC17.9(b) `bit_ceil` shift-clamp pin was inert exactly so:
     two `sel()`s emitted 4 `And`s, so dropping the clamp `And` stayed green). Pin the guarded
     value's **operand chain** (e.g. assert `Shl.operand[1]` IS the clamp `And`, not a bare `Sub`)
     and **demonstrate red-on-disable** by actually removing the guard, not merely asserting present.
   - **Multi-site / multi-form contracts** — the "apply X at every site/form of class C"
     class (e.g. "strip the specifier prefix at every positional decl resolution"). A green
     suite over a SUBSET of the sites/forms is NOT proof: latent misses at the unexercised
     sites survive review *and* green. (Cycle-13 audit case: a 4th missed strip site — the
     enum-enumerator value loop — survived the cycle-12 review because its test exercised
     only a variable decl.) Close it one of two ways: **(a)** funnel ALL sites through ONE
     chokepoint so coverage is by-construction — duplication is what breeds the missed site;
     or **(b)** have the closing test exercise EVERY form of C, *including forms not yet
     consumed by any shipped language*, via a synthetic schema that constructs the consuming
     shape itself. An unconsumed substrate's misses are latent by definition — the test must
     build the consuming shape, not wait for a real consumer to expose it.
   - **Real-execution corpus example** — when the feature produces *observable end-to-end
     behavior* (a source construct that compiles to a binary whose exit code / stdout reflects
     it), ship a **runnable corpus example** exercising it: `examples/<lang>/<name>/`
     (`main.<ext>` + `expected.json` — the differential runner compiles → spawns → asserts the
     exact exit/stdout). A binary that runs correctly proves the whole source→…→`.exe` chain a
     unit/MIR-tier test cannot. **First check the existing corpus** — if a fixture already
     exercises the feature, reuse it (add an `optimizedPipelines` arm or an assertion) rather
     than duplicate; add a NEW example only for a genuine coverage gap. **Make it a *good*
     exercise, not a vacuous one:** the feature must actually manifest at runtime with operands
     no earlier pass can fold away (cycle 10r's division corpus uses runtime function-args,
     never a literal `100/7` that const-folds before the idiv ever runs). **Witness the
     OPTIMIZER, not just the lowering:** a runtime-observable feature's corpus MUST carry an
     optimized arm that runs the REAL shipped pipeline — `{"shippedPipeline": "release"}`, NEVER
     a hand-listed `passes` subset and NEVER baseline-only — so the optimizer×feature composition
     (Inlining / Mem2Reg / LICM over the feature's NEW MIR shapes) is exercised end-to-end at
     runtime. A baseline-only example runs the no-op `debug` pipeline, and a hand-listed subset
     drops passes the release build runs; either silently lets a future optimizer regression — or
     a frame-slack-masked overrun — pass green (the FC7-C3 array-storage width overrun the
     `passes`-subset corpus MASKED; the FC12 variadic cycle then shipped 18/19 examples
     baseline-only, leaving the optimizer's effect on the new va_list cursor/alloca shapes
     unwitnessed). The MIR/LIR pins lock the *pre-optimizer* shape; only a `release`-arm corpus
     proves the optimizer preserves the feature's runtime semantics. **Carve-out — do not
     manufacture a vacuous corpus:** a feature with *no* runtime-observable behavior (pure
     substrate, a diagnostic-only fail-loud, a MIR-tier transform with no runtime difference —
     e.g. single-CU `static`→Local DCE just drops an unused symbol — a behavior-preserving
     refactor) is proven by its appropriate-tier strict test (+ red-on-disable); a corpus that
     exercises nothing is itself the masked-effectiveness trap.
   - **Cross-target runtime closure — encoding pins are NOT runtime proof.** When a feature's
     correctness is *runtime control-flow* on a target the local gate cannot execute (any
     non-host CPU/ABI — e.g. an ARM64 binary on an x86_64 host), byte/encoding pins prove the
     bytes are *right*, never that the program *runs* right: a function can encode every
     instruction perfectly yet omit the link-register (x30) spill in a non-leaf frame → perfect
     bytes, runtime SIGSEGV (the v0.0.2 catch — `2b07e3b` shipped a "Runnable ARM64-ELF FFI
     proof" that byte-pinned green then SIGSEGV'd on the native-arm64 leg; `dbf84b0` was the x30
     fix). So such a feature is **NOT `✅ CLOSED` on local-green + byte-pins** — it stays
     *runtime-pending* until the binary has actually **executed on the target** and produced the
     asserted exit/stdout. Each cross-target has a matching CI leg that builds AND runs `ctest`
     **natively** — gate the closure on that leg going **green** (push, confirm via next cycle's
     Step 0 baseline or `gh run watch`, then mark CLOSED; never on the push alone):
       - **ARM64-Linux** → the native `ubuntu-24.04-arm` leg (`run-linux-arm64: true`); RISC-V /
         WASM → an emulator-gated leg.
       - **macOS-ARM64** → the **`macos-latest` (Apple Silicon) leg** (`run-macos: true`) — it
         runs `ctest` on real arm64 hardware, so it auto-executes a Mach-O corpus exactly like the
         arm64-Linux leg (it is NOT leg-less).
     **Local pre-push run — native when the host matches the target.** If you build ON the
     target's own OS/arch, the corpus executes in your LOCAL `ctest` and goes **green locally**
     before the push: e.g. **on a macOS (Apple Silicon) machine the macOS-ARM64 corpus runs in
     local `ctest` → green → push → the `macos-latest` leg re-confirms** (no human step). Off the
     target host, ARM64-Linux can still run locally under `qemu-aarch64`, but **macOS-ARM64 is the
     one target with no off-Mac emulator** (nothing runs a Mach-O on Windows/Linux) — so from a
     non-Mac host (the loop's usual env) either hand the Mach-O to a Mac for a manual pre-push run
     (a 2-step, **§B** hand-off: present the binary + expected exit/stdout) or rely on the
     `macos-latest` CI leg to execute it post-push.
     In every case ALSO ship a **host-independent** structural pin that is red-on-disable on
     *every* leg (e.g. the non-leaf frame puts the link register into `savedRegs`), so a
     regression is caught even when the one execution path is unavailable — the execution run is
     the end-to-end witness, the structural pin the always-on guard; keep both, never collapse
     them.
6. **The full commit gate.** The operational checklist (with commands) is **Step 6 (§C)** —
   the single source of truth for the gate items. **Any red the cycle cannot self-repair →
   STOP and report. Do not push.** Self-repair = a mechanical fix obvious from the failure
   (missing include, stale assertion, fold-induced break); if the red implies a design choice
   or reveals a real blocker, it is a **§B gate**, not a repair.
7. **No un-anchored issue — every issue you come across is ANCHORED *and* HANDLED, never worked
   around.** The moment a cycle *comes across* ANY issue — a bug, a silent-miscompile risk, a
   build/gate/CI fragility, a flaky test, a stale doc, a missing guard, a surprising behavior —
   it is **FORBIDDEN** to leave it un-anchored **or** to route past it with a workaround. This
   holds **even when the issue is outside the current cycle's scope, and even when its proper
   fix belongs to a later cycle.** "Not my cycle" / "I'll remember it" / "I excluded the failing
   test" / "it passes on the other leg" / "green modulo X" is *precisely the trigger to anchor*,
   never license to drop. Two obligations, BOTH mandatory, BOTH in **this** cycle:
   - **(a) Anchor it now** — a real registry row in `_deferred-anchor-registry.md` (name +
     what/why + trigger + closing-work), committed THIS cycle. A prose-only note in a commit
     message, a chat reply, or a code comment is **NOT** an anchor: an un-anchored issue is
     invisible to the next cycle, to the anchor guard, and to the plan sweep — so it *will* be
     silently lost. (If the issue is a live `D-*` you must also cite it in `src`/config; if it is
     purely infra/docs, the registry row alone suffices.)
   - **(b) Address it properly** — **FIX it now** if it blocks the cycle's gate, is small, or is
     within reach (default to fixing); **ELSE** pin it as a genuine **deferred anchor** with an
     explicit trigger + closing-work (§F/§D) — a later cycle is legitimate ONLY behind a named
     blocker or an unfired trigger. Either way it is *handled*: NEVER a silent skip, a masked
     test exclusion, a swallowed error, or a "temporarily disabled" that no anchor tracks.
   A workaround that *hides* an issue (excluding a failing test, catch-and-swallow, "it's green
   on the other leg so ignore it here") is the exact silent-failure the bar exists to prevent —
   it violates §A.2 (no workarounds) and §A.4 (fail loud) as well as this rule. **Motivating
   catch:** the TF-C51 fat-archive gate hit a real GNU-on-Windows COFF `-Wa,-mbig-obj` scope gap
   on an *unrelated* test TU (`test_mir_to_lir.cpp`, "file too big"); the first instinct —
   exclude that test from the Windows leg — was a workaround. Correct handling per §A.7:
   root-cause → anchor `D-BUILD-GNU-WINDOWS-BIGOBJ-SCOPE` → fix it (project-wide flag) → witness
   the TU now builds+passes → commit. **An orthogonal issue you merely *found* is still yours to
   anchor + handle** — the discovery is the obligation.

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

## C. The cycle — ten steps (+ design-review gate at 3.5 · self-audit gate at 8.5)

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

### Step 6 — Fail-loud gate
This is the canonical gate checklist (§A.6 is its one-line statement). Verify every item:
- `cmake --build build` clean (no link errors).
- `ctest --test-dir build --output-on-failure` 100%, including the new tests.
- anchor-registry guard OK: `tools/check-anchor-registry.ps1` (or `.sh`).
- agnosticism scan clean (no hardcoded language/CPU/format in shared substrate).
- CI-hazard screen clean (from Step 5): no GCC-vs-MSVC portability traps. Local green ≠ CI green.
- review folded clean.
- **§A.7 issue-anchoring — nothing worked around.** Every issue this cycle *came across* — including
  out-of-scope / later-cycle ones — is ANCHORED in the registry (this commit) **and** handled (fixed
  now, or pinned as a deferred anchor with a trigger). If you excluded, disabled, skipped, or
  "green-modulo"-ed ANYTHING to reach green, it MUST carry an anchor + a proper fix-or-defer decision;
  a silent workaround is a gate failure, not a pass.

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
| Anchor guard | `tools/check-anchor-registry.ps1` (or `tools/check-anchor-registry.sh`) |
| Priority spine | `.plans/00-compiler-implementation-plan - tbd.md` §0.1 |
| Deferral registry | `.plans/_deferred-anchor-registry.md` |
| Per-cycle plan | `/feature-dev:feature-dev` (Step 3) |
| Plan-lock design audit | independent `dss-audit` lens on the plan, pre-build (Step 3.5) |
| Per-cycle review (+ re-review the fold) | `/pr-review-toolkit:review-pr` ×N to a fixed point (Step 5) |
| Pre-commit self-audit | independent `dss-audit` lens on the built cycle (Step 8.5) — findings loop back to Step 4 |
| Conventions + strict tests | the `dss-code-prime` skill (§7, §9, §13) |

**The loop's own creed:** it holds itself to the same fail-loud, no-workaround, agnostic
standard it enforces on the code. When in doubt — about a definition, a design, or whether
the bar is met — it **pauses and asks** rather than guessing.

---

## H. Worktree & agent operations — every new worktree inherits root permissions

A worktree spawned for a cycle (an implementation agent, an independent audit, a probe) is
the **same repo on the same machine** as root. It must therefore run with the **same
permissions root already has** and **never re-prompt** for what root trusts. This is
non-negotiable: a worktree that stalls or spams permission prompts is an operational defect,
not the user's job to click through. Enforce it on both layers:

1. **Permission allowlist — blanket, not per-command.** The project
   `.claude/settings.local.json` `permissions.allow` list must carry the blanket tool grants
   **`Bash(*)` AND `PowerShell(*)`** (on Windows, cmake/ctest builds run through PowerShell —
   `Bash(*)` alone leaves every worktree *build* command prompting and being approved one at a
   time). If a tool starts prompting inside a worktree, add its `(*)` blanket entry to root's
   settings rather than approving individual commands; the worktree inherits it immediately.
2. **Sandbox — agents disable it for trusted build/VCS commands.** Any worktree/agent prompt
   that is *not* an allowlist miss is the **sandbox** flagging a write outside the Windows
   workspace root (typically the agent's WSL `/home`/`/tmp` build dirs). Spawn **every**
   worktree agent with a standing instruction to pass `dangerouslyDisableSandbox: true` on its
   build / compile / `git` / `wsl` commands (trusted repo operations). Bake that line into the
   agent prompt verbatim — do not rely on the agent inferring it.

**Corollary — prefer the root for fast, sequential, low-risk cycles.** A worktree buys
parallel isolation; it costs permission friction and (if spawned at a stale base) a slow
build. When a cycle is *sequential* and *config-shaped* (e.g. a shipped-descriptor cycle whose
re-probe compiles SQLite), run it in the **root**: it inherits root permissions automatically
and gets the current HEAD's compile-time wins (post the c97 resolver fix, the SQLite re-probe
is ~seconds, not ~15 minutes). Reserve worktrees for genuinely parallel or higher-risk *code*
changes — and when you use one, **reset it to the current HEAD** first (worktrees can spawn at
a stale base like p18), so it too builds fast.
