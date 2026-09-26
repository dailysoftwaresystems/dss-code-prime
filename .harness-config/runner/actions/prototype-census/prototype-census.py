#!/usr/bin/env python3
"""prototype-census — does every prototype DSS ships have the TYPE the platform's own header gives it?

Over every `symbols` row of the tree's shipped descriptors (`src/dss-config/shippedLibs/**/*.json`) that the leg's
pair sees, TWO verdicts per symbol, each from a different judge, because each judge is blind where the other sees:

  (a) DSS JUDGES ITS OWN PROTOTYPE. The reference compiler first SPELLS the header's type for the symbol (a TU per
      header initializes a struct from each symbol; clang's refusal names the type, typedefs expanded in its `(aka
      …)` form). Then a program per header prints `_Generic(&sym, typeof(<that spelling>) *: "yes", default: "NO")`,
      compiled by the leg's dsscp for the leg's own pair and RUN on the leg. It answers with DSS's own type system,
      so it sees what DSS resolves — an untagged 64-bit core, a typedef's identity, a width — and nothing it
      re-implements. ★ KNOWN BLINDNESS: DSS's `_Generic` treats function types differing only in a parameter's
      POINTEE `const` as compatible (D-C-GENERIC-MATCHES-FUNCTION-TYPES-DIFFERING-IN-A-POINTEE-CONST, owned by the
      C semantics lane), so verdict (a) answers "yes" for a prototype that drops a `const`. The self-test PINS that
      blindness (arm `a-blind-const`): the day the fix lands the arm fails, and this note and the arm's expectation
      are updated together.
  (b) THE REFERENCE JUDGES THE DESCRIPTOR'S TEXT. The symbol's signature for the pair — the flat `signature`, or
      the ONE `variants` arm whose `when` the pair matches, else the row's `default` arm (the reader's own rule; a
      pair the reader would refuse is reported PAIR-REFUSED) — is rendered to C with typedef NAMES kept (the
      reference resolves them its own way), and the reference compiles and runs `_Generic(&sym, <rendered> *:
      "yes", default: "NO")` against its real header. It sees the prototype's SHAPE — a dropped `const`, an opaque
      type modelled `void *`, a comparator's parameters — which (a) cannot, and trusts the typedef names, which (a)
      checks.
  A symbol where the two disagree is a finding in itself; the report keeps both.

Verdicts: yes · NO · REF-UNDECLARED (the reference's header does not declare it) · REF-CANNOT-SPELL (the reference
cannot compile the rendered text, e.g. a DSS-only name) · DSS-REFUSED (DSS would not compile the judge probe) ·
PAIR-REFUSED (no arm of the row's `signature` serves the pair, which the descriptor reader refuses).
Last line: `prototype-census: OK format=… symbols=… a-yes=… a-no=… b-yes=… b-no=… disagree=… …`.

Usage: prototype-census.py --tree=<repo> (--os=<os> --processor=<cpu> | --target=<arch:format-doc>) --dsscp=<path>
                           --cc=<reference> [--flags=a,b] --out=<report.tsv> [--only=<header>]
       prototype-census.py --selftest --tree=<repo> (--os=… --processor=… | --target=…) --dsscp=<path> --cc=<ref>
The pair is the leg's NATIVE one (the judge program must RUN there), derived from the harness's {os}/{processor}."""
import argparse
import glob
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

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
MARKERS = ''.join('struct dss_census_bare_%s { int x; }; ' % c for c in sorted(BARE))
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
        return SCALAR[t[1]] if t[1] in SCALAR else 'struct dss_census_bare_%s' % t[1]
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


# ── the pair and the descriptors ─────────────────────────────────────────────────────────────────────────────────────
def pair_facts(tree, target):
    arch, _, fmt_doc = target.partition(':')
    d = json.load(open(os.path.join(tree, 'src', 'dss-config', 'object-formats', fmt_doc + '.format.json'),
                       encoding='utf-8'))
    return {'arch': arch, 'format': d['format']['kind'], 'dataModel': d.get('dataModel'),
            'longDoubleFormat': d.get('longDoubleFormat')}


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


def visible_symbols(tree, pair, only=None):
    root = os.path.join(tree, 'src', 'dss-config', 'shippedLibs')
    idents, docs = {}, []
    for f in sorted(glob.glob(os.path.join(root, '**', '*.json'), recursive=True)):
        d = json.load(open(f, encoding='utf-8'))
        docs.append((os.path.relpath(f, root).replace(os.sep, '/'), d))
        for st in d.get('structs', []):
            idents.setdefault(st['name'], STRUCT_SPELLING.get(st['name'], 'struct ' + st['name']))
        for td in d.get('typedefs', []):
            idents[td['name']] = td['name']
    rows = []
    for rel, d in docs:
        if pair['format'] not in (d.get('availableObjectFormats') or ['elf', 'macho', 'pe']):
            continue
        if only and d.get('header') != only:
            continue
        seen = set()
        for s in d.get('symbols', []):
            if pair['format'] not in (s.get('availableObjectFormats') or ['elf', 'macho', 'pe']) or s['name'] in seen:
                continue
            seen.add(s['name'])
            rows.append({'rel': rel, 'header': d['header'], 'name': s['name'], 'kind': s.get('kind', 'function'),
                         'sig': pair_signature(s, pair)})
    return rows, idents


# ── running things ───────────────────────────────────────────────────────────────────────────────────────────────────
def run(argv, cwd, timeout=300, env=None):
    p = subprocess.run(argv, cwd=cwd, capture_output=True, text=True, timeout=timeout, env=env, encoding='utf-8',
                       errors='replace')
    return p.returncode, p.stdout, p.stderr


CLANG_INIT = re.compile(r":(\d+):\d+: error: initializing 'struct dss_census_sink' with an expression of "
                        r"incompatible type '([^']*)'(?: \(aka '([^']*)'\))?")
UNDECLARED = re.compile(r":(\d+):\d+: error: (?:use of undeclared identifier|call to undeclared)")


def reference_spellings(rows, cc, flags, work):
    out, groups = {}, {}
    for r in rows:
        groups.setdefault((r['rel'], r['header']), []).append(r)
    for (rel, header), items in groups.items():
        lines = ['#include <%s>' % header, 'struct dss_census_sink { int x; };']
        first = len(lines) + 1
        lines += ['struct dss_census_sink dss_census_%d = %s%s;' % (i, '&' if r['kind'] == 'object' else '', r['name'])
                  for i, r in enumerate(items)]
        with open(os.path.join(work, 'ref.c'), 'w', encoding='utf-8', newline='\n') as o:
            o.write('\n'.join(lines) + '\n')
        _, _, err = run([cc] + flags + ['-fsyntax-only', '-ferror-limit=0', 'ref.c'], work)
        found = {int(m.group(1)): (m.group(2), m.group(3) or m.group(2)) for m in CLANG_INIT.finditer(err)}
        undecl = {int(m.group(1)) for m in UNDECLARED.finditer(err)}
        for i, r in enumerate(items):
            ln = first + i
            out[(rel, r['name'])] = found.get(ln, 'REF-UNDECLARED' if ln in undecl else 'REF-ERROR')
    return out


def verdict_program(header, lines, prelude=''):
    return '\n'.join(['#include <stdio.h>', '#include <%s>' % header, prelude, 'int main(void) {'] + lines +
                     ['    return 0;', '}']) + '\n'


def run_verdicts(groups, compile_and_run):
    """groups: {(rel, header): [(name, generic-association-type)]} → {(rel, name): verdict}; isolates refusals."""
    out = {}
    for (rel, header), items in groups.items():
        text = verdict_program(header, ['    puts(_Generic(&%s, %s: "yes %s", default: "NO %s"));' % (n, t, n, n)
                                        for n, t in items], prelude=MARKERS)
        so, err = compile_and_run(text, 'all')
        if so is not None:
            for line in so.splitlines():
                v, _, n = line.partition(' ')
                out[(rel, n)] = v
            continue
        for k, (n, t) in enumerate(items):
            one = verdict_program(header, ['    puts(_Generic(&%s, %s: "yes %s", default: "NO %s"));' % (n, t, n, n)],
                                  prelude=MARKERS)
            so, err = compile_and_run(one, 'one%d' % k)
            out[(rel, n)] = so.split(' ', 1)[0] if so else None, err
    return out


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
            return None, next((l for l in (so + se).splitlines() if 'error[' in l), (so + se).strip()[:160])
        stem = 'dss_%s' % tag
        exe = [p for p in glob.glob(os.path.join(outdir, '**', '*'), recursive=True)
               if os.path.isfile(p) and os.path.basename(p) in (stem, stem + '.exe', 'main', 'main.exe')]
        if not exe:
            return None, 'no artifact'
        os.chmod(exe[0], 0o755)
        return run([exe[0]], work, timeout=60)[1], None
    return go


def ref_runner(cc, flags, work):
    def go(text, tag):
        src = os.path.join(work, 'ref_%s.c' % tag)
        with open(src, 'w', encoding='utf-8', newline='\n') as o:
            o.write(text)
        exe = os.path.join(work, 'ref_%s.exe' % tag)
        rc, so, se = run([cc] + flags + ['-w', '-o', exe, src], work)
        if rc != 0:
            return None, next((l for l in se.splitlines() if 'error' in l), se.strip()[:160])
        return run([exe], work, timeout=60)[1], None
    return go


def census(tree, target, dsscp, cc, flags, only=None):
    pair = pair_facts(tree, target)
    rows, idents = visible_symbols(tree, pair, only)
    work = tempfile.mkdtemp(prefix='census-')
    try:
        spell = reference_spellings(rows, cc, flags, work)
        a_groups, b_groups, b_errors = {}, {}, {}
        for r in rows:
            s = spell.get((r['rel'], r['name']))
            if isinstance(s, tuple):
                a_groups.setdefault((r['rel'], r['header']), []).append((r['name'], 'typeof(%s) *' % s[1]))
                if r['sig'] is None:                        # the reader refuses this row on the pair
                    b_errors[(r['rel'], r['name'])] = 'PAIR-REFUSED:no arm of the signature serves this pair'
                    continue
                try:
                    t = Parser(r['sig'], idents).type()
                    ptr = declarator(('ptr', t), '').strip()
                    b_groups.setdefault((r['rel'], r['header']), []).append((r['name'], ptr))
                except Exception as e:                      # an unrenderable signature is a finding of its own
                    b_errors[(r['rel'], r['name'])] = 'UNRENDERABLE:%s' % e
        a = run_verdicts(a_groups, dss_runner(dsscp, target, tree, work))
        b = run_verdicts(b_groups, ref_runner(cc, flags, work))
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
        if isinstance(va, tuple):
            va = va[0] or 'DSS-REFUSED:' + (va[1] or '')[:120]
        if isinstance(vb, tuple):
            vb = vb[0] or 'REF-CANNOT-SPELL:' + (vb[1] or '')[:120]
        report.append((r, 'ok', s[0], s[1], va, vb))
    return pair, report


def summarize(pair, report):
    c = {}
    for r, st, _, _, va, vb in report:
        for k, v in (('a', va), ('b', vb)):
            key = '%s-%s' % (k, v.split(':', 1)[0])
            c[key] = c.get(key, 0) + 1
        if va in ('yes', 'NO') and vb in ('yes', 'NO') and va != vb:
            c['disagree'] = c.get('disagree', 0) + 1
    return ('prototype-census: OK format=%s symbols=%d a-yes=%d a-no=%d b-yes=%d b-no=%d disagree=%d '
            'ref-undeclared=%d dss-refused=%d ref-cannot-spell=%d pair-refused=%d'
            % (pair['format'], len(report), c.get('a-yes', 0), c.get('a-NO', 0), c.get('b-yes', 0), c.get('b-NO', 0),
               c.get('disagree', 0), c.get('a-REF-UNDECLARED', 0), c.get('a-DSS-REFUSED', 0),
               c.get('b-REF-CANNOT-SPELL', 0) + c.get('b-UNRENDERABLE', 0), c.get('b-PAIR-REFUSED', 0)))


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
# an exact match, a different parameter type, and ★ THE PINNED BLINDNESS: a parameter differing only in its POINTEE
# const, which DSS's `_Generic` accepts today (D-C-GENERIC-MATCHES-FUNCTION-TYPES-DIFFERING-IN-A-POINTEE-CONST). When
# the C semantics lane's fix lands, `a-blind-const` fails: flip its expectation to 'NO' and delete the KNOWN BLINDNESS
# note in this file's header in the same edit.
SELFTEST_A = [
    ('a-exact', 'void (char *)', 'yes'),
    ('a-different-type', 'void (long)', 'NO'),
    ('a-blind-const', 'void (const char *)', 'yes'),
]


def selftest(tree, target, dsscp, cc, flags):
    failures = []
    arms_b = selftest_b(pair_facts(tree, target))
    synth = tempfile.mkdtemp(prefix='census-selftest-')
    work = tempfile.mkdtemp(prefix='census-selftest-work-')
    try:
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
            got = run_verdicts({('string.json', 'string.h'): [(name, ptr)]}, ref_runner(cc, flags, work))
            v = got.get(('string.json', name))
            v = v[0] if isinstance(v, tuple) else v
            if v != want:
                failures.append('%s: reference verdict %r, want %r' % (arm, v, want))
        judge = dss_runner(dsscp, target, tree, work)
        for arm, ctype, want in SELFTEST_A:
            text = '\n'.join(['#include <stdio.h>', 'void dss_census_f(char *p) { (void)p; }', 'int main(void) {',
                              '    puts(_Generic(&dss_census_f, typeof(%s) *: "yes", default: "NO"));' % ctype,
                              '    return 0;', '}']) + '\n'
            so, err = judge(text, arm)
            got = (so or '').strip() or 'DSS-REFUSED:%s' % err
            if got != want:
                note = ' — has the pointee-const fix landed? update the pin' if arm == 'a-blind-const' else ''
                failures.append('%s: DSS verdict %r, want %r%s' % (arm, got, want, note))
    finally:
        shutil.rmtree(synth, ignore_errors=True)
        shutil.rmtree(work, ignore_errors=True)
    for f in failures:
        print('SELFTEST FAIL: ' + f)
    n = len(SELFTEST_A) + len(arms_b)
    print('prototype-census: SELFTEST %s (%d arms)' % ('FAILED' if failures else 'OK', n))
    return 1 if failures else 0


# The leg's NATIVE pair — the one whose judge program the leg can RUN — from the harness's own {os}/{processor}
# (derived by the tool, never typed). A leg this table does not name is refused by name, not guessed.
NATIVE_TARGET = {
    ('linux', 'x86_64'): 'x86_64:elf64-x86_64-linux-exec',
    ('linux', 'arm64'): 'arm64:elf64-aarch64-linux-exec',
    ('macos', 'arm64'): 'arm64:macho64-arm64-darwin-exec',
    ('windows', 'x86_64'): 'x86_64:pe64-x86_64-windows-exec',
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--tree', required=True)
    ap.add_argument('--target', default='')
    ap.add_argument('--os', default='')
    ap.add_argument('--processor', default='')
    ap.add_argument('--dsscp', required=True)
    ap.add_argument('--cc', default='clang')
    ap.add_argument('--flags', default='-std=gnu2x,-D_GNU_SOURCE')
    ap.add_argument('--out', default='')
    ap.add_argument('--only', default='')
    ap.add_argument('--selftest', action='store_true')
    a = ap.parse_args()
    flags = [f for f in a.flags.split(',') if f]
    if not a.target:
        a.target = NATIVE_TARGET.get((a.os, a.processor), '')
        if not a.target:
            print('prototype-census: REFUSED — no native pair for os=%r processor=%r (known: %s)'
                  % (a.os, a.processor, ', '.join('%s/%s' % k for k in sorted(NATIVE_TARGET))))
            return 2
    if a.selftest:
        return selftest(a.tree, a.target, a.dsscp, a.cc, flags)
    pair, report = census(a.tree, a.target, a.dsscp, a.cc, flags, a.only or None)
    with open(a.out, 'w', encoding='utf-8', newline='\n') as o:
        o.write('descriptor\tsymbol\treference_declared\treference_canonical\tverdict_a_dss\tverdict_b_reference\n')
        for r, st, decl, canon, va, vb in report:
            o.write('\t'.join([r['rel'], r['name'], decl, canon, va, vb]) + '\n')
    print(summarize(pair, report))
    return 0


if __name__ == '__main__':
    sys.exit(main())
