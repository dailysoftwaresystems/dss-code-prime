# DssHarness — the harness this repository runs on

`DssHarness` is a cross-platform .NET tool, built in `dailysoftwaresystems/repo-harness` and published
on nuget.org. It is replacing `scripts/`: the anchor registries, the worktrees, the legs, the builds,
the test runs and the corpus runners all move to it, so that one implementation serves every host and
no capability depends on whether a `.sh` and a `.ps1` agree.

    dotnet tool install --global DssHarness      # or `dotnet tool update --global DssHarness`
    DssHarness --version                         # every leg must match the root host

⚠ **A leg installs only a PUBLISHED version.** A locally built tool cannot reach a remote leg, every
leg must run the same version as the machine driving it, a host ahead of the root refuses the run, and
there is no downgrade path. So a fix we need in the tool costs a release before any leg can use it.
⇒ Read the published set with
`curl -s https://api.nuget.org/v3-flatcontainer/dssharness/index.json`, never from a document: **this
one included**. ✔MEASURED 2026-09-16 at `305604f1` — published `0.5.1`, `0.5.2`, `0.5.3`; installed
here `0.5.3`; `DssHarness legs` reported `OK - 8 of 8 leg(s) can run` after installing 0.5.3 onto wsl
Ubuntu, ssh macos and ssh arm64-vps (that run was the orchestrator's, same day, same commit).

## What it does today, and what is still a script

★ **Every verb this migration was waiting for SHIPS.** ✔MEASURED 2026-09-16 at `305604f1`:
`curl -s https://api.nuget.org/v3-flatcontainer/dssharness/index.json` lists `0.5.1`, `0.5.2`,
`0.5.3`; `DssHarness --version` reports `0.5.3`; and `DssHarness --help` lists every verb below.
⚠ **This table used to say `build` was 0.6.0, `test` 0.7.0, `sync` 0.8.0 and `run` 0.9.0.** Those
releases never happened — all four landed in 0.5.3 instead. A document still quoting them is stale.

| Verb | What it replaces here |
|---|---|
| `init`, `verify-git`, `legs`, `install-missing-tools`, `host-exec`, `help` | host discovery, the leg catalogue, host provisioning, remote invocation |
| `create-worktree`, `delete-worktree`, `list-worktree` | `scripts/lane-worktree` |
| `write-anchor`, `set-anchor`, `read-anchor`, `read-anchors`, `check-anchor-balance` | `scripts/anchors`, `scripts/apply-registry-row`, `scripts/burndown-queue` |
| `check-anchor-citations` | `scripts/check-anchor-registry` |
| `check-root-litter` | `scripts/check-root-litter` |
| `fix-line-endings` | `scripts/check-line-endings` |
| `check-ci-legs` | `scripts/check-ci-legs` |
| `build` | `scripts/local-build`; with `--time`, `scripts/profile-compile` and `scripts/compile-bench` |
| `test` | `scripts/run-gate` |
| `sync` | the leg drivers (`wsl-leg`, `remote-leg`, `macos-leg`), `leg-tree`, the carriages (`ssh-macos`, `ssh-arm64-vps`, `carriage-excludes`, `check-carriage-paths`) |
| `run` | the sqlite corpus driver, `sqlite-round-trip`, `sqlite-runtime-bench` and `macho-alias-ld64-matrix`, as predefined runners |

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
  pe64-under-wine arm is cross-OS, so that launcher stays in `real-examples/c/sqlite/legs.json` with
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

★ **The reason is the `run` contract, and it is why a flat directory cannot work.** A `run` line is a
program and its arguments with **no shell**, so anything that is not a one-liner — a Python program, a
fixture, a data table — has to live in a FILE beside the action, and a flat directory gives it nowhere
to live that is obviously owned by that action. It is the same rule this repository already applies to
`scripts/`: **one directory per script, named for the script, assets alongside.**

⚠ **MEASURED 2026-09-16 against DssHarness 0.5.3: the tool can express HALF of it, and the
layout is declared as ruled anyway.**

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

⛔ **The door is still `scripts/anchors/{write,set,read}-anchor` while those scripts exist**, and
`read-anchor <ID> --json` is still the only sanctioned way to read a cell. `write-anchor` gained
`--relocating` for a row that exists elsewhere in the repository and is being moved rather than named.

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
