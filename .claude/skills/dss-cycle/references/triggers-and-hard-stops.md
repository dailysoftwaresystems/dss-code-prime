# Triggers, §B gates and hard stops

When a trigger, a §B gate or a hard stop genuinely stops the cycle, and when it does not. The
decision gate itself — what the agent decides, how it reports, and the cases that still pause the
loop — is in `SKILL.md`; the pause-and-ask gate it amended on 2026-09-21 is kept below, whole.

## Contents
- The pause-and-ask gate as written before 2026-09-21 — AMENDED 2026-09-21 (the current rule is the
  decision gate in `SKILL.md`)
- Triggers
  - A §B trigger is a predicate, not a ritual (operator ruling 2026-08-17)
  - A trigger waiting on a component WE build is not a gate (operator ruling 2026-08-26)
- Hard stops — always route through the pause gate: a hard stop gates opening a capability, never
  fixing a defect (operator ruling 2026-08-15) · OPT7 / inlining · trigger-gated anchors ·
  correctness-critical anchors · a red CI leg

## The pause-and-ask gate as written before 2026-09-21 — AMENDED 2026-09-21

The operator's ruling of 2026-09-21 — *"you do everything. I'm not your babysitter."* — amends it: forks,
meaning forks included, documented-behaviour changes and a new engine mechanism at step 4 are decided by
the agent, written into the row and reported veto-able. The gates' three escape hatches — a deferral,
carrying a net-open rise, growing a ratchet baseline — stay the operator's, because standing orders
govern them (close, do not file; no follow-ups; a ratchet only comes down). The loop pauses, with one
crisp question, for five cases: what no measurement can answer and no standing order covers; the OPT7 /
inlining hard stop; a correctness-critical anchor whose negative miscompile-pin cannot be constructed;
those escape hatches; review passes that never converge. The execution stops are unchanged. The current
rule is the decision gate in `SKILL.md`.

### The pause-and-ask gate — the most important behavioral rule

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

## Triggers

### ★★★ A §B TRIGGER IS A PREDICATE, NOT A RITUAL (operator ruling 2026-08-17)

A row pinned "§B — operator decision" is gated on a **stated reason**. If a lane MEASURES that
reason to be FALSE, the §B **was never triggered** — discharging it is not closing a §B on the
lane's own authority, it is discovering the gate does not apply.

**The rule, and all three clauses are load-bearing:**
- every §B row states its trigger as a **testable predicate**, not as a mood;
- a lane MAY discharge a §B by **measuring the predicate false**, provided it records the
  measurement **in the row** and **flags it in the cycle report** so the operator can veto;
- an **INCONCLUSIVE** measurement escalates. **Silence is never a discharge.**

✔The case that produced this ruling: bare `asm` was pinned §B because the spelling
"needs a new standard-mode axis". A lane measured that DSS already declares GNU mode in the
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

### ★★★★ A TRIGGER WAITING ON A COMPONENT *WE* BUILD IS NOT A GATE (operator ruling 2026-08-26)

> *"if C has any intrinsic, good time to build the intrinsic then the OPT"* … *"if [it] needs
> something also to be built that can be built, build the one that can be built, then the
> [gated row]"*

Said while overruling the two rows a cycle had defended as *"legitimately gated"*.

**The distinction, and it silently re-verdicts a large slice of the registry's ⏳ rows —
classify every gated row by WHO OWNS THE TRIGGER:**
- **Outside us** → genuinely gated: an operator decision, a profile from a real workload, an
  upstream project, a hardware platform we do not have.
- **A component of THIS compiler** → **NOT gated.** It is a two-part task that was written down
  as one part. **Build the prerequisite, then close the row — in the same cycle.**

⚠ **§E#5 (don't build a consumer-less mechanism) DOES NOT APPLY when the missing consumer is
itself something we are supposed to build.** That is the exact misreading this ruling corrects.
The test is still *"does a consumer exist?"* — what changed is that **a consumer we are committed
to building counts**, so the honest response is to build it now, not to record that it is absent.
⇒ And filing the prerequisite as its own new row is the follow-up habit wearing a different hat.

★ **Prefer a prerequisite that unblocks MORE THAN ONE row, and name which when you pick it.**
✔The case that produced the ruling: an escape-analysis / points-to substrate for `mirMayAlias` is
the stated trigger for the MemorySSA walk-past-precision row **and** the stated prerequisite for
clause (c) of the **OPT7 / inlining** legality gate. One substrate, two rows.

⚠ **CHECK THE TREE BEFORE CALLING THE PREREQUISITE MISSING.** ✔MEASURED 2026-08-26: DSS's HIR
intrinsic registry was about to be described as absent. It EXISTS — `HirIntrinsicRegistry`,
`Hir::intrinsicRegistry()`, `makeIntrinsicCall`, and a shipped `umulh`
builtin-intrinsic node. Only the *routing* of a shipped C construct through it is missing, which
is a far smaller job than the row implied.

⚠ **AND THE MIRROR-IMAGE ERROR, corrected by the operator the SAME DAY: scheduling lives in the
PLANS, not in the tree.** A cycle concluded from `src/dss-config/targets/` holding only
little-endian `x86_64` and `arm64` that the big-endian trigger *"had not fired"* — while it is
operator-sequenced as plan 23 **FC19** with a toolchain verified by execution. ★ **The absence of
a thing in the tree is not evidence that it is unscheduled.** Read a trigger predicate against
the plans AND the tree, never the tree alone.

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

- **OPT7 / inlining** (`G-406`, plus its cross-CU sub-anchor) — first inter-procedural pass, touches
  linkage / DCE / cross-CU legality. A supervised cycle; **never open autonomously.**
  ⇒ **Gated: opening the arc.** ⇒ **NOT gated: fixing a defect in inlining that already ships**,
  per the ruling above.
- **Trigger-gated anchors** — NOT a TODO. "Do not build until the trigger fires." If it has not
  fired, skip and report "trigger not fired". Backlog ordering is sequencing guidance, not a closure
  license.
- **Correctness-critical anchors** (silent-miscompile class) — the closing cycle MUST ship a negative
  miscompile-pin that breaks iff the transform mis-fires. If the pin cannot be constructed, STOP and
  bring a decision brief. Never ship on review alone.
- ★★★ **A RED CI LEG — read at step 0 with `dssharness check-ci-legs`.** It stops the cycle from
  picking new work until it is diagnosed, and since repairing it is a FIX, **no other hard stop
  applies to the repair itself**. ⛔ The stop is on *proceeding past it*, never on fixing it, and
  **never** on touching the operator's PR: do not label, re-run or push to re-trigger CI.
  ⚠ **The local gate cannot substitute for it, and assuming otherwise is the defect.** ✔MEASURED
  2026-09-14: the two legs that were red run in configurations **no local leg builds** — the local
  Windows gate is **MinGW GCC**, so every `deps = msvc` fact is invisible to it, and the two SSH
  legs (macOS, arm64 VPS) skip `-LE repo-guard`, so a guard-only defect is invisible to THEM.
  ⚠ **WSL was NOT in that set** — ✔MEASURED 2026-09-16, the WSL driver ran every guard while the
  two ssh drivers excluded the label; the counts said so (Windows 2207, WSL 2206, macOS/VPS 2167). SUPERSEDED 2026-09-24 by the eight declared legs — windows-x86_64-release now builds with MSVC locally, and the repo-guard label runs on the two Windows legs while every remote leg, WSL included, excludes it through remoteExcludes (see leg-hosts.md)
  ★★★ **THE DURABLE RULE: a guard is HOST-INDEPENDENT — it reads the TREE — so which legs execute
  it is a CONFIGURATION decision, and a leg that skips it cannot see a guard-only defect.**
  **Re-derive the set from the run in front of you**, never from this page: the divergence above
  existed precisely because three programs each answered the question separately.
  ⚠ Read the count from `dssharness test --legs windows-x86_64-debug --label repo-guard` — the leg's
  ledger counts the tests the label ran — or from that commit's own
  `repo-guard label applied to N test(s)` configure line — never from here.
