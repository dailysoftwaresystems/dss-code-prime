#!/usr/bin/env python3
# PURPOSE: list OPEN registry rows whose Closing-work cell waits on a blocker that has since CLOSED.
"""Stale-blocker scan: an open row waiting on something that already landed.

★★★ WHY THIS EXISTS, AND WHY NEITHER EXISTING GATE CAN SEE IT

A row's STATUS cell stays honest while the SENTENCE keeping it open goes false.
That is [[feedback-a-rows-premise-has-a-shelf-life]], and its worst form is a row
whose Closing-work cell says *"blocked on [[X]]"* when X closed weeks ago: every
queue in this project reads the status cell, so nobody picks the row up, and the
work sits in the registry looking like a decision rather than like an oversight.

Neither anchor gate can catch it, and for good reasons of their own:

  * `check-anchor-balance` compares row NAMES across two commits. A row that was
    open before and is open now is, to it, a non-event -- which is correct for
    what that gate measures.
  * `check-anchor-registry` RESOLVES a citation, i.e. asks whether the cited row
    EXISTS. It deliberately globs all three registries so a `D-*` cited in `src/`
    still resolves after its row moves to the archive on close. Asking whether
    the cited row is still OPEN is a different question, and answering it inside
    that guard would break the resolution it is there to provide.

✔MEASURED 2026-09-08 (cycle P65), the run that produced this script: **71**
citations across the two working registries, **32 of them in production rows and
16 of those at P1** -- including `D-LIR-SUBREGISTER-AWARE-ALLOCATION-FOR-ALIASED-VIEWS`,
whose Closing-work cell reads *"BLOCKED ON THE SAME ONE THING AS ITS SIBLING and
on nothing of its own"* and names a row that closed 2026-08-27, compiled and run
under qemu.

────────────────────────────────────────────────────────────────────────────────
⚠⚠ THIS PRODUCES LEADS, NOT VERDICTS, AND THAT IS WHY IT IS NOT A ctest GUARD

A cited row can be closed while the thing the citing row waits for genuinely did
not land: the citation may name a PARENT whose closure covered a different half,
or a sibling that closed on the other axis. Every hit must be READ before it is
acted on. Registering this as a gate would make it refuse honest trees, and --
worse -- would invite a future cycle to silence it with an escape hatch, which is
the failure mode [[feedback-an-escape-every-row-triggers-disarms-the-guard]]
names. It is an orchestrator's triage instrument and stays one.

⚠ THE HEURISTIC IS DELIBERATELY NARROW so the output is worth reading. Only a
`[[wikilink]]` in the CLOSING-WORK cell counts. That is the cell that states what
the row is waiting for; a Cross-refs mention is a relationship, not a dependency,
and counting those turned a readable list into noise when it was tried.

────────────────────────────────────────────────────────────────────────────────
USAGE

    python scripts/check-stale-blockers/check-stale-blockers.py
    python scripts/check-stale-blockers/check-stale-blockers.py --production
    python scripts/check-stale-blockers/check-stale-blockers.py --band P1
    python scripts/check-stale-blockers/check-stale-blockers.py --selftest

Exit status is 0 whether or not hits are found -- it reports, it does not judge.
A non-zero status means the SCAN itself could not run.

⚠ It reads through `anchors.py`'s own `read_rows`, never off a raw table line:
storage escapes every `|` as `\\|`, and a raw read hands the escape back doubled.
That is the defect measured in P54 across 35 pipes in 14 rows.
"""
import argparse
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, "scripts", "anchors"))

import anchors  # noqa: E402

WIKILINK = re.compile(r"\[\[([A-Za-z0-9_-]+)\]\]")


def scan(root, buckets=None, band=None):
    """-> [(row, cited_name)] for every open row citing a CLOSED blocker.

    The index is built over EVERY bucket including the archive, because the
    blocker we are looking for is by definition closed and therefore lives
    there. Only the CITING side is filtered.
    """
    rows = anchors.read_rows(root, anchors.BUCKETS)
    by_name = {r.name.strip("`"): r for r in rows}

    hits = []
    for row in rows:
        if row.closed:
            continue
        if buckets and row.bucket not in buckets:
            continue
        if band and row.priority != band:
            continue
        seen = set()
        for cited in WIKILINK.findall(row.cell(anchors.C_CLOSING)):
            if cited in seen:
                continue
            seen.add(cited)
            target = by_name.get(cited)
            if target is not None and target.closed:
                hits.append((row, cited))
    return hits


def self_test():
    """Prove the matcher can FIRE and can STAY SILENT, on synthetic rows.

    ★ Without the negative arm, a green run is equally consistent with "the
    registry is clean" and with "this scanner matches nothing at all" -- the
    vacuous-pass shape this repository has paid for repeatedly.
    """
    ok = True

    def pin(cond, what):
        nonlocal ok
        print("  [%s] %s" % ("ok  " if cond else "FAIL", what))
        ok = ok and cond

    # The wikilink matcher is the only text-level judgement this script makes.
    #
    # ⚠ THE FIXTURE NAMES DELIBERATELY CARRY NO `D-` PREFIX, and that is not
    #   cosmetic. `scripts/` is a SCANNED ROOT for `check-anchor-registry`, which
    #   refuses any `D-*` cited there with no row in `.plans/`. A synthetic
    #   fixture id spelled like a real anchor is indistinguishable from a real
    #   citation to every grep in this repository -- so it reds that guard, and
    #   the "fix" nearest to hand is an allowlist entry, i.e. an escape carved
    #   into a guard to accommodate a test. ✔MEASURED: the first draft of this
    #   self-test used two anchor-shaped fixture names and reddened that guard
    #   for exactly this reason -- and the FIRST repair, a comment explaining the
    #   mistake, reddened it AGAIN, because a comment quoting an id is still a
    #   citation to every grep. Neither the fixture nor its explanation may spell
    #   one. Naming them so they cannot be mistaken for anchors costs nothing and
    #   keeps the other guard's refusal intact.
    pin(WIKILINK.findall("blocked on [[FIXTURE-ALPHA]] and [[FIXTURE-BETA]]")
        == ["FIXTURE-ALPHA", "FIXTURE-BETA"],
        "(1) the matcher finds every wikilink in a cell")
    pin(WIKILINK.findall("no links here at all") == [],
        "(2) the matcher stays silent on a cell with no wikilink")
    pin(WIKILINK.findall("a bare FIXTURE-GAMMA name is not a citation") == [],
        "(3) a bare anchor id is NOT a dependency -- only [[...]] counts")

    # ...and the live scan must be able to answer at all.
    try:
        live = scan(ROOT)
        pin(True, "(4) the live registries scan without error (%d hit(s))" % len(live))
    except Exception as exc:  # noqa: BLE001 - the point is to report, not to raise
        pin(False, "(4) the live scan RAISED: %r" % (exc,))

    print("check-stale-blockers: self-test %s - 4 arms" % ("OK" if ok else "FAILED"))
    return 0 if ok else 1


def main(argv):
    ap = argparse.ArgumentParser(add_help=True)
    ap.add_argument("--production", action="store_true")
    ap.add_argument("--harness", action="store_true")
    ap.add_argument("--band", default=None, help="P0..P5")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args(argv)

    if a.selftest:
        return self_test()

    buckets = []
    if a.production:
        buckets.append("production")
    if a.harness:
        buckets.append("harness")

    hits = scan(ROOT, buckets or None, a.band)
    for row, cited in sorted(hits, key=lambda h: (h[0].priority, h[0].name)):
        print("%-4s %-11s %-62s waits on CLOSED %s"
              % (row.priority, row.bucket, row.name.strip("`")[:62], cited))
    print("check-stale-blockers: %d citation(s) -- an OPEN row naming a CLOSED "
          "blocker in its Closing-work cell." % len(hits))
    print("  These are LEADS. A cited row can close on a half this row was not "
          "waiting for; read each before acting.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
