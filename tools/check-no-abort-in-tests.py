#!/usr/bin/env python3
"""Refuse a live `abort()` call site in test-support / test code.

D-TEST-ABORT-IN-A-FIXTURE-HAS-NO-GUARD — the guard this row exists to demand.

WHY THIS EXISTS, and it is a measured recurrence rather than a style rule.
`std::abort()` in a test fixture kills the whole test PROCESS, so every sibling
test in that executable loses its verdict and the harness cannot report which
unit failed. It has landed TWICE in this repo:

  * `tests/test_support/repo_root.hpp` — the walkers aborted on a miss; the
    symptom reached the operator as a bare "Subprocess aborted".
  * `tests/analysis/semantic/semantic_test_fixture.hpp` (2026-08-17) — a
    config-mutating pin drove `loadShipped` to a LEGITIMATE refusal and the
    binary died with 0xc0000409 mid-suite, taking NINE passing tests' results
    with it and reporting an exception code instead of the load error.

Both were fixed by hand. The class was never gated, which is why it recurred:
`repo_root.hpp` carries a clear, correct explanation of exactly this fault, and
the fixture one directory over aborted anyway. ⇒ NOTHING MAKES A CALL SITE READ
A NEIGHBOUR'S COMMENT. A comment is not a guard.

★★ THE MATCHER MUST NOT BE A BARE TOKEN GREP, and that is the sharpest
constraint here. Both textual occurrences of `std::abort` in the FIXED fixture
are inside its explanatory comment — so a token grep reds on the very file that
documents the fix. That is the same failure shape as
D-GATE-ANCHOR-CITATION-RESOLVES-VIA-ITS-OWN-BUG-REPORT: a marker that is merely
PRESENT can always be tripped by writing about it. This strips comments and
string literals FIRST and matches only what the compiler would actually call.

FAIL-CLOSED, like every check in this battery: an empty scan is a COLLAPSE, not
a pass. A guard that reports success while scanning nothing is the worst defect
a guard can have, and this repo has already shipped that bug once
(D-GATE-ANCHOR-GUARD-FAILS-OPEN-ON-MISSING-ROOT).

Exit codes: 0 = clean, 1 = a live abort call site, 2 = the scan collapsed.
"""
import io
import os
import re
import sys

ROOTS = ('tests', 'integrated_tests')
EXTS = ('.cpp', '.hpp', '.h', '.cc')
# Floor: far below the live figure so ordinary churn never trips it. This catches
# a COLLAPSED scan (a moved subtree, a drifted extension filter), not drift.
FILE_FLOOR = 200

# ALLOWLIST is BY PROOF: a site earns a place here only with a recorded reason
# why aborting the whole test binary is the CORRECT behaviour there. It is empty,
# and that is a measured claim rather than an aspiration.
ALLOWLIST: dict[str, str] = {}

# ══ INVENTORY — PRE-EXISTING DEBT, A RATCHET, AND NOT THE SAME THING AS PROOF ══
#
# ★★★ THE SCOPE MEASUREMENT IS THE REASON THIS EXISTS. The row that demanded this
# guard named TWO occurrences. ✔MEASURED when the guard first ran: **61 live
# call sites across 29 files** — the identical
# `ADD_FAILURE() << "loadShipped(...) failed"; std::abort();` idiom, copy-pasted
# across the whole test tree. ⇒ The two known cases were not the class; they were
# the two that happened to be noticed. **A defect found twice by hand is evidence
# of a population, not of a pair.**
#
# ⚠ THIS IS DELIBERATELY *NOT* THE ALLOWLIST, and the distinction is the honest
# part. An allowlist entry claims "aborting here is right". These 61 claim only
# "this is unfixed debt that predates the guard". Folding them into ALLOWLIST
# would launder 61 unexamined sites as 61 proofs — the rubber-stamp this project's
# bar exists to refuse.
#
# ★★ WHY A RATCHET RATHER THAN WAITING FOR ZERO: the guard's whole purpose is to
# stop the NEXT occurrence, and the two known ones landed years apart precisely
# because nothing was watching. Gating new sites now is worth more than gating
# nothing until a 61-site sweep can be scheduled. Precedent inside this repo: the
# anchor-balance gate is exactly this shape — a count that may not grow.
#
# ⛔ COUNTS ARE PER FILE, NOT PER LINE, ON PURPose: a line-keyed inventory would
# false-red on every unrelated edit above a site, and a guard that cries wolf on
# ordinary churn gets disabled. Per-file counts also stop the sideways move of
# deleting a site in one file and adding one in another.
# ⛔ The ceiling may only be LOWERED. When you fix sites, drop the number here in
# the same commit; the guard tells you the new value. Raising an entry, or adding
# a file, is a FAILURE — that is the ratchet.
# ⇒ Burn-down tracked by D-TEST-ABORT-IN-A-FIXTURE-HAS-NO-GUARD, which stays OPEN
#   until this dict is empty. A guard existing is not the debt being paid.
INVENTORY: dict[str, int] = {
    'tests/analysis/compilation_unit/test_compilation_unit.cpp': 2,
    'tests/analysis/compilation_unit/toy_cu_fixture.hpp': 1,
    'tests/analysis/preprocess/test_preprocessor.cpp': 1,
    'tests/analysis/semantic/test_declarator_engine.cpp': 1,
    'tests/analysis/semantic/test_fc3_width_semantics.cpp': 1,
    'tests/analysis/semantic/test_semantic_analyzer_genericity.cpp': 8,
    'tests/analysis/syntactic/test_corpus.cpp': 1,
    'tests/analysis/syntactic/test_parser_commit_polarity.cpp': 1,
    'tests/analysis/syntactic/test_parser_recovery.cpp': 2,
    'tests/asm/test_asm_arm64_tls.cpp': 1,
    'tests/asm/test_asm_x86_tls.cpp': 1,
    'tests/asm/test_asm_x86_variable.cpp': 1,
    'tests/core/e2e_harness.hpp': 1,
    'tests/core/test_diagnostic_budget_threading.cpp': 1,
    'tests/hir/test_hir_lowering_c_subset.cpp': 11,
    'tests/hir/test_hir_lowering_multi_lang.cpp': 1,
    'tests/hir/test_hir_lowering_toy.cpp': 2,
    'tests/hir/test_hir_lowering_tsql.cpp': 2,
    'tests/hir/test_hir_text.cpp': 1,
    'tests/lir/lowered_lir_fixture.hpp': 3,
    'tests/lir/test_lir.cpp': 2,
    'tests/lir/test_lir_pass_util.cpp': 2,
    'tests/lir/test_lir_text.cpp': 1,
    'tests/lir/test_lir_verifier.cpp': 2,
    'tests/lir/test_mir_to_lir.cpp': 2,
    'tests/mir/test_mir.cpp': 2,
    'tests/mir/test_mir_lowering_c_subset.cpp': 5,
    'tests/opt/test_prune_unreachable.cpp': 1,
    'tests/test_support/run_binary.hpp': 1,
}

_ABORT = re.compile(r'\b(?:std::)?abort\s*\(')


def strip_comments_and_strings(text: str) -> str:
    """Blank out //, /* */, "..." and '...' so only real code remains.

    Newlines are PRESERVED so reported line numbers stay true — a guard that
    names the wrong line sends the reader hunting and gets distrusted.
    """
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        nxt = text[i + 1] if i + 1 < n else ''
        if c == '/' and nxt == '/':
            while i < n and text[i] != '\n':
                i += 1
        elif c == '/' and nxt == '*':
            i += 2
            while i < n and not (text[i] == '*' and i + 1 < n and text[i + 1] == '/'):
                out.append('\n' if text[i] == '\n' else ' ')
                i += 1
            i += 2
        elif c in '"\'':
            quote = c
            # Raw string literals R"delim( ... )delim" — the payload can contain
            # anything, including `abort(`, so it must be skipped wholesale.
            if quote == '"' and out and out[-1] == 'R':
                m = re.match(r'"([^(]{0,16})\(', text[i:])
                if m:
                    close = ')' + m.group(1) + '"'
                    end = text.find(close, i + m.end())
                    end = n if end < 0 else end + len(close)
                    for ch in text[i:end]:
                        out.append('\n' if ch == '\n' else ' ')
                    i = end
                    continue
            i += 1
            while i < n and text[i] != quote:
                if text[i] == '\\':
                    i += 1
                i += 1
            i += 1
        else:
            out.append(c)
            i += 1
    return ''.join(out)


def main() -> int:
    scanned = 0
    findings: list[tuple[str, int, str]] = []
    per_file: dict[str, list[tuple[int, str]]] = {}
    for root in ROOTS:
        if not os.path.isdir(root):
            print(f'no-abort-in-tests: FAIL - scan root {root!r} does not exist.',
                  file=sys.stderr)
            print('  A missing root would silently shrink coverage; refusing to '
                  'report a partial scan as a pass.', file=sys.stderr)
            return 2
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
                code = strip_comments_and_strings(text)
                for lineno, line in enumerate(code.split('\n'), 1):
                    if _ABORT.search(line):
                        key = f'{path}:{lineno}'
                        if key in ALLOWLIST:
                            continue
                        raw = text.split('\n')[lineno - 1].strip()
                        findings.append((path, lineno, raw[:120]))
                        per_file.setdefault(path, []).append((lineno, raw[:120]))

    if scanned < FILE_FLOOR:
        print(f'no-abort-in-tests: FAIL - scanned only {scanned} file(s), below '
              f'the floor of {FILE_FLOOR}.', file=sys.stderr)
        print('  This does NOT mean the tests are clean - it means THIS SCAN '
              'COLLAPSED (a moved subtree or a drifted extension filter).',
              file=sys.stderr)
        print('  Refusing to report a pass. Fix the scan; do not lower the floor.',
              file=sys.stderr)
        return 2

    # ── THE RATCHET. New file with a site, or an existing file that GREW, fails.
    regressions: list[str] = []
    for path, sites in sorted(per_file.items()):
        ceiling = INVENTORY.get(path, 0)
        if len(sites) > ceiling:
            where = ', '.join(f'line {n}' for n, _ in sites[ceiling:])
            regressions.append(
                f'    {path}: {len(sites)} site(s), inventory allows {ceiling}'
                f'  (new: {where})')
            for lineno, raw in sites[ceiling:]:
                regressions.append(f'        {path}:{lineno}: {raw}')

    if regressions:
        print('no-abort-in-tests: FAIL - a NEW abort() call site in test code:',
              file=sys.stderr)
        for line in regressions:
            print(line, file=sys.stderr)
        print('  abort() kills the whole test PROCESS: every sibling test in that',
              file=sys.stderr)
        print('  executable loses its verdict and the harness cannot report which',
              file=sys.stderr)
        print('  unit failed. THROW instead - GoogleTest reports a throw as a',
              file=sys.stderr)
        print('  failure of that ONE test. See tests/test_support/repo_root.hpp.',
              file=sys.stderr)
        print('  Do NOT raise the INVENTORY ceiling to make this pass - the',
              file=sys.stderr)
        print('  ceiling only ever comes DOWN. That is the whole point.',
              file=sys.stderr)
        return 1

    # A file that IMPROVED must lower its ceiling in the same commit, or the
    # ratchet silently loosens: the gap becomes free headroom for a future
    # regression to hide in. Same reasoning as the anchor-balance gate refusing
    # to be widened to fit a cycle.
    stale = []
    for path, ceiling in sorted(INVENTORY.items()):
        actual = len(per_file.get(path, []))
        if actual < ceiling:
            stale.append(f'    {path}: {actual} site(s) now, inventory still '
                         f'says {ceiling} -> lower it to {actual}'
                         + (' (or delete the entry)' if actual == 0 else ''))
    if stale:
        print('no-abort-in-tests: FAIL - the INVENTORY is STALE and now grants '
              'unused headroom:', file=sys.stderr)
        for line in stale:
            print(line, file=sys.stderr)
        print('  You fixed sites without lowering the ceiling. Unclaimed headroom',
              file=sys.stderr)
        print('  is exactly where the next regression hides. Update the dict.',
              file=sys.stderr)
        return 1

    total = sum(len(v) for v in per_file.values())
    if total:
        print(f'no-abort-in-tests: OK ({scanned} files scanned; {total} '
              f'pre-existing site(s) in {len(per_file)} file(s), all within the '
              f'INVENTORY ratchet; {len(ALLOWLIST)} allowlisted by proof). '
              f'DEBT, not a pass - see D-TEST-ABORT-IN-A-FIXTURE-HAS-NO-GUARD.')
    else:
        print(f'no-abort-in-tests: OK ({scanned} files, 0 live abort() call '
              f'sites, {len(ALLOWLIST)} allowlisted)')
    return 0


def _selftest() -> int:
    """The comment/string stripper is the whole correctness of this guard."""
    cases = [
        ('// std::abort() in a line comment',                 False, 'line comment'),
        ('/* std::abort() in a block comment */',             False, 'block comment'),
        ('/*\n * std::abort()\n */',                          False, 'multiline block'),
        ('const char* s = "std::abort()";',                   False, 'string literal'),
        ('auto c = \'x\'; // abort()',                        False, 'char + comment'),
        ('R"(raw std::abort() payload)"',                     False, 'raw string'),
        ('R"delim(std::abort())delim"',                       False, 'raw w/ delim'),
        ('  std::abort();',                                   True,  'real std::abort'),
        ('  abort();',                                        True,  'real bare abort'),
        # ★ This case was first written expecting False, and the self-test caught
        # the EXPECTATION, not the matcher: `std :: abort ()` is a real call and
        # must be caught. `\babort\s*\(` matches it via the optional-`std::` arm.
        # Corrected the test, NOT the pattern — a guard weakened every time it
        # fires asserts nothing.
        ('  std :: abort ();',                                True,  'spaced-out ns (still caught)'),
        ('if (x) { abort(); }',                               True,  'inline real call'),
        ('// comment\nstd::abort();',                         True,  'comment then real'),
        ('/* c */ abort(); // trailing',                      True,  'block, real, trailing'),
        ('std::string k = "a"; abort();',                     True,  'string then real'),
    ]
    bad = 0
    for src, expect, label in cases:
        got = bool(_ABORT.search(strip_comments_and_strings(src)))
        mark = 'ok ' if got == expect else 'FAIL'
        if got != expect:
            bad += 1
        print(f'  [{mark}] {label:<30} expect={expect!s:<5} got={got}')
    # Line numbers must survive stripping, or findings name the wrong line.
    stripped = strip_comments_and_strings('/*\n\n*/\nabort();')
    lineno = next(i for i, ln in enumerate(stripped.split('\n'), 1)
                  if _ABORT.search(ln))
    if lineno != 4:
        print(f'  [FAIL] line numbers shift after stripping: got {lineno}, want 4')
        bad += 1
    else:
        print('  [ok ] line numbers preserved across a multiline block comment')
    print(f'selftest: {"FAIL" if bad else "OK"} ({bad} failure(s))')
    return 1 if bad else 0


if __name__ == '__main__':
    if '--selftest' in sys.argv:
        sys.exit(_selftest())
    sys.exit(main())
