#!/usr/bin/env python3
# PURPOSE: refuse a duplicate, implicitly-numbered, or newly-uncovered `DiagnosticCode` ordinal.
"""check-diagnostic-codes.py -- the ALLOCATION gate for `DiagnosticCode` ordinals.

★★★ WHY THIS EXISTS, and it is a measured failure rather than a hypothetical.

`DiagnosticCode` is a single flat ordinal space. Every value in it is an
OPERATOR-VISIBLE published identity -- it renders as `error[D0029]`, it appears
in docs and in `expected.json` fixtures, and renumbering one rewrites a name
users have already seen. So the space has exactly two hazards, and this cycle
hit both:

  (1) ★ TWO CONCURRENT LANES ALLOCATED THE SAME ORDINAL. During the AP5/AP6
      close-out, one lane was told `0xD029` was free while another lane had
      already taken it for `D_DependencyBuildFailed`. Nothing mechanical
      noticed. It was caught only because the second lane happened to
      RE-MEASURE the header instead of trusting its brief -- i.e. by luck and
      diligence, which is the same non-mechanism a WITNESSED run replaces. Two
      lanes, one counter, no lock.

  (2) ★ A CODE LANDED WITH NO TEST AT ALL. `D-AP6-NEW-DIAGNOSTIC-CODES-HAD-NO-VALUE-PIN`
      closed on exactly this and it RE-OPENED ONE CYCLE LATER:
      `D_LanguageTargetIsaMismatch` (0xD02A) shipped engine code in `src/`
      while appearing in ZERO test files. The hand-maintained contiguity pin in
      `tests/core/test_parse_diagnostic.cpp` cannot see this -- it can only
      check rows somebody remembered to add to it, so a lane that allocates and
      never touches the table is invisible to the very instrument meant to
      catch it.

⇒ THE INSTRUMENT MUST READ THE ENUM, NOT A TABLE SOMEBODY MAINTAINS BY HAND.
   The enum is the single source of truth for what has been allocated; anything
   that asks a human to keep a second list in sync has the same failure mode as
   the thing it is checking.

★★ THE THREE CHECKS, and why each is drawn where it is:

  A. DUPLICATE VALUE -> FATAL, no baseline, no exceptions. This is hazard (1)
     and it is unambiguous: two names on one ordinal is never intentional.

  B. ENUMERATOR WITH NO EXPLICIT VALUE -> FATAL. An unvalued enumerator takes
     `predecessor + 1`, so INSERTING a row above it silently renumbers it and
     everything below -- a published `error[Dxxxx]` changes with no diff at the
     changed line. Today all 370 carry explicit values; this check keeps it
     that way rather than discovering the exception after it ships.

  C. A CODE NO TEST EXERCISES -> RATCHET against a frozen baseline. This is
     hazard (2). It is a ratchet and not a hard zero because the debt predates
     the gate: 36 codes already had no executable test reference when this was
     written, and clearing them is not one cycle's work. NEW debt fails.

★ CHECK C READS TEST SOURCES WITH COMMENTS STRIPPED, and that is load-bearing.
  Diagnostic codes are discussed constantly in test prose -- 7 codes are named
  ONLY inside comments. Counting a comment as coverage would let a lane satisfy
  this gate by MENTIONING its new code in a sentence. What is being asserted is
  narrow and worth stating exactly: the code's NAME appears in compiled test
  code. That is not proof the code is asserted on, and this script does not
  claim it is. It is proof the code is not entirely unknown to the test tree,
  which is the specific thing that went wrong at 0xD02A.

★ DEFINE THE COMPLEMENT, NOT THE VARIANTS. Same discipline as
  `check-anchor-balance.py`'s glyph inversion: a code is UNCOVERED unless a test
  names it. There is no enumeration of "ways a test might reference a code" to
  fall out of date -- a reference shape nobody has thought of yet counts as
  uncovered, which is the safe direction.

★ AND IT REFUSES TO PASS VACUOUSLY. A parse that collapses -- enum block not
  found, implausibly few enumerators, no test files -- exits 2 rather than
  reporting "0 duplicates, OK". An instrument that cannot tell "clean" from
  "never ran" is the exact failure a witnessed run exists to forbid -- the rule
  `dssharness build`, `test` and `run` now carry from config, where a zero exit
  code with no success match reports as itself -- and it would be absurd for the
  gate that enforces that lesson to embody its inverse.

⚠⚠ RUN THIS ON A QUIET TREE, and the reason is specific rather than general
   caution. This gate reads the WORKTREE, and the project's own red-on-disable
   discipline REQUIRES a lane to temporarily write knowingly-wrong bytes into
   shipped source to prove a pin inverts. ✔MEASURED 2026-08-15: while a sibling
   lane was proving its 0xD029 pin, the header transiently read
   `D_DependencyBuildFailed = 0xD0F9`, and a run taken in that window reported a
   GREEN verdict and a next-free D_ band of 0xD0FA -- both computed from bytes
   that exist in no real tree. Nothing was wrong with the instrument; the tree
   was mid-mutation by design. Same family as two concurrent ctest runs in one
   build directory, which yield no verdict at all.

Usage:
    python .harness-config/runner/actions/check-diagnostic-codes/check-diagnostic-codes.py
    python .harness-config/runner/actions/check-diagnostic-codes/check-diagnostic-codes.py --self-test
    python .harness-config/runner/actions/check-diagnostic-codes/check-diagnostic-codes.py --list-uncovered
    python .harness-config/runner/actions/check-diagnostic-codes/check-diagnostic-codes.py --cross-branch

`--cross-branch` is the ALLOCATION-TIME view across every place this repository can allocate an
ordinal (the other worktrees' working headers and every ref not merged into HEAD); see
`cross_branch_findings`. It is opt-in: the ctest form stays hermetic.

Exit codes:  0 = pass   1 = gate failure   2 = usage / collapsed scan
"""

import argparse
import importlib.util
import os
import re
import sys
sys.dont_write_bytecode = True  # a by-path load must not write __pycache__ beside another action (the rule: check-scripts-index)

# ── OUTPUT ENCODING — NOT COSMETIC, AND THE STREAM IS HALF THE FACT ─────────────
# ✔MEASURED 2026-08-23 (CPython 3.14.3, Windows, BOTH streams PIPES, which is
# exactly how ctest runs every guard): `sys.stdout` comes up
# `encoding='cp1252' errors='surrogateescape'` and `sys.stderr` comes up
# `errors='backslashreplace'`. `surrogateescape` rescues only lone surrogates left
# by an earlier decode; it does NOTHING for an ordinary unencodable character. So a
# report printed on STDOUT — where this guard names every uncovered code, every
# duplicate ordinal and every UNVALUED enumerator, all of them identifiers and
# initialiser TEXT read straight out of the header —
# raises `UnicodeEncodeError` and kills the guard INSIDE ITS OWN REPORT: the run
# still reds, but the finding is lost and the traceback names a `print` rather than
# the thing that was wrong. STDERR merely mangles the glyph into an escape.
# ⚠ This is the one guard in the battery whose stdout report carries CODE TEXT rather
# than only paths: an enumerator's initialiser is reproduced verbatim.
# Applied at IMPORT, so every path this module can print on is covered.
for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError, OSError):   # pragma: no cover - odd stream
        pass


HEADER_REL = "src/core/types/parse_diagnostic.hpp"
TESTS_REL = "tests"
ENUM_DECL = "enum class DiagnosticCode"

# A parse that finds fewer than this many enumerators has collapsed. The real
# count was 370 when this gate was written; the floor is deliberately far below
# it so that ordinary growth or removal never trips it, and only a BROKEN REGEX
# (which yields 0, or a handful) does.
MIN_PLAUSIBLE_CODES = 300

# ── ORDINALS CLAIMED ON ANOTHER BRANCH AND NOT YET MERGED ────────────────────
# ★★ THE COLLISION THIS GATE WAS BUILT FOR HAS A CROSS-BRANCH TWIN, AND READING
#    ONLY THIS WORKTREE CANNOT SEE IT. ✔MEASURED 2026-08-15: PR #54 allocates
#    `S_InlineAsm*` at 0xE065..0xE06B and `L_SideStructure*` at 0xB010..0xB012.
#    Those codes are NOT in this tree's enum, so `next_free_by_band` computed
#    `S_ 0xE065` and `L_ 0xB010` — handing an allocator on THIS branch exactly
#    the slots the other branch already owns, with the tool's own authority
#    behind the answer. That is the 0xD029 failure again, one scope up: a
#    stale-by-construction "next free" number, trusted because it came from a
#    machine instead of a brief.
#
# ⇒ Reserved ranges are SUBTRACTED from the append point and any code landing
#   inside one is a FAILURE, so the conflict is caught on this branch, before
#   the merge, while it is still one line to move.
#
# ⚠ THIS LIST IS HAND-MAINTAINED AND THAT IS A KNOWN WEAKNESS — it is the one
#   place this tool cannot derive from a source of truth, because the source of
#   truth is a different branch. Keep entries few and RETIRE THEM ON MERGE:
#   once the other branch lands, its codes are in the enum and the ordinary
#   duplicate check covers them properly, at which point a stale reservation
#   here would start refusing legitimate ordinals. Delete the row, do not
#   "update" it.
#
# ✔VERIFIED 2026-08-16 AGAINST THE REAL BRANCH, which was NOT possible when
#   these rows were written — `feature/c23-conformance-burndown-3` did not then
#   exist on `origin` and the ranges were taken on report. It exists now
#   (`b52784a`), so the rows were checked by reading its enum directly:
#     git show origin/feature/c23-conformance-burndown-3:src/core/types/parse_diagnostic.hpp
#   ⇒ S_ 0xE065-0xE06B is exactly 7 InlineAsm codes, L_ 0xB010-0xB012 exactly 3
#   SideStructure codes — both ranges correct, neither over- nor under-claimed.
#   ⇒ Their D_ band stops at 0xD021 and ours starts at 0xD022, so the band this
#   branch actually grew has NO overlap at all.
#   ★ The interesting negative: S_ 0xE062-0xE064 appear on BOTH branches with
#   IDENTICAL names AND values. That is shared ancestry, not a collision, and it
#   is why the check must compare (value -> name) rather than "value seen twice
#   across branches" — the latter would report three false conflicts here.
#   ⓘ This verification is a SNAPSHOT. If #54 gains codes before it merges the
#   ranges can widen; re-run the command above rather than trusting this note.
#
# ★★★ RETIRED 2026-08-18 — THE TWO PR #54 ROWS ARE GONE, BY THIS TABLE'S OWN RULE.
# ✔MEASURED: `feature/c23-conformance-burndown-3` (which IS PR #54) rebased onto
# `origin/main`, so main's table arrived on the very branch it was reserving
# against — and all ten codes (S_ 0xE065..0xE06B, L_ 0xB010..0xB012) are now IN
# THIS TREE'S ENUM. The gate consequently reported this branch colliding with its
# own reservation: precisely the "a stale reservation would start refusing
# legitimate ordinals" failure the note above predicts, arriving by rebase rather
# than by merge. The rule stated there applies unchanged — *"its codes are in the
# enum and the ordinary duplicate check covers them properly ... Delete the row,
# do not 'update' it."*
# ⚠ THE RESERVATION IS NOT LOST FOR ANYONE ELSE: `origin/main` still carries these
# rows, which is where a DIFFERENT branch would read them. Deleting them here
# removes only the self-collision.
#
# Each entry: (low, high, why) — INCLUSIVE on both ends.
RESERVED_ELSEWHERE = ()


def reserved_hit(value):
    """The reservation covering `value`, or None."""
    for low, high, why in RESERVED_ELSEWHERE:
        if low <= value <= high:
            return (low, high, why)
    return None


def find_reserved_conflicts(rows):
    """[(name, value, why)] for codes in THIS tree sitting on a claimed ordinal."""
    out = []
    for name, value, _raw in rows:
        if value is None:
            continue
        hit = reserved_hit(value)
        if hit:
            out.append((name, value, hit[2]))
    return out


# ── CHECK C's FROZEN BASELINE ────────────────────────────────────────────────
# ✔MEASURED 2026-08-15 against the tree that introduced this gate: these codes
# are allocated in the enum and named nowhere in compiled test code.
#
# ★ SHRINKING THIS LIST IS ALWAYS CORRECT AND NEEDS NO PERMISSION -- covering a
#   code and deleting its line here is the intended direction of travel, and the
#   gate reports every name that becomes covered so the progress is visible.
# ⛔ GROWING IT MEANS "we shipped a diagnostic no test exercises". That is a §B
#   decision for the operator, not a convenience for the cycle that wants green.
#
# ⚠ `D_LanguageTargetIsaMismatch` (0xD02A) IS DELIBERATELY ABSENT even though it
#   was uncovered at the moment of writing. It is THIS cycle's debt -- the very
#   miss that motivated check C -- and baselining it would have made the gate
#   green by excusing the thing it was built to catch. The gate is RED until
#   that test lands. That is the gate working, not the gate misconfigured.
#
# ✔ONE ENTRY HERE WAS FOUND BY THIS GATE CORRECTING THE MEASUREMENT THAT BUILT
#   IT. The list was first drafted from an ad-hoc scan using a plain SUBSTRING
#   test, which saw `P_InvalidEscapeSequence` (0x0005) in the tests and declared
#   `P_InvalidEscape` (0x0012) covered. `find_uncovered` tokenizes identifiers
#   instead, so it reported the miss on its first real run -- the case pinned by
#   the self-test's "a longer identifier does not cover a shorter code name".
#   ⇒ the substring shortcut is not available to a future edit of this file.
UNCOVERED_BASELINE = frozenset({
    "P_NumericLiteralOutOfRange",
    "P_UnclosedScope",
    "P_UnfinishedTree",
    "C_CircularShape",
    "C_UnclosableScope",
    "C_InvalidShippedFfiHeaderPath",
    "S_IndirectCallNotSupported",
    "S_DuplicateLabel",
    "S_UndefinedLabel",
    "S_BitIntWidthNotConstant",
    "S_BitIntSignedWidthTooSmall",
    "S_BitIntWidthExceedsMax",
    "S_BitIntWidthAboveC1Limit",
    "D_OutputDirCreateFailed",
    "D_StaticLibFatArchiveUnsupported",
    "D_CompileUnitNullNoDiagnostic",
    "D_SynthRecipeFamilyUnknown",
    "H_SehJumpIntoRegion",
    "H_SehLabelAddress",
    "I_BlockNotTerminated",
    "I_ExtensionTypeInMir",
    "I_SehStructure",
    "I_ArgPositionDuplicate",
    "I_BitIntWidthInconsistent",
    "I_VlaStackRestorePairing",
    "A_FunctionEncodeAborted",
    "K_ImageWriteCloseFailed",
    "K_CrossCuImageEmitDeferred",
    "K_ImageExecBitFailed",
    "K_ArchiveFieldOverflow",
    "F_ShippedConstantVariantAmbiguous",
    "F_ShippedTypedefVariantAmbiguous",
    "F_ShippedMacroVariantAmbiguous",
})


# ── primitives, all pure so the self-test drives the SAME code as main() ─────

def strip_comments(text):
    """Remove /* */ and // comments.

    Enum bodies and test bodies contain no string literals that could hold a
    comment opener, so the naive strip is exact for both callers. Newlines
    inside block comments are preserved so line numbers survive.
    """
    def blank(m):
        return re.sub(r"[^\n]", " ", m.group(0))
    text = re.sub(r"/\*.*?\*/", blank, text, flags=re.S)
    text = re.sub(r"//[^\n]*", "", text)
    return text


def extract_enum_body(header_text):
    """Return the text between `enum class DiagnosticCode ... {` and its `};`.

    Returns None if the block cannot be located -- callers MUST treat that as a
    collapsed scan, never as an empty enum.
    """
    start = header_text.find(ENUM_DECL)
    if start < 0:
        return None
    brace = header_text.find("{", start)
    if brace < 0:
        return None
    end = header_text.find("\n};", brace)
    if end < 0:
        return None
    return header_text[brace + 1:end]


ENUMERATOR_RE = re.compile(
    r"^[ \t]+([A-Za-z_][A-Za-z0-9_]*)[ \t]*(?:=[ \t]*([^,\n]+?))?[ \t]*,",
    re.M)


def parse_enumerators(enum_body):
    """[(name, value_or_None, raw_value_text)] in declaration order."""
    out = []
    for m in ENUMERATOR_RE.finditer(strip_comments(enum_body)):
        name, raw = m.group(1), (m.group(2) or "").strip()
        value = None
        if re.fullmatch(r"0[xX][0-9A-Fa-f]+", raw):
            value = int(raw, 16)
        elif re.fullmatch(r"[0-9]+", raw):
            value = int(raw, 10)
        out.append((name, value, raw))
    return out


def find_duplicates(rows):
    """{value: [names...]} for every value carried by more than one name."""
    seen = {}
    for name, value, _raw in rows:
        if value is None:
            continue
        seen.setdefault(value, []).append(name)
    return {v: ns for v, ns in sorted(seen.items()) if len(ns) > 1}


def find_unvalued(rows):
    """Names with no explicit integer value -- implicit predecessor + 1."""
    return [(name, raw) for name, value, raw in rows if value is None]


def find_uncovered(rows, test_code_text):
    """Names that never appear in COMPILED test code (comments already stripped)."""
    present = set(re.findall(r"[A-Za-z_][A-Za-z0-9_]*", test_code_text))
    return {name for name, _v, _r in rows if name not in present}


def next_free_by_band(rows):
    """{band_letter: (highest_used, next_free)} keyed on the `X_` name prefix.

    ★ THIS IS THE HALF THAT PREVENTS THE COLLISION RATHER THAN REPORTING IT.
    The 0xD029 clash happened because a lane needed to know the next free slot
    and was TOLD one by a brief that had gone stale between being written and
    being read. A checker that only says "you collided" arrives after the code
    is written, the tests are written and the docs quote the number.

    The band comes from the NAME prefix, not the value's high nibble: the two do
    not agree (`H_*` lives at 0xF, `X_*` at 0x2), and the prefix is the thing an
    allocator actually has in hand when picking a slot.

    ⓘ `next_free` is `highest + 1` -- the APPEND point, deliberately not the
    lowest hole. Holes in these runs are withdrawn allocations that are pinned
    as holes on purpose (0xD027, and 0xD021's gap before it); back-filling one
    re-uses a number that may already have been published.
    """
    bands = {}
    for name, value, _raw in rows:
        if value is None or "_" not in name:
            continue
        letter = name.split("_", 1)[0]
        if len(letter) != 1:
            continue
        bands[letter] = max(bands.get(letter, 0), value)

    out = {}
    for letter, high in sorted(bands.items()):
        # Walk PAST any range another branch has claimed. A single `+ 1` here is
        # what handed out 0xE065 and 0xB010 while PR #54 already owned them.
        nxt = high + 1
        while True:
            hit = reserved_hit(nxt)
            if not hit:
                break
            nxt = hit[1] + 1
        out[letter] = (high, nxt)
    return out


# ── file-system side, kept thin so the pure core above is what gets tested ───

_OWNING_TREE = None


def _owning_tree():
    """`.harness-config/runner/actions/owning-tree/owning-tree.py` -- the one owner of "which tree is this file in?".

    Loaded by path from this file's sibling directory (a hyphen is not a module name). It
    FAILS LOUD when absent rather than falling back to a local walk: a second copy of the
    answer is the drift that owner exists to end.
    """
    global _OWNING_TREE
    if _OWNING_TREE is None:
        path = os.path.join(os.path.dirname(os.path.dirname(os.path.realpath(__file__))),
                            "owning-tree", "owning-tree.py")
        if not os.path.isfile(path):
            sys.exit("check-diagnostic-codes: cannot find %s -- this guard's root is "
                     "resolved there and nowhere else" % path)
        spec = importlib.util.spec_from_file_location("dss_owning_tree", path)
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        _OWNING_TREE = mod
    return _OWNING_TREE


def repo_root():
    """The tree THIS FILE lives in -- never the tree the caller's shell is standing in.

    ⚠ This was a bare `git rev-parse --show-toplevel`. ✔MEASURED 2026-09-15 (P66): run by
    path with its cwd inside a different repository it reported OK over THAT repository's
    header and test files, silently; from a directory inside no repository it would not
    run at all. ctest pins `WORKING_DIRECTORY`, so no gate saw either. The walk and the
    measurement that chose it are in `.harness-config/runner/actions/owning-tree/owning-tree.py`; this guard
    reads files only, so git is not consulted.
    """
    ot = _owning_tree()
    try:
        return ot.resolve(__file__)
    except ot.Refusal as exc:
        sys.exit("check-diagnostic-codes: %s" % exc)


def read_test_code(root):
    """Concatenated test sources with comments stripped, plus the file count."""
    chunks, count = [], 0
    tests_dir = os.path.join(root, TESTS_REL)
    if not os.path.isdir(tests_dir):
        sys.exit("collapsed scan: no %s directory under %s" % (TESTS_REL, root))
    for dirpath, _dirnames, filenames in os.walk(tests_dir):
        for fn in filenames:
            if not fn.endswith((".cpp", ".cc", ".hpp", ".h")):
                continue
            path = os.path.join(dirpath, fn)
            with open(path, "r", encoding="utf-8", errors="replace") as fh:
                chunks.append(strip_comments(fh.read()))
            count += 1
    if count == 0:
        sys.exit("collapsed scan: found NO test sources under %s -- every code "
                 "would look uncovered, which is a broken scan, not a finding"
                 % TESTS_REL)
    return "\n".join(chunks), count


# ── THE CROSS-BRANCH VIEW (opt-in: `--cross-branch`) ─────────────────────────
#
# ★★★ THE HAZARD, ✔MEASURED 2026-09-23 (P68 round 9): the gate above reads ONE header, so the "next free"
# it prints is this tree's answer only. Over the eleven worktrees of that round -- each lane's WORKING header,
# uncommitted allocations included -- the union's next free `S_` slot was 0xE088 while main's own tree said
# 0xE086: a lane allocating from main's view that day would have taken two slots another tree already held.
# Nothing collided only because the coordinator allocated by hand, which is the non-mechanism this view
# replaces (the 0xD029 clash was the same shape inside one branch).
#
# ⇒ `--cross-branch` reads every OTHER place an ordinal can be allocated from this repository, through git
# objects and the other worktrees' files -- never the network, so a ref is as fresh as the last fetch and the
# report names each ref's commit:
#   * each OTHER WORKTREE's working header (a lane in flight, uncommitted allocations included);
#   * each branch and remote-tracking ref NOT merged into HEAD (a merged one adds nothing by definition).
# Each is judged by what it ADDED since its merge-base with HEAD, as (name, value) pairs -- so a code present at
# the fork and renamed on one side is shared identity, not a collision, and a squash-merged branch whose pairs
# already sit in this tree adds nothing.
#
# A COLLISION is a value added under two names, or a name added at two values, by any two sides (this tree
# included). A tree FAILS on the collisions IT must fix: one involving an allocation of its own that is not yet
# PUBLISHED (in its working header, absent from its HEAD commit), or two LIVE worktrees colliding with each
# other. A side colliding with a code this tree already PUBLISHED is that side's to fix before it merges, and
# a collision between refs nobody is working on here is history: both are REPORTED, never failed.
# ✔MEASURED 2026-09-24 on this host: without that split the view was red on every run -- a two-month-old
# backup branch and an abandoned branch each held a pair that main had published differently long ago -- and
# a guard every run trips is a disarmed guard. The append point per band is printed over the UNION of every
# side, dead refs included (conservative: a slot anyone ever held is skipped).
#
# ⚠ OPT-IN, and the ctest form stays HERMETIC: a gate whose verdict depends on which branches happen to be
# fetched on the host would go red for a reason no diff explains. The pure core is pinned by the self-test on
# every run; the git layer by a synthetic repository.
# ⓘ `RESERVED_ELSEWHERE` stays: a hand-written reservation still covers what no ref or worktree on this host
# holds (a branch on another machine).


def _pairs(rows):
    """{name: value} for the valued enumerators of `rows`."""
    return {n: v for n, v, _raw in rows if v is not None}


def _clashes(left, right):
    """Every identity two addition sets hold differently -> [(kind, key, left pair, right pair)], a pair being
    (name, value): a value held under two names, or a name held at two values."""
    out = []
    by_value = {v: n for n, v in right.items()}
    for n, v in sorted(left.items()):
        m = by_value.get(v)
        if m is not None and m != n:
            out.append(("value", "0x%04X" % v, (n, v), (m, v)))
        w = right.get(n)
        if w is not None and w != v:
            out.append(("name", n, (n, v), (n, w)))
    return out


def _render(label, pair):
    return (label, "%s = 0x%04X" % pair)


def cross_branch_findings(ours, published, sides):
    """The PURE core of `--cross-branch`.

    `ours`      -- {name: value}, this tree's WORKING header.
    `published` -- {name: value}, this tree's header at its HEAD commit (what it has already published).
    `sides`     -- [(label, base, theirs, live)]: `base` the {name: value} at that side's merge-base with this
                   HEAD, `theirs` the side's own {name: value}, `live` True for a worktree (allocating on this
                   host now), False for a ref.
    -> (failures, notes, union_rows): each collision a (kind, key, (where, what), (where, what)) tuple;
       `failures` = the collisions this tree must fix (one of its UNPUBLISHED pairs is in it, or two LIVE sides
       collide); `notes` = every other one (a side colliding with a pair this tree PUBLISHED -- that side must
       move before it merges -- or one only refs are in); `union_rows` = rows for `next_free_by_band` over this
       tree plus every side's additions.
    A side's ADDITIONS are its pairs absent from its own base, and THIS TREE's are judged against that same
    base, so a code that existed at the fork -- renamed on one side or not -- is never counted as allocated
    since it. Two sides are compared on the additions this tree does not already hold (a squash-merged pair is
    shared)."""
    published_pairs = set(published.items())
    failures, notes, added = set(), set(), []
    for label, base, theirs, live in sides:
        base_pairs = set(base.items())
        add = {n: v for n, v in theirs.items() if (n, v) not in base_pairs}
        mine = {n: v for n, v in ours.items() if (n, v) not in base_pairs}
        for kind, key, left, right in _clashes(mine, add):
            finding = (kind, key, _render("this tree", left), _render(label, right))
            (notes if left in published_pairs else failures).add(finding)
        added.append((label, live, {n: v for n, v in add.items() if ours.get(n) != v}))
    for i, (l1, live1, a1) in enumerate(added):
        for l2, live2, a2 in added[i + 1:]:
            for kind, key, left, right in _clashes(a1, a2):
                finding = (kind, key, _render(l1, left), _render(l2, right))
                (failures if live1 and live2 else notes).add(finding)
    union_rows = ([(n, v, "") for n, v in ours.items()]
                  + [(n, v, "") for _label, _live, add in added for n, v in add.items()])
    return sorted(failures), sorted(notes), union_rows


def _git(root, *args):
    """`git -C <root> <args>` through the ONE git entry point (owning-tree's `run_git`)."""
    return _owning_tree().run_git(["-C", root] + list(args), capture_output=True, text=True,
                                  encoding="utf-8", errors="replace")


def _pairs_of_text(text):
    body = extract_enum_body(text or "")
    return _pairs(parse_enumerators(body)) if body else None


def _pairs_at(root, rev):
    p = _git(root, "show", "%s:%s" % (rev, HEADER_REL))
    return _pairs_of_text(p.stdout) if p.returncode == 0 else None


def read_cross_branch_sides(root):
    """-> (sides, notes): the git layer of `--cross-branch` over the repository `root` belongs to. `sides` as
    `cross_branch_findings` takes them; `notes` = what was skipped and why (a side with no header at its fork,
    the merged refs), so the report states its own scope. Exits 2 when git cannot answer at all."""
    head = _git(root, "rev-parse", "HEAD")
    if head.returncode != 0:
        print("check-diagnostic-codes --cross-branch: git cannot read HEAD in %s (%s)"
              % (root, head.stderr.strip()))
        sys.exit(2)
    head = head.stdout.strip()
    sides, notes = [], []

    def fork_of(rev):
        mb = _git(root, "merge-base", "HEAD", rev)
        return mb.stdout.strip() if mb.returncode == 0 else ""

    # (1) every OTHER worktree's WORKING header
    listing = _git(root, "worktree", "list", "--porcelain")
    if listing.returncode != 0:
        print("check-diagnostic-codes --cross-branch: `git worktree list` failed in %s (%s)"
              % (root, listing.stderr.strip()))
        sys.exit(2)
    entry = {}
    for line in listing.stdout.splitlines() + [""]:
        if line:
            key, _sp, value = line.partition(" ")
            entry[key] = value
            continue
        path, rev = entry.get("worktree"), entry.get("HEAD")
        entry = {}
        if not path or not rev or _owning_tree().same_path(path, root):
            continue
        header = os.path.join(path, HEADER_REL)
        if not os.path.isfile(header):
            notes.append("worktree %s: no %s (skipped)" % (path, HEADER_REL))
            continue
        with open(header, "r", encoding="utf-8", errors="replace") as fh:
            theirs = _pairs_of_text(fh.read())
        fork = fork_of(rev)
        base = _pairs_at(root, fork) if fork else None
        if theirs is None or base is None:
            notes.append("worktree %s: header unreadable there or at its fork (skipped)" % path)
            continue
        sides.append(("worktree %s (working tree, fork %s)" % (path, fork[:8]), base, theirs, True))

    # (2) every branch and remote-tracking ref NOT merged into HEAD
    refs = _git(root, "for-each-ref", "--format=%(refname:short) %(objectname)", "refs/heads", "refs/remotes")
    if refs.returncode != 0:
        print("check-diagnostic-codes --cross-branch: `git for-each-ref` failed in %s (%s)"
              % (root, refs.stderr.strip()))
        sys.exit(2)
    merged = 0
    for line in refs.stdout.splitlines():
        ref, _sp, obj = line.partition(" ")
        if not ref or ref.endswith("/HEAD") or obj == head:
            continue
        if _git(root, "merge-base", "--is-ancestor", obj, "HEAD").returncode == 0:
            merged += 1
            continue
        fork = fork_of(obj)
        theirs = _pairs_at(root, obj)
        base = _pairs_at(root, fork) if fork else None
        if theirs is None or base is None:
            notes.append("ref %s: no %s at the ref or at its fork (skipped)" % (ref, HEADER_REL))
            continue
        sides.append(("ref %s (%s, fork %s)" % (ref, obj[:8], fork[:8]), base, theirs, False))
    notes.append("%d merged ref(s) add nothing and were not read" % merged)
    return sides, notes


def cross_branch_main(root, ours_rows):
    """`--cross-branch`: print every side, FAIL on a collision this tree must fix, REPORT the rest, and print
    the union's append points."""
    sides, scope = read_cross_branch_sides(root)
    published = _pairs_at(root, "HEAD")
    if published is None:
        print("check-diagnostic-codes --cross-branch: %s is unreadable at HEAD in %s, so which of this tree's "
              "codes are already published cannot be told" % (HEADER_REL, root))
        sys.exit(2)
    failures, notes, union_rows = cross_branch_findings(_pairs(ours_rows), published, sides)
    print("check-diagnostic-codes --cross-branch: %d side(s) beside this tree" % len(sides))
    for label, base, theirs, _live in sides:
        base_pairs = set(base.items())
        print("  %3d added  %s" % (sum(1 for p in theirs.items() if p not in base_pairs), label))
    for line in scope:
        print("  (%s)" % line)
    if notes:
        print("\n  NOT this tree's to fix (%d): a side holding a code this tree already PUBLISHED at HEAD must "
              "renumber before it merges; a collision only refs are in is history until one of them is "
              "worked on:" % len(notes))
        for kind, key, (w1, x1), (w2, x2) in notes:
            print("    %s %s: %s in %s  |  %s in %s" % (kind, key, x1, w1, x2, w2))
    if failures:
        print("\nFAIL -- CROSS-BRANCH ORDINAL COLLISION: an allocation this tree has not published, or two "
              "worktrees in flight, hold one identity differently.")
        for kind, key, (w1, x1), (w2, x2) in failures:
            print("    %s %s: %s in %s  |  %s in %s" % (kind, key, x1, w1, x2, w2))
        print("  Move the side that has NOT published to the union's next free slot below.")
    print("  next free ordinal per band over this tree AND every side (append point):")
    print("    " + "   ".join("%s_ 0x%04X" % (letter, nxt)
                              for letter, (_high, nxt) in next_free_by_band(union_rows).items()))
    return 1 if failures else 0


# ── the self-test: red-on-disable for the instrument itself ──────────────────

def _enum(*lines):
    return ("enum class DiagnosticCode : std::uint16_t {\n"
            + "\n".join("    " + ln for ln in lines)
            + "\n};\n")


def _cross_branch_git_arm():
    """The git layer of `--cross-branch`, end to end, on a THROWAWAY repository -> (ok, detail).

    main allocates S_Main; branch `feat` (not merged) allocates S_Feat at 0xE002; branch `done` is merged; a
    second WORKTREE (`lane`, forked before main's commit) is read twice: first with its working header equal to
    the fork (the CONTROL: nothing collides), then with an UNCOMMITTED S_Lane at 0xE002 -- the in-flight
    allocation no ref can show, colliding with `feat`."""
    import shutil
    import stat
    import tempfile
    tmp = tempfile.mkdtemp(prefix="dss-xbranch-")
    repo, lane = os.path.join(tmp, "repo"), os.path.join(tmp, "lane")

    def git(*args):
        p = _git(repo, *args)
        if p.returncode != 0:
            raise RuntimeError("git %s: %s" % (" ".join(args), (p.stderr or p.stdout).strip()))
        return p.stdout

    def header(root, *lines):
        with open(os.path.join(root, *HEADER_REL.split("/")), "w", encoding="utf-8", newline="\n") as fh:
            fh.write(_enum(*lines))

    def onerror(func, path, _exc):
        os.chmod(path, stat.S_IWRITE | stat.S_IREAD)
        func(path)
    try:
        os.makedirs(os.path.join(repo, *os.path.dirname(HEADER_REL).split("/")))
        hooks = os.path.join(tmp, "no-hooks")
        os.makedirs(hooks)
        git("init", "-q", "-b", "main")
        for key, value in (("user.email", "selftest@example.invalid"), ("user.name", "selftest"),
                           ("commit.gpgsign", "false"), ("core.hooksPath", hooks)):
            git("config", key, value)
        header(repo, "S_Old = 0xE001,")
        git("add", HEADER_REL)
        git("commit", "-q", "-m", "base")
        git("checkout", "-q", "-b", "feat")
        header(repo, "S_Old = 0xE001,", "S_Feat = 0xE002,")
        git("commit", "-q", "-am", "feat")
        git("checkout", "-q", "main")
        git("branch", "done")
        git("worktree", "add", "-q", "-b", "lane", lane, "main")
        header(repo, "S_Old = 0xE001,", "S_Main = 0xE003,")
        git("commit", "-q", "-am", "main")
        def view(root):
            """-> (failures, notes, union, labels, scope) as `root`'s tree sees the repository."""
            with open(os.path.join(root, *HEADER_REL.split("/")), encoding="utf-8") as fh:
                ours = _pairs_of_text(fh.read())
            sides, scope = read_cross_branch_sides(root)
            f, n, u = cross_branch_findings(ours, _pairs_at(root, "HEAD"), sides)
            return f, n, u, [label for label, _b, _t, _l in sides], scope

        control = view(repo)
        header(lane, "S_Old = 0xE001,", "S_Lane = 0xE002,")
        main_view, lane_view = view(repo), view(lane)
        kinds = lambda found: [(k, key) for k, key, _a, _b in found]   # noqa: E731
        ok = (control[0] == [] and control[1] == []
              # from main: the lane's in-flight slot collides with an unmerged REF -- the lane's to fix
              and main_view[0] == [] and kinds(main_view[1]) == [("value", "0xE002")]
              and sum(1 for lb in main_view[3] if lb.startswith("worktree ")) == 1
              and sum(1 for lb in main_view[3] if lb.startswith("ref feat ")) == 1 and len(main_view[3]) == 2
              and any(n.startswith("2 merged ref(s)") for n in main_view[4])
              and next_free_by_band(main_view[2]).get("S") == (0xE003, 0xE004)
              # from the lane: the same collision is ITS unpublished allocation -- a FAILURE
              and kinds(lane_view[0]) == [("value", "0xE002")] and lane_view[1] == [])
        return ok, "control=%r main=%r lane=%r" % (control[:2], main_view, lane_view[:2])
    except (OSError, RuntimeError) as exc:
        return False, "the synthetic repository could not be built: %s" % exc
    finally:
        shutil.rmtree(tmp, onerror=onerror)


def self_test():
    """Each case fails LOUDLY if the corresponding check is weakened or removed.

    Three families:
      (a) hazard (1) -- duplicate ordinals, including the shapes a naive
          text-compare would miss (case-different hex, decimal-vs-hex);
      (b) hazard (2) -- the coverage ratchet, including the comment-strip
          property without which a MENTION would count as coverage;
      (c) the collapse guards -- a scan that finds nothing must not report OK.
    """
    failures = []

    def check(label, got, want):
        if got != want:
            failures.append("  FAIL %s\n       got  %r\n       want %r"
                            % (label, got, want))

    # ── (a) DUPLICATE DETECTION ─────────────────────────────────────────────
    body = extract_enum_body(_enum("D_A = 0xD001,", "D_B = 0xD002,"))
    check("clean enum has no duplicates", find_duplicates(parse_enumerators(body)), {})

    # ★ THE 0xD029 CASE ITSELF: two lanes, one ordinal.
    body = extract_enum_body(_enum("D_DependencyBuildFailed = 0xD029,",
                                   "D_LanguageTargetIsaMismatch = 0xD029,"))
    check("two names on one ordinal is caught",
          find_duplicates(parse_enumerators(body)),
          {0xD029: ["D_DependencyBuildFailed", "D_LanguageTargetIsaMismatch"]})

    # ★ Case-different hex is the SAME ordinal. A text-compare over the literal
    #   would call these distinct; the check compares parsed VALUES.
    body = extract_enum_body(_enum("D_A = 0xd029,", "D_B = 0xD029,"))
    check("0xd029 and 0xD029 are one ordinal",
          find_duplicates(parse_enumerators(body)), {0xD029: ["D_A", "D_B"]})

    # ★ Decimal and hex spellings of one value likewise collide.
    body = extract_enum_body(_enum("D_A = 0x0010,", "D_B = 16,"))
    check("16 and 0x0010 are one ordinal",
          find_duplicates(parse_enumerators(body)), {16: ["D_A", "D_B"]})

    # ★ A duplicate hiding inside a COMMENT is not a duplicate. Enum bodies are
    #   dense with prose citing other codes' numbers; counting those would make
    #   the gate cry wolf until someone disabled it.
    body = extract_enum_body(_enum("// D_Ghost = 0xD001, -- prose citing a code",
                                   "D_A = 0xD001,"))
    check("a commented-out enumerator is not a collision",
          find_duplicates(parse_enumerators(body)), {})

    # ── (b) THE UNVALUED CHECK ──────────────────────────────────────────────
    body = extract_enum_body(_enum("D_A = 0xD001,", "D_Implicit,", "D_B = 0xD003,"))
    check("an unvalued enumerator is caught",
          [n for n, _raw in find_unvalued(parse_enumerators(body))], ["D_Implicit"])

    body = extract_enum_body(_enum("D_A = 0xD001,", "D_B = 0xD002,"))
    check("fully-valued enum reports no unvalued",
          find_unvalued(parse_enumerators(body)), [])

    # ── (c) THE COVERAGE RATCHET ────────────────────────────────────────────
    rows = parse_enumerators(extract_enum_body(
        _enum("D_Covered = 0xD001,", "D_Bare = 0xD002,")))

    check("a code named in compiled test code is covered",
          find_uncovered(rows, "EXPECT_EQ(x, DiagnosticCode::D_Covered);"),
          {"D_Bare"})

    # ★★ THE COMMENT-STRIP PROPERTY. Without it, a lane satisfies this gate by
    #    writing its new code's name in a sentence. `find_uncovered` is handed
    #    ALREADY-STRIPPED text by main(), so the case models that contract by
    #    stripping here too -- if someone routes raw text in, this goes red.
    commented = strip_comments("// D_Bare is discussed at length here.\n"
                               "EXPECT_EQ(x, DiagnosticCode::D_Covered);\n")
    check("a code named ONLY in a test comment stays UNCOVERED",
          find_uncovered(rows, commented), {"D_Bare"})

    block_commented = strip_comments("/* D_Bare, D_Covered: see the plan. */\n")
    check("a block comment covers nothing",
          find_uncovered(rows, block_commented), {"D_Bare", "D_Covered"})

    # ★ Substring safety: `D_Bare` must not be counted as covered by a longer
    #   identifier that merely contains it.
    check("a longer identifier does not cover a shorter code name",
          find_uncovered(rows, "D_BareMetalThing D_Covered"), {"D_Bare"})

    # ── (c2) NEXT-FREE-PER-BAND, the allocation-time half ───────────────────
    rows = parse_enumerators(extract_enum_body(
        _enum("D_A = 0xD029,", "D_B = 0xD02A,", "P_A = 0x0005,", "H_A = 0xF01A,")))
    # ★ Bands come from the NAME prefix, and `H_*` at 0xF is the case that
    #   proves it: keying on the value's high nibble would file it under 'D'
    #   and hand the next D_ allocator 0xF01B.
    check("next free is per NAME band, not per high nibble",
          next_free_by_band(rows),
          {"D": (0xD02A, 0xD02B), "P": (0x0005, 0x0006), "H": (0xF01A, 0xF01B)})

    # ★ A HOLE IS NOT OFFERED FOR RE-USE. 0xD027 is withdrawn-and-pinned; the
    #   append point stays above the highest, because a back-filled number may
    #   already have been published.
    rows = parse_enumerators(extract_enum_body(
        _enum("D_A = 0xD026,", "D_C = 0xD028,")))
    check("a withdrawn slot is not offered as next-free",
          next_free_by_band(rows), {"D": (0xD028, 0xD029)})

    # ── (c3) CROSS-BRANCH RESERVATIONS ──────────────────────────────────────
    # ★★ RUN ON A SYNTHETIC TABLE, UNCONDITIONALLY — changed 2026-08-18 with the
    # retirement of the PR #54 rows. These cases used to read the LIVE table under
    # `if RESERVED_ELSEWHERE:`, and the comment claimed that made them "go red the
    # day somebody empties it". ✔They do not: they SKIP. So the mechanism stopped
    # being pinned at exactly the moment the table went empty — which is the
    # moment the next author reaches for it and needs it to work.
    # ⚠ The live table is still exercised, by (c3-live) below, whenever it has
    # rows; what changed is that an EMPTY table no longer silences the mechanism.
    global RESERVED_ELSEWHERE
    _live = RESERVED_ELSEWHERE
    RESERVED_ELSEWHERE = ((0xE065, 0xE06B, "synthetic: pins the mechanism"),)
    try:
        low, high, _why = RESERVED_ELSEWHERE[0]
        rows = parse_enumerators(extract_enum_body(
            _enum("S_Taken = 0x%04X," % low, "S_Fine = 0x%04X," % (low - 1))))
        check("a code landing on a claimed ordinal is caught",
              [(n, v) for n, v, _w in find_reserved_conflicts(rows)],
              [("S_Taken", low)])

        # ★★ THE ACTUAL 0xE065 BUG: the append point must step OVER the whole
        #    claimed range, not merely past the highest local code.
        rows = parse_enumerators(extract_enum_body(
            _enum("S_Last = 0x%04X," % (low - 1))))
        check("next-free skips a claimed range entirely",
              next_free_by_band(rows), {"S": (low - 1, high + 1)})
    finally:
        RESERVED_ELSEWHERE = _live

    # (c3-live) — the real table, when it has rows: every reserved range must be
    # well formed. A row with low > high silently reserves NOTHING.
    for _low, _high, _why in RESERVED_ELSEWHERE:
        check("live reservation 0x%04X..0x%04X is well formed" % (_low, _high),
              _low <= _high and bool(_why), True)

    check("an unclaimed ordinal is not reported as a conflict",
          find_reserved_conflicts(parse_enumerators(extract_enum_body(
              _enum("D_A = 0xD001,")))), [])

    # ── (d) THE COLLAPSE GUARDS ─────────────────────────────────────────────
    check("a missing enum block is None, not an empty body",
          extract_enum_body("struct Unrelated { int x; };"), None)
    check("an unterminated enum block is None",
          extract_enum_body("enum class DiagnosticCode : std::uint16_t {\n  D_A = 1,"),
          None)

    # ── (e) THE ROOT IS THE TREE THIS FILE LIVES IN, whatever the caller's cwd ──
    # Arms, oracle and synthesized negatives are owned by .harness-config/runner/actions/owning-tree/owning-tree.py.
    for ok, why, detail in _owning_tree().root_arms(repo_root, (SystemExit,), False, __file__):
        # ★ Printed when it HOLDS as well: `check` reports only failures, and a red-on-disable
        # transcript must SHOW the CONTROL arm staying green rather than imply it by a count.
        if ok:
            print("  ok   %s" % why)
        check(why if ok else "%s -- %s" % (why, detail), ok, True)

    # ── (f) THE CROSS-BRANCH VIEW -- the pure core on DATA, then the git layer on a throwaway repository ──
    # ★ Every rule is a one-line fact: an addition is judged against ITS side's fork, a rename of a code that
    # existed at the fork is shared identity, a pair this tree already holds is shared, two OTHER sides can
    # collide with each other, and the union's append point steps past a slot another side took.
    base = {"S_Old": 0xE001}
    kinds = lambda found: [(k, key) for k, key, _a, _b in found]   # noqa: E731
    fails, notes, _u = cross_branch_findings({"S_Old": 0xE001, "S_Mine": 0xE002}, base,
                                             [("lane", base, {"S_Old": 0xE001, "S_Theirs": 0xE002}, True)])
    check("cross-branch: this tree's UNPUBLISHED allocation of a value a side holds under another name FAILS",
          (kinds(fails), notes), ([("value", "0xE002")], []))
    fails, notes, _u = cross_branch_findings({"S_Old": 0xE001, "S_Twice": 0xE002}, base,
                                             [("lane", base, {"S_Old": 0xE001, "S_Twice": 0xE003}, False)])
    check("cross-branch: this tree's unpublished name at a value an unmerged REF holds it at FAILS too",
          (kinds(fails), notes), ([("name", "S_Twice")], []))
    fails, notes, _u = cross_branch_findings({"S_Old": 0xE001, "S_Pub": 0xE002},
                                             {"S_Old": 0xE001, "S_Pub": 0xE002},
                                             [("dead", base, {"S_Old": 0xE001, "S_Stale": 0xE002}, False)])
    check("cross-branch: a side holding a value this tree already PUBLISHED under another name is REPORTED, "
          "not failed (that side renumbers before it merges)",
          (fails, kinds(notes)), ([], [("value", "0xE002")]))
    fails, notes, _u = cross_branch_findings({"S_Old": 0xE001}, base,
                                             [("lane", base, {"S_Renamed": 0xE001}, True)])
    check("cross-branch: a code that existed at the fork, renamed on one side, is NOT a collision",
          (fails, notes), ([], []))
    fails, notes, _u = cross_branch_findings({"S_Old": 0xE001, "S_Shared": 0xE002}, base,
                                             [("squashed", base, {"S_Old": 0xE001, "S_Shared": 0xE002}, False)])
    check("cross-branch: a pair this tree already holds (a squash-merged branch) is NOT a collision",
          (fails, notes), ([], []))
    fails, notes, _u = cross_branch_findings({"S_Old": 0xE001}, base,
                                             [("lane-a", base, {"S_Old": 0xE001, "S_A": 0xE002}, True),
                                              ("lane-b", base, {"S_Old": 0xE001, "S_B": 0xE002}, True)])
    check("cross-branch: two LIVE worktrees taking one slot FAILS -- a collision this tree can see",
          (kinds(fails), notes), ([("value", "0xE002")], []))
    fails, notes, _u = cross_branch_findings({"S_Old": 0xE001}, base,
                                             [("ref-a", base, {"S_Old": 0xE001, "S_A": 0xE002}, False),
                                              ("lane-b", base, {"S_Old": 0xE001, "S_B": 0xE002}, True)])
    check("cross-branch: a worktree against an unmerged REF is REPORTED here (that lane fails on it in its own "
          "tree)", (fails, kinds(notes)), ([], [("value", "0xE002")]))
    _f, _n, union = cross_branch_findings({"S_Old": 0xE001}, base,
                                          [("dead", base, {"S_Old": 0xE001, "S_Far": 0xE007}, False)])
    check("cross-branch: the union's append point steps past a slot ANY side ever took, a dead ref's included",
          next_free_by_band(union), {"S": (0xE007, 0xE008)})
    ok, detail = _cross_branch_git_arm()
    check("cross-branch: the git layer reads an in-flight worktree (control clean; then its UNCOMMITTED "
          "allocation collides with an unmerged ref -- a note from main, a FAILURE from the lane), skips "
          "merged refs, and steps the union's append point -- %s" % detail
          if not ok else "cross-branch: the git layer, end to end", ok, True)
    if ok:
        print("  ok   cross-branch: the git layer, end to end (control clean; the lane's in-flight "
              "collision is a note from main and a failure from the lane)")

    if failures:
        print("check-diagnostic-codes: SELF-TEST FAILED (%d case(s))" % len(failures))
        print("\n".join(failures))
        return 1
    print("check-diagnostic-codes: self-test OK")
    return 0


# ── main ─────────────────────────────────────────────────────────────────────

def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--self-test", action="store_true",
                    help="run the instrument's own red-on-disable cases and exit")
    ap.add_argument("--list-uncovered", action="store_true",
                    help="print every uncovered code, baselined or not, and exit 0")
    ap.add_argument("--cross-branch", action="store_true",
                    help="ALSO read every other worktree's working header and every ref not merged into HEAD: "
                         "fail on an ordinal two sides allocated differently, and print the append point "
                         "per band over the UNION (opt-in; the ctest form stays hermetic)")
    args = ap.parse_args()

    if args.self_test:
        return self_test()

    root = repo_root()
    header_path = os.path.join(root, HEADER_REL)
    if not os.path.isfile(header_path):
        sys.exit("collapsed scan: %s not found" % HEADER_REL)
    with open(header_path, "r", encoding="utf-8", errors="replace") as fh:
        header_text = fh.read()

    body = extract_enum_body(header_text)
    if body is None:
        sys.exit("collapsed scan: could not locate `%s ... };` in %s -- the "
                 "enum moved or was reshaped. REPORTING NOTHING IS NOT A PASS."
                 % (ENUM_DECL, HEADER_REL))

    rows = parse_enumerators(body)
    if len(rows) < MIN_PLAUSIBLE_CODES:
        sys.exit("collapsed scan: parsed only %d enumerators from %s (floor %d). "
                 "The parse broke; a clean verdict here would be fiction."
                 % (len(rows), HEADER_REL, MIN_PLAUSIBLE_CODES))

    test_code, test_files = read_test_code(root)

    duplicates = find_duplicates(rows)
    unvalued = find_unvalued(rows)
    uncovered = find_uncovered(rows, test_code)

    new_uncovered = sorted(uncovered - UNCOVERED_BASELINE)
    retired = sorted(UNCOVERED_BASELINE - uncovered)

    if args.list_uncovered:
        print("%d code(s) with no compiled test reference:" % len(uncovered))
        for name in sorted(uncovered):
            mark = "baselined" if name in UNCOVERED_BASELINE else "NEW"
            print("  %-9s %s" % (mark, name))
        return 0

    print("check-diagnostic-codes: %d codes in %s, %d test file(s) scanned"
          % (len(rows), HEADER_REL, test_files))

    failed = False

    if duplicates:
        failed = True
        print("\nFAIL -- ORDINAL COLLISION: two names share one published identity.")
        print("  This is the 0xD029 failure. Whichever code is newer must move to "
              "the next free ordinal; an ALREADY-PUBLISHED code never moves.")
        for value, names in duplicates.items():
            print("    0x%04X  %s" % (value, ", ".join(names)))

    if unvalued:
        failed = True
        print("\nFAIL -- IMPLICIT ORDINAL: enumerator(s) with no explicit value.")
        print("  These take `predecessor + 1`, so inserting a row above them "
              "silently renumbers a published error code.")
        for name, raw in unvalued:
            print("    %s%s" % (name, (" = %s" % raw) if raw else ""))

    conflicts = find_reserved_conflicts(rows)
    if conflicts:
        failed = True
        print("\nFAIL -- CROSS-BRANCH ORDINAL COLLISION: this branch allocated a "
              "code another branch already owns.")
        print("  Caught BEFORE the merge, which is the whole point -- moving it "
              "now is one line; after both land it is a published renumber.")
        for name, value, why in conflicts:
            print("    0x%04X  %s\n             claimed by %s" % (value, name, why))

    if new_uncovered:
        failed = True
        print("\nFAIL -- %d code(s) allocated with NO compiled test reference:"
              % len(new_uncovered))
        for name in new_uncovered:
            print("    %s" % name)
        # ASCII only in printed output: this runs under the Windows console's
        # legacy code page on the MSVC-Debug gate leg, where a stray non-ASCII
        # byte renders as a replacement character and makes the ONE line a
        # reader needs look like corruption.
        print("  A code no test names is a code nothing pins. Add a test, or "
              "escalate it as a section-B decision -- do NOT append it to "
              "UNCOVERED_BASELINE to reach green.")

    if retired:
        print("\n  progress: %d baselined code(s) are now covered and their "
              "UNCOVERED_BASELINE line(s) should be deleted:" % len(retired))
        for name in retired:
            print("    %s" % name)

    if args.cross_branch:
        # The in-tree verdict above is printed first and still counts: the union's answer is for an
        # allocator, and it is no excuse for a collision inside this tree.
        print()
        failed = cross_branch_main(root, rows) != 0 or failed

    if failed:
        return 1

    print("check-diagnostic-codes: OK -- 0 collisions, 0 implicit ordinals, "
          "0 new uncovered codes (%d baselined)." % len(uncovered))
    # Printed on the GREEN path on purpose: this is the number a lane needs
    # BEFORE it allocates, and the collision this gate exists for happened
    # because that number came from a brief instead of from the header.
    print("  next free ordinal per band (append point, not the lowest hole):")
    print("    " + "   ".join(
        "%s_ 0x%04X" % (letter, nxt)
        for letter, (_high, nxt) in next_free_by_band(rows).items()))
    if RESERVED_ELSEWHERE:
        print("  reserved by another branch and SKIPPED in the numbers above:")
        for low, high, why in RESERVED_ELSEWHERE:
            print("    0x%04X-0x%04X  %s" % (low, high, why))
    return 0


if __name__ == "__main__":
    # ★★ THE NO-ARGUMENT FORM VERIFIES THE TREE **AND** PROVES THE GATE CAN FAIL.
    # Added 2026-08-23, when this gate had
    # no ctest entry and no CI step: it ran only when a human remembered it -- and
    # the whole reason it exists is that "a lane allocates an ordinal and nothing
    # mechanical notices". Registering it was half the fix; the other half is here,
    # because `main()` runs `self_test()` ONLY under `--self-test` and the ctest
    # entry passes no flag. ✔MEASURED 2026-08-22 on `enum_name_table_guard`: exactly
    # that shape left its ctest form verifying the tree and proving nothing for a
    # day, while a comment beside it claimed otherwise. ⇒ the self-test now runs
    # on every invocation that does not already ask for something narrower, and BOTH
    # statuses are honoured -- `rc or rc_self`, so neither half can hide the other.
    if len(sys.argv) > 1:
        sys.exit(main())
    _rc = main()
    print()
    _rc_self = self_test()
    sys.exit(_rc or _rc_self)
