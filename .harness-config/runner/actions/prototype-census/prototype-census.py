#!/usr/bin/env python3
"""prototype-census — does every prototype and every integer value DSS ships agree with the platform's own header?

Over every row of the tree's shipped descriptors (`src/dss-config/shippedLibs/**/*.json`) that the leg's pair sees,
the reference compiler of the leg's host and the leg's own dsscp are made to answer the same question.

PROTOTYPES (the `census` step) — TWO verdicts per `symbols` row, each from a different judge, because each judge is
blind where the other sees:

  (a) DSS JUDGES ITS OWN PROTOTYPE. The reference compiler first SPELLS the type of `&sym` under the platform header
      (a TU per header initializes `struct census_probe_sink *` from each `&sym`; the refusal names the address's
      type, typedefs expanded where the compiler prints an `aka` form). Then a program per header prints
      `_Generic(&sym, typeof(<that spelling>): "yes", default: "NO")`, compiled by the leg's dsscp for the leg's own
      pair and RUN on the leg. It answers with DSS's own type system, so it sees what DSS resolves — an untagged 64-bit
      core, a typedef's identity, a width — and nothing it re-implements. A parameter's POINTEE `const` is part of what
      it compares (the self-test's `a-pointee-const` arm pins it): a prototype the probe DECLARES with `char *` is "NO"
      against a header's `const char *`. A descriptor row that spells no qualifier at all makes NO claim about one,
      though, so verdict (a) cannot see a `const` such a row dropped — verdict (b) is the judge that does. ★ A
      reference that prints no `aka` form for a typedef (gcc keeps `FILE **`) hands DSS the typedef NAME, which DSS
      resolves to its own typedef — so that typedef's identity is judged by clang legs only.
  (b) THE REFERENCE JUDGES THE DESCRIPTOR'S TEXT. The symbol's signature for the pair — the flat `signature`, or the
      ONE `variants` arm whose `when` the pair matches, else the row's `default` arm (the reader's own rule; a pair the
      reader would refuse is reported PAIR-REFUSED) — is rendered to C with typedef NAMES kept (the reference resolves
      them its own way), and the reference compiles and runs `_Generic(&sym, <rendered> *: "yes", default: "NO")`
      against its real header. It sees the prototype's SHAPE — a dropped `const`, an opaque type modelled `void *`, a
      comparator's parameters — which (a) cannot, and trusts the typedef names, which (a) checks.
  A symbol where the two disagree is a finding in itself; the report keeps both.

VALUES (the `values` step) — every integer `constants` row and every macro whose replacement is an integer literal,
in every descriptor the pair sees, BY NAME ONLY: a program per header prints `NAME=<value>` for each name the header
defines as a macro (`#ifdef`, else `NAME=nodef`), compiled and run ONCE by the reference compiler and ONCE by the leg's
dsscp. DSS's value is what DSS's own preprocessor selects — this program never re-implements the descriptor's variant
selection. A name both define with different values is a MISMATCH and fails the step; a name only one side defines as a
macro (an enum constant, a name the pair's header lacks) is reported, not judged. Built after P69 found Linux aarch64's
O_DIRECTORY/O_NOFOLLOW/O_TMPFILE carrying x86_64's values (D-FFI-FCNTL-AARCH64-OPEN-FLAGS-TAKE-X86-64-VALUES).
A FORK between two references' HEADERS of one pair, once DECIDED, is data beside this program (`decided-forks.json`:
per format, dialect, header and name — both values, the authority, the reason). Such a name reads `decided`, counted
apart from `match` and `mismatch` and named in the step's log (`VALUES DECIDED:`), only while BOTH recorded values
still hold: a value that moved on either side, or a name the run never reads, fails the step and names the entry, so
the decision is read again. An entry is its own format's and dialect's alone — no other leg reads it. ✔MEASURED
2026-10-08, the first two: on pe, mingw-w64's <stdio.h> gives L_tmpnam 14 and TMP_MAX 32767 where the UCRT's own
header, which cl reads, gives 260 and 2147483647 — and the MinGW gcc targets that same UCRT.

THE PAIR is the leg's NATIVE one — the judge programs must RUN there — and it is DERIVED, never tabled: the host's
executable format is read from this interpreter's own image (ELF, PE or Mach-O magic), the processor is the harness's
`{processor}`, and the tree's object-format documents name the `cli` document(s) of that kind for that target. When
several match (ELF's exec and pie) they must agree on every fact the descriptors are read with, or the run is refused.

THE REFERENCE is `--cc`, always named: the ctest entry hands it the build's own C compiler, a run the step's `cc` input.
Its dialect is READ from the compiler itself, never from its name: clang's and gcc's from their `--version` banner
(✔MEASURED 2026-09-30, clang 18.1.3 and gcc 13.3.0); MSVC's cl, which answers no `--version`, from the banner it prints
when run with no arguments (✔MEASURED 2026-10-07, cl 19.51.36260); any other compiler is refused by name, never parsed
by guess. A reference the host does not have AT ALL is refused in this program's own words (`absent_reference`), not
the system's — which differ from host to host, so the absence is asked of the PATH and never read off the error
(`reference_dialect`): it names the missing program, says that the census has no reference of its own and never
picks one — which compiler a leg builds with is the leg's fact, and a PATH does not say — how a run names the leg's
(`--input cc=<name>`), and which of the compilers it reads the step's PATH holds (✔MEASURED 2026-10-08: neither
Windows leg has a `cc`, the default of a run's `cc` input, so a run there that names none ends at its first step,
exit 2, with that refusal). Each dialect carries how it is DRIVEN — its standard mode (gcc and clang
`-std=gnu2x -D_GNU_SOURCE`, cl `/std:clatest`, which `typeof` needs), its syntax-only check, its output option — and
how its diagnostics name the type
of `&sym` and an undeclared name. `--flags` adds to that, in the reference's own spelling. cl (✔MEASURED 2026-10-07,
windows-x86_64-release) prints its diagnostics on STDOUT, keeps typedef names (it has no `aka` form) and the calling
convention (`__cdecl`, which DSS's own pe config erases), names the type in two shapes (C4047 for a function, an array
or a pointer to a pointer, C4133 for an object), follows an undeclared name's C2065 with a C4133 naming `int *`, and
stops judging a TU at 100 errors (C1003) — so the spelling step re-runs the rows a stopped TU never reached. It spells
a struct, union or enum type WITHOUT its tag word (`tm *` for `struct tm *`), which is no C type to hand DSS, so the
spelling step asks cl, in a TU of its own, which spelled names are only tags and of which kind (a tag's `typedef`
line fails, C2061; `struct <a union's tag>` fails naming the kind, C2011), and puts the word back — that TU, too,
re-run for the names a 100-error stop never reached. And its `_Generic` KEEPS a parameter's top-level `restrict` when
it compares function types — a callback's parameter's too (✔MEASURED 2026-10-07 and 2026-10-08: `void f(char
*restrict)` against `void (*)(char *)` is NO on cl 19.51.36260, and yes on gcc, clang and DSS, as C 6.7.6.3p15 has
it) — while a descriptor's text cannot spell what is no part of a function's type. So on cl a row whose verdict (b)
is NO and whose spelling holds a parameter's `restrict` is ASKED AGAIN, in the form cl answers (✔MEASURED 2026-10-08,
probe-reference-cc run 20261008-085400-883c8a53): first THE WITNESS — cl's own spelling as the type of `&sym`, the
qualifier on both sides, must be yes — then the descriptor's text against that spelling with each parameter's
`restrict` removed, `_Generic((<spelling>)0, <text>: …)`, the qualifier on NEITHER side; that yes or NO is the
verdict (threads.json's `mtx_timedlock`, whose header declares two restrict parameters and whose descriptor models
both `ptr<void>`, is a NO in that form as it is on Linux, and would be a yes with the right text). Only a `restrict`
that ENDS a parameter is removed; a pointee's is part of the type on every reference and stays. REF-CANNOT-JUDGE is
left for the row where that question cannot be put, its reason in the cell. gcc and clang, for their part, PRINT a
const or noreturn function's address type with the attribute as a decoration neither takes as part of a type — gcc
first, clang last for noreturn, a form no compiler accepts as a type name (✔MEASURED 2026-10-08) — so the spelling
step removes it, by a closed list held to each leg's compiler (`DECORATIONS`) and only on a row where the
reference's own `_Generic` takes the bare spelling as the type of THAT row's symbol (gcc prints the noreturn of a
function an OBJECT points to at the front too, and does not take the bare type there), counts the rows it removed
one from (`ref-decoration-dropped`) and names the removal in the report's last column; any other attribute, and a
removal the reference does not witness, is handed on.

Verdicts: yes · NO · REF-UNDECLARED (the reference's header does not declare it) · REF-NO-HEADER (the reference has
no such header at all — Apple ships no <threads.h>) · REF-ERROR (the reference refused the spelling probe some other
way) · REF-CANNOT-SPELL (the reference cannot compile the rendered text, e.g. a DSS-only name) · REF-CANNOT-JUDGE
(verdict (b) alone, on a reference whose `_Generic` keeps a parameter's `restrict`: the question could not be put
without it — the spelling holds a `restrict` this program cannot place, or the reference does not take its own
spelling as the symbol's type; the reason is in the cell, and verdict (a) still judges the row) · DSS-REFUSED (DSS
would not compile the judge probe) · PAIR-REFUSED (no arm of the row's `signature` serves the pair, which the
descriptor reader refuses) · JUDGE-FAILED (a judge program exited non-zero — its stdout is never read as verdicts).
A cell that keeps a compiler's or a judge's message keeps it on ONE line, named relative to the scratch directory
the program was compiled in, whole up to a bound; past it the message's START and its END are both kept around a
`[...]` (its code opens it, and a parse refusal of DSS's closes with what it met), each cut at a word's end.
THE CENSUS LINE COUNTS EVERY ROW: verdict (a)'s categories partition the symbols and so do verdict (b)'s, and a
report holding a verdict of either that the line does not count fails the step (`prototype-census: FAILED — …`) — a
census never passes over rows it does not account for. ✔MEASURED 2026-10-06: before this rule the Mac's line said OK
over 25 `threads.json` rows (REF-ERROR — no <threads.h>) that no count named; and until 2026-10-08 only verdict
(a)'s list was held — a verdict (b) outside every count read `OK … b-yes=1`, every other (b) field 0 (the P69
review's measurement on a copy of `summarize`; the `s-accounting` arm now holds both lists).
Last line: `prototype-census: OK …` / `prototype-census: VALUES OK …` / `prototype-census: SELFTEST OK …`.

Usage: prototype-census.py --tree=<repo> --processor=<cpu> --dsscp=<path> --cc=<reference> [--flags=a,b]
                           --out=<report.tsv> [--only=<header>]
       prototype-census.py --values --tree=<repo> --processor=<cpu> --dsscp=<path> --cc=<reference> [--flags=a,b]
                           --out=<report.tsv>
       prototype-census.py --selftest --tree=<repo> --processor=<cpu> --dsscp=<path> --cc=<reference> [--flags=a,b]
`--cc` is required: a compiler path or a name on PATH. `--flags` is comma-separated and each flag is percent-decoded,
so a comma INSIDE one flag is written `%2C`."""
import argparse
import glob
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from urllib.parse import unquote

# A report line can carry a character cp1252 cannot (a reference compiler's quote, a header's type spelling); on a
# pipe under a Windows code page it would be lost or mangled, so both streams write UTF-8 from import on — argument
# parsing and --help print before main() runs.
for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError, OSError):
        pass

# ── the hir-text type grammar, read only to RENDER a descriptor's text for verdict (b) ─────────────────────────────
SCALAR = {'i8': 'signed char', 'u8': 'unsigned char', 'i16': 'short', 'u16': 'unsigned short', 'i32': 'int',
          'u32': 'unsigned int', 'f32': 'float', 'f64': 'double'}
BARE = {'i64', 'u64', 'i128', 'u128', 'f80', 'f128'}          # no C spelling without a tag: rendered to a marker
NAMED = {'char': 'char', 'void': 'void', 'bool': '_Bool'}
STRUCT_SPELLING = {'FILE': 'FILE', 'DIR': 'DIR', '_div_t': 'div_t', '_ldiv_t': 'ldiv_t', '_lldiv_t': 'lldiv_t'}
MARKERS = ''.join('struct census_probe_bare_%s { int x; }; ' % c for c in sorted(BARE))
TOKEN = re.compile(r'\s*(\.\.\.|->|[@~]\d+|\d+|[A-Za-z_][A-Za-z0-9_]*|"[^"]*"|[<>(),{}])')


def tokenize(text):
    pos, out = 0, []
    while pos < len(text):
        m = TOKEN.match(text, pos)
        if not m:
            if not text[pos:].strip():
                break
            raise ValueError('cannot tokenize at %r' % text[pos:pos + 20])
        out.append(m.group(1))
        pos = m.end()
    return out


class Parser:
    def __init__(self, text, idents):
        self.t, self.i, self.idents = tokenize(text), 0, idents

    def peek(self):
        return self.t[self.i] if self.i < len(self.t) else None

    def take(self, want=None):
        tok = self.peek()
        if want is not None and tok != want:
            raise ValueError('expected %r, got %r' % (want, tok))
        self.i += 1
        return tok

    def type(self):
        tok = self.take()
        if tok == 'fn':
            self.take('(')
            params, variadic = [], False
            while self.peek() != ')':
                if self.peek() == '...':
                    self.take()
                    variadic = True
                else:
                    params.append(self.type())
                if self.peek() == ',':
                    self.take()
            self.take(')')
            ret = ('named', 'void')
            if self.peek() == '->':
                self.take()
                ret = self.type()
            return ('fn', params, variadic, ret)
        if tok in ('arr', 'vec'):
            self.take('<')
            inner = self.type()
            self.take(',')
            n = int(self.take())
            self.take('>')
            return ('arr', inner, n)
        if tok in ('ptr', 'const', 'volatile', 'complex', 'atomic', 'ref', 'nullable', 'optional', 'slice'):
            self.take('<')
            inner = self.type()
            self.take('>')
            return (tok, inner)
        if tok in ('struct', 'union', 'enum'):
            name = self.take().strip('"')
            while self.peek() in ('opaque', 'packed'):
                self.take()
            if self.peek() == '{':
                depth = 0
                while True:
                    x = self.take()
                    depth += (x == '{') - (x == '}')
                    if depth == 0:
                        break
            return (tok, name)
        if tok in SCALAR or tok in BARE:
            tag = None
            if self.peek() and self.peek().startswith('"'):
                tag = self.take().strip('"')
            return ('scalar', tok, tag)
        if tok in NAMED:
            return ('named', tok)
        if tok in self.idents:
            return ('ident', self.idents[tok])
        raise ValueError('unknown type token %r' % tok)


def base_spelling(t):
    k = t[0]
    if k == 'scalar':
        if t[2]:
            return t[2]
        return SCALAR[t[1]] if t[1] in SCALAR else 'struct census_probe_bare_%s' % t[1]
    if k == 'named':
        return NAMED[t[1]]
    if k == 'ident':
        return t[1]
    if k in ('struct', 'union', 'enum'):
        return STRUCT_SPELLING.get(t[1], '%s %s' % (k, t[1]))
    if k == 'complex':
        return base_spelling(t[1]) + ' _Complex'
    raise ValueError('no base spelling for %r' % (t,))


def declarator(t, inner):
    k = t[0]
    if k == 'ptr':
        return declarator(t[1], '(*' + inner + ')' if t[1][0] in ('fn', 'arr') else '*' + inner)
    if k in ('const', 'volatile'):
        if t[1][0] == 'ptr':
            return declarator(t[1], '').rstrip() + ' ' + k + (' ' + inner if inner else '')
        return k + ' ' + declarator(t[1], inner)
    if k == 'arr':
        return declarator(t[1], inner + '[%d]' % t[2])
    if k == 'atomic':
        return '_Atomic(' + declarator(t[1], '') + ')' + (' ' + inner if inner else '')
    if k == 'fn':
        ps = [declarator(p, '') for p in t[1]] + (['...'] if t[2] else [])
        return declarator(t[3], inner + '(' + (', '.join(ps) if ps else 'void') + ')')
    return base_spelling(t) + (' ' + inner if inner else '')


# ── the pair ─────────────────────────────────────────────────────────────────────────────────────────────────────────
# The magic numbers of the three executable formats a host can run, read from this interpreter's OWN image: the kind of
# executable the host runs is a fact of the host, never a table keyed on an operating-system name.
EXEC_MAGIC = ((b'\x7fELF', 'elf'), (b'MZ', 'pe'), (b'\xcf\xfa\xed\xfe', 'macho'), (b'\xfe\xed\xfa\xcf', 'macho'),
              (b'\xca\xfe\xba\xbe', 'macho'))


def host_format_kind(image=None):
    with open(image or sys.executable, 'rb') as f:
        head = f.read(4)
    for magic, kind in EXEC_MAGIC:
        if head.startswith(magic):
            return kind
    return None


def read_format_doc(tree, name):
    return json.load(open(os.path.join(tree, 'src', 'dss-config', 'object-formats', name + '.format.json'),
                          encoding='utf-8'))


def pair_facts_of(doc):
    return {'arch': doc.get('targetArch'), 'format': doc['format']['kind'], 'dataModel': doc.get('dataModel'),
            'longDoubleFormat': doc.get('longDoubleFormat')}


def native_target(tree, processor, kind):
    """The DSS target spec (`arch:format-doc`) of the leg's own executables: every object-format document of the host's
    kind for this processor that produces a `cli` artifact. Two that differ on a fact the descriptors are read with
    are refused — never settled by picking one."""
    if kind is None:
        return None, 'the host runs an executable format this program cannot recognize'
    found = []
    for path in sorted(glob.glob(os.path.join(tree, 'src', 'dss-config', 'object-formats', '*.format.json'))):
        doc = json.load(open(path, encoding='utf-8'))
        if doc['format']['kind'] == kind and doc.get('targetArch') == processor \
                and 'cli' in (doc.get('artifactProfiles') or []):
            found.append((doc['format']['name'], pair_facts_of(doc)))
    if not found:
        return None, 'no object-format document is a %s cli format for the processor %r' % (kind, processor)
    facts = {json.dumps(f, sort_keys=True) for _, f in found}
    if len(facts) != 1:
        return None, 'the %s cli formats for %r disagree on the pair facts: %s' % (
            kind, processor, ', '.join('%s=%s' % (n, json.dumps(f, sort_keys=True)) for n, f in found))
    return '%s:%s' % (processor, found[0][0]), None


def pair_facts(tree, target):
    return pair_facts_of(read_format_doc(tree, target.partition(':')[2]))


def when_matches(when, pair):
    """The reader's MATCH-ALL-SPECIFIED contract: every key a `when` names equals the pair's value; a key naming a
    fact the pair lacks (a format document that declares no long-double format) never matches."""
    return all(pair.get(k) is not None and pair.get(k) == v for k, v in when.items())


def pair_signature(sym, pair):
    """The signature text the pair reads, by the descriptor reader's rule: a flat string; or the ONE `variants` arm
    whose `when` the pair matches, else the `default` arm. None where the reader REFUSES the row (no arm and no
    default, or two arms) — reported, never replaced by some other arm's text."""
    sig = sym.get('signature')
    if isinstance(sig, str):
        return sig
    if not isinstance(sig, dict):
        return None
    arms = [v for v in sig.get('variants') or [] if isinstance(v, dict)]
    hit = [v['value'] for v in arms if isinstance(v.get('when'), dict) and when_matches(v['when'], pair)]
    if len(hit) == 1:
        return hit[0]
    if hit:
        return None
    dflt = [v['value'] for v in arms if v.get('default') is True]
    return dflt[0] if dflt else None


def pair_documents(tree, pair, only=None):
    root = os.path.join(tree, 'src', 'dss-config', 'shippedLibs')
    docs = []
    for f in sorted(glob.glob(os.path.join(root, '**', '*.json'), recursive=True)):
        d = json.load(open(f, encoding='utf-8'))
        rel = os.path.relpath(f, root).replace(os.sep, '/')
        if pair['format'] not in (d.get('availableObjectFormats') or ['elf', 'macho', 'pe']):
            continue
        if only and d.get('header') != only:
            continue
        docs.append((rel, d))
    return docs


def visible_symbols(tree, pair, only=None):
    root = os.path.join(tree, 'src', 'dss-config', 'shippedLibs')
    idents = {}
    for f in sorted(glob.glob(os.path.join(root, '**', '*.json'), recursive=True)):
        d = json.load(open(f, encoding='utf-8'))
        for st in d.get('structs', []):
            idents.setdefault(st['name'], STRUCT_SPELLING.get(st['name'], 'struct ' + st['name']))
        for td in d.get('typedefs', []):
            idents[td['name']] = td['name']
    rows = []
    for rel, d in pair_documents(tree, pair, only):
        seen = set()
        for s in d.get('symbols', []):
            if pair['format'] not in (s.get('availableObjectFormats') or ['elf', 'macho', 'pe']) or s['name'] in seen:
                continue
            seen.add(s['name'])
            rows.append({'rel': rel, 'header': d['header'], 'name': s['name'], 'kind': s.get('kind', 'function'),
                         'sig': pair_signature(s, pair)})
    return rows, idents


INTEGER_LITERAL = re.compile(r'^\(?\s*[-+]?\s*(0[xX][0-9a-fA-F]+|0[0-7]*|[1-9][0-9]*)[uUlL]*\s*\)?$')


def value_names(tree, pair):
    """{(rel, header): [name, …]} — every integer `constants` row and every macro some arm of which is an integer
    literal, of every descriptor the pair's format sees. NAMES ONLY: which arm (if any) serves the pair is DSS's own
    preprocessor's answer, read back from the program it compiles."""
    groups = {}
    for rel, d in pair_documents(tree, pair):
        names = []
        for c in d.get('constants', []):
            if c.get('preprocessorVisible') is False:
                continue
            if isinstance(c.get('value'), int) or any(isinstance(v.get('value'), int)
                                                      for v in c.get('variants') or [] if isinstance(v, dict)):
                names.append(c['name'])
        for m in d.get('macros', []):
            if 'params' in m:
                continue
            reps = [m.get('replacement')] + [v.get('replacement') for v in m.get('variants') or []
                                             if isinstance(v, dict) and 'params' not in v]
            if any(isinstance(r, str) and INTEGER_LITERAL.match(r.strip()) for r in reps):
                names.append(m['name'])
        if names:
            groups[(rel, d['header'])] = sorted(set(names))
    return groups


def values_program(header, names):
    """A header the compiler does not have prints `census_probe_header=absent` instead of failing to compile (Apple
    ships no <stdbit.h>, ✔MEASURED 2026-09-30); a name it defines prints its value, one it does not `nodef`."""
    body = []
    for n in names:
        body += ['#ifdef %s' % n, '    printf("%s=%%lld\\n", (long long)(%s));' % (n, n), '#else',
                 '    puts("%s=nodef");' % n, '#endif']
    return '\n'.join(['#include <stdio.h>', '#if __has_include(<%s>)' % header, '#include <%s>' % header,
                      'int main(void) {'] + body + ['    return 0;', '}', '#else', 'int main(void) {',
                      '    puts("census_probe_header=absent");', '    return 0;', '}', '#endif']) + '\n'


VALUE_LINE = re.compile(r'^([A-Za-z_][A-Za-z0-9_]*)=(nodef|absent|-?[0-9]+)$')


def parse_values(text):
    out = {}
    for line in (text or '').splitlines():
        m = VALUE_LINE.match(line.strip())
        if m:
            out[m.group(1)] = m.group(2)
    return out


# ── running things ───────────────────────────────────────────────────────────────────────────────────────────────────
def run(argv, cwd, timeout=300, env=None):
    """No program this census starts reads its standard input; it is closed, so one that tried (a compiler run with
    no arguments, to print its banner) cannot wait on the step's."""
    p = subprocess.run(argv, cwd=cwd, capture_output=True, text=True, timeout=timeout, env=env, encoding='utf-8',
                       errors='replace', stdin=subprocess.DEVNULL)
    return p.returncode, p.stdout, p.stderr


def split_flags(value):
    """`--flags`: comma-separated, each flag percent-decoded — the probe-reference-cc convention, so a comma INSIDE
    one flag (`-Wl,-z,noexecstack`) is written `%2C`."""
    return [unquote(item) for item in (value or '').split(',') if item != '']


# The three diagnostic dialects this program reads: how each is DRIVEN (its standard mode, its syntax-only check, the
# options that quiet a judge program's compile and name its image) and how its diagnostics name the address type of
# `&sym` a pointer initialization of `struct census_probe_sink *` refuses, an undeclared identifier, and a TU it
# stopped judging. ✔MEASURED 2026-09-30 on clang 18.1.3 and gcc 13.3.0 (x86_64 and aarch64): gcc quotes with ‘’ under a
# UTF-8 locale and with '' under the C locale, and neither stops (each told never to: `-ferror-limit=0`,
# `-fmax-errors=0`). ✔MEASURED 2026-10-07 on MSVC's cl 19.51.36260 (windows-x86_64-release, probe-reference-cc runs
# 20261007-192407-162eaca7 and 20261007-192415-f76f2e6f): every diagnostic on STDOUT as `<file>(<line>): warning
# C<code>: ...`; `/Zs` reports what `/c` does, at the default warning level as at /W3 and /W4; the type of `&sym` in
# C4047 "'initializing': 'census_probe_sink *' differs in levels of indirection from '<type>'" (a function, an array,
# a pointer to a pointer) or C4133 "'initializing': incompatible types - from '<type>' to 'census_probe_sink *'" (an
# object), the struct's tag word dropped; C2065 for an undeclared name; and "fatal error C1003: error count exceeds
# 100; stopping compilation" on the line it stopped at, with nothing after it judged. `typeof` needs `/std:clatest`
# (`/std:c11` refuses it, C2061), which leaves `__STDC__` undefined, so the UCRT also declares its non-standard names.
# A const or noreturn function's address type, as gcc and clang PRINT it, carries the attribute as a decoration
# neither takes as part of a type. gcc prints it first, `__attribute__((const)) double (*)(double)`, a spelling gcc
# and clang both accept and ignore with a warning (gcc: "attribute does not apply to types"; clang: "attribute ignored
# when parsing type"). clang prints noreturn last, `void (*)(int) __attribute__((noreturn))`, which no compiler
# accepts as a type name, clang included. ✔MEASURED 2026-10-08 on gcc 13.2.0 (MinGW-w64), gcc 13.3.0 and Apple clang
# 21.0.0 (probe-reference-cc runs 20261008-030455-c2dbd912, 20261008-030523-2d321664, 20261008-030734-bf53356e and
# 20261008-030745-1b0463d9); cl prints neither. The census hands on the TYPE, so a decoration is removed -- by this
# CLOSED list, never by a pattern over attributes: per dialect, the exact text, the END of the spelling the dialect
# prints it at, and the WITNESS -- what the reference itself says when that text is written there in a type name, held
# to the leg's own compiler by the self-test (arm `d-witness`). That what is removed is no part of the type BY THE
# REFERENCE'S OWN JUDGEMENT is not a property of the list, though: it is WITNESSED ROW BY ROW (keep_unwitnessed) --
# a removal is made only where the reference's own `_Generic` takes the bare spelling as the type of that row's
# symbol. ✔MEASURED 2026-10-08, gcc 13.2.0 (probe-reference-cc run 20261008-085400-883c8a53): the address of an
# OBJECT declared `void (*p)(int) __attribute__((noreturn))` is spelled `__attribute__((noreturn)) void (**)(int)`,
# the nested function type's noreturn at the front too, and gcc's `_Generic` answers NO to `void (**)(int)` for it
# -- where it answers yes to the bare type of a noreturn FUNCTION's address; Apple clang 21 prints that object's
# type `void (**)(int) __attribute__((noreturn))` and DOES take the bare type (run 20261008-085224-eb714084). A
# row whose removal the reference does not witness keeps the reference's own spelling and is in no count. The
# self-test holds the witness on a plain function (arms `d-noreturn`, `d-const`), on that object (`d-nested`) and,
# with no compiler, the rule itself (`d-unwitnessed`). Any other attribute, one at the other end and one inside a
# type stay in the spelling and are refused by name downstream. The census line counts the rows a decoration was
# removed from (`ref-decoration-dropped`) and the report names what was removed, row by row: a row judged after a
# removal never reads like a row judged as spelled.
DECORATIONS = {
    'gcc': (('__attribute__((const))', 'leading', 'attribute does not apply to types'),
            ('__attribute__((noreturn))', 'leading', 'attribute does not apply to types')),
    'clang': (('__attribute__((noreturn))', 'trailing', "expected ')'"),),
    'msvc': (),
}
Q_OPEN, Q_CLOSE = "['‘]", "['’]"
# gcc's and clang's line that says a name is UNDECLARED is read on the TU's OWN lines only, as cl's lines are
# (MSVC_LINE): the file name the compiler prints, at the start of a line or after a directory separator, then
# `:<line>:<column>: error: `. A header's own undeclared name sits on a line whose NUMBER a row's line can share, and
# is not that row's (✔MEASURED 2026-10-08, probe-reference-cc runs 20261008-085224-eb714084 and
# 20261008-085400-883c8a53: the header's `<header>:<line>:<column>: error: …` beside the TU's own, on other lines,
# gcc 13.2.0 and Apple clang 21). The `%s` is the TU's name, escaped. The SPELLING patterns need no such anchor: they
# name `struct census_probe_sink`, which only the TU declares, after its includes.
GNU_OWN_LINE = r"(?m)(?:^|[\\/])%s:(\d+):\d+: error: "
DIALECTS = {
    'clang': {
        'spell': re.compile(r":(\d+):\d+: (?:warning|error): incompatible pointer types initializing "
                            r"'struct census_probe_sink \*' with an expression of type '([^']*)'"
                            r"(?: \(aka '([^']*)'\))?"),
        'undeclared': GNU_OWN_LINE + r"(?:use of undeclared identifier|call to undeclared)",
        'standard': ['-std=gnu2x', '-D_GNU_SOURCE'],
        'syntax': ['-fsyntax-only', '-ferror-limit=0'],
        'quiet': ['-w'],
        'output': ['-o', '{exe}'],
        'tagless': False,
        'generic_keeps_restrict': False,
    },
    'gcc': {
        'spell': re.compile(r":(\d+):\d+: (?:warning|error): initialization of %sstruct census_probe_sink \*%s from "
                            r"incompatible pointer type %s([^‘’']*)%s(?: \{aka %s([^‘’']*)%s\})?"
                            % (Q_OPEN, Q_CLOSE, Q_OPEN, Q_CLOSE, Q_OPEN, Q_CLOSE)),
        'undeclared': GNU_OWN_LINE + r"%s[^‘’']*%s undeclared" % (Q_OPEN, Q_CLOSE),
        'standard': ['-std=gnu2x', '-D_GNU_SOURCE'],
        'syntax': ['-fsyntax-only', '-fmax-errors=0'],
        'quiet': ['-w'],
        'output': ['-o', '{exe}'],
        'tagless': False,
        'generic_keeps_restrict': False,
    },
    'msvc': {
        'spell': None,                     # read by msvc_diagnostics(): by CODE, never by the message's words
        'undeclared': None,
        'standard': ['/std:clatest'],
        'syntax': ['/nologo', '/Zs', '/diagnostics:classic'],
        'quiet': ['/nologo', '/w'],
        'output': ['/Fe{exe}'],
        # cl spells a struct, union or enum type WITHOUT its tag word (`tm *` for `struct tm *`, ✔MEASURED
        # 2026-10-07, probe-reference-cc run 20261007-212436-45695ebb), which is no C type to hand DSS: the census
        # asks cl which spelled names are only tags, and of which kind (reference_tag_kinds), and puts the word back.
        'tagless': True,
        # cl's `_Generic` does not erase a PARAMETER's top-level `restrict` when it compares function types
        # (✔MEASURED 2026-10-07, cl 19.51.36260, probe-reference-cc run 20261008-014747-3f3f43d5: `void f(char
        # *restrict)` against `void (*)(char *)` is NO, `__restrict` the same, and a top-level `const` it does
        # erase), where C 6.7.6.3p15 takes the parameter unqualified and gcc 13, Apple clang 21 and DSS answer yes
        # (runs 20261008-014747-3f3f43d5 and 20261008-014812-7accf3cb). A descriptor's text cannot spell that
        # qualifier, so cl's plain NO on a row whose header declares one says nothing about the row -- but cl DOES
        # answer when the qualifier is on both sides or on neither (✔MEASURED 2026-10-08, run
        # 20261008-085400-883c8a53: its own spelling against `&sym` is yes; that spelling without its parameters'
        # restrict, cast onto 0, is yes against the right text and NO against a wrong one; the same cast with the
        # qualifier kept is NO against the right text, so it is the removal that makes it answer; a callback's
        # parameter is kept the same way; a POINTEE's restrict is part of the type there as everywhere). So such a
        # NO is asked again in that form (asked_again), and the self-test holds each dialect's entry, and the
        # form, to the leg's own compiler.
        'generic_keeps_restrict': True,
    },
}
# cl's own line, read by its CODE: `<file>(<line>): warning|error|fatal error C<nnnn>: <message>`, on the TU's own
# lines only (a header's diagnostic names the header's file). The two codes that spell `&sym`'s type quote three
# things — the context ('initializing'), the sink and the type, in an order that differs by code — so the type is the
# one quoted string after the context that is not the sink, whatever words the message uses around it. The severity
# words and the banner are cl's English ones, as measured: a cl whose banner is another is not identified, and the
# census refuses it by name; a line whose severity word is another is not read, so its row is REF-ERROR, never a
# spelling.
MSVC_LINE = r"(?:^|[\\/])%s\((\d+)\)\s*:\s*(?:warning|error|fatal error)\s+(C\d{4})\s*:(.*)$"
MSVC_SPELL_CODES = ('C4047', 'C4133')
MSVC_SINK = ('census_probe_sink *', 'struct census_probe_sink *')
MSVC_QUOTED = re.compile(r"'([^']*)'")
# cl answers no `--version`; with no arguments it prints this banner (to stderr) and its usage, exiting 0.
MSVC_BANNER = re.compile(r"Microsoft \(R\) C/C\+\+ Optimizing Compiler Version (\S+) for (\S+)")


def output_args(dialect, exe):
    return [a.format(exe=exe) for a in DIALECTS[dialect]['output']]


def msvc_diagnostics(text, tu):
    """cl's diagnostics of the TU `tu`: ({line: (declared, canonical)}, {undeclared lines}, the line it stopped at or
    None). cl prints no `aka` form, so its spelling is both the declared and the canonical one."""
    line_re = re.compile(MSVC_LINE % re.escape(tu))
    found, undecl, stopped = {}, set(), None
    for raw in text.splitlines():
        m = line_re.search(raw.strip())
        if not m:
            continue
        ln, code, message = int(m.group(1)), m.group(2), m.group(3)
        if code == 'C2065':
            undecl.add(ln)
        elif code == 'C1003':
            stopped = ln if stopped is None else min(stopped, ln)
        elif code in MSVC_SPELL_CODES:
            quoted = MSVC_QUOTED.findall(message)
            rest = [q for q in quoted[1:] if q not in MSVC_SINK]
            if len(quoted) == 3 and len(rest) == 1:
                found.setdefault(ln, (rest[0], rest[0]))
    return found, undecl, stopped


def undecorated(spelling, dialect):
    """(`spelling` without the decorations its dialect prints at an end of a type and does not take as part of it,
    the texts removed) -- by DECORATIONS, the exact text at the end its entry names, and nothing else."""
    removed, again = [], True
    while again:
        again = False
        for text, where, _ in DECORATIONS[dialect]:
            if where == 'leading' and spelling.startswith(text + ' '):
                spelling, again = spelling[len(text):].lstrip(' '), True
            elif where == 'trailing' and spelling.endswith(' ' + text):
                spelling, again = spelling[:-len(text)].rstrip(' '), True
            else:
                continue
            removed.append(text)
    return spelling, removed


def spelling_diagnostics(dialect, text, tu):
    """({line: (declared, canonical)}, {undeclared lines}, the line the compiler stopped at or None, {line: (the
    decorations removed from its spelling, the (declared, canonical) spelling AS PRINTED)}) for the TU `tu`. The
    spelling as printed is kept beside a removal because the removal is still to be witnessed, row by row
    (keep_unwitnessed)."""
    if dialect == 'msvc':
        return msvc_diagnostics(text, tu) + ({},)
    forms = DIALECTS[dialect]
    found, gone = {}, {}
    for m in forms['spell'].finditer(text):
        printed = (m.group(2), m.group(3) or m.group(2))
        declared, removed = undecorated(printed[0], dialect)
        canonical, removed_too = undecorated(printed[1], dialect)
        found[int(m.group(1))] = (declared, canonical)
        if removed or removed_too:
            gone[int(m.group(1))] = (' '.join(dict.fromkeys(removed + removed_too)), printed)
    undecl = {int(m.group(1)) for m in re.finditer(forms['undeclared'] % re.escape(tu), text)}
    return found, undecl, None, gone


KEPT_LIMIT = 600


def kept_message(text, limit=KEPT_LIMIT):
    """A compiler's or a judge's message as a report cell keeps it: ONE line (every run of whitespace a single
    space, so a tab or a line break never splits a row of the report), whole up to `limit` characters. Past that its
    START and its END are both kept, around a `[...]` that says something was left out: a message opens with its
    code, and a parse refusal of DSS's closes with what it met (`… — got ')'`) after a list of what it expected
    that outgrows any bound (✔MEASURED 2026-10-08, the Mac's plain census, run 20261008-101158-9c7dd27f: two
    `P_NoAlternativeMatched` cells past 600 characters, cut before those closing words while only the start was
    kept). The end is given a quarter of the bound. Each side is cut at a word's END -- never mid-word, never
    silently; an end that holds no word boundary is left out whole, and a start that holds none is cut at its
    share of the bound, there being no word to end on."""
    line = ' '.join((text or '').split())
    if len(line) <= limit:
        return line
    tail = limit // 4
    cut = line.rfind(' ', 0, limit - tail + 1)
    back = line.find(' ', max(len(line) - tail - 1, cut + 1))
    head = line[:cut] if cut > 0 else line[:limit - tail]
    return head + ' [...]' + (line[back:] if back >= 0 else '')


# ── the tag word cl drops ────────────────────────────────────────────────────────────────────────────────────────
# The words of a cl spelling that are never a name to classify: the qualifiers, calling conventions and type
# keywords it prints, and `bool`, its spelling of `_Bool`, which is no type name in a C TU that does not include
# <stdbool.h> (✔MEASURED 2026-10-07: cl refuses `typedef bool x;` with C2061 there). A type KEYWORD left off this
# list is harmless (cl accepts it as a type, so it keeps its spelling); a qualifier or a calling convention left off
# would be read as a tag, which is why those are listed.
SPELLING_WORDS = frozenset(('const', 'volatile', 'restrict', '__restrict', '_Atomic', '__unaligned', '__ptr32',
                            '__ptr64', '__w64', '__cdecl', '__stdcall', '__fastcall', '__vectorcall', '__thiscall',
                            '__clrcall', 'signed', 'unsigned', 'char', 'short', 'int', 'long', 'float', 'double',
                            'void', '_Bool', 'bool', '_Complex', '__int8', '__int16', '__int32', '__int64',
                            'struct', 'union', 'enum'))
TAG_WORDS = ('struct', 'union', 'enum')
SPELLED_WORD = re.compile(r'[A-Za-z_][A-Za-z0-9_]*')
# C2011's "'<name>': '<kind>' type redefinition", the kind a C keyword in quotes (✔MEASURED 2026-10-07, run
# 20261007-212436-45695ebb: `struct <a union's tag> *` -> 'union', `union tm *` -> 'struct').
MSVC_TAG_KIND = re.compile(r"'([^']*)'\s*:\s*'(struct|union|enum)'")


def tagless_names(spelling):
    """The names of a spelling that may be a tag cl printed without its word: every word that is not one of
    SPELLING_WORDS and does not already follow a tag word."""
    out, prev = [], None
    for w in SPELLED_WORD.findall(spelling):
        if w not in SPELLING_WORDS and prev not in TAG_WORDS and w not in out:
            out.append(w)
        prev = w
    return out


def tagless_candidates(spellings):
    """The names to ask a tagless reference about, over rows' (declared, canonical) spellings: each ONCE, in the
    order first seen. cl's two spellings of a row are one string, and a name asked twice doubles its lines in the tag
    TU and halves what a round judges before the 100-error stop (✔MEASURED 2026-10-07: cl judges 50 struct tags a
    round, probe-reference-cc run 20261008-023549-32645222; asked twice, it reached 25)."""
    names = []
    for pair in spellings:
        for spelling in pair:
            for n in tagless_names(spelling):
                if n not in names:
                    names.append(n)
    return names


def with_tags(spelling, kinds):
    """`spelling` with each name of `kinds` ({name: 'struct'|'union'|'enum'}) preceded by its tag word, as a whole
    word and only where no tag word precedes it already."""
    def sub(m):
        return '%s %s' % (kinds[m.group(0)], m.group(0)) if m.group(0) in kinds else m.group(0)
    return re.sub(r'(?<![A-Za-z0-9_])(?<!struct )(?<!union )(?<!enum )[A-Za-z_][A-Za-z0-9_]*', sub, spelling)


def msvc_tag_kinds(text, tu, names, first):
    """cl's answer to a tag TU whose line `first + 2*i` is `typedef <names[i]> census_probe_type_<i>;` and line
    `first + 2*i + 1` is `struct <names[i]> *census_probe_tag_<i>;`. -> ({name: kind} for every name that is NOT a
    type -- an error, a C2xxx code, on its typedef line; ✔MEASURED 2026-10-07: C2061 for a tag, then C2059 -- of the
    kind C2011 names on its struct line, else 'struct'}, the line cl stopped at or None)."""
    line_re = re.compile(MSVC_LINE % re.escape(tu))
    not_type, kind, stopped = set(), {}, None
    for raw in text.splitlines():
        m = line_re.search(raw.strip())
        if not m:
            continue
        ln, code, message = int(m.group(1)), m.group(2), m.group(3)
        if code == 'C1003':
            stopped = ln if stopped is None else min(stopped, ln)
            continue
        i, which = divmod(ln - first, 2)
        if not 0 <= i < len(names):
            continue
        if which == 0 and code.startswith('C2'):
            not_type.add(names[i])
        elif which == 1 and code == 'C2011':
            k = MSVC_TAG_KIND.search(message)
            if k:
                kind[names[i]] = k.group(2)
    return {n: kind.get(n, 'struct') for n in names if n in not_type}, stopped


def reference_tag_kinds(header, names, cc, flags, dialect, work):
    """{name: kind} for the names of a tagless dialect's spellings under <header> that are only a TAG, asked of the
    reference itself in one TU per round; a TU it stopped judging has its later names re-run, and a TU that judged
    none is never looped on -- those names keep the reference's spelling, which DSS then refuses by name."""
    if dialect != 'msvc':
        raise ValueError('the %s dialect drops tag words and has no reader for its tag TU' % dialect)
    forms, kinds, pending = DIALECTS[dialect], {}, list(names)
    while pending:
        lines = ['#include <%s>' % header]
        first = len(lines) + 1
        for i, n in enumerate(pending):
            lines += ['typedef %s census_probe_type_%d;' % (n, i), 'struct %s *census_probe_tag_%d;' % (n, i)]
        with open(os.path.join(work, 'tags.c'), 'w', encoding='utf-8', newline='\n') as o:
            o.write('\n'.join(lines) + '\n')
        _, so, se = run([cc] + forms['standard'] + flags + forms['syntax'] + ['tags.c'], work)
        got, stopped = msvc_tag_kinds(so + se, 'tags.c', pending, first)
        judged = [n for i, n in enumerate(pending) if stopped is None or first + 2 * i + 1 < stopped]
        kinds.update((n, k) for n, k in got.items() if n in judged)
        if not judged:
            break
        pending = [n for n in pending if n not in judged]
    return kinds


def row_verdict(ln, found, undecl):
    """An undeclared name is REF-UNDECLARED even where its line ALSO carries a spelling: cl takes an undeclared
    identifier for an `int` and adds a C4133 naming 'int *' on the same line (✔MEASURED 2026-10-07), which spells no
    symbol at all."""
    if ln in undecl:
        return 'REF-UNDECLARED'
    return found.get(ln, 'REF-ERROR')


# The compilers whose dialect this program reads, under the names a PATH holds them by.
REFERENCES_READ = ('clang', 'gcc', 'cl')


def absent_reference(cc, which=shutil.which):
    """The refusal of a reference compiler the host does not have, in this program's words rather than the system's
    (which are in the host's language, and are not the same from host to host — `reference_dialect` says how the
    absence is decided): which program is missing, that the census has no reference of its own and never picks one,
    how a run names the leg's, and which of the compilers it reads the step's PATH holds — a list to choose from,
    never a choice: which of them the LEG builds with is the leg's fact, and a PATH does not say."""
    here = [name for name in REFERENCES_READ if which(name)]
    return ('the reference compiler %r is not on this host: no program of that name can be started (`--cc`; on a '
            'run, the step\'s `cc` input). The census has no reference of its own and never picks one: name the '
            'compiler this leg builds with, `--input cc=<name>` on a run. Of the compilers it reads (%s), this '
            'step\'s PATH holds: %s' % (cc, ', '.join(REFERENCES_READ), ', '.join(here) or 'none of them'))


def reference_dialect(cc, work, start=None, which=shutil.which):
    """clang, gcc or msvc, read from the compiler itself; None (with what it printed, or why it could not be started)
    for anything else. clang and gcc answer `--version`; cl answers no `--version` and prints its banner when run with
    no arguments. `start` and `which` are what starts a program and what looks a name up on the PATH — `run` and
    `shutil.which` — named so that the self-test holds each way a start fails, on every leg."""
    start = start or run
    if not cc:
        return None, 'no reference compiler is named (--cc is empty)'
    try:
        rc, so, se = start([cc, '--version'], work, timeout=60)
    except (OSError, subprocess.SubprocessError) as e:
        # WHETHER THE PROGRAM IS WHAT IS MISSING is asked of the PATH, never read off the error, which is not the same
        # from host to host: a name no directory holds is "file not found" on Windows and macOS and "permission
        # denied" on a host one of whose PATH entries refuses the lookup (✔MEASURED 2026-10-08 on the WSL leg:
        # `[Errno 13]`, and `[Errno 2]` once that one entry is taken off the PATH), and a working directory
        # that does not exist is "file not found" too wherever the directory is entered before the program is looked
        # for (✔MEASURED on macOS: `[Errno 2]`, naming the directory). So a compiler is ABSENT when its start failed,
        # the directory it was to run in exists, and the PATH holds no file of that name, executable or not; a file
        # that is there and cannot be started keeps the system's words.
        if isinstance(e, OSError) and os.path.isdir(work) and which(cc, os.F_OK) is None:
            return None, absent_reference(cc, which)
        return None, 'cannot run %r --version: %s' % (cc, e)
    banner = (so + se).strip()
    first = banner.splitlines()[0] if banner else ''
    if rc == 0 and 'clang' in first.lower():
        return 'clang', first
    if rc == 0 and ('gcc' in first.lower() or 'Free Software Foundation' in banner):
        return 'gcc', first
    try:
        rc2, so2, se2 = start([cc], work, timeout=60)
    except (OSError, subprocess.SubprocessError) as e:
        return None, 'the reference %r is neither clang nor gcc (banner: %r), and cannot run alone: %s' % (cc, first, e)
    m = MSVC_BANNER.search(so2 + se2)
    if rc2 == 0 and m:
        return 'msvc', m.group(0)
    return None, 'the reference %r is neither clang, gcc nor cl (its --version banner: %r)' % (cc, first)


def reference_spellings(rows, cc, flags, dialect, work, dropped=None):
    """{(rel, name): (declared, canonical)} — the reference's spelling of the TYPE OF `&sym` — or a verdict string.
    A compiler that STOPS judging a TU (cl at 100 errors, C1003) has every row from the line it stopped at re-run in a
    TU of its own, until each row is judged; a TU that judged none of its rows is never looped on — each of those
    rows is REF-ERROR. `dropped`, a mapping the caller hands in, receives {(rel, name): the decorations removed from
    that row's spelling} (DECORATIONS) -- for the rows whose removal the reference itself witnessed, and no other
    (keep_unwitnessed)."""
    forms = DIALECTS[dialect]
    out, groups = {}, {}
    for r in rows:
        groups.setdefault((r['rel'], r['header']), []).append(r)
    for (rel, header), items in groups.items():
        pending, removed = list(items), {}
        while pending:
            # A header the reference does not have at all is its own verdict, REF-NO-HEADER, read from a marker of
            # this program's own (`__has_include`, the values step's rule) — never inferred from a missing spelling.
            lines = ['#if __has_include(<%s>)' % header, '#include <%s>' % header,
                     'struct census_probe_sink { int x; };']
            first = len(lines) + 1
            lines += ['struct census_probe_sink *census_probe_%d = &%s;' % (i, r['name'])
                      for i, r in enumerate(pending)]
            lines += ['#else', '#error census_probe_header_absent', '#endif']
            with open(os.path.join(work, 'ref.c'), 'w', encoding='utf-8', newline='\n') as o:
                o.write('\n'.join(lines) + '\n')
            _, so, se = run([cc] + forms['standard'] + flags + forms['syntax'] + ['ref.c'], work)
            text = so + se
            if 'census_probe_header_absent' in text:
                for r in pending:
                    out[(rel, r['name'])] = 'REF-NO-HEADER'
                break
            found, undecl, stopped, gone = spelling_diagnostics(dialect, text, 'ref.c')
            rest = []
            for i, r in enumerate(pending):
                ln = first + i
                if stopped is not None and ln >= stopped:
                    rest.append(r)
                    continue
                out[(rel, r['name'])] = row_verdict(ln, found, undecl)
                if ln in gone and isinstance(out[(rel, r['name'])], tuple):
                    removed[r['name']] = gone[ln]
            if len(rest) == len(pending):
                for r in rest:
                    out[(rel, r['name'])] = 'REF-ERROR'
                break
            pending = rest
        if removed:
            held = keep_unwitnessed(rel, header, removed, out, ref_runner(cc, flags, dialect, work))
            if dropped is not None:
                dropped.update(((rel, name), texts) for name, texts in held.items())
        if forms['tagless']:
            keys = [(rel, r['name']) for r in items if isinstance(out.get((rel, r['name'])), tuple)]
            names = tagless_candidates(out[k] for k in keys)
            kinds = reference_tag_kinds(header, names, cc, flags, dialect, work) if names else {}
            for k in keys:
                out[k] = (with_tags(out[k][0], kinds), with_tags(out[k][1], kinds))
    return out


def verdict_program(header, lines, prelude=''):
    return '\n'.join(['#include <stdio.h>', '#include <%s>' % header, prelude, 'int main(void) {'] + lines +
                     ['    return 0;', '}']) + '\n'


def run_verdicts(groups, compile_and_run, tag=''):
    """groups: {(rel, header): [(name, generic-association-type[, the controlling expression])]} → {(rel, name):
    verdict}; isolates refusals. The controlling expression is `&name` unless the item gives its own (asked_again
    asks about a TYPE, cast onto 0). `tag` prefixes the scratch names of this call's programs."""
    def asks(item):
        return '    puts(_Generic(%s, %s: "yes %s", default: "NO %s"));' % (
            item[2] if len(item) > 2 else '&' + item[0], item[1], item[0], item[0])
    out = {}
    for (rel, header), items in groups.items():
        text = verdict_program(header, [asks(item) for item in items], prelude=MARKERS)
        so, err = compile_and_run(text, tag + 'all')
        if so is not None:
            for line in so.splitlines():
                v, _, n = line.partition(' ')
                out[(rel, n)] = v
            continue
        for k, item in enumerate(items):
            so, err = compile_and_run(verdict_program(header, [asks(item)], prelude=MARKERS), '%sone%d' % (tag, k))
            out[(rel, item[0])] = so.split(' ', 1)[0] if so else None, err
    return out


def verdict_of(value):
    """(the word a judge printed or None, why it printed none) of one run_verdicts value: a row judged in its
    header's whole program is the bare word, an isolated row a pair."""
    return value if isinstance(value, tuple) else (value, None)


def keep_unwitnessed(rel, header, removed, spellings, judge):
    """THE WITNESS OF A REMOVAL, row by row. `removed`: {name: (the decorations removed, the spelling as printed)} of
    one header's rows; `spellings`: {(rel, name): (declared, canonical)}, the bare ones. → {name: the decorations
    removed} for the rows whose BARE canonical spelling the reference's own `_Generic` takes as the type of `&name`
    (`judge`: the reference's compile-and-run). Every other row -- a NO, a spelling the reference will not compile,
    no answer at all -- gets the spelling as printed back in `spellings` and is in no count: what the reference
    does not witness is not removed."""
    asked = run_verdicts({(rel, header): [(name, spellings[(rel, name)][1]) for name in removed]}, judge, 'witness_')
    held = {}
    for name, (texts, printed) in removed.items():
        if verdict_of(asked.get((rel, name)))[0] == 'yes':
            held[name] = texts
        else:
            spellings[(rel, name)] = printed
    return held


def judged_output(argv, work):
    """Run a judge program. Its stdout is returned ONLY when it exited 0: a judge that crashed printed a partial list
    of verdicts, and reading them would report the symbols it never reached as missing instead of the crash."""
    rc, so, se = run(argv, work, timeout=60)
    if rc != 0:
        return None, 'JUDGE-FAILED:exit %d %s' % (rc, kept_message((se.strip().splitlines() or [''])[0]))
    return so, None


def dss_runner(dsscp, target, tree, work):
    env = dict(os.environ, DSS_CONFIG_ROOT=tree)

    def go(text, tag):
        src = os.path.join(work, 'dss_%s.c' % tag)
        with open(src, 'w', encoding='utf-8', newline='\n') as o:
            o.write(text)
        outdir = os.path.join(work, 'dss_out_%s' % tag)
        shutil.rmtree(outdir, ignore_errors=True)
        rc, so, se = run([dsscp, '--compile', src, '--language', 'c', '--target', target, '--output', outdir], work,
                         env=env)
        if rc != 0:
            return None, kept_message(next((l for l in (so + se).splitlines() if 'error[' in l), so + se))
        stem = 'dss_%s' % tag
        exe = [p for p in glob.glob(os.path.join(outdir, '**', '*'), recursive=True)
               if os.path.isfile(p) and os.path.basename(p) in (stem, stem + '.exe', 'main', 'main.exe')]
        if not exe:
            return None, 'no artifact'
        os.chmod(exe[0], 0o755)
        return judged_output([exe[0]], work)
    return go


def ref_runner(cc, flags, dialect, work):
    """The reference compiles and links a judge program, in its own dialect's spelling, and runs it. cl prints its
    errors on stdout, gcc and clang on stderr, so both are read. The program is compiled by a name RELATIVE to
    `work`, the compiler's working directory, as the spelling and tag TUs are: a compiler names the file in its
    diagnostics as it was named to it, and a report cell that kept one would otherwise open with the scratch
    directory's path. (The built judge is RUN by its full path: a relative program name is resolved against the
    parent's directory on Windows, not the child's.)"""
    forms = DIALECTS[dialect]

    def go(text, tag):
        src, exe = 'ref_%s.c' % tag, 'ref_%s.exe' % tag
        with open(os.path.join(work, src), 'w', encoding='utf-8', newline='\n') as o:
            o.write(text)
        rc, so, se = run([cc] + forms['standard'] + flags + forms['quiet'] + output_args(dialect, exe) + [src], work)
        if rc != 0:
            said = so + se
            return None, kept_message(next((l for l in said.splitlines() if 'error' in l), said))
        return judged_output([os.path.join(work, exe)], work)
    return go


# ── a parameter's restrict, on a reference whose `_Generic` keeps it ────────────────────────────────────────────
# The words a reference spells a `restrict` qualifier with (cl 19.51 prints `restrict` for `__restrict` too), the
# words a pointer's qualifier run can hold in a reference's spelling, and the calling conventions a declarator group
# can open with (`(__cdecl *)`).
RESTRICT_WORDS = frozenset(('restrict', '__restrict'))
POINTER_QUALIFIERS = frozenset(('const', 'volatile', 'restrict', '__restrict', '_Atomic', '__unaligned', '__ptr32',
                                '__ptr64', '__w64'))
CALLING_WORDS = frozenset(('__cdecl', '__stdcall', '__fastcall', '__vectorcall', '__thiscall', '__clrcall'))
SPELLING_TOKEN = re.compile(r'[A-Za-z_][A-Za-z0-9_]*|\.\.\.|\S')


def without_parameter_restrict(spelling):
    """(`spelling` with every `restrict` that is a PARAMETER's own top-level qualifier removed, how many were
    removed, [where] for each `restrict` this program cannot place).

    A parameter's own qualifier is no part of a function's type (C 6.7.6.3p15), so removing one leaves the type the
    type it was; a POINTEE's restrict is part of the type and must stay (✔MEASURED 2026-10-08 on gcc 13.2.0 and Apple
    clang 21, probe-reference-cc runs 20261008-085400-883c8a53 and 20261008-085224-eb714084: their `_Generic` takes
    the spelling with a parameter's restrict removed as the same type -- a plain parameter, a callback's, a restrict
    pointer to an array, one beside a const -- and answers NO when a pointee's is removed). The rule reads the
    spelling of an ADDRESS type, whose own top-level pointer carries no qualifier. A `restrict` in the qualifier run
    of a `*` is
      a PARAMETER's, removed, when the run ENDS a parameter: the next token is a `,` of a parameter list, the `)`
        that closes one, or the `)` of a declarator group (`(*restrict )` of a pointer to an array) that sits,
        through declarator groups only, in a parameter list -- at any depth of function types;
      a POINTEE's, kept, when a `*`, a `(` or a `[` follows it: the restrict pointer is then what something points
        to, returns or holds (a callback's RETURN type is the case two references judge differently -- gcc drops
        the qualifier, clang keeps it -- and each goes on answering that one its own way);
      UNPLACED anywhere else -- after no `*`, at the end of the whole spelling, in parentheses that are neither a
        declarator group nor a parameter list (an `_Atomic(`, a `typeof(`) -- never removed, and reported.
    A parenthesis opens a DECLARATOR group when a `*` is its first token after any calling-convention words, a
    PARAMETER LIST when it follows a `)`, and neither otherwise."""
    tokens = [(m.group(0), m.start(), m.end()) for m in SPELLING_TOKEN.finditer(spelling)]

    def group_kind(i):
        j = i + 1
        while j < len(tokens) and tokens[j][0] in CALLING_WORDS:
            j += 1
        if j < len(tokens) and tokens[j][0] == '*':
            return 'declarator'
        return 'parameters' if i > 0 and tokens[i - 1][0] == ')' else 'other'

    def where(k):
        return spelling[max(0, tokens[k][1] - 16):tokens[k][2] + 8].strip()

    stack, cut, unplaced, i = [], [], [], 0
    while i < len(tokens):
        word = tokens[i][0]
        if word == '(':
            stack.append(group_kind(i))
        elif word == ')':
            if stack:
                stack.pop()
        elif word == '*':
            j = i + 1
            while j < len(tokens) and tokens[j][0] in POINTER_QUALIFIERS:
                j += 1
            mine = [k for k in range(i + 1, j) if tokens[k][0] in RESTRICT_WORDS]
            after = tokens[j][0] if j < len(tokens) else None
            if after == ',':
                placed = 'parameter' if stack and stack[-1] == 'parameters' else None
            elif after == ')':
                depth = len(stack) - 1
                while depth >= 0 and stack[depth] == 'declarator':
                    depth -= 1
                placed = 'parameter' if depth >= 0 and stack[depth] == 'parameters' else None
            else:
                placed = 'pointee' if after in ('*', '(', '[') else None
            for k in mine:
                if placed == 'parameter':
                    cut.append((tokens[k][1], tokens[k][2]))
                elif placed is None:
                    unplaced.append(where(k))
            i = j
            continue
        elif word in RESTRICT_WORDS:
            unplaced.append(where(i))
        i += 1
    bare = spelling
    for start, end in reversed(cut):
        bare = bare[:start] + bare[end:]
    return bare, len(cut), unplaced


def asked_again(groups, spell, keys, runner):
    """Verdict (b) of the rows `keys`, put to a reference whose `_Generic` keeps a parameter's `restrict` in the form
    that reference ANSWERS. Per row: THE WITNESS first -- its own spelling must be the type of `&name` by its own
    `_Generic` (the qualifier on both sides), so that what is asked next is asked about the symbol's type and no
    other; then THE QUESTION WITH THE QUALIFIER ON NEITHER SIDE -- the descriptor's text against that spelling
    without its parameters' restrict, cast onto 0. → {key: 'yes' | 'NO' | a JUDGE-FAILED cell | a REF-CANNOT-JUDGE
    cell carrying its own reason}, for the rows that question changes or cannot be put for; a row whose spelling
    holds a `restrict` in no parameter's place is not in it (its plain verdict stands: a pointee's restrict is part
    of the type, and no descriptor spells one)."""
    out, witness, question = {}, {}, {}
    for group, items in groups.items():
        for name, text in items:
            key = (group[0], name)
            if key not in keys:
                continue
            bare, removed, unplaced = without_parameter_restrict(spell[key][1])
            if unplaced:
                out[key] = ('REF-CANNOT-JUDGE:its spelling holds a restrict this program cannot place (near `%s`)'
                            % kept_message(unplaced[0]))
            elif removed:
                witness.setdefault(group, []).append((name, spell[key][1]))
                question.setdefault(group, []).append((name, text, '(%s)0' % bare))
    taken, ask = run_verdicts(witness, runner, 'witness_'), {}
    for group, items in question.items():
        for item in items:
            key = (group[0], item[0])
            word, why = verdict_of(taken.get(key))
            if word == 'yes':
                ask.setdefault(group, []).append(item)
            elif (why or '').startswith('JUDGE-FAILED'):
                out[key] = why
            else:
                out[key] = ('REF-CANNOT-JUDGE:the reference does not take its own spelling as the type of the '
                            'symbol (%s)' % ('its _Generic answered %s' % word if word else why or 'no answer'))
    answers = run_verdicts(ask, runner, 'bare_')
    for group, items in ask.items():
        for item in items:
            key = (group[0], item[0])
            word, why = verdict_of(answers.get(key))
            if word in ('yes', 'NO'):
                out[key] = word
            elif (why or '').startswith('JUDGE-FAILED'):
                out[key] = why
            else:
                out[key] = ("REF-CANNOT-JUDGE:the reference refused the question put without the parameters' "
                            "restrict (%s)" % (why or 'no answer'))
    return out


def reference_verdicts(groups, spell, runner, forms):
    """Verdict (b) of every row of `groups` ({(rel, header): [(name, the rendered type of `&name`)]}): the
    reference's `_Generic(&name, <text>: …)`. On a reference whose `_Generic` keeps a parameter's `restrict` (the
    dialect's `generic_keeps_restrict`), a NO on a row whose spelling holds that word says nothing yet -- no
    descriptor's text can spell a parameter's qualifier -- so each such row is asked again in the form that
    reference answers (asked_again). `spell`: {(rel, name): (declared, canonical)}."""
    b = run_verdicts(groups, runner)
    if forms['generic_keeps_restrict']:
        again = {key for key, value in b.items() if verdict_of(value)[0] == 'NO'
                 and RESTRICT_WORDS.intersection(SPELLED_WORD.findall(spell[key][1]))}
        b.update(asked_again(groups, spell, again, runner))
    return b


def census(tree, target, dsscp, cc, flags, dialect, only=None):
    pair = pair_facts(tree, target)
    rows, idents = visible_symbols(tree, pair, only)
    work = tempfile.mkdtemp(prefix='census-')
    try:
        dropped = {}
        spell = reference_spellings(rows, cc, flags, dialect, work, dropped)
        a_groups, b_groups, b_errors = {}, {}, {}
        for r in rows:
            r['dropped'] = dropped.get((r['rel'], r['name']), '')
            s = spell.get((r['rel'], r['name']))
            if isinstance(s, tuple):
                a_groups.setdefault((r['rel'], r['header']), []).append((r['name'], 'typeof(%s)' % s[1]))
                if r['sig'] is None:                        # the reader refuses this row on the pair
                    b_errors[(r['rel'], r['name'])] = 'PAIR-REFUSED:no arm of the signature serves this pair'
                    continue
                try:
                    # `&sym`'s type is a pointer to the row's type, for a function and an object alike.
                    ptr = declarator(('ptr', Parser(r['sig'], idents).type()), '').strip()
                    b_groups.setdefault((r['rel'], r['header']), []).append((r['name'], ptr))
                except Exception as e:                      # an unrenderable signature is a finding of its own
                    b_errors[(r['rel'], r['name'])] = 'UNRENDERABLE:%s' % e
        a = run_verdicts(a_groups, dss_runner(dsscp, target, tree, work))
        b = reference_verdicts(b_groups, spell, ref_runner(cc, flags, dialect, work), DIALECTS[dialect])
    finally:
        shutil.rmtree(work, ignore_errors=True)
    report = []
    for r in rows:
        key = (r['rel'], r['name'])
        s = spell.get(key)
        if not isinstance(s, tuple):
            report.append((r, s, '', '', s, s))
            continue
        va = a.get(key, 'DSS-REFUSED:no verdict')
        vb = b_errors.get(key) or b.get(key, 'REF-CANNOT-SPELL:no verdict')
        # A message a runner kept is already one line, relative to the scratch directory and cut on a word
        # (kept_message): a cell takes it whole.
        if isinstance(va, tuple):
            va = va[0] or (va[1] if (va[1] or '').startswith('JUDGE-FAILED') else 'DSS-REFUSED:' + (va[1] or ''))
        if isinstance(vb, tuple):
            vb = vb[0] or (vb[1] if (vb[1] or '').startswith('JUDGE-FAILED') else 'REF-CANNOT-SPELL:' + (vb[1] or ''))
        report.append((r, 'ok', s[0], s[1], va, vb))
    return pair, report


# Verdict (a)'s categories, and verdict (b)'s: every row of a report has exactly one of each, so each list's counts
# sum to the symbols. A row the spelling step could not spell carries that step's verdict in BOTH columns, which is
# why the three REF- spelling verdicts are in both lists.
A_CATEGORIES = ('yes', 'NO', 'REF-UNDECLARED', 'REF-NO-HEADER', 'REF-ERROR', 'DSS-REFUSED', 'JUDGE-FAILED')
B_CATEGORIES = ('yes', 'NO', 'REF-UNDECLARED', 'REF-NO-HEADER', 'REF-ERROR', 'REF-CANNOT-SPELL', 'UNRENDERABLE',
                'REF-CANNOT-JUDGE', 'PAIR-REFUSED', 'JUDGE-FAILED')


def summarize(pair, report, dialect):
    """(ok, line): the census line, and whether it ACCOUNTS for every row -- by BOTH verdicts. A verdict (a) outside
    A_CATEGORIES or a verdict (b) outside B_CATEGORIES, or counts of either that do not sum to the symbols, makes the
    line a FAILED one that names the verdict and the side (the census never passes over a row it does not count).
    A judge that failed is counted on the side it failed on."""
    c = {}
    for r, st, _, _, va, vb in report:
        for k, v in (('a', va), ('b', vb)):
            key = '%s-%s' % (k, (v or 'NONE').split(':', 1)[0])
            c[key] = c.get(key, 0) + 1
        if va in ('yes', 'NO') and vb in ('yes', 'NO') and va != vb:
            c['disagree'] = c.get('disagree', 0) + 1
        if r.get('dropped'):
            c['dropped'] = c.get('dropped', 0) + 1
    uncounted = []
    for side, categories in (('a', A_CATEGORIES), ('b', B_CATEGORIES)):
        counted = sum(c.get('%s-%s' % (side, k), 0) for k in categories)
        unknown = sorted(k[2:] for k in c if k.startswith(side + '-') and k[2:] not in categories)
        if counted != len(report) or unknown:
            uncounted.append('%d of %d row(s) carry a verdict (%s) this line does not count (%s)'
                             % (len(report) - counted, len(report), side, ', '.join(unknown) or 'none named'))
    ok = not uncounted
    head = 'prototype-census: OK' if ok else 'prototype-census: FAILED — %s; the counts follow' % '; '.join(uncounted)
    return ok, ('%s format=%s dialect=%s symbols=%d a-yes=%d a-no=%d b-yes=%d b-no=%d disagree=%d '
                'ref-undeclared=%d ref-no-header=%d ref-error=%d dss-refused=%d ref-cannot-spell=%d '
                'ref-cannot-judge=%d pair-refused=%d a-judge-failed=%d b-judge-failed=%d ref-decoration-dropped=%d'
                % (head, pair['format'], dialect, len(report), c.get('a-yes', 0), c.get('a-NO', 0), c.get('b-yes', 0),
                   c.get('b-NO', 0), c.get('disagree', 0), c.get('a-REF-UNDECLARED', 0),
                   c.get('a-REF-NO-HEADER', 0), c.get('a-REF-ERROR', 0), c.get('a-DSS-REFUSED', 0),
                   c.get('b-REF-CANNOT-SPELL', 0) + c.get('b-UNRENDERABLE', 0), c.get('b-REF-CANNOT-JUDGE', 0),
                   c.get('b-PAIR-REFUSED', 0), c.get('a-JUDGE-FAILED', 0), c.get('b-JUDGE-FAILED', 0),
                   c.get('dropped', 0)))


# ── a DECIDED fork ───────────────────────────────────────────────────────────────────────────────────────────────
# Two references of ONE pair can read two headers that give a name two values (✔MEASURED 2026-10-08: on pe,
# mingw-w64's <stdio.h> gives L_tmpnam 14 and TMP_MAX 32767 where the UCRT's own header, which cl reads, gives 260 and
# 2147483647 -- one library, the UCRT, both times). Which header is the pair's authority is a DECISION, recorded as
# data beside this program, one entry per (format, dialect, header, name) with BOTH values. The values step reads
# such a name as `decided` only while both recorded values still hold; anything else about it fails the step and
# names the entry, so the decision is read again. A name the table does not hold is judged as every other is.
DECIDED_FORKS = 'decided-forks.json'
DECIDED_KEYS = ('format', 'dialect', 'header', 'name', 'reference', 'dss', 'authority', 'reason')


class KeyWrittenTwice(Exception):
    """A JSON object of the decided-forks table writes one key twice."""


def each_key_once(pairs):
    """An object of the table as a dict -- REFUSED, by the key's name, when it writes a key twice: JSON's reader
    would keep the last value and say nothing, and which of two recorded values is the decision is not this
    program's to pick (an entry's `dss` written twice; a second `decided` list)."""
    seen = {}
    for key, value in pairs:
        if key in seen:
            raise KeyWrittenTwice(key)
        seen[key] = value
    return seen


def read_decided(path):
    """The entries of a decided-forks table, each whole, none twice and no key written twice -- or ValueError saying
    which is not."""
    try:
        with open(path, encoding='utf-8') as f:
            doc = json.load(f, object_pairs_hook=each_key_once)
    except KeyWrittenTwice as e:
        raise ValueError('%s writes the key `%s` twice in one object' % (path, e.args[0]))
    except (OSError, ValueError) as e:
        raise ValueError('cannot read %s: %s' % (path, e))
    entries = doc.get('decided') if isinstance(doc, dict) else None
    if not isinstance(entries, list):
        raise ValueError('%s holds no `decided` list' % path)
    seen = set()
    for i, e in enumerate(entries):
        if not isinstance(e, dict) or sorted(e) != sorted(DECIDED_KEYS):
            raise ValueError('entry %d of %s does not hold exactly %s' % (i, path, ', '.join(DECIDED_KEYS)))
        key = decided_key(e)
        if not all(isinstance(e[k], str) and e[k].strip() for k in DECIDED_KEYS if k not in ('reference', 'dss')) \
                or not all(type(e[k]) is int for k in ('reference', 'dss')) or e['reference'] == e['dss']:
            raise ValueError('entry %s of %s: every field is text but `reference` and `dss`, two DIFFERENT integers'
                             % ('/'.join(map(str, key)), path))
        if key in seen:
            raise ValueError('entry %s is in %s twice' % ('/'.join(key), path))
        seen.add(key)
    return entries


def decided_key(entry):
    return tuple(entry[k] for k in ('format', 'dialect', 'header', 'name'))


def decided_unreachable(tree, entries):
    """[(entry key, why)] — the entries NO run of this census can meet: a format no object-format document of the
    tree has, a dialect this program does not read, or a name the descriptors give that format no value for. (Which
    legs exist is the harness's to say; an entry of a real format and dialect that no LEG pairs is met by none and
    judged by none — each leg's self-test prints how many of the table's entries are its own.)"""
    kinds = {json.load(open(p, encoding='utf-8'))['format']['kind']
             for p in glob.glob(os.path.join(tree, 'src', 'dss-config', 'object-formats', '*.format.json'))}
    out, names = [], {}
    for e in entries:
        key = decided_key(e)
        if e['format'] not in kinds:
            out.append((key, 'no object-format document is of the kind %r' % e['format']))
        elif e['dialect'] not in DIALECTS:
            out.append((key, 'this program reads no dialect %r' % e['dialect']))
        else:
            if e['format'] not in names:
                read = value_names(tree, {'format': e['format']})
                names[e['format']] = {(header, n) for (_, header), ns in read.items() for n in ns}
            if (e['header'], e['name']) not in names[e['format']]:
                out.append((key, 'the descriptors give the %s pair no value named %s in <%s>'
                            % (e['format'], e['name'], e['header'])))
    return out


def values_side(runner, header, names, tag):
    """One side's {name: printed value} for a header, and {name: why} for the names it cannot print. The whole header
    is one program; if that program fails, each name is ISOLATED in a program of its own — a single macro that is not
    an integer expression (an include guard defined empty: Apple's `__STDBOOL_H`, `__TARGETCONDITIONALS__`,
    ✔MEASURED 2026-09-30) must not take the header's other names down with it."""
    so, err = runner(values_program(header, names), tag)
    if so is not None:
        printed = parse_values(so)
        if printed.get('census_probe_header') == 'absent':
            return {n: 'absent' for n in names}, {}
        return printed, {}
    printed, errors = {}, {}
    for k, n in enumerate(names):
        one, why = runner(values_program(header, [n]), '%s_%d' % (tag, k))
        if one is None:
            errors[n] = why
            continue
        got = parse_values(one)
        printed[n] = 'absent' if got.get('census_probe_header') == 'absent' else got.get(n)
    return printed, errors


def value_verdict(d, r):
    if d == 'absent':
        return 'DSS-NO-HEADER'
    if r == 'absent':
        return 'ref-no-header'
    if d != 'nodef' and r != 'nodef':
        return 'match' if int(d) == int(r) else 'MISMATCH'
    if d == 'nodef' and r == 'nodef':
        return 'neither-macro'
    return 'dss-not-a-macro' if d == 'nodef' else 'ref-not-a-macro'


def values_census(tree, target, dsscp, cc, flags, dialect, groups=None, decided=()):
    """[(header, name, dss, ref, verdict)] and the failures. A failure is DSS's: a value program DSS could not build or
    run, a name it printed nothing for, or a header of its own it does not have. What the REFERENCE cannot print (a
    macro that is not an integer expression) or does not ship is reported per name and not judged. `decided`: the
    decided forks (read_decided); an entry of this pair's format and this reference's dialect makes its name
    `decided` while both sides print the values it records, and a FAILURE naming it otherwise — a value that moved, a
    side that no longer prints one, or a name this run never reads."""
    pair = pair_facts(tree, target)
    groups = groups if groups is not None else value_names(tree, pair)
    forks = {(e['header'], e['name']): e for e in decided
             if e['format'] == pair['format'] and e['dialect'] == dialect}
    work = tempfile.mkdtemp(prefix='census-values-')
    rows, failures, met = [], [], set()
    try:
        dss, ref = dss_runner(dsscp, target, tree, work), ref_runner(cc, flags, dialect, work)
        for i, ((rel, header), names) in enumerate(sorted(groups.items())):
            dv, derr = values_side(dss, header, names, 'dss%d' % i)
            rv, rerr = values_side(ref, header, names, 'ref%d' % i)
            for n in names:
                fork = forks.get((header, n))
                if fork is not None:
                    met.add((header, n))
                    d = None if n in derr else dv.get(n)
                    r = None if n in rerr else rv.get(n)
                    if d == str(fork['dss']) and r == str(fork['reference']):
                        rows.append((header, n, d, r, 'decided'))
                    else:
                        failures.append('<%s> %s: the decided fork %s no longer holds — DSS prints %s where it '
                                        'records %d, the reference prints %s where it records %d; read the decision '
                                        'again (%s)' % (header, n, '/'.join(decided_key(fork)), d or 'nothing',
                                                        fork['dss'], r or 'nothing', fork['reference'], DECIDED_FORKS))
                    continue
                if n in derr:
                    failures.append('<%s> %s: DSS could not print it: %s' % (header, n, (derr[n] or '')[:160]))
                    continue
                d = dv.get(n)
                if d is None:
                    failures.append('<%s> %s: DSS printed no line for it' % (header, n))
                    continue
                if n in rerr:
                    rows.append((header, n, d, '-', 'ref-not-an-integer'))
                    continue
                r = rv.get(n)
                if r is None:
                    rows.append((header, n, d, '-', 'ref-printed-nothing'))
                    continue
                verdict = value_verdict(d, r)
                if verdict == 'DSS-NO-HEADER':
                    failures.append('<%s> %s: DSS has no such header on its own pair' % (header, n))
                    continue
                rows.append((header, n, d, r, verdict))
    finally:
        shutil.rmtree(work, ignore_errors=True)
    for header, n in sorted(set(forks) - met):
        failures.append('<%s> %s: the decided fork %s names a value this run never read; read the decision again '
                        '(%s)' % (header, n, '/'.join(decided_key(forks[(header, n)])), DECIDED_FORKS))
    return pair, rows, failures


def values_summary(pair, rows, failures):
    c = {}
    for *_, v in rows:
        c[v] = c.get(v, 0) + 1
    ok = c.get('MISMATCH', 0) == 0 and not failures
    return ok, ('prototype-census: VALUES %s format=%s arch=%s names=%d match=%d mismatch=%d decided=%d '
                'dss-not-a-macro=%d ref-not-a-macro=%d neither-macro=%d ref-no-header=%d ref-not-an-integer=%d '
                'ref-printed-nothing=%d failures=%d'
                % ('OK' if ok else 'FAILED', pair['format'], pair['arch'], len(rows), c.get('match', 0),
                   c.get('MISMATCH', 0), c.get('decided', 0), c.get('dss-not-a-macro', 0), c.get('ref-not-a-macro', 0),
                   c.get('neither-macro', 0), c.get('ref-no-header', 0), c.get('ref-not-an-integer', 0),
                   c.get('ref-printed-nothing', 0), len(failures)))


# ── the self-test ──────────────────────────────────────────────────────────────────────────────────────────────────
# PART B — the REFERENCE judges a SYNTHETIC descriptor's text (a one-symbol <string.h> in a scratch tree): the rendered
# prototype must match exactly when it is right, and fail on an untagged core and on a dropped pointee const. `size_t`
# is spelled for the pair's data model (`unsigned long` on LP64, `unsigned long long` on LLP64). The per-pair arms
# prove the arm SELECTION: the arm the pair's format selects is the right text and every other arm a wrong one, so a
# selector that took the wrong arm (or the default over a matching arm) turns `b-variant-arm` into NO.
def selftest_b(pair):
    size_t = 'u64 "unsigned long"' if pair['dataModel'] == 'LP64' else 'u64 "unsigned long long"'
    right = 'fn(ptr<const<char>>) -> %s' % size_t
    wrong = 'fn(ptr<const<char>>) -> u32'
    others = sorted({'elf', 'macho', 'pe'} - {pair['format']})
    return [
        ('b-right', 'strlen', right, 'yes'),
        ('b-untagged-core', 'strlen', 'fn(ptr<const<char>>) -> u64', 'NO'),
        ('b-dropped-const', 'strcpy', 'fn(ptr<char>, ptr<char>) -> ptr<char>', 'NO'),
        ('b-variant-arm', 'strlen', {'variants': [{'when': {'format': f}, 'value': wrong} for f in others]
                                     + [{'when': {'format': pair['format']}, 'value': right},
                                        {'default': True, 'value': wrong}]}, 'yes'),
        ('b-variant-default', 'strlen', {'variants': [{'when': {'format': f}, 'value': wrong} for f in others]
                                         + [{'default': True, 'value': right}]}, 'yes'),
    ]
# PART A — DSS judges, through the SAME judge program shape, functions the probe declares itself (no descriptor):
# an exact match, a different parameter type, and a parameter differing only in its POINTEE const — which DSS's
# `_Generic` tells apart, as every reference compiler does (C 6.7.6.1p2, 6.7.6.3p15). The association is
# `typeof(<type of &f>)`, the shape the census writes.
SELFTEST_A = [
    ('a-exact', 'void (*)(char *)', 'yes'),
    ('a-different-type', 'void (*)(long)', 'NO'),
    ('a-pointee-const', 'void (*)(const char *)', 'NO'),
]
# PART S — the SPELLING step reads the reference's own refusal, through the dialect the banner named: a function and an
# object are each spelled as the type of their ADDRESS (the census judges `&sym` against exactly that), and a name the
# header does not declare is REF-UNDECLARED — on cl too, whose C2065 comes with a C4133 naming 'int *' on its line.
SELFTEST_S = [
    ('s-function', 'census_probe_function', 'function', 'double(*)(constchar*)'),
    ('s-object', 'census_probe_object', 'object', 'int*'),
    ('s-undeclared', 'census_probe_no_such_name', 'function', None),
]
# The header part S spells from: one function and one object whose types no reference spells differently — the arm
# tests the dialect's PARSE, not a compiler's printing style. ✔MEASURED 2026-10-06: a `short` return is NOT such a
# type — gcc 13.3.0 prints it `short int (*)(const char *)` where clang prints `short (*)(const char *)`, so the first
# version of this arm failed on the arm64 VPS's gcc (run 20261006-185142-391e0747) with a correct parse.
SELFTEST_S_HEADER = 'double census_probe_function(const char *);\nextern int census_probe_object;\n'
# A dialect that spells one of part S's types its own way, by arm: cl names a function's calling convention,
# `double (__cdecl *)(const char *)` (✔MEASURED 2026-10-07, cl 19.51.36260), which DSS's pe config erases — so that is
# the spelling the census hands DSS, and the one the arm requires there.
SELFTEST_S_DIALECT = {'msvc': {'s-function': 'double(__cdecl*)(constchar*)'}}
# More rows of one header than any reference judges in one TU (cl stops at 100 errors): every one REF-UNDECLARED, so a
# stopped TU's rows are re-run until each is judged — on every dialect, each by its own mechanism. (arm
# `s-error-limit`)
SELFTEST_S_LIMIT = 130
# An `undeclared` line of ANOTHER file is not the TU's (arm `s-other-file`), read with no compiler from each
# dialect's measured shapes (2026-10-08): a header's own undeclared name sits on line 5, the number a row's line
# shares, and that row keeps its spelling; a file whose name only ENDS in the TU's is another file; the TU named
# through a directory is the TU. Each row: a dialect, the compiler's text, the lines that read undeclared, and the
# line-5 row's verdict.
# ⚠ THE TEXTS ARE WRITTEN WITH `@` WHERE THE COMPILER PRINTS THE COLON AFTER A FILE NAME, and `compiler_lines` puts
# it back, so what the arm reads is the compiler's text byte for byte. A file name, a colon and a number is what the
# repository's positional-citation guard counts as a citation of a line in ANY program it reads, this one included
# (✔MEASURED 2026-10-08: the rows below, spelled as printed, moved this file's count from 0 to 12 and that guard
# red); a compiler's own diagnostic cites nothing.
def compiler_lines(text):
    return text.replace('@', ':')


SELFTEST_S_OTHER_FILE = [
    ('gcc', compiler_lines(
        "In file included from ref.c@2:\n"
        "/usr/include/census_probe_other.h@5:45: error: ‘census_probe_in_header’ undeclared here (not in a "
        "function); did you mean ‘census_probe_h’?\n"
        "ref.c@5:44: warning: initialization of ‘struct census_probe_sink *’ from incompatible pointer type "
        "‘int *’ [-Wincompatible-pointer-types]\n"
        "ref.c@7:45: error: ‘census_probe_in_tu’ undeclared here (not in a function)\n"
        "myref.c@9:45: error: 'census_probe_elsewhere' undeclared here (not in a function)"),
     {7}, ('int *', 'int *')),
    ('clang', compiler_lines(
        "In file included from ref.c@2:\n"
        "/usr/include/census_probe_other.h@5:45: error: use of undeclared identifier 'census_probe_in_header'\n"
        "ref.c@5:27: warning: incompatible pointer types initializing 'struct census_probe_sink *' with an "
        "expression of type 'int *' [-Wincompatible-pointer-types]\n"
        "C:\\w\\ref.c@7:45: error: use of undeclared identifier 'census_probe_in_tu'\n"
        "myref.c@9:45: error: use of undeclared identifier 'census_probe_elsewhere'"),
     {7}, ('int *', 'int *')),
]
# A KEPT MESSAGE (arms `k-scratch-path`, `k-cut-on-a-word`). A judge program the reference REFUSES is named in its
# refusal as it was named to the compiler -- relative to the scratch directory, so the cell that keeps the refusal
# never opens with that directory's path (through the leg's own compiler: this program holds one syntax error). And
# the message itself is kept on one line, whole up to its bound; past it its start and its end are kept around the
# mark, each cut at a word's end -- the end left out whole where it holds no word boundary: each row a text, a bound,
# and what is kept.
SELFTEST_K_REFUSED = 'int census_probe_refused(void) { return census_probe_not ; ; + ; }\nint main(void) { return 0 }\n'
SELFTEST_K_KEPT = [
    ('a short message is kept whole', 40, 'a short message is kept whole'),
    ('one\tline,\r\n  whatever   the compiler wrapped', 60, 'one line, whatever the compiler wrapped'),
    ("error[P_Probe]: expected 'Identifier', 'IntLiteral', 'FloatLiteral', 'StringStart' or 'CharStart' — got ')'",
     60, "error[P_Probe]: expected 'Identifier', [...] — got ')'"),
    ('one two three four five six seven eight nine ten eleven', 40, 'one two three four five six [...] ten eleven'),
    ('exactly-the-bound', 17, 'exactly-the-bound'),
    ('the last word of this message is averyveryverylongtokenwithoutanyspace', 40,
     'the last word of this message [...]'),
    ('nowhitespaceanywhereinthismessage', 10, 'nowhites [...]'),
]
# PART M — cl's diagnostics, READ from its measured lines (windows-x86_64-release, 2026-10-07), on every leg: the arms
# need no cl, so the parse cl's legs depend on is held wherever the census runs. Each: an arm, the text, and the
# (spellings, undeclared, stopped) msvc_diagnostics must read from it for the TU `ref.c`.
SELFTEST_M = [
    ('m-c4047', "C:\\w\\ref.c(13): warning C4047: 'initializing': 'census_probe_sink *' differs in levels of "
                "indirection from 'double (__cdecl *)(const char *)'",
     {13: ('double (__cdecl *)(const char *)', 'double (__cdecl *)(const char *)')}, set(), None),
    ('m-c4133', "ref.c(14): warning C4133: 'initializing': incompatible types - from 'int *' to 'census_probe_sink *'",
     {14: ('int *', 'int *')}, set(), None),
    ('m-c2065', "ref.c(15): error C2065: 'census_probe_no_such_name': undeclared identifier\n"
                "ref.c(15): warning C4133: 'initializing': incompatible types - from 'int *' to 'census_probe_sink *'",
     {15: ('int *', 'int *')}, {15}, None),
    ('m-c1003', "ref.c(101): error C2065: 'census_probe_missing_99': undeclared identifier\n"
                "ref.c(102): fatal error C1003: error count exceeds 100; stopping compilation",
     {}, {101}, 102),
    ('m-other-file', "C:\\sdk\\stdlib.h(13): warning C4047: 'initializing': 'census_probe_sink *' differs in levels "
                     "of indirection from 'int *'\nmyref.c(14): error C2065: 'x': undeclared identifier\n"
                     "ref.c(16): warning C4047: 'initializing': 'other *' differs in levels of indirection from 'int *'",
     {}, set(), None),
]
# cl's answer to a TAG TU (reference_tag_kinds), read without cl: names a_tag, a_type, u_tag and e_tag from line 2,
# two lines each -- a typedef line, then a struct line -- in the measured shapes (C2061 then C2059 on a tag's typedef
# line; C2011 naming the kind on the struct line of a union's and an enum's tag; nothing for a typedef), and a TU cl
# stopped at line 4. (arms `m-tag-kinds`, `m-tag-stop`)
SELFTEST_M_TAGS = [
    ('m-tag-kinds', "tags.c(2): error C2061: syntax error: identifier 'census_probe_type_0'\n"
                    "tags.c(2): error C2059: syntax error: ';'\n"
                    "tags.c(6): error C2061: syntax error: identifier 'census_probe_type_2'\n"
                    "tags.c(6): error C2059: syntax error: ';'\n"
                    "tags.c(7): error C2011: 'u_tag': 'union' type redefinition\n"
                    "C:\\sdk\\tags.h(1): note: see declaration of 'u_tag'\n"
                    "tags.c(8): error C2061: syntax error: identifier 'census_probe_type_3'\n"
                    "tags.c(8): error C2059: syntax error: ';'\n"
                    "tags.c(9): error C2011: 'e_tag': 'enum' type redefinition",
     ['a_tag', 'a_type', 'u_tag', 'e_tag'], ({'a_tag': 'struct', 'u_tag': 'union', 'e_tag': 'enum'}, None)),
    ('m-tag-stop', "tags.c(2): error C2061: syntax error: identifier 'census_probe_type_0'\n"
                   "tags.c(4): fatal error C1003: error count exceeds 100; stopping compilation",
     ['a_tag', 'b_tag', 'c_tag'], ({'a_tag': 'struct'}, 4)),
]
# Which words of a cl spelling are names to classify -- each asked about once over a header's rows -- and how a tag
# word goes back in: a whole word, never twice. (arms `m-tagless-names`, `m-with-tags`)
SELFTEST_M_WORDS = (
    ("int (__cdecl *)(const char *const ,stat *const )", ['stat']),
    ("unsigned __int64 (__cdecl *)(bool *,_locale_t,struct tm *)", ['_locale_t']),
)
SELFTEST_M_WITH_TAGS = ("tm *(__cdecl *)(const tm *const ,struct tm *,tm_x)", {'tm': 'struct'},
                        "struct tm *(__cdecl *)(const struct tm *const ,struct tm *,tm_x)")
# PART T — a spelled STRUCT, UNION and ENUM type keeps its tag word on every dialect, and DSS takes the spelling as the
# type it is: gcc and clang print the word, cl drops it (✔MEASURED 2026-10-07) and the census puts it back by asking
# cl which names are only tags. Each row: an arm, the name, its kind, and a tag phrase its spelling must hold; the DSS
# judge declares the same header and defines its names.
SELFTEST_T_HEADER = ('struct census_probe_tag_s { int x; };\nunion census_probe_tag_u { int y; };\n'
                     'enum census_probe_tag_e { CENSUS_PROBE_TAG_E0 };\n'
                     'typedef struct census_probe_tag_s census_probe_tag_t;\n'
                     'extern struct census_probe_tag_s census_probe_tag_sobj;\n'
                     'extern union census_probe_tag_u census_probe_tag_uobj;\n'
                     'extern enum census_probe_tag_e census_probe_tag_eobj;\n'
                     'struct census_probe_tag_s *census_probe_tag_fn(union census_probe_tag_u *, census_probe_tag_t *);\n')
SELFTEST_T_DEFINITIONS = ('struct census_probe_tag_s census_probe_tag_sobj;\n'
                          'union census_probe_tag_u census_probe_tag_uobj;\n'
                          'enum census_probe_tag_e census_probe_tag_eobj;\n'
                          'struct census_probe_tag_s *census_probe_tag_fn(union census_probe_tag_u *u, '
                          'census_probe_tag_t *t) { (void)u; (void)t; return 0; }\n')
SELFTEST_T = [
    ('t-struct', 'census_probe_tag_sobj', 'object', 'struct census_probe_tag_s'),
    ('t-union', 'census_probe_tag_uobj', 'object', 'union census_probe_tag_u'),
    ('t-enum', 'census_probe_tag_eobj', 'object', 'enum census_probe_tag_e'),
    ('t-function', 'census_probe_tag_fn', 'function', 'union census_probe_tag_u'),
]
# More tag names under one header than cl judges in one tag TU (two errors a tag, and it stops at 100): every
# spelling must still get its word back, so the names a stopped tag TU never reached are re-run -- on every dialect,
# each by its own mechanism (gcc and clang print the word). (arm `t-error-limit`)
SELFTEST_T_LIMIT = 130
# PART D — a DECORATION the reference prints and does not take as part of a type (DECORATIONS): a function declared
# noreturn and one declared const, with the attribute where the compiler has it — the declarations the platform
# headers make. The spelling the census hands on is the bare type on every dialect (gcc printed the attribute first,
# clang prints noreturn last, cl prints none), the removal is RECORDED for the row, DSS's own `_Generic` takes the
# bare spelling as the type of a function DSS sees declared `_Noreturn`, and so does the REFERENCE's own `_Generic`,
# of the function it decorated itself — what was removed is no part of the type by the reference's own judgement,
# which clang's witness text, a refusal of the form it prints, would not say alone. Each row: an arm, the name, the
# bare spelling with its spaces removed, a dialect's own, and what each dialect is recorded to have had removed.
# `d-witness` holds every entry of the leg's dialect to the leg's own compiler: the decoration written in a type
# name, at the end the entry names, must draw the entry's witness from the reference (cl has no entry). The closed
# list itself is held with no compiler by `d-closed-list` — each row a dialect, a spelling, what is handed on and
# what was removed: only an entry's exact text at its own end goes — and the census line's count by `d-counted`.
SELFTEST_D_HEADER = ('#if defined(__GNUC__)\n'
                     '__attribute__((noreturn)) void census_probe_noreturn(int);\n'
                     '__attribute__((const)) int census_probe_const(int);\n'
                     '#else\n'
                     '_Noreturn void census_probe_noreturn(int);\n'
                     'int census_probe_const(int);\n'
                     '#endif\n')
SELFTEST_D_DEFINITIONS = ('_Noreturn void census_probe_noreturn(int x) { (void)x; for (;;) { } }\n'
                          'int census_probe_const(int x) { return x; }\n')
SELFTEST_D = [
    ('d-noreturn', 'census_probe_noreturn', 'void(*)(int)', {'msvc': 'void(__cdecl*)(int)'},
     {'gcc': '__attribute__((noreturn))', 'clang': '__attribute__((noreturn))'}),
    ('d-const', 'census_probe_const', 'int(*)(int)', {'msvc': 'int(__cdecl*)(int)'},
     {'gcc': '__attribute__((const))'}),
]
SELFTEST_D_LIST = [
    ('gcc', '__attribute__((const)) double (*)(double)', 'double (*)(double)', ['__attribute__((const))']),
    ('gcc', '__attribute__((noreturn)) void (*)(int)', 'void (*)(int)', ['__attribute__((noreturn))']),
    ('gcc', '__attribute__((ms_abi)) int (*)(int)', '__attribute__((ms_abi)) int (*)(int)', []),
    ('gcc', 'void (*)(int) __attribute__((noreturn))', 'void (*)(int) __attribute__((noreturn))', []),
    ('gcc', 'void (*)(__attribute__((noreturn)) void (*)(int))', 'void (*)(__attribute__((noreturn)) void (*)(int))',
     []),
    ('clang', 'void (*)(int) __attribute__((noreturn))', 'void (*)(int)', ['__attribute__((noreturn))']),
    ('clang', '__attribute__((noreturn)) void (*)(int)', '__attribute__((noreturn)) void (*)(int)', []),
    ('clang', 'int (*)(int) __attribute__((const))', 'int (*)(int) __attribute__((const))', []),
    ('msvc', 'void (__cdecl *)(int) __attribute__((noreturn))', 'void (__cdecl *)(int) __attribute__((noreturn))', []),
]
# A REMOVAL IS WITNESSED ROW BY ROW. `d-nested` holds it to the leg's own compiler on the shape that found the rule:
# an OBJECT that points to a noreturn function, declared the one way gcc honours (the attribute after the
# declarator). Per dialect, the spelling the census hands on, its spaces removed, and what it records as removed
# (✔MEASURED 2026-10-08, probe-reference-cc runs 20261008-085400-883c8a53, 20261008-091425-260d26e9 and
# 20261008-085224-eb714084): gcc prints the attribute at the front and its own `_Generic` does NOT take the bare
# type, so the spelling keeps it and nothing is recorded; clang prints it last and DOES, so it is removed and
# recorded; cl is not handed the attribute at all. `d-unwitnessed` holds the rule with no compiler, through a
# scripted judge: of two rows a decoration was removed from, the one the judge takes the bare type of stays bare
# and is counted, the one it answers NO for gets its spelling back as printed and is in no count -- as does one the
# judge will not compile, and one it gives no answer for.
SELFTEST_D_NESTED_HEADER = ('#if defined(__GNUC__)\n'
                            'extern void (*census_probe_nested)(int) __attribute__((noreturn));\n'
                            '#else\n'
                            'extern void (*census_probe_nested)(int);\n'
                            '#endif\n')
SELFTEST_D_NESTED = {'gcc': ('__attribute__((noreturn))void(**)(int)', ''),
                     'clang': ('void(**)(int)', '__attribute__((noreturn))'),
                     'msvc': ('void(__cdecl**)(int)', '')}
SELFTEST_D_UNWITNESSED = [
    # (name, the judge's answer: yes, NO, None = it will not compile the row, '' = it prints no line for the row)
    ('census_probe_held', 'yes'), ('census_probe_refuted', 'NO'), ('census_probe_unspellable', None),
    ('census_probe_unanswered', ''),
]
# PART R — a PARAMETER's top-level `restrict`, on every leg. The header declares two functions: one restrict
# parameter; and one with a POINTEE's restrict, a callback whose parameter is restrict, and a restrict beside a
# const. Each row: the name, a RIGHT text (it spells the pointee's restrict, which is part of the type, and no
# parameter's, which is not) and a WRONG one (the first: another parameter type; the second: the pointee's restrict
# left out -- wrong on EVERY reference, which is what holds the rule that a pointee's restrict is never removed).
#   `b-restrict-parameter` holds the dialect's `generic_keeps_restrict` to the leg's own reference: its PLAIN
#     `_Generic(&f, <the right text>)` is "yes" from a reference that erases the qualifier (gcc, clang) and "NO"
#     from one that keeps it (cl).
#   `b-restrict-right` and `b-restrict-wrong` are the census's own verdict (b) (reference_verdicts): yes for the
#     right text and NO for the wrong one, on every leg -- on a keeping reference through the question asked again,
#     where a plain NO, or a REF-CANNOT-JUDGE, would be no verdict at all.
#   `b-restrict-again` puts that question to the leg's reference WHATEVER its dialect says (asked_again, forced), so
#     the witness and the cast form are held to gcc and clang as they are to cl: yes and NO again.
#   `b-restrict-placed` holds the removal with no compiler -- each row a spelling as a reference prints it (cl's,
#     gcc's, clang's: ✔MEASURED 2026-10-08), the spelling without its parameters' restrict, how many were removed
#     and how many the rule cannot place.
#   `b-cannot-judge` holds what is left of REF-CANNOT-JUDGE, with no compiler, through a scripted judge -- each row
#     a name, its spelling, the judge's plain answer, its answer to the witness and to the question asked again
#     (None = it will not compile it), and what the report keeps (a verdict word, and for REF-CANNOT-JUDGE a phrase
#     of its reason); then the same NO from a reference that ERASES the qualifier, which is never asked again; and
#     the census line of one such row.
SELFTEST_R_HEADER = ('static void census_probe_restrict(char *restrict p) { (void)p; }\n'
                     'static void census_probe_restrict_deep(char *restrict *p, void (*f)(int *restrict),\n'
                     '                                       char *const restrict q) { (void)p; (void)f; (void)q; }\n')
SELFTEST_R = [
    ('census_probe_restrict', 'void (*)(char *)', 'void (*)(int *)'),
    ('census_probe_restrict_deep', 'void (*)(char *restrict *, void (*)(int *), char *)',
     'void (*)(char **, void (*)(int *), char *)'),
]
SELFTEST_R_PLACED = [
    ('void (__cdecl *)(char *restrict )', 'void (__cdecl *)(char * )', 1, 0),
    ('void (__cdecl *)(char *__restrict )', 'void (__cdecl *)(char * )', 1, 0),
    ('int (__cdecl *)(mtx_t *restrict ,const struct timespec *restrict )',
     'int (__cdecl *)(mtx_t * ,const struct timespec * )', 2, 0),
    ('void (__cdecl *)(char *restrict *,void (__cdecl *)(int *restrict ))',
     'void (__cdecl *)(char *restrict *,void (__cdecl *)(int * ))', 1, 0),
    ('void (__cdecl *)(char *restrict const )', 'void (__cdecl *)(char * const )', 1, 0),
    ('void (__cdecl *)(int (*restrict )[3])', 'void (__cdecl *)(int (* )[3])', 1, 0),
    ('void (*)(char * restrict,  const char * restrict,  int)', 'void (*)(char * ,  const char * ,  int)', 2, 0),
    ('void (*)(char *const restrict)', 'void (*)(char *const )', 1, 0),
    ('char *restrict *', 'char *restrict *', 0, 0),
    ('void (*)(char *restrict (*)(void))', 'void (*)(char *restrict (*)(void))', 0, 0),
    ('void (__cdecl *)(char *restrict [4])', 'void (__cdecl *)(char *restrict [4])', 0, 0),
    ('int (__cdecl *)(mtx_t *,restrict_t *)', 'int (__cdecl *)(mtx_t *,restrict_t *)', 0, 0),
    ('void (__cdecl *)(_Atomic(char *restrict ))', 'void (__cdecl *)(_Atomic(char *restrict ))', 0, 1),
    ('void (__cdecl *restrict )(char *)', 'void (__cdecl *restrict )(char *)', 0, 1),
    ('char *restrict', 'char *restrict', 0, 1),
    ('void (__cdecl *)(int [restrict 4])', 'void (__cdecl *)(int [restrict 4])', 0, 1),
]
SELFTEST_R_CL = 'void (__cdecl *)(char *restrict )'
SELFTEST_R_JUDGED = [
    ('j_wrong', SELFTEST_R_CL, 'NO', 'yes', 'NO', 'NO', ''),
    ('j_right', SELFTEST_R_CL, 'NO', 'yes', 'yes', 'yes', ''),
    ('j_unwitnessed', SELFTEST_R_CL, 'NO', 'NO', 'yes', 'REF-CANNOT-JUDGE', 'its _Generic answered NO'),
    ('j_unspelled', SELFTEST_R_CL, 'NO', None, 'yes', 'REF-CANNOT-JUDGE', 'does not take its own spelling'),
    ('j_unasked', SELFTEST_R_CL, 'NO', 'yes', None, 'REF-CANNOT-JUDGE', 'refused the question'),
    ('j_unplaced', 'void (__cdecl *)(_Atomic(char *restrict ))', 'NO', 'yes', 'yes', 'REF-CANNOT-JUDGE',
     'cannot place'),
    ('j_pointee', 'char *restrict *', 'NO', 'yes', 'yes', 'NO', ''),
    ('j_plain_yes', SELFTEST_R_CL, 'yes', 'NO', 'NO', 'yes', ''),
    ('j_no_restrict', 'int (__cdecl *)(mtx_t *,restrict_t *)', 'NO', 'yes', 'yes', 'NO', ''),
]
# The banner cl prints when run with no arguments (✔MEASURED 2026-10-07), and gcc's `--version` first line, which is
# not cl's. (arm `m-banner`)
SELFTEST_M_BANNER = ('Microsoft (R) C/C++ Optimizing Compiler Version 19.51.36260 for x64',
                     'gcc.EXE (MinGW-W64 x86_64-ucrt-posix-seh, built by Brecht Sanders, r8) 13.2.0')
# A row whose header the reference does not have at all: its own verdict, REF-NO-HEADER (no reference header
# file of that name exists on any -I path of the self-test).
SELFTEST_S_NO_HEADER = ('s-no-header', 'census_probe_absent.h', 'census_probe_in_no_header', 'REF-NO-HEADER')
# The census LINE accounts for every row, by BOTH verdicts (arm `s-accounting`): a report holding each verdict (a)
# category the census gives is OK and every category's count is printed, and so is one holding each verdict (b)
# category -- a judge that failed counted on its own side; a report holding a verdict of EITHER side that the line
# does not count is FAILED, and the line says which side and which verdict. Each report is its (a, b) pairs; a row
# the spelling step could not spell carries one verdict in both columns, as the census writes it.
SELFTEST_ACCOUNTING_A = [(v, 'yes') for v in ('yes', 'NO', 'DSS-REFUSED:probe', 'JUDGE-FAILED:exit 1')] + \
                        [(v, v) for v in ('REF-UNDECLARED', 'REF-NO-HEADER', 'REF-ERROR')]
SELFTEST_ACCOUNTING_A_COUNTS = ('symbols=7 a-yes=1 a-no=1 b-yes=4 b-no=0 ',
                                'ref-undeclared=1 ref-no-header=1 ref-error=1 dss-refused=1',
                                'a-judge-failed=1 b-judge-failed=0')
SELFTEST_ACCOUNTING_B = [('yes', v) for v in ('yes', 'NO', 'REF-CANNOT-SPELL:probe', 'UNRENDERABLE:probe',
                                              'REF-CANNOT-JUDGE:probe', 'PAIR-REFUSED:probe', 'JUDGE-FAILED:exit 1')] + \
                        [(v, v) for v in ('REF-UNDECLARED', 'REF-NO-HEADER', 'REF-ERROR')]
SELFTEST_ACCOUNTING_B_COUNTS = ('symbols=10 a-yes=7 a-no=0 b-yes=1 b-no=1 ',
                                'ref-undeclared=1 ref-no-header=1 ref-error=1 dss-refused=0 ref-cannot-spell=2 '
                                'ref-cannot-judge=1 pair-refused=1 a-judge-failed=0 b-judge-failed=1')
# PART V — the VALUES step, on a header BOTH sides get from the self-test alone, so no platform header can move it: DSS
# reads a synthetic descriptor added to a COPY of the tree's config (through DSS_CONFIG_ROOT), the reference a
# synthetic header on its -I path. Each row: the name, DSS's value, the reference header's definition (None = the
# reference header does not define it), and the verdict the census must give. `v-no-header` names a second header
# only DSS ships.
SELFTEST_V = [
    ('v-match', 'CENSUS_PROBE_RIGHT', 8, '8', 'match'),
    ('v-mismatch', 'CENSUS_PROBE_WRONG', 7, '2147483647', 'MISMATCH'),
    ('v-ref-empty-macro', 'CENSUS_PROBE_EMPTY', 1, '', 'ref-not-an-integer'),
    ('v-dss-only', 'CENSUS_PROBE_DSS_ONLY', 42, None, 'ref-not-a-macro'),
    ('v-decided', 'CENSUS_PROBE_DECIDED', 5, '6', 'decided'),
]
SELFTEST_V_NO_HEADER = ('v-no-header', 'CENSUS_PROBE_ONLY_DSS_HAS_THE_HEADER', 5, 'ref-no-header')
# A DECIDED fork (the table's rule, on a synthetic table keyed to the leg's own format and dialect): `v-decided`,
# above, is an entry whose two recorded values (DSS 5, the reference 6) both hold — counted `decided`, beside an
# UNDECLARED mismatch that still fails the step (`v-line`: `mismatch=1 decided=1`, VALUES FAILED). Then a second
# header, each of whose names DSS prints 5 and the reference 6, under entries that no longer hold — each row an arm,
# the name, the entry's recorded (reference, dss), and a phrase the failure naming the entry must hold; the last
# entry names a value the run never reads. `v-decided-table` reads the REAL table beside this program: none of its
# entries may be one no run can meet, and an entry of a dialect this program does not read must be reported so.
# An entry is keyed by format AND dialect: `v-decided-other-pair` hands the first run two more entries for a value
# both sides print alike, one keyed to another format and one to another dialect, each recording values neither side
# prints -- read on this pair, either would fail the step; they are not this pair's, so the name stays a `match`.
SELFTEST_V_MOVED = [
    ('v-decided-reference-moved', 'CENSUS_PROBE_REFERENCE_MOVED', (7, 5), 'the reference prints 6 where it records 7'),
    ('v-decided-dss-moved', 'CENSUS_PROBE_DSS_MOVED', (6, 4), 'DSS prints 5 where it records 4'),
    ('v-decided-unmet', 'CENSUS_PROBE_NEVER_READ', (6, 5), 'names a value this run never read'),
]
# The table's READER, with no compiler (`v-table-refused`): each of these documents is written and must be refused in
# the words beside it -- no `decided` list, an entry with a field missing and one with a field more, two equal
# values, a value that is text, a blank text field, an entry twice -- as must a file that is no JSON at all; and a
# table of two whole entries is read as two. A KEY WRITTEN TWICE in one object is refused by the key's name: an
# entry whose `dss` is written twice (JSON's reader would keep the last), and a table with a second `decided` list
# (it would keep the second) -- those two are written as TEXT, since no document built here can hold them. A
# document the reader BREAKS on (any error that is not its refusal) is named by the arm too, never left to end the
# self-test on a traceback.
SELFTEST_V_ENTRY = {'format': 'pe', 'dialect': 'gcc', 'header': 'census_probe.h', 'name': 'CENSUS_PROBE',
                    'reference': 1, 'dss': 2, 'authority': 'the self-test', 'reason': 'the self-test'}
SELFTEST_V_TABLES = [
    ('holds no `decided` list', {'purpose': 'a table with no list'}),
    ('does not hold exactly', {'decided': [{k: v for k, v in SELFTEST_V_ENTRY.items() if k != 'reason'}]}),
    ('does not hold exactly', {'decided': [dict(SELFTEST_V_ENTRY, note='one field more')]}),
    ('two DIFFERENT integers', {'decided': [dict(SELFTEST_V_ENTRY, dss=1)]}),
    ('two DIFFERENT integers', {'decided': [dict(SELFTEST_V_ENTRY, reference='1')]}),
    ('two DIFFERENT integers', {'decided': [dict(SELFTEST_V_ENTRY, reason=' ')]}),
    ('twice', {'decided': [SELFTEST_V_ENTRY, dict(SELFTEST_V_ENTRY, reference=3)]}),
    ('the key `dss` twice', '{"decided": [%s, "dss": 3}]}' % json.dumps(SELFTEST_V_ENTRY)[:-1]),
    ('the key `decided` twice', '{"decided": [], "decided": [%s]}' % json.dumps(SELFTEST_V_ENTRY)),
]


def scripted_judge(answers):
    """A judge with no compiler, for the self-test's rule arms: `answers` maps (the kind of program -- '' for a
    plain one, else the prefix of its scratch tag: `witness`, `bare` --, a row's name) to the word the program
    prints for that row, to '' (it prints no line for the row), or to None (a program holding the row does not
    compile -- so run_verdicts judges the rows one by one, as it does for a real refusal). A program's rows are read
    from its own text."""
    def go(text, tag):
        kind = tag.rpartition('_')[0]
        said = [(name, answers.get((kind, name))) for name in re.findall(r'default: "NO ([A-Za-z0-9_]+)"', text)]
        if any(word is None for _, word in said):
            return None, 'the scripted judge will not compile %s' % ', '.join(n for n, word in said if word is None)
        return ''.join('%s %s\n' % (word, name) for name, word in said if word), None
    return go


def selftest(tree, target, dsscp, cc, flags, dialect, forks):
    failures = []
    arms_b = selftest_b(pair_facts(tree, target))
    synth = tempfile.mkdtemp(prefix='census-selftest-')
    work = tempfile.mkdtemp(prefix='census-selftest-work-')
    try:
        # ── flags are percent-decoded ─────────────────────────────────────────────────────────────────────────────
        if split_flags('-Wl%2C-z%2Cnoexecstack,,-DX=a%20b') != ['-Wl,-z,noexecstack', '-DX=a b']:
            failures.append('f-percent-decoding: %r' % split_flags('-Wl%2C-z%2Cnoexecstack,,-DX=a%20b'))
        # ── the native pair is derived, and a host kind with no document is refused ───────────────────────────────
        got, why = native_target(tree, target.partition(':')[0], 'no-such-kind')
        if got is not None:
            failures.append('p-unknown-kind: derived %r for a kind no document has' % got)
        # ── a reference the host does not have is refused in this program's words, and none is picked for it ──────
        #    The name no host has goes through the REAL attempt to start it, so each leg's own error is read; then each
        #    way a start fails is held with the error and the PATH given, the same on every leg.
        absent = 'census-probe-no-such-compiler'
        is_absent = 'the reference compiler %r is not on this host' % absent

        def told(error, found, where=work):
            """What a start that raises `error` is told as: `found` is the PATH's answer for the name, and gcc is
            the one compiler beside it."""
            def start(argv, cwd, timeout=0):
                raise error
            return reference_dialect(absent, where, start=start, which=lambda name, mode=os.X_OK: (
                found if name == absent else '/census-probe/gcc' if name == 'gcc' else None))[1]
        read_as, said = reference_dialect(absent, work)
        nowhere = reference_dialect(absent, os.path.join(work, 'census-probe-no-such-directory'))
        wrong = [what for what, good in (
            ('the name no host has, through the real attempt to start it, was read as %r and told as %r'
             % (read_as, said),
             read_as is None and said.startswith(is_absent) and 'never picks one' in said
             and '`--input cc=<name>`' in said),
            ('"file not found" and no such file on the PATH is not told as an absent compiler',
             told(FileNotFoundError(2, 'census-probe'), None).startswith(is_absent)),
            ('"permission denied" and no such file on the PATH (an entry of the PATH refusing the lookup) is not '
             'told as an absent compiler', told(PermissionError(13, 'census-probe'), None).startswith(is_absent)),
            ('"permission denied" for a file that IS on the PATH is told as an absent compiler',
             told(PermissionError(13, 'census-probe'), '/census-probe/' + absent).startswith('cannot run ')),
            ('gcc alone on the PATH is not the list', told(FileNotFoundError(2, 'census-probe'), None)
             .endswith('PATH holds: gcc')),
            ('none of the three on the PATH is not the list',
             absent_reference(absent, which=lambda name, mode=os.X_OK: None).endswith('PATH holds: none of them')),
            ('a directory that does not exist was read as %r' % (nowhere,),
             nowhere[0] is None and nowhere[1].startswith('cannot run ')),
        ) if not good]
        if wrong:
            failures.append('r-absent-reference: ' + '; '.join(wrong))
        # ── part S ────────────────────────────────────────────────────────────────────────────────────────────────
        with open(os.path.join(work, 'census_probe_s.h'), 'w', encoding='utf-8', newline='\n') as o:
            o.write(SELFTEST_S_HEADER)
        rows = [{'rel': 'selftest', 'header': 'census_probe_s.h', 'name': n, 'kind': k} for _, n, k, _ in SELFTEST_S]
        spell = reference_spellings(rows, cc, flags + ['-I' + work], dialect, work)
        for arm, name, _, want in SELFTEST_S:
            want = SELFTEST_S_DIALECT.get(dialect, {}).get(arm, want)
            s = spell.get(('selftest', name))
            if want is None:
                good = s == 'REF-UNDECLARED'
            else:
                good = isinstance(s, tuple) and s[1].replace(' ', '') == want
            if not good:
                failures.append('%s: spelling %r' % (arm, s))
        many = [{'rel': 'selftest', 'header': 'census_probe_s.h', 'name': 'census_probe_missing_%d' % i,
                 'kind': 'function'} for i in range(SELFTEST_S_LIMIT)]
        got_many = reference_spellings(many, cc, flags + ['-I' + work], dialect, work)
        wrong = [v for v in got_many.values() if v != 'REF-UNDECLARED']
        if len(got_many) != SELFTEST_S_LIMIT or wrong:
            failures.append('s-error-limit: %d of %d rows judged, %d of them not REF-UNDECLARED (the first: %r)'
                            % (len(got_many), SELFTEST_S_LIMIT, len(wrong), wrong[:3]))
        # ... and an `undeclared` line of another file is not the TU's (each dialect's measured lines, no compiler).
        other = []
        for d, text, want_undecl, want_row in SELFTEST_S_OTHER_FILE:
            found_o, undecl_o, _, _ = spelling_diagnostics(d, text, 'ref.c')
            if undecl_o != want_undecl or row_verdict(5, found_o, undecl_o) != want_row:
                other.append((d, sorted(undecl_o), row_verdict(5, found_o, undecl_o)))
        if other:
            failures.append('s-other-file: per dialect, the lines read undeclared and the verdict of the row on line '
                            '5: %r' % other)
        # ── a kept message: named relative to the scratch directory, and cut on a word ───────────────────────────
        so_k, err_k = ref_runner(cc, flags, dialect, work)(SELFTEST_K_REFUSED, 'k')
        if so_k is not None or 'ref_k.c' not in (err_k or '') or work in err_k or os.path.basename(work) in err_k:
            failures.append('k-scratch-path: the reference\'s refusal of ref_k.c, compiled in %r, was kept as %r'
                            % (work, err_k))
        cut_k = [(text, limit, kept_message(text, limit)) for text, limit, want in SELFTEST_K_KEPT
                 if kept_message(text, limit) != want]
        if cut_k or len(kept_message('word ' * 400)) > KEPT_LIMIT + len(' [...] '):
            failures.append('k-cut-on-a-word: %r; a 2000-character message kept at %d characters'
                            % (cut_k, len(kept_message('word ' * 400))))
        # ── part M (cl's measured diagnostics, read without cl) ──────────────────────────────────────────────────────
        for arm, text, want_found, want_undecl, want_stopped in SELFTEST_M:
            got = msvc_diagnostics(text, 'ref.c')
            if got != (want_found, want_undecl, want_stopped):
                failures.append('%s: read %r, want %r' % (arm, got, (want_found, want_undecl, want_stopped)))
        if row_verdict(15, {15: ('int *', 'int *')}, {15}) != 'REF-UNDECLARED':
            failures.append('m-c2065: an undeclared line that also spells a type was not REF-UNDECLARED')
        cl_banner, gcc_banner = SELFTEST_M_BANNER
        if not MSVC_BANNER.search(cl_banner) or MSVC_BANNER.search(gcc_banner):
            failures.append('m-banner: cl %r, gcc %r' % (bool(MSVC_BANNER.search(cl_banner)),
                                                       bool(MSVC_BANNER.search(gcc_banner))))
        for arm, text, names, want in SELFTEST_M_TAGS:
            got = msvc_tag_kinds(text, 'tags.c', names, 2)
            if got != want:
                failures.append('%s: read %r, want %r' % (arm, got, want))
        words = [(s, tagless_names(s), want) for s, want in SELFTEST_M_WORDS]
        if any(got != want for _, got, want in words):
            failures.append('m-tagless-names: %r' % [(s, got) for s, got, want in words if got != want])
        # ... and over rows, each name is asked about ONCE: a row's two spellings are one string on cl, and the first
        # row comes round again.
        once = tagless_candidates([(s, s) for s, _ in SELFTEST_M_WORDS] + [(SELFTEST_M_WORDS[0][0],) * 2])
        if once != [n for _, want in SELFTEST_M_WORDS for n in want]:
            failures.append('m-tagless-names: the names of the rows, each wanted once: %r' % once)
        spelled, kinds, want = SELFTEST_M_WITH_TAGS
        if with_tags(spelled, kinds) != want:
            failures.append('m-with-tags: %r' % with_tags(spelled, kinds))
        # ── part T (a tag word kept, and DSS takes the spelling) ──────────────────────────────────────────────────
        with open(os.path.join(work, 'census_probe_t.h'), 'w', encoding='utf-8', newline='\n') as o:
            o.write(SELFTEST_T_HEADER)
        rows_t = [{'rel': 'selftest', 'header': 'census_probe_t.h', 'name': n, 'kind': k} for _, n, k, _ in SELFTEST_T]
        spell_t = reference_spellings(rows_t, cc, flags + ['-I' + work], dialect, work)
        judge_t = dss_runner(dsscp, target, tree, work)
        for arm, name, _, phrase in SELFTEST_T:
            s = spell_t.get(('selftest', name))
            if not (isinstance(s, tuple) and phrase.replace(' ', '') in s[1].replace(' ', '')):
                failures.append('%s: spelling %r holds no %r' % (arm, s, phrase))
                continue
            text = '\n'.join(['#include <stdio.h>', SELFTEST_T_HEADER + SELFTEST_T_DEFINITIONS + 'int main(void) {',
                              '    puts(_Generic(&%s, typeof(%s): "yes", default: "NO"));' % (name, s[1]),
                              '    return 0;', '}']) + '\n'
            so, err = judge_t(text, arm)
            got = (so or '').strip() or 'DSS-REFUSED:%s' % err
            if got != 'yes':
                failures.append('%s: DSS verdict %r on the spelling %r' % (arm, got, s[1]))
        with open(os.path.join(work, 'census_probe_tl.h'), 'w', encoding='utf-8', newline='\n') as o:
            o.write(''.join('struct census_probe_many_%d { int x; };\nextern struct census_probe_many_%d '
                            'census_probe_many_obj_%d;\n' % (i, i, i) for i in range(SELFTEST_T_LIMIT)))
        got_tl = reference_spellings([{'rel': 'selftest', 'header': 'census_probe_tl.h',
                                       'name': 'census_probe_many_obj_%d' % i, 'kind': 'object'}
                                      for i in range(SELFTEST_T_LIMIT)], cc, flags + ['-I' + work], dialect, work)
        bare = []
        for i in range(SELFTEST_T_LIMIT):
            s = got_tl.get(('selftest', 'census_probe_many_obj_%d' % i))
            if not (isinstance(s, tuple) and s[1].replace(' ', '') == 'structcensus_probe_many_%d*' % i):
                bare.append(s)
        if bare:
            failures.append('t-error-limit: %d of %d spellings are not the tagged type (the first: %r)'
                            % (len(bare), SELFTEST_T_LIMIT, bare[:2]))
        # ── part D (a decoration is not handed on, and DSS takes the bare type) ───────────────────────────────────
        with open(os.path.join(work, 'census_probe_d.h'), 'w', encoding='utf-8', newline='\n') as o:
            o.write(SELFTEST_D_HEADER)
        gone_d = {}
        spell_d = reference_spellings([{'rel': 'selftest', 'header': 'census_probe_d.h', 'name': n,
                                        'kind': 'function'} for _, n, _, _, _ in SELFTEST_D],
                                      cc, flags + ['-I' + work], dialect, work, gone_d)
        ref_d = ref_runner(cc, flags + ['-I' + work], dialect, work)
        for arm, name, want, own, removed in SELFTEST_D:
            s = spell_d.get(('selftest', name))
            if not (isinstance(s, tuple) and s[1].replace(' ', '') == own.get(dialect, want)):
                failures.append('%s: spelling %r, want %r' % (arm, s, own.get(dialect, want)))
                continue
            if gone_d.get(('selftest', name), '') != removed.get(dialect, ''):
                failures.append('%s: recorded as removed %r, want %r' % (arm, gone_d.get(('selftest', name), ''),
                                                                        removed.get(dialect, '')))
                continue
            text = '\n'.join(['#include <stdio.h>', SELFTEST_D_DEFINITIONS + 'int main(void) {',
                              '    puts(_Generic(&%s, typeof(%s): "yes", default: "NO"));' % (name, s[1]),
                              '    return 0;', '}']) + '\n'
            so, err = judge_t(text, arm)
            got = (so or '').strip() or 'DSS-REFUSED:%s' % err
            if got != 'yes':
                failures.append('%s: DSS verdict %r on the spelling %r' % (arm, got, s[1]))
            text = '\n'.join(['#include <stdio.h>', '#include <census_probe_d.h>',
                              SELFTEST_D_DEFINITIONS + 'int main(void) {',
                              '    puts(_Generic(&%s, %s: "yes", default: "NO"));' % (name, s[1]),
                              '    return 0;', '}']) + '\n'
            so, err = ref_d(text, arm)
            got = (so or '').strip() or 'REF-REFUSED:%s' % err
            if got != 'yes':
                failures.append('%s: the reference\'s own verdict %r on the spelling %r, the type of a function it '
                                'decorated itself' % (arm, got, s[1]))
        unwitnessed = []
        for k, (text, where, witness) in enumerate(DECORATIONS[dialect]):
            written = '%s void (*)(int)' % text if where == 'leading' else 'void (*)(int) %s' % text
            with open(os.path.join(work, 'witness.c'), 'w', encoding='utf-8', newline='\n') as o:
                o.write('typedef typeof(%s) census_probe_witness_%d;\n' % (written, k))
            _, so, se = run([cc] + DIALECTS[dialect]['standard'] + flags + DIALECTS[dialect]['syntax'] + ['witness.c'],
                            work)
            if witness not in so + se:
                unwitnessed.append((text, where, witness, (so + se).strip()[:200]))
        if unwitnessed:
            failures.append('d-witness: the reference did not say what its entry records: %r' % unwitnessed)
        listed = [(d, s, undecorated(s, d), (bare, removed)) for d, s, bare, removed in SELFTEST_D_LIST]
        if any(got != want for _, _, got, want in listed):
            failures.append('d-closed-list: %r' % [(d, s, got) for d, s, got, want in listed if got != want])
        _, line_d = summarize({'format': 'selftest'},
                              [({'rel': 'selftest', 'name': 'd0', 'dropped': SELFTEST_D_LIST[0][3][0]}, 'ok', '', '',
                                'yes', 'yes'), ({'rel': 'selftest', 'name': 'd1'}, 'ok', '', '', 'yes', 'yes')],
                              dialect)
        if not line_d.endswith(' ref-decoration-dropped=1'):
            failures.append('d-counted: two rows, one judged after a removal, read %r' % line_d)
        # ... and a removal is WITNESSED row by row: by the leg's own compiler, on an object that points to a
        # noreturn function; and by a scripted judge, with no compiler, both where every row's program compiles and
        # where one row's does not (the rows are then judged one by one).
        with open(os.path.join(work, 'census_probe_dn.h'), 'w', encoding='utf-8', newline='\n') as o:
            o.write(SELFTEST_D_NESTED_HEADER)
        key_dn, gone_dn = ('selftest', 'census_probe_nested'), {}
        spell_dn = reference_spellings([{'rel': 'selftest', 'header': 'census_probe_dn.h',
                                         'name': 'census_probe_nested', 'kind': 'object'}],
                                       cc, flags + ['-I' + work], dialect, work, gone_dn).get(key_dn)
        want_dn = SELFTEST_D_NESTED[dialect]
        if not (isinstance(spell_dn, tuple) and spell_dn[1].replace(' ', '') == want_dn[0]) \
                or gone_dn.get(key_dn, '') != want_dn[1]:
            failures.append('d-nested: the address of an object that points to a noreturn function is handed on as '
                            '%r with %r recorded as removed; want %r with %r'
                            % (spell_dn, gone_dn.get(key_dn, ''), want_dn[0], want_dn[1]))
        texts_u = DECORATIONS['gcc'][1][0]
        bare_u, printed_u = ('void (**)(int)',) * 2, ('%s void (**)(int)' % texts_u,) * 2
        for rows_u in (SELFTEST_D_UNWITNESSED, [row for row in SELFTEST_D_UNWITNESSED if row[1] is not None]):
            spellings_u = {('selftest', name): bare_u for name, _ in rows_u}
            held_u = keep_unwitnessed('selftest', 'census_probe_u.h',
                                      {name: (texts_u, printed_u) for name, _ in rows_u}, spellings_u,
                                      scripted_judge({('witness', name): word for name, word in rows_u}))
            if held_u != {name: texts_u for name, word in rows_u if word == 'yes'} \
                    or spellings_u != {('selftest', name): bare_u if word == 'yes' else printed_u
                                       for name, word in rows_u}:
                failures.append('d-unwitnessed: of the rows %r, counted as removed: %r; handed on: %r'
                                % (rows_u, held_u, spellings_u))
                break
        # ── part R (a parameter's restrict, and the reference that keeps it) ──────────────────────────────────────
        with open(os.path.join(work, 'census_probe_r.h'), 'w', encoding='utf-8', newline='\n') as o:
            o.write(SELFTEST_R_HEADER)
        group_r = ('selftest', 'census_probe_r.h')
        runner_r = ref_runner(cc, flags + ['-I' + work], dialect, work)
        spell_r = reference_spellings([{'rel': 'selftest', 'header': 'census_probe_r.h', 'name': name,
                                        'kind': 'function'} for name, _, _ in SELFTEST_R],
                                      cc, flags + ['-I' + work], dialect, work)
        if not all(isinstance(spell_r.get(('selftest', name)), tuple) for name, _, _ in SELFTEST_R):
            for arm in ('b-restrict-parameter', 'b-restrict-right', 'b-restrict-wrong', 'b-restrict-again'):
                failures.append('%s: the reference did not spell the restrict header\'s functions: %r'
                                % (arm, spell_r))
        else:
            keeps = DIALECTS[dialect]['generic_keeps_restrict']
            plain_r = run_verdicts({group_r: [(name, right) for name, right, _ in SELFTEST_R]}, runner_r)
            plain_r = [verdict_of(plain_r.get(('selftest', name)))[0] for name, _, _ in SELFTEST_R]
            if plain_r != ['NO' if keeps else 'yes'] * len(SELFTEST_R):
                failures.append('b-restrict-parameter: the reference\'s plain answers to the right texts are %r; '
                                'its dialect says its _Generic %s a parameter\'s restrict (spellings: %r)'
                                % (plain_r, 'keeps' if keeps else 'erases', spell_r))
            for arm, column, want in (('b-restrict-right', 1, 'yes'), ('b-restrict-wrong', 2, 'NO')):
                got = reference_verdicts({group_r: [(row[0], row[column]) for row in SELFTEST_R]}, spell_r, runner_r,
                                         DIALECTS[dialect])
                got = [verdict_of(got.get(('selftest', row[0])))[0] or got.get(('selftest', row[0]))
                       for row in SELFTEST_R]
                if got != [want] * len(SELFTEST_R):
                    failures.append('%s: verdict (b) of the restrict header\'s functions against %r is %r, want %s '
                                    '(spellings: %r)' % (arm, [row[column] for row in SELFTEST_R], got, want,
                                                         spell_r))
            again = [asked_again({group_r: [(row[0], row[column]) for row in SELFTEST_R]}, spell_r,
                                 {('selftest', row[0]) for row in SELFTEST_R}, runner_r) for column in (1, 2)]
            again = [[answers.get(('selftest', row[0])) for row in SELFTEST_R] for answers in again]
            if again != [['yes'] * len(SELFTEST_R), ['NO'] * len(SELFTEST_R)]:
                failures.append('b-restrict-again: the question put with the qualifier on neither side answered %r '
                                'for the right texts and %r for the wrong ones (spellings: %r)'
                                % (again[0], again[1], spell_r))
        placed = [(spelled, without_parameter_restrict(spelled), (bare, removed, unplaced))
                  for spelled, bare, removed, unplaced in SELFTEST_R_PLACED]
        placed = [(spelled, got) for spelled, got, want in placed
                  if (got[0], got[1], len(got[2])) != want]
        if placed:
            failures.append('b-restrict-placed: %r' % placed)
        group_j = ('selftest', 'census_probe_j.h')
        spell_j = {('selftest', name): (spelled, spelled) for name, spelled, _, _, _, _, _ in SELFTEST_R_JUDGED}
        script_j = {}
        for name, _, plain, witness, bare, _, _ in SELFTEST_R_JUDGED:
            script_j.update({('', name): plain, ('witness', name): witness, ('bare', name): bare})
        items_j = [(name, 'void (*)(char *)') for name, _, _, _, _, _, _ in SELFTEST_R_JUDGED]
        got_j = reference_verdicts({group_j: items_j}, spell_j, scripted_judge(script_j),
                                   {'generic_keeps_restrict': True})
        wrong_j = []
        for name, _, _, _, _, want, phrase in SELFTEST_R_JUDGED:
            cell = got_j.get(('selftest', name))
            cell = verdict_of(cell)[0] or cell
            if (cell or '').split(':', 1)[0] != want or phrase not in (cell or ''):
                wrong_j.append((name, cell))
        erased_j = reference_verdicts({group_j: items_j[:2]}, spell_j, scripted_judge(script_j),
                                      {'generic_keeps_restrict': False})
        cannot = got_j.get(('selftest', 'j_unwitnessed'))
        _, line_j = summarize({'format': 'selftest'},
                              [({'rel': 'selftest', 'name': 'j'}, 'ok', '', '', 'NO', cannot)], dialect)
        if wrong_j or [erased_j.get(('selftest', name)) for name, _ in items_j[:2]] != ['NO', 'NO'] \
                or ' b-no=0 ' not in line_j or ' ref-cannot-judge=1 ' not in line_j:
            failures.append('b-cannot-judge: rows kept otherwise than wanted: %r; an erasing reference\'s two NOs '
                            'kept as %r; the line of one REF-CANNOT-JUDGE row: %r' % (wrong_j, erased_j, line_j))
        arm_n, header_n, name_n, want_n = SELFTEST_S_NO_HEADER
        got_n = reference_spellings([{'rel': 'selftest', 'header': header_n, 'name': name_n, 'kind': 'function'}],
                                    cc, flags + ['-I' + work], dialect, work).get(('selftest', name_n))
        if got_n != want_n:
            failures.append('%s: spelling %r, want %r' % (arm_n, got_n, want_n))
        for side, pairs, counts, stray in (('a', SELFTEST_ACCOUNTING_A, SELFTEST_ACCOUNTING_A_COUNTS,
                                            ('UNCOUNTED', 'yes')),
                                           ('b', SELFTEST_ACCOUNTING_B, SELFTEST_ACCOUNTING_B_COUNTS,
                                            ('yes', 'UNCOUNTED'))):
            rep = [({'rel': 'selftest', 'name': 'n%d' % i}, 'ok', '', '', va, vb) for i, (va, vb) in enumerate(pairs)]
            ok_all, line_all = summarize({'format': 'selftest'}, rep, dialect)
            if not (ok_all and line_all.startswith('prototype-census: OK ') and all(p in line_all for p in counts)):
                failures.append('s-accounting: a report of every verdict (%s) category read %r' % (side, line_all))
            ok_new, line_new = summarize({'format': 'selftest'},
                                         rep + [({'rel': 'selftest', 'name': 'u'}, 'ok', '', '') + stray], dialect)
            if ok_new or not line_new.startswith('prototype-census: FAILED') \
                    or 'a verdict (%s) this line does not count (UNCOUNTED)' % side not in line_new \
                    or 'a verdict (%s) this line' % ('b' if side == 'a' else 'a') in line_new:
                failures.append('s-accounting: a report holding a verdict (%s) the line does not count read %r'
                                % (side, line_new))
        # ── part B ────────────────────────────────────────────────────────────────────────────────────────────────
        os.makedirs(os.path.join(synth, 'src', 'dss-config', 'shippedLibs'))
        shutil.copytree(os.path.join(tree, 'src', 'dss-config', 'object-formats'),
                        os.path.join(synth, 'src', 'dss-config', 'object-formats'))
        for arm, name, sig, want in arms_b:
            with open(os.path.join(synth, 'src', 'dss-config', 'shippedLibs', 'string.json'), 'w',
                      encoding='utf-8') as o:
                json.dump({'header': 'string.h', 'standard': 'c89', 'symbols': [
                    {'name': name, 'signature': sig, 'kind': 'function', 'linkage': 'external'}]}, o)
            pair = pair_facts(synth, target)
            rows, idents = visible_symbols(synth, pair, 'string.h')
            if rows[0]['sig'] is None:
                failures.append('%s: the pair selected no arm of the synthetic signature' % arm)
                continue
            ptr = declarator(('ptr', Parser(rows[0]['sig'], idents).type()), '').strip()
            got = run_verdicts({('string.json', 'string.h'): [(name, ptr)]}, ref_runner(cc, flags, dialect, work))
            v = got.get(('string.json', name))
            v = v[0] if isinstance(v, tuple) else v
            if v != want:
                failures.append('%s: reference verdict %r, want %r' % (arm, v, want))
        # ── part A ────────────────────────────────────────────────────────────────────────────────────────────────
        judge = dss_runner(dsscp, target, tree, work)
        for arm, ctype, want in SELFTEST_A:
            text = '\n'.join(['#include <stdio.h>', 'void census_probe_f(char *p) { (void)p; }', 'int main(void) {',
                              '    puts(_Generic(&census_probe_f, typeof(%s): "yes", default: "NO"));' % ctype,
                              '    return 0;', '}']) + '\n'
            so, err = judge(text, arm)
            got = (so or '').strip() or 'DSS-REFUSED:%s' % err
            if got != want:
                failures.append('%s: DSS verdict %r, want %r' % (arm, got, want))
        # A judge that exits non-zero is never read as verdicts, even when it printed one.
        crash = '\n'.join(['#include <stdio.h>', 'int main(void) {', '    puts("yes census_probe_f");',
                           '    return 3;', '}']) + '\n'
        so, err = judge(crash, 'a-judge-exit')
        if so is not None or not (err or '').startswith('JUDGE-FAILED:exit 3'):
            failures.append('a-judge-exit: a judge exiting 3 read as %r / %r' % (so, err))
        # ── part V ────────────────────────────────────────────────────────────────────────────────────────────────
        vtree = os.path.join(synth, 'values-tree')
        shutil.copytree(os.path.join(tree, 'src', 'dss-config'), os.path.join(vtree, 'src', 'dss-config'))
        libs = os.path.join(vtree, 'src', 'dss-config', 'shippedLibs')
        with open(os.path.join(libs, 'census_probe_v.json'), 'w', encoding='utf-8') as o:
            json.dump({'header': 'census_probe_v.h', 'standard': 'c89',
                       'constants': [{'name': n, 'value': v, 'type': 'i32'} for _, n, v, _, _ in SELFTEST_V]}, o)
        arm_h, name_h, value_h, want_h = SELFTEST_V_NO_HEADER
        with open(os.path.join(libs, 'census_probe_dss_only.json'), 'w', encoding='utf-8') as o:
            json.dump({'header': 'census_probe_dss_only.h', 'standard': 'c89',
                       'constants': [{'name': name_h, 'value': value_h, 'type': 'i32'}]}, o)
        refdir = os.path.join(work, 'values-reference')
        os.makedirs(refdir)
        with open(os.path.join(refdir, 'census_probe_v.h'), 'w', encoding='utf-8', newline='\n') as o:
            o.write(''.join('#define %s %s\n' % (n, d) for _, n, _, d, _ in SELFTEST_V if d is not None))
        vpair = pair_facts(vtree, target)

        def fork(header, name, reference, dss):
            return {'format': vpair['format'], 'dialect': dialect, 'header': header, 'name': name,
                    'reference': reference, 'dss': dss, 'authority': 'the self-test', 'reason': 'the self-test'}
        elsewhere = [dict(fork('census_probe_v.h', 'CENSUS_PROBE_RIGHT', 6, 5), **other) for other in (
            {'format': next(k for k in ('elf', 'macho', 'pe') if k != vpair['format'])},
            {'dialect': next(k for k in sorted(DIALECTS) if k != dialect)})]
        _, vrows, vfail = values_census(vtree, target, dsscp, cc, flags + ['-I' + refdir], dialect, {
            ('census_probe_v.json', 'census_probe_v.h'): sorted(n for _, n, _, _, _ in SELFTEST_V),
            ('census_probe_dss_only.json', 'census_probe_dss_only.h'): [name_h]},
            decided=[fork('census_probe_v.h', 'CENSUS_PROBE_DECIDED', 6, 5)] + elsewhere)
        got = {n: v for _, n, _, _, v in vrows}
        for arm, name, _, _, want in SELFTEST_V + [(arm_h, name_h, value_h, None, want_h)]:
            if got.get(name) != want:
                failures.append('%s: values verdict %r, want %r (failures: %s)' % (arm, got.get(name), want,
                                                                                    '; '.join(vfail)))
        if vfail:
            failures.append('v-no-failure: the synthetic values run reported failures: %s' % '; '.join(vfail))
        if got.get('CENSUS_PROBE_RIGHT') != 'match' or any('CENSUS_PROBE_RIGHT' in f for f in vfail):
            failures.append('v-decided-other-pair: an entry of another format (%s) or of another dialect (%s) was '
                            'read on this pair: verdict %r, failures %r'
                            % (elsewhere[0]['format'], elsewhere[1]['dialect'], got.get('CENSUS_PROBE_RIGHT'), vfail))
        ok_v, line_v = values_summary(vpair, vrows, vfail)
        if ok_v or ' mismatch=1 decided=1 ' not in line_v or not line_v.startswith('prototype-census: VALUES FAILED '):
            failures.append('v-line: an undeclared mismatch beside a decided entry read %r' % line_v)
        moved = [n for _, n, _, _ in SELFTEST_V_MOVED[:2]]
        with open(os.path.join(libs, 'census_probe_f.json'), 'w', encoding='utf-8') as o:
            json.dump({'header': 'census_probe_f.h', 'standard': 'c89',
                       'constants': [{'name': n, 'value': 5, 'type': 'i32'} for n in moved]}, o)
        with open(os.path.join(refdir, 'census_probe_f.h'), 'w', encoding='utf-8', newline='\n') as o:
            o.write(''.join('#define %s 6\n' % n for n in moved))
        _, frows, ffail = values_census(vtree, target, dsscp, cc, flags + ['-I' + refdir], dialect, {
            ('census_probe_f.json', 'census_probe_f.h'): sorted(moved)},
            decided=[fork('census_probe_f.h', n, ref_v, dss_v) for _, n, (ref_v, dss_v), _ in SELFTEST_V_MOVED])
        for arm, name, _, phrase in SELFTEST_V_MOVED:
            said = [f for f in ffail if '> %s: ' % name in f]
            if len(said) != 1 or phrase not in said[0] or any(n == name for _, n, _, _, _ in frows):
                failures.append('%s: the run said %r of it (rows: %r)' % (arm, said, frows))
        if len(ffail) != len(SELFTEST_V_MOVED):
            failures.append('v-decided-unmet: %d failure(s) for %d entries that do not hold: %r'
                            % (len(ffail), len(SELFTEST_V_MOVED), ffail))
        table = os.path.join(work, 'census_probe_table.json')
        unrefused = []
        for phrase, doc in SELFTEST_V_TABLES + [('cannot read', None)]:
            with open(table, 'w', encoding='utf-8') as o:
                o.write('{' if doc is None else doc if isinstance(doc, str) else json.dumps(doc))
            try:
                unrefused.append((phrase, read_decided(table)))
            except ValueError as e:
                if phrase not in str(e):
                    unrefused.append((phrase, str(e)))
            except Exception as e:                      # no refusal at all: the reader BROKE on the document
                unrefused.append((phrase, '%s: %s' % (type(e).__name__, e)))
        with open(table, 'w', encoding='utf-8') as o:
            json.dump({'decided': [SELFTEST_V_ENTRY, dict(SELFTEST_V_ENTRY, name='CENSUS_PROBE_OTHER')]}, o)
        try:
            whole = len(read_decided(table))
        except ValueError as e:
            whole = str(e)
        if unrefused or whole != 2:
            failures.append('v-table-refused: tables read that must be refused, or refused in other words: %r; a '
                            'table of two whole entries read as %r' % (unrefused, whole))
        mine = [e for e in forks if e['format'] == vpair['format'] and e['dialect'] == dialect]
        print('prototype-census: decided forks: %d entr%s, %d of them this leg\'s (%s, %s)'
              % (len(forks), 'y' if len(forks) == 1 else 'ies', len(mine), vpair['format'], dialect))
        for e in forks:
            print('prototype-census: decided fork %s: %s' % ('/'.join(decided_key(e)),
                                                             'this leg\'s' if e in mine else 'not this leg\'s'))
        lost = decided_unreachable(tree, forks)
        probe = decided_unreachable(tree, [fork('census_probe_v.h', 'CENSUS_PROBE_DECIDED', 6, 5),
                                           dict(fork('stdio.h', 'TMP_MAX', 1, 2), dialect='census-probe-no-dialect')])
        if lost or len(probe) != 2 or 'no value named' not in probe[0][1] or 'reads no dialect' not in probe[1][1]:
            failures.append('v-decided-table: entries no run can meet: %r; two entries that cannot be met, a name '
                            'no descriptor has and a dialect not read, were reported %r' % (lost, probe))
    finally:
        shutil.rmtree(synth, ignore_errors=True)
        shutil.rmtree(work, ignore_errors=True)
    for f in failures:
        print('SELFTEST FAIL: ' + f)
    # f-percent-decoding, p-unknown-kind, r-absent-reference, a-judge-exit; part S + s-error-limit + s-other-file +
    # s-no-header + s-accounting; k-scratch-path and k-cut-on-a-word; part M + m-banner + the two tag readings +
    # m-tagless-names + m-with-tags; part T + t-error-limit; part D + d-witness + d-closed-list + d-counted +
    # d-nested + d-unwitnessed; part R's b-restrict-parameter, b-restrict-right, b-restrict-wrong, b-restrict-again,
    # b-restrict-placed and b-cannot-judge; parts B, A, V; v-no-header and v-no-failure; v-line and
    # v-decided-other-pair; the entries that no longer hold; v-table-refused and v-decided-table.
    n = (4 + len(SELFTEST_S) + 1 + 1 + 2 + 2 + len(SELFTEST_M) + 1 + len(SELFTEST_M_TAGS) + 2 + len(SELFTEST_T) + 1 +
         len(SELFTEST_D) + 3 + 2 + 6 + len(arms_b) + len(SELFTEST_A) + len(SELFTEST_V) + 2 + 2 +
         len(SELFTEST_V_MOVED) + 2)
    print('prototype-census: SELFTEST %s (%d arms)' % ('FAILED' if failures else 'OK', n))
    return 1 if failures else 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--tree', required=True)
    ap.add_argument('--processor', required=True)
    ap.add_argument('--dsscp', required=True)
    ap.add_argument('--cc', required=True)
    ap.add_argument('--flags', default='')
    ap.add_argument('--out', default='')
    ap.add_argument('--only', default='')
    ap.add_argument('--selftest', action='store_true')
    ap.add_argument('--values', action='store_true')
    a = ap.parse_args()
    flags = split_flags(a.flags)
    target, why = native_target(a.tree, a.processor, host_format_kind())
    if target is None:
        print('prototype-census: REFUSED — no native pair: %s' % why)
        return 2
    work = tempfile.mkdtemp(prefix='census-dialect-')
    try:
        dialect, banner = reference_dialect(a.cc, work)
    finally:
        shutil.rmtree(work, ignore_errors=True)
    if dialect is None:
        print('prototype-census: REFUSED — %s' % banner)
        return 2
    forks = []
    if a.selftest or a.values:
        try:
            forks = read_decided(os.path.join(os.path.dirname(os.path.abspath(__file__)), DECIDED_FORKS))
        except ValueError as e:
            print('prototype-census: REFUSED — the decided forks: %s' % e)
            return 2
    if a.selftest:
        return selftest(a.tree, target, a.dsscp, a.cc, flags, dialect, forks)
    if not a.out:
        print('prototype-census: REFUSED — --out names no report file')
        return 2
    if a.values:
        pair, rows, failures = values_census(a.tree, target, a.dsscp, a.cc, flags, dialect, decided=forks)
        with open(a.out, 'w', encoding='utf-8', newline='\n') as o:
            o.write('header\tname\tdss\treference\tverdict\n')
            for row in rows:
                o.write('\t'.join(str(x) for x in row) + '\n')
        for f in failures:
            print('VALUES FAIL: ' + f)
        for header, name, d, r, v in rows:
            if v == 'MISMATCH':
                print('VALUES MISMATCH: <%s> %s dss=%s reference=%s' % (header, name, d, r))
            elif v == 'decided':
                print('VALUES DECIDED: <%s> %s dss=%s reference=%s (%s)' % (header, name, d, r, DECIDED_FORKS))
        ok, line = values_summary(pair, rows, failures)
        print(line)
        return 0 if ok else 1
    pair, report = census(a.tree, target, a.dsscp, a.cc, flags, dialect, a.only or None)
    with open(a.out, 'w', encoding='utf-8', newline='\n') as o:
        o.write('descriptor\tsymbol\treference_declared\treference_canonical\tverdict_a_dss\tverdict_b_reference\t'
                'reference_decoration_dropped\n')
        for r, st, decl, canon, va, vb in report:
            o.write('\t'.join([r['rel'], r['name'], decl, canon, va, vb, r.get('dropped', '')]) + '\n')
    ok, line = summarize(pair, report, dialect)
    print(line)
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
