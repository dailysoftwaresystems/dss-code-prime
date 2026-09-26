# Lane discipline — briefing, running and folding lanes (step 5)

The rules step 5 carries after its opening sentences (the opening is in `workflow-steps.md`). Read them
before writing any lane brief, and again when a lane reports. What to delegate and the four-agent cap
are in `delegation.md`; worktrees are in `worktrees.md`.

## Contents
- Rule 1 — Contention is per FILE, not per directory
- Rule 2 — The orchestrator is a lane too — one lane at a time holds `src/dss-config/**` or `src/core/types/*schema*`; `.plans/**` is a guard input; ask what a file is an INPUT to
- Rule 3 — A lane that builds gets its own build tree
- Rule 4 — A lane that writes scratch files gets its own scratch directory — and writes its rows to a rows directory there
- Rule 5 — An anchor id is never line-wrapped — and a whole-tree number is not a lane's
- Rule 6 — A brief states an interface only if its author has run it, and a mechanism only with an instrument; a lane that refutes its brief is the control loop working
- Rule 7 — A brief that assigns `tests/<dir>/` grants its `CMakeLists.txt` append-only
- Rule 8 — A message to a live lane restates its subject and owned paths
- Rule 9 — The deliverable travels in the report, never as a path — except the registry rows, which go to a rows directory
- Rule 10 — Copy a file into your scratch directory before editing it — your only sanctioned undo
- Rule 11 — A byte-identity baseline is an isolating pair, never inherited
- Rule 12 — A path somebody else must resolve is absolute; a negative result carries its scope; a brief that relays a prior lane's artifact opens one first

### 1. Contention is per FILE, not per directory

   ⚠⚠ **CONTENTION IS PER *FILE*, NOT PER DIRECTORY — AND A DIRECTORY-SHAPED
   FORBIDDEN LIST COSTS A LANE ITS WHOLE RUN.** ✔MEASURED 2026-08-28 (P44): lane `h` was
   forbidden `src/core/**`, `src/ffi/**` and `src/program/**` because a sibling owned *some*
   files beneath them. It needed three specific files, **none of which the sibling touched**,
   and it stopped rather than edit a forbidden path — correctly, by the rule as written
   — after a ~27-minute run that produced zero edits. Justifying the grant then took one
   `grep`. ⇒ **State each lane's owned and forbidden sets as PATHS, and when a lane asks
   for a file inside a forbidden directory, MEASURE whether any sibling touches that FILE
   before refusing.** A directory-shaped forbidden list is a GUESS about contention, not a
   measurement of it — and the guess fails in the expensive direction, because a lane that
   obeys it looks compliant while doing nothing. ★ The repair is cheap and does not restart
   the work: grant the file, say why it is safe, and **resume the SAME agent** rather than
   spawning a fresh one that has to re-derive everything.

### 2. The orchestrator is a lane too — one lane at a time holds `src/dss-config/**` or `src/core/types/*schema*`; `.plans/**` is a guard input; ask what a file is an INPUT to

   ★★ **THE ORCHESTRATOR IS A LANE TOO — ITS OWN EDITS OBEY THE SAME OWNERSHIP.**
   `src/dss-config/**` is a FILE SET like any other, and a config document is an INPUT to
   every lane's build. Editing one while a lane is running does not merely risk a merge
   conflict — it changes what that lane's binaries MEAN between two runs.
   ⚠ ✔MEASURED 2026-08-20 (cycle P22): the orchestrator
   corrected a relocation `nativeId` while a lane was mid
   red-on-disable run. A test's verdict flipped between two runs of the same binary, and the
   lane reported a stale tree as a defect in its final report. **The damage is not the wasted
   report — it is that a red-on-disable observation is the ONE measurement this project
   treats as proof, and a config edit underneath one silently corrupts it.**
   ⇒ Announce the orchestrator's own owned paths alongside the lanes'; hold a config edit
   until the lanes that read it have reported, or hand it to a lane that owns it. Re-measure
   anything a lane reported across such an edit before acting on it — and when a lane's
   report and the tree disagree, suspect the SEQUENCING before suspecting the lane.
   ★★★★ **HANDING `src/dss-config/**` TO A LANE DOES NOT FIX THIS — IT ONLY MOVES WHOSE
   HAND IS ON IT, AND ✔THE HAZARD RECURRED THAT WAY ON 2026-08-26 (cycle P38).** The P22
   row above is CLOSED and the
   mechanism it built is sound; what recurred was the SCHEDULING. A lane was given
   `src/dss-config/targets/**` + `src/core/types/target_schema.*` and run CONCURRENTLY
   with three lanes that gate — so `test_support/test_config_snapshot` reddened in one
   lane and `core/test_target_schema` was momentarily UNCOMPILABLE in another, neither
   caused by the lane reporting it. ★ **`test_config_snapshot` WAS RIGHT AND MUST NOT BE
   "FIXED": it deliberately compares the run's snapshot against the LIVE tree, which is
   the only clause proving the copy is still taken at ctest RUN time.** Softening it to
   stop the flap would delete the mechanism's honesty check to hide an orchestration
   error — the *guard weakened every time it fires* failure, exactly.
   ⇒ **THE RULE: at most ONE lane may hold `src/dss-config/**` or `src/core/types/*schema*`
   at a time, and NO OTHER LANE'S ctest VERDICT IS TRUSTWORTHY WHILE IT DOES.** Either
   sequence that lane alone, or treat the concurrent lanes' gates as PROVISIONAL and
   re-gate the integrated tree once it is quiescent. ★ A lane reporting "N-1/N, the one
   red is another lane's config edit" has diagnosed it correctly — that report is a
   SEQUENCING finding, and the integration gate, not the lane, is what settles it.
   ★★ **AND THE TREE THAT RULE NAMES IS TOO NARROW: `.plans/**` IS AN INPUT TO A
   GUARD, AND A GUARD IS A CTEST ENTRY, SO EVERY LANE'S GATE READS IT.**
   ⚠ ✔MEASURED 2026-08-24 (cycle P31):
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

### 3. A lane that builds gets its own build tree

   ★★ **A LANE THAT BUILDS GETS ITS OWN BUILD TREE.** File ownership is not enough, because
   two lanes with disjoint FILE sets still collide in a shared `build/`: one relinks the DLL
   while the other is mid-`ctest`. ⚠ ✔MEASURED 2026-08-20 (cycle P22): `0xc0000043`
   (STATUS_SHARING_VIOLATION) mid-suite, plus a set of failures that appeared and vanished
   between two runs of the same binary. **A gate result taken from a shared build tree is not
   attributable to anything** — which makes it worthless exactly when it matters, during a
   red-on-disable observation.
   ⇒ Name the lane's build tree in its brief (`build/<lane>`), and clear it once green (the
   one-root rule). ⏳ SCRIPT-ERA (superseded 2026-09-24: `dssharness build` in the lane's worktree names the directory itself, `<lane>/build/<variant>`; see build-layout.md) `dssharness build` gives each leg its own variant-keyed directory INSIDE the
   tree it is run in, so a lane worktree isolates itself — ✔MEASURED from one:
   `-S <lane>/. -B <lane>/build/x86_64-mingw-gcc-debug`.

### 4. A lane that writes scratch files gets its own scratch directory — and writes its rows to a rows directory there

   ★★ **AND A LANE THAT WRITES SCRATCH FILES GETS ITS OWN SCRATCH DIRECTORY.** The per-lane
   BUILD tree isolates artifacts; it isolates neither the scratchpad nor the working tree.
   ⚠ ✔MEASURED 2026-08-20 (cycle P23):
   four lanes were given one `scratchpad/<cycle>/` directory, one lane's
   mutation harness was OVERWRITTEN by another lane's file of the same name mid-run, and the
   next three red-on-disable cycles executed the WRONG SCRIPT with the first lane's arguments.
   Nothing was corrupted only because that harness restored its subject from a `finally` and
   verified the hash. ⇒ Name `scratchpad/<cycle>/<lane>/` in the brief.
   ★★★ **AND THE BRIEF MUST REQUIRE THE LANE TO *WRITE ITS ROW TO A FILE* THERE, NOT MERELY TO
   EMIT IT.** ✔MEASURED TWICE in cycle P44: a finished lane's task transcript came back **0 BYTES**,
   so its row — correct, complete, already written — reached the orchestrator not at all. The
   second time it was a lane that had emitted the row as a TOOL OUTPUT rather than as prose, which
   a text-only harvester silently drops. **Both failures look identical from the orchestrator's
   side: a harvest that simply reports one fewer row, indistinguishable from a lane that produced
   none.** ⇒ Every brief names an exact ROWS DIRECTORY — an absolute path, or one that names its root
   (rule 12) — and says: write each row there in anchor-rows' format,
   `<rows dir>/<ANCHOR ID>/{priority,status,trigger,closing,crossrefs}.txt` — UTF-8 with no byte-order mark,
   one file per cell, each cell VERBATIM on one line (the door stores a line break as a space, so a wrapped
   id or path is stored cut), each written from the stored text (`dssharness read-anchor <ID> --json`) or the
   lane's own words and NEVER from a redacted display, whose masks are not the text (✔MEASURED 2026-09-26:
   `C 6.7.2.5's` displayed as `C <ip>'s`, and a cell copied from the display stored the mask), a pipe written plain (the door escapes it, and refuses one already escaped),
   nothing else in the directory, and a NEW row carrying at least `priority`, `status` and `trigger` — and
   reply with the directory, the ids (the NEW ones named as new: the fold declares them with the stage step's
   `new` input) and each cell file's md5. Rule 9 holds the scope split: the rows go to
   that directory, everything else travels inline. ⚠ The recovery is
   always to ASK THE LANE TO WRITE IT — **never to retype the row from a report**, because a
   retyped row can WRAP an anchor id, and a wrapped id does not fail: it goes invisible to every
   grep and MINTS a false one.

### 5. An anchor id is never line-wrapped — and a whole-tree number is not a lane's

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
   every other lane's uncommitted edits. A lane scopes its gate with `dssharness test --filter <regex>` to its own subjects
   and treats a failure outside them as somebody else's until proven otherwise; the ONLY
   attributable whole-tree number is the orchestrator's, after the fold.

### 6. A brief states an interface only if its author has run it, and a mechanism only with an instrument; a lane that refutes its brief is the control loop working

   ★★ **A BRIEF MAY STATE AN INTERFACE ONLY IF ITS AUTHOR HAS RUN IT** — the same standard as
   §5's "a measurement is stated only with the instrument that produced it", one level up: an
   invocation is a claim about the world, and writing one from memory is writing an
   unmeasured fact into the place a lane trusts most. ⚠ ✔MEASURED 2026-08-20 (cycle P23): the
   orchestrator's own common brief spelled the witness gate as `-- ctest …`; its real interface was
   `<log-path> <success-regex> <command> [args...]`. TWO lanes hit it, it refused
   (fail-closed, correctly), and one left a file literally named `--` in the repo root. The
   fix is one command: run the invocation once before pasting it into a brief.
   ★★ **AND THE SAME STANDARD BINDS A MECHANISM, NOT ONLY AN INTERFACE: A BRIEF THAT NAMES THE
   FIELD A DECISION READS, OR THE ROLE A VALUE CARRIES, IS MAKING A MEASUREMENT AND OWES AN
   INSTRUMENT.** ⚠ ✔MEASURED 2026-08-20 (cycle P23): a brief told a lane
   to route the COFF weak-external decision on the auxiliary record's `Characteristics` field. gcc
   emits `Characteristics = 1` for **all four** weak shapes, so routing on it would have classified
   every gcc weak DEFINITION as unresolvable — *precisely the defect the lane existed to fix*. The
   field that discriminates is the record's own `TagIndex`. ★ **This is the same trap as the
   Mach-O `isData` no-call-signal case (a relocation's arithmetic substituted for its role) and as
   the PE `/ALTERNATENAME` declare-and-refuse revisit condition (a front-end feature substituted
   for the existence of a caller). The trap is not any particular field — it is reaching for
   whichever field sits nearest the decision and assuming it carries it.** Where a brief cannot
   supply an instrument, it says *"unmeasured, verify first"* rather than stating the fact flat.
   ★★ **AND THE LANE THAT REFUTES ITS BRIEF IS THE CONTROL LOOP WORKING, NOT A LANE GOING
   OFF-BRIEF** — say so in the brief, so the lane knows a refutation is a deliverable.
   ⚠ **AND THE FIRST WRITE-UP OF THIS RULE MISSTATED ITS OWN MEASUREMENT** — it said
   that invocation exits 127 with an empty log. ✔RE-MEASURED: it exits **2**, with a named
   refusal. The 127-and-empty-log shape is the DIFFERENT invocation `bash <C:/.../script.sh>`,
   where bash cannot open the SCRIPT (see below [→ actions.md, its last section](actions.md)). Two failures that look alike were being
   described as one, inside the rule that exists to stop exactly that.

### 7. A brief that assigns `tests/<dir>/` grants its `CMakeLists.txt` append-only

   ★★ **A BRIEF THAT ASSIGNS `tests/<dir>/` GRANTS THAT DIRECTORY'S `CMakeLists.txt` AS
   APPEND-ONLY — AND SAYS SO.** A new `test_*.cpp` cannot RUN without a `dss_add_test` block, and
   that file belongs to the directory rather than to any lane, so a brief that lists the test file
   and not its registration leaves the lane a choice between not landing the test and editing an
   unowned file. ⚠ ✔MEASURED 2026-08-20 (cycle P23): four lanes added tests and
   three shared `CMakeLists.txt` files were each edited by lanes that had not been given them.
   Append-only edits merged cleanly; the damage came from ONE lane rewriting a whole file in CRLF,
   reddening `line_endings_guard` for three other lanes' work and leaving a diff nobody could claim.
   ⇒ **Append a block; never reorder, reformat, or rewrite the file whole.** Append-only is what
   makes a shared file safe under concurrency, and it is also what makes a violation visible.

### 8. A message to a live lane restates its subject and owned paths

   ★★ **A MESSAGE TO A LIVE LANE RE-STATES THAT LANE'S SUBJECT AND OWNED PATHS, IN ITS
   OPENING LINES.** A lane handle is an opaque id; several lanes run at once; and a message from
   the orchestrator carries the orchestrator's authority. ⚠ ✔MEASURED 2026-08-20 (cycle
   P23): an ownership-NARROWING message
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

### 9. The deliverable travels in the report, never as a path — except the registry rows, which go to a rows directory

   ★★ **THE DELIVERABLE TRAVELS IN THE REPORT, NEVER AS A PATH — AND THE BRIEF SAYS SO.** A lane's
   red-on-disable transcript, its md5s and any number the fold will quote come back INLINE in the reply;
   its registry rows are the one exception, and go to a rows directory (rule 4). `scratchpad/<cycle>/<lane>/` keeps its P23 job — a private place for
   harnesses and intermediates — and stops being a place a RESULT is left.
   ★★★ **THE SCOPE SPLIT, stated once (the P31 / P44 resolution, 2026-09-25):** REGISTRY ROWS are written
   to a rows directory in the lane's scratch, in anchor-rows' format — one file per cell, each cell VERBATIM
   (P44, rule 4) — at a path that is ABSOLUTE or names its root, because a worktree lane's scratch is not the
   main tree's (P31 below, rule 12); the report names the directory, the ids and each cell file's md5.
   Everything else a lane delivers — findings, measurements, verdicts, transcripts, md5s — travels INLINE in
   the report (P31).
   ⚠ ✔MEASURED 2026-08-24 (cycle P31): TWO lanes in one
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
   fold your work — the registry rows excepted (rule 4) — the work is not reported yet.*
   ⇒ **A lane that uses a `git worktree` NAMES IT in its report**, because the orchestrator must
   remove it at the fold — `lane-fold.py land`, then `dssharness delete-worktree` for its host copies — and
   cannot remove one it does not know about.

### 10. Copy a file into your scratch directory before editing it — your only sanctioned undo

   ★★ **AND BEFORE EDITING A FILE YOU OWN, COPY IT INTO YOUR SCRATCH DIRECTORY — that
   copy is your ONLY sanctioned undo.** The standing order forbids `git stash` / `checkout --` /
   `clean` / `reset` because the tree is shared, and that prohibition is correct and stays
   BLANKET. ⚠ But it was SILENT about a need it creates: a lane that corrupts its own
   exclusively-owned file has no way back except the one thing it is forbidden to do.
   ✔MEASURED 2026-08-24 (cycle P31):
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

### 11. A byte-identity baseline is an isolating pair, never inherited

   ★★ **A BYTE-IDENTITY BASELINE IS TAKEN AS AN ISOLATING PAIR, NEVER INHERITED —
   AND IN A SHARED TREE ITS SHELF LIFE IS MEASURED IN HOURS.**
   ⚠ ✔MEASURED 2026-08-24 (cycle P31):
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

### 12. A path somebody else must resolve is absolute; a negative result carries its scope; a brief that relays a prior lane's artifact opens one first

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
   run-it-before-you-write-it rule [→ rule 6 of this file](lane-discipline.md), violated one level up by the party that enforces it. ★ The
   damage that was nearly done is the instructive part: not wasted effort, but a **RECONSTRUCTED
   baseline presented as the prior lane's** — a byte-identity claim with no provenance, which is
   evidence-shaped and worth nothing.
