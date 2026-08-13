# The bar, the cardinal rule, the audit pass, and cross-session honesty — full text

## A. The bar it judges against — NON-NEGOTIABLE

These are the same six non-negotiables `dss-cycle` builds to (§A there). The auditor's job is to
**confirm each held, by evidence** — not to trust that it did.

1. **Source / target / linker agnostic.** No identity branch in shared substrate — never
   `if (schema.name() == "...")`, `if (arch == "...")`, `if (format == "...")`. Vocabulary is
   config-driven (`.lang.json` / `.target.json` / `.format.json`); the engine walks a closed verb
   set. The auditor watches especially for the **subtle** forms (§E) — a hardcode does not have to
   be an `if`-on-identity to break the rule.
2. **Best long-term solution, no workarounds.** The complete clean solution, not a "tight slice".
   A real blocker must be **named and pinned**, never silently deferred. Note the *other* direction
   too: speculatively building a trigger-gated deferral is *also* a no-workaround violation (§F).
3. **No follow-ups for the hard part.** The difficult core lands in its own cycle unless a *genuine*
   named blocker or unfired trigger stops it. "It was getting big" is not a blocker.
4. **Fail loud.** Every unsupported construct emits a real diagnostic; no silent miscompile, no
   swallowed error. `*Fatal` + `X_*` / `D-*` patterns.
5. **Strict-assertion tests.** Every test asserts the strongest provable property (exact counts,
   full-sequence / byte equality, `static_assert`, death-test message match). A test that still
   passes when the implementation is silently broken is **not** strict enough (§E catalogues the
   ways this hides).
6. **The full gate held:** build green · full ctest green · anchor-registry guard OK · agnosticism
   scan clean · review folded · **and all CI legs green, not just local** (§F).

**The auditor's own meta-rule:** it holds itself to the same standard — it never reports "clean"
on something it has not independently verified (§B).

---

## B. The cardinal rule — green ≠ clean

> **A passing build and a green test suite are necessary, never sufficient. "Clean" requires the
> auditor to have independently re-run or inspected the evidence — not trusted the implementer's
> closure report.**

This is the difference between an audit and a rubber stamp. Throughout the cycles this skill
codifies, the green-build masked: a dead config wire (the knob that lied), an effectiveness pin that
proved nothing because the optimized arm folded the work away, a regalloc clobber mechanism with no
behavior test, a conservative-default hardcode that broke agnosticism "a tiny bit", and a
cross-platform CI break invisible to the local MSVC build. Every one passed `ctest`.

So every verdict (§G) **separates two lists**:

- **VERIFIED-CLEAN** — the auditor re-ran/inspected the evidence and it holds.
- **GREEN-BUT-RULE-BREAKING** — passes the mechanical gate, violates the bar (§A) or a guardrail
  (§F). These are regressions to fix, *not* closed items, no matter how green.

If the auditor could not verify an item (e.g. CI legs it can't run locally, §K), it says
**"unverified"** explicitly — never rounds it up to clean.

---

## C. The audit pass — the steps

### Step 0 — Orient & record the baseline
- `git branch --show-current`, `git log --oneline -8`, `git status -s`, and remote sync
  (`git rev-list --left-right --count origin/<branch>...HEAD`).
- Establish the **diff window**: an explicit baseline argument, else the last audited commit, else
  the prior cycle tip. The audit examines `git log <baseline>..HEAD`.
- If a baseline is being *recorded for a future audit* (e.g. before a `/loop` run), capture it
  explicitly (§G) so the next pass has a precise "from".
- **Dirty tree = work in flight.** Do not run the full battery on a mid-edit tree (it may be red
  for innocent reasons). Inspect the WIP read-only (§D) and report "in flight", not a verdict.

### Step 1 — Run the verification battery (§D)
Build, full ctest, anchor-registry guard, agnosticism scan. This re-establishes ground truth from
*the auditor's own run*, not the report.

### Step 2 — Apply the rule-lens (§E)
Read the diff for the **subtle** violations the green gate cannot see. This is the heart of the
audit — the catalogue in §E is the checklist.

### Step 3 — Enforce the guardrails (§F)
Speculative trigger-gated closure, the OPT7 hard stop, correctness-critical miscompile-pins, red
pushes, CI legs. These are go/no-go.

### Step 4 — Hunt silent gaps (and ratchet the recurring ones)
For each *claimed* closure, ask: *what would I see if this were silently broken, and did the test
suite force that to surface?* If the answer is "the test would still pass" → the closure is
asserted, not proven (§E, "prove-don't-assert"). When the weakness is not a one-off but a *class the
implementer's own standing instructions failed to force* — a missing integration/differential/corpus
proof, absent hardening, or a test that stays green on a silently-broken impl — flag the instance
(§H) **and** ratchet `dss-cycle` itself (§J), so the class cannot recur next cycle.

### Step 5 — Verdict (+ prompt)
Render the verdict in the §G shape (verified-clean vs green-but-rule-breaking vs unverified). For
every green-but-rule-breaking or unverified item, author the implementer prompt that closes it (§H).

### Pre-build variant — the plan-lock design review (`dss-cycle` Step 3.5)
The steps above audit *committed code*; the same bar also judges a **plan before it is built** — the
gate `dss-cycle` runs at its Step 3.5, and the mode this skill applied to the linkage P1+P2 plan
(2026-06-04). What changes:

- **No battery.** Nothing is built, so §D (build/ctest) does not apply. Apply the bar (§A), the
  rule-lens (§E), and the guardrails (§F) **to the plan**: *would this design, built faithfully,
  hold the line?* A planned `if (lang/arch/format == …)`, a tight-slice dressed as "phase 1", a
  speculative trigger-gated build, a silent-ignore, a "just add a test" where the strongest provable
  pin is required, an OPT7 / hard-stop crossing — each is a finding **now**.
- **Verify the premises, don't trust the narrative** (§B, applied to a design). Independently confirm
  the plan's load-bearing claims against the *actual code* — the vocabulary it reuses, the stub it
  says it closes, the precedent it leans on, the deferral anchor it cites. A grounded GO beats a
  plausible one.
- **Render a design-review GO / findings**, never a verified-clean verdict (nothing is built). State
  the **closure gates** the built result must satisfy (§H shape) so the implementation is bound to them.
- **Independence is preserved.** Judging a plan against the bar is **not authoring it**: the
  post-build audit stays fully independent and re-verifies the built artifacts from scratch — a GO on
  the plan buys the *code* no trust (green is never clean until re-run). In an autonomous loop the
  plan review runs as an *independent* reviewer (a fresh subagent), distinct from the post-build
  auditor, so neither rubber-stamps the other.

---

## K. Cross-session honesty — what the auditor can and cannot do

- It audits **output** — committed and pushed artifacts (git, plans, tests). It **cannot** watch a
  separate live session's reasoning, intercept a commit before it lands, or **stop** another session's
  loop. There is no cross-session control channel; the agent tools reach only subagents this skill
  spawns, not an independent loop. The loop's own stop-on-red gate is the real-time brake — the
  auditor is the after-the-fact check on whether it held.
- **CI legs it cannot run locally** (macOS clang, Linux GCC) it marks **unverified** and says how to
  confirm (`gh run list`) — it never reports green it did not observe. The local build is one platform;
  the cross-platform blind spot (§E #7) is real and shared.
- Periodic auditing while a human is away ≈ one thorough audit on their return, in *outcome* — because
  detection without the ability to act or relay changes nothing until they read it. Prefer one rigorous
  pass over polling-theater; offer a timestamped trail only if the human explicitly wants faster triage,
  and be honest it is a log, not a hand on the wheel.

---
