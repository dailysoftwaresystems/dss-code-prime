"""Unit tests for refresh_landing_log.

Pure-Python; uses only stdlib so it runs identically on Windows, Linux,
and macOS without any pip install.

Run from repo root:
    py tools/test_refresh_landing_log.py
"""

from __future__ import annotations

import re
import sys
import unittest
from pathlib import Path

THIS_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(THIS_DIR))

from refresh_landing_log import (  # noqa: E402  (path bootstrap above)
    Commit,
    PlanSpec,
    classify_commit,
    collect_commits_by_pr,
    render_hash_block,
    rewrite_text,
)


SUBJECT_PATTERN = re.compile(
    r"^Substrate hardening (SH[0-9]+)[a-z]?(?:\s+(review)(?:\s+round-([0-9]+))?)?:"
)


def _spec(tmp: Path) -> PlanSpec:
    return PlanSpec(path=tmp, subject_pattern=SUBJECT_PATTERN)


class RenderHashBlockTest(unittest.TestCase):
    def test_empty(self):
        self.assertEqual(render_hash_block([]), "")

    def test_single_initial(self):
        out = render_hash_block([Commit("abc1234", "initial")])
        self.assertEqual(out, "`abc1234`")

    def test_initial_plus_review(self):
        out = render_hash_block(
            [Commit("abc1234", "initial"), Commit("def5678", "review")]
        )
        self.assertEqual(out, "`abc1234` + `def5678` (review followup)")

    def test_initial_plus_two_review_rounds(self):
        out = render_hash_block(
            [
                Commit("aaaaaaa", "initial"),
                Commit("bbbbbbb", "round-1"),
                Commit("ccccccc", "round-2"),
            ]
        )
        self.assertEqual(
            out,
            "`aaaaaaa` + `bbbbbbb` (round-1) + `ccccccc` (round-2)",
        )

    def test_three_sub_deliverables_each_labeled(self):
        # SH4 lands as three sub-PRs (SH4a, SH4c, SH4b). Each carries its
        # full PR id as the label so reviewers can map hashes to sub-PRs
        # without consulting git.
        out = render_hash_block(
            [
                Commit("e22a728", "sub:SH4a"),
                Commit("ded11d3", "sub:SH4c"),
                Commit("130d5b1", "sub:SH4b"),
            ]
        )
        self.assertEqual(
            out,
            "`e22a728` (SH4a) + `ded11d3` (SH4c) + `130d5b1` (SH4b)",
        )

    def test_mixed_all_initial_renders_first_only(self):
        # Defensive: two commits both classified as `initial` would mean
        # the config grouped unrelated commits — render only the first to
        # surface the misconfiguration rather than fabricate a label.
        out = render_hash_block(
            [Commit("aaa", "initial"), Commit("bbb", "initial")]
        )
        self.assertEqual(out, "`aaa`")


class ClassifyCommitTest(unittest.TestCase):
    def _match(self, subject: str) -> re.Match[str]:
        m = SUBJECT_PATTERN.match(subject)
        assert m, subject
        return m

    def test_initial(self):
        self.assertEqual(classify_commit(self._match("Substrate hardening SH3: foo")), "initial")

    def test_review(self):
        self.assertEqual(
            classify_commit(self._match("Substrate hardening SH3 review: bar")),
            "review",
        )

    def test_round(self):
        self.assertEqual(
            classify_commit(self._match("Substrate hardening SH3 review round-2: bar")),
            "round-2",
        )

    def test_sub_deliverable(self):
        # `SH4a:` (sub-PR letter, no review marker) classifies as
        # sub:<full-id> so the renderer can label it `(SH4a)`.
        self.assertEqual(
            classify_commit(self._match("Substrate hardening SH4a: foo")),
            "sub:SH4a",
        )

    def test_sub_deliverable_with_review_marker_prefers_round_or_review(self):
        # If somehow both a sub letter AND a review marker appear, the
        # review classification wins (the suffix is metadata, but a
        # review followup is the more load-bearing kind).
        self.assertEqual(
            classify_commit(self._match("Substrate hardening SH4a review: bar")),
            "review",
        )


class CollectCommitsByPrTest(unittest.TestCase):
    def test_initial_only(self):
        subjects = [
            ("abc12340000000000000000000000000000000aa", "Substrate hardening SH2: opt CI into macOS"),
            ("fff99990000000000000000000000000000000bb", "Unrelated commit"),
        ]
        spec = _spec(Path("dummy"))
        result = collect_commits_by_pr(spec, subjects)
        self.assertEqual(list(result.keys()), ["SH2"])
        self.assertEqual(result["SH2"], [Commit("abc1234", "initial")])

    def test_initial_plus_review_in_chrono_order(self):
        subjects = [
            ("a" * 40, "Substrate hardening SH3: cross-tree guard"),
            ("b" * 40, "Substrate hardening SH3 review: address review"),
        ]
        spec = _spec(Path("dummy"))
        result = collect_commits_by_pr(spec, subjects)
        self.assertEqual(
            result["SH3"],
            [Commit("a" * 7, "initial"), Commit("b" * 7, "review")],
        )

    def test_multiple_prs(self):
        subjects = [
            ("a" * 40, "Substrate hardening SH2: foo"),
            ("b" * 40, "Substrate hardening SH3: bar"),
            ("c" * 40, "Substrate hardening SH3 review: baz"),
        ]
        spec = _spec(Path("dummy"))
        result = collect_commits_by_pr(spec, subjects)
        self.assertEqual(set(result.keys()), {"SH2", "SH3"})
        self.assertEqual(result["SH2"], [Commit("a" * 7, "initial")])
        self.assertEqual(
            result["SH3"],
            [Commit("b" * 7, "initial"), Commit("c" * 7, "review")],
        )


SECTION_HEADER = "### PR landing log\n\n"


def _wrap(body: str) -> str:
    """Wrap a table body in the landing-log section header so rewrite_text
    actually processes it (the section gate ignores text outside `###
    PR landing log`)."""
    return SECTION_HEADER + body


class RewriteTextTest(unittest.TestCase):
    def test_first_run_inserts_close_marker_and_hashes(self):
        original = _wrap(
            "| SH2 | ✅ done | Description... <!-- LANDING-LOG-HASHES: SH2 --> |\n"
        )
        commits = {"SH2": [Commit("ab0800e", "initial")]}
        new = rewrite_text(original, commits)
        self.assertEqual(
            new,
            _wrap(
                "| SH2 | ✅ done | Description... <!-- LANDING-LOG-HASHES: SH2 -->`ab0800e`<!-- /LANDING-LOG-HASHES --> |\n"
            ),
        )

    def test_idempotent_on_second_run(self):
        original = _wrap("| SH2 | <!-- LANDING-LOG-HASHES: SH2 --> |\n")
        commits = {"SH2": [Commit("ab0800e", "initial")]}
        once = rewrite_text(original, commits)
        twice = rewrite_text(once, commits)
        self.assertEqual(once, twice)

    def test_replaces_stale_hash_block(self):
        original = _wrap(
            "| SH3 | <!-- LANDING-LOG-HASHES: SH3 -->`stalehex`<!-- /LANDING-LOG-HASHES --> |\n"
        )
        commits = {"SH3": [Commit("ac76408", "initial"), Commit("def0123", "review")]}
        new = rewrite_text(original, commits)
        self.assertIn(
            "<!-- LANDING-LOG-HASHES: SH3 -->`ac76408` + `def0123` (review followup)<!-- /LANDING-LOG-HASHES -->",
            new,
        )
        self.assertNotIn("stalehex", new)

    def test_marker_absent_row_is_untouched(self):
        # Row with no marker — hand-managed; the script must not modify it.
        original = _wrap(
            "| PR0 | ✅ shipped | `4aef654` (initial) + `3aca464` (review followup). |\n"
        )
        commits = {"PR0": [Commit("differen", "initial")]}
        self.assertEqual(rewrite_text(original, commits), original)

    def test_missing_close_marker_inserted_then_filled(self):
        # The user puts an opening marker, never the closing. First run
        # fills it; second run is a no-op.
        original = _wrap("| SH1 | <!-- LANDING-LOG-HASHES: SH1 --> body |\n")
        commits = {"SH1": [Commit("a1b2c3d", "initial")]}
        first = rewrite_text(original, commits)
        self.assertEqual(
            first,
            _wrap(
                "| SH1 | <!-- LANDING-LOG-HASHES: SH1 -->`a1b2c3d`<!-- /LANDING-LOG-HASHES --> body |\n"
            ),
        )
        self.assertEqual(rewrite_text(first, commits), first)

    def test_unknown_pr_renders_empty(self):
        # If git has no commits matching the PR identifier, the hash block
        # is empty — but the markers stay in place so a future commit
        # can populate it without re-editing.
        original = _wrap("| SHX | <!-- LANDING-LOG-HASHES: SHX --> |\n")
        new = rewrite_text(original, commits_by_pr={})
        self.assertEqual(
            new,
            _wrap(
                "| SHX | <!-- LANDING-LOG-HASHES: SHX --><!-- /LANDING-LOG-HASHES --> |\n"
            ),
        )

    def test_multiple_markers_on_distinct_lines(self):
        original = _wrap(
            "| SH2 | <!-- LANDING-LOG-HASHES: SH2 --> |\n"
            "| SH3 | <!-- LANDING-LOG-HASHES: SH3 --> |\n"
        )
        commits = {
            "SH2": [Commit("ab0800e", "initial")],
            "SH3": [Commit("ac76408", "initial")],
        }
        new = rewrite_text(original, commits)
        self.assertEqual(
            new,
            _wrap(
                "| SH2 | <!-- LANDING-LOG-HASHES: SH2 -->`ab0800e`<!-- /LANDING-LOG-HASHES --> |\n"
                "| SH3 | <!-- LANDING-LOG-HASHES: SH3 -->`ac76408`<!-- /LANDING-LOG-HASHES --> |\n"
            ),
        )

    def test_close_marker_does_not_cross_lines(self):
        # If row N has an unclosed opener and row N+1 has a closer for a
        # different opener, we must NOT match across them — that would
        # silently swallow row N's body prose between the two markers.
        original = _wrap(
            "| SH1 | <!-- LANDING-LOG-HASHES: SH1 --> hand-typed body |\n"
            "| SH2 | <!-- LANDING-LOG-HASHES: SH2 -->`old`<!-- /LANDING-LOG-HASHES --> |\n"
        )
        commits = {
            "SH1": [Commit("aaaaaaa", "initial")],
            "SH2": [Commit("bbbbbbb", "initial")],
        }
        new = rewrite_text(original, commits)
        # Row 1 got its own close marker inserted; row 2 was updated in place.
        self.assertIn(
            "<!-- LANDING-LOG-HASHES: SH1 -->`aaaaaaa`<!-- /LANDING-LOG-HASHES --> hand-typed body",
            new,
        )
        self.assertIn(
            "<!-- LANDING-LOG-HASHES: SH2 -->`bbbbbbb`<!-- /LANDING-LOG-HASHES -->",
            new,
        )
        self.assertNotIn("`old`", new)

    def test_section_gate_skips_marker_references_in_prose(self):
        # The SH1 surface section explains the marker format as part of
        # prose AFTER the landing log table. The script must NOT treat
        # those references as live anchors.
        original = (
            "### PR landing log\n\n"
            "| SH2 | <!-- LANDING-LOG-HASHES: SH2 --> |\n"
            "\n"
            "## 2. PRs\n\n"
            "The marker format is `<!-- LANDING-LOG-HASHES: SH3 -->`.\n"
        )
        commits = {
            "SH2": [Commit("ab0800e", "initial")],
            "SH3": [Commit("ac76408", "initial")],
        }
        new = rewrite_text(original, commits)
        # SH2 anchor inside the table was filled.
        self.assertIn(
            "<!-- LANDING-LOG-HASHES: SH2 -->`ab0800e`<!-- /LANDING-LOG-HASHES -->",
            new,
        )
        # SH3 reference in prose stayed verbatim (no closing marker added).
        self.assertIn(
            "The marker format is `<!-- LANDING-LOG-HASHES: SH3 -->`.\n",
            new,
        )

    def test_no_landing_log_section_returns_unchanged(self):
        original = "## 1. Vision\n\nSome prose. <!-- LANDING-LOG-HASHES: PR1 -->\n"
        self.assertEqual(rewrite_text(original, {"PR1": [Commit("aaa", "initial")]}),
                         original)


if __name__ == "__main__":
    unittest.main(verbosity=2)
