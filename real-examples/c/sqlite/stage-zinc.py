#!/usr/bin/env python3
"""Stage ONE zlib header directory PER TARGET CONFIGURATION — both drivers.

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
  stage-zinc.py --zlib-h <path> --zconf-h <path> --dest <dir> [--catalogue <legs.json>]

  ZINC-STAGE-OK=<key>|<dir>|<GUARD>=<0|1> …|<note>
  ZINC-STAGE-FAIL=<key>|<reason>
  ZINC-STAGES=<ok>/<total>
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


def main(argv=None):
    p = argparse.ArgumentParser(prog="stage-zinc.py", description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--zlib-h", required=True, help="the deriving host's zlib.h")
    p.add_argument("--zconf-h", required=True, help="the deriving host's zconf.h")
    p.add_argument("--dest", required=True,
                   help="the zinc/ ROOT — one subdirectory is written per stage")
    p.add_argument("--catalogue", default=harness_legs.CATALOGUE)
    args = p.parse_args(argv)

    # A missing SOURCE header is not one stage's problem — no leg on any host
    # could be compiled without it — so it is a hard, immediate refusal, matching
    # the run-wide `die` both drivers already have for a missing zlib.h.
    for label, path in (("--zlib-h", args.zlib_h), ("--zconf-h", args.zconf_h)):
        if not os.path.isfile(path):
            sys.stderr.write("stage-zinc.py: FATAL: %s %r is not a file. Every leg "
                             "parses this one header; without it there is no leg to "
                             "build on any host.\n" % (label, path))
            return 2
    try:
        stages = harness_legs.header_stages(harness_legs.load_catalogue(args.catalogue))
    except harness_legs.LegError as exc:
        sys.stderr.write("stage-zinc.py: FATAL: %s\n" % exc)
        return 2
    if not stages:
        sys.stderr.write("stage-zinc.py: FATAL: the catalogue declares no header "
                         "stages — every leg would have no include dir.\n")
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
    return 0 if ok == len(stages) else 1


if __name__ == "__main__":
    sys.exit(main())
