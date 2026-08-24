#!/usr/bin/env python3
# PURPOSE: refuse a new present-tense refusal sentence that cites an anchor row already marked CLOSED.
"""check-stale-refusal-citations.py -- the LIVE-BLOCKER CITATION ratchet.

D-COMMENT-A-CLAIM-TRUE-WHEN-TYPED-AND-FALSE-WHEN-THE-COMMIT-LANDED -- the species
this guard mechanises.

★★★ WHY THIS EXISTS, AND WHY THE WINDOW IS MINUTES RATHER THAN CYCLES.
An author writes *"X therefore stays refused fail-loud (D-SOME-ROW)"* while that
row is OPEN. A sibling lane closes the row IN THE SAME COMMIT. The sentence was
TRUE WHEN IT WAS TYPED and FALSE WHEN THE COMMIT LANDED -- and it still READS as
evidence, so the next author is sent to fix an engine gap that no longer exists.
Nothing fails. Nothing is silent-in-the-usual-sense either: the claim is loud,
prominent, and wrong.

★★★ AND THE REASON IT MUST BE MECHANICAL RATHER THAN A SWEEP. The cycle that
named the species closed it claiming TWO instances, found by reading the diff.
Two independent auditors then found at least EIGHT MORE in the same tree, in
files the diff never touched -- including one in a config `$comment` FOURTEEN
LINES ABOVE the very feature it declared absent. An instance-driven sweep finds
the instances someone happened to read; a predicate finds the class.

── THE PREDICATE, AND EVERY NARROWING IN IT IS A MEASURED FALSE POSITIVE ────────
A finding is ONE SENTENCE that satisfies all six:
  (1) it cites at least one id that IS a deferral row (any sanctioned home);
  (2) EVERY row id it cites is CLOSED;
  (3) after the ids are MASKED OUT, it carries a PERSISTENCE word followed within
      26 characters by a REFUSAL word (or the predicative `still/stays/remains
      open`);
  (4) no past-tense GOVERNOR sits immediately before that phrase;
  (5) no RETRACTION marker appears anywhere in the sentence;
  (6) no RESIDUAL-BOUNDARY qualifier scopes the claim to something OUTSIDE what
      the cited row closed.

★ (1) IS A MEMBERSHIP TEST, NOT A SHAPE TEST. An id that names no row anywhere is
somebody else's finding (`check-anchor-registry` refuses an unresolvable
citation); here it is simply not a citation of a closed row.

⚠⚠ EVERY FIGURE BELOW WAS RE-MEASURED AGAINST THE **SHIPPED** PREDICATE ON
2026-08-24, AFTER ITS LAST EDIT, not carried over from the drafts that produced
it. A guard about claims that were true when they were typed does not get to quote
a number it took before its own last change -- and two of these figures DID move
under exactly that pressure while this file was being written. ✔The shipped live
population is 9 sites across 8 files, over 2582 governed files, 2274 known rows,
1261 of them closed. ✔Every lever below is measured with the others held fixed;
all six relaxed at once gives 147, so the predicate as a whole is worth 16x.

★★ (2) IS THE ATTRIBUTION RULE AND IT ERRS TOWARD SILENCE ON PURPOSE. A sentence
that cites BOTH a closed row and an open one has an open row its persistence
claim can honestly attach to. ✔MEASURED 12 -> 9, and every site it drops is
correct -- e.g. `src/opt/passes/licm.hpp` calls one anchor *"(closed by that
gate)"* and its sibling *"(still open …)"* IN THE SAME SENTENCE. ⓘ Deferring is
not dropping: the day the last open row in such a sentence closes, the file's
count rises above its ceiling and this guard reds.

★★★ (3) DEMANDS A **PERSISTENCE** WORD, NOT A COPULA, AND THIS IS THE SINGLE
BIGGEST PRECISION LEVER. The tempting spelling admits `is`/`are`, which turns
every DEFINITIONAL sentence into a finding -- *"an unclosed frame description IS
A REFUSAL, not a truncation"*, *"a repeat in this name space IS A REFUSAL"*,
*"`nullptr` in a variadic position IS REJECTED fail-loud"*. Those cite closed
rows and are PERMANENTLY TRUE: the closure DELIVERED the refusal, so the refusal
is the fix and not the gap. ✔MEASURED: admitting the copula moves the population
9 -> 101. What the defect actually asserts is that a state PERSISTS -- `still`,
`stays`, `remains`, `not yet`, `today`, `currently` -- and every known instance is
spelled that way, which is why the class is the whole of the positive test rather
than one member of it.
⚠ THE COST IS STATED RATHER THAN HIDDEN: a bare *"X is refused (D-CLOSED-ROW)"*
that went stale is NOT reported. That is the FP-safe direction, and this guard's
first false accusation is the one that gets it turned off.

★★ (3) ALSO MASKS THE IDS, AND THE HONEST MEASUREMENT IS THAT THE MASK BUYS
**NOTHING TODAY**. Row names ARE English -- one ends in `-IS-UNELECTABLE`, another
carries `CANNOT` in the middle -- but ✔MEASURED against the shipped predicate,
masked and unmasked BOTH report 9: the persistence-only trigger already refuses
the constructions a row name accidentally forms. It was decisive in the
copula-admitting draft, where the same pair measured 124 masked against 154
unmasked.
★ IT STAYS ANYWAY, and the reason is not caution: a row's NAME is not prose in any
draft, so a guard that reads it as prose is wrong even on a tree where being wrong
happens to cost nothing. The property is pinned by a self-test arm that asserts
the RAW pattern DOES match the fixture while the predicate does not -- so the
guarantee does not depend on today's population, and widening the trigger later
cannot silently reintroduce the class.

★ (4) IS A **BACKWARD WINDOW**, NOT A WHOLE-SENTENCE TEST, and the difference is
what keeps the exemption from swallowing real findings. A repaired site reads
*"THIS PARAGRAPH SAID `movw $imm, %reg` IS STILL REFUSED AND IT WAS FALSE IN ITS
OWN COMMIT"* -- the governor (`SAID`) sits immediately BEFORE the phrase, where a
governor grammatically belongs. A whole-sentence test would also exonerate any
long sentence that happens to use `was` about something else.

★★★ (6) IS THE LARGEST FALSE-POSITIVE CLASS THIS GUARD HAS, AND IT WAS FOUND BY AN
INDEPENDENT REVIEWER RATHER THAN BY ME. ✔MEASURED 17 -> 9: EIGHT sites are a TRUE
sentence citing a CLOSED row, because **the row closed a SUBSET of what the
sentence refuses**. `large_frame_arm64/main.c` says a frame **> 16 MiB** stays
fail-loud while the row it cites closed by implementing frames **up to** 16 MiB;
`hir_to_mir.cpp` says **any OTHER** lvalue kind is still unsupported;
`test_mir_lowering_c_subset.cpp` says a width **EXCEEDING** the 64-bit base stays
rejected. Nothing is stale in any of them. The tell is structural -- the claim's
SUBJECT carries a threshold or an exception quantifier -- which is why this is a
narrowing rather than eight inventory lines with no argument behind them.
See `BOUNDARY` for the window and for the genuine finding a whole-sentence form
would have silenced.

── THE GOVERNED SET, STATED EXPLICITLY ─────────────────────────────────────────
Every file `git ls-files --cached --others --exclude-standard` reports, MINUS
`.plans/`.
  * WHY GIT AND NOT A ROOT LIST: the repository already owns a definition of what
    it contains and `.gitignore` already excludes `build/` and the scratch trees.
    A second definition here would drift. `--others` means a lane's brand new,
    not-yet-added file is scanned, so a stale claim cannot land by arriving
    before its `git add`. Same reasoning, same call, as `check-wrapped-anchor-ids`.
  * ⚠⚠ WHY `.plans/` IS OUT, AND IT IS A MEASUREMENT RATHER THAN A CARVE-OUT.
    ✔MEASURED 2026-08-24 with the shipped predicate: 40 of the 49 tree-wide
    candidates live under `.plans/`, **27 of them in
    `_deferred-anchor-registry.md` alone** and 5 more in `plan-00`. Those
    documents are REWRITTEN EVERY CYCLE by design, so a per-file ceiling on them
    would move in BOTH directions on ordinary editing -- a new row reds as a
    regression, a swept row reds as unclaimed headroom -- and a guard that fires
    on routine work is a guard that gets weakened, which asserts nothing.
    ★ AND THE PLAN SIDE IS ALREADY GOVERNED, BY THE INSTRUMENT THAT OWNS IT: a
    registry row whose own opening verdict contradicts its marker is
    `check-anchor-balance`'s mismarked-closure arm, and a row that still PRESENTS
    as blocked while its trigger has fired is that same instrument's `unblocked`
    arm. A closed row's status cell RECAPPING what used to be refused is not a
    defect at all -- it is what a closure looks like.
    ⇒ The subject here is documentation shipped INSIDE the artifact: source
    comments, `$comment` keys in `.lang`/`.target`/`.format` config, example
    headers, `expected.json` rationales, test comments. A reader takes those as a
    live statement of what the compiler does, and nothing rewrites them per cycle.

── THE ROW SETS: IMPORTED, NEVER RE-DERIVED ────────────────────────────────────
`check-anchor-balance` is imported and asked for both populations.
  * A ROW IS CLOSED IFF ITS STATUS CELL BEGINS WITH THE CLOSURE MARK after
    `lstrip("*_ ")` -- `is_closed()`, unchanged and unread by this file. THE
    COMPLEMENT IS DEFINED, NEVER THE VARIANTS: an ad-hoc enumeration of status
    glyphs has produced a wrong count TWICE in this repository, both times TOO
    LOW, and this guard would fail in the DANGEROUS direction if it repeated the
    mistake -- a row wrongly believed OPEN makes a stale sentence invisible.
  * The same import carries the escaped-pipe-aware row splitter, the three table
    widths, and the per-table shape recognition. A second copy of any of that is
    how two instruments start disagreeing about the same registry -- an
    unescaped-pipe split has already mis-read 161 rows here.
  * A NAME IS OPEN IF **ANY** OF ITS HOMES IS OPEN, which is the safe direction:
    a closed duplicate cannot mask an open original, so a citation is convicted
    only when every home agrees the work is done.

── THE RATCHET, AND WHY IT IS NOT A BAN ────────────────────────────────────────
Pre-existing candidates are recorded in `inventory.json` as per-file ceilings
that may only come DOWN. Green means NO NEW instance landed; it never means the
tree is clean.
  * ★★★ THE RATCHET IS ALSO WHAT MAKES A STATE PREDICATE CATCH A TRANSITION, and
    that is the design rather than a side effect. This guard has no base ref, so
    it cannot ask what changed. It does not need to: the day a sibling lane
    closes a row, every sentence anywhere in the tree that still calls that row a
    live blocker BECOMES a candidate, the owning file rises above its ceiling,
    and the run reds -- naming the file and quoting the sentence. That is exactly
    the minutes-wide window the species is about, detected without a diff.
  * `--write` is the BURN-DOWN verb and CAN ONLY LOWER. It refuses, and writes
    NOTHING, while any file sits above its ceiling -- otherwise the command a
    lane reaches for after a red would launder the regression that caused it.
  * `--baseline` establishes new ground unconditionally, says so loudly, and is
    meant to be reviewed AS A DIFF. It is the bootstrap verb, never the way to
    make a red go away.
ⓘ The verb split is ported from `check-wrapped-anchor-ids` / `check-plan-citations`
rather than re-invented: a second, differently-behaved ratchet in the same
battery is how two instruments start disagreeing about what a ratchet promises.

── NO LINE NUMBERS, ANYWHERE, INCLUDING IN THIS GUARD'S OWN OUTPUT ─────────────
A finding names the file, the row ids it cites, and quotes the sentence. That is
enough to find it with a search, and it stays true when the file moves. Operator
rule, 2026-08-19.

── FAIL-CLOSED ─────────────────────────────────────────────────────────────────
Too few governed files, too few known rows, or a missing inventory is a REFUSAL
with exit 2, never a pass. An empty scan is a COLLAPSE
(D-GATE-ANCHOR-GUARD-FAILS-OPEN-ON-MISSING-ROOT).

Usage:
    python scripts/check-stale-refusal-citations/check-stale-refusal-citations.py
    python scripts/check-stale-refusal-citations/check-stale-refusal-citations.py --list
    python scripts/check-stale-refusal-citations/check-stale-refusal-citations.py --write
    python scripts/check-stale-refusal-citations/check-stale-refusal-citations.py --baseline
    python scripts/check-stale-refusal-citations/check-stale-refusal-citations.py --selftest

★★ THE NO-ARGUMENT FORM (the ctest form) VERIFIES THE TREE **AND THEN RUNS THE
SELF-TEST**, honouring both statuses and short-circuiting NEITHER. A guard whose
self-test runs only behind a flag proves nothing when registered without one, and
`return rc or selftest()` would skip the arms exactly when the tree is red -- i.e.
at the only moment anyone needs to trust the instrument
(D-TEST-NONFATAL-GUARD-DEGRADES-TO-A-VACUOUS-PASS).

Exit codes: 0 clean · 1 a new instance / a stale inventory · 2 the scan
collapsed · 3 usage error.
"""

import contextlib
import importlib.util
import io
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

# ── OUTPUT ENCODING -- NOT COSMETIC, AND THE STREAM IS HALF THE FACT ───────────
# ✔MEASURED 2026-08-23 (CPython 3.14.3, Windows, BOTH streams PIPES, which is
# exactly how ctest runs every guard): `sys.stdout` comes up `cp1252` with
# `errors='surrogateescape'`, and `surrogateescape` rescues only lone surrogates
# left by an earlier decode -- it does NOTHING for an ordinary unencodable
# character. This guard QUOTES TREE PROSE, and this tree's comments carry ⚠, ★,
# box drawing and em-dashes, so a report would raise `UnicodeEncodeError` and kill
# the guard INSIDE ITS OWN FINDING: the run still reds, but the sentence is lost
# and the traceback names a `print`. Applied at IMPORT rather than in `main()`, so
# `--help`, argument errors and any death during module initialisation are covered
# too. D-GATE-PYTHON-GUARD-DIES-PRINTING-TREE-TEXT-ON-A-WINDOWS-PIPE
for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError, OSError):    # pragma: no cover - odd stream
        pass

EXIT_OK, EXIT_RATCHET, EXIT_COLLAPSE, EXIT_USAGE = 0, 1, 2, 3

GUARD = "stale-refusal-citations"
INVENTORY_REL = os.path.join("scripts", "check-stale-refusal-citations",
                             "inventory.json")

# ⚠ The plan tree is EXCLUDED -- see "THE GOVERNED SET" in the module docstring
# for the measurement, and for which instrument owns the registry side instead.
EXCLUDED_PREFIX = ".plans/"

# Floors catch a COLLAPSED scan, never drift. Set far below the live figures
# (2621 governed files, 2271 known rows, 1258 of them closed ✔MEASURED
# 2026-08-24) so ordinary churn cannot trip them.
# ★ THE NAME FLOOR IS THE LOAD-BEARING ONE AND ITS DIRECTION WAS CHECKED: losing
# rows from the KNOWN set shrinks the closed set, which makes stale sentences
# INVISIBLE -- a silent pass. Losing the OPEN set only inflates the closed set,
# which over-reports, i.e. reds. So both are floored and the dangerous direction
# is floored hardest.
FILE_FLOOR = 1200
NAME_FLOOR = 1200
CLOSED_FLOOR = 500


class Collapse(Exception):
    """The scan could not be trusted. Distinct from a finding."""


# ═══════════════════════ THE SENTENCE, AND THE PREDICATE ═════════════════════
#
# ★ A JSON `$comment` IS ONE PHYSICAL LINE CARRYING MANY PARAGRAPHS, so the
# two-character escape `\n` is a paragraph break for this reader exactly as a real
# newline is. Without that, `x86_64.target.json` -- the file that carried the
# fourteen-lines-above instance -- is a single unsplittable blob and every
# sentence in it is "the same sentence" as every other.
_ESCAPED_NEWLINE = re.compile(r"\\n|\\r")
_BLOCK = re.compile(r"\n\s*\n")
# ★★ A TERMINATOR MAY BE FOLLOWED BY CLOSERS, and omitting them joined two
# unrelated statements into one "sentence". ✔MEASURED 2026-08-24 in
# `hir_to_mir.cpp`: a parenthetical ending `… but never SILENTLY here.)` did not
# terminate, so the claim inside it was welded to the `unsupported(node, "…")`
# call BELOW it -- and that call string carries two anchor ids. The finding was
# real text against a real citation and the two had nothing to do with each other.
# ★★ AND A JSON STRING VALUE FOLLOWED BY THE NEXT **KEY** ENDS A SENTENCE, which
# is a structural boundary as hard as a full stop. ✔MEASURED in both
# `pe64-*.format.json`: a `$comment` about `addrAlign` ran on through
# `"kind": "rodata", … ` into the NEXT `$comment`, whose first token is an anchor
# id -- so a sentence about a numeric field value was convicted of citing a row it
# never mentions.
# ⚠⚠ THE `"key":` LOOKAHEAD IS NOT DECORATION, AND DROPPING IT COST A REAL
# FINDING. A bare `",` rule also splits `", "` between two ELEMENTS OF A STRING
# LIST -- and this repository writes multi-sentence prose that way, one paragraph
# per list, one fragment per element. ✔MEASURED 2026-08-24: the bare rule silenced
# `check-wrapped-anchor-ids.py`'s own inventory comment, where the citation sits in
# one element and *"which stays OPEN until `ceilings` is empty"* in the next -- a
# genuine live instance, of a row THIS CYCLE closed. A boundary between a VALUE and
# a KEY is structural; a boundary between two prose fragments is typography.
_TERMINATOR = re.compile(
    "(?:[.;!?][)\\]\"'”’]*|\",(?=\\s*\"[A-Za-z_$][A-Za-z0-9_$-]*\"\\s*:))(?=\\s)")


def sentences(text):
    """The text as sentences: blank-line blocks, then `. ; ! ?` boundaries.

    ★★★ THE UNIT IS A SENTENCE AND NOT A LINE WINDOW, AND THAT IS LOAD-BEARING
    RATHER THAN TIDY. ✔MEASURED 2026-08-24 against a sibling lane's hand-written
    ±6-line predicate over the same tree: a 2 KB JSON `$comment` is ONE PHYSICAL
    LINE, so a line window swallows the whole paragraph, and one occurrence of a
    past-tense word anywhere in those 2 KB suppressed a hit that was real --
    `examples/c-subset/c_inline_asm_memory_arithmetic/expected.json`, which this
    guard reported and the line-window predicate did not. A line is a formatting
    artefact; a claim is a sentence.

    ⚠ `:` IS DELIBERATELY NOT A TERMINATOR. The house style writes
    `<claim>: <because>`, so a colon separates a claim from its reason and both
    halves belong to one assertion -- splitting there would put the refusal in
    one unit and the citation that justifies it in another.
    """
    out = []
    for block in _BLOCK.split(_ESCAPED_NEWLINE.sub("\n\n", text)):
        block = " ".join(block.split())
        if not block:
            continue
        start = 0
        for m in _TERMINATOR.finditer(block):
            out.append(block[start:m.end()])
            start = m.end()
        if start < len(block):
            out.append(block[start:])
    return [s for s in (s.strip() for s in out) if s]


# ── the vocabulary. Each list is a POSITIVE declaration, so it is enumerated ──
# and every member was read in the tree before it was added. That is the same
# licence `check-anchor-balance`'s WALK_BACK list takes and for the same reason:
# there is no complement of "asserts a refusal persists" to invert. The residual
# risk is a MISS, never a false accusation, and a miss is this guard's safe
# direction.
PERSISTENCE = (r"STILL|STAYS?|REMAINS?|CONTINUES? TO BE|NOT YET|AS YET|SO FAR|"
               r"TO DATE|FOR NOW|AT PRESENT|CURRENTLY|TODAY")
REFUSAL = (r"REFUSED|REFUSES|REFUSAL|UNSUPPORTED|UNSUPPORTABLE|NOT SUPPORTED|"
           r"REJECTED|REJECTS|FAIL-?LOUD|FAILS? LOUD|UNSPELLABLE|UNELECTABLE|"
           r"UNUSABLE|NOT IMPLEMENTED|UNIMPLEMENTED|BLOCKED|NOT ACCEPTED")

# ★ THE 26-CHARACTER GAP WAS SIZED, NOT GUESSED. It admits the spellings the tree
# actually uses -- `stay refused`, `IS STILL REFUSED`, `stays fail-loud`,
# `therefore stay REFUSED`, `still fails loud S_...` -- while refusing to reach
# across a clause. ⚠ It is also what makes the id MASK an effective separator: a
# masked row name is 20-60 characters wide, so a persistence word on one side of a
# citation can never bind to a refusal word on the other.
GAP = 26

# ★★★ `OPEN` IS A REFUSAL WORD ONLY IN THE **PREDICATIVE** CONSTRUCTION, AND IT
# GETS ITS OWN TIGHT PATTERN RATHER THAN A SEAT IN `REFUSAL`. Calling a row "still
# open" is the same claim as calling it "still refused" -- the row is being
# presented as a LIVE BLOCKER, which is the species, and this pattern contributes
# three live findings including one inside a sibling guard's own inventory comment
# about a row THIS CYCLE closed. ⚠ BUT `open` IS ALSO ORDINARY COMPILER JARGON:
# ✔MEASURED 2026-08-24, `hir_to_mir.cpp` says an `Alloca` is emitted "into the
# ENTRY block, which is still the open block here", where `open` is an ATTRIBUTIVE
# adjective on `block` and nothing is deferred at all. Inside the general 26-char
# gap that convicted it. Requiring the persistence word and `open` to be ADJACENT
# admits `still open`, `stays OPEN`, `remain open` and `still-open` while refusing
# `still the open block`, and it is a construction rather than an exception list.
OPEN_CLAIM = r"\b(?:STILL|STAYS?|REMAINS?)[-\s]+OPEN\b"

CLAIM = re.compile(r"\b(?:%s)\b[^.;!?]{0,%d}?\b(?:%s)\b|%s"
                   % (PERSISTENCE, GAP, REFUSAL, OPEN_CLAIM), re.IGNORECASE)

# ★★★ A REFUSAL SCOPED TO A **BOUNDARY OUTSIDE WHAT THE ROW CLOSED** IS TRUE, NOT
# STALE, AND IT IS THE LARGEST FALSE-POSITIVE CLASS THIS GUARD HAS.
# ✔MEASURED 2026-08-24: `examples/c-subset/large_frame_arm64/main.c` says a frame
# **> 16 MiB** stays fail-loud while the row it cites closed by implementing frames
# **up to** 16 MiB. The sentence is TRUE, the citation is CORRECT, and the row
# closed a SUBSET of what the sentence refuses. Eight of the twenty-one live sites
# are that shape -- the AArch64 frame-offset family, `Any OTHER lvalue kind`, and
# `a width EXCEEDING the 64-bit base`.
# ★ THE TELL IS STRUCTURAL, WHICH IS WHY THIS IS A NARROWING AND NOT AN EXCEPTION
# LIST: the claim's SUBJECT carries a quantified threshold (`> N`, `beyond`,
# `exceeding`, `outside`) or an exception quantifier (`any other`, `other than`).
# Prose that means "the thing itself is still refused" has no such qualifier.
# ⚠ IT IS A **BACKWARD** WINDOW, for the same reason the governor is: the
# qualifier belongs to the subject, which precedes the verb. ✔MEASURED that the
# whole-sentence form would silence a GENUINE finding --
# `test_parser_speculation.cpp` writes *"The deep-nest residual (…) is the
# separate, still-open <row>"*, where the qualifier describes something else
# entirely and sits 100 characters upstream.
# ⚠ `->` and `=>` are excluded from the comparison: they are C++ and prose arrows,
# not thresholds.
BOUNDARY = re.compile(
    r"(?:(?<![-=])>=?|≥|\bBEYOND\b|\bEXCEED(?:S|ING)?\b|\bOUTSIDE\b|"
    r"\bGREATER THAN\b|\bLARGER THAN\b|\bMORE THAN\b|\bANY OTHER\b|"
    r"\bEVERY OTHER\b|\bALL OTHER\b|\bOTHER THAN\b|\bANYTHING ELSE\b)"
    r"[^.;!?]{0,70}$", re.IGNORECASE)

# ★ THE GOVERNOR WINDOW LOOKS **BACK** FROM THE PHRASE, because that is where a
# past-tense governor grammatically sits: *"THIS PARAGRAPH **SAID** … IS STILL
# REFUSED"*, *"it **was** … still open"*, *"**until** this cycle it stayed
# refused"*. 48 characters is one clause.
GOVERNOR = re.compile(
    r"\b(?:WAS|WERE|SAID|USED TO|UNTIL|HAD BEEN|BEFORE|PREVIOUSLY|FORMERLY|"
    r"ONCE|CLAIMED|READ|WROTE|WRITTEN|AT THE TIME|THEN)\b[^.;!?]{0,48}$",
    re.IGNORECASE)
# A sentence that RETRACTS its own claim is exempt wherever the retraction sits --
# by construction it is talking about a sentence rather than about the compiler.
# ★ THE `… IS CLOSED` FAMILY IS PART OF IT, AND IT WAS ADDED FROM A MEASURED FALSE
# POSITIVE rather than anticipated. `x86_64.target.json` carries a paragraph whose
# whole subject is this defect species; it says *"the older `Imm16` … `checkSlotShape`
# still refuses THAT one here at load … and [[<row>]] is CLOSED"*. The refusal is
# real, permanent and about a DIFFERENT thing, and the sentence states the row's
# status itself. A sentence that says the row it cites is closed is not presenting
# that row as a live blocker, which is the entire predicate.
RETRACTION = re.compile(
    r"\bNO LONGER\b|\bWAS FALSE\b|\bIS FALSE\b|\bWAS WRONG\b|\bSINCE CLOSED\b|"
    r"\bNOW COMPILES\b|\bNOW LANDS\b|\bCORRECTED\b|\bUSED TO\b|"
    r"\b(?:IS|ARE|WAS|WERE|NOW|HAS BEEN|HAVE BEEN)\s+CLOSED\b|"
    r"\bCLOSED IT\b|\bCLOSES IT\b", re.IGNORECASE)

# ⚠ ONE CHARACTER WIDE AND ASCII, so masking preserves every offset into the
# original sentence and the quoted excerpt still lines up with what the author
# wrote. A word character would splice the id's neighbours into a new word.
MASK_CHAR = "#"


def claim_in(sentence, anchor_token):
    """The persistence-refusal phrase in `sentence`, with row ids masked, or None.

    Returns the `re.Match` against the MASKED text; its offsets are valid in the
    original because the mask is width-preserving.
    """
    masked = anchor_token.sub(lambda m: MASK_CHAR * len(m.group(0)), sentence)
    m = CLAIM.search(masked)
    if m is None:
        return None
    if GOVERNOR.search(masked[:m.start()]) or RETRACTION.search(masked):
        return None
    if BOUNDARY.search(masked[:m.start()]):
        return None
    return m


def findings_in(text, anchor_token, names, closed):
    """-> [(cited_ids, excerpt, sentence)] for one file's text.

    `names` is every id that HAS a row; `closed` is the subset whose every home
    is closed. Both come from `check-anchor-balance`; neither is derived here.
    """
    found = []
    for sentence in sentences(text):
        cited = set(anchor_token.findall(sentence)) & names
        if not cited or (cited - closed):
            continue
        m = claim_in(sentence, anchor_token)
        if m is None:
            continue
        found.append((sorted(cited),
                      sentence[max(0, m.start() - 30):m.end() + 30].strip(),
                      sentence))
    return found


# ═══════════════════════ THE ROW SETS AND THE GOVERNED SET ═══════════════════
def _load_anchor_balance(root):
    """`check-anchor-balance` as a module, or a loud death.

    Imported rather than copied for the reason `check-wrapped-anchor-ids` imports
    it: two copies of "what a row is and when it is closed" is exactly the drift
    the whole registry discipline exists to stop. The import fails LOUD if the
    sibling moves.
    """
    sibling = os.path.join(root, "scripts", "check-anchor-balance",
                           "check-anchor-balance.py")
    if not os.path.isfile(sibling):
        raise Collapse(
            "cannot find the shared anchor vocabulary at %s.\n"
            "  This guard must take its row population, its closed-status rule and "
            "its anchor-token pattern from that script, or the two instruments "
            "start disagreeing about what a CLOSED row IS. Restore the sibling; do "
            "NOT copy its definitions here."
            % os.path.relpath(sibling, root).replace("\\", "/"))
    spec = importlib.util.spec_from_file_location("_anchor_balance", sibling)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def row_sets(root):
    """-> (anchor_token, names, closed).

    `scan.names` is EVERY data row of every recognized deferral table in every
    sanctioned home; `scan.rows` holds the OPEN ones only, keyed `relpath#name`.
    The closed set is therefore the COMPLEMENT, computed rather than matched --
    no glyph is enumerated anywhere in this file.
    """
    ab = _load_anchor_balance(root)
    try:
        scan = ab.scan_worktree(root)
    except SystemExit as exc:                # `scan_worktree` exits on a bad tree
        raise Collapse("the shared registry scan refused this tree: %s" % exc)
    names = set(k.split("#", 1)[1] for k in scan.names)
    opened = set(k.split("#", 1)[1] for k in scan.rows)
    closed = names - opened
    if len(names) < NAME_FLOOR:
        raise Collapse(
            "harvested only %d deferral row name(s), below the floor of %d.\n"
            "  This does NOT mean the tree is clean -- it means THE ROW HARVEST "
            "COLLAPSED, and a row this guard cannot see is a row it silently "
            "believes OPEN, which makes every stale citation of it invisible."
            % (len(names), NAME_FLOOR))
    if len(closed) < CLOSED_FLOOR:
        raise Collapse(
            "only %d of %d row(s) resolved as CLOSED, below the floor of %d.\n"
            "  This does NOT mean the tree is clean -- it means THE ROW HARVEST "
            "COLLAPSED. Fix the scan; do not lower the floor."
            % (len(closed), len(names), CLOSED_FLOOR))
    return ab.ANCHOR_TOKEN, names, closed


def governed_files(root):
    p = subprocess.run(
        ["git", "ls-files", "-z", "--cached", "--others", "--exclude-standard"],
        cwd=root, capture_output=True, text=True, encoding="utf-8", errors="replace")
    if p.returncode != 0:
        raise Collapse(
            "`git ls-files` failed in %s: %s\n"
            "  The governed set IS the git listing; without it this guard would "
            "scan an unknown subset and report a pass over it."
            % (root, (p.stderr or "").strip()[:200]))
    files = [f for f in p.stdout.split("\0")
             if f and not f.startswith(EXCLUDED_PREFIX)]
    if len(files) < FILE_FLOOR:
        raise Collapse(
            "git listed only %d governed file(s), below the floor of %d.\n"
            "  This does NOT mean the tree is clean -- it means THIS SCAN "
            "COLLAPSED. Fix the scan; do not lower the floor."
            % (len(files), FILE_FLOOR))
    return files


def census(root, files, anchor_token, names, closed):
    """-> {relpath: [(cited_ids, excerpt, sentence)]}"""
    per_file = {}
    for rel in files:
        try:
            with io.open(os.path.join(root, rel), encoding="utf-8",
                         errors="replace") as fh:
                text = fh.read()
        except OSError:
            continue                  # deleted between the listing and the read
        if "D-" not in text:          # cheap reject; a citation needs a `D-`
            continue
        hits = findings_in(text, anchor_token, names, closed)
        if hits:
            per_file[rel] = hits
    return per_file


# ═══════════════════════════ THE INVENTORY ═══════════════════════════════════
# ⚠ EVERY SENTENCE BELOW IS INSIDE THIS GUARD'S OWN GOVERNED SET, and the first
# draft of a sibling guard's inventory comment is one of the live findings this
# guard reports (*"which stays OPEN until `ceilings` is empty"*, naming a row the
# same cycle closed). So the citation here sits in a sentence of its own, with no
# persistence claim anywhere near it.
_INVENTORY_COMMENT = [
    "Per-file ceilings for STALE REFUSAL CITATIONS: a present-tense sentence that",
    "asserts something is refused, unsupported or blocked while every deferral row",
    "it cites has a CLOSED status cell. Such a sentence was true when it was typed",
    "and false when the commit landed, and it reads as evidence either way.",
    "A ceiling may only come DOWN. Re-measure the claim, correct the sentence, and",
    "lower the number in the SAME commit; the guard prints the new value. Raising",
    "an entry, or adding a file, is a FAILURE -- that is the ratchet.",
    "This file is DEBT, not a pass: green means no NEW instance landed.",
    "EVERY ENTRY HERE IS AN UNREPAIRED CANDIDATE, NOT A SANCTIONED EXCEPTION. The",
    "three false-positive classes an independent review found -- a residual scoped",
    "outside what the row closed, an attributive `open`, and a run-on across a JSON",
    "or parenthesis boundary -- were removed from the PREDICATE, so nothing is",
    "silenced by a bare line with no argument behind it. Each remaining entry is a",
    "sentence somebody should re-measure through the CLI and then correct.",
    "Species record: D-COMMENT-A-CLAIM-TRUE-WHEN-TYPED-AND-FALSE-WHEN-THE-COMMIT-LANDED.",
]


def load_inventory(root):
    path = os.path.join(root, INVENTORY_REL)
    if not os.path.isfile(path):
        raise Collapse(
            "the inventory %s does not exist. Without it every count is "
            "unconstrained and this guard asserts nothing."
            % INVENTORY_REL.replace("\\", "/"))
    with io.open(path, "r", encoding="utf-8") as fh:
        return json.load(fh)["ceilings"]


def write_inventory(root, ceilings):
    """Serialize FIRST, then write a temp file, then `os.replace`.

    ⚠⚠ THE ORDER IS THE WHOLE SAFETY PROPERTY AND IT IS NOT DEFENSIVE CODING. A
    patcher in this same cycle truncated a 377 KB file to ZERO bytes by opening it
    `"w"` -- which truncates immediately -- and only then running an encode that
    raised. Encoding to bytes before anything is opened means a failure leaves the
    original untouched; writing beside the target and renaming means a crash
    mid-write leaves the original untouched too. `os.replace` is atomic on both
    hosts.
    """
    body = {"_comment": _INVENTORY_COMMENT,
            "ceilings": dict(sorted(ceilings.items()))}
    blob = (json.dumps(body, indent=2, ensure_ascii=False) + "\n").encode("utf-8")
    path = os.path.join(root, INVENTORY_REL)
    tmp = path + ".tmp"
    with io.open(tmp, "wb") as fh:
        fh.write(blob)
    os.replace(tmp, path)


# ═════════════════════════════ THE VERBS ═════════════════════════════════════
def _report_site(cited, excerpt, prefix="      "):
    return ("%s-> cites %s\n%s   \"%s\""
            % (prefix, ", ".join(cited), prefix, " ".join(excerpt.split())))


def run(root, write=False, baseline=False):
    anchor_token, names, closed = row_sets(root)
    files = governed_files(root)
    now = census(root, files, anchor_token, names, closed)
    counts = dict((rel, len(hits)) for rel, hits in now.items())

    if baseline:
        write_inventory(root, counts)
        print("%s: BASELINED %d file(s), %d site(s) over %d governed file(s), "
              "%d known row(s), %d of them CLOSED."
              % (GUARD, len(counts), sum(counts.values()), len(files), len(names),
                 len(closed)))
        print("  ⚠ This establishes NEW ground and CAN RAISE ceilings. Review the "
              "diff: every raised number is a stale refusal citation that is now "
              "permitted. Use `--write` for burn-down; it can only lower.")
        return EXIT_OK

    ceilings = load_inventory(root)

    if write:
        above = sorted((rel, ceilings.get(rel, 0), n)
                       for rel, n in counts.items() if n > ceilings.get(rel, 0))
        if above:
            print("%s: REFUSING to re-baseline -- %d file(s) sit ABOVE their "
                  "ceiling:" % (GUARD, len(above)))
            for rel, ceiling, n in above:
                print("    %s: ceiling %d, actual %d" % (rel, ceiling, n))
            print("")
            print("  `--write` is the BURN-DOWN verb: it may only LOWER, and it has")
            print("  written nothing. Correct the sentence instead -- re-measure the")
            print("  claim through the CLI and say what is true now. A rename that")
            print("  carries sites needs a hand edit of the JSON, which is visible in")
            print("  review. `--baseline` is the only verb that establishes new")
            print("  ground, and it announces every raise it makes.")
            return EXIT_RATCHET
        write_inventory(root, counts)
        print("%s: re-baselined %d file(s), %d site(s)"
              % (GUARD, len(counts), sum(counts.values())))
        return EXIT_OK

    new, stale = [], []
    for rel in sorted(counts):
        ceiling = ceilings.get(rel, 0)
        if counts[rel] > ceiling:
            new.append((rel, ceiling, counts[rel]))
    for rel in sorted(ceilings):
        if counts.get(rel, 0) < ceilings[rel]:
            stale.append((rel, ceilings[rel], counts.get(rel, 0)))

    if new:
        print("%s: FAIL - a NEW stale refusal citation landed:" % GUARD)
        for rel, ceiling, n in new:
            print("    %s: %d site(s), inventory allows %d" % (rel, n, ceiling))
            for cited, excerpt, _ in now[rel][ceiling:]:
                print(_report_site(cited, excerpt))
        print("")
        print("  Each sentence above asserts that something is refused, unsupported")
        print("  or blocked, and every deferral row it cites is CLOSED. Either the")
        print("  sentence went stale -- most often because a sibling lane closed the")
        print("  row IN THIS COMMIT -- or it cites the wrong row.")
        print("  FIX: re-measure the claim through the CLI, then say what is true")
        print("  now. If the sentence is worth keeping as history, say so in it")
        print("  (\"was refused until …\", \"THIS PARAGRAPH SAID …, and it was false\")")
        print("  -- a past-tense governor in front of the claim clears this guard,")
        print("  because it is then a statement about a sentence, not about the")
        print("  compiler.")
        print("  Do NOT raise the ceiling to make this pass - the ceiling only ever")
        print("  comes DOWN. That is the whole point.")
        return EXIT_RATCHET

    if stale:
        print("%s: FAIL - the inventory is STALE and now grants unused headroom:"
              % GUARD)
        for rel, ceiling, n in stale:
            print("    %s: %d site(s) now, ceiling still says %d -> lower it to %d%s"
                  % (rel, n, ceiling, n,
                     " (or delete the entry)" if n == 0 else ""))
        print("")
        print("  You corrected sentences without lowering the ceiling. Unclaimed")
        print("  headroom is exactly where the next one hides. Re-baseline in the")
        print("  same commit:")
        print("      python scripts/check-stale-refusal-citations/"
              "check-stale-refusal-citations.py --write")
        print("  That verb only lowers, so it cannot hide a regression while it does.")
        return EXIT_RATCHET

    total = sum(counts.values())
    if total:
        print("%s: OK (%d governed file(s) scanned against %d known row(s), %d of "
              "them CLOSED; %d site(s) across %d file(s), all within the inventory "
              "ratchet). DEBT, not a pass - see "
              "D-COMMENT-A-CLAIM-TRUE-WHEN-TYPED-AND-FALSE-WHEN-THE-COMMIT-LANDED."
              % (GUARD, len(files), len(names), len(closed), total, len(counts)))
    else:
        print("%s: OK (%d governed file(s) scanned against %d known row(s), %d of "
              "them CLOSED; 0 site(s))"
              % (GUARD, len(files), len(names), len(closed)))
    return EXIT_OK


def list_sites(root):
    """Print every site the census finds, ceilings ignored. A reading verb."""
    anchor_token, names, closed = row_sets(root)
    files = governed_files(root)
    now = census(root, files, anchor_token, names, closed)
    for rel in sorted(now):
        print("%s  (%d)" % (rel, len(now[rel])))
        for cited, excerpt, _ in now[rel]:
            print(_report_site(cited, excerpt, prefix="    "))
    print("%s: %d site(s) across %d file(s) of %d governed, %d known row(s), %d "
          "CLOSED." % (GUARD, sum(len(v) for v in now.values()), len(now),
                       len(files), len(names), len(closed)))
    return EXIT_OK


# ═══════════════════════════════ SELF-TEST ═══════════════════════════════════
#
# ★★★ THE ARMS ARE DRIVEN THROUGH THE **REAL** SCAN, not through the matcher
# alone, and their fixtures are SYNTHESIZED AT RUN TIME into a temp directory that
# is created and deleted here.
# ⛔ NO ON-DISK FIXTURE CARRYING A REAL STALE CITATION EXISTS ANYWHERE IN THE
# GOVERNED SET, and that is a decision rather than an omission: such a fixture IS
# an instance of the defect, it would sit in this guard's own inventory forever,
# and the first person to "repair" it would silently break the self-test.
# ★ EVERY RED ARM ASSERTS THE **MESSAGE**, never merely a non-zero exit: this
# guard has two distinct failures sharing exit 1 (a new site / a stale ceiling)
# and three sharing exit 2, and an arm that checks only the code cannot tell which
# one it proved. That exact mistake was measured in a sibling guard.
# ⚠⚠ EVERY SYNTHETIC ROW NAME SPELLED IN THIS FILE CARRIES TWO SEGMENTS AFTER THE
# `D-` (`D-XX-BAZ`), WHICH IS UNDER `anchor_registry_guard`'s THREE-SEGMENT
# COLLECTION THRESHOLD while still being matched by `check-anchor-balance`'s
# two-segment `ANCHOR_TOKEN` -- fixtures live in exactly that gap. A longer
# literal here would be a citation of an anchor with no row, INSIDE the guard that
# reports stale citations, and the registry guard refuses the whole tree for it.
# ✔MEASURED on this file's first draft: a three-segment fixture reddened
# `check-anchor-registry` at once. Settled precedent, not preference --
# D-GATE-ANCHOR-BALANCE-SELFTEST-FIXTURES-ARE-ANCHOR-SHAPED renamed eleven such
# names rather than allowlisting them, because an allowlist entry silences a name
# repo-wide and forever.
# ⚠ THE TRAP THAT ROW RECORDS FROM ITS OWN FIX IS PROSE: it came back twice, once
# in a FAIL help string and once in a docstring that spelled a fixture out in
# order to EXPLAIN the rename. So the comments and the help text obey the same
# rule as the literals, and the one fixture that NEEDS a realistic length is
# ASSEMBLED from fragments no grep can join.

EXPECTED_ARMS = 59

# The one synthetic plan document every arm's temp repo carries. The closure mark
# is taken from the shared module at run time rather than written here -- this
# file enumerates no status glyph anywhere, which is the property the module
# docstring claims.
#   `_CLOSED_ROW`  the subject: closed by default, re-opened by two arms.
#   `_OPEN_ROW`    an open row, so the attribution rule (2) has both populations.
#   `_FILLER_ROW`  ★ PERMANENTLY CLOSED AND CITED BY NOTHING. It exists because
#                  `CLOSED_FLOOR` is a real refusal even at 1: the two arms that
#                  make the subject OPEN would otherwise leave the synthetic tree
#                  with ZERO closed rows and collapse, and a collapse would have
#                  been reported as "the sentence is not a finding". ✔That is not
#                  hypothetical -- both arms did exactly that on first run.
_PLAN_HEADER = ("| Anchor | Trigger | Closing work | Cross-refs |\n"
                "| --- | --- | --- | --- |\n")
# ★ THE SUBJECT'S NAME DELIBERATELY CARRIES A REFUSAL WORD, because that is the
# false-positive class the mask exists for and a fixture that cannot reproduce it
# cannot pin it. Two segments, so the registry guard never sees it.
_CLOSED_ROW = "D-XX-REFUSED"
_OPEN_ROW = "D-XX-BAR"
_FILLER_ROW = "D-XX-BAZ"
# ★ ASSEMBLED, NEVER WRITTEN, and for the reason `check-wrapped-anchor-ids`
# records: a four-segment `D-…` literal in this file is a citation of an anchor
# with no row, and `anchor_registry_guard` would refuse it. The arm that needs a
# REALISTIC id length -- the mask-separation property only holds because real row
# names are 20-60 characters wide -- builds one at run time instead.
_LONG_CLOSED_ROW = "D-XX" + "-QQQQQQQQQ" + "-RRRRRRRRR" + "-SSSSSSSSS"


def _plan_text(closed_mark, subject_closed=True, subject_mark=None):
    mark = closed_mark if subject_mark is None else subject_mark
    subject = ("| `%s` | %s **CLOSED 2026-01-01** | none | r |\n"
               % (_CLOSED_ROW, mark)) if subject_closed else (
        "| `%s` | OPEN | w | r |\n" % _CLOSED_ROW)
    return (_PLAN_HEADER
            + subject
            + "| `%s` | OPEN | w | r |\n" % _OPEN_ROW
            + "| `%s` | %s **CLOSED 2026-01-01** | none | r |\n"
            % (_LONG_CLOSED_ROW, closed_mark)
            + "| `%s` | %s **CLOSED 2026-01-01** | none | r |\n"
            % (_FILLER_ROW, closed_mark))


def _tmp_repo(root, files, ceilings, closed_mark, subject_mark=None):
    """A throwaway repo with `files`, a synthetic plan, and an inventory."""
    box = tempfile.mkdtemp(prefix="stale-refusal-selftest-")
    subprocess.run(["git", "init", "-q"], cwd=box, capture_output=True)
    payload = dict(files)
    payload[".plans/00-synthetic.md"] = _plan_text(closed_mark,
                                                   subject_mark=subject_mark)
    sibling = os.path.join("scripts", "check-anchor-balance",
                           "check-anchor-balance.py")
    with io.open(os.path.join(root, sibling), encoding="utf-8") as fh:
        payload[sibling.replace(os.sep, "/")] = fh.read()
    for rel, text in payload.items():
        full = os.path.join(box, rel.replace("/", os.sep))
        os.makedirs(os.path.dirname(full), exist_ok=True)
        with io.open(full, "w", encoding="utf-8", newline="\n") as fh:
            fh.write(text)
    if ceilings is not None:
        inv = os.path.join(box, INVENTORY_REL)
        os.makedirs(os.path.dirname(inv), exist_ok=True)
        with io.open(inv, "wb") as fh:
            fh.write((json.dumps({"_comment": _INVENTORY_COMMENT,
                                  "ceilings": dict(sorted(ceilings.items()))},
                                 indent=2, ensure_ascii=False) + "\n")
                     .encode("utf-8"))
    return box


def _capture(box, write=False, baseline=False):
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf), contextlib.redirect_stderr(buf):
        try:
            rc = run(box, write, baseline)
        except Collapse as exc:
            print("%s: FATAL - %s" % (GUARD, exc))
            rc = EXIT_COLLAPSE
    return rc, buf.getvalue()


def selftest(root):
    bad = 0
    arms = []

    def check(label, cond, detail=""):
        nonlocal bad
        arms.append(label)
        if not cond:
            bad += 1
        print("  [%s] %s%s" % ("ok " if cond else "FAIL", label,
                               (" (" + detail + ")") if detail else ""))

    ab = _load_anchor_balance(root)
    tok = ab.ANCHOR_TOKEN
    NAMES = {_CLOSED_ROW, _OPEN_ROW, _LONG_CLOSED_ROW, _FILLER_ROW}
    CLOSED = {_CLOSED_ROW, _LONG_CLOSED_ROW, _FILLER_ROW}

    def hits(text):
        return findings_in(text, tok, NAMES, CLOSED)

    # ── A. the predicate, in isolation ──────────────────────────────────────
    check("a persistence+refusal sentence citing a CLOSED row is a finding",
          [h[0] for h in hits("it therefore stays refused fail-loud (%s).\n"
                              % _CLOSED_ROW)] == [[_CLOSED_ROW]])
    check("the SAME sentence citing an OPEN row is not",
          hits("it therefore stays refused fail-loud (%s).\n" % _OPEN_ROW) == [])
    check("a sentence citing BOTH a closed and an open row is attributed to the "
          "open one",
          hits("%s landed but %s still refuses it.\n" % (_CLOSED_ROW, _OPEN_ROW))
          == [])
    check("an id that names NO row is not a citation",
          hits("it still stays refused (D-ZZ-NOSUCH).\n") == [])
    check("a citation with no refusal claim is not a finding",
          hits("the memory-direction axis lives here (%s).\n" % _CLOSED_ROW) == [])
    check("a refusal claim with no citation is not a finding",
          hits("the 16-bit form still stays refused fail-loud.\n") == [])

    # ★ THE COPULA ARM. This is the narrowing that took the tree-wide population
    # from 153 to 26, so it is pinned from the REFUSING side: if somebody widens
    # PERSISTENCE to admit `is`, this arm reds and they have to argue for it.
    check("a DEFINITIONAL `is a refusal` sentence is NOT a finding (the copula "
          "is deliberately not a persistence word)",
          hits("an unclosed frame description is a refusal, not a truncation "
               "(%s).\n" % _CLOSED_ROW) == [])
    check("... and neither is `is REJECTED fail-loud`, which a closure DELIVERS",
          hits("`nullptr` in a variadic position is rejected fail-loud (%s).\n"
               % _CLOSED_ROW) == [])

    # ★★ THE MASK ARM, AND IT ASSERTS THE MASK IS **LOAD-BEARING** RATHER THAN
    # MERELY THAT THE RESULT IS EMPTY: the same sentence is checked against the
    # raw `CLAIM` pattern, which MUST match, and against the real predicate, which
    # must not. An arm that only asserted the empty result would still pass if the
    # sentence were inert for some other reason. Without the mask this is the
    # single largest false-positive class -- 153 candidates against 26.
    check("a row name that CONTAINS a refusal word does not convict its own "
          "citation, and the mask is what stops it",
          hits("this is still the axis (%s).\n" % _CLOSED_ROW) == []
          and CLAIM.search("this is still the axis (%s)." % _CLOSED_ROW) is not None)
    # ★ THE ID LENGTH IS THE POINT OF THIS ARM, NOT AN ACCIDENT OF THE FIXTURE.
    # A masked row name is 20-60 characters wide -- wider than `GAP` -- so it
    # SEPARATES the two halves of the phrase. ⚠ With a short synthetic id the
    # same sentence IS a finding, which is correct and is why the fixture uses a
    # realistic-length name: this arm pins the separation, not a fiction.
    check("... and a REAL-LENGTH masked id separates a persistence word from a "
          "refusal word on the other side of it",
          hits("this is still true of %s, refusal aside.\n" % _LONG_CLOSED_ROW)
          == [])

    check("a past-tense GOVERNOR immediately before the claim clears it",
          hits("this paragraph said it is still refused, and it was written while "
               "%s was open.\n" % _CLOSED_ROW) == [])
    check("a RETRACTION anywhere in the sentence clears it",
          hits("it stays refused (%s) -- no longer true.\n" % _CLOSED_ROW) == [])
    check("a sentence that says the row it cites IS CLOSED is not presenting it "
          "as a live blocker",
          hits("the older slot still refuses that one, and %s is CLOSED.\n"
               % _CLOSED_ROW) == [])
    check("a governor AFTER the claim does NOT clear it (the window looks back)",
          [h[0] for h in hits("it stays refused (%s) for the reason the audit "
                              "read.\n" % _CLOSED_ROW)] == [[_CLOSED_ROW]])
    check("`stays OPEN` is the same claim as `stays refused`",
          [h[0] for h in hits("that row stays open until the ceilings are empty "
                              "(%s).\n" % _CLOSED_ROW)] == [[_CLOSED_ROW]])
    # ★ THE ATTRIBUTIVE `open`. `hir_to_mir.cpp` calls the block it is appending
    # to "the open block"; nothing is deferred and the row it cites is closed.
    check("an ATTRIBUTIVE `open` (`still the open block`) is compiler jargon, "
          "not a live-blocker claim",
          hits("emitted into the entry block, which is still the open block here "
               "(%s).\n" % _CLOSED_ROW) == [])

    # ── A2. the RESIDUAL-BOUNDARY exemption ─────────────────────────────────
    # ★★ THE LARGEST FALSE-POSITIVE CLASS, and it is a TRUE sentence citing a
    # CLOSED row: the row closed a SUBSET of what the sentence refuses.
    check("a QUANTIFIED threshold before the claim marks a residual, not a stale "
          "claim",
          hits("the form reaches 16 MiB, and a frame > 16 MiB stays fail-loud "
               "(%s).\n" % _CLOSED_ROW) == [])
    check("... and so does an EXCEPTION quantifier",
          hits("any other lvalue kind is still unsupported (%s).\n" % _CLOSED_ROW)
          == [])
    check("... but the SAME claim with no qualifier is still a finding",
          [h[0] for h in hits("a frame stays fail-loud (%s).\n" % _CLOSED_ROW)]
          == [[_CLOSED_ROW]])
    # ⚠ THE WINDOW IS WHAT KEEPS THE EXEMPTION FROM SWALLOWING A REAL FINDING:
    # `test_parser_speculation.cpp` uses a qualifier about something else 100
    # characters upstream of a genuine `still-open` claim.
    check("a qualifier far UPSTREAM does not reach the claim",
          [h[0] for h in hits(
              "the deep-nest residual is an N-deep host-recursion memory "
              "hierarchy constant factor exceeding nothing anybody measured in "
              "the depth-capped walk, and it is the separate, still-open %s.\n"
              % _CLOSED_ROW)] == [[_CLOSED_ROW]])
    check("a C++ arrow is not a threshold",
          [h[0] for h in hits("p->kind still stays refused (%s).\n" % _CLOSED_ROW)]
          == [[_CLOSED_ROW]])

    # ── A3. the two structural sentence boundaries ──────────────────────────
    check("a terminator followed by a CLOSER still ends the sentence",
          hits("(the address arm currently fails loud too.) unsupported(node, "
               "\"see %s\");\n" % _CLOSED_ROW) == [])
    check("a JSON value followed by the next KEY ends the sentence",
          hits('{"$comment": "addrAlign must stay 0, validate-rejected on PE '
               'rows.", "kind": "rodata", "x": 0 }, { "$comment": "%s (PE arm)." '
               '}\n' % _CLOSED_ROW) == [])
    check("... but a boundary between two PROSE list elements does NOT",
          [h[0] for h in hits('["burn-down tracked by %s,", "which stays open '
                              'until the ceilings are empty."]\n' % _CLOSED_ROW)]
          == [[_CLOSED_ROW]])
    check("the claim and the citation must share ONE sentence",
          hits("it therefore stays refused fail-loud. The axis is %s.\n"
               % _CLOSED_ROW) == [])
    check("an escaped `\\n` inside a JSON $comment ends the sentence too",
          hits('{"$comment": "it therefore stays refused fail-loud.'
               '\\nThe axis is %s."}\n' % _CLOSED_ROW) == [])
    check("... and a JSON $comment paragraph that DOES carry both is found",
          [h[0] for h in hits(
              '{"$comment": "there is deliberately no 16-bit sibling and it '
              'stays fail-loud (%s).\\nSomething else."}\n' % _CLOSED_ROW)]
          == [[_CLOSED_ROW]])
    check("a colon does NOT end a sentence (a claim and its reason are one unit)",
          [h[0] for h in hits("it stays refused: the engine cannot elect it "
                              "(%s).\n" % _CLOSED_ROW)] == [[_CLOSED_ROW]])
    check("the reported excerpt quotes the author's own words",
          "stays refused fail-loud" in
          hits("it therefore stays refused fail-loud (%s).\n" % _CLOSED_ROW)[0][1])

    # ── B. the four row-mandated cases, through the REAL scan ───────────────
    # The floors are the only thing standing in the way of a synthetic tree, so
    # they are lowered for the duration by patching the module globals -- the
    # alternative is 1200 synthetic files and 1200 synthetic rows per arm.
    global FILE_FLOOR, NAME_FLOOR, CLOSED_FLOOR
    saved = (FILE_FLOOR, NAME_FLOOR, CLOSED_FLOOR)
    FILE_FLOOR = NAME_FLOOR = CLOSED_FLOOR = 1
    boxes = []

    def synth(body, ceilings={}, mark=None):
        box = _tmp_repo(root, {"src/subject.cpp": body}, ceilings,
                        ab.CLOSED_MARK if mark is None else mark)
        boxes.append(box)
        return box

    try:
        rc, out = _capture(synth("// it therefore stays refused fail-loud (%s).\n"
                                 % _CLOSED_ROW))
        check("(1) a SYNTHETIC stale citation in the governed set goes RED",
              rc == EXIT_RATCHET, "rc=%d" % rc)
        check("(1) ... and the finding NAMES the file and the row it cites",
              "src/subject.cpp" in out and _CLOSED_ROW in out)
        check("(1) ... and QUOTES the sentence",
              "stays refused fail-loud" in out)
        check("(1) ... and cites NO line number",
              not re.search(r"src/subject\.cpp:\d", out))
        check("(1) ... and says what the author should do about it",
              "re-measure the claim" in out.lower())

        rc, out = _capture(synth("// it therefore stays refused fail-loud (%s).\n"
                                 % _OPEN_ROW))
        check("(2) the SAME sentence citing an OPEN row is GREEN", rc == EXIT_OK)
        check("(2) ... and the green line reports a real scan",
              "%s: OK" % GUARD in out and "0 site(s)" in out)

        rc, out = _capture(synth("// the axis lives here (%s).\n" % _CLOSED_ROW))
        check("(3) a citation of a CLOSED row with no refusal claim is GREEN",
              rc == EXIT_OK)

        rc, out = _capture(synth("// it still stays refused fail-loud.\n"))
        check("(4) a refusal claim citing NOTHING is GREEN", rc == EXIT_OK)

        # ★★★ THE TRANSITION ARM -- the whole reason this guard can catch a
        # minutes-wide defect without a base ref. The FILE is byte-identical
        # across these two runs; only the registry moved.
        body = "// it therefore stays refused fail-loud (%s).\n" % _CLOSED_ROW
        box_open = _tmp_repo(root, {"src/subject.cpp": body}, {},
                             ab.CLOSED_MARK)
        boxes.append(box_open)
        plan = os.path.join(box_open, ".plans", "00-synthetic.md")
        with io.open(plan, "w", encoding="utf-8", newline="\n") as fh:
            fh.write(_plan_text(ab.CLOSED_MARK, subject_closed=False))
        rc_before, out_before = _capture(box_open)
        with io.open(plan, "w", encoding="utf-8", newline="\n") as fh:
            fh.write(_plan_text(ab.CLOSED_MARK))
        rc_after, out_after = _capture(box_open)
        check("A ROW CLOSING turns an UNTOUCHED file's sentence into a red",
              rc_before == EXIT_OK and rc_after == EXIT_RATCHET
              and "a NEW stale refusal citation" in out_after,
              "before=%d after=%d" % (rc_before, rc_after))
        check("... and the BEFORE half was a real green, not a collapse",
              "%s: OK" % GUARD in out_before and "0 site(s)" in out_before)

        # ── C. the ratchet, both directions ─────────────────────────────────
        two = ("// it therefore stays refused fail-loud (%s).\n"
               "// and it also still remains unsupported (%s).\n"
               % (_CLOSED_ROW, _CLOSED_ROW))
        rc, out = _capture(synth(two, {"src/subject.cpp": 2}))
        check("a site AT its ceiling is GREEN and reported as DEBT",
              rc == EXIT_OK and "DEBT, not a pass" in out)
        rc, out = _capture(synth(two, {"src/subject.cpp": 1}))
        check("one site OVER the ceiling is RED, named as a NEW instance",
              rc == EXIT_RATCHET and "a NEW stale refusal citation" in out)
        rc, out = _capture(synth(two, {"src/subject.cpp": 3}))
        check("a ceiling ABOVE the live count is RED (unclaimed headroom)",
              rc == EXIT_RATCHET and "STALE and now grants unused headroom" in out)
        rc, out = _capture(synth("// nothing here\n", {"src/subject.cpp": 1}))
        check("a ceiling for a file with NO sites left is RED, and says to delete "
              "the entry", rc == EXIT_RATCHET and "or delete the entry" in out)

        # ── D. `--write` may only LOWER; `--baseline` may raise, loudly ─────
        box = synth(two, {"src/subject.cpp": 1})
        before = io.open(os.path.join(box, INVENTORY_REL), "rb").read()
        rc, out = _capture(box, write=True)
        check("`--write` REFUSES to raise a ceiling",
              rc == EXIT_RATCHET and "REFUSING to re-baseline" in out
              and "may only LOWER" in out)
        check("... and wrote NOTHING while refusing",
              io.open(os.path.join(box, INVENTORY_REL), "rb").read() == before)
        box = synth(two, {"src/subject.cpp": 5})
        rc, out = _capture(box, write=True)
        check("`--write` lowers a stale ceiling",
              rc == EXIT_OK and "re-baselined" in out)
        check("... to exactly what the tree holds",
              json.load(io.open(os.path.join(box, INVENTORY_REL),
                                encoding="utf-8"))["ceilings"]
              == {"src/subject.cpp": 2})
        box = synth(two, {"src/subject.cpp": 1})
        rc, out = _capture(box, baseline=True)
        check("`--baseline` DOES raise, and says so loudly",
              rc == EXIT_OK and "BASELINED" in out and "CAN RAISE ceilings" in out)
        check("... writing exactly the live census",
              json.load(io.open(os.path.join(box, INVENTORY_REL),
                                encoding="utf-8"))["ceilings"]
              == {"src/subject.cpp": 2})

        # ★ The atomic write leaves no debris and no partial file.
        check("`--write` leaves no `.tmp` beside the inventory",
              not os.path.exists(os.path.join(box, INVENTORY_REL + ".tmp")))

        # ── E. fail-closed ──────────────────────────────────────────────────
        rc, out = _capture(synth(two, None))
        check("a MISSING inventory is a COLLAPSE, never a pass",
              rc == EXIT_COLLAPSE and "does not exist" in out)

        FILE_FLOOR = 10 ** 6
        rc, out = _capture(synth(two, {"src/subject.cpp": 2}))
        check("a governed-file count below the FLOOR is a COLLAPSE",
              rc == EXIT_COLLAPSE and "governed file(s), below the floor" in out)
        FILE_FLOOR = 1
        NAME_FLOOR = 10 ** 6
        rc, out = _capture(synth(two, {"src/subject.cpp": 2}))
        check("a row harvest below the FLOOR is a COLLAPSE, not a clean tree",
              rc == EXIT_COLLAPSE and "ROW HARVEST" in out
              and "row name(s)" in out)
        NAME_FLOOR = 1
        CLOSED_FLOOR = 10 ** 6
        rc, out = _capture(synth(two, {"src/subject.cpp": 2}))
        check("a CLOSED harvest below the FLOOR is a COLLAPSE",
              rc == EXIT_COLLAPSE and "resolved as CLOSED" in out)
        CLOSED_FLOOR = 1

        # ★ THE CLOSED-STATUS RULE IS THE SHARED ONE, PROVEN BY SUBSTITUTION: a
        # row whose status cell leads with a DIFFERENT glyph is OPEN, so the same
        # sentence stops being a finding. This is what pins "define the
        # complement, never enumerate the glyphs" -- nothing here knows what the
        # closure mark IS.
        box = _tmp_repo(root, {"src/subject.cpp":
                               "// it therefore stays refused fail-loud (%s).\n"
                               % _CLOSED_ROW}, {}, ab.CLOSED_MARK,
                        subject_mark=ab.DISCLOSED_MARK)
        boxes.append(box)
        rc, out = _capture(box)
        check("a status cell leading with a NON-closure marker is OPEN, so the "
              "identical sentence is GREEN",
              rc == EXIT_OK and "0 site(s)" in out, "rc=%d" % rc)

        # A repo with no `check-anchor-balance` at all.
        box = tempfile.mkdtemp(prefix="stale-refusal-noab-")
        boxes.append(box)
        os.makedirs(os.path.join(box, ".plans"))
        os.makedirs(os.path.join(box, "src"))
        subprocess.run(["git", "init", "-q"], cwd=box, capture_output=True)
        with io.open(os.path.join(box, ".plans", "00-synthetic.md"), "w",
                     encoding="utf-8", newline="\n") as fh:
            fh.write(_plan_text(ab.CLOSED_MARK))
        with io.open(os.path.join(box, "src", "subject.cpp"), "w",
                     encoding="utf-8", newline="\n") as fh:
            fh.write(two)
        rc, out = _capture(box)
        check("a MISSING check-anchor-balance is a COLLAPSE (the import fails "
              "loud)", rc == EXIT_COLLAPSE and "shared anchor vocabulary" in out)

        # ── F. the plan tree really is out of the governed set ──────────────
        box = _tmp_repo(root, {"src/subject.cpp": "// nothing\n"}, {},
                        ab.CLOSED_MARK)
        boxes.append(box)
        with io.open(os.path.join(box, ".plans", "01-prose.md"), "w",
                     encoding="utf-8", newline="\n") as fh:
            fh.write("It therefore stays refused fail-loud (`%s`).\n" % _CLOSED_ROW)
        rc, out = _capture(box)
        check("the SAME sentence under `.plans/` is not governed here",
              rc == EXIT_OK, "rc=%d" % rc)
    finally:
        FILE_FLOOR, NAME_FLOOR, CLOSED_FLOOR = saved
        for box in boxes:
            shutil.rmtree(box, ignore_errors=True)
        leaked = [b for b in boxes if os.path.exists(b)]
        check("every synthetic root was removed and the floors restored",
              not leaked and (FILE_FLOOR, NAME_FLOOR, CLOSED_FLOOR) == saved)

    if len(arms) != EXPECTED_ARMS:
        print("  [FAIL] expected %d arms, ran %d - an arm was dropped or added "
              "without updating EXPECTED_ARMS" % (EXPECTED_ARMS, len(arms)))
        bad += 1
    print("%s selftest: %s (%d arm(s), %d failure(s))%s"
          % (GUARD, "FAIL" if bad else "OK", len(arms), bad,
             "" if bad else " - every red arm asserted the MESSAGE of the refusal "
             "it names; this guard is PROVEN able to fail."))
    return EXIT_RATCHET if bad else EXIT_OK


def repo_root():
    p = subprocess.run(["git", "rev-parse", "--show-toplevel"],
                       capture_output=True, text=True, encoding="utf-8",
                       errors="replace")
    if p.returncode != 0:
        raise Collapse("not inside a git repository")
    return p.stdout.strip()


def main(argv):
    known = ("--write", "--baseline", "--selftest", "--list")
    unknown = [a for a in argv if a not in known]
    if unknown:
        print("%s: unknown argument(s): %s" % (GUARD, " ".join(unknown)))
        return EXIT_USAGE
    write, baseline = "--write" in argv, "--baseline" in argv
    # ⚠ The two write verbs are OPPOSITE promises -- one may only lower, the other
    # may raise -- so a run naming both has no defined meaning. Refuse rather than
    # pick; picking would make the safer spelling a coin toss.
    if write and baseline:
        print("%s: `--write` and `--baseline` are different verbs and cannot be "
              "combined." % GUARD)
        print("  `--write` burns down (it may only LOWER); `--baseline` "
              "establishes new ground (it may RAISE, and says so). Choose one.")
        return EXIT_USAGE
    try:
        root = repo_root()
        if "--selftest" in argv:
            return selftest(root)
        if "--list" in argv:
            return list_sites(root)
        if write or baseline:
            return run(root, write=write, baseline=baseline)
        rc = run(root)
        print("")
        rc_self = selftest(root)
        # ★ BOTH HALVES RUN UNCONDITIONALLY. `return rc or selftest(root)` would
        # SHORT-CIRCUIT the self-test whenever the tree check reds, so a broken
        # instrument would stay hidden behind exactly the failure it was supposed
        # to be trusted to report.
        return rc or rc_self
    except Collapse as exc:
        print("%s: FATAL - %s" % (GUARD, exc))
        print("  This does NOT mean the tree is clean - it means the SCAN "
              "COLLAPSED.")
        return EXIT_COLLAPSE


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
