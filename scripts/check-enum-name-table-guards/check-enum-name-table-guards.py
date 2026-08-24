#!/usr/bin/env python3
# PURPOSE: refuse an `EnumNameTable` vocabulary declared in `src/` without a `DSS_CHECK_ENUM_NAME_TABLE` well-formedness assert.
"""check-enum-name-table-guards.py -- the DECLARATION gate for closed vocabularies.

★★★ WHY THIS EXISTS, and it is a measured gap rather than a hypothetical.

`EnumNameTable<E, N>` is an aggregate. `EnumNameTable<E, 30>` written with **29**
initializers COMPILES: the missing row value-initializes to `{E(0), ""}`, so
`fromName("")` starts RESOLVING -- and for a vocabulary whose enumerator 0 is a
sentinel (`None`, `Default`, `Unspecified`), an empty string silently selects
"this knob does nothing". A dropped row would not break the build; it would make
`"lowering": ""` load clean and disable the intrinsic.

`DSS_CHECK_ENUM_NAME_TABLE` (in `src/core/types/enum_name_table.hpp`) is the
predicate that refuses that shape at COMPILE time, along with a duplicate spelling
and a duplicate ENUMERATOR -- the last of which `DSS_CHECK_KEY_VOCABULARY(allNames(...))`
CANNOT see, because `allNames` erases the value half of every row.

⇒ THE HAZARD THIS GATE CLOSES IS THE NEXT TABLE, NOT THE PRESENT ONES.
When the predicate landed it was applied to ONE table. ✔MEASURED by the step-10
audit of cycle P23: **64** vocabularies were declared in `src/`, **1** carried the
predicate, **10** carried the names-only form, and **53** carried no well-formedness
guard of any kind -- including every table that cycle had just minted or converted.
The sweep fixed all 53. Nothing, however, would have noticed the 65th, so the
sweep without this gate would decay to exactly the state it repaired.

★ THE INSTRUMENT READS THE DECLARATIONS, NOT A LIST SOMEBODY MAINTAINS.
That is the same correction `check-diagnostic-codes.py` records: a hand-maintained
pin can only check rows somebody remembered to add to it.

  python scripts/check-enum-name-table-guards/check-enum-name-table-guards.py
  python scripts/check-enum-name-table-guards/check-enum-name-table-guards.py --self-test

⚠ NO `.ps1` TWIN, DELIBERATELY: this is Python and therefore already runs on both
hosts. A twin would be a second implementation of something that was never split.
"""
import argparse
import io
import os
import re
import sys

# ── OUTPUT ENCODING — NOT COSMETIC, AND THE STREAM IS HALF THE FACT ─────────────
# ✔MEASURED 2026-08-23 (CPython 3.14.3, Windows, BOTH streams PIPES, which is
# exactly how ctest runs every guard): `sys.stdout` comes up
# `encoding='cp1252' errors='surrogateescape'` and `sys.stderr` comes up
# `errors='backslashreplace'`. `surrogateescape` rescues only lone surrogates left
# by an earlier decode; it does NOTHING for an ordinary unencodable character. So a
# report printed on STDOUT — where this guard names every unguarded vocabulary and
# the file that declares it —
# raises `UnicodeEncodeError` and kills the guard INSIDE ITS OWN REPORT: the run
# still reds, but the finding is lost and the traceback names a `print` rather than
# the thing that was wrong. STDERR merely mangles the glyph into an escape.
# ⚠ Names and paths are ASCII in this tree TODAY (\u2714MEASURED 2026-08-23: 0 of 2605
# tracked paths are unencodable in cp1252), which makes this prophylactic rather
# than a live red — and one non-ASCII identifier away from not being.
# Applied at IMPORT, so every path this module can print on is covered.
# D-GATE-PYTHON-GUARD-DIES-PRINTING-TREE-TEXT-ON-A-WINDOWS-PIPE
for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError, OSError):   # pragma: no cover - odd stream
        pass


DECL = re.compile(r'inline\s+constexpr\s+EnumNameTable\s*<[^>]*>\s*\n?\s*(k[A-Za-z0-9_]+)\s*\{')
GUARD = re.compile(r'DSS_CHECK_ENUM_NAME_TABLE\s*\(\s*(k[A-Za-z0-9_]+)\s*\)')

# The macro's own definition names its parameter, not a table.
DEFINE = re.compile(r'#\s*define\s+DSS_CHECK_ENUM_NAME_TABLE')

# A floor, so a scan that silently collapses to nothing REDS instead of passing.
# ✔MEASURED 2026-08-20: 64 declarations across 12 files. The floor is set below
# that with room for a file to be split, not at it -- a floor equal to the count
# reds on every honest addition.
DECL_FLOOR = 40


def repo_root():
    here = os.path.dirname(os.path.abspath(__file__))
    return os.path.abspath(os.path.join(here, '..', '..'))


def harvest(root):
    """Return {table name: file} and the set of guarded names."""
    decls, guarded = {}, set()
    src = os.path.join(root, 'src')
    for dirpath, _dirs, files in os.walk(src):
        for fn in files:
            if not fn.endswith(('.hpp', '.cpp', '.h')):
                continue
            path = os.path.join(dirpath, fn)
            rel = os.path.relpath(path, root).replace('\\', '/')
            try:
                text = io.open(path, encoding='utf-8', newline='').read()
            except (OSError, UnicodeDecodeError):
                continue
            for m in DECL.finditer(text):
                decls.setdefault(m.group(1), rel)
            for m in GUARD.finditer(text):
                line_start = text.rfind('\n', 0, m.start()) + 1
                if DEFINE.match(text, line_start):
                    continue
                guarded.add(m.group(1))
    return decls, guarded


def check(root, decl_floor=DECL_FLOOR, out=sys.stdout):
    decls, guarded = harvest(root)
    if len(decls) < decl_floor:
        print('check-enum-name-table-guards: SCAN COLLAPSED -- found only %d declaration(s)'
              ' under src/, floor is %d. The scan, not the tree, is what changed.'
              % (len(decls), decl_floor), file=out)
        return 2

    missing = sorted(n for n in decls if n not in guarded)
    if missing:
        print('check-enum-name-table-guards: %d vocabular%s declared without'
              ' DSS_CHECK_ENUM_NAME_TABLE:' % (len(missing), 'y' if len(missing) == 1 else 'ies'),
              file=out)
        for n in missing:
            print('  %s   (%s)' % (n, decls[n]), file=out)
        print('', file=out)
        print('An EnumNameTable with one row too few is legal C++ and makes "" a resolving',
              file=out)
        print('spelling. Add `DSS_CHECK_ENUM_NAME_TABLE(<name>);` after the declaration.',
              file=out)
        print('DSS_CHECK_KEY_VOCABULARY(allNames(...)) does NOT satisfy this: allNames erases',
              file=out)
        print('the value half, so it cannot see two rows sharing an enumerator.', file=out)
        return 1

    print('check-enum-name-table-guards: OK (%d vocabular%s in src/, all guarded)'
          % (len(decls), 'y' if len(decls) == 1 else 'ies'), file=out)
    return 0


# ── the self-test: red-on-disable for the instrument itself ──────────────────
def self_test():
    """Every arm asserts the MESSAGE of the refusal it names, not merely a code."""
    import shutil
    import tempfile

    arms = 0
    tmp = tempfile.mkdtemp(prefix='entg-')
    try:
        src = os.path.join(tmp, 'src', 'core')
        os.makedirs(src)

        def write(name, body):
            with io.open(os.path.join(src, name), 'w', encoding='utf-8', newline='') as f:
                f.write(body)

        def run(floor=3):
            buf = io.StringIO()
            rc = check(tmp, decl_floor=floor, out=buf)
            return rc, buf.getvalue()

        guarded_tbl = ('inline constexpr EnumNameTable<E%(i)d, 2> kT%(i)d{{{\n'
                       '    { E%(i)d::A, "a" },\n'
                       '    { E%(i)d::B, "b" },\n'
                       '}}};\n'
                       'DSS_CHECK_ENUM_NAME_TABLE(kT%(i)d);\n')
        bare_tbl = ('inline constexpr EnumNameTable<E%(i)d, 2> kT%(i)d{{{\n'
                    '    { E%(i)d::A, "a" },\n'
                    '}}};\n')

        # arm 1 -- GREEN: every declaration guarded.
        write('a.hpp', ''.join(guarded_tbl % {'i': i} for i in range(3)))
        rc, msg = run()
        assert rc == 0 and 'OK (3 vocabularies' in msg, (rc, msg)
        arms += 1

        # arm 2 -- RED: one declaration with no guard, and the message NAMES it.
        write('b.hpp', bare_tbl % {'i': 9})
        rc, msg = run()
        assert rc == 1, (rc, msg)
        assert 'kT9' in msg and 'src/core/b.hpp' in msg, msg
        assert '1 vocabulary declared without' in msg, msg
        arms += 1

        # arm 3 -- RED: the names-only form does NOT satisfy the gate, and the
        # message says why. This is the arm that matters: the tree already had
        # ten tables in exactly this state and they read as guarded.
        write('b.hpp', (bare_tbl % {'i': 9}) + 'DSS_CHECK_KEY_VOCABULARY(allNames(kT9));\n')
        rc, msg = run()
        assert rc == 1 and 'kT9' in msg, msg
        assert 'allNames erases' in msg, msg
        arms += 1

        # arm 4 -- the macro's own DEFINITION must not count as a guard.
        write('b.hpp', (bare_tbl % {'i': 9})
              + '#define DSS_CHECK_ENUM_NAME_TABLE(kT9) static_assert(true)\n')
        rc, msg = run()
        assert rc == 1 and 'kT9' in msg, msg
        arms += 1

        # arm 5 -- GREEN after restore: the red arms were the change, not the fixture.
        write('b.hpp', guarded_tbl % {'i': 9})
        rc, msg = run()
        assert rc == 0 and 'OK (4 vocabularies' in msg, (rc, msg)
        arms += 1

        # arm 6 -- SCAN COLLAPSED: a floor above the tree reds rather than passing.
        rc, msg = run(floor=99)
        assert rc == 2 and 'SCAN COLLAPSED' in msg and 'floor is 99' in msg, msg
        arms += 1

        # arm 7 -- a wrapped declaration is still found (the name may sit on its
        # own line when the template argument list is long).
        write('c.hpp', 'inline constexpr EnumNameTable<ELong, kCount>\n'
                       'kWrapped{{{\n    { ELong::A, "a" },\n}}};\n'
                       'DSS_CHECK_ENUM_NAME_TABLE(kWrapped);\n')
        rc, msg = run()
        assert rc == 0 and 'OK (5 vocabularies' in msg, (rc, msg)
        arms += 1
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    print('check-enum-name-table-guards: self-test OK - %d arms exercised, every red arm'
          ' asserting the MESSAGE of the refusal it names; this guard is PROVEN able to fail.'
          % arms)
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[2])
    ap.add_argument('--self-test', action='store_true',
                    help='drive the guard against synthetic trees and refuse if it cannot fail')
    args = ap.parse_args()
    if args.self_test:
        return self_test()
    # star star THE NO-ARGUMENT FORM -- the one ctest uses -- VERIFIES THE REAL TREE
    # AND THEN PROVES IT CAN FAIL, in that order.
    # ⚠ IT DID NOT, AND THE COMMENT IN CMakeLists.txt SAID IT DID. Measured
    # 2026-08-22: this returned `check(...)` alone, so the ctest entry would have
    # passed identically with every assertion inside `check` deleted -- the vacuous
    # pass the sibling guards were built to refuse, in the guard that refuses it for
    # enum tables. Found while writing `check-shell-portability` against the same
    # convention. D-GATE-ENUM-NAME-TABLE-CTEST-FORM-NEVER-SELF-TESTED.
    rc = check(repo_root())
    if rc != 0:
        return rc
    return self_test()


if __name__ == '__main__':
    sys.exit(main())
