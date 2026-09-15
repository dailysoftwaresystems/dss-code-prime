#!/usr/bin/env python3
"""source-census.py -- the `source:` census provider for `check-doc-census`.

★★★ WHY THIS EXISTS, and it is this repository's own measured failure rather than
a tidiness rule.

`check-doc-census` shipped in P59 able to check a figure written in a MARKDOWN
document. ✔MEASURED 2026-09-07 (cycle P63), three sites in one cycle, by three
different people: a figure written in a SOURCE COMMENT has no such check and goes
stale exactly the same way. `src/program/compile_pipeline.hpp` carried SEVEN false
figures at once -- an exported-symbol count documented as 21 against a live 28,
"EIGHTEEN exported functions" against 24, "three exported structs" against 4 --
and `src/link/format/macho.cpp` carried two stale `ncmds` enumerations.

★★ THE PART THAT DECIDES THE SHAPE OF THIS FILE. That `compile_pipeline.hpp` block
is ITSELF a paragraph-long meditation on going stale: its author noticed the first
spelling of the instrument counted its own comment, re-anchored the pattern to the
start of a line, and wrote down why. The re-anchored instrument then went stale by
six anyway. **Care did not fix it, and care will not fix it.** Nothing machine-checks
a figure written in a source comment, so every such figure decays --
[[D-TEST-CMAKE-COMMENT-QUOTES-A-CORPUS-COUNT-THE-TEST-IT-REGISTERS-FORBIDS]] states
the general form: *a count in a comment is a measurement with no instrument
attached*, so it decays silently in the direction of looking authoritative.

⇒ ★★★ THE PATTERN IS DATA, NEVER PROSE. The obvious repair -- write the grep beside
the number -- fails for the same reason the number does: a pattern quoted in a
comment is prose, and prose rots. So `source-census.json` owns the pattern, keyed by
the census key, and the comment carries only the marker and the figure. The reader
re-derives with a command; `ctest` re-derives on every run.

THE CONTRACT, and every clause is a way a source figure could lie:
  1. the declaration parses, and declares a NON-EMPTY `keys` object -- an empty
     census agrees with any document that says zero;
  2. every key declares `files` (repo-relative glob patterns) and `matchingLines`
     (a regex), both non-empty, and nothing else it does not understand;
  3. every `files` pattern MATCHES AT LEAST ONE FILE. A glob that silently stops
     matching -- a directory renamed, a file moved -- reports 0, and a comment that
     had rotted to 0 would then AGREE with it. That is the fails-toward-clean
     direction this project refuses in a guard, so it is a refusal;
  4. no pattern escapes the repository (absolute, or climbing out through `..`) --
     a census whose subject is outside the tree is not a census of this tree;
  5. the regex compiles. A broken pattern must not degrade to "matched nothing".

Any clause that fails exits NON-ZERO, which `check-doc-census` reads as a provider
collapse and reports rather than comparing anything against it.

Exit codes: 0 OK -- 2 the declaration or the scan collapsed -- 3 usage.

Usage:
    python scripts/check-doc-census/source-census.py --json    # {key: count}
    python scripts/check-doc-census/source-census.py           # human-readable
    python scripts/check-doc-census/source-census.py --repo <p>   # another tree
"""
from __future__ import annotations

import glob
import io
import json
import os
import re
import sys

# See `check-doc-census.py`'s note: on Windows both streams come up `cp1252` under a
# pipe, which is exactly how ctest runs this, and a report naming a tree path with a
# non-ASCII glyph then dies inside its own `print`.
# D-GATE-PYTHON-GUARD-DIES-PRINTING-TREE-TEXT-ON-A-WINDOWS-PIPE
for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError, OSError):   # pragma: no cover - odd stream
        pass

EXIT_OK, EXIT_COLLAPSE, EXIT_USAGE = 0, 2, 3

# The tree censused defaults to the one THIS SCRIPT LIVES IN, never the caller's cwd --
# the same rule `check-doc-census.py` follows, and for the same reason: an instrument
# that silently measures whichever directory it was launched from can be made to say
# anything by cd-ing somewhere else.
SELF_REPO = os.path.realpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))

# ★ The declaration sits beside this file, not inside it, so adding a checked figure
#   is a data edit. It is resolved against `--repo` so the self-test controls what is
#   at that path without this file growing a test-only branch.
DECLARATION = os.path.join("scripts", "check-doc-census", "source-census.json")

# What a key may declare. Anything else is a typo that would otherwise be ignored
# silently -- and an ignored `matchingLine` (no `s`) is a key that counts nothing.
KEY_FIELDS = {"files", "matchingLines", "$comment"}


class Collapse(Exception):
    """The declaration or the scan failed structurally. Never reported as zero."""


def load_declaration(repo):
    path = os.path.join(repo, DECLARATION)
    try:
        text = io.open(path, encoding="utf-8").read()
    except OSError as e:
        raise Collapse("the source-census declaration could not be read (%s): %s"
                       % (e, path))
    try:
        doc = json.loads(text)
    except ValueError as e:
        raise Collapse("the source-census declaration is not JSON (%s): %s" % (e, path))
    if not isinstance(doc, dict):
        raise Collapse("the source-census declaration must be a JSON object: %s" % path)
    keys = doc.get("keys")
    # Clause 1. A census with nothing in it agrees with any document that says zero,
    # and a document agreeing with zeroes is the worst possible pass.
    if not isinstance(keys, dict) or not keys:
        raise Collapse(
            "the source-census declaration has an EMPTY `keys` object -- a census that "
            "reports nothing agrees with any figure that says zero, so it is refused "
            "rather than trusted: %s" % path)
    return keys


def resolve(repo, key, patterns):
    """Every pattern's matches, de-duplicated, in a stable order."""
    found = []
    for pattern in patterns:
        if not isinstance(pattern, str) or not pattern:
            raise Collapse("key '%s': a `files` pattern must be a non-empty string" % key)
        if os.path.isabs(pattern) or pattern.startswith("~"):
            raise Collapse("key '%s': the `files` pattern '%s' is absolute -- a census "
                           "of this tree may not name a path outside it" % (key, pattern))
        native = pattern.replace("/", os.sep)
        hits = [os.path.realpath(p)
                for p in glob.glob(os.path.join(repo, native), recursive=True)
                if os.path.isfile(p)]
        # Clause 4, checked on the RESOLVED path so a `..` buried mid-pattern cannot
        # slip past a prefix test on the pattern text.
        root = os.path.realpath(repo) + os.sep
        for h in hits:
            if not h.startswith(root):
                raise Collapse("key '%s': the `files` pattern '%s' resolved OUTSIDE the "
                               "tree (%s)" % (key, pattern, h))
        # Clause 3. A glob that stopped matching reports zero, and a rotted figure of
        # zero would agree with it.
        if not hits:
            raise Collapse(
                "key '%s': the `files` pattern '%s' matched NO file. A pattern that has "
                "stopped matching reports 0 lines, and a figure that has rotted to 0 "
                "would then AGREE with it -- fix the pattern, never accept the zero."
                % (key, pattern))
        found.extend(hits)
    return sorted(set(found))


def count_key(repo, key, spec):
    if not isinstance(spec, dict):
        raise Collapse("key '%s': the declaration must be a JSON object" % key)
    unknown = sorted(set(spec) - KEY_FIELDS)
    if unknown:
        # A misspelled field is a key that counts something other than what its author
        # wrote down, and silence about it is how that becomes permanent.
        raise Collapse("key '%s': unknown field(s) %s -- a key declares exactly %s"
                       % (key, ", ".join(unknown), ", ".join(sorted(KEY_FIELDS))))
    patterns = spec.get("files")
    if not isinstance(patterns, list) or not patterns:
        raise Collapse("key '%s': `files` must be a non-empty list of glob patterns" % key)
    source = spec.get("matchingLines")
    if not isinstance(source, str) or not source:
        raise Collapse("key '%s': `matchingLines` must be a non-empty regex string" % key)
    try:
        pattern = re.compile(source)
    except re.error as e:
        # Clause 5. A pattern that does not compile must never degrade to "matched
        # nothing" -- that is a zero with the shape of an answer.
        raise Collapse("key '%s': `matchingLines` is not a valid regex (%s): %s"
                       % (key, e, source))

    total = 0
    for path in resolve(repo, key, patterns):
        text = io.open(path, encoding="utf-8", errors="replace").read()
        for line in text.splitlines():
            if pattern.search(line):
                total += 1
    return total


def census(repo):
    return {key: count_key(repo, key, spec)
            for key, spec in sorted(load_declaration(repo).items())}


def main(argv):
    repo, rest, i = SELF_REPO, [], 0
    while i < len(argv):
        if argv[i] == "--repo":
            if i + 1 >= len(argv):
                print("source-census: --repo needs a path", file=sys.stderr)
                return EXIT_USAGE
            repo = os.path.realpath(argv[i + 1])
            i += 2
            continue
        rest.append(argv[i])
        i += 1

    unknown = [a for a in rest if a != "--json"]
    if unknown:
        print("source-census: unknown argument(s): %s" % " ".join(unknown), file=sys.stderr)
        return EXIT_USAGE
    if not os.path.isdir(repo):
        print("source-census: not a directory: %s" % repo, file=sys.stderr)
        return EXIT_USAGE

    try:
        values = census(repo)
    except Collapse as e:
        print("source-census: FAIL (structural) -- %s" % e, file=sys.stderr)
        return EXIT_COLLAPSE

    if "--json" in rest:
        print(json.dumps(values, indent=2, sort_keys=True))
    else:
        for key in sorted(values):
            print("%-40s %d" % (key, values[key]))
    return EXIT_OK


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
