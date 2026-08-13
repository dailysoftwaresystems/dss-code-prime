# Turning findings into action + the test-hardening ratchet

## H. Prompt-authoring — turning a finding into action

A finding nobody can act on is commentary. For every green-but-rule-breaking or unverified item, the
auditor emits a **ready-to-paste implementer prompt** (handed to `dss-cycle` / the loop / the user).
The proven shape:

1. **The non-negotiables**, numbered — the specific correctness/agnosticism requirements, each with
   *why* (so the implementer can't satisfy the letter and miss the intent).
2. **Prove-don't-assert gates** — the test that must go **red** if the thing is wrong (red-on-disable
   for guards; effectiveness assertion for optimizations; non-default-through-the-wire for config),
   *demonstrated*, not claimed.
3. **An explicit closure gate** — "Do NOT mark `D-*` closed until (a) … (b) … (c) all CI legs green."
4. **The standing rules**, restated for re-affirmation — best-long-term / no-workaround / source-target-linker agnostic /
   fail-loud — and a note that any new shared-code path must be config-driven.
5. **Cross-platform reminder** when the change is encoding/include-heavy.

Anchor the prompt in the *catch*: name the silent failure it prevents (e.g. "ConstFold folds the
op away → the optimized arm tests nothing", "the guard exists but no test makes it matter") so the
implementer understands the failure mode, not just the task.

---

## J. The test-hardening ratchet — feeding gaps back into `dss-cycle`

A weak test caught once is a finding; a weak-test *class* caught once will recur every cycle until
the implementer's standing instructions forbid it. The per-instance repair (§G flag + §H prompt)
fixes *this* cycle; the ratchet fixes *every future* cycle by hardening `dss-cycle`'s own
`SKILL.md`. This is the only place the auditor touches the implementer — and it edits its
*instructions*, never its code, tests, or config.

**When it fires — evidence-gated, never speculative.** Only from a concrete gap *observed in the
audited delta*, of one of these classes:

- **No real-execution proof.** A closure rests on a unit/inspection test where an *integration /
  differential (run-and-diff) / corpus* test is the only thing that proves the behaviour end-to-end
  (e.g. codegen "verified" by reading MIR but never actually executed).
- **No hardening.** A mechanism whose failure would be silent ships with no fail-loud negative test
  / death-test / **red-on-disable** pin (§E #4, §F correctness-critical).
- **Proof that proves nothing.** Effectiveness-masking (§E #2), the knob that lies (§E #3), or an
  assertion weaker than the strongest provable property (§A.5) — a test that stays green on a
  silently-broken impl.
- **Not** an honestly-named, trigger-gated deferral — those are legit (§F). Never ratchet an honest
  pin; ratchet only an *unflagged* gap.

**The trigger question:** *would a careful implementer following `dss-cycle`'s current `SKILL.md`
have known to write the stronger test?*

- The instruction that would have prevented it is **absent or too weak** → **ratchet** (this section).
- The instruction already exists and was simply **ignored** → that is an implementer-discipline
  finding for the verdict + prompt (§H), **not** a skill edit. Do not paper over non-compliance by
  rewriting an already-correct rule.

**What the ratchet does — upgrade-in-place first:**

1. **Prefer strengthening an existing `dss-cycle` instruction** over adding a new one — tighten the
   wording, add the missing gate, name the newly-seen failure mode. Proliferating near-duplicate
   checklist items rots the skill; a sharper existing rule is better long-term. Add a *new*
   instruction only when no existing one covers the class.
2. **Anchor it in the catch.** The new/strengthened text names the *silent failure it prevents*,
   with the concrete example from the cycle that motivated it (same discipline as §H) — so the
   implementer internalises the *why*, not a box to tick.
3. **Make it prove-don't-assert.** It must demand the test that goes **red** when the thing is wrong
   (red-on-disable / effectiveness assertion / non-default-driven-through-the-wire), never merely
   "add a test".
4. **Keep the creed.** The instruction itself must honour best-long-term / no-workaround /
   source-target-linker agnostic / fail-loud, and must never bake a target- or format-specific test
   requirement into a *universal* rule.

**Authority + how to land it — a bounded extension of §I:**

- **Separate, clearly-tagged commit**, never commingled with a verdict or a plan-hygiene fix:
  `docs(audit): ratchet dss-cycle test discipline — <class> (from <cycle>/<finding>)`.
- **Surface it in the verdict** under a `Ratchet applied` line (§G) so the human sees exactly which
  implementer instruction changed and can veto — it is reversible markdown.
- **Unambiguous class → apply it. Borderline → propose, don't commit.** If it is genuinely arguable
  whether this is a recurring class (vs a one-off), do *not* edit the skill — quote the exact
  instruction text you *would* add, in the verdict, and let the human apply it. Same rule as §I: when
  unsure, it's a judgment call — report, don't commit.
- **Touch only `dss-cycle`'s standing test-discipline prose** — never its run mechanics, anchors, or
  anything else — and **never while the loop might be reading/writing that file** (race; flag in the
  verdict instead, exactly as §I).

This closes the loop: the auditor doesn't just *catch* lazy tests — it **raises the floor** so the
same laziness is structurally impossible next cycle.

---
