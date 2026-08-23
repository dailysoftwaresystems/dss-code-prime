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

Exit codes: 0 = clean, 1 = a new literal / a stale inventory, 2 = the scan collapsed.
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

# ALLOWLIST is BY PROOF and is keyed by PATH, not by `path:line`. A line key is a
# positional citation: it silently points at unrelated code the moment anything
# above it moves, which is the defect `check-plan-citations` exists to stop one
# level up. A path key is stable, and here it is also the HONEST key -- the claim
# is about what the whole file IS.
ALLOWLIST = {
    'tests/test_support/test_wait_budget.hpp':
        'THIS FILE IS THE SHARED BUDGET. It exists to define one measured '
        'wall-clock cap for every test that waits, and its header carries the '
        'measurement that sized it (622 ms idle / 1912 ms under 3x contention / '
        'CI crossed a 2 s deadline at 2204 ms). A literal here is the thing every '
        'other literal is supposed to be replaced BY, so refusing it would refuse '
        'the fix.',
}

# ══ INVENTORY — PRE-EXISTING DEBT, A RATCHET, AND NOT THE SAME THING AS PROOF ══
#
# ★★ THE SCOPE MEASUREMENT IS THE REASON THIS IS A RATCHET AND NOT A BAN. The row
# that demanded this guard recorded that ZERO deadlines remained under `tests/lsp/`
# after its fix, and that is true. ✔MEASURED 2026-08-23 when this guard first ran:
# **48 live literals across 11 files**, none of them under `tests/lsp/` — 21 of
# them the identical `runBinary(exe, std::chrono::milliseconds{5000})` idiom,
# copy-pasted across the link/program suites. ⇒ **The four known sites were not the
# class; they were the four that happened to be noticed.**
#
# ⚠ THIS IS DELIBERATELY *NOT* THE ALLOWLIST, and the distinction is the honest
# part. An allowlist entry claims "a literal here is right". These 48 claim only
# "this is unfixed debt that predates the guard". Folding them in would launder 48
# unexamined sites as 48 proofs.
#
# ⛔ COUNTS ARE PER FILE, NOT PER LINE, on purpose: a line-keyed inventory would
# false-red on every unrelated edit above a site, and a guard that cries wolf on
# ordinary churn gets disabled.
# ⛔ The ceiling may only be LOWERED. When you fix sites, drop the number here in
# the same commit; the guard tells you the new value. Raising an entry, or adding
# a file, is a FAILURE — that is the ratchet.
# ⇒ Burn-down tracked by D-TEST-WALL-CLOCK-LITERAL-INVENTORY-IS-DEBT, which stays
#   OPEN until this dict is empty. A guard existing is not the debt being paid.
INVENTORY = {
    'integrated_tests/runner.cpp': 2,
    'tests/core/substrate/test_process_spawn.cpp': 1,
    'tests/harness/test_sqlite_harness_legs.cpp': 3,
    'tests/link/test_coff_object_reader.cpp': 5,
    'tests/link/test_lk10_entry_slice_c.cpp': 1,
    'tests/program/test_ffi_resolve_library.cpp': 7,
    'tests/program/test_project_config.cpp': 6,
    'tests/program/test_static_link.cpp': 7,
    'tests/test_support/run_binary.hpp': 3,
    'tests/test_support/test_run_binary_capture.cpp': 3,
    'tests/test_support/test_run_binary_deadline_clock.cpp': 10,
}

# ── THE MATCHER ───────────────────────────────────────────────────────────────
# A NUMERIC duration literal. `chrono::milliseconds(timeoutMs)` is a variable and
# is none of this guard's business; `chrono::milliseconds{5000}` is the subject.
# ★ The optional identifier between the type and the brace is load-bearing: it
# admits `std::chrono::seconds kWaitBudget{60}`, the DECLARATION form. Without it
# the guard would be blind to exactly the shape it is asking people to write, and
# would then have nothing to allowlist by proof — a hole that looks like a policy.
_UNITS = 'nanoseconds|microseconds|milliseconds|seconds|minutes|hours'
_LITERAL = re.compile(
    r'\bchrono::(?:' + _UNITS + r')\s*(?:[A-Za-z_]\w*\s*)?[{(]\s*([0-9]+)')
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


def violations(text):
    """-> [(line_no, matched_text)] for one translation unit's source.

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
            digits = m.group(1) if rx is _LITERAL else m.group(0)
            if _ZERO.match(digits.rstrip('nsumih')):
                continue
            hits.append((code.count('\n', 0, m.start()) + 1, m.group(0)))
    return sorted(hits)


def scan_tree(roots):
    """-> (per_file, scanned) or exits 2 when a root is missing."""
    per_file = {}
    scanned = 0
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
                    continue
                hits = violations(text)
                if hits:
                    raw = text.split('\n')
                    per_file[path] = [(n, raw[n - 1].strip()[:110] if n <= len(raw) else '')
                                      for n, _ in hits]
    return per_file, scanned


def main():
    # A stale allowlist entry is a silent widening: the proof it records stops
    # being about anything, and the path keeps being exempt.
    stale_allow = [p for p in ALLOWLIST if not os.path.isfile(p)]
    if stale_allow:
        print('wall-clock-in-tests: FAIL - ALLOWLIST names %d path(s) that no longer '
              'exist:' % len(stale_allow), file=sys.stderr)
        for p in stale_allow:
            print('    %s' % p, file=sys.stderr)
        print('  An exemption whose subject is gone exempts nothing and hides the next '
              'file that takes its place. Delete the entry.', file=sys.stderr)
        return 1

    per_file, scanned = scan_tree(ROOTS)

    if scanned < FILE_FLOOR:
        print('wall-clock-in-tests: FAIL - scanned only %d file(s), below the floor '
              'of %d.' % (scanned, FILE_FLOOR), file=sys.stderr)
        print('  This does NOT mean the tests are clean - it means THIS SCAN '
              'COLLAPSED (a moved subtree or a drifted extension filter).',
              file=sys.stderr)
        print('  Refusing to report a pass. Fix the scan; do not lower the floor.',
              file=sys.stderr)
        return 2

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
        print('  Route it through the shared measured budget in '
              'tests/test_support/test_wait_budget.hpp, or add a named budget '
              'beside it carrying the MEASUREMENT that sized it.', file=sys.stderr)
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
