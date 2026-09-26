#!/usr/bin/env python3
# PURPOSE: measure what Apple's ld64 does with a second defined symbol at the address of a canonical one, with and without -dead_strip.
"""macho-alias-ld64-matrix.py -- what Apple's ld64 does with a SECOND defined symbol at the
address of a canonical one, with and without `-dead_strip`.

THE QUESTION. A Mach-O writer can express an alias -- one body, two defined names -- by
appending one more nlist entry at the same n_value. Whether that is enough depends on
ld64: if it mints a zero-length atom for the alias, a reference that reaches only the
alias can keep that atom while `-dead_strip` removes the body -- perfect bytes, wrong
program. `rc=0` cannot see that; the EXIT CODE can.

THE CELLS. Three producer variants, each with ONE body and TWO defined names at the
SAME address, differing only in how the second name is declared:
  plain     a second label + `.globl`. This is the shape a DSS writer would emit if it
            simply appended one more nlist at the same n_value with n_desc = 0.
  altentry  the same, plus `.alt_entry`, i.e. n_desc = N_ALT_ENTRY (0x0200) -- the shape
            DSS already stamps on its synthetic block labels.
  setalias  `.globl` + `.set`, the ATTRIBUTION CONTROL: a clang-authored alias. If a cell
            fails here too, the behaviour is ld64's and not a DSS emission bug.
Each is linked with dead-strip off and on by a caller that references ONLY the alias --
the whole point -- plus a canonical-only control (`plain`, referenced by the canonical
name, at both settings) that says whether the alias had anything to do with a body
-dead_strip removed: eight cells. Then an n_desc dump of `_alias`/`_canon` per variant.

THE OBSERVABLES, per cell: LINK (`ok`, or the step that failed), EXITCODE (the program
must RUN and return 42; a death by signal N reads 128+N, as a shell reports it),
SAMEADDR (do both names resolve to one address in `nm -n`) and TEXTSIZE (the `__text`
size from `otool -l`). ⚠ A cell FAILS on an assemble or link failure or an exit code
other than 42; SAMEADDR is REPORTED beside it and never fails a cell. The answer this
matrix gave is cited as evidence in `src/link/format/macho.cpp` and
`tests/link/test_macho_writer.cpp`: a plain alias (n_desc = 0) is correct and
N_ALT_ENTRY is not needed.

macOS ONLY, BY NATURE: every cell is `/usr/bin/clang`, `/usr/bin/nm`, `/usr/bin/otool` and
Apple's ld64 -- absolute paths, so no PATH can substitute another toolchain. On any other
host, or with one of the three not an executable file, the program refuses by name
(exit 2) BEFORE it creates anything.

Output: the scratch directory, the table, the n_desc dump, then `MATRIX_FAIL=<0|1>` as
the last line of every run that reached the matrix. The scratch is a fresh
`dss-macho-alias-matrix-*` directory under the system temp directory -- one per run, so
two runs cannot clobber each other (the shell program this replaced wiped and reused one
fixed `/tmp` path) -- and it is LEFT in place, holding every source, object, executable
and error log for inspection.

Usage:
  python3 .harness-config/runner/actions/macho-alias-ld64-matrix/macho-alias-ld64-matrix.py
Exit codes: 0 every cell passed · 1 a cell failed (MATRIX_FAIL=1) · 2 refused (not macOS,
a tool missing, an argument given) · 90 the scratch directory could not be created or
filled.
"""
import os
import platform
import subprocess
import sys
import tempfile

# ── OUTPUT ENCODING, AT IMPORT ───────────────────────────────────────────────────
# ✔MEASURED 2026-08-23 (CPython 3.14.3, Windows, both streams pipes): stdout comes up
# cp1252 there, so a printed character outside it raises `UnicodeEncodeError`. The
# matrix only runs on macOS, but this module is IMPORTED on every leg by the output
# encoding guard, and its refusal is printed wherever it is started.
for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError, OSError):   # pragma: no cover - an odd stream
        pass

TOOL = "macho-alias-ld64-matrix"

CC = "/usr/bin/clang"
NM = "/usr/bin/nm"
OTOOL = "/usr/bin/otool"
TOOLS = (CC, NM, OTOOL)

# ── the subjects, byte for byte what the shell program generated (LF, 4-space indents) ─
BODY_H_S = (".section __TEXT,__text,regular,pure_instructions\n"
            ".globl _canon\n"
            ".p2align 2\n"
            "_canon:\n"
            "    mov w0, #42\n"
            "    ret\n")
SETALIAS_S = BODY_H_S + ".globl _alias\n.set _alias, _canon\n"
PLAIN_S = (".section __TEXT,__text,regular,pure_instructions\n"
           ".globl _canon\n"
           ".globl _alias\n"
           ".p2align 2\n"
           "_canon:\n"
           "_alias:\n"
           "    mov w0, #42\n"
           "    ret\n")
ALTENTRY_S = (".section __TEXT,__text,regular,pure_instructions\n"
              ".globl _canon\n"
              ".globl _alias\n"
              ".alt_entry _alias\n"
              ".p2align 2\n"
              "_canon:\n"
              "_alias:\n"
              "    mov w0, #42\n"
              "    ret\n")
# The caller references ONLY the alias -- the whole point. The control references
# only the canonical name.
CALLALIAS_C = "int alias(void);\nint main(void){ return alias(); }\n"
CALLCANON_C = "int canon(void);\nint main(void){ return canon(); }\n"
SOURCES = (("body.h.s", BODY_H_S), ("setalias.s", SETALIAS_S), ("plain.s", PLAIN_S),
           ("altentry.s", ALTENTRY_S), ("callalias.c", CALLALIAS_C),
           ("callcanon.c", CALLCANON_C))

VARIANTS = ("plain", "altentry", "setalias")
ROW_FORMAT = "%-10s %-12s %-8s %-8s %-9s %-9s %s"


def preflight(system=None, tools=TOOLS):
    """None when the matrix can run on this host, else why it cannot."""
    system = platform.system() if system is None else system
    if system != "Darwin":
        return ("this matrix measures Apple's ld64 and runs only on macOS; this host "
                "is %s" % (system or "unknown"))
    for tool in tools:
        if not (os.path.isfile(tool) and os.access(tool, os.X_OK)):
            return ("%s is not an executable file on this host -- every cell needs "
                    "Apple's clang, nm and otool (the Xcode command line tools)" % tool)
    return None


def write_sources(work):
    """The six subject files into `work`, UTF-8 and LF on every host."""
    for name, text in SOURCES:
        with open(os.path.join(work, name), "w", encoding="utf-8", newline="\n") as fh:
            fh.write(text)


def _flush():
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.flush()
        except (AttributeError, ValueError, OSError):
            pass


def _status(rc):
    """A child's exit status as a shell reports it: death by signal N is 128+N."""
    return 128 - rc if rc < 0 else rc


def _lines(text):
    """`text` split the way grep and awk read it: on newlines only."""
    parts = text.split("\n")
    if parts and parts[-1] == "":
        parts.pop()
    return parts


def run_status(argv, work, err_file=None, quiet=False):
    """`argv` from `work`, stdout inherited; stderr into `work/err_file`, discarded when
    `quiet`, else inherited -> its exit status. A program that cannot be started reads
    127 (not found) or 126 (not executable), as a shell reports it."""
    _flush()
    try:
        if err_file is not None:
            with open(os.path.join(work, err_file), "wb") as fh:
                p = subprocess.run(argv, cwd=work, stdin=subprocess.DEVNULL, stderr=fh)
        else:
            p = subprocess.run(argv, cwd=work, stdin=subprocess.DEVNULL,
                               stderr=subprocess.DEVNULL if quiet else None)
    except FileNotFoundError:
        return 127
    except OSError:
        return 126
    return _status(p.returncode)


def captured(argv, work):
    """`argv`'s stdout as text whatever its exit code, stderr inherited -- a `$(...)`."""
    _flush()
    try:
        p = subprocess.run(argv, cwd=work, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE)
    except OSError:
        return ""
    return p.stdout.decode("utf-8", "replace")


def nm_addresses(exe, symbol, work):
    """`nm -n <exe> | awk '$3=="<symbol>"{print $1}'` as `$(...)` hands it back: the
    first field of every line whose third field is `symbol`, newline-joined."""
    hits = []
    for line in _lines(captured([NM, "-n", exe], work)):
        fields = line.split()
        if len(fields) >= 3 and fields[2] == symbol:
            hits.append(fields[0])
    return "\n".join(hits)


def text_size(exe, work):
    """`otool -l <exe> | awk '/sectname __text/{f=1} f&&/size /{print $2; exit}'`."""
    seen = False
    for line in _lines(captured([OTOOL, "-l", exe], work)):
        if "sectname __text" in line:
            seen = True
        if seen and "size " in line:
            fields = line.split()
            return fields[1] if len(fields) > 1 else ""
    return ""


def ndesc(obj, work):
    """`nm -m <obj> | grep -E '_alias|_canon' | tr '\\n' ' '` -- the trailing space kept."""
    return "".join(line + " " for line in _lines(captured([NM, "-m", obj], work))
                   if "_alias" in line or "_canon" in line)


def row(*cells):
    sys.stdout.write(ROW_FORMAT % cells + "\n")


def probe(variant, asm, caller, ref, ds, work):
    """One cell. True when it passed: assembled, linked, ran, and returned 42."""
    tag = "%s-%s-ds%s" % (variant, ref, ds)
    if run_status([CC, "-c", "-o", tag + ".o", asm], work, err_file=tag + ".asm.err") != 0:
        row(variant, ref, ds, "ASMFAIL", "-", "-", "-")
        return False
    ldflags = ["-Wl,-dead_strip"] if ds == "1" else []
    if run_status([CC] + ldflags + ["-o", tag + ".exe", caller, tag + ".o"], work,
                  err_file=tag + ".link.err") != 0:
        row(variant, ref, ds, "LINKFAIL", "-", "-", "-")
        return False
    ec = run_status([os.path.join(work, tag + ".exe")], work)
    ca = nm_addresses(tag + ".exe", "_canon", work)
    aa = nm_addresses(tag + ".exe", "_alias", work)
    same = "no"
    if ca and ca == aa:
        same = "yes"
    elif not ca or not aa:
        same = "missing(%s/%s)" % (ca, aa)
    row(variant, ref, ds, "ok", str(ec), same, text_size(tag + ".exe", work))
    # ★ rc=0 IS NOT AN OBSERVABLE. The program must RUN and return 42. A stripped body
    #   behind a zero-length atom shows up here and nowhere else.
    if ec != 42:
        sys.stdout.write("  ^^ EXIT CODE NOT 42\n")
        return False
    return True


def main(argv=None):
    argv = sys.argv[1:] if argv is None else list(argv)
    if argv in (["-h"], ["--help"]):
        sys.stdout.write(__doc__ or "%s: no help text (python -OO)\n" % TOOL)
        return 0
    if argv:
        sys.stderr.write("%s: REFUSED: it takes no arguments, got: %s (see --help)\n"
                         % (TOOL, " ".join(argv)))
        return 2
    why = preflight()
    if why:
        sys.stderr.write("%s: REFUSED: %s\n" % (TOOL, why))
        return 2
    try:
        work = tempfile.mkdtemp(prefix="dss-macho-alias-matrix-")
    except OSError as exc:
        sys.stderr.write("%s: cannot create a scratch directory (%s)\n" % (TOOL, exc))
        return 90
    sys.stdout.write("scratch: %s (left in place)\n" % work)
    try:
        write_sources(work)
    except OSError as exc:
        sys.stderr.write("%s: cannot write the subjects into %s (%s)\n" % (TOOL, work, exc))
        return 90

    fail = 0
    row("VARIANT", "REFERENCED", "DEADSTRIP", "LINK", "EXITCODE", "SAMEADDR", "TEXTSIZE")
    for variant in VARIANTS:
        for ds in ("0", "1"):
            if not probe(variant, variant + ".s", "callalias.c", "alias", ds, work):
                fail = 1
    # Canonical-only control: the alias is never referenced. If -dead_strip is going
    # to remove a body it should not, this is the cell that says whether the alias had
    # anything to do with it.
    for ds in ("0", "1"):
        if not probe("plain", "plain.s", "callcanon.c", "canon", ds, work):
            fail = 1

    sys.stdout.write("\nn_desc of _alias in each producer variant (0x0200 = N_ALT_ENTRY):\n")
    for variant in VARIANTS:
        run_status([CC, "-c", "-o", "nd-%s.o" % variant, "%s.s" % variant], work, quiet=True)
        sys.stdout.write("  %-9s %s\n" % (variant, ndesc("nd-%s.o" % variant, work)))
    sys.stdout.write("\nMATRIX_FAIL=%d\n" % fail)
    return fail


if __name__ == "__main__":
    sys.exit(main())
