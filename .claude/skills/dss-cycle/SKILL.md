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

## ★★★ PRODUCTION ANCHORS ARE THE PRIORITY, ALWAYS — operator ruling 2026-08-25

> *"the priority is always production anchors. ALWAYS. harness we fix as we need when we face the
> problem (NEVER LATER)."*

The registry is **two files** since 2026-08-25, split at the operator's instruction (*"the priority
is real errors, not cosmetics"*):

| file | holds |
|---|---|
| `.plans/_deferred-anchor-registry-production.md` | a defect **a user of the compiler could hit** — in the shipped binary, or in the config it reads |
| `.plans/_deferred-anchor-registry-harness.md` | a defect **only we can hit** — tests, gates, guards, cycle machinery, plans, scripts, carriages, CI |

⚠ A row's bucket follows the **DEFECT, never the instrument that found it**. `D-CONFIG-*` and
`D-DIAG-*` are PRODUCTION deliberately: in this architecture a `.lang/.target/.format.json` document
IS the compiler's behaviour, and a diagnostic IS its output to a user.

**How the ruling binds this loop, clause by clause:**

1. **Step 1 picks from PRODUCTION.** A harness row is never picked *because it is next*. If §0.1 is
   dry, promote an eligible **production** anchor. `scripts/burndown-queue/burndown-queue.py`
   already bands production errors highest — the ruling makes the FILE the outer sort key, above
   any band.
2. **"NEVER LATER" is the load-bearing half.** A harness defect is fixed **at the moment it is
   faced** — this cycle, in the lane that hit it, as part of that lane's work. A gate that lies, a
   guard blind to its subject, a script that blocks the work in front of you: fix it NOW. **Filing
   it and routing around it is exactly the failure this ruling names.**
3. **The harness registry is a RECORD, not a backlog.** It is drained by encounter, not by
   scheduling. A harness row is normally written already ✅ CLOSED, naming a fix that landed in the
   same cycle.
4. **Still file the row.** *Anchor every issue found* is not repealed — a harness defect fixed
   silently teaches nobody, and the row is what makes the fix auditable. This ruling governs what
   gets **SCHEDULED**, not what gets **RECORDED**.
5. **Step 2's "clear blockers FIRST" is unchanged, and is now the ONLY route a harness row takes
   into a cycle** — it is worked because it BLOCKS the production priority, never on its own ticket.
6. **The cycle report states production movement SEPARATELY**, because a single total cannot answer
   the question the operator is actually asking. ★ A cycle whose closures are all harness rows has
   hardened the workshop and shipped nothing; say so plainly rather than letting a healthy total
   imply otherwise.

⚠ **Do not quote a per-bucket count from here or from the handoff — re-derive it.** ✔MEASURED
2026-08-25, and the correction is the reason this warning is here: the P34 handoff's own
"475 production OPEN" was wrong by 20, caught only by cross-checking the per-bucket split against
`check-anchor-balance`'s registry total. The instrument, which reuses that gate's own row scanner
rather than re-typing the "is this row open" vocabulary:

```
python scripts/check-anchor-balance/check-anchor-balance.py --breakdown --denominator registry
```

⚠ That gate canonicalises BOTH registry files to ONE key — deliberately, so that MOVING a row
between buckets is correctly a no-op — so its breakdown gives the registry TOTAL, and a per-bucket
split must sum to it. ★ **That sum is the cross-check that catches a mis-bucketed or double-counted
row, and it is the only reason the P34 error surfaced at all.**
## ★★★ A GATE HOST HOLDS THE REPO AND NOTHING ELSE — operator ruling 2026-08-25

*"keep macos and vps linux arm64 updated with our repo files, and free of stale files/worktrees.
you own the cleanup."*

**The remote checkout is not a place work accumulates. It is a MIRROR of the tree under test,
and the cycle owns keeping it one.** A leg that runs against a host holding anything else is
not testing the tree it reports on.

⚠ **✔MEASURED 2026-08-25 (cycle P34), and it produced a RED that looked like a code defect:**
the macOS host held **16,312 files against a local 6,660**. `--push` is a `tar` extract and
**tar extraction never deletes**, so that tree was the UNION of every tree ever pushed —
including `.plans/_deferred-anchor-registry.md`, deleted locally in the same cycle and still
sitting there at 6.7 MB. `plan_citations_guard` counted **4908 citations across 213 documents**
where the live tree has **2853 across 212**, and reddened. The identical guard was `rc=0`
locally. ★ **The failure named the guard, and the guard was innocent** — hours can go into a
subject that was never wrong, because a stale remote file is invisible from the driver.

⚠ **AND 9,638 OF THOSE FILES WERE `.claude/worktrees/**` — A FULL COPY OF THE REPO PER LIVE
AGENT, shipped to the gate host on every push.** That is not merely transport cost: the
examples runner **globs `examples/<lang>/*`**, and a worktree carries its own `examples/`
tree, so a gate host holding one can run a corpus belonging to somebody's uncommitted lane
and report the result as the cycle's.

**The three rules:**
- **A push is a SYNC, never an accumulation.** `ssh-macos.{sh,ps1}` take `--prune`/`-Prune`
  and `macos-leg` passes it; the VPS path already had `rsync --delete`. A transport that only
  adds is a transport that silently diverges.
- **Worktrees are excluded at the transport, on BOTH carriages.** An agent worktree never
  belongs on a gate host. ⓘ rsync does NOT delete excluded paths, so adding the exclude does
  not clean a host that already holds one — that needs an explicit removal, once.
- **The cleanup is the CYCLE's job, not a thing to notice later.** Before a leg is trusted,
  the host holds the repo and nothing else.

★ **The general form, which is the part worth carrying: ask what the remote tree IS, not what
you last sent it.** Every reasoning error here came from thinking about the PUSH — "I sent the
right files" — when the question is what the far side now CONTAINS. The same distinction that
makes `git status` worth reading after a merge you are sure about.

⚠ **This narrows, and does not repeal, the standing order against cleaning those hosts.** No
`git clean`, no `reset --hard`, no `checkout --` on either machine unless the operator names it
(`macos-leg --reset-to` stays opt-in for exactly that reason). What is authorised is removing
what the repo does not have: stale files and worktrees.
⇒ ★★★ **THE OPERATOR NAMED IT ON 2026-08-26. Read the next section — restoring a leg clone is
now REQUIRED where this paragraph once forbade it, and `scripts/leg-tree/` is the only thing
that may do it.**

## ★★★ EVERY LEG HOST KEEPS A CLONE, AND THE LEG CLEANS UP AFTER ITSELF — operator ruling 2026-08-26

> *"we should use already cloned repo in each leg [...] you can keep using the sync process you
> already use, but CLEAN UP the changes after you finish them (you can also use worktree in leg
> host if needed for parallel legs, clean up also needed). [...] don't forget to check each leg
> branch before working in it, and also clean up the changes after finished. that's the standard"*

| host | leg repository |
|---|---|
| WSL | `~/src/dss-code-prime` |
| arm64 VPS | `~/src/Github/dss-code-prime` |
| macOS | `~/src/dss-code-prime` |

**The shape, and all four steps are the standard:**
1. **PREPARE** — `git fetch`, put the host's clone on the DRIVER's branch at the DRIVER's commit,
   `git clean -fd`. *This is the "check each leg branch before working in it" clause, and it is
   done by MOVING the host rather than by asserting about it.*
2. **SYNC** — the existing rsync/tar carriage, `.git` withheld on **all three** now. The host's
   own history is the authority; the sync supplies only the working tree.
3. **RUN** — build + ctest. Repo guards on the **root host only**.
4. **RESTORE** — `git reset --hard`, `git clean -fd`, `git worktree prune`. On **every** exit
   path, `die` included.

★ **ONE OWNER: `scripts/leg-tree/leg-tree.sh`.** Both verbs, all three hosts. The two remote legs
inline its text into their payload (`"$(cat …)"` — command-substitution output is not re-expanded,
so the script's own `$` and quotes arrive verbatim); the WSL leg sources it. **Never hand-roll
these git commands in a leg** — that is the four-hand-written-exclude-lists mistake with a
destructive verb attached.

⚠ **`-fd`, NEVER `-fdx`.** Ignored paths (`build/`, the ccache) are the leg's own working state;
deleting them buys tidiness and costs every leg a cold rebuild.

⚠ **CLEANUP BELONGS TO THE MODE THAT MADE THE MESS, not to every mode that runs afterwards.**
✔Caught while wiring this: `remote-leg --mode sync-only` exists to LEAVE a host staged for a
manual probe, and `--mode test-only` runs over whatever is already there. A restore on exit would
have deleted the staging as the command returned, and a prepare would have made `test-only` test
HEAD while reporting on the staged tree. ⇒ `full` prepares and restores; `sync-only` prepares
only; `test-only` does neither.

⚠ **WHY THIS REPLACED THE OLD ARRANGEMENT.** ✔MEASURED 2026-08-26: the macOS clone sat on branch
`…-3` at `8cb9afbd` (**three commits back**) with a **2,696-path index under a 2,759-path working
tree**, while two carriages withheld `.git` and one shipped it. ★ **The cost is not the disk — it
is that guards ask git questions.** `check-line-endings` reads `git ls-files --eol`;
`check-shell-portability` had ALREADY been rewritten in 2026-08-22 to stop asking `git ls-files`
because this very host answered about a commit that deleted `tools/*.sh` in P17 and produced
**seven violations against files that do not exist**. A host whose git disagrees with its files
makes every git-reading guard a coin flip, and the flip is invisible from the driver.

★★ **`git worktree prune` IS PART OF RESTORE, and it is the clause a human cleanup cannot cover.**
✔MEASURED 2026-08-26: both remote hosts carried a registered, `prunable` `dss-probe-6f4aab73`
worktree from a cycle that never cleaned up — and it **survived the operator's own manual pass**,
because a stale worktree registration lives in `.git/worktrees/` and **never appears in
`git status`**. A parallel lane may take a worktree on a leg host; the lane that takes it owns
removing it.

⚠ **AND THE TILDE DOES NOT EXPAND.** ✔MEASURED against the live VPS on the first run: every leg
names its repo `~/src/…`, and `cd "$var"` does **not** expand a tilde held in a variable — `~` is
expanded only where it appears unquoted in the source text. `leg-tree` normalises the path once,
where both verbs share it. ★ The dangerous half was not the failed `cd`: `restore` returns 0 when
the directory is missing, so an unexpanded path would have left every host dirty forever while
every leg reported success.

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
   ★★★ **The candidate set is `_deferred-anchor-registry-production.md` — ALWAYS** (operator, 2026-08-25). A harness row
   enters a cycle only by BLOCKING the production priority (step 2), never on its own ticket; and a
   harness defect you FACE is fixed in this cycle, never filed for later. See the ruling above.
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
   ★★ **THE ORCHESTRATOR IS A LANE TOO — ITS OWN EDITS OBEY THE SAME OWNERSHIP.**
   `src/dss-config/**` is a FILE SET like any other, and a config document is an INPUT to
   every lane's build. Editing one while a lane is running does not merely risk a merge
   conflict — it changes what that lane's binaries MEAN between two runs.
   ⚠ ✔MEASURED 2026-08-20 (cycle P22,
   `D-CYCLE-CONFIG-EDITS-NOT-SEQUENCED-AGAINST-LANE-OWNERSHIP`): the orchestrator
   corrected a relocation `nativeId` while a lane was mid
   red-on-disable run. A test's verdict flipped between two runs of the same binary, and the
   lane reported a stale tree as a defect in its final report. **The damage is not the wasted
   report — it is that a red-on-disable observation is the ONE measurement this project
   treats as proof, and a config edit underneath one silently corrupts it.**
   ⇒ Announce the orchestrator's own owned paths alongside the lanes'; hold a config edit
   until the lanes that read it have reported, or hand it to a lane that owns it. Re-measure
   anything a lane reported across such an edit before acting on it — and when a lane's
   report and the tree disagree, suspect the SEQUENCING before suspecting the lane.
   ★★ **AND THE TREE THAT RULE NAMES IS TOO NARROW: `.plans/**` IS AN INPUT TO A
   GUARD, AND A GUARD IS A CTEST ENTRY, SO EVERY LANE'S GATE READS IT.**
   ⚠ ✔MEASURED 2026-08-24 (cycle P31, `D-CYCLE-THE-ORCHESTRATOR-EDITED-PLANS-UNDER-A-RUNNING-LANE-AND-FLIPPED-ITS-GATE`):
   a lane's `plan_citations_guard` was RED in one gate and GREEN in the next **with no edit
   of its own in between**, because the orchestrator applied registry rows and re-baselined
   the citation ratchet while that gate was in flight. `anchor_registry_guard`,
   `plan_citations_guard`, `check-anchor-balance`, `check-stale-refusal-citations` and
   `check-retyped-closed-sets` all take `.plans/**` as their SUBJECT ⇒ a row written
   mid-gate moves a lane's verdict exactly as a config edit moves a lane's binary.
   ★ **THE DIRECTION THAT COSTS SOMETHING IS THE FLATTERING ONE.** That guard went
   red→GREEN, so the lane could have concluded its earlier red was a flake and stopped
   looking. It measured instead and named the mechanism, which is the only reason this is
   written down rather than sitting in a wrong number.
   ⇒ the orchestrator announces `src/dss-config/**` **and `.plans/**`** among its owned
   paths, and holds a row application or a ratchet re-baseline until the lanes whose gates
   read them have reported — the same hold it already owes a config edit.
   ★★ **THE GENERAL FORM, WHICH IS THE PART WORTH CARRYING: ASK WHAT A FILE IS AN
   INPUT TO, NOT WHICH DIRECTORY IT LIVES IN.** Both instances of this defect came from
   reasoning about the directory — the first framed the hazard as *a config document is
   an input to the compiler* and so stopped at `src/dss-config/**`. Any tree a GUARD takes
   as its subject is a shared input, whatever it is called.
   ★★ **A LANE THAT BUILDS GETS ITS OWN BUILD TREE.** File ownership is not enough, because
   two lanes with disjoint FILE sets still collide in a shared `build/`: one relinks the DLL
   while the other is mid-`ctest`. ⚠ ✔MEASURED 2026-08-20 (cycle P22): `0xc0000043`
   (STATUS_SHARING_VIOLATION) mid-suite, plus a set of failures that appeared and vanished
   between two runs of the same binary. **A gate result taken from a shared build tree is not
   attributable to anything** — which makes it worthless exactly when it matters, during a
   red-on-disable observation.
   ⇒ Name the lane's build tree in its brief (`build/<lane>`), and clear it once green (the
   one-root rule). `scripts/local-build/local-build.{sh,ps1} --tree <name>` takes one.
   ★★ **AND A LANE THAT WRITES SCRATCH FILES GETS ITS OWN SCRATCH DIRECTORY.** The per-lane
   BUILD tree isolates artifacts; it isolates neither the scratchpad nor the working tree.
   ⚠ ✔MEASURED 2026-08-20 (cycle P23,
   `D-CYCLE-LANE-SCRATCHPADS-ARE-SHARED-AND-LANES-CLOBBER-EACH-OTHER`):
   four lanes were given one `scratchpad/<cycle>/` directory, one lane's
   mutation harness was OVERWRITTEN by another lane's file of the same name mid-run, and the
   next three red-on-disable cycles executed the WRONG SCRIPT with the first lane's arguments.
   Nothing was corrupted only because that harness restored its subject from a `finally` and
   verified the hash. ⇒ Name `scratchpad/<cycle>/<lane>/` in the brief.
   ★★ **AN ANCHOR ID IS NEVER LINE-WRAPPED, AND THIS CLAUSE IS THE PROOF OF WHY.** The row
   above was cited here for hours WITHOUT EXISTING, and the step-10 audit was the first thing
   to notice — because the id was split across two lines, so neither the registry guard nor a
   human's grep could match it. ✔MEASURED 2026-08-20: **17 of the 78** distinct `D-*` ids cited
   on that cycle's added lines were wrapped; 16 were harmless only because the same id appears
   unwrapped nearby. ★ **A wrapped id does not fail — it becomes INVISIBLE**, which is the one
   failure mode a fail-loud project cannot detect by watching for a failure. Break the line
   BEFORE the id or AFTER it, never inside it — the convention the harness scripts already
   spell as `ANCHOR, ONE LINE, DO NOT WRAP`.
   ⚠ **The same measurement carries a second, larger consequence: a WHOLE-TREE gate number
   taken by any lane is not attributable to that lane**, because the source tree still holds
   every other lane's uncommitted edits. A lane scopes its gate with `-R` to its own subjects
   and treats a failure outside them as somebody else's until proven otherwise; the ONLY
   attributable whole-tree number is the orchestrator's, after the fold.
   ★★ **A BRIEF MAY STATE AN INTERFACE ONLY IF ITS AUTHOR HAS RUN IT** — the same standard as
   §5's "a measurement is stated only with the instrument that produced it", one level up: an
   invocation is a claim about the world, and writing one from memory is writing an
   unmeasured fact into the place a lane trusts most. ⚠ ✔MEASURED 2026-08-20 (cycle P23): the
   orchestrator's own common brief spelled `run-gate.sh -- ctest …`; the real interface is
   `<log-path> <success-regex> <command> [args...]`. TWO lanes hit it, it refused
   (fail-closed, correctly), and one left a file literally named `--` in the repo root. The
   fix is one command: run the invocation once before pasting it into a brief.
   ★★ **AND THE SAME STANDARD BINDS A MECHANISM, NOT ONLY AN INTERFACE: A BRIEF THAT NAMES THE
   FIELD A DECISION READS, OR THE ROLE A VALUE CARRIES, IS MAKING A MEASUREMENT AND OWES AN
   INSTRUMENT.** ⚠ ✔MEASURED 2026-08-20 (cycle P23,
   `D-CYCLE-BRIEF-ROUTED-A-DECISION-ONTO-A-FIELD-THAT-DOES-NOT-DISCRIMINATE`): a brief told a lane
   to route the COFF weak-external decision on the auxiliary record's `Characteristics` field. gcc
   emits `Characteristics = 1` for **all four** weak shapes, so routing on it would have classified
   every gcc weak DEFINITION as unresolvable — *precisely the defect the lane existed to fix*. The
   field that discriminates is the record's own `TagIndex`. ★ **This is the same trap as
   `D-LK-MACHO-ISDATA-NO-CALL-SIGNAL` (a relocation's arithmetic substituted for its role) and as
   `D-LK-PE-ALTERNATENAME-DECLARE-AND-REFUSE`'s revisit condition (a front-end feature substituted
   for the existence of a caller). The trap is not any particular field — it is reaching for
   whichever field sits nearest the decision and assuming it carries it.** Where a brief cannot
   supply an instrument, it says *"unmeasured, verify first"* rather than stating the fact flat.
   ★★ **AND THE LANE THAT REFUTES ITS BRIEF IS THE CONTROL LOOP WORKING, NOT A LANE GOING
   OFF-BRIEF** — say so in the brief, so the lane knows a refutation is a deliverable.
   ⚠ **AND THE FIRST WRITE-UP OF THIS RULE MISSTATED ITS OWN MEASUREMENT** — it said
   that invocation exits 127 with an empty log. ✔RE-MEASURED: it exits **2**, with a named
   refusal. The 127-and-empty-log shape is the DIFFERENT invocation `bash <C:/.../run-gate.sh>`,
   where bash cannot open the SCRIPT (see below). Two failures that look alike were being
   described as one, inside the rule that exists to stop exactly that.
   ★★ **A BRIEF THAT ASSIGNS `tests/<dir>/` GRANTS THAT DIRECTORY'S `CMakeLists.txt` AS
   APPEND-ONLY — AND SAYS SO.** A new `test_*.cpp` cannot RUN without a `dss_add_test` block, and
   that file belongs to the directory rather than to any lane, so a brief that lists the test file
   and not its registration leaves the lane a choice between not landing the test and editing an
   unowned file. ⚠ ✔MEASURED 2026-08-20 (cycle P23,
   `D-CYCLE-BRIEF-ASSIGNS-A-TEST-FILE-WITHOUT-ITS-BUILD-REGISTRATION`): four lanes added tests and
   three shared `CMakeLists.txt` files were each edited by lanes that had not been given them.
   Append-only edits merged cleanly; the damage came from ONE lane rewriting a whole file in CRLF,
   reddening `line_endings_guard` for three other lanes' work and leaving a diff nobody could claim.
   ⇒ **Append a block; never reorder, reformat, or rewrite the file whole.** Append-only is what
   makes a shared file safe under concurrency, and it is also what makes a violation visible.
   ★★ **A MESSAGE TO A LIVE LANE RE-STATES THAT LANE'S SUBJECT AND OWNED PATHS, IN ITS
   OPENING LINES.** A lane handle is an opaque id; several lanes run at once; and a message from
   the orchestrator carries the orchestrator's authority. ⚠ ✔MEASURED 2026-08-20 (cycle
   P23, `D-CYCLE-A-LANE-MESSAGE-DELIVERED-TO-THE-WRONG-LANE`): an ownership-NARROWING message
   — reassigning a file set and asserting *"your scope was always X"* — was delivered to
   the wrong lane. Had it been obeyed, two lanes would have edited one file set and BOTH reports
   would have become unattributable, which is the same damage class as editing a lane's config
   underneath it. **It did no damage for exactly one reason: the recipient's BRIEF named its own
   subject and listed those paths as FORBIDDEN**, so the instruction contradicted a written
   boundary instead of arriving into a vacuum — and the lane refused it and answered with a
   measurement (`git status --short` + `stat -c %y`) rather than a denial.
   ★ The reusable half: **an instruction that names the recipient's scope can be REFUTED by the
   recipient; one that only names the work cannot.** Redundancy in the addressing is what makes
   mis-delivery detectable at the destination, which is the only place it can still be caught.
   ★★ **THE DELIVERABLE TRAVELS IN THE REPORT, NEVER AS A PATH — AND THE BRIEF SAYS SO.** A lane's
   registry row text, its red-on-disable transcript, its md5s and any number the fold will quote come
   back INLINE in the reply. `scratchpad/<cycle>/<lane>/` keeps its P23 job — a private place for
   harnesses and intermediates — and stops being a place a RESULT is left.
   ⚠ ✔MEASURED 2026-08-24 (cycle P31,
   `D-CYCLE-A-LANE-DELIVERABLE-LEFT-IN-THE-SCRATCHPAD-IS-INVISIBLE-TO-THE-FOLD`): TWO lanes in one
   cycle reported by citing a path, and both paths were empty when the orchestrator read them — one
   of them holding the lane's **registry row**, which IS that lane's deliverable, and the other a
   483-row byte-identity baseline taken at a named commit.
   ★ **The mechanism is an interaction between two rules that are each correct alone**, which is why
   neither side looked wrong: `scratchpad/` is gitignored, and a `git worktree` gets **no copy of an
   ignored directory** — so a lane working in a worktree writes into a scratchpad the main tree does
   not have, while the orchestrator reads one the lane never wrote to. Do NOT "fix" this by
   un-ignoring `scratchpad/`: it holds build spill and half-written harnesses, and it would not help
   the worktree half at all, because the ignore rule is not what separates the two trees.
   ⇒ **The one-line test to put in the brief:** *if the orchestrator would have to open a file to
   fold your work, the work is not reported yet.*
   ⇒ **A lane that uses a `git worktree` NAMES IT in its report**, because the orchestrator must
   `git worktree remove` it at the fold and cannot remove one it does not know about.
   ★★ **AND BEFORE EDITING A FILE YOU OWN, COPY IT INTO YOUR SCRATCH DIRECTORY — that
   copy is your ONLY sanctioned undo.** The standing order forbids `git stash` / `checkout --` /
   `clean` / `reset` because the tree is shared, and that prohibition is correct and stays
   BLANKET. ⚠ But it was SILENT about a need it creates: a lane that corrupts its own
   exclusively-owned file has no way back except the one thing it is forbidden to do.
   ✔MEASURED 2026-08-24 (cycle P31, `D-CYCLE-THE-NEVER-CHECKOUT-RULE-LEAVES-A-LANE-NO-WAY-TO-UNDO-ITS-OWN-EDIT`):
   a lane ran `git checkout -- <its own config file>` to undo a malformed patch of its own, then
   disclosed it unprompted. ★ **The disclosure is the only reason anyone knows** — a restored
   file looks exactly like a file that was never edited, so this violation leaves nothing in any
   diff, which makes it the one class of rule-break that cannot be caught after the fact.
   ⇒ restore from your scratch copy: it restores exactly one file, cannot reach another lane's
   work even by mistake, and needs no judgement about what `--` would have swept.
   ★ The distinction to hold: **the ban is on the INSTRUMENT, not on the intent.** Undoing your
   own bad edit is legitimate; doing it with a tree-wide tool is not. And the prohibition keeps NO
   carve-out for "only my own files" — a tired lane applies that to a file it merely BELIEVES
   it owns, which is the case the rule exists for.
   ★★ **A BYTE-IDENTITY BASELINE IS TAKEN AS AN ISOLATING PAIR, NEVER INHERITED —
   AND IN A SHARED TREE ITS SHELF LIFE IS MEASURED IN HOURS.**
   ⚠ ✔MEASURED 2026-08-24 (cycle P31, `D-CYCLE-A-BYTE-IDENTITY-BASELINE-EXPIRES-WHEN-THE-SHARED-TREE-MOVES`):
   a lane diffed a predecessor's 483-row baseline, taken two hours earlier at the same commit, and
   got **13 differing lines with 8 examples flipping to NO-ARTIFACT — none of them its own**. A
   sibling lane's front-end work had landed in between, while the instrument's `cfgroot` snapshot
   still pinned HEAD's language document.
   ★ **The trap is that both failure modes produce the SAME diff:** *"my change moved these
   bytes"* and *"the world moved underneath my baseline"* are indistinguishable by looking, and only
   one is a defect. A lane that trusts an inherited baseline either hunts a regression it did not
   cause, or — worse — accepts 13 moved rows as noise and misses a real one.
   ⇒ **Take BOTH halves yourself:** revert only YOUR files to HEAD with
   `git cat-file -p HEAD:<path>` (never `checkout --` or `stash`, which reach the whole shared
   tree), leave every other lane's work in place, take the BEFORE; restore your files, take the
   AFTER. Both runs then see the same sibling state, so your diff is the only variable left. ✔That
   is what produced that lane's result: **zero differing lines in 486 pre-existing rows**, the final
   manifest differing by exactly one ADDED row for its new example.
   ★ Two corollaries, each paid for: **an inherited baseline is usable only with a CONTROL** that
   re-derives a handful of its rows against the live tree — cheap, and it separates stale from
   broken in one run; and **a baseline's identity is the CONFIG SNAPSHOT plus the commit**, not the
   commit alone, so an instrument pinning a `cfgroot` must record which one and a reader must never
   assume HEAD.
   ⭐ **AND A PATH THAT SOMEBODY ELSE MUST RESOLVE IS ABSOLUTE, OR NAMES ITS ROOT.** TWO roots
   answer to the name `scratchpad/`: the repository's (gitignored) and the SESSION's, under
   `…/AppData/Local/Temp/claude/<project>/<session>/scratchpad/`, which is outside the repo
   entirely. A lane writing to one and reporting a bare relative path sends the orchestrator to the
   other, and both readings are plausible.
   ★★★ **AND THE CLAUSE THE ORCHESTRATOR'S OWN ERROR HERE ADDS, WHICH BINDS EVERY
   PARTY: A NEGATIVE RESULT CARRIES THE SCOPE IT WAS TAKEN OVER.** ⚠ ✔MEASURED 2026-08-24
   (cycle P31): the orchestrator ran `find` over the REPO root, found none of a lane's seven
   instruments, and told that live lane *"✔MEASURED just now: none of them exists"*. They were
   intact in the session root the whole time — the search could not have seen them. The lane
   began rebuilding a 483-row byte-identity baseline on that word, and the next hazard was a
   reconstructed baseline reconciled against a real one: a claim with two provenances and no way to
   separate them. ⇒ *"not found under `<root>`"* is a measurement; *"does not exist"* is a claim
   the instrument did not make. Before telling a lane something of its own is missing, search every
   root that could hold it — and prefer **asking the lane where it put the thing**, since it is
   the one party that knows. ★ This is the same species as a lane's vacuous key-name scan and as
   a guard clause that cannot fire on the gating leg: **a SCOPED instrument reporting an UNSCOPED
   claim.** It is worse from the orchestrator, because a lane can refute its brief, while a lane
   cannot easily refute a measurement handed down as fact.
   ★★ **AND A BRIEF THAT RELAYS A PRIOR LANE'S ARTIFACT MUST OPEN ONE OF THEM FIRST**, or say
   *"unverified, rebuild your own"*. Same measurement: the replacement brief for that second lane
   asserted its scratchpad *"ALREADY CONTAINS the instruments and baselines"* and named seven files,
   relayed from the prior lane's report with none of them opened — this section's own
   run-it-before-you-write-it rule, violated one level up by the party that enforces it. ★ The
   damage that was nearly done is the instructive part: not wasted effort, but a **RECONSTRUCTED
   baseline presented as the prior lane's** — a byte-identity claim with no provenance, which is
   evidence-shaped and worth nothing.
6. **Review and fold** — `/pr-review-toolkit:review-pr`, plus the agnosticism pass and the CI-hazard
   screen. ★★ **If this cycle created or modified a `.sh`/`.ps1` pair, TWIN PARITY IS PART OF THIS
   STEP** — same inputs, same properties, same flags, same exit codes, both siblings changed in this
   commit. It is a review obligation because it is not decidable by a script; see the pairing section
   below. **Re-review the fold** if folding changed logic; iterate to a fixed point. Passes that
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
- ⚠⚠ **AND `bash <script>` FROM A WINDOWS-NATIVE PARENT IS NOT THE BASH YOU MEAN.**
  ✔MEASURED 2026-08-20 (cycle P23): from a Windows-native process, `bash` resolves to
  `C:\WINDOWS\system32\bash.exe` — **WSL's** — which cannot open a `C:/...` path. Two
  distinct failures follow and they look alike:
  * `bash scripts/run-gate/run-gate.sh <C:/...log>` — the script RUNS and cannot write its
    log; it now exits **2** with a named refusal that identifies the shell.
  * `bash <C:/.../run-gate.sh>` — bash cannot open the SCRIPT, so **it never executes**.
    Exit **127**, empty log, and **no edit inside any script can ever improve this shape**.
    The only fix is at the CALL SITE: invoke it as a relative path from Git Bash, or run the
    `.ps1` twin.
  ★ Worth stating because the second shape reads as *"the gate refused"* when what happened
  is *"the wrong bash ran"* — an instrument that misattributes is the failure this project
  cares most about.
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
### `.sh`/`.ps1` pairing is a JUDGEMENT THE AUTHOR MAKES AND WRITES DOWN — never a gate

**Operator ruling 2026-08-19:** *"some scripts are posix executed only, and don't have a .ps1 pair. so
we must just enforce the dss cycle and dss code prime skills to always create the pair, except when the
execution is posix only."*

- **Create the `.ps1` twin whenever the capability must reach the Windows leg.** That leg is where this
  project's primary ctest runs; a bash-only capability is one the main gate cannot use.
- **Omit it — and say so in the header — when either holds:** the script is already cross-platform (a
  `.py` runs on both hosts, so a twin would be a second implementation of something never split), or
  execution is POSIX-ONLY BY NATURE (`wsl-leg` runs inside a WSL distro where PowerShell is not the
  shell; `profile-compile` drives a POSIX toolchain over a carriage).
- **Where a pair exists, the two must not drift — and that is checked IN THE REVIEW, at the moment the
  script is written or changed.** Operator ruling 2026-08-19: *"the parity must be checked in the
  review, before the commit, when the script is being created or modified. Not after and not a script
  to it. After committed it must be already working."*
  ⚠ **Not automatable, and the reason is not laziness:** a script can do literally anything, so
  equivalence of two arbitrary programs is not a property a detector can decide. What a gate CAN see is
  existence and metadata — which is why `scripts_index_guard` refuses a sibling whose `PURPOSE:`
  contradicts its primary, and stops exactly there.
  **What the reviewer owes when a `.sh`/`.ps1` pair is touched:** the two scan the same inputs, check
  the same properties, accept the same flags, and return the same exit codes for the same conditions —
  and a change to one landed in the other in the SAME commit. Pairing by EXISTENCE is not pairing by
  BEHAVIOUR.

⚠ **This is deliberately NOT enforced by a guard, and the reason is measured:** ✔11 of 21 script
directories carry no `.ps1` and every one is correct. A gate cannot tell a deliberate POSIX-only script
from a forgotten twin, so it would need an allowlist of eleven exceptions — the convention written twice,
in the place least likely to be read, reddening honest work by default. The anchor that demanded such a
gate was WITHDRAWN on that ruling.

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
