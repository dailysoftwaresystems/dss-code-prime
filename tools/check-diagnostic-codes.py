#!/usr/bin/env python3
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
      diligence, which is the same non-mechanism that `tools/run-gate.sh` was
      written to replace. Two lanes, one counter, no lock.

  (2) ★ A CODE LANDED WITH NO TEST AT ALL. `D-AP6-NEW-DIAGNOSTIC-CODES-HAD-NO-
      VALUE-PIN` closed on exactly this and it RE-OPENED ONE CYCLE LATER:
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
  "never ran" is the exact failure `tools/run-gate.sh` exists to forbid, and it
  would be absurd for the gate that enforces that lesson to embody its inverse.

⚠⚠ RUN THIS ON A QUIET TREE, and the reason is specific rather than general
   caution. This gate reads the WORKTREE, and the project's own red-on-disable
   discipline REQUIRES a lane to temporarily write knowingly-wrong bytes into
   shipped source to prove a pin inverts. ✔MEASURED 2026-08-15: while a sibling
   lane was proving its 0xD029 pin, the header transiently read
   `D_DependencyBuildFailed = 0xD0F9`, and a run taken in that window reported a
   GREEN verdict and a next-free D_ band of 0xD0FA -- both computed from bytes
   that exist in no real tree. Nothing was wrong with the instrument; the tree
   was mid-mutation by design. Same family as
   `D-GATE-TWO-CONCURRENT-CTEST-RUNS-IN-ONE-BUILD-DIR-YIELD-NO-VERDICT`.

Usage:
    python tools/check-diagnostic-codes.py
    python tools/check-diagnostic-codes.py --self-test
    python tools/check-diagnostic-codes.py --list-uncovered

Exit codes:  0 = pass   1 = gate failure   2 = usage / collapsed scan
"""

import argparse
import os
import re
import subprocess
import sys

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
# Each entry: (low, high, why) — INCLUSIVE on both ends.
RESERVED_ELSEWHERE = (
    (0xE065, 0xE06B,
     "PR #54 (feature/c23-conformance-burndown-3): S_InlineAsm* x7. "
     "RETIRE THIS ROW WHEN #54 MERGES."),
    (0xB010, 0xB012,
     "PR #54 (feature/c23-conformance-burndown-3): L_SideStructure* x3. "
     "RETIRE THIS ROW WHEN #54 MERGES."),
)


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
    "P_InvalidEscape",
    "C_CircularShape",
    "C_UnclosableScope",
    "C_InvalidLanguageName",
    "C_InvalidShippedFfiHeaderPath",
    "S_IndirectCallNotSupported",
    "S_DuplicateLabel",
    "S_UndefinedLabel",
    "S_BitIntWidthNotConstant",
    "S_BitIntSignedWidthTooSmall",
    "S_BitIntWidthExceedsMax",
    "S_BitIntWidthAboveC1Limit",
    "D_OutputDirCreateFailed",
    "D_DirectoryScanFailed",
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
    "L_CcRegLookupFailed",
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

def repo_root():
    p = subprocess.run(["git", "rev-parse", "--show-toplevel"],
                       capture_output=True, text=True)
    if p.returncode != 0:
        sys.exit("not inside a git repository")
    return p.stdout.strip()


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


# ── the self-test: red-on-disable for the instrument itself ──────────────────

def _enum(*lines):
    return ("enum class DiagnosticCode : std::uint16_t {\n"
            + "\n".join("    " + ln for ln in lines)
            + "\n};\n")


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
    # ★ Uses the REAL RESERVED_ELSEWHERE table, so these cases go red the day
    #   somebody empties it while another branch still owns those ordinals.
    if RESERVED_ELSEWHERE:
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

    check("an unclaimed ordinal is not reported as a conflict",
          find_reserved_conflicts(parse_enumerators(extract_enum_body(
              _enum("D_A = 0xD001,")))), [])

    # ── (d) THE COLLAPSE GUARDS ─────────────────────────────────────────────
    check("a missing enum block is None, not an empty body",
          extract_enum_body("struct Unrelated { int x; };"), None)
    check("an unterminated enum block is None",
          extract_enum_body("enum class DiagnosticCode : std::uint16_t {\n  D_A = 1,"),
          None)

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
    sys.exit(main())
