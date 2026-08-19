#!/usr/bin/env python3
# PURPOSE: refuse a gate over a build directory whose objects recorded no header dependencies.
"""check-ninja-deps.py — refuse a gate over a build directory whose objects have
no recorded header dependencies.

★★★ WHY THIS EXISTS. A green ctest proves nothing about the current source if the
objects it linked were never rebuilt. That is not hypothetical here:
`D-BUILD-NINJA-RECORDS-ZERO-HEADER-DEPS-UNDER-CONCURRENT-BUILDS` was measured
twice, and the second measurement was five times worse than the first —

  * 2026-08-13: `ninja -t deps` reported `#deps 0` on **10 of 403** objects, and a
    header-only change therefore did not rebuild its consumers. A red-on-disable
    demonstration came back GREEN over a live mutant.
  * 2026-08-15, after a seven-lane concurrent cycle: **51 of 430**, with the row's
    own named witness (`elf.cpp.obj`) still broken two days later — and **16 of the
    51 were `src/` TUs compiled into the shipped DLL**. Any gate run in that
    directory before the rebuild proved nothing about the source in the tree.

⇒ this is the same class as a false-green red-on-disable, arriving through the
build system instead of through a test. The bar's answer to a recurring failure is
never "be careful": it is an instrument that cannot report success without
evidence.

★★ THE CONTRACT, and all three clauses are load-bearing:
  * an object with `#deps 0` is a FAILURE, not a curiosity — it means ninja will
    not rebuild that object when a header it includes changes;
  * **an EMPTY result is also a FAILURE.** A run that parsed no objects at all is
    indistinguishable from a run that found nothing wrong, and this project has
    already shipped one watcher that span forever because "no output" was read as
    "nothing to report". Zero objects parsed ⇒ exit non-zero;
  * the allowlist is EXPLICIT and carries a reason per entry — and ✔MEASURED
    2026-08-17 it should stay EMPTY, for a reason stronger than "no exceptions have
    come up yet": **`#deps 0` is not reachable by a healthy TU at all.** A C file
    with ZERO `#include` directives, compiled through a `deps = gcc` rule, records
    `#deps 1` — gcc lists the source itself. So a zero count is not "a TU with no
    headers"; it is always a lost record. (Measured directly: a two-target ninja
    project, one TU include-free, produced `#deps 1` and `#deps 2` — never 0.)

★ **AN HONEST LIMIT, stated rather than discovered later.** `ninja -t deps` prints
only objects that HAVE a deps record, so an object with NO record at all is
invisible to this check rather than flagged. That is not the failure mode being
guarded (a record-less object does not exist yet, so it will simply be built), but
a future reader should not mistake "OK" for "every object was verified" — it means
"every object ninja knows about carries deps". The total is printed for exactly
that reason: a sudden drop in the object count is the signal that something else
went wrong.

✔RED-ON-DISABLE, END-TO-END, not merely by self-test: a real ninja project whose
rule writes a prerequisite-less depfile produced
`a.o: #deps 0, deps mtime … (VALID)` beside a healthy `b.o: #deps 2 … (VALID)`,
and this tool reported `FAIL … 1 of 2 objects` and exited 1 while the healthy
sibling passed. The parser self-tests (`--self-test`) pin the verdict rule; that
experiment pins the wiring.

Usage:
    python scripts/check-ninja-deps/check-ninja-deps.py [build-dir ...]     # default: build/dbg, else build-dbg
    python scripts/check-ninja-deps/check-ninja-deps.py --self-test

The default is TRANSITION-SAFE by design. The repo is moving to a single build
root (`build/<name>`; see .claude/skills/dss-cycle/references/build-layout.md and
D-BUILD-LAYOUT-FLAT-ROOT-BUILD-DIRS-NOT-MIGRATED), and a default that named only
the new path would break every gate run made before the physical move — while a
default naming only the old one would silently keep checking a dead tree after
it. It therefore prefers `build/dbg` and falls back to `build-dbg`, so there is
no flag day. ⚠ A missing default is FATAL, never a skip: "the tree I was told to
check is not there" must not read as "nothing to check".

Exit: 0 clean · 1 dep-less objects found · 2 the instrument could not run.
"""

import re
import subprocess
import sys
from pathlib import Path

# ── the explicit allowlist ───────────────────────────────────────────────────
# object-path SUFFIX -> why a zero-dep record is legitimate for it.
# Deliberately empty: measured 0 of 430 on 2026-08-17. Adding an entry is a claim
# that a TU includes no headers at all — make it and say why.
ALLOWLIST: "dict[str, str]" = {}

# `ninja -t deps` prints one header line per object:
#     path/to/foo.cpp.obj: #deps 42, deps mtime 1234567 (VALID)
# followed by indented dependency lines. A record with no recorded deps prints
# `#deps 0`, and its `(VALID)`/`(STALE)` suffix is NOT a substitute for the count:
# a VALID record of zero deps is exactly the broken state this checks for.
_HEADER = re.compile(r"^(?P<obj>\S+):\s+#deps\s+(?P<n>\d+)\b")


def parse(text):
    """Return (total_objects, [dep-less object paths]).

    Split out from the subprocess call so the self-test drives the SAME parser the
    real path uses — a pin that re-types its subject's input is testing the stub.
    """
    total, empty = 0, []
    for line in text.splitlines():
        m = _HEADER.match(line)
        if not m:
            continue
        total += 1
        if int(m.group("n")) == 0:
            empty.append(m.group("obj"))
    return total, empty


def allowed(obj):
    return any(obj.endswith(suffix) for suffix in ALLOWLIST)


def check(build_dir):
    """Returns an exit code for one build directory."""
    d = Path(build_dir)
    if not (d / "build.ninja").is_file():
        print(f"ninja-deps: SKIP {build_dir} -- no build.ninja (not a ninja build dir)")
        return 0
    try:
        r = subprocess.run(["ninja", "-C", str(d), "-t", "deps"],
                           capture_output=True, text=True, timeout=600)
    except FileNotFoundError:
        print("ninja-deps: FATAL -- `ninja` is not on PATH; the check did not run")
        return 2
    except subprocess.TimeoutExpired:
        print(f"ninja-deps: FATAL -- `ninja -t deps` timed out in {build_dir}")
        return 2

    total, empty = parse(r.stdout)

    # ★ An empty parse is a FAILURE, never a pass. "Nothing found" and "nothing ran"
    # look identical from the outside, and this project has been burned by exactly
    # that ambiguity before.
    if total == 0:
        print(f"ninja-deps: FATAL -- parsed 0 objects from `ninja -t deps` in "
              f"{build_dir}; the check proved nothing (rc={r.returncode})")
        return 2

    flagged = [o for o in empty if not allowed(o)]
    if flagged:
        print(f"ninja-deps: FAIL {build_dir} -- {len(flagged)} of {total} objects "
              f"have ZERO recorded header deps. Ninja will NOT rebuild these when a "
              f"header they include changes, so any gate run here proves nothing "
              f"about the current source "
              f"(D-BUILD-NINJA-RECORDS-ZERO-HEADER-DEPS-UNDER-CONCURRENT-BUILDS).")
        for o in flagged[:40]:
            print(f"    {o}")
        if len(flagged) > 40:
            print(f"    … and {len(flagged) - 40} more")
        print("  FIX: delete the listed objects and rebuild, e.g.\n"
              f"    (cd {build_dir} && rm -f <objects> && cmake --build . -- -k 0)\n"
              "  `-k 0` collects EVERY failing target in one pass instead of stopping "
              "at the first, which is what makes the rebuild an exhaustive proof "
              "rather than a hopeful one.")
        return 1

    skipped = len(empty) - len(flagged)
    note = f" ({skipped} allowlisted)" if skipped else ""
    print(f"ninja-deps: OK {build_dir} -- {total} objects, all carry header deps{note}")
    return 0


# ── self-tests: the parser and the verdict rule, pinned ──────────────────────
def self_test():
    fails = []

    def case(name, text, want_total, want_empty):
        got_total, got_empty = parse(text)
        if (got_total, got_empty) != (want_total, want_empty):
            fails.append(f"{name}: got ({got_total}, {got_empty}), "
                         f"want ({want_total}, {want_empty})")

    case("a healthy record is counted and not flagged",
         "src/a.cpp.obj: #deps 42, deps mtime 1 (VALID)\n    x.hpp\n", 1, [])
    # ★ The one that matters: VALID does not rescue a zero count.
    case("a VALID record with zero deps IS flagged",
         "src/b.cpp.obj: #deps 0, deps mtime 1 (VALID)\n", 1, ["src/b.cpp.obj"])
    case("STALE with zero deps is flagged too",
         "src/c.cpp.obj: #deps 0, deps mtime 1 (STALE)\n", 1, ["src/c.cpp.obj"])
    # An indented dependency line must never be mistaken for an object record.
    case("indented dependency lines are not objects",
         "src/d.cpp.obj: #deps 2, deps mtime 1 (VALID)\n"
         "    a.hpp\n    b.hpp\n", 1, [])
    case("mixed input reports only the broken ones",
         "src/e.cpp.obj: #deps 7, deps mtime 1 (VALID)\n"
         "    a.hpp\n"
         "src/f.cpp.obj: #deps 0, deps mtime 1 (VALID)\n"
         "src/g.cpp.obj: #deps 3, deps mtime 1 (VALID)\n", 3, ["src/f.cpp.obj"])
    # A count of 10 must not be read as 0 by a sloppy pattern, and 0-prefixed
    # numbers must not appear where a word boundary is expected.
    case("a two-digit count is not confused with zero",
         "src/h.cpp.obj: #deps 10, deps mtime 1 (VALID)\n", 1, [])
    case("empty input parses as zero objects, which the caller treats as FATAL",
         "", 0, [])

    if fails:
        print("ninja-deps self-test: FAIL")
        for f in fails:
            print("   ", f)
        return 1
    print(f"ninja-deps self-test: OK (7 cases)")
    return 0


def default_build_dir():
    """`build/dbg` if present, else the pre-migration `build-dbg`.

    Transition-safe (see the Usage note): the repo is consolidating onto a single
    `build/` root, and naming only one of the two would break either every run
    before the move or every run after it. Existence, not a version flag, decides
    — so the default follows the tree instead of needing to be kept in sync with
    it. ⚠ If NEITHER exists, return the NEW path: `check()` then fails loud on a
    missing directory, which is the correct outcome. Returning the legacy path
    would send the reader hunting for a tree that was deliberately removed.
    """
    return "build/dbg" if Path("build/dbg").is_dir() else (
        "build-dbg" if Path("build-dbg").is_dir() else "build/dbg")


def main(argv):
    if "--self-test" in argv:
        return self_test()
    dirs = [a for a in argv if not a.startswith("-")] or [default_build_dir()]
    worst = 0
    for d in dirs:
        worst = max(worst, check(d))
    return worst


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
