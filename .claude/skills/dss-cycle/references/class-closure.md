# Class closure — a defect is closed when its CLASS is closed, at its ONE HOME

Operator, 2026-11-03, after cycle P69 round 1 had run 35 checkpoints: the project does not converge because
fixed defects keep coming back as their neighbours, and every review finds another. The cause is in how fixes
are made, not in how many are made. This file is the procedure that makes a fix stay fixed.

## Contents
- What was measured
- The rule: invariant, home, site list, universe
- The five obligations, in order
- The class-closure record (what a hand-back carries)
- Recurrence: the home audit
- How review judges a closure
- What this does not change

## What was measured

✔MEASURED over the 35 checkpoints of P69 round 1 (`git log <base>..HEAD`, subjects and touched paths):
- one theme in 12 of 35 subjects (symbol aliases, weak definitions, select-any copies), fixed in four separate
  checkpoints; bit-fields in three (a type computed per tier, then "one type in every tier"); tentative and common
  definitions in four or more; "every filesystem question goes through the substrate" said twice, for two hosts;
- a row closed in one wave and refuted by a measurement in a later one, because the fix covered the construct as
  written and not the construct as an optimizer emits it;
- one header edited 22 times and four sources 10–14 times in one round: the places where one decision is taken
  by many callers.

Each fix was correct for its instance. The defects came back because the DECISION was made in several places
and each fix reached the place where it had been seen. A review then found the next place. Passing more reviews
does not stop this; deciding in one place does.

## The rule: invariant, home, site list, universe

- **Invariant.** One sentence, shaped *"X is decided by Y for every Z"*. A defect is an instance of a violated
  invariant; if you cannot write the sentence, you have not understood the defect yet.
- **Home.** The ONE function, table or config document that decides it. If no such place exists, creating it IS
  the fix. (Agnosticism already says where it lives: config vocabulary read by a universal algorithm.)
- **Site list.** Every place that decides it today: path and symbol, found by searching the DATA (the kind enum,
  the schema key, the diagnostic, the verb) across every tier from parse to encode to link, every object format,
  every target, both examples runners and the schema documents — not by searching the file the defect was seen in.
- **Universe.** What Z ranges over, written as a product of vocabulary the repository already declares: formats ×
  targets × tiers × optimization pipelines × the forms of the construct (declare / define / reference, winner /
  loser, read / write, with and without the qualifier) × both directions (accepted / refused).

## The five obligations, in order

1. **Write the invariant, the home and the site list BEFORE editing.** The plan (step 3) carries them; the design
   audit (step 4) searches for sites the plan missed and refuses a plan whose list is short.
2. **One site → fix it there. More than one → CONSOLIDATE.** One home decides, every other site reads it, the
   duplicates are deleted in the same wave. Patching one site of several, or all of them in parallel, is refused:
   a duplicate is a future miss. (Multi-site contracts, `the-bar.md` §A.5: funnel through one chokepoint.)
3. **The pin covers the universe, table-driven from the config vocabulary.** A test of the reported case alone
   is an instance pin and does not close the row. Drive the pin over the vocabulary (every format, every target,
   every form) so a vocabulary row added later is covered the day it lands. The arm
   (`mutation-arms.md`) mutates the HOME, and the exact red set is the universe's cells.
4. **Neighbour sweep before the hand-back.** For the changed home, measure the neighbours on DSS AND on each
   reference separately: the same invariant at the other cells of the universe, the opposite direction, and the
   optimized form of the same source (the release pipeline, `cl /O2`, `gcc -O2`: an optimizer's output is a
   different input). Every neighbour that fails is fixed in this wave — disclosed is not closed — and a cell that
   cannot be measured is NAMED unmeasured with the reason. The reviewer is not the one who finds neighbours.
5. **The closing claim states the universe.** "Invariant I holds over U; cells C measured on references R; cells N
   not measured because B." A closing that names a case instead of a universe is an instance closure; the row does
   not close.

## The class-closure record (what a hand-back carries, per fixed row)

Four lines, in the lane's findings file and in the hand-back:
- invariant · home (path, symbol);
- site list: each site before and after (consolidated, deleted, or "the only one" with the search that proves it);
- universe and which cells the pin and the arm cover;
- neighbour sweep: each neighbour, its measurement, its fix.

The fold refuses a row whose record is missing or whose site list was not searched by data. A lane that cannot
fill a line says which one and why, to the orchestrator, before it builds.

## Recurrence: the home audit

When the same home (or the same theme) is touched a second time in one PR or round, the next edit to it is
preceded by a HOME AUDIT, not a patch:
- find it: `git log <base>..HEAD --name-only -- <path>` counted per path, and the registry rows by theme; a path
  edited five or more times in a round is a hotspot;
- read the whole home and every consumer; write down the invariants it should uphold and the ones it upholds only
  by coincidence;
- the work is the design that makes them structural (one decision, one place), delivered with the fix;
- the report says "home audit" and names what was consolidated.

## How review judges a closure

The reviewer receives the class-closure record with the diff and judges in this order:
1. is the site list complete? (search the data independently, not the lane's files);
2. is the universe complete, and is the pin table-driven over it?;
3. the diff itself.

Every finding is classified, and the class decides the route:
- **(a) a defect the diff introduced** and **(b) another instance of a class the diff claims to close** — SEND BACK;
  (b) is counted as "class reopened" in the report and the order is "run the neighbour sweep for the whole class",
  never "fix this finding". One send-back per class is the budget: a second one on the same class means step 1
  was wrong, and the next action is the home audit.
- **(c) a pre-existing defect of another class in code the diff does not touch** — it does NOT hold the fold. It
  enters the registry through the normal door with a measured size (the files, what must be built, what must be
  measured first) and is the FIRST row of the lane's next wave, brief written with this procedure. The fold lands
  and is committed and pushed; reported with its numbers, veto-able.

## What this does not change

Disclosed is not closed; no follow-ups; the bar; every standing order. A defect met while closing a row, in the
files the wave is working, is still fixed in this wave. This file only fixes WHERE the fix goes (the one home) and
WHAT counts as done (the universe), and stops a review from holding a fold hostage to code the fold never touched.
