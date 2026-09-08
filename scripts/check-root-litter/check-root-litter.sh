#!/usr/bin/env bash
# PURPOSE: refuse any untracked file sitting directly in a repository root -- the probe litter lanes leave behind when a bare filename lands in their cwd.
#
# ★★★ WHY A GUARD AND NOT A SENTENCE IN A BRIEF. The brief already said "scratch
# files go in .temp/<lane>-scratch/", and lanes still left litter at the root
# TWICE IN TWO CYCLES: a 0-byte file named `256` (from a `> 256` that reached a
# shell as a redirect), and then FIFTEEN reference-probe artifacts -- `pa.c`,
# `bw2.c`, `g.s`, four compiled binaries, six captured `.txt`s. This repository
# has already measured that *a rule living only in a document has no teeth at the
# moment of the decision*, and this is that measurement again.
#
# ★ THE MECHANISM IS NOT CARELESSNESS, WHICH IS WHY EXHORTATION DOES NOT FIX IT.
# A bare filename lands in the CURRENT WORKING DIRECTORY, and a lane's cwd is a
# repository root unless it changed it. `gcc -o pa_gcc pa.c` writes `pa_gcc`
# wherever the shell is standing, silently, with no warning and no error. The
# author is thinking about the probe, not about the cwd.
#
# ⚠ WHY "ANY UNTRACKED FILE AT DEPTH 1" IS THE RIGHT TEST, with no allowlist.
# Real work lands in `src/`, `tests/`, `examples/`, `scripts/`, `docs/`, `.plans/`
# -- never loose at the root. A genuinely NEW top-level file (a CONTRIBUTING.md,
# say) trips this exactly once, and the fix is `git add`, which is what its author
# was going to do anyway. ⇒ An allowlist would be an escape every lane could take,
# and this project has measured what that costs: a P56 guard whose escape every
# subject satisfied refused nothing at all, and three mutants came back green.
#
# ⚠ DIRECTORIES ARE NOT CHECKED, deliberately. `.worktrees/`, `build/`, `.temp/`
# and the session scratch are all ignored-or-legitimate directory trees, and
# `git status --porcelain` collapses an untracked directory to one entry anyway.
# The defect this guards is loose FILES; a stray directory has never happened and
# guarding it would trade a real signal for a speculative one.
#
# Exit codes: 0 clean - 1 litter found (named) - 2 cannot run (not a repo / no git).
set -uo pipefail

_here="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd -P)" \
  || { echo "check-root-litter: cannot resolve this script's own directory" >&2; exit 2; }

# ⚠ ANCHORED ON THIS FILE, NOT ON THE CALLER'S CWD -- the same rule
# `lane-worktree.sh` learned the hard way ([[D-SCRIPT-LANE-WORKTREE-REPO-ROOT-IS-CWD-KEYED]]):
# a guard that asks "what repository is my caller standing in?" answers about
# whichever tree somebody happened to cd into.
_root="$(cd "$_here/../.." && pwd -P)" || exit 2

command -v git >/dev/null 2>&1 || {
    echo "check-root-litter: CANNOT RUN -- git is not on PATH" >&2; exit 2; }
git -C "$_root" rev-parse --git-dir >/dev/null 2>&1 || {
    echo "check-root-litter: CANNOT RUN -- '$_root' is not a git repository" >&2; exit 2; }

# `--porcelain` prefixes untracked entries with `?? `. A path with no `/` is at
# depth 1. An untracked DIRECTORY is reported with a trailing `/`, so the same
# test excludes it.
#
# ⚠⚠ NOT `mapfile`, AND THE GUARD THAT CAUGHT ME IS THE POINT. The first spelling
# of this line was `mapfile -t _litter < <(…)`, and `shell_portability_guard`
# refused it: macOS ships bash **3.2.57**, `mapfile` does not exist there, and the
# script would have **CONTINUED WITH AN EMPTY ARRAY** — reporting "no litter" on a
# host where it can never see any. A guard against silent-clean failing silently
# clean, on a supported host, written by someone whose whole cycle was about that
# class. `bash -n` cannot see it; only that guard can.
# ⇒ A `while read` loop over a pipeline is bash-3.2 clean and needs no version gate.
_litter=()
while IFS= read -r _p; do
    [ -n "$_p" ] && _litter+=("$_p")
done < <(
    git -C "$_root" status --porcelain=v1 --untracked-files=normal 2>/dev/null \
    | sed -n 's/^?? //p' \
    | grep -vE '/' \
    | LC_ALL=C sort
)

if [ "${#_litter[@]}" -eq 0 ]; then
    echo "check-root-litter: OK -- no untracked files at the root of $_root"
    exit 0
fi

echo "check-root-litter: FAIL -- ${#_litter[@]} untracked file(s) directly in the repository root:" >&2
for f in "${_litter[@]}"; do echo "    $f" >&2; done
cat >&2 <<'WHY'

  Nothing a lane creates belongs in a repository root. A bare filename lands in
  your CURRENT WORKING DIRECTORY, and your cwd is a repository root unless you
  changed it -- so `gcc -o probe probe.c` writes both files here, silently.

  FIX: `cd` into your scratch directory before running probes, and write every
  output path explicitly. Then remove the files above BY NAME with `rm`.
  ⛔ NOT with `git clean` -- a concurrent workstream shares this tree, and that
     verb reaches far past the files you are looking at.

  If one of these is real work: `git add` it, which is what makes it not litter.
WHY
exit 1
