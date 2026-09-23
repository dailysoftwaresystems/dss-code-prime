# DssHarness — the harness this repository runs on

`DssHarness` is a cross-platform .NET tool, built in `dailysoftwaresystems/repo-harness` and published
on nuget.org. It REPLACED `scripts/`: the anchor registries, the worktrees, the legs, the builds,
the test runs and the corpus runners moved to it, so that one implementation serves every host and
no capability depends on whether a `.sh` and a `.ps1` agree. Every program this repository still
ships is one of its ACTIONS, under `.harness-config/runner/actions/`; there is no `scripts/` and no
`real-examples/` directory.

    dotnet tool install --global DssHarness      # or `dotnet tool update --global DssHarness`
    DssHarness --version                         # every leg must match the root host

⚠ **A leg installs only a PUBLISHED version.** A locally built tool cannot reach a remote leg, every
leg must run the same version as the machine driving it, a host ahead of the root refuses the run, and
there is no downgrade path. So a fix we need in the tool costs a release before any leg can use it.
⇒ Read the published set with
`curl -s https://api.nuget.org/v3-flatcontainer/dssharness/index.json`, never from a document: **this
one included**. ⇒ and ask the machine what it has with `DssHarness --version`.

★ **`DssHarness legs` INSTALLS OR UPDATES the tool on every host it measures**, bringing each leg up
to the version the driving machine runs — ✔MEASURED repeatedly, most recently when the three remote
hosts were carried forward by that command alone. ⇒ so `legs` is both the survey and the remedy for a
host that is behind, and a version skew is usually one command from gone rather than a task.

⚠ **DO NOT RECORD THE INSTALLED VERSION, OR ANY OTHER CURRENT-STATE NUMBER, IN THIS FILE.** Ask the
machine: `DssHarness --version`, and the published set from nuget's index as the block above says.
★ **The rule, because this file has been broken on it twice:** a statement about WHAT THE TOOL CAN DO
is durable and belongs here; a statement about WHAT IS TRUE OF THIS MACHINE RIGHT NOW is stale-able
and belongs in a command the reader runs. *"`buildOutputs` accepts a platform mapping"* never rots.
*"the installed version is X"*, *"N of M legs can run"*, *"this host holds N object files"* all rot,
and a stale number in a skill is WORSE than no number, because a contextless session trusts it.

**Capabilities this repository depends on, stated as contract rather than as a release note** — each
verifiable with a command, none carrying a version:
- `buildOutputs` accepts a **platform mapping**, so one entry can name a `.exe` on windows and a bare
  name elsewhere; a config naming no path for a platform some leg builds on is REFUSED at
  config-read time, naming the leg.
- A `tools` entry may declare `platforms`, so a host is never probed for a compiler its own platform
  does not use, and never counted against that host's legs.
- `toolchains[].platforms` is CHECKED: a leg naming a compiler absent from its own `os` is refused.
- **Exit 21 (`Incomplete`)** separates *nothing failed but not every leg reported* from a pass, with
  `complete` beside `passed` in the `--json` ledger. 21 is NOT a pass, and an interrupted run reports
  `complete: false` too.
- `sync --adopt "<host>"` takes over a checkout the harness did not create, naming the cost first.

## ★★★ THE UPSTREAM DOCS ARE THE AUTHORITY, AND THEY ARE READABLE FROM HERE

This file summarises the tool. **A summary rots, so read the source when the answer matters** — the
same rule the version block above applies to nuget, for the same reason.

    https://raw.githubusercontent.com/dailysoftwaresystems/repo-harness/refs/heads/release/stable/docs/architecture.md
    https://raw.githubusercontent.com/dailysoftwaresystems/repo-harness/refs/heads/release/stable/docs/releasing.md

★ **`release/stable` is the ref to read, and the choice is load-bearing.** ✔MEASURED: repo-harness
publishes `release/stable` and `release/beta` beside `main`, and a PR lands on `main` BEFORE the
version bump that deploys it — so `main` describes behaviour nobody can install for as long as that
window lasts, and a leg installs only a PUBLISHED version. Reading a ref that tracks what is RELEASED
removes that trap rather than explaining it. Read `release/beta` when the question is what is coming
next; read `main` only when the question is what is merged but undeployed.

⚠ **Never a local clone.** A clone under `repo-harness` is a WORKING COPY and can sit on any branch,
including the consumer-findings branch this repository files against, which may describe something
that never ships.

★★★ **AND THE INSTALLED BINARY STILL OUTRANKS EVERY DOCUMENT FOR ONE QUESTION: what can I run now.**
Ask it, do not look it up — `DssHarness --version`, `DssHarness help <topic>`, `DssHarness <verb>
--help`. ⚠ **WE HAVE PAID FOR GETTING THIS BACKWARDS.** The `clock-step-probe` runner action's own
comment records its `.yml` being written against a capability the then-installed tool refused with
exit **12**, *'is not a file name'*, and only a later release loaded it. Writing against a documented
capability before it is installable costs a release before any leg can use it.
⇒ When a document and the binary disagree, the binary wins for today, and the gap is a deploy you are
waiting on — a fact worth stating in the row rather than discovering in a red leg.

**What `architecture.md` answers, by section, so a question goes straight to one:** Layering ·
Worktrees · Anchor registries · Hosts, trees and legs · Parallel execution · Leg integrity ·
Cross-leg contamination · Syncing a tree · Predefined runners · Reporting · Exit codes ·
Success witnesses · Timeouts.

⇒ **Before filing anything upstream as a missing capability, check there.** The commonest mistake is
reporting a gap that is a verb nobody found; `help <topic>` and `<verb> --help` are the other two
places that close that question without costing a release.

★ Two answers from it that this repository kept re-deriving, recorded here because they decide whether
a script can be DELETED rather than merely replaced — both are statements about the CONTRACT, which is
why they are safe to write down:
- **Success witnesses.** A test invocation must declare a `successPattern`, and a runner phase may;
  the work passes only if the exit code is zero **AND** the pattern matches the command's own output.
  That is the contract the retired `run-gate` enforced, so the property SURVIVED that program's
  deletion rather than being lost with it.
- **Timeouts are deliberately not wall-clock** for phases and legs, because a time budget is a guess
  about workload size and honest runs exceeding it get killed. A phase declares a **stall** bound
  instead — no output for N seconds means hung — since output cadence stays stable when duration does
  not. ⇒ a long step must EMIT PROGRESS, or no bound can be set on it honestly.

## What it does today, and what is still a script

★ **Every verb this migration was waiting for SHIPS** — `build`, `test`, `sync` and `run` among
them. ⇒ confirm against the machine rather than against this sentence: `DssHarness --help` lists
every verb the installed tool has, and `DssHarness --version` says which tool that is.
⚠ **This table used to say `build` was 0.6.0, `test` 0.7.0, `sync` 0.8.0 and `run` 0.9.0.** Those
releases never happened — all four landed in 0.5.3 instead. A document still quoting them is stale.

| Verb | What it replaces here |
|---|---|
| `init`, `verify-git`, `legs`, `install-missing-tools`, `host-exec`, `help` | host discovery, the leg catalogue, host provisioning, remote invocation |
| `create-worktree`, `delete-worktree`, `list-worktree` | the plain lane-worktree lifecycle. ⚠ The `lane-worktree` ACTION stays for what the verbs lack: a VERIFIED evidence copy (`--preserve-to`) and the seed manifest `lane-fold` adjudicates a fold with |
| `write-anchor`, `set-anchor`, `read-anchor`, `read-anchors`, `check-anchor-balance` | the eight anchor launchers — **DELETED**. The `anchors` action stays as the row LIBRARY other actions load; `apply-registry-row` stays for a lane's one-line VERBATIM row file, a different input than `set-anchor`'s file-per-cell |
| `check-anchor-citations` | resolving cited ids, over the roots in `anchors.citationRoots` (`.harness-config` since 2026-09-18, `tests` since 2026-09-19). ✔MEASURED: `--current-tree` reads tracked and untracked-not-ignored files and never an ignored one. From round four — the release this tree lands with — a citation resolves only to the row whose id it EXACTLY is (case counts; `read-anchor` agrees), and an id that runs into a hyphen at a line's end is reported as CUT whatever rows exist, even cut at its first or second hyphen when the next line completes it (0.5.7 resolved by substring and saw no cut at all). ⚠ Still unseen: a wrap inside a string literal whose prefix happens to be a row, and an id cut at a hyphen that OPENS the next line (that one fails as unresolved, not as cut) — `check-wrapped-anchor-ids` owns the first. ⚠ The `check-anchor-registry` action stays for what no verb covers: the plans' table cell-width property and the retired-id matcher |
| `check-root-litter` | the `check-root-litter` program — **DELETED** |
| `fix-line-endings` | rewriting endings. ⚠ The `check-line-endings` action stays: HEAD and index blobs, a CR instrument that cannot see a CR, `--files`, and its watchdog |
| `check-ci-legs` | the `check-ci-legs` program — **DELETED** |
| `build` | the `local-build` program — **DELETED**. ⓘ It also reads `ninja -t deps` since 0.5.7; the `check-ninja-deps` action stays because CI runs `ctest` with no DssHarness installed |
| `test` | the `run-gate` program — **DELETED** |
| `sync` | the leg drivers, the carriages and the exclude derivation — **DELETED**. The `owning-tree` action is the one owner of *which tree am I standing in*; the `.sh` and PowerShell owners it stood beside were folded into it |
| `run` | EVERY program this repository ships: each is an action under `.harness-config/runner/actions/` with a `predefinedRunners` entry, started by `dssharness run <name>` |

⚠ **A verb EXISTING is not a script deleted, and this rule did not change when the verbs shipped.**
**Nothing is deleted before the verb that replaces it has been proven on all four legs**, and the
deletions land one commit per verb group — so a group that cannot be proven leaves everything after
it untouched. Until its replacement is proven, a script stays and is the only way to do that job.

## `legs` and `install-missing-tools`

- Both take **`--legs <names>`** — `--legs a,b` or `--legs a b`, naming legs or leg sets. Without it,
  every declared leg. Both also take `--json`, `-v/--verbose` and `-C/--directory`
  (✔MEASURED 2026-09-16 at `305604f1`, `DssHarness legs --help` and
  `DssHarness install-missing-tools --help`).
- ⚠ **`legs` is NOT a read-only probe.** Its own description: it *"installs or updates DssHarness on
  hosts that are behind"*. So does `host-exec`, before it runs anything remotely. Running `legs` to
  "just look" changes three remote hosts.
- `install-missing-tools` installs the .NET SDK on every WSL distribution and ssh host, plus every
  `tools` entry that declares how to install itself; **an entry that declares no install is reported,
  never installed**, and a privileged install with no declared credential is refused by name.

## This repository's configuration

`.harness-config/config.json` is the whole contract and it is tracked. It declares:

- **eight legs** — `{Debug, Release}` across Windows, WSL x86_64, macOS arm64 and the arm64 VPS — and
  a `gate` leg set holding all eight, which is the gate a round owes;
- **four toolchains** (MSVC and MinGW GCC on Windows, GCC on Linux, Clang on macOS) and two build
  configs;
- **three emulators** — qemu for arm64 inside WSL, qemu for x86_64 on the VPS, Rosetta on the Mac.
  ⚠ **Not wine**: a DssHarness emulator changes the processor and never the operating system, and the
  pe64-under-wine arm is cross-OS, so that launcher stays in `.harness-config/runner/actions/real-examples/c/sqlite/legs.json` with
  the rest of the corpus's own leg catalogue;
- **every remote leg's repository directory**, because `sync` creates it when it is not there;
- the never-transfer floor, the contention tools, the worktree budget, and both registry paths.

### What under `.harness-config/` is tracked, and what is ignored

⚠ This section used to say that everything except the contract and the runner actions is ignored, and
it counted "its worktrees" among them. **Both halves were wrong.** ✔MEASURED 2026-09-16 at `305604f1`
with `git ls-files .harness-config/` and `git check-ignore -v`:

| state | what |
|---|---|
| tracked | `config.json`, and **five** `.gitkeep` placeholders — `sshItems/`, `wslDistros/`, `runner/actions/`, `runner/.env/`, `runner/.secrets/` |
| ignored | everything else under `.harness-config/`, by `/.harness-config/*` |

★ **The placeholders are the point.** git stores no directory, so without them a fresh clone has
nowhere to put its connection data and nothing says the slots exist. The rule excludes each
directory's CONTENTS rather than the directory, because git cannot re-include a file whose parent is
itself excluded — and the negation names ONE file, so `sshItems/<host>/` and everything in it (a key,
an address, a user) is still excluded, and a `.gitkeep` deeper down is not what the negation matches.

### ★★★ The runner actions: ONE DIRECTORY PER ACTION

    .harness-config/runner/actions/<action-name>/<action-name>.yml

**Operator ruling, 2026-09-16**, verbatim: *"please put the actions inside
`.harness-config\runner\actions\<action-name>\<action-name>.yml`, because if needed to have
additional files like a big python command or anything else, you can use it inside the .yml file, for
example: `python3 ./bla.py`"*. ⚠ **This SUPERSEDES the flat `actions/<name>.yml`** that the migration
contract's S3 and R28 were written against.

★★★ **Operator ruling, 2026-09-21**, verbatim: *"I don't want .sh/.ps1 files inside
.harness-config\runner\actions. entrypoint is .yml, you can call .py files, BUT NOT .sh/.ps1 please.
They are specific per OS. I don't want this anymore"*. ⇒ An action's entry point is its `.yml`, and
every step starts `python3 <file>.py` (or a DssHarness verb); no `.sh` and no `.ps1` lives anywhere
under `.harness-config/runner/actions/`, and `check-scripts-index` refuses one by name. A program that
was a `.sh`/`.ps1` twin became ONE `.py` carrying the union of both twins' checks, so the same step
runs on every leg its runner declares — the host dispatch that picked a twin is gone with the twins.

★ **The reason is the `run` contract, and it is why a flat directory cannot work.** A `run` line is a
program and its arguments with **no shell**, so anything that is not a one-liner — a Python program, a
fixture, a data table — has to live in a FILE beside the action, and a flat directory gives it nowhere
to live that is obviously owned by that action. It is the rule this repository applies to every
program it ships: **one directory per program, named for it, assets alongside.**

⚠ **The table below was written when the tool could express only HALF of this ruling, and the layout
was declared as ruled anyway.** ⇒ **re-answer it before relying on it** — `DssHarness help runners`
states the current contract, and the upstream `architecture.md` section named at the top of this file
gives the reasons. A capability claim pinned to a release goes quietly false the release after; this
one has already moved once.

| half of the ruling | state |
|---|---|
| **supporting files** beside the action | ✅ works, and is in use — the clock-step probe's only `run` line starts a program under `actions/clock-step-probe/` |
| the **`.yml`** inside that directory | ⛔ refused: `DssHarness legs` passes (exit 0), `DssHarness run` fails **exit 12**, *'is not a file name. A runner's action names one file directly inside …'* |

★ **`config.json` names the ruled path regardless, and `DssHarness run` therefore refuses this
runner until repo-harness is fixed.** That cost was weighed rather than absorbed: no ctest entry
invokes `run`, and the four runners that are real work are not written yet. **Bending the layout to
what today's tool accepts would have made the tool's limitation permanent** — the next author would
read a flat tree as the convention and never learn a ruling existed.

★ **The upstream fix is NOT to delete the check.** `RepoHarness.Core.Runners.ActionFileParser.LoadAsync`
refuses any `action` whose `Path.GetFileName` is not the whole string, and its own comment gives a
sound reason: resolving a name with a separator would let a runner run a file from anywhere on the
machine, and *"only files a reviewer sees in this directory may declare what a runner does"*. A
subdirectory of `actions/` is equally reviewable, so the correct form is to combine, resolve to a full
path, and require it to stay UNDER the actions directory — which still refuses `..` and a rooted path.
⚠ **A second defect rides it:** `RepoHarness.Core.Configuration.HarnessConfigValidator` ACCEPTS this
value while the verb rejects it, so a config `legs` calls valid fails only when a runner is invoked —
against `help configuration`'s promise that every problem is listed at once when the file is read.
Both are **REPORTED** (see the closing section).

⚠⚠ **A `run` line's working directory is the REPOSITORY ROOT, not the action's directory.**
✔MEASURED by a step that printed its own cwd. **So the ruling's own example `python3 ./bla.py` does
NOT work as written** — it looks for `bla.py` beside `CMakeLists.txt`. Name a supporting file from the
root: `python3 ./.harness-config/runner/actions/<action>/bla.py`, which is verified working with
forward slashes on the Windows host. ★ That spelling also survives the `.yml` moving, because it was
never relative to the `.yml`.
★ **A step can ask to run beside its own files**: `workingDirectoryRoot: action` runs it in the
action's directory, and then `python3 ./bla.py` is right — ✔MEASURED through `DssHarness run` on the
Windows leg, and it is the form every action in this tree uses (`DssHarness help runners` states the
three roots, `tree` the default). A step may also name the operating systems it runs on (`runOn`);
a Python program needs none, which is the point of having one program instead of a twin per OS.
ⓘ `actions/` is tracked (its `.gitkeep`, and every action directory under it); `runner/.env/` and
`runner/.secrets/` are ignored, resolved against the MAIN checkout so worktrees share them. Precedence
is values, then secrets, then the runner's `env`, then the step's `env` — so a path set in the tracked
config **cannot** be overridden per machine, and a machine-specific path belongs only in `runner/.env/`.

### The worktrees root is somewhere else entirely

⚠ **The worktrees root is NOT under `.harness-config/`.** This repository's `worktrees.root` is
`.worktrees`, at the CHECKOUT root, ignored as a whole DIRECTORY by its own rule — deliberately, so
that the spelling without a trailing slash cannot answer NOT-IGNORED and ship N lane checkouts to
every gate host. `.harness-config/worktrees/` exists but is empty and untracked: it is the tool's
DEFAULT root, which this configuration overrides.
⚠ **`DssHarness help worktrees` states the default, not your configuration** — it says a worktree
sits below `.harness-config/worktrees/<name>`. ✔MEASURED here: `DssHarness list-worktree` reports
`.manifests`, which exists only under `.worktrees/`, so the behaviour honours `worktrees.root` and
the help topic's path is the one to distrust.

No secret is ever tracked, and no host address, user name or key path appears in any tracked file.

## The anchor registries, after the migration

**Two documents, and every anchor is a production anchor.**

| file | holds |
|---|---|
| `.plans/_deferred-anchor-registry-production.md` | every still-open row |
| `.plans/_deferred-anchor-registry-done.md` | every closed row, in one table |

The harness registry is gone: a defect in the harness is repo-harness's to fix, and a defect in this
repository's own build wiring or tests is a production row like any other. Its 187 open rows and the
archive's 544 closed ones are readable in git at the parent of the commit that deleted them.

⛔ **The door is `dssharness {write,set,read}-anchor`**, and `read-anchor <ID> --json` is still the
only sanctioned way to read a cell. This line used to say *"still `scripts/anchors/…` while those
scripts exist"*; ✔those eight launchers are DELETED on this branch, so the condition has fired.
⚠ **Two spellings changed with the door, and a stale command FAILS rather than misbehaving:** the
registry selector is `--pending` where the scripts said `--production` (`--done` is unchanged), and
the status vocabulary is FOUR values — `open`, `gated`, `disclosed`, `closed`.
ⓘ `🔵 DISCLOSED` is open work whose debt PRE-DATES the cycle. It is what stops
`check-anchor-balance` from reading a newly-FOUND pre-existing defect as newly-CREATED debt, and it
is not a way to silence the gate: a disclosed row stays in the pending registry and stays work.

## Exit codes

`0` success · `10` usage · `11` not initialised · `12` invalid configuration · `13` refused ·
`14` a tool is missing · `15` a host is unavailable · `20` the command failed · `70` internal ·
`130` cancelled. Codes 1–9 are reserved per command.

⚠ **`host-exec` returns the wrapped command's exit code unchanged**, so a remote 20 and a local 20 are
the same number for two different reasons unless you know which leg was asked.

## The rule that matters most

**A defect in DssHarness is a repo-harness issue, never a local workaround — AND IT IS REPORTED TO
THE OPERATOR.** The whole point of the move is that one implementation serves every host; a patch
here that routes around the tool re-creates the drift the migration exists to end. Reproduce it,
state the measurement, and file it there.

★★★ **The reporting half is an operator ruling, 2026-09-16**, verbatim: *"please also put in
dss-cycle skill that any issue found in DssHarness must be reported to me (the operator)"*. It exists
because the rule used to say only where the FIX goes and was silent on who must be TOLD — and under
the cycle's silence-is-the-default output contract, a tool defect that is neither a failure nor a
blocker for this cycle had **no route to the operator at all**. It usually is neither: the cycle
routes around it by using the script that still exists, and the finding dies in a lane report.

**A report owes four things, and nothing more:**
1. what was run; 2. what happened; 3. what should have happened;
4. **the exact source symbol in repo-harness that produces it** — path + SYMBOL, ⛔ **never a line
   number**.

- **Report it in the cycle that FINDS it**, never filed for later. Same shape as *fix it when you
  face it*.
- **A DssHarness defect is not a `D-*` row here** — it is not this repository's defect — so the cycle
  report is its only route out. See the `dss-cycle` output contract, item 6.
- ⚠ **Reporting does not replace fixing our side.** Where the defect has a correct LOCAL expression
  that is not a workaround — spelling a `minVersion` with three components because the tool documents
  semantic versions, say — fix it here AND report the tool's part. The two are not alternatives.
