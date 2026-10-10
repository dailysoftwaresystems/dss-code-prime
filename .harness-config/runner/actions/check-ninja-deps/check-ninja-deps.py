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
object's path and names exactly ONE record, or nothing is printed for any.
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
#   * the unit's own `#include` lines are read from its source;
#   * each is looked up where the unit's own command line looks — beside the source
#     for the quoted form, then every include directory the edge states;
#   * a header found there is a header the record MUST name, unless the precompiled
#     header the edge names already read it (its own record names it): the compiler
#     does not list again what the precompiled state already holds;
#   * a header found in none of them is the toolchain's own, which ninja's reader of
#     `/showIncludes` does not keep: it drops a path that holds `program files` or
#     `microsoft visual studio`. That is read off the tree too, twice — the compiler
#     its `CMakeCache.txt` names must live under such a path, and NO record of the tree
#     may name a path under one (a single such path and the reader does not drop them
#     here, whatever any ninja's source says).
# Anything else — a header of the project the record does not name, an `#include`
# whose text names no file, a source or a cache that cannot be read, a compiler
# outside those paths — is the LOST record this tool exists to catch, and still is.
# ⚠ A HEADER OF THE PROJECT UNDER SUCH A PATH IS NOT EXCUSED, and that is the point:
# a tree built below `Program Files` has every header dropped, ninja rebuilds nothing
# on a header's change there, and "all records are empty" is then the defect itself.
# The toolchain is excused because it is not the project's source; a compiler that
# moves is caught by the host-compiler stamp every precompiled header includes.
# ★ THE FIRST RULE WRITTEN HERE EXCUSED ONLY A SOURCE WITH NO `#include` AT ALL, AND
# THAT WAS A FALSE FAIL. ✔MEASURED 2026-10-10 on an MSVC Release tree of this repo:
# `1 of 803 objects have ZERO recorded header deps` for a unit whose includes were
# one header of the shared precompiled header and four standard headers — every one
# of them answered as above, so its empty record was the correct one, and the entry
# reddened a leg on a tree that had lost nothing.
# ✔MEASURED THE SAME DAY, ON THE MSVC LEG, WITH THIS RULE (the action's `record` step
# for the records, the freshness entry's own line for the verdict):
#   * the object that makes the tests' precompiled header compiles a source naming
#     every standard header of the precompiled list and `<gtest/gtest.h>`, with no
#     precompiled state to hide any of them, and its record is 23 paths: googletest's
#     21, the build tree's compiler stamp, its own header. Not one standard header.
#   * `pch_stub.cpp`, which has no include, recorded 0 paths; given five (one the
#     precompiled header had read, `<vector>` and `<string>` of its list, `<csetjmp>`
#     and `<clocale>` of no precompiled header) it recorded 0 paths again, and the
#     entry said `excused: ... of its 5 include(s), 1 the precompiled header its edge
#     names already read, 4 the toolchain's own` — the convicted shape, on a real tree.
#   * a test unit with project headers recorded those five and no toolchain path.
#   On the macOS leg, `deps = gcc`, one unit's record named 896 paths, 869 of them the
#   toolchain's: the flavours differ exactly as the rule says.
# ★ AND THE ESCAPE IS DIRECTIONAL: on a `deps = gcc` tree this arm is UNREACHABLE,
# because `#deps 0` cannot occur there — so nothing is weakened on four of the five
# CI legs, and the fifth stops reporting a defect its tree does not contain.
# ⓘ THE PCH IS NOT A HOLE. An object's ninja edge carries
# `| …/cmake_pch.hxx …/cmake_pch.cxx.pch` as EXPLICIT implicit inputs, so ninja
# rebuilds it when the PCH moves from the BUILD GRAPH, never from `.ninja_deps`.
# A zero deps record cannot cost that rebuild. ✔Read out of the generated
# `build.ninja`.
_NINJA_INCLUDE = re.compile(r"^\s*(?:include|subninja)\s+(.+?)\s*$")
_NINJA_DEPS_MODE = re.compile(r"^\s*deps\s*=\s*(\w+)\s*$")
_INCLUDE_DIRECTIVE = re.compile(r"^\s*#\s*include\b")
_INCLUDE_NAMED = re.compile(r'^\s*#\s*include\s*(?:<([^>\n]+)>|"([^"\n]+)")')
_CACHE_COMPILER = re.compile(r"^CMAKE_(?:C|CXX)_COMPILER:[A-Z]+=(.+)$")
_COMMAND_WORD = re.compile(r'(?:[^\s"]|"[^"]*")+')
# What ninja's `deps = msvc` reader calls a system header and does not record.
_NINJA_DROPS = ("program files", "microsoft visual studio")
# The spellings a command line states an include directory with, longest first.
_INCLUDE_DIR_FLAGS = ("-external:I", "/external:I", "-imsvc", "/imsvc", "-isystem", "-I", "/I")


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


def include_directories(edge):
    """The directories an edge's own command line searches, in the order it states them,
    each in `_sep`'s spelling.

    ⚠ THE SPELLING IS NOT COSMETIC. A manifest written on Windows states a directory with
    backslashes, and read on a host where a backslash is an ordinary character the
    directory names nothing: every header it holds is then "found in no directory of the
    command line" and counted as the toolchain's own -- a LOST record excused. ✔MEASURED
    2026-10-10 on the macOS leg, the self-test's own MSVC-shaped tree: three cases red,
    each saying `0 the precompiled header ..., 3 the toolchain's own`.
    """
    dirs = []
    for name in ("INCLUDES", "FLAGS"):
        words = [w.replace('"', "") for w in _COMMAND_WORD.findall(edge["vars"].get(name, ""))]
        i = 0
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


def toolchain_headers_are_dropped(build_dir, records):
    """-> (True, "") when ninja's `deps = msvc` reader records none of this tree's
    toolchain headers, else (False, why not). `records` is every record of the tree.

    Two facts, both read on the tree. The toolchain's headers live under the compiler's
    own installation, and the tree's `CMakeCache.txt` names the compiler: it must lie
    under a path the reader drops. And the reader must in fact drop such paths HERE: one
    record of this tree that names a path under them refutes it, whatever any ninja's
    source says. ⚠ ANY DOUBT REFUSES: no cache, no compiler line, one compiler outside
    those paths, or one such recorded path, and a header found in no directory of the
    command line is NOT excused — on such a tree its path would have been recorded, so
    an empty record lost it.
    """
    kept = sorted({d for deps in records.values() for d in deps
                   if any(word in d.lower() for word in _NINJA_DROPS)})
    if kept:
        return (False, f"{len(kept)} path(s) the records of this tree name lie under a path "
                       f"ninja's reader of `/showIncludes` is taken to drop (the first: "
                       f"{kept[0]}), so on this tree a toolchain header IS recorded")
    try:
        text = (Path(build_dir) / "CMakeCache.txt").read_text(encoding="utf-8",
                                                              errors="replace")
    except OSError:
        return (False, "the tree has no readable CMakeCache.txt to name its compiler")
    compilers = [m.group(1).strip()
                 for m in (_CACHE_COMPILER.match(line) for line in text.splitlines()) if m]
    if not compilers:
        return (False, "the tree's CMakeCache.txt names no compiler")
    for c in compilers:
        if not any(word in c.lower() for word in _NINJA_DROPS):
            return (False, f"the tree's compiler is {c}, under no path ninja's reader of "
                           f"`/showIncludes` drops, so its headers are recorded")
    return (True, "")


def msvc_zero_record(obj, build_dir, table, records, toolchain):
    """Is `#deps 0` the CORRECT record of this object of a `deps = msvc` tree?

    -> (True, what answers every include) | (False, what the record lost). The rule is
    the block above `_NINJA_INCLUDE`; every doubt is a refusal. `table` is `edges()`'s,
    `records` is `parse_records()`'s keyed by `_key`, `toolchain` is
    `toolchain_headers_are_dropped()`'s answer.
    """
    edge = table.get(_key(build_dir, obj))
    if edge is None or not edge["inputs"]:
        return (False, "the manifest names no source for it")
    src = _sep(edge["inputs"][0])
    path = src if os.path.isabs(src) else os.path.join(build_dir, src)
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            lines = fh.readlines()
    except OSError:
        return (False, f"its source {src} cannot be read")
    named = []
    for line in lines:
        if not _INCLUDE_DIRECTIVE.match(line):
            continue
        m = _INCLUDE_NAMED.match(line)
        if not m:
            return (False, f"{src} holds an `#include` whose text names no file "
                           f"({line.strip()[:60]})")
        named.append((m.group(1) or m.group(2), m.group(2) is not None))
    if not named:
        return (True, f"{src} carries no `#include` directive")
    # What the precompiled header(s) this edge names already read: the record of the
    # object that makes each one (CMake states the product as a phony name of it).
    in_pch = set()
    for implicit in edge["implicit"]:
        if not _sep(implicit).lower().endswith(".pch"):
            continue
        maker = table.get(_key(build_dir, implicit))
        if maker is None:
            continue
        for made in (maker["inputs"] if maker["rule"] == "phony" else maker["outputs"]):
            in_pch.update(_key(build_dir, d) for d in records.get(_key(build_dir, made), ()))
    dirs = [d if os.path.isabs(d) else os.path.join(build_dir, d)
            for d in include_directories(edge)]
    from_pch = from_toolchain = 0
    for name, quoted in named:
        places = ([os.path.dirname(path)] if quoted else []) + dirs
        found = next((os.path.join(d, name) for d in places
                      if os.path.isfile(os.path.join(d, name))), None)
        if found is None:
            if not toolchain[0]:
                return (False, f"{src} includes {name}, which no directory of its command "
                               f"line holds, and {toolchain[1]}")
            from_toolchain += 1
        elif _key(build_dir, found) in in_pch:
            from_pch += 1
        else:
            return (False, f"{src} includes {name}, which is {_sep(found)}: a header the "
                           f"record must name, and neither it nor the record of a "
                           f"precompiled header the edge names does")
    return (True, f"{src}: of its {len(named)} include(s), {from_pch} the precompiled "
                  f"header its edge names already read, {from_toolchain} the toolchain's own")


def zero_records(build_dir, manifest_text, deps_text, objs):
    """-> ([(object, what excuses it)], [(object, what its record lost)]) for the
    zero-record objects `objs` of one build directory.

    ★ ONE FUNCTION FOR THE REAL PATH AND THE SELF-TEST: `check()` hands it what
    `ninja -t deps` printed, the self-test a text of the same shape, and both read the
    same manifest, sources and cache from a directory.
    ★ THE ESCAPE IS DIRECTIONAL, AND DECIDED HERE: only a tree whose every `deps =` rule is
    `msvc` can hold a correct zero record. With one `deps = gcc` rule in the manifest no
    object is judged at all -- each stays lost, nothing said for it -- so a gcc tree pays
    nothing and can never be excused.
    """
    modes = deps_modes(manifest_text)
    if "msvc" not in modes or "gcc" in modes:
        return [], [(o, "") for o in objs]
    table = edges(manifest_text, build_dir)
    records = {_key(build_dir, o): deps for o, deps in parse_records(deps_text).items()}
    toolchain = toolchain_headers_are_dropped(build_dir, records)
    excused, lost = [], []
    for o in objs:
        ok, why = msvc_zero_record(o, build_dir, table, records, toolchain)
        (excused if ok else lost).append((o, why))
    return excused, lost


def select_record(records, wanted):
    """-> the objects of `records` whose path ends with `wanted`, in either separator's spelling."""
    want = _sep(wanted)
    return [o for o in records if want and _sep(o).endswith(want)]


def record(build_dir, wanted):
    """Print what ninja recorded for each object asked for; returns an exit code.

    `wanted` is one or more ENDS of an object's path as `ninja -t deps` spells it, separated
    by commas. An end that matches no record, or more than one, is FATAL with what matched,
    and then NOTHING is printed for any of them: a record printed for the wrong object reads
    exactly like the right one. Each path is printed as recorded, and the count says how
    many lie under a path ninja's `deps = msvc` reader drops -- on a tree where the rule
    holds that figure is zero for every object, however many toolchain headers it includes.
    """
    verdict, message = target_verdict(build_dir, False)
    if verdict != "run":
        print(message)
        return 2
    try:
        r = subprocess.run(["ninja", "-C", str(build_dir), "-t", "deps"],
                           capture_output=True, text=True, timeout=600)
    except FileNotFoundError:
        print("ninja-deps: FATAL -- `ninja` is not on PATH; nothing was read")
        return 2
    except subprocess.TimeoutExpired:
        print(f"ninja-deps: FATAL -- `ninja -t deps` timed out in {build_dir}")
        return 2
    records = parse_records(r.stdout)
    chosen = []
    for end in wanted.split(","):
        hits = select_record(records, end)
        if len(hits) != 1:
            print(f"ninja-deps: FATAL -- {len(hits)} of the {len(records)} object(s) ninja holds a "
                  f"record for in {build_dir} end with {end!r}, and each end asked for names ONE "
                  f"record" + (": " + ", ".join(hits[:8]) if hits else "") + ". Nothing was printed.")
            return 2
        chosen.append(hits[0])
    for obj in chosen:
        deps = records[obj]
        dropped = [d for d in deps if any(word in d.lower() for word in _NINJA_DROPS)]
        print(f"ninja-deps: RECORD {obj} -- {len(deps)} path(s), {len(dropped)} of them under "
              f"a path ninja's reader of `/showIncludes` drops")
        for d in deps:
            print(f"    {d}")
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
    # Asked ONLY when something was flagged; `zero_records` itself judges nothing
    # unless every `deps =` rule of the manifest is `msvc`.
    msvc_excused, why_lost = [], {}
    if flagged:
        msvc_excused, lost = zero_records(build_dir, _ninja_manifest_text(build_dir),
                                          r.stdout, flagged)
        flagged = [o for o, _why in lost]
        why_lost = {o: why for o, why in lost if why}

    if flagged:
        print(f"ninja-deps: FAIL {build_dir} -- {len(flagged)} of {total} objects "
              f"have ZERO recorded header deps. Ninja will NOT rebuild these when a "
              f"header they include changes, so any gate run here proves nothing "
              f"about the current source.")
        for o in flagged[:40]:
            # On a `deps = msvc` tree each is said with what its record lost: an
            # object refused there was judged, not merely counted.
            print(f"    {o}" + (f"  -- {why_lost[o]}" if o in why_lost else ""))
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
        note += (f" ({len(msvc_excused)} excused: `deps = msvc` records only the headers "
                 "the compiler lists and ninja's reader keeps, and every `#include` of "
                 "these sources is answered by the precompiled header its edge names or "
                 "by the toolchain's own directories, so a zero record is the correct "
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
def self_test():
    arms = {}          # arm name -> what failed in it, in the order the arms ran

    def arm(name):
        """The problems of the arm `name`. Naming an arm is what gives it a verdict line."""
        return arms.setdefault(name, [])

    def case(name, text, want_total, want_empty):
        problems = arm(name)
        got_total, got_empty = parse(text)
        if (got_total, got_empty) != (want_total, want_empty):
            problems.append(f"got ({got_total}, {got_empty}), "
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

    # The record reader is the same header line, with what stands under it.
    problems = arm("a record's own lines are read under its object, and under no other")
    got = parse_records("a.obj: #deps 2, deps mtime 1 (VALID)\n    x.hpp\n    y.hpp\n\n"
                        "b.obj: #deps 0, deps mtime 1 (VALID)\n\n"
                        "c.obj: #deps 1, deps mtime 1 (VALID)\n    z.hpp\n")
    want = {"a.obj": ["x.hpp", "y.hpp"], "b.obj": [], "c.obj": ["z.hpp"]}
    if got != want:
        problems.append(f"got {got!r}, want {want!r}")

    # ... and ONE record is asked for by the end of its object's path.
    problems = arm("one record is named by the end of its object's path, in either "
                   "separator's spelling")
    for wanted, want in (("b.obj", ["t/b.obj"]), ("t\\b.obj", ["t/b.obj"]), (".obj", ["t/a.obj", "t/b.obj"]),
                         ("c.obj", []), ("", [])):
        got = select_record({"t/a.obj": [], "t/b.obj": ["x.hpp"]}, wanted)
        if got != want:
            problems.append(f"{wanted!r} selected {got!r}, want {want!r}")

    # ── the TARGET-RESOLUTION verdict, which had no pin at all until 2026-08-23
    # and was WRONG in the one direction that matters.
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
            problems = arm(name)
            kind, msg = target_verdict(path, allow)
            if kind != want_kind or says not in msg:
                problems.append(f"got ({kind!r}, {msg!r}), want {want_kind!r} saying {says!r}")
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
                    problems.append(f"check() returned {rc}, want {want_rc}")
                if says not in _buf.getvalue():
                    problems.append(f"check() printed {_buf.getvalue()!r}, "
                                    f"which does not say {says!r}")

        verdict_case("a MISSING directory is FATAL, never a skip", missing, False,
                     "fatal", 2, "does not exist")
        verdict_case("--allow-non-ninja does NOT excuse a missing directory",
                     missing, True, "fatal", 2, "does not exist")
        verdict_case("a directory with no build.ninja is FATAL by default",
                     empty, False, "fatal", 2, "no build.ninja")
        verdict_case("a directory with no build.ninja is a reported SKIP only when it "
                     "was ASKED for", empty, True, "skip", 0, "--allow-non-ninja was passed")
        verdict_case("a real ninja tree is RUN", ninja, False, "run", 0, "")

        # ── the `deps = msvc` excusal, pinned in BOTH directions ─────────────
        # ★ Written as a REMOVE-direction fixture: the manifest below is the one a
        # real MSVC tree emits, and each LOST case removes one property the excused
        # unit had rather than adding one that would grant the excuse.
        # ★ THE TREE IS THE SHAPE CMake's Ninja generator WRITES FOR MSVC, read off a
        # real one: objects and the precompiled header's products relative with
        # backslashes, the product a phony name of the object that makes it (spelt
        # with a `.\\` the edges that use it do not have), the header and the product
        # as implicit inputs, include directories as `-I` and `-external:I`.
        msvc_dir = Path(_tmp) / "msvctree"
        for sub in ("src", "inc/core", "ext/include/gtest", "pch"):
            (msvc_dir / sub).mkdir(parents=True)
        (msvc_dir / "inc" / "core" / "mine.hpp").write_text("int mine();\n", encoding="utf-8")
        (msvc_dir / "ext" / "include" / "gtest" / "gtest.h").write_text(
            "int gtest();\n", encoding="utf-8")
        (msvc_dir / "src" / "beside.hpp").write_text("int beside();\n", encoding="utf-8")
        sources = {
            "no_includes.cpp": '// a comment that merely SAYS #include, which is not a directive\n'
                               'static_assert(true, "x");\n',
            # THE UNIT THE FIRST RULE FAILED: one header of the precompiled header and
            # the toolchain's own, in both spellings of the directive.
            "all_answered.cpp": "#include <gtest/gtest.h>\n#include <vector>\n# include <string>\n",
            "toolchain_only.cpp": "#include <vector>\nint f();\n",
            "own_header.cpp": '#include <gtest/gtest.h>\n#include "core/mine.hpp"\n',
            "beside.cpp": '#include <gtest/gtest.h>\n#include "beside.hpp"\n',
            "unnamed.cpp": "#include <gtest/gtest.h>\n#include DSS_SOME_HEADER\n",
        }
        for name, text in sources.items():
            (msvc_dir / "src" / name).write_text(text, encoding="utf-8")
        uses_pch = r" | pch\cmake_pch.hxx pch\cmake_pch.cxx.pch || order" + "\n"
        searches = r"  INCLUDES = -Iinc -external:Iext\include -external:W0" + "\n"
        (msvc_dir / "build.ninja").write_text(
            "include rules.ninja\n"
            + r"build pch\.\\cmake_pch.cxx.pch: phony pch\cmake_pch.cxx.obj" + "\n"
            + r"build pch\cmake_pch.cxx.obj: CXX pch\cmake_pch.cxx" + "\n" + searches
            + "build a.obj: CXX src/no_includes.cpp" + uses_pch
            + r"build t\all_answered.obj: CXX src/all_answered.cpp" + uses_pch + searches
            + r"build t\no_pch.obj: CXX src/all_answered.cpp || order" + "\n" + searches
            + r"build t\toolchain_only.obj: CXX src/toolchain_only.cpp" + "\n"
            + r"build t\own_header.obj: CXX src/own_header.cpp" + uses_pch + searches
            + r"build t\beside.obj: CXX src/beside.cpp" + uses_pch + searches
            + r"build t\unnamed.obj: CXX src/unnamed.cpp" + uses_pch + searches,
            encoding="utf-8")
        (msvc_dir / "rules.ninja").write_text(
            "rule CXX\n  command = cl\n  deps = msvc\n", encoding="utf-8")
        cache = msvc_dir / "CMakeCache.txt"
        in_program_files = ("CMAKE_CXX_COMPILER:FILEPATH=C:/Program Files/Microsoft Visual "
                            "Studio/18/x/VC/Tools/MSVC/14/bin/Hostx64/x64/cl.exe\n")
        cache.write_text(in_program_files, encoding="utf-8")
        # What `ninja -t deps` prints: the precompiled header's own record names the
        # header it read; every object under test carries a zero record.
        pch_reads = ("pch/cmake_pch.cxx.obj: #deps 2, deps mtime 1 (VALID)\n"
                     "    ext/include/gtest/gtest.h\n    pch/cmake_pch.hxx\n\n")
        pch_reads_nothing = ("pch/cmake_pch.cxx.obj: #deps 1, deps mtime 1 (VALID)\n"
                             "    pch/cmake_pch.hxx\n\n")

        def msvc_case(name, obj, deps_text, want_excused, says):
            problems = arm(name)
            m = _ninja_manifest_text(str(msvc_dir))
            if deps_modes(m) != {"msvc"}:
                problems.append(f"deps_modes read {deps_modes(m)!r}, want {{'msvc'}}")
            excused, lost = zero_records(
                str(msvc_dir), m, deps_text + f"{obj}: #deps 0, deps mtime 1 (VALID)\n", [obj])
            got = excused if want_excused else lost
            other = lost if want_excused else excused
            if [o for o, _w in got] != [obj] or other or says not in got[0][1]:
                problems.append(f"excused={excused!r} lost={lost!r}, want it "
                                f"{'excused' if want_excused else 'lost'} saying {says!r}")

        # THE EXCUSED SIDE.
        msvc_case("msvc: a zero record of a unit with NO #include is excused",
                  "a.obj", pch_reads, True, "carries no `#include` directive")
        msvc_case("msvc: a zero record is excused when the precompiled header and the "
                  "toolchain answer every include", "t/all_answered.obj", pch_reads, True,
                  "of its 3 include(s), 1 the precompiled header its edge names already "
                  "read, 2 the toolchain's own")
        msvc_case("msvc: a zero record is excused for the toolchain's headers alone, on an "
                  "edge with no precompiled header", "t/toolchain_only.obj", pch_reads,
                  True, "of its 1 include(s), 0 the precompiled header")
        # THE LOST SIDE — each case REMOVES one property the excused unit had.
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
        cache.write_text("CMAKE_CXX_COMPILER:FILEPATH=C:/BuildTools/VC/bin/cl.exe\n",
                         encoding="utf-8")
        msvc_case("msvc: a compiler outside the paths ninja's reader drops excuses no "
                  "toolchain header", "t/toolchain_only.obj", pch_reads, False,
                  "under no path ninja's reader")
        cache.unlink()
        msvc_case("msvc: a tree whose cache cannot name its compiler excuses no toolchain "
                  "header", "t/toolchain_only.obj", pch_reads, False,
                  "no readable CMakeCache.txt")
        cache.write_text(in_program_files, encoding="utf-8")
        # The reader's own rule is read off the tree, never trusted: ONE record that names a
        # path under the compiler's installation, and no toolchain header is excused.
        kept = ("t/elsewhere.obj: #deps 1, deps mtime 1 (VALID)\n"
                "    C:/Program Files/Microsoft Visual Studio/18/x/VC/Tools/MSVC/14/include/vector\n\n")
        msvc_case("msvc: a tree whose records name a path the reader is taken to drop excuses "
                  "no toolchain header", "t/toolchain_only.obj", pch_reads + kept, False,
                  "a toolchain header IS recorded")
        msvc_case("msvc: an object the manifest cannot name a source for still FAILS",
                  "ghost.obj", pch_reads, False, "names no source")
        # THE DIRECTIONAL HALF, ON THE SAME TREE: one `deps = gcc` rule beside the msvc
        # one, and the unit the first case excused is excused no longer -- nor judged.
        (msvc_dir / "rules.ninja").write_text(
            "rule CXX\n  command = cl\n  deps = msvc\n"
            "rule CC\n  command = gcc\n  deps = gcc\n", encoding="utf-8")
        problems = arm("msvc: on a tree that also declares deps = gcc, no zero record is excused")
        excused, lost = zero_records(
            str(msvc_dir), _ninja_manifest_text(str(msvc_dir)),
            pch_reads + "a.obj: #deps 0, deps mtime 1 (VALID)\n", ["a.obj"])
        if excused or lost != [("a.obj", "")]:
            problems.append(f"excused={excused!r} lost={lost!r}, want a.obj lost and unjudged")
        # ... and a gcc manifest is not read as an msvc one in the first place.
        problems = arm("a gcc manifest is not read as declaring deps = msvc")
        gcc_dir = Path(_tmp) / "gcctree"
        gcc_dir.mkdir()
        (gcc_dir / "build.ninja").write_text(
            "rule CXX\n  command = g++\n  deps = gcc\n"
            "build a.o: CXX src/no_includes.cpp\n", encoding="utf-8")
        if "msvc" in deps_modes(_ninja_manifest_text(str(gcc_dir))):
            problems.append("it was read as declaring deps = msvc")
        # AND A MIXED TREE IS NOT AN MSVC TREE: the guard keeps its full strength
        # wherever any gcc-flavoured rule is present.
        problems = arm("a manifest of both flavours is read as both")
        (gcc_dir / "build.ninja").write_text(
            "rule CXX\n  command = g++\n  deps = gcc\n"
            "rule RC\n  command = rc\n  deps = msvc\n", encoding="utf-8")
        _mixed = deps_modes(_ninja_manifest_text(str(gcc_dir)))
        if not ("msvc" in _mixed and "gcc" in _mixed):
            problems.append(f"read as {_mixed!r}, want both flavours")
    finally:
        import shutil as _shutil
        _shutil.rmtree(_tmp, ignore_errors=True)
        problems = arm("the self-test's temporary tree is removed")
        if Path(_tmp).exists():
            problems.append("it is still there")

    # ── THE DEFAULT TREE IS THE ONE THIS FILE LIVES IN; AN EXPLICIT ONE STILL WINS ──
    # Arms, oracle and synthesized negatives are owned by .harness-config/runner/actions/owning-tree/owning-tree.py.
    # The resolver pinned is `default_build_dir()` itself, its `build/dbg` or `build-dbg`
    # tail taken off -- so reverting the default to a cwd-relative path reddens here.
    def _default_tree():
        d = os.path.realpath(default_build_dir())
        for tail in (os.path.join("build", "dbg"), "build-dbg"):
            if d.endswith(os.sep + tail):
                return d[:-len(tail) - 1]
        return d
    for ok, why, detail in _owning_tree().root_arms(_default_tree, (SystemExit,), False,
                                                    __file__):
        problems = arm(f"default build tree: {why}")
        if not ok:
            problems.append(detail)
    problems = arm("an EXPLICIT build-tree argument wins over the default")
    if build_dirs(["x/explicit-tree", "--allow-non-ninja"]) != ["x/explicit-tree"]:
        problems.append("the default was taken")

    failed = 0
    for name, problems in arms.items():
        if problems:
            failed += 1
            # ONE line per arm whatever a problem holds: the reader of these lines is a program.
            said = "; ".join(problems).replace("\r", " ").replace("\n", " ")
            print(f"ninja-deps self-test: FAILED  {name} -- {said}")
        else:
            print(f"ninja-deps self-test: ok      {name}")
    if failed:
        print(f"ninja-deps self-test: FAIL ({len(arms)} arm(s), {failed} failed)")
        return 1
    print(f"ninja-deps self-test: OK ({len(arms)} arm(s))")
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
