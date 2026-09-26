# Worktree & agent operations

## Contents
- A lane worktree lives inside the repo, at `.worktrees/<short-name>` (operator ruling 2026-08-26) —
  the rule in brief, as the cycle's root file stated it
- H. Worktree & agent operations
  - H.0 A byte-changing measurement belongs in a worktree, not the shared tree
  - H.0a …and never under the session scratch directory
  - H.0b …and inside the repo, at `<repo>/.worktrees/<short-name>` — one owner; `remove` and `land`;
    the lane verbs resolve their tree from their own location; the MAX_PATH budget; `.gitignore`
  - Every new worktree inherits root permissions — the allowlist and the sandbox; prefer the root for
    fast, sequential cycles

## ★★★ A LANE WORKTREE LIVES INSIDE THE REPO, AT `.worktrees/<short-name>` — operator ruling 2026-08-26

> *"I want the worktrees implementation to be inside the project root, .worktrees directory (where
> 100% of it's internal content ignored by .gitignore). This way we stop contaminating builds
> outside repository bounds."* … *"worktrees MUST be ignored by ALL host copies to run legs"*

**This SUPERSEDES the short-absolute-root convention** (`C:/dssp40k`, `C:/dss-<cycle><lane>-rod`).
A worktree outside the repository is a full checkout — plus its `build/` — that nothing owns and no
guard can see. ✔MEASURED 2026-08-26: the tree was already carrying **9,661 files / 410 MB** of
orphaned checkouts under `.claude/worktrees/`, three full copies, one of them 287 MB, and
**`git worktree list` knew about none of them**.

**★ ONE OWNER: DssHarness — `dssharness create-worktree` / `delete-worktree` / `list-worktree`** (operator,
2026-09-24): **never hand-roll `git worktree add` in a lane.** Seeding, folding and landing run through the
lane-fold action's own program — `lane-fold.py seed` and `land` — as a NAMED INTERIM until the action
declares them as manual steps, the only route besides a reported DssHarness bug; `seed` is MANDATORY right
after `create-worktree`, before any work. The `lane-worktree` action stays only for the VERIFIED evidence
copy `land` asks it for (`--preserve-to`). Every directory they use is read from configuration
(`references/worktrees.md`).

```bash
dssharness create-worktree k      # -> <repo>/.worktrees/k (worktrees.root); a name is at most 10 characters
python3 .harness-config/runner/actions/lane-fold/lane-fold.py seed k   # MANDATORY, before any work: resets the seed manifest (P57)
python3 .harness-config/runner/actions/lane-fold/lane-fold.py land k production --apply   # a FINISHED lane leaves this way
dssharness delete-worktree k      # after land, its host copies; a lane NOT landed, the worktree too
```

⚠ **`seed` is MANDATORY, right after `create-worktree` and before any work** — `seed k --empty` for a lane
created at HEAD and given nothing. It resets the lane's seed manifest and records its base. ✔MEASURED P57: a
stale manifest left by an earlier lane of the same name makes the fold SILENTLY DROP the lane's work, and
`create-worktree` does not reset it.

Four clauses, and each is measured rather than asserted — detail in `references/worktrees.md` §H.0b:

1. **`.gitignore`'s `/.worktrees/` rule is what satisfies the "ALL host copies" clause.** The
   transport derives what it carries from git, so that one line is what stops four full repo copies
   riding to macOS and the arm64 VPS on every push. ★ It is ALSO declared independently of git:
   ✔MEASURED 2026-09-23, `.harness-config/config.json`'s `sync.neverTransfer` names `.worktrees`,
   `.claude/worktrees`, `.temp`, `build`, `scratchpad` and `test-scratch`, and `.secrets` at any
   depth (`**/.secrets`, which covers the root one too) — once, for every host. `dssharness sync --dry-run` lists every path it would write, which is how to CHECK
   this rather than trust it.
2. ⚠ **The MAX_PATH budget is now SPENT, not slack.** The move costs **46 characters** on every
   build path: longest build-relative suffix **163**, so `C:/dssp40k` had **87 spare** and
   `.worktrees/k` has **46**. `lane-worktree.py` refuses by arithmetic (exit 3) a name whose longest
   build path would not stay under `worktrees.pathLimit`, every term read from
   `.harness-config/config.json` and named in the refusal — the defect it prevents fails as a per-TU
   compile error in files the lane never touched. ⏳ SCRIPT-ERA (superseded 2026-09-24: `dssharness create-worktree` holds each worktree's longest build path under worktrees.pathLimit, and a name within worktrees.maxNameLength, 10 characters; see dss-harness.md)
   ⇒ **Keep lane names SHORT** (`k`, `l`, `rod`); a descriptive name spends that margin.
3. **The lane that takes a worktree owns removing it** — `lane-fold.py land` for a finished lane, then
   `dssharness delete-worktree` for the host copies `land` leaves; `dssharness delete-worktree` alone
   otherwise — and the registration must go with it, because a stale
   registration lives in `.git/worktrees/` and **never appears in `git status`**.
4. ⚠ **`-fd`, never `-fdx`.** `git clean -fd` does not delete ignored paths, so a live worktree
   survives a leg restore; `-fdx` would destroy it mid-build.

✔**The move was measured, not hoped:** a real 2,792-file worktree at `.worktrees/probe` moved
**zero** of the 16 runnable registered guards — identical verdicts and output volume — and
`git status` reported 0 lines for it.

## H. Worktree & agent operations — every new worktree inherits root permissions

### ★★ H.0 — A BYTE-CHANGING MEASUREMENT BELONGS IN A WORKTREE, NOT THE SHARED TREE
✔MEASURED 2026-08-13, and it cost real confusion. A lane needed to measure the corpus-wide byte
effect of a config change, so it applied the change to the **shared** working tree, measured for
~30 minutes, then reverted. In that window the change was live where every other lane and the
main loop could see it — and it was picked up and landed by a different actor acting on an
operator decision the lane had no way to observe. The lane then reported its own change as an
unexplained mutation of the tree, and was **right to**: from where it sat, a file changing
underneath it is not consent.

Nobody did anything wrong; the shared tree was the wrong venue. So:

### ★★ H.0a — AND IT GOES AT A **SHORT** ABSOLUTE PATH, NEVER UNDER THE SESSION SCRATCH DIRECTORY

✔MEASURED 2026-08-23 (cycle P29).
Two cycle rules — *mutate in a worktree* and *every temporary file lives under the session scratch
directory* — compose into a path that **cannot be built on Windows**:
`…/AppData/Local/Temp/claude/C--Source-DailySoftware-dss-code-prime/<uuid>/scratchpad/<cycle>/lane-<x>/wt`
is **~150 characters before any build path is appended**, and the generated `.obj.d` paths then
exceed MAX_PATH.

⚠⚠ **THE FAILURE MODE IS THE DANGEROUS PART.** It is not a link error at the end — it is a **per-TU
compile error in files the lane never touched** (`fatal error: opening dependency file
tests\core\CMakeFiles\…\<name>.cpp.obj.d: No such file or directory`, repeated across `tests/core`),
so it reads as **somebody else's breakage** and sends the lane to investigate an unrelated subsystem.
✔The control that settles it: the same commit, same patch, same generator, re-created at
`C:/dss-<cycle><lane>-rod` builds clean.

⇒ **A worktree is NEVER placed under the session scratch directory.** The scratch-directory rule
governs a lane's *files*; it was never meant to govern a *build root*, and on this host the two
cannot both be satisfied.

### ★★★ H.0b — AND IT GOES INSIDE THE REPO, AT `<repo>/.worktrees/<short-name>` — operator ruling 2026-08-26

> *"I want the worktrees implementation to be inside the project root, .worktrees directory (where
> 100% of it's internal content ignored by .gitignore). This way we stop contaminating builds
> outside repository bounds."* … and, the same day, as an absolute: *"worktrees MUST be ignored by
> ALL host copies to run legs"*.

**This SUPERSEDES the short-absolute-root convention** (`C:/dssp40k`, `C:/dss-<cycle><lane>-rod`).
Those roots kept the tree clean but scattered full checkouts — each with its own `build/` — across
the filesystem, where nothing owned them, no guard could see them, and `git worktree list` was the
only record they existed. ✔MEASURED 2026-08-26, and it is why the ruling is right: the repository
was ALREADY carrying **9,661 files / 410 MB** of orphaned checkouts under `.claude/worktrees/`
(three full copies, one 287 MB with its own `build/perf-lane/`), and **`git worktree list` knew
about none of them**.

**★ ONE OWNER: DssHarness** — `dssharness create-worktree <name>`, `delete-worktree <name>`,
`list-worktree` (operator, 2026-09-24). **Never hand-roll `git worktree add` in
a lane.** A location rule is only as good as the last person who remembered it, and this skill
already records what happens to a rule that lives only in a document.

```bash
dssharness create-worktree k                                                            # -> <repo>/.worktrees/k
python3 .harness-config/runner/actions/lane-fold/lane-fold.py seed k                    # MANDATORY, before any work: resets the seed manifest (P57)
python3 .harness-config/runner/actions/lane-fold/lane-fold.py land k production --apply  # the way a FINISHED lane leaves
dssharness delete-worktree k                                                            # after land, its host copies; a lane NOT landed, the worktree too
```

⚠ **`seed` is MANDATORY, right after `create-worktree` and before any work** — `seed k --empty` for a lane
created at HEAD and given nothing. It carries the main tree's uncommitted state in, RESETS the lane's seed
manifest and records its base. ✔MEASURED P57: a stale manifest left by an earlier lane of the same name makes
the fold SILENTLY DROP the lane's work, and `create-worktree` does not reset it — it knows nothing of
lane-fold's manifests, and lane-fold keeps a landed lane's manifest on purpose.

⚠ `dssharness run lane-fold` runs that action's self-test and nothing else — its `.yml` declares no other step
(✔MEASURED 2026-09-25, DssHarness 0.5.12) — so seeding, folding and landing run as its program's verbs, as
above: a NAMED INTERIM, and the only route besides a reported DssHarness bug, which ends when the action
declares them as manual steps (scheduled: the harness lane's next-PR item).

★ **Every directory both tools use is READ, never spelled in them:** the lanes' root and the
evidence roots from `.harness-config/config.json` (`worktrees.root`, `worktrees.evidenceRoots`), and
lane-fold's two bookkeeping directories beside the lanes — the seed manifests `add` writes and `fold`
reads, and the evidence `land` keeps — from `lane-fold/lane-fold-config.json` (`manifests`,
`evidence`). Both programs read that one file, so a change there moves both. ⏳ SCRIPT-ERA (superseded 2026-09-24: with `dssharness create-worktree`, lane-fold's `seed` writes the manifest that `add` used to, which is why `seed` is MANDATORY right after creation; see the block above)

⚠ **`remove` REFUSES (exit 8) a worktree that still carries the lane's work** — by its own
`git status`, a tracked modification or an untracked file that is not ignored (seeded paths
included); or a commit its HEAD holds that no ref of the repository reaches, asked at the repository
root as `git rev-list <HEAD> --not --glob=refs/*` (removing the worktree would orphan that commit,
and gc would delete it) — until it is told `--discard-work`, which names every path
and commit it discards. It cannot tell folded work from unfolded work; `lane-fold.py land` can,
builds that flag only from its own "nothing left to fold" measurement, and refuses a lane whose HEAD
left its base: past the recorded base, or, for a format-1 manifest that records none, holding such a
commit.

⚠ **`remove` REFUSES (exit 7) a worktree whose evidence roots (`worktrees.evidenceRoots`:
`scratchpad/` AND `.temp/`) hold any file**, until it is told `--preserve-to <dir>` or
`--discard-evidence`. ✔MEASURED P66: the gate used to count `scratchpad/` only, while
every live lane kept its evidence under `.temp/<lane>-scratch/`, so it counted zero for all of them.
A preserve refuses a destination inside the worktree or one already holding a same-named file with
other bytes, and re-reads every file at the destination. `--discard-scratchpad` is retired and refused. [→ plain removal is `dssharness delete-worktree <name>`: without `--force` it refuses work that would be lost, a locked worktree, and a worktree whose evidence directories hold anything — `--delete-evidence` waives only that last check, `--force` all three — and it removes the worktree's host copies too; the action's `remove` stays for `--preserve-to`, and `land` calls it](dss-harness.md)

★ **A finished lane is LANDED, never removed by hand:** `lane-fold.py land <lane> production`
folds it, applies its `row/` cells through the row writer all-or-nothing and re-reads each row, checks
nothing is left to fold, keeps the whole evidence set under `.worktrees/.evidence/<lane>-<stamp>/`
(the lanes' root and lane-fold's `evidence`, both read),
and only then removes the worktree. Without `--apply` it is a dry run; a stopped landing can be re-run. SUPERSEDED 2026-09-25 by the one row format, anchor-rows' rows directory — until `land` switches to it in the next PR, it reads only `<ANCHOR>.<cell>` files under the lane's `.temp/<lane>-scratch/row/` and refuses, loudly, a lane without them; a lane's rows directory is applied by the anchor-rows action (see lane-discipline.md) [→ `land` leaves the worktree's host copies: follow it with `dssharness delete-worktree <lane>`, which removes them even for a name whose worktree is gone — `land` moves to `delete-worktree` in the next PR](dss-harness.md)

⚠ **THE LANE VERBS RESOLVE THEIR TREE FROM THE PROGRAM'S OWN LOCATION, NOT FROM YOUR
`cwd` — SINCE P53.** The lane-worktree shell and PowerShell programs of the time and
`lane-fold.py` each carried a bare `git rev-parse --show-toplevel`, so
every path they derived was rooted at whichever repository the CALLER happened to be standing
in. ✔MEASURED in P52 it silently redirected a whole guard run at another repository, and
✔MEASURED again in P53 it hit the ORCHESTRATOR live: a shell that had drifted into
`.worktrees/io` made `lane-fold fold io --apply` resolve `…/.worktrees/io/.worktrees/io`.
⇒ **Closed: the lane verbs' repo root is no longer `cwd`-keyed.** Each verb anchors on its own
file through the one owner, `.harness-config/runner/actions/owning-tree/owning-tree.py`, and every
one accepts an explicit **`--repo <path>`** for a caller that genuinely means another tree.

⚠ **DO NOT "IMPROVE" THIS INTO "the tree that owns `.worktrees/`".** The orchestrator proposed
exactly that in P53 and the lane REFUTED it by measurement: from inside `.worktrees/lw`, the
main-checkout answer resolves `remove io` onto a LIVE SIBLING LANE'S uncommitted work, where the
script-anchored answer resolves to a path that does not exist and refuses. It is also wrong for a
submodule, where `--git-common-dir` names `<super>/.git/modules/<child>` — rooting a removal
*inside* `.git`. The blast radius inverts; the original predicate was right.

⚠ **THE MAX_PATH BUDGET IS NOW SPENT, NOT SLACK — AND THIS IS THE ONE THING TO CARRY FROM H.0a.**
Moving from a 10-char root into the repository root costs **46 characters** of the MAX_PATH budget
on every build path. ✔MEASURED 2026-08-26 in a live lane worktree: the longest build-relative
suffix is **163 chars**, so `C:/dssp40k` totalled 173 (**87 spare**) and `<repo>/.worktrees/k`
totals 214 (**46 spare**). It fits — but the margin more than halved, and this repository's test
names are what dominate that suffix and keep growing.
⇒ `lane-worktree.py` **refuses by arithmetic** (exit 3) a name whose longest build path would not
stay under `worktrees.pathLimit`: the lanes' root and the name, `/build/<longest variant>/`, then
`pathBudgetReserve` and `pathBudgetMargin` — every term read from `.harness-config/config.json`, and
every term, with how long a name would still fit, named in the refusal. ✔ Its self-test exercises
both sides: a name ONE character over the exact budget is refused, one character shorter is admitted. ⏳ SCRIPT-ERA (superseded 2026-09-24: `dssharness create-worktree` applies the same budget from .harness-config/config.json and caps a name at worktrees.maxNameLength, 10 characters; see dss-harness.md)
⇒ **Keep lane names SHORT** — `k`, `l`, `rod`. A descriptive name spends the margin that protects
the next long test name.

⚠ **`.gitignore`'s `/.worktrees/` rule is what keeps lane checkouts off every gate host**, and it
is a REQUIREMENT, not tidiness. Since 2026-08-26 the carriages derive their exclude list from git
(and `sync.neverTransfer` in `.harness-config/config.json` names it a second time), so that one
line is what stops four full repo copies riding to
macOS and the arm64 VPS on every push — and a gate host holding one runs somebody's uncommitted
`examples/` corpus and reports it as the cycle's. It is therefore ALSO pinned in that script's
`MUST_NEVER_TRAVEL` floor, which re-asks git and **refuses the carriage** if the rule is edited
away. ✔The floor's refusal arm is exercised: removing the one line makes it exit 3, naming
`.worktrees/`. ⏳ SCRIPT-ERA (superseded 2026-09-24: the carriage is `dssharness sync`, whose floor is sync.neverTransfer in .harness-config/config.json; see dss-harness.md)
⚠ The anchoring (`/.worktrees/`, not `worktrees/`) is deliberate — a bare glob would match a
future `examples/**/worktrees/` fixture, and this repo has already paid for an unanchored rule.

✔**MEASURED, so the move is not a hope:** a real 2,792-file worktree at `.worktrees/probe` moved
**zero** of the 16 runnable registered guards — identical verdict and output volume before and
after — and `git status` reported **0 lines** for it.

⚠ **And read a build's exit code from the PROCESS, not from the tail of a pipeline.** The same run
reported `EXIT=0` over that failure because `$LASTEXITCODE` after a PowerShell pipeline is the LAST
command's — `Select-String`'s — not `cmake`'s. An instrument that reports success over a failed build
is the same class as a red-on-disable whose mutant never compiled in.

- **Any experiment whose whole point is that bytes change** — byte-neutrality probes, red-on-disable
  mutants, "what does the corpus look like if…" — runs in a `git worktree`, never in the shared tree.
  The lane hands back a `git apply --check`-verified **patch**; the main loop decides whether it lands.
- ⚠ **A revert is not a safe undo once a concurrent lane has edited the same file.** Copying a
  pristine copy back destroys the other lane's interleaved work. Reverse-apply the specific patch
  (`git apply -R`), or leave it and report.
- ★ **Do not start a revert war with a concurrent actor.** Report the state and wait — that lane's
  judgement here was exactly right, and the only thing it lacked was information the main loop had.
- ⇒ The main loop owes the other direction: when it applies a lane's patch on an operator decision
  that lane could not see, **tell the lane**. An unexplained tree mutation is otherwise indistinguishable
  from a defect, and a careful lane will spend real effort investigating it.

A worktree spawned for a cycle (an implementation agent, an independent audit, a probe) is
the **same repo on the same machine** as root. It must therefore run with the **same
permissions root already has** and **never re-prompt** for what root trusts. This is
non-negotiable: a worktree that stalls or spams permission prompts is an operational defect,
not the user's job to click through. Enforce it on both layers:

1. **Permission allowlist — blanket, not per-command.** The project
   `.claude/settings.local.json` `permissions.allow` list must carry the blanket tool grants
   **`Bash(*)` AND `PowerShell(*)`** (on Windows, cmake/ctest builds run through PowerShell —
   `Bash(*)` alone leaves every worktree *build* command prompting and being approved one at a
   time). If a tool starts prompting inside a worktree, add its `(*)` blanket entry to root's
   settings rather than approving individual commands; the worktree inherits it immediately.
2. **Sandbox — agents disable it for trusted build/VCS commands.** Any worktree/agent prompt
   that is *not* an allowlist miss is the **sandbox** flagging a write outside the Windows
   workspace root (typically the agent's WSL `/home`/`/tmp` build dirs). Spawn **every**
   worktree agent with a standing instruction to pass `dangerouslyDisableSandbox: true` on its
   build / compile / `git` / `wsl` commands (trusted repo operations). Bake that line into the
   agent prompt verbatim — do not rely on the agent inferring it.

**Corollary — prefer the root for fast, sequential, low-risk cycles.** A worktree buys
parallel isolation; it costs permission friction and (if spawned at a stale base) a slow
build. When a cycle is *sequential* and *config-shaped* (e.g. a shipped-descriptor cycle whose
re-probe compiles SQLite), run it in the **root**: it inherits root permissions automatically
and gets the current HEAD's compile-time wins (post the c97 resolver fix, the SQLite re-probe
is ~seconds, not ~15 minutes). Reserve worktrees for genuinely parallel or higher-risk *code*
changes — and when you use one, **reset it to the current HEAD** first (worktrees can spawn at
a stale base like p18), so it too builds fast.
