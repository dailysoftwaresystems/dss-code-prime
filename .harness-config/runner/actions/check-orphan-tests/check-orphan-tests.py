#!/usr/bin/env python3
# PURPOSE: refuse a test source that no CMake target compiles and no ctest entry runs.
"""check-orphan-tests.py -- the ORPHAN TEST SOURCE guard.

ONE program on every host. It replaces a `.sh` / `.ps1` pair that carried the same
contract, the same allowlist and the same floors twice; where the two disagreed this file
takes the stricter reading, and every such choice is listed under DECISIONS below.

THE CONTRACT. Every `*.cpp` under the tests root is either REGISTERED by a structural
`dss_add_test(NAME ... SOURCES ...)` call in some `CMakeLists.txt` under that root, or
named by a row of `allowlist.json` (beside this file) whose reference THIS GUARD VERIFIES.

WHY IT EXISTS. An orphan is a test source no CMakeLists names: it compiles nowhere, no
ctest entry runs it, and not one of its assertions ever executes -- while it sits in the
tree reading like a test. A missing test at least looks missing; this is absent coverage
wearing the appearance of coverage.

THE PARSE IS STRUCTURAL, NOT A GREP. A loose regex over CMakeLists text counts the
function definition, its usage docblock and prose comments as registrations, in the
direction that greens the guard. So:
  * comments are stripped the way CMake strips them: a `#` outside a quoted argument
    opens a line comment, `#[[ ... ]]` / `#[==[ ... ]==]` a BRACKET comment that may span
    lines, and a backslash escapes the next character (an escaped quote does not toggle
    the quoted state, an escaped `#` does not open a comment);
  * the call is `dss_add_test` as a whole word followed by `(` -- matched without regard
    to ASCII case, because CMake command names are case-insensitive -- and its arguments
    are read to the MATCHING `)`, so a multi-line call parses like a single-line one;
  * arguments split on spaces and are walked as the two keywords the real helper declares
    through `cmake_parse_arguments` (`NAME` one value, `SOURCES` many), compared EXACTLY,
    as `cmake_parse_arguments` compares them;
  * a source token has `${CMAKE_CURRENT_SOURCE_DIR}` expanded and `.`/`..` collapsed
    against its CMakeLists' directory; anything else it cannot place is a refusal (E1 a
    CMake variable, E2 an absolute path, E3 a path escaping the root), as are a call with
    no NAME (E4), a call registering nothing (E5) and an unterminated call (E6).

THE ALLOWLIST lives once, in `allowlist.json`, as rows `{source, site, reason}` (paths
relative to the tests root). Every row is machine-checked on every run: the source still
EXISTS; `dss_add_test` does NOT register it (else the exemption is false); the SITE still
exists; and the site STRUCTURALLY references the source (a `.cpp` token in its
comment-stripped text). A row missing a field, with an EMPTY field, with an unknown key,
or repeating a source is a COLLAPSE and clears nothing -- such a row can be checked by
nothing, and both predecessors let a row with an empty column clear its source anyway.

FAIL-CLOSED. The root must exist; three floors (sources, CMakeLists, DISTINCT registered
sources) catch a collapsed scan -- the last is the one that fails quietest; a registration
naming a file that does not exist (PHANTOM) is a collapse; a directory the walk cannot
list, a CMakeLists that cannot be read or decoded, or any crash of this program is a
collapse (2), never 1, which means ORPHAN.

EXIT CODES: 0 clean · 1 ORPHAN · 2 SCAN COLLAPSED (including a failed self-test) ·
3 STALE ALLOWLIST. Every class is reported in one run; the exit carries the highest
(2 > 1 > 3).

USAGE (the tree is the one this file lives in, never the caller's cwd):
    check-orphan-tests.py            scan <tree>/tests, then run the self-test
    check-orphan-tests.py <root>     scan <root> only (the self-test's children use this)

THE SELF-TEST runs on every no-argument (ctest) run whose live verdict is GREEN, against
a MIRROR in a per-run temp directory (every CMakeLists copied byte for byte, every source
created empty -- this guard never opens a `.cpp`). The twelve inherited arms re-invoke
this program as a child and read its EXIT CODE, because the exit code is the contract
ctest consumes; the arms added with this port drive the production census in process.
Every restore is byte-verified, every expectation is captured once before any arm runs,
and the arm count is checked against `EXPECTED_ARMS`.

DECISIONS
  * A RED live verdict (1, 2 or 3) skips the self-test and exits with the live code: a red
    tree cannot seed the green mirror the arms mutate, and running it anyway reported a
    real orphan as a broken guard (exit 2, "fix the guard"). The red is itself the proof
    that the guard fails for that class.
  * Arm 6 empties the densest CMakeLists (it no longer renames them away) and predicts the
    EXACT distinct registration count the child must report, so the CMakeLists floor can
    never make the arm infeasible on a healthy tree.
  * The comment rules above go beyond both predecessors (CMake's own rules); bracket
    ARGUMENTS (`[[...]]` not preceded by `#`) and parentheses inside quotes are still read
    as the predecessors read them.
  * A CMakeLists must be valid UTF-8 (a leading BOM is dropped, as CMake drops it); an
    undecodable one is a collapse naming the file.
  * More than one argument, or an empty one, is a usage error (2).
"""
from __future__ import annotations

import importlib.util
import json
import os
import posixpath
import re
import subprocess
import sys
import tempfile
import traceback

# A Windows pipe comes up cp1252 and dies printing a glyph; reconfigure BEFORE anything can
# print (`guard_output_encoding_guard` imports this module in a child to probe exactly
# this). Nothing else runs at import.
for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8", errors="replace", line_buffering=True)
    except (AttributeError, ValueError, OSError):  # pragma: no cover - an odd stream
        pass

EXIT_OK, EXIT_ORPHAN, EXIT_COLLAPSE, EXIT_STALE = 0, 1, 2, 3

# Floors far below the live figures: they catch a COLLAPSED scan, not drift. If one fires,
# fix the scan; never lower the floor.
SOURCE_FLOOR = 150
CMAKELISTS_FLOOR = 12
REGISTRATION_FLOOR = 150
PHANTOM_SHOWN = 20

ALLOWLIST_FILE = "allowlist.json"
ALLOWLIST_KEYS = ("source", "site", "reason")
ALLOWLIST_SHOWN = ".harness-config/runner/actions/check-orphan-tests/allowlist.json"

EXPECTED_ARMS = 40

CALL_RX = re.compile(r"dss_add_test[ \t]*\(", re.ASCII | re.IGNORECASE)
WORD_CHAR_RX = re.compile(r"[A-Za-z0-9_]")
DRIVE_RX = re.compile(r"[A-Za-z]:")
SPACES_RX = re.compile(r" +")
DIGITS_RX = re.compile(r"[0-9]+")
SUMMARY_RX = re.compile(r"orphan-tests: (?:OK|FAIL) - ")
BRACKET_OPEN_RX = re.compile(r"\[(=*)\[")
_OUTSIDE_RX = re.compile(r"[\"#\\]")
_INSIDE_RX = re.compile(r"[\"\\]")


class Collapse(Exception):
    """A structural failure: never reported as a pass."""


# ── the tree this file lives in ────────────────────────────────────────────────────────

_OWNING_TREE = None


def _owning_tree():
    """`owning-tree/owning-tree.py`, loaded as a SIBLING by path; a missing owner is a collapse."""
    global _OWNING_TREE
    if _OWNING_TREE is None:
        here = os.path.dirname(os.path.realpath(__file__))
        path = os.path.join(os.path.dirname(here), "owning-tree", "owning-tree.py")
        if not os.path.isfile(path):
            raise Collapse("cannot load the tree owner beside %s" % here)
        sys.dont_write_bytecode = True          # nothing of this run lands in the tree
        spec = importlib.util.spec_from_file_location("dss_owning_tree", path)
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        _OWNING_TREE = mod
    return _OWNING_TREE


def repo_root():
    """The DSS tree THIS FILE lives in -- never the caller's cwd. Raises Collapse."""
    ot = _owning_tree()
    try:
        return ot.resolve(__file__)
    except ot.Refusal as exc:
        raise Collapse("no DSS tree contains %s -- %s"
                       % (os.path.dirname(os.path.realpath(__file__)), exc))


# ── the CMake reader ───────────────────────────────────────────────────────────────────

def decode_cmakelists(data, shown):
    """Strict UTF-8, one leading BOM dropped (CMake drops it too). Undecodable -> Collapse."""
    try:
        text = data.decode("utf-8")
    except UnicodeDecodeError as exc:
        raise Collapse("%s is not valid UTF-8 (%s)" % (shown, exc))
    return text[1:] if text.startswith("\ufeff") else text


def lines_of(text):
    """Split on `\\n` ONLY; one trailing CR dropped; the final newline opens no line."""
    parts = text.split("\n")
    if parts and parts[-1] == "":
        parts.pop()
    return [p[:-1] if p.endswith("\r") else p for p in parts]


class _StripState:
    __slots__ = ("quote", "bracket")

    def __init__(self):
        self.quote, self.bracket = False, ""


def strip_line(line, st):
    """One line with its comments removed; the quote and bracket-comment state is carried
    across lines in `st` (reset per file)."""
    out = []
    i, n = 0, len(line)
    while i < n:
        if st.bracket:
            j = line.find(st.bracket, i)
            if j < 0:
                return "".join(out)
            i, st.bracket = j + len(st.bracket), ""
            continue
        if st.quote:
            j = i
            while True:
                m = _INSIDE_RX.search(line, j)
                if not m or line[m.start()] == '"':
                    break
                j = m.start() + 2               # an escape: the next character is content
            if not m:
                out.append(line[i:])
                return "".join(out)
            out.append(line[i:m.end()])
            i, st.quote = m.end(), False
            continue
        m = _OUTSIDE_RX.search(line, i)
        if not m:
            out.append(line[i:])
            return "".join(out)
        k, c = m.start(), line[m.start()]
        if c == "\\":
            out.append(line[i:k + 2])
            i = k + 2
        elif c == '"':
            out.append(line[i:k + 1])
            i, st.quote = k + 1, True
        else:                                   # '#': a bracket comment, or the rest of the line
            out.append(line[i:k])
            b = BRACKET_OPEN_RX.match(line, k + 1)
            if not b:
                return "".join(out)
            i, st.bracket = b.end(), "]" + b.group(1) + "]"
    return "".join(out)


def dir_of(path):
    return path.rsplit("/", 1)[0] if "/" in path else ""


def norm_path(directory, token):
    """`directory/token` with `.` and `..` collapsed; "" when it climbs out of the root."""
    full = token if directory == "" else directory + "/" + token
    stack = []
    for part in full.split("/"):
        if part in ("", "."):
            continue
        if part == "..":
            if not stack:
                return ""
            stack.pop()
            continue
        stack.append(part)
    return "/".join(stack)


def resolve_token(fname, directory, tok, quiet, errs):
    """One source token -> a tests-root-relative path, or "" with an E1/E2/E3 message
    (suppressed when `quiet`: the structural-reference pass can only cost a row its proof)."""
    t = tok.replace('"', "").replace("${CMAKE_CURRENT_SOURCE_DIR}", ".")
    if "${" in t:
        if not quiet:
            errs.append("%s: SOURCES token %s carries a CMake variable this guard cannot resolve, "
                        "so the parse is INCOMPLETE. Teach the parser to expand it, or use a "
                        "literal path; do not let a token go unread." % (fname, tok))
        return ""
    if t.startswith("/") or DRIVE_RX.match(t):
        if not quiet:
            errs.append("%s: SOURCES token %s is an ABSOLUTE path, so it cannot be placed in the "
                        "tests-root-relative scan set." % (fname, tok))
        return ""
    p = norm_path(directory, t)
    if p == "" and not quiet:
        errs.append("%s: SOURCES token %s resolves OUTSIDE the tests root, so this guard cannot "
                    "account for it." % (fname, tok))
    return p


class FileParse:
    __slots__ = ("regs", "toks", "errs", "calls")

    def __init__(self):
        self.regs, self.toks, self.errs, self.calls = [], [], [], 0


def parse_call_args(fname, directory, argtext, fp):
    """The arguments of one call, walked as `cmake_parse_arguments` walks them."""
    mode, name, nsrc = "", "", 0
    for t in SPACES_RX.split(argtext.replace("\t", " ").replace("\n", " ")):
        if t == "":
            continue
        if t == "NAME" or t == "SOURCES":
            mode = t
            continue
        if mode == "NAME":
            name, mode = t, ""
            continue
        if mode == "SOURCES":
            p = resolve_token(fname, directory, t, False, fp.errs)
            if p:
                fp.regs.append((p, name, fname))
                nsrc += 1
    fp.calls += 1
    if name == "":
        fp.errs.append("%s: a dss_add_test call declares no NAME." % fname)
    if nsrc == 0:
        fp.errs.append("%s: dss_add_test(NAME %s) registers no resolvable source." % (fname, name))


def parse_file(fname, text):
    """-> FileParse: registrations (path, name, file), structural `.cpp` references
    (file, path), refusals, and the number of calls read."""
    fp = FileParse()
    st = _StripState()
    stripped = "".join(strip_line(line, st) + "\n" for line in lines_of(text))
    directory = dir_of(fname)
    pos = 0
    while True:
        m = CALL_RX.search(stripped, pos)
        if not m:
            break
        s, e = m.start(), m.end()
        if s > 0 and WORD_CHAR_RX.match(stripped[s - 1]):
            pos = e                              # the tail of a longer identifier
            continue
        depth, i = 1, e
        while i < len(stripped):
            c = stripped[i]
            if c == "(":
                depth += 1
            elif c == ")":
                depth -= 1
                if depth == 0:
                    break
            i += 1
        if depth != 0:
            fp.errs.append("%s: an unterminated dss_add_test( call - the parse cannot be trusted."
                           % fname)
            break
        parse_call_args(fname, directory, stripped[e:i], fp)
        pos = i + 1
    # Every STRUCTURAL `.cpp` token in the file -- the evidence an allowlist site must carry.
    # `(` and `)` separate tokens here too, or `add_executable(r x.cpp)` would yield `x.cpp)`.
    flat = stripped.replace("\t", " ").replace("\n", " ").replace("(", " ").replace(")", " ")
    for tok in SPACES_RX.split(flat):
        t = tok.replace('"', "")
        if len(t) < 5 or not t.endswith(".cpp"):
            continue
        p = resolve_token(fname, directory, t, True, fp.errs)
        if p:
            fp.toks.append((fname, p))
    return fp


# ── the allowlist ──────────────────────────────────────────────────────────────────────

def allowlist_path():
    return os.path.join(os.path.dirname(os.path.realpath(__file__)), ALLOWLIST_FILE)


def load_allowlist(path, shown=ALLOWLIST_SHOWN):
    """-> ([(source, site, reason)], [refusal]). A row that cannot be checked clears nothing."""
    try:
        with open(path, "rb") as fh:
            data = json.loads(fh.read().decode("utf-8"))
    except (OSError, ValueError) as exc:
        return [], ["orphan-tests: FAIL - the allowlist %s cannot be read (%s)." % (shown, exc)]
    if not isinstance(data, list):
        return [], ["orphan-tests: FAIL - the allowlist %s is not a JSON array of rows." % shown]
    rows, errors, seen = [], [], set()
    for n, row in enumerate(data, 1):
        if not isinstance(row, dict):
            errors.append("orphan-tests: FAIL - allowlist row %d is not an object." % n)
            continue
        unknown = sorted(k for k in row if k not in ALLOWLIST_KEYS)
        absent = [k for k in ALLOWLIST_KEYS if k not in row]
        empty = [k for k in ALLOWLIST_KEYS
                 if k in row and (not isinstance(row[k], str) or row[k].strip() == "")]
        if unknown:
            errors.append("orphan-tests: FAIL - allowlist row %d carries unknown key(s) %s; a row "
                          "holds exactly %s." % (n, ", ".join("'%s'" % k for k in unknown),
                                                 ", ".join(ALLOWLIST_KEYS)))
        if absent:
            errors.append("orphan-tests: FAIL - allowlist row %d is missing the field(s) %s."
                          % (n, ", ".join("'%s'" % k for k in absent)))
        if empty:
            errors.append("orphan-tests: FAIL - allowlist row %d has an EMPTY %s." % (
                n, ", ".join("'%s'" % k for k in empty)))
        if unknown or absent or empty:
            continue
        if row["source"] in seen:
            errors.append("orphan-tests: FAIL - allowlist row %d repeats the source '%s' of an "
                          "earlier row; one source, one row." % (n, row["source"]))
            continue
        seen.add(row["source"])
        rows.append((row["source"], row["site"], row["reason"]))
    if errors:
        errors.append("  A row this guard cannot check would clear its source on nobody's "
                      "authority, so it clears nothing and the census is refused. Fix the row "
                      "in %s; never widen the allowlist to make a red go away." % shown)
    return rows, errors


# ── the census ─────────────────────────────────────────────────────────────────────────

class Census:
    def __init__(self, label):
        self.label = label
        self.root = None             # the physical tests root, once it is known to exist
        self.root_missing = False
        self.sources, self.cmakelists = [], []
        self.regs, self.reg_paths, self.toks, self.errs = [], [], set(), []
        self.calls = 0
        self.collapse_msgs, self.stale_msgs, self.orphans = [], [], []
        self.n_stale = self.n_allow = 0
        self.rc = EXIT_OK

    @property
    def collapsed(self):
        return bool(self.collapse_msgs)


def _walk_error(exc):
    raise Collapse("cannot list %s (%s)" % (getattr(exc, "filename", None) or "?",
                                            getattr(exc, "strerror", None) or exc))


def enumerate_tree(root):
    """-> (sources, cmakelists): every `*.cpp` and every `CMakeLists.txt` (exact-case names,
    regular files or symlinks to one), dot-directories included, ordinal order. A directory
    that cannot be listed raises Collapse."""
    sources, cmakelists = [], []
    for dirpath, dirnames, filenames in os.walk(root, onerror=_walk_error):
        dirnames.sort()
        for fn in filenames:
            if not (fn.endswith(".cpp") or fn == "CMakeLists.txt"):
                continue
            full = os.path.join(dirpath, fn)
            if not os.path.isfile(full):
                continue
            rel = os.path.relpath(full, root).replace(os.sep, "/")
            (sources if fn.endswith(".cpp") else cmakelists).append(rel)
    return sorted(sources), sorted(cmakelists)


def census(root_in, label, allow_rows, allow_errors):
    """The whole verdict over one tests root. It reads the tree and prints nothing."""
    c = Census(label)
    if not os.path.isdir(root_in):
        c.root_missing, c.rc = True, EXIT_COLLAPSE
        return c
    root = c.root = os.path.realpath(root_in)
    try:
        c.sources, c.cmakelists = enumerate_tree(root)
    except Collapse as exc:
        c.collapse_msgs.append("orphan-tests: FAIL - the tests-root walk failed: %s. A partial "
                               "enumeration can neither clear nor accuse a source. Refusing to "
                               "report a verdict." % exc)
        c.rc = EXIT_COLLAPSE
        return c
    if not c.cmakelists:
        c.collapse_msgs.append("orphan-tests: FAIL - found 0 CMakeLists.txt under the tests root, "
                               "below its floor of %d." % CMAKELISTS_FLOOR)
        c.collapse_msgs.append("  Nothing was parsed, so NOTHING is registered. Refusing to report "
                               "a verdict at all; fix the scan, do not lower the floor.")
    for rel in c.cmakelists:
        try:
            with open(os.path.join(root, *rel.split("/")), "rb") as fh:
                fp = parse_file(rel, decode_cmakelists(fh.read(), rel))
        except (OSError, Collapse) as exc:
            c.collapse_msgs.append("orphan-tests: FAIL - the CMakeLists parser failed on %s (%s). "
                                   "A partial parse can only UNDER-count registrations, so it can "
                                   "never be trusted to clear a source. Refusing to report a "
                                   "verdict." % (rel, getattr(exc, "strerror", None) or exc))
            continue
        c.regs.extend(fp.regs)
        c.toks.update(fp.toks)
        c.errs.extend(fp.errs)
        c.calls += fp.calls
    c.reg_paths = sorted(set(p for p, _n, _f in c.regs))
    n_src, n_cml, n_reg = len(c.sources), len(c.cmakelists), len(c.reg_paths)

    if n_src < SOURCE_FLOOR:
        c.collapse_msgs.append("orphan-tests: FAIL - the source scan found only %d *.cpp under the "
                               "tests root, below its floor of %d." % (n_src, SOURCE_FLOOR))
        c.collapse_msgs.append("  This does NOT mean every test is wired - it means the SCAN "
                               "COLLAPSED. Refusing to report a pass; fix the scan, do not lower "
                               "the floor.")
    if n_cml < CMAKELISTS_FLOOR:
        c.collapse_msgs.append("orphan-tests: FAIL - the CMakeLists scan found only %d files, below "
                               "its floor of %d." % (n_cml, CMAKELISTS_FLOOR))
        c.collapse_msgs.append("  A collapsed CMakeLists scan registers nothing, so it cannot clear "
                               "a single source. Refusing to report a pass; fix the scan, do not "
                               "lower the floor.")
    if n_reg < REGISTRATION_FLOOR:
        c.collapse_msgs.append("orphan-tests: FAIL - the parser resolved only %d distinct registered "
                               "sources, below its floor of %d." % (n_reg, REGISTRATION_FLOOR))
        c.collapse_msgs.append("  THIS IS THE DIMENSION THAT FAILS QUIETEST: both enumerations can "
                               "look healthy while the PARSE has stopped matching, and every clear "
                               "then evaporates. Refusing to report a pass; fix the parser, do not "
                               "lower the floor.")
    if c.errs:
        c.collapse_msgs.append("orphan-tests: FAIL - %d CMakeLists token(s) could not be resolved, "
                               "so the parse is INCOMPLETE:" % len(c.errs))
        c.collapse_msgs.extend("    " + e for e in c.errs)

    # PHANTOM: the other direction of the same property -- a registration naming nothing.
    src_set = set(c.sources)
    phantoms = [p for p in c.reg_paths if p not in src_set]
    if phantoms:
        c.collapse_msgs.append("orphan-tests: FAIL - %d registration(s) name a source that does NOT "
                               "exist under the tests root:" % len(phantoms))
        c.collapse_msgs.extend("    PHANTOM: " + p for p in phantoms[:PHANTOM_SHOWN])
        if len(phantoms) > PHANTOM_SHOWN:
            c.collapse_msgs.append("    ... and %d more." % (len(phantoms) - PHANTOM_SHOWN))
        c.collapse_msgs.append("  A registration pointing at nothing clears nobody, so the "
                               "registered set is partly fiction and this guard refuses to clear "
                               "anything from it. Either the source was renamed without its "
                               "CMakeLists, or the parser is reading text that is not a "
                               "registration at all.")

    # The allowlist: a row that failed validation clears nothing; the rest are checked.
    c.collapse_msgs.extend(allow_errors)
    reg_set, cml_set = set(c.reg_paths), set(c.cmakelists)
    c.n_allow = len(set(src for src, _site, _why in allow_rows))
    for ap, asite, _why in allow_rows:
        if ap not in src_set:
            c.stale_msgs.append("  STALE: the allowlist names '%s', which does NOT exist under the "
                                "tests root." % ap)
            c.stale_msgs.append("         An exemption for a deleted file reads like live coverage "
                                "policy and protects nothing. Delete the row.")
        elif ap in reg_set:
            c.stale_msgs.append("  STALE: the allowlist exempts '%s', but dss_add_test now "
                                "REGISTERS it." % ap)
            c.stale_msgs.append("         The real mechanism covers it, so the exemption is a false "
                                "statement about this tree. Delete the row.")
        elif asite not in cml_set:
            c.stale_msgs.append("  STALE: the allowlist says '%s' is referenced by '%s', which does "
                                "not exist." % (ap, asite))
        elif (asite, ap) not in c.toks:
            c.stale_msgs.append("  STALE: '%s' no longer STRUCTURALLY references '%s'." % (asite, ap))
            c.stale_msgs.append("         That reference IS the reason the row exists, so the file "
                                "may now be genuinely orphaned. Re-check it, then either register "
                                "it or correct the row - do not widen the exemption.")
        else:
            continue
        c.n_stale += 1

    # THE PROPERTY: every source is registered or allowlisted.
    cleared = reg_set | set(src for src, _site, _why in allow_rows)
    c.orphans = [s for s in c.sources if s not in cleared]

    c.rc = EXIT_OK
    if c.n_stale:
        c.rc = EXIT_STALE
    if c.orphans:
        c.rc = EXIT_ORPHAN
    if c.collapsed:
        c.rc = EXIT_COLLAPSE
    return c


def render(c):
    """The report, every class in one run, ending in the summary line the arms read."""
    out = ["orphan-tests: root=%s" % c.label]
    if c.root_missing:
        out.append("orphan-tests: FAIL - the tests root does not exist. Refusing to report a pass "
                   "on a scan of nothing.")
        out.append("orphan-tests: FAIL - 0 sources / 0 CMakeLists / 0 registrations from 0 calls / "
                   "0 allowlisted: 0 orphan(s), 0 stale allowlist entries.")
        return out
    out.extend(c.collapse_msgs)
    if c.orphans:
        out.append("orphan-tests: FAIL - %d test source(s) under the tests root are named by NO "
                   "CMakeLists.txt." % len(c.orphans))
        out.append("They compile nowhere and no ctest entry runs them. Every assertion inside them "
                   "is dead")
        out.append("text. This is ABSENT COVERAGE wearing the appearance of coverage: the file is "
                   "there, it")
        out.append("reads like a test, the diff that added it looked like added coverage, and the "
                   "suite is")
        out.append("green because it never had anything to say about them.")
        out.append("")
        out.extend("  ORPHAN: " + o for o in c.orphans)
        out.append("")
        out.append("Fix: either")
        out.append("  (a) REGISTER it - add dss_add_test(NAME <dir>/<stem> SOURCES <file>) to the")
        out.append("      CMakeLists.txt of its own directory, then RUN it. A registration that "
                   "compiles")
        out.append("      is not yet a test that asserts; check that it fails when it should.")
        out.append("  (b) DELETE it, if it was superseded and nobody noticed because nothing ran it.")
        out.append("  (c) if it is genuinely NOT a test source (a runner main, a PCH stub), add a "
                   "row to")
        out.append("      %s, naming the reason AND the" % ALLOWLIST_SHOWN)
        out.append("      CMakeLists that references it. The guard verifies that reference on every "
                   "run, so")
        out.append("      the exemption cannot quietly stop being true.")
        out.append("Do NOT widen this guard to make a red go away. An orphan is the one defect here "
                   "that")
        out.append("costs nothing to fix and everything to leave in place.")
    if c.n_stale:
        out.append("orphan-tests: FAIL - the ALLOWLIST no longer describes this tree:")
        out.append("")
        out.extend(c.stale_msgs)
        out.append("An exemption list that can rot is the pattern this repository has anchored "
                   "twice: a")
        out.append("guard weakened every time it fires ends up asserting nothing. Fix the ROW.")
    verdict = "FAIL" if (c.collapsed or c.orphans or c.n_stale) else "OK"
    out.append("orphan-tests: %s - %d sources / %d CMakeLists / %d registrations from %d calls / "
               "%d allowlisted: %d orphan(s), %d stale allowlist entries."
               % (verdict, len(c.sources), len(c.cmakelists), len(c.reg_paths), c.calls,
                  c.n_allow, len(c.orphans), c.n_stale))
    return out


def summary_number(text, label):
    """A census number read off the summary line BY LABEL, never by position."""
    for line in text.split("\n"):
        if not SUMMARY_RX.match(line):
            continue
        words = SPACES_RX.split(line)
        for i in range(1, len(words)):
            if words[i] == label and DIGITS_RX.fullmatch(words[i - 1]):
                return words[i - 1]
    return ""


# ── the self-test ──────────────────────────────────────────────────────────────────────

def _say(msg):
    print(msg, flush=True)


def _read(path):
    with open(path, "rb") as fh:
        return fh.read()


def _write(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as fh:
        fh.write(data)


def _native(root, rel):
    return os.path.join(root, *rel.split("/"))


def _appended(data, *lines):
    """`data` with `lines` appended, each on a line of its own."""
    if data and not data.endswith(b"\n"):
        data += b"\n"
    return data + b"".join(line.encode("utf-8") + b"\n" for line in lines)


def _without_line(data, number):
    """`data` minus its 1-based line `number`; every other byte untouched."""
    lines = data.split(b"\n")
    del lines[number - 1]
    return b"\n".join(lines)


def select_witness(root, cmakelists):
    """The first single-line `dss_add_test(...)` whose SOLE source token is a plain
    same-directory name occurring EXACTLY ONCE in its file's raw text -- uniqueness is the
    selection criterion, so deleting that one line provably removes every mention.
    -> (file, line number, token, resolved path) or None."""
    for rel in cmakelists:
        raw = decode_cmakelists(_read(_native(root, rel)), rel)
        for n, line in enumerate(raw.split("\n"), 1):
            s = line.lstrip(" \t").rstrip(" \t\r")
            if not (s.startswith("dss_add_test(") and s.endswith(")")):
                continue
            if s.count("(") != 1 or s.count(")") != 1:
                continue
            mode, src, nsrc = "", "", 0
            for t in SPACES_RX.split(s[len("dss_add_test("):-1]):
                if t == "":
                    continue
                if t in ("NAME", "SOURCES"):
                    mode = t
                    continue
                if mode == "NAME":
                    mode = ""
                    continue
                if mode == "SOURCES":
                    src, nsrc = t, nsrc + 1
            if nsrc != 1 or "/" in src or "$" in src or raw.count(src) != 1:
                continue
            d = dir_of(rel)
            return rel, n, src, (d + "/" + src if d else src)
    return None


def _run_child(root):
    """This program, re-invoked on `root` -> (rc, merged output). A guard that cannot
    re-invoke itself FAILS its self-test; it never skips it."""
    if not sys.executable:
        return None, "sys.executable is empty, so this guard cannot re-invoke itself"
    try:
        p = subprocess.run([sys.executable, "-B", os.path.realpath(__file__), root],
                           stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, timeout=600)
    except (OSError, subprocess.TimeoutExpired) as exc:
        return None, "the child could not run: %s" % exc
    return p.returncode, p.stdout.decode("utf-8", "replace")


_CATEGORIES = (
    (("0", "10"), "green controls"), (("1",), "orphan"), (("1b",), "comment-is-not-coverage"),
    (("2", "3"), "whole-root collapse"), (("4", "5", "6"), "per-floor collapse"),
    (("7", "8", "9"), "stale-allowlist"), (("11",), "phantom"), (("12",), "stale-site"),
)
_PREFIX_CATEGORIES = (("P", "parser-semantics"), ("E", "parse-incomplete"),
                      ("X", "precedence"), ("A", "allowlist-schema"), ("C", "crash"),
                      ("U", "usage"), ("R", "owning-tree root"), ("T", "temp-cleanup"))


def _category(label):
    head = label.split(" ", 1)[0]
    for heads, name in _CATEGORIES:
        if head in heads:
            return name
    for prefix, name in _PREFIX_CATEGORIES:
        if head.startswith(prefix):
            return name
    return "other"


class _Arms:
    """Every arm is counted once by label; a failed assertion fails its arm, never the run."""

    def __init__(self):
        self.ran, self.failed = [], []

    def begin(self, label):
        self.ran.append(label)

    def fail(self, label, *lines):
        if label not in self.failed:
            self.failed.append(label)
        for i, line in enumerate(lines):
            _say(("orphan-tests: SELF-TEST FAIL - arm %s " % label if i == 0 else "  ") + line)
        return False

    def holds(self, label, ok, detail):
        self.begin(label)
        if ok:
            _say("orphan-tests: self-test arm %s holds" % label)
            return True
        return self.fail(label, detail)

    def breakdown(self):
        order, count = [], {}
        for label in self.ran:
            cat = _category(label)
            if cat not in count:
                order.append(cat)
                count[cat] = 0
            count[cat] += 1
        return ", ".join("%d %s" % (count[c], c) for c in order)


class _Subject:
    """The mirror, its pristine CMakeLists-only twin and an empty root, with the live census
    figures captured ONCE, before any arm runs, under names no arm writes."""

    def __init__(self, base, live, allow_rows):
        self.base, self.live, self.allow_rows = base, live, allow_rows
        self.mirror = os.path.join(base, "mirror")
        self.pristine = os.path.join(base, "pristine")
        self.empty = os.path.join(base, "empty")
        for d in (self.mirror, self.pristine, self.empty):
            os.makedirs(d)
        self.src, self.cml, self.reg = len(live.sources), len(live.cmakelists), len(live.reg_paths)
        for rel in live.sources:
            _write(_native(self.mirror, rel), b"")
        self.bytes = {}
        for rel in live.cmakelists:
            self.bytes[rel] = _read(_native(live.root, rel))
            _write(_native(self.mirror, rel), self.bytes[rel])
            _write(_native(self.pristine, rel), self.bytes[rel])

    def m(self, rel):
        return _native(self.mirror, rel)

    def restore(self, rel):
        """Copy the pristine bytes back and PROVE they match."""
        _write(self.m(rel), self.bytes[rel])
        return _read(self.m(rel)) == self.bytes[rel]


def _child_arm(arms, label, root, want, note="", says=(), silent=(), census_want=None):
    """One inherited arm: re-invoke this program on `root` and judge what ctest would see."""
    arms.begin(label)
    rc, text = _run_child(root)
    if rc != want:
        kind = ("an arm built to stay GREEN reddened, so the subject is not faithful and no red "
                "below means anything." if want == EXIT_OK else
                "an arm built to red did not red, so a green run of it says nothing.")
        return arms.fail(label, "exited %s, expected %d. This guard CANNOT be" % (rc, want),
                         "trusted: " + kind, *("  | " + line for line in text.split("\n")))
    _say("orphan-tests: self-test arm %s rc=%d as expected%s" % (label, want, note))
    ok = True
    for needle in says:
        if needle not in text:
            ok = arms.fail(label, "exited as expected but its message never said",
                           "'%s', so the red does not tell the reader what actually happened."
                           % needle)
    for needle in silent:
        if needle in text:
            ok = arms.fail(label, "also emitted '%s', so this arm is NOT isolating the" % needle,
                           "one mechanism it claims to test and could pass on the strength of "
                           "another.")
    if census_want is not None:
        got = tuple(summary_number(text, k) for k in
                    ("sources", "CMakeLists", "registrations", "orphan(s),"))
        want_t = tuple(str(x) for x in census_want)
        if got != want_t:
            ok = arms.fail(label, "was not SURGICAL. Got %s/%s/%s sources/CMakeLists/"
                           "registrations" % got[:3],
                           "and %s orphan(s); wanted %s/%s/%s and %s. Either the mutant stopped "
                           "parsing or it" % ((got[3],) + want_t),
                           "changed more than the one thing this arm claims, and then the red "
                           "means something else.")
    return ok


def _judge(arms, label, sub, want, says=(), silent=(), counts=None, rows=None, errors=()):
    """The production census over the mirror, judged in process. -> True when it held."""
    c = census(sub.mirror, "st", sub.allow_rows if rows is None else rows, list(errors))
    text = "\n".join(render(c))
    ok = True
    if c.rc != want:
        ok = arms.fail(label, "census rc=%d, expected %d:" % (c.rc, want),
                       *("  | " + line for line in text.split("\n")))
    for needle in says:
        if needle not in text:
            ok = arms.fail(label, "never said '%s'." % needle)
    for needle in silent:
        if needle in text:
            ok = arms.fail(label, "also said '%s', so it is not isolating its mechanism." % needle)
    if counts is not None:
        got = (len(c.sources), len(c.cmakelists), len(c.reg_paths), len(c.orphans))
        if got != tuple(counts):
            ok = arms.fail(label, "census %s != expected %s (sources/CMakeLists/registrations/"
                           "orphans)." % (got, tuple(counts)))
    return ok


def _census_arm(arms, label, sub, want, **kw):
    arms.begin(label)
    if _judge(arms, label, sub, want, **kw):
        _say("orphan-tests: self-test arm %s rc=%d as expected" % (label, want))


def self_test(live, allow_rows):
    """Red-on-disable. -> EXIT_OK, or EXIT_COLLAPSE when any arm failed or went missing."""
    arms = _Arms()
    ot = _owning_tree()
    base = tempfile.mkdtemp(prefix="orphan-tests-selftest-")
    try:
        sub = _Subject(base, live, allow_rows)
        _inherited_and_added_arms(arms, sub)
        for ok, why, detail in ot.root_arms(repo_root, (Collapse,), False, __file__):
            arms.holds("R " + why, ok, "did not hold: %s" % detail)
    finally:
        removed = ot.remove_tree(base)
    arms.holds("T1 TEMP-TREES-REMOVED", removed, "left %s behind." % base)
    if len(arms.ran) != EXPECTED_ARMS:
        arms.failed.append("EXPECTED_ARMS")
        _say("orphan-tests: SELF-TEST FAIL - %d arm(s) ran, EXPECTED_ARMS says %d. An arm that "
             "silently stops running is a property that silently stops being proven."
             % (len(arms.ran), EXPECTED_ARMS))
    if arms.failed:
        _say("orphan-tests: SELF-TEST FAILED. Treat the verdict above as UNPROVEN: this guard has "
             "not")
        _say("  demonstrated that it can fail, which is the only thing that makes a green mean")
        _say("  anything. Fix the guard before trusting its census.")
        return EXIT_COLLAPSE
    _say("orphan-tests: self-test OK - %d arms exercised (%s); this guard is PROVEN able to fail."
         % (len(arms.ran), arms.breakdown()))
    return EXIT_OK


_P_LABELS = ("P1 QUOTED-HASH-IS-CONTENT", "P2 WHOLE-WORD-CALL-NAME",
             "P3 LOWERCASE-KEYWORDS-DO-NOT-REGISTER", "P4 UPPERCASE-COMMAND-REGISTERS",
             "P5 BRACKET-COMMENT-IS-NOT-COVERAGE", "P6 ESCAPED-QUOTE-DOES-NOT-TOGGLE",
             "P7 ESCAPED-HASH-IS-NOT-A-COMMENT")
_E_LABELS = ("E1 PARSE-INCOMPLETE-UNRESOLVED-VARIABLE", "E2 PARSE-INCOMPLETE-ABSOLUTE-PATH",
             "E3 PARSE-INCOMPLETE-OUTSIDE-THE-ROOT", "E4 PARSE-INCOMPLETE-NO-NAME",
             "E5 PARSE-INCOMPLETE-NO-SOURCE", "E6 PARSE-INCOMPLETE-UNTERMINATED-CALL")
_STRUCTURE_LABELS = ("11 PHANTOM-REGISTRATION", "12 STALE-ALLOW-SITE-GONE",
                     "X1 PRECEDENCE-COLLAPSE-OUTRANKS-ORPHAN-AND-STALE",
                     "X2 PRECEDENCE-ORPHAN-OUTRANKS-STALE", "A1 ALLOWLIST-MISSING-FIELD",
                     "A2 ALLOWLIST-EMPTY-FIELD", "A3 ALLOWLIST-UNKNOWN-KEY",
                     "A4 ALLOWLIST-DUPLICATE-SOURCE")
_STALE_LABELS = ("7 STALE-ALLOW-FILE-GONE", "8 STALE-ALLOW-NOW-REGISTERED",
                 "9 STALE-ALLOW-SITE-DROPPED-REFERENCE")


def _refuse(arms, labels, why):
    for label in labels:
        arms.begin(label)
        arms.fail(label, why)


def _inherited_and_added_arms(arms, sub):
    live = sub.live

    # ARM 0 -- the GREEN CONTROL. Without it every red below is worthless.
    _child_arm(arms, "0 GREEN-CONTROL", sub.mirror, EXIT_OK, " (mirror is a faithful subject)",
               census_want=(sub.src, sub.cml, sub.reg, 0))

    # ARM 1 -- THE ORPHAN: delete one whole single-line registration.
    w = select_witness(sub.pristine, live.cmakelists)
    if w is None:
        _refuse(arms, ("1 ORPHAN", "1b COMMENT-IS-NOT-COVERAGE") + _P_LABELS,
                "no witness registration could be selected (it must be a single-line "
                "dss_add_test whose sole source token occurs EXACTLY ONCE in its file). Without a "
                "unique witness the mutation would be ambiguous, so the arm is REFUSED rather "
                "than run weakly.")
    else:
        w_file, w_line, w_tok, w_path = w
        mutant = _without_line(sub.bytes[w_file], w_line)
        stem = w_tok[:-4] if w_tok.endswith(".cpp") else w_tok
        registration = "dss_add_test(NAME %s/%s SOURCES %s)" % (dir_of(w_path) or ".", stem, w_tok)
        if mutant == sub.bytes[w_file]:
            _refuse(arms, ("1 ORPHAN", "1b COMMENT-IS-NOT-COVERAGE"),
                    "the mutation changed NO bytes of the witness file. An arm that did not "
                    "mutate anything cannot prove a red.")
        else:
            _write(sub.m(w_file), mutant)
            _child_arm(arms, "1 ORPHAN", sub.mirror, EXIT_ORPHAN, " (witness %s)" % w_path,
                       says=("  ORPHAN: " + w_path,),
                       census_want=(sub.src, sub.cml, sub.reg - 1, 1))
            # ARM 1b -- A COMMENT IS NOT COVERAGE: the deleted registration re-added commented.
            _write(sub.m(w_file), _appended(mutant, "# " + registration))
            _child_arm(arms, "1b COMMENT-IS-NOT-COVERAGE", sub.mirror, EXIT_ORPHAN,
                       " (witness still orphaned)", says=("  ORPHAN: " + w_path,),
                       census_want=(sub.src, sub.cml, sub.reg - 1, 1))
        _parser_arms(arms, sub, mutant, w_file, w_path, registration)
        if not sub.restore(w_file):
            arms.fail("1 ORPHAN", "restoring the witness file did not reproduce the pristine "
                      "bytes; later arms would run against arm 1's mutant.")

    # ARMS 2 / 3 -- the two whole-root collapses (both floor-independent).
    _child_arm(arms, "2 COLLAPSE-EMPTY-ROOT", sub.empty, EXIT_COLLAPSE,
               says=("found 0 CMakeLists.txt under the tests root",))
    _child_arm(arms, "3 COLLAPSE-MISSING-ROOT", os.path.join(sub.empty, "definitely-not-here"),
               EXIT_COLLAPSE, says=("the tests root does not exist",))

    # ARMS 4-6 -- ONE ARM PER FLOOR, each isolated by asserted absence.
    _child_arm(arms, "4 COLLAPSE-SOURCE-FLOOR", sub.pristine, EXIT_COLLAPSE,
               says=("the source scan found only 0 *.cpp",),
               silent=("the CMakeLists scan found only", "the parser resolved only"))

    keep = 2
    stashed = live.cmakelists[keep:]
    for rel in stashed:
        os.replace(sub.m(rel), sub.m(rel) + ".stashed")
    _child_arm(arms, "5 COLLAPSE-CMAKELISTS-FLOOR", sub.mirror, EXIT_COLLAPSE,
               says=("the CMakeLists scan found only %d files" % keep,),
               silent=("the source scan found only",))
    for rel in stashed:
        os.replace(sub.m(rel) + ".stashed", sub.m(rel))

    # ARM 6 -- the registration floor: the densest CMakeLists are EMPTIED (the CMakeLists
    # enumeration stays exactly as healthy) until the EXACT distinct count left is below the
    # floor, and the child must report that exact number.
    by_file = {}
    for p, _n, f in live.regs:
        by_file.setdefault(f, set()).add(p)
    emptied, left = [], set(live.reg_paths)
    for f in sorted(by_file, key=lambda f: (-len(by_file[f]), f)):
        if len(left) < REGISTRATION_FLOOR:
            break
        emptied.append(f)
        left = set(p for p, _n, g in live.regs if g not in emptied)
    if len(left) >= REGISTRATION_FLOOR:
        _refuse(arms, ("6 COLLAPSE-REGISTRATION-FLOOR",),
                "could not drive the registration count below its floor (%d left, floor %d). The "
                "arm is REFUSED rather than run in a shape that proves something else."
                % (len(left), REGISTRATION_FLOOR))
    else:
        for rel in emptied:
            _write(sub.m(rel), b"")
        _child_arm(arms, "6 COLLAPSE-REGISTRATION-FLOOR", sub.mirror, EXIT_COLLAPSE,
                   says=("the parser resolved only %d distinct registered sources" % len(left),),
                   silent=("the source scan found only", "the CMakeLists scan found only"))
    for rel in live.cmakelists:
        if not sub.restore(rel):
            arms.fail("6 COLLAPSE-REGISTRATION-FLOOR", "after the floor arms, a mirror CMakeLists "
                      "no longer matches its pristine copy; later arms would run against a "
                      "mutated subject.")
            break

    _incomplete_arms(arms, sub, w)

    # ARMS 7-9 -- the stale-allowlist refusals, driven by the FIRST allowlist row.
    if not sub.allow_rows:
        _refuse(arms, _STALE_LABELS, "the allowlist has no row to drive it, so the arm is "
                "REFUSED rather than skipped.")
    else:
        a_path, a_site, _why = sub.allow_rows[0]
        a_base = a_path.rsplit("/", 1)[-1]
        a_stem = a_base[:-4] if a_base.endswith(".cpp") else a_base
        os.remove(sub.m(a_path))
        _child_arm(arms, _STALE_LABELS[0], sub.mirror, EXIT_STALE,
                   says=("does NOT exist under the tests root",),
                   census_want=(sub.src - 1, sub.cml, sub.reg, 0))
        _write(sub.m(a_path), b"")
        a_tok = posixpath.relpath(a_path, dir_of(a_site) or ".")
        _write(sub.m(a_site), _appended(sub.bytes[a_site], "dss_add_test(NAME %s/%s SOURCES %s)"
                                        % (dir_of(a_path) or ".", a_stem, a_tok)))
        _child_arm(arms, _STALE_LABELS[1], sub.mirror, EXIT_STALE,
                   says=("dss_add_test now REGISTERS it",),
                   census_want=(sub.src, sub.cml, sub.reg + 1, 0))
        if not sub.restore(a_site):
            arms.fail(_STALE_LABELS[1], "restoring the allowlist reference site after arm 8 did "
                      "not reproduce the pristine bytes.")
        dropped = b"\n".join(line for line in sub.bytes[a_site].split(b"\n")
                             if a_base.encode("utf-8") not in line)
        if dropped == sub.bytes[a_site]:
            _refuse(arms, (_STALE_LABELS[2],), "removed no line, so the reference site still "
                    "references the allowlisted source and the arm would pass for the wrong "
                    "reason.")
        else:
            _write(sub.m(a_site), dropped)
            _child_arm(arms, _STALE_LABELS[2], sub.mirror, EXIT_STALE,
                       says=("no longer STRUCTURALLY references",),
                       census_want=(sub.src, sub.cml, sub.reg, 0))
        if not sub.restore(a_site):
            arms.fail(_STALE_LABELS[2], "restoring the reference site after arm 9 did not "
                      "reproduce the pristine bytes.")

    _structure_arms(arms, sub, w)

    # ARM 10 -- GREEN AFTER RESTORE: every mutation above undone, the subject is back.
    _child_arm(arms, "10 GREEN-AFTER-RESTORE", sub.mirror, EXIT_OK, " (all mutations undone)",
               census_want=(sub.src, sub.cml, sub.reg, 0))

    # The command's own fail-closed plumbing.
    said = []
    rc = _guarded(lambda: 1 // 0, said.append)
    arms.holds("C1 CRASH-IS-A-COLLAPSE", rc == EXIT_COLLAPSE and any("crashed" in s for s in said),
               "a crash returned %s, not %d." % (rc, EXIT_COLLAPSE))
    arms.holds("U1 USAGE-EXTRA-OR-EMPTY-ARGUMENT-REFUSED",
               bool(usage_error(["a", "b"])) and bool(usage_error([""]))
               and not usage_error([]) and not usage_error(["x"]),
               "the argument contract drifted: extra/empty must be refused, none/one accepted.")
    bom = b"\xef\xbb\xbf" + b"dss_add_test(NAME a/b SOURCES b.cpp)\n"
    arms.holds("P8 BOM-IS-NOT-CONTENT",
               decode_cmakelists(bom, "bom") == bom[3:].decode("utf-8")
               and bom.decode("utf-8").startswith("\ufeff"),
               "a leading BOM survived the decode (or the fixture carries none).")


def _parser_arms(arms, sub, mutant, w_file, w_path, registration):
    """CMake's lexical rules, each on arm 1's mutant (the witness registration deleted) with
    the registration put back in the shape under test -- and, for every rule that HIDES a
    registration, the near-identical shape that does NOT hide it, so no arm passes vacuously."""
    orphan = "  ORPHAN: " + w_path
    full = (sub.src, sub.cml, sub.reg, 0)
    short = (sub.src, sub.cml, sub.reg - 1, 1)

    def pair(label, hiding, visible, hidden_rc=EXIT_ORPHAN, hidden_says=(orphan,)):
        arms.begin(label)
        _write(sub.m(w_file), _appended(mutant, *hiding))
        ok = _judge(arms, label, sub, hidden_rc, says=hidden_says, counts=short)
        _write(sub.m(w_file), _appended(mutant, *visible))
        if not _judge(arms, label, sub, EXIT_OK, counts=full):
            ok = arms.fail(label, "the shape that does NOT hide the registration failed to "
                           "register the witness, so the hiding shape proved nothing.")
        if ok:
            _say("orphan-tests: self-test arm %s rc=%d hidden, rc=0 visible, as expected"
                 % (label, hidden_rc))

    pair(_P_LABELS[0], ["message(# not a comment) " + registration],
         ['message("# not a comment") ' + registration])
    pair(_P_LABELS[1], ["x" + registration], [registration])
    pair(_P_LABELS[2], [registration.replace("NAME", "name").replace("SOURCES", "sources")],
         [registration], hidden_rc=EXIT_COLLAPSE,
         hidden_says=(orphan, "declares no NAME", "registers no resolvable source"))
    _write(sub.m(w_file), _appended(mutant, registration.replace("dss_add_test", "DSS_ADD_TEST")))
    _census_arm(arms, _P_LABELS[3], sub, EXIT_OK, counts=full)
    pair(_P_LABELS[4], ["#[=[", "]]", registration, "]=]"], ["# [=[", "]]", registration, "]=]"])
    pair(_P_LABELS[5], ['message("a " # b") ' + registration],
         ['message("a \\" # b") ' + registration])
    pair(_P_LABELS[6], ["message(a#b) " + registration], ["message(a\\#b) " + registration])


def _incomplete_arms(arms, sub, w):
    """E1-E6: one arm per way the parse can be INCOMPLETE, each appended to the witness file
    beside a token that still resolves, and each counted EXACTLY -- no other refusal fires."""
    if w is None:
        _refuse(arms, _E_LABELS, "no witness registration, so the arm is REFUSED.")
        return
    w_file, _line, tok, _path = w
    far = "../" * 12 + "x.cpp"
    cases = (
        ("dss_add_test(NAME st/e1 SOURCES %s ${DSS_SELFTEST_UNKNOWN}/x.cpp)" % tok,
         ["%s: SOURCES token ${DSS_SELFTEST_UNKNOWN}/x.cpp carries a CMake variable" % w_file]),
        ("dss_add_test(NAME st/e2 SOURCES %s /abs/x.cpp C:/abs/x.cpp)" % tok,
         ["%s: SOURCES token /abs/x.cpp is an ABSOLUTE path" % w_file,
          "%s: SOURCES token C:/abs/x.cpp is an ABSOLUTE path" % w_file]),
        ("dss_add_test(NAME st/e3 SOURCES %s %s)" % (tok, far),
         ["%s: SOURCES token %s resolves OUTSIDE the tests root" % (w_file, far)]),
        ("dss_add_test(SOURCES %s)" % tok,
         ["%s: a dss_add_test call declares no NAME." % w_file]),
        ("dss_add_test(NAME st/e5)",
         ["%s: dss_add_test(NAME st/e5) registers no resolvable source." % w_file]),
        ("dss_add_test(NAME st/e6 SOURCES %s" % tok,
         ["%s: an unterminated dss_add_test( call - the parse cannot be trusted." % w_file]),
    )
    for label, (line, messages) in zip(_E_LABELS, cases):
        _write(sub.m(w_file), _appended(sub.bytes[w_file], line))
        _census_arm(arms, label, sub, EXIT_COLLAPSE,
                    says=["%d CMakeLists token(s) could not be resolved" % len(messages)] + messages,
                    counts=(sub.src, sub.cml, sub.reg, 0))
        if not sub.restore(w_file):
            arms.fail(label, "restoring the witness file did not reproduce the pristine bytes.")


def _structure_arms(arms, sub, w):
    """Phantom, a vanished allowlist site, precedence, and the allowlist schema."""
    if w is None or not sub.allow_rows:
        _refuse(arms, _STRUCTURE_LABELS, "needs a witness and an allowlist row; REFUSED rather "
                "than skipped.")
        return
    live = sub.live
    w_file, w_line, _tok, w_path = w
    a_path = sub.allow_rows[0][0]

    # 11 -- PHANTOM, isolated: the witness source deleted, its registration kept.
    os.remove(sub.m(w_path))
    _census_arm(arms, _STRUCTURE_LABELS[0], sub, EXIT_COLLAPSE,
                says=("1 registration(s) name a source that does NOT exist",
                      "    PHANTOM: " + w_path),
                silent=("  ORPHAN:", "below its floor"), counts=(sub.src - 1, sub.cml, sub.reg, 0))
    _write(sub.m(w_path), b"")

    # 12 -- a row's site vanishes. Chosen among sites holding NO registration, so the removal
    # orphans nothing and the stale class is the only one that can speak.
    per_site = {}
    for _p, _n, f in live.regs:
        per_site[f] = per_site.get(f, 0) + 1
    quiet = [site for _src, site, _w in sub.allow_rows if per_site.get(site, 0) == 0]
    if not quiet:
        _refuse(arms, (_STRUCTURE_LABELS[1],), "no allowlist row's site holds zero registrations, "
                "so removing a site would also orphan sources; the arm is REFUSED.")
    else:
        site = quiet[0]
        os.replace(sub.m(site), sub.m(site) + ".stashed")
        _census_arm(arms, _STRUCTURE_LABELS[1], sub, EXIT_STALE,
                    says=("is referenced by '%s', which does not exist." % site,),
                    counts=(sub.src, sub.cml - 1, sub.reg, 0))
        os.replace(sub.m(site) + ".stashed", sub.m(site))
        if _read(sub.m(site)) != sub.bytes[site]:
            arms.fail(_STRUCTURE_LABELS[1], "the site did not come back byte-identical.")

    # X1 / X2 -- several classes at once: every class reported, the exit the highest.
    no_witness = _without_line(sub.bytes[w_file], w_line)
    os.remove(sub.m(a_path))
    _write(sub.m(w_file), _appended(
        no_witness, "dss_add_test(NAME st/x1 SOURCES ${DSS_SELFTEST_UNKNOWN}/x.cpp)"))
    _census_arm(arms, _STRUCTURE_LABELS[2], sub, EXIT_COLLAPSE,
                says=("carries a CMake variable", "  ORPHAN: " + w_path,
                      "STALE: the allowlist names '%s'" % a_path))
    _write(sub.m(w_file), no_witness)
    _census_arm(arms, _STRUCTURE_LABELS[3], sub, EXIT_ORPHAN,
                says=("  ORPHAN: " + w_path, "STALE: the allowlist names '%s'" % a_path))
    _write(sub.m(a_path), b"")
    if not sub.restore(w_file):
        arms.fail(_STRUCTURE_LABELS[3], "the witness file did not come back byte-identical.")

    # A1-A4 -- the allowlist schema. A row that cannot be checked is a collapse AND clears
    # nothing: its source, allowlisted only, surfaces as an ORPHAN -- the fail-open, closed.
    rows = [{"source": s, "site": t, "reason": r} for s, t, r in sub.allow_rows]
    box = os.path.join(sub.base, "allowlists")
    os.makedirs(box, exist_ok=True)

    def schema(label, mutate, message, orphaned):
        doc = [dict(r) for r in rows]
        mutate(doc)
        path = os.path.join(box, label.split(" ")[0] + ".json")
        _write(path, json.dumps(doc, indent=2).encode("utf-8"))
        got_rows, errs = load_allowlist(path, "fixture")
        says = [message] + (["  ORPHAN: " + a_path] if orphaned else [])
        _census_arm(arms, label, sub, EXIT_COLLAPSE, says=says, rows=got_rows, errors=errs)

    schema(_STRUCTURE_LABELS[4], lambda d: d[0].pop("site"),
           "allowlist row 1 is missing the field(s) 'site'", True)
    schema(_STRUCTURE_LABELS[5], lambda d: d[0].update(site=""),
           "allowlist row 1 has an EMPTY 'site'", True)
    schema(_STRUCTURE_LABELS[6], lambda d: d[0].update(note="x"),
           "allowlist row 1 carries unknown key(s) 'note'", True)
    schema(_STRUCTURE_LABELS[7], lambda d: d.append(dict(d[0])),
           "allowlist row %d repeats the source '%s'" % (len(rows) + 1, a_path), False)


# ── the command ────────────────────────────────────────────────────────────────────────

def usage_error(argv):
    """At most one argument, and never an empty one."""
    if len(argv) > 1 or (argv and argv[0] == ""):
        return ("check-orphan-tests: usage: check-orphan-tests.py [<tests root>] -- got %d "
                "argument(s)%s." % (len(argv), ", one of them empty" if "" in argv else ""))
    return None


def _guarded(fn, err=None):
    """Run `fn`; a crash is exit 2 with its traceback -- never 1, which is ORPHAN."""
    err = err or _say
    try:
        return fn()
    except Exception as exc:
        err("orphan-tests: FAIL - the guard crashed (%s: %s); a crash is never a verdict."
            % (type(exc).__name__, exc))
        err(traceback.format_exc().rstrip("\n"))
        return EXIT_COLLAPSE


def main(argv):
    why = usage_error(argv)
    if why:
        print(why, file=sys.stderr, flush=True)
        return EXIT_COLLAPSE

    def body():
        try:
            tree = repo_root()
        except Collapse as exc:
            print("check-orphan-tests: SCAN COLLAPSED -- %s" % exc, file=sys.stderr, flush=True)
            return EXIT_COLLAPSE
        if argv:
            root_in, label, run_self_test = argv[0], argv[0], False
        else:
            root_in, label, run_self_test = os.path.join(tree, "tests"), "tests", True
        rows, errors = load_allowlist(allowlist_path())
        live = census(root_in, label, rows, errors)
        for line in render(live):
            _say(line)
        if not run_self_test:
            return live.rc
        if live.rc != EXIT_OK:
            _say("orphan-tests: self-test NOT run - the live verdict is red (rc=%d), and a red tree "
                 "cannot seed the green mirror the arms mutate. The red above is itself the proof "
                 "that this guard fails for that class; fix the tree and the next run proves the "
                 "rest." % live.rc)
            return live.rc
        rc_self = self_test(live, rows)
        return rc_self if rc_self != EXIT_OK else live.rc

    return _guarded(body)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
