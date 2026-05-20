#!/usr/bin/env python3
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
    py tools/refresh_landing_log.py --check   # CI gate; exits non-zero on drift
    py tools/refresh_landing_log.py --write   # apply rewrite in place

Configuration in `tools/landing-log-config.json` lists which plan files
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

REPO_ROOT = Path(__file__).resolve().parent.parent
CONFIG_PATH = REPO_ROOT / "tools" / "landing-log-config.json"

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


def git_log_subjects() -> list[tuple[str, str]]:
    """Return (full_sha, subject) pairs for every commit reachable from HEAD.

    Reverse-chronological is git's default; we reverse to chronological so
    callers see "initial" before "review followup".
    """
    out = subprocess.check_output(
        ["git", "log", "--pretty=format:%H %s"],
        cwd=REPO_ROOT,
        text=True,
        encoding="utf-8",
    )
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
    parser.add_argument(
        "--config",
        type=Path,
        default=CONFIG_PATH,
        help="path to landing-log-config.json (default: tools/landing-log-config.json)",
    )
    args = parser.parse_args()

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
    return 0


if __name__ == "__main__":
    sys.exit(main())
