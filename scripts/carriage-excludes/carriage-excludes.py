#!/usr/bin/env python3
# PURPOSE: emit the transport exclude list for a gate carriage, derived from what git ignores rather than re-typed once per carriage.
"""carriage-excludes.py -- ONE owner for "what must never reach a gate host".

★★★ WHY THIS EXISTS, and it is a measured leak rather than a tidiness rule.
Operator ruling 2026-08-25: *"keep macos and vps linux arm64 updated with our repo
files, and free of stale files/worktrees."* A gate host holds the repo and NOTHING
else -- a leg that runs against a host holding anything else is not testing the tree
it reports on.

Until 2026-08-26 that rule was enforced by FOUR HAND-MAINTAINED ENUMERATIONS, one per
carriage (`wsl-leg.sh`, `remote-leg.sh`, `ssh-macos.sh`, `ssh-macos.ps1`), each a
guess at the complement of the repository.
D-SCRIPT-CARRIAGE-EXCLUDES-ARE-A-HAND-LIST-AND-MISS-NESTED-IGNORED-TREES

✔MEASURED 2026-08-26 on the hosts themselves, and on the lists themselves:
  * the **arm64 VPS gate host held `.kilo/` -- 3,671 files, 61 MB, 3,667 of them
    `node_modules`**, out of 25,198 files total. Every list spells the exclude
    `node_modules` ANCHORED at the top level, and `.kilo/node_modules` is not at the
    top level. Git had ignored that tree the whole time; the enumerations simply
    could not see it.
  * exercising the macOS `.sh` list against this same tree ships **9,284 members
    where the derivation ships 3,583** -- 3,938 of `.kilo/node_modules`, 1,431 of
    `test-scratch/`, 268 of `.temp/`, 49 `__pycache__` entries, and
    `.claude/settings.local.json`, none of which that list names at all.
  * ⚠ AND FIVE FILES OF `.secrets/`, which is connection data and key PATHS for real
    machines in a repository slated to go public. Of the four lists only
    `remote-leg.sh` withheld it. That is the cost of a list per carriage stated as
    plainly as it can be: the same omission is invisible in three places at once.
⚠ THE macOS HOST ITSELF HELD ONLY 4 `.kilo` FILES (28 KB), NOT THE TREE -- its pushes
run through the `.ps1`/bsdtar path, which excludes differently. An earlier draft of
this comment reported the VPS's 3,671 as the Mac's. The leak is real and latent in
that carriage either way, but a host is not interchangeable with the list that feeds
it, and the number belongs to whichever one was actually measured.

★★★ THE FIX IS TO DERIVE, NOT TO ADD A LINE. Adding `.kilo` to four lists repairs
this instance and leaves the mechanism: the next ignored tree that appears at a depth
nobody typed leaks exactly the same way. The set of things a gate host must not hold
is already written down, once, in a place that is authoritative and that git will
enumerate on demand -- `.gitignore` plus the checkout's own excludes. So the four
lists become one QUESTION: `git status --porcelain --ignored -z`.
⚠ The derivation describes THIS tree, deliberately. A carriage ships the working tree
including its dirty files, so "what git ignores HERE" is exactly the right question --
`.kilo/` is covered on this machine through `.git/info/exclude`, which is local and
uncommitted, and that is correct rather than a gap.

✔MEASURED before the switch, because a derivation that loses a case is worse than the
enumeration it replaced:
  * **0 tracked files** live under any of the 30 ignored directories, so excluding an
    ignored directory can never drop a file the repository owns. ⚠ The first probe of
    this said otherwise -- `grep -F "build/"` matched
    `examples/c/project_module_standalone_build/` -- and the answer only came out right
    once the probe did a real prefix match. A substring is not a path.
  * every name the four hand lists carried is covered, EXCEPT `.venv/`,
    `node_modules/` and `.kilo/`, which were added to `.gitignore` in the same commit.
    ⚠ `git check-ignore build-lane` answers NOT-IGNORED for a directory that does not
    exist, because `build-*/` is a directory-only pattern -- so the probe must spell
    the trailing slash (`git check-ignore -- 'build-lane/'`), and a floor built on the
    unslashed form would have reported four false gaps.

★★ THE FLOOR IS A REFUSAL, NOT A SILENT TOP-UP. If `.gitignore` stops covering
something that must never travel, this script REFUSES (exit 3) and names the one-line
repair. Quietly appending the missing name instead would leave `.gitignore` wrong
while every leg kept passing, and would restore -- inside the fix -- the second copy
this script exists to delete. A carriage that refuses to push costs one commit; a
carriage that ships a build tree costs an unattributable gate result.

★ The list is handed to rsync/tar through `--exclude-from=FILE` and never as shell
words. That is not a style preference: this project has been burned four times in one
day by quoting, and a file has no quoting.

Exit codes: 0 OK · 2 git could not answer (the tree is not a repository, or git
failed -- NEVER an empty list, which would mean "ship everything") · 3 the floor is
breached · 4 usage error.
"""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

# ★ AT IMPORT, COVERING BOTH STREAMS, and inside `main()` would NOT be enough --
# argparse prints usage and `--help` before main's body ever runs. Without this a
# non-cp1252 character in a path (or in the census line) is lost or mangled the
# moment this script's output goes through a pipe on Windows, which is the only way
# a carriage ever calls it. `check-guard-output-encoding` re-measures the property
# every run rather than trusting this comment.
for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError, OSError):
        pass

# ── the floor ───────────────────────────────────────────────────────────────
# Directories that must never reach a gate host, whatever `.gitignore` currently
# says. Each is PROBED, not emitted: this list does not add excludes, it asserts
# that the real owner (`.gitignore` + the checkout's excludes) still covers them.
# ⚠ Spell every entry with its trailing slash -- see the header.
MUST_NEVER_TRAVEL = (
    "build/",            # the host builds its own
    "build-lane/",       # representative of `build-*/`
    "target/",
    ".dss-deps/",
    "scratchpad/",
    "Testing/",
    "test-scratch/",
    ".claude/worktrees/",  # a full copy of the repo per live agent
    # ★ The SANCTIONED home for lane worktrees since the 2026-08-26 ruling that
    #   moved them inside the repo root. Same cost as the line above and then
    #   some, because lanes are routinely FOUR at a time: without this the push
    #   ships four full checkouts, and a gate host holding one runs somebody's
    #   uncommitted `examples/` corpus and reports it as the cycle's. The
    #   operator stated the requirement as an absolute -- "worktrees MUST be
    #   ignored by ALL host copies to run legs" -- so it is pinned here rather
    #   than merely written in `.gitignore`: this floor RE-ASKS git and refuses
    #   the carriage if that rule is ever edited away.
    # ⚠ The trailing slash is load-bearing, per the module docstring: the
    #   directory is ABSENT most of the time, and `git check-ignore .worktrees`
    #   answers NOT-IGNORED (rc=1) for a nonexistent path while `.worktrees/`
    #   answers rc=0. ✔MEASURED 2026-08-26, both spellings, absent directory.
    ".worktrees/",
    ".secrets/",
    ".temp/",
    ".venv/",
    "node_modules/",
    ".kilo/",
    "__pycache__/",
)


# ★★★ A LANE WORKTREE'S `.git` IS A FILE, AND A WINDOWS-CREATED ONE NAMES A
# WINDOWS-ABSOLUTE GITDIR THAT A POSIX git CANNOT FOLLOW.
# ✔MEASURED 2026-08-31 (P47), from inside WSL, which is the ONLY namespace the WSL and
# arm64 carriages ever run this tool from:
#     $ cat .worktrees/sq/.git
#     gitdir: C:/Source/DailySoftware/dss-code-prime/.git/worktrees/sq
#     $ git -C /mnt/c/.../.worktrees/sq rev-parse --show-toplevel
#     fatal: not a git repository: /mnt/c/.../.worktrees/sq/C:/Source/.../worktrees/sq
# -- `C:/…` is not absolute to a POSIX git, so it is JOINED to the worktree path. This
# tool then refused with `is not a git repository`, and the carriage that called it
# refused to rsync at all, so NO LANE COULD CARRY ITS OWN WORKTREE TO A GATE HOST
# while `.worktrees/` is the SANCTIONED home for lane worktrees (ruling 2026-08-26).
# The shell half of the same defect lives in `scripts/leg-tree/leg-tree.sh`
# (`leg_tree_driver_identity`), and this is its Python twin -- two implementations
# because the two callers are in two languages, kept in step BY REVIEW and pinned by
# the same measurement, exactly as the harness drivers' mirrored regions are.
#   D-SCRIPT-CARRIAGES-CANNOT-IDENTIFY-A-CROSS-NAMESPACE-LANE-WORKTREE
# ⚠ RESOLVED ONCE AND CACHED, not per call: `_git` runs many times per invocation and
# re-deriving would put a `wslpath` fork in front of every one of them.
_GIT_DIR_FOR: dict[str, list[str]] = {}


def _git_prefix(repo: Path) -> list[str]:
    """The argv prefix that makes git answer about `repo` in THIS namespace."""
    key = str(repo)
    cached = _GIT_DIR_FOR.get(key)
    if cached is not None:
        return cached

    prefix = ["-C", key]
    probe = subprocess.run(("git", *prefix, "rev-parse", "--git-dir"),
                           capture_output=True, text=True,
                           encoding="utf-8", errors="replace")
    if probe.returncode != 0:
        dot_git = repo / ".git"
        if dot_git.is_file():
            raw = ""
            for line in dot_git.read_text(encoding="utf-8",
                                          errors="replace").splitlines():
                if line.startswith("gitdir:"):
                    raw = line[len("gitdir:"):].strip()
                    break
            # Same three cases, in the same order, as the shell twin: already a
            # directory here; FOREIGN-ABSOLUTE (`X:/…`), translated by wslpath and by
            # nothing else; otherwise relative to the worktree, as git itself allows.
            # ⚠ The order matters -- testing "not POSIX-absolute" first turns
            # `C:/…` into `<worktree>/C:/…`, which is the very mangling this undoes.
            if raw:
                gd = Path(raw)
                if not gd.is_dir():
                    if len(raw) > 1 and raw[1] == ":" and raw[0].isalpha():
                        conv = subprocess.run(("wslpath", "-u", raw),
                                              capture_output=True, text=True,
                                              encoding="utf-8", errors="replace")
                        gd = Path(conv.stdout.strip()) if conv.returncode == 0 else gd
                    elif not raw.startswith("/"):
                        gd = repo / raw
                if gd.is_dir():
                    prefix = ["--git-dir", str(gd), "--work-tree", key]

    _GIT_DIR_FOR[key] = prefix
    return prefix


def _git(repo: Path, *argv: str) -> subprocess.CompletedProcess:
    # ★ `cwd=repo` IS LOAD-BEARING UNDER THE `--git-dir` FORM AND INERT UNDER `-C`.
    # `check-ignore -- <name>` resolves its pathspec against the PROCESS's working
    # directory, and `-C` used to supply that implicitly; `--work-tree` does not move
    # the process at all. Without this the FLOOR check would ask about `.worktrees/`
    # relative to whatever directory the carriage happened to start in -- and a floor
    # that answers about the wrong directory fails toward "not ignored", which is the
    # loud direction, but for the wrong reason and with an unactionable message.
    return subprocess.run(
        ("git", *_git_prefix(repo), *argv), cwd=str(repo),
        capture_output=True, text=True, encoding="utf-8", errors="replace",
    )


def _die(code: int, *lines: str) -> None:
    for line in lines:
        print(f"carriage-excludes: {line}", file=sys.stderr)
    sys.exit(code)


def ignored_paths(repo: Path) -> list[str]:
    """Repo-relative paths git considers ignored, directories collapsed.

    ⚠ NEVER returns an empty list to mean "git said nothing". A failure here is a
    refusal, because the empty list is indistinguishable from "exclude nothing",
    which is the exact accident this script exists to prevent.
    """
    proc = _git(repo, "status", "--porcelain", "--ignored", "-z")
    if proc.returncode != 0:
        _die(2, f"git status --ignored failed in {repo} (rc={proc.returncode})",
             (proc.stderr or "").strip() or "(no stderr)",
             "Refusing to emit an exclude list -- an empty one ships everything.")
    # `-z` means no quoting and no escaping, so a path with a space, a quote or a
    # newline arrives intact. That is the whole reason for it.
    return sorted(
        rec[3:] for rec in proc.stdout.split("\0")
        if rec.startswith("!! ") and len(rec) > 3
    )


def check_floor(repo: Path) -> list[str]:
    """Names in MUST_NEVER_TRAVEL that git would NOT ignore. Empty is the pass."""
    breached = []
    for name in MUST_NEVER_TRAVEL:
        if _git(repo, "check-ignore", "-q", "--", name).returncode != 0:
            breached.append(name)
    return breached


def render(paths: list[str], fmt: str) -> list[str]:
    """Format repo-relative paths into one transport's exclude dialect.

    * `rsync` -- anchored at the transfer root with a leading `/`; a trailing `/`
      keeps git's own directory-only meaning.
    * `tar`   -- members are named `./x`, so the pattern is `./x` with no trailing
      slash. Both dialects in play accept this: the `.sh` carriage gets GNU tar
      1.35 from Git Bash, the `.ps1` gets bsdtar 3.8.4 from System32. ✔MEASURED
      2026-08-26 -- and `--anchored` was deliberately NOT used, because bsdtar has
      no such flag and the `./` prefix already anchors against a `./`-rooted name.
    * `plain` -- as git spells it, for a reader or a diff.
    """
    if fmt == "plain":
        return list(paths)
    if fmt == "rsync":
        return ["/" + p for p in paths]
    return ["./" + p.rstrip("/") for p in paths]


def main() -> int:
    ap = argparse.ArgumentParser(
        prog="carriage-excludes.py", add_help=True,
        description="Emit a gate carriage's transport exclude list, derived from git.")
    ap.add_argument("--format", required=True, choices=("rsync", "tar", "plain"))
    ap.add_argument("--repo", default=".", help="repository to ask (default: cwd)")
    ap.add_argument("--also", action="append", default=[], metavar="PATH",
                    help="a POLICY withhold this carriage adds on top of git's answer "
                         "-- e.g. `.git` on a carriage that syncs no history. Repeatable.")
    ap.add_argument("--out", metavar="FILE",
                    help="write here instead of stdout (what --exclude-from wants)")
    args = ap.parse_args()

    repo = Path(args.repo).resolve()
    top = _git(repo, "rev-parse", "--show-toplevel")
    if top.returncode != 0:
        _die(2, f"{repo} is not a git repository -- cannot derive an exclude list.")
    repo = Path(top.stdout.strip())

    breached = check_floor(repo)
    if breached:
        _die(3,
             "the exclude FLOOR is breached -- git no longer ignores:",
             *(f"    {b}" for b in breached),
             "A gate host would receive these. Add each to .gitignore and re-run;",
             "do NOT paste them into a carriage, which is the duplication this replaces.")

    paths = ignored_paths(repo)
    policy = [a.strip("/") + "/" if (repo / a).is_dir() else a for a in args.also]
    lines = render(paths, args.format) + render(policy, args.format)

    if args.format == "tar":
        odd = [p for p in paths if any(c in p for c in "*?[")]
        for p in odd:
            print(f"carriage-excludes: WARNING -- '{p}' carries a glob metacharacter "
                  "that tar will read as a pattern.", file=sys.stderr)

    body = "".join(line + "\n" for line in lines)
    if args.out:
        Path(args.out).write_text(body, encoding="utf-8", newline="\n")
    else:
        sys.stdout.write(body)

    # ★ THE CENSUS IS THE WITNESS. Every leg log carries this line, so a reader can
    # see that the derivation ran and found a plausible number -- a leg that pushed
    # with `0 from git` is visibly wrong on the transcript rather than merely slow.
    print(f"carriage-excludes: {len(lines)} pattern(s) for {args.format} "
          f"({len(paths)} git-ignored, {len(policy)} policy) from {repo}",
          file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
