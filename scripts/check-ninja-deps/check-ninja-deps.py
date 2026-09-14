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

★★★ AND THE THIRD CLAUSE WAS FALSE IN THE CODE FOR AS LONG AS IT HAS BEEN
WRITTEN HERE — D-GATE-NINJA-DEPS-EXITS-ZERO-ON-A-DIRECTORY-THAT-DOES-NOT-EXIST.
✔MEASURED 2026-08-23 at 6dc63be0, from the repo root:

    $ python scripts/check-ninja-deps/check-ninja-deps.py build-dbg
    ninja-deps: SKIP build-dbg -- no build.ninja (not a ninja build dir)
    rc=0
    $ ls -d build-dbg
    ls: cannot access 'build-dbg': No such file or directory

`build-dbg` has not existed since the one-root `build/` migration, and the gate
reference instructed every cycle to run exactly that command. ⇒ **the
build-verifiability check has been running against a nonexistent directory and
returning 0, which every caller read as a pass.** A MISSING DIRECTORY IS
"NOTHING RAN", and this file's own contract already forbids that outcome.
⚠ THE OTHER TWO DOCUMENTED CLAIMS WERE BOTH HALF-RIGHT, AND SAYING WHICH HALF
MATTERS BECAUSE A FIX AIMED AT THE WRONG ONE IS WORSE THAN NONE:
`references/build-layout.md` and `default_build_dir`'s own docstring say the
auto-pick "returns the NEW path when neither exists, so it fails loud on a
missing tree". ✔The RETURN half is true and was always true; the FAIL-LOUD half
described `check()`, which skipped. One code path, one fix, and it is in
`target_verdict` below.

★★ IS A BARE `SKIP` EVER LEGITIMATE? THE JUDGEMENT, RATHER THAN A DELETION.
  * A directory that DOES NOT EXIST: never. The caller named a tree it wanted
    verified and there is nothing there — that is the "nothing ran" case, and no
    flag opts out of it.
  * A directory that EXISTS but carries no `build.ninja`: legitimately possible —
    this project's CMake also configures under non-ninja generators, where
    `ninja -t deps` has nothing to say. So the CONCEPT is kept, but it must be
    ASKED FOR: `--allow-non-ninja` turns that one case into a reported SKIP.
    Without it, it is FATAL, because an unasked-for skip is indistinguishable
    from a pass.
⇒ the illegitimate case (a stale path silently returning 0) is now unreachable,
and the legitimate one is visible in the command line that requested it.

Usage:
    python scripts/check-ninja-deps/check-ninja-deps.py [build-dir ...]     # default: build/dbg, else build-dbg
    python scripts/check-ninja-deps/check-ninja-deps.py --allow-non-ninja <dir>
    python scripts/check-ninja-deps/check-ninja-deps.py --self-test

The default is TRANSITION-SAFE by design. The repo is moving to a single build
root (`build/<name>`; see .claude/skills/dss-cycle/references/build-layout.md and
D-BUILD-LAYOUT-FLAT-ROOT-BUILD-DIRS-NOT-MIGRATED), and a default that named only
the new path would break every gate run made before the physical move — while a
default naming only the old one would silently keep checking a dead tree after
it. It therefore prefers `build/dbg` and falls back to `build-dbg`, so there is
no flag day. ⚠ A missing default is FATAL, never a skip: "the tree I was told to
check is not there" must not read as "nothing to check".

Exit: 0 clean · 1 dep-less objects found · 2 the instrument could not run
(including: the named directory does not exist, or is not a ninja build dir and
`--allow-non-ninja` was not passed).
"""

import os
import re
import subprocess
import sys
from pathlib import Path

# ── OUTPUT ENCODING — NOT COSMETIC, AND THE STREAM IS HALF THE FACT ─────────────
# ✔MEASURED 2026-08-23 (CPython 3.14.3, Windows, BOTH streams PIPES, which is
# exactly how ctest runs every guard): `sys.stdout` comes up
# `encoding='cp1252' errors='surrogateescape'` and `sys.stderr` comes up
# `errors='backslashreplace'`. `surrogateescape` rescues only lone surrogates left
# by an earlier decode; it does NOTHING for an ordinary unencodable character. So a
# report printed on STDOUT — where this guard names every object that carries no
# header dependencies, as paths parsed out of ninja's own output —
# raises `UnicodeEncodeError` and kills the guard INSIDE ITS OWN REPORT: the run
# still reds, but the finding is lost and the traceback names a `print` rather than
# the thing that was wrong. STDERR merely mangles the glyph into an escape.
# ⚠ The paths come from a BUILD tree, which is not under this repository's naming
# discipline at all — a dependency path can carry anything the toolchain emits.
# Applied at IMPORT, so every path this module can print on is covered.
# D-GATE-PYTHON-GUARD-DIES-PRINTING-TREE-TEXT-ON-A-WINDOWS-PIPE
for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError, OSError):   # pragma: no cover - odd stream
        pass


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


# ── the `deps = msvc` half, and why the allowlist still stays empty ──────────
# ★★★ THE THIRD CLAUSE'S MEASUREMENT WAS TAKEN ON ONE DEPFILE FLAVOUR AND READ AS
# A FACT ABOUT ALL OF THEM — D-GATE-NINJA-DEPS-READS-A-GCC-DEPFILE-FACT-AS-A-FACT-ABOUT-EVERY-NINJA-TREE.
# The docstring above says it in its own words: *"A C file with ZERO `#include`
# directives, compiled through a `deps = gcc` rule, records `#deps 1` — gcc lists
# the source itself."* That is TRUE, and it is true OF `deps = gcc`. Ninja's
# `deps = msvc` flavour parses `/showIncludes`, which reports HEADERS ONLY — the
# source is never listed — so a TU with no `#include` of its own records `#deps 0`
# and that record is CORRECT.
#
# ✔MEASURED 2026-09-14 on a clean MSVC 19.51 Release tree of this repo
# (`deps = msvc`, `msvc_deps_prefix = Note: including file:`): 2 of 669 objects at
# `#deps 0` — `src/core/…/rule_id.cpp.obj` (the source has ZERO `#include`
# directives) and `tests/…/dss_test_pch.dir/test_support/pch_stub.cpp.obj` (a
# `static_assert` and nothing else). BOTH were DELETED and rebuilt ALONE at
# `ninja -j 1` with nothing else running, and BOTH came back `#deps 0 … (VALID)`.
# So this is structural, not the concurrency defect this tool was written for, and
# the tool's own printed FIX ("delete the listed objects and rebuild") was measured
# to do nothing. ⇒ It had been failing `windows-msvc-release` — the matrix's ONLY
# MSVC leg — on every CI run since the ctest entry landed, while every gcc/clang
# leg stayed green, because the premise only ever held for them.
#
# ★★ THE REPAIR IS A CHECKED PROPERTY, NOT A PATH ALLOWLIST, and the allowlist
# above stays EMPTY. On a `deps = msvc` tree a zero record is excused ONLY for an
# object whose own source carries no `#include` directive at all — read out of the
# source, per object, every run. An msvc object whose source DOES include
# something and records zero is still a hard FAIL: that is the lost record this
# tool exists to catch, and it is still caught.
# ★ AND THE ESCAPE IS DIRECTIONAL: on a `deps = gcc` tree this arm is UNREACHABLE,
# because `#deps 0` cannot occur there — so nothing is weakened on four of the five
# CI legs, and the fifth stops reporting a defect its tree does not contain.
# ⓘ THE PCH IS NOT A HOLE. `rule_id.cpp.obj`'s ninja edge carries
# `| …/cmake_pch.hxx …/cmake_pch.cxx.pch` as EXPLICIT implicit inputs, so ninja
# rebuilds it when the PCH moves from the BUILD GRAPH, never from `.ninja_deps`.
# A zero deps record cannot cost that rebuild. ✔Read out of the generated
# `build.ninja` at the same commit.
_NINJA_INCLUDE = re.compile(r"^\s*(?:include|subninja)\s+(.+?)\s*$")
_NINJA_DEPS_MODE = re.compile(r"^\s*deps\s*=\s*(\w+)\s*$")
_INCLUDE_DIRECTIVE = re.compile(r"^\s*#\s*include\b")


def _ninja_unescape(tok):
    """Ninja escapes `$:`, `$ ` and `$$` in paths. Undo exactly those three."""
    out, i = [], 0
    while i < len(tok):
        if tok[i] == "$" and i + 1 < len(tok):
            out.append(tok[i + 1])
            i += 2
        else:
            out.append(tok[i])
            i += 1
    return "".join(out)


def _ninja_manifest_text(build_dir):
    """`build.ninja` plus the files it includes/subninjas, ONE level down.

    One level is enough and is stated rather than assumed: CMake's Ninja generator
    emits `CMakeFiles/rules.ninja` (which carries every `deps =` line) and per-dir
    `build.ninja` files from the top manifest, and nothing deeper is needed to
    answer either question this function is asked.
    """
    d = Path(build_dir)
    top = d / "build.ninja"
    try:
        text = top.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return ""
    parts = [text]
    for line in text.splitlines():
        m = _NINJA_INCLUDE.match(line)
        if not m:
            continue
        sub = d / _ninja_unescape(m.group(1))
        try:
            parts.append(sub.read_text(encoding="utf-8", errors="replace"))
        except OSError:
            continue
    return "\n".join(parts)


def deps_modes(manifest_text):
    """The set of `deps = <flavour>` values this manifest declares."""
    return {m.group(1) for m in
            (_NINJA_DEPS_MODE.match(line) for line in manifest_text.splitlines()) if m}


def _sep(p):
    """One spelling for a path key.

    ⚠ NOT COSMETIC, and it cost a round: on Windows `ninja -t deps` prints object
    paths with FORWARD slashes (`src/core/CMakeFiles/…`) while the generated
    `build.ninja` writes the same object with BACKslashes. Keyed raw, every lookup
    missed, every zero-dep object looked like one whose source could not be named,
    and the repair reported exactly the failure it had just fixed. ✔MEASURED on the
    MSVC tree, first run after the arm landed.
    """
    return p.replace("\\", "/")


def object_sources(manifest_text):
    """object path -> first explicit input of its `build` edge (the source).

    Keyed on the separator-normalised spelling, so the `ninja -t deps` side and
    the `build.ninja` side cannot disagree — see `_sep`.
    """
    sources = {}
    for line in manifest_text.splitlines():
        if not line.startswith("build "):
            continue
        head, sep, rest = line[len("build "):].partition(": ")
        if not sep:
            continue
        # outputs before ": ", then `RULE input input… | implicit… || order…`
        outs = [_ninja_unescape(t) for t in head.split(" ") if t]
        tail = rest.split(" ")
        ins = []
        for tok in tail[1:]:
            if tok in ("|", "||"):
                break
            if tok:
                ins.append(_ninja_unescape(tok))
        if not ins:
            continue
        for o in outs:
            sources.setdefault(_sep(o), ins[0])
    return sources


def source_has_include(path):
    """True when the file carries a `#include` directive — or cannot be read.

    ⚠ UNREADABLE READS AS *HAS INCLUDES*, deliberately: the caller uses this only
    to EXCUSE a zero record, so the direction of any doubt must be to refuse the
    excuse and fail loud.
    """
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            return any(_INCLUDE_DIRECTIVE.match(line) for line in fh)
    except OSError:
        return True


def target_verdict(build_dir, allow_non_ninja):
    """-> ("run"|"skip"|"fatal", message) for one named build directory.

    ★ A PURE FUNCTION, SPLIT OUT SO THE SELF-TEST DRIVES THE SAME DECISION THE
    REAL PATH DRIVES. The three outcomes are deliberately NOT collapsed: they are
    the difference between "verified", "you asked me not to look" and "I could
    not look", and returning 0 for the last of those is the defect this split
    closes (D-GATE-NINJA-DEPS-EXITS-ZERO-ON-A-DIRECTORY-THAT-DOES-NOT-EXIST).
    """
    d = Path(build_dir)
    if not d.is_dir():
        return ("fatal",
                f"ninja-deps: FATAL -- {build_dir} does not exist; the check did NOT "
                f"run. A missing tree is 'nothing ran', never 'nothing to check'. "
                f"(If the path is a pre-migration flat one, the tree is now "
                f"build/<name> -- see D-BUILD-LAYOUT-FLAT-ROOT-BUILD-DIRS-NOT-MIGRATED.)")
    if not (d / "build.ninja").is_file():
        if allow_non_ninja:
            return ("skip",
                    f"ninja-deps: SKIP {build_dir} -- no build.ninja, and "
                    f"--allow-non-ninja was passed. NOTHING WAS VERIFIED here.")
        return ("fatal",
                f"ninja-deps: FATAL -- {build_dir} exists but has no build.ninja, so "
                f"`ninja -t deps` has nothing to read and the check did NOT run. If "
                f"this tree is deliberately built by a non-ninja generator, say so "
                f"with --allow-non-ninja; an unasked-for skip is indistinguishable "
                f"from a pass.")
    return ("run", "")


def check(build_dir, allow_non_ninja=False):
    """Returns an exit code for one build directory."""
    d = Path(build_dir)
    verdict, message = target_verdict(build_dir, allow_non_ninja)
    if verdict == "skip":
        print(message)
        return 0
    if verdict == "fatal":
        print(message)
        return 2
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

    # ── the `deps = msvc` arm (see the block above `_NINJA_INCLUDE`) ──────────
    # Reached ONLY when the manifest declares `deps = msvc` AND something was
    # flagged, so a gcc tree pays nothing and can never enter it.
    msvc_excused = []
    if flagged:
        manifest = _ninja_manifest_text(build_dir)
        modes = deps_modes(manifest)
        if "msvc" in modes and "gcc" not in modes:
            srcs = object_sources(manifest)
            still = []
            for o in flagged:
                src = srcs.get(_sep(o))
                if src is None:
                    still.append(o)          # cannot name its source -> fail loud
                    continue
                p = src if os.path.isabs(src) else os.path.join(build_dir, src)
                if source_has_include(p):
                    still.append(o)
                else:
                    msvc_excused.append((o, src))
            flagged = still

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

    skipped = len(empty) - len(flagged) - len(msvc_excused)
    note = f" ({skipped} allowlisted)" if skipped else ""
    # ★ THE EXCUSAL IS SAID OUT LOUD, per object, with the reason and the source it
    # was read from. An excusal nobody can see is the silent skip this file refuses
    # everywhere else.
    if msvc_excused:
        note += (f" ({len(msvc_excused)} excused: `deps = msvc` records HEADERS ONLY, "
                 "and these sources carry no `#include` directive, so a zero record is "
                 "the correct record)")
    print(f"ninja-deps: OK {build_dir} -- {total} objects, all carry header deps{note}")
    for o, src in msvc_excused[:40]:
        print(f"    excused: {o}  <- {src}")
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

    # ── the TARGET-RESOLUTION verdict, which had no pin at all until 2026-08-23
    # and was WRONG in the one direction that matters
    # (D-GATE-NINJA-DEPS-EXITS-ZERO-ON-A-DIRECTORY-THAT-DOES-NOT-EXIST).
    # ★ Driven through `check()` as well as `target_verdict`, because the defect
    # was not in the decision — there was no decision — it was in the EXIT CODE
    # `check()` returned for it. Both arms assert the MESSAGE: "fatal" has two
    # distinct causes that share exit 2, and an arm that reads only the code
    # cannot tell which one it proved.
    import tempfile as _tempfile
    _tmp = _tempfile.mkdtemp(prefix="ninja-deps-selftest-")
    try:
        missing = str(Path(_tmp) / "no-such-tree")
        empty = str(Path(_tmp) / "not-a-ninja-tree")
        Path(empty).mkdir()
        ninja = str(Path(_tmp) / "looks-like-ninja")
        Path(ninja).mkdir()
        (Path(ninja) / "build.ninja").write_text("# marker\n", encoding="utf-8")

        def verdict_case(name, path, allow, want_kind, want_rc, says):
            kind, msg = target_verdict(path, allow)
            if kind != want_kind or says not in msg:
                fails.append(f"{name}: got ({kind!r}, {msg!r}), want {want_kind!r} "
                             f"saying {says!r}")
            if kind != "run":
                # ⚠ `check()`'s own print is CAPTURED here. A self-test that emits
                # the word FATAL five times on its way to OK teaches the reader to
                # skim past FATAL, which is the opposite of what this battery wants.
                import contextlib as _ctx
                import io as _io
                _buf = _io.StringIO()
                with _ctx.redirect_stdout(_buf):
                    rc = check(path, allow)
                if rc != want_rc:
                    fails.append(f"{name}: check() returned {rc}, want {want_rc}")
                if says not in _buf.getvalue():
                    fails.append(f"{name}: check() printed {_buf.getvalue()!r}, "
                                 f"which does not say {says!r}")

        verdict_case("a MISSING directory is FATAL, never a skip", missing, False,
                     "fatal", 2, "does not exist")
        verdict_case("--allow-non-ninja does NOT excuse a missing directory",
                     missing, True, "fatal", 2, "does not exist")
        verdict_case("a directory with no build.ninja is FATAL by default",
                     empty, False, "fatal", 2, "no build.ninja")
        verdict_case("... and a reported SKIP only when it was ASKED for",
                     empty, True, "skip", 0, "--allow-non-ninja was passed")
        verdict_case("a real ninja tree is RUN", ninja, False, "run", 0, "")

        # ── the `deps = msvc` excusal, pinned in BOTH directions ─────────────
        # ★ Written as a REMOVE-direction fixture: the manifest below is the one a
        # real MSVC tree emits, and each case removes the property that earns the
        # excusal rather than adding one that grants it
        # ([[feedback-a-fixture-must-synthesize-the-negative]]).
        msvc_dir = Path(_tmp) / "msvctree"
        (msvc_dir / "src").mkdir(parents=True)
        (msvc_dir / "src" / "no_includes.cpp").write_text(
            '// a comment that merely SAYS #include, which is not a directive\n'
            'static_assert(true, "x");\n', encoding="utf-8")
        (msvc_dir / "src" / "has_includes.cpp").write_text(
            "#include <vector>\nint f();\n", encoding="utf-8")
        (msvc_dir / "build.ninja").write_text(
            "include rules.ninja\n"
            "build a.obj: CXX src/no_includes.cpp | pch.hxx || order\n"
            "build b.obj: CXX src/has_includes.cpp\n", encoding="utf-8")
        (msvc_dir / "rules.ninja").write_text(
            "rule CXX\n  command = cl\n  deps = msvc\n", encoding="utf-8")

        def msvc_case(name, objs, want_flagged, want_excused):
            m = _ninja_manifest_text(str(msvc_dir))
            if deps_modes(m) != {"msvc"}:
                fails.append(f"{name}: deps_modes read {deps_modes(m)!r}, want {{'msvc'}}")
            srcs = object_sources(m)
            flagged, excused = [], []
            for o in objs:
                s = srcs.get(_sep(o))
                if s is None or source_has_include(str(msvc_dir / s)):
                    flagged.append(o)
                else:
                    excused.append(o)
            if flagged != want_flagged or excused != want_excused:
                fails.append(f"{name}: flagged={flagged!r} excused={excused!r}, "
                             f"want flagged={want_flagged!r} excused={want_excused!r}")

        msvc_case("an msvc zero-dep TU with NO #include is excused",
                  ["a.obj"], [], ["a.obj"])
        msvc_case("an msvc zero-dep TU that DOES #include still FAILS",
                  ["b.obj"], ["b.obj"], [])
        msvc_case("an object the manifest cannot name a source for still FAILS",
                  ["ghost.obj"], ["ghost.obj"], [])
        # THE DIRECTIONAL HALF: a gcc manifest must not reach the arm at all.
        gcc_dir = Path(_tmp) / "gcctree"
        gcc_dir.mkdir()
        (gcc_dir / "build.ninja").write_text(
            "rule CXX\n  command = g++\n  deps = gcc\n"
            "build a.o: CXX src/no_includes.cpp\n", encoding="utf-8")
        if "msvc" in deps_modes(_ninja_manifest_text(str(gcc_dir))):
            fails.append("a gcc manifest was read as declaring deps = msvc")
        # AND A MIXED TREE IS NOT AN MSVC TREE: the guard keeps its full strength
        # wherever any gcc-flavoured rule is present.
        (gcc_dir / "build.ninja").write_text(
            "rule CXX\n  command = g++\n  deps = gcc\n"
            "rule RC\n  command = rc\n  deps = msvc\n", encoding="utf-8")
        _mixed = deps_modes(_ninja_manifest_text(str(gcc_dir)))
        if not ("msvc" in _mixed and "gcc" in _mixed):
            fails.append(f"a mixed manifest read as {_mixed!r}, want both flavours")
    finally:
        import shutil as _shutil
        _shutil.rmtree(_tmp, ignore_errors=True)
        if Path(_tmp).exists():
            fails.append("self-test temp tree was not removed")

    if fails:
        print("ninja-deps self-test: FAIL")
        for f in fails:
            print("   ", f)
        return 1
    print("ninja-deps self-test: OK (7 parser cases, 5 target-verdict cases, "
          "5 deps=msvc excusal cases)")
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
    ★ THAT LAST SENTENCE WAS A CLAIM ABOUT `check()` THAT `check()` DID NOT HONOUR
    until 2026-08-23: it printed SKIP and returned 0 for a missing directory, so
    the auto-pick landed on the right path and the tool then said nothing about
    it. `target_verdict` is where the claim became true
    (D-GATE-NINJA-DEPS-EXITS-ZERO-ON-A-DIRECTORY-THAT-DOES-NOT-EXIST).
    """
    return "build/dbg" if Path("build/dbg").is_dir() else (
        "build-dbg" if Path("build-dbg").is_dir() else "build/dbg")


def main(argv):
    if "--self-test" in argv:
        return self_test()
    unknown = [a for a in argv
               if a.startswith("-") and a not in ("--self-test", "--allow-non-ninja")]
    if unknown:
        print("ninja-deps: FATAL -- unknown argument(s): %s. Refusing to run rather "
              "than silently ignoring a flag the caller believed in."
              % " ".join(unknown))
        return 2
    allow_non_ninja = "--allow-non-ninja" in argv
    dirs = [a for a in argv if not a.startswith("-")] or [default_build_dir()]
    worst = 0
    for d in dirs:
        worst = max(worst, check(d, allow_non_ninja))
    return worst


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
