# Orchestration — every lane is a DssHarness agent

The ONE statement of how a cycle's lanes are made, seeded, folded, given rows, kept and removed. Other
references state their own rule in a line and point here. The tool's own text is `dssharness help
orchestrators` and each verb's `--help`; read the verb's help before writing its line into a brief
(lane-discipline.md, rule 6).

**Words.** A LANE is a DssHarness agent of the cycle's orchestrator, with its own worktree, work, plans and
rows directories. The Agent-tool session doing a lane's work is its SUBAGENT. The SESSION running this skill
is the orchestrator. A *reasoning agent*, in the four-agent cap, is any live subagent, lane or not.

## Contents
- The rule (MANDATORY) — which subagents are lanes; a remnant lane
- The layout
- Names
- The lifecycle, in commands — aiming a lane's subagent at its worktree
- Seeding and refreshing
- Folding, and the rows it applies
- The rows directory — the fold checks every lane row itself
- Deleting an agent: evidence, transcripts, host copies
- The cycle's end: deleting the orchestrator
- Orientation and the four-lane cap
- Exit codes

## The rule (MANDATORY)

> *"let's migrate our skills to mandatory use dssharness instead of lane-fold.py"* … *"Once it's working,
> remove lane-fold.py."* — operator, 2026-09-29.

Since 2026-09-29 a lane is an AGENT of the cycle's orchestrator, and DssHarness does the whole lifecycle:
`create-agent` makes the worktree and seeds it, `fold-agent` folds the lane's work and applies its rows,
`delete-agent` keeps its evidence and transcripts and removes its worktree and every host copy. Never make a
lane with `create-worktree`, never copy a lane's files into the main tree by hand, never apply a lane's rows by
hand (the one exception is below, where the fold itself says to), never remove a lane's worktree any other
way. The orchestrator is the only one that creates, seeds, refreshes, folds and deletes agents; a lane never
runs these verbs.

- **Which subagents are lanes.** A LANE is any subagent that changes the repository's files — source, tests,
  plans, the skill — and it always gets an agent. A subagent that only reads (a planner, the design audit, a
  reviewer, the self-audit) and writes at most its findings file into the orchestrator's work directory is
  not a lane: it gets no agent, and counts against the four-agent cap all the same.
- **A remnant lane** (no-follow-ups.md) is a new agent of the SAME orchestrator, briefed by it. A lane briefed
  to run `/dss-cycle` runs it AS A LANE: its priority is its brief's rows; it implements, reviews and gates in
  its worktree and files its rows in its rows directory; it never creates an orchestrator, never runs the
  agent verbs, and leaves the orchestration — step 0's `list-orchestrator`, step 5's agents, step 9's handoff,
  step 11's commit — to the orchestrator. Its brief opens *"you are lane `<o>/<a>`"*.

A plain `dssharness create-worktree` / `delete-worktree` stays for what is NOT a lane — a probe or a
byte-changing measurement (worktrees.md) — and its name must not be the orchestrator's.

## The layout

Everything is in the MAIN checkout, git-ignored (`dssharness init` writes the rules; each directory keeps a
tracked `.gitkeep`), and `sync` withholds both directories by name:

| where | what |
|---|---|
| `.orchestrators/<o>/agent.json` | the orchestrator's record: model, parallel limit, session |
| `.orchestrators/<o>/logs/<name>.jsonl` | one JSON line per run that reached the orchestrator or one of its agents and acted or refused, with its outcome and exit code — the event times a ledger needs, READ, never recalled; a dry run writes none, its refusals included, and neither does a refusal made before an agent is reached, such as a name refused up front (✔MEASURED 2026-09-29, in this checkout and in a throwaway repository) |
| `.orchestrators/<o>/logs/<a>/` | the lane's Claude transcripts, kept by `delete-agent` |
| `.orchestrators/<o>/plans/<o>/` | the orchestrator's own ledger, handoffs and pause notes |
| `.orchestrators/<o>/plans/<a>/` | the lane's handoff, pause and stop notes |
| `.orchestrators/<o>/work/<o>/` | the orchestrator's own work directory — its undo copies, gate logs, staged rows, review findings; the tool makes none, the session does |
| `.orchestrators/<o>/work/<a>/` | the lane's work directory: its brief, scratch, logs, mutant transcripts |
| `.orchestrators/<o>/agents/<a>/` | `agent.json` (state), `seed.json` (what it shares with the main tree), `rows/`, `applied-rows.json`, `evidence/` |
| `.worktrees/<o>/<a>` | the lane's worktree, its build tree inside; `list-worktree` / `delete-worktree` name it `o/a`, its host copies `o--a` |

`create-agent` makes the lane's work, plans and rows directories (✔MEASURED 2026-09-30 in a throwaway
repository; an agent the orchestration release made has no `rows/`, and its lane makes one). The work, plans and rows directories are
OUTSIDE the lane's worktree: every brief gives them as ABSOLUTE paths, and the lane writes there, never into
its worktree's own `.temp`. An agent is never named after its orchestrator: its work directory would be the
orchestrator's.

## Names

Lowercase letters and digits joined by single hyphens, each name — the orchestrator's and each agent's — capped
at `worktrees.maxNameLength` in config.json, a longer one refused up front (✔MEASURED 2026-09-29 in this
checkout). The orchestrator is named for the cycle id (`p69`); an agent for its lane, and an agent name is
NEVER used twice — a deleted agent's name is refused (✔MEASURED 2026-09-29 in a throwaway repository), so a
remnant lane, or one a review sends back after it was deleted, is a new agent with a new name. Both names spend ONE path budget: `.worktrees/<o>/<a>` plus the deepest build path must stay under
Windows' 260 characters, so every character of the orchestrator's name is one the agent's cannot have — keep
it to two or three. `create-agent` refuses up front and names the longest agent name that still fits: read
the refusal.

## The lifecycle, in commands

```bash
# once per cycle: the session's own model id and session id (its transcript's file name), never literals here
dssharness create-orchestrator <o> --model <model id> --parallel 4 --session <session id>

# per lane, BEFORE its subagent starts: the worktree and its seed in one act
dssharness create-agent <o> <a> --model <the subagent's model id> [--empty]
#   ... spawn the subagent (below), then record its id; run again, create-agent records only that:
dssharness create-agent <o> <a> --model <the subagent's model id> --session <subagent id>

dssharness seed-agent <o> <a> [--empty] [--force]               # seed again; --force overwrites the lane's own changes
dssharness refresh-agent <o> <a> [<path>...] [--apply]          # the main tree's later changes, to a live lane
dssharness fold-agent <o> <a> [--apply] [--new <ID>]... [--accept-lost <ID>:<cell>]... [--settled <path>]...
#   its work AND its rows; the worktree is kept
dssharness delete-agent <o> <a> [--apply] [--new <ID>]... [--accept-lost <ID>:<cell>]... [--settled <path>]...
#   after its LAST review
dssharness delete-agent <o> <a> --discard-uncommitted [--apply] # abandon: nothing folded, no rows applied
dssharness list-orchestrator [<o>] [--json]
dssharness delete-orchestrator <o> --delete-evidence            # at the cycle's end; it has NO dry run
```

⚠ `fold-agent`, `refresh-agent` and `delete-agent` are DRY RUNS until `--apply`. `create-orchestrator`,
`create-agent`, `seed-agent` and `delete-orchestrator` act at once — `delete-orchestrator` deletes the whole
audit trail the moment it runs, so run `list-orchestrator <o>` first and check the five conditions below. Run
every verb from the main checkout (`delete-agent` refuses to run from inside the worktree it removes).

**Aiming a lane's subagent at its worktree** — nothing does it but the brief:
- The brief names `<repo>/.worktrees/<o>/<a>` as the root of EVERY path the lane edits, absolute — a
  subagent's shell goes back to the main checkout between calls — and has it run every `dssharness` command
  with `-C <repo>/.worktrees/<o>/<a>`: without it, `build` and `test` build and test the MAIN tree, and an edit
  made there bypasses the fold.
- Never spawn a lane with the Agent tool's own `isolation: "worktree"`: that checkout is not the agent's, and
  no fold reads it.
- A lane never commits in its worktree: the fold refuses a lane that did.
- A lane a review sends back is RESUMED — `SendMessage` to its subagent's id — never replaced by a fresh
  subagent: an agent records ONE session, recording another replaces it, and `delete-agent` keeps only the
  recorded session's transcripts.

## Seeding and refreshing

`create-agent` hands the lane the main tree's UNCOMMITTED state — each changed file copied, each deletion
made, each recorded with its digest in `seed.json` — so a fold leaves what it was handed out of the lane's
work. That is why a lane set is created only after the previous set is committed (lane-sets-and-folding.md):
the lane then starts from the tree the gate saw. `--empty` hands it nothing (it starts at HEAD). `seed-agent`
seeds a live lane again; it is refused over changes of the lane's own, and `--force` OVERWRITES them
(✔MEASURED 2026-09-29 in a throwaway repository).

`refresh-agent` hands a LIVE lane the main tree's changes under the paths given — the anchor registries'
directory, `.plans/`, when none is — and records them as handed, so its fold leaves them out. It refuses,
copying nothing, where the lane changed one of them (✔MEASURED 2026-09-29 in a throwaway repository) or
deleted one (DOCUMENTED, `refresh-agent --help`): name the two registry
files when a live lane holds a plan file of its own. Refresh a lane when it needs what a sibling's fold
landed — a row it cites, a file it reads.
- ★ **A refresh writes a guard input UNDER a live lane** — its own `.plans/**`, which its next gate reads.
  Refresh a lane only between its gates, and tell it what was refreshed (lane-discipline.md, rules 2 and 8).
  A fold writes only the MAIN tree's `.plans/**`, which no live lane's gate reads.

## Folding, and the rows it applies

★★★★ "Complete" means FOLDED (lane-sets-and-folding.md): on a lane's report, `fold-agent <o> <a>` — a dry run,
which names everything that would refuse the fold, its rows' refusals included, and shows every cell whose
stored text a row would lose as a word diff — read it; then `--apply` with `--new <ID>` for each id the report
names as new and `--accept-lost <ID>:<cell>` for each lost cell read and accepted (the dry run prints that
command, on its `to write them:` line); then re-derive the balance and run `check-anchor-registry`.

- Files first. The fold writes the lane's own changes, removes what it deleted, and leaves out what it was
  handed and did not change. Everything that refuses it is named before anything is written: the main tree
  changed one of its paths since — two lanes on one file, or a commit — a commit made inside the lane, a link, a
  directory, a path leading out of the tree or one the tool cannot look at, an anchor registry the lane changed
  as a file (its rows go in through its rows directory only), and any row the checks below refuse. ⚠ A file of either tree that changes after it was weighed stops the fold THERE, exit 21, with part of
  the lane's files written and none of its rows: run the same `fold-agent --apply` again once that file has
  settled — a 21 is never a fold. A path reconciled by hand is left out with `--settled <path>`, which is not a
  `--force`.
- Then the rows, all or nothing: a new id written as `write-anchor` writes one, an existing row changed in the
  cells that differ — a changed status moves it (✔MEASURED 2026-09-29 in a throwaway repository: a close moved
  the row to the done registry) — one already as declared left alone; a write that fails puts both registries
  back byte for byte.
- A row an earlier fold applied, still declared as it was then, is left alone, so a change the registries took
  since stands; a lane sent back that CHANGES a row's cells has the changed cells applied anew; and a changed
  declaration over a row the registries changed since refuses the WHOLE fold (✔MEASURED in a throwaway
  repository: all three by dry runs on 2026-09-29, a changed cell applied anew by `--apply` on 2026-09-30). The
  same `--new` list serves every later fold of the lane: one naming a row an earlier fold applied is let stand
  (✔MEASURED 2026-09-30, same repository). So the orchestrator never edits a row a live lane owns. Where it
  happened anyway, the fold names the row and says to set it by hand: read both, `set-anchor` the row as it
  should be, have the lane (resumed) rewrite its cell files from the stored row (`dssharness read-anchor <ID>
  --json`) — a row already as declared is recorded, not written again — and fold again.
- What a fold wrote is recorded as shared, so when a review sends the lane back its next change of such a
  path — putting it back as it was included — folds like any other.
- A review finding on a lane's work is fixed by sending the lane back (resumed, above) and folding again.
  Where the orchestrator fixes a lane's path in the main tree instead, it `refresh-agent`s that path to the
  lane before any send-back — otherwise the next fold is refused over it.

## The rows directory

A lane files each registry row as a directory of cell files, VERBATIM, one file per cell, UTF-8:

```
<repo>/.orchestrators/<o>/agents/<a>/rows/<ANCHOR ID>/status.txt
                                                     trigger.txt
                                                     closing.txt
                                                     cross-refs.txt
                                                     priority.txt    (required for a NEW row)
```

The status decides the registry, as `write-anchor` does (closed → the done registry). Any other file in a row's
directory, a missing cell or a link is refused — ✔MEASURED 2026-09-29 in this checkout: a dry run refused a
`crossrefs.txt`, naming it and the missing `cross-refs.txt`. The brief gives the absolute path; the report names
the ids, the NEW ones named as new — they are the fold's `--new` list. The lane never applies a row, and the
orchestrator never retypes one.

★★★ **THE FOLD CHECKS EVERY LANE ROW ITSELF.** `fold-agent` and `delete-agent` refuse (1) an id no registry
holds that `--new` did not name — a mistyped id would mint a second row; the refusal names the rows that begin
the same way; (2) an id or path the door would store cut — the door stores a line break as a space; (3) an id a
row newly cites that no row holds, the batch's own rows counting; (4) a cell whose stored text the row would
lose, until `--accept-lost <ID>:<cell>` names it. ✔MEASURED 2026-09-30 in a throwaway repository: (1), (2) — a
cut id and a cut path — and (3) in one dry run (eight problems, exit 13, nothing written), the corrected rows
applied; and (4) in three runs: the dry run showed the lost cell's word diff and exited 0, and `--apply` refused
it (13) until `--accept-lost` named it. The door itself refuses (2) and (3) for every write — `write-anchor` a cut value (exit 10),
`set-anchor` a citation no row holds (exit 13), measured the same day. ⚠ The orchestration release made none of these
checks (✔MEASURED 2026-09-29, same method: it stored a cut id, a cut path and a citation of no row, and made a
mistyped id of an existing row a new closed row); it was reported and fixed. So `--new` names only the ids the
report names as new, and `--accept-lost` only a lost cell whose word diff was read — never one accepted unread.
`anchor-rows` stays for the orchestrator's OWN cell files (anchors-and-deferrals.md), whose batch it rehearses
in a throwaway repository with a balance receipt; a lane's rows never go through it.

## Deleting an agent: evidence, transcripts, host copies

★★★ After the lane's LAST review, and only then (the operator, 2026-09-28: *"only removes copies after last
review is fine: while modifications are needed the files can't be removed"*): `delete-agent <o> <a>`, read the
dry run — anything left to fold, or a row still to apply, means the gate did not see the lane's final state:
fold it, re-gate — then `--apply`. It folds what is left and applies the rows, held to the same four checks
with the same `--new` and `--accept-lost`, proves nothing is left to fold, copies the recorded session's transcripts into `logs/<a>/` (one not found is said; one found
and not kept stops it before anything is closed), keeps what the evidence roots hold — every root `worktrees.evidenceRoots`
names before the fold or after it — in `agents/<a>/evidence/<run>/`, each copy proved (✔MEASURED 2026-09-29 in a
throwaway repository: a file under each evidence root, kept byte-identical), records the agent closed, deletes
the kept originals itself (one it cannot delete is named, left, and stops the removal), and only then removes
the worktree and its copies on hosts through
`delete-worktree` — never forced, its evidence check kept, so a file written late stops the removal instead of
being lost (DOCUMENTED, `help orchestrators`; no probe wrote a file late). It says deleted only once the
directory, git's record and every recorded host copy are gone (✔MEASURED 2026-09-29 in this checkout: the
copies went on WSL, macOS and the arm64 VPS).

- Transcripts are kept only for a recorded session: ✔MEASURED 2026-09-29 in this checkout, `--session <subagent id>` (the id the
  Agent tool returns) kept both of that subagent's transcript files, each read back. Record it as soon as the
  subagent is spawned.
- A closed agent is never folded, seeded or refreshed again. Exit 21 means the delete stopped part way: run the
  same `delete-agent` again once what it names is dealt with — it compares the worktree with what closing
  recorded, never with the main tree; a file changed or new since is work left for you, a file gone since is
  the debris of the stopped removal. A directory with no `.git` of its own is never forced; the tool names the
  `delete-worktree --force` that removes it, and the orchestrator runs it only after looking at what the
  directory holds — `--force` loses whatever it held.
- `--discard-uncommitted` abandons a lane: nothing folded, no rows applied, its evidence and transcripts
  still kept.

## The cycle's end: deleting the orchestrator

> *"At cycle's end: All green, reviewed, review fixed, final review passed, commited and pushed in main tree to
> branch's PR"* … *"I forgot to mention the gate too, must have passed in all legs"* — operator, 2026-09-29.

★★★ `dssharness delete-orchestrator <o> --delete-evidence` runs at the cycle's END and not before — when ALL of
these hold:
1. every agent of it is deleted;
2. everything is green;
3. it was reviewed, the review's findings fixed, and the final review passed;
4. the gate passed on ALL legs;
5. the work is committed and pushed, in the main tree, to the branch's PR.

Until then the orchestrator IS the cycle's audit trail — its logs, the kept evidence and the transcripts are
what another session reads to take the cycle over. It has NO dry run, and it deletes the ledger in
`plans/<o>/` too, so whatever of it the next session needs is already in `.plans/_handoff.md`, committed (step
9). Without `--delete-evidence` it refuses while anything was kept (✔MEASURED 2026-09-29, in this checkout
and in a throwaway repository).

## Orientation and the four-lane cap

- Step 0 runs `dssharness list-orchestrator`: an open agent is a lane in flight, and its pause or stop note is
  in `plans/<a>/`.
- ≤4 reasoning agents live at once (delegation.md) stays the rule; `--parallel 4` makes the tool refuse a fifth
  LANE, counting every agent from `create-agent` until `delete-agent` — a folded lane waiting for its last
  review still holds a slot. Delete a finished set's agents before creating the next set's.

## Exit codes

The codes and what to do about each: dss-harness.md, *Exit codes* (`dssharness help exit-codes`). For these
verbs: 13 — refused, nothing written — except at `delete-agent`'s last stage, where 13 (a run holds a host
copy), 15 (its host cannot be reached) and 20 (removing it failed there) mean the worktree is gone and a
recorded copy stays: run `delete-agent` again when it can be (DOCUMENTED, `help worktrees`); 10 — a flag that
cannot be combined, such as `--new` beside `--discard-uncommitted`; 21 — stopped part way (a fold, a hand-over,
a deletion): run the same command again once what it names is dealt with, and never read it as a pass.
