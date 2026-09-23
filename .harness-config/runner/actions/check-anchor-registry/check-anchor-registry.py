#!/usr/bin/env python3
# PURPOSE: refuse a `D-*` anchor cited in a scanned root that resolves to no registry row, and refuse a markdown table row whose unescaped pipes would silently drop cells.
"""check-anchor-registry.py -- the deferred-anchor citation guard and the plans' table-cell guard.

ONE program on every host. It replaces a `.sh` / `.ps1` pair that carried the same contract
twice; where the two disagreed, this file takes the STRICTER reading, and every such choice
is listed under DECISIONS below.

THE CONTRACT
  1. Every anchor id cited in a SCANNED ROOT (`src/`, `examples/`, `docs/` -- see
     `ROOT_SPECS`) must resolve: it must be a SUBSTRING of the plan corpus
     (`.plans/**/*.md`). Substring, not equality, on purpose: a citation of a parent
     name resolves through the more specific row that contains it, and a line-wrapped
     fragment resolves through the whole name. The wider citation roots belong to
     `dssharness check-anchor-citations` (`anchors.citationRoots`), not to this guard.
  2. No markdown table row under `.plans/` may carry MORE unescaped-pipe cells than its
     own table's header: the renderer silently drops the surplus. A SHORT row is padded
     and loses nothing, so it is counted and never fails. There is no exception list.
  3. A citation may not name a RETIRED id (a registry row whose status cell OPENS with
     the retirement marker). Retired ids still resolve, because the plans mention them;
     this check is what keeps a dead name from surviving on that mention.
  4. A document that QUOTES a dead id as evidence declares it on a line of its own,
     positionally (only decoration may precede the ANCHOR-GUARD-QUOTED-NOT-CITED:
     token). The declaration exempts the id IN THAT FILE ONLY and is refused when it is
     STALE (the id is cited nowhere else in that file), FALSE (the id resolves in
     `.plans/`) or LEAKED (another scanned file cites it too).

THE GRAMMAR. An anchor id is `D-` plus one segment plus at least one more hyphenated
segment (`ANCHOR_CORE`); a head-only name stays informal. It is collected only at an
ASCII word boundary -- the in-comment phrase in `src/asm/format/fixed32.hpp` that spells
a 32-bit fixed-width word with hyphens has an anchor-shaped tail, and the boundary is
what keeps it out. Matching is case-sensitive: a lower-case token is never a citation.
⚠ This file's own fixtures are ASSEMBLED by `_fx()` from segments, so no anchor-shaped
token ever appears in its bytes: `.harness-config` is a citation root, and a written-out
fixture would be a citation of a row that does not exist.

FAIL-CLOSED. Every scan has a floor (per root, and three for the cell-width scan), a
missing root is a refusal, an unreadable file or a directory the walk cannot list is a
refusal, and the retired-id extraction must find at least one row. A guard that reports
a pass over what it never read is the worst defect a guard can have.

THE SELF-TEST RUNS FIRST, ON EVERY INVOCATION, AND TAKES NO FLAG. ctest passes no
argument, so a flag would be a self-test CI never runs. Every arm drives the production
function it names; an arm that asserts an ABSENCE also proves the forbidden token CAN be
produced, so it cannot pass vacuously. Arms are counted against `EXPECTED_ARMS` and the
printed count is derived from the arms that ran.

EXIT CODES (the precedence is the order the checks exit in)
  6  the self-test failed (nothing after it runs)
  2  the tree could not be resolved; or a root scan collapsed (every failing root is
     reported first)
  5  a quotation declaration is STALE, FALSE or LEAKED
  2  the retired-id extraction found no marked row
  4  a citation names a retired id
  1  an anchor resolves nowhere -- or 2 when the cell-width scan collapsed, because a
     scan that checked nothing makes the other verdicts untrustworthy
  then the cell-width status: 2 collapsed, 3 a row drops content, 0 clean.
  A usage error (any argument) and a crash of this program are 2 as well -- never 1,
  which means "an anchor resolves nowhere".

DECISIONS (where the two predecessors disagreed, or both were blind)
  (a) the resolve reads `.plans/**/*.md` only (the smaller corpus, i.e. the stricter
      resolve, and the one the FAIL message names); FALSE is judged against EVERY file
      under `.plans/` (the larger corpus, the stricter FALSE). Dotfiles count in both.
  (b) the include globs match case-INsensitively (scans more).
  (c) the boundary is an ASCII look-behind `(?<![A-Za-z0-9_])` (collects more).
  (d) LEAKED, retired membership and the locator stay case-SENSITIVE: collection is
      case-sensitive, so a lower-case token is never a citation to begin with.
  (e) wrap recovery is the two-line join transcribed verbatim, NOT a reuse of
      `check-wrapped-anchor-ids`: importing that module runs git at import time and can
      exit the importer (it resolves its tree with git's agreement), and this guard
      reads no git.
  (f) a leading UTF-8 BOM is stripped (it would add a phantom first header cell);
      lines split on `\\n` only, one trailing CR dropped.
  (g) an unreadable file, or a directory the walk cannot list, is a collapse (2).
  (h) any argument is a usage error (2): the guard has exactly one form.
  (i) an uncaught exception exits 2, never 1.
  Excluded directories below a root (`worktrees`, `__pycache__`, `.git`) are pruned by
  exact NAME below the root only -- a checkout whose ancestor happens to be named
  `worktrees` still scans. Symlinked regular files are read.

Exit codes: 0 clean · 1 an anchor resolves nowhere · 2 a scan collapsed · 3 a table
row drops content · 4 a retired id is cited · 5 a quotation declaration is refused ·
6 the self-test failed.

Usage (from anywhere -- the tree is the one this file lives in, never the cwd):
    python .harness-config/runner/actions/check-anchor-registry/check-anchor-registry.py
"""
from __future__ import annotations

import fnmatch
import importlib.util
import os
import re
import shutil
import sys
import tempfile
import traceback

# A Windows pipe comes up cp1252 and dies printing a glyph; reconfigure BEFORE anything can
# print (`guard_output_encoding_guard` probes exactly this, by importing this module in a
# child). Line buffering keeps the stdout and stderr halves of a report in order when a
# caller merges the two streams. Nothing else runs at import.
for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8", errors="replace", line_buffering=True)
    except (AttributeError, ValueError, OSError):  # pragma: no cover - an odd stream
        pass

EXIT_OK, EXIT_MISSING, EXIT_COLLAPSE, EXIT_CELLS, EXIT_RETIRED, EXIT_QUOTE, EXIT_SELFTEST = \
    0, 1, 2, 3, 4, 5, 6

# ── the grammar ────────────────────────────────────────────────────────────────────────
# Spelled ONCE; the boundary is bolted on where it is needed. `{1,}` hyphen groups after
# the head make a name formal; a head-only name stays informal.
ANCHOR_CORE = r"D-[A-Z0-9_]+(?:-[A-Z0-9_]+)+"
_BOUNDARY = r"(?<![A-Za-z0-9_])"
ANCHOR_RX = re.compile(_BOUNDARY + ANCHOR_CORE)
ANCHOR_WHOLE_RX = re.compile(ANCHOR_CORE + r"\Z")
# The DECLARATION grammar: `*`, so a head-only id can be declared. Its boundary is tested
# by hand against the preceding character, like the wrap join's.
DECL_ID_RX = re.compile(r"D-[A-Z0-9_]+(?:-[A-Z0-9_]+)*")
WORD_CHAR_RX = re.compile(r"[A-Za-z0-9_]")

# ── the roots ──────────────────────────────────────────────────────────────────────────
# (root, floor, include globs). Floors count DISTINCT anchors from the raw single-line scan,
# before wrap recovery, so a burst of wrapped citations can never mask a root that stopped
# being read. They catch a COLLAPSED scan, not drift: fix the scan, never the floor.
_SOURCE_GLOBS = ("*.cpp", "*.hpp", "*.json", "*.c", "*.s", "*.inc", "*.probes",
                 "CMakeLists.txt", "*.md")
ROOT_SPECS = (
    ("src", 400, _SOURCE_GLOBS),
    ("examples", 150, _SOURCE_GLOBS),
    ("docs", 8, ("*.md",)),
)
# Another checkout of this repository, bytecode, git's own directory: pruned by exact NAME
# below a root. An agent's worktree under a scanned root would otherwise be scanned twice
# and every document would face its own duplicate (measured once as six false LEAKED reds).
EXCLUDE_DIRS = frozenset(("worktrees", "__pycache__", ".git"))

# ── the cell-width scan ────────────────────────────────────────────────────────────────
CELL_WIDTH_ROOT = ".plans"
CELL_WIDTH_FLOORS = (20, 100, 1500)     # files, tables, table data rows
PREVIEW_MAX = 110
DELIM_RX = re.compile(r"\|?[ ]*:?-+:?[ ]*(?:\|[ ]*:?-+:?[ ]*)*\|?")
_NOT_PRINTABLE_ASCII_RX = re.compile(r"[^ -~]")
_PIPE_PLACEHOLDER = "\x01"

# ── wrap recovery (the two-line join, transcribed; see decision (e)) ──────────────────
_WRAP_TAIL_RX = re.compile(r"D-[A-Z0-9_]+(?:-[A-Z0-9_]+)*\Z")
_WRAP_CONT_RX = re.compile(r"[A-Z0-9_]+(?:-[A-Z0-9_]+)*")
_LEAD_NON_WORD_RX = re.compile(r"\A[^A-Za-z0-9_]+")
# A FILE filter only, deliberately LOOSER than the join (no boundary, any trailing
# whitespace): it decides which files are worth splitting into lines, never a verdict.
_WRAP_FILE_FILTER_RX = re.compile(r"D-[A-Z0-9_]+(?:-[A-Z0-9_]+)*-[ \t\r\f\v]*$", re.MULTILINE)

# ── quotation declarations ─────────────────────────────────────────────────────────────
DECL_MARKER = "ANCHOR-GUARD-QUOTED-NOT-CITED:"
DECL_LINE_RX = re.compile(r"[^A-Za-z0-9_]*" + re.escape(DECL_MARKER))
_DECL_FILE_FILTER_RX = re.compile(r"^[^A-Za-z0-9_\n]*" + re.escape(DECL_MARKER), re.MULTILINE)

# ── retired ids ────────────────────────────────────────────────────────────────────────
REGISTRY_GLOB = "_deferred-anchor-registry*.md"
RETIRED_FLOOR = 1
# A row line opens `| ` then a backticked id. The marker is POSITIONAL: the status cell must
# OPEN with the retirement headline, where prose never sits (two searching forms each
# reddened on a row that merely wrote about the token). Two cell positions are read: the
# plan-side four-cell tables and the registry's six-cell layout.
RETIRED_ROW_RX = re.compile(r"\| `D-", re.ASCII)
RETIRED_MARK_RX = re.compile(" *\\**✅ \\*\\*CLOSED [0-9-]+ — RETIRED-ID")

EXPECTED_ARMS = 60


class Collapse(Exception):
    """A structural failure: the scan cannot be trusted. Exit 2, never a pass."""


class ScanError(Collapse):
    """A file could not be read, or a directory could not be listed."""


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


# ── reading and walking ────────────────────────────────────────────────────────────────

def read_text(path, shown=None):
    """The file's text: bytes decoded as UTF-8 (undecodable bytes kept as surrogates), one
    leading BOM dropped. An unreadable file raises ScanError -- it is never skipped."""
    try:
        with open(path, "rb") as fh:
            data = fh.read()
    except OSError as exc:
        raise ScanError("cannot read %s (%s)" % (shown or path, exc.strerror or exc))
    text = data.decode("utf-8", "surrogateescape")
    return text[1:] if text.startswith("﻿") else text


def lines_of(text):
    """Lines split on `\\n` ONLY (never `splitlines`, which also splits on U+2028 and
    friends), one trailing CR dropped per line; the newline ending the last line opens none."""
    parts = text.split("\n")
    if parts and parts[-1] == "":
        parts.pop()
    return [p[:-1] if p.endswith("\r") else p for p in parts]


def _walk_error(exc):
    raise ScanError("cannot list %s (%s)" % (getattr(exc, "filename", None) or "?",
                                             getattr(exc, "strerror", None) or exc))


def _walk_files(top, prune):
    """Every regular file (a symlink to one included) under `top`, as `/`-joined paths
    relative to it, ordinal order. `prune` names directories skipped BELOW `top` by exact
    name. A directory that cannot be listed raises ScanError (fail closed)."""
    out = []
    for dirpath, dirnames, filenames in os.walk(top, onerror=_walk_error):
        dirnames[:] = sorted(d for d in dirnames if d not in prune)
        for fn in filenames:
            full = os.path.join(dirpath, fn)
            if os.path.isfile(full):
                out.append(os.path.relpath(full, top).replace(os.sep, "/"))
    return sorted(out)


def glob_hit(name, globs):
    """Decision (b): an include glob matches case-INsensitively, with an explicit fold."""
    low = name.lower()
    return any(fnmatch.fnmatchcase(low, g.lower()) for g in globs)


# ── wrap recovery ──────────────────────────────────────────────────────────────────────

def wrap_prefix(line):
    """The anchor-shaped fragment a line ENDS mid-name with (the hyphen dropped), or "".

    The search LOOPS: the leftmost candidate can sit inside a longer hyphenated word (a
    word character before it) while a genuine fragment starts later on the same line."""
    s = line.rstrip(" \t")
    if not s.endswith("-"):
        return ""
    s = s[:-1]
    off = 0
    while True:
        m = _WRAP_TAIL_RX.search(s, off)
        if not m:
            return ""
        start = m.start()
        if start == 0 or not WORD_CHAR_RX.match(s[start - 1]):
            return s[start:]
        off = start + 1


def wrap_continuation(nxt):
    """The head of a continuation line: every leading non-alphanumeric byte dropped, then an
    UPPER-CASE run -- a continuation that resumes in lower case is prose, not a name."""
    m = _WRAP_CONT_RX.match(_LEAD_NON_WORD_RX.sub("", nxt, count=1))
    return m.group(0) if m else ""


def join_wrapped(rel, lines):
    """-> [(rel, joined name)] for every line ending mid-name whose NEXT line continues it."""
    out = []
    for i, line in enumerate(lines):
        pre = wrap_prefix(line)
        if not pre:
            continue
        cont = wrap_continuation(lines[i + 1] if i + 1 < len(lines) else "")
        if cont:
            out.append((rel, pre + "-" + cont))
    return out


# ── quotation declarations ─────────────────────────────────────────────────────────────

def quote_records(rel, lines):
    """-> [(kind, rel, id)], kind DECL or STALE, one per declared id (first-seen order).

    Recognition is POSITIONAL (only decoration before the marker). Ids sit on an ASCII word
    boundary -- which is also what keeps the marker's own anchor-shaped tail out."""
    is_decl = [bool(DECL_LINE_RX.match(line)) for line in lines]
    ids, seen = [], set()
    for line, decl in zip(lines, is_decl):
        if not decl:
            continue
        for m in DECL_ID_RX.finditer(line):
            s = m.start()
            if s > 0 and WORD_CHAR_RX.match(line[s - 1]):
                continue
            if m.group(0) not in seen:
                seen.add(m.group(0))
                ids.append(m.group(0))
    out = []
    for aid in ids:
        cited = any(aid in line for line, decl in zip(lines, is_decl) if not decl)
        out.append(("DECL" if cited else "STALE", rel, aid))
    return out


# ── retired ids ────────────────────────────────────────────────────────────────────────

def retired_ids(lines):
    """-> the key of every positionally-retired row, one entry PER ROW (a key in two
    registries counts twice). Splits on EVERY pipe, escaped or not."""
    out = []
    for raw in lines:
        line = raw[:-1] if raw.endswith("\r") else raw
        if not RETIRED_ROW_RX.match(line):
            continue
        cells = line.split("|")
        if len(cells) < 3:
            continue            # no status cell at all: nothing positional to read
        key = cells[1].replace("`", "").replace(" ", "")
        if RETIRED_MARK_RX.match(cells[2]) or (len(cells) > 4 and RETIRED_MARK_RX.match(cells[4])):
            out.append(key)
    return out


# ── the per-root scan ──────────────────────────────────────────────────────────────────

class FileScan:
    __slots__ = ("rel", "tokens", "wraps", "decls")

    def __init__(self, rel, tokens, wraps, decls):
        self.rel, self.tokens, self.wraps, self.decls = rel, tokens, wraps, decls


def scan_text(rel, text):
    """One file, read once: its single-line anchors (in order, repeats kept), its wrapped
    joins and its declaration records."""
    tokens = ANCHOR_RX.findall(text)
    wraps, decls = [], []
    want_wrap = bool(_WRAP_FILE_FILTER_RX.search(text))
    want_decl = bool(_DECL_FILE_FILTER_RX.search(text))
    if want_wrap or want_decl:
        lines = lines_of(text)
        if want_wrap:
            wraps = join_wrapped(rel, lines)
        if want_decl:
            decls = quote_records(rel, lines)
    return FileScan(rel, tokens, wraps, decls)


class RootScan:
    __slots__ = ("label", "ok", "anchors", "files", "msgs")

    def __init__(self, label):
        self.label, self.ok, self.anchors, self.files, self.msgs = label, False, set(), [], []


def root_files(root_path, globs):
    """The files of one root this guard reads: include globs (decision b), dotfiles
    included, EXCLUDE_DIRS pruned below the root."""
    return [rel for rel in _walk_files(root_path, EXCLUDE_DIRS)
            if glob_hit(rel.rsplit("/", 1)[-1], globs)]


def scan_root(root_path, label, floor, globs, sink=None):
    """-> RootScan. The root must exist, be fully readable, and clear its floor of DISTINCT
    anchors; only then are they added to `sink`. Messages are returned, never printed."""
    rs = RootScan(label)
    if not os.path.isdir(root_path):
        rs.msgs.append("anchor-registry: FAIL - scan root '%s' does not exist. A missing root "
                       "would silently shrink coverage; refusing to report a partial scan as a "
                       "pass." % label)
        return rs
    try:
        for rel in root_files(root_path, globs):
            shown = label + "/" + rel
            fs = scan_text(shown, read_text(os.path.join(root_path, rel), shown))
            rs.files.append(fs)
            rs.anchors.update(fs.tokens)
    except ScanError as exc:
        rs.msgs.append("anchor-registry: FAIL - root '%s' could not be read completely: %s."
                       % (label, exc))
        rs.msgs.append("  An unreadable file or directory would silently shrink coverage; "
                       "refusing to report a partial scan as a pass.")
        return rs
    if len(rs.anchors) < floor:
        rs.msgs.append("anchor-registry: FAIL - root '%s' yielded only %d anchors, below its "
                       "floor of %d." % (label, len(rs.anchors), floor))
        rs.msgs.append("  This does NOT mean that root is clean - it means ITS scan collapsed "
                       "(unreadable files, a drifted include filter, or a moved subtree).")
        rs.msgs.append("  Refusing to report a pass. Fix the scan; do not lower the floor.")
        return rs
    rs.ok = True
    if sink is not None:
        sink.update(rs.anchors)
    return rs


# ── the plans: one read serves the cell-width scan, the resolve and the registry ───────

class PlanFiles:
    """Every `*.md` under `.plans/` (recursive, dotfiles included, nothing pruned), read once."""

    def __init__(self, tree):
        self.tree = tree
        self.root = os.path.join(tree, CELL_WIDTH_ROOT)
        self.exists = os.path.isdir(self.root)
        self.texts = []          # [(rel, text)], ordinal by rel
        self.errors = []
        self.found = 0           # files the walk named, read or not
        if not self.exists:
            return
        try:
            rels = [r for r in _walk_files(self.root, frozenset()) if r.endswith(".md")]
        except ScanError as exc:
            self.errors.append(str(exc))
            return
        self.found = len(rels)
        for r in rels:
            rel = CELL_WIDTH_ROOT + "/" + r
            try:
                self.texts.append((rel, read_text(os.path.join(self.root, r), rel)))
            except ScanError as exc:
                self.errors.append(str(exc))


class PlanCorpus:
    """The resolve (decision a): an anchor resolves iff it is a SUBSTRING of the plan corpus.

    `vocab` (every anchor-shaped token the plans carry) and `blob` (the vocabulary joined) are
    only shortcuts -- each can answer True only where the corpus text would -- and the text
    itself is the one authority allowed to conclude "missing"."""

    def __init__(self, texts):
        self.text = "\n".join(t for _rel, t in texts)
        self.vocab = set()
        for _rel, t in texts:
            self.vocab.update(ANCHOR_RX.findall(t))
        self.blob = "\n".join(sorted(self.vocab))

    def resolves(self, anchor):
        if not anchor:
            return False            # an empty candidate would match everything
        return anchor in self.vocab or anchor in self.blob or anchor in self.text


def plans_everything_text(tree, md_texts):
    """Decision (a), FALSE side: every file under `.plans/`, whatever its extension."""
    root = os.path.join(tree, CELL_WIDTH_ROOT)
    have = dict(md_texts)
    parts = []
    for r in _walk_files(root, frozenset()):
        rel = CELL_WIDTH_ROOT + "/" + r
        parts.append(have[rel] if rel in have else read_text(os.path.join(root, r), rel))
    return "\n".join(parts)


# ── the cell-width property ────────────────────────────────────────────────────────────

def _trim(s):
    return s.strip(" \t")


def cells_of(line):
    """-> (count, lo, hi, parts): the row split on UNESCAPED pipes (`\\|` is content), a
    first and a last part that trim to empty dropped."""
    parts = line.replace("\\|", _PIPE_PLACEHOLDER).split("|")
    lo, hi = 0, len(parts) - 1
    if _trim(parts[lo]) == "":
        lo += 1
    if hi >= lo and _trim(parts[hi]) == "":
        hi -= 1
    return (hi - lo + 1 if hi >= lo else 0), lo, hi, parts


def display(s):
    """ASCII-only, single-line, bounded: non-ASCII is stripped BEFORE the cut."""
    t = _NOT_PRINTABLE_ASCII_RX.sub("", s.replace(_PIPE_PLACEHOLDER, "|"))
    return _trim(t)[:PREVIEW_MAX]


class CellWidth:
    def __init__(self):
        self.root_missing = False
        self.files = self.tables = self.rows = self.under = 0
        self.over = []           # [(rel, line, header line, expected, actual, dropped)]
        self.errors = []


def cell_width_file(rel, lines, res):
    """One file, with a one-line lookahead (a header is known only by the delimiter row
    AFTER it); the last line of every file is judged with an empty lookahead."""
    in_fence, hdr, hdr_line, skip_next = False, -1, 0, False
    n = len(lines)
    for i in range(n):
        line = lines[i]
        nxt = lines[i + 1] if i + 1 < n else ""
        s = _trim(line)
        if s.startswith("```") or s.startswith("~~~"):
            in_fence, hdr = not in_fence, -1
            continue
        if in_fence:
            continue
        if "|" not in line:
            hdr = -1
            continue
        if skip_next:
            skip_next = False
            continue
        if "|" in nxt and DELIM_RX.fullmatch(_trim(nxt)):
            hdr, hdr_line, skip_next = cells_of(line)[0], i + 1, True
            res.tables += 1
            continue
        if hdr < 0:
            continue
        res.rows += 1
        count, lo, hi, parts = cells_of(line)
        if count < hdr:
            res.under += 1
            continue
        if count == hdr:
            continue
        ex = ""
        for k in range(lo + hdr, hi + 1):
            ex = ex + ("" if ex == "" else " // ") + _trim(parts[k])
        res.over.append((rel, i + 1, hdr_line, hdr, count, display(ex)))


def cell_width_scan(plans):
    res = CellWidth()
    if not plans.exists:
        res.root_missing = True
        return res
    res.errors = list(plans.errors)
    res.files = plans.found
    for rel, text in plans.texts:
        cell_width_file(rel, lines_of(text), res)
    return res


def cell_width_report(res, floors, out):
    """Print the whole cell-width report; -> 2 collapsed, 3 a row drops content, 0 clean."""
    if res.root_missing:
        out("anchor-registry: FAIL - cell-width root '%s' does not exist. Refusing to report a "
            "pass on a scan of nothing." % CELL_WIDTH_ROOT)
        return EXIT_COLLAPSE
    file_floor, table_floor, row_floor = floors
    collapsed = False
    for why in res.errors:
        out("anchor-registry: FAIL - cell-width scan: %s." % why)
        out("  An unread plan file checks nothing. Refusing to report a pass; fix the scan.")
        collapsed = True
    if res.files < file_floor:
        out("anchor-registry: FAIL - cell-width scan found only %d markdown files under %s, "
            "below its floor of %d." % (res.files, CELL_WIDTH_ROOT, file_floor))
        out("  This does NOT mean the plans are clean - it means the SCAN COLLAPSED. Refusing "
            "to report a pass; fix the scan, do not lower the floor.")
        collapsed = True
    if res.tables < table_floor:
        out("anchor-registry: FAIL - cell-width scan found only %d tables, below its floor of "
            "%d." % (res.tables, table_floor))
        out("  A collapsed table scan checks nothing. Refusing to report a pass; fix the scan, "
            "do not lower the floor.")
        collapsed = True
    if res.rows < row_floor:
        out("anchor-registry: FAIL - cell-width scan found only %d table data rows, below its "
            "floor of %d." % (res.rows, row_floor))
        out("  A collapsed row scan checks nothing. Refusing to report a pass; fix the scan, do "
            "not lower the floor.")
        collapsed = True
    if res.over:
        out("anchor-registry: FAIL - %d markdown table row(s) carry MORE cells than their table "
            "header." % len(res.over))
        out("The surplus is SILENTLY DROPPED by the renderer: the text stays in the file and")
        out("vanishes from the page. This is invisible from both sides - nothing in the raw")
        out("text shows it, and nothing in the diff shows it.")
        out("")
        for rel, ln, hl, exp, act, dropped in res.over:
            out("  %s:%d  header@%d expected=%d actual=%d" % (rel, ln, hl, exp, act))
            out("      dropped: %s" % dropped)
        out("")
        out("Fix: the surplus is CONTENT that was read as a column boundary. Escape a content")
        out("pipe as \\| (the only form that survives inside a code span); join a genuinely")
        out("extra cell with the registry seam marker instead of a column boundary. Never")
        out("delete text to make the count fit, and never widen the check.")
    out("anchor-registry: cell-width %d tables / %d rows in %d files: %d violation(s), %d short "
        "rows (padded, no loss)." % (res.tables, res.rows, res.files, len(res.over), res.under))
    if collapsed:
        return EXIT_COLLAPSE
    return EXIT_CELLS if res.over else EXIT_OK


# ── the citation index (built at most once, only when a consumer needs it) ─────────────

class CitationIndex:
    """Distinct (path, token) pairs in first-seen order: every root's single-line anchors,
    then the recovered wrapped names -- so a wrapped citation is located like any other."""

    def __init__(self, scans):
        self.pairs, seen = [], set()
        for rs in scans:
            for fs in rs.files:
                for t in fs.tokens:
                    if (fs.rel, t) not in seen:
                        seen.add((fs.rel, t))
                        self.pairs.append((fs.rel, t))
        for rs in scans:
            for fs in rs.files:
                for pair in fs.wraps:
                    if pair not in seen:
                        seen.add(pair)
                        self.pairs.append(pair)

    def _paths(self, keep):
        out, seen = [], set()
        for p, t in self.pairs:
            if keep(p, t) and p not in seen:
                seen.add(p)
                out.append(p)
        return out

    def leaked(self, aid, declaring):
        """Other files citing exactly `aid` -- EQUALITY: a longer name is a different anchor."""
        return self._paths(lambda p, t: t == aid and p != declaring)

    def locate(self, anchor):
        """Files citing `anchor` or a MORE SPECIFIC name containing it (substring)."""
        return self._paths(lambda p, t: anchor in t)


# ── the verdict ────────────────────────────────────────────────────────────────────────

def _root_names(specs):
    return ", ".join(root + "/" for root, _f, _g in specs)


def verify(tree, specs=None, cw_floors=None, out=None, err=None):
    """The post-self-test body, in the predecessors' order and with their early exits.
    Floors are parameters so the self-test can judge a miniature tree; None = the constants."""
    specs = ROOT_SPECS if specs is None else specs
    cw_floors = CELL_WIDTH_FLOORS if cw_floors is None else cw_floors
    out = out or _say_out
    err = err or _say_err

    # CHECK 2 (cell-width) is REPORTED first and applied last: both halves report in one run.
    plans = PlanFiles(tree)
    cw_status = cell_width_report(cell_width_scan(plans), cw_floors, out)

    # The roots: every failing root is reported before the exit.
    anchors, scans, failed = set(), [], False
    for root, floor, globs in specs:
        rs = scan_root(os.path.join(tree, root), root, floor, globs, anchors)
        for m in rs.msgs:
            err(m)
        failed = failed or not rs.ok
        scans.append(rs)
    if failed:
        return EXIT_COLLAPSE

    # Wrap recovery AFTER the floors: a recovered name widens what must resolve and never
    # moves what proves the scan ran. The joined name is ADDED; the prefix stays.
    for rs in scans:
        for fs in rs.files:
            for _rel, name in fs.wraps:
                if ANCHOR_WHOLE_RX.match(name):
                    anchors.add(name)

    corpus = PlanCorpus(plans.texts)
    index = []                                  # built lazily, at most once

    def the_index():
        if not index:
            index.append(CitationIndex(scans))
        return index[0]

    # Quotation declarations, before the anchor set is frozen.
    everything = []                              # the FALSE corpus, read lazily

    def false_corpus():
        if not everything:
            everything.append(plans_everything_text(tree, plans.texts))
        return everything[0]

    quote_failed, exempt = False, set()
    for rs in scans:
        for fs in rs.files:
            for kind, dpath, did in fs.decls:
                if kind == "STALE":
                    err("anchor-registry: FAIL - a quotation declaration is STALE.")
                    err("    %s declares %s as QUOTED-NOT-CITED, but that id appears nowhere else "
                        "in that file." % (dpath, did))
                    err("    The quotation it exempted is gone. Delete the declaration; an "
                        "exemption that outlives what it covered silences a future real citation.")
                    quote_failed = True
                    continue
                try:
                    is_false = did in false_corpus()
                except ScanError as exc:
                    err("anchor-registry: FAIL - the FALSE check could not read the plans: %s." % exc)
                    return EXIT_COLLAPSE
                if is_false:
                    err("anchor-registry: FAIL - a quotation declaration is FALSE.")
                    err("    %s declares %s as QUOTED-NOT-CITED, but that id RESOLVES in .plans/."
                        % (dpath, did))
                    err("    It names live work, so it is an ordinary citation. Remove it from the "
                        "declaration and let the ordinary check own it.")
                    quote_failed = True
                    continue
                elsewhere = the_index().leaked(did, dpath)
                if elsewhere:
                    err("anchor-registry: FAIL - a quotation declaration LEAKED.")
                    err("    %s declares %s as QUOTED-NOT-CITED, but that id is also cited in:%s"
                        % (dpath, did, "".join(" " + p for p in elsewhere)))
                    err("    One document may not exempt a name on another document's behalf. "
                        "Either those are live citations and the id needs a row, or they are "
                        "quotations and each file declares its own.")
                    quote_failed = True
                    continue
                exempt.add(did)
    if quote_failed:
        err("  A quotation declaration says 'this document QUOTES a dead id as evidence, it does "
            "not cite live work'.")
        err("  It is scoped to ONE file and it EXPIRES: that is what makes it narrower than an "
            "Allowlist entry, which")
        err("  silences a name repo-wide and forever. Repair the declaration; do not widen it.")
        return EXIT_QUOTE
    anchors -= exempt
    src_anchors = sorted(anchors)
    missing = [a for a in src_anchors if not corpus.resolves(a)]

    # Retired ids: the extraction must find at least one row, or it collapsed.
    retired = []
    for rel, text in plans.texts:
        if "/" not in rel[len(CELL_WIDTH_ROOT) + 1:] and fnmatch.fnmatchcase(
                rel[len(CELL_WIDTH_ROOT) + 1:], REGISTRY_GLOB):
            retired.extend(retired_ids(lines_of(text)))
    if len(retired) < RETIRED_FLOOR:
        err("anchor-registry: FAIL - the retired-id scan found 0 marked rows.")
        err("  This does NOT mean no id is retired - it means THIS scan collapsed (the "
            "`RETIRED-ID` token was renamed, or the registry's column layout moved).")
        err("  Refusing to report a pass. Fix the scan; do not delete the check.")
        return EXIT_COLLAPSE
    cited_retired = [rid for rid in retired if rid in anchors]
    if cited_retired:
        err("anchor-registry: FAIL - %d citation(s) name a RETIRED anchor id:" % len(cited_retired))
        for rid in cited_retired:
            err("    %s" % rid)
            for line in _retired_locator(tree, specs, rid):
                err(line)
        err("  A retired id resolves only because the plans still MENTION it. Repoint each")
        err("  citation at the live row named in the retired row's own status cell.")
        return EXIT_RETIRED

    if not missing:
        out("anchor-registry: OK (%d src anchors all resolve to plans, %d retired id(s) uncited)"
            % (len(src_anchors), len(retired)))
        return cw_status

    out("anchor-registry: FAIL - the following anchors are cited in a SCANNED ROOT")
    out("(%s - NOT src/ alone) but" % _root_names(specs))
    out("have no matching row/citation in any .plans/*.md file:")
    out("")
    idx = the_index()
    if not idx.pairs:
        out("anchor-registry: WARNING - the citation index is EMPTY, so no 'cited in:' line can "
            "be produced.")
        out("  The per-root floors above passed, so this should be impossible; the locator's scan")
        out("  (same roots, same include globs) must have stopped matching. Fix the scan.")
    for a in missing:
        out("  %s" % a)
        for p in idx.locate(a):
            out("    cited in: %s" % p)
    out("")
    out("Fix: either")
    out("  (a) add a row in .plans/_deferred-anchor-registry-production.md naming the")
    out("      trigger + closing work -- write it with DssHarness write-anchor,")
    out("      which is the only door; a hand-edited table is how the halves drift, OR")
    out("  (b) correct the citation to the id of the row that already covers it,")
    out("      confirmed with DssHarness read-anchor <ID> --json, OR")
    out("  (c) if the string is a code-internal pin and NOT deferred work, rename it")
    out("      out of the D- shape under a PIN- prefix, as the retired Allowlist was.")
    out("")
    out("Discipline: this leak recurred TWICE before this guard landed.")
    out("See .plans/_deferred-anchor-registry-production.md for the discipline rationale.")
    # A COLLAPSED scan (2) beats a missing anchor (1) beats dropped content (3).
    return EXIT_COLLAPSE if cw_status == EXIT_COLLAPSE else EXIT_MISSING


def _retired_locator(tree, specs, rid):
    """`      <path>:<line>:<text>` for every line of every scanned file carrying `rid`
    (substring, the fixed-string search the report has always printed). Failure path only,
    so the files are read again here rather than held for the whole run."""
    lines_out = []
    for root, _floor, globs in specs:
        base = os.path.join(tree, root)
        if not os.path.isdir(base):
            continue
        try:
            rels = root_files(base, globs)
        except ScanError as exc:
            lines_out.append("      %s: %s" % (root, exc))
            continue
        for rel in rels:
            shown = root + "/" + rel
            try:
                text = read_text(os.path.join(base, rel), shown)
            except ScanError as exc:
                lines_out.append("      %s" % exc)
                continue
            if rid not in text:
                continue
            for n, line in enumerate(lines_of(text), 1):
                if rid in line:
                    lines_out.append("      %s:%d:%s" % (shown, n, line))
    return lines_out


# ── output ─────────────────────────────────────────────────────────────────────────────

def _say_out(msg):
    print(msg, file=sys.stdout, flush=True)


def _say_err(msg):
    print(msg, file=sys.stderr, flush=True)


class _Capture:
    """Both streams into one list, in order -- what `2>&1` gives a caller."""

    def __init__(self):
        self.lines = []

    def __call__(self, msg):
        self.lines.append(msg)

    @property
    def text(self):
        return "\n".join(self.lines)


# ── the self-test ──────────────────────────────────────────────────────────────────────

def _fx(*segments):
    """A fixture anchor id, ASSEMBLED -- this file's bytes never carry one (see the header)."""
    return "-".join(("D",) + segments)


def _write(base, rel, text):
    path = os.path.join(base, *rel.split("/"))
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="") as fh:
        fh.write(text)
    return path


def _ordered(text, *needles):
    """True when every needle appears, in order."""
    at = 0
    for n in needles:
        at = text.find(n, at)
        if at < 0:
            return False
        at += len(n)
    return True


_LIVE = ("MINI", "LIVE")
_OLD = ("MINI", "OLD", "ID")
_REGISTRY_HEAD = ("| Anchor | Priority | Status | Trigger | Closing work | Cross-refs |\n"
                  "|---|---|---|---|---|---|\n")
_MINI_SPECS = (("src", 1, _SOURCE_GLOBS), ("examples", 1, _SOURCE_GLOBS), ("docs", 1, ("*.md",)))


def _retired_row(aid, headline):
    return ("| `%s` | P1 | ✅ CLOSED | ✅ **CLOSED 2026-01-01 — %s** | t | c |"
            % (aid, headline))


def _mini_tree(base, extra=None, drop=()):
    """A tree `verify` can judge with every floor at 1: a plan table citing the live id, a
    registry with one retired row, and the three roots citing the live id."""
    live = _fx(*_LIVE)
    files = {
        ".plans/p.md": "| id | note |\n|---|---|\n| %s | the live row |\n" % live,
        ".plans/_deferred-anchor-registry-done.md":
            _REGISTRY_HEAD + _retired_row(_fx(*_OLD), "RETIRED-ID, renamed") + "\n",
        "src/a.cpp": "// %s\n" % live,
        "examples/b.c": "/* %s */\n" % live,
        "docs/c.md": "%s\n" % live,
    }
    files.update(extra or {})
    for rel, text in sorted(files.items()):
        if rel not in drop and text is not None:
            _write(base, rel, text)
    return base


def _run_mini(base, extra=None, drop=(), cw_floors=(1, 1, 1)):
    """-> (rc, merged output) of `verify` over a fresh miniature tree."""
    tree = _mini_tree(base, extra, drop)
    cap = _Capture()
    rc = verify(tree, _MINI_SPECS, cw_floors, cap, cap)
    return rc, cap.text


def _cw(base, files, floors=(0, 0, 0)):
    """-> (result, status, report text) of the cell-width scan over `.plans/` holding `files`."""
    for rel, text in files.items():
        _write(base, ".plans/" + rel, text)
    res = cell_width_scan(PlanFiles(base))
    cap = _Capture()
    status = cell_width_report(res, floors, cap)
    return res, status, cap.text


class _Arms:
    def __init__(self):
        self.ran, self.failed, self.order, self.count = [], [], [], {}

    def run(self, label, cat, fn):
        """One arm: `fn()` -> (expected, actual). An exception fails only this arm."""
        self.ran.append(label)
        if cat not in self.count:
            self.order.append(cat)
            self.count[cat] = 0
        self.count[cat] += 1
        try:
            expected, actual = fn()
        except Exception as exc:  # an arm that cannot run is an arm that failed
            expected, actual = "the arm to run", "raised %s: %s" % (type(exc).__name__, exc)
        if expected == actual:
            return True
        _say_err("anchor-registry: SELF-TEST arm '%s' FAILED - this guard cannot be trusted to "
                 "fail." % label)
        _say_err("    expected: %s" % expected)
        _say_err("    actual  : %s" % actual)
        self.failed.append(label)
        return False

    def breakdown(self):
        """Derived from the arms that RAN -- never a literal (both predecessors printed 21
        while running 20)."""
        return ", ".join("%d %s" % (self.count[c], c) for c in self.order)


def _box(root, name):
    d = os.path.join(root, name)
    os.makedirs(d)
    return d


def self_test():
    """Red-on-disable, every arm driving the production code. -> 0 or EXIT_SELFTEST."""
    arms = _Arms()
    st = tempfile.mkdtemp(prefix="anchor-registry-selftest-")
    try:
        _arms_wrap(arms)
        _arms_quote(arms)
        _arms_root(arms, st)
        _arms_retired(arms)
        _arms_resolve(arms, st)
        _arms_cell_width(arms, st)
        _arms_file_selection(arms, st)
        _arms_end_to_end(arms, st)
        _arms_plumbing(arms, st)
        for ok, why, detail in _owning_tree().root_arms(repo_root, (Collapse,), False, __file__):
            arms.run(why, "owning-tree root",
                     lambda ok=ok, detail=detail: ("holds", "holds" if ok else detail))
    finally:
        _owning_tree().remove_tree(st)
    if len(arms.ran) != EXPECTED_ARMS:
        _say_err("anchor-registry: FAIL - the self-test ran %d arm(s); EXPECTED_ARMS says %d. An "
                 "arm that silently stops running is a property that silently stops being "
                 "proven." % (len(arms.ran), EXPECTED_ARMS))
        return EXIT_SELFTEST
    if arms.failed:
        _say_err("anchor-registry: FAIL - the self-test did not pass, so no verdict from this run "
                 "means anything.")
        return EXIT_SELFTEST
    _say_out("anchor-registry: self-test OK - %d arms (%s); this guard is PROVEN able to fail."
             % (len(arms.ran), arms.breakdown()))
    return EXIT_OK


def _arms_wrap(arms):
    cat = "wrap recovery"
    fixture = [
        "// a one-line citation " + _fx("CTL", "ONE") + " needs no recovery",
        "// wrapped, comment continuation " + _fx("WRAPA") + "-",
        "// TAILA is the rest of it",
        "# wrapped, decoration continuation " + _fx("WRAPB") + "-",
        "# -- TAILB ends it",
        "// wrapped into prose " + _fx("WRAPC") + "-",
        "// tailc is a word, not a name",
        "// an inner fragment of a longer word FIXE" + _fx("32", "BIT", "WORD") + "-",
        "// TAILD must not be reached",
    ]
    got = [name for _rel, name in join_wrapped("wrap.txt", fixture)]

    def names(lines):
        return [name for _rel, name in join_wrapped("wrap.txt", lines)]

    arms.run("wrap-join-recovers-both-continuation-shapes", cat, lambda: (
        _fx("WRAPA", "TAILA") + " " + _fx("WRAPB", "TAILB") + " ",
        "".join(n + " " for n in got)))
    arms.run("wrap-join-refuses-a-lower-case-continuation", cat, lambda: (
        "absent=True producible=True",
        "absent=%s producible=%s" % (
            not any(n.startswith(_fx("WRAPC")) for n in got),
            names([fixture[5], "// TAILC resumes in upper case"]) == [_fx("WRAPC", "TAILC")])))
    arms.run("wrap-join-refuses-a-fragment-inside-a-longer-word", cat, lambda: (
        "absent=True producible=True",
        "absent=%s producible=%s" % (
            not any("TAILD" in n for n in got),
            any("TAILD" in n for n in names(
                ["// the same tail at a boundary FIXE " + _fx("32", "BIT", "WORD") + "-",
                 fixture[8]])))))
    arms.run("wrap-join-leaves-an-unwrapped-citation-alone", cat, lambda: (
        "absent=True producible=True",
        "absent=%s producible=%s" % (
            not any(n.startswith(_fx("CTL")) for n in got),
            names(["// the same citation wrapped " + _fx("CTL") + "-", "// ONE continues"])
            == [_fx("CTL", "ONE")])))


def _arms_quote(arms):
    cat = "quotation classification"
    decl = [
        "prose that quotes " + _fx("QUO", "HERE") + " as evidence of a deleted row",
        "  > " + DECL_MARKER + " " + _fx("QUO", "HERE") + " " + _fx("QUO", "GONE")
        + " WORD-" + _fx("QUO", "INNER"),
    ]
    got = quote_records("decl.txt", decl)
    got_text = "".join("%s:%s:%s " % r for r in got)
    arms.run("quotation-declaration-classifies-cited-and-absent-ids", cat, lambda: (
        "DECL:decl.txt:" + _fx("QUO", "HERE") + " STALE:decl.txt:" + _fx("QUO", "GONE") + " ",
        got_text))
    # The marker's own tail is anchor-shaped; the raw grammar (no boundary) lifts it out.
    arms.run("quotation-marker-does-not-cite-its-own-anchor-shaped-tail", cat, lambda: (
        "absent=True producible=True",
        "absent=%s producible=%s" % (
            _fx("QUOTED") not in got_text,
            any(m.group(0).startswith(_fx("QUOTED")) for m in DECL_ID_RX.finditer(decl[1])))))
    arms.run("quotation-ids-must-sit-on-a-word-boundary", cat, lambda: (
        "absent=True producible=True",
        "absent=%s producible=%s" % (
            _fx("QUO", "INNER") not in got_text,
            any(r[2] == _fx("QUO", "INNER") for r in quote_records(
                "decl.txt", [decl[0], decl[1].replace("WORD-", "WORD- ")])))))
    mid = ["see the " + DECL_MARKER + " convention, which would exempt " + _fx("QUO", "MID")]
    arms.run("quotation-declaration-recognition-is-positional", cat, lambda: (
        "absent=True producible=True",
        "absent=%s producible=%s" % (
            quote_records("mid.txt", mid) == [],
            quote_records("mid.txt", ["  " + DECL_MARKER + " " + _fx("QUO", "MID")])
            == [("STALE", "mid.txt", _fx("QUO", "MID"))])))


def _arms_root(arms, st):
    cat = "root existence/floor"
    anchor = _fx("SELFTEST", "FIXTURE", "ANCHOR")
    base = _box(st, "roots")
    root = _box(base, "root")
    _write(root, "a.md", "cite %s here\n" % anchor)
    gone = os.path.join(base, "no-such-root")

    def missing():
        rs = scan_root(gone, gone, 1, ("*.md",), set())
        said = "\n".join(rs.msgs)
        return ("failed=True says=True", "failed=%s says=%s" % (
            not rs.ok, _ordered(said, "does not exist",
                                "refusing to report a partial scan as a pass")))

    def below():
        rs = scan_root(root, root, 99, ("*.md",), set())
        said = "\n".join(rs.msgs)
        return ("failed=True says=True", "failed=%s says=%s" % (
            not rs.ok, _ordered(said, "yielded only 1 anchors, below its floor of 99",
                                "do not lower the floor")))

    def at_floor():
        sink = set()
        rs = scan_root(root, root, 1, ("*.md",), sink)
        return ("ok=True|" + anchor, "ok=%s%s|%s" % (rs.ok, "".join(rs.msgs),
                                                     " ".join(sorted(sink))))

    arms.run("missing-root-refuses-and-says-so", cat, missing)
    arms.run("below-floor-root-refuses-and-names-the-floor", cat, below)
    arms.run("at-floor-root-passes-and-collects", cat, at_floor)


def _arms_retired(arms):
    cat = "retired-id matcher"
    r = [_fx("RET" + c, "FIXTURE", "ROW") for c in "ABCDE"]
    mark = "✅ **CLOSED 2026-01-01 — RETIRED-ID, renamed**"
    rows = [
        "| `%s` | %s | t | c |" % (r[0], mark),
        "| `%s` | \U0001F7E0 **OPEN** — the prose here merely says \"as a retired id\" | t | c |"
        % r[1],
        "| `%s` | \U0001F7E0 **OPEN** — this row DOCUMENTS the RETIRED-ID token | t | c |" % r[2],
        _retired_row(r[3], "RETIRED-ID, renamed"),
        _retired_row(r[4], "an ordinary closure"),
    ]
    got = retired_ids(rows)
    arms.run("retired-matcher-extracts-a-positionally-marked-row", cat, lambda: (
        "present=True", "present=%s" % (r[0] in got)))
    arms.run("retired-matcher-ignores-prose-that-says-retired-id", cat, lambda: (
        "absent=True producible=True",
        "absent=%s producible=%s" % (
            r[1] not in got, retired_ids(["| `%s` | %s | t | c |" % (r[1], mark)]) == [r[1]])))
    arms.run("retired-matcher-ignores-a-row-that-documents-the-token", cat, lambda: (
        "absent=True token-present=True producible=True",
        "absent=%s token-present=%s producible=%s" % (
            r[2] not in got, "RETIRED-ID" in rows[2],
            retired_ids(["| `%s` | %s | t | c |" % (r[2], mark)]) == [r[2]])))
    arms.run("retired-matcher-reads-the-SIX-cell-layout-too", cat, lambda: (
        "present=True", "present=%s" % (r[3] in got)))
    arms.run("retired-matcher-ignores-a-plain-six-cell-closure", cat, lambda: (
        "absent=True producible=True",
        "absent=%s producible=%s" % (
            r[4] not in got, retired_ids([_retired_row(r[4], "RETIRED-ID, renamed")]) == [r[4]])))


def _arms_resolve(arms, st):
    cat = "widened-core and resolve"
    vis, hdo = _fx("VIS", "ROW"), _fx("HEADONLY")
    base = _box(st, "resolve")
    visroot = _box(base, "vis")
    text = "cite %s and %s here\n" % (vis, hdo)
    _write(visroot, "vis.md", text)
    sink = set()
    scan_root(visroot, visroot, 1, ("*.md",), sink)
    arms.run("widened-core-collects-a-two-segment-id", cat, lambda: (
        vis + " ", "".join(a + " " for a in sorted(sink))))
    arms.run("widened-core-still-ignores-a-head-only-name", cat, lambda: (
        "absent=True producible=True",
        "absent=%s producible=%s" % (hdo not in sink, hdo in DECL_ID_RX.findall(text))))
    # The collection boundary, pinned directly: an anchor-shaped TAIL of a longer hyphenated
    # word (the shape of the `fixed32.hpp` phrase) is not a citation; the same tail after a
    # space is one.
    word = _fx("32", "BIT", "WORD")
    inner = _box(base, "inner")
    _write(inner, "i.md", "an inner fragment FIXE%s of a longer word\n" % word)
    spaced = _box(base, "spaced")
    _write(spaced, "s.md", "the same tail FIXE %s at a boundary\n" % word)

    def boundary():
        got_inner, got_spaced = set(), set()
        scan_root(inner, inner, 0, ("*.md",), got_inner)
        scan_root(spaced, spaced, 0, ("*.md",), got_spaced)
        return ("absent=True producible=True", "absent=%s producible=%s"
                % (not got_inner, got_spaced == {word}))

    arms.run("collection-stops-at-a-word-boundary", cat, boundary)

    def corpus(name, *texts):
        tree = _box(base, name)
        _box(tree, ".plans")
        for i, t in enumerate(texts):
            _write(tree, ".plans/p%d.md" % i, t)
        return PlanCorpus(PlanFiles(tree).texts)

    cites = corpus("cites", "the plans cite %s in prose\n" % vis)
    nowhere = _fx("VIS", "NOWHERE")
    arms.run("a-two-segment-id-the-plans-cite-resolves", cat, lambda: (
        "yes", "yes" if cites.resolves(vis) else "no"))
    arms.run("a-two-segment-id-the-plans-do-not-cite-stays-a-finding", cat, lambda: (
        "no producible=True", "%s producible=%s" % (
            "yes" if cites.resolves(nowhere) else "no",
            corpus("cites-too", "x %s\n" % vis, "and %s\n" % nowhere).resolves(nowhere))))
    longer = corpus("longer", "only the child %s is named\n" % _fx("VIS", "ROW", "CHILD"))
    arms.run("a-parent-id-resolves-through-a-more-specific-plan-name", cat, lambda: (
        "vocab-has-it=False resolves=True",
        "vocab-has-it=%s resolves=%s" % (vis in longer.vocab, longer.resolves(vis))))
    # Glued to a word character, the plan-side token is invisible to the grammar, so only
    # the text authority can answer -- the substring contract, pinned from the plan side.
    glued = corpus("glued", "an unbounded mention x%s in prose\n" % vis)
    arms.run("the-corpus-text-is-the-authority-when-the-vocabulary-cannot-answer", cat, lambda: (
        "vocab-has-it=False resolves=True",
        "vocab-has-it=%s resolves=%s" % (vis in glued.vocab or vis in glued.blob,
                                         glued.resolves(vis))))
    empty = corpus("empty")
    arms.run("an-empty-plan-corpus-resolves-nothing", cat, lambda: (
        "empty-resolves=False blank-resolves=False producible=True",
        "empty-resolves=%s blank-resolves=%s producible=%s" % (
            empty.resolves(vis), cites.resolves(""), cites.resolves(vis))))


def _arms_cell_width(arms, st):
    cat = "cell-width"
    n = [0]

    def fresh():
        n[0] += 1
        return _box(st, "cw%d" % n[0])

    head = "| h1 | h2 |\n|---|---|\n"

    def too_wide():
        res, status, text = _cw(fresh(), {"w.md": head + "| a | b |\n| a | b | c |\n"})
        line = "  %s:%d  header@%d expected=%d actual=%d" % (".plans/w.md", 4, 1, 2, 3)
        return ("status=3 offender=True dropped=True",
                "status=%d offender=%s dropped=%s" % (status, line in text.split("\n"),
                                                      "      dropped: c" in text.split("\n")))

    def escaped():
        _r1, s1, _t = _cw(fresh(), {"e.md": head + "| a | b \\| c |\n"})
        _r2, s2, _t = _cw(fresh(), {"e.md": head + "| a | b | c |\n"})
        return ("escaped=0 unescaped=3", "escaped=%d unescaped=%d" % (s1, s2))

    def short():
        res, status, _t = _cw(fresh(), {"s.md": head + "| a |\n"})
        return ("status=0 rows=1 under=1", "status=%d rows=%d under=%d"
                % (status, res.rows, res.under))

    def fenced():
        table = head + "| a | b | c |\n"
        r1, _s, _t = _cw(fresh(), {"f.md": "```text\n" + table + "```\n"})
        r2, _s, _t = _cw(fresh(), {"f.md": table})
        return ("fenced tables=0 over=0; open over=1", "fenced tables=%d over=%d; open over=%d"
                % (r1.tables, len(r1.over), len(r2.over)))

    def per_table():
        text = (head + "| 1 | 2 | 3 | 4 |\n\n"
                "| a | b | c | d |\n|---|---|---|---|\n| 1 | 2 | 3 | 4 |\n")
        res, _s, _t = _cw(fresh(), {"t.md": text})
        return ("tables=2 offenders=[(3, 2, 4)]", "tables=%d offenders=%s"
                % (res.tables, [(o[1], o[3], o[4]) for o in res.over]))

    def not_last():
        res, _s, _t = _cw(fresh(), {"a.md": head + "| 1 | 2 |\n| 1 | 2 | 3 |",
                                    "b.md": head + "| 1 | 2 |\n"})
        return ("offenders=[('.plans/a.md', 4)]", "offenders=%s"
                % [(o[0], o[1]) for o in res.over])

    floor_says = ("markdown files under", "tables, below its floor", "table data rows, below")

    def floor(which, floors):
        def arm():
            _r, status, text = _cw(fresh(), {"g.md": head + "| 1 | 2 |\n"}, floors)
            said = tuple(s in text for s in floor_says)
            want = tuple(i == which for i in range(3))
            return ("status=2 said=%s" % (want,), "status=%d said=%s" % (status, said))
        return arm

    def shown():
        s = "─" * 100 + "a" + _PIPE_PLACEHOLDER + "b" + "x" * 150
        cut_first = _NOT_PRINTABLE_ASCII_RX.sub("", s[:PREVIEW_MAX])
        return ("display=%r order-matters=True" % (("a|b" + "x" * 150)[:PREVIEW_MAX],),
                "display=%r order-matters=%s" % (display(s), cut_first != display(s)))

    def root_missing():
        cap = _Capture()
        status = cell_width_report(cell_width_scan(PlanFiles(fresh())), (0, 0, 0), cap)
        return ("status=2 says=True", "status=%d says=%s" % (
            status, "cell-width root '.plans' does not exist" in cap.text))

    arms.run("cell-width-root-missing-refuses", cat, root_missing)
    arms.run("cell-width-too-wide-row-fails-3-naming-its-line", cat, too_wide)
    arms.run("cell-width-escaped-pipe-is-content-and-the-unescaped-row-fails", cat, escaped)
    arms.run("cell-width-short-row-is-counted-never-fatal", cat, short)
    arms.run("cell-width-fenced-table-ignored-the-same-table-open-fails", cat, fenced)
    arms.run("cell-width-each-table-takes-its-own-header-width", cat, per_table)
    arms.run("cell-width-last-line-of-a-non-last-file-is-judged", cat, not_last)
    arms.run("cell-width-file-floor-refuses-alone", cat, floor(0, (5, 0, 0)))
    arms.run("cell-width-table-floor-refuses-alone", cat, floor(1, (0, 5, 0)))
    arms.run("cell-width-row-floor-refuses-alone", cat, floor(2, (0, 0, 50)))
    arms.run("cell-width-display-strips-non-ascii-before-the-cut", cat, shown)


def _arms_file_selection(arms, st):
    cat = "file selection"
    base = _box(st, "select")
    a = [_fx("SEL", str(i), "ANCHOR") for i in range(6)]

    def collected(root, globs=("*.md",)):
        sink = set()
        scan_root(root, root, 1, globs, sink)
        return sink

    dot = _box(base, "dot")
    _write(dot, "sub/.dss-project.json", '{"cites": "%s"}\n' % a[0])
    arms.run("a-dotfile-is-collected", cat, lambda: (
        "True", str(a[0] in collected(dot, ("*.json",)))))

    pruned = _box(base, "pruned")
    _write(pruned, "keep/k.md", "%s\n" % a[1])
    for d in sorted(EXCLUDE_DIRS):
        _write(pruned, d + "/copy/x.md", "%s\n" % a[2])
    _write(pruned, "keep/worktrees-notes/y.md", "%s\n" % a[3])
    arms.run("excluded-directories-below-a-root-are-pruned-by-exact-name", cat, lambda: (
        "kept=True pruned=True near-name-kept=True",
        "kept=%s pruned=%s near-name-kept=%s" % (
            a[1] in collected(pruned), a[2] not in collected(pruned),
            a[3] in collected(pruned))))

    checkout = _box(_box(base, "worktrees"), "checkout")
    _write(checkout, "src/z.md", "%s\n" % a[4])
    arms.run("a-checkout-whose-ancestor-is-named-worktrees-still-scans", cat, lambda: (
        "True", str(a[4] in collected(os.path.join(checkout, "src")))))

    nested = _box(base, "nested")
    _write(nested, "sub/deep/x.md", "%s\n" % a[5])
    arms.run("a-nested-file-matches-its-glob", cat, lambda: (
        "True", str(a[5] in collected(nested))))

    upper = _box(base, "upper")
    _write(upper, "X.MD", "%s\n" % a[0])
    arms.run("include-globs-match-case-insensitively", cat, lambda: (
        "collected=True case-sensitive-would-drop=True",
        "collected=%s case-sensitive-would-drop=%s" % (
            a[0] in collected(upper), not fnmatch.fnmatchcase("X.MD", "*.md"))))


def _arms_end_to_end(arms, st):
    cat = "end-to-end verdict"
    n = [0]

    def fresh():
        n[0] += 1
        return _box(st, "e2e%d" % n[0])

    live, old = _fx(*_LIVE), _fx(*_OLD)
    nowhere, quoted = _fx("MINI", "NOWHERE"), _fx("MINI", "QUOTED")

    def decl_file(aid):
        return "prose that quotes %s\n  %s %s\n" % (aid, DECL_MARKER, aid)

    def green():
        rc, text = _run_mini(fresh())
        return ("rc=0 ok=True", "rc=%d ok=%s" % (rc, (
            "anchor-registry: OK (%d src anchors all resolve to plans, %d retired id(s) uncited)"
            % (1, 1)) in text.split("\n")))

    def missing():
        rc, text = _run_mini(fresh(), {"src/m.cpp": "// %s\n" % nowhere})
        return ("rc=1 named=True located=True", "rc=%d named=%s located=%s" % (
            rc, ("  " + nowhere) in text.split("\n"), "    cited in: src/m.cpp" in text))

    def retired():
        rc, text = _run_mini(fresh(), {"docs/r.md": "see %s\n" % old})
        return ("rc=4 said=True located=True", "rc=%d said=%s located=%s" % (
            rc, "1 citation(s) name a RETIRED anchor id" in text,
            ("      %s:%d:see %s" % ("docs/r.md", 1, old)) in text.split("\n")))

    def retired_collapse():
        rc, text = _run_mini(fresh(), {".plans/_deferred-anchor-registry-done.md":
                                       _REGISTRY_HEAD + _retired_row(old, "an ordinary closure")
                                       + "\n"})
        return ("rc=2 said=True", "rc=%d said=%s" % (
            rc, "the retired-id scan found 0 marked rows" in text))

    def collapse_beats_missing():
        rc, text = _run_mini(fresh(), {"src/m.cpp": "// %s\n" % nowhere}, cw_floors=(99, 1, 1))
        return ("rc=2 both-reported=True", "rc=%d both-reported=%s" % (
            rc, "cell-width scan found only" in text and ("  " + nowhere) in text))

    def missing_beats_cells():
        rc, text = _run_mini(fresh(), {
            "src/m.cpp": "// %s\n" % nowhere,
            ".plans/p.md": "| id | note |\n|---|---|\n| %s | a | surplus |\n" % live})
        return ("rc=1 both-reported=True", "rc=%d both-reported=%s" % (
            rc, "carry MORE cells" in text and ("  " + nowhere) in text))

    def retired_beats_collapse():
        rc, _text = _run_mini(fresh(), {"docs/r.md": "see %s\n" % old}, cw_floors=(99, 1, 1))
        return ("rc=4", "rc=%d" % rc)

    def quote_false():
        rc, text = _run_mini(fresh(), {"docs/q.md": decl_file(live)})
        return ("rc=5 said=True", "rc=%d said=%s" % (
            rc, "a quotation declaration is FALSE" in text))

    def quote_leaked():
        rc, text = _run_mini(fresh(), {"docs/q.md": decl_file(quoted),
                                       "src/l.cpp": "// %s\n" % quoted})
        return ("rc=5 said=True", "rc=%d said=%s" % (
            rc, "declares %s as QUOTED-NOT-CITED, but that id is also cited in: src/l.cpp"
            % quoted in text))

    def quote_exempts():
        rc_with, _t = _run_mini(fresh(), {"docs/q.md": decl_file(quoted)})
        rc_without, text = _run_mini(fresh(), {"docs/q.md": "prose that quotes %s\n" % quoted})
        return ("with=0 without=1 named=True", "with=%d without=%d named=%s" % (
            rc_with, rc_without, ("  " + quoted) in text.split("\n")))

    def quote_stale():
        rc, text = _run_mini(fresh(), {"docs/s.md": "  %s %s\n" % (DECL_MARKER,
                                                                   _fx("MINI", "GONE"))})
        return ("rc=5 said=True", "rc=%d said=%s" % (
            rc, "a quotation declaration is STALE" in text))

    def every_root_reported():
        rc, text = _run_mini(fresh(), drop=("examples/b.c", "docs/c.md"))
        return ("rc=2 examples=True docs=True", "rc=%d examples=%s docs=%s" % (
            rc, "scan root 'examples' does not exist" in text,
            "scan root 'docs' does not exist" in text))

    arms.run("e2e-green-control", cat, green)
    arms.run("e2e-missing-anchor-fails-1-with-its-location", cat, missing)
    arms.run("e2e-retired-id-cited-fails-4-with-its-location", cat, retired)
    arms.run("e2e-retired-extraction-collapse-fails-2", cat, retired_collapse)
    arms.run("e2e-cell-width-collapse-outranks-a-missing-anchor", cat, collapse_beats_missing)
    arms.run("e2e-a-missing-anchor-outranks-dropped-content", cat, missing_beats_cells)
    arms.run("e2e-a-retired-citation-is-not-outranked-by-a-cell-collapse", cat,
             retired_beats_collapse)
    arms.run("e2e-quotation-false-fails-5", cat, quote_false)
    arms.run("e2e-quotation-leaked-fails-5", cat, quote_leaked)
    arms.run("e2e-a-valid-quotation-exempts-and-without-it-the-id-is-missing", cat, quote_exempts)
    arms.run("e2e-quotation-stale-fails-5", cat, quote_stale)
    arms.run("e2e-every-failing-root-is-reported-before-exiting", cat, every_root_reported)


def _arms_plumbing(arms, st):
    cat = "fail-closed plumbing"
    arms.run("the-floors-are-the-declared-constants", cat, lambda: (
        "roots=[400, 150, 8] cells=(20, 100, 1500) retired=1",
        "roots=%s cells=%s retired=%d" % ([f for _r, f, _g in ROOT_SPECS], CELL_WIDTH_FLOORS,
                                          RETIRED_FLOOR)))

    def crash():
        cap = _Capture()
        rc = _guarded(lambda: 1 // 0, cap)
        return ("rc=2 said=True", "rc=%d said=%s" % (rc, "crashed" in cap.text))

    arms.run("a-crash-exits-2-never-1", cat, crash)
    arms.run("any-argument-is-a-usage-error", cat, lambda: (
        "bogus=refused none=accepted",
        "bogus=%s none=%s" % ("refused" if usage_error(["--bogus"]) else "accepted",
                              "refused" if usage_error([]) else "accepted")))

    def unreadable():
        box = _box(st, "unreadable")
        path = _write(box, "gone.md", "x\n")
        os.remove(path)
        try:
            read_text(path, "gone.md")
            return ("ScanError naming gone.md", "no error")
        except ScanError as exc:
            return ("ScanError naming gone.md", "ScanError naming gone.md"
                    if "gone.md" in str(exc) else str(exc))

    def unlistable():
        box = _box(st, "unlistable")
        path = _write(box, "a-file.md", "x\n")
        try:
            _walk_files(path, frozenset())
            return ("ScanError", "no error")
        except ScanError:
            return ("ScanError", "ScanError")

    arms.run("an-unreadable-file-is-a-collapse-not-a-skip", cat, unreadable)
    arms.run("a-directory-the-walk-cannot-list-is-a-collapse", cat, unlistable)


# ── the command ────────────────────────────────────────────────────────────────────────

def usage_error(argv):
    """Decision (h): the guard has ONE form; any argument is refused rather than ignored."""
    if argv:
        return ("check-anchor-registry: this guard takes no argument (got: %s). It always runs "
                "its self-test and then the verdict on the tree it lives in."
                % " ".join(argv))
    return None


def _guarded(fn, err=None):
    """Run `fn`; a crash is exit 2 with its traceback -- never 1, which is a verdict."""
    err = err or _say_err
    try:
        return fn()
    except Exception as exc:
        err("anchor-registry: FAIL - the guard crashed (%s: %s); a crash is never a verdict."
            % (type(exc).__name__, exc))
        err(traceback.format_exc().rstrip("\n"))
        return EXIT_COLLAPSE


def main(argv):
    why = usage_error(argv)
    if why:
        _say_err(why)
        return EXIT_COLLAPSE

    def body():
        try:
            tree = repo_root()
        except Collapse as exc:
            _say_err("check-anchor-registry: FAIL -- %s" % exc)
            return EXIT_COLLAPSE
        if self_test() != EXIT_OK:
            return EXIT_SELFTEST
        return verify(tree)

    return _guarded(body)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
