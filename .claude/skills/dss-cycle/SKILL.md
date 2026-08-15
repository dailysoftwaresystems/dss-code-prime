---
name: dss-cycle
description: >
  Advance the DSS Code Prime compiler by exactly ONE development cycle — pick the next priority from
  the plan-00 §0.1 stepper, clear its blockers first, plan it, design-audit the plan before locking
  it, implement the best long-term solution, review, pass the fail-loud gate, pin deferrals, update
  the plans, self-audit before lock, then commit and push. Use this whenever the
  user asks to run a cycle, do the next cycle, continue the compiler work, advance the plan, work the
  stepper, or pick up the next priority — and whenever /loop drives continuous autonomous progress —
  even if they never say "skill". One cycle per invocation. It PAUSES and asks on any pending
  definition, architectural fork, gated anchor, or hard-stop boundary; it never guesses, never
  workarounds, and never breaks source (language) / target (processor) / linker (object-format)
  agnosticism. NOT for judging finished work (use dss-audit), reconciling plan staleness (use
  dss-plan-sweep), or running the multi-host matrix (use dss-cross-leg-test).
user-invocable: true
argument-hint: "[optional: specific priority or anchor to take this cycle]"
---

# DSS Code Prime — Development Cycle Loop

One cycle per invocation: pick → clear blockers → plan → design-audit → implement → review → gate →
pin → cross-plan → self-audit → commit → push.

## When to use

- Advancing the compiler by one real priority, autonomously or under `/loop`.
- Finishing a `… WIP` cycle already in flight — that *is* this cycle's priority.

**Not this skill:** judging finished work → `dss-audit`. Plan staleness → `dss-plan-sweep`.
Multi-host matrix → `dss-cross-leg-test`.

**Conventions authority:** the `dss-code-prime` skill wins on any conflict.

## The pause-and-ask gate — the most important behavioral rule

The loop is autonomous for **execution** and escalates **decisions**. When any of these appears,
**PAUSE and ask the user — do not assume a default, do not guess, do not pivot to other work:**

- **A pending definition or ambiguity** — any requirement, behaviour, naming, scope, or schema shape
  that is underspecified.
- **An architectural fork** — a fork is *real* only if you can state ≥2 concrete, defensible
  long-term designs. If you cannot articulate a genuine second option there is no fork, and the hard
  part lands this cycle. **Never invent a fork to escape the work.**
- **A gated anchor** whose trigger has not fired, or a correctness-critical anchor whose negative
  miscompile-pin cannot be constructed.
- **A hard-stop boundary** (see below).

**How to present a decision, always this shape:** (1) the problem and why it blocks, in one or two
sentences; (2) 2–4 candidate **long-term** solutions, each no-workaround and agnostic — if one breaks
agnosticism say so and why it is still listed, usually to be rejected; (3) each one's trade-off —
what it costs, buys, forecloses; (4) a **recommendation** with reasoning; (5) the ask. If a fact is
missing, ask for the fact — never invent it. When the user answers, capture the decision and its
rationale in the owning plan this cycle so it is not re-litigated next invocation.

The loop resumes only after the user answers. **While paused, do not start a different cycle.**

## Workflow

**Delegation is the default** — see the file map. The orchestrator judges; it should not be the one
hand-typing every edit or reading every subsystem.

0. **Orient.** Read `.plans/_handoff.md` first — the previous cycle's claim, not ground truth; where
   it disagrees with your own measurements, say so and correct it this cycle. Check `git status`,
   branch, last commit subject. Read plan-00 §0.1 and skim the anchor registry. Establish a green
   baseline (`cmake --build build`, then full `ctest`). **A red baseline with no WIP-repair context
   is itself a pause gate** — present it; do not silently "fix it".
1. **Pick the next priority** from §0.1, top-to-bottom. An explicit argument overrides the auto-pick
   but is still subject to the bar, the pause gate, and the hard-stop checks. If §0.1 is dry, promote
   an *eligible* anchor (unconditional, or trigger already fired) into §0.1, then pick it.
2. **Clear blockers FIRST** — from the §0.1 "Blocked by" column *and* the registry *and* any
   "requires deferrals" note. Highest-priority blocker first, before the priority itself.
3. **Plan it** — delegate to `/feature-dev:feature-dev` or a `Plan` / `code-architect` agent.
4. **Design-audit the plan before lock** — an **independent** subagent applies the `dss-audit` bar to
   the *plan*. An agnosticism break, a tight slice, a speculative build, or a weak-test plan caught
   here is far cheaper than after the diff lands. Scale the rigor: trivial mechanical cycle → quick
   self-check; new engine mechanism → full independent review **and** a §B pause to the user.
5. **Implement** — delegate in parallel by **disjoint file sets** (engine `.cpp/.hpp` vs
   `src/dss-config/**.json` vs `examples/` vs `tests/`), one agent per set, launched in one message.
   Name each agent's owned and forbidden paths. Build the best long-term agnostic solution: extend
   config vocabulary, never branch the engine on identity. Any new `D-*` cited in `src/` is
   registered in the same commit.
6. **Review and fold** — `/pr-review-toolkit:review-pr`, plus the agnosticism pass and the CI-hazard
   screen. **Re-review the fold** if folding changed logic; iterate to a fixed point. Passes that
   keep surfacing logic findings without converging are a pause signal — stop and report, do not grind.
7. **Fail-loud gate** — the mechanical battery, including the anchor-balance gate.
8. **Pin every deferral** discovered this cycle.
9. **Cross-plan update**, including rewriting `.plans/_handoff.md`, in the same commit as the code.
10. **Self-audit before lock** — an **independent** subagent runs the `dss-audit` rule-lens and
    guardrails on the complete, gate-passed cycle. On findings, return to step 5 and re-flow through
    this gate until clean. A finding implying a *design choice* is a pause gate, not a loop.
11. **Commit and push.** `.plans/_handoff.md` must be staged in THIS commit — it ships with the work
    it describes, never in a follow-up. Subject `Cycle <id>: <concise summary>`; body lists anchors
    closed/opened plus the test delta; end with the repo's standard Co-Authored-By trailer (currently
    `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>`). Push immediately — it starts CI while
    context is hot. Stay on the current feature branch.
12. **Report and end** (contract below). The invocation ends here; under `/loop` the next invocation
    begins the next cycle with fresh context.

## Output contract

A one-line cycle summary — priority closed, anchors touched, test delta, commit hash — plus:

```
anchors: opened N, closed M, net ±K — OPEN was <before>, now <after>
next: <one line, matching the top NEXT entry in .plans/_handoff.md>
```

**The anchor line is MANDATORY and carries numbers, not an adjective.** The gate already refuses
`after > before`, so this line is the receipt, not the check — "anchored a few follow-ups" is exactly
what the gate exists to make impossible to say. If the report and the handoff disagree about what
comes next, fix the handoff: it is the one a future reader will find.

## Hard stops — always route through the pause gate

★★★ **OPERATOR RULING 2026-08-15 — A HARD STOP GATES *OPENING A CAPABILITY*, NEVER *FIXING A DEFECT*.
THERE IS NO HARD STOP ON FIXES, ANYWHERE, AT ALL.** Verbatim: *"please remove the hard stop on FIXES
at all!"* If the work is repairing something already shipped that is wrong — a silent miscompile, a
crash on legal input, a false rule, a conformance divergence, a guard that asserts nothing — it is a
FIX, and **no hard stop applies to it** regardless of which subsystem it lands in. Fixes proceed
autonomously under the ordinary bar.
⚠ **Why this needed saying:** a hard stop is a scope guard against a cycle quietly starting a large
new arc. Applied to a fix it inverts into the opposite of its purpose — it becomes a reason to leave
known-broken shipped behaviour in place, which is precisely the deferral §A.7 forbids, wearing a
governance rule as a disguise. ✔The case that produced this ruling: `hwtime.h` was blocked by
`__inline__` handling, which touches the inliner; treating that as OPT7-gated would have parked a
measured defect behind a rule written to stop *new pass development*.
★ **The distinction to apply, and it is about the DELIVERABLE, not the file you edit:**
*"does this make something CORRECT that is currently WRONG?"* → **FIX, no gate.**
*"does this make DSS able to do something it has never done?"* → **capability, gate still applies.**
Touching a gated subsystem's source does not by itself make it a capability; the OPT7 gate is about
opening the inter-procedural *arc*, not about every line in `src/opt/`.

- **OPT7 / inlining** (`G-406`, sub-anchor `D-OPT7-1`) — first inter-procedural pass, touches
  linkage / DCE / cross-CU legality. A supervised cycle; **never open autonomously.**
  ⇒ **Gated: opening the arc.** ⇒ **NOT gated: fixing a defect in inlining that already ships**,
  per the ruling above.
- **Trigger-gated anchors** — NOT a TODO. "Do not build until the trigger fires." If it has not
  fired, skip and report "trigger not fired". Backlog ordering is sequencing guidance, not a closure
  license.
- **Correctness-critical anchors** (silent-miscompile class) — the closing cycle MUST ship a negative
  miscompile-pin that breaks iff the transform mis-fires. If the pin cannot be constructed, STOP and
  bring a decision brief. Never ship on review alone.

## Stop-command handling

On a stop mid-cycle: **finish the current cycle's full flow through commit and push**, then halt. Do
not begin a new cycle. Two things the stop does not override — **a gate you cannot reach cleanly**
(report, never push broken), and **an unanswered pause gate** (you cannot fabricate a resolution;
commit WIP only if legitimate, re-present the brief, halt). The stop tightens the loop to a close; it
never lowers the bar.

## File map

- Read `references/the-bar.md` **at the start of every cycle** — the six non-negotiables in full,
  with the worked cases behind each. This is the standard everything else here serves.
- Read `references/delegation.md` before steps 3–5 — what to delegate, how to split by disjoint file
  sets, and what the orchestrator keeps.
- Read `references/gate-and-cross-plan.md` at steps 7 and 9 — the full fail-loud gate battery and the
  cross-plan update including the handoff rewrite.
- Read `references/anchors-and-deferrals.md` at step 8, and whenever pinning a deferral or judging
  whether an anchor is eligible.
- Read `references/operator-discipline.md` when reporting or claiming anything — the bar applies to
  the operator, not only to the code.
- Read `references/worktrees.md` before any byte-changing measurement or agent worktree operation.

## Failure modes this skill exists to prevent

- **Guessing past a decision.** A pending definition, a real fork, an unfired trigger, or a hard stop
  is a pause — not a default, not a pivot to other work.
- **Inventing a fork to avoid the hard part.** If you cannot state a second defensible design, the
  hard part lands this cycle.
- **Breaking agnosticism** by branching the engine on language, arch, or format identity instead of
  extending config vocabulary.
- **Shipping a test that passes both ways**, or closing a correctness-critical anchor without its
  negative miscompile-pin.
- **Auditing your own work.** Steps 4 and 10 are independent subagents precisely so they cannot
  rubber-stamp reasoning they authored.
- **Committing the handoff separately** — it then describes a tree that no longer exists.
