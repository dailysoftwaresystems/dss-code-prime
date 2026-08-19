#!/usr/bin/env python3
"""Support subcommands for tools/profile-compile.sh — the cross-leg compile-time
profile.

★ WHY THIS IS PYTHON AND NOT MORE SHELL: every piece here is either JSON
manipulation or a measurement whose correctness depends on WHICH CLOCK IS READ,
and both of those went wrong in shell during the ad-hoc runs this tool was
promoted from. See `timed-gate` for the clock, and `kit`/`manifest` for the
subject.

Subcommands
  kit         build a self-contained, RELOCATABLE profiling kit from a manifest
  manifest    materialize a kit's template onto THIS host's paths, verifying each
  build-type  read a compiler's CMake build type from the tree that produced it
  timed-gate  run a command under tools/run-gate.sh and time it on a MONOTONIC clock
  gcc-reference  the yardstick: build the SAME TUs with gcc, from the SAME manifest
  agg-trace   aggregate a DSS_OPT_TRACE log into per-pass totals

Every subcommand exits non-zero on any refusal, and prints WHAT it refused and
WHY — a profile that quietly measured something other than what was asked for is
the failure mode this whole tool exists to prevent.
"""

import argparse
import io
import json
import os
import re
import shutil
import subprocess
import sys
import time
from collections import defaultdict


def die(msg):
    sys.stderr.write('profile-compile-support: FATAL: %s\n' % msg)
    raise SystemExit(1)


def load_manifest(path):
    if not os.path.isfile(path):
        die('no such manifest: %s' % path)
    with io.open(path, encoding='utf-8') as f:
        return json.load(f)


# ── kit ─────────────────────────────────────────────────────────────────────
# ★★★ THE SUBJECT IS COPIED, NEVER RE-STAGED. The sqlite harness pulls upstream
# on EVERY run, so two hosts that each stage for themselves are not compiling the
# same program and their timings are not comparable. Copying makes the SUBJECT a
# constant and leaves the HOST as the only variable, which is the entire
# experiment. (Same reason the kit is transferred to each leg rather than rebuilt
# there.)
#
# ★ THE ROOTS ARE DECLARED BY THE CALLER, AND EVERY MANIFEST PATH MUST FALL UNDER
# EXACTLY ONE OF THEM. The predecessor of this code carried a hard-coded list of
# stage subdirectories ('sqlite/src', 'cfg', 'zinc', …) — a restatement of a
# layout that belongs to whoever produced the manifest, and one that goes silently
# stale the moment that producer adds a directory. Here the manifest itself says
# which files matter, and a path under NO declared root is a refusal that names
# the path rather than a file quietly missing from the kit.
def cmd_kit(args):
    roots = []
    for spec in args.root:
        if '=' not in spec:
            die('--root wants NAME=PATH, got %r' % spec)
        name, path = spec.split('=', 1)
        path = os.path.abspath(path)
        if not os.path.isdir(path):
            die('--root %s: no such directory: %s' % (name, path))
        roots.append((name, path.replace('\\', '/').rstrip('/')))
    if not roots:
        die('at least one --root NAME=PATH is required')

    d = load_manifest(args.manifest)
    out = os.path.abspath(args.out)
    if os.path.isdir(out):
        shutil.rmtree(out)
    os.makedirs(out)

    def tokenize(p):
        q = os.path.abspath(p).replace('\\', '/')
        for name, root in roots:
            if q == root or q.startswith(root + '/'):
                return name, root, '@KIT@/%s%s' % (name, q[len(root):])
        die('manifest path falls under no declared --root: %s\n'
            '    declared roots: %s' % (p, ', '.join('%s=%s' % r for r in roots)))

    copied_dirs = set()
    copied_files = 0

    def materialize(p, is_dir):
        nonlocal copied_files
        name, root, token = tokenize(p)
        rel = token[len('@KIT@/'):]
        dst = os.path.join(out, *rel.split('/'))
        src = os.path.abspath(p)
        if is_dir:
            if rel in copied_dirs:
                return token
            if not os.path.isdir(src):
                die('include dir named by the manifest does not exist: %s' % src)
            shutil.copytree(src, dst, dirs_exist_ok=True)
            copied_dirs.add(rel)
        else:
            if not os.path.isfile(src):
                die('source named by the manifest does not exist: %s' % src)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            shutil.copy2(src, dst)
            copied_files += 1
        return token

    # Include DIRECTORIES first: a source that also lives inside one is then a
    # cheap overwrite rather than a missing file.
    d['includes'] = [materialize(p, True) for p in d.get('includes', [])]
    d['sources'] = [materialize(p, False) for p in d.get('sources', [])]
    libs = []
    for e in d.get('resolveLibraries', []):
        libs.append(dict(e, path=materialize(e['path'], False)))
    d['resolveLibraries'] = libs

    tmpl = os.path.join(out, 'manifest.template.json')
    with io.open(tmpl, 'w', encoding='utf-8') as f:
        json.dump(d, f, indent=2)
    total = sum(os.path.getsize(os.path.join(r, f))
                for r, _, fs in os.walk(out) for f in fs)
    print('kit: %d sources, %d include dirs, %d libs, %.1f MiB -> %s'
          % (len(d['sources']), len(d['includes']), len(libs),
             total / 1048576.0, out))
    print('KIT-OK')


# ── manifest ────────────────────────────────────────────────────────────────
# Rewrites the kit template onto THIS host's paths. The template carries a
# literal @KIT@ prefix, so this is a SUBSTITUTION and never a guess about which
# path separator (or drive letter) the producing host used.
def cmd_manifest(args):
    kit = os.path.abspath(args.kit).replace('\\', '/')
    d = load_manifest(os.path.join(args.kit, 'manifest.template.json'))

    def fix(p):
        if '@KIT@' not in p:
            die('kit template path carries no @KIT@ token — it is not relocatable: %s' % p)
        return p.replace('@KIT@', kit).replace('\\', '/')

    d['sources'] = [fix(p) for p in d['sources']]
    d['includes'] = [fix(p) for p in d['includes']]
    d['resolveLibraries'] = [dict(e, path=fix(e['path']))
                             for e in d.get('resolveLibraries', [])]
    d['targets'] = [args.target]
    # EVERY path is checked before the compiler is ever started. A kit that lost
    # a file in transfer otherwise shows up as a compile error hundreds of lines
    # into a log, attributed to the compiler.
    missing = [p for p in d['sources'] + d['includes'] if not os.path.exists(p)]
    missing += [e['path'] for e in d['resolveLibraries']
                if not os.path.exists(e['path'])]
    if missing:
        die('%d manifest path(s) do not exist on this host:\n    %s%s'
            % (len(missing), '\n    '.join(missing[:10]),
               '\n    …' if len(missing) > 10 else ''))
    with io.open(args.out, 'w', encoding='utf-8') as f:
        json.dump(d, f, indent=2)
    print('manifest: %d sources, %d includes, target %s -> %s'
          % (len(d['sources']), len(d['includes']), args.target, args.out))
    print('MANIFEST-OK')


# ── build-type ──────────────────────────────────────────────────────────────
# ★★★ THE COMPILER'S BUILD TYPE IS A FACT ABOUT THE TREE THAT PRODUCED IT, so it
# is READ from that tree — never inferred from a directory name and never taken
# from the intent of whoever configured it. `build/rel` is a NAME;
# CMAKE_BUILD_TYPE is an ANSWER, and `cmake -B build/rel -DCMAKE_BUILD_TYPE=Debug`
# is one command away from making them disagree.
#
# ✔MEASURED 2026-08-18 — this is why the check exists at all rather than as
# ceremony: real-examples/c/sqlite/build-and-test.ps1 selected the NEWEST compiler
# under any build root regardless of type, which on a developer box is always
# `build/dbg`, while its .sh twin builds Release unconditionally. The resulting
# -O0-vs-O3 comparison was published as a HOST property
# (D-PERF-WINDOWS-HOST-COMPILES-8X-SLOWER-THAN-LINUX, ~8x); controlled, it is ~2.1x.
#
# ★ TWO GENERATOR FAMILIES KEEP THE ANSWER IN TWO PLACES:
#   * SINGLE-config (Ninja, Unix Makefiles): CMAKE_BUILD_TYPE is the answer, and
#     EMPTY is an answer too — CMake's default of no optimisation flags at all.
#   * MULTI-config (Visual Studio, Xcode): the cache CANNOT answer, because one
#     tree holds every config and CMAKE_BUILD_TYPE is an entry the generator
#     ignores. What the generator DOES state is the config it built, written into
#     the OUTPUT PATH — so the answer is the path component naming one of the
#     configs the cache DECLARES in CMAKE_CONFIGURATION_TYPES. Two facts the tree
#     states about itself, cross-checked; a component matching nothing declared is
#     not an answer.
def read_build_type(binary):
    b = os.path.abspath(binary)
    tree, walked = None, []
    d = os.path.dirname(b)
    while d:
        walked.append(d)
        if os.path.isfile(os.path.join(d, 'CMakeCache.txt')):
            tree = d
            break
        parent = os.path.dirname(d)
        if parent == d:
            break
        d = parent
    if tree is None:
        return ('<unknown>', 'NO CMakeCache.txt in any ancestor of %s — nothing '
                             'on this machine states how it was built (walked: %s)'
                % (b, '; '.join(walked)))
    cache = os.path.join(tree, 'CMakeCache.txt')
    btype = cfgs = gen = ''
    with io.open(cache, encoding='utf-8', errors='replace') as f:
        for ln in f:
            m = re.match(r'^CMAKE_BUILD_TYPE:[^=]*=(.*)$', ln)
            if m:
                btype = m.group(1).strip()
                continue
            m = re.match(r'^CMAKE_CONFIGURATION_TYPES:[^=]*=(.*)$', ln)
            if m:
                cfgs = m.group(1).strip()
                continue
            m = re.match(r'^CMAKE_GENERATOR:[^=]*=(.*)$', ln)
            if m:
                gen = m.group(1).strip()
    if cfgs:
        declared = [c for c in cfgs.split(';') if c]
        rel = b[len(tree):].strip('\\/')
        parts = [p for p in re.split(r'[\\/]', rel) if p]
        hits = [p for p in parts if p.lower() in [c.lower() for c in declared]]
        if hits:
            return (hits[-1],                       # DEEPEST match wins
                    "the multi-config generator's own output subdirectory %r, "
                    'cross-checked against CMAKE_CONFIGURATION_TYPES=%s in %s '
                    "(generator %r)" % (hits[-1], cfgs, cache, gen))
        return ('<unknown>',
                '%s is a MULTI-CONFIG tree (generator %r declares '
                'CMAKE_CONFIGURATION_TYPES=%s) and no component of %r names one of '
                'them' % (cache, gen, cfgs, rel))
    if btype:
        return (btype, 'CMAKE_BUILD_TYPE in %s (single-config generator %r)'
                % (cache, gen))
    return ('<none>', '%s declares NO CMAKE_BUILD_TYPE (single-config generator '
                      "%r) — CMake's default of no optimisation flags" % (cache, gen))


def cmd_build_type(args):
    btype, how = read_build_type(args.binary)
    print(btype)
    print(how)
    if args.require:
        # Case-insensitive on purpose: CMake uppercases the build type to look up
        # CMAKE_<LANG>_FLAGS_<CFG>, so `release` and `Release` select identical
        # flags and refusing one would be this check failing on a spelling.
        if btype.lower() != args.require.lower():
            die('compiler build type is %r, not %r — the measurement would not be '
                'comparable with any other leg.\n    compiler : %s\n    read from: %s'
                % (btype, args.require, args.binary, how))
    return 0


# ── timed-gate ──────────────────────────────────────────────────────────────
# ★★★ TWO SEPARATE DISCIPLINES, DELIBERATELY COMPOSED RATHER THAN REIMPLEMENTED:
#
# 1. THE WITNESS. tools/run-gate.sh already refuses to report success on an
#    exit-0 that produced no evidence of work, and captures rc DIRECTLY rather
#    than after a pipe. It is invoked here rather than copied — this tool's own
#    predecessor printed `PROFILE-LEG-OK vps-arm64 rc=1` over a compile that had
#    died before parsing a single file, which is precisely the self-authored
#    success string run-gate.sh exists to make unsayable.
#
# 2. THE CLOCK, AND IT MUST BE MONOTONIC. ✔MEASURED: the first gcc reference used
#    `date +%s%N` (CLOCK_REALTIME) and reported a compile that took MINUS 11.7
#    SECONDS — WSL2's CLOCK_REALTIME oscillates by tens of seconds every few
#    seconds on this machine (project_wsl2_clock_realtime_broken_2026_08_01). The
#    compiler's own `--time` uses steady_clock and was never affected, which is
#    exactly why the discrepancy was traceable at all.
#    ⚠ BOTH READS HAPPEN IN THIS ONE PROCESS. time.monotonic()'s reference point
#    is documented as undefined, so only differences taken WITHIN a process are
#    valid; timing by shelling out to two `python -c` calls would be relying on an
#    implementation detail (that every one of these OSes happens to use a
#    system-wide counter) rather than on the contract.
def cmd_timed_gate(args):
    gate = os.path.join(args.repo, 'tools', 'run-gate.sh')
    if not os.path.isfile(gate):
        die('tools/run-gate.sh not found under %s — the witness discipline is not '
            'optional, so this refuses rather than time an unwitnessed command'
            % args.repo)
    # ★ THE PATH IS BASH-FACING, SO IT CARRIES FORWARD SLASHES ON EVERY HOST.
    # os.path.join on Windows emits `…\tools\run-gate.sh`; bash then eats each
    # backslash as an escape and the invocation dies rc=127 on a mangled path
    # (`…baselinetoolsrun-gate.sh: No such file or directory`) — ✔MEASURED on the
    # documented Windows leg (Git Bash + Windows python), where every OTHER
    # consumer of this tool's paths is a native tool and never noticed. Windows
    # APIs accept forward slashes, so this is the one spelling correct for both.
    gate = gate.replace(os.sep, '/')
    # ★★★ AND THE SHELL IS RESOLVED BY ABSOLUTE PATH, NEVER BY BARE NAME —
    # ✔MEASURED on the same leg: shutil.which('bash') answered Git's
    # usr\bin\bash.EXE while subprocess.call(['bash', …]) executed a DIFFERENT,
    # extensionless `bash` sitting in the first directory of PATH, which printed
    # `/bin/bash: C:/…/run-gate.sh: No such file or directory` (rc=127) about a
    # file that provably exists and opens. The two resolvers disagree because
    # CreateProcess tries the extensionless name and shutil.which does not; a
    # bare interpreter name is therefore resolved by WHATEVER a machine's PATH
    # shadows, and this tool's contract is that the host is the only variable.
    shell = os.environ.get('DSS_PROFILE_BASH') or shutil.which('bash')
    if not shell:
        die('no bash resolvable on PATH for the timed gate — set DSS_PROFILE_BASH '
            'to an absolute bash path')
    cmd = [shell, gate, args.log, args.witness] + args.command
    t0 = time.monotonic()
    rc = subprocess.call(cmd)          # rc captured DIRECTLY, never after a pipe
    elapsed = time.monotonic() - t0
    print('TIMED-GATE elapsed=%.1fs rc=%d' % (elapsed, rc))
    return rc


# ── gcc-reference ───────────────────────────────────────────────────────────
# THE YARDSTICK. "First-class compile time" needs a number to be first-class
# AGAINST, and it must be the SAME TUs with the SAME defines and includes — which
# is why it is driven from the SAME materialized manifest rather than from its own
# file list.
# ✔MEASURED 2026-08-18 on WSL (x86_64), 103 TUs, gcc 13 -O2:
#       -j1  21.5 s      -j32  4.8 s        (DSS on that host: 1m40.8 s)
def cmd_gcc_reference(args):
    d = load_manifest(args.manifest)
    srcs = d['sources']
    flags = ['-D' + x for x in d.get('defines', [])] + \
            ['-I' + x for x in d.get('includes', [])]
    try:
        ver = subprocess.run([args.cc, '-dumpversion'], capture_output=True,
                             text=True).stdout.strip()
    except OSError as e:
        die('%s is not runnable on this host (%s)' % (args.cc, e))
    print('reference: %d TUs, %s %s, %s' % (len(srcs), args.cc, ver, args.opt))

    def run(label, jobs):
        od = os.path.join(args.out, 'gccref' + label.replace('-', ''))
        if os.path.isdir(od):
            shutil.rmtree(od)
        os.makedirs(od)
        t0 = time.monotonic()
        running, objs = [], []
        for i, f in enumerate(srcs):
            o = os.path.join(od, '%d.o' % i)
            objs.append(o)
            running.append(subprocess.Popen(
                [args.cc, args.opt, '-c', f, '-o', o, '-w'] + flags,
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
            while len(running) >= jobs:
                running = [p for p in running if p.poll() is None] or running
                if len(running) >= jobs:
                    running[0].wait()
                    running.pop(0)
        for p in running:
            p.wait()
        compile_s = time.monotonic() - t0
        t0 = time.monotonic()
        rc = subprocess.call([args.cc, args.opt, '-o', os.path.join(od, 'a.out')]
                             + objs + args.link_libs.split(),
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        link_s = time.monotonic() - t0
        # A missing object is a REFERENCE THAT DID NOT BUILD THE SAME PROGRAM, and
        # it is reported as such: the timing would otherwise be a fast number for
        # less work.
        bad = [o for o in objs if not os.path.exists(o)]
        print('%-7s compile=%7.1fs  link=%5.1fs  TOTAL=%7.1fs   (link rc=%d, %d/%d objects)'
              % (label, compile_s, link_s, compile_s + link_s, rc,
                 len(objs) - len(bad), len(objs)))
        return not bad and rc == 0

    ok = run('-j1', 1)
    ok = run('-j%d' % args.jobs, args.jobs) and ok
    if not ok:
        die('the reference did not build the same program — see the object counts '
            'above; a yardstick that compiled less than the subject is not a yardstick')
    print('GCC-REFERENCE-OK')


# ── agg-trace ───────────────────────────────────────────────────────────────
# ⚠ THE PER-CU OPTIMIZES RUN ON MANY THREADS AND THEIR LINES INTERLEAVE, so
# start/done pairing is impossible. It is also unnecessary: every `done` line
# carries its own duration, so durations aggregate without pairing.
# The MERGED whole-program optimize is the one that matters most and it runs
# ALONE, after every per-CU job has joined. The boundary is the LAST
# `iter=0 pass=Identity start` line — every optimize invocation begins with one,
# and the merged invocation is the final one to begin.
DONE = re.compile(r'^opt: iter=(\d+) pass=(\w+) done (\d+)ms mutated=(\d)')
START0 = re.compile(r'^opt: iter=0 pass=Identity start')
REDERIVE = re.compile(r'^opt:\s+rederiveStructCfMarkers whole-module (\d+)ms')
SUB = re.compile(r'^opt:\s+(\w+) sub: (.*)$')


def _tally(chunk, label):
    per_pass = defaultdict(lambda: [0, 0])
    per_iter = defaultdict(int)
    rederive = [0, 0]
    subs = defaultdict(lambda: defaultdict(int))
    for ln in chunk:
        m = DONE.match(ln)
        if m:
            per_pass[m.group(2)][0] += int(m.group(3))
            per_pass[m.group(2)][1] += 1
            per_iter[int(m.group(1))] += int(m.group(3))
            continue
        m = REDERIVE.match(ln)
        if m:
            rederive[0] += int(m.group(1))
            rederive[1] += 1
            continue
        m = SUB.match(ln)
        if m:
            for k, v in re.findall(r'(\w+)=(\d+)ms', m.group(2)):
                subs[m.group(1)][k] += int(v)
    total = sum(v[0] for v in per_pass.values())
    print('=== %s ===' % label)
    print('  %-14s%10s%8s%8s' % ('pass', 'total', 'calls', 'share'))
    for name, (ms, calls) in sorted(per_pass.items(), key=lambda kv: -kv[1][0]):
        print('  %-14s%9.2fs%8d%7.1f%%'
              % (name, ms / 1000.0, calls, (100.0 * ms / total) if total else 0.0))
    print('  %-14s%9.2fs' % ('PASS TOTAL', total / 1000.0))
    if rederive[1]:
        print('  %-14s%7.2fs%8d   (outside the pass loop)'
              % ('rederiveCfMarkers', rederive[0] / 1000.0, rederive[1]))
    if per_iter:
        print('  by iteration: %s'
              % '  '.join('iter%d=%.1fs' % (k, v / 1000.0)
                          for k, v in sorted(per_iter.items())))
    for owner, kv in sorted(subs.items()):
        print('  %s sub-timers: %s'
              % (owner, '  '.join('%s=%.2fs' % (k, v / 1000.0)
                                  for k, v in sorted(kv.items(), key=lambda x: -x[1]))))
    print()


def cmd_agg_trace(args):
    with io.open(args.log, encoding='utf-8', errors='replace') as f:
        lines = f.read().splitlines()
    starts = [i for i, ln in enumerate(lines) if START0.match(ln)]
    if not starts:
        die('%s carries no `opt: iter=0 pass=Identity start` line — either the run '
            'was not made with DSS_OPT_TRACE=1, or it never reached the optimizer'
            % args.log)
    print('optimize invocations (iter=0 Identity start): %d' % len(starts))
    boundary = starts[-1]
    print('merged-module optimize begins at line %d\n' % (boundary + 1))
    _tally(lines[:boundary],
           'PER-CU optimizes (x%d, PARALLEL — these ms are thread-time, not wall)'
           % max(0, len(starts) - 1))
    _tally(lines[boundary:],
           'MERGED whole-program optimize (x1, SERIAL — ms are wall)')
    print('AGG-TRACE-OK')


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest='cmd', required=True)

    p = sub.add_parser('kit', help='build a relocatable profiling kit')
    p.add_argument('--manifest', required=True, help='a real .dss-project.json')
    p.add_argument('--root', action='append', default=[], metavar='NAME=PATH',
                   help='declare a root every manifest path must fall under')
    p.add_argument('--out', required=True)
    p.set_defaults(fn=cmd_kit)

    p = sub.add_parser('manifest', help='materialize a kit onto this host')
    p.add_argument('--kit', required=True)
    p.add_argument('--target', required=True, help='e.g. x86_64:elf64-x86_64-linux-exec')
    p.add_argument('--out', required=True)
    p.set_defaults(fn=cmd_manifest)

    p = sub.add_parser('build-type', help="read a compiler's CMake build type")
    p.add_argument('binary')
    p.add_argument('--require', help='refuse unless the build type is this')
    p.set_defaults(fn=cmd_build_type)

    p = sub.add_parser('timed-gate', help='run under run-gate.sh, timed monotonically')
    p.add_argument('--repo', required=True)
    p.add_argument('--log', required=True)
    p.add_argument('--witness', required=True)
    p.add_argument('command', nargs=argparse.REMAINDER)
    p.set_defaults(fn=cmd_timed_gate)

    p = sub.add_parser('gcc-reference', help='the gcc yardstick, same manifest')
    p.add_argument('--manifest', required=True)
    p.add_argument('--out', required=True)
    p.add_argument('--jobs', type=int, default=os.cpu_count() or 4)
    p.add_argument('--cc', default='gcc')
    p.add_argument('--opt', default='-O2')
    p.add_argument('--link-libs', default='-lz -lm -lpthread -ldl')
    p.set_defaults(fn=cmd_gcc_reference)

    p = sub.add_parser('agg-trace', help='aggregate a DSS_OPT_TRACE log')
    p.add_argument('log')
    p.set_defaults(fn=cmd_agg_trace)

    args = ap.parse_args()
    if args.cmd == 'timed-gate' and args.command and args.command[0] == '--':
        args.command = args.command[1:]
    raise SystemExit(args.fn(args) or 0)


if __name__ == '__main__':
    main()
