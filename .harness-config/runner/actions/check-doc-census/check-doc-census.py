#!/usr/bin/env python3
# PURPOSE: refuse a documented figure a census refutes, or an unpinned quantified claim about the corpus in config prose, and repair figures in place.
"""check-doc-census.py -- THE DOCUMENTED-FIGURE GUARD.

★★★ WHY THIS EXISTS, and it is this repository's own measured failure rather
than a tidiness rule.

`.harness-config/runner/actions/examples-census/examples-census.py` was written precisely so nobody
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

    ... over the <!--census-quoted:examples:manifests-->**788** manifests ...

The marker is an HTML comment, so it is invisible in every rendered view, and it
binds to the number that immediately follows it. Adding a checked figure needs
no change to this file; deleting one is caught by the floor. Any scanned file in
the tree may carry a claim -- there is no registration step, because a
registration step is the thing that gets forgotten.

★★★ AND A SOURCE COMMENT IS A DOCUMENT. ✔MEASURED 2026-09-07 (cycle P63), three
sites in ONE cycle by three different people: `src/program/compile_pipeline.hpp`
carried SEVEN false figures at once -- an exported-symbol count, a count of
exported functions, a count of exported structs, a call-site tally, a driver call
counted as one that happens zero times, a route count and a corpus percentage --
and `src/link/format/macho.cpp` carried two stale `ncmds` enumerations. ⚠ THE LIVE
VALUES ARE DELIBERATELY NOT WRITTEN HERE. They belong to that header, they move,
and a guard's own docstring restating them would be this very defect one directory
over -- the first draft of this paragraph did exactly that, and the figure it
inherited from its handover was already wrong by one when it was written down.
★★ The part that decides this widening: that `compile_pipeline.hpp`
block is ITSELF a paragraph-long meditation on going stale -- its author noticed
the first spelling of the instrument counted its own comment, re-anchored the
pattern to the start of a line, and wrote down why. The re-anchored instrument
then went stale by six anyway. **Care did not fix it, and care will not fix it.**
⇒ The scan reads SOURCE as well as prose (`SCANNED_SUFFIXES`), and the marker
grammar needed no change at all -- an HTML comment is legal text inside a `//` or
a `/* */` comment, so

    // ★ <!--census-quoted:source:opt.mirRebuildPolicies-->11 policies drive one
    // rebuilder, so a fatal that cannot name its pass is unattributable.

is checked by the same six clauses that check a README. A second, C-shaped marker
grammar would have been a parallel vocabulary for one concept, which is the slow
break [[feedback_reuse_pipeline_verbs_across_languages]] names.

⚠⚠ WIDENING THE SCAN MADE THIS FILE A SUBJECT, AND THAT IS PAID FOR HERE RATHER
THAN ESCAPED. ✔MEASURED at the widening: this source carried ELEVEN literal
markers -- one illustrating the syntax above, ten inside the self-test where they
are FIXTURE TEXT carrying deliberately WRONG figures. By the grammar those ARE
claims, so the guard refused its own source, and it was right to. The illustration
above now takes `census-quoted:` (the sanctioned escape, whose provider and key are
still checked); the self-test COMPOSES its markers from parts through `_mark()`, so
the fixture text this file builds is no longer a claim this repository never meant
to make. ★ Neither is a blanket exemption for `.py`: arm 20 asserts a live marker in
a Python file IS scanned and IS caught, so the widening cannot quietly stop at `.md`.

★ WHY A PROVIDER PREFIX (`examples:`). The key namespace belongs to the
instrument that owns it, so a second census can be added as a row in PROVIDERS
without touching the marker grammar or re-interpreting any existing claim. It was
spelled from the first day, when there was only one provider, because a namespace
retro-fitted onto unnamespaced keys is a migration -- and P63's `source:` provider
cashed that in: it joined as ONE `PROVIDERS` row and re-interpreted no existing
claim. ⓘ This paragraph used to end "today there is exactly one provider", which
the change that added the second made false in the same commit; the count is gone
rather than corrected, because `PROVIDERS` is right there and can be counted.

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
  6. at least one document was read at all.

⚠ THE EXCLUSIONS ARE PART OF THE CONTRACT AND ARE TESTED AS SUCH. `.worktrees/`
holds full checkouts of this repository while lanes are in flight; scanning them
would red this guard on another lane's half-finished document, and widening the
exclusion until the red stops is how an exclusion silently swallows the real
tree. Self-test arm 11 asserts a drifted claim inside `.worktrees/` is IGNORED
and arm 12 asserts one in the live tree beside it is still CAUGHT, so the
exclusion is pinned in BOTH directions -- see
`[[feedback-an-escape-every-row-triggers-disarms-the-guard]]`.

★★★ THE SECOND CLAUSE: A QUANTIFIED CLAIM ABOUT THE CORPUS, IN CONFIG PROSE. A figure
is one way a document states a measurement; a QUANTIFIER is the other -- "sqlite uses
ONLY X", "the corpus NEVER spells Y", "shell.c references NOTHING from Z". Each is a
count of zero or one with the number left out, so it rots exactly like a figure when
the corpus moves -- and worse, because a config document writes it as the REASON for
what a descriptor ships: a stale one does not merely misinform, it closes the audit
that would have found the gap. The row's instance (e): `shippedLibs/sys/ioctl.json`
said SQLite did not use that header's request macros, so none shipped, while
os_unix.c used `_IOWR` -- and the macho build carried the miss as a parse death.
⇒ In a config document's `$` prose (every string under a `$`-prefixed key of a
`src/dss-config/**/*.json`), a sentence that puts a CORPUS name, a USAGE verb and an
exclusivity or absence QUANTIFIER within `CLAIM_ROT_WINDOW` words of each other takes
one of two forms, or is not written:
  * PINNED -- it carries its measurement: a MEASURED/verified word AND a date
    (YYYY-MM-DD) or a revision hash in the same sentence, so its age is visible and
    re-running it is mechanical;
  * HISTORY -- it quotes a retracted claim inside its own correction ("this comment
    used to say ...", "the pre-c92 comment asserted '...'", RETRACTED).
Anything else is a DECISION phrased as a measurement: state the decision, or measure
it. There is no `--write` for prose.
✔MEASURED 2026-09-24, when the clause landed (P68 round 11): 85 config documents held
32 such sentences -- 29 refused, 1 pinned, 2 history -- and the 29 were rewritten in
the same change, because a guard the tree fails is never shipped. ⓘ The predicate is
a CUT, not a parser: a usage word right after an article is a noun ("the
reference"), a multi-word quantifier ("exactly four", "no other", "doesn't use")
sits at its first word, and the window is 8 words -- widening it to 10 on the same
tree added three sentences, and in each the quantifier was a literal zero belonging
to another clause (a value, a call argument, an array bound).

Exit codes: 0 OK - 1 a document disagrees with the census - 2 the scan or a
provider collapsed (structural: fix the scan, never lower the floor) - 3 usage.

Usage:
    python .harness-config/runner/actions/check-doc-census/check-doc-census.py             # verify
    python .harness-config/runner/actions/check-doc-census/check-doc-census.py --write     # repair figures
    python .harness-config/runner/actions/check-doc-census/check-doc-census.py --selftest  # prove it fails
    python .harness-config/runner/actions/check-doc-census/check-doc-census.py --repo <p>  # act on another tree
"""
from __future__ import annotations

import importlib.util
import io
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
sys.dont_write_bytecode = True  # a by-path load must not write __pycache__ beside another action (the rule: check-scripts-index)

# ── OUTPUT ENCODING -- NOT COSMETIC ────────────────────────────────────────────
# ✔MEASURED 2026-08-23 (CPython, Windows, BOTH streams PIPES, which is exactly how
# ctest runs every guard): `sys.stdout` comes up `encoding='cp1252'`. A report
# printed on stdout that names a document containing a non-ASCII glyph then raises
# `UnicodeEncodeError` and kills the guard INSIDE ITS OWN REPORT -- the run still
# reds, but the finding is lost and the traceback names a `print`. Applied at
# IMPORT so every path this module can print on is covered.
for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError, OSError):   # pragma: no cover - odd stream
        pass

EXIT_OK, EXIT_DISAGREE, EXIT_COLLAPSE, EXIT_USAGE = 0, 1, 2, 3
# The repair a refusal names: the `write` manual step of this action (check-doc-census.yml), run
# through the harness like every other step -- never this file started by hand -- on the runner that
# runs it on ONE leg, this machine's own tree (config.json `check-doc-census-write`): this action's own
# runner keeps both local legs, and a `--manual-step write` through it would rewrite the WSL leg's
# synced copy too (P68 round 13's audit, F1-A11).
REPAIR_VERB = "dssharness run check-doc-census-write"

# The tree acted on defaults to the one THIS SCRIPT LIVES IN, never the caller's
# cwd -- the same rule `.harness-config/runner/actions/lane-worktree/` follows, and for the same reason:
# a guard that silently measures whichever directory it was launched from is a
# guard that can be made green by cd-ing somewhere else.
def self_repo():
    """The tree THIS FILE lives in, by the owner's walk -- never a count of `..`.

    ★ ASKED LAZILY, only when no `--repo` names the tree. It was a module-level
    `__file__/../..`, which named `.harness-config/runner` the moment this file moved out
    of `scripts/` (2026-09-18) -- the depth-hardcoding defect `owning-tree` exists to end.
    Lazy because the self-test runs COPIES of the providers under synthetic roots with
    `--repo`, and a copy has no sibling owner to load.
    """
    mod = _owning_tree_module()
    try:
        return mod.owning_tree(__file__)
    except mod.Refusal as exc:
        print("check-doc-census: %s" % exc, file=sys.stderr)
        raise SystemExit(EXIT_COLLAPSE)


def _owning_tree_module():
    """`owning-tree`, loaded by path from this file's sibling action directory; missing, a collapse."""
    path = os.path.join(os.path.dirname(os.path.dirname(os.path.realpath(__file__))),
                        "owning-tree", "owning-tree.py")
    if not os.path.isfile(path):
        print("check-doc-census: cannot find %s -- this guard's tree is resolved there and "
              "nowhere else" % path, file=sys.stderr)
        raise SystemExit(EXIT_COLLAPSE)
    spec = importlib.util.spec_from_file_location("dss_owning_tree", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def remedy_runner_fact():
    """-> (ok, detail): the runner REPAIR_VERB names is declared in this tree's config.json, runs this action's
    `write` step ALONE, on ONE leg whose definition names no other host -- this machine's own tree (P68 round 13's
    audit, F1-A11: the remedy named the two-leg runner, whose `--manual-step write` rewrote the WSL leg's copy
    too). Read from config, never assumed: a second leg added there reds the self-test."""
    ot = _owning_tree_module()
    cfg = ot.load_jsonc(os.path.join(self_repo(), ".harness-config", "config.json"))
    name = REPAIR_VERB.split()[-1]
    runner = (cfg.get("predefinedRunners") or {}).get(name) if isinstance(cfg, dict) else None
    legs = (runner.get("legs") or []) if isinstance(runner, dict) else []
    leg = (cfg.get("legs") or {}).get(legs[0]) if len(legs) == 1 else None
    others = [k for k in (cfg.get("hosts") or {}) if k != "local" and isinstance(leg, dict) and k in leg]
    ok = (isinstance(runner, dict) and runner.get("action") == "check-doc-census/check-doc-census.yml"
          and runner.get("steps") == ["write"] and isinstance(leg, dict) and not others)
    return ok, "runner %r: %r; its leg names another host: %r" % (name, runner, others)


# Where this repository's programs live, relative to a tree root: DssHarness's actions
# directory (`dssharness help layout`). Spelled ONCE in this file.
ACTIONS_REL = os.path.join(".harness-config", "runner", "actions")

# Each provider is the ARGV of a command, relative to the repo root, that prints a
# flat {key: int} JSON object on stdout. Resolved against `--repo`, so the
# self-test controls what is at that path without this file growing a test-only
# branch.
PROVIDERS = {
    "examples": (os.path.join(ACTIONS_REL, "examples-census", "examples-census.py"), "--json"),
    # ★ THE PROVIDER THAT ANSWERS A SOURCE-CODE QUESTION. Its keys are declared in
    #   `source-census.json` beside it, each naming a file set and the regex whose
    #   matching lines it counts -- so the PATTERN is data and never re-typed in the
    #   comment it checks. A pattern quoted in prose rots exactly like the number.
    "source": (os.path.join(ACTIONS_REL, "check-doc-census", "source-census.py"), "--json"),
}

# ── WHAT IS SCANNED, AND IT IS A DECLARATION RATHER THAN A CONDITION ───────────
# The set is spelled here, once, so widening it is a data edit and a reader can see
# the whole subject at a glance. It was `f.lower().endswith(".md")` buried in the
# walk until 2026-09-07 (P63), which is a policy written where nobody looks for one.
# ⚠ `.txt` is here for `CMakeLists.txt`, which carries counted claims in comments
#   like every other build document. ⚠ `.json` is here because this project's config
#   documents argue in `$comment` prose and those arguments carry figures too.
# ⚠ WHAT IS DELIBERATELY OUT, because an omission with no reason is an escape:
#   expected-OUTPUT fixtures (`.diag`, `.tree`, `.dsshir`, `.jsonl`) and binaries.
#   Those are compared byte-for-byte against a producer, so a marker in one is part
#   of an EXPECTATION rather than a claim -- and `--write` repairing a figure inside
#   an expected output would silently move the thing the test measures.
# ✔MEASURED 2026-09-07 at the widening, and stated as the EXISTENCE claim rather
#   than as file counts that would rot the way this guard exists to stop: **NO file
#   this set excludes carries a census marker.** Re-derive by walking the same skip
#   set and searching `CLAIM` in every file whose suffix is NOT in this tuple -- the
#   answer must stay zero, and a non-zero is a suffix missing from this line rather
#   than a reason to leave it out. The walk is not this guard's cost: the whole tree
#   read and scanned in well under a second, against a provider run of seconds.
SCANNED_SUFFIXES = (".md", ".c", ".h", ".cpp", ".hpp", ".inc", ".s", ".py", ".sh",
                    ".ps1", ".cmake", ".txt", ".json", ".yml", ".yaml")

# Directories never scanned. `.worktrees` holds in-flight lane checkouts of this
# same repository (see the header); the rest are build output and secrets.
SKIP_DIRS = {".git", ".worktrees", ".secrets", "node_modules", "__pycache__"}
SKIP_DIR_PREFIXES = ("build",)

# ⚠⚠ AND SO IS EVERY DIRECTORY `.gitignore` DECLARES, FOR THE SAME REASON `.worktrees`
# IS. ✔MEASURED 2026-09-05 (P62): this guard walked `.temp/` -- 176 markdown files of
# session scratch -- and one of them was a lane's working COPY of `examples/README.md`,
# so the run reported 14 phantom divergences against a file that is not a document of
# this project, and `--write` would have EDITED THAT COPY to "repair" it. The class is
# identical to the `census-quoted:` story above: a guard biting a document it has no
# business reading. It had always done this; it only became visible when a scratch file
# happened to carry the marker.
# ★ DECLARATION-DRIVEN, NOT A SECOND HAND-KEPT LIST. The set is read from `.gitignore`'s
#   directory-only entries, so adding a scratch home to the ignore file is the whole
#   edit and the two cannot drift. A hardcoded twin of `.temp`, `scratchpad`,
#   `test-scratch`, ... is exactly the duplicate that goes stale on the next one.
# ★ NAME-KEYED, MATCHING THE HARD LIST'S SEMANTICS: `.temp/` skips a directory called
#   `.temp` at any depth, which is what os.walk pruning can express. A path-anchored
#   ignore (`/src/dss-config/runtime/platform/dist/`) contributes its LAST segment
#   only; over-skipping a same-named directory elsewhere is not a risk this guard runs,
#   because every such entry names build output.
# ⚠ It DEGRADES TO THE HARD LIST when there is no `.gitignore` -- the self-test's
#   synthetic roots rely on that, and arm 24 makes the degradation itself observable.
def scratch_dirs(repo):
    """Directory names `.gitignore` declares, so scratch is never read as a document."""
    out = set()
    try:
        text = io.open(os.path.join(repo, ".gitignore"), encoding="utf-8",
                       errors="replace").read()
    except OSError:
        return out
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith("#") or line.startswith("!"):
            continue
        if not line.endswith("/"):
            continue                      # a file pattern, not a directory home
        name = line.rstrip("/").rstrip("/").split("/")[-1]
        if name and "*" not in name and "?" not in name:
            out.add(name)
    return out

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
# forced to avoid writing it down. (`.harness-config/runner/actions/check-line-endings/` carries a
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

def scanned_docs(repo):
    """Every file the tree may carry a claim in, excluding the homes listed above."""
    out = []
    skip = SKIP_DIRS | scratch_dirs(repo)
    for root, dirs, files in os.walk(repo):
        dirs[:] = sorted(d for d in dirs
                         if d not in skip and not d.startswith(SKIP_DIR_PREFIXES))
        for f in sorted(files):
            if f.lower().endswith(SCANNED_SUFFIXES):
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


# ── THE SECOND CLAUSE: A QUANTIFIED CLAIM ABOUT THE CORPUS, IN CONFIG PROSE ────────
# The header says why. This is the predicate, spelled once; every word class is a
# DECLARATION a reader can audit, and the self-test pins each allowed form in both
# directions plus the window's edge.
CLAIM_ROT_ROOT = os.path.join("src", "dss-config")
CLAIM_ROT_WINDOW = 8        # the corpus word, the usage verb and the quantifier within this many words
CLAIM_ROT_DOC_FLOOR = 1     # a walk that read no config document is a COLLAPSE, never a clean pass

_CR_WORD = re.compile(r"[\w'`./<>*-]+")
_CR_USAGE = re.compile(r"^(uses?|used|using|calls?|called|references?|referenced|needs?|needed|"
                       r"touch(?:es|ed)?|reach(?:es|ed)?|consumes?|consumed|spells?|spelled)$", re.I)
_CR_QUANT_1 = re.compile(r"^(only|solely|exclusively|nothing|none|never|zero|neither|0)$", re.I)
_CR_QUANT_2 = re.compile(r"\b(exactly (?:one|two|three|four|five|six|seven|eight|nine|ten|\d+)|no other|"
                         r"no (?:consumer|caller|user|use|site|reference)s?|(?:does|do)(?:n't| not) "
                         r"(?:use|call|reference|need|touch|reach|spell|include)|the (?:sole|whole) consumer)\b",
                         re.I)
_CR_CORPUS = re.compile(r"^(sqlite\w*|amalgamation|testfixture|tclsqlite\w*|os_unix\.c|os_win\.c|shell\.c|"
                        r"sqlite3\.c|test\d+\.c|mem\d\.c|corpus)$", re.I)
_CR_MEASURED = re.compile(r"✔\s*(?:RE-)?MEASURED|\bMEASURED\b|re-measured|grep-verified|\bverified\b", re.I)
_CR_PINNED = re.compile(r"\b(?:\d{4}-\d{2}-\d{2}|[0-9a-f]{8,40})\b")
_CR_ARTICLES = {"the", "a", "an", "this", "that", "each", "every", "its", "their", "one"}
_CR_HISTORY = re.compile(r"\b(?:note|hand-?off|handover|brief)\b[^.]{0,40}\bsaid\b|used to (?:say|read|assert)|"
                         r"previously read|\bIT READ\b|pre-\w+ comment|RETRACTED|WAS FALSE|asserted '|"
                         r"earlier revision|It previously|this comment used to", re.I)
_CR_SENTENCE = re.compile(r"(?<=[.!?])(?<!e\.g\.)(?<!i\.e\.)(?<!etc\.)(?<!vs\.)(?<!cf\.)\s+")


def claim_rot_verdict(sentence, window=CLAIM_ROT_WINDOW):
    """None (not a quantified corpus-usage claim), 'PINNED', 'HISTORY' or 'REFUSE'."""
    toks = [m.group(0).strip(".,;:()[]{}\"'") for m in _CR_WORD.finditer(sentence)]
    # A usage word right after an article is a NOUN ("the reference resolves"), not a
    # claim about the corpus.
    usage = [i for i, t in enumerate(toks) if _CR_USAGE.match(t)
             and not (i and toks[i - 1].lower() in _CR_ARTICLES)]
    corpus = [i for i, t in enumerate(toks) if _CR_CORPUS.match(t)]
    quant = [i for i, t in enumerate(toks) if _CR_QUANT_1.match(t)]
    for m in _CR_QUANT_2.finditer(sentence):          # a multi-word quantifier sits at its first word
        quant.append(len(_CR_WORD.findall(sentence[:m.start()])))
    if not (usage and quant and corpus):
        return None
    if not any(max(a, b, c) - min(a, b, c) <= window for a in usage for b in quant for c in corpus):
        return None
    if _CR_HISTORY.search(sentence):
        return "HISTORY"
    if _CR_MEASURED.search(sentence) and _CR_PINNED.search(sentence):
        return "PINNED"
    return "REFUSE"


def _dollar_strings(node, in_dollar=False, path=""):
    """Every string under a `$`-prefixed key -- a config document's prose -- with its JSON pointer."""
    if isinstance(node, dict):
        for k, v in node.items():
            yield from _dollar_strings(v, in_dollar or k.startswith("$"), "%s/%s" % (path, k))
    elif isinstance(node, list):
        for i, v in enumerate(node):
            yield from _dollar_strings(v, in_dollar, "%s/%d" % (path, i))
    elif isinstance(node, str) and in_dollar:
        yield path, node


def claim_rot_findings(repo):
    """-> (config documents read, `$` sentences read, [(rel, json pointer, sentence)] refused)."""
    root = os.path.join(repo, CLAIM_ROT_ROOT)
    docs = sents = 0
    refused = []
    skip = SKIP_DIRS | scratch_dirs(repo)
    for dp, dn, fn in os.walk(root):
        dn[:] = sorted(d for d in dn if d not in skip)
        for f in sorted(fn):
            if not f.endswith(".json"):
                continue
            p = os.path.join(dp, f)
            rel = os.path.relpath(p, repo).replace(os.sep, "/")
            try:
                doc = json.load(io.open(p, encoding="utf-8"))
            except ValueError as e:
                raise Collapse("%s is not JSON (%s) -- its prose cannot be read, so it cannot be "
                               "judged" % (rel, e))
            docs += 1
            for pointer, text in _dollar_strings(doc):
                for s in _CR_SENTENCE.split(text):
                    sents += 1
                    if claim_rot_verdict(s) == "REFUSE":
                        refused.append((rel, pointer, s.strip()))
    if docs < CLAIM_ROT_DOC_FLOOR:
        raise Collapse("the claim-rot clause read %d config document(s) under %s -- a walk that "
                       "reads nothing is a structural failure, not a pass"
                       % (docs, CLAIM_ROT_ROOT.replace(os.sep, "/")))
    return docs, sents, refused


# ── THE GUARD ──────────────────────────────────────────────────────────────────

def run(repo, write):
    docs = scanned_docs(repo)
    if not docs:
        raise Collapse("no scannable document was found under %s (suffixes: %s) -- the "
                       "scan read nothing, which is a structural failure, not a pass."
                       % (repo, " ".join(SCANNED_SUFFIXES)))

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
            "the scan COLLAPSED: found only %d documented figure(s) across %d scanned "
            "document(s), floor is %d. Deleting a census marker removes a figure from "
            "this guard's sight; fix the scan or restore the markers, never lower the "
            "floor." % (len(claims), len(docs), CLAIM_FLOOR))

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

    # THE SECOND CLAUSE is read BEFORE anything is written, so its structural collapse
    # (no config document read) stops a --write exactly as it stops a verify.
    rot_docs, rot_sents, refused = claim_rot_findings(repo)

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

    rc = EXIT_OK
    if wrong and not write:
        print("check-doc-census: FAIL -- %d documented figure(s) the census refutes:"
              % len(wrong))
        for c, actual in wrong:
            print("    %s  census:%s:%s  documented %s, actual %d"
                  % (c.doc, c.provider, c.key, c.raw, actual))
        print("  Repair them in place (the prose is untouched, only the numbers move):")
        print("      %s" % REPAIR_VERB)
        print("  ⚠ A figure is a DATED INVENTORY. If a SENTENCE around one has also gone "
              "false, --write will not notice -- read the claim, not only the number.")
        rc = EXIT_DISAGREE
    elif wrong:
        print("check-doc-census: repaired %d figure(s). Re-run to verify." % len(wrong))

    # The second clause reports in BOTH modes: `--write` repairs figures, never prose, so
    # a refused sentence keeps a --write red too.
    if refused:
        print("check-doc-census: FAIL -- %d quantified claim(s) about the corpus's usage in "
              "config prose, neither pinned to a measurement nor quoted as history:" % len(refused))
        for rel, pointer, sentence in refused:
            print("    %s %s\n        %s" % (rel, pointer, sentence[:400]))
        print("  State the DECISION the sentence argues, or pin it: a MEASURED/verified word and "
              "a date (YYYY-MM-DD) or a revision in the SAME sentence. A retracted claim may be "
              "quoted inside its own correction. `--write` does not touch prose.")
        rc = EXIT_DISAGREE
    else:
        print("check-doc-census: %d config document(s), %d `$` sentence(s): no unpinned "
              "corpus-usage claim" % (rot_docs, rot_sents))

    if rc == EXIT_OK and not wrong:
        print("check-doc-census: OK -- every documented figure matches the census.")
    return rc


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
#   The guard resolves `examples` to `.harness-config/runner/actions/examples-census/examples-census.py`
#   under `--repo` and always has; the fixture simply controls what is there. The
#   `source` provider is stubbed the same way at `.harness-config/runner/actions/check-doc-census/`.
#
# ⚠⚠ AND THE MARKERS BELOW ARE COMPOSED, NEVER WRITTEN LITERALLY. Since the scan
#   reads `.py`, a literal `<!--census:...-->` in this file is a CLAIM about this
#   repository, and the ten in this self-test carried deliberately WRONG figures --
#   fixture text the guard would (correctly) refuse. `_mark()` builds them from
#   parts, which is not an exemption for `.py`: arm 20 asserts a live marker in a
#   Python file IS caught, so the widened scan is pinned in both directions.

def _mark(key, provider="examples", quoted=False):
    """A census marker, composed rather than spelled -- see the note above."""
    return "<!--census%s:%s:%s-->" % ("-quoted" if quoted else "", provider, key)


_STUB = """import json, sys
print(json.dumps({%s}))
"""

_STUB_KEYS = {"manifests": 788, "arms": 879, "targets": 2678, "top.source": 757,
              "top.sources": 26, "top.project": 5, "top.targets": 788,
              "arms.passes": 310, "arms.shippedPipeline": 569,
              "arms.mustDifferTrue": 695, "arms.mustDifferFalse": 6,
              "dependsOn.entries": 22}

# The `source` provider's stub key set. `policies` mirrors the live declaration's
# `opt.mirRebuildPolicies`; the value is what the fixture's source comment must state.
_SOURCE_KEYS = {"policies": 11, "exports": 28}


def _stub_for(keys):
    return _STUB % ", ".join("%r: %d" % (k, v) for k, v in sorted(keys.items()))


def _fixture(tmp, name, stub_body=None, doc_body=None, src_body=None,
             source_stub_body=None):
    # ⚠ Each arm gets its OWN root. An earlier draft reused one directory and the
    # later arms silently rebuilt the fixture the earlier ones were still asserting
    # against -- a green that meant nothing.
    root = os.path.join(tmp, name)
    census_dir = os.path.join(root, ACTIONS_REL, "examples-census")
    os.makedirs(census_dir, exist_ok=True)
    body = stub_body if stub_body is not None else _stub_for(_STUB_KEYS)
    io.open(os.path.join(census_dir, "examples-census.py"), "wb").write(body.encode("utf-8"))

    # The `source` provider, stubbed at ITS real path for the same reason.
    source_dir = os.path.join(root, ACTIONS_REL, "check-doc-census")
    os.makedirs(source_dir, exist_ok=True)
    src_stub = source_stub_body if source_stub_body is not None else _stub_for(_SOURCE_KEYS)
    io.open(os.path.join(source_dir, "source-census.py"), "wb").write(src_stub.encode("utf-8"))

    docs = os.path.join(root, "examples")
    os.makedirs(docs, exist_ok=True)
    if doc_body is None:
        doc_body = "# fixture\n\n" + "".join(
            "- key %s: %s**%d** today\n" % (k, _mark(k), v)
            for k, v in sorted(_STUB_KEYS.items()))
    io.open(os.path.join(docs, "README.md"), "wb").write(doc_body.encode("utf-8"))

    # A SOURCE file, only when an arm asks for one, so the markdown arms keep
    # measuring exactly what they measured before this widening.
    if src_body is not None:
        code = os.path.join(root, "src", "opt")
        os.makedirs(code, exist_ok=True)
        io.open(os.path.join(code, "rebuild.hpp"), "wb").write(src_body.encode("utf-8"))

    # The config document THE SECOND CLAUSE reads. EVERY fixture carries one, because the
    # clause refuses a walk that reads none (arm 41); its prose makes no claim, so the
    # figure arms keep measuring exactly what they measured before the clause existed.
    _say(root, _NEUTRAL_PROSE)
    return root


# THE SECOND CLAUSE's fixture: one config document whose `$comment` is the sentence under test.
_FIXTURE_CONFIG = "shippedLibs/fixture.json"
_NEUTRAL_PROSE = "A neutral descriptor note: this header ships two constants, both measured."


def _say(root, sentence, rel=_FIXTURE_CONFIG, doc=None):
    """Write `doc` (default: a descriptor whose `$comment` is `sentence`) at the config `rel`."""
    p = os.path.join(root, CLAIM_ROT_ROOT, *rel.split("/"))
    os.makedirs(os.path.dirname(p), exist_ok=True)
    body = doc if doc is not None else {"$comment": sentence, "header": "fixture.h"}
    _write(p, body if isinstance(body, str) else json.dumps(body, ensure_ascii=False, indent=2))
    return p


def _src(root):
    return os.path.join(root, "src", "opt", "rebuild.hpp")


# The fixture source comment, written the way a real one is: prose the guard must
# not touch, on both sides of a marker and its figure.
_SRC_BODY = ("// ── THE REBUILD SUBSTRATE ─────────────────────────────\n"
             "// ★ %s**11** concrete policies drive one rebuilder, so a\n"
             "// fatal that cannot name its pass is unattributable.\n"
             "#pragma once\n") % _mark("policies", provider="source")


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
        # 1b -- the refusal names the REPAIR as the harness runs it (the `write` manual step).
        ok &= _arm("1b REMEDY-IS-THE-HARNESS-STEP", root, EXIT_DISAGREE, says=REPAIR_VERB)
        # 1c -- ...and the runner it names runs that step alone, on ONE leg of this machine's own tree (F1-A11).
        _fact_ok, _fact_detail = remedy_runner_fact()
        print("  %-34s %s" % ("1c REMEDY-RUNNER-IS-ONE-LEG", "OK" if _fact_ok else "FAIL"))
        if not _fact_ok:
            print("      %s" % _fact_detail[:600])
        ok &= _fact_ok

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
        _write(d, _read(d).replace(_mark("targets") + "**2678**",
                                   _mark("targets") + " lots", 1))
        ok &= _arm("5 NO-NUMBER-AFTER-MARKER", root, EXIT_COLLAPSE, says="not followed by a number")
        _write(d, _read(d).replace(_mark("targets") + " lots",
                                   _mark("targets") + "**2678**", 1))

        # 6 -- the cheapest way to go green: delete the markers. Refused.
        few = _fixture(tmp, "thin",
                       doc_body="# thin\n\n%s**788**\n" % _mark("manifests"))
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
        ok &= _arm("10 NO-DOCUMENTS", bare, EXIT_COLLAPSE, says="no scannable document")

        # 11 + 12 -- THE EXCLUSION, PINNED IN BOTH DIRECTIONS.
        # 11: a drifted claim inside `.worktrees/` is IGNORED (an in-flight lane's
        #     half-edited document must not red this guard) ...
        both = _fixture(tmp, "exclusion")
        wt = os.path.join(both, ".worktrees", "lane", "examples")
        os.makedirs(wt, exist_ok=True)
        _write(os.path.join(wt, "README.md"),
               "# lane copy\n\n%s**1**\n" % _mark("manifests"))
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
        _write(qd, _read(qd) + "\n\nDocumenting the syntax: %s**1** is how a claim "
                               "is written.\n" % _mark("manifests", quoted=True))
        ok &= _arm("13 QUOTATION-IGNORED", q, EXIT_OK, says="1 quotation")
        # 14: ... but a quotation naming a key that does not exist still REDS, so a typo
        #     cannot hide behind the escape.
        _write(qd, _read(qd).replace("census-quoted:examples:manifests",
                                     "census-quoted:examples:manifestz", 1))
        ok &= _arm("14 QUOTATION-KEY-STILL-CHECKED", q, EXIT_COLLAPSE, says="does not report")
        _write(qd, _read(qd).replace("census-quoted:examples:manifestz",
                                     "census-quoted:examples:manifests", 1))
        # 15: ... and a LIVE claim in the same document is still caught.
        _write(qd, _read(qd).replace(_mark("manifests") + "**788**",
                                     _mark("manifests") + "**634**", 1))
        ok &= _arm("15 LIVE-CLAIM-BESIDE-QUOTATION", q, EXIT_DISAGREE,
                   says="documented 634, actual 788")

        # 16-19 -- THE `.gitignore`-DECLARED SCRATCH HOMES, PINNED IN EVERY DIRECTION.
        # ⚠ THE FIXTURE SYNTHESIZES THE *NEGATIVE*: the drifted scratch document is
        #   written FIRST and arm 16 proves it REDS without the declaration, so arms 17
        #   and 18 are measuring the skip rather than an empty directory. An arm that
        #   only ADDED the ignore line would stay green if the skip did nothing at all.
        sc = _fixture(tmp, "scratch")
        st = os.path.join(sc, ".temp", "p62-lane-scratch")
        os.makedirs(st, exist_ok=True)
        _write(os.path.join(st, "examples_README.md"),
               "# a lane's working COPY\n\n%s**1**\n" % _mark("manifests"))
        # 16: with NO `.gitignore`, `.temp` is not declared scratch and the copy REDS --
        #     this is the state P62 actually found, and the arm that makes it a defect.
        ok &= _arm("16 SCRATCH-UNDECLARED-REDS", sc, EXIT_DISAGREE,
                   says="documented 1, actual 788")
        # 17: declaring it in `.gitignore` -- the ONLY edit -- silences it.
        _write(os.path.join(sc, ".gitignore"), "*.obj\n.temp/\nbuild/\n")
        ok &= _arm("17 SCRATCH-DECLARED-IGNORED", sc, EXIT_OK, says="every documented figure")
        # 18: ... and the SAME drift in the live tree beside it is still CAUGHT, so the
        #     new exclusion cannot have widened to swallow the real document.
        _write(_doc(sc), _read(_doc(sc)).replace("**788**", "**1**", 1))
        ok &= _arm("18 LIVE-TREE-STILL-CAUGHT-BESIDE-SCRATCH", sc, EXIT_DISAGREE,
                   says="documented 1, actual 788")
        # 19: a FILE pattern is not a directory home. `.temp` written without its
        #     trailing slash must NOT skip anything -- otherwise `*.md`-style entries
        #     could silence arbitrary documents, and the escape would be unbounded.
        _write(_doc(sc), _read(_doc(sc)).replace("**1**", "**788**", 1))
        _write(os.path.join(sc, ".gitignore"), "*.obj\n.temp\nbuild/\n")
        ok &= _arm("19 FILE-PATTERN-DOES-NOT-SKIP", sc, EXIT_DISAGREE,
                   says="documented 1, actual 788")

        # ══ 20-27 -- THE WIDENED SCAN: A SOURCE COMMENT IS A DOCUMENT ═════════════
        # ⚠ THE FIXTURE SYNTHESIZES THE *NEGATIVE* HERE TOO. Arm 20 breaks the source
        #   figure FIRST and asserts the refusal; only then does 21 assert the corrected
        #   file passes. An ADD-direction pair -- write an agreeing source file, watch it
        #   stay green -- would stay green if `SCANNED_SUFFIXES` lost every source
        #   suffix, which is precisely the regression these arms exist to catch.
        #   See [[feedback-a-fixture-must-synthesize-the-negative]].
        src = _fixture(tmp, "source", src_body=_SRC_BODY)
        sp = _src(src)
        # 20: a STALE figure in a `.hpp` comment REDS. This is the whole widening: at
        #     HEAD before P63 the scan read `.md` only, so this file was invisible.
        _write(sp, _read(sp).replace("**11**", "**9**", 1))
        ok &= _arm("20 SOURCE-FIGURE-STALE-REDS", src, EXIT_DISAGREE,
                   says="documented 9, actual 11")
        # 21: ... and the corrected file passes. The control for arm 20 -- without it a
        #     red proves only that something in the fixture broke.
        _write(sp, _read(sp).replace("**9**", "**11**", 1))
        ok &= _arm("21 SOURCE-FIGURE-CORRECT-PASSES", src, EXIT_OK,
                   says="every documented figure")

        # 22 -- `--write` repairs a SOURCE figure and leaves the prose around it alone,
        #       byte for byte. A guard that reflows a comment while fixing a number is
        #       editing more than it was asked to, and a source comment is where that
        #       would be least forgivable.
        rep = _fixture(tmp, "sourcewrite", src_body=_SRC_BODY)
        rp = _src(rep)
        _write(rp, _read(rp).replace("**11**", "**9**", 1))
        _arm("22-setup", rep, EXIT_DISAGREE)
        _arm("22-write", rep, EXIT_OK, write=True)
        after = _read(rp)
        prose_kept = (after == _SRC_BODY)
        print("  %-34s %s" % ("22 WRITE-REPAIRS-SOURCE",
                              "OK" if prose_kept else "FAIL"))
        if not prose_kept:
            print("      got: %r" % after)
        ok &= prose_kept

        # 23 -- a marker in a SOURCE file naming an unknown PROVIDER reds. The
        #       vocabulary check is not a markdown privilege.
        _write(sp, _read(sp).replace("census:source:policies", "census:corpus:policies", 1))
        ok &= _arm("23 SOURCE-UNKNOWN-PROVIDER", src, EXIT_COLLAPSE,
                   says="unknown census provider")
        _write(sp, _read(sp).replace("census:corpus:policies", "census:source:policies", 1))
        # 24 -- ... and an unknown KEY reds too. A typo in a comment must never read as
        #       agreement, which is the one thing a silently-skipped marker would do.
        _write(sp, _read(sp).replace("census:source:policies", "census:source:policiez", 1))
        ok &= _arm("24 SOURCE-UNKNOWN-KEY", src, EXIT_COLLAPSE, says="does not report")
        _write(sp, _read(sp).replace("census:source:policiez", "census:source:policies", 1))

        # 25 -- THE FLOOR STILL REFUSES DELETION, and now it must do so for a source
        #       file: deleting the comment's marker is still the cheapest green.
        thin = _fixture(tmp, "sourcethin", doc_body="# thin\n", src_body=_SRC_BODY)
        ok &= _arm("25 SOURCE-FLOOR-COLLAPSE", thin, EXIT_COLLAPSE,
                   says="floor is %d" % CLAIM_FLOOR)

        # 26 + 27 -- THE `source` PROVIDER'S OWN REFUSALS, run against the REAL
        # `source-census.py` (not a stub) over a synthetic declaration. Both are the
        # fails-toward-clean direction: a census that reports nothing, and a glob that
        # has stopped matching, each produce a tidy zero that a rotted figure agrees with.
        ok &= _source_provider_arms(tmp)

        # 28-42 -- THE SECOND CLAUSE.
        ok &= _claim_rot_arms(tmp)

    print("check-doc-census --selftest: %s" % ("PASS" if ok else "FAIL"))
    return EXIT_OK if ok else EXIT_DISAGREE


# The sentence every claim-rot arm varies: an exclusivity claim about the corpus's usage.
_CR_BARE = "sqlite uses only `_IOWR` from this header."


def _rot(label, root, want_refused, says=None):
    """One IN-PROCESS arm of the second clause: `claim_rot_findings` -- the function the
    guard itself calls -- over the fixture root. `want_refused` is True (a refusal), False
    (a clean walk) or a Collapse message fragment."""
    try:
        docs, _sents, refused = claim_rot_findings(root)
        got = "refused %d of %d document(s)" % (len(refused), docs)
        good = (bool(refused) == want_refused) if isinstance(want_refused, bool) else False
        text = " ".join("%s %s %s" % r for r in refused)
    except Collapse as e:
        got = "COLLAPSE"
        good = isinstance(want_refused, str) and want_refused in str(e)
        text = str(e)
    if good and says is not None:
        good = says in text
    print("  %-34s %s %s" % (label, got, "OK" if good else "FAIL"))
    if not good:
        print("      says: %s" % text[:900])
    return good


def _claim_rot_arms(tmp):
    """Arms 28-42: the second clause, each allowed form pinned in BOTH directions.

    ⚠ THE NEGATIVE COMES FIRST (arm 28): the bare claim must RED before any arm may show
    a variant of it passing -- otherwise a clause that never ran would pass every green
    arm below. ★ THREE ARMS RUN THE GUARD END TO END (28 red, 29 green, 34 the row's own
    instance) and assert its exit code and its report; the predicate's other edges call
    `claim_rot_findings`, the function the guard calls, in process -- the same code, at a
    fraction of the cost of a subprocess per sentence (a guard's seconds are gate seconds).
    """
    ok = True
    root = _fixture(tmp, "claimrot")

    # 28 -- the bare claim REDS end to end, naming the document and the JSON pointer.
    _say(root, _CR_BARE)
    ok &= _arm("28 CLAIM-UNPINNED-REDS", root, EXIT_DISAGREE,
               says="%s/%s /$comment" % (CLAIM_ROT_ROOT.replace(os.sep, "/"), _FIXTURE_CONFIG))
    # 29 + 30 -- PINNED passes: a MEASURED word and a date (end to end, asserting the
    #            clause's own success line), or a MEASURED word and a revision.
    _say(root, _CR_BARE[:-1] + " (✔MEASURED 2026-09-24 over the staged tree).")
    ok &= _arm("29 CLAIM-DATE-PIN-PASSES", root, EXIT_OK, says="no unpinned corpus-usage claim")
    _say(root, _CR_BARE[:-1] + " (✔MEASURED at sqlite f544d3599a10).")
    ok &= _rot("30 CLAIM-REVISION-PIN-PASSES", root, False)
    # 31 + 32 -- ... and HALF a pin is no pin: the MEASURED word alone, or the date alone.
    _say(root, _CR_BARE[:-1] + " (✔MEASURED over the staged tree).")
    ok &= _rot("31 CLAIM-MEASURED-WORD-ALONE-REDS", root, True)
    _say(root, _CR_BARE[:-1] + " (as of 2026-09-24).")
    ok &= _rot("32 CLAIM-DATE-ALONE-REDS", root, True)
    # 33 -- HISTORY passes: arm 28's sentence, verbatim, inside its own correction.
    _say(root, "This comment used to say " + _CR_BARE[:-1] + "; os_unix.c reaches `_IO` and `_IOR` too.")
    ok &= _rot("33 CLAIM-HISTORY-PASSES", root, False)

    # 34 -- THE ROW'S INSTANCE (e), RE-PLANTED VERBATIM where it lived, end to end. The
    #       sentence that shipped no request macro while os_unix.c used `_IOWR` reds today.
    _say(root, _NEUTRAL_PROSE)
    _say(root, "the _IOC/_IOR/_IOW request-ENCODING macros differ per-OS, but SQLite doesn't use "
               "this header's macros, so none are shipped (no over-ship).", rel="shippedLibs/sys/ioctl.json")
    ok &= _arm("34 INSTANCE-E-REPLANTED-REDS", root, EXIT_DISAGREE, says="shippedLibs/sys/ioctl.json")
    os.remove(os.path.join(root, CLAIM_ROT_ROOT, "shippedLibs", "sys", "ioctl.json"))

    # 35 + 36 -- a usage word after an article is a NOUN, and that rule is not an escape:
    #            the same sentence with the VERB reds.
    _say(root, "Only the reference compilers decide this layout; sqlite is never the reference here.")
    ok &= _rot("35 USAGE-NOUN-PASSES", root, False)
    _say(root, "Only the reference compilers decide this layout; sqlite never references it.")
    ok &= _rot("36 USAGE-VERB-REDS", root, True)

    # 37 + 38 -- prose is every string under a `$` key, at any depth, and ONLY there.
    _say(root, None, doc={"$comment": _NEUTRAL_PROSE, "header": _CR_BARE})
    ok &= _rot("37 NON-DOLLAR-VALUE-PASSES", root, False)
    _say(root, None, doc={"$comment": _NEUTRAL_PROSE, "$notes": ["fine.", {"why": _CR_BARE}]})
    ok &= _rot("38 DOLLAR-SUBTREE-REDS", root, True, says="/$notes/1/why")

    # 39 + 40 -- THE WINDOW'S EDGE: corpus word and usage verb at 0 and 1, the quantifier
    #            at CLAIM_ROT_WINDOW (red) and one past it (clean).
    fill = ["alpha", "beta", "gamma", "delta", "epsilon", "zeta", "eta", "theta", "iota"]
    edge = CLAIM_ROT_WINDOW - 2
    _say(root, "sqlite uses %s only." % " ".join(fill[:edge]))
    ok &= _rot("39 WINDOW-EDGE-REDS", root, True)
    _say(root, "sqlite uses %s only." % " ".join(fill[:edge + 1]))
    ok &= _rot("40 WINDOW-PAST-EDGE-PASSES", root, False)

    # 41 + 42 -- the clause's own collapses: no config document read, and one it cannot read.
    shutil.rmtree(os.path.join(root, CLAIM_ROT_ROOT))
    ok &= _rot("41 NO-CONFIG-COLLAPSES", root, "read 0 config document(s)")
    _say(root, None, doc='{"$comment": "a truncated document"')
    ok &= _rot("42 CONFIG-NOT-JSON-COLLAPSES", root, "is not JSON")
    return ok


def _source_provider_arms(tmp):
    """Arms 26-27: the `source` provider refuses a zero it cannot vouch for.

    ⚠ These run the SHIPPED `source-census.py`, copied to its real path under a
    synthetic root, so what is measured is the resolution that ships rather than a
    re-implementation. The provider reads its declaration from `--repo`, so the
    fixture controls the declaration without the provider growing a test-only branch.
    """
    ok = True
    real = os.path.join(os.path.dirname(os.path.abspath(__file__)), "source-census.py")

    def run_provider(name, declaration, expect_rc, says):
        root = os.path.join(tmp, "srccensus-" + name)
        here = os.path.join(root, ACTIONS_REL, "check-doc-census")
        os.makedirs(here, exist_ok=True)
        _write(os.path.join(here, "source-census.py"), _read(real))
        _write(os.path.join(here, "source-census.json"), declaration)
        code = os.path.join(root, "src")
        os.makedirs(code, exist_ok=True)
        _write(os.path.join(code, "a.cpp"), "class P : public MirRebuildPolicy {};\n")
        p = subprocess.run([sys.executable, os.path.join(here, "source-census.py"),
                            "--json", "--repo", root],
                           capture_output=True, text=True, encoding="utf-8",
                           errors="replace")
        out = (p.stdout or "") + (p.stderr or "")
        good = p.returncode == expect_rc and says in out
        print("  %-34s rc=%d (want %d) %s"
              % (name, p.returncode, expect_rc, "OK" if good else "FAIL"))
        if not good:
            print("      says: %s" % out.strip().replace("\n", "\n      ")[:600])
        return good

    # 26 -- the CONTROL first: a declaration that resolves must report the real count,
    #       so arm 27's red cannot be "the provider is broken".
    ok &= run_provider(
        "26 PROVIDER-CONTROL-COUNTS",
        json.dumps({"keys": {"policies": {"files": ["src/**/*.cpp"],
                                          "matchingLines": "public\\s+MirRebuildPolicy"}}}),
        EXIT_OK, '"policies": 1')
    # 27 -- a `files` pattern that has stopped matching. It would report 0, and a figure
    #       that had rotted to 0 would AGREE with it. Refused instead.
    ok &= run_provider(
        "27 PROVIDER-GLOB-MATCHES-NOTHING",
        json.dumps({"keys": {"policies": {"files": ["src/renamed/**/*.cpp"],
                                          "matchingLines": "public\\s+MirRebuildPolicy"}}}),
        EXIT_COLLAPSE, "matched NO file")
    return ok


# ── ENTRY ──────────────────────────────────────────────────────────────────────

def main(argv):
    repo = None
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

    if repo is None:
        repo = self_repo()
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
