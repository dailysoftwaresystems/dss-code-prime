#!/usr/bin/env python3
# PURPOSE: refuse DSS_EXPORT on a member of an already-exported class, which is MSVC error C2487.
"""Refuse `DSS_EXPORT` on a MEMBER of a class that is already `DSS_EXPORT`.

D-BUILD-EXPORT-MACRO-ON-AN-EXPORTED-CLASS-MEMBER-BREAKS-MSVC — the row this
guard closes.

WHY THIS EXISTS, and it is a measured CI break rather than a style rule.
`DSS_EXPORT` expands to `__declspec(dllexport)` under MSVC, to
`__attribute__((visibility("default")))` under GCC and Clang, and to nothing in a
static build (`src/core/export.hpp`). Marking a class exported already exports
every member of it, so repeating the macro on a member is redundant everywhere —
but only MSVC calls it an error:

    error C2487: 'addSectionRow': member of dll interface class may not be
                 declared with dll interface

★★ THE COMPILER THAT REFUSES IT IS THE ONE NO LOCAL LEG RUNS. The four-leg gate
is Windows/MinGW-GCC, WSL x86_64 GCC, qemu arm64 GCC and macOS Clang; all four
accept the shape silently. MSVC exists only in CI. ✔MEASURED: the declaration in
`src/link/object_format_schema.hpp` landed in cycle P34 (`5085664a`) and sat
green through eight cycles and a 1708/1708 local Windows gate before the
`windows-msvc-release` job on CI run 33156833090 failed to BUILD on it. A defect
class whose only detector is a fourteen-minute remote job is a defect class that
lands, which is why this is a static check every leg can run.

★★★ WHAT IS REFUSED IS THE MEMBER, NOT THE MACRO INSIDE THE BODY, and the
distinction is MEASURED rather than reasoned. Compiled with `cl /std:c++20` from
Visual Studio 18, one arm per shape, inside a `struct DSS_EXPORT Outer`:

    member function          DSS_EXPORT bool f(int&);        -> error C2487
    static member function   static DSS_EXPORT bool f();     -> error C2487
    static member data       static DSS_EXPORT int n;        -> error C2487
    nested class             class DSS_EXPORT N { ... };     -> ACCEPTED
    nested struct            struct DSS_EXPORT N { ... };    -> ACCEPTED
    friend declaration       friend DSS_EXPORT void f();     -> ACCEPTED

⇒ A guard keyed on "any `DSS_EXPORT` inside an exported class body" would refuse
NINE live sites MSVC is perfectly happy with — `PhaseTimers::Scope`,
`TreeCursor::Bookmark`, `TokenStream::Bookmark`, `TreeBuilder::OpenScope`,
`TreeBuilder::Checkpoint`, `DiagnosticReporter::Snapshot`,
`LexerModeStack::Snapshot`, `SchemaWalker::Snapshot` and
`CompilationUnit::PrivateTag` — and would have been turned off the same day. The
rule below therefore permits exactly the two shapes the compiler permits: a
nested type introduced by `class`/`struct`/`union`, and a `friend` declaration.

★ A BAN, NOT A RATCHET, and the population is why. `check-wall-clock-in-tests`
and `check-no-abort-in-tests` carry per-file inventories because they were
written over 48 and 12 pre-existing sites that could not be fixed in one commit.
✔MEASURED here over `src/`, `tests/`, `integrated_tests/` and `libs/`: after the
one site above is repaired the live population is ZERO, and there is no
legitimate instance to grandfather — the shape does not compile on a compiler
this project ships for. A ratchet with an empty inventory is a ban with extra
moving parts.

FAIL-CLOSED, like every check in this battery: an empty scan is a COLLAPSE, not a
pass (D-GATE-ANCHOR-GUARD-FAILS-OPEN-ON-MISSING-ROOT). Two floors are checked —
files scanned, and exported classes FOUND — because a drifted extension filter
and a drifted `struct DSS_EXPORT` matcher both look exactly like a clean tree.

★ THE COMMENT/STRING STRIPPER IS IMPORTED, NOT COPIED, from
`check-no-abort-in-tests`, which owns the audited one. This file's own prose
contains `DSS_EXPORT` many times, and the header it guards explains the rule in a
comment directly above the declaration — so a matcher that does not strip
comments reds on the documentation of the fix. Three copies of a C comment
stripper in one repository is the duplicated-site shape this project keeps
closing everywhere else.

★ NO `.ps1` TWIN, per the operator ruling of 2026-08-19: a `.py` already runs on
both hosts, so a twin would be a second implementation of something that was
never split.

Exit codes:
    0 = no member carries the macro (and the self-test passed)
    1 = a violation, or the self-test failed
    2 = the scan collapsed (a moved root, a drifted filter, no exported class)
"""

import importlib.util
import io
import os
import re
import sys

# ★ AT IMPORT, NOT IN `main()`, AND BOTH STREAMS. On Windows a Python child whose
# stdout is a PIPE — which is how ctest runs every guard — comes up
# cp1252/surrogateescape, and `surrogateescape` does nothing for an ordinary
# unencodable character. This report quotes source text and prints ★ / ⚠ / ⇒, so
# without this it would die INSIDE ITS OWN REPORT: the run reds, the finding is
# lost, and the traceback names a `print`.
# D-GATE-PYTHON-GUARD-DIES-PRINTING-TREE-TEXT-ON-A-WINDOWS-PIPE
for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError, OSError):   # pragma: no cover - odd stream
        pass

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# Every root that compiles against `core/export.hpp`. `tests/` is included
# deliberately: a test target sees `DSS_SHARED_BUILD` without
# `DSS_BUILDING_DLL`, so `DSS_EXPORT` is `__declspec(dllimport)` there, and
# C2487 names a "dll interface class" either way.
ROOTS = ('src', 'tests', 'integrated_tests', 'libs')

SUFFIXES = ('.hpp', '.h', '.cpp', '.inc', '.ipp')

# Floors. Both are far below the live counts (≈880 files, ≈370 exported
# classes) and exist only to turn a collapsed scan into a refusal.
FILE_FLOOR = 400
EXPORTED_CLASS_FLOOR = 100

MACRO = 'DSS_EXPORT'

# An exported class head: `struct DSS_EXPORT Name` / `class DSS_EXPORT Name`.
_CLASS_HEAD = re.compile(r'\b(?:struct|class|union)\s+' + MACRO + r'\s+(\w+)')
_MACRO_TOKEN = re.compile(r'\b' + MACRO + r'\b')

# What may precede the macro inside an exported body. `class`/`struct`/`union`
# introduces a NESTED TYPE; `friend` introduces a non-member. Both are accepted
# by MSVC (measured — see the module docstring).
_NESTED_TYPE_LEAD = re.compile(r'\b(?:struct|class|union)\s*$')
_FRIEND_LEAD = re.compile(r'\bfriend\b[^;{}]*$')

# How far back to look for that lead-in. A declaration may wrap, so this is
# generous; it is bounded so the scan stays linear in the file.
_LEAD_LOOKBEHIND = 120


def _load_stripper():
    """The comment/string stripper from `check-no-abort-in-tests`, or a loud death."""
    here = os.path.dirname(os.path.abspath(__file__))
    sibling = os.path.join(os.path.dirname(here), 'check-no-abort-in-tests',
                           'check-no-abort-in-tests.py')
    if not os.path.isfile(sibling):
        sys.exit('export-macro-placement: FAIL - cannot find the shared '
                 'comment/string stripper at %s.\n'
                 '  This guard must strip comments and string literals before '
                 'matching, or it reds on the very prose that documents a fix. '
                 'Restore the sibling script or move the stripper somewhere both '
                 'can reach; do NOT copy it.' % sibling)
    spec = importlib.util.spec_from_file_location('_no_abort_in_tests', sibling)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod.strip_comments_and_strings


strip_comments_and_strings = _load_stripper()


def _body_span(code, head_end):
    """(open, close) offsets of the class body that starts after `head_end`.

    Returns None for a forward declaration or an elaborated type specifier —
    anything whose `;` arrives before its `{`.
    """
    brace = code.find('{', head_end)
    if brace < 0:
        return None
    semi = code.find(';', head_end)
    if 0 <= semi < brace:
        return None
    depth, i, n = 0, brace, len(code)
    while i < n:
        if code[i] == '{':
            depth += 1
        elif code[i] == '}':
            depth -= 1
            if depth == 0:
                return (brace, i)
        i += 1
    return None  # unterminated body: a preprocessor-sliced header, not our subject


def violations_in(code):
    """Offsets of every `DSS_EXPORT` that sits on a MEMBER of an exported class.

    `code` must already be stripped of comments and string literals. Offsets are
    de-duplicated because nested exported classes are scanned by both their own
    head and their enclosing one.
    """
    found = set()
    exported_classes = 0
    for head in _CLASS_HEAD.finditer(code):
        span = _body_span(code, head.end())
        if span is None:
            continue
        exported_classes += 1
        open_at, close_at = span
        body = code[open_at:close_at]
        for tok in _MACRO_TOKEN.finditer(body):
            at = open_at + tok.start()
            lead = code[max(0, at - _LEAD_LOOKBEHIND):at]
            if _NESTED_TYPE_LEAD.search(lead):
                continue  # nested type — MSVC accepts it
            if _FRIEND_LEAD.search(lead):
                continue  # friend declaration — not a member
            found.add((at, head.group(1)))
    return sorted(found), exported_classes


def sites_with_lines(raw):
    """Every violation in `raw`, as (line, source text, owning class).

    ⚠ THE OFFSET IS INTO THE STRIPPED TEXT, SO THE LINE MUST BE COUNTED THERE.
    ✔MEASURED: the shared stripper preserves NEWLINES but not LENGTH — it
    collapses a comment rather than blanking it in place — so counting newlines
    in `raw` up to a stripped offset named line 288 for a declaration that lives
    near line 1250. A guard that names the wrong line sends the reader hunting
    and gets distrusted, which is that stripper's own stated reason for
    preserving newlines at all.
    """
    code = strip_comments_and_strings(raw)
    sites, count = violations_in(code)
    lines = raw.splitlines()
    out = []
    for at, owner in sites:
        idx = code.count('\n', 0, at)
        out.append((idx + 1, lines[idx].strip() if idx < len(lines) else '', owner))
    return out, count


def scan_tree(roots):
    per_file = {}
    scanned = 0
    exported_classes = 0
    missing = []
    for root in roots:
        abs_root = os.path.join(REPO, root)
        if not os.path.isdir(abs_root):
            missing.append(root)
            continue
        for dirpath, dirnames, filenames in os.walk(abs_root):
            dirnames[:] = [d for d in dirnames
                           if d not in ('.git', 'build', '_deps', '.worktrees')]
            for name in sorted(filenames):
                if not name.endswith(SUFFIXES):
                    continue
                path = os.path.join(dirpath, name)
                scanned += 1
                with io.open(path, 'r', encoding='utf-8', errors='replace') as fh:
                    raw = fh.read()
                sites, count = sites_with_lines(raw)
                exported_classes += count
                if sites:
                    rel = os.path.relpath(path, REPO).replace(os.sep, '/')
                    per_file[rel] = sites
    return per_file, scanned, exported_classes, missing


def main():
    per_file, scanned, exported_classes, missing = scan_tree(ROOTS)

    # ★ EVERY COLLAPSE CHECK RUNS BEFORE THE VERDICT. A tree that was never read
    # cannot honestly be called clean, and both floors below are shapes that look
    # identical to a clean tree from the outside.
    if missing and len(missing) == len(ROOTS):
        print('export-macro-placement: FAIL - none of the scan roots exist under '
              '%s: %s' % (REPO, ', '.join(missing)), file=sys.stderr)
        print('  THIS SCAN COLLAPSED. Refusing to report a pass.', file=sys.stderr)
        return 2
    if scanned < FILE_FLOOR:
        print('export-macro-placement: FAIL - scanned only %d file(s), below the '
              'floor of %d.' % (scanned, FILE_FLOOR), file=sys.stderr)
        print('  This does NOT mean the tree is clean - it means THIS SCAN '
              'COLLAPSED (a moved subtree or a drifted extension filter).',
              file=sys.stderr)
        print('  Fix the scan; do not lower the floor.', file=sys.stderr)
        return 2
    if exported_classes < EXPORTED_CLASS_FLOOR:
        print('export-macro-placement: FAIL - found only %d exported class bod(ies), '
              'below the floor of %d.' % (exported_classes, EXPORTED_CLASS_FLOOR),
              file=sys.stderr)
        print('  The files were read, so the CLASS-HEAD matcher is what drifted - '
              'if `%s` is spelled differently now, this guard has been silently '
              'checking nothing.' % MACRO, file=sys.stderr)
        print('  Fix the matcher; do not lower the floor.', file=sys.stderr)
        return 2

    if per_file:
        print('export-macro-placement: FAIL - %s on a MEMBER of an already-exported '
              'class:' % MACRO, file=sys.stderr)
        for path, sites in sorted(per_file.items()):
            for lineno, raw, owner in sites:
                print('    %s:%d: in %s' % (path, lineno, owner), file=sys.stderr)
                print('        %s' % raw, file=sys.stderr)
        print('  MSVC refuses this outright: error C2487, "member of dll interface '
              'class may not be declared with dll interface". GCC and Clang accept '
              'it silently, so it builds on every local leg and breaks the '
              'windows-msvc CI job only.', file=sys.stderr)
        print('  THE FIX IS TO DELETE THE MACRO, never to un-export the class: the '
              'enclosing class already exports every member, so the macro on the '
              'member buys nothing anywhere.', file=sys.stderr)
        print('  A NESTED CLASS may carry it (measured: MSVC accepts that), and so '
              'may a `friend` declaration. Neither is a member.', file=sys.stderr)
        return 1

    print('export-macro-placement: OK (%d files scanned, %d exported class bod(ies), '
          '0 members carrying %s)' % (scanned, exported_classes, MACRO))
    return 0


# ─────────────────────────────── self-test ────────────────────────────────────
#
# ★★ IT RUNS ON EVERY INVOCATION, NOT BEHIND A FLAG. A guard whose self-test
# hides behind `--selftest` and whose ctest entry passes no flag proves nothing
# about its own ability to fail — the shape measured live in
# `no_abort_in_tests_guard` and in `enum_name_table_guard`
# (D-GATE-TWO-GUARDS-SELF-TEST-BEHIND-A-FLAG-NOBODY-PASSES).
#
# ★★ AND EVERY ARM IS SYNTHESIZED IN THE **REMOVE** DIRECTION WHERE IT CAN BE:
# the accepted arms are the shapes that must NOT red, and they are the ones that
# would silently make this guard useless if the matcher over-reached. An
# ADD-direction fixture alone would stay green while the guard refused nine live
# sites (feedback-a-fixture-must-synthesize-the-negative).

_MUST_REFUSE = [
    ('member function',
     'struct DSS_EXPORT Outer { DSS_EXPORT bool f(int& o); };'),
    ('member function, attribute first',
     'struct DSS_EXPORT Outer {\n'
     '    [[nodiscard]] DSS_EXPORT bool\n'
     '    f(Info info, std::uint16_t& out);\n'
     '};'),
    ('static member function',
     'class DSS_EXPORT Outer { public: static DSS_EXPORT bool f(); };'),
    ('static member data',
     'struct DSS_EXPORT Outer { static DSS_EXPORT int counter; };'),
    ('member of a NESTED exported class',
     'struct DSS_EXPORT Outer { class DSS_EXPORT N { DSS_EXPORT void g(); }; };'),
]

_MUST_ACCEPT = [
    ('nested class',
     'struct DSS_EXPORT Outer { class DSS_EXPORT N { public: int n; }; };'),
    ('nested struct',
     'class DSS_EXPORT Outer { struct DSS_EXPORT N { int n; }; };'),
    ('friend declaration',
     'struct DSS_EXPORT Outer { friend DSS_EXPORT void f(); };'),
    ('plain members of an exported class',
     'struct DSS_EXPORT Outer { bool f(int& o); int n; };'),
    ('free function outside any class',
     'DSS_EXPORT bool f(int& o);\nstruct DSS_EXPORT Outer { int n; };'),
    ('forward declaration, no body',
     'struct DSS_EXPORT Outer;\nDSS_EXPORT void f();'),
    ('macro inside a COMMENT in an exported body',
     'struct DSS_EXPORT Outer {\n'
     '    // NO DSS_EXPORT here: the class already exports its members.\n'
     '    bool f(int& o);\n'
     '};'),
    ('macro inside a STRING in an exported body',
     'struct DSS_EXPORT Outer { char const* s = "DSS_EXPORT"; };'),
    ('member of a NON-exported class',
     'struct Plain { DSS_EXPORT bool f(int& o); };'),
]


def _selftest():
    bad = 0
    print('selftest: export-macro-placement')

    for label, src in _MUST_REFUSE:
        sites, _ = violations_in(strip_comments_and_strings(src))
        if not sites:
            print('  [FAIL] not refused: %s' % label)
            bad += 1
        else:
            print('  [ok  ] refused: %s' % label)

    for label, src in _MUST_ACCEPT:
        sites, _ = violations_in(strip_comments_and_strings(src))
        if sites:
            print('  [FAIL] wrongly refused: %s' % label)
            bad += 1
        else:
            print('  [ok  ] accepted: %s' % label)

    # ★ THE REPORTED LINE MUST BE THE REAL ONE. The stripper preserves newlines
    # but NOT length, so a naive offset->line map over the RAW text is wrong by
    # however much comment prose precedes the site — measured at 288-vs-1250 on
    # the live header before this arm existed. The fixture puts a long comment
    # block and a string literal ahead of the violation precisely so a
    # regression to raw-offset counting cannot pass.
    lead = ''.join('// filler comment line %d\n' % i for i in range(40))
    fixture = (lead
               + 'char const* s = "DSS_EXPORT and some padding text";\n'
               + 'struct DSS_EXPORT Outer {\n'
               + '    /* a long block comment that is much wider than the code\n'
               + '       it explains, twice over, so the offsets diverge */\n'
               + '    DSS_EXPORT bool f(int& o);\n'
               + '};\n')
    expected_line = fixture.splitlines().index('    DSS_EXPORT bool f(int& o);') + 1
    located, _ = sites_with_lines(fixture)
    if len(located) != 1 or located[0][0] != expected_line:
        print('  [FAIL] reported line %s, expected %d (offset->line mapping)'
              % ([s[0] for s in located], expected_line))
        bad += 1
    else:
        print('  [ok  ] the reported line survives leading comments and strings')

    # The floors must be REACHABLE downward, or they are decoration: a guard
    # whose collapse arm cannot fire has no collapse arm.
    empty, count = violations_in('')
    if empty or count:
        print('  [FAIL] an empty translation unit reported findings')
        bad += 1
    else:
        print('  [ok  ] an empty translation unit reports nothing (floors do the work)')

    # The imported stripper must really be the sibling's, not a silent fallback.
    if strip_comments_and_strings.__module__ != '_no_abort_in_tests':
        print('  [FAIL] the shared stripper was not imported from its owner')
        bad += 1
    else:
        print('  [ok  ] the comment/string stripper is the SHARED one, not a copy')

    print('selftest: %s (%d failure(s))' % ('FAIL' if bad else 'OK', bad))
    return 1 if bad else 0


if __name__ == '__main__':
    if '--selftest' in sys.argv:
        sys.exit(_selftest())
    # ★ The no-argument form (the ctest form) verifies the TREE and then proves
    # the guard can fail. Either half alone is a vacuous pass.
    rc = main()
    print()
    rc_self = _selftest()
    sys.exit(rc or rc_self)
