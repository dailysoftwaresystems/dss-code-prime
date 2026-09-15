#!/usr/bin/env bash
# PURPOSE: read the PR's CI verdict per leg from job METADATA, which outlives the logs, and separate a real test failure from a budget overrun.
#
# ★★★ WHY THIS EXISTS, AND IT IS A MEASURED COST, NOT A CONVENIENCE.
# [[D-CI-TWO-RELEASE-LEGS-HAVE-BEEN-RED-FOR-TEN-DAYS-WHILE-EVERY-LOCAL-LEG-WAS-GREEN]]
# ✔MEASURED 2026-09-14: PR #57's Pipeline had been red on EVERY run that executed
# the matrix — six of them, 2026-09-09 to 2026-09-10 — with two legs
# (`windows-msvc-release`, `macos-clang-release`) failing at the `Test` step while
# the local four-leg gate was 2179/2178/2148/2148 GREEN at the very same commit.
# No step of `/dss-cycle` read CI at all, so nothing in the cycle could see it; the
# operator had to point at it.
#
# ★★ AND BY THEN THE EVIDENCE HAD EXPIRED. `gh run view --log-failed` answers
# **HTTP 410** on every one of those runs, `gh run rerun --failed` is refused, and
# the `Upload test logs on failure` artefacts — 1,018,340 B and 1,001,107 B, which
# name the failing tests — report `expired: true` with
# `expires_at = 2026-09-13T16:26:42Z`: **three days after creation, against the
# `retention-days: 7` the workflow asks for.** The effective retention is a
# repository setting that silently overrides the workflow's request. ⇒ a cycle that
# learns about CI from its LOGS has a three-day window and no way to know it closed.
#
# ★★★ THE JOB METADATA DOES NOT EXPIRE WITH THE LOGS, AND IT ANSWERS THE QUESTION
# THE LOGS WERE BEING ASKED. `/actions/runs/<id>/jobs` returns, per step,
# `conclusion` + `started_at` + `completed_at`. That is enough to separate the two
# hypotheses a failed `Test` step leaves open, which have OPPOSITE remedies:
#   · a ctest `--stop-time` budget overrun cannot take LESS than `ctest_budget_min`;
#   · a `timeout-minutes` kill cannot take less than `ctest_step_timeout_min`;
#   · anything materially shorter is a REAL TEST FAILURE, and a real failure is
#     FIXED — never hidden under a larger cap.
# ✔That distinction was made with this instrument and it REFUTED the budget
# hypothesis outright: the worst red Test step measured **829 s against a 3000 s
# budget (27.6%)**, and macOS's worst **417 s (13.9%)**. Neither leg had ever been
# within 70% of its cap. Meanwhile the leg genuinely AT its cap was a GREEN one —
# `linux-clang-asan` at 4559–6525 s of 6600 s, 69%–98.9%
# (D-CI-ASAN-LEG-WALL-CLOCK-GROWS-WITH-THE-CORPUS), which this tool prints too.
#
# ⚠ A RUN WHOSE `run-tests` WAS **SKIPPED** IS NOT EVIDENCE ABOUT TESTS, AND
# READING IT AS SUCH IS THE MISTAKE THIS TOOL EXISTS TO STOP REPEATING.
# `gh run list` reported 27 consecutive `failure` runs on the branch; MEASURED per
# job, every run before 2026-09-09 failed at **`label-check`** (the `Run Pipes`
# label was absent) with the whole matrix `skipped`. "Red for twenty commits" and
# "red on six runs that tested anything" are different facts and only the second
# one is about the tree. This tool therefore reports the JOB, never the run
# rollup, and says out loud when the matrix did not run.
#
# ⛔ READ-ONLY, BY CONSTRUCTION. It issues `gh api` GETs and nothing else. It does
# NOT label, re-run, push, or otherwise touch the PR: re-triggering CI is the
# operator's call. A leg it reports red is a HARD STOP for the cycle to read, not
# something to retry until it passes.
#
# Usage:
#   scripts/check-ci-legs/check-ci-legs.sh                  # the newest Pipeline run on the current branch
#   scripts/check-ci-legs/check-ci-legs.sh --limit 6        # the newest N runs on the current branch
#   scripts/check-ci-legs/check-ci-legs.sh --branch <name>  # another branch
#   scripts/check-ci-legs/check-ci-legs.sh --run <id> ...   # named runs, in the order given
#
# ⓘ THE BUDGET HAS TWO SOURCES, AND THE ORDER MATTERS.
# First choice is the JOB NAME, which is authoritative for the run that produced it —
# GitHub spells the matrix values into it. ⚠ But GitHub TRUNCATES a long one: the
# `linux-clang-asan` job's name arrives as `run-tests (linux-clang-asan,
# ubuntu-latest, clang-19, clang++-19, Debug, address,undefined, 45, 1...`, with both
# budget fields cut off — and that is the leg whose budget matters most, at **69%–98.9%
# of its 6600 s cap** across the six matrix runs measured on 2026-09-14. An instrument
# whose warning cannot fire on the one leg approaching its cap is answering the
# adjacent question, so the fallback reads `ctest_budget_min` for that leg name out of
# `.github/workflows/pipeline-pr.yml` in the WORKING TREE, and says `(workflow)` beside
# the number so a reader knows it came from the file rather than from the run. If
# neither source answers, `? s cap` is printed and the leg is NOT classified: a
# discriminator that invents its denominator is worse than one that says it has none.
#
# ⚠ NO ctest ENTRY READS CI, AND THE REASON IS STATED RATHER THAN LEFT AS AN OMISSION.
# Reading CI needs the NETWORK and an authenticated `gh`; a ctest guard doing that would red
# on any host without credentials — a guard that fails for a property of the machine rather
# than of the tree, which this repository refuses by name. ✔What proves the READ: EXECUTION,
# both twins, against six real Pipeline runs on 2026-09-14, printing identical rows and
# returning rc 1 on the HEAD run; the refusal arms (`gh` absent, detached HEAD, unreadable
# run, empty job list) were each reached during that work.
# ⓘ WHAT ctest DOES pin is the LOCAL half, hermetically: `test-check-ci-legs.py` (entry
# `check_ci_legs_git_environment_guard`) drives COPIES of both twins against a stub gh, and
# proves which branch they ask about, which repository gh's own git sees, and the GH_REPO
# refusal -- the same arms on each twin, so that part of the pair is held by a detector.
#
# Exit: 0 every leg green · 1 at least one leg red · 2 the instrument could not run
# (no `gh`, not authenticated, no such run). ⚠ 2 is NOT "green": an instrument that
# could not look must never read as a pass.
set -uo pipefail

# ★ RUN FROM THE REPO ROOT, ALWAYS — resolved from this script's own location, the same
# shape `remote-leg.sh` uses. Two things depend on it and BOTH fail quietly otherwise:
# `gh api repos/:owner/:repo/...` resolves the repository from the working directory,
# and the workflow-file budget fallback below is a repo-relative path. A tool invoked
# from a subdirectory would lose the fallback without saying so.
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
cd -- "${SCRIPT_DIR}/../.." || exit 2

# ★★★ NEITHER git NOR gh IS ASKED IN THE CALLER'S GIT ENVIRONMENT. The `cd` above moves the
# working directory and nothing else: an exported GIT_DIR or GIT_WORK_TREE still decides which
# repository git answers for -- and gh finds `:owner/:repo` BY RUNNING git.
# ✔MEASURED 2026-09-15 (P66 lane ge; gh a recording stub, and the real gh 2.89.0 for one
# read-only GET): with another repository's GIT_DIR, or GIT_DIR + GIT_WORK_TREE, exported, this
# tool read THAT repository's branch and asked `gh run list --branch <its branch>` where it
# should have refused a detached HEAD; given `--branch`, gh's own git still saw THAT
# repository's origin, and the real `gh api repos/:owner/:repo` answered HTTP 404 there against
# `dailysoftwaresystems/dss-code-prime` without the variables. GIT_INDEX_FILE alone steered
# neither. The `.ps1` twin behaved identically.
# ⇒ every git AND gh call goes through `leg_tree_unsteered`, the sh owner of that removal. Pinned
# hermetically -- a stub gh on PATH, no network -- by `test-check-ci-legs.py` beside this file.
# The empty argument is load-bearing: `.` forwards THIS script's positionals otherwise.
# shellcheck source=../leg-tree/leg-tree.sh
. "${SCRIPT_DIR}/../leg-tree/leg-tree.sh" "" || {
    printf 'check-ci-legs: FATAL -- cannot load scripts/leg-tree/leg-tree.sh; CI was NOT read\n' >&2
    exit 2
}

WORKFLOW="Pipeline"
# The matrix builder's own source, read ONLY when GitHub truncated a job name out of
# its budget fields. Relative to the repo root, which is where this script is run from.
WORKFLOW_FILE=".github/workflows/pipeline-pr.yml"
BRANCH=""
LIMIT=1
RUNS=()

while [ "$#" -gt 0 ]; do
    case "$1" in
        --workflow) WORKFLOW="${2:?--workflow needs a value}"; shift 2 ;;
        --branch)   BRANCH="${2:?--branch needs a value}"; shift 2 ;;
        --limit)    LIMIT="${2:?--limit needs a value}"; shift 2 ;;
        --run)      shift; while [ "$#" -gt 0 ] && [ "${1#-}" = "$1" ]; do RUNS+=("$1"); shift; done ;;
        -h|--help)  sed -n '/^# Usage:/,/^# (including/p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        # An unknown flag is a REFUSAL: silently ignoring one is how a tool answers
        # a different question than the caller asked and still looks authoritative.
        *) printf 'check-ci-legs: unknown argument %s (try --help)\n' "$1" >&2; exit 2 ;;
    esac
done

# ★★ A CALLER'S GH_REPO IS REFUSED -- NOT HONOURED, AND NOT STRIPPED. It is gh's own explicit
# override, not git steering (it is not among the names `leg_tree_unsteered` removes), and it
# makes gh answer for whatever repository it names. This tool's subject is fixed: the CI of the
# tree it lives in. Honouring the override would report another repository's legs under this
# tree's name -- the default branch is still read from THIS tree -- and whether it names this
# tree cannot be checked without re-deriving gh's own remote resolution. Stripping it would
# silently discard an instruction the caller gave. So a non-empty GH_REPO stops the read and
# names it, exit 2: the instrument did not run. (gh itself treats an empty GH_REPO as unset.)
if [ -n "${GH_REPO:-}" ]; then
    printf 'check-ci-legs: FATAL -- GH_REPO is set (%s), so gh would answer for THAT repository; this tool reads only the CI of the tree it lives in. Unset GH_REPO for this call. CI was NOT read (this is not a pass)\n' "$GH_REPO" >&2
    exit 2
fi

command -v gh >/dev/null 2>&1 || {
    printf 'check-ci-legs: FATAL -- `gh` is not on PATH; CI was NOT read (this is not a pass)\n' >&2
    exit 2
}

if [ "${#RUNS[@]}" -eq 0 ]; then
    [ -n "$BRANCH" ] || BRANCH="$(leg_tree_git_unsteered rev-parse --abbrev-ref HEAD 2>/dev/null)"
    [ -n "$BRANCH" ] && [ "$BRANCH" != "HEAD" ] || {
        printf 'check-ci-legs: FATAL -- no branch to ask about (detached HEAD?); pass --branch or --run\n' >&2
        exit 2
    }
    # ⚠ A `read` LOOP, NOT `mapfile`: `mapfile` is bash 4+, and `shell_portability_guard`
    # refuses an undeclared bash-4 dependency in this tree. ✔It caught this exact line
    # on the run that introduced it — `[undeclared-bash-4-dependency]`.
    while IFS= read -r _rg_id; do
        [ -n "$_rg_id" ] && RUNS+=("$_rg_id")
    done < <(leg_tree_unsteered gh run list --workflow "$WORKFLOW" --branch "$BRANCH" \
        --limit "$LIMIT" --json databaseId --jq '.[].databaseId' 2>/dev/null)
    [ "${#RUNS[@]}" -gt 0 ] || {
        printf 'check-ci-legs: FATAL -- no `%s` run found for branch %s; CI was NOT read\n' \
            "$WORKFLOW" "$BRANCH" >&2
        exit 2
    }
fi

rc=0
for run in "${RUNS[@]}"; do
    meta=$(leg_tree_unsteered gh api "repos/:owner/:repo/actions/runs/$run" \
        --jq '[.head_sha[0:8], .head_branch, .created_at, .conclusion] | @tsv' 2>/dev/null) || {
        printf 'check-ci-legs: FATAL -- run %s could not be read\n' "$run" >&2
        exit 2
    }
    printf '\n=== run %s  %s\n' "$run" "$meta"

    # ⚠ `gh api --jq`, NEVER A STANDALONE `jq`. ✔MEASURED on this workstation's Git
    # Bash: there is no `jq` on PATH, and the first draft of this file piped to one
    # — every run printed `jq: command not found` and the tool still EXITED 0. A
    # missing dependency that reads as "no legs failed" is the exact silent pass
    # this repository refuses everywhere else. `gh` carries its own jq, so the
    # dependency set is `gh` alone and the `command -v gh` check above covers it.
    #
    # ⚠ THE SKIPPED-MATRIX CASE IS REPORTED, NEVER INFERRED AWAY. A run whose
    # `run-tests` is `skipped` says nothing about the tree, and a caller that
    # counted it as a red would be counting the label, not the code.
    legs=$(leg_tree_unsteered gh api "repos/:owner/:repo/actions/runs/$run/jobs?per_page=100" --jq '
        [.jobs[] | select(.name|test("run-tests \\("))] as $m |
        if ($m|length) == 0 then
            "NO-MATRIX\t" + ([.jobs[] | .name + "=" + (.conclusion // "?")] | join(" "))
        else
            ($m[] | . as $j |
            (($j.steps // []) | map(select(.name=="Build"))  | first) as $b |
            (($j.steps // []) | map(select(.name=="Test"))   | first) as $t |
            [ ($j.name | capture("run-tests \\((?<leg>[^,]+)").leg),
              $j.conclusion,
              "build=" + (($b.conclusion) // "-"),
              "test="  + (($t.conclusion)  // "-"),
              "build_s=" + (if $b and $b.started_at and $b.completed_at
                            then ((($b.completed_at|fromdate) - ($b.started_at|fromdate))|tostring) else "-" end),
              "test_s="  + (if $t and $t.started_at and $t.completed_at
                            then ((($t.completed_at|fromdate) - ($t.started_at|fromdate))|tostring) else "-" end),
              "budget_s=" + ((($j.name | capture(", (?<b>[0-9]+), [0-9]+\\)$") | .b) // "?")
                             | if . == "?" then "?" else ((tonumber * 60)|tostring) end)
            ] | @tsv)
        end' 2>/dev/null) || {
        printf 'check-ci-legs: FATAL -- jobs for run %s could not be read\n' "$run" >&2
        exit 2
    }
    # An EMPTY answer is FATAL, not a green run: it means the query returned
    # nothing at all, which is indistinguishable from "every leg passed" and must
    # never be read as one.
    [ -n "$legs" ] || {
        printf 'check-ci-legs: FATAL -- run %s returned NO job rows; nothing was verified\n' "$run" >&2
        exit 2
    }

    if printf '%s' "$legs" | grep -q '^NO-MATRIX'; then
        printf '  ⚠ THE MATRIX DID NOT RUN in this run — `run-tests` is absent/skipped, so it says\n'
        printf '    NOTHING about the tree. Jobs present: %s\n' "${legs#NO-MATRIX	}"
        printf '    (on this repo that is normally the `Run Pipes` label being absent.)\n'
        continue
    fi

    printf '%s\n' "$legs" | while IFS=$'\t' read -r leg concl b t bs ts budget; do
        [ -n "$leg" ] || continue
        note=""
        used="${ts#test_s=}"; cap="${budget#budget_s=}"
        capsrc=""
        # The workflow-file fallback, for a job name GitHub truncated. Read once per
        # leg, by leg NAME, from the matrix builder's own JSON fragment.
        if [ "$cap" = "?" ] && [ -f "$WORKFLOW_FILE" ]; then
            _wf=$(sed -n "s/.*\"name\":\"${leg}\".*\"ctest_budget_min\":\([0-9]\+\).*/\1/p" \
                  "$WORKFLOW_FILE" | head -1)
            if [ -n "$_wf" ]; then cap=$(( _wf * 60 )); capsrc=" (workflow)"; fi
        fi
        # THE DISCRIMINATOR, and it is the whole reason this prints seconds.
        if [ "$t" = "test=failure" ] && [ "$used" != "-" ] && [ "$cap" != "?" ]; then
            if [ "$used" -ge "$cap" ] 2>/dev/null; then
                note="  <- AT OR OVER THE ctest BUDGET: read this as a possible overrun, not a test failure"
            else
                note="  <- REAL TEST FAILURE: ${used}s of a ${cap}s budget, nowhere near the cap. FIX IT; do not raise the cap"
            fi
        elif [ "$t" = "test=success" ] && [ "$used" != "-" ] && [ "$cap" != "?" ]; then
            if [ "$used" -gt $(( cap * 8 / 10 )) ] 2>/dev/null; then
                note="  <- GREEN BUT ABOVE 80% OF ITS BUDGET; re-derive it before it reds (D-CI-ASAN-LEG-WALL-CLOCK-GROWS-WITH-THE-CORPUS)"
            fi
        fi
        printf '  %-26s %-8s %-16s %-14s %-12s %s%s%s\n' "$leg" "$concl" "$b" "$t" "$ts" "$cap s cap" "$capsrc" "$note"
    done

    printf '%s' "$legs" | grep -q 'failure' && rc=1
done

if [ "$rc" -ne 0 ]; then
    printf '\ncheck-ci-legs: RED. At least one CI leg failed.\n'
    printf '  ⛔ This is a HARD STOP for the cycle, not a retry. Reproduce the failing leg\n'
    printf '     LOCALLY in its own configuration — the logs behind these verdicts expire in\n'
    printf '     three days and a red leg nobody reproduces becomes a red leg nobody can.\n'
fi
exit "$rc"
