# Output contract — silence is the default

What the cycle may emit in chat, in what form, and the final report's shape. `SKILL.md` carries the
six categories and the report template; this is the full text.

## Contents
- Silence is the default (operator instruction 2026-08-17) — the complete list of what is worth
  emitting, including a DssHarness finding (operator ruling 2026-09-16)
- Everything else is noise — and the form an allowed emission takes (second pass, 2026-08-17)
- The final report and its mandatory anchor line

## Output contract

★★★ **OPERATOR INSTRUCTION 2026-08-17 — SILENCE IS THE DEFAULT. EMIT NOTHING THE OPERATOR DOES NOT
NEED IN ORDER TO PROCEED.** Verbatim: *"requires no output tokens unless what I need to know to
proceed (failures, done/not done, final report, etc)"*.

**The complete list of things worth emitting:**
1. **A failure or a blocker** — something is red, refused, or cannot proceed. State it, with the
   measurement, and what you are doing about it.
2. **A pause gate** — a decision only the operator can make (§B, a pending definition, an unfired
   trigger, a hard stop). This is the one case where length is justified: options, trade-offs,
   recommendation. AMENDED 2026-09-21 by "you do everything. I'm not your babysitter." — a fork is decided by the agent and reported veto-able in this same shape, the decision and why in place of a recommendation, and the agent pauses only for the cases of the decision gate, each with one crisp question (see SKILL.md)
3. **Done / not done** — a step's terminal state, when the operator's next action depends on it.
4. **The final report** — the output contract below.
5. **A direct answer to a direct question.**
6. ★★★ **A DssHarness finding** (operator ruling, 2026-09-16: *"please also put in dss-cycle skill
   that any issue found in DssHarness must be reported to me (the operator)"*). AMENDED 2026-09-21 by "you do everything. I'm not your babysitter." — the agent SENDS the report to the repo-harness session with SendMessage and tells the operator it was sent, never asking the operator to relay it (see dss-harness.md) **Emit it even when
   it neither fails nor blocks this cycle** — which is the usual case, because the cycle routes
   around a tool defect by using the script that still exists, and the finding then matches none of
   categories 1–5 and dies in a lane report. SUPERSEDED 2026-09-24 by *build, sync, test and worktree management go through DssHarness* — a DssHarness BUG is the one exception, and a route around it lives only as long as the bug, is named as such, and is retired the day the fix ships (see dss-harness.md) That silence is what this item closes.
   - **Same FORM as everything else:** what was run, what happened, what should have happened, and
     the exact source symbol in repo-harness that produces it — **path + SYMBOL, never a line
     number**. A few lines. No significance commentary, no derivation.
   - **Report it in the cycle that FINDS it**, never filed for later — the same shape as *fix it when
     you face it*.
   - **A DssHarness defect does NOT become a `D-*` row here**; it is not this repository's defect.
     **Its route out is the report the agent SENDS to the repo-harness session, and the cycle report's
     line saying it was sent**, which is why this item has to exist at all. That session is found by
     listing the local sessions and picking the one working in the repo-harness checkout.
   - ⚠ **Reporting does not replace fixing our side.** Where the defect has a correct LOCAL
     expression that is not a workaround — spelling a `minVersion` with three components because the
     tool documents semantic versions, say — fix it here AND report the tool's part. Not alternatives.

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
recommendation. Everything else is a sentence or three. AMENDED 2026-09-21 by the decision gate — the exception is the decision brief, reported veto-able: options, trade-offs, the decision and why (see SKILL.md)

A one-line cycle summary — priority closed, anchors touched, test delta, commit hash — plus:

```
anchors: opened N, closed M, net ±K — OPEN was <before>, now <after>
next: <one line, matching the top NEXT entry in .plans/_handoff.md>
```

**The anchor line is MANDATORY and carries numbers, not an adjective.** The gate already refuses
`after > before`, so this line is the receipt, not the check — "anchored a few follow-ups" is exactly
what the gate exists to make impossible to say. If the report and the handoff disagree about what
comes next, fix the handoff: it is the one a future reader will find.
