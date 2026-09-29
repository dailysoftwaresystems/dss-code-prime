# Leg hosts — a gate host holds the repo and nothing else

What a remote gate host may hold, and the standard every leg follows on it: prepare, sync, run,
restore. The transport is `dssharness sync` (`dss-harness.md`).

## Contents
- A gate host holds the repo and nothing else (operator ruling 2026-08-25) — the three rules; what
  this narrows and does not repeal
- Every leg host keeps a clone, and the leg cleans up after itself (operator ruling 2026-08-26) —
  PREPARE · SYNC · RUN · RESTORE; `-fd`, never `-fdx`; cleanup belongs to the mode that made the mess;
  `git worktree prune`; the tilde does not expand

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
- **A push is a SYNC, never an accumulation.** `dssharness sync` deletes what the source no
  longer has and verifies the copy afterwards — one transport, every host. A transport that only
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
(a deliberate reset stays opt-in for exactly that reason). What is authorised is removing
what the repo does not have: stale files and worktrees.
⇒ ★★★ **THE OPERATOR NAMED IT ON 2026-08-26. Read the next section — restoring a leg clone is
now REQUIRED where this paragraph once forbade it, and the transport is the only thing that may
do it — `dssharness sync` today.**

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
2. **SYNC** — the existing rsync/tar carriage ⏳ SCRIPT-ERA (superseded 2026-09-24: the carriage is `dssharness sync`; see dss-harness.md), `.git` withheld on **all three** now. The host's
   own history is the authority; the sync supplies only the working tree.
3. **RUN** — `dssharness test` (it builds first). The `repo-guard` label runs on the two Windows legs;
   every remote leg excludes it through `remoteExcludes` in `.harness-config/config.json`.
4. **RESTORE** — `git reset --hard`, `git clean -fd`, `git worktree prune`. On **every** exit
   path, `die` included.

★ **THE CARRIAGE IS `dssharness sync`.** The repository's own shell owner of these steps,
`leg-tree` (its prepare and restore verbs inlined into each remote leg's payload), was retired on
2026-09-21, when no leg called it any more; its tree half lives on in
`.harness-config/runner/actions/owning-tree/owning-tree.py`. **Never hand-roll these git commands in
a leg** — that is the four-hand-written-exclude-lists mistake with a destructive verb attached.

⚠ **`-fd`, NEVER `-fdx`.** Ignored paths (`build/`, the ccache) are the leg's own working state;
deleting them buys tidiness and costs every leg a cold rebuild.

⚠ **CLEANUP BELONGS TO THE MODE THAT MADE THE MESS, not to every mode that runs afterwards.**
✔Caught while wiring this: a sync-only mode exists to LEAVE a host staged for a manual probe, and
a test-only mode runs over whatever is already there. A restore on exit would have deleted the
staging as the command returned, and a prepare would have made test-only test HEAD while reporting
on the staged tree. ⇒ the three shapes must stay distinct. Today they are `dssharness sync`,
`dssharness test --use-staged` and `dssharness test --no-build`.

⚠ **WHY THIS REPLACED THE OLD ARRANGEMENT.** ✔MEASURED 2026-08-26: the macOS clone sat on branch
`…-3` at `8cb9afbd` (**three commits back**) with a **2,696-path index under a 2,759-path working
tree**, while two carriages withheld `.git` and one shipped it. ★ **The cost is not the disk — it
is that guards ask git questions.** `check-line-endings` reads `git ls-files --eol`;
`check-shell-portability` (retired since, with the shell programs) had ALREADY been rewritten in 2026-08-22 to stop asking `git ls-files`
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
expanded only where it appears unquoted in the source text. The retired `leg-tree` normalised the
path once, where both of its verbs shared it. ★ The dangerous half was not the failed `cd`: `restore` returned 0 when
the directory is missing, so an unexpanded path would have left every host dirty forever while
every leg reported success.
