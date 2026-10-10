# Worktree operations

## Contents
- Worktrees live inside the repo, under `.worktrees/` (operator ruling 2026-08-26) — the rule in brief;
  a LANE's worktree is a DssHarness agent's (`references/orchestration.md`)
- H. Worktree operations
  - H.0 A byte-changing measurement belongs in a worktree, not the shared tree
  - H.0a …and never under the session scratch directory
  - H.0b …and inside the repo, under `<repo>/.worktrees/` — one owner; the MAX_PATH budget; the ignore rules
  - Every new worktree inherits root permissions — the allowlist and the sandbox; prefer the root for
    fast, sequential cycles

## ★★★ WORKTREES LIVE INSIDE THE REPO, UNDER `.worktrees/` — operator ruling 2026-08-26

> *"I want the worktrees implementation to be inside the project root, .worktrees directory (where
> 100% of it's internal content ignored by .gitignore). This way we stop contaminating builds
> outside repository bounds."* … *"worktrees MUST be ignored by ALL host copies to run legs"*

**This SUPERSEDES the short-absolute-root convention** (`C:/dssp40k`, `C:/dss-<cycle><lane>-rod`).
A worktree outside the repository is a full checkout — plus its `build/` — that nothing owns and no
guard can see. ✔MEASURED 2026-08-26: the tree was already carrying **9,661 files / 410 MB** of
orphaned checkouts under `.claude/worktrees/`, three full copies, one of them 287 MB, and
**`git worktree list` knew about none of them**.

**★ ONE OWNER: DssHarness** (operator, 2026-09-24): **never hand-roll `git worktree add`.**
- **A LANE's worktree is an AGENT's** — `dssharness create-agent <o> <a>` makes it at
  `<repo>/.worktrees/<o>/<a>` and seeds it, `fold-agent` folds it, `delete-agent` removes it with its host
  copies after the lane's LAST review. That lifecycle is `references/orchestration.md`, MANDATORY since
  2026-09-29; this file never restates it.
- **A worktree that is NOT a lane** — a probe, a byte-changing measurement (H.0) — is a plain one:

```bash
dssharness create-worktree k                  # -> <repo>/.worktrees/k (worktrees.root); never an orchestrator's name
dssharness delete-worktree k                  # add --discard-uncommitted / --delete-evidence only once what it holds is kept elsewhere
dssharness list-worktree [--hosts] [--json]   # every worktree (an agent's as o/a), its host copies, and the copies gone worktrees left
```

ⓘ Without its two flags `delete-worktree` refuses a worktree holding uncommitted work or evidence, and on
Windows it refuses, whole and before anything is removed, one something holds -- a shell standing in it, a file
open without delete sharing, a program running from it -- naming what holds it.

Four clauses, and each is measured rather than asserted — detail in §H.0b:

1. **The ignore rules are what satisfy the "ALL host copies" clause.** `dssharness init` writes them —
   `/<worktrees.root>/*`, here `/.worktrees/*`, and the fixed `/.orchestrators/*`, each beside its tracked
   `.gitkeep` — and `sync` withholds both directories by name, independently of git. ★ `.worktrees` is ALSO
   declared in configuration; `.orchestrators` rests on the tool's own withholding:
   ✔MEASURED 2026-09-23, `.harness-config/config.json`'s `sync.neverTransfer` names `.worktrees`,
   `.claude/worktrees`, `.temp`, `build`, `scratchpad` and `test-scratch`, and `.secrets` at any
   depth (`**/.secrets`, which covers the root one too) — once, for every host. `dssharness sync --dry-run`
   lists every path it would write, which is how to CHECK this rather than trust it.
2. ⚠ **The MAX_PATH budget is SPENT, not slack.** `create-worktree` and `create-agent` refuse up front a
   name whose longest build path would pass the path limit — the PLATFORM's own: this repository declares
   no `worktrees.pathLimit`, on purpose, because a declared one replaces the platform's limit on every host
   (dss-harness.md) —, the reserve and the margin read from
   `.harness-config/config.json` and the longest name that still fits named in the refusal — the defect it
   prevents fails as a per-TU compile error in files the lane never touched. An agent's orchestrator and
   agent names share the one budget. ⇒ **Keep names SHORT**; a descriptive name spends the margin.
3. **Whoever creates a worktree removes it** — the orchestrator an agent's with `delete-agent`, after its
   last review; a probe's creator with `delete-worktree` — and the registration goes with it, because a stale
   registration lives in `.git/worktrees/` and **never appears in `git status`**.
4. ⚠ **`-fd`, never `-fdx`.** `git clean -fd` does not delete ignored paths, so a live worktree
   survives a leg restore; `-fdx` would destroy it mid-build.

✔**The move was measured, not hoped:** a real 2,792-file worktree at `.worktrees/probe` moved
**zero** of the 16 runnable registered guards — identical verdicts and output volume — and
`git status` reported 0 lines for it.

## H. Worktree operations — every new worktree inherits root permissions

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

### ★★★ H.0b — AND IT GOES INSIDE THE REPO, UNDER `<repo>/.worktrees/` — operator ruling 2026-08-26

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

**★ ONE OWNER: DssHarness** — the agent verbs for a lane (`references/orchestration.md`),
`create-worktree` / `delete-worktree` / `list-worktree` for anything else (operator, 2026-09-24). A
location rule is only as good as the last person who remembered it, and this skill already records what
happens to a rule that lives only in a document.

⚠ **THE MAX_PATH BUDGET IS SPENT, NOT SLACK — AND THIS IS THE ONE THING TO CARRY FROM H.0a.**
Moving from a 10-char root into the repository root costs every build path the characters of
`<repo>/.worktrees/` — and an agent's worktree adds its orchestrator's directory. ✔MEASURED 2026-08-26
in a live lane worktree: the move more than halved the margin, and this repository's test names are what
dominate the build-relative suffix and keep growing. The tool holds the line by arithmetic: the root and the
name, `build/<longest variant>/`, then `worktrees.pathBudgetReserve` and `pathBudgetMargin`, every term read
from `.harness-config/config.json` and the longest name that still fits named in the refusal — read the
refusal, never a remembered figure; every character of an orchestrator's name is one its agents' cannot have
(orchestration.md).
⇒ **Keep names SHORT.** A descriptive name spends the margin that protects the next long test name.

⚠ **The ignore rules are what keep checkouts off every gate host**, and they are a REQUIREMENT, not
tidiness. `dssharness init` writes them in the CONTENTS shape — `/.worktrees/*` from `worktrees.root` and
the fixed `/.orchestrators/*`, each beside a tracked `.gitkeep` — and names either directory it finds as a
link, writing nothing through it; the carriage, `dssharness sync`, withholds both directories by NAME
(DOCUMENTED, `dssharness help layout`). ✔MEASURED 2026-09-29: a `sync --dry-run` from the main checkout, with
an agent's worktree open, listed no path below either directory — one host, WSL. A gate host holding a checkout runs somebody's
uncommitted `examples/` corpus and reports it as the cycle's.

✔**MEASURED, so the move is not a hope:** a real 2,792-file worktree at `.worktrees/probe` moved
**zero** of the 16 runnable registered guards — identical verdict and output volume before and
after — and `git status` reported **0 lines** for it.

⚠ **And read a build's exit code from the PROCESS, not from the tail of a pipeline.** A run once
reported `EXIT=0` over a failed build because `$LASTEXITCODE` after a PowerShell pipeline is the LAST
command's — `Select-String`'s — not `cmake`'s. An instrument that reports success over a failed build
is the same class as a red-on-disable whose mutant never compiled in.

- **Any experiment whose whole point is that bytes change** — byte-neutrality probes, red-on-disable
  mutants, "what does the corpus look like if…" — runs in a worktree, never in the shared tree: a lane's own
  agent worktree, or a plain probe worktree. What lands in the main tree lands through the fold, or as a
  verified patch the main loop decides on.
  ★ **A red-on-disable ARM changes no tree at all**: `dssharness check-mutations` applies the mutation in a
  WORKER COPY it keeps beside the tree (`<tree>.mutation-<key>w-<n>`, a whole copy with a whole build, synced
  by content before every sweep), so a sweep may be run on a lane's worktree and on the main checkout alike.
  A worker's path is twenty characters longer than its tree's; the workers go with their tree —
  `delete-agent` and `delete-worktree` remove those kept beside it, `clean` a leg's with its build directory.
  The hand-applied mutant, which does change a tree, remains only for the one class of mutant the installed
  tool refuses as an arm (mutation-arms.md).
- ⚠ **A revert is not a safe undo once a concurrent lane has edited the same file.** Copying a
  pristine copy back destroys the other lane's interleaved work. Reverse-apply the specific patch
  (`git apply -R`), or leave it and report.
- ★ **Do not start a revert war with a concurrent actor.** Report the state and wait — that lane's
  judgement here was exactly right, and the only thing it lacked was information the main loop had.
- ⇒ The main loop owes the other direction: when it applies a lane's patch on an operator decision
  that lane could not see, **tell the lane**. An unexplained tree mutation is otherwise indistinguishable
  from a defect, and a careful lane will spend real effort investigating it.

A worktree spawned for a cycle (a lane's agent, an independent audit, a probe) is
the **same repo on the same machine** as root. It must therefore run with the **same
permissions root already has** and **never re-prompt** for what root trusts. This is
non-negotiable: a worktree that stalls or spams permission prompts is an operational defect,
not the user's job to click through. Enforce it on both layers:

1. **Permission allowlist — blanket, not per-command.** The project
   `.claude/settings.local.json` `permissions.allow` list must carry the blanket tool grants
   **`Bash(*)` AND `PowerShell(*)`** (on Windows, cmake/ctest builds run through PowerShell —
   `Bash(*)` alone leaves every worktree *build* command prompting and being approved one at a
   time). If a tool starts prompting inside a worktree, add its `(*)` blanket entry to root's
   settings rather than approving individual commands; the worktree inherits it immediately. A lane
   also WRITES OUTSIDE its worktree — its work, plans and rows directories under the main checkout's
   `.orchestrators/` — so the grant must cover that path too.
2. **Sandbox — subagents disable it for trusted build/VCS commands.** Any permission prompt from a
   lane's subagent that is *not* an allowlist miss is the **sandbox** flagging a write outside the Windows
   workspace root (typically the subagent's WSL `/home`/`/tmp` build dirs). Spawn **every**
   lane subagent with a standing instruction to pass `dangerouslyDisableSandbox: true` on its
   build / compile / `git` / `wsl` commands (trusted repo operations). Bake that line into the
   brief verbatim — do not rely on the subagent inferring it.

**Corollary — prefer the root for fast, sequential, low-risk cycles.** A worktree buys
parallel isolation; it costs permission friction and (if spawned at a stale base) a slow
build. When a cycle is *sequential* and *config-shaped* (e.g. a shipped-descriptor cycle whose
re-probe compiles SQLite), run it in the **root**: it inherits root permissions automatically
and gets the current HEAD's compile-time wins (post the c97 resolver fix, the SQLite re-probe
is ~seconds, not ~15 minutes). Reserve worktrees for genuinely parallel or higher-risk *code*
changes — and a lane's agent is made at the current HEAD by `create-agent`, so it builds fast.
