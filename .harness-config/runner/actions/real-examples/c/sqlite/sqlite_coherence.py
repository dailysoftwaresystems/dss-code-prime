#!/usr/bin/env python3
"""sqlite_coherence.py -- FAIL-LOUD coherence gate for a STAGED SQLITE SOURCE SET.

It replaced `check-source-coherence.sh` on 2026-09-21 (lane mig, part 4: no `.sh`/`.ps1` under
the actions directory). Same CLI, same exit codes, same report; the drivers import `run_check`
or run this file.

WHY THIS EXISTS -- the defect it was written for, MEASURED 2026-08-04. The harness reuses ONE
sqlite build directory across runs and pulls the checkout underneath it; `make` then regenerates
only what the requested target needs. The amalgamation artefacts (`sqlite3.c`, `shell.c`,
`tclsqlite3.c`, the `tsrc/` tree) are prerequisites of NOTHING the harness asks for, so they sat
in the build directory looking current while everything around them moved on: a JULY
`sqlite3.c` beside an AUGUST `sqlite3.h`, every file saying `SQLITE_VERSION "3.54.0"`. The
amalgamation inlines its own `SQLITE_SOURCE_ID`, but `shell.c` includes the `sqlite3.h` beside
it, so the CLI COMPILED and LINKED clean and then refused to run with sqlite's own "SQLite header
and source version mismatch". An instrument whose input silently changed vintage reports a pass
over work it did not do; this gate is what stops that.

TWO ID CLASSES, AND WHY ONLY ONE IS COMPARED WITH THE OTHER. A staged tree carries 64-hex fossil
check-in ids in two roles:
  IDENTITY  `SQLITE_SOURCE_ID` -- in `sqlite3.h`, inlined into `sqlite3.c` / `tclsqlite3.c` /
            `tsrc/sqlite3.h`, compiled into `libsqlite3.a`. Every copy MUST agree. ENFORCED.
  FTS5      `fts5: <date> <id>` -- baked into the generated `fts5.c` from the checkout AT
            GENERATION TIME; make legitimately leaves it alone while the tree moves, so in a
            reused build dir it LAGS (MEASURED: stamped 2026-07-24 while HEAD was 2026-08-03, the
            content current). It is REPORTED and never compared with the identity id; it IS
            enforced against ITSELF -- two different stamps in one stage means one copy is stale.
Both classes are printed on every run: silence about either is a gate bug.

USAGE
  sqlite_coherence.py [--checkout <sqlite-git-dir>] [--require-cli] [--label <name>] <dir> [<dir>...]
  sqlite_coherence.py --self-test              (alias --selftest)

  --checkout     also require the single identity id to equal the checkout's manifest.uuid --
                 the stage came from THAT source state, not merely from ONE state.
  --require-cli  also require the CLI triple (sqlite3.c, shell.c, sqlite3.h) in one directory:
                 shell.c's include is QUOTED, so a header missing beside it is silently taken
                 from an -I dir instead.
  --label        the name the report carries (default: the first directory).

EXIT: 0 coherent · 1 INCOHERENT (names the files and their ids) · 2 usage/IO. There is no
warn-and-continue path and no skip: a stage in which no identity id is found FAILS, because a
gate that finds nothing must never report OK.

WHAT THE PORT DECIDED (each pinned by a self-test arm):
  * the scan reads `.c`/`.h`/`.a` files at depth 1 and 2 below each directory (depth 3 is not
    read -- the old `find -maxdepth 2`), THROUGH symbolic links (a file the build reads through a
    link is an input; `find -type f` skipped it in silence); a file or directory that cannot be
    read is an I/O refusal (exit 2) naming it (`grep 2>/dev/null` skipped it in silence);
  * `--checkout` compares whenever there is exactly ONE identity id, even when the fts5 stamps
    diverge (the old gate compared only on an otherwise-clean stage and printed "matches" for an
    incoherent one); with SEVERAL identity ids it says it did NOT compare, never "matches";
  * `manifest.uuid` loses every whitespace byte, CR included (a CRLF checkout never matched);
  * `--label` without a value is a usage error (the old loop never ended);
  * with more than one directory, a divergence row names its directory;
  * ids and rows are ordered by code point (the old `sort` followed the host locale).
"""
from __future__ import annotations

import io
import os
import re
import shutil
import stat
import subprocess
import sys
import tempfile
import traceback

# A cp1252 console turns a printed glyph into a traceback; reconfigure before anything can print.
for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError, OSError):  # pragma: no cover - an odd stream
        pass

PROG = os.path.basename(__file__)
THIS = os.path.realpath(__file__)
# One pattern for both classes; the optional `fts5: ` prefix is what separates them. Matched
# over BYTES: libsqlite3.a is binary.
ID_RE = re.compile(rb"(?:fts5: )?20[0-9]{2}-[0-9]{2}-[0-9]{2} [0-9]{2}:[0-9]{2}:[0-9]{2} "
                   rb"[0-9a-f]{64}")
FTS5_PREFIX = "fts5: "
SCAN_SUFFIXES = (".c", ".h", ".a")
RULE = "-" * 66
DASH = "—"
USAGE = ("usage: %s [--checkout DIR] [--require-cli] [--label NAME] <dir>... | --self-test"
         % PROG)


class _Unreadable(Exception):
    pass


def _scan_files(d):
    """The `.c`/`.h`/`.a` files at depth 1 and 2 below `d`, code-point order of their paths,
    symbolic links followed (the depth bound makes a link cycle harmless)."""
    def entries(where):
        """[(path, name, is_file, is_dir)] of `where`, links followed; an OSError is an I/O
        refusal."""
        try:
            with os.scandir(where) as it:
                return [(e.path, e.name, e.is_file(), e.is_dir())
                        for e in sorted(it, key=lambda e: e.name)]
        except OSError as exc:
            raise _Unreadable("cannot list %s: %s" % (where, exc))
    found = []
    for path, name, is_file, is_dir in entries(d):
        if is_file and name.endswith(SCAN_SUFFIXES):
            found.append(path)
        elif is_dir:
            found += [p for p, n, f, _d in entries(path) if f and n.endswith(SCAN_SUFFIXES)]
    return sorted(found)


def scan_dir(d, name_dir=False):
    """-> (identity rows, fts5 rows), each row (where, id): ONE row per DISTINCT id per file, so a
    file holding two ids (sqlite3.c inlines fts5.c) reports both. `where` is the path relative
    to `d`, prefixed by `d` itself when `name_dir` (several directories are being scanned)."""
    ident, fts = [], []
    for f in _scan_files(d):
        try:
            with open(f, "rb") as fh:
                data = fh.read()
        except OSError as exc:
            raise _Unreadable("cannot read %s: %s" % (f, exc))
        rel = os.path.relpath(f, d).replace(os.sep, "/")
        where = os.path.join(d, rel) if name_dir else rel
        for raw in sorted({m.group(0).decode("ascii") for m in ID_RE.finditer(data)}):
            if raw.startswith(FTS5_PREFIX):
                fts.append((where, raw[len(FTS5_PREFIX):]))
            else:
                ident.append((where, raw))
    return ident, fts


def report_class(label, rows, enforce, out):
    """Print one class; -> 1 when its ids diverge and it is enforced, else 0. The report is
    printed either way: a class that is not enforced is still a fact the reader is owed."""
    ids = sorted({i for _w, i in rows})
    if not ids:
        out.write("   %-16s (none present)\n" % (label + ":"))
        return 0
    if len(ids) == 1:
        out.write("   %-16s %s\n" % (label + ":", ids[0]))
        out.write("   %-16s %d file(s) agree\n" % ("", len(rows)))
        return 0
    out.write("   %-16s %d DIFFERENT ids across %d file(s)%s\n"
              % (label + ":", len(ids), len(rows), "  <-- INCOHERENT" if enforce else ""))
    for where, i in sorted(rows, key=lambda r: (r[1], r[0])):
        out.write("        %-34s %s\n" % (where, i))
    return 1 if enforce else 0


def run_check(dirs, checkout=None, require_cli=False, label=None, out=None, err=None):
    """Judge the staged tree made of `dirs` -> 0 coherent, 1 incoherent, 2 usage/IO. The report
    goes to `out`, every `[X]` statement to `err` (stdout / stderr by default)."""
    out = sys.stdout if out is None else out
    err = sys.stderr if err is None else err
    dirs = list(dirs)
    if not dirs:
        err.write("%s: %s\n" % (PROG, USAGE))
        return 2
    out.write("\n== sqlite source coherence: %s ==\n" % (label or dirs[0]))
    ident, fts = [], []
    for d in dirs:
        if not os.path.isdir(d):
            err.write(" [X] not a directory: %s\n" % d)
            return 2
        out.write("   scanning        %s\n" % d)
        try:
            i, f = scan_dir(d, name_dir=len(dirs) > 1)
        except _Unreadable as exc:
            err.write(" [X] %s %s an unread file is not a coherent one; this gate refuses to "
                      "certify around it.\n" % (exc, DASH))
            return 2
        ident += i
        fts += f

    rc = 0
    rc |= report_class("SQLITE_SOURCE_ID", ident, True, out)
    rc |= report_class("fts5 stamp", fts, True, out)

    ids = sorted({i for _w, i in ident})
    if not ids:
        out.write(RULE + "\n")
        err.write(" [X] NO SQLITE_SOURCE_ID FOUND in: %s\n" % " ".join(dirs))
        err.write("     A coherence gate that finds nothing must not report OK. Either these\n")
        err.write("     are not sqlite source dirs, or sqlite3.h/sqlite3.c were never generated.\n")
        return 1
    uniq = ids[0]

    # --checkout: ONE state is necessary but not sufficient -- say WHICH state.
    if checkout:
        uuid_file = os.path.join(checkout, "manifest.uuid")
        if not os.path.isfile(uuid_file):
            err.write("%s: no manifest.uuid under %s\n" % (PROG, checkout))
            return 2
        try:
            with open(uuid_file, "rb") as fh:
                want = "".join(fh.read().decode("utf-8", "replace").split())
        except OSError as exc:
            err.write("%s: cannot read %s: %s\n" % (PROG, uuid_file, exc))
            return 2
        if len(ids) > 1:
            out.write("   %-16s NOT compared: the stage carries %d different SQLITE_SOURCE_IDs, and "
                      "no ONE checkout can certify several (%s/manifest.uuid is %s)\n"
                      % ("checkout:", len(ids), checkout, want))
        elif uniq.rsplit(" ", 1)[-1] != want:
            out.write(RULE + "\n")
            if rc == 0:
                err.write(" [X] the stage is internally coherent but does NOT match the checkout.\n")
            else:
                err.write(" [X] the stage's SQLITE_SOURCE_ID does NOT match the checkout either "
                          "(its fts5 stamps diverge, above).\n")
            err.write("     stage    : %s\n" % uniq)
            err.write("     checkout : %s  (%s/manifest.uuid)\n" % (want, checkout))
            rc = 1
        else:
            out.write("   %-16s matches %s/manifest.uuid\n" % ("checkout:", checkout))

    # --require-cli: shell.c's include is QUOTED, so it resolves beside shell.c FIRST. A stage
    # that ships shell.c without its own sqlite3.h silently picks one up from an -I dir, which is
    # exactly how the mixed-vintage binary was built. The LAST directory holding shell.c is the
    # one judged (the old rule, kept).
    if require_cli:
        sdir = None
        for d in dirs:
            if os.path.isfile(os.path.join(d, "shell.c")):
                sdir = d
        missing = [] if sdir else ["shell.c"]
        for f in ("sqlite3.c", "sqlite3.h"):
            if not sdir or not os.path.isfile(os.path.join(sdir, f)):
                missing.append(f)
        if missing:
            out.write(RULE + "\n")
            err.write(" [X] --require-cli: the CLI triple is incomplete; missing: %s\n"
                      % " ".join(missing))
            err.write('     shell.c does #include "sqlite3.h" (QUOTED) %s the header MUST sit\n'
                      % DASH)
            err.write("     beside it or the build silently takes one from an -I dir.\n")
            rc = 1
        else:
            out.write("   %-16s sqlite3.c + shell.c + sqlite3.h all present in %s\n"
                      % ("CLI triple:", sdir))

    out.write(RULE + "\n")
    if rc == 0:
        out.write(" [OK] coherent at %s\n" % uniq)
    else:
        err.write(" [X] INCOHERENT SQLITE SOURCE STAGE %s refusing to certify this tree.\n" % DASH)
        err.write("     The staged tree is of MIXED VINTAGE. A build from these inputs can\n")
        err.write("     COMPILE AND LINK CLEAN and then die at startup with sqlite's own\n")
        err.write('     "SQLite header and source version mismatch". Regenerate the derived files\n')
        err.write("     from ONE source state using the tree's own rules, e.g. in the build dir:\n")
        err.write("         make sqlite3.c shell.c tclsqlite3.c\n")
        err.write("     (hand-copying one file is a workaround: it fixes the symptom you noticed\n")
        err.write("     and leaves the ones you did not.)\n")
    return rc


# ── argv ─────────────────────────────────────────────────────────────────────────────

def parse_cli(args):
    """-> ("self-test",) | ("help",) | ("usage", message) | ("check", dirs, checkout, cli, label).
    `--self-test` runs as soon as it is reached (the old parser's order)."""
    checkout, require_cli, label, dirs = None, False, None, []
    i = 0
    while i < len(args):
        a = args[i]
        if a in ("--self-test", "--selftest"):
            return ("self-test",)
        if a == "--checkout":
            value = args[i + 1] if i + 1 < len(args) else ""
            if not value:
                return ("usage", "--checkout needs a directory")
            checkout, i = value, i + 2
        elif a == "--require-cli":
            require_cli, i = True, i + 1
        elif a == "--label":
            if i + 1 >= len(args):
                return ("usage", "--label needs a value")
            label, i = args[i + 1] or None, i + 2
        elif a in ("-h", "--help"):
            return ("help",)
        elif a.startswith("-"):
            return ("usage", "unknown option: %s" % a)
        else:
            dirs.append(a)
            i += 1
    if not dirs:
        return ("usage", USAGE)
    return ("check", dirs, checkout, require_cli, label)


def main(argv=None):
    act = parse_cli(list(sys.argv[1:] if argv is None else argv))
    if act[0] == "self-test":
        return self_test()
    if act[0] == "help":
        doc = __doc__.split("USAGE\n", 1)[1].split("\nWHAT THE PORT DECIDED", 1)[0]
        print("USAGE\n" + doc.rstrip())
        return 0
    if act[0] == "usage":
        print("%s: %s" % (PROG, act[1]), file=sys.stderr)
        return 2
    _check, dirs, checkout, require_cli, label = act
    return run_check(dirs, checkout, require_cli, label)


# ── SELF-TEST -- red-on-disable, by construction ─────────────────────────────────────
#
# Every arm builds a fixture that DIFFERS FROM A PASSING ONE IN ONE WAY and demands the gate
# change verdict; an arm that asserts an ABSENCE proves, beside it, that the thing can be
# present. `scNN` = the old gate's 11 checks (labels kept), `nNN` = new.

EXPECTED_ARMS = 32         # 11 sc + 21 new

ID_A = "2026-07-06 16:26:30 7f49a7a90eda01753c0dff65197bd7bc48a751e24a46919d30af6e2baf0788fc"
ID_B = "2026-08-03 15:05:05 0f873f565192e7d1e0bfa1f1c147d02f6cca5b91492f4498736d2c8896599e9d"
ID_F = "2026-07-24 16:28:47 2f1f4f73535386549c12694dc57cfe555eec689ae6824c6241aaf8d5befcd74d"

_SKIP = object()


def _put(path, data):
    parent = os.path.dirname(path)
    if parent and not os.path.isdir(parent):
        os.makedirs(parent)
    with open(path, "wb") as fh:
        fh.write(data if isinstance(data, bytes) else data.encode("utf-8"))


def _mk_hdr(path, source_id):
    _put(path, '#define SQLITE_VERSION "3.54.0"\n#define SQLITE_SOURCE_ID      "%s"\n' % source_id)


def _mk_fts(path, source_id):
    _put(path, 'sqlite3_result_text(p,"fts5: %s",-1,0);\n' % source_id)


def _hash(source_id):
    return source_id.rsplit(" ", 1)[-1]


def _run(dirs, checkout=None, require_cli=False, label=None):
    """(rc, merged report) -- stdout and stderr into ONE stream, in write order (the old `2>&1`)."""
    buf = io.StringIO()
    rc = run_check(dirs, checkout, require_cli, label, out=buf, err=buf)
    return rc, buf.getvalue()


class _Arms:
    def __init__(self):
        self.passed = self.failed = self.skipped = self.ran = 0
        self.labels = set()

    def arm(self, label, fn):
        self.ran += 1
        if label in self.labels:
            self._fail(label, "DUPLICATE arm label (the count would lie)")
            return
        self.labels.add(label)
        try:
            r = fn()
        except Exception as exc:  # an arm that crashes is a FAILED arm, never a silent one
            self._fail(label, "raised %s: %s" % (type(exc).__name__, exc))
            return
        ok, detail = r if isinstance(r, tuple) else (r, "")
        if ok is _SKIP:
            self.skipped += 1
            print("  [SKIP] %s %s %s" % (label, DASH, detail))
        elif ok:
            self.passed += 1
            print("  [PASS] %s" % label)
        else:
            self._fail(label, detail)

    def _fail(self, label, detail):
        self.failed += 1
        print("  [FAIL] %s" % label)
        for line in str(detail).split("\n"):
            if line:
                print("         %s" % line)

    def finish(self, expected):
        if self.ran != expected:
            self.failed += 1
            print("  [FAIL] ran %d arm(s), but EXPECTED_ARMS declares %d %s an arm was added, "
                  "removed or never reached" % (self.ran, expected, DASH))
        print("\npassed=%d failed=%d skipped=%d" % (self.passed, self.failed, self.skipped))
        return 0 if self.failed == 0 else 1


def _expect(rc_want, needles=(), absent=()):
    """An arm body: (rc, report) must have rc `rc_want`, carry every needle and none of `absent`."""
    def check(result):
        rc, text = result
        ok = (rc == rc_want and all(n in text for n in needles)
              and not any(a in text for a in absent))
        return ok, "want rc=%d, needles %r, absent %r\ngot rc=%d:\n%s" % (
            rc_want, list(needles), list(absent), rc, text)
    return check


def _cli(args, timeout=60):
    return subprocess.run([sys.executable, THIS] + list(args), stdin=subprocess.DEVNULL,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=timeout)


def _body(A, T):
    ok = os.path.join(T, "ok")
    _mk_hdr(os.path.join(ok, "sqlite3.h"), ID_B)
    _mk_hdr(os.path.join(ok, "sqlite3.c"), ID_B)
    A.arm("sc01 coherent stage passes", lambda: _expect(0, ["[OK] coherent"])(_run([ok])))

    mix = os.path.join(T, "mix")
    _mk_hdr(os.path.join(mix, "sqlite3.h"), ID_B)
    _mk_hdr(os.path.join(mix, "sqlite3.c"), ID_A)
    mixed = _run([mix])
    A.arm("sc02 mixed SQLITE_SOURCE_ID fails", lambda: _expect(1, ["INCOHERENT"])(mixed))
    A.arm("sc03   … and NAMES the divergent files",
          lambda: (mixed[0] == 1
                   and re.search(r"^ {8}sqlite3\.c +" + re.escape(ID_A) + "$", mixed[1], re.M)
                   is not None
                   and re.search(r"^ {8}sqlite3\.h +" + re.escape(ID_B) + "$", mixed[1], re.M)
                   is not None, mixed[1]))
    A.arm("sc04   … and PRINTS both ids", lambda: _expect(1, [ID_A, ID_B])(mixed))

    empty = os.path.join(T, "empty")
    os.makedirs(empty)
    A.arm("sc05 stage with no id at all fails",
          lambda: _expect(1, ["NO SQLITE_SOURCE_ID FOUND"])(_run([empty])))

    fts = os.path.join(T, "fts")
    _mk_hdr(os.path.join(fts, "sqlite3.h"), ID_B)
    _mk_fts(os.path.join(fts, "fts5.c"), ID_F)
    fts2 = os.path.join(T, "fts2")
    _mk_hdr(os.path.join(fts2, "sqlite3.h"), ID_B)
    _mk_fts(os.path.join(fts2, "fts5.c"), ID_F)
    _mk_fts(os.path.join(fts2, "tsrc", "fts5.c"), ID_A)
    A.arm("sc06 lagging fts5 stamp does NOT false-red",
          lambda: (lambda r: (r[0] == 0 and _hash(ID_F)[:8] in r[1] and "[OK] coherent" in r[1]
                              and _run([fts2])[0] == 1, r[1]))(_run([fts])))
    A.arm("sc07 two different fts5 stamps fail",
          lambda: (lambda r: (r[0] == 1 and re.search(r"fts5 stamp: +2 DIFFERENT ids", r[1])
                              is not None, r[1]))(_run([fts2])))

    co = os.path.join(T, "co")
    _put(os.path.join(co, "manifest.uuid"), _hash(ID_A) + "\n")
    A.arm("sc08 coherent-but-wrong-checkout fails",
          lambda: _expect(1, ["does NOT match the checkout"])(_run([ok], checkout=co)))
    co_b = os.path.join(T, "co-b")
    _put(os.path.join(co_b, "manifest.uuid"), _hash(ID_B) + "\n")
    A.arm("sc09 coherent-and-right-checkout passes",
          lambda: _expect(0, ["matches"])(_run([ok], checkout=co_b)))

    cli = os.path.join(T, "cli")
    _mk_hdr(os.path.join(cli, "sqlite3.c"), ID_B)
    _put(os.path.join(cli, "shell.c"), '#include "sqlite3.h"\n')
    A.arm("sc10 CLI stage missing sqlite3.h fails",
          lambda: _expect(1, ["CLI triple is incomplete"])(_run([cli], require_cli=True)))
    cli_ok = os.path.join(T, "cli-ok")
    _mk_hdr(os.path.join(cli_ok, "sqlite3.c"), ID_B)
    _mk_hdr(os.path.join(cli_ok, "sqlite3.h"), ID_B)
    _put(os.path.join(cli_ok, "shell.c"), '#include "sqlite3.h"\n')
    A.arm("sc11 complete CLI stage passes",
          lambda: _expect(0, ["CLI triple:"])(_run([cli_ok], require_cli=True)))

    # ── new arms ──
    notdir = os.path.join(T, "plain-file")
    _put(notdir, "x")
    A.arm("n01 a non-directory is a usage/IO error (exit 2)",
          lambda: _expect(2, ["[X] not a directory: " + notdir])(_run([notdir])))
    A.arm("n02 --checkout without a manifest.uuid is exit 2",
          lambda: _expect(2, ["no manifest.uuid under " + empty])(_run([ok], checkout=empty)))
    A.arm("n03 NO false 'matches' on an incoherent tree (control: the same checkout on a coherent "
          "tree prints it)",
          lambda: (lambda r, c: (r[0] == 1 and "matches" not in r[1] and "NOT compared" in r[1]
                                 and c[0] == 0 and ("matches %s/manifest.uuid" % co_b) in c[1],
                                 r[1]))(
              _run([mix], checkout=co_b), _run([ok], checkout=co_b)))

    def crlf_uuid():
        co_crlf = os.path.join(T, "co-crlf")
        _put(os.path.join(co_crlf, "manifest.uuid"), _hash(ID_B).encode() + b"\x0d\n")
        with open(os.path.join(co_crlf, "manifest.uuid"), "rb") as fh:
            has_cr = b"\x0d" in fh.read()
        return (lambda r: (has_cr and r[0] == 0 and "matches" in r[1], r[1]))(
            _run([ok], checkout=co_crlf))
    A.arm("n04 a CRLF manifest.uuid still matches (control: the fixture really holds CR)",
          crlf_uuid)

    two_a, two_b = os.path.join(T, "two-a"), os.path.join(T, "two-b")
    _mk_hdr(os.path.join(two_a, "sqlite3.h"), ID_B)
    _mk_hdr(os.path.join(two_b, "sqlite3.c"), ID_A)
    two = _run([two_a, two_b])
    A.arm("n05 a divergence ACROSS two directories fails and prints both ids",
          lambda: _expect(1, [ID_A, ID_B, "INCOHERENT"])(two))
    A.arm("n06 with two directories each divergence row NAMES its directory",
          lambda: _expect(1, [os.path.join(two_b, "sqlite3.c"),
                              os.path.join(two_a, "sqlite3.h")])(two))

    d2 = os.path.join(T, "depth2")
    _mk_hdr(os.path.join(d2, "sqlite3.h"), ID_B)
    _mk_hdr(os.path.join(d2, "sub", "x.c"), ID_A)
    A.arm("n07 depth 2 IS scanned (a diverging id one directory down reds)",
          lambda: _expect(1, [ID_A, "sub/x.c"])(_run([d2])))
    d3 = os.path.join(T, "depth3")
    _mk_hdr(os.path.join(d3, "sqlite3.h"), ID_B)
    _mk_hdr(os.path.join(d3, "sub", "deeper", "x.c"), ID_A)
    A.arm("n08 depth 3 is NOT scanned (control: the same id at depth 2 reds)",
          lambda: (lambda r: (r[0] == 0 and "[OK] coherent" in r[1] and ID_A not in r[1]
                              and _run([d2])[0] == 1, r[1]))(_run([d3])))

    def binary_archive():
        ar = os.path.join(T, "ar")
        _mk_hdr(os.path.join(ar, "sqlite3.h"), ID_B)
        _put(os.path.join(ar, "libsqlite3.a"),
             b"!<arch>\n\x00\x01\xff" + ID_A.encode("ascii") + b"\x00\xfe\n")
        clean = os.path.join(T, "ar-clean")
        _mk_hdr(os.path.join(clean, "sqlite3.h"), ID_B)
        _put(os.path.join(clean, "libsqlite3.a"), b"!<arch>\n\x00\x01\xff no id here \x00\xfe\n")
        r, c = _run([ar]), _run([clean])
        return (r[0] == 1 and ID_A in r[1] and "libsqlite3.a" in r[1] and c[0] == 0, r[1])
    A.arm("n09 an id inside a BINARY .a is read (control: the same .a without it passes)",
          binary_archive)

    no_c = os.path.join(T, "cli-no-c")
    _mk_hdr(os.path.join(no_c, "sqlite3.h"), ID_B)
    _put(os.path.join(no_c, "shell.c"), '#include "sqlite3.h"\n')
    A.arm("n10 shell.c WITHOUT sqlite3.c fails --require-cli, naming sqlite3.c",
          lambda: _expect(1, ["missing: sqlite3.c"])(_run([no_c], require_cli=True)))

    def label_hang():
        try:
            p = _cli([ok, "--label"], timeout=60)
        except subprocess.TimeoutExpired:
            return False, "the CLI did not return within 60 s (the old loop never ended)"
        return (p.returncode == 2 and b"--label needs a value" in p.stderr,
                "rc %d, stderr %r" % (p.returncode, p.stderr))
    A.arm("n11 --label WITHOUT a value is exit 2, promptly (the old parser looped forever)",
          label_hang)

    def fts_masks():
        fm = os.path.join(T, "fts-mask")
        _mk_hdr(os.path.join(fm, "sqlite3.h"), ID_B)
        _mk_fts(os.path.join(fm, "fts5.c"), ID_F)
        _mk_fts(os.path.join(fm, "tsrc", "fts5.c"), ID_A)
        ok_a = os.path.join(T, "ok-a")
        _mk_hdr(os.path.join(ok_a, "sqlite3.h"), ID_A)
        control = _run([ok_a], checkout=co)
        rc, text = _run([fm], checkout=co)
        return (rc == 1 and "does NOT match the checkout" in text and "stage    : " + ID_B in text
                and ("matches %s" % co) not in text
                and control[0] == 0 and ("matches %s" % co) in control[1], text)
    A.arm("n12 an fts5 divergence does NOT mask a checkout mismatch (control: a stage AT that "
          "checkout prints 'matches')", fts_masks)

    def usage_codes():
        p1, p2, p3 = _cli([]), _cli(["--bogus", ok]), _cli([ok, "--checkout"])
        return ([p.returncode for p in (p1, p2, p3)] == [2, 2, 2]
                and b"usage:" in p1.stderr and b"unknown option: --bogus" in p2.stderr
                and b"--checkout needs a directory" in p3.stderr,
                "rcs %r / %r / %r" % ([p.returncode for p in (p1, p2, p3)], p2.stderr, p3.stderr))
    A.arm("n13 CLI usage errors are exit 2: no directory, an unknown option, --checkout without "
          "a value", usage_codes)

    def cli_ok_run():
        p = _cli([ok])
        so, se = p.stdout.decode("utf-8"), p.stderr.decode("utf-8")
        bad = _cli([mix])
        return (p.returncode == 0 and ("== sqlite source coherence: %s ==" % ok) in so
                and (" [OK] coherent at %s" % ID_B) in so and "[X]" not in se
                and b"[X]" in bad.stderr, "rc %d\nstdout %s\nstderr %s" % (p.returncode, so, se))
    A.arm("n14 CLI end to end: exit 0, the report on STDOUT, labelled by the first directory "
          "(control: an incoherent stage does write [X] to stderr)", cli_ok_run)

    def cli_bad_run():
        p = _cli(["--label", "staged sqlite (test)", mix])
        so, se = p.stdout.decode("utf-8"), p.stderr.decode("utf-8")
        return (p.returncode == 1 and "== sqlite source coherence: staged sqlite (test) ==" in so
                and "[X] INCOHERENT SQLITE SOURCE STAGE" in se and "[X]" not in so,
                "rc %d\nstdout %s\nstderr %s" % (p.returncode, so, se))
    A.arm("n15 CLI: an incoherent stage is exit 1, --label names the report, every [X] is on "
          "STDERR", cli_bad_run)
    A.arm("n16 CLI --selftest is an alias of --self-test",
          lambda: parse_cli(["--selftest"]) == ("self-test",) == parse_cli(["--self-test"]))

    def unreadable():
        if os.name != "posix" or (hasattr(os, "geteuid") and os.geteuid() == 0):
            return _SKIP, "needs a POSIX host and a non-root user to make a file unreadable"
        ur = os.path.join(T, "unreadable")
        _mk_hdr(os.path.join(ur, "sqlite3.h"), ID_B)
        locked = os.path.join(ur, "sqlite3.c")
        _mk_hdr(locked, ID_A)
        os.chmod(locked, 0)
        try:
            if os.access(locked, os.R_OK):
                return _SKIP, "this filesystem ignores the mode bits (the file stays readable)"
            return _expect(2, ["cannot read " + locked])(_run([ur]))
        finally:
            os.chmod(locked, stat.S_IRUSR | stat.S_IWUSR)
    A.arm("n17 an UNREADABLE file is an I/O refusal (exit 2) naming it, never a silent skip",
          unreadable)

    def symlinked():
        sl = os.path.join(T, "symlinked")
        _mk_hdr(os.path.join(sl, "sqlite3.h"), ID_B)
        stale = os.path.join(T, "elsewhere", "sqlite3.c")
        _mk_hdr(stale, ID_A)
        try:
            os.symlink(stale, os.path.join(sl, "sqlite3.c"))
        except (OSError, NotImplementedError) as exc:
            return _SKIP, "this host cannot create a symlink here (%s)" % exc
        return _expect(1, [ID_A, "INCOHERENT"])(_run([sl]))
    A.arm("n18 a SYMLINKED file is scanned (the build reads through it; find -type f skipped it)",
          symlinked)

    def order():
        o1 = os.path.join(T, "order")
        _mk_hdr(os.path.join(o1, "b.h"), ID_B)
        _mk_hdr(os.path.join(o1, "a.h"), ID_B)
        _mk_hdr(os.path.join(o1, "c.h"), ID_A)
        r = _run([o1])
        rows = [m.group(1) for m in re.finditer(r"^ {8}(\S+) +20[0-9]{2}-", r[1], re.M)]
        return rows == ["c.h", "a.h", "b.h"], "rows %r" % rows
    A.arm("n19 divergence rows are ordered by id, then path (code point)", order)
    A.arm("n20 a lagging fts5 stamp is never compared with the identity id (the plain id inside "
          "'fts5: <id>' is not an identity id)",
          lambda: (lambda r: (r[0] == 0 and "%-16s %s" % ("SQLITE_SOURCE_ID:", ID_B) in r[1]
                              and "%-16s %s" % ("fts5 stamp:", ID_F) in r[1]
                              and ID_F not in r[1].split("fts5 stamp:")[0], r[1]))(_run([fts])))
    A.arm("n21 run_check with NO directory is a usage error (exit 2), not an empty pass",
          lambda: _expect(2, ["usage:"])(_run([])))


def self_test():
    print("== %s --self-test ==" % PROG)
    A = _Arms()
    T = tempfile.mkdtemp(prefix="dss-sqlite-coherence-st-")
    try:
        _body(A, T)
    except Exception:
        A.failed += 1
        print("  [FAIL] the self-test CRASHED (its remaining arms did not run):")
        for line in traceback.format_exc().rstrip().split("\n"):
            print("         %s" % line)
    finally:
        shutil.rmtree(T, ignore_errors=True)
    return A.finish(EXPECTED_ARMS)


if __name__ == "__main__":
    sys.exit(main())
