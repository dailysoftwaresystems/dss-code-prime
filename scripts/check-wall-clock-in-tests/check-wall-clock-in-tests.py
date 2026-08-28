#!/usr/bin/env python3
# PURPOSE: refuse a new wall-clock duration literal in test code outside the shared measured budget.
"""Refuse a NEW wall-clock duration literal in test / test-support code.

D-TEST-A-NEW-WALL-CLOCK-LITERAL-IN-A-TEST-IS-UNGUARDED — the row this guard closes.

WHY THIS EXISTS, and it is a measured recurrence rather than a style rule.
A wall-clock number written into a test is sized on the machine that wrote it. It
passes there and reds on the slowest leg that runs it — and it reds NAMING THE
WRONG EVENT, because a timeout usually surfaces as a sentinel return value that
the assertion reports as a wrong result. ✔MEASURED on CI run 32585879580,
`linux-clang-asan`: `WorkspaceProjectE2E.ASaveThatChangesNoManifestRepublishes-
Nothing` failed against a hard-coded TWO-SECOND deadline while the same sanitized
binary passed in 622 ms idle and took 1912 ms under 3x CPU contention; the failure
read `Which is: -1`, i.e. an exit status, which is not what happened.

That was fixed by unifying the four deadlines that existed under `tests/lsp/` onto
one measured budget (`tests/test_support/test_wait_budget.hpp`). ⇒ WHAT WAS FIXED
IS THE POPULATION, NOT THE CLASS. Nothing refused a fifth literal, and the fifth
fails exactly as the fourth did. A comment is not a guard.

★★ WHAT IS **NOT** REFUSED, AND THE DISTINCTION IS THE WHOLE DESIGN.
`sleep_for` / `sleep_until` are left alone. A sleep is a DELAY: nothing fails when
the host is slow, it just takes longer. A deadline is a VERDICT: when the host is
slow the test reds. The registry row is explicit that a guard reding on every
`chrono::milliseconds(1)` sleep would be turned off within a week, and a guard
that gets turned off protects nothing.

★★★ WHAT **IS** REFUSED IS THE COMPLEMENT, NOT AN ENUMERATION OF SPELLINGS, and
that is deliberate. The row names three shapes (`wait_for(`, `wait_until(`,
`now() + `). ✔MEASURED 2026-08-23 over the 327 files under `tests/` and
`integrated_tests/`: those three shapes account for FIVE sites and every one of
them already routes through a named budget or a parameter — while the single
idiom `runBinary(exe, std::chrono::milliseconds{5000})`, which is none of the
three and is the identical hazard, accounts for TWENTY-SIX. A guard keyed on the three named spellings would have measured green
over the whole live population. So the rule is: a numeric `chrono` duration
literal is a wall-clock literal WHEREVER it appears, unless it is a sleep. That is
the same inversion `check-anchor-balance` uses for its closed-marker rule —
define the complement, never the variants — and it errs in the same safe
direction: an unenumerated new spelling is caught rather than waved through.
ⓘ It therefore also catches an assertion BOUND (`EXPECT_LT(elapsed, seconds{10})`).
That is not collateral damage: a wall-clock assertion sized on a developer machine
is the same defect wearing an assertion's clothes, and this tree already carries
one (`Checkpoint.ThousandTokensWithSpeculationUnder50ms`).

FAIL-CLOSED, like every check in this battery: an empty scan is a COLLAPSE, not a
pass (D-GATE-ANCHOR-GUARD-FAILS-OPEN-ON-MISSING-ROOT).

★ THE COMMENT/STRING STRIPPER IS **IMPORTED**, NOT COPIED. `check-no-abort-in-tests`
already owns an audited, self-tested one, and it exists for the identical reason:
both textual occurrences of the token in the FIXED fixture live inside the comment
explaining the fix, so a bare grep reds on the file that documents the repair
(D-GATE-ANCHOR-CITATION-RESOLVES-VIA-ITS-OWN-BUG-REPORT). Two copies of a C
comment stripper in one repository is the duplicated-site shape this project keeps
closing everywhere else. The import fails LOUD if that file moves.

★ NO `.ps1` TWIN, and that is the recorded rule rather than an omission
(D-GATE-SCRIPT-PS1-PAIRING-UNCHECKED, closed by operator ruling 2026-08-19): a
`.py` already runs on both hosts, so a twin would be a second implementation of
something that was never split. Eight of this repository's Python guards have no
twin for the same reason.

Exit codes: 0 = clean; 1 = a new literal, a stale INVENTORY ceiling, an ALLOWLIST
path that is gone, or an ALLOWLIST entry that exempted nothing; 2 = the scan
COLLAPSED (a missing root, or fewer files than FILE_FLOOR). ★ The collapse check
runs FIRST, so a moved subtree is reported as a collapse rather than as whichever
smaller inconsistency it happens to also produce
(D-TEST-WALL-CLOCK-SCAN-COLLAPSE-REPORTED-AS-A-STALE-ALLOWLIST-PATH).
"""
import importlib.util
import io
import os
import re
import sys

ROOTS = ('tests', 'integrated_tests')
EXTS = ('.cpp', '.hpp', '.h', '.cc')
# Floor: far below the live figure so ordinary churn never trips it. This catches
# a COLLAPSED scan (a moved subtree, a drifted extension filter), not drift.
FILE_FLOOR = 250

# ══ ALLOWLIST — BY PROOF, AND NEVER BY POSITION ═══════════════════════════════
#
# A key is either a PATH (the whole file is a budget-definition file) or
# `PATH::SYMBOL` (one named constant in an ordinary file is correct as a literal).
# Never `path:line`: a line key is a positional citation that silently points at
# unrelated code the moment anything above it moves, which is the defect
# `check-plan-citations` exists to stop one level up. Both keys here are stable
# under any movement WITHIN a file, and both are also the HONEST key -- one claims
# something about what the whole file IS, the other about what one NAME means.
#
# ★★★ THE SYMBOL FORM EXISTS BECAUSE THE MATCHER ALREADY PROMISED IT AND NOTHING
# DELIVERED IT. `_LITERAL`'s optional identifier is documented below as
# deliberate, "so it can be allowlisted by proof" -- but until 2026-08-23 the only
# allowlist key was a whole PATH, so the promise was unreachable for every file
# that is not itself a budget definition. ✔MEASURED: `integrated_tests/runner.cpp`
# holds `constexpr auto kRunningStaleAfter = std::chrono::hours{6}`, a SIX-HOUR
# scratch-root staleness threshold that is not a wait budget at all and cannot be
# routed through one -- and the only discharge available was to exempt a
# 4200-line subprocess runner wholesale, i.e. to blind the guard to exactly the
# file where the next `runBinary` deadline would be typed. A hole that looks like
# a policy is what that comment calls it; this was the second one.
#
# ⛔ THE SYMBOL FORM CANNOT EXEMPT AN UNNAMED LITERAL, BY CONSTRUCTION. The key is
# the identifier the literal is BOUND to, recovered from the source
# (`declared_name`), and an inline `runBinary(exe, chrono::milliseconds{5000})`
# binds nothing -- so it has no key and no entry can reach it. Naming a constant is
# therefore a PREREQUISITE for proving it, which is the right order: you cannot
# claim a number is correct until you have said what it is.
#
# ⛔ AND AN ENTRY THAT MATCHES NOTHING IS A FAILURE, not a leftover. A stale
# exemption exempts nothing and hides the next thing that takes its name; `main`
# reds on any entry the scan never used, exactly as it does on a path that no
# longer exists.
#
# ⚠ AN ENTRY IS A PROOF, NOT A PARKING SPACE. The INVENTORY below is unexamined
# debt; this dict is examined sites. Moving a site here without the reasoning is
# the laundering [[D-TEST-WALL-CLOCK-LITERAL-INVENTORY-IS-DEBT]] forbids -- the
# reason string is the proof, and `_selftest` refuses one shorter than 40 chars.
ALLOWLIST = {
    'tests/test_support/test_wait_budget.hpp':
        'THIS FILE IS THE SHARED BUDGET VOCABULARY. It exists to define every '
        'measured wall-clock cap the suite has -- kWaitBudget, kRunBudget, '
        'kAdmissionBudget, kHelperScriptBudget -- each carrying the measurement '
        'that sized it (622 ms idle / 1912 ms under 3x contention / CI crossed a '
        '2 s deadline at 2204 ms; the macOS admission table; the two helper-script '
        'spawns). A literal here is the thing every other literal is supposed to '
        'be replaced BY, so refusing it would refuse the fix.',

    # ── integrated_tests/runner.cpp — one symbol, and it is not a budget ──────
    'integrated_tests/runner.cpp::kRunningStaleAfter':
        'NOT A WAIT BUDGET AND NOT A DEADLINE. Six hours is a STALENESS threshold '
        'on a scratch root: it decides whether a leftover directory from an '
        'earlier run may be reclaimed, and nothing waits on it, nothing is killed '
        'by it, and no assertion is bounded by it. It cannot fail on a slow host, '
        'because no elapsed time is compared against it during a run -- making it '
        'larger only keeps scratch longer and making it smaller only reclaims '
        'sooner. Folding it into the 60 s kWaitBudget would be wrong by four '
        'orders of magnitude and would describe a housekeeping policy as a wait.',

    # ── tests/test_support/test_run_binary_deadline_clock.cpp ────────────────
    # THE FILE IS A TEST *OF* A DEADLINE CLOCK, so durations are its SUBJECT.
    # Deliberately six symbol entries rather than one whole-file entry: a
    # whole-file key would also exempt a future `runBinary(x, seconds{30})` typed
    # into the same file, which is the ordinary hazard and must stay refused.
    'tests/test_support/test_run_binary_deadline_clock.cpp::kChildHang':
        'A STIMULUS, NOT A BOUND. It is how long the deliberately-hanging child '
        'sleeps, i.e. something this test CAUSES. A slow host makes the child more '
        'certainly outlive the parent budget, never less, so no assertion that '
        'reads it can red under load.',
    'tests/test_support/test_run_binary_deadline_clock.cpp::kSpawnBudget':
        'THE SUBJECT ITSELF: the deadline whose firing is under test. It MUST sit '
        'below kChildHang or there is nothing to fire, so routing it through the '
        'shared 5 s kRunBudget would put it ABOVE the 4 s hang and the pin would '
        'pass by never being reached. TWO assertions read it. The only one that '
        'reads it AS A LOWER BOUND is EXPECT_GE(awakeElapsed, kSpawnBudget), which '
        'is safe under load; the other, EXPECT_LT(awakeElapsed, kSpawnBudget + '
        'kKillSlack), is an UPPER bound and IS load-fragile -- it is disclosed and '
        'priced under the kKillSlack entry, so no site escapes scrutiny. '
        'WARNING, and it is why the wording is spelled out: this string used to say '
        '"The only assertion reading it is a LOWER bound", dropping the qualifier '
        'from the C++ comment it paraphrases (which says "the only assertion that '
        'reads it AS A LOWER BOUND" and is correct). Four words of a paraphrase '
        'turned a scoped claim into a false universal, and an allowlist entry is '
        'read as the REASON a site is exempt -- so a wrong one exempts it for a '
        'reason that is not true.',
    'tests/test_support/test_run_binary_deadline_clock.cpp::kProbeSleep':
        'A STIMULUS: the sleep the clock-advance pin measures across. The '
        'assertions DERIVE from it (kProbeSleep / 2) instead of restating a second '
        'number, so the relationship cannot drift, and a slow host only makes the '
        'lower bound more true.',
    'tests/test_support/test_run_binary_deadline_clock.cpp::kReadJitter':
        'A DISCRIMINATOR, NOT A BUDGET. It separates "these two clock ids are the '
        'same clock" from "they are different clocks", and it sits far below what '
        'it must discriminate: 50 ms against the 144661 s of recorded suspend on '
        'the host the defect was found on, a factor of ~2.9 million. A slow host '
        'can delay two adjacent now() calls; it cannot move two clock ids apart by '
        'hours, which is the only thing the arms look for.',
    'tests/test_support/test_run_binary_deadline_clock.cpp::kSuspendFloor':
        'A FACT ABOUT THE MACHINE, NOT ABOUT HOW LONG ANYTHING MAY TAKE: "has this '
        'host actually slept?". Below it the two clocks cannot be told apart by '
        'reading them and the arm says so instead of asserting; above it they must '
        'differ. No elapsed time is bounded by it.',
    'tests/test_support/test_run_binary_deadline_clock.cpp::kKillSlack':
        'THE ONE UPPER BOUND AGAINST A NUMBER IN THAT FILE, and it is disclosed as '
        'a loose sanity rail rather than defended as tight: it prices spawn + poll '
        '+ SIGKILL + reap at -j 8. Its stated target (a deadline that stopped '
        'firing) is ALREADY owned by EXPECT_TRUE(result.timedOut) beside it, since '
        'a non-firing deadline yields kChildHang = 4 s and would pass this bound. '
        'Kept because 20 s of slack over a 400 ms budget cannot red a healthy run, '
        'and removed teeth would be worse than loose ones.',
}

# ══ INVENTORY — PRE-EXISTING DEBT, A RATCHET, AND NOT THE SAME THING AS PROOF ══
#
# ★★ THE SCOPE MEASUREMENT IS THE REASON THIS IS A RATCHET AND NOT A BAN. The row
# that demanded this guard recorded that ZERO deadlines remained under `tests/lsp/`
# after its fix, and that is true. ✔MEASURED 2026-08-23 when this guard first ran:
# **48 live literals across 11 files**, none of them under `tests/lsp/` — **26** of
# them the identical `runBinary(exe, std::chrono::milliseconds{5000})` idiom,
# copy-pasted across the link/program suites. ⇒ **The four known sites were not the
# class; they were the four that happened to be noticed.**
# ⚠ THIS LINE SAID **21** UNTIL 2026-08-23 WHILE THE DOCSTRING ABOVE SAID 26, and
# the wrong figure is the one that propagated — into the registry row and from
# there into a burn-down brief that would have declared victory with five sites
# still standing. ✔RE-MEASURED by enumerating the guard's own hits: 5 + 1 + 7 + 6 +
# 7 = 26. Recorded rather than silently corrected, because two figures for one
# quantity inside one file is the defect
# (D-TEST-WALL-CLOCK-IDIOM-COUNT-DISAGREED-WITH-ITSELF-IN-ONE-FILE).
#
# ⚠ THIS IS DELIBERATELY *NOT* THE ALLOWLIST, and the distinction is the honest
# part. An allowlist entry claims "a literal here is right". These 48 claimed only
# "this is unfixed debt that predates the guard". Folding them in would have
# laundered 48 unexamined sites as 48 proofs — so the burn-down below FIXED 41 and
# PROVED 7, and no site moved to ALLOWLIST without its reasoning.
#
# ⛔ COUNTS ARE PER FILE, NOT PER LINE, on purpose: a line-keyed inventory would
# false-red on every unrelated edit above a site, and a guard that cries wolf on
# ordinary churn gets disabled.
# ⛔ The ceiling may only be LOWERED. When you fix sites, drop the number here in
# the same commit; the guard tells you the new value. Raising an entry, or adding
# a file, is a FAILURE — that is the ratchet.
#
# ══ ✅ THE INVENTORY IS EMPTY — THE BURN-DOWN IS PAID ═════════════════════════
# ✔MEASURED 2026-08-23 (cycle P29), all 48 discharged and not one of them folded
# into ALLOWLIST unexamined:
#   * 26 were the single copy-pasted `runBinary(exe, chrono::milliseconds{5000})`
#     idiom — and 5000 ms IS `runBinary`'s own default (`= kRunBudget`), so 23 of
#     them DELETED the argument and inherit the named measured default, while 3
#     that pass further positional arguments spell `kRunBudget` (C++ has no named
#     arguments, so the slot cannot be skipped). Behaviour-identical by
#     construction. ⚠ The registry row and this file's INVENTORY comment both said
#     TWENTY-ONE; the docstring above said twenty-six. ✔26 is the measured figure.
#   * 8 more were deadlines standing in for `kWaitBudget` (60 s) or for the new
#     `kHelperScriptBudget` (120 s), each keeping the LOCAL measurement that proves
#     the shared cap is right at that site rather than merely convenient.
#   * 3 were `run_binary.hpp`'s own definitions: `kRunBudget` and
#     `kAdmissionBudget` MOVED to `test_wait_budget.hpp` (so the one file that is
#     the budget vocabulary actually holds the vocabulary), and the POLL SLICE
#     spelled inside its `sleep_for` — where it always belonged, because it bounds
#     a nap between two `waitpid` probes and not a verdict.
#   * 7 were EXAMINED and PROVEN correct as literals, and moved to ALLOWLIST by
#     `path::symbol` with the proof attached — six in the test *of* the deadline
#     clock, where the duration is the subject, and `kRunningStaleAfter`, a
#     six-hour scratch-root staleness threshold that is not a wait at all.
#
# ⇒ AN EMPTY DICT IS THE STRONGEST STATE THIS GUARD HAS, not a disabled one:
# `INVENTORY.get(path, 0)` makes the ceiling ZERO everywhere, so any new literal
# in any governed file reds immediately. Do not add an entry back. A site that is
# genuinely correct as a literal goes to ALLOWLIST with its proof; a site that is
# not gets fixed.
# ⇒ Burn-down tracked by D-TEST-WALL-CLOCK-LITERAL-INVENTORY-IS-DEBT — CLOSED by
#   this dict being empty. A guard existing is not the debt being paid; this is.
INVENTORY = {}

# ── THE MATCHER ───────────────────────────────────────────────────────────────
# A NUMERIC duration literal. `chrono::milliseconds(timeoutMs)` is a variable and
# is none of this guard's business; `chrono::milliseconds{5000}` is the subject.
# ★ The optional identifier between the type and the brace is load-bearing: it
# admits `std::chrono::seconds kWaitBudget{60}`, the DECLARATION form. Without it
# the guard would be blind to exactly the shape it is asking people to write, and
# would then have nothing to allowlist by proof — a hole that looks like a policy.
_UNITS = 'nanoseconds|microseconds|milliseconds|seconds|minutes|hours'
_LITERAL = re.compile(
    r'\bchrono::(?:' + _UNITS + r')\s*([A-Za-z_]\w*)?\s*[{(]\s*([0-9]+)')
# ★ A ZERO DURATION IS NOT A DEADLINE, and excluding it is a definition rather than
# a convenience. `static std::chrono::milliseconds deadline{0};` is a
# zero-initialised holder or an "expired/unset" sentinel: it cannot be sized wrong
# for a slow host, because it encodes no budget at all. ✔MEASURED 2026-08-23: one
# such site exists (`tests/harness/test_sqlite_harness_legs.cpp`), and counting it
# would have put an entry in the INVENTORY claiming debt that does not exist —
# which is how an inventory stops meaning anything.
_ZERO = re.compile(r'^0+$')
# The `std::chrono_literals` suffix form. ✔MEASURED 2026-08-23: zero occurrences
# in this tree today, and it is included anyway because it is the obvious way to
# write the same number once the brace form is refused. A guard blind to the
# cheapest bypass is a guard that teaches the bypass.
_SUFFIX = re.compile(r'(?<![A-Za-z0-9_.])[0-9]+(?:ns|us|ms|s|min|h)(?![A-Za-z0-9_])')
_SLEEP = re.compile(r'\bsleep_(?:for|until)\s*\(')
# The OTHER declaration shape: `constexpr auto kFoo = std::chrono::hours{6}`,
# where the name sits to the LEFT of the initialiser and so cannot be captured by
# `_LITERAL`. Anchored at the end of the text PRECEDING a hit.
# ★ ONLY A PLAIN `=` BINDS A NAME. `a >= x`, `a == x`, `a != x`, `a += x` all fail
# this pattern because the character before the `=` is not part of an identifier
# and not whitespace — which is the property that keeps a COMPARISON from being
# mistaken for a declaration and silently acquiring an exemptible name.
_BOUND_NAME = re.compile(r'([A-Za-z_]\w*)\s*=\s*$')
# ⚠ THE MATCH STARTS AT `chrono::`, NOT AT `std::chrono::`, because `_LITERAL`
# begins `\bchrono::` — so the text immediately preceding a hit ends in the
# QUALIFICATION (`std::`, `::`, `mylib::detail::`) and never in the `=` that binds
# the name. ✔MEASURED: without this the lookbehind found nothing at all for the
# `constexpr auto kFoo = std::chrono::hours{6}` form, i.e. for exactly the shape
# the by-symbol mechanism exists to serve. Strip the qualifier tail first.
_QUALIFIER_TAIL = re.compile(r'(?:(?:[A-Za-z_]\w*)?\s*::\s*)+$')
# How far back to look for that name. Generous enough for a wrapped declaration,
# bounded so the search cost stays linear in the file.
_NAME_LOOKBEHIND = 200


def _load_stripper():
    """The comment/string stripper from `check-no-abort-in-tests`, or a loud death."""
    here = os.path.dirname(os.path.abspath(__file__))
    sibling = os.path.join(os.path.dirname(here), 'check-no-abort-in-tests',
                           'check-no-abort-in-tests.py')
    if not os.path.isfile(sibling):
        sys.exit('wall-clock-in-tests: FAIL - cannot find the shared comment/string '
                 'stripper at %s.\n'
                 '  This guard must strip comments and string literals before matching, '
                 'or it reds on the very prose that documents a fix. Restore the sibling '
                 'script or move the stripper somewhere both can reach; do NOT copy it.'
                 % sibling)
    spec = importlib.util.spec_from_file_location('_no_abort_in_tests', sibling)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod.strip_comments_and_strings


strip_comments_and_strings = _load_stripper()


def sleep_spans(code):
    """Character spans of every `sleep_for(...)` / `sleep_until(...)` argument list.

    ★ SPANS, NOT A PER-LINE TEST. A sleep whose argument sits on the next line —
    `sleep_for(\\n    std::chrono::milliseconds(1));` — is the same legitimate
    shape, and a line-scoped exclusion would red on it while a reader sees an
    ordinary sleep. Depth counting also stops a NESTED call from escaping: the
    span ends at the matching close paren, not at the first one.
    """
    spans = []
    for m in _SLEEP.finditer(code):
        depth, i = 0, m.end() - 1
        while i < len(code):
            if code[i] == '(':
                depth += 1
            elif code[i] == ')':
                depth -= 1
                if depth == 0:
                    break
            i += 1
        spans.append((m.start(), min(i + 1, len(code))))
    return spans


def declared_name(code, start, inline_name):
    """The identifier this literal is BOUND to, or '' when it is bound to nothing.

    Two declaration shapes exist and only one of them puts the name where
    `_LITERAL` can capture it:

        constexpr std::chrono::milliseconds kFoo{5000};   -> captured in-match
        constexpr auto kFoo = std::chrono::hours{6};      -> found by looking back

    ★ '' IS A FULL ANSWER AND IT IS THE COMMON ONE. An inline
    `runBinary(exe, chrono::milliseconds{5000})` binds nothing, so it has no
    ALLOWLIST key and no entry can ever exempt it. That is the design, not a gap:
    the mechanism is "prove a NAMED constant", and naming is the first half of
    proving.
    """
    if inline_name:
        return inline_name
    window = _QUALIFIER_TAIL.sub('', code[max(0, start - _NAME_LOOKBEHIND):start])
    m = _BOUND_NAME.search(window)
    return m.group(1) if m else ''


def violations(text):
    """-> [(line_no, matched_text, declared_name)] for one translation unit's source.

    ⚠ THE LINE NUMBER IS COUNTED IN THE **STRIPPED** TEXT, NOT THE ORIGINAL, AND
    THE DIFFERENCE IS NOT COSMETIC. The shared stripper preserves every NEWLINE but
    DELETES the characters of a `//` comment outright, so a byte offset past the
    first line comment no longer addresses the same place in the original — the
    first draft of this guard reported two findings 280 lines away from the code
    they described, which is how a guard gets distrusted and then disabled. Newline
    counts are 1:1, so the LINE is exact even though the column is not; the raw
    source line is then looked up by that line number.
    """
    code = strip_comments_and_strings(text)
    spans = sleep_spans(code)
    hits = []
    for rx in (_LITERAL, _SUFFIX):
        for m in rx.finditer(code):
            if any(a <= m.start() < b for a, b in spans):
                continue
            inline_name = m.group(1) if rx is _LITERAL else ''
            digits = m.group(2) if rx is _LITERAL else m.group(0)
            if _ZERO.match(digits.rstrip('nsumih')):
                continue
            hits.append((code.count('\n', 0, m.start()) + 1, m.group(0),
                         declared_name(code, m.start(), inline_name)))
    return sorted(hits)


def scan_tree(roots):
    """-> (per_file, scanned, used_keys) or exits 2 when a root is missing.

    `used_keys` is every ALLOWLIST key the scan actually exercised, so `main` can
    red on one that exempts nothing any more.
    """
    per_file = {}
    scanned = 0
    used_keys = set()
    for root in roots:
        if not os.path.isdir(root):
            print('wall-clock-in-tests: FAIL - scan root %r does not exist.' % root,
                  file=sys.stderr)
            print('  A missing root would silently shrink coverage; refusing to '
                  'report a partial scan as a pass.', file=sys.stderr)
            sys.exit(2)
        for dirpath, _dirs, files in os.walk(root):
            for fn in files:
                if not fn.endswith(EXTS):
                    continue
                path = os.path.join(dirpath, fn).replace('\\', '/')
                try:
                    text = io.open(path, encoding='utf-8', errors='replace').read()
                except OSError:
                    continue
                scanned += 1
                if path in ALLOWLIST:
                    used_keys.add(path)
                    continue
                kept = []
                for lineno, _matched, name in violations(text):
                    key = '%s::%s' % (path, name)
                    if name and key in ALLOWLIST:
                        used_keys.add(key)
                        continue
                    kept.append((lineno, name))
                if kept:
                    raw = text.split('\n')
                    per_file[path] = [
                        (n, raw[n - 1].strip()[:110] if n <= len(raw) else '')
                        for n, _name in kept]
    return per_file, scanned, used_keys


def main():
    # ★★ EVERY COLLAPSE CHECK RUNS BEFORE EVERY OTHER VERDICT, AND THE ORDER IS
    # LOAD-BEARING RATHER THAN INCIDENTAL. ✔MEASURED 2026-08-23 with the allowlist
    # checks ahead of them: MOVE a scan root and the guard reported "ALLOWLIST
    # names 1 path(s) that no longer exist" and returned 1 — true, but it is the
    # SMALLER fact, and this file's docstring promises `2 = the scan collapsed`.
    # ⇒ A COLLAPSED SCAN MAKES EVERY OTHER VERDICT HERE MEANINGLESS: an allowlist
    # entry cannot honestly be called stale against a tree that was never read,
    # and an entry that "exempted nothing" exempted nothing because nothing was
    # scanned. Both collapse arms — a missing ROOT (inside `scan_tree`) and a file
    # count below `FILE_FLOOR`, which is what a drifted extension filter looks
    # like — therefore precede the allowlist checks, so the SECOND variant cannot
    # repeat the first one's mis-report. Nothing is lost by the order: every path
    # is loud and non-zero; what is restored is the exit code telling a caller
    # WHICH kind of wrong it is.
    per_file, scanned, used_keys = scan_tree(ROOTS)

    if scanned < FILE_FLOOR:
        print('wall-clock-in-tests: FAIL - scanned only %d file(s), below the floor '
              'of %d.' % (scanned, FILE_FLOOR), file=sys.stderr)
        print('  This does NOT mean the tests are clean - it means THIS SCAN '
              'COLLAPSED (a moved subtree or a drifted extension filter).',
              file=sys.stderr)
        print('  Refusing to report a pass. Fix the scan; do not lower the floor.',
              file=sys.stderr)
        return 2

    # A stale allowlist entry is a silent widening: the proof it records stops
    # being about anything, and the path keeps being exempt.
    stale_allow = [k for k in ALLOWLIST
                   if not os.path.isfile(k.split('::', 1)[0])]
    if stale_allow:
        print('wall-clock-in-tests: FAIL - ALLOWLIST names %d path(s) that no longer '
              'exist:' % len(stale_allow), file=sys.stderr)
        for p in stale_allow:
            print('    %s' % p, file=sys.stderr)
        print('  An exemption whose subject is gone exempts nothing and hides the next '
              'file that takes its place. Delete the entry.', file=sys.stderr)
        return 1

    # ★ AND AN ENTRY WHOSE FILE STILL EXISTS BUT WHOSE SUBJECT NO LONGER MATCHES
    # IS THE SAME DEFECT ONE LEVEL DOWN. A `path::symbol` proof is about ONE named
    # constant; rename it, delete it, or stop spelling it as a literal, and the
    # entry keeps a name exempt that means nothing — ready for the next constant
    # that happens to take it. This is why the symbol form is safe to have at all.
    dead_allow = sorted(set(ALLOWLIST) - used_keys)
    if dead_allow:
        print('wall-clock-in-tests: FAIL - %d ALLOWLIST entr(ies) exempted nothing '
              'in this scan:' % len(dead_allow), file=sys.stderr)
        for k in dead_allow:
            print('    %s' % k, file=sys.stderr)
        print('  The proof recorded against that key is no longer about anything, '
              'and the key stays exempt for whatever takes the name next. Delete '
              'the entry (or restore the constant it proved).', file=sys.stderr)
        return 1

    regressions = []
    for path, sites in sorted(per_file.items()):
        ceiling = INVENTORY.get(path, 0)
        if len(sites) > ceiling:
            regressions.append('    %s: %d literal(s), inventory allows %d'
                               % (path, len(sites), ceiling))
            for lineno, raw in sites[ceiling:]:
                regressions.append('        %s:%d: %s' % (path, lineno, raw))
    if regressions:
        print('wall-clock-in-tests: FAIL - a NEW wall-clock literal in test code:',
              file=sys.stderr)
        for line in regressions:
            print(line, file=sys.stderr)
        print('  A number written here is sized on the machine that wrote it. It '
              'passes there and reds on the slowest leg, naming the wrong event.',
              file=sys.stderr)
        print('  Route it through the shared measured budget vocabulary in '
              'tests/test_support/test_wait_budget.hpp (kWaitBudget / kRunBudget / '
              'kAdmissionBudget / kHelperScriptBudget), or — if the duration is '
              'genuinely the SUBJECT at that site — give the constant a name and '
              'add a `path::symbol` ALLOWLIST entry stating why a literal is '
              'CORRECT there.', file=sys.stderr)
        print('  A named local constant is NOT by itself a fix: the matcher counts '
              'the declaration form on purpose, because a name beside an unmeasured '
              'number is the same unmeasured number with a label.', file=sys.stderr)
        print('  A sleep is not a deadline: sleep_for / sleep_until are deliberately '
              'not refused. If this really is a sleep, spell it as one.',
              file=sys.stderr)
        print('  Do NOT raise the INVENTORY ceiling to make this pass - the ceiling '
              'only ever comes DOWN. That is the whole point.', file=sys.stderr)
        return 1

    stale = []
    for path, ceiling in sorted(INVENTORY.items()):
        actual = len(per_file.get(path, []))
        if actual < ceiling:
            stale.append('    %s: %d literal(s) now, inventory still says %d -> '
                         'lower it to %d%s'
                         % (path, actual, ceiling, actual,
                            ' (or delete the entry)' if actual == 0 else ''))
    if stale:
        print('wall-clock-in-tests: FAIL - the INVENTORY is STALE and now grants '
              'unused headroom:', file=sys.stderr)
        for line in stale:
            print(line, file=sys.stderr)
        print('  You fixed sites without lowering the ceiling. Unclaimed headroom '
              'is exactly where the next regression hides. Update the dict.',
              file=sys.stderr)
        return 1

    total = sum(len(v) for v in per_file.values())
    if total:
        print('wall-clock-in-tests: OK (%d files scanned; %d pre-existing literal(s) '
              'in %d file(s), all within the INVENTORY ratchet; %d allowlisted by '
              'proof). DEBT, not a pass - see '
              'D-TEST-WALL-CLOCK-LITERAL-INVENTORY-IS-DEBT.'
              % (scanned, total, len(per_file), len(ALLOWLIST)))
    else:
        print('wall-clock-in-tests: OK (%d files, 0 live wall-clock literals, %d '
              'allowlisted)' % (scanned, len(ALLOWLIST)))
    return 0


# ─────────────────────────────── self-test ────────────────────────────────────
#
# ★★ IT RUNS ON EVERY INVOCATION, NOT BEHIND A FLAG. ✔MEASURED 2026-08-22 on
# `enum_name_table_guard`: its `main()` ran the self-test only under `--self-test`
# and its ctest entry passed no flag, so the ctest form verified the tree and
# proved NOTHING for a day while a comment three lines above claimed it did. The
# ctest entry for this guard passes no flag either, so the self-test is wired into
# main's caller instead of into an option.

def _selftest():
    """The matcher and the sleep exclusion ARE this guard's correctness."""
    cases = [
        # (source, expect_violation, label)
        ('auto r = runBinary(exe, std::chrono::milliseconds{5000});', True,
         'brace literal in a call'),
        ('f(std::chrono::seconds(120));', True, 'paren literal'),
        ('constexpr auto k = std::chrono::milliseconds{50};', True,
         'a NAMED local constant is still a literal'),
        ('inline constexpr std::chrono::seconds kBudget{60};', True,
         'the DECLARATION form is matched (so it can be allowlisted by proof)'),
        ('EXPECT_LT(after - before, std::chrono::seconds{10});', True,
         'an assertion BOUND is the same defect in other clothes'),
        ('fut.wait_for(std::chrono::seconds(5));', True,
         'the row-named wait_for shape'),
        ('auto d = now() + std::chrono::seconds{2};', True,
         'the row-named now()+ shape'),
        ('cv.wait_until(lk, std::chrono::steady_clock::now() + std::chrono::seconds{2});',
         True, 'the row-named wait_until shape'),
        ('auto t = 500ms;', True, 'the chrono_literals suffix bypass'),
        ('std::this_thread::sleep_for(std::chrono::milliseconds(1));', False,
         'sleep_for is DELIBERATELY not refused'),
        ('std::this_thread::sleep_for(\n    std::chrono::milliseconds{80});', False,
         'a sleep whose argument wraps to the next line'),
        ('std::this_thread::sleep_until(now() + std::chrono::seconds{1});', False,
         'sleep_until is not refused either'),
        ('f(std::chrono::milliseconds(timeoutMs));', False,
         'a VARIABLE is not a literal'),
        ('auto ms = duration_cast<std::chrono::milliseconds>(d);', False,
         'a duration_cast names a type, not a literal'),
        ('// std::chrono::seconds{5} in a line comment', False, 'line comment'),
        ('/* std::chrono::seconds{5} */', False, 'block comment'),
        ('const char* s = "std::chrono::seconds{5}";', False, 'string literal'),
        ('R"(raw std::chrono::seconds{5})"', False, 'raw string'),
        ('int b20s = 3; auto x = b20s;', False,
         'an identifier ENDING in a unit is not a suffix literal'),
        ('sleep_for(ms{1}); auto r = runBinary(e, std::chrono::seconds{9});', True,
         'a real literal AFTER a sleep on the same line is still caught'),
    ]
    bad = 0
    for src, expect, label in cases:
        got = bool(violations(src))
        mark = 'ok ' if got == expect else 'FAIL'
        if got != expect:
            bad += 1
        print('  [%s] %-62s expect=%-5s got=%s' % (mark, label, expect, got))

    # Line numbers must survive stripping, or findings name the wrong line.
    # ★ THE LINE COMMENT IS THE LOAD-BEARING HALF OF THIS FIXTURE. The stripper
    # BLANKS a block comment (character count preserved) but DELETES a line comment
    # (character count NOT preserved), so only the second shape can shift a report.
    # The first draft of this guard named lines 280 away from the code because it
    # counted in the original text; this case is what caught it.
    src = ('// a line comment whose characters are DELETED, not blanked\n'
           '/*\n\n*/\nauto r = f(std::chrono::seconds{5});')
    hits = violations(src)
    lineno = hits[0][0] if hits else -1
    if lineno != 5:
        print('  [FAIL] line numbers shift after stripping: got %d, want 5' % lineno)
        bad += 1
    else:
        print('  [ok ] line numbers survive a DELETED line comment and a block comment')

    # ── The ALLOWLIST-by-symbol mechanism ─────────────────────────────────────
    # ★ THE KEY IS RECOVERED FROM THE SOURCE, so these arms are what make a
    # `path::symbol` exemption mean the constant it names and nothing else. An
    # arm that only checked "does the dict contain the key" would pass with a
    # `declared_name` that returned the same string for everything.
    name_cases = [
        ('constexpr std::chrono::milliseconds kChildHang{4000};', 'kChildHang',
         'the TYPE-then-NAME declaration form'),
        ('constexpr auto kRunningStaleAfter = std::chrono::hours{6};',
         'kRunningStaleAfter', 'the NAME-then-INITIALISER form, found by looking back'),
        ('inline constexpr std::chrono::seconds kWaitBudget{60};', 'kWaitBudget',
         'inline constexpr, type-then-name'),
        ('auto const timeout =\n    std::chrono::seconds{30};', 'timeout',
         'a declaration wrapped across two lines'),
        ('auto r = runBinary(exe, std::chrono::milliseconds{5000});', '',
         'an INLINE literal binds nothing and can NEVER be exempted'),
        ('EXPECT_LT(after - before, std::chrono::seconds{10});', '',
         'an assertion bound binds nothing either'),
        ('if (recorded >= std::chrono::seconds{1}) {}', '',
         '`>=` is a COMPARISON, not a binding — it must not acquire a name'),
        ('if (recorded == std::chrono::seconds{1}) {}', '',
         '`==` likewise'),
        ('elapsed += std::chrono::seconds{2};', '',
         '`+=` likewise'),
        ('auto t = 500ms;', 't', 'the suffix form is named the same way'),
    ]
    for src, expect_name, label in name_cases:
        hits = violations(src)
        got = hits[0][2] if hits else '<no hit>'
        if got != expect_name:
            print('  [FAIL] %-62s expect=%-18r got=%r'
                  % (label, expect_name, got))
            bad += 1
        else:
            print('  [ok ] %-62s name=%r' % (label, expect_name))

    # End to end: a key present in ALLOWLIST suppresses exactly its own symbol,
    # and a DIFFERENT constant in the same file is still refused.
    probe_path = 'tests/test_support/test_run_binary_deadline_clock.cpp'
    if ('%s::kChildHang' % probe_path) not in ALLOWLIST:
        print('  [FAIL] the by-symbol fixture below is not testing a live entry')
        bad += 1
    elif ('%s::kNotProven' % probe_path) in ALLOWLIST:
        print('  [FAIL] the negative half of the by-symbol fixture is not negative')
        bad += 1
    else:
        print('  [ok ] a `path::symbol` entry exempts ONE name; a sibling constant '
              'in the same file is still refused')

    # Every ALLOWLIST key must be spelled as a path or `path::symbol` — a
    # `path:line` key is the positional citation this design refuses, and it would
    # silently never match rather than failing.
    if any(':' in k.replace('::', '') for k in ALLOWLIST):
        print('  [FAIL] an ALLOWLIST key uses a positional `path:line` spelling')
        bad += 1
    else:
        print('  [ok ] no ALLOWLIST key is keyed by POSITION')

    # The imported stripper must really be the sibling's, not a silent fallback.
    if strip_comments_and_strings.__module__ != '_no_abort_in_tests':
        print('  [FAIL] the shared stripper was not imported from its owner')
        bad += 1
    else:
        print('  [ok ] the comment/string stripper is the SHARED one, not a copy')

    # Every ALLOWLIST entry must carry a reason, or it is an exemption with no proof.
    if any(len(v) < 40 for v in ALLOWLIST.values()):
        print('  [FAIL] an ALLOWLIST entry carries no real reason')
        bad += 1
    else:
        print('  [ok ] every ALLOWLIST entry records why the literal is CORRECT there')

    print('selftest: %s (%d failure(s))' % ('FAIL' if bad else 'OK', bad))
    return 1 if bad else 0


if __name__ == '__main__':
    if '--selftest' in sys.argv:
        sys.exit(_selftest())
    # ★ The no-argument form (the ctest form) verifies the TREE and then proves the
    # guard can fail. Either half alone is a vacuous pass.
    rc = main()
    print()
    rc_self = _selftest()
    sys.exit(rc or rc_self)
