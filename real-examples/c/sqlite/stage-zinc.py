#!/usr/bin/env python3
"""Stage the CONFIGURED headers a leg compiles against, PER TARGET — both drivers.

Two families, one rule: a header that has been through somebody's `./configure`
is a MEASUREMENT OF THAT MACHINE, and handing it to a foreign target is a
cross-compile category error. This file materialises one copy per declared
configuration and VERIFIES what it wrote.

  1. zlib's `zconf.h`   — D-HARNESS-SQLITE-STAGE-ZCONF-IS-PE-SHAPED
                          (one dir per `recipeTransform`, from `zconfGuards`)
  2. sqlite's `sqlite_cfg.h`
                        — D-HARNESS-MACHO-LEG-INHERITS-THE-DERIVING-LINUX-HOSTS-
                          CONFIGURE-PROBES
                          (one dir per TARGET OS, from `configureAnswers`)

D-HARNESS-SQLITE-STAGE-ZCONF-IS-PE-SHAPED.

THE DEFECT THIS CLOSES.  The sqlite harness compiles ~185 upstream TUs, three of
which (`ext/misc/{sqlar,compress,zipfile}.c`, plus `shell.c`) `#include <zlib.h>`.
The harness cannot use the target's zlib headers — there are none, that is what
cross-compiling means — so it stages the DERIVING POSIX host's copy. That copy
has been through `./configure`, which rewrites zconf.h in place:

    #if 1    /* was set to #if 1 by ./configure */
    #  define Z_HAVE_UNISTD_H
    #endif

`#if 1` there is a measurement OF THE DERIVING HOST. It is the header-level twin
of the recipe's `HAVE_*`/`Z_HAVE_*` defines that `recipeTransform` already exists
to drop, and handing it to a foreign target is the same cross-compile category
error one level down: on a Windows target `Z_HAVE_UNISTD_H` makes zconf.h
`#include <unistd.h>`, which does not exist there.

Until TF-C115 build-and-test.ps1 flipped that one guard with a `perl -0777 -pi`
in its staging step and produced ONE zinc/ for ALL FIVE legs — so the shared
stage was pe-shaped, and the driver correctly REFUSED (recorded `poisoned`) any
leg whose recipeTransform was not `windows-selfconfig` rather than compile it
against a header configured for somebody else's target.  build-and-test.sh did
not flip it at all, so ITS pe64 leg had the mirror-image defect.  This file
replaces the need for both: one implementation, called by both drivers, staging
one zinc/ per `recipeTransform` from the guards each leg DECLARES in legs.json.

WHAT IT DOES NOT DO.  It does not know what either guard means, which targets
have <unistd.h>, or what a leg is.  It reads the catalogue through
harness_legs.py, writes one directory per declared stage, and VERIFIES what it
wrote.  Everything target-shaped is a declaration.

FAIL LOUD, PER STAGE.  A stage that cannot be produced prints
`ZINC-STAGE-FAIL=<key>|<reason>` and the exit status is non-zero — but every
other stage is still written and still reported, so a driver can poison exactly
the legs that lost their header and build the rest.  There is no fallback to
another stage's copy: that fallback IS the defect.

USAGE
  stage-zinc.py --zlib-h <path> --zconf-h <path> --dest <dir>
                --sqlite-cfg-h <path> --cfg-dest <dir> [--catalogue <legs.json>]

  ZINC-STAGE-OK=<key>|<dir>|<GUARD>=<0|1> …|<note>
  ZINC-STAGE-FAIL=<key>|<reason>
  ZINC-STAGES=<ok>/<total>
  CFG-STAGE-OK=<targetOs>|<dir>|<HAVE_*>=<0|1> …|<note>
  CFG-STAGE-FAIL=<targetOs>|<reason>
  CFG-STAGES=<ok>/<total>

★ THE sqlite_cfg.h PAIR IS OPTIONAL HERE AND MANDATORY WHERE IT IS USED. Passing
neither flag prints an explicit `CFG-STAGES=0/0|NOT REQUESTED: …` line — never
silence, because "the caller did not ask" and "it asked and got none" have
opposite meanings. Passing only ONE of the two is a hard error. Both drivers pass
both, and both REFUSE to build a leg without its staged header; the reason the
pair is not argparse-required is that this tool has a second, legitimate caller
that wants the zlib family alone (tests/harness/test_sqlite_harness_legs.cpp,
which drives it over a zconf.h FIXTURE and has no sqlite build dir to point at).
See the comment above `_CFG_DEFINE_RE` and the one in `main()`.
"""

import argparse
import os
import re
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import harness_legs  # noqa: E402  (path fixed up immediately above)


class StageError(Exception):
    """One stage could not be produced. Never downgraded to a warning."""


# The `./configure`-rewritten site for one guard, as it appears in a staged
# zconf.h. THREE lines, and all three are checked, because a two-line match
# would also hit the LATER conditional re-definitions:
#
#     #ifndef Z_HAVE_UNISTD_H
#     #  ifdef __WATCOMC__
#     #    define Z_HAVE_UNISTD_H          <-- 4-space define, NOT this site
#     #  endif
#     #endif
#
# The configure site is the only one whose `#define` is indented by exactly two
# spaces under an `#if`/`#ifdef` at column 0. Requiring EXACTLY ONE match is what
# makes that reasoning checkable rather than assumed: if upstream ever reshapes
# the header, this raises instead of editing the wrong line.
def _find_configure_site(lines, guard):
    hits = []
    for i, line in enumerate(lines):
        if line.rstrip() != "#  define %s" % guard:
            continue
        if i == 0:
            continue
        prev = lines[i - 1]
        if not prev.startswith("#if"):
            continue
        if prev.startswith("#ifndef"):
            continue  # a re-definition guard, not the configure site
        hits.append(i - 1)
    if len(hits) != 1:
        raise StageError(
            "expected EXACTLY ONE ./configure site for %s in the staged zconf.h "
            "(a `#if …` / `#  define %s` pair at column 0), found %d. Upstream "
            "zlib may have reshaped the header; this refuses to guess which line "
            "to rewrite rather than silently edit the wrong one."
            % (guard, guard, len(hits)))
    return hits[0]


# The re-definition sites that could turn a guard we set OFF back ON. Reported,
# never silently relied on: what actually fires depends on the TARGET's own
# predefined macros (`__WATCOMC__`, `_LARGEFILE64_SOURCE`, `_WIN32`), which this
# tool cannot and should not evaluate. Naming them keeps "we set it to 0" from
# being read as "it is definitely 0".
def _redefinition_sites(lines, guard):
    out = []
    for i, line in enumerate(lines):
        if line.strip() != "#    define %s" % guard:
            continue
        # The INNERMOST enclosing conditional, i.e. the actual condition. A
        # preprocessor directive may carry whitespace between the `#` and the
        # keyword (`#  ifdef __WATCOMC__` is exactly that), so the keyword is
        # read from the normalised form — matching on the raw text found the
        # OUTER `#ifndef` instead and reported the same useless line twice.
        cond = re.sub(r"^#\s*", "#", lines[i - 1].strip()) if i else ""
        if cond.startswith("#if") and cond not in out:
            out.append(cond)
    return out


def apply_guards(text, guards):
    """Rewrite each declared guard's ./configure site. Returns (text, notes)."""
    lines = text.split("\n")
    notes = []
    for guard in sorted(guards):
        want = bool(guards[guard])
        at = _find_configure_site(lines, guard)
        lines[at] = ("#if %d    /* set to #if %d by the dss sqlite harness "
                     "(stage-zinc.py); ./configure's value was a fact about the "
                     "DERIVING host */" % (1 if want else 0, 1 if want else 0))
        if not want:
            for cond in _redefinition_sites(lines, guard):
                notes.append("%s is OFF but zconf.h re-enables it under `%s`"
                             % (guard, cond))
    return "\n".join(lines), notes


def verify_guards(path, guards):
    """Re-READ the written file and confirm every guard. The write is not the
    evidence; the file on disk is."""
    with open(path, "r", encoding="utf-8") as f:
        lines = f.read().split("\n")
    for guard in sorted(guards):
        want = 1 if guards[guard] else 0
        at = _find_configure_site(lines, guard)
        got = lines[at].split("/*", 1)[0].strip()
        if got != "#if %d" % want:
            raise StageError(
                "verification FAILED for %s in %s: the ./configure site reads %r, "
                "expected '#if %d'" % (guard, path, got, want))


def stage_one(key, guards, zlib_h, zconf_h, dest_root):
    """Write dest_root/<key>/{zlib.h,zconf.h}. Returns a note string."""
    # Forward slashes on every host: this string is printed for a driver to put
    # straight into a manifest `includes` entry, and DSS/PowerShell/bash all read
    # `C:/x/y` while only bash mis-reads `C:\x\y`.
    out = os.path.join(dest_root, key).replace("\\", "/")
    os.makedirs(out, exist_ok=True)
    shutil.copyfile(zlib_h, os.path.join(out, "zlib.h"))
    with open(zconf_h, "r", encoding="utf-8", errors="surrogateescape") as f:
        text = f.read()
    staged, notes = apply_guards(text, guards)
    target = os.path.join(out, "zconf.h")
    with open(target, "w", encoding="utf-8", errors="surrogateescape",
              newline="") as f:
        f.write(staged)
    verify_guards(target, guards)
    return out, "; ".join(notes)


# ── sqlite's own ./configure header ─────────────────────────────────────────
#
# D-HARNESS-MACHO-LEG-INHERITS-THE-DERIVING-LINUX-HOSTS-CONFIGURE-PROBES.
#
# `sqlite_cfg.h` is written by sqlite's autosetup `configure` on the DERIVING
# host, and the recipe's `_HAVE_SQLITE_CONFIG_H` makes `sqliteInt.h` include it —
# so every leg inherited that machine's answers. Exactly the zconf.h story above,
# one layer up, with one difference that shapes the code below: the file is
# GENERATED rather than shipped, and it spells its two answers in two forms:
#
#     #define HAVE_PREAD64 1
#     /* #undef HAVE_PREAD64 */
#
# So a rewrite must recognise BOTH and, like the zconf one, insist on finding
# EXACTLY ONE site — a second site would mean the generator changed shape and
# the safe move is to raise rather than to edit the first match.
#
# ★ EVERY OTHER LINE IS COPIED BYTE-FOR-BYTE. The deriving host's answers for the
# ~49 rows that DO NOT vary across our targets are correct for all of them
# (machine-diffed against a Mac's own generated header: exactly three lines
# differ), and keeping them is the whole reason this is a rewrite instead of the
# blunt "drop `_HAVE_SQLITE_CONFIG_H` and let sqlite self-configure" that would
# throw away HAVE_GMTIME_R, HAVE_FDATASYNC and HAVE_USLEEP with it.
#
# ⚠ BOTH PATTERNS TOLERATE A TRAILING COMMENT, and that is not cosmetic: the
# staged line carries one, `verify_answers` re-READS the staged file, and an
# `$`-anchored undef pattern therefore found ZERO sites in a file this tool had
# just written correctly. Caught by the verification step itself on first run —
# which is the argument for having one.
_CFG_DEFINE_RE = r"^#define[ \t]+%s(?:[ \t].*)?$"
_CFG_UNDEF_RE = r"^/\*[ \t]*#undef[ \t]+%s[ \t]*\*/.*$"


def _comment_safe(text):
    """`text` as it can appear INSIDE a /* … */ comment. C comments do not nest,
    so quoting a line that is itself `/* #undef X */` would END the comment early
    and leave stray `*/` tokens in a header every TU parses. No leg hits this
    today (all three answers are `#define`d on the deriving host and only ever
    turned OFF), and it is neutralised anyway: a rule that holds by luck is one
    the next declaration breaks."""
    return text.replace("/*", "").replace("*/", "").strip()


def _find_answer_site(lines, name):
    """The one line `configure` wrote for `name`, in either spelling."""
    define = re.compile(_CFG_DEFINE_RE % re.escape(name))
    undef = re.compile(_CFG_UNDEF_RE % re.escape(name))
    hits = [i for i, line in enumerate(lines)
            if define.match(line.rstrip()) or undef.match(line.rstrip())]
    if len(hits) != 1:
        raise StageError(
            "expected EXACTLY ONE ./configure site for %s in the deriving host's "
            "sqlite_cfg.h (a `#define %s …` or `/* #undef %s */` line), found %d. "
            "Either upstream's configure no longer emits this answer — in which "
            "case the leg declarations naming it are stale — or the header has "
            "changed shape; this refuses to guess which line to rewrite."
            % (name, name, name, len(hits)))
    return hits[0]


def apply_answers(text, answers):
    """Rewrite each declared configure answer. Returns (text, notes).

    The rewritten line keeps the EXACT spelling `configure` itself uses for each
    state, so the staged file still reads as a generated config header rather
    than as something hand-edited: `#define NAME 1` / `/* #undef NAME */`."""
    lines = text.split("\n")
    notes = []
    for name in sorted(answers):
        want = bool(answers[name])
        at = _find_answer_site(lines, name)
        was = lines[at].strip()
        was_defined = was.startswith("#define")
        # The quoted evidence must be safe to sit INSIDE a comment. `/* #undef X */`
        # quoted verbatim would terminate the note early — two complete comments on
        # one line is fine (the `#undef` form produces exactly that), NESTING is not.
        quoted = _comment_safe(was)
        if "/*" in quoted or "*/" in quoted:
            raise StageError(
                "the deriving host's line for %s cannot be quoted safely inside a "
                "comment even after neutralisation: %r. C comments do not nest, "
                "and a header every TU parses must not depend on that."
                % (name, was))
        lines[at] = ("#define %s 1" % name) if want else ("/* #undef %s */" % name)
        lines[at] += ("    /* set by the dss sqlite harness (stage-zinc.py) from "
                      "this leg's declared configureAnswers; the DERIVING host "
                      "said: %s */" % quoted)
        if want is not was_defined:
            notes.append("%s: the deriving host said `%s`, this target declares %s"
                         % (name, was, "1" if want else "undefined"))
    return "\n".join(lines), notes


def verify_answers(path, answers):
    """Re-READ the written file and confirm every answer. The write is not the
    evidence; the file on disk is."""
    with open(path, "r", encoding="utf-8") as f:
        lines = f.read().split("\n")
    for name in sorted(answers):
        want = bool(answers[name])
        at = _find_answer_site(lines, name)
        got = lines[at].split("/*", 1)[0].strip() if want else lines[at].strip()
        expect = ("#define %s 1" % name) if want else ("/* #undef %s */" % name)
        if not got.startswith(expect):
            raise StageError(
                "verification FAILED for %s in %s: the ./configure site reads %r, "
                "expected it to begin %r" % (name, path, got, expect))


def stage_one_cfg(key, answers, sqlite_cfg_h, dest_root):
    """Write dest_root/<key>/sqlite_cfg.h. Returns (dir, note)."""
    out = os.path.join(dest_root, key).replace("\\", "/")
    os.makedirs(out, exist_ok=True)
    with open(sqlite_cfg_h, "r", encoding="utf-8", errors="surrogateescape") as f:
        text = f.read()
    staged, notes = apply_answers(text, answers)
    target = os.path.join(out, "sqlite_cfg.h")
    with open(target, "w", encoding="utf-8", errors="surrogateescape",
              newline="") as f:
        f.write(staged)
    verify_answers(target, answers)
    return out, "; ".join(notes)


def main(argv=None):
    # ✔MEASURED, and CAUSED HERE: `stage-zinc.py --help` crashed on a Windows
    # console the moment this file's prose acquired a `★`. Python picks the
    # console's cp1252 for stdout and argparse's write of `description=__doc__`
    # raises UnicodeEncodeError before one line of help appears. It is the same
    # defect harness_legs.py's main() already carries the fix for, and the same
    # answer: a tool whose --help cannot run on one of its two supported hosts is
    # a tool nobody reads the help of, so the STREAM is fixed rather than the
    # prose de-punctuated. `errors="replace"` keeps degrade-don't-die for anything
    # a terminal still cannot render — and it covers the DIAGNOSTICS too, which is
    # the half that matters: every refusal below is quoted prose.
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except (AttributeError, ValueError, OSError):
            pass   # a redirected/wrapped stream that cannot be reconfigured
    p = argparse.ArgumentParser(prog="stage-zinc.py", description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--zlib-h", required=True, help="the deriving host's zlib.h")
    p.add_argument("--zconf-h", required=True, help="the deriving host's zconf.h")
    p.add_argument("--dest", required=True,
                   help="the zinc/ ROOT — one subdirectory is written per stage")
    p.add_argument("--sqlite-cfg-h", default="",
                   help="the DERIVING host's generated sqlite_cfg.h (the one its "
                        "./configure wrote into the sqlite build dir)")
    p.add_argument("--cfg-dest", default="",
                   help="the cfg/ ROOT — one subdirectory is written per TARGET "
                        "OS, each holding that target's sqlite_cfg.h. A SEPARATE "
                        "root from --dest on purpose: the two families are keyed "
                        "differently (recipeTransform vs target OS) and a shared "
                        "root would let a transform named 'linux' collide with a "
                        "target OS named 'linux'.")
    p.add_argument("--catalogue", default=harness_legs.CATALOGUE)
    args = p.parse_args(argv)

    # ★ THE sqlite_cfg.h PAIR IS OPTIONAL *HERE* AND MANDATORY AT THE POINT OF USE,
    # and that split is deliberate rather than a softening. It is optional here
    # because this tool has a second caller that legitimately wants the zlib family
    # alone: tests/harness/test_sqlite_harness_legs.cpp drives the REAL script over
    # a zconf.h FIXTURE to prove every leg gets its own staged zlib header, and it
    # has no sqlite build dir to point --sqlite-cfg-h at. It is mandatory at the
    # point of use because BOTH drivers refuse to continue when no cfg stage was
    # produced (`stage-zinc.py produced NO per-target sqlite_cfg.h` → die) and
    # poison any leg whose stage is missing. So the guarantee — no leg is ever
    # compiled against the DERIVING host's answers — is enforced where a leg is
    # actually compiled, not by argparse.
    # ⚠ HALF THE PAIR IS A HARD ERROR: a caller that names the source header but no
    # destination (or the reverse) meant to stage and did not, and silently doing
    # nothing is exactly the reading this tool must never permit.
    want_cfg = bool(args.sqlite_cfg_h) or bool(args.cfg_dest)
    if want_cfg and not (args.sqlite_cfg_h and args.cfg_dest):
        sys.stderr.write("stage-zinc.py: FATAL: --sqlite-cfg-h and --cfg-dest are "
                         "one request and must be given together (got %s). Staging "
                         "a per-target sqlite_cfg.h needs both the DERIVING host's "
                         "header to rewrite and a root to write into.\n"
                         % ("only --sqlite-cfg-h" if args.sqlite_cfg_h
                            else "only --cfg-dest"))
        return 2
    # A missing SOURCE header is not one stage's problem — no leg on any host
    # could be compiled without it — so it is a hard, immediate refusal, matching
    # the run-wide `die` both drivers already have for a missing zlib.h.
    sources = [("--zlib-h", args.zlib_h), ("--zconf-h", args.zconf_h)]
    if want_cfg:
        sources.append(("--sqlite-cfg-h", args.sqlite_cfg_h))
    for label, path in sources:
        if not os.path.isfile(path):
            sys.stderr.write("stage-zinc.py: FATAL: %s %r is not a file. Every leg "
                             "parses this one header; without it there is no leg to "
                             "build on any host.\n" % (label, path))
            return 2
    try:
        legs = harness_legs.load_catalogue(args.catalogue)
        stages = harness_legs.header_stages(legs)
        cfg_stages = harness_legs.configure_stages(legs)
    except harness_legs.LegError as exc:
        sys.stderr.write("stage-zinc.py: FATAL: %s\n" % exc)
        return 2
    if not stages:
        sys.stderr.write("stage-zinc.py: FATAL: the catalogue declares no header "
                         "stages — every leg would have no include dir.\n")
        return 2
    if want_cfg and not cfg_stages:
        sys.stderr.write("stage-zinc.py: FATAL: the catalogue declares no configure "
                         "stages — every leg would fall back to the DERIVING "
                         "host's sqlite_cfg.h, which is the defect.\n")
        return 2

    ok = 0
    for key, guards in stages.items():
        try:
            out, note = stage_one(key, guards, args.zlib_h, args.zconf_h, args.dest)
        except (StageError, OSError) as exc:
            # NO FALLBACK. A leg without its own header is poisoned by the driver;
            # handing it a sibling stage's copy is the defect, not the recovery.
            sys.stdout.write("ZINC-STAGE-FAIL=%s|%s\n" % (key, exc))
            continue
        ok += 1
        sys.stdout.write("ZINC-STAGE-OK=%s|%s|%s|%s\n" % (
            key, out,
            " ".join("%s=%d" % (n, 1 if v else 0) for n, v in sorted(guards.items())),
            note))
    sys.stdout.write("ZINC-STAGES=%d/%d\n" % (ok, len(stages)))

    # The sqlite configure header, same discipline: per-stage failure, no
    # fallback, every stage still attempted and still reported.
    #
    # ★ "NOT REQUESTED" IS PRINTED, LOUDLY, rather than left as silence. A reader
    # of a log must be able to tell "this caller did not ask for the per-target
    # sqlite_cfg.h" apart from "it asked and got none" — those have opposite
    # meanings and only one of them is fine.
    if not want_cfg:
        sys.stdout.write(
            "CFG-STAGES=0/0|NOT REQUESTED: no --sqlite-cfg-h/--cfg-dest was "
            "given, so NO per-target sqlite_cfg.h was produced. Any leg compiled "
            "against a list that does not carry one inherits the DERIVING host's "
            "./configure answers "
            "(D-HARNESS-MACHO-LEG-INHERITS-THE-DERIVING-LINUX-HOSTS-CONFIGURE-"
            "PROBES). Both drivers pass the pair and REFUSE to build without it.\n")
        return 0 if ok == len(stages) else 1
    cfg_ok = 0
    for key, answers in cfg_stages.items():
        try:
            out, note = stage_one_cfg(key, answers, args.sqlite_cfg_h, args.cfg_dest)
        except (StageError, OSError) as exc:
            sys.stdout.write("CFG-STAGE-FAIL=%s|%s\n" % (key, exc))
            continue
        cfg_ok += 1
        sys.stdout.write("CFG-STAGE-OK=%s|%s|%s|%s\n" % (
            key, out,
            " ".join("%s=%d" % (n, 1 if v else 0) for n, v in sorted(answers.items())),
            note))
    sys.stdout.write("CFG-STAGES=%d/%d\n" % (cfg_ok, len(cfg_stages)))
    return 0 if (ok == len(stages) and cfg_ok == len(cfg_stages)) else 1


if __name__ == "__main__":
    sys.exit(main())
