#!/usr/bin/env python3
"""sqlite_verdicts.py -- the decisions Step 8 and Step 9 make about what a run PROVED.

  * the CONFOUND SUPPLY: the leg's OWN earned list (legs.json `confounds`, resolved and
    corroborated by `harness_legs.py`), or the operator's DSS_CONFOUNDS override for EVERY leg --
    refused when the transport lost the list, when the plan's environment probes did not
    measure (`confoundGating` must be `probed`), or when the leg's run directory was not
    corroborated (`runDirectoryGating` must be `not-required` or `measured`);
  * the CONFOUND REPORT, printed verbatim (the resolver composes it; an EMPTY one is refused --
    an unexplained exclusion is not an earned one);
  * the CLASSIFIER: `native:`/`emulated:` scoped patterns (inert on the other mode), the
    permutation prefix sqlite ITSELF declares stripped (longest first), `re.search` against the
    name as reported AND stripped -- the .sh's `[[ =~ ]]` semantics, CASE-SENSITIVE (the .ps1's
    case-folding excused MORE, the dangerous direction);
  * the CAPABILITY-WITNESS check (a declared capability's witness file must have ASSERTED
    something; a witness never in this corpus is reported, never counted as a pass);
  * the VERDICT LADDER, first match wins, with the .sh's meaning (the .ps1 tested segment 0's
    summary alone, which made an earned-abort PASS unreachable);
  * the Step-9 LEDGER COUNTS in `ArmVerdictLedger::renderCountsLine()`'s words, the classes read
    from the resolver (`--verdict-classes`), never from a mirrored list.

The union of `build-and-test.sh` and `build-and-test.ps1` (lane mig, part 4, 2026-09-21).
Nothing runs at import.
"""
from __future__ import annotations

import collections
import re
import sys

import sqlite_common as C

for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError, OSError):  # pragma: no cover - an odd stream
        pass
sys.dont_write_bytecode = True

SCOPES = ("native", "emulated")


# ── the supply ──────────────────────────────────────────────────────────────────────

def confound_supply(leg, override):
    """-> the patterns in force on `leg`. `override` (DSS_CONFOUNDS split) replaces every leg's
    earned list; otherwise the leg's plan must carry its list and both gatings must be usable."""
    if override is not None:
        return list(override)
    d = leg.d
    # The BY-NAME supply (`confoundsByName`): an ARMED row (`confoundsByEvidence`) excuses a
    # failure only on evidence from that failure's own execution, never by name.
    missing = [k for k in ("confoundsByName", "confoundsByEvidence", "executionEvidence")
               if d.get(k) is None]
    if missing:
        C.die("[%s] the resolved leg plan carries NO %s entry.\n      harness_legs.py refuses "
              "to plan a leg that does not declare `confounds`, so this is a transport defect "
              "between the resolver and this driver — NOT a leg with nothing earned. Treating it "
              "as an empty list would silently report every failure on this leg as a DSS defect."
              % (leg.label, "/".join(missing)))
    gating = d.get("confoundGating") or "<unset>"
    if gating != "probed":
        C.die("[%s] the resolved leg plan says confoundGating='%s', not 'probed'.\n      A conditional "
              "confound row (`requires: [<environment probe>]`) is honoured ONLY where the named\n"
              "      probe MEASURED its defect as PRESENT on THIS machine, and this plan carries no "
              "such measurement.\n      'unprobed' — nothing was measured, so every conditional row "
              "is INACTIVE. Safe, and not usable:\n        the withheld excusals surface as GENUINE "
              "reds and read as compiler regressions.\n        Resolve the plan WITHOUT "
              "`--environment-probes skip` so harness_legs.py measures.\n      'injected' — the "
              "verdicts were READ FROM A FILE (`--probe-verdicts`), so conditional rows ARE\n"
              "        honoured, on evidence gathered somewhere this driver cannot vouch for. Drop "
              "the flag and let it measure." % (leg.label, gating))
    rdg = d.get("runDirectoryGating") or "<unset>"
    if rdg not in ("not-required", "measured"):
        C.die("[%s] the resolved leg plan says runDirectoryGating='%s', which is neither "
              "'not-required' nor 'measured'.\n      A confound row declaring `requiresRunDirectory` "
              "is honoured ONLY where THIS RUN measured\n      the named precondition on THIS LEG'S "
              "own run directory, and a PLAN can never carry that\n      measurement: it is resolved "
              "before any run directory exists. Call harness_legs.py\n      --corroborate-run-dir "
              "with this leg's run directory and supply THAT result here.\n      'unmeasured' is "
              "fail-safe (every corroborated row is INACTIVE) and NOT fit to run on: the\n      "
              "withheld excusals surface as GENUINE reds and read as compiler regressions."
              % (leg.label, rdg))
    return [str(p) for p in d.get("confoundsByName") or []]


def print_confound_report(leg_label, report_text, log=C.LOG):
    """The resolver's account of WHY a failure was excused, printed verbatim, one trailing CR
    stripped per line; an EMPTY report is refused."""
    if not (report_text or "").strip():
        C.die("[%s] the resolved leg plan carries an EMPTY confound report.\n      harness_legs.py "
              "emits at least one line for every leg — the rows that are ACTIVE, and one line\n"
              "      per INACTIVE row saying which probe withheld it. An empty report means the "
              "account of WHY a\n      failure was excused did not arrive, and an unexplained "
              "exclusion is not an earned one." % leg_label)
    for line in report_text.split("\n"):
        line = line[:-1] if line.endswith("\r") else line
        if line:
            log.info(line)


# ── the classifier ──────────────────────────────────────────────────────────────────

def leg_mode(leg):
    """`emulated` when the leg runs through a declared launcher, else `native` -- the resolver's
    own run mode, never a host or arch identity test."""
    return "emulated" if leg.run_mode == "launched" else "native"


def split_scope(pattern):
    for s in SCOPES:
        if pattern.startswith(s + ":"):
            return s, pattern[len(s) + 1:]
    return "", pattern


def strip_prefix(name, prefixes):
    """The name with the LONGEST permutation prefix sqlite declares stripped (the caller's list
    is longest-first); the name itself when none applies."""
    for p in prefixes:
        if p and name.startswith(p):
            return name[len(p):]
    return name


Classified = collections.namedtuple("Classified", ["real", "confound", "scoped"])


def classify(failures, patterns, prefixes, mode):
    """Dedup the failure names (code-point order, as `sort -u`), then excuse each one matched by
    a pattern in force for `mode`, against the name as reported OR with its permutation prefix
    removed. -> Classified(real, confound, scoped) -- `scoped` = excused only by a mode-scoped
    pattern (a coverage statement, not a clean pass)."""
    compiled = []
    for p in patterns:
        scope, rx = split_scope(p)
        try:
            compiled.append((scope, re.compile(rx)))
        except re.error as exc:
            C.die("the confound pattern %r is not a regular expression this driver can compile (%s). "
                  "The catalogue's lint refuses such a row; an operator DSS_CONFOUNDS entry must be "
                  "one too." % (p, exc))
    real, confound, scoped = [], [], []
    for t in sorted(set(f for f in failures if f)):
        bare = strip_prefix(t, prefixes)
        hit = None
        for scope, rx in compiled:
            if scope and scope != mode:
                continue
            if rx.search(t) or rx.search(bare):
                hit = scope
                break
        if hit is None:
            real.append(t)
        else:
            confound.append(t)
            if hit:
                scoped.append(t)
    return Classified(real, confound, scoped)


def warn_scoped(leg_label, scoped, mode, log=C.LOG):
    """An excusal that depends on HOW the leg runs is a coverage statement, not a clean pass: it
    is said out loud, with the count, the mode and the truncation caveat."""
    if not scoped:
        return
    log.warn("[%s] %d failure(s) excused ONLY because this leg runs '%s': %s"
             % (leg_label, len(scoped), mode, " ".join(scoped)))
    log.warn("      these are NOT evidence of correctness on a native run of this target — and a "
             "crash-simulation")
    log.warn("      abort can TRUNCATE the rest of its .test file, so coverage there is partial.")


# ── capability witnesses ────────────────────────────────────────────────────────────

Witnesses = collections.namedtuple("Witnesses", ["declared", "checked", "gaps", "absent"])


def witness_check(witnesses, ran_files, inert_files):
    """`witnesses` = [(capability, file-stem)]. A witness whose file RAN and asserted nothing is
    a GAP; one that never appeared in this corpus is ABSENT (reported, never a pass)."""
    ran, inert = set(ran_files), set(inert_files)
    checked, gaps, absent = 0, [], []
    for cap, stem in witnesses:
        f = stem + ".test"
        if f in ran:
            checked += 1
            if f in inert:
                gaps.append("%s(%s)" % (cap, stem))
        else:
            absent.append("%s(%s)" % (cap, f))
    return Witnesses(len(witnesses), checked, gaps, absent)


def parse_witnesses(spec):
    """The stage's witness declaration -> [(capability, file-stem)]: a mapping
    {cap: stem | {"file": stem}}, a list of `cap=stem` strings, or one space-separated string."""
    out = []
    if isinstance(spec, dict):
        for cap in sorted(spec):
            v = spec[cap]
            stem = v.get("file") if isinstance(v, dict) else v
            if stem:
                out.append((str(cap), str(stem)))
        return out
    items = spec.split() if isinstance(spec, str) else list(spec or [])
    for w in items:
        cap, _, stem = str(w).partition("=")
        if cap and stem:
            out.append((cap, stem))
    return out


# ── the Step-9 ledger ───────────────────────────────────────────────────────────────

def verdict_classes(resolver):
    """`--verdict-classes` -> OrderedDict(verdict -> class), read from the vocabulary's OWNER."""
    r = resolver.call(["--verdict-classes"])
    out = collections.OrderedDict()
    for line in r.out.splitlines():
        parts = line.rstrip("\r").split("\t")
        if len(parts) == 2 and parts[0] and parts[1]:
            out[parts[0]] = parts[1]
    if r.rc != 0 or not out:
        C.die("could not read the verdict classes from harness_legs.py (--verdict-classes, rc=%d):\n"
              "      %s\n      Without them the ledger line cannot say which class a leg fell in."
              % (r.rc, (r.err or r.out).strip() or "<no diagnostic>"))
    return out


LedgerCounts = collections.namedtuple("LedgerCounts", [
    "count", "verified", "structural", "environmental", "harness", "skipped", "failed",
    "accounted", "total", "unnamed", "bogus"])


def ledger_counts(legs, classes):
    count = collections.OrderedDict((v, 0) for v in classes)
    unnamed, bogus = [], []
    for lg in legs:
        v = lg.verdict or ""
        if not v:
            unnamed.append(lg.label)
        elif v not in count:
            bogus.append("%s=%s" % (lg.label, v))
        else:
            count[v] += 1

    def by(cls):
        return sum(n for v, n in count.items() if classes[v] == cls)
    verified, structural = by("verified"), by("structural")
    environmental, harness, failed = by("environmental"), by("harness"), by("failed")
    skipped = structural + environmental + harness
    return LedgerCounts(count, verified, structural, environmental, harness, skipped, failed,
                        verified + skipped + failed, len(legs), unnamed, bogus)


def counts_line(lc):
    """`ArmVerdictLedger::renderCountsLine()`'s words, every class named even at 0."""
    c = lc.count

    def n(v):
        return c.get(v, 0)
    return ("verdicts : %d verified (%d ran, %d expect-error), %d skipped [structural: %d by-runOn, "
            "%d no-emulator-declared; environmental: %d emulator-missing, %d "
            "launcher-prerequisite-missing, %d build-input-missing; harness: %d not-selected], %d "
            "poisoned  (of %d declared legs)"
            % (lc.verified, n("ran"), n("expect-error-asserted"), lc.skipped, n("skipped-by-runOn"),
               n("skipped-no-emulator-declared"), n("skipped-emulator-missing"),
               n("skipped-launcher-prerequisite-missing"), n("skipped-build-input-missing"),
               n("not-selected-by-runner"), lc.failed, lc.total))
