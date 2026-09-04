#!/usr/bin/env python3
# PURPOSE: refuse a documented corpus figure that the census refutes, and repair it in place.
"""check-doc-census.py -- THE DOCUMENTED-FIGURE GUARD.

★★★ WHY THIS EXISTS, and it is this repository's own measured failure rather
than a tidiness rule.

`scripts/examples-census/examples-census.py` was written precisely so nobody
would hand-count the corpus again. Its own PURPOSE line says it exists to
"re-derive every corpus-manifest figure examples/README.md states", and its
header records three separate ad-hoc parsers that were written, trusted and
thrown away before it. **It worked. The numbers rotted anyway.**

✔MEASURED 2026-09-04 (cycle P59), `examples-census.py` against the block it was
built to serve: the README states **634** manifests; the tree holds **788**. It
states **514** manifests carrying `optimizedPipelines` declaring **707** arms;
the tree holds **659** and **879**. And one claim did not merely drift, it
INVERTED: the README says of `mustDifferFromBaseline` that "**0** declare it
false" -- six manifests declare it false today.

⇒ ★★ THE LESSON, AND IT IS THE REASON THIS GUARD IS NOT A SECOND CENSUS: the
missing half was never the derivation. It was the COMPARISON. An instrument
somebody must remember to run, and then hand-transcribe, is a document with
extra steps -- `[[feedback-an-instrument-that-answers-an-adjacent-question]]`.
The census answers "what is true"; nothing answered "does the document agree",
and that is the only question a reader of the document is actually asking.

★★★ THE CLAIM IS DECLARED BY THE DOCUMENT, NOT LISTED HERE. A guard holding its
own table of "the twelve figures in the README" is a third copy of the same
fact, and would rot exactly like the first two. Instead each figure MARKS ITSELF:

    ... over the <!--census:examples:manifests-->**788** manifests in the tree ...

The marker is an HTML comment, so it is invisible in every rendered view, and it
binds to the number that immediately follows it. Adding a checked figure needs
no change to this file; deleting one is caught by the floor. Any markdown
document in the tree may carry a claim -- there is no registration step, because
a registration step is the thing that gets forgotten.

★ WHY A PROVIDER PREFIX (`examples:`). The key namespace belongs to the
instrument that owns it, so a second census can be added as a row in PROVIDERS
without touching the marker grammar or re-interpreting any existing claim. Today
there is exactly one provider and the prefix is still spelled, because a
namespace retro-fitted onto unnamespaced keys is a migration.

THE CONTRACT, and every clause is a way a documented figure can lie:
  1. every marker names a KNOWN provider and a key that provider actually
     reports -- a typo resolves to nothing, and "resolves to nothing" must never
     read as "agrees";
  2. every marker is followed by a parseable integer (markdown emphasis and
     whitespace may sit between, nothing else);
  3. every bound figure EQUALS the provider's value for that key;
  4. the provider ran, exited 0, and reported a non-empty key set -- a census
     that collapses prints a tidy set of zeroes, and a document agreeing with
     zeroes is the worst possible pass;
  5. the scan has a FLOOR. Deleting the markers is the cheapest way to make this
     guard green, so too few claims is a REFUSAL, not a clean run;
  6. at least one markdown document was read at all.

⚠ THE EXCLUSIONS ARE PART OF THE CONTRACT AND ARE TESTED AS SUCH. `.worktrees/`
holds full checkouts of this repository while lanes are in flight; scanning them
would red this guard on another lane's half-finished document, and widening the
exclusion until the red stops is how an exclusion silently swallows the real
tree. Self-test arm 11 asserts a drifted claim inside `.worktrees/` is IGNORED
and arm 12 asserts one in the live tree beside it is still CAUGHT, so the
exclusion is pinned in BOTH directions -- see
`[[feedback-an-escape-every-row-triggers-disarms-the-guard]]`.

Exit codes: 0 OK - 1 a document disagrees with the census - 2 the scan or a
provider collapsed (structural: fix the scan, never lower the floor) - 3 usage.

Usage:
    python scripts/check-doc-census/check-doc-census.py             # verify
    python scripts/check-doc-census/check-doc-census.py --write     # repair figures
    python scripts/check-doc-census/check-doc-census.py --selftest  # prove it fails
    python scripts/check-doc-census/check-doc-census.py --repo <p>  # act on another tree
"""
from __future__ import annotations

import io
import json
import os
import re
import subprocess
import sys
import tempfile

# ── OUTPUT ENCODING -- NOT COSMETIC ────────────────────────────────────────────
# ✔MEASURED 2026-08-23 (CPython, Windows, BOTH streams PIPES, which is exactly how
# ctest runs every guard): `sys.stdout` comes up `encoding='cp1252'`. A report
# printed on stdout that names a document containing a non-ASCII glyph then raises
# `UnicodeEncodeError` and kills the guard INSIDE ITS OWN REPORT -- the run still
# reds, but the finding is lost and the traceback names a `print`. Applied at
# IMPORT so every path this module can print on is covered.
# D-GATE-PYTHON-GUARD-DIES-PRINTING-TREE-TEXT-ON-A-WINDOWS-PIPE
for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError, OSError):   # pragma: no cover - odd stream
        pass

EXIT_OK, EXIT_DISAGREE, EXIT_COLLAPSE, EXIT_USAGE = 0, 1, 2, 3

# The tree acted on defaults to the one THIS SCRIPT LIVES IN, never the caller's
# cwd -- the same rule `scripts/lane-worktree/` follows, and for the same reason:
# a guard that silently measures whichever directory it was launched from is a
# guard that can be made green by cd-ing somewhere else.
SELF_REPO = os.path.realpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))

# Each provider is the ARGV of a command, relative to the repo root, that prints a
# flat {key: int} JSON object on stdout. Resolved against `--repo`, so the
# self-test controls what is at that path without this file growing a test-only
# branch.
PROVIDERS = {
    "examples": (os.path.join("scripts", "examples-census", "examples-census.py"), "--json"),
}

# Directories never scanned. `.worktrees` holds in-flight lane checkouts of this
# same repository (see the header); the rest are build output and secrets.
SKIP_DIRS = {".git", ".worktrees", ".secrets", "node_modules", "__pycache__"}
SKIP_DIR_PREFIXES = ("build",)

# A marker, then optional markdown emphasis / whitespace, then the figure.
# The figure may carry `,` thousands separators; the separator style is PRESERVED
# on --write, so repairing a number never restyles the prose around it.
#
# ⚠⚠ `census-quoted:` IS THE QUOTATION ESCAPE, AND IT EXISTS BECAUSE THIS GUARD BIT
# ITSELF WITHIN AN HOUR OF SHIPPING. ✔MEASURED 2026-09-04: the registry row recording
# this guard's own closure ILLUSTRATES the marker syntax with a concrete example, and
# that illustration was promptly bound as a LIVE claim in
# `.plans/_deferred-anchor-registry-done.md` -- so the next corpus change would have
# reddened the guard on an ARCHIVED AUDIT TRAIL, and `--write` would have EDITED a
# closed row's text to "repair" it. A document explaining the convention cannot be
# forced to avoid writing it down. (`scripts/check-line-endings/` carries a
# CR-INSTRUMENT-QUOTED region for exactly this reason.)
# ★ IT IS A PER-MARKER ESCAPE, NOT A REGION, ON PURPOSE: a begin/end region left
#   unterminated silences the REST OF THE FILE, and this project does not ship an
#   escape whose failure mode is silence over an unbounded range.
# ★ AND A QUOTED MARKER STILL VALIDATES ITS PROVIDER AND KEY (clause 1 applies to it
#   unchanged) -- quoting suppresses the COMPARISON, never the vocabulary check, so a
#   typo cannot hide behind it and a quotation of a key that no longer exists reds.
CLAIM = re.compile(
    r"<!--\s*census(?P<quoted>-quoted)?:(?P<provider>[A-Za-z0-9_-]+):"
    r"(?P<key>[A-Za-z0-9_.$-]+)\s*-->"
    r"(?P<gap>[*_`\s]*)"
    r"(?P<figure>\d[\d,]*)?"
)

# Far below the live figure so ordinary churn never trips it, and high enough that
# a collapsed scan -- or a document whose markers were deleted to silence a red --
# cannot masquerade as a pass. Raise it by MEASURING, never to make a red go away.
CLAIM_FLOOR = 10


class Collapse(Exception):
    """The scan or a provider failed structurally. Never reported as a clean pass."""


class Claim:
    __slots__ = ("doc", "provider", "key", "documented", "raw", "start", "end", "quoted")

    def __init__(self, doc, provider, key, documented, raw, start, end, quoted=False):
        self.doc = doc
        self.provider = provider
        self.key = key
        self.documented = documented
        self.raw = raw
        self.start = start
        self.end = end
        self.quoted = quoted


# ── READING THE TREE ───────────────────────────────────────────────────────────

def markdown_docs(repo):
    """Every markdown document in the tree, excluding the homes listed above."""
    out = []
    for root, dirs, files in os.walk(repo):
        dirs[:] = sorted(d for d in dirs
                         if d not in SKIP_DIRS and not d.startswith(SKIP_DIR_PREFIXES))
        for f in sorted(files):
            if f.lower().endswith(".md"):
                out.append(os.path.join(root, f))
    return out


def claims_in(repo, path):
    text = io.open(path, encoding="utf-8", errors="replace").read()
    rel = os.path.relpath(path, repo).replace(os.sep, "/")
    found = []
    for m in CLAIM.finditer(text):
        figure = m.group("figure")
        if m.group("quoted"):
            # A QUOTATION. Its provider and key are still checked (see the CLAIM
            # comment); only the figure comparison is suppressed, and no figure need
            # follow it at all.
            found.append(Claim(rel, m.group("provider"), m.group("key"),
                               None, figure or "", m.start(), m.end(), quoted=True))
            continue
        if figure is None:
            # Clause 2. A marker with nothing countable after it resolves to
            # nothing, and "resolves to nothing" must never read as "agrees".
            raise Collapse(
                "%s: the claim `census:%s:%s` is not followed by a number "
                "(only markdown emphasis and whitespace may sit between the marker "
                "and the figure it binds)."
                % (rel, m.group("provider"), m.group("key")))
        found.append(Claim(rel, m.group("provider"), m.group("key"),
                           int(figure.replace(",", "")), figure,
                           m.start("figure"), m.end("figure")))
    return text, found


# ── READING THE CENSUS ─────────────────────────────────────────────────────────

def provider_values(repo, name):
    if name not in PROVIDERS:
        raise Collapse(
            "unknown census provider '%s' -- known providers: %s. A marker naming a "
            "provider that does not exist can never disagree with anything, so it is "
            "refused rather than skipped."
            % (name, ", ".join(sorted(PROVIDERS)) or "(none)"))
    argv = PROVIDERS[name]
    cmd = [sys.executable, os.path.join(repo, argv[0])] + list(argv[1:])
    try:
        p = subprocess.run(cmd, cwd=repo, capture_output=True, text=True,
                           encoding="utf-8", errors="replace")
    except OSError as e:
        raise Collapse("census provider '%s' could not be launched (%s): %s"
                       % (name, e, " ".join(cmd)))
    if p.returncode != 0:
        raise Collapse(
            "census provider '%s' exited %d -- the figures it reports cannot be "
            "trusted, so nothing is compared against them.\n  cmd: %s\n  %s"
            % (name, p.returncode, " ".join(cmd), (p.stderr or p.stdout).strip()[:800]))
    try:
        values = json.loads(p.stdout)
    except ValueError as e:
        raise Collapse("census provider '%s' did not print JSON (%s): %s"
                       % (name, e, p.stdout.strip()[:400]))
    if not isinstance(values, dict) or not values:
        # Clause 4. A census that collapses prints a tidy set of zeroes, and a
        # document agreeing with zeroes is the worst possible pass.
        raise Collapse(
            "census provider '%s' reported an EMPTY key set. A census over an empty "
            "corpus agrees with any document that says zero; fix the provider rather "
            "than trusting this run." % name)
    return values


# ── THE GUARD ──────────────────────────────────────────────────────────────────

def run(repo, write):
    docs = markdown_docs(repo)
    if not docs:
        raise Collapse("no markdown document was found under %s -- the scan read "
                       "nothing, which is a structural failure, not a pass." % repo)

    per_doc, claims = {}, []
    for d in docs:
        text, found = claims_in(repo, d)
        if found:
            per_doc[d] = text
            claims.extend(found)

    # ★ THE FLOOR COUNTS LIVE CLAIMS ONLY. Converting a claim to a quotation removes it
    #   from this guard's sight exactly as deleting it would, so it must not buy floor
    #   headroom; quotations are REPORTED separately instead, so a reader can see how
    #   many there are rather than having them vanish into the total.
    quoted = [c for c in claims if c.quoted]
    claims = [c for c in claims if not c.quoted]

    if len(claims) < CLAIM_FLOOR:
        # Clause 5. Deleting the markers is the cheapest way to make this guard
        # green. That route is a refusal.
        raise Collapse(
            "the scan COLLAPSED: found only %d documented figure(s) across %d markdown "
            "document(s), floor is %d. Deleting a `<!--census:...-->` marker removes a "
            "figure from this guard's sight; fix the scan or restore the markers, never "
            "lower the floor." % (len(claims), len(docs), CLAIM_FLOOR))

    cache = {}
    for name in sorted({c.provider for c in claims + quoted}):
        cache[name] = provider_values(repo, name)

    def check_vocabulary(c):
        """Clause 1, and it applies to a QUOTATION unchanged -- quoting suppresses the
        comparison, never the vocabulary check."""
        values = cache[c.provider]
        if c.key not in values:
            raise Collapse(
                "%s: the %s `census%s:%s:%s` names a key the provider does not report. "
                "Known keys: %s"
                % (c.doc, "quotation" if c.quoted else "claim",
                   "-quoted" if c.quoted else "", c.provider, c.key,
                   ", ".join(sorted(values))))
        return values[c.key]

    for c in quoted:
        check_vocabulary(c)

    wrong, repaired = [], {}
    for c in claims:
        actual = check_vocabulary(c)
        if actual != c.documented:
            wrong.append((c, actual))
            if write:
                repaired.setdefault(c.doc, []).append((c, actual))

    if write and repaired:
        for doc, text in list(per_doc.items()):
            rel = os.path.relpath(doc, repo).replace(os.sep, "/")
            edits = repaired.get(rel)
            if not edits:
                continue
            # Rewrite right-to-left so earlier spans keep their offsets.
            for c, actual in sorted(edits, key=lambda e: e[0].start, reverse=True):
                text = text[:c.start] + restyle(c.raw, actual) + text[c.end:]
            io.open(doc, "wb").write(text.encode("utf-8"))
            print("  repaired %-40s %d figure(s)" % (rel, len(edits)))

    print("check-doc-census: %d figure(s) in %d document(s), %d provider(s)%s"
          % (len(claims), len(per_doc), len(cache),
             "" if not quoted else
             ", plus %d quotation(s) of the marker syntax (vocabulary checked, figure "
             "not compared)" % len(quoted)))

    if wrong and not write:
        print("check-doc-census: FAIL -- %d documented figure(s) the census refutes:"
              % len(wrong))
        for c, actual in wrong:
            print("    %s  census:%s:%s  documented %s, actual %d"
                  % (c.doc, c.provider, c.key, c.raw, actual))
        print("  Repair them in place (the prose is untouched, only the numbers move):")
        print("      python scripts/check-doc-census/check-doc-census.py --write")
        print("  ⚠ A figure is a DATED INVENTORY. If a SENTENCE around one has also gone "
              "false, --write will not notice -- read the claim, not only the number.")
        return EXIT_DISAGREE

    if wrong:
        print("check-doc-census: repaired %d figure(s). Re-run to verify." % len(wrong))
        return EXIT_OK

    print("check-doc-census: OK -- every documented figure matches the census.")
    return EXIT_OK


def restyle(raw, value):
    """Render `value` in the separator style the document already used."""
    return "{:,}".format(value) if "," in raw else str(value)


# ── SELF-TEST ──────────────────────────────────────────────────────────────────
#
# ★ THE FIXTURE SYNTHESIZES THE NEGATIVE. Each arm below BREAKS something and
#   asserts this guard refuses with a message that names it; the control arm (0)
#   asserts the untouched fixture is GREEN. An ADD-direction fixture -- building a
#   tree that already agrees and checking it passes -- would stay green if the
#   comparison were deleted outright.
#   See [[feedback-a-fixture-must-synthesize-the-negative]].
#
# ★ THE PROVIDER IS A STUB AT THE REAL PATH, not a test-only branch in this file.
#   The guard resolves `examples` to `scripts/examples-census/examples-census.py`
#   under `--repo` and always has; the fixture simply controls what is there.

_STUB = """import json, sys
print(json.dumps({%s}))
"""

_STUB_KEYS = {"manifests": 788, "arms": 879, "targets": 2678, "top.source": 757,
              "top.sources": 26, "top.project": 5, "top.targets": 788,
              "arms.passes": 310, "arms.shippedPipeline": 569,
              "arms.mustDifferTrue": 695, "arms.mustDifferFalse": 6,
              "dependsOn.entries": 22}


def _fixture(tmp, name, stub_body=None, doc_body=None):
    # ⚠ Each arm gets its OWN root. An earlier draft reused one directory and the
    # later arms silently rebuilt the fixture the earlier ones were still asserting
    # against -- a green that meant nothing.
    root = os.path.join(tmp, name)
    census_dir = os.path.join(root, "scripts", "examples-census")
    os.makedirs(census_dir, exist_ok=True)
    body = stub_body if stub_body is not None else (
        _STUB % ", ".join("%r: %d" % (k, v) for k, v in sorted(_STUB_KEYS.items())))
    io.open(os.path.join(census_dir, "examples-census.py"), "wb").write(body.encode("utf-8"))
    docs = os.path.join(root, "examples")
    os.makedirs(docs, exist_ok=True)
    if doc_body is None:
        doc_body = "# fixture\n\n" + "".join(
            "- key %s: <!--census:examples:%s-->**%d** today\n" % (k, k, v)
            for k, v in sorted(_STUB_KEYS.items()))
    io.open(os.path.join(docs, "README.md"), "wb").write(doc_body.encode("utf-8"))
    return root


def _doc(root):
    return os.path.join(root, "examples", "README.md")


def _read(p):
    return io.open(p, encoding="utf-8").read()


def _write(p, s):
    io.open(p, "wb").write(s.encode("utf-8"))


def _arm(label, root, expect_rc, says=None, write=False):
    argv = ["--repo", root] + (["--write"] if write else [])
    p = subprocess.run([sys.executable, os.path.abspath(__file__)] + argv,
                       capture_output=True, text=True, encoding="utf-8", errors="replace")
    out = (p.stdout or "") + (p.stderr or "")
    ok = p.returncode == expect_rc
    if ok and says is not None:
        ok = says in out
    print("  %-34s rc=%d (want %d) %s" % (label, p.returncode, expect_rc, "OK" if ok else "FAIL"))
    if not ok:
        print("      says: %s" % out.strip().replace("\n", "\n      ")[:900])
    return ok


def selftest():
    print("check-doc-census --selftest")
    ok = True
    with tempfile.TemporaryDirectory() as tmp:
        # 0 -- THE CONTROL. Without it a red proves only that something broke.
        root = _fixture(tmp, "control")
        ok &= _arm("0 CONTROL-AGREES", root, EXIT_OK, says="every documented figure")

        # 1 -- the figure moved. The whole point.
        d = _doc(root)
        _write(d, _read(d).replace("**788**", "**634**", 1))
        ok &= _arm("1 FIGURE-DRIFTED", root, EXIT_DISAGREE, says="documented 634, actual 788")

        # 2 -- --write repairs it, and the repaired tree verifies clean.
        ok &= _arm("2 WRITE-REPAIRS", root, EXIT_OK, says="repaired", write=True)
        ok &= _arm("2b VERIFIES-AFTER-WRITE", root, EXIT_OK, says="every documented figure")

        # 2c -- the separator STYLE survives a repair. A guard that restyles the
        # prose while fixing a number is editing more than it was asked to, and
        # `targets` (2678) is the arm that can show it: a comma-styled claim must
        # come back comma-styled.
        styled = _fixture(tmp, "styled")
        sd = _doc(styled)
        _write(sd, _read(sd).replace("**2678**", "**1,111**", 1))
        _arm("2c-setup", styled, EXIT_DISAGREE)
        _arm("2c-write", styled, EXIT_OK, write=True)
        style_kept = "**2,678**" in _read(sd)
        print("  %-34s %s" % ("2c SEPARATOR-STYLE", "OK" if style_kept else "FAIL"))
        ok &= style_kept

        # 3 -- a key the provider does not report. A typo must not read as agreement.
        _write(d, _read(d).replace("census:examples:manifests",
                                   "census:examples:manifestz", 1))
        ok &= _arm("3 UNKNOWN-KEY", root, EXIT_COLLAPSE, says="does not report")
        _write(d, _read(d).replace("census:examples:manifestz",
                                   "census:examples:manifests", 1))

        # 4 -- a provider that does not exist.
        _write(d, _read(d).replace("census:examples:arms", "census:corpus:arms", 1))
        ok &= _arm("4 UNKNOWN-PROVIDER", root, EXIT_COLLAPSE, says="unknown census provider")
        _write(d, _read(d).replace("census:corpus:arms", "census:examples:arms", 1))

        # 5 -- a marker bound to nothing countable.
        _write(d, _read(d).replace("<!--census:examples:targets-->**2678**",
                                   "<!--census:examples:targets--> lots", 1))
        ok &= _arm("5 NO-NUMBER-AFTER-MARKER", root, EXIT_COLLAPSE, says="not followed by a number")
        _write(d, _read(d).replace("<!--census:examples:targets--> lots",
                                   "<!--census:examples:targets-->**2678**", 1))

        # 6 -- the cheapest way to go green: delete the markers. Refused.
        few = _fixture(tmp, "thin",
                       doc_body="# thin\n\n<!--census:examples:manifests-->**788**\n")
        ok &= _arm("6 FLOOR-COLLAPSE", few, EXIT_COLLAPSE, says="floor is %d" % CLAIM_FLOOR)

        # 7 -- the provider itself failed. Nothing may be compared against it.
        broke = _fixture(tmp, "provfail", stub_body="import sys\nsys.exit(9)\n")
        ok &= _arm("7 PROVIDER-FAILED", broke, EXIT_COLLAPSE, says="exited 9")

        # 8 -- the provider collapsed to an empty key set: agrees with any zero.
        empty = _fixture(tmp, "provempty", stub_body="print('{}')\n")
        ok &= _arm("8 PROVIDER-EMPTY", empty, EXIT_COLLAPSE, says="EMPTY key set")

        # 9 -- the provider printed something that is not JSON.
        junk = _fixture(tmp, "provjunk", stub_body="print('not json')\n")
        ok &= _arm("9 PROVIDER-NOT-JSON", junk, EXIT_COLLAPSE, says="did not print JSON")

        # 10 -- nothing to read at all.
        bare = os.path.join(tmp, "bare")
        os.makedirs(bare, exist_ok=True)
        ok &= _arm("10 NO-DOCUMENTS", bare, EXIT_COLLAPSE, says="no markdown document")

        # 11 + 12 -- THE EXCLUSION, PINNED IN BOTH DIRECTIONS.
        # 11: a drifted claim inside `.worktrees/` is IGNORED (an in-flight lane's
        #     half-edited document must not red this guard) ...
        both = _fixture(tmp, "exclusion")
        wt = os.path.join(both, ".worktrees", "lane", "examples")
        os.makedirs(wt, exist_ok=True)
        _write(os.path.join(wt, "README.md"),
               "# lane copy\n\n<!--census:examples:manifests-->**1**\n")
        ok &= _arm("11 WORKTREE-IGNORED", both, EXIT_OK, says="every documented figure")
        # 12: ... and the SAME drift in the live tree beside it is still CAUGHT, so
        #     the exclusion cannot have widened to swallow the real document.
        _write(os.path.join(both, "examples", "README.md"),
               _read(os.path.join(both, "examples", "README.md")).replace("**788**", "**1**", 1))
        ok &= _arm("12 LIVE-TREE-STILL-CAUGHT", both, EXIT_DISAGREE, says="documented 1, actual 788")

        # 13-15 -- THE QUOTATION ESCAPE, PINNED IN BOTH DIRECTIONS.
        # ⚠ An escape every marker can take disarms the guard
        # ([[feedback-an-escape-every-row-triggers-disarms-the-guard]]), so it is not
        # enough that a quotation is ignored: a LIVE claim beside it must still be
        # caught, and a quotation must still have its key checked.
        q = _fixture(tmp, "quoted")
        qd = _doc(q)
        # 13: a deliberately WRONG figure behind a quotation is IGNORED ...
        _write(qd, _read(qd) + "\n\nDocumenting the syntax: "
                               "<!--census-quoted:examples:manifests-->**1** is how a claim "
                               "is written.\n")
        ok &= _arm("13 QUOTATION-IGNORED", q, EXIT_OK, says="1 quotation")
        # 14: ... but a quotation naming a key that does not exist still REDS, so a typo
        #     cannot hide behind the escape.
        _write(qd, _read(qd).replace("census-quoted:examples:manifests",
                                     "census-quoted:examples:manifestz", 1))
        ok &= _arm("14 QUOTATION-KEY-STILL-CHECKED", q, EXIT_COLLAPSE, says="does not report")
        _write(qd, _read(qd).replace("census-quoted:examples:manifestz",
                                     "census-quoted:examples:manifests", 1))
        # 15: ... and a LIVE claim in the same document is still caught.
        _write(qd, _read(qd).replace("<!--census:examples:manifests-->**788**",
                                     "<!--census:examples:manifests-->**634**", 1))
        ok &= _arm("15 LIVE-CLAIM-BESIDE-QUOTATION", q, EXIT_DISAGREE,
                   says="documented 634, actual 788")

    print("check-doc-census --selftest: %s" % ("PASS" if ok else "FAIL"))
    return EXIT_OK if ok else EXIT_DISAGREE


# ── ENTRY ──────────────────────────────────────────────────────────────────────

def main(argv):
    repo = SELF_REPO
    rest, i = [], 0
    while i < len(argv):
        a = argv[i]
        if a == "--repo":
            if i + 1 >= len(argv):
                print("check-doc-census: --repo needs a path", file=sys.stderr)
                return EXIT_USAGE
            repo = os.path.realpath(argv[i + 1])
            i += 2
            continue
        rest.append(a)
        i += 1

    unknown = [a for a in rest if a not in ("--write", "--selftest")]
    if unknown:
        print("check-doc-census: unknown argument(s): %s" % " ".join(unknown), file=sys.stderr)
        print(__doc__.strip().splitlines()[-4], file=sys.stderr)
        return EXIT_USAGE

    if "--selftest" in rest:
        return selftest()

    if not os.path.isdir(repo):
        print("check-doc-census: not a directory: %s" % repo, file=sys.stderr)
        return EXIT_USAGE

    try:
        return run(repo, write="--write" in rest)
    except Collapse as e:
        print("check-doc-census: FAIL (structural) -- %s" % e, file=sys.stderr)
        return EXIT_COLLAPSE


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
