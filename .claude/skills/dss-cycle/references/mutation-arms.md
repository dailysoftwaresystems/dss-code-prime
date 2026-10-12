# Mutation arms — red-on-disable is `dssharness check-mutations`

## Contents
- Rule 1 — an arm's claim is a gtest TEST in a self-contained test binary
- The rule (operator orders, 2026-10-10): a pin is an arm; the sweep is a step of the gate; who writes an arm
  and who sweeps it
- The registry's standing conditions
- The verdicts, and what each one obliges
- What this repository declares — the configuration block, what a runner is, how its binary is started
- The registry's layout — one file many hands append to; validation; the hand merge
- How to declare an arm — one worked example per class
- Finding an arm's claim, target, runner, case count, diagnostic and legs
- Reading an arm that did not pass
- The command lines
- What a sweep costs, and from which trees it can run
- What has no arm — each class named with its measurement

**Read this before proving a pin, declaring an arm, merging the registry, or closing a row on a red-on-disable.**
The grammar and every verdict are the tool's: `dssharness help mutations` is the authority and outranks this file
for the question *what does the installed tool do*. This file says how this repository uses it.

## ★★★★ RULE 1 — AN ARM'S CLAIM IS A gtest TEST IN A SELF-CONTAINED TEST BINARY

**The claim an arm proves is one TEST of a unit-test binary — a target `dss_add_test` builds — and that binary's
target is the arm's `runner`.** The tool starts the runner's binary WHOLE, with the report arguments and nothing
else, in the worker copy's root: no argument of a ctest entry, none of ctest's `ENVIRONMENT`, none of its
`WORKING_DIRECTORY`, and no ctest FIXTURE run before it. A binary that passes started that way is self-contained;
nothing else can be a runner.

**THAT BINDS EVERY CASE OF THE RUNNER, NOT ONLY THE CLAIM** — the tool judges the EXACT set of cases that go red:
- **A case that needs what another ctest entry makes, or what ctest's environment says, is red started bare** —
  under every mutant, and in the unmutated control, which leaves every arm of that runner `stopped`.
- **A worker's first build, and every mutant of a site the build stamp digests, is A BUILD OF ITS OWN.** It
  carries a stamp no earlier run on that machine has seen, so whatever a test keeps per user and per stamp — a
  cache of built objects, a runtime it warmed — starts COLD. A case whose verdict depends on that state being
  warm is red there whatever the mutant changes, and green in the suite that ran before it (met 2026-10-12: a
  test binary red in a sweep's fresh worker and green in the full suite of the same bytes).
- So **an arm's runner holds no case whose verdict depends on a fixture, on ctest's environment, or on state an
  earlier run left behind.** Such a case is made self-contained, or moved out of the runner, BEFORE an arm names
  that runner: a red beside the declared set reads `violated`, and the arm has proved nothing about its claim.

**An `examples/**` or `integrated_tests/**` entry is a corpus check the gate runs — never an arm's pin.** The
examples runner is one example per invocation (its example directory is an argument, its scratch a per-build
working directory), so there is no "binary run whole" for the tool to judge (✔MEASURED 2026-10-10: started bare,
it reads the report argument as its example — *"argv[1] is not a directory"* — and its control is red, which
`stopped` the arm AND failed the leg).

What follows for a mutant:
- **It names a unit-test binary beside an example** → its arm declares the unit cases: the exact red set of that
  binary. The example stays what it was — a runnable corpus entry the gate runs.
- **It names ONLY an example** → it has no arm yet. Its lane writes the unit-tier claim first — a TEST at the tier
  of the fix, which the bar asks for anyway — and the arm is declared on that. **Until a mutant has such a claim it
  has no arm, and the row that rests on it does not close.**
- **One mutation several unit binaries read** → one arm per binary (`<id>_<pin>`); an arm has exactly one runner.

## ★★★★ THE RULE — operator orders, 2026-10-10

> *"migrate the mutations to the new verb from dss-harness: check-mutations (in dss-cycle skill as mandatory)"*

**A red-on-disable pin is an ARM of `tests/mutations/arms.registry`, and `dssharness check-mutations` is what reads
it.** An arm declares the file mutated, the exact text taken out and the text put in, whether the mutation must
redden a TEST or stop the BUILD, the build target, the test target whose binary runs, how many cases that binary
runs, a diagnostic the run must say, the legs it runs on, and the EXACT set of cases that must go red. The tool
mutates that text in a WORKER COPY of the leg's tree — never in the tree itself, a lane's worktree or the main
checkout — builds, proves every object that depends on the site was rebuilt, runs the binary whole, judges what
failed against the declared set, and puts the site back, checked by its hash.

Four obligations follow, and each is a gate:

1. **A pin is proven when its arm reads `passed`, and by nothing else.** A test written, a mutant applied by hand,
   a reading on other bytes: none of them is the proof. A pin with no arm is an unproven pin.
2. **THE SWEEP IS A STEP OF THE GATE, AND IT IS THE ORCHESTRATOR'S.** At the round's close, in the main tree,
   beside `dssharness test --legs gate`: every arm, on the leg its `S` row names — one `dssharness
   check-mutations --legs <leg>` per leg that an `S` row names (the command lines below). Per fold, the fold's
   own arms (`--arms`), on the bytes the fold landed as — after it landed, never before (who writes an arm and
   who sweeps it, below). A leg that did not reach its arms is STATED with the tool's reason, never skipped; a
   sweep is read by its leg line, never by its exit code alone.
3. **A row closes when its arms PASS on the bytes it closes on.** A reading made on earlier bytes stands as what
   it is and closes no row.
4. **A fold names its arms, and nothing closes on an unswept one.** A fold is handed back with its arms WRITTEN,
   so it carries no closure that rests on one: a new row is born `disclosed` with *"FIX FOLDED …; its arms not
   yet passed: `<arm ids>`"*, a row that would have closed keeps its status, and the report lists every arm with
   its state — written and unswept, and on which leg; or read in the lane's own tree, with the run id and the
   bytes it read. The rows close when the orchestrator's sweep has read the arms `passed` (obligation 3).

★ **Why the verb and not a careful hand.** Every check a reading by hand asked a person to remember is one the tool
makes itself, on every arm, and refuses to pass without: the before-text stands in its site exactly once; the
mutation is never replaced by itself; every dependent object is witnessed rebuilt from the build's own records;
the binary runs whole, and a run that counts another number of cases is refused; the reds are an exact set, so a
mutant that reddens the WRONG case does not pass; a neighbour (`G`) must have run and stayed green; the diagnostic
must be said; the site is put back and checked by hash. And a BUILD-RED mutant, which a reading by hand could only
call VOID, is an arm with a paired control.

### ★★★★ WHO WRITES AN ARM, AND WHO SWEEPS IT — said here, and pointed at from every other file

- **A LANE WRITES ITS ARMS WITH ITS FOLD**: the rows, the texts, the registry validated (below). It hands the fold
  back with them WRITTEN and names each as unswept. **A fold is never held for a sweep.**
- **THE ORCHESTRATOR SWEEPS THEM, FROM THE MAIN CHECKOUT, AFTER THE FOLD HAS LANDED** — the fold's own arms, on the
  bytes the fold landed as, each on the leg its `S` row names. **An arm that does not read `passed` GOES BACK TO
  ITS LANE with the tool's own text** — its verdict, its detail, its reds beside the declared ones — and leads
  that lane's next fold.
- **A LANE SWEEPS IN ITS OWN TREE ONLY WHAT IT NEEDS TO TRUST A `diag`**: an arm whose red set or diagnostic it
  cannot write from what it has read. That is a development reading — it says so, names its run and the bytes it
  read, and closes nothing.
- **WHY ONE HAND SWEEPS.** A sweep's workers are whole builds kept beside the tree they copy: a lane that sweeps
  builds the tree again beside its own worktree, and on a Windows leg can place no worker at all (the path
  limit, below), while the main checkout's workers stay warm from fold to fold. And a fold that waits for its own
  sweep waits for the slots the whole round is queueing for.
- **THE ORCHESTRATOR SWEEPS WHOLE LEGS AND NEVER CHUNKS ONE BY HAND.** A sweep is admitted ARM BY ARM: an arm
  holds one of its machine's slots only while it runs, and gives it back before the next asks. One command that
  names every arm of a leg therefore starves no other leg, and a leg cut into hand-made batches buys nothing and
  pays an unmutated control of every runner per batch. A WSL distribution runs ONE worker, so a WSL leg drives
  its arms one after another however it is asked.

## The registry's standing conditions

Each is enforced by the tool or by this file's layout rather than restated in a row:

- **(a) Every TEST-RED arm names the diagnostic it must produce** — a `diag` file — and a red that does not say it
  is `violated`, not a pass. A BUILD-RED arm matches NO compiler text (three compiler families word one rejection
  three ways): its proof is the PAIRED POSITIVE CONTROL, the same construct with one value substituted, which must
  build.
- **(b) An arm that produces no red is `survived`.** No row, field or token can declare an arm inert, and none is
  ever added: that would be an escape built into the mechanism whose purpose is to close one.
- **(c) An unregistered mutation is driven by nobody.** Nothing is swept but an `A` row; a row naming an arm no
  `A` row declares is refused; a text file no row cites is refused.
- **(d) A count is DERIVED, never typed.** How many arms: `grep -c '^A | ' tests/mutations/arms.registry`. No
  figure is written into the registry's comments, a plan or a skill.
- **(e) Every arm in the registry must read `passed`.** One `violated` arm fails its sweep; one runner that is red
  UNMUTATED fails its whole leg. A mutant that cannot be an arm (the last section) has no row, not a failing one.
- **(f) Every `C` row says how its red was MEASURED** — the sweep that read it (its run id), and a hand reading's
  run id where one exists. A red predicted from the code and never observed is not a `C` row.
- **(g) Every mutation text and every diagnostic is a FILE.** The grammar has no escape; the text a field would
  hold is the kind that holds a `|`.

## The verdicts, and what each one obliges

The words are the tool's (`help mutations`, `help verdicts`, `help exit-codes`); a report uses them and no other.

| verdict | what it says | what to do |
|---|---|---|
| `passed` | the arm held as declared | nothing: the only verdict that proves a pin |
| `violated` | the declaration did not hold — a site or text not there, the before-text not in its site exactly once, target or runner not built, no object they build depending on a site, OTHER cases red than the `C` rows, another number of cases run, a `G` case not run, the diagnostic not said, a TEST-RED mutation that does not compile, a BUILD-RED one that does or whose control does not | READ why first (the section on reading an arm); then fix the declaration, or the code it guards |
| `survived` | the mutation built and ran, and no case failed | the pin is vacuous: strengthen the test, and tell the orchestrator at once |
| `unattributed` | the run failed and nothing ties it to a case: no report, a failing exit with no failing case, a run past its bound or silent too long | contain the crash or hang in a case |
| `unwitnessed` | the build passed and an object that depends on a site was not rebuilt | find out what actually ran: nothing about the pin is known |
| `failed` | the build failed at a step that is no object depending on the site (a link, another object, the configure itself) | read the arm's build log, or its `configure.log`; a void reading — and where the mutation itself stopped the configure, the mutant cannot be an arm (the last section) |
| `stopped` | not driven to a verdict: the sweep was stopped, no worker was left, or the UNMUTATED run of its binary did not pass | read the control's log: a runner red unmutated stops every arm that runs it and fails the leg |
| `not-admitted` | its machine did not admit it | a wait, not a failure: run again; it measured nothing |
| `poisoned` | a site could not be put back | the worker is retired; report it |
| `skipped-not-selected` | `--arms` or its `S` row leaves it out of this leg | nothing |
| `skipped-unavailable` | no worker fits the room or the path limit on that leg | STATE it with the tool's reason; sweep from a tree that fits |
| `unmeasured`, `inputs-moved`, `contended` | the build's own guards | let the tree settle; run again |

A leg's verdict is the worst of its own and its arms'. The command exits 0 only when every selected leg passed; 1
for `violated`, 2 `survived`, 8 `unattributed`, 5 `unwitnessed`, 20 `failed`, 7 `not-admitted`, 21 where nothing
failed and not every arm reached a verdict — **21 is never a pass**.

## What this repository declares

`.harness-config/config.json`:

    "mutations": {
      "registry": "tests/mutations/arms.registry",
      "textDirectory": "tests/mutations/texts",
      "reportArgs": ["--gtest_output=xml:{report}"]
    }

- **`reportArgs`** is how a test binary writes the JUnit report the tool judges by. Every test target links
  GoogleTest's own `main` (`dss_add_test`), so each takes `--gtest_output=xml:<file>`; a case is named
  `Suite.Case`.
- **`workers` and `runTimeFactor` are left to the tool's defaults.** A worker is a whole copy of the tree with a
  whole build of its own; a leg gets no more of them than it has arms to drive, ONE in a WSL distribution, and
  the tool runs fewer than it wants where a host has no room for another, and says so (✔MEASURED 2026-10-10:
  two on the Mac, one on WSL — so a WSL leg drives its arms one after another).
- **`worktrees.pathLimit` is NOT declared.** It replaces the platform's own limit on EVERY host, so a declared 260
  held the Mac, WSL and the VPS to Windows' limit and no lane worktree's worker could be placed anywhere
  (✔MEASURED 2026-10-10: `skipped-unavailable`, *"every path must stay under 260"*, on the Mac leg; placed at
  once with the line gone). Windows keeps its own limit without being told (✔MEASURED the same day: the same
  refusal on the Windows leg, from a lane worktree, with no `pathLimit` declared).
- **A RUNNER is a test TARGET** — `dss_<the ctest entry's name with every / as _>` — and its binary is started as
  Rule 1 says. Started that way a test binary reads the worker's own `src/dss-config/` (the repository root baked
  into it), never ctest's per-run snapshot.

## The registry's layout — one file many hands append to

The tool takes ONE registry file, so it is a per-FILE contention point at every fold. It is laid out so that two
additions seldom meet, and its own header states the same rules:

- **One section per AREA of the compiler** — the first directory of the arm's site under `src/` (a site outside
  `src/` goes to `tests`) — in alphabetical order, each opened by its own comment block. Never a section per
  author, lane or cycle: none of those is a fact of the tree.
- **An arm's id** is `<area>-<what the mutation makes the code do>`: lowercase words joined by hyphens, at most 80
  characters, a phrase a reader of a red line understands. Where several test binaries read ONE mutation, each is
  an arm of its own, `<that id>_<pin>`.
- **An arm is a BLOCK**: its `A` row, then its `M`, `B`, `C`, `G` and `S` rows, then ONE blank line. Inside a
  section the blocks are sorted by arm id.
- **Every arm carries ONE `S` row naming ONE leg**, and its why says which rule chose the leg (the section on
  finding an arm's leg). An arm with no `S` row is refused by this convention, though the tool would run it on
  every leg. Each section's comment block states its area's default leg.
- **The `A` row's why opens `CLAIM AT <test file>, TEST <Case>.`** — the test that is the claim — and then says
  what mechanism the mutation switches off and what would be true if nothing noticed. A BUILD-RED arm's opens
  `CLAIM AT <file>, <the construct that refuses to compile>.` Never a line number.
- **A text file** is `tests/mutations/texts/<the arm's id, up to its _>.<role>`, the role one of `before`, `after`,
  `control-before`, `control-after`, `2.before` / `2.after` for an `M` row's site (`3.` for the next); a `diag`
  that belongs to one arm of several takes that arm's whole id. One flat directory; the tool refuses a text no
  row cites. ⚠ **A row cites a text by its path from the repository root** (`tests/mutations/texts/<name>`), never
  by its bare name (✔MEASURED 2026-10-10: with bare names every text read *"is in the text directory and no row
  cites it"*).
- **A text holds exactly the text**: no line before or after it, one line ending at its end, UTF-8, LF. The
  before-text must stand in its site EXACTLY once — take enough of the surrounding lines to make it so, and no
  more. The registry itself is ASCII. `.gitattributes` pins the whole directory (`tests/mutations/** text
  eol=lf`), so no checkout rewrites a text; the tool reads a text by its site's line endings either way.

### The validation — after every edit of the registry, before anything else reads it

    dssharness check-mutations --arms no-such-arm --legs <any declared leg> -C <tree>

It reads the registry and every text, touches no host and takes about a second. **Exit 12** lists every problem by
its line. **Exit 10 with `--arms names 'no-such-arm', which no A row of the registry declares` is the VALID
answer** — a refusal read as a pass, because the tool has no read-only form (✔MEASURED 2026-10-10). The whole
registry is read before `--arms` is weighed, and EVERY problem of it is listed whatever `--arms` names — an `S`
row that names neither a leg nor a leg set among them (`dssharness help mutations`) — so this one form validates
the whole file. ⚠ It does not say whether each before-text still stands in its site: that is said as `violated`,
inside a worker, by the first sweep of the arm.

### The hand merge — when a tree and the main tree both changed the registry

`fold-agent` and `rebase-agent` never merge: they refuse a path both sides changed and ask for it settled. On
COPIES, never in place:

1. Three copies in the work directory: `base` (the registry at the tree's base commit, `git -C <tree> show
   <base>:tests/mutations/arms.registry`), `mine` (the tree's), `theirs` (the main tree's).
2. `git merge-file --union -p mine base theirs > merged` — both sides' rows are kept where both added at one place.
3. Put the blocks of a section back in order by arm id where two additions met, and remove nothing else.
4. Copy `merged` into the tree; the text directory needs no merge unless one text was changed on both sides.
5. VALIDATE (above). Two `A` rows of one id — what the union leaves when both sides edited the SAME row — are
   refused there by line (✔MEASURED 2026-10-10: *"is declared twice, first at line …"*); settle that row by hand.
6. Report the merged file's md5; the orchestrator settles the path.

(`merge=union` in `.gitattributes` is NOT set, on purpose: neither verb runs git's merge driver, and a union that
settled a real conflict in silence would be found only by the next sweep.)

## How to declare an arm — one worked example per class

The row grammar, as `help mutations` gives it (fields separated by `|` and trimmed, the last taking the rest of
the line, `#` a comment, nothing escaped):

    A | arm | site | before | after | red kind | target | runner | cases | diag | why
    C | arm | case | why          a case the mutation must redden (at least one for TEST-RED)
    G | arm | case | why          a neighbour that must run and stay green
    S | arm | legs | why          the legs it runs on
    M | arm | site | before | after | why      another site, mutated, put back and checked with the arm's own:
                                               another FILE, or a further text of a file the arm already mutates
    B | arm | control before | control after | why      a BUILD-RED arm's paired control

Every example below is an arm of the registry: read it there for its rows in full. `T/` stands for
`tests/mutations/texts/` here and nowhere else — a row spells the path out.

**(1) A `.cpp` site, one case of one test binary — the common arm.**
`analysis-not-predicate-entered-without-a-probe`: site `src/analysis/syntactic/parser.cpp`, three lines taken out
and two put in; `TEST-RED`; target `dsscp-lib`; runner `dss_analysis_syntactic_test_parser_speculation` and
that binary's case count; its `diag` the assertion's own message; one `C` row, one `G` row, one `S` row.

    A | analysis-not-predicate-entered-without-a-probe | src/analysis/syntactic/parser.cpp | T/analysis-not-predicate-entered-without-a-probe.before | T/analysis-not-predicate-entered-without-a-probe.after | TEST-RED | dsscp-lib | dss_analysis_syntactic_test_parser_speculation | <cases> | T/analysis-not-predicate-entered-without-a-probe.diag | CLAIM AT tests/analysis/syntactic/test_parser_speculation.cpp, TEST NotFollowedByVetoesACleanCloseOnEveryPathToTheCandidate. <what is switched off, and what it would mean>
    C | analysis-not-predicate-entered-without-a-probe | ParserSpeculation.NotFollowedByVetoesACleanCloseOnEveryPathToTheCandidate | MEASURED <how: the sweep's run id, the hand reading's>: <what the red shows>
    G | analysis-not-predicate-entered-without-a-probe | ParserSpeculation.SpeculativeAltAcceptsTokenLeafBranches | <why this neighbour is untouched>
    S | analysis-not-predicate-entered-without-a-probe | <leg> | <why this leg>

**(2) A HEADER site.** `core-x87-extended-loses-a-significand-digit`: the same shape, site
`src/core/types/float_format.hpp`; the target stays `dsscp-lib`. Every object that includes the header is rebuilt
and witnessed, so the arm costs more than a `.cpp` one (✔MEASURED 2026-10-10: 117 s against 55 s).

**(3) BUILD-RED, with its paired control.** `ffi-symbol-linkage-table-repeats-a-spelling`: the mutation makes a
table repeat a spelling, which its own well-formedness check refuses to compile. Runner `-`, cases `0`, diag the
word `PAIRED-CONTROL`, exactly one `B` row and no `C`, `G` or `M` row; the control respells the same line with a
spelling no other row has and MUST build — it proves the refusal is the repeat, not the edit.

    A | ffi-symbol-linkage-table-repeats-a-spelling | src/ffi/shipped_lib_descriptor.cpp | T/….before | T/….after | BUILD-RED | dsscp-lib | - | 0 | PAIRED-CONTROL | CLAIM AT src/ffi/shipped_lib_descriptor.cpp, <the check that refuses>. <why>
    B | ffi-symbol-linkage-table-repeats-a-spelling | T/….control-before | T/….control-after | <why the control builds>

**(4) A red set that depends on the leg.** `program-commons-round-fetches-for-a-replaced-name`: part of its red
set is cases that run only where the native Mach-O cases run and are skipped elsewhere, so the arm's `S` row
names that leg. The same
mutation on another platform, where another set reddens, is ANOTHER arm (`<id>_<pin>`) with its own `S` row.

**(5) Two FILES — an `M` row.** `link-only-a-strong-definition-replaces-a-common-in-allocation-and-search`: one
rule two files apply; the mutant changes both the same way, so the two still agree with each other and only the
cases pinning the format's own answer redden. The `A` row holds the first site, an `M` row the other FILE.
Its first sweep read `violated` (✔MEASURED 2026-10-10: eight cases red, three declared), which is how a new
mutant's red set is found: the five others were READ in the arm's run log, each understood, and only then
declared.

**(6) A document the compiler reads at RUN time.** `dss-config-inferred-type-row-ignores-attribute-specifiers`:
site `src/dss-config/sources/c.lang.json`, the same shape as (1). Nothing compiles a document, so the object that
witnesses its mutation is an includer of the build stamp: the build declares each of them as depending on every
file under `src/dss-config/` (✔MEASURED 2026-10-10: `violated`, *"no object target 'dsscp-lib' or runner '…'
builds depends on 'src/dss-config/sources/c.lang.json'"* before that declaration; `passed` with it). The runner,
started bare, reads the worker's own document.

**(7) Several sites of ONE file — still ONE arm.** `mir-pin-decided-at-the-alloca`: the `A` row holds the first
text, and an `M` row naming the SAME file — spelt as the arm's first row for that file spells it — adds a further
text of it, one `M` row per further site. Every text of a file is replaced together, as one edit of it, and the
file is put back whole and checked once; each before-text must stand exactly once in the file as the tree holds
it, and two texts of one arm that overlap are refused when the registry is read. Never one before-text spanning
every site: any unrelated edit between the sites would break the arm.

**(8) A SCRIPT site — a CMake module, a runner action's program — behind the script's own self-test.**
`tests-stamp-does-not-digest-a-handed-files-bytes`: site `cmake/DssBuildStamp.cmake`, which no compiler reads. Its
claim is a case of the test binary that DRIVES the script's self-test (the last section says how such a binary is
built): the binary runs the self-test once, and each case reads ONE self-test arm's own verdict line. So the `C`
rows are the cases of the self-test arms the mutation fails, and the `diag` is the script's own failure line for
the claim. Target and runner are BOTH the test's target: its object is the one the build declares as depending on
the script (✔MEASURED 2026-10-10: 47 to 49 s an arm on a warm worker). A runner action's program is the same
class: the arms whose site is `.harness-config/runner/actions/check-ninja-deps/check-ninja-deps.py` name the
target `dss_ninja_deps_selftest_guard`.
⚠ A script's fixture can read differently per HOST — a path's separator, a file system's case — and then a
mutation's red exists on one kind of host only. Such an arm's `S` row names a leg of that kind (rule 1), and its
`C` rows say on which host the red was read; a hand pre-reading on the other kind of host shows it `survived`,
which is the fact and not a defect of the arm.

    A | tests-stamp-does-not-digest-a-handed-files-bytes | cmake/DssBuildStamp.cmake | T/….before | T/….after | TEST-RED | dss_build_build_stamp_identity | dss_build_build_stamp_identity | <cases> | T/….diag | CLAIM AT tests/program/test_build_stamp_identity.cpp, TEST <Case>. <why>

## Finding an arm's claim, target, runner, case count, diagnostic and legs

- **the claim** — the TEST written for the mechanism: `CLAIM AT <its file>, TEST <Case>`. If the only thing that
  notices the mutation is an example, write the unit-tier TEST first (Rule 1).
- **`runner`** — the claim's ctest entry `<dir>/<name>` is the target `dss_<dir>_<name>` (every `/` an `_`):
  `analysis/syntactic/test_parser_speculation` → `dss_analysis_syntactic_test_parser_speculation`.
- **`target`** — the CMake target that compiles the site: `dsscp-lib` for a `.cpp` or a header under `src/` (the
  compiler's library; an object of any library it is built from counts), `dsscp` for `src/main.cpp`, the runner
  itself for a header only the tests include. The tool builds the target and the runner, and judges only objects
  THEY build.
- **`cases`** — how many cases the runner's binary runs, skipped ones included; an equality. Count the binary's
  `TEST` lines for a first value; the sweep is the authority: a wrong count reads `violated` with both numbers,
  and the arm's entry in the `--json` ledger carries `cases` beside `declaredCases`. ⚠ Every arm of a runner
  states that count, so a change that adds a case to a test binary updates the `cases` field of EVERY arm that
  runs it, in the same change (`grep '| <runner> |' tests/mutations/arms.registry`).
- **`diag`** — a text the mutated run must SAY, written FROM AN OBSERVED RUN, never from the recipe: the failing
  assertion's own message, or the value it prints that shows the mechanism is off — never the case's name (the
  `C` row already holds that).
- **the exact red set** — one `C` row per case that goes red, each saying how it was measured. For a mutant
  already read by hand, its recorded red cases of THIS binary; for a new one, declare what the claim is, sweep,
  and where the tool reads `violated` with other reds, READ why each is red before adding its `C` row: a case red
  for a reason nobody understood is not a pin.
- **`G`** — a case of the same binary that the mutation must not touch. It proves the binary announced the case
  and that the mutation did not simply break everything.
- **`S` — ONE leg, chosen by rule, never by habit; the row's why names the rule:**
  1. **A claim about a platform, a format or a toolchain names a leg of THAT platform**: a PE-native claim a
     Windows leg (swept from the main checkout — below), a Mach-O-native claim a Mac leg, a site compiled only
     under one platform's preprocessor branch a leg that compiles it. A second leg — a second arm, `<id>_<pin>`
     — only where the claim itself is about two platforms.
  2. **Any other claim names its AREA'S DEFAULT LEG — a leg that sweeps with MORE THAN ONE WORKER, on a machine
     that is NOT the one the round's own legs build on.** A claim about no platform can be shown red on any
     leg, so it goes where a sweep costs least:

     | area of the site | default leg |
     |---|---|
     | `analysis`, `asm`, `core`, `dss-config`, `hir`, `lsp`, `source-factory`, `tokenizer`, `tests` | `macos-arm64-debug` |
     | `ffi`, `link`, `lir`, `mir`, `opt`, `program` | `macos-arm64-debug` |

     **What makes a leg a poor default, whatever its compiler:**
     - **it sweeps with ONE worker** — a WSL distribution does — so its arms are driven one after another;
     - **its machine's heavy slots are the ones every local leg queues for** — WSL shares this host's with the
       Windows legs — so each arm waits behind the round's own builds, and an arm that waits out the
       admission's limit ends the whole sweep as not-admitted;
     - **its disk cannot hold a worker** — the arm64 VPS is no area's default: one worker's build is most of
       its free disk (✔MEASURED 2026-10-10: a worker's build of this repository asked for about 5 GiB under
       the Mac's clang and about 11 GiB under WSL's gcc);
     - **no worker fits beside the tree** — a Windows leg, from a lane's worktree (the path limit, below).

     The Mac has two heavy slots of its own and sweeps with two workers. ✔MEASURED 2026-10-12, sweeps from the
     main checkout while the round's lanes built: the WSL leg drove 16 arms in 70 minutes, one of them waiting
     24 minutes for a slot; the Mac leg drove 101 arms in about 50 minutes. ✔MEASURED 2026-10-10, both legs in
     one command: the Mac's four arms took nine minutes on warm workers; WSL's three arms took 25 minutes of
     the leg's own work — its first worker built whole, and two builds repeated from clean after a clock step
     the tool detected — AFTER a 39-minute wait for a slot.
     **A SCRIPT-DRIVEN arm (class 8) takes the same default.** Its runner starts the script's interpreter and
     reads the worker's own tree, which every leg has; nothing of it asks for one host. Only an arm whose red
     exists on one KIND of host — a path's separator, a file system's case — is a rule-1 arm, on a leg of that
     kind.
  3. **THE PRACTICE: a leg's arms go in ONE `check-mutations` command** — one unmutated control of each runner,
     warm workers — never one command per arm, and never a leg cut into batches by hand (who sweeps, above).
  4. The round's close sweeps each leg's own arms, one command per leg.

## Reading an arm that did not pass

The leg's line counts its arms by verdict, and `ARMS`, below the table, names each that did not pass and why.
`--json` carries, per arm: `verdict`, `detail`, `durationSeconds`, `worker`, `cases` and `declaredCases`, `reds` and
`declaredReds`, and `records` — the arm's directory `<run>/<leg>/arms/<arm>/` on the host the leg runs on, holding
`arm.json` and the logs of its build and its run; the runner's unmutated control is in
`<run>/<leg>/controls/<runner>/` (`run.log`, `report.xml`, `build.log`), a worker's whole build in
`<run>/<leg>/workers/<n>/`. A remote leg's are read with `dssharness run read-leg-path --legs <leg> --input
path=.harness-config/runs/<the LEG'S OWN run id>/<leg>/… -v` (`--input match=<regex>` keeps the lines that
match; the leg's own run id is the second `run` line of the sweep's output and the `runDirectory` of its leg).

- `reds` ≠ `declaredReds` — a case you did not declare went red, or a declared one stayed green. Open the run log,
  read each difference's failure text, and decide: the declaration was incomplete (add the `C` row, with how it
  was measured), or the pin does not bite where you thought (fix the test).
- `cases` ≠ `declaredCases` — the binary gained or lost a case since the arm was written.
- *"its diagnostic not said"* — the red came from another assertion than the one the arm names.
- *"its before-text not in its site exactly once"* — the site moved under the arm: re-take the text from the tree.
- *"no object … depends on '<site>'"* — the site is compiled into no object and none is declared to depend on it
  (the last section).
- `stopped`, *"the unmutated <runner> has N red"* — the runner is not self-contained, or is red on these bytes:
  read `controls/<runner>/run.log`. It fails the leg, so fix it or take the arm out before the next sweep.
- `failed`, *"the mutated build failed, and named no step that failed: configure exited 1"* — the mutation stopped
  the worker's own configure. Read `configure.log` in the arm's directory (a directory path given to
  `read-leg-path` lists it): where it holds the mutated mechanism's own refusal of the tree, the mutant cannot be
  an arm (the last section).

## The command lines

    # validate the registry and its texts (exit 10 naming the unknown arm = valid; exit 12 = problems, by line)
    dssharness check-mutations --arms no-such-arm --legs <leg> -C <tree>
    # one arm, on its leg
    dssharness check-mutations --arms <arm id> --legs <leg> --json -C <tree>
    # a fold's own arms (the `A` rows `git diff` shows added in the registry), on the legs they name
    dssharness check-mutations --arms <id>,<id>,<id> --legs <leg>,<leg> --json -C <tree>
    # every arm of a leg
    dssharness check-mutations --legs <leg> --json -C <tree>
    # THE GATE'S STEP, in the main tree at the round's close: each leg's own arms, one command per leg.
    # The legs are those the S rows name:
    #     grep '^S | ' tests/mutations/arms.registry | awk -F' [|] ' '{print $3}' | sort -u
    dssharness check-mutations --legs <leg> --json
    # the tool's own proof that it judges arms rightly with a leg's toolchain (no registry needed)
    dssharness check-mutations --self-test --legs <legs> -C <tree>

Every one of them is a leg command: started in the background, its whole output through the redactor. A sweep YOU
started and no longer want may be stopped — it puts every site back, still reports each arm it had judged, and
ends interrupted; a run that is not yours is never touched (dss-harness.md, STOPPING A RUN). A sweep takes a lock
of its own on the leg's variant and never holds off a build, test or sync of the leg — but one harness command at
a time on one tree stays the rule.

## What a sweep costs, and from which trees it can run

- **A worker is a whole copy of the tree and a whole build**, kept beside the tree as `<tree>.mutation-<key>w-<n>`
  and reused: before every sweep it is synced again by content, so its build stays warm, and only the first sweep
  of a tree pays the full build (✔MEASURED 2026-10-10 on the Mac: two workers built whole in about eight
  minutes). Each arm then costs the rebuild of what depends on its site, the runner's whole run, and its
  machine's admission (✔MEASURED the same sweep: 55 s, 59 s, 117 s and 266 s for four arms; the unmutated control
  of each runner is run once per leg).
- **Why ONE leg per arm.** An arm proves that its claim's TEST can fail; that is shown once, on any leg the claim
  runs on. Named on every leg of the gate, the same arm is driven eight times — and the two legs of one machine
  share that machine's heavy slots, so they queue rather than run side by side. At the measured one to four
  minutes an arm, a registry of a few hundred arms is hours on one leg each and days on eight. The gate's own
  eight runs already prove the claim's test is GREEN on every leg; the sweep adds that it can go RED.
- **Each worker and each arm is admitted as a unit of its own** (`help admission`): where another leg holds one of
  the machine's slots, every arm waits its settle time, so a sweep on a busy machine is paced by the admission,
  not by the arms. **An arm holds its slot only while it runs** and gives it back before the next asks, so a
  whole leg in one command starves no other leg of the machine — which is why a leg is swept WHOLE and never cut
  into batches by hand (who sweeps, above).
- **On WSL the clock can step under a build**, and the tool then trusts nothing that build stamped: it says
  *"rebuilding from clean, because a clock step"* and builds again (✔MEASURED 2026-10-10: twice in one sweep
  of three arms). It is a cost, never a verdict — the arm is judged on the build that followed.
- **A worker's path is twenty characters longer than its tree's.** On Windows, where the platform's limit binds,
  **a worker fits beside the MAIN checkout and never beside a lane worktree** (✔MEASURED 2026-10-10 from a lane
  worktree: `skipped-unavailable`, *"its build needs paths of 275 characters … and every path must stay under
  260"*). So a Windows leg is swept from the main checkout and from nowhere else — one of the reasons the sweep
  of a fold's arms is the orchestrator's, after the fold has landed (who sweeps, above).
- **A sweep's unmutated control is the one build of this repository that is HANDED its dependencies** (each
  worker is given the sources its leg's build fetched, inside the worker's own tree), so it is where the build
  stamp's handed-dependency term and the configure guard's hand-over rules are exercised: no leg of the gate
  builds that way. It is also why a mutant of the guard's excuse for handed directories stops every worker's
  configure (the last section).
- **Room**: the tool places a worker only where its copy and build fit, and says what keeps one out. `dssharness
  clean --legs <leg>` removes a leg's build directory AND its workers; `delete-agent` removes the workers beside a
  lane's worktree and beside each host's copy.

## What has no arm

Each is named with the measurement that shows it. **Such a mutant has NO row in the registry** (condition (e)),
**and no reading by hand stands in for the arm**: what cannot be an arm is STATED as absent, where the reader of
the code will find it, with what was measured.

- **A site no object depends on** — a file nothing compiles and no object is declared to depend on (a test's data
  file, a script). The verb reads `violated`, *"no object … depends on '<site>'"*, before it mutates anything.
  The answer is to make the dependency TRUE in the build graph — as the build stamp's includers are declared to
  depend on every file under `src/dss-config/` — never to route around the witness; until it is declared, no arm.
- **A mutation the tree's own CONFIGURE refuses** — a mutant of a configure-time guard that makes the guard refuse
  the tree it runs in. The verb reads `failed`, *"the mutated build failed, and named no step that failed:
  configure exited 1"* (✔MEASURED 2026-10-10 on two mutants of the build stamp's configure guard; each arm's
  `configure.log` held the guard's own refusal — one because a worker is handed its dependencies inside its tree,
  one in every tree). No red kind fits: a BUILD-RED arm must stop at an object that depends on its site, and a
  configure has none. Such a mutant has no row, and no hand reading either — a tree that does not configure builds
  nothing to read. **Write the mutant a configure SURVIVES instead**: one that switches the same refusal or excuse
  off where only the script's own fixture meets it, so the real tree still configures and the fixture's case goes
  red (✔ the guard's refusal of an undeclared directory: its mutant refuses none; the real tree, whose every
  directory is declared, configures; the fixture cases that expect a refusal go red). Where no such mutant exists
  — the claim IS that the real tree, or a worker handed its dependencies, configures — the configure of every tree
  and of every worker is that claim's test, and the report says so in those words.
- **A direction THE TREE ITSELF pins** — a guard that judges the source tree first and proves its refusals only on
  a tree it judged green. A mutant that blinds it to something the tree HOLDS reddens the tree's own verdict
  before any self-test arm runs, so every case of its entry but one goes red: no set an arm declares
  (✔MEASURED 2026-10-12: the guard of orphan test sources, blind to a registered C source while the tree held
  such sources — the full suite's one red on both Windows legs was the guard's own entry). The self-test arm
  pins the direction on its fixture, the tree pins it by being judged on every run, and the guard's own header
  says that the arm is absent and why. A mutant that leaves the tree green — a spelling the tree does not use, a
  rule only the fixture meets — IS an arm, like any other of the script class.
- **NOT a class: a claim that only a SCRIPT entry holds** (a `cmake -P` self-test, a runner action's own
  self-test). It gets a gtest case that DRIVES that entry — the entry's own command line, on the tree the binary
  was configured from, one case per claim, failing (never skipping) when the entry cannot be started — and then
  its mutants are arms like any other (class 8 above). The pattern has four properties: (1) the entry's command
  line is written ONCE in the build and baked into a generated header, so the test starts exactly what the entry
  was; (2) the test binary is registered under the entry's own ctest name and REPLACES the script entry — one
  truth, one run per leg; (3) it runs the script once per process and has one case per self-test arm, each
  reading that arm's own verdict line, plus a case that every arm is a case and every case an arm, so a new
  self-test arm with no case is red; (4) the build declares the object of the source that includes the header as
  depending on the script's files, so a mutated script rebuilds it and the tool witnesses the rebuild.
  **THE PATTERN IS IN THE TREE ONCE, AND THIS IS HOW ANY SCRIPT ENTRY'S PINS BECOME ARMS.** The driving code —
  run once, read each verdict line, the three assertions — is `tests/test_support/script_selftest.hpp`; the
  build's part is two functions of the top-level CMakeLists, `dss_declare_script_selftest(<id> ARGC <n> COMMAND …
  SCRIPTS …)` (the command line into the generated header) and `dss_use_script_selftest(<target> <id>)` (the
  header, and the object's dependency on the scripts, for the one target that runs the entry). A further script
  entry costs four things and no copy of either: the script prints ONE VERDICT LINE PER ARM (`<marker>ok <name>`,
  `<marker>FAILED <name> -- <what failed>`); one declaration; a test source that is a TABLE of the arms' names
  with the three shared cases, registered under the entry's own ctest name in the test directory nearest its
  subject; one opt-in. ✔ Built so: the build stamp's entry, `build/build_stamp_identity`, the dependency-record
  check's, `ninja_deps_selftest_guard`, and the guard of orphan test sources, `orphan_tests_guard`.
  ⚠ An entry that leaves the top-level CMakeLists for a test directory leaves the `repo-guard` label behind — the
  label is given to the TOP directory's entries whose name ends in `guard` — so it runs on every leg. Decide
  that, and say it where the entry is registered: a self-test of a program's own rules belongs on every leg (a
  host's paths are its own); a check of the source tree does not, and keeps the label by a `set_tests_properties`
  beside its registration (`orphan_tests_guard` does).
  ⚠ A script that judges the TREE before it proves itself gives that judgement a verdict line and a case of its
  own, and writes a red report to standard error: the driver keeps standard output in a file, and a red nobody
  can read in the test's log is a red nobody can fix from CI.
- **NOT a class: an example pin.** Rule 1: write the unit-tier claim.
