#!/usr/bin/env python3
# PURPOSE: re-derive the prioritized burndown queue from the registry, production errors first.
"""burndown-queue.py -- THE PRIORITIZED QUEUE, RE-DERIVED RATHER THAN REMEMBERED.

OPERATOR INSTRUCTION 2026-08-24, verbatim: *"grab anchors from this list + the
handoff, make a prioritized /loop considering production errors the most prioritized
items and address all of them."*

★★★ WHY THIS IS AN INSTRUMENT AND NOT A LIST IN THE HANDOFF.

This project's own memory carries the rule in capitals: NEVER DERIVE A STATUS OR A
PRIORITY FROM A WRITTEN LIST -- RE-DERIVE IT FROM THE REGISTRY, EVERY TIME. That rule
was paid for six times, and twice the error reached the operator: a queue named an
already-closed row; a backlog answer recommended a row closed months earlier; four
claims in one handoff header went stale at once; and on one day alone three separate
status claims in the queue were wrong in three different directions.

A hand-written queue is stale the moment the next cycle closes a row. This reads the
rows and sorts them, so the queue is a VIEW, never a copy.

★ IT REUSES `check-anchor-balance` RATHER THAN RE-IMPLEMENTING IT. That script owns
this repository's hard-won vocabulary for "is this row open", "is it gated", "has its
trigger fired", "is its opener discharged" -- a regex family whose comments record six
separate defects, including two where a NEGATED or ATTRIBUTIVE mention flipped a
verdict. Re-typing any of it here would re-open all of them. The standing order is
explicit: use the script that exists, and fix it rather than routing around it.

★★★ THE BAND IS A SORT KEY, NOT A VERDICT -- and this warning is load-bearing.
✔MEASURED 2026-08-24 (cycle P31): a census of "103 misglyphed registry rows" was a
KEYWORD SIEVE mistaken for a verdict; the real number was **4**. So every banded row
prints the PHRASE that banded it and the words around it, and the residue -- rows no
pattern matched -- is printed LOUDLY rather than dropped. A queue that silently
swallows what it could not classify is the same defect wearing a hat.

★ AND AN ATTRIBUTIVE MENTION IS NOT A CLAIM ABOUT THIS ROW. A row that says
"[[D-OTHER]] is a silent miscompile" is not itself one. The discriminator is the house
one, taken from `declares_no_trigger`: if an anchor id sits between the nearest clause
boundary and the phrase, the phrase is about somebody else.

THE BANDS, highest first. "Production error" is the operator's word and it means the
shipped compiler does something WRONG, not that it is missing something:

  P0 WRONG-OUTPUT  DSS produces an incorrect result, drops something silently, or
                   crashes on legal input. These ship bad binaries. Nothing outranks them.
  P1 REFUSED       DSS refuses, cannot parse, cannot link, or cannot build something a
                   reference accepts -- real code that does not compile today.
  P2 DIVERGENT     a conformance divergence or an absent capability in a PRODUCT
                   namespace: DSS answers differently, or not at all.
  P3 HARNESS       the test/gate/build/cycle instruments. Real defects, but they cost
                   confidence rather than correctness.
  P4 RECORD        the plans, the registry, the documentation.
  P5 ENV           environment and upstream: explicitly not ours to fix in the compiler.

USAGE
    python burndown_queue.py                    # the whole queue, banded
    python burndown_queue.py --band P0 P1       # only those bands
    python burndown_queue.py --schedulable      # drop rows whose trigger has not fired
    python burndown_queue.py --top 40           # first N after sorting
    python burndown_queue.py --counts           # band/severity census only
    python burndown_queue.py --json

★ NO `.ps1` TWIN, DELIBERATELY: a `.py` runs unchanged on every host this project gates
on, so a PowerShell sibling would be a second implementation of something never split.
"""
import argparse
import importlib.util
import io
import json
import os
import re
import sys

for _s in (sys.stdout, sys.stderr):
    try:
        _s.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass

HERE = os.path.dirname(os.path.abspath(__file__))


def repo_root():
    """Walk up until `.plans/` and `scripts/` are both present.

    ⚠ Not a fixed number of `dirname` calls: this file is expected to move from
    `scratchpad/p33/orch/` to `scripts/burndown-queue/`, and a hard-coded depth is
    exactly the defect the P17 consolidation had to repair in 17 scripts at once.
    """
    d = HERE
    while True:
        if os.path.isdir(os.path.join(d, ".plans")) and \
           os.path.isdir(os.path.join(d, "scripts")):
            return d
        parent = os.path.dirname(d)
        if parent == d:
            sys.exit("burndown-queue: no repository root above %s "
                     "(looked for a directory holding BOTH .plans/ and scripts/)" % HERE)
        d = parent


ROOT = repo_root()
BALANCE_PY = os.path.join(ROOT, "scripts", "check-anchor-balance",
                          "check-anchor-balance.py")

# A hyphenated filename is not an importable module name, so load it by path. This is
# the reuse the standing order demands -- see the module docstring.
if not os.path.isfile(BALANCE_PY):
    sys.exit("burndown-queue: cannot find %s -- this instrument REUSES its row "
             "vocabulary and must not re-implement it." % BALANCE_PY)
_spec = importlib.util.spec_from_file_location("check_anchor_balance", BALANCE_PY)
bal = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(bal)

# A registry this size cannot legitimately collapse to a handful. A queue over an empty
# set prints "nothing to do", which is the most flattering possible lie.
FLOOR_OPEN = 300

# ⚠ ANY markdown table row, not just an anchor-shaped one. The first draft of this
# reader required `D-<SEG>-<SEG>` in cell 1 and left **99 open rows unreadable** --
# plan-00 §0.2 and plan-08 number their deferred items `D1`, `C2-D1`, which no anchor
# regex will ever match. They surfaced only because the residue is printed rather than
# dropped, which is the whole argument for printing it.
ROWSTART = re.compile(r"^\|(?!\s*[-:]+\s*\|)")
SEVERITY = ["\U0001F534", "\U0001F7E0", "\U0001F7E1", "\U0001F7E2"]   # red orange yellow green
SEV_NAME = {"\U0001F534": "RED", "\U0001F7E0": "ORANGE",
            "\U0001F7E1": "YELLOW", "\U0001F7E2": "GREEN"}

# ── BAND PATTERNS ────────────────────────────────────────────────────────────────
#
# ⚠ Every one of these is a SIEVE. It proposes a band and prints its own evidence; it
# does not decide anything. Read the row before acting on its band.
#
# ⚠ Ordered: the FIRST band whose pattern hits wins, so P0's patterns must be the
# narrowest. A row that both miscompiles and refuses is a miscompile.

WRONG = re.compile(
    r"silent(?:ly)?\s+(?:miscompil\w+|wrong\w*|drop\w*|truncat\w*|narrow\w*|empt\w*)"
    r"|silent\s+(?:miscompile|wrongness|attribute\s+drop|prefix|skip)"
    r"|miscompil\w+"
    r"|CRASH(?:ES|ED|ING)?\b|\bcrashes\b|0xC0000409|0xc0000409|STATUS_STACK_BUFFER_OVERRUN"
    r"|SEGV|segfault|__fastfail"
    r"|wrong\s+(?:answer|value|result|code|bytes|offset|register|symbol|type|order)"
    r"|emit(?:s|ted)?\s+the\s+wrong"
    r"|goes?\s+silent|silently\s+(?:accept|admit|ignor|mint|pass|green)"
    r"|undefined\s+behaviou?r"
    r"|reads?\s+as\s+a\s+different\s+failure\s+class",
    re.IGNORECASE)

REFUSED = re.compile(
    r"\brefus\w+|\brejects?\b|cannot\s+(?:link|compile|build|parse|read|open)"
    r"|blocks?\s+(?:real|every|the)\b"
    r"|\bP0\d{3}\b"
    r"|parse\s+error|fails?\s+to\s+(?:parse|compile|link)"
    r"|is\s+not\s+recognized|not\s+recognised|is\s+NOT\s+recognized",
    re.IGNORECASE)

# Namespaces. ★ BOTH `D-C-*` AND `D-CSUBSET-*` DENOTE THE C LANGUAGE -- the 426
# `D-CSUBSET-*` ids are frozen by operator ruling, so a sweep of either prefix ALONE
# misses part of the language. Same trap on `D-FFI-` vs `D-FF1-` (digit one), which is
# an open row in its own right.
NS_HARNESS = ("D-TEST-", "D-GATE-", "D-HARNESS-", "D-BUILD-", "D-CYCLE-", "D-EXAMPLES-")
NS_RECORD = ("D-DOC-", "D-PLANS-")
NS_ENV = ("D-ENV-", "D-UPSTREAM-")

# ★★★ THE BUCKET IS THE FILE, NOT THE PREFIX -- operator ruling 2026-08-25, *"the
# priority is always production anchors. ALWAYS"*. The registry is TWO FILES, and which
# one holds a row IS the production/harness verdict. `NS_HARNESS` above is a GUESS over
# the anchor's spelling, and a guess is what this instrument used to band by.
#
# ⚠⚠ ✔MEASURED 2026-08-31 (cycle P46), and it cost a near-miss rather than a report:
# `D-CONF-REFERENCE-DIFFERENTIAL-ORACLE` lives in `-harness.md` and was printed as the
# **#1 row of the P0 WRONG-OUTPUT band**, with nothing on the line saying it was
# harness -- because `D-CONF-` is not in `NS_HARNESS` and never was. The orchestrator
# read the queue top-down and came within one step of seeding a lane on it, which the
# standing ruling forbids outright (harness is drained BY ENCOUNTER, never scheduled).
# ★ The instrument was not lying. It was answering *"does this id LOOK harness-y?"*
# when the question is *"which registry file holds this row?"* -- an adjacent question,
# and it failed toward *schedulable*, which is the flattering direction.
#
# ⇒ GROUND TRUTH WHERE IT EXISTS, HEURISTIC ONLY WHERE IT DOES NOT. For the two
# registry files the FILE decides, in BOTH directions: a harness-registry row is P3
# however it is spelled, and a production-registry row is NEVER demoted to P3 by its
# prefix. Per-plan rows have no such file, so they keep the namespace sieve.
# ⇒ D-QUEUE-BURNDOWN-BANDED-A-HARNESS-ROW-INTO-THE-PRODUCTION-QUEUE (closed 2026-08-31)
REG_PRODUCTION = ".plans/_deferred-anchor-registry-production.md"
REG_HARNESS = ".plans/_deferred-anchor-registry-harness.md"
BUCKET_PRODUCTION = "PRODUCTION"
BUCKET_HARNESS = "harness"
BUCKET_PLAN = "plan"


def bucket_of(rel):
    """-> which registry file holds this row. `plan` = a per-plan section 3.1 row."""
    if rel == REG_PRODUCTION:
        return BUCKET_PRODUCTION
    if rel == REG_HARNESS:
        return BUCKET_HARNESS
    return BUCKET_PLAN

BANDS = ("P0", "P1", "P2", "P3", "P4", "P5")
BAND_TITLE = {
    "P0": "WRONG-OUTPUT   -- ships a bad binary: silent wrongness, or a crash on legal input",
    "P1": "REFUSED        -- real code that does not compile, link or parse today",
    "P2": "DIVERGENT      -- a product-namespace divergence or an absent capability",
    "P3": "HARNESS        -- test / gate / build / cycle instruments",
    "P4": "RECORD         -- plans, registry, documentation",
    "P5": "ENV            -- environment and upstream; not ours to fix in the compiler",
}

_CLAUSE_BREAK = re.compile(r"[.;—]|\*\*")
_ANCHOR_TOKEN = re.compile(r"D-[A-Z0-9]+(?:-[A-Z0-9]+)+")

# ── A NEGATED PHRASE IS NOT A CLAIM, AND THIS COST A RE-RUN TO LEARN ────────────
#
# ⚠⚠ ✔MEASURED: the first draft of this instrument banded 201 rows P0, and reading
# the head of that band showed the sieve counting ROWS THAT EXPLICITLY DENY BEING
# PRODUCTION ERRORS -- `D-CSUBSET-BLOCK-SCOPE-UNKNOWN-ATTRIBUTE-SILENT` says
# "NOT A MISCOMPILE", `D-CSUBSET-TYPEDEF-REDECLARATION-REJECTED` says "FAIL-LOUD,
# NEVER A MISCOMPILE", `D-LINK-WRITER-DANGLING-SYMLINK-CLAIM-MISROUTE` says "this is
# NOT a silent miscompile". That is the "103 misglyphed rows / the real number was 4"
# defect reproduced exactly, inside the instrument whose docstring warns about it.
#
# ★ THREE DISTINCT NEGATION CLASSES, and only the first is ordinary negation:
#   1. DENIAL      -- "not a miscompile", "never wrong code", "nothing is silent"
#   2. REQUIREMENT -- "must NOT silently truncate", "never silently drop the row".
#      A rule the FIX must satisfy is not a description of the defect.
#   3. COUNTERFACTUAL -- "would have SILENTLY ACCEPTED", "would become a silent
#      miscompile". A hazard avoided is not a hazard shipped.
#
# ⚠ The window looks only BEHIND the hit, exactly as `check-anchor-balance`'s
# `_DECL_NEGATOR` does, so a phrase that CONTAINS a negator is unaffected.
# ⚠ Deliberately CONSERVATIVE. For a queue the safe error is leaving a row in P0 --
# you read it and demote it by hand -- never hiding one. And nothing is hidden anyway:
# every demotion is printed under DEMOTED with the phrase that caused it.
_NEG_WINDOW = 46
_NEGATOR = re.compile(
    r"(?:NOT\s+A|NOT\s+AN|IS\s+NOT|WAS\s+NOT|ARE\s+NOT|WERE\s+NOT|NEVER|NO\s+LONGER"
    r"|NOTHING|RATHER\s+THAN|INSTEAD\s+OF|FAR\s+FROM|CANNOT|CAN\s+NOT|MUST\s+NOT"
    r"|MUST\s+NEVER|DO\s+NOT|DOES\s+NOT|DID\s+NOT|SHOULD\s+NOT|MAY\s+NOT|WILL\s+NOT"
    r"|WOULD\s+HAVE|WOULD\s+BE|WOULD\s+HAVE\s+BEEN|COULD\s+HAVE|WOULD"
    r"|TO\s+STOP|TO\s+PREVENT|GUARDS?\s+AGAINST|PREVENTS?|STOPS?|REFUSES?\s+TO"
    r"|NO\s+PROGRAM|NEITHER)"
    r"[\s\w,'’—-]{0,%d}$" % _NEG_WINDOW, re.IGNORECASE)


def negated(flat, match):
    return bool(_NEGATOR.search(flat[max(0, match.start() - _NEG_WINDOW - 12):match.start()]))


def own_claim(flat, match):
    """True when `match` is a claim about THIS row rather than about a cited one.

    ★ The discriminator is the house one, lifted in SHAPE (not by import, because
    `declares_no_trigger` hard-codes its own pattern) from `check-anchor-balance`'s
    `declares_no_trigger`: a row's own verdict says "is a silent miscompile", never
    "[[D-OTHER]], which is a silent miscompile". If an anchor id sits between the
    nearest clause boundary and the phrase, the phrase is about somebody else.
    """
    head = flat[:match.start()]
    breaks = list(_CLAUSE_BREAK.finditer(head))
    clause = head[breaks[-1].end():] if breaks else head
    return not _ANCHOR_TOKEN.search(clause)


def first_own_match(pattern, flat):
    """First hit that is this row's OWN claim and is NOT negated.

    -> (match, None) on a live claim; (None, first-suppressed-match) when every hit
    was attributive or negated; (None, None) when there was no hit at all. The
    suppressed match is RETURNED, never discarded, so the caller can print what it
    chose not to believe.
    """
    suppressed = None
    for m in pattern.finditer(flat):
        if not own_claim(flat, m) or negated(flat, m):
            suppressed = suppressed or m
            continue
        return m, None
    return None, suppressed


def evidence(flat, match, width=54):
    lo = max(0, match.start() - width // 2)
    hi = min(len(flat), match.end() + width // 2)
    return ("..." if lo else "") + flat[lo:hi].strip() + ("..." if hi < len(flat) else "")


def band_of(name, flat, bucket=BUCKET_PLAN):
    """-> (band, evidence, demoted_phrase_or_None).

    `demoted` is set when a WRONG phrase was FOUND and then disbelieved -- attributive
    or negated. It is carried out so the report can print it: a sieve that silently
    swallows the thing it decided not to count is unauditable, which is the whole
    complaint against the census this instrument is modelled on avoiding.

    ⚠ `bucket` is GROUND TRUTH and outranks every namespace guess below -- see the
    `bucket_of` comment. It defaults to `plan` so that a caller which does not know the
    row's home gets the old heuristic rather than a silent mis-band.
    """
    base = name.split("#")[-1]
    if base.startswith(NS_ENV):
        return "P5", "namespace", None
    if base.startswith(NS_RECORD):
        return "P4", "namespace", None
    m, sup = first_own_match(WRONG, flat)
    if bucket == BUCKET_HARNESS:
        # ★ THE FILE SAID SO. No prefix needs to agree, and this is the arm that catches
        # `D-CONF-*`, `D-SCRIPT-*` and every other harness id nobody thought to list.
        return "P3", ("registry file; note: %s" % evidence(flat, m)) if m else \
            "registry file (-harness.md)", None
    if bucket != BUCKET_PRODUCTION and base.startswith(NS_HARNESS):
        # ⚠ A harness row can still describe a PRODUCT miscompile it found. Band it by
        # what it IS -- a harness row -- but say so, because the sieve saw the phrase.
        # ⚠ SKIPPED ENTIRELY for a production-registry row: the file already said the
        # row is production, and letting a spelling demote it to P3 is this defect in
        # the OTHER direction -- a real production error filed under the workshop.
        return "P3", ("namespace; note: %s" % evidence(flat, m)) if m else "namespace", None
    if m:
        return "P0", evidence(flat, m), None
    demoted = evidence(flat, sup) if sup else None
    m2, _ = first_own_match(REFUSED, flat)
    if m2:
        return "P1", evidence(flat, m2), demoted
    return "P2", "product namespace, no live wrong-output or refusal phrase", demoted


def severity_of(status):
    for g in SEVERITY:
        if g in status:
            return g
    return ""


def load_row_text(root):
    """-> {"relpath#anchor": (full_flat_row, status_cell)} for every table row.

    ⚠ Reads the WHOLE row, not `check-anchor-balance`'s status EXCERPT: an excerpt is
    truncated, and banding on a truncated cell is how a sieve reports a plausible zero.
    Cells are split with that module's own `split_row`, so an escaped pipe is not
    mistaken for a separator -- a defect it measured across 161 rows.
    """
    out = {}
    plans = os.path.join(root, ".plans")
    for n in sorted(os.listdir(plans)):
        if not n.endswith(".md"):
            continue
        rel = ".plans/%s" % n
        with io.open(os.path.join(plans, n), encoding="utf-8", newline="") as fh:
            for line in fh.read().split("\n"):
                if not ROWSTART.match(line):
                    continue
                cells = bal.split_row(line)
                if len(cells) < 3:
                    continue
                # ★ `row_name` is the SAME identity function the balance scanner keys
                # its open set by -- anchor token when there is one, decoration-stripped
                # cell otherwise. Deriving the key any other way is how the two
                # instruments come to disagree about which row is which.
                #
                # ⚠⚠ AND THE PATH HALF IS PART OF THAT IDENTITY, WHICH THIS LINE USED TO
                # HAND-ROLL AS `"%s#%s" % (rel, ...)`. ✔MEASURED 2026-08-26 (cycle P36):
                # `bal.row_key` CANONICALISES both registry files to ONE prefix (a row's
                # identity is its anchor id; which of the two files holds it is a filing
                # decision), so a hand-built `rel#name` key NEVER matched a registry row
                # -- and ALL 694 registry rows fell into `residue` and were banded by
                # nothing. The queue reported a confident P0/P1/P2 census over the 324
                # plan rows while the 500 PRODUCTION rows, which the standing operator
                # ruling names as THE candidate set, were not in it at all.
                # ★ The comment above was already right about reusing the scanner's
                # identity function -- it was applied to the NAME half and hand-rolled
                # for the PATH half, and the canonicalisation lives in the path half.
                # A half-reused identity function is how two instruments that both look
                # correct disagree about which rows exist.
                key = bal.row_key(rel, bal.row_name(cells[1]))
                flat = bal.strip_decoration(line)
                status = bal.strip_decoration(cells[2])
                # ⚠ `rel` IS CARRIED even though `row_key` deliberately canonicalises
                # the two registry files to ONE prefix. That canonicalisation is right
                # for the BALANCE gate -- moving a row between buckets must be a no-op
                # there -- and exactly wrong here, where the file IS the priority.
                # Reusing the key without re-reading the home is how this instrument
                # lost the production/harness distinction in the first place.
                out.setdefault(key, (flat, status, rel))
    return out


def build(root):
    scan = bal.scan_worktree(root)
    if len(scan.rows) < FLOOR_OPEN:
        sys.exit("burndown-queue: FAIL -- the scan COLLAPSED: %d open row(s), floor is "
                 "%d. A queue over an empty set says 'nothing to do', which is the most "
                 "flattering possible lie. Fix the scan, never the floor."
                 % (len(scan.rows), FLOOR_OPEN))
    text = load_row_text(root)
    items, residue = [], []
    for key in scan.rows:
        flat, status, rel = text.get(key, ("", "", ""))
        if not flat:
            # A row the balance scanner counted OPEN but whose line this reader could
            # not find. NEVER silent: it is the difference between the two instruments.
            residue.append(key)
            continue
        bucket = bucket_of(rel)
        band, why, demoted_phrase = band_of(key, flat, bucket)
        gated = key in scan.gated_rows
        unblocked = key in scan.unblocked
        items.append({
            "demoted": demoted_phrase,
            "key": key,
            "anchor": key.split("#")[-1],
            "home": key.split("#")[0],
            "source": rel,
            "bucket": bucket,
            "band": band,
            "why": why,
            "sev": severity_of(status),
            "sev_name": SEV_NAME.get(severity_of(status), "-"),
            # SCHEDULABLE means: not gated at all, or gated by something already
            # discharged. `check-anchor-balance` owns both of those verdicts.
            "schedulable": (not gated) or unblocked,
            "unblocked": unblocked,
            "status": status[:160],
        })
    order_band = {b: i for i, b in enumerate(BANDS)}
    order_sev = {g: i for i, g in enumerate(SEVERITY)}
    items.sort(key=lambda r: (order_band[r["band"]],
                              0 if r["schedulable"] else 1,
                              order_sev.get(r["sev"], len(SEVERITY)),
                              r["anchor"]))
    return items, residue, scan


def main(argv):
    ap = argparse.ArgumentParser(add_help=True)
    ap.add_argument("--band", nargs="+", choices=BANDS)
    ap.add_argument("--schedulable", action="store_true")
    ap.add_argument("--top", type=int)
    ap.add_argument("--counts", action="store_true")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--evidence", action="store_true",
                    help="print the phrase that banded each row")
    a = ap.parse_args(argv)

    items, residue, scan = build(ROOT)
    shown = [r for r in items
             if (not a.band or r["band"] in a.band)
             and (not a.schedulable or r["schedulable"])]
    if a.top:
        shown = shown[:a.top]

    if a.json:
        print(json.dumps({"items": shown, "residue": residue}, indent=2))
        return 0

    print("burndown-queue: %d OPEN row(s) across .plans/ -- %d SCHEDULABLE, %d gated "
          "on a trigger that has not fired"
          % (len(items),
             sum(1 for r in items if r["schedulable"]),
             sum(1 for r in items if not r["schedulable"])))
    prod_total = sum(1 for r in items if r["bucket"] == BUCKET_PRODUCTION)
    print("burndown-queue: BY BUCKET (the operator's outer sort key) -- "
          "PRODUCTION %d, harness %d, per-plan %d"
          % (prod_total,
             sum(1 for r in items if r["bucket"] == BUCKET_HARNESS),
             sum(1 for r in items if r["bucket"] == BUCKET_PLAN)))
    print("\nBAND       total  schedulable  PROD   RED  ORANGE  YELLOW  GREEN")
    for b in BANDS:
        rows = [r for r in items if r["band"] == b]
        if not rows:
            continue
        print("  %-3s %9d %12d %5d %5d %7d %7d %6d   %s"
              % (b, len(rows), sum(1 for r in rows if r["schedulable"]),
                 sum(1 for r in rows if r["bucket"] == BUCKET_PRODUCTION),
                 sum(1 for r in rows if r["sev_name"] == "RED"),
                 sum(1 for r in rows if r["sev_name"] == "ORANGE"),
                 sum(1 for r in rows if r["sev_name"] == "YELLOW"),
                 sum(1 for r in rows if r["sev_name"] == "GREEN"),
                 BAND_TITLE[b].split("--")[0].strip()))
    if a.counts:
        _residue_report(residue)
        _demoted_report(items)
        return 0

    band = None
    for r in shown:
        if r["band"] != band:
            band = r["band"]
            print("\n=== %s %s ===" % (band, BAND_TITLE[band]))
        # ★ THE BUCKET IS ON EVERY LINE, not just in the summary. A reader takes the
        # NEXT TICKET off this list, and the ruling that harness is never scheduled has
        # to be visible at the row a lane would be seeded on -- not two screens up.
        print("  %-6s %-9s %-10s %s%s"
              % (r["sev_name"], "" if r["schedulable"] else "GATED",
                 "" if r["bucket"] == BUCKET_PRODUCTION else r["bucket"],
                 r["anchor"], "  [UNBLOCKED]" if r["unblocked"] else ""))
        if a.evidence:
            print("           why: %s" % r["why"])
    _residue_report(residue)
    print("\n⚠ THE BAND IS A SORT KEY, NOT A VERDICT. Every band comes from a keyword "
          "sieve over the row's own text; a sieve of this shape once reported 103 rows "
          "where the true number was 4. Read the row before acting on its band.")
    print("⚠ These are a DATED VIEW, not an invariant. Re-run rather than quote.")
    return 0


def _demoted_report(items):
    """Rows where a WRONG phrase was FOUND and then disbelieved.

    ★ This exists because the alternative -- dropping them quietly -- makes the sieve
    unauditable in exactly the direction that hides a real production error. If the
    negator is wrong about one of these, this list is where a reader sees it.
    ✔MEASURED at first run: the negator moved **92** rows out of P0 (201 -> 109), which
    is a large enough correction that it must be visible rather than merely applied.
    """
    dem = [r for r in items if r.get("demoted")]
    if not dem:
        return
    print("\n⚠ DEMOTED OUT OF P0 -- %d row(s) whose wrong-output phrase was NEGATED "
          "(\"not a miscompile\"), a REQUIREMENT (\"must never silently truncate\") or a "
          "COUNTERFACTUAL (\"would have silently accepted\"). Printed because a sieve "
          "that hides what it disbelieved cannot be checked:" % len(dem))
    for r in sorted(dem, key=lambda x: x["anchor"])[:25]:
        print("    %-3s %s\n         %s" % (r["band"], r["anchor"], r["demoted"]))
    if len(dem) > 25:
        print("    ... and %d more (--json for all)" % (len(dem) - 25))


def _residue_report(residue):
    if not residue:
        return
    print("\n⚠ RESIDUE -- %d row(s) counted OPEN by check-anchor-balance whose table "
          "line this reader could not locate. NOT dropped, because the residue of an "
          "enumeration must never be silent:" % len(residue))
    for k in sorted(residue)[:40]:
        print("    %s" % k)
    if len(residue) > 40:
        print("    ... and %d more" % (len(residue) - 40))


def selftest():
    """Pin the bucket rule in BOTH directions, and the two arms my own premise got wrong.

    ★ WHY THIS EXISTS AT ALL: `burndown-queue.py` is not wired into ctest and had no
    self-test, so the defect this pins -- a harness row printed at the top of the P0
    band -- was caught by a human reading the output, once. The proof that fixed it
    lived in a scratchpad script that does not survive the cycle. An instrument nobody
    can regression-test is an instrument that silently drifts back.

    ★ PURE-FUNCTION ARMS. `bucket_of` and `band_of` need no tree, no temp repo and no
    git, so this costs milliseconds and runs on EVERY invocation rather than behind a
    flag nobody passes.

    ⚠ EVERY POSITIVE ARM HAS A CONTROL THAT MOVES. (a) and (b) each re-band the SAME
    fixture text under a different bucket, so a green arm proves the BUCKET did the
    work -- not the sieve agreeing by luck, which is how a vacuous arm reads green
    forever.
    """
    failed = [0]

    def pin(ok, why, detail=""):
        print("  %-4s %s%s" % ("ok" if ok else "FAIL", why,
                               ("   " + detail) if detail else ""))
        if not ok:
            failed[0] += 1

    # ⓘ "miscompiles" is a live WRONG phrase: an own claim, unnegated, no anchor token
    # in its clause. If the sieve's vocabulary changes, arm (a2) goes red and says so.
    wrong = "OPEN. This miscompiles the emitted image."
    plain = "OPEN. A capability that is absent."

    b, _, _ = band_of("x#D-CONF-REFERENCE-DIFFERENTIAL-ORACLE", wrong, BUCKET_HARNESS)
    pin(b == "P3", "(a) a harness-registry row with a NON-harness prefix bands P3, "
        "not P0 -- the measured defect", "got %s" % b)
    b2, _, _ = band_of("x#D-CONF-REFERENCE-DIFFERENTIAL-ORACLE", wrong, BUCKET_PLAN)
    pin(b2 == "P0", "(a2) CONTROL: identical text with no registry home still bands "
        "P0 -- so (a) is the BUCKET's doing, not the sieve's", "got %s" % b2)

    # ⚠ EVERY FIXTURE ID BELOW IS A REAL REGISTRY ROW, AND THAT IS NOT DECORATION.
    # `scripts/` is a root `check-anchor-registry` scans, so a plausible-looking
    # invented id here is an anchor cited in the tree with no row -- precisely the leak
    # that guard exists to catch, and the first draft of this self-test minted two.
    # ⓘ `band_of` is a pure function of (name, flat, bucket), so passing a real id with
    # a bucket it does not actually live in is exactly what arm (b) needs to ask; it
    # asserts nothing about where that row is really filed.
    b, _, _ = band_of("x#D-BUILD-CONCURRENT-CONTENTION-YIELDS-TRUNCATED-OBJECT-TREATED-AS-CURRENT",
                      wrong, BUCKET_PRODUCTION)
    pin(b == "P0", "(b) a production-registry row is NOT demoted to P3 by a "
        "harness-looking prefix -- the strictly worse direction", "got %s" % b)
    b2, _, _ = band_of("x#D-BUILD-CONCURRENT-CONTENTION-YIELDS-TRUNCATED-OBJECT-TREATED-AS-CURRENT",
                       wrong, BUCKET_PLAN)
    pin(b2 == "P3", "(b2) CONTROL: the same id with no registry home still takes the "
        "namespace sieve", "got %s" % b2)

    pin(bucket_of(REG_PRODUCTION) == BUCKET_PRODUCTION
        and bucket_of(REG_HARNESS) == BUCKET_HARNESS
        and bucket_of(".plans/14-linker-plan - tbd.md") == BUCKET_PLAN,
        "(c) bucket_of maps production / harness / per-plan")

    # ⚠ (d) AND (d2) PIN THE THING I FIRST GOT WRONG. The invariant I reached for was
    # "every harness row bands P3", and the measurement refuted it with 23 rows. Both
    # bands below are LOWER priority than P3 and the verdict is more specific, so the
    # code is right; these arms exist so nobody "fixes" it back by reordering `band_of`.
    b, _, _ = band_of("x#D-ENV-MACOS-GATEKEEPER-ADMISSION-IRREDUCIBLE", wrong,
                      BUCKET_HARNESS)
    pin(b == "P5", "(d) a D-ENV-* row in -harness.md bands P5, NOT P3 -- deliberate: "
        "ENV is more specific AND lower", "got %s" % b)
    b, _, _ = band_of("x#D-PLANS-LINE-CITATION-ROT", wrong, BUCKET_HARNESS)
    pin(b == "P4", "(d2) a D-PLANS-* row in -harness.md bands P4, NOT P3 -- same "
        "reason", "got %s" % b)

    b, _, _ = band_of("x#D-CSUBSET-VLA", plain, BUCKET_PRODUCTION)
    pin(b == "P2", "(e) GREEN CONTROL: an ordinary production row with no wrong or "
        "refusal phrase still bands P2 -- the fix did not move normal banding",
        "got %s" % b)

    print("burndown-queue: self-test %s - 8 arm(s), %d failure(s); this instrument is "
          "PROVEN able to fail." % ("OK" if not failed[0] else "FAIL", failed[0]))
    return 1 if failed[0] else 0


if __name__ == "__main__":
    _rc = selftest()
    sys.exit(_rc or main(sys.argv[1:]))
