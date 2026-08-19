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

## ★★★ THE GOAL IS TO *WORK* — one working reference makes the behaviour REQUIRED

Operator ruling 2026-08-19: *"we must never crash on correct code, even if gcc fails, we must do it right. … If we have a reference that works, we must too (of course, with the implementation always following our project's best practices)."*

**The test is the DISJUNCTION, not the consensus.** If ANY reference (gcc, clang, MSVC, …) compiles and runs a correct construct, DSS must too. A reference's FAILURE is therefore never evidence against DSS — when DSS accepts what one reference rejects and another accepts, DSS is **right**, and the divergence is a **NON-DSS CONFOUND** to attribute and record. **Never make DSS fail in order to match a failing reference.** This bounds the bidirectional rule: "accepting what no reference accepts is a defect" turns on **NO** — not one. The implementation still owes the full bar (agnostic, config-driven, best-long-term, fail-loud, strictly tested): "it works" is the requirement, not the excuse. ⚠ Probe references **separately** — "the reference" is not one voice, and P14 nearly narrowed a working header chain because only gcc's failure was on file and MSVC's success was not. Full case: `references/the-bar.md` §A.3b.

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

### ★★★ A §B TRIGGER IS A PREDICATE, NOT A RITUAL (operator ruling 2026-08-17)

A row pinned "§B — operator decision" is gated on a **stated reason**. If a lane MEASURES that
reason to be FALSE, the §B **was never triggered** — discharging it is not closing a §B on the
lane's own authority, it is discovering the gate does not apply.

**The rule, and all three clauses are load-bearing:**
- every §B row states its trigger as a **testable predicate**, not as a mood;
- a lane MAY discharge a §B by **measuring the predicate false**, provided it records the
  measurement **in the row** and **flags it in the cycle report** so the operator can veto;
- an **INCONCLUSIVE** measurement escalates. **Silence is never a discharge.**

✔The case that produced this ruling: `D-CSUBSET-INLINE-ASM-SPELLING` was pinned §B because bare
`asm` "needs a new standard-mode axis". A lane measured that DSS already declares GNU mode in the
reference compilers' own machine-readable spelling (defines `__GNUC__`/`__clang__`, does **not**
define `__STRICT_ANSI__`) — so no axis exists or was added, and the predicate was false. It also
measured that DSS was **accepting `int asm = 42;`**, which no reference compiler accepts in GNU
mode: the pinned state was shipping an *invented extension*, not merely withholding a feature.
Operator ruling: **keep it.** *"Reverting sound, measured, conformance-correcting work to
re-present it as a brief would destroy value to satisfy a ritual."*

⚠ **Without this rule the next lane's only options are an unauthorized close or a wrong revert** —
which is why the procedural hole, not the keyword row, was the real deliverable of that cycle.
⚠ Discharging a predicate does not discharge what the predicate did not cover. In that same case
the references' acceptance genuinely IS conditional (under `__STRICT_ANSI__` they require
`__asm__`), so a **trigger-gated** row was opened for the day a strict-conformance mode ships —
the §B's original concern preserved and made testable, without holding the fix hostage to it.

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
   Name each agent's owned and forbidden paths. ★★ **At most FOUR reasoning agents live at once**
   (operator instruction 2026-08-19) — more work than that runs in waves of four. Script execution
   (builds, `ctest`, the guards, a remote leg) does **not** count against the cap; see
   `references/delegation.md`. Build the best long-term agnostic solution: extend
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

★★★ **OPERATOR INSTRUCTION 2026-08-17 — SILENCE IS THE DEFAULT. EMIT NOTHING THE OPERATOR DOES NOT
NEED IN ORDER TO PROCEED.** Verbatim: *"requires no output tokens unless what I need to know to
proceed (failures, done/not done, final report, etc)"*.

**The complete list of things worth emitting:**
1. **A failure or a blocker** — something is red, refused, or cannot proceed. State it, with the
   measurement, and what you are doing about it.
2. **A pause gate** — a decision only the operator can make (§B, a pending definition, an unfired
   trigger, a hard stop). This is the one case where length is justified: options, trade-offs,
   recommendation.
3. **Done / not done** — a step's terminal state, when the operator's next action depends on it.
4. **The final report** — the output contract below.
5. **A direct answer to a direct question.**

⛔ **Everything else is noise, and the list of what NOT to emit is the useful half:** no progress
narration ("lane X is running", "starting the build"), no interim summaries of work that is not
finished, no restating a lane's report back, no explaining a finding that is already written into
the registry row and the handoff — **the row IS the deliverable; a prose retelling is a second copy
that will go stale**. Do not announce what you are about to do, then do it, then announce that you
did it. Do not re-report a number the operator has already been given.
⚠ **This does NOT license silent failure or thin measurement.** Rigor is unchanged: measure
everything, anchor everything, write the registry rows and the handoff in full. The instruction is
about the CHAT CHANNEL only — put the detail where it persists, not where it scrolls past.
★ Test to apply before emitting: *does the operator have to do something differently because of
this?* If no, it belongs in the row, not in the reply.

★★★ **OPERATOR CORRECTION 2026-08-17, SECOND PASS — THE CATEGORY LIST WAS NOT ENOUGH, BECAUSE THE
LEAK IS NOT *WHICH* ITEMS GET EMITTED, IT IS *HOW*.** Verbatim: *"silent mode is not working. You
need to talk only things I need to know (forks, errors, final reports, etc.), not your own
reasoning."* An emission can sit squarely in category 1 or 3 and still be almost entirely noise,
because the fact arrives wrapped in the reasoning that produced it. **The fact is the payload; the
reasoning is not.**

**FORM, not just category — an allowed emission carries the fact and its measurement, and stops:**
- ⛔ **No significance commentary.** Not *"that changes what delivered means"*, not *"this is worth
  flagging"*, not *"the interesting part is…"*. State the fact; the operator ranks it.
- ⛔ **No meta about the telling.** Not *"I'd rather say it now than at commit time"*, not *"stating
  it plainly rather than burying it"*, not *"before I put it to you"*. Just say it.
- ⛔ **No derivation.** The operator wants the conclusion and the number, not the path. *"886/886"*,
  not the reasoning that made you re-run it.
- ⛔ **No roads not taken.** What you considered and rejected belongs in the row, never in the reply.
- ⛔ **No relaying a lane's report.** A lane's findings go into the registry and the handoff. Emit
  only the part that changes the operator's next action, in your own one line.
- ★ **Length is the tell.** A failure, a done/not-done, or an answer is **1–3 lines**. If it runs
  longer and is not a §B pause gate, the excess is reasoning — cut it, do not compress it.

⚠ **The one exception stays the §B pause gate**, which needs options, trade-offs and a
recommendation. Everything else is a sentence or three.

A one-line cycle summary — priority closed, anchors touched, test delta, commit hash — plus:

```
anchors: opened N, closed M, net ±K — OPEN was <before>, now <after>
next: <one line, matching the top NEXT entry in .plans/_handoff.md>
```

**The anchor line is MANDATORY and carries numbers, not an adjective.** The gate already refuses
`after > before`, so this line is the receipt, not the check — "anchored a few follow-ups" is exactly
what the gate exists to make impossible to say. If the report and the handoff disagree about what
comes next, fix the handoff: it is the one a future reader will find.

## ★★★ USE THE SCRIPT THAT EXISTS — AND FIX IT RATHER THAN ROUTING AROUND IT

**Operator instruction 2026-08-19, verbatim:** *"if a tool has a problem, fix before using again, not
workaround an own tool. reusable tools exists to avoid bunch of problems like mangling or edge cases"*.

- **Look in `references/scripts.md` first.** It indexes every script under `scripts/`, each with its
  purpose. If one covers the job, invoke it — not "something like it" typed inline.
- **A defect in one of them is FIXED in the cycle that hits it.** A workaround at the call site leaves
  the defect for the next caller and forks the behaviour silently. This is a FIX, so by the 2026-08-15
  ruling **no hard stop gates it**, whatever subsystem it lands in.
- ⚠ **The reason is measured, not aesthetic.** These scripts hold this project's accumulated edge
  cases: a `wsl.exe bash -c` with a variable that once became `rsync -a --delete / /` and reported
  exit 0; quoted heredocs eating backslashes; unanchored rsync excludes that silently skipped a
  changed `.cpp`; `command -v` lying over non-interactive ssh on macOS. Re-typing the pipeline inline
  re-opens all of them at once.

### ★★ MANDATORY: a script added, renamed, deleted, or REPURPOSED updates the reference

In the **same commit**, exactly like `.plans/_handoff.md` — a reference that ships one commit late is
a reference the next reader cannot trust. This is enforced rather than asked: each script declares its
purpose once in a `PURPOSE:` line in its own header, both indexes (`scripts/README.md` and
`references/scripts.md`) are generated from those declarations, and the `scripts_index_guard` ctest
entry reds when the tree and the indexes disagree.

```bash
python scripts/check-scripts-index/check-scripts-index.py --write
```

⚠ A new script also inherits the repository's layout, and the guard checks it: one directory per
script named for the script, every sibling implementation inside it (`scripts/<name>/<name>.{sh,ps1,py}`),
assets alongside, and no script loose at the top or buried in a subdirectory.
⚠ **What the guard does NOT check is `.sh`/`.ps1` PAIRING**, and this line used to assert it did —
*"the pairing is a contract, not a courtesy"* — which was false in two directions at once: nothing
enforces it, and eight script directories are Python-only by design. Pairing matters where a
capability must reach the Windows leg, and where it does, the two siblings must not drift — the
guard now refuses a sibling whose `PURPOSE:` contradicts its primary. Full enforcement is
[[D-GATE-SCRIPT-PS1-PAIRING-UNCHECKED]], which is open and measured, not something to assert here.

## ★★★ NEVER CITE A LINE NUMBER — CITE SOMETHING THE FILE CARRIES

**Operator rule, 2026-08-19, verbatim:** *"we must never document line numbers, we must document
method names, comment ids or defined anchors. everything that changes is unreliable. so it's just a
matter of, when finding the path:line, replace the line number by a fixed reference."*

Applies to **every** artifact a cycle writes — registry rows, the handoff, plans, skill references,
commit messages, and code comments alike.

```
✗  src/mir/lowering.cpp  + a line number    <- moves the instant anything above it changes
✓  src/mir/lowering.cpp — lowerCallArgs()
✓  tests/CMakeLists.txt — the `no RUN_SERIAL` rationale block
✓  [[D-TEST-INTEGRATED-FIXED-TEMP-PATH-COLLIDES]]
```

**A symbol survives every edit above it; a line number survives none** — and the failure mode is the
bad one: a citation that BREAKS gets noticed, while one that silently becomes WRONG still resolves,
still reads as evidence, and now points at unrelated prose.

⚠ **✔MEASURED twice inside one cycle (P17), which is why this is a rule and not advice.** Inserting a
one-line header into eighteen scripts moved **16** plan citations off their subjects. The rows then
written to RECORD that defect shipped **three more** wrong numbers of their own, each naming the
first line of an explanatory comment instead of the code it explained. Independent audit caught both;
no gate saw either.

**Enforced** by `plan_citations_guard` (ctest) over `.plans/**` and `.claude/**` as a **ratchet** —
the ~2365 pre-existing citations sit in a per-document inventory whose ceilings may only come
**DOWN**. A new one reds immediately; converting one reds until its ceiling is lowered in the same
commit, because unclaimed headroom is where the next one hides.

```bash
python scripts/check-plan-citations/check-plan-citations.py --write
```

⚠ **Green there means no NEW positional citation landed — never that the plans cite stably.** The
inventory is DEBT: burn it down in whatever document you are already editing.

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
  the operator, not only to the code, and it opens with the **never-cite-a-line-number** rule.
- Read `references/scripts.md` **before writing any script, probe, or one-off shell pipeline** —
  the index of every script this repository already ships, each with its purpose. Most of what a
  cycle needs is already there, and re-typing it inline re-opens the edge cases it was taught
  (`wsl.exe` quoting, heredocs eating backslashes, unanchored rsync excludes, ssh dropping PATH).
- Read `references/worktrees.md` before any byte-changing measurement or agent worktree operation.
- Read `references/build-layout.md` before creating ANY build tree (step 5) and before reporting a
  cycle complete (step 11) — **one root `build/`, subdirectories for distinct builds, and lane builds
  cleared once the gate covering them is green.** Operator instruction 2026-08-17; a surviving
  `build/lane-*` blocks the completion report the same way the anchor-balance gate does.

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
