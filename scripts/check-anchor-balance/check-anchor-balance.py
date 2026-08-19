#!/usr/bin/env python3
# PURPOSE: refuse a cycle that ends with more OPEN deferral-registry rows than it began.
"""Anchor balance gate: a cycle may not end with more OPEN deferral rows than it began.

WHY THIS IS A SCRIPT AND NOT AN INLINE GREP
-------------------------------------------
The first version of this gate was an inline
    grep -cE '\\| `D-[A-Z0-9-]+` \\| (RED|ORANGE|WARN)'
which ENUMERATED the glyphs that mean open. It was blind to the hourglass, and the
hourglass is what `D-OPT6-LICM-SPECULATIVE-LOAD-HOIST` carried -- so the gate saw
269 open rows where there were 579, and a cycle could close one orange row, open one
hourglass row, and be congratulated for it.

So the rule here is INVERTED and that inversion is the whole point: a row is OPEN
unless its status cell carries an explicit CLOSED marker. A glyph nobody has thought
of yet counts as open, which is the safe direction -- the gate over-reports rather
than waving work through. Enumerating the open glyphs is the same mistake as
enumerating build-directory layouts: define the complement, not the variants.

The comparison is by ROW NAME, not by count, because "which rows moved" is the
question a reader actually has, and a count cannot answer it.

star star star WHAT THE DENOMINATOR IS, AND WHY IT IS NOT JUST THE REGISTRY (2026-08-13)
------------------------------------------------------------------------------------
`dss-cycle` SKILL.md section F.2 sanctions MORE THAN ONE HOME for a deferral:
a feature-area anchor belongs in its plan's own deferred-items table, a project-level
known-open item in plan-00 section 0.2, an orphan/cross-cutting anchor in
`_deferred-anchor-registry.md`.  Section F.4 then lets a `src/` citation resolve to
EITHER home.  This tool used to count ONLY registry rows -- so a cycle that closed a
registry row and deferred the actual work into a plan-side row was reported as an
IMPROVEMENT.  That is `D-GATE-BALANCE-COUNTS-ONLY-THE-REGISTRY`, and it is the THIRD
time this instrument has flattered the cycle (see the direction note in is_closed()).

The fix is to count EVERY SANCTIONED HOME, so that MOVING a deferral between homes is
arithmetically NEUTRAL instead of looking like progress.  ✔MEASURED 2026-08-13: the
plan-side homes hold 378 distinct anchors, only 8 of which also have a registry row --
so this is a real population, not a rounding error.

star THE ONE-TIME JUMP IS DECOMPOSED, NEVER SILENT.  Widening a denominator makes the
headline number leap for entirely historical reasons.  The report therefore always
prints registry-side and plan-side counts SEPARATELY and names which denominator
gated, so the jump is attributable rather than mysterious.  Note that the gate itself
does NOT need a stored baseline: both sides of the comparison are counted with the
same rule, so widening changes the HEADLINE but not the DELTA.  `--denominator
registry` reproduces the old headline on demand.

star HOW TABLES ARE RECOGNIZED -- BY HEADER SHAPE, NEVER BY HEADING NUMBER.
The obvious implementation is "parse the table under each plan's section 3.1".  It is
wrong, and ✔MEASURED wrong on this tree:
  - `17-shader-gpu-plan` keeps its anchor table under section **5.4**, not 3.1;
  - `09.5`, `24`, `28` keep theirs under sections **9 / 6 / 12**;
  - `17-shader-gpu-plan` section **3.1 is not an anchor table at all** (it is a
    "Tier | Example | External tools?" prose table);
  - `23-full-c-plan` section **3.1 is not even a table** -- it is a prose paragraph
    that happens to name a dozen anchors.
Heading numbers are decoration; the COLUMN SHAPE is the contract.  So tables are
matched on their normalized header cells, and anything unrecognized that still looks
like an anchor table is REPORTED AND FAILS THE RUN (the severity rule lives in
scan_document's docstring: FATAL iff the MEASUREMENT is incomplete) -- a silently
skipped table is the exact defect this whole anchor is about.
"""
import argparse
import io
import os
import re
import subprocess
import sys

PLANS_DIR = ".plans"
REG_REL = ".plans/_deferred-anchor-registry.md"

# The ONLY marker that means closed. Everything else in a status cell is open.
CLOSED_MARK = "✅"  # white heavy check mark
# A row that DISCLOSES debt which already existed rather than CREATING new debt.
# Operator ruling 2026-08-14: "the balance gate forbids a cycle that OPENS NEW debt;
# it does not forbid a cycle that DISCLOSES PRE-EXISTING debt. Those are different
# quantities and the gate should count them separately." Before this, the gate
# counted them as one -- so a cycle that honestly wrote up a defect it merely FOUND
# was punished exactly like one that shipped new deferrals, and the cheapest way to
# pass was to not write the row. That is the precise dishonesty this gate exists to
# prevent, produced BY the gate. A disclosed row is still OPEN work and still counts
# in every total; it is exempt only from the net-increase FAILURE.
# star IT IS NOT A LOOPHOLE, AND THE ASYMMETRY IS THe POINT: the marker asserts the
# defect PRE-DATES this cycle, which is checkable -- the reviewer can look for it in
# the base ref. Marking a defect you introduced is a false statement about history,
# not a formatting choice.
DISCLOSED_MARK = "🔵"  # large blue circle

# A markdown table separator row: | --- | :--: | ... |
SEP_ROW = re.compile(r"^\s*\|[\s:|-]+\|\s*$")

# An HTML comment occupying whole lines. ✔MEASURED 2026-08-13: one sits INSIDE the
# body of `22-optimizer-plan` section 3.1 (line 387), severing nine anchor rows from
# their header. See the "transparent, but never silent" note in scan_document().
COMMENT_OPEN = re.compile(r"^\s*<!--")
COMMENT_CLOSE = re.compile(r"-->\s*$")

# star THE ANCHOR TOKEN IS DELIBERATELY WIDER THAN THE OLD ROW REGEX.
# The previous gate identified rows with `^\| `(D-[A-Z0-9-]+)` \|` -- backticked, no
# underscore, no dot, no strikethrough. ✔MEASURED 2026-08-13: that shape was blind to
# TEN registry rows, two of them OPEN (`D-TEST-QEMU_LD_PREFIX-AMBIENT-ONLY` and
# `D-TEST-CORPUS-NO-QEMU-X86_64-ON-ARM64-HOST` -- both carry an UNDERSCORE, which the
# old character class excluded). An open row the gate cannot SEE is a hole an opened
# row can slip through, so the shape now admits `_`, `.` and decoration.
# This token is used for two jobs only: NAMING a row, and deciding whether an
# UNRECOGNIZED table looks like an anchor table. Inclusion of a row in a RECOGNIZED
# table never depends on it -- every data row of a recognized deferral table counts,
# so no row can be invisible because of how it is spelled.
ANCHOR_TOKEN = re.compile(r"D-[A-Za-z0-9_]+(?:[-.][A-Za-z0-9_]+)+")


class Shape(object):
    """A recognized deferral-table shape: how to find its status cell."""

    def __init__(self, kind, status_col):
        self.kind = kind
        # 1-based index into the row's cells, or None when the shape HAS no status
        # cell (see RESERVED below).
        self.status_col = status_col


# ── The sanctioned deferral-table shapes, ✔MEASURED across all 39 files in .plans/ ──
#
# 1. REGISTRY-SHAPED  `| Anchor | Trigger | Closing work | Cross-refs |`
#    `_deferred-anchor-registry.md` (both "Anchor Index" tables) and
#    `17-shader-gpu-plan` section 5.4, which says so in prose: "Column shape matches
#    `_deferred-anchor-registry.md`". The HEADER says cell 2 is "Trigger"; in PRACTICE
#    every row leads cell 2 with its status, and that de-facto usage is the contract
#    the old gate already relied on.
#
# 2. DEFERRED-ITEMS  `| # | Deferred item | ... |`
#    Plans 12 / 13 / 14 / 22 (their section 3.1 "Deferred-items registry" tables) and
#    plan-00 section 0.2. The TAIL columns differ between plans and that is fine --
#    only the first two cells are load-bearing:
#       12: `# | Deferred item | Source | Real blocker | Landing window`
#       13/14/22: `# | Deferred item | Why deferred (not a silent gap) | Owner / closure | Trigger`
#       00 s0.2: `# | Deferred item | Why deferred (not a silent gap) | Class | Owner / closure | Trigger`
#    So the match is a PREFIX match on ("#", "deferred item"), not an exact header.
#    ⚠ plan-00 section 0.2 numbers its rows `D1`, `D2` ... -- NOT `D-*-*-*` at all. No
#    anchor regex would ever have found them; counting every data row of a recognized
#    table is what makes them visible.
#
# 3. RESERVED  `| Anchor | Owns |`
#    Plans 09.5 section 9, 24 section 6, 28 section 12 -- anchors reserved for a plan
#    that has not opened yet, which those plans say "move into
#    `_deferred-anchor-registry` as active rows" when it does. There is NO status
#    column, so `status_col` is None and every row is unconditionally OPEN. That is
#    both the safe direction AND the correct one: it is what makes the eventual MOVE
#    into the registry arithmetically neutral instead of a +1 regression.
SHAPES_EXACT = {
    ("anchor", "trigger", "closing work", "cross-refs"): Shape("registry", 2),
    ("anchor", "owns"): Shape("reserved", None),
}
SHAPES_PREFIX = [
    (("#", "deferred item"), Shape("deferred-items", 2)),
]

# ── Tables that carry an anchor-looking token in cell 1 but are NOT deferral tables ──
# NAMED, not guessed. Each was read before being listed here:
#   `pr | title | scope`        13-assembler section 3 PR breakdown; one row's cell 1 is
#                               "~~AS3~~ ... (substrate slice + binary ops + D-ML7-2.1)"
#                               -- a PR title that MENTIONS an anchor.
#   `phase | what | closes`     23-full-c cluster tables; the anchors live in the
#                               "Closes" column, and cell 1 is a phase id like "**FC8**".
#   `pattern | reason`          the registry's own "Allowlist (code-internal pins, NOT
#                               deferrals)" table -- it says NOT DEFERRALS on the tin.
#   `order | gap to address | resolution`   v2-gap-catalog; cell 1 is an ordinal.
# ⚠ THIS LIST IS AN ENUMERATION, AND ENUMERATIONS ARE HOW THIS GATE WENT WRONG TWICE.
# It is safe here ONLY because it is not the complement of anything: a header that is
# in NEITHER the recognized nor the excluded list is not ignored, it FAILS THE RUN.
# The residual is loud, so the enumeration cannot hide a table -- it can only decide
# whether a KNOWN table is counted. Keep it that way.
EXCLUDED_HEADERS = {
    ("pr", "title", "scope"),
    ("phase", "what", "closes"),
    ("pattern", "reason"),
    ("order", "gap to address", "resolution"),
    # ── `.plans/_handoff.md` prose tables (added 2026-08-13, read before adding) ──
    # The handoff is a STATE document, not an anchor home: no row of these tables
    # declares a deferral, and none has a status cell for this gate to read. They
    # trip the "looks like an anchor table" heuristic only because a first cell may
    # MENTION a `D-*` name -- e.g. "FC18 — D-DIAG-CORPUS-EVERY-CODE" naming the
    # phase, or a priority row naming the anchor that blocks it.
    # ★ Excluded BY HEADER SHAPE, deliberately, NOT by filename. If a future handoff
    # ever grows a real 4-column anchor table, it still gets counted -- excluding the
    # whole file would have created exactly the silent skip this gate exists to stop.
    ("destination", "the named gap"),
    ("pr", "branch", "what it is doing", "last update"),
    ("date", "commit", "what shipped", "gate"),
}


def strip_decoration(text):
    """Drop markdown emphasis/strikethrough/code ticks and collapse whitespace.

    ⚠ Does NOT lowercase. Row IDENTITY must preserve case -- lowercasing it once
    silently mapped `D-B` to `d-b`, which the self-test caught before it shipped.
    Header MATCHING lowercases separately, in norm_cell().
    """
    return " ".join(text.replace("*", "").replace("`", "").replace("~", "").split())


def norm_cell(text):
    """Strip markdown decoration, collapse whitespace, fold case: for header matching."""
    return strip_decoration(text).lower()


def split_row(line):
    """Cells of a markdown table row, 1-based (index 0 is the empty pre-pipe lead)."""
    return line.split("|")


def is_closed(cell):
    """A status cell is CLOSED iff it OPENS with the closure mark.

    Everything else -- every other glyph, every novel glyph, no glyph at all -- is
    OPEN.  The complement is defined, never the variants, so a marker nobody has
    thought of yet counts as open, which is the safe direction.

    star **THE LEADING-POSITION TEST IS THE FIX FOR A MEASURED UNDERCOUNT, 2026-08-12.**
    This previously asked `CLOSED_MARK not in status` -- the mark ANYWHERE in the cell.
    Open rows legitimately use the check mark mid-prose to flag a DONE HALF or a piece
    of good news, e.g. D-EXAMPLES-RELEASE-ARM-NOT-COMPILED-WHEN-ITS-BASELINE-DID-NOT-RUN
    opens `orange **OPEN -- normal ...` and later says `... the arms themselves are GOOD`
    with a check mark.  The substring test read that as CLOSED.  MEASURED at the time of
    the fix by running BOTH rules over the registry at HEAD: the substring rule reported
    577 open, the leading-position rule reports 642 -- EXACTLY 65 rows hidden, 11.3%.
    star CORRECTED 2026-08-12, hours after this docstring was first written, by an
    independent audit.  It originally read "49 rows ... of which ~39 were genuinely open
    ... ~7%".  Those came from a scratch regex that only matched rows whose cell HEAD
    carried an EXPLICIT open glyph -- so rows with no marker at all plus a mid-prose check
    mark were invisible to the very scan that was measuring the blind spot.  The registry
    row D-GATE-ANCHOR-BALANCE-DECORATIVE-CLOSE-MARK-UNDERCOUNTS carried the right numbers
    and this docstring did not, which is the worse way round: this is the surface a future
    maintainer reads FIRST.  Measure with the tool, never with a scratch grep that may
    share the bug you are hunting.
    star star AND NOTE THE DIRECTION, BECAUSE IT IS NOW THE SAME DIRECTION THREE TIMES.
    Version 1 of this gate enumerated the OPEN glyphs and was blind to the hourglass,
    reporting 269 open where there were 579.  That was fixed by inverting to "open unless
    closed" -- and the inversion introduced the mid-prose blind spot (577 vs 642).  Fixing
    THAT left the denominator itself too narrow: registry-only, blind to every plan-side
    home (`D-GATE-BALANCE-COUNTS-ONLY-THE-REGISTRY`, 2026-08-13).  All three errors
    flattered the cycle by hiding open work.  An instrument that keeps failing toward
    "you are doing fine" is the one to distrust; when you change this rule, ask which
    direction the new version errs in and pin it in self_test().

    ⚠ The leading-strip set is `*_ ` and is deliberately UNCHANGED from the 2026-08-12
    version, so `--denominator registry` reproduces the historical headline byte for
    byte. ✔MEASURED 2026-08-13: no status cell in ANY recognized table begins with a
    strikethrough, so widening the strip set would buy nothing and would only risk
    turning an open row closed.
    """
    return cell.lstrip().lstrip("*_ ").startswith(CLOSED_MARK)


def is_disclosed(cell):
    """A status cell DISCLOSES pre-existing debt iff it opens with the disclosed mark.

    Same leading-position test as is_closed, and for the same reason: a mark anywhere
    in the prose would let a row that merely MENTIONS disclosure claim the exemption.
    """
    return cell.lstrip().lstrip("*_ ").startswith(DISCLOSED_MARK)


def row_name(cell1):
    """Stable identity for a row: its anchor token, else its decoration-stripped cell.

    Decoration is stripped so that STRIKING a row through (`D1` -> `~~D1~~`, the house
    style for "landed") does not read as one row closing and a different row opening.

    ⓘ The fallback is not a defect to be regexed away. ANCHOR_TOKEN deliberately
    requires two segments, so a hypothetical `D-B` and plan-00's real `D7` both take
    the fallback -- and the fallback returns exactly `D-B` / `D7` anyway. Widening the
    token to admit one-segment names would buy nothing and would start matching prose.
    """
    m = ANCHOR_TOKEN.search(cell1)
    if m:
        return m.group(0)
    return strip_decoration(cell1)[:80] or "<blank>"


def scan_document(text, relpath):
    """-> (rows, unrecognized)

    rows          : {"relpath#name": (is_open, status_excerpt, shape_kind)}
    findings      : [(relpath, line_no, severity, what)]

    star THE SEVERITY RULE, AND IT IS NOT A SOFTENING: **FATAL iff the MEASUREMENT is
    incomplete.** An unrecognized table shape and an orphan row both mean rows exist
    that this gate could not count -- the instrument cannot do its job, so the run
    fails. A table interrupted by an HTML comment is different in kind: those rows WERE
    counted, the denominator is whole, and what remains is a rendering defect in
    someone else's file. Making that fatal would conflate "I cannot measure" with "your
    markdown is untidy", and a gate whose failures mean two different things gets
    ignored for both. It is printed on EVERY run and carries its own anchor instead.

    Every data row of a RECOGNIZED table is counted. Recognition is by column shape,
    never by section number -- see the module docstring for why that distinction was
    measured, not assumed.

    star THE ORPHAN CHECK EXISTS BECAUSE THIS SCANNER IS TABLE-BASED AND ITS PREDECESSOR
    WAS LINE-BASED.  The old gate matched `^\\| `D-X` \\|` anywhere in the file, so a row
    that had drifted outside its table -- a broken separator, a stray blank line, a row
    pasted under a heading -- was still counted.  A table-based reader would drop it
    SILENTLY, trading one blind spot for another.  So any line that looks like an anchor
    row and was not consumed as part of some table is reported and fails the run.
    (`D-PLANS-REGISTRY-MALFORMED-ROW-CELLS` records that malformed rows really do occur
    in this registry, so this is a live hazard, not a hypothetical one.)
    """
    rows = {}
    findings = []
    lines = text.split("\n")
    consumed = set()
    i = 0
    while i < len(lines):
        line = lines[i]
        if not (line.lstrip().startswith("|")
                and i + 1 < len(lines) and SEP_ROW.match(lines[i + 1])):
            i += 1
            continue

        header = tuple(norm_cell(c) for c in split_row(line)[1:-1])
        j = i + 2
        data = []
        consumed.add(i)
        consumed.add(i + 1)
        while j < len(lines):
            if lines[j].lstrip().startswith("|"):
                data.append(lines[j])
                consumed.add(j)
                j += 1
                continue
            # star TRANSPARENT FOR COUNTING, BUT NEVER SILENT.
            # A whole-line HTML comment inside a table body ends the table as far as a
            # markdown RENDERER is concerned, which would strand every row beneath it.
            # Dropping those rows would understate the denominator -- the very failure
            # this gate exists to prevent -- so the comment is stepped over and the
            # rows are COUNTED. The interruption is reported as a WARN on every run,
            # not a FATAL, because nothing was lost from the count: see the severity
            # rule in this function's docstring (FATAL iff the MEASUREMENT is
            # incomplete). The markdown really is malformed and the fix -- move the
            # comment above or below the table -- belongs to whoever owns the plan.
            # Counting them is not forgiving the defect; it is refusing to let the
            # defect corrupt the measurement while it is being fixed.
            if COMMENT_OPEN.match(lines[j]):
                k = j
                while k < len(lines) and not COMMENT_CLOSE.search(lines[k]):
                    k += 1
                if k < len(lines) and k + 1 < len(lines) \
                        and lines[k + 1].lstrip().startswith("|"):
                    for c in range(j, k + 1):
                        consumed.add(c)
                    findings.append(
                        (relpath, j + 1, "WARN",
                         "TABLE BODY INTERRUPTED BY AN HTML COMMENT (rows below it "
                         "were counted, but they render OUTSIDE the table)"))
                    j = k + 1
                    continue
            break

        shape = SHAPES_EXACT.get(header)
        if shape is None:
            for prefix, cand in SHAPES_PREFIX:
                if header[:len(prefix)] == prefix:
                    shape = cand
                    break

        if shape is not None:
            for raw in data:
                cells = split_row(raw)
                if len(cells) < 3:
                    continue
                name = row_name(cells[1])
                if shape.status_col is None:
                    # No status column exists in this shape, so nothing can close a
                    # row in place. Unconditionally OPEN -- and a check mark sitting
                    # in some OTHER column must NOT close it (pinned in self_test).
                    opened, excerpt = True, "(reserved anchor - no status column)"
                else:
                    status = cells[shape.status_col] if len(cells) > shape.status_col else ""
                    opened = not is_closed(status)
                    excerpt = " ".join(status.split())[:80]
                # Only OPEN rows are recorded, so when one name carries several rows in
                # the same file (the registry has a few), the name counts as OPEN if
                # ANY of its rows is open. Same behaviour as the predecessor, and it is
                # the safe direction: a closed duplicate cannot mask an open original.
                if opened:
                    rows["%s#%s" % (relpath, name)] = (True, excerpt, shape.kind)
        elif header not in EXCLUDED_HEADERS:
            # Unrecognized shape. Only a table that LOOKS like an anchor table is a
            # finding -- ordinary prose tables are none of this gate's business.
            if any(len(split_row(r)) > 1 and ANCHOR_TOKEN.search(split_row(r)[1])
                   for r in data):
                findings.append((relpath, i + 1, "FATAL",
                                 "UNRECOGNIZED TABLE SHAPE: "
                                 + " ".join(line.split())[:110]))
        i = j

    for n, raw in enumerate(lines):
        if n in consumed or not raw.lstrip().startswith("|"):
            continue
        cells = split_row(raw)
        if len(cells) > 2 and ANCHOR_TOKEN.search(cells[1]):
            findings.append((relpath, n + 1, "FATAL",
                             "ORPHAN ANCHOR ROW (in no table): "
                             + " ".join(raw.split())[:100]))
    return rows, findings


def repo_root():
    p = subprocess.run(["git", "rev-parse", "--show-toplevel"],
                       capture_output=True, text=True, encoding="utf-8", errors="replace")
    if p.returncode != 0:
        sys.exit("not inside a git repository")
    return p.stdout.strip()


def plan_files_at(root, ref):
    """Depth-1 `.plans/*.md` as of `ref`. Fails loud rather than scanning nothing."""
    p = subprocess.run(["git", "ls-tree", "--name-only", "%s:%s" % (ref, PLANS_DIR)],
                       cwd=root, capture_output=True, text=True,
                       encoding="utf-8", errors="replace")
    if p.returncode != 0:
        sys.exit("cannot list %s at %s: %s" % (PLANS_DIR, ref, (p.stderr or "").strip()[:200]))
    names = [n for n in p.stdout.split("\n") if n.strip().endswith(".md")]
    if not names:
        sys.exit("no .md files found under %s at %s - the scan collapsed, which is "
                 "NOT the same as a clean tree" % (PLANS_DIR, ref))
    return ["%s/%s" % (PLANS_DIR, n.strip()) for n in names]


def scan_at_ref(root, ref):
    rows, findings = {}, []
    for rel in plan_files_at(root, ref):
        p = subprocess.run(["git", "show", "%s:%s" % (ref, rel)], cwd=root,
                           capture_output=True, text=True, encoding="utf-8", errors="replace")
        if p.returncode != 0:
            sys.exit("cannot read %s at %s: %s" % (rel, ref, (p.stderr or "").strip()[:200]))
        r, u = scan_document(p.stdout, rel)
        rows.update(r)
        findings.extend(u)
    return rows, findings


def scan_worktree(root):
    rows, findings = {}, []
    plans = os.path.join(root, PLANS_DIR)
    if not os.path.isdir(plans):
        sys.exit("no %s directory in %s" % (PLANS_DIR, root))
    names = sorted(n for n in os.listdir(plans) if n.endswith(".md"))
    if not names:
        sys.exit("no .md files under %s - the scan collapsed, which is NOT a clean tree"
                 % PLANS_DIR)
    for n in names:
        rel = "%s/%s" % (PLANS_DIR, n)
        with io.open(os.path.join(plans, n), encoding="utf-8") as fh:
            r, u = scan_document(fh.read(), rel)
        rows.update(r)
        findings.extend(u)
    return rows, findings


def is_registry(key):
    return key.startswith(REG_REL + "#")


def split_homes(rows):
    reg = {k: v for k, v in rows.items() if is_registry(k)}
    plan = {k: v for k, v in rows.items() if not is_registry(k)}
    return reg, plan


def gated_set(rows, denominator):
    if denominator == "registry":
        return {k: v for k, v in rows.items() if is_registry(k)}
    return rows


# ─────────────────────────────── self-test ────────────────────────────────────

def _doc(*rows):
    return "\n".join(rows)


REG_HDR = ["| Anchor | Trigger | Closing work | Cross-refs |", "|---|---|---|---|"]
DEF_HDR = ["| # | Deferred item | Why deferred | Owner / closure | Trigger |",
           "|---|---|---|---|---|"]
P00_HDR = ["| # | Deferred item | Why deferred | Class | Owner / closure | Trigger |",
           "|---|---|---|---|---|---|"]
RES_HDR = ["| Anchor | Owns |", "|--------|------|"]


def self_test():
    """Red-on-disable for the instrument itself.

    Two families of case, and BOTH must hold:
      (a) the glyph-agnostic inversion -- if someone 'helpfully' rewrites is_closed()
          to enumerate open glyphs, the novel-glyph cases fail;
      (b) the widened denominator -- if someone narrows the scan back to the registry,
          or lets an unknown table shape pass quietly, those cases fail.
    """
    cases = []

    def case(doc, expect, why, path=REG_REL, expect_fatal=0, expect_warn=0):
        cases.append((doc, expect, why, path, expect_fatal, expect_warn))

    # ── (a) the inversion, on the REGISTRY shape (all pre-existing pins, preserved) ──
    case(_doc(*REG_HDR, "| `D-A` | ✅ **CLOSED** | done | refs |"), set(),
         "a closed row is not open")
    case(_doc(*REG_HDR, "| `D-B` | \U0001f7e0 **OPEN** | work | refs |"), {"D-B"},
         "orange is open")
    case(_doc(*REG_HDR, "| `D-C` | ⏳ **OPEN** | work | refs |"), {"D-C"},
         "HOURGLASS is open (the 1st miss)")
    case(_doc(*REG_HDR, "| `D-D` | \U0001f534 **OPEN** | work | refs |"), {"D-D"},
         "red is open")
    case(_doc(*REG_HDR, "| `D-E` | ⚠ **OPEN** | work | refs |"), {"D-E"},
         "warning is open")
    case(_doc(*REG_HDR, "| `D-F` | \U0001f9ff **NOVEL GLYPH** | work | refs |"), {"D-F"},
         "a glyph nobody enumerated is open -- the whole point")
    case(_doc(*REG_HDR, "| `D-G` | no glyph at all | work | refs |"), {"D-G"},
         "no marker at all is open")
    case(_doc(*REG_HDR, "| `D-H` | ✅ **CLOSED** | supersedes a \U0001f7e0 row | refs |"),
         set(), "only the STATUS cell decides")
    case(_doc(*REG_HDR, "not a row at all"), set(), "non-rows are ignored")
    case(_doc(*REG_HDR, "| `D-I` | \U0001f7e0 **OPEN** -- half ✅ done, half not | w | r |"),
         {"D-I"}, "mid-prose check mark does NOT close (the 2nd miss)")
    case(_doc(*REG_HDR, "| `D-J` | ⏳ **PARTIALLY CLOSED -- ✅ part (1); (2) open** | w | r |"),
         {"D-J"}, "PARTIALLY closed is OPEN, however many marks it carries")
    case(_doc(*REG_HDR,
              "| `D-K` | ✅ **CLOSED 2026-01-01.** *(Was: OPEN -- \U0001f534 HIGH.)* | d | r |"),
         set(), "a closed row may recap 'Was: OPEN' and stays closed")
    case(_doc(*REG_HDR, "| `D-L` | **✅ CLOSED** | done | refs |"), set(),
         "leading markdown emphasis before the mark still closes")

    # ── (b) THE WIDENED DENOMINATOR: plan-side shapes ──
    # star The 3rd miss (`D-GATE-BALANCE-COUNTS-ONLY-THE-REGISTRY`): a plan-side row
    # was worth ZERO to this gate, so moving work out of the registry looked like
    # progress. These pin that a plan row counts exactly like a registry row.
    case(_doc(*DEF_HDR, "| D-PL-OPEN-ROW | plain prose, no glyph | why | owner | trig |"),
         {"D-PL-OPEN-ROW"}, "plan sec3.1 row with no glyph is OPEN", path=".plans/22-x.md")
    case(_doc(*DEF_HDR, "| ~~D-PL-DONE-ROW~~ | ✅ **CLOSED 2026-01-01** | why | own | t |"),
         set(), "plan sec3.1 leading check mark closes (cell-1 strikethrough ignored)",
         path=".plans/22-x.md")
    # ★ REQUIRED CASE 1: mid-prose check mark in a plan-shaped row.
    case(_doc(*DEF_HDR,
              "| D-PL-HALF | **OPEN** -- the ✅ layout half landed; va_arg half open | w | o | t |"),
         {"D-PL-HALF"},
         "plan row: mid-prose check mark does NOT close (the 2nd miss, plan side)",
         path=".plans/14-x.md")
    # ★ REQUIRED CASE 2: a glyph nobody has enumerated, on the plan-side path.
    case(_doc(*DEF_HDR, "| D-PL-NOVEL | \U0001fae0 **melting face status** | w | o | t |"),
         {"D-PL-NOVEL"}, "plan row: a novel glyph is OPEN (the whole point, plan side)",
         path=".plans/13-x.md")
    # ✔MEASURED shape of plan 12: doneness recorded in the LAST cell, not the status
    # cell. That row reads OPEN. It is the SAFE direction and it is left that way on
    # purpose -- moving the test to "a check mark anywhere in the row" would re-import
    # the exact 2026-08-12 undercount. Pinned so nobody 'fixes' it.
    case(_doc(*DEF_HDR, "| D-PL-LATE | hardening item | cosmetic | none. | ML6 c1 ✅ done |"),
         {"D-PL-LATE"},
         "check mark in a LATER cell does not close (plan 12's real shape)",
         path=".plans/12-x.md")
    # plan-00 section 0.2: 6 columns, a `Class` column, and ids that are not anchors.
    case(_doc(*P00_HDR, "| D7 | **Some project-level item** | why | HIGH | plan 14 | trig |"),
         {"D7"}, "plan-00 sec0.2 `D7` row counts even though it is not a D-*-* anchor",
         path=".plans/00-x.md")
    case(_doc(*P00_HDR, "| ~~D2~~ | ✅ **closed 2026-05-26.** | why | - | - | - |"),
         set(), "plan-00 sec0.2 closed row is closed", path=".plans/00-x.md")
    # RESERVED tables have no status column at all.
    case(_doc(*RES_HDR, "| `D-AXIS-ASYNC-DI` | language-side async DI API |"),
         {"D-AXIS-ASYNC-DI"}, "reserved-anchor row is OPEN (no status column)",
         path=".plans/24-x.md")
    case(_doc(*RES_HDR, "| `D-AXIS-DONE` | ✅ shipped, honest |"), {"D-AXIS-DONE"},
         "a check mark in a NON-status column cannot close a reserved row",
         path=".plans/24-x.md")

    # ── (c) blind spots of the OLD row regex, now counted ──
    case(_doc(*REG_HDR, "| `D-TEST-QEMU_LD_PREFIX-AMBIENT-ONLY` | ⚠ OPEN | w | r |"),
         {"D-TEST-QEMU_LD_PREFIX-AMBIENT-ONLY"},
         "UNDERSCORE in the name: invisible to the old regex, counted now")
    case(_doc(*REG_HDR, "| ~~`D-LK-DYN-RODATA-ITEM-RELOC`~~ | ⚠ still open | w | r |"),
         {"D-LK-DYN-RODATA-ITEM-RELOC"},
         "STRIKETHROUGH name: invisible to the old regex, counted now")

    # ── (d) fail-loud on shapes, and NO false alarm on the named exclusions ──
    case(_doc("| Widget | Notes |", "|---|---|",
              "| `D-SOMETHING-NEW-HERE` | invented table |"),
         set(), "an UNRECOGNIZED anchor-looking table is REPORTED, never skipped",
         path=".plans/99-x.md", expect_fatal=1)
    case(_doc("| PR | Title | Scope |", "|---|---|---|",
              "| ~~AS3~~ | cycle 3 landed (binary ops + D-ML7-2.1) | s |"),
         set(), "a NAMED non-deferral table is excluded, and raises no false alarm",
         path=".plans/13-x.md")
    case(_doc("| Tier | Example | External tools? |", "|---|---|---|",
              "| **Production pipeline** | dss-code-prime compiling sqlite | Zero. |"),
         set(), "an ordinary prose table with no anchors is simply ignored",
         path=".plans/17-x.md")
    # star The line-based predecessor would have counted these; a table-based reader
    # must not lose them to silence. Both are REPORTED, which fails the run.
    case(_doc("## A heading, and then a row with no table header at all",
              "| `D-ORPHANED-ROW-HERE` | \U0001f7e0 **OPEN** | work | refs |"),
         set(), "an ORPHAN anchor row outside any table is REPORTED, never dropped",
         path=".plans/99-y.md", expect_fatal=1)
    case(_doc(*REG_HDR, "| `D-IN-TABLE-FINE` | ⚠ OPEN | w | r |", "",
              "| `D-AFTER-THE-BLANK-LINE` | ⚠ OPEN | w | r |"),
         {"D-IN-TABLE-FINE"},
         "a row severed from its table by a blank line is REPORTED, not silently lost",
         expect_fatal=1)
    # star 22-optimizer section 3.1's REAL defect, pinned: an HTML comment mid-table.
    # The rows below it must still be COUNTED (dropping them understates the
    # denominator) AND the interruption must still be REPORTED (the file is malformed).
    case(_doc(*DEF_HDR, "| D-BEFORE-THE-COMMENT | still open | w | o | t |",
              "<!-- a user-supervised note parked inside the table body -->",
              "| D-AFTER-THE-COMMENT | also still open | w | o | t |",
              "| D-AFTER-AND-CLOSED | ✅ **CLOSED** | w | o | t |"),
         {"D-BEFORE-THE-COMMENT", "D-AFTER-THE-COMMENT"},
         "rows under a mid-table HTML comment are COUNTED and the break is REPORTED",
         path=".plans/22-x.md", expect_warn=1)
    case(_doc(*DEF_HDR, "| D-LAST-ROW-OF-TABLE | open | w | o | t |",
              "<!-- a comment that legitimately FOLLOWS the table -->",
              "some prose"),
         {"D-LAST-ROW-OF-TABLE"},
         "a comment AFTER the last row is not an interruption (no false alarm)",
         path=".plans/22-x.md")

    failed = 0
    for doc, expect, why, path, expect_fatal, expect_warn in cases:
        rows, findings = scan_document(doc, path)
        got = set(k.split("#", 1)[1] for k in rows)
        # star SEVERITY IS ASSERTED, NOT JUST THE COUNT. FATAL means "rows exist that
        # I could not count"; WARN means "the file is malformed but nothing was lost".
        # A case that produced the right number of findings at the WRONG severity would
        # silently turn a gate failure into a note, so both are pinned.
        n_fatal = sum(1 for f in findings if f[2] == "FATAL")
        n_warn = sum(1 for f in findings if f[2] == "WARN")
        ok = (got == expect) and (n_fatal == expect_fatal) and (n_warn == expect_warn)
        if not ok:
            failed += 1
        extra = ""
        if (n_fatal, n_warn) != (expect_fatal, expect_warn):
            extra = "  findings expected=%d fatal/%d warn got=%d/%d" % (
                expect_fatal, expect_warn, n_fatal, n_warn)
        print("  %-4s %-62s expected=%s got=%s%s"
              % ("ok" if ok else "FAIL", why, sorted(expect) or "-", sorted(got) or "-",
                 extra))

    # ── (e) namespacing: the same anchor in two homes is two rows of bookkeeping ──
    a, _ = scan_document(_doc(*REG_HDR, "| `D-DUP-ROW-NAME` | ⚠ OPEN | w | r |"), REG_REL)
    b, _ = scan_document(_doc(*DEF_HDR, "| D-DUP-ROW-NAME | still open here | w | o | t |"),
                         ".plans/14-x.md")
    merged = dict(a)
    merged.update(b)
    ok = len(merged) == 2
    failed += 0 if ok else 1
    print("  %-4s %-62s expected=2 got=%d"
          % ("ok" if ok else "FAIL",
             "the same anchor in two homes stays two distinct rows", len(merged)))

    print("self-test: %d case(s), %d failed" % (len(cases) + 1, failed))
    return 1 if failed else 0


# ──────────────────────────────────── main ─────────────────────────────────────

def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--base", default="HEAD",
                    help="git ref the cycle started from (default HEAD)")
    ap.add_argument("--denominator", choices=("registry", "registry+plans"),
                    default="registry+plans",
                    help="which home(s) the gate counts. 'registry' reproduces the "
                         "pre-2026-08-13 headline; the default counts every home "
                         "SKILL.md section F.2 sanctions.")
    ap.add_argument("--breakdown", action="store_true",
                    help="also print OPEN rows per plan file")
    ap.add_argument("--self-test", action="store_true",
                    help="check the instrument, not the registry")
    args = ap.parse_args()

    # ⚠ A GATE THAT CRASHES WHILE REPORTING IS WORSE THAN ONE THAT SAYS NOTHING.
    # ✔MEASURED 2026-08-12: this script died with UnicodeEncodeError on a Windows
    # cp1252 console the first time it had to print an OPENED row, because the
    # status excerpt began with 🟠 (U+1F7E0). Every prior run had printed only
    # counts, so the reporting path had never been exercised with real content —
    # the crash was latent for exactly as long as the gate was passing. The
    # excerpt is diagnostic prose; an unencodable glyph must degrade to a
    # placeholder, never take the gate down.
    try:
        sys.stdout.reconfigure(errors="replace")
    except (AttributeError, ValueError):          # pragma: no cover - old python
        pass

    if args.self_test:
        return self_test()

    root = repo_root()
    before, _ = scan_at_ref(root, args.base)
    after, findings = scan_worktree(root)

    b_reg, b_plan = split_homes(before)
    a_reg, a_plan = split_homes(after)
    gate_before = gated_set(before, args.denominator)
    gate_after = gated_set(after, args.denominator)

    closed = sorted(set(gate_before) - set(gate_after))
    opened = sorted(set(gate_after) - set(gate_before))

    print("anchor-balance: denominator = %s   (SKILL.md sec F.2 sanctions BOTH homes; "
          "use --denominator registry for the pre-2026-08-13 headline)" % args.denominator)
    print("anchor-balance: OPEN at %-10s registry=%-5d plans=%-5d total=%d"
          % (args.base, len(b_reg), len(b_plan), len(before)))
    print("anchor-balance: OPEN now %-13s registry=%-5d plans=%-5d total=%d"
          % ("", len(a_reg), len(a_plan), len(after)))
    print("anchor-balance: GATED count %d -> %d   (net %+d)"
          % (len(gate_before), len(gate_after), len(gate_after) - len(gate_before)))
    disclosed = sorted(n for n in opened if is_disclosed(after[n][1]))
    created = [n for n in opened if n not in set(disclosed)]
    print("anchor-balance: closed %d, opened %d  (created %d, disclosed-pre-existing %d)"
          % (len(closed), len(opened), len(created), len(disclosed)))
    for n in closed:
        print("  - %s" % n)
    for n in opened:
        print("  + %s   %s" % (n, after[n][1]))

    if args.breakdown:
        print()
        per = {}
        for k in after:
            per[k.split("#", 1)[0]] = per.get(k.split("#", 1)[0], 0) + 1
        for f in sorted(per, key=lambda x: (-per[x], x)):
            print("  %-52s %d OPEN" % (f, per[f]))

    # ⚠ FAIL LOUD ON AN UNPARSED TABLE. A silently skipped anchor table is the exact
    # defect D-GATE-BALANCE-COUNTS-ONLY-THE-REGISTRY names, so an unknown shape that
    # LOOKS like an anchor table fails the run even when the balance itself is fine.
    # Fix it by teaching SHAPES_* the new shape, or by NAMING it in EXCLUDED_HEADERS
    # after reading it -- never by deleting this check.
    warns = [f for f in findings if f[2] == "WARN"]
    fatals = [f for f in findings if f[2] == "FATAL"]

    # Printed on EVERY run, pass or fail: the file is malformed, but the rows were
    # counted, so the denominator is whole and the balance verdict stands.
    if warns:
        print()
        print("anchor-balance: WARN - %d malformed structure(s); rows were COUNTED, "
              "but the markdown is wrong:" % len(warns))
        for rel, line_no, _sev, what in warns:
            print("  %s:%d   %s" % (rel, line_no, what))
        print("  TABLE BODY INTERRUPTED -> move the HTML comment above or below the "
              "table. Not a gate failure: nothing was lost from the count.")

    # ⚠ FAIL LOUD WHEN THE MEASUREMENT ITSELF IS INCOMPLETE. A silently skipped anchor
    # table is the exact defect D-GATE-BALANCE-COUNTS-ONLY-THE-REGISTRY names, so an
    # unknown shape or an orphan row fails the run even when the balance is fine.
    # Fix it by teaching the tool the shape, or by NAMING it in EXCLUDED_HEADERS after
    # reading it -- never by deleting this check.
    if fatals:
        print()
        print("anchor-balance: FAIL - %d anchor-bearing structure(s) this gate could "
              "NOT COUNT:" % len(fatals))
        for rel, line_no, _sev, what in fatals:
            print("  %s:%d   %s" % (rel, line_no, what))
        print("  UNRECOGNIZED TABLE SHAPE -> teach SHAPES_EXACT/SHAPES_PREFIX the shape, "
              "or NAME it in EXCLUDED_HEADERS after reading it.")
        print("  ORPHAN ANCHOR ROW -> the row has drifted out of its table (broken "
              "separator or stray blank line); put it back.")
        print("  Do NOT let either be skipped - a silently skipped row is the exact "
              "defect this gate exists to stop.")
        return 1

    net_new = len(gate_after) - len(gate_before) - len(disclosed)
    if disclosed:
        print("anchor-balance: %d disclosed-pre-existing row(s) are EXEMPT from the net "
              "increase (they record debt that already existed)." % len(disclosed))
    if net_new > 0:
        print()
        print("anchor-balance: FAIL - this cycle CREATED %d more OPEN row(s) than it closed."
              % net_new)
        print("  Close what you opened, or take it to the operator as a decision (dss-cycle "
              "section B). Do NOT widen this gate to fit the cycle.")
        return 1
    print("anchor-balance: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
