#!/usr/bin/env python3
# PURPOSE: regenerate the PR landing-log hash anchors in the plans from git log.
"""Regenerate `### PR landing log` hash anchors from git log.

The recurring "missing commit hash in landing log" bug class (PR8 review
caught four such drifts in the v2 sub-plan) motivated this script. Each
landed PR gets its commit hashes inlined into the plan's landing-log
table via paired HTML-comment markers:

    <!-- LANDING-LOG-HASHES: SH2 -->`ab0800e`<!-- /LANDING-LOG-HASHES -->

The script regenerates the content between the markers from `git log`.
Body prose outside the markers is hand-written and never touched. Rows
without an opening marker are opt-out — left untouched.

Usage:
    py .harness-config/runner/actions/refresh_landing_log/refresh_landing_log.py --check   # CI gate; exits non-zero on drift
    py .harness-config/runner/actions/refresh_landing_log/refresh_landing_log.py --write   # apply rewrite in place

Configuration in `.harness-config/runner/actions/refresh_landing_log/landing-log-config.json` lists which plan files
have landing logs and what commit-subject pattern matches PR landings.
The pattern's first capture group is the PR identifier (e.g. "SH2");
optional second/third groups identify review followups vs round-N
followups.
"""

from __future__ import annotations

import argparse
import difflib
import json
import re
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path

# ★ BOTH STREAMS, AT IMPORT -- the property `guard_output_encoding_guard` ratchets. This file
# was DEBT in that guard's inventory: it reconfigured only stdout, only inside `main()`. When
# `REPO_ROOT` began loading `owning-tree` at import (2026-09-18) it became protected by that
# import's block -- TRANSITIVELY, i.e. by accident, until the day the import moved. Made its
# own property here, and the inventory entry is deleted in the same change.
for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError, OSError):  # pragma: no cover - an odd stream
        pass

# ⓘ `REPO_ROOT` is defined below `_owning_tree()`, as that owner's answer: it was
# `Path(__file__).resolve().parent.parent.parent`, which named `.harness-config/runner`
# the day this file moved out of `scripts/` (2026-09-18) -- and CI runs `--check` on every
# push, so the count would have been the first thing to red there.
# The config is an ASSET OF THIS SCRIPT, so it is addressed from the script
# directory rather than through the repo root: the two are now independent
# facts, and only one of them can be wrong when the script moves again.
CONFIG_PATH = Path(__file__).resolve().parent / "landing-log-config.json"

# Opening marker placed by hand; closing marker added/maintained by the
# script. The space inside the comment delimiters MUST match exactly —
# Markdown renderers swallow these as HTML comments.
OPEN_MARKER_RE = re.compile(
    r"<!--\s*LANDING-LOG-HASHES:\s*(?P<pr>[A-Za-z0-9]+)\s*-->"
)
CLOSE_MARKER = "<!-- /LANDING-LOG-HASHES -->"
CLOSE_MARKER_RE = re.compile(r"<!--\s*/LANDING-LOG-HASHES\s*-->")

# Restrict marker hunting to the `### PR landing log` section so marker
# references that appear inside body prose (e.g. the SH1 surface section
# explaining HOW the marker works) aren't mistaken for live anchors.
LANDING_LOG_HEADING_RE = re.compile(r"^###\s+PR landing log\s*$", re.MULTILINE)
NEXT_HEADING_RE = re.compile(r"^#{1,3}\s+\S", re.MULTILINE)


def _landing_log_section(text: str) -> tuple[int, int] | None:
    """Return (start, end) byte offsets of the landing-log section body.

    Start is the byte right after the heading line; end is the byte
    immediately before the next `###`-or-shallower heading, or len(text)
    when the section runs to EOF. Returns None when no landing-log
    heading is present (a plan file without one is silently skipped).
    """
    heading = LANDING_LOG_HEADING_RE.search(text)
    if not heading:
        return None
    body_start = heading.end()
    next_heading = NEXT_HEADING_RE.search(text, heading.end() + 1)
    body_end = next_heading.start() if next_heading else len(text)
    return body_start, body_end


@dataclass
class Commit:
    sha: str
    kind: str  # "initial" | "review" | "round-N" | "sub:<full-id>"


@dataclass
class PlanSpec:
    path: Path
    subject_pattern: re.Pattern[str]


def load_config(config_path: Path) -> list[PlanSpec]:
    raw = json.loads(config_path.read_text(encoding="utf-8"))
    out: list[PlanSpec] = []
    for entry in raw["plans"]:
        out.append(
            PlanSpec(
                path=REPO_ROOT / entry["path"],
                subject_pattern=re.compile(entry["subjectPattern"]),
            )
        )
    return out


def _owning_tree():
    """`.harness-config/runner/actions/owning-tree/owning-tree.py` -- the owner of asking git WITHOUT the caller's git environment.

    Loaded by path from this file's sibling directory (a hyphen is not a module name). It FAILS
    LOUD when absent: the rule it owns must not be spelled a second time here.
    """
    import importlib.util
    path = Path(__file__).resolve().parent.parent / "owning-tree" / "owning-tree.py"
    if not path.is_file():
        sys.exit(f"refresh_landing_log: cannot find {path} -- this tool asks git through it "
                 f"and nowhere else")
    spec = importlib.util.spec_from_file_location("dss_owning_tree", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def _repo_root() -> Path:
    """The tree THIS FILE lives in, by the owner's walk -- a missing tree exits, loudly."""
    ot = _owning_tree()
    try:
        return Path(ot.resolve(__file__))
    except ot.Refusal as exc:
        sys.exit(f"refresh_landing_log: {exc}")


REPO_ROOT = _repo_root()


def git_log_subjects() -> list[tuple[str, str]]:
    """Return (full_sha, subject) pairs for every commit reachable from HEAD.

    Reverse-chronological is git's default; we reverse to chronological so
    callers see "initial" before "review followup".

    ★★ ASKED WITHOUT THE CALLER'S GIT ENVIRONMENT. `cwd=` moves git's working directory and
    nothing else. ✔MEASURED 2026-09-15 (P66 lane ge): on a tree whose history holds 111
    commits, this function returned ONE -- another repository's -- under that repository's
    GIT_DIR, and again under GIT_DIR + GIT_WORK_TREE (GIT_INDEX_FILE alone changed nothing:
    `git log` reads no index). `--check` could not show it on that tree only because no commit
    there matches a configured subject pattern, so both histories rendered every marker empty;
    against a history that does match, the verdict flips (self-test arm 3).
    ⇒ `owning-tree.run_git`. A failing `git log` still raises, as `check_output` did.
    """
    p = _owning_tree().run_git(
        ["log", "--pretty=format:%H %s"],
        cwd=REPO_ROOT,
        capture_output=True,
        text=True,
        encoding="utf-8",
    )
    if p.returncode != 0:
        raise subprocess.CalledProcessError(p.returncode, p.args, p.stdout, p.stderr)
    out = p.stdout
    rows = [line.split(" ", 1) for line in out.splitlines() if line]
    rows.reverse()
    return [(sha, subj) for sha, subj in rows if sha and subj]


def classify_commit(match: re.Match[str]) -> str:
    """Map a subject-pattern match's optional groups to a Commit.kind.

    Groups recognised:
      1: PR id (e.g. "SH4")  — the landing-log row key.
      2: optional "review" marker.
      3: optional round-N number.

    The match's full leading text (`match.group(0)`) is inspected
    separately for a sub-deliverable letter immediately after the
    captured PR id (e.g. "SH4a", "SH4c"). Sub-deliverables render as
    `<sha>` (SH4a), keeping each sub-PR identifiable in the landing log
    without inventing a parallel row per sub.
    """
    groups = match.groups()
    has_review_marker = len(groups) >= 2 and groups[1]
    round_n = groups[2] if len(groups) >= 3 else None
    pr_id = groups[0]
    # Detect a single letter suffix between the captured PR id and the
    # next regex-consumed text. `m.group(0)` covers the whole match;
    # `m.end(1)` is the byte index right after the captured PR id.
    suffix_start = match.end(1)
    rest = match.group(0)[suffix_start - match.start():]
    suffix = ""
    if rest and rest[0].isalpha():
        suffix = rest[0]
    if round_n:
        return f"round-{round_n}"
    if has_review_marker:
        return "review"
    if suffix:
        return f"sub:{pr_id}{suffix}"
    return "initial"


def collect_commits_by_pr(
    spec: PlanSpec, all_subjects: list[tuple[str, str]]
) -> dict[str, list[Commit]]:
    by_pr: dict[str, list[Commit]] = {}
    for sha, subject in all_subjects:
        m = spec.subject_pattern.match(subject)
        if not m:
            continue
        pr_id = m.group(1)
        commit = Commit(sha=sha[:7], kind=classify_commit(m))
        by_pr.setdefault(pr_id, []).append(commit)
    return by_pr


def _kind_label(kind: str) -> str:
    """Render a Commit.kind for inclusion in a hash block annotation."""
    if kind == "review":
        return "review followup"
    if kind.startswith("sub:"):
        return kind[len("sub:"):]
    return kind


def render_hash_block(commits: list[Commit]) -> str:
    """Inline-render commits as `sha1` + `sha2` (kind) + `sha3` (kind).

    Two flavours of layout:
      - All commits classified as `initial` (the normal case for a
        single-deliverable PR): render the first bare, no others — the
        invariant "1 commit per PR" makes annotation redundant.
      - Any non-`initial` kind present (review followups, round-N
        followups, OR sub-deliverable letters SH4a/SH4b/...): annotate
        every commit beyond the first with its kind. Sub-deliverable
        kinds carry their full PR id (e.g. `SH4a`) so reviewers can map
        each hash back to the sub-PR without consulting git.

    Returns the empty string for an empty list.
    """
    if not commits:
        return ""
    if all(c.kind == "initial" for c in commits):
        # The single-commit case; multi-commit "all initial" is degenerate
        # but possible if a config bug groups unrelated commits into one
        # PR — render only the first to surface the misconfiguration.
        return f"`{commits[0].sha}`"
    # Annotate every commit (including the first) when we have a mix:
    # this keeps sub-deliverable PRs readable (SH4a / SH4b / SH4c each
    # carries its own label rather than the first being implicit-initial).
    if any(c.kind.startswith("sub:") for c in commits):
        parts = [f"`{c.sha}` ({_kind_label(c.kind)})" for c in commits]
        return " + ".join(parts)
    # Single deliverable with review followups: classic shape — first
    # bare, followups annotated.
    parts: list[str] = []
    for i, c in enumerate(commits):
        if i == 0:
            parts.append(f"`{c.sha}`")
            continue
        parts.append(f"`{c.sha}` ({_kind_label(c.kind)})")
    return " + ".join(parts)


def rewrite_text(original: str, commits_by_pr: dict[str, list[Commit]]) -> str:
    """Produce the new file contents.

    For each opening marker inside the landing-log section, locate the
    matching closing marker (or create one immediately after) and
    replace the content between with the rendered hash block. Rows
    without an opening marker are untouched. Files without a
    `### PR landing log` heading are returned unchanged.
    """
    bounds = _landing_log_section(original)
    if bounds is None:
        return original
    section_start, section_end = bounds

    out = []
    cursor = 0
    for m in OPEN_MARKER_RE.finditer(original, section_start, section_end):
        # Emit everything up to and including the opening marker.
        out.append(original[cursor : m.end()])
        cursor = m.end()
        pr_id = m.group("pr")
        block = render_hash_block(commits_by_pr.get(pr_id, []))

        # Look for the closing marker on the SAME LINE — keeps the
        # paired-marker scope inside one table row and prevents a
        # missing close on row N from being matched against row N+1.
        line_end = original.find("\n", cursor)
        search_end = line_end if line_end != -1 else len(original)
        close_match = CLOSE_MARKER_RE.search(original, cursor, search_end)
        if close_match:
            out.append(block)
            out.append(original[close_match.start() : close_match.end()])
            cursor = close_match.end()
        else:
            # First run on a hand-marked row: insert a close marker.
            out.append(block)
            out.append(CLOSE_MARKER)
    out.append(original[cursor:])
    return "".join(out)


def process_plan(
    spec: PlanSpec, all_subjects: list[tuple[str, str]]
) -> tuple[str, str]:
    original = spec.path.read_text(encoding="utf-8")
    commits_by_pr = collect_commits_by_pr(spec, all_subjects)
    new = rewrite_text(original, commits_by_pr)
    return original, new


# ── the self-test: this tool reads the history of the tree it lives in ──────────────────
_FX_PATTERN = r"^Fixture landing (FX[0-9]+)[a-z]?(?:\s+(review)(?:\s+round-([0-9]+))?)?:"
_FX_PLAN = (".plans/fx-landing-plan.md",
            "# fixture plan\n\n### PR landing log\n\n| PR | hashes |\n|---|---|\n"
            "| FX1 | <!-- LANDING-LOG-HASHES: FX1 --><!-- /LANDING-LOG-HASHES --> |\n")
SELF_TEST_ARMS = 6


def self_test() -> int:
    """Red-on-disable for the git environment `git_log_subjects` runs in, against a history that MATCHES.

    ★ WHY A FIXTURE AND NOT THIS TREE. ✔MEASURED 2026-09-15 (P66 lane ge): no commit reachable from
    this repository's HEAD matches any configured subject pattern, so `--check` renders every marker
    empty from ANY history and could not see a steered `git log` at all. The fixture history holds one
    matching commit and a marker carrying its hash. THIS FILE is copied into it, with the owner it asks
    git through, and the COPY's `--check` runs in a child under each environment -- so a
    `git_log_subjects` reverted to a bare git call reddens arms 3 and 4 by name.
      (1) CONTROL                  no steering                    -> --check rc 0
      (2) the verdict CAN flip     the marker holds another hash  -> --check rc 1, so the rc 0 below means something
      (3) GIT_DIR                  another repository             -> --check rc 0 (negative: bare git log lacks FX1)
      (4) GIT_DIR + GIT_WORK_TREE  another repository             -> --check rc 0 (negative proven the same way)
      (5) GIT_INDEX_FILE           another repository's, ABSOLUTE -> --check rc 0, and bare git log is unmoved by it
      (6) the fixture box is removed, git's read-only objects included
    """
    import shutil
    import tempfile

    ot = _owning_tree()
    base = ot.git_environment()
    ran: list[str] = []
    failed: list[str] = []

    def arm(ok: bool, label: str, detail: str = "") -> None:
        ran.append(label)
        if not ok:
            failed.append(label)
        print("  %-4s %s%s" % ("ok" if ok else "FAIL", label, "" if ok else "   [%s]" % detail))

    def git(cwd: Path, *args: str) -> str:
        p = ot.run_git(["-C", str(cwd), "-c", "user.email=landing-log@example.invalid",
                        "-c", "user.name=landing-log", "-c", "commit.gpgsign=false"] + list(args),
                       capture_output=True, text=True, encoding="utf-8", errors="replace")
        if p.returncode != 0:
            raise RuntimeError("git %s failed: %s" % (" ".join(args), p.stderr.strip()))
        return p.stdout.strip()

    box = Path(tempfile.mkdtemp(prefix="landing-log-selftest-")).resolve()
    try:
        own, other = box / "own", box / "other"
        actions = own / ".harness-config" / "runner" / "actions"
        tool = actions / "refresh_landing_log" / "refresh_landing_log.py"
        tool.parent.mkdir(parents=True)
        shutil.copyfile(Path(__file__).resolve(), tool)
        (tool.parent / "landing-log-config.json").write_text(
            json.dumps({"plans": [{"path": _FX_PLAN[0], "subjectPattern": _FX_PATTERN}]}), encoding="utf-8")
        owner = actions / "owning-tree" / "owning-tree.py"
        owner.parent.mkdir(parents=True)
        shutil.copyfile(Path(__file__).resolve().parent.parent / "owning-tree" / "owning-tree.py", owner)
        plan = own / _FX_PLAN[0]
        plan.parent.mkdir(parents=True)
        plan.write_text(_FX_PLAN[1], encoding="utf-8", newline="\n")
        git(own, "init", "-q")
        git(own, "add", "-A")
        git(own, "commit", "-q", "--no-verify", "-m", "Fixture landing FX1: the landed change")
        sha7 = git(own, "rev-parse", "HEAD")[:7]
        marked = _FX_PLAN[1].replace("FX1 --><!--", "FX1 -->`%s`<!--" % sha7)
        plan.write_text(marked, encoding="utf-8", newline="\n")
        git(own, "add", "-A")
        git(own, "commit", "-q", "--no-verify", "-m", "fixture: record the landing hash")
        other.mkdir()
        (other / "other.txt").write_text("another repository\n", encoding="utf-8")
        git(other, "init", "-q")
        git(other, "add", "-A")
        git(other, "commit", "-q", "--no-verify", "-m", "an unrelated commit")

        def check(extra: dict) -> tuple[int, str]:
            p = subprocess.run([sys.executable, str(tool), "--check"], cwd=str(box),
                               env=dict(base, GIT_CEILING_DIRECTORIES=str(box), **extra),
                               capture_output=True, text=True, encoding="utf-8", errors="replace",
                               timeout=180)
            return p.returncode, ((p.stdout or "") + (p.stderr or "")).strip()

        rc, said = check({})
        arm(rc == 0, "(1) CONTROL: --check is clean where the marker holds the hash its own history renders",
            "rc=%d %s" % (rc, said[-200:]))

        plan.write_text(marked.replace("`%s`" % sha7, "`0000000`"), encoding="utf-8", newline="\n")
        rc, said = check({})
        plan.write_text(marked, encoding="utf-8", newline="\n")
        arm(rc == 1 and "out of date" in said,
            "(2) the verdict CAN flip: a marker that differs from the rendered history is drift (rc 1)",
            "rc=%d %s" % (rc, said[-200:]))

        other_git = other / ".git"
        steers = (("GIT_DIR", {"GIT_DIR": str(other_git)}, True),
                  ("GIT_DIR + GIT_WORK_TREE", {"GIT_DIR": str(other_git), "GIT_WORK_TREE": str(other)}, True),
                  ("an ABSOLUTE GIT_INDEX_FILE", {"GIT_INDEX_FILE": str((other_git / "index").resolve())}, False))
        for n, (label, extra, steers_bare_git) in enumerate(steers, start=3):
            bare = subprocess.run(["git", "-C", str(own), "log", "--pretty=format:%s"],
                                  env=dict(base, **extra), capture_output=True, text=True,
                                  encoding="utf-8", errors="replace")
            lacks = "Fixture landing FX1" not in bare.stdout
            rc, said = check(extra)
            arm(rc == 0 and lacks == steers_bare_git,
                "(%d) a caller's %s naming another repository leaves --check reading its own history"
                % (n, label),
                "bare-git-log-lacks-FX1=%s (expected %s) rc=%d %s" % (lacks, steers_bare_git, rc, said[-200:]))
    finally:
        gone = ot.remove_tree(str(box))
    arm(gone, "(6) the fixture box is removed -- git's read-only objects included", "left behind: %s" % box)

    if len(ran) != SELF_TEST_ARMS:
        print("refresh_landing_log self-test: FAIL -- %d arm(s) ran, %d expected" % (len(ran), SELF_TEST_ARMS))
        return 1
    if failed:
        print("refresh_landing_log self-test: FAIL -- %d of %d arm(s)" % (len(failed), len(ran)))
        return 1
    print("refresh_landing_log self-test: OK -- %d arm(s): the landing log is read from this tree's own "
          "history under GIT_DIR, GIT_DIR + GIT_WORK_TREE and an absolute GIT_INDEX_FILE" % len(ran))
    return 0


def main() -> int:
    # Plan files are UTF-8 with emoji status markers; Windows defaults
    # stdout to cp1252 which would crash on the diff output. Reconfigure
    # once at entry so --check can print the diff cleanly on any host.
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8")
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument(
        "--check",
        action="store_true",
        help="exit non-zero if any plan file would change",
    )
    mode.add_argument(
        "--write",
        action="store_true",
        help="apply the rewrite in place",
    )
    mode.add_argument(
        "--self-test",
        action="store_true",
        help="prove this tool reads the history of the tree it lives in, whatever git "
             "environment the caller exported (ctest: landing_log_git_environment_guard)",
    )
    parser.add_argument(
        "--config",
        type=Path,
        default=CONFIG_PATH,
        help="path to landing-log-config.json (default: .harness-config/runner/actions/refresh_landing_log/landing-log-config.json)",
    )
    args = parser.parse_args()
    if args.self_test:
        return self_test()

    plans = load_config(args.config)
    all_subjects = git_log_subjects()

    drift = False
    for spec in plans:
        if not spec.path.exists():
            print(f"missing plan file: {spec.path}", file=sys.stderr)
            drift = True
            continue
        original, new = process_plan(spec, all_subjects)
        if original == new:
            continue
        drift = True
        if args.check:
            diff = difflib.unified_diff(
                original.splitlines(keepends=True),
                new.splitlines(keepends=True),
                fromfile=str(spec.path) + " (current)",
                tofile=str(spec.path) + " (regenerated)",
            )
            sys.stdout.writelines(diff)
        if args.write:
            spec.path.write_text(new, encoding="utf-8")
            print(f"updated: {spec.path}")

    if drift and args.check:
        print("\nLanding-log hashes are out of date. Run with --write to regenerate.",
              file=sys.stderr)
        return 1
    # ★ A SUCCESS WITNESS, printed only on this path. `--check` used to say nothing when
    # nothing drifted, so a run that read no plan at all and a run that verified every one
    # were indistinguishable -- and the DssHarness action that runs it declares a
    # `successPattern`, because exiting 0 is not proof the check ran.
    if args.check:
        print(f"refresh_landing_log: --check OK ({len(plans)} plan file(s), every "
              f"landing-log marker current)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
