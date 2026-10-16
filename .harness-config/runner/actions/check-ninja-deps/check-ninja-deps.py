#!/usr/bin/env python3
# PURPOSE: refuse a gate over a build directory whose objects recorded no header dependencies.
"""check-ninja-deps.py — refuse a gate over a build directory whose objects have
no recorded header dependencies.

★★★ WHY THIS EXISTS. A green ctest proves nothing about the current source if the
objects it linked were never rebuilt. That is not hypothetical here:
ninja recording ZERO header deps under concurrent builds was measured
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
WRITTEN HERE: the tool exited ZERO on a directory that does not exist.
✔MEASURED 2026-08-23 at 6dc63be0, from the repo root:

    $ python <this program> build-dbg        (started by hand, as every program was then)
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

Usage: `dssharness run check-ninja-deps` -- its steps run `--self-test`, then the leg's own build directory
(`{buildDir}`, which the runner's `requireBuild` just built). The program's forms:
    [build-dir ...]                 # default: build/dbg, else build-dbg, in THIS script's tree
    --allow-non-ninja <dir>         # a directory that is not a ninja build, named on purpose
    <build-dir> --record=<object>[,<object>...]   # print the record of each object whose path ends so
    --self-test
`--record` is the action's step `record`, which has a runner of its own: what ninja holds for an
object, path by path, in a build directory of the leg's own tree AS IT STANDS -- the reading a red
entry is judged from. It builds nothing first (a rebuild could replace the very record asked
for), so the directory is named by hand, relative to the tree. Each `<object>` is the END of one
object's path and names exactly ONE record, or nothing is printed for any. The directory is one
of the tree THIS PROGRAM LIVES IN -- one whose real path is anywhere else is refused -- and
every line printed passes the one redactor (.harness-config/runner/actions/redact/redact.py).
    dssharness run check-ninja-deps-record --legs <leg> --input build=build/<processor>-<toolchain>-<config> --input object=<the end of an object's path> -v

The default is TRANSITION-SAFE by design. The repo is moving to a single build
root (`build/<name>`; see .claude/skills/dss-cycle/references/build-layout.md), and
a default that named only
the new path would break every gate run made before the physical move — while a
default naming only the old one would silently keep checking a dead tree after
it. It therefore prefers `build/dbg` and falls back to `build-dbg`, so there is
no flag day. ⚠ A missing default is FATAL, never a skip: "the tree I was told to
check is not there" must not read as "nothing to check".

Exit: 0 clean · 1 dep-less objects found · 2 the instrument could not run
(including: the named directory does not exist, or is not a ninja build dir and
`--allow-non-ninja` was not passed).
"""

import importlib.util
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path
sys.dont_write_bytecode = True  # a by-path load must not write __pycache__ beside another action (the rule: check-scripts-index)

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
# A FACT ABOUT ALL OF THEM: a gcc depfile fact, read as a fact about every ninja tree.
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
# above stays EMPTY. On a `deps = msvc` tree a zero record is the CORRECT record of
# an object exactly when the compiler listed no header ninja's reader keeps — and
# that is read out of the tree, per object, every run (`msvc_zero_record`):
#   * the object's OWN rule must be `deps = msvc`: on any other rule a zero record is
#     a lost one, and a manifest that cannot be read whole excuses nothing;
#   * an edge that MAKES a precompiled header never has a correct zero record — the
#     compiler reads every header of it there and lists them, the forced one first;
#   * the unit's includes are what its command line FORCES in (`/FI`, `-include`) and
#     what its source's directives name, read as the preprocessor reads a directive
#     line: after line splices and comments, `#` or `%:`, `include`, `include_next`
#     or `import`. A command line that holds an `@file` is refused: its words are unread;
#   * a forced header is the precompiled header's own only where a `/Yu` word names the
#     SAME file and the product (`.pch`) is among the edge's implicit inputs;
#   * every other include is looked up where the unit's own command line looks — beside
#     the source for the quoted form, then every include directory the edge states;
#   * a header found there is a header the record MUST name, unless the precompiled
#     header the edge names already read it (its own record names it), or it lies
#     OUTSIDE the project under a path ninja's reader of `/showIncludes` drops (one
#     that holds `program files` or `microsoft visual studio`) — and NO record of the
#     tree names a path under one: a single such path and the reader does not drop
#     them here, whatever any ninja's source says;
#   * a header found in none of them is one the compiler found in its own directories.
#     Where it stands is read off the TREE'S OWN RECORDS, per header: a record that
#     names a path ending with it, or names a file in a directory that holds it, says
#     the reader keeps it here — then the empty record lost it. A header the records
#     place nowhere is the toolchain's own, which the reader drops.
# Anything else — a header of the project the record does not name, a directive
# whose text names no file, a source that cannot be read — is the LOST record this
# tool exists to catch, and still is.
# ⚠ WHERE THE COMPILER LIVES IS NOT ASKED, and that was a proxy read twice wrong: a
# compiler outside those paths with the platform's own headers under them convicted a
# correct record, and one under them with a kit elsewhere excused a lost one.
# ⚠ THE ONE THING THE RECORDS CANNOT SAY: a header the reader keeps, in a directory no
# record of the tree names, that this unit alone includes. Its only witness is the
# record that is lost. It is excused as the toolchain's own, and that limit is stated
# here rather than hidden behind a location nobody measured.
# ⚠ A HEADER OF THE PROJECT UNDER SUCH A PATH IS NOT EXCUSED, and that is the point:
# a tree built below `Program Files` has every header dropped, ninja rebuilds nothing
# on a header's change there, and "all records are empty" is then the defect itself —
# said as that: THE TREE CANNOT BE GUARDED THERE, not "this unit lost its record".
# What is the project's is read off the tree: the build directory, and the source
# directory its `CMakeCache.txt` names; a cache that names none makes every header
# under such a path the project's. The toolchain is excused because it is not the
# project's source; a compiler that moves is caught by the host-compiler stamp every
# precompiled header includes.
# ★ THE FIRST RULE WRITTEN HERE EXCUSED ONLY A SOURCE WITH NO `#include` AT ALL, AND
# THAT WAS A FALSE FAIL. ✔MEASURED 2026-10-10 on an MSVC Release tree of this repo:
# `1 of 803 objects have ZERO recorded header deps` for a unit whose includes were
# one header of the shared precompiled header and four standard headers — every one
# of them answered as above, so its empty record was the correct one, and the entry
# reddened a leg on a tree that had lost nothing.
# ✔MEASURED THE SAME DAY, ON THE MSVC LEG, WITH THE RULE AS IT THEN STOOD -- which did
# not yet count a forced header, so its figures are one include short of today's -- (the
# action's `record` step for the records, the freshness entry's own line for the verdict):
#   * the object that makes the tests' precompiled header compiles a source that names
#     NOTHING — one comment line — and is handed its header by `/FI`. That header names
#     every standard header of the precompiled list and `<gtest/gtest.h>`, with no
#     precompiled state to hide any of them, and the record is 23 paths: googletest's
#     21, the build tree's compiler stamp, the forced header itself. Not one standard
#     header. (✔MEASURED 2026-10-12 on the same leg's tree, by reading: all 14 objects
#     that make a precompiled header are that shape, and the rule as it then stood
#     excused an empty record of each one — "carries no `#include` directive" — with
#     the forced header never read.)
#   * `pch_stub.cpp`, which has no include, recorded 0 paths; given five (one the
#     precompiled header had read, `<vector>` and `<string>` of its list, `<csetjmp>`
#     and `<clocale>` of no precompiled header) it recorded 0 paths again, and the
#     entry said `excused: ... of its 5 include(s), 1 the precompiled header its edge
#     names already read, 4 the toolchain's own` — the convicted shape, on a real tree.
#   * a test unit with project headers recorded those five and no toolchain path.
#   On the macOS leg, `deps = gcc`, one unit's record named 896 paths, 869 of them the
#   toolchain's: the flavours differ exactly as the rule says.
# ★ AND THE ESCAPE IS DIRECTIONAL, PER OBJECT: an object of a `deps = gcc` rule is never
# excused — a zero record there is always a lost one, and it is said with its rule — so
# nothing is weakened on the gcc and clang legs, and the MSVC leg stops reporting a
# defect its tree does not contain.
# ⓘ THE PCH IS NOT A HOLE. An object's ninja edge carries
# `| …/cmake_pch.hxx …/cmake_pch.cxx.pch` as EXPLICIT implicit inputs, so ninja
# rebuilds it when the PCH moves from the BUILD GRAPH, never from `.ninja_deps`.
# A zero deps record cannot cost that rebuild. ✔Read out of the generated
# `build.ninja`.
_NINJA_INCLUDE = re.compile(r"^\s*(?:include|subninja)\s+(.+?)\s*$")
_NINJA_DEPS_MODE = re.compile(r"^\s*deps\s*=\s*(\w+)\s*$")
_NINJA_RULE = re.compile(r"^rule\s+(\S+)\s*$")
# A directive line, once splices and comments are gone: `#` or its digraph, the
# directive's name, and what follows it.
_DIRECTIVE = re.compile(r"[ \t\f\v]*(?:#|%:)[ \t\f\v]*([A-Za-z_][A-Za-z0-9_]*)?(.*)$", re.DOTALL)
_HEADER_NAME = re.compile(r'[ \t\f\v]*(?:<([^>\n]+)>|"([^"\n]+)")')
_INCLUDE_DIRECTIVES = ("include", "include_next", "import")
_CODE_BREAK = re.compile(r'/\*|//|["\']')
_RAW_STRING_OPEN = re.compile(r'"([^ ()\\\n]{0,16})\(')
_RAW_STRING_PREFIXES = ("R", "LR", "uR", "UR", "u8R")
_CACHE_HOME = re.compile(r"^CMAKE_HOME_DIRECTORY:[A-Z]+=(.+)$")
_COMMAND_WORD = re.compile(r'(?:[^\s"]|"[^"]*")+')
# What ninja's `deps = msvc` reader calls a system header and does not record.
_NINJA_DROPS = ("program files", "microsoft visual studio")
# The spellings a command line states an include directory with, longest first.
_INCLUDE_DIR_FLAGS = ("-external:I", "/external:I", "-imsvc", "/imsvc", "-isystem", "-I", "/I")
# The spellings that force a header in before the source's first line (`-include <file>`
# is read beside them: it is always a word of its own), and those that make and use a
# precompiled header.
_FORCED_INCLUDE_FLAGS = ("/FI", "-FI")
_PCH_MAKE_FLAGS = ("/Yc", "-Yc")
_PCH_USE_FLAGS = ("/Yu", "-Yu")


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


def read_manifest(build_dir):
    """`build.ninja` plus the files it includes/subninjas, ONE level down
    -> (text, [what of it could not be read]).

    One level is what CMake's Ninja generator writes — `CMakeFiles/rules.ninja`, which
    carries every `deps =` line — and it is CHECKED rather than assumed: a file that
    cannot be read, and an include found one level further down, are each a problem said
    by name, and a manifest with a problem excuses nothing (`zero_records`).
    """
    d = Path(build_dir)
    try:
        text = (d / "build.ninja").read_text(encoding="utf-8", errors="replace")
    except OSError as exc:
        return "", [f"build.ninja cannot be read ({exc.strerror or exc})"]
    parts, problems = [text], []
    for line in text.splitlines():
        m = _NINJA_INCLUDE.match(line)
        if not m:
            continue
        name = _ninja_unescape(m.group(1))
        try:
            sub = (d / name).read_text(encoding="utf-8", errors="replace")
        except OSError as exc:
            problems.append(f"{name}, which build.ninja includes, cannot be read "
                            f"({exc.strerror or exc})")
            continue
        parts.append(sub)
        deeper = [inner.strip() for inner in sub.splitlines() if _NINJA_INCLUDE.match(inner)]
        if deeper:
            problems.append(f"{name} includes a further file ({deeper[0]}), which this check "
                            f"does not read")
    return "\n".join(parts), problems


def rule_flavours(manifest_text):
    """rule name -> the `deps =` flavour it declares ("" where it declares none)."""
    flavours, current = {}, None
    for line in manifest_text.splitlines():
        m = _NINJA_RULE.match(line)
        if m:
            current = m.group(1)
            flavours[current] = ""
        elif current is not None and line[:1] in (" ", "\t"):
            d = _NINJA_DEPS_MODE.match(line)
            if d:
                flavours[current] = d.group(1)
        elif line.strip() and not line.lstrip().startswith("#"):
            current = None
    return flavours


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


def _key(build_dir, p):
    """One spelling for a path of the build: absolute, with this host's separators and case.

    Three writers spell one file three ways — `build.ninja` relative with backslashes
    (and a `.\\\\` before a precompiled header's product), `ninja -t deps` relative with
    forward slashes, a command line absolute — and every lookup below is between two of
    them. A path that is not absolute is the build directory's.
    """
    p = _sep(p)
    if not os.path.isabs(p):
        p = os.path.join(build_dir, p)
    return os.path.normcase(os.path.normpath(p))


def _ninja_words(text):
    """The words of a manifest line: split at spaces, a `$`-escaped character taken as itself."""
    out, cur, i = [], [], 0
    while i < len(text):
        c = text[i]
        if c == "$" and i + 1 < len(text):
            cur.append(text[i + 1])
            i += 2
            continue
        if c == " ":
            if cur:
                out.append("".join(cur))
                cur = []
        else:
            cur.append(c)
        i += 1
    if cur:
        out.append("".join(cur))
    return out


def edges(manifest_text, build_dir):
    """output (as `_key` spells it) -> its `build` edge.

    An edge is `{"rule", "outputs", "inputs", "implicit", "vars"}`: its explicit inputs
    (the first is a compile edge's source), the implicit ones after `|` (where CMake
    states a precompiled header), and the variables bound under it (`INCLUDES`, `FLAGS`).
    Order-only inputs and validations are no input of the question asked here.
    """
    table, current = {}, None
    for line in manifest_text.splitlines():
        if line.startswith("build "):
            current = None
            head, sep, rest = line[len("build "):].partition(": ")
            if not sep:
                continue
            outs = [w for w in _ninja_words(head) if w != "|"]
            tail = _ninja_words(rest)
            if not tail:
                continue
            explicit, implicit, where = [], [], 0
            for word in tail[1:]:
                if word == "|":
                    where = 1
                elif word in ("||", "|@"):
                    where = 2
                elif where == 0:
                    explicit.append(word)
                elif where == 1:
                    implicit.append(word)
            current = {"rule": tail[0], "outputs": outs, "inputs": explicit,
                       "implicit": implicit, "vars": {}}
            for o in outs:
                table.setdefault(_key(build_dir, o), current)
        elif current is not None and line[:1] in (" ", "\t") and "=" in line:
            name, _, value = line.strip().partition("=")
            current["vars"][name.strip()] = value.strip()
        else:
            current = None
    return table


def parse_records(text):
    """object (as `ninja -t deps` spells it) -> the paths its record names.

    The same header line `parse` counts by; the indented lines under it are the record.
    """
    records, current = {}, None
    for line in text.splitlines():
        m = _HEADER.match(line)
        if m:
            current = records.setdefault(m.group("obj"), [])
        elif current is not None and line[:1] in (" ", "\t") and line.strip():
            current.append(line.strip())
        else:
            current = None
    return records


def command_words(edge):
    """The words of an edge's own command line this check reads — its `INCLUDES` and its
    `FLAGS`, quotes taken off — less `-Xclang`, which only hands the next word on."""
    words = []
    for name in ("INCLUDES", "FLAGS"):
        words += [w.replace('"', "") for w in _COMMAND_WORD.findall(edge["vars"].get(name, ""))]
    return [w for w in words if w != "-Xclang"]


def include_directories(words):
    """The directories a command line searches, in the order it states them, each in
    `_sep`'s spelling: glued to the flag or the word after it.

    ⚠ THE SPELLING IS NOT COSMETIC. A manifest written on Windows states a directory with
    backslashes, and read on a host where a backslash is an ordinary character the
    directory names nothing: every header it holds is then "found in no directory of the
    command line" and counted as the toolchain's own -- a LOST record excused. ✔MEASURED
    2026-10-10 on the macOS leg, the self-test's own MSVC-shaped tree: three cases red,
    each saying `0 the precompiled header ..., 3 the toolchain's own`.
    """
    dirs, i = [], 0
    while i < len(words):
        for flag in _INCLUDE_DIR_FLAGS:
            if words[i].startswith(flag):
                d = words[i][len(flag):]
                if not d and i + 1 < len(words):
                    i += 1
                    d = words[i]
                if d:
                    dirs.append(_sep(d))
                break
        i += 1
    return dirs


def forced_includes(words):
    """The headers a command line forces in before the source's first line, in `_sep`'s
    spelling: `/FI<file>`, `/FI <file>`, `-include <file>`."""
    forced, i = [], 0
    while i < len(words):
        value = ""
        if words[i] == "-include":
            i += 1
            value = words[i] if i < len(words) else ""
        else:
            for flag in _FORCED_INCLUDE_FLAGS:
                if words[i].startswith(flag):
                    value = words[i][len(flag):]
                    if not value and i + 1 < len(words):
                        i += 1
                        value = words[i]
                    break
        if value:
            forced.append(_sep(value))
        i += 1
    return forced


def _raw_string_at(text, i):
    """The opening of a raw string literal whose `"` stands at `i`, or None."""
    k = i
    while k > 0 and (text[k - 1].isalnum() or text[k - 1] == "_"):
        k -= 1
    return _RAW_STRING_OPEN.match(text, i) if text[k:i] in _RAW_STRING_PREFIXES else None


def _digit_separator_at(text, i):
    """Whether the `'` at `i` separates digits of a number (`1'000`) and opens no character
    literal: the run of name characters before it begins with a digit. (A literal's prefix
    -- `u`, `U`, `L`, `u8` -- begins with a letter.)"""
    k = i
    while k > 0 and (text[k - 1].isalnum() or text[k - 1] in "_'."):
        k -= 1
    return k < i and text[k].isdigit()


def _code_lines(text):
    """`text` as its logical lines of CODE: every line splice joined, every comment a
    space, a string or character literal kept whole (a `//` inside one opens no comment)
    and a raw string literal emptied (a line of its text is no line of the program).

    ★ EVERY LITERAL BUT A RAW ONE ENDS AT ITS LINE'S END, closed or not: whatever this
    reader gets wrong inside one line -- a stray quote in skipped text, a spelling it does
    not know -- cannot swallow the directive on the next. What can is a comment or a raw
    string opened where the compiler opens none, and those two are read exactly.
    """
    text = text.replace("\r\n", "\n").replace("\r", "\n").replace("\\\n", "")
    out, i, n = [], 0, len(text)
    while i < n:
        m = _CODE_BREAK.search(text, i)
        if not m:
            out.append(text[i:])
            break
        out.append(text[i:m.start()])
        i = m.start()
        if m.group() == "'" and _digit_separator_at(text, i):
            out.append("'")
            i += 1
        elif m.group() == "/*":
            j = text.find("*/", i + 2)
            out.append(" ")
            i = n if j < 0 else j + 2
        elif m.group() == "//":
            j = text.find("\n", i)
            i = n if j < 0 else j
        elif m.group() == '"' and _raw_string_at(text, i):
            raw = _raw_string_at(text, i)
            j = text.find(")" + raw.group(1) + '"', raw.end())
            out.append('""')
            i = n if j < 0 else j + len(raw.group(1)) + 2
        else:
            quote, j = m.group(), i + 1
            while j < n and text[j] != quote and text[j] != "\n":
                j += 2 if text[j] == "\\" else 1
            j = min(j, n)
            if j < n and text[j] == quote:
                j += 1
            out.append(text[i:j])
            i = j
    return "".join(out).split("\n")


def include_directives(text):
    """Every include directive of a source text -> [(header name | None, quoted?, the
    directive as written)]: `include`, `include_next` and `import`, read as the
    preprocessor reads a directive line. A name of None is a directive whose text names no
    file -- a macro's expansion -- which nothing here can look up."""
    found = []
    for line in _code_lines(text):
        m = _DIRECTIVE.match(line)
        if not m or m.group(1) not in _INCLUDE_DIRECTIVES:
            continue
        h = _HEADER_NAME.match(m.group(2))
        found.append(((h.group(1) or h.group(2)) if h else None,
                      bool(h and h.group(2) is not None), line.strip()))
    return found


def _dropped(path):
    """Whether ninja's `deps = msvc` reader is taken to drop `path` as a system header's."""
    low = path.lower()
    return any(word in low for word in _NINJA_DROPS)


def _within(path, root):
    """Whether `path` is `root` or lies under it; both as `_key` spells a path."""
    return path == root or path.startswith(root.rstrip(os.sep) + os.sep)


def tree_evidence(build_dir, records):
    """What the tree itself says of where a header stands and of what ninja's reader keeps.

    `records` is every record of the tree, keys and paths as `_key` spells them. ->
      "recorded"  every path a record names;
      "places"    every directory a record names a file in;
      "kept"      the recorded paths under a path the reader is taken to drop: ONE of them,
                  and the reader does not drop such paths HERE, whatever any ninja's source says;
      "home"      the project's source directory as the tree's `CMakeCache.txt` names it,
                  or None where the cache cannot be read or names none;
      "placed"    what `recorded_place` has answered so far, a header's name -> its place.
    ⚠ A PLACE IS A NAME'S, NOT A COMPILER'S SEARCH: a header of the toolchain that shares
    its name with a file a record names, or with a file in a recorded directory, is
    placed there and its unit's empty record convicted. That error is toward FAIL, the
    only direction this check may err in, and the line says where the header was placed.
    """
    recorded = sorted({d for deps in records.values() for d in deps})
    home = None
    try:
        cache = (Path(build_dir) / "CMakeCache.txt").read_text(encoding="utf-8", errors="replace")
    except OSError:
        cache = ""
    for line in cache.splitlines():
        m = _CACHE_HOME.match(line)
        if m:
            home = _key(build_dir, m.group(1).strip())
    return {"recorded": recorded, "places": sorted({os.path.dirname(d) for d in recorded}),
            "kept": [d for d in recorded if _dropped(d)], "home": home, "placed": {}}


def recorded_place(name, evidence):
    """Where this tree's records place a header no command line states: a recorded path
    that ends with it, or the file of that name under a directory a record names a file
    in -- that directory, or one as many levels above it as the name has directories of
    its own (`sys/types.h` beside a recorded `<root>/sys/stat.h` is under `<root>`).
    None where they place it nowhere."""
    tail = os.path.normcase(os.path.normpath(name))
    if tail not in evidence["placed"]:
        place = next((d for d in evidence["recorded"]
                      if d == tail or d.endswith(os.sep + tail)), None)
        for directory in ([] if place else evidence["places"]):
            root = directory
            for _ in range(tail.count(os.sep) + 1):
                if os.path.isfile(os.path.join(root, tail)):
                    place = os.path.join(root, tail)
                    break
                root = os.path.dirname(root)
            if place:
                break
        evidence["placed"][tail] = place
    return evidence["placed"][tail]


def msvc_zero_record(obj, build_dir, table, flavours, records, evidence):
    """Is `#deps 0` the CORRECT record of this object?

    -> (True, what answers every include) | (False, what the record lost). The rule is
    the block above `_NINJA_INCLUDE`; every doubt is a refusal. `table` is `edges()`'s,
    `flavours` is `rule_flavours()`'s, `records` is `parse_records()`'s with keys and
    paths as `_key` spells them, `evidence` is `tree_evidence()`'s.
    """
    edge = table.get(_key(build_dir, obj))
    if edge is None or not edge["inputs"]:
        return (False, "the manifest names no source for it")
    # The flavour is the edge's own where it binds one, else its rule's.
    bound = edge["vars"].get("deps")
    flavour = bound if bound is not None else flavours.get(edge["rule"], "")
    if flavour != "msvc":
        whose = "its edge binds" if bound is not None else f"its rule {edge['rule']} is"
        return (False, f"{whose} `deps = {flavour or '(none)'}`, where a zero record is always "
                       f"a lost one: only `deps = msvc` leaves a correct one")
    words = command_words(edge)
    unread = next((w for w in words if w.startswith("@")), None)
    if unread:
        return (False, f"its command line holds {unread}, a file of further words this check "
                       f"does not read")
    if any(w.startswith(_PCH_MAKE_FLAGS) or w == "-emit-pch" for w in words):
        return (False, "its edge MAKES a precompiled header: the compiler reads every header of "
                       "it there and lists them, the forced one first, so its record is never empty")
    src = _sep(edge["inputs"][0])
    path = src if os.path.isabs(src) else os.path.join(build_dir, src)
    try:
        with open(path, "r", encoding="utf-8", errors="replace", newline="") as fh:
            text = fh.read()
    except OSError:
        return (False, f"its source {src} cannot be read")
    named = [(name, True, True) for name in forced_includes(words)]
    for name, quoted, shown in include_directives(text):
        if name is None:
            return (False, f"{src} holds an include directive whose text names no file "
                           f"({shown[:60]})")
        named.append((_sep(name), quoted, False))
    if not named:
        return (True, f"{src} names no header: it carries no include directive and its command "
                      f"line forces none in")
    # What the precompiled header(s) this edge names already read: the record of the
    # object that makes each one (CMake states the product as a phony name of it).
    in_pch, has_pch = set(), False
    for implicit in edge["implicit"]:
        if not _sep(implicit).lower().endswith(".pch"):
            continue
        has_pch = True
        maker = table.get(_key(build_dir, implicit))
        if maker is None:
            continue
        for made in (maker["inputs"] if maker["rule"] == "phony" else maker["outputs"]):
            in_pch.update(records.get(_key(build_dir, made), ()))
    used = {_key(build_dir, w[len(flag):]) for w in words for flag in _PCH_USE_FLAGS
            if w.startswith(flag) and w[len(flag):]}
    dirs = [d if os.path.isabs(d) else os.path.join(build_dir, d)
            for d in include_directories(words)]
    project = [_key(build_dir, ".")] + ([evidence["home"]] if evidence["home"] else [])
    from_pch = from_toolchain = from_dropped = 0
    for name, quoted, forced in named:
        if forced and has_pch and _key(build_dir, name) in used:
            from_pch += 1
            continue
        what = f"forces in {name}" if forced else f"includes {name}"
        if forced:
            places = [build_dir] + dirs
        else:
            places = ([os.path.dirname(path)] if quoted else []) + dirs
        found = next((os.path.join(d, name) for d in places
                      if os.path.isfile(os.path.join(d, name))), None)
        if found is None:
            placed = recorded_place(name, evidence)
            if placed is not None:
                return (False, f"{src} {what}, which no directory of its command line holds "
                               f"and this tree's records place at {_sep(placed)}: a header "
                               f"ninja's reader keeps here, and the record does not name it")
            from_toolchain += 1
            continue
        key = _key(build_dir, found)
        if key in in_pch:
            from_pch += 1
        elif _dropped(key) and not evidence["kept"]:
            if evidence["home"] is None:
                return (False, f"{src} {what}, which is {_sep(found)}: under a path ninja's "
                               f"reader of `/showIncludes` drops, in a tree whose CMakeCache.txt "
                               f"names no source directory, so nothing says it is not the "
                               f"project's own")
            if any(_within(key, root) for root in project):
                return (False, f"{src} {what}, which is {_sep(found)}: a header of the project "
                               f"under a path ninja's reader of `/showIncludes` drops, so no "
                               f"record of this tree can name it and the tree cannot be "
                               f"guarded there")
            from_dropped += 1
        else:
            return (False, f"{src} {what}, which is {_sep(found)}: a header the record must "
                           f"name, and neither it nor the record of a precompiled header the "
                           f"edge names does")
    return (True, f"{src}: of its {len(named)} include(s), {from_pch} the precompiled header "
                  f"its edge names already read, {from_toolchain} the toolchain's own, "
                  f"{from_dropped} under a path the reader drops")


def zero_records(build_dir, manifest_text, deps_text, objs, manifest_problems=()):
    """-> ([(object, what excuses it)], [(object, what its record lost)]) for the
    zero-record objects `objs` of one build directory.

    ★ ONE FUNCTION FOR THE REAL PATH AND THE SELF-TEST: `check()` hands it what
    `ninja -t deps` printed, the self-test a text of the same shape, and both read the
    same manifest, sources and cache from a directory.
    ★ THE ESCAPE IS DIRECTIONAL, AND DECIDED PER OBJECT: only an object whose OWN rule is
    `deps = msvc` can hold a correct zero record, so a gcc object beside an msvc one is
    never excused and an msvc one is still judged. A manifest that could not be read
    whole (`manifest_problems`) excuses nothing.
    ★ THE DIRECTORY IS MADE ABSOLUTE ONCE, HERE: every path below is keyed against it, and
    a relative one joined twice named no file at all.
    """
    build_dir = os.path.abspath(str(build_dir))
    if manifest_problems:
        why = (f"the manifest could not be read whole ({manifest_problems[0]}), so nothing "
               f"of it is trusted to excuse a record")
        return [], [(o, why) for o in objs]
    table = edges(manifest_text, build_dir)
    flavours = rule_flavours(manifest_text)
    records = {_key(build_dir, o): [_key(build_dir, d) for d in deps]
               for o, deps in parse_records(deps_text).items()}
    evidence = tree_evidence(build_dir, records)
    excused, lost = [], []
    for o in objs:
        ok, why = msvc_zero_record(o, build_dir, table, flavours, records, evidence)
        (excused if ok else lost).append((o, why))
    return excused, lost


def select_record(records, wanted):
    """-> the objects of `records` whose path ends with `wanted`, in either separator's spelling."""
    want = _sep(wanted)
    return [o for o in records if want and _sep(o).endswith(want)]


def ninja_deps(build_dir, run=None):
    """What `ninja -t deps` prints for one build directory -> (text, "") | (None, why not).

    The ONE place ninja is started, so a case can hand `check()` and `record()` a text of
    the same shape. A ninja that is not there, hangs, or exits non-zero is said as that:
    its exit code and the first line it printed are read, never dropped. (`run` is a
    case's stand-in for `subprocess.run`.)
    """
    try:
        r = (run or subprocess.run)(["ninja", "-C", str(build_dir), "-t", "deps"],
                                    capture_output=True, text=True, timeout=600)
    except FileNotFoundError:
        return None, "`ninja` is not on PATH"
    except subprocess.TimeoutExpired:
        return None, f"`ninja -t deps` timed out in {build_dir}"
    if r.returncode != 0:
        said = ((r.stderr or "") + (r.stdout or "")).strip().splitlines()
        return None, (f"`ninja -t deps` exited {r.returncode} in {build_dir}"
                      + (f" ({said[0][:200]})" if said else ""))
    return r.stdout, ""


_REDACT = None


def _redact():
    """`redact/redact.py`, the ONE redactor, loaded once by path from this file's sibling
    directory (a second statement of what must not leave a machine is the drift it ends)."""
    global _REDACT
    if _REDACT is None:
        path = os.path.join(os.path.dirname(os.path.dirname(os.path.realpath(__file__))),
                            "redact", "redact.py")
        spec = importlib.util.spec_from_file_location("dss_redact", path)
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        _REDACT = mod
    return _REDACT


def record(build_dir, wanted, deps=ninja_deps, tree=None, shown=None):
    """Print what ninja recorded for each object asked for; returns an exit code.

    `wanted` is one or more ENDS of an object's path as `ninja -t deps` spells it, separated
    by commas. An end that matches no record, or more than one, is FATAL with what matched,
    and then NOTHING is printed for any of them: a record printed for the wrong object reads
    exactly like the right one. Each path is printed as recorded, and the count says how
    many lie under a path ninja's `deps = msvc` reader drops -- on a tree where the rule
    holds that figure is zero for every object, however many toolchain headers it includes.
    ★ IT READS A BUILD DIRECTORY OF THE TREE THIS PROGRAM LIVES IN, AND NO OTHER: a directory
    whose real path is not under that tree is refused before anything is read. And every
    line it prints passes the one redactor: a `deps = gcc` record names headers by absolute
    paths, an account's home among them. (`deps`, `tree` and `shown` are a case's: the text
    ninja would print, the tree to hold the directory to, a stand-in for the redactor.)
    """
    tree = os.path.realpath(tree if tree is not None else repo_root())
    if not _within(os.path.normcase(os.path.realpath(build_dir)), os.path.normcase(tree)):
        print("ninja-deps: FATAL -- the directory asked for is not under the tree this program "
              "lives in: a record is read from a build directory of that tree and from no "
              "other. Nothing was read.")
        return 2
    if shown is None:
        try:
            shown = _redact().lazy_redactor(tree=tree, places={"<temp>": tempfile.gettempdir()})
        except (OSError, ImportError, AttributeError, SyntaxError) as exc:
            print(f"ninja-deps: FATAL -- the redactor could not be loaded "
                  f"({type(exc).__name__}); a record is printed through it or not at all. "
                  f"Nothing was read.")
            return 2
    verdict, message = target_verdict(build_dir, False)
    if verdict != "run":
        print(shown(message))
        return 2
    text, why = deps(build_dir)
    if text is None:
        print(shown(f"ninja-deps: FATAL -- {why}; nothing was read"))
        return 2
    records = parse_records(text)
    chosen = []
    for end in wanted.split(","):
        hits = select_record(records, end)
        if len(hits) != 1:
            print(shown(f"ninja-deps: FATAL -- {len(hits)} of the {len(records)} object(s) ninja "
                        f"holds a record for in {build_dir} end with {end!r}, and each end asked "
                        f"for names ONE record" + (": " + ", ".join(hits[:8]) if hits else "")
                        + ". Nothing was printed."))
            return 2
        chosen.append(hits[0])
    for obj in chosen:
        paths = records[obj]
        dropped = [d for d in paths if _dropped(d)]
        print(shown(f"ninja-deps: RECORD {obj} -- {len(paths)} path(s), {len(dropped)} of them "
                    f"under a path ninja's reader of `/showIncludes` drops"))
        for d in paths:
            print(shown(f"    {d}"))
    return 0


def target_verdict(build_dir, allow_non_ninja):
    """-> ("run"|"skip"|"fatal", message) for one named build directory.

    ★ A PURE FUNCTION, SPLIT OUT SO THE SELF-TEST DRIVES THE SAME DECISION THE
    REAL PATH DRIVES. The three outcomes are deliberately NOT collapsed: they are
    the difference between "verified", "you asked me not to look" and "I could
    not look", and returning 0 for the last of those is the defect this split
    closes.
    """
    d = Path(build_dir)
    if not d.is_dir():
        return ("fatal",
                f"ninja-deps: FATAL -- {build_dir} does not exist; the check did NOT "
                f"run. A missing tree is 'nothing ran', never 'nothing to check'. "
                f"(If the path is a pre-migration flat one, the tree is now "
                f"build/<name>.)")
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


def check(build_dir, allow_non_ninja=False, deps=ninja_deps):
    """Returns an exit code for one build directory."""
    verdict, message = target_verdict(build_dir, allow_non_ninja)
    if verdict == "skip":
        print(message)
        return 0
    if verdict == "fatal":
        print(message)
        return 2
    text, why = deps(build_dir)
    if text is None:
        print(f"ninja-deps: FATAL -- {why}; the check did not run")
        return 2

    total, empty = parse(text)

    # ★ An empty parse is a FAILURE, never a pass. "Nothing found" and "nothing ran"
    # look identical from the outside, and this project has been burned by exactly
    # that ambiguity before.
    if total == 0:
        print(f"ninja-deps: FATAL -- parsed 0 objects from `ninja -t deps` in "
              f"{build_dir}; the check proved nothing")
        return 2

    flagged = [o for o in empty if not allowed(o)]

    # ── the zero records, judged (see the block above `_NINJA_INCLUDE`) ───────
    # Asked ONLY when something was flagged. Every flagged object comes back excused
    # or lost WITH ITS REASON: an object of a `deps = gcc` rule is lost for being one,
    # so a line below never stands bare and "nothing was judged" is said per object.
    msvc_excused, why_lost = [], {}
    if flagged:
        manifest, problems = read_manifest(build_dir)
        msvc_excused, lost = zero_records(build_dir, manifest, text, flagged, problems)
        flagged = [o for o, _why in lost]
        why_lost = dict(lost)

    if flagged:
        print(f"ninja-deps: FAIL {build_dir} -- {len(flagged)} of {total} objects "
              f"have ZERO recorded header deps. Ninja will NOT rebuild these when a "
              f"header they include changes, so any gate run here proves nothing "
              f"about the current source.")
        for o in flagged[:40]:
            print(f"    {o}  -- {why_lost[o]}")
        if len(flagged) > 40:
            print(f"    … and {len(flagged) - 40} more")
        print("  FIX: delete the listed objects and rebuild, e.g.\n"
              f"    (cd {build_dir} && rm -f <objects> && cmake --build . -- -k 0)\n"
              "  `-k 0` collects EVERY failing target in one pass instead of stopping "
              "at the first, which is what makes the rebuild an exhaustive proof "
              "rather than a hopeful one. A line that says the tree CANNOT BE GUARDED "
              "is not mended by a rebuild: it names what of the tree to move.")
        return 1

    skipped = len(empty) - len(flagged) - len(msvc_excused)
    note = f" ({skipped} allowlisted)" if skipped else ""
    # ★ THE EXCUSAL IS SAID OUT LOUD, per object, with the reason and the source it
    # was read from. An excusal nobody can see is the silent skip this file refuses
    # everywhere else.
    if msvc_excused:
        note += (f" ({len(msvc_excused)} excused: `deps = msvc` records only the headers "
                 "the compiler lists and ninja's reader keeps, and every include of "
                 "these units is answered by the precompiled header its edge names or "
                 "lies where the reader drops it, so a zero record is the correct "
                 "record)")
    print(f"ninja-deps: OK {build_dir} -- {total} objects, all carry header deps{note}")
    for o, why in msvc_excused[:40]:
        print(f"    excused: {o}  <- {why}")
    return 0


# ── self-tests: the parser and the verdict rules, pinned — ONE VERDICT LINE PER ARM ──
# ★ THE OUTPUT IS A CONTRACT, read by tests/harness/test_ninja_deps_selftest.cpp:
#     ninja-deps self-test: ok      <the arm's name>
#     ninja-deps self-test: FAILED  <the arm's name> -- <what failed>
# and one closing line, `ninja-deps self-test: OK (<n> arm(s))` or `... FAIL (...)`.
# That test holds a case per arm and refuses an arm it has no case for, so an arm added
# here is added there. It is what lets a mutation of THIS program be an arm of the
# mutation sweep, which judges the exact cases a mutation reddens: a self-test with one
# verdict for everything has no cases to judge.
def _self_test_arms(arm):
    """Every arm of the self-test, in the order its verdicts are printed. `arm(name)` names
    one and returns the list its problems are appended to."""
    import contextlib as _ctx
    import io as _io
    import shutil as _shutil

    def printed(call):
        """-> (what `call()` returned, what it printed).

        ⚠ A case's own FATAL lines are CAPTURED. A self-test that emits the word FATAL a
        dozen times on its way to OK teaches the reader to skim past FATAL, which is the
        opposite of what this battery wants.
        """
        buf = _io.StringIO()
        with _ctx.redirect_stdout(buf):
            rc = call()
        return rc, buf.getvalue()

    def case(name, text, want_total, want_empty):
        problems = arm(name)
        got_total, got_empty = parse(text)
        if (got_total, got_empty) != (want_total, want_empty):
            problems.append(f"counted {got_total} and flagged [{' ; '.join(got_empty)}], want "
                            f"{want_total} and [{' ; '.join(want_empty)}]")

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

    # The record reader is the same header line, with what stands under it.
    problems = arm("a record's own lines are read under its object, and under no other")
    got = parse_records("a.obj: #deps 2, deps mtime 1 (VALID)\n    x.hpp\n    y.hpp\n\n"
                        "b.obj: #deps 0, deps mtime 1 (VALID)\n\n"
                        "c.obj: #deps 1, deps mtime 1 (VALID)\n    z.hpp\n")
    want = {"a.obj": ["x.hpp", "y.hpp"], "b.obj": [], "c.obj": ["z.hpp"]}
    if got != want:
        # A space is shown as `_`: what a reader of this line must see is an indentation kept.
        problems.append("read as " + "; ".join(f"{o}=[{','.join(d).replace(' ', '_')}]"
                                               for o, d in got.items())
                        + ", want a.obj=[x.hpp,y.hpp]; b.obj=[]; c.obj=[z.hpp]")

    # ... and ONE record is asked for by the end of its object's path.
    problems = arm("one record is named by the end of its object's path, in either "
                   "separator's spelling")
    for wanted, want in (("b.obj", ["t/b.obj"]), ("t\\b.obj", ["t/b.obj"]), (".obj", ["t/a.obj", "t/b.obj"]),
                         ("c.obj", []), ("", [])):
        got = select_record({"t/a.obj": [], "t/b.obj": ["x.hpp"]}, wanted)
        if got != want:
            problems.append(f"the end [{wanted}] selected [{' ; '.join(got)}], want "
                            f"[{' ; '.join(want)}]")

    # ── A SOURCE'S INCLUDES, READ AS THE PREPROCESSOR READS A DIRECTIVE LINE ─────────
    # ★ ONE READER. Every row of the second to the fifth arm is a spelling a pattern over
    # the raw lines missed TOWARD EXCUSED: a directive nobody saw is an include nobody
    # looked up. The last two are the other direction -- text that is no directive, and
    # text that only looks like a comment's opening and so must hide nothing after it.
    def directive_case(name, rows):
        def shape(found):
            return "[" + ", ".join(f"{header or 'no-file'}:{'quoted' if quoted else 'angle'}"
                                   for header, quoted in found) + "]"
        problems = arm(name)
        for text, want in rows:
            got = [(header, quoted) for header, quoted, _shown in include_directives(text)]
            if got != want:
                problems.append(f"read as {shape(got)}, want {shape(want)}: {text!r}")

    directive_case("a directive: the angle and the quoted form, in any spacing",
                   (("#include <a/b.h>\n", [("a/b.h", False)]),
                    ('#include "a/b.h"\n', [("a/b.h", True)]),
                    ("\t #  include<a.h>\n", [("a.h", False)]),
                    ('#include"a.h"\r\n#include <b.h>\r\n', [("a.h", True), ("b.h", False)]),
                    ("int f();\n#include <a.h> // why\n", [("a.h", False)])))
    directive_case("a directive: a comment before `#`, or inside the directive, hides nothing",
                   (('/* why */ #include "a.h"\n', [("a.h", True)]),
                    ("/* two\n lines */#include <a.h>\n", [("a.h", False)]),
                    ("# /* x */ include /* y */ <a.h>\n", [("a.h", False)])))
    directive_case("a directive: a line splice inside it hides nothing",
                   (("#\\\ninclude <a.h>\n", [("a.h", False)]),
                    ("#inc\\\nlude <a.h>\n", [("a.h", False)]),
                    ('#include \\\r\n"a.h"\r\n', [("a.h", True)])))
    directive_case("a directive: `%:` is `#`",
                   (("%:include <a.h>\n", [("a.h", False)]),
                    ('  %: include "a.h"\n', [("a.h", True)])))
    directive_case("a directive: `include_next` and `import` are includes",
                   (("#include_next <a.h>\n", [("a.h", False)]),
                    ('#import "a.h"\n', [("a.h", True)])))
    directive_case("a directive: one whose text names no file is read as naming none",
                   (("#include DSS_HEADER\n", [(None, False)]),
                    ("#include\n", [(None, False)]),
                    ("#include <unclosed\n", [(None, False)])))
    directive_case("a directive: a comment's text, a string's and a raw string's are none",
                   (("// #include <a.h>\n", []),
                    ("/*\n#include <a.h>\n*/\n", []),
                    ("// spliced \\\n#include <a.h>\n", []),
                    ('const char* s = R"x(\n#include <a.h>\n)x";\n', []),
                    ('const char* s = "\\\n#include <a.h>";\n', [])))
    directive_case("a directive: a `#` after code on its line opens none, and no other "
                   "directive is an include",
                   (("int f(); #include <a.h>\n", []),
                    ("#define A 1\n#if A\n#pragma once\n#endif\n", [])))
    directive_case("a directive: what only looks like a comment's opening hides nothing after it",
                   (('const char* s = "//";\n#include <a.h>\n', [("a.h", False)]),
                    ('const char* s = "/*";\n#include <a.h>\n', [("a.h", False)]),
                    ("char c = '\"'; const char* s = \"/*\";\n#include <a.h>\n", [("a.h", False)]),
                    ("int n = 1'000; const char* s = \"'\"; const char* t = \"/*\";\n"
                     "#include <a.h>\n", [("a.h", False)]),
                    ('const char* s = R"x(/*)x";\n#include <a.h>\n', [("a.h", False)])))

    # ── THE WORDS OF A COMMAND LINE THAT SAY WHERE A HEADER COMES FROM ───────────────
    problems = arm("an include directory is read in every spelling, glued or separated, quoted "
                   "or not, from INCLUDES and from FLAGS")
    got = include_directories(command_words({"vars": {
        "INCLUDES": r'-Ia /Ib -external:Ic /external:Id -imsvc e /imsvc f -isystem g -I "h i" '
                    r'-I"j k" -external:W0',
        "FLAGS": r"/nologo -Il\m -Xclang -isystem -Xclang n"}}))
    want = ["a", "b", "c", "d", "e", "f", "g", "h i", "j k", "l/m", "n"]
    if got != want:
        problems.append(f"read as [{' ; '.join(got)}], want [{' ; '.join(want)}]")
    problems = arm("a header forced in is read in every spelling")
    got = forced_includes(command_words({"vars": {
        "FLAGS": r'/nologo /FIa.h /FI b.h -FIc.h -include d.h -Xclang -include -Xclang e\f.h '
                 r'/FI"g h.h" /Fpx.pch /Yuy.h'}}))
    want = ["a.h", "b.h", "c.h", "d.h", "e/f.h", "g h.h"]
    if got != want:
        problems.append(f"read as [{' ; '.join(got)}], want [{' ; '.join(want)}]")

    # ── EACH RULE'S OWN FLAVOUR ──────────────────────────────────────────────────────
    problems = arm("each rule's own `deps =` flavour is read, and a rule that declares none has none")
    got = rule_flavours("rule CXX\n  command = cl\n# a comment inside the rule\n  deps = msvc\n"
                        "rule CC\n  deps = gcc\n  command = gcc\n\n"
                        "rule PLAIN\n  command = x\n"
                        "build a.o: CC a.c\n  deps = msvc\n")
    want = {"CXX": "msvc", "CC": "gcc", "PLAIN": ""}
    if got != want:
        problems.append("the flavours read " + ", ".join(f"{rule}={flavour or 'none'}"
                                                         for rule, flavour in sorted(got.items()))
                        + ", want CC=gcc, CXX=msvc, PLAIN=none")

    # ── NINJA'S OWN ANSWER: its exit code and what it said are READ ──────────────────
    class _Ran:
        def __init__(self, returncode, stdout="", stderr=""):
            self.returncode, self.stdout, self.stderr = returncode, stdout, stderr

    def answering(result):
        def run(_argv, **_how):
            if isinstance(result, BaseException):
                raise result
            return result
        return run

    one = "a.obj: #deps 1, deps mtime 1 (VALID)\n    a.h\n"
    problems = arm("ninja's answer: what it printed is handed on when it exited zero")
    if ninja_deps("x", answering(_Ran(0, one))) != (one, ""):
        problems.append("an exit code of 0 did not hand on what ninja printed, with no reason")
    problems = arm("ninja's answer: an exit code other than zero is said, with the first line "
                   "ninja printed")
    text, why = ninja_deps("x", answering(_Ran(1, one, "ninja: error: loading build.ninja: gone\n")))
    if text is not None:
        problems.append("an exit code of 1 handed on what ninja printed")
    elif "exited 1" not in why or "error: loading build.ninja: gone" not in why:
        problems.append(f"an exit code of 1 is said as [{why}], which lacks the code or the "
                        f"first line ninja printed")
    problems = arm("ninja's answer: a ninja that failed and printed nothing is said by its exit "
                   "code alone")
    text, why = ninja_deps("x", answering(_Ran(3)))
    if text is not None or why != "`ninja -t deps` exited 3 in x":
        problems.append(f"an exit code of 3 with nothing printed is said as [{why}]")
    problems = arm("ninja's answer: a ninja that is not there, or does not end, is said as that")
    for raised, says in ((FileNotFoundError("ninja"), "not on PATH"),
                         (subprocess.TimeoutExpired("ninja", 600), "timed out")):
        text, why = ninja_deps("x", answering(raised))
        if text is not None or says not in why:
            problems.append(f"{type(raised).__name__} is said as [{why}], want no text and a "
                            f"reason saying [{says}]")

    # ── the TARGET-RESOLUTION verdict, which had no pin at all until 2026-08-23
    # and was WRONG in the one direction that matters.
    # ★ Driven through `check()` as well as `target_verdict`, because the defect
    # was not in the decision — there was no decision — it was in the EXIT CODE
    # `check()` returned for it. Both arms assert the MESSAGE: "fatal" has two
    # distinct causes that share exit 2, and an arm that reads only the code
    # cannot tell which one it proved.
    _tmp = os.path.realpath(tempfile.mkdtemp(prefix="ninja-deps-selftest-"))
    held_cwd = os.getcwd()
    try:
        missing = str(Path(_tmp) / "no-such-tree")
        empty = str(Path(_tmp) / "not-a-ninja-tree")
        Path(empty).mkdir()
        ninja = str(Path(_tmp) / "looks-like-ninja")
        Path(ninja).mkdir()
        (Path(ninja) / "build.ninja").write_text("# marker\n", encoding="utf-8")

        def verdict_case(name, path, allow, want_kind, want_rc, says):
            problems = arm(name)
            kind, msg = target_verdict(path, allow)
            if kind != want_kind or says not in msg:
                problems.append(f"it is {kind} ({msg}), want {want_kind} saying [{says}]")
            if kind != "run":
                rc, out = printed(lambda: check(path, allow))
                if rc != want_rc:
                    problems.append(f"check() returned {rc}, want {want_rc}")
                if says not in out:
                    problems.append(f"check() printed [{out}], which does not say [{says}]")

        verdict_case("a MISSING directory is FATAL, never a skip", missing, False,
                     "fatal", 2, "does not exist")
        verdict_case("--allow-non-ninja does NOT excuse a missing directory",
                     missing, True, "fatal", 2, "does not exist")
        verdict_case("a directory with no build.ninja is FATAL by default",
                     empty, False, "fatal", 2, "no build.ninja")
        verdict_case("a directory with no build.ninja is a reported SKIP only when it "
                     "was ASKED for", empty, True, "skip", 0, "--allow-non-ninja was passed")
        verdict_case("a real ninja tree is RUN", ninja, False, "run", 0, "")

        # ── WHICH ZERO RECORD IS THE CORRECT ONE, pinned in BOTH directions ──────────
        # ★ Written as a REMOVE-direction fixture: the manifest below is the one a
        # real MSVC tree emits, and each LOST case removes one property an excused
        # unit had rather than adding one that would grant the excuse.
        # ★ THE TREE IS THE SHAPE CMake's Ninja generator WRITES FOR MSVC, read off a
        # real one: objects and the precompiled header's products relative with
        # backslashes, the product a phony name of the object that makes it (spelt
        # with a `.\\` the edges that use it do not have), the header and the product
        # as implicit inputs, include directories as `-I` and `-external:I` -- and the
        # words that make and use the precompiled header: the object that MAKES it
        # compiles a source of one comment line and is handed its header by `/FI`
        # beside `/Yc`; an object that USES it carries `/Yu` and `/FI` of that header.
        # ★ ONE MANIFEST HOLDS ALL THREE KINDS OF RULE -- `deps = msvc`, `deps = gcc` and
        # one that declares none -- so every excused case below is also the proof that
        # an object is judged by ITS OWN rule, whatever stands beside it.
        # ★ THE PROJECT IS TWO DIRECTORIES, AND THE FIXTURE KEEPS THEM APART: the build
        # directory, and the source directory the cache names (`home`), neither inside the
        # other -- so a rule that knows only one of them is a red of its own.
        msvc_dir = Path(_tmp) / "msvctree"
        home = Path(_tmp) / "sourcetree"
        home_inc = home / "Program Files" / "inc"
        kit = Path(_tmp) / "Program Files" / "kit" / "include"
        for sub in ("src", "inc/core", "ext/include/gtest", "pch", "pch2", "sdk/sys",
                    "Program Files/gen"):
            (msvc_dir / sub).mkdir(parents=True)
        kit.mkdir(parents=True)
        home_inc.mkdir(parents=True)
        # A header of the project's SOURCE directory under a path the reader drops.
        (home_inc / "home.h").write_text("int home();\n", encoding="utf-8")
        headers = {
            "inc/core/mine.hpp": "int mine();\n",
            "ext/include/gtest/gtest.h": "int gtest();\n",
            "src/beside.hpp": "int beside();\n",
            "pch/cmake_pch.hxx": "#include <gtest/gtest.h>\n",
            "pch/other.hxx": "int other();\n",
            "pch2/direct.hxx": "#include <gtest/gtest.h>\n",
            # A kit that is NOT under a path the reader drops and that no command line
            # states: what says it is there is a record of the tree.
            "sdk/unique.h": "int unique();\n",
            "sdk/sys/deep.h": "int deep();\n",
            # A header of the project's BUILD directory under a path the reader drops.
            "Program Files/gen/gen.h": "int gen();\n",
        }
        for name, text in headers.items():
            (msvc_dir / name).write_text(text, encoding="utf-8")
        # A kit OUTSIDE the project, under a path the reader drops, stated on a command line.
        (kit / "kit.h").write_text("int kit();\n", encoding="utf-8")
        sources = {
            "no_includes.cpp": '// a comment that merely SAYS #include, which is not a directive\n'
                               'static_assert(true, "x");\n',
            # THE UNIT THE FIRST RULE FAILED: one header of the precompiled header and
            # the toolchain's own, in both spellings of the directive.
            "all_answered.cpp": "#include <gtest/gtest.h>\n#include <vector>\n# include <string>\n",
            "toolchain_only.cpp": "#include <vector>\nint f();\n",
            "own_header.cpp": '#include <gtest/gtest.h>\n#include "core/mine.hpp"\n',
            "beside.cpp": '#include <gtest/gtest.h>\n#include "beside.hpp"\n',
            "angle_beside.cpp": "#include <beside.hpp>\n",
            "unnamed.cpp": "#include <gtest/gtest.h>\n#include DSS_SOME_HEADER\n",
            "commented.cpp": '/* why */ #include "core/mine.hpp"\n',
            "backslash.cpp": '#include "core\\mine.hpp"\n',
            "sdk_user.cpp": "#include <unique.h>\n",
            "sdk_nested.cpp": "#include <sys/deep.h>\n",
            "kit_user.cpp": "#include <kit.h>\n",
            "gen_user.cpp": '#include "gen.h"\n',
            "home_user.cpp": '#include "home.h"\n',
        }
        for name, text in sources.items():
            (msvc_dir / "src" / name).write_text(text, encoding="utf-8")
        (msvc_dir / "pch" / "cmake_pch.cxx").write_text("/* generated by CMake */\n", encoding="utf-8")
        (msvc_dir / "pch2" / "direct.cxx").write_text("/* generated by CMake */\n", encoding="utf-8")
        uses_pch = r" | pch\cmake_pch.hxx pch\cmake_pch.cxx.pch || order" + "\n"
        searches = r"  INCLUDES = -Iinc -external:Iext\include -external:W0" + "\n"
        product = r"/Fppch\.//cmake_pch.cxx.pch"
        makes = rf"  FLAGS = /nologo /Ycpch\cmake_pch.hxx {product} /FIpch\cmake_pch.hxx" + "\n"
        uses = rf"  FLAGS = /nologo /Yupch\cmake_pch.hxx {product} /FIpch\cmake_pch.hxx" + "\n"
        (msvc_dir / "build.ninja").write_text(
            "include rules.ninja\n"
            + r"build pch\.\\cmake_pch.cxx.pch: phony pch\cmake_pch.cxx.obj" + "\n"
            + r"build pch\cmake_pch.cxx.obj: CXX pch\cmake_pch.cxx | pch\cmake_pch.hxx" + "\n"
            + searches + makes
            + "build a.obj: CXX src/no_includes.cpp" + uses_pch + uses
            + r"build t\all_answered.obj: CXX src/all_answered.cpp" + uses_pch + searches + uses
            + r"build t\no_pch.obj: CXX src/all_answered.cpp || order" + "\n" + searches
            + r"build t\toolchain_only.obj: CXX src/toolchain_only.cpp" + "\n"
            + r"build t\own_header.obj: CXX src/own_header.cpp" + uses_pch + searches + uses
            + r"build t\beside.obj: CXX src/beside.cpp" + uses_pch + searches + uses
            + r"build t\angle_beside.obj: CXX src/angle_beside.cpp" + "\n" + searches
            + r"build t\unnamed.obj: CXX src/unnamed.cpp" + uses_pch + searches + uses
            + r"build t\bare.obj: CXX src/no_includes.cpp" + "\n"
            + r"build t\forced.obj: CXX src/no_includes.cpp" + "\n" + searches
            + r"  FLAGS = /nologo /FIinc\core\mine.hpp" + "\n"
            # `/Yu` and `/FI` of the header, and NO product among the edge's inputs.
            + r"build t\yu_no_product.obj: CXX src/no_includes.cpp | pch\cmake_pch.hxx || order"
            + "\n" + uses
            # The product is an input, and `/FI` forces in ANOTHER header than `/Yu` names.
            + r"build t\fi_other.obj: CXX src/no_includes.cpp" + uses_pch
            + rf"  FLAGS = /nologo /Yupch\cmake_pch.hxx {product} /FIpch\other.hxx" + "\n"
            + r"build t\rsp.obj: CXX src/no_includes.cpp" + "\n"
            + r"  FLAGS = /nologo @t\rsp.obj.rsp" + "\n"
            + r"build t\commented.obj: CXX src/commented.cpp" + "\n" + searches
            + r"build t\backslash.obj: CXX src/backslash.cpp" + "\n" + searches
            + r"build t\gone.obj: CXX src/gone.cpp" + "\n"
            + r"build t\gcc_rule.obj: CC src/no_includes.cpp" + "\n"
            + r"build t\plain_rule.obj: PLAIN src/no_includes.cpp" + "\n"
            + r"build t\edge_gcc.obj: CXX src/no_includes.cpp" + "\n" + "  deps = gcc\n"
            # A precompiled header whose product is an OUTPUT of the edge that makes it.
            + r"build pch2\direct.pch pch2\direct.obj: CXX pch2\direct.cxx | pch2\direct.hxx"
            + "\n" + searches
            + r"  FLAGS = /nologo /Ycpch2\direct.hxx /Fppch2\direct.pch /FIpch2\direct.hxx" + "\n"
            + r"build t\direct_user.obj: CXX src/all_answered.cpp | pch2\direct.hxx pch2\direct.pch"
            + "\n" + searches
            + r"  FLAGS = /nologo /Yupch2\direct.hxx /Fppch2\direct.pch /FIpch2\direct.hxx" + "\n"
            + r"build t\sdk_user.obj: CXX src/sdk_user.cpp" + "\n" + searches
            + r"build t\sdk_nested.obj: CXX src/sdk_nested.cpp" + "\n" + searches
            + r"build t\kit_user.obj: CXX src/kit_user.cpp" + "\n"
            + f'  INCLUDES = -Iinc -external:I"{kit}" -external:W0\n'
            + r"build t\gen_user.obj: CXX src/gen_user.cpp" + "\n"
            + r'  INCLUDES = -Iinc -I"Program Files\gen"' + "\n"
            + r"build t\home_user.obj: CXX src/home_user.cpp" + "\n"
            + f'  INCLUDES = -Iinc -I"{home_inc}"\n',
            encoding="utf-8")
        (msvc_dir / "rules.ninja").write_text(
            "rule CXX\n  command = cl\n  deps = msvc\n"
            "rule CC\n  command = gcc\n  deps = gcc\n"
            "rule PLAIN\n  command = lib\n", encoding="utf-8")
        # ★ THE COMPILER THIS CACHE NAMES LIVES OUTSIDE EVERY PATH THE READER DROPS, and no
        # case below depends on it: where the compiler lives is not asked.
        cache = msvc_dir / "CMakeCache.txt"
        names_its_home = (f"CMAKE_HOME_DIRECTORY:INTERNAL={_sep(str(home))}\n"
                          "CMAKE_CXX_COMPILER:FILEPATH=C:/BuildTools/VC/bin/cl.exe\n")
        cache.write_text(names_its_home, encoding="utf-8")
        # What `ninja -t deps` prints: the precompiled header's own record names the
        # headers it read; every object under test carries a zero record.
        pch_reads = ("pch/cmake_pch.cxx.obj: #deps 2, deps mtime 1 (VALID)\n"
                     "    ext/include/gtest/gtest.h\n    pch/cmake_pch.hxx\n\n")
        pch_reads_nothing = ("pch/cmake_pch.cxx.obj: #deps 1, deps mtime 1 (VALID)\n"
                             "    pch/cmake_pch.hxx\n\n")
        # ninja keys a record by the FIRST output of its edge.
        direct_reads = ("pch2/direct.pch: #deps 2, deps mtime 1 (VALID)\n"
                        "    ext/include/gtest/gtest.h\n    pch2/direct.hxx\n\n")

        def zero(obj):
            return f"{obj}: #deps 0, deps mtime 1 (VALID)\n"

        def elsewhere(*paths):
            """The record of an object that is not under test, naming `paths`."""
            return (f"t/elsewhere.obj: #deps {len(paths)}, deps mtime 1 (VALID)\n"
                    + "".join(f"    {p}\n" for p in paths) + "\n")

        def judged(tree, deps_text, obj):
            manifest, manifest_problems = read_manifest(str(tree))
            return zero_records(str(tree), manifest, deps_text + zero(obj), [obj],
                                manifest_problems)

        def says_so(problems, excused, lost, obj, want_excused, says):
            state = ("excused" if [o for o, _w in excused] == [obj] and not lost
                     else "lost" if [o for o, _w in lost] == [obj] and not excused
                     else "neither")
            why = ((excused + lost)[0][1] if state != "neither"
                   else f"excused={excused!r} lost={lost!r}")
            wanted = "excused" if want_excused else "lost"
            if state != wanted or says not in why:
                problems.append(f"it is {state} ({why}), want it {wanted} saying [{says}]")

        def msvc_case(name, obj, deps_text, want_excused, says):
            problems = arm(name)
            excused, lost = judged(msvc_dir, deps_text, obj)
            says_so(problems, excused, lost, obj, want_excused, says)

        # THE EXCUSED SIDE.
        msvc_case("msvc: a zero record of a unit that names no header at all is excused",
                  "t/bare.obj", pch_reads, True, "names no header")
        # THE SHAPE OF A REAL TREE'S OWN EMPTY RECORDS: a source that names nothing, on an
        # edge that hands it the precompiled header.
        msvc_case("msvc: a unit handed its only header by the precompiled header its edge names "
                  "is excused", "a.obj", pch_reads, True,
                  "of its 1 include(s), 1 the precompiled header its edge names already read")
        msvc_case("msvc: a zero record is excused when the precompiled header and the "
                  "toolchain answer every include", "t/all_answered.obj", pch_reads, True,
                  "of its 4 include(s), 2 the precompiled header its edge names already "
                  "read, 2 the toolchain's own")
        msvc_case("msvc: a zero record is excused for the toolchain's headers alone, on an "
                  "edge with no precompiled header", "t/toolchain_only.obj", pch_reads,
                  True, "of its 1 include(s), 0 the precompiled header its edge names already "
                        "read, 1 the toolchain's own")
        msvc_case("msvc: the angle form does not look beside the source",
                  "t/angle_beside.obj", pch_reads, True, "1 the toolchain's own")
        msvc_case("msvc: a precompiled header whose product is its maker's own output is read "
                  "like a phony one", "t/direct_user.obj", direct_reads, True,
                  "of its 4 include(s), 2 the precompiled header its edge names already "
                  "read, 2 the toolchain's own")
        msvc_case("msvc: a header outside the project under a path the reader drops is excused, "
                  "though the command line states its directory", "t/kit_user.obj", pch_reads,
                  True, "0 the toolchain's own, 1 under a path the reader drops")
        # THE LOST SIDE — each case REMOVES one property an excused unit had.
        msvc_case("msvc: a header of the project the precompiled header did not read is a "
                  "LOST record", "t/own_header.obj", pch_reads, False,
                  "includes core/mine.hpp")
        msvc_case("msvc: a header beside the source is found there, and LOST",
                  "t/beside.obj", pch_reads, False, "includes beside.hpp")
        msvc_case("msvc: an #include whose text names no file is refused, not guessed",
                  "t/unnamed.obj", pch_reads, False, "names no file")
        msvc_case("msvc: the same source on an edge that names NO precompiled header is "
                  "LOST", "t/no_pch.obj", pch_reads, False, "includes gtest/gtest.h")
        msvc_case("msvc: a header the precompiled header's own record does not name is "
                  "LOST", "t/all_answered.obj", pch_reads_nothing, False,
                  "includes gtest/gtest.h")
        msvc_case("msvc: an edge that MAKES a precompiled header never has a correct zero "
                  "record", "pch/cmake_pch.cxx.obj", "", False, "MAKES a precompiled header")
        msvc_case("msvc: a header the command line forces in is an include of the unit, and "
                  "LOST", "t/forced.obj", pch_reads, False, "forces in inc/core/mine.hpp")
        msvc_case("msvc: a forced header is the precompiled header's only where the product is "
                  "an input of the edge", "t/yu_no_product.obj", pch_reads, False,
                  "forces in pch/cmake_pch.hxx")
        msvc_case("msvc: a forced header other than the one `/Yu` names is not the precompiled "
                  "header's", "t/fi_other.obj", pch_reads, False, "forces in pch/other.hxx")
        msvc_case("msvc: a command line that holds an @file is refused by name",
                  "t/rsp.obj", pch_reads, False, "a file of further words")
        msvc_case("msvc: a directive behind a comment is still an include, and LOST",
                  "t/commented.obj", pch_reads, False, "includes core/mine.hpp")
        msvc_case("msvc: a quoted name spelt with a backslash is the same header on every host",
                  "t/backslash.obj", pch_reads, False, "includes core/mine.hpp")
        msvc_case("msvc: a source that cannot be read excuses nothing",
                  "t/gone.obj", pch_reads, False, "cannot be read")
        msvc_case("msvc: an object the manifest cannot name a source for still FAILS",
                  "ghost.obj", pch_reads, False, "names no source")
        # WHERE A HEADER NO COMMAND LINE STATES STANDS IS READ OFF THE TREE'S OWN RECORDS:
        # one of them names it, or names a file in a directory that holds it, and the
        # reader keeps it here -- so the empty record lost it.
        msvc_case("msvc: a header the tree's records name elsewhere is one the reader keeps, "
                  "and LOST", "t/toolchain_only.obj",
                  pch_reads + elsewhere("D:/Kits/10/ucrt/vector"), False,
                  "this tree's records place")
        msvc_case("msvc: a header in a directory the tree's records name is one the reader "
                  "keeps, and LOST", "t/sdk_user.obj", pch_reads + elsewhere("sdk/kept.h"),
                  False, "this tree's records place")
        msvc_case("msvc: a header is placed from a record deeper in its own include root",
                  "t/sdk_nested.obj", pch_reads + elsewhere("sdk/sys/other.h"), False,
                  "this tree's records place")
        # The reader's own rule is read off the tree, never trusted: ONE record that names a
        # path it is taken to drop, and no header under such a path is excused.
        msvc_case("msvc: a tree whose records name a path the reader is taken to drop excuses "
                  "no header under one", "t/kit_user.obj",
                  pch_reads + elsewhere("C:/Program Files/Microsoft Visual Studio/18/x/VC/Tools/"
                                        "MSVC/14/include/vector"),
                  False, "a header the record must name")
        msvc_case("msvc: a header of the project under a path the reader drops is a tree that "
                  "cannot be guarded there", "t/gen_user.obj", pch_reads, False,
                  "the tree cannot be guarded there")
        msvc_case("msvc: a header of the source directory the cache names is the project's too",
                  "t/home_user.obj", pch_reads, False, "the tree cannot be guarded there")
        cache.write_text("CMAKE_CXX_COMPILER:FILEPATH=C:/BuildTools/VC/bin/cl.exe\n",
                         encoding="utf-8")
        msvc_case("msvc: a tree whose cache names no source directory excuses no header under "
                  "a path the reader drops", "t/kit_user.obj", pch_reads, False,
                  "names no source directory")
        cache.write_text(names_its_home, encoding="utf-8")
        # AN OBJECT IS JUDGED BY ITS OWN RULE.
        msvc_case("msvc: an object of a `deps = gcc` rule is LOST by its own rule, beside the "
                  "msvc ones", "t/gcc_rule.obj", pch_reads, False, "its rule CC is `deps = gcc`")
        msvc_case("msvc: an object of a rule that declares no flavour is LOST, said as that",
                  "t/plain_rule.obj", pch_reads, False, "its rule PLAIN is `deps = (none)`")
        msvc_case("msvc: an edge that binds its own flavour is judged by it",
                  "t/edge_gcc.obj", pch_reads, False, "its edge binds `deps = gcc`")

        # ── A MANIFEST THAT CANNOT BE READ WHOLE EXCUSES NOTHING ─────────────────────
        broken = Path(_tmp) / "brokentree"
        deeper = Path(_tmp) / "deepertree"
        for d in (broken, deeper):
            (d / "src").mkdir(parents=True)
            (d / "src" / "bare.cpp").write_text("int f();\n", encoding="utf-8")
            (d / "build.ninja").write_text("include rules.ninja\nbuild a.obj: CXX src/bare.cpp\n",
                                           encoding="utf-8")
        (deeper / "rules.ninja").write_text(
            "rule CXX\n  command = cl\n  deps = msvc\ninclude more.ninja\n", encoding="utf-8")
        problems = arm("the manifest: a file it includes that cannot be read is named, and "
                       "excuses nothing")
        if read_manifest(str(msvc_dir))[1]:
            problems.append(f"the whole one reads with {read_manifest(str(msvc_dir))[1]!r}")
        said = read_manifest(str(broken))[1]
        if len(said) != 1 or "rules.ninja, which build.ninja includes, cannot be read" not in said[0]:
            problems.append(f"the broken one reads with {said!r}")
        said = read_manifest(missing)[1]
        if len(said) != 1 or "build.ninja cannot be read" not in said[0]:
            problems.append(f"a directory with no manifest reads with {said!r}")
        excused, lost = judged(broken, "", "a.obj")
        says_so(problems, excused, lost, "a.obj", False, "could not be read whole")
        problems = arm("the manifest: an include one level further down is named, and excuses "
                       "nothing")
        said = read_manifest(str(deeper))[1]
        if len(said) != 1 or "includes a further file (include more.ninja)" not in said[0]:
            problems.append(f"it reads with {said!r}")
        excused, lost = judged(deeper, "", "a.obj")
        says_so(problems, excused, lost, "a.obj", False, "could not be read whole")

        # ── A BUILD DIRECTORY NAMED RELATIVE TO WHERE THE CALLER STANDS ──────────────
        # Every path is keyed against the directory, so a relative one joined twice named
        # no file: the precompiled header's record was then never found.
        problems = arm("a build directory named relative to where the caller stands is read "
                       "the same")
        os.chdir(_tmp)
        try:
            excused, lost = judged("msvctree", pch_reads, "t/all_answered.obj")
        finally:
            os.chdir(held_cwd)
        says_so(problems, excused, lost, "t/all_answered.obj", True,
                "2 the precompiled header its edge names already read, 2 the toolchain's own")

        # ── `check()` USES THE VERDICT: the text ninja would print is handed in ──────
        def check_case(name, tree, answer, want_rc, says, never=()):
            problems = arm(name)
            rc, out = printed(lambda: check(str(tree), False, deps=lambda _dir: answer))
            unsaid = [s for s in says if s not in out]
            there = [s for s in never if s in out]
            if rc != want_rc or unsaid or there:
                problems.append(f"returned {rc}, want {want_rc}; unsaid: "
                                f"{' ;; '.join(unsaid) or 'nothing'}; said and must not be: "
                                f"{' ;; '.join(there) or 'nothing'}; printed: {out}")

        check_case("check: a tree whose every zero record is the correct one is OK, each "
                   "excusal said", msvc_dir,
                   (pch_reads + zero("a.obj") + zero("t/all_answered.obj"), ""), 0,
                   ("3 objects, all carry header deps", "(2 excused:", "excused: a.obj  <- ",
                    "excused: t/all_answered.obj  <- "), ("ninja-deps: FAIL",))
        check_case("check: a tree with no zero record is OK, and nothing is said to be excused",
                   msvc_dir, (pch_reads, ""), 0, ("1 objects, all carry header deps",),
                   ("excused",))
        check_case("check: one lost record FAILS the tree, said with what it lost", msvc_dir,
                   (pch_reads + zero("a.obj") + zero("t/own_header.obj"), ""), 1,
                   ("1 of 3 objects", "    t/own_header.obj  -- ", "includes core/mine.hpp"),
                   ("    a.obj",))
        check_case("check: a zero record on a rule that is not `deps = msvc` is said with its "
                   "rule", msvc_dir, (pch_reads + zero("t/gcc_rule.obj"), ""), 1,
                   ("1 of 2 objects", "    t/gcc_rule.obj  -- its rule CC is `deps = gcc`"))
        check_case("check: a manifest that cannot be read whole FAILS every zero record",
                   broken, (zero("a.obj"), ""), 1,
                   ("1 of 1 objects", "the manifest could not be read whole", "rules.ninja"))
        check_case("check: ninja's own failure is FATAL, said with what ninja said", msvc_dir,
                   (None, "`ninja -t deps` exited 1 in x (ninja: error: loading build.ninja)"),
                   2, ("FATAL", "exited 1", "error: loading build.ninja", "the check did not run"))
        check_case("check: an answer that names no object is FATAL", msvc_dir, ("", ""), 2,
                   ("FATAL", "parsed 0 objects"))

        # ── `record()`: ONE record, of a directory of the program's own tree, every line
        # through the redactor ────────────────────────────────────────────────────────
        mark = "DSS-NINJA-DEPS-MARK"
        # Two paths the reader is taken to drop, ONE word of the rule each.
        own = (f"t/{mark}.obj: #deps 3, deps mtime 1 (VALID)\n"
               f"    inc/{mark}.hpp\n"
               "    C:/Program Files/kit/include/kit.h\n"
               "    D:/BuildTools/Microsoft Visual Studio/18/x/include/vector\n\n")

        def recorded(deps_text):
            return lambda _dir: (deps_text, "")

        def masked(text):
            return text.replace(mark, "<masked>")

        problems = arm("record: one record is printed whole, every line through the redactor")
        rc, out = printed(lambda: record(str(msvc_dir), f"{mark}.obj",
                                         deps=recorded(pch_reads + own), tree=_tmp, shown=masked))
        want = ("ninja-deps: RECORD t/<masked>.obj -- 3 path(s), 2 of them under a path ninja's "
                "reader of `/showIncludes` drops\n"
                "    inc/<masked>.hpp\n"
                "    C:/Program Files/kit/include/kit.h\n"
                "    D:/BuildTools/Microsoft Visual Studio/18/x/include/vector\n")
        if rc != 0 or out != want:
            problems.append(f"returned {rc} and printed [{out}], want 0 and [{want}]")
        problems = arm("record: a record that holds nothing to mask is printed as it was recorded")
        rc, out = printed(lambda: record(str(msvc_dir), "cmake_pch.cxx.obj",
                                         deps=recorded(pch_reads + own), tree=_tmp, shown=masked))
        want = ("ninja-deps: RECORD pch/cmake_pch.cxx.obj -- 2 path(s), 0 of them under a path "
                "ninja's reader of `/showIncludes` drops\n"
                "    ext/include/gtest/gtest.h\n"
                "    pch/cmake_pch.hxx\n")
        if rc != 0 or out != want:
            problems.append(f"returned {rc} and printed [{out}], want 0 and [{want}]")
        problems = arm("record: an end that names no record, or two, prints nothing")
        for end, hits in (("no-such.obj", "0 of the 2"), (".obj", "2 of the 2"),
                          ("MARK.obj,no-such.obj", "0 of the 2")):
            rc, out = printed(lambda: record(str(msvc_dir), end, deps=recorded(pch_reads + own),
                                             tree=_tmp, shown=masked))
            if (rc != 2 or hits not in out or "Nothing was printed" not in out
                    or "ninja-deps: RECORD" in out or mark in out):
                problems.append(f"the end [{end}] returned {rc} and printed [{out}], want 2, "
                                f"[{hits}], no record and nothing unmasked")
        problems = arm("record: a directory outside the program's tree is refused before "
                       "anything is read")
        asked = []

        def never_asked(directory):
            asked.append(directory)
            return (pch_reads + own, "")

        # The second tree is a NAME the directory's own begins with: under it by its
        # letters, and not by its path. The third directory is SPELT from inside the tree
        # and leaves it through `..`: under it as written, and not where it leads.
        for which, directory, tree in (
                ("a directory inside the one asked for", str(msvc_dir), str(msvc_dir / "inc")),
                ("a sibling whose name the asked one begins with", str(msvc_dir),
                 str(Path(_tmp) / "msvc")),
                ("left through two dots by the one asked for",
                 os.path.join(str(msvc_dir), "inc", ".."), str(msvc_dir / "inc"))):
            rc, out = printed(lambda: record(directory, f"{mark}.obj", deps=never_asked,
                                             tree=tree, shown=masked))
            if rc != 2 or "is not under the tree" not in out or "RECORD" in out or asked:
                problems.append(f"with the tree {which}, it returned {rc}, asked ninja "
                                f"{len(asked)} time(s) and printed [{out}]")
                del asked[:]
        problems = arm("record: the redactor is the sibling action's own, and the tree's path "
                       "is not printed")
        inside = (f"t/x.obj: #deps 1, deps mtime 1 (VALID)\n"
                  f"    {_sep(str(msvc_dir))}/inc/core/mine.hpp\n\n")
        rc, out = printed(lambda: record(str(msvc_dir), "x.obj", deps=recorded(inside), tree=_tmp))
        # The line wanted is the sibling redactor's own rendering of that path ON THIS HOST, never a
        # spelling fixed here: a word of it may be the host's own name (a host named `tree` masks it).
        want = _redact().lazy_redactor(tree=_tmp, places={"<temp>": tempfile.gettempdir()})(
            f"    {_sep(str(msvc_dir))}/inc/core/mine.hpp")
        if rc != 0 or _sep(_tmp).lower() in _sep(out).lower() or want not in out.splitlines():
            problems.append(f"returned {rc} and printed [{out}], want 0 and the path as the "
                            f"sibling redactor shows it here, [{want}]")
        problems = arm("record: ninja's own failure is FATAL, and nothing is printed")
        rc, out = printed(lambda: record(str(msvc_dir), f"{mark}.obj", tree=_tmp, shown=masked,
                                         deps=lambda _dir: (None, "`ninja -t deps` exited 1 in x")))
        if rc != 2 or "exited 1" not in out or "nothing was read" not in out or "RECORD" in out:
            problems.append(f"returned {rc} and printed [{out}]")

        # ── `main`: EVERY DIRECTORY NAMED IS CHECKED, and the worst exit code is the program's ──
        problems = arm("main: every directory named is checked, and the worst exit code is the "
                       "program's")
        for which, argv, want_rc, says in (
                ("a directory that is no ninja tree, asked for as that",
                 [empty, "--allow-non-ninja"], 0, "--allow-non-ninja was passed"),
                ("the same directory and no flag", [empty], 2, "no build.ninja"),
                ("a missing directory before it", [missing, empty, "--allow-non-ninja"], 2,
                 "--allow-non-ninja was passed"),
                ("a missing directory after it", [empty, missing, "--allow-non-ninja"], 2,
                 "does not exist")):
            rc, out = printed(lambda: main(list(argv)))
            if rc != want_rc or says not in out:
                problems.append(f"with {which}, it returned {rc}, want {want_rc} saying [{says}]; "
                                f"printed [{out}]")
    finally:
        os.chdir(held_cwd)
        _shutil.rmtree(_tmp, ignore_errors=True)
        # `closing`: this arm runs whatever stopped the ones above, and is not where it stopped.
        problems = arm("the self-test's temporary tree is removed", closing=True)
        if Path(_tmp).exists():
            problems.append("it is still there")

    # ── `main`: WHAT `--record` REFUSES, AND A FLAG NOBODY KNOWS ─────────────────────
    # Each row must be refused BY THAT RULE: every one of them would be refused two lines
    # later for another reason, with the same exit code, so the message is what is read.
    def main_case(name, rows, says):
        problems = arm(name)
        for argv in rows:
            try:
                rc, out = printed(lambda: main(list(argv)))
            except (Exception, SystemExit) as exc:   # noqa: BLE001 - a row that is not refused
                rc, out = None, f"{type(exc).__name__}: {exc}"
            if rc != 2 or says not in out:
                problems.append(f"[{' '.join(argv)}] returned {rc} and printed [{out}], want 2 "
                                f"saying [{says}]")

    main_case("main: `--record` written twice is refused",
              (["x", "--record=a.obj", "--record=b.obj"],), "is written once")
    main_case("main: `--record` reads ONE directory, named on the command line",
              (["--record=a.obj"], ["x", "y", "--record=a.obj"]), "is written once")
    main_case("main: `--record` takes no other option",
              (["x", "--record=a.obj", "--allow-non-ninja"],), "is written once")
    # A directory is named in both rows: with the refusal gone, nothing defaults to a tree.
    main_case("main: an unknown argument is refused, not ignored",
              (["x", "--recrod=a.obj"], ["x", "--self-check"]), "unknown argument")

    problems = arm("an EXPLICIT build-tree argument wins over the default")
    if build_dirs(["x/explicit-tree", "--allow-non-ninja"]) != ["x/explicit-tree"]:
        problems.append("the default was taken")

    # ── THE SELF-TEST'S OWN FRAME: what a run that stops says, and how a verdict is printed ──
    def stops(inner):
        inner("ran")
        try:
            inner("stopped here")
            raise RuntimeError("the cause")
        finally:
            inner("closed", closing=True)

    problems = arm("a self-test that stops says where and why, and the arms that ran keep their "
                   "verdicts")
    got = _run_arms(stops)
    want = {"ran": [],
            "stopped here": ["the self-test stopped in this arm, or right after it "
                             "(RuntimeError: the cause)"],
            "closed": [],
            "the self-test ran to its end": ["it stopped after naming 3 arm(s) (RuntimeError: "
                                             "the cause)"]}
    if got != want or list(got) != list(want):
        problems.append("the arms read " + "; ".join(f"{n}: {' / '.join(p) or 'ok'}"
                                                     for n, p in got.items()))

    def stops_at_once(_inner):
        raise RuntimeError("the cause")

    problems = arm("a self-test that stops before its first arm says so in its last")
    got = _run_arms(stops_at_once)
    want = {"the self-test ran to its end": ["it stopped after naming 0 arm(s) (RuntimeError: "
                                             "the cause)"]}
    if got != want:
        problems.append("the arms read " + "; ".join(f"{n}: {' / '.join(p) or 'ok'}"
                                                     for n, p in got.items()))
    problems = arm("a failing arm's detail is printed on ONE line, through the redactor")
    under_temp = os.path.join(tempfile.gettempdir(), "dss-ninja-deps-detail")
    rc, out = printed(lambda: _print_verdicts({"fine": [], "failing": [f"at {under_temp}\nand on"]}))
    lines = out.splitlines()
    # The detail wanted is the one redactor's own rendering ON THIS HOST, on one line -- never a spelling
    # fixed here: a word of it may be the host's own name (a host named `ninja` masks it).
    said = (_redact().lazy_redactor(tree=None, places={"<temp>": tempfile.gettempdir()})(
        f"at {under_temp}\nand on").replace("\r", " ").replace("\n", " "))
    if (rc != 1 or len(lines) != 3 or lines[0] != "ninja-deps self-test: ok      fine"
            or lines[1] != f"ninja-deps self-test: FAILED  failing -- {said}"
            or tempfile.gettempdir().lower() in out.lower()
            or lines[2] != "ninja-deps self-test: FAIL (2 arm(s), 1 failed)"):
        problems.append(f"returned {rc} and printed {len(lines)} line(s): "
                        + " // ".join(lines).replace(tempfile.gettempdir(), "(the temp directory)"))

    # ── THE DEFAULT TREE IS THE ONE THIS FILE LIVES IN; AN EXPLICIT ONE STILL WINS ──
    # Arms, oracle and synthesized negatives are owned by .harness-config/runner/actions/owning-tree/owning-tree.py.
    # The resolver pinned is `default_build_dir()` itself, its `build/dbg` or `build-dbg`
    # tail taken off -- so reverting the default to a cwd-relative path reddens here.
    # ⚠ THE TAIL COMES OFF BEFORE LINKS ARE RESOLVED: a tree whose `build` is a link to
    # another disk still has its default UNDER the tree, and resolved first it named the
    # disk -- three arms red on a host that had done nothing wrong.
    def _default_tree():
        d = default_build_dir()
        for tail in (os.path.join("build", "dbg"), "build-dbg"):
            if d.endswith(os.sep + tail):
                return os.path.realpath(d[:-len(tail) - 1])
        return os.path.realpath(d)
    # ⚠ THOSE ARMS NEED `git`, AND A HOST WITHOUT IT IS SAID AS THAT: the owner refuses by
    # name, and the refusal is this arm's verdict rather than a traceback before the first.
    problems = arm("default build tree: the root arms could be run")
    try:
        root = _owning_tree().root_arms(_default_tree, (SystemExit,), False, __file__)
    except Exception as exc:   # noqa: BLE001 - the owner's Refusal, or whatever a host gives
        root = []
        problems.append(f"{type(exc).__name__}: {exc}")
    for ok, why, detail in root:
        problems = arm(f"default build tree: {why}")
        if not ok:
            problems.append(detail)


def _run_arms(body):
    """Run `body(arm)` -> {arm name: what failed in it}, in the order the arms were named.

    `arm(name)` names an arm and returns the list its problems are appended to: naming an
    arm is what gives it a verdict line. `arm(name, closing=True)` is for an arm a
    `finally` runs: it is never the arm a run stopped in.
    ★ A SELF-TEST THAT STOPS SAYS WHERE AND WHY, IN A VERDICT LINE. The verdicts are
    printed after the last arm, so a run that raised printed none: every case of the test
    that reads them failed with "0 verdict(s)" and the cause was a traceback somewhere
    else. The arms that ran keep their verdicts, the one it stopped in fails with the
    cause, and so does the last arm -- which exists for nothing else.
    """
    arms = {}
    current = []

    def arm(name, closing=False):
        if not closing:
            current[:] = [name]
        return arms.setdefault(name, [])

    stopped = ""
    try:
        body(arm)
    except (Exception, SystemExit) as exc:   # noqa: BLE001 - whatever it is, it is said
        stopped = f"{type(exc).__name__}: {exc}"
        if current:
            arms[current[0]].append(f"the self-test stopped in this arm, or right after it "
                                    f"({stopped})")
    problems = arms.setdefault("the self-test ran to its end", [])
    if stopped:
        problems.append(f"it stopped after naming {len(arms) - 1} arm(s) ({stopped})")
    return arms


def _print_verdicts(arms):
    """One verdict line per arm and the closing count; returns the exit code.

    A failing arm's detail holds paths of a temporary tree, which sits under the account's
    own directory: it passes the one redactor (built only when something failed), whole,
    and is then put on ONE line -- the reader of these lines is a program.
    """
    shown = []

    def detail(text):
        if not shown:
            try:
                shown.append(_redact().lazy_redactor(tree=None,
                                                     places={"<temp>": tempfile.gettempdir()}))
            except Exception as exc:   # noqa: BLE001 - a detail is shown redacted or not at all
                why = type(exc).__name__
                shown.append(lambda _text: f"<detail withheld: the redactor could not be "
                                           f"loaded ({why})>")
        return shown[0](text)

    failed = 0
    for name, problems in arms.items():
        if problems:
            failed += 1
            said = detail("; ".join(problems)).replace("\r", " ").replace("\n", " ")
            print(f"ninja-deps self-test: FAILED  {name} -- {said}")
        else:
            print(f"ninja-deps self-test: ok      {name}")
    if failed:
        print(f"ninja-deps self-test: FAIL ({len(arms)} arm(s), {failed} failed)")
        return 1
    print(f"ninja-deps self-test: OK ({len(arms)} arm(s))")
    return 0


def self_test():
    return _print_verdicts(_run_arms(_self_test_arms))


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
    it. `target_verdict` is where the claim became true.
    ⚠ BOTH CANDIDATES WERE RELATIVE TO THE CALLER'S CWD until P66, so the default named
    whichever tree the process happened to start in. ✔MEASURED 2026-09-15: run by path
    from inside a different repository holding an empty `build/dbg`, it probed THAT
    directory ("exists but has no build.ninja") instead of this tree's. Both are now
    resolved under the tree this file lives in (.harness-config/runner/actions/owning-tree/owning-tree.py); an
    explicit directory argument still wins, taken relative to the caller as typed.
    """
    root = repo_root()
    dbg = os.path.join(root, "build", "dbg")
    flat = os.path.join(root, "build-dbg")
    return dbg if os.path.isdir(dbg) else (flat if os.path.isdir(flat) else dbg)


_OWNING_TREE = None


def _owning_tree():
    """`.harness-config/runner/actions/owning-tree/owning-tree.py` -- the one owner of "which tree is this file in?".

    Loaded by path from this file's sibling directory (a hyphen is not a module name). It
    FAILS LOUD when absent rather than falling back to a local walk: a second copy of the
    answer is the drift that owner exists to end.
    """
    global _OWNING_TREE
    if _OWNING_TREE is None:
        path = os.path.join(os.path.dirname(os.path.dirname(os.path.realpath(__file__))),
                            "owning-tree", "owning-tree.py")
        if not os.path.isfile(path):
            print("ninja-deps: FATAL -- cannot find %s; this tool's root is resolved there "
                  "and nowhere else, so the check did NOT run." % path)
            sys.exit(2)
        spec = importlib.util.spec_from_file_location("dss_owning_tree", path)
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        _OWNING_TREE = mod
    return _OWNING_TREE


def repo_root():
    """The tree THIS FILE lives in -- see .harness-config/runner/actions/owning-tree/owning-tree.py."""
    ot = _owning_tree()
    try:
        return ot.resolve(__file__)
    except ot.Refusal as exc:
        print("ninja-deps: FATAL -- %s; the check did NOT run." % exc)
        sys.exit(2)


def build_dirs(argv):
    """The build trees to check: every positional argument as typed, else this tree's default."""
    return [a for a in argv if not a.startswith("-")] or [default_build_dir()]


def main(argv):
    if "--self-test" in argv:
        return self_test()
    wanted = [a[len("--record="):] for a in argv if a.startswith("--record=")]
    unknown = [a for a in argv
               if a.startswith("-") and not a.startswith("--record=")
               and a not in ("--self-test", "--allow-non-ninja")]
    if unknown:
        print("ninja-deps: FATAL -- unknown argument(s): %s. Refusing to run rather "
              "than silently ignoring a flag the caller believed in."
              % " ".join(unknown))
        return 2
    allow_non_ninja = "--allow-non-ninja" in argv
    dirs = build_dirs(argv)
    if wanted:
        # The objects of ONE named directory: the default tree is never guessed for a reading.
        named = [a for a in argv if not a.startswith("-")]
        if len(wanted) != 1 or len(named) != 1 or allow_non_ninja:
            print("ninja-deps: FATAL -- `--record=<object>[,<object>...]` is written once, reads "
                  "ONE build directory named on the command line, and takes no other option.")
            return 2
        return record(named[0], wanted[0])
    worst = 0
    for d in dirs:
        worst = max(worst, check(d, allow_non_ninja))
    return worst


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
