#!/usr/bin/env python3
# PURPOSE: refuse a new live `abort()` call site in test or test-support code.
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

★★ THE NO-ARGUMENT FORM (the ctest form) VERIFIES THE TREE **AND THEN RUNS THE
SELF-TEST**, honouring both statuses. Until 2026-08-23 `--selftest` was a separate
branch and the registered entry passes no flag, so the matcher — which is "the
whole correctness of this guard", by its own self-test's docstring — was proven by
nothing mechanical. Same shape as D-GATE-ENUM-NAME-TABLE-CTEST-FORM-NEVER-SELF-TESTED,
and the two halves run UNCONDITIONALLY so a red tree cannot short-circuit the proof
that the instrument still works.

Exit codes: 0 = clean, 1 = a live abort call site, 2 = the scan collapsed.
"""
import io
import os
import re
import shutil
import subprocess
import sys
import tempfile

# ── OUTPUT ENCODING — NOT COSMETIC, AND THE STREAM IS HALF THE FACT ─────────────
# ✔MEASURED 2026-08-23 (CPython 3.14.3, Windows, BOTH streams PIPES, which is
# exactly how ctest runs every guard): `sys.stdout` comes up
# `encoding='cp1252' errors='surrogateescape'` and `sys.stderr` comes up
# `errors='backslashreplace'`. `surrogateescape` rescues only lone surrogates left
# by an earlier decode; it does NOTHING for an ordinary unencodable character.
# ⇒ This guard prints the RAW SOURCE LINE of every finding, and the test tree it
# scans is full of box-drawing and typographic characters. It prints them on
# STDERR, so it does not die — but `backslashreplace` mangles the evidence into an
# escape, and a report that renders the offending line differently from the file
# it names is an instrument arguing with itself. The same text on STDOUT would
# raise `UnicodeEncodeError` and kill the guard inside its own report.
# Applied at IMPORT, so every path this module can print on is covered.
# D-GATE-PYTHON-GUARD-DIES-PRINTING-TREE-TEXT-ON-A-WINDOWS-PIPE
for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding='utf-8', errors='replace')
    except (AttributeError, ValueError, OSError):   # pragma: no cover - odd stream
        pass

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
# ⇣ BURN-DOWN, 2026-08-24 (P32 lane b, closing
#   D-TEST-A-TORN-SHIPPED-CONFIG-CRASHES-A-SUITE-INSTEAD-OF-REDDING-IT):
#   the whole of `tests/hir/**` came OFF this list -- 17 sites across 5 files,
#   every one the same `ADD_FAILURE() << "loadShipped(...) failed";
#   std::abort();` idiom or its golden-reader twin. They now THROW, through
#   `tests/test_support/shipped_schema_or_throw.hpp`, and the load failure
#   arrives as a named GoogleTest failure of the ONE running case instead of a
#   0xC0000409 that unwinds nothing. 61 -> 44 sites, 29 -> 24 files.
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
    'tests/lir/lowered_lir_fixture.hpp': 3,
    'tests/lir/test_lir.cpp': 2,
    'tests/lir/test_lir_pass_util.cpp': 2,
    'tests/lir/test_lir_text.cpp': 1,
    'tests/lir/test_lir_verifier.cpp': 2,
    'tests/lir/test_mir_to_lir.cpp': 2,
    'tests/mir/test_mir.cpp': 2,
    'tests/mir/test_mir_lowering_c.cpp': 5,
    'tests/opt/test_prune_unreachable.cpp': 1,
    'tests/test_support/run_binary.hpp': 1,
}

_ABORT = re.compile(r'\b(?:std::)?abort\s*\(')


def _is_digit_separator(out: list, text: str, i: int) -> bool:
    """Is the `'` at text[i] a C++14 DIGIT SEPARATOR rather than a char-literal quote?

    ★ THE TEST IS "WHAT TOKEN AM I INSIDE", NOT "WHAT CHARACTER IS NEXT TO ME", and
    the difference is the whole correctness of it. Walk BACK over the emitted code
    across the token characters a numeric literal can contain -- digits, hex letters,
    an exponent's letters, and earlier separators -- and ask what that token STARTS
    with. A run starting with a DIGIT is a numeric literal, so the quote separates
    digits. Anything else is a char literal.

    ⚠ THE NAIVE TEST -- "alphanumeric on both sides" -- IS WRONG, AND WRONG ON REAL
    CODE: it reads the PREFIXED char literals `L'a'`, `u'a'`, `U'a'` and `u8'a'` as
    separators, because `L` and `8` are alphanumeric too. Asking for the token's
    FIRST character excludes every one of them (`L`, `u`, `U` are not digits) while
    still accepting `0x1'F`, `1'000'000` and `20'001`.

    ⓘ `u8'a'` is the case that makes the walk-back need to cross letters as well as
    digits: stopping at the first non-digit would find `8` and call it numeric.
    """
    if i + 1 >= len(text) or not (text[i + 1].isalnum() or text[i + 1] == '_'):
        return False            # a separator must sit BETWEEN digits
    j = len(out) - 1
    while j >= 0 and (out[j].isalnum() or out[j] == "'"):
        j -= 1
    return j + 1 < len(out) and out[j + 1].isdigit()


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
        elif c == "'" and _is_digit_separator(out, text, i):
            # ★★ A C++14 DIGIT SEPARATOR IS NOT A QUOTE, AND READING IT AS ONE
            # BLANKS CODE. ✔MEASURED 2026-09-02 (P54, found by lane `ov`):
            # `std::string(20'001, '3')` turned `wall_clock_in_tests_guard` RED
            # while `std::string(20001, '3')` — same value, same line — stayed
            # green. The separator's quote opened a "char literal" that ran to
            # the quote before `3`, so `'001, '` was blanked away.
            #
            # ⚠ THE REPORTED DIRECTION WAS "FAILS TOWARD NOISY". IT IS BOTH.
            # The span that gets blanked is REAL CODE, so a violation sitting
            # inside it is ERASED before the scan ever sees it — this can hide a
            # finding, not merely invent one. NINE guards share this function.
            out.append(c)
            i += 1
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


# U+2502 BOX DRAWINGS LIGHT VERTICAL — absent from cp1252, which is the entire
# reason it is the fixture. Built with `chr()` rather than written as the glyph or
# as a backslash escape: the line that carries the fixture stays pure ASCII, so no
# editor, transfer or re-encode anywhere in the chain can quietly turn it into
# something cp1252 CAN encode and leave the arm passing while asserting nothing.
_BOX = chr(0x2502)


def _pipe_arm() -> bool:
    """Drive a FINDING through a real cp1252 pipe, in a CHILD process.

    ★★★ NOTHING ELSE IN THIS FILE CAN WITNESS THIS, AND THE REASON IS STRUCTURAL.
    The stripper cases above are pure string work; they never touch a stream. The
    defect is a property of the PIPE — a run attached to a terminal proves nothing
    — and of the CHILD, because the encoding is fixed when the interpreter starts.
    (D-GATE-PYTHON-GUARD-DIES-PRINTING-TREE-TEXT-ON-A-WINDOWS-PIPE)

    ★ `PYTHONIOENCODING=cp1252` IS FORCED, which is what makes this arm mean
    something on every host. Left to the ambient locale it would be vacuous on
    Linux and macOS, where the pipe is already UTF-8 — i.e. on most of the gate,
    including both remote legs. The module's own `reconfigure` at import overrides
    the variable, which is precisely the property under test.

    ⚠ THE EXIT CODE CANNOT CARRY THE VERDICT. This guard reports findings on
    STDERR, whose default handler is `backslashreplace`, so the unfixed form does
    not die — it prints the offending source line with the glyph replaced by an
    escape, i.e. it reports a line that does not match the file it names. The arm
    therefore asserts the GLYPH ITSELF survived, which is the only assertion that
    separates a faithful report from a mangled one.
    """
    root = tempfile.mkdtemp(prefix='no-abort-pipe-')
    try:
        os.makedirs(os.path.join(root, 'tests', 'unit'))
        os.makedirs(os.path.join(root, 'integrated_tests'))
        io.open(os.path.join(root, 'tests', 'unit', 'fixture.cpp'), 'w',
                encoding='utf-8', newline='\n').write(
                    '#include <cstdlib>\n'
                    'void f() {\n'
                    '    abort();  // %s a character cp1252 cannot encode\n'
                    '}\n' % _BOX)
        io.open(os.path.join(root, 'integrated_tests', 'keep.cpp'), 'w',
                encoding='utf-8', newline='\n').write('void g() {}\n')
        driver = ('import importlib.util, sys\n'
                  'spec = importlib.util.spec_from_file_location("g", %r)\n'
                  'm = importlib.util.module_from_spec(spec)\n'
                  'spec.loader.exec_module(m)\n'
                  'm.FILE_FLOOR = 1\n'
                  'sys.exit(m.main())\n' % os.path.abspath(__file__))
        env = dict(os.environ)
        env['PYTHONIOENCODING'] = 'cp1252'
        p = subprocess.run([sys.executable, '-c', driver], cwd=root, env=env,
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        out = (p.stdout + p.stderr).decode('utf-8', 'replace')
        died = 'UnicodeEncodeError' in out
        reported = 'a NEW abort() call site' in out
        faithful = _BOX in out
        ok = p.returncode == 1 and not died and reported and faithful
        print(f'  [{"ok " if ok else "FAIL"}] '
              f'{"finding survives a cp1252 PIPE, glyph intact":<30} '
              f'rc={p.returncode} died={died} reported={reported} '
              f'glyph_intact={faithful}')
        return ok
    finally:
        shutil.rmtree(root, ignore_errors=True)


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
        # ★★ C++14 DIGIT SEPARATORS. The first arm is the MEASURED regression
        # (P54, lane `ov`): read as a quote, the separator opens a char literal
        # that runs to the quote before `3` and BLANKS `'001, '` -- real code.
        # The second arm is the direction the original report missed: the blanked
        # span can CONTAIN the violation, so this fails toward clean as well as
        # toward noisy. Nine guards share this stripper.
        ("std::string s(20'001, '3'); abort();",              True,  'digit sep + real call'),
        # ⚠ ONE separator, deliberately, and the count is the whole arm. With an
        # EVEN number (`1'000'000`) the mis-paired quotes re-pair and the call
        # survives by accident -- that spelling passed over the live mutant and
        # discriminated NOTHING. An ODD count leaves the quote unclosed, so the
        # blanking runs to end of text and ERASES the `abort()` behind it.
        ("if (n > 20'001) { abort(); }",                      True,  'ODD sep count cannot hide a call'),
        ("auto c = 20'001; // abort()",                       False, 'sep, then only a comment'),
        # ⚠ THE PREFIXED CHAR LITERALS, which a naive "alphanumeric on both
        # sides" test misreads as separators -- `L`, `u`, `U`, `8` are all
        # alphanumeric. Each payload holds a decoy that must STAY blanked.
        ("auto c = L'a'; /* abort() */",                       False, "L'a' is a char literal"),
        ("auto c = u8'a'; // abort()",                         False, "u8'a' is a char literal"),
        ("auto c = U'a'; auto d = u'b'; // abort()",           False, "U'a'/u'b' are char literals"),
        ("char q = '\\''; abort();",                          True,  'escaped quote then real'),
        ("auto h = 0x1'Fu; abort();",                         True,  'hex literal separator'),
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
    if not _pipe_arm():
        bad += 1
    print(f'selftest: {"FAIL" if bad else "OK"} ({bad} failure(s))')
    return 1 if bad else 0


if __name__ == '__main__':
    _argv = sys.argv[1:]
    _unknown = [a for a in _argv if a != '--selftest']
    if _unknown:
        # Fail loud rather than scan: a typo'd flag that is silently ignored turns
        # an intended self-test into a tree check whose result is read as the
        # other one's.
        print('no-abort-in-tests: unknown argument(s): %s' % ' '.join(_unknown),
              file=sys.stderr)
        sys.exit(2)
    if '--selftest' in _argv:
        sys.exit(_selftest())
    # ★ THE NO-ARGUMENT FORM IS THE CTEST FORM, and it must do BOTH halves: verify
    # the tree, then prove the instrument can still fail. Either alone is a
    # vacuous pass. ⚠ BOTH RUN UNCONDITIONALLY — `main() or _selftest()` would
    # short-circuit the proof exactly when the tree reddens, so a broken
    # instrument would hide behind the failure it was trusted to report.
    _rc = main()
    print('')
    _rc_self = _selftest()
    sys.exit(_rc or _rc_self)
