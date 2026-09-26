#!/usr/bin/env python3
"""sqlite_base.py -- the base of the SQLite corpus harness: derive a make target's recipe, read
back what the compiler built, and keep the per-(leg, artifact) verdict ledger.

It replaced `base-harness.sh` and its PowerShell twin `base-harness.ps1` on 2026-09-21 (lane mig,
part 4: no `.sh`/`.ps1` under the actions directory). The twins held DIFFERENT halves of one
contract -- the recipe derivation lived only in the bash file (the PowerShell driver ran it inside
WSL), while the reader, the manifest call, the build and the ledger were written twice -- so this
module is their UNION: one implementation of each decision, and the only place it is decided.

  * recipe derivation   `emit_recipe` over `make -n`, built from the pure pieces
                        `join_continuations`, `recipe_defines`, `recipe_includes`, `recipe_span`,
                        `recipe_token_span`, `span_tus`, `archive_members`, `archive_tus`,
                        `dedup_by_basename`, and the DROP LEDGER (`DropLedger`, `<recipe>.drops`);
  * artefact read-back  `ARTIFACT_MARKER` (defined HERE and nowhere else in this action),
                        `reported_artifacts`, `reported_artifact`;
  * one build           `generate_manifest` (+ `manifest_error`), `build_artifact`,
                        `compile_time_suffix`;
  * the ledger          `VerdictLedger`.

Nothing here asks what kind of machine it runs on: every decision is keyed on its ARGUMENTS.
Nothing here exits the process either: a refusal is an exception the caller renders in its own
vocabulary (`RecipeRefused`, exit 1 on the CLI; `HarnessUsageError`, exit 2), and a build's
outcome is a `BuildResult` value.

CLI -- a LIBRARY, whose one verb is its self-test. (Its `emit-recipe` verb, the bash twin's function
transcribed, left on 2026-09-25: no program ran it -- every caller imports `emit_recipe`.)
  python sqlite_base.py --self-test          (alias --selftest)
      `passed=N failed=N skipped=N`; exit 0 only when failed=0.

WHAT THE UNION DECIDED (each pinned by a self-test arm; the old behaviour is named beside it):
  * A report is a line that STARTS with `dsscp: artifact <spec> ` once its line end (CR
    included) is removed. The marker anywhere else on a line, or a report naming an empty path,
    is a claim that cannot be read: it is never used and never ignored -- alone it leaves the log
    with NO artefact (code 1), beside a report it makes the log AMBIGUOUS (code 2), and the error
    quotes the line. (The bash reader matched the marker anywhere and returned the whole line as a
    "path"; the PowerShell reader skipped such a line in silence.)
  * The reported path is `abspath` + `normpath`, never `realpath` (on macOS `/var` would become
    `/private/var`, a different spelling of the same file than every other line of the run).
  * A build's verdict is its LOG, because dsscp exits 0 on some fatal errors: a line containing
    `error[` (case-SENSITIVE, as the bash twin; the PowerShell twin folded case) is a diagnostic;
    the exit code is RECORDED, never judged; a compiler that cannot be started is its own code
    (`LAUNCH_FAILED`), not "the compiler said nothing"; an artefact must be a regular FILE.
  * A relative `.c` token of the recipe resolves against the BUILD DIR only (make's cwd). The bash
    twin tried the caller's cwd first, so a same-named file there was silently compiled instead.
  * Every list is sorted by CODE POINT (`sort -u` followed the host locale).
  * A recipe is read with universal newlines: a CRLF recipe left a CR inside the last token of
    every line, which then did not end in `.c` and fell out of the TU set without a word.
  * The build-host tool guard refuses a TU whose IMMEDIATE parent directory is named `tool` (and
    is not the build dir itself), or any `lempar.c`. The old `(^|/)tool/` refused every file of a
    checkout that merely sat under some `tool/` directory.
  * An archive's members are READ from its bytes (`archive_members`), GNU and BSD alike, never
    listed by the host's `ar t` -- whose spelling of a name is a property of the host (✔MEASURED
    2026-09-23: the Mac's `ar` keeps the GNU `/` terminator, and every member then left the TU
    set in silence). A file that is not an archive is refused, never read as an empty one (as a
    failing `ar` once was); a member that is not a `.o` is NOTED, never skipped in silence; the
    source walk is sorted (it was `find | head -1`, directory-listing order); a second candidate
    source for one archive member, and a path the basename dedup removes, are both REPORTED.
  * Exit 2 means the ARGUMENTS are malformed -- an unknown flag, a missing value, a switch that
    is not 0/1, a floor that is not a non-negative integer (the bash twin's `[ -lt ]` on a
    non-number switched the floor OFF), an unknown mode or scope -- and all of it is refused
    BEFORE make runs. Exit 1 means well-formed arguments the world refuses: a missing build dir,
    search root or archive, a make that cannot start, an archive that cannot be read or is
    not one, an unwritable output, a
    missing link line (`NoLinkLine`; the bash twin returned 2 with NO message), the guards and
    the floors. Every "is required" message names the REAL flag (5 of the 6 did not).
  * The manifest's recipe transform and stack reserve are REQUIRED: the PowerShell twin omitted
    them when they were $null, and the generator's defaults for an omitted flag are the pe64 ones.
  * The ledger is keyed by (leg, artifact) TUPLES ("a/b"+"c" and "a"+"b/c" were one key) and an
    EMPTY verdict is no verdict (the bash rule; the PowerShell ledger counted the key).

⚠ `make -n` IS NOT FREE OF SIDE EFFECTS, and this module does not pretend otherwise: GNU make
remakes an out-of-date MAKEFILE even under `-n`, and under `-B` every makefile that has a rule
counts as out of date. ✔MEASURED 2026-09-21 (GNU make 4.3, a scratch Makefile with a self-remake
rule): `make -n -B` EXECUTED that rule, and `make -n -B -o Makefile` did not. sqlite's autosetup
Makefile carries such a rule (it re-runs configure in the build dir), so the old drivers'
`always_make` dry run RE-CONFIGURED the tree it was only reading. ⇒ The dry run passes
`-o Makefile` ("treat it as very old; never remake it"): deriving a recipe is a READ.
"""
from __future__ import annotations

import collections
import importlib.util
import io
import json
import os
import re
import shutil
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

# The action is `requireInputsUnmoved`: no `__pycache__` may appear beside its programs.
sys.dont_write_bytecode = True
try:
    import sqlite_common as C
except ImportError:  # loaded by path from another directory: take the SIBLING, registered once
    _SPINE = os.path.join(os.path.dirname(os.path.realpath(__file__)), "sqlite_common.py")
    _spec = importlib.util.spec_from_file_location("sqlite_common", _SPINE)
    C = importlib.util.module_from_spec(_spec)
    sys.modules["sqlite_common"] = C
    _spec.loader.exec_module(C)

# The report line the compiler writes for every artefact it commits, as a wire format:
#   dsscp: artifact <targetSpec> <absolute path>
# (`TargetSpec::parse` refuses whitespace, so the spec is one token and the path is the whole
# remainder of the line.) THE one definition in this action: every reader imports it.
ARTIFACT_MARKER = "dsscp: artifact "

PREFIX = "base-harness: "
PREREQ_MODES = ("whole-blob", "link-line")
TOKEN_SCOPES = ("all", "recipe")
LAUNCH_FAILED = 6          # 5 stays the driver's own "reported but not executable" code
DASH = "—"
CR = "\r"
THIS = os.path.realpath(__file__)
REQUIRED_ARGS = ("build_dir", "make_target", "recipe_file", "out_tus", "out_defines", "out_includes")

Result = C.Result          # (rc, out, err): what `generate_manifest` returns


class RecipeRefused(Exception):
    """Well-formed arguments the world refuses (exit 1 on the CLI). The message is the refusal,
    every line prefixed `base-harness: ` as the bash twin printed it."""
    exit_code = 1


class NoLinkLine(RecipeRefused):
    """`link-line` mode found no recipe line writing `-o <target>`: a NAMED refusal."""


class HarnessUsageError(Exception):
    """Malformed arguments (exit 2 on the CLI)."""
    exit_code = 2


class LedgerHole(Exception):
    """`VerdictLedger.assert_complete`: a declared (leg, artifact) never reached a verdict."""
    exit_code = 1

    def __init__(self, artifact, missing):
        self.artifact, self.missing = artifact, list(missing)
        super().__init__(
            "%sno '%s' verdict was ever recorded for leg(s): %s\n%severy declared leg must reach "
            "a verdict %s silence about one is a harness bug."
            % (PREFIX, artifact, " ".join(self.missing), PREFIX, DASH))


def _msg(*lines):
    return "\n".join(PREFIX + line for line in lines)


def _refuse(*lines):
    raise RecipeRefused(_msg(*lines))


def _usage(line):
    raise HarnessUsageError(PREFIX + line)


# ── small I/O helpers ────────────────────────────────────────────────────────────────

def _lines(text):
    """`text` as lines: split on LF, the trailing CR of each line (a CRLF file) stripped, the
    empty remainder after a final newline dropped."""
    out = (text or "").split("\n")
    if out and out[-1] == "":
        out.pop()
    return [line.rstrip(CR) for line in out]


def _read_text(path):
    with open(path, "rb") as fh:
        return fh.read().decode("utf-8", "replace")


def _write_lines(path, items):
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("".join("%s\n" % item for item in items))


def _write_out(path, items, what):
    try:
        _write_lines(path, items)
    except OSError as exc:
        _refuse("cannot write the %s file '%s': %s" % (what, path, exc))


# ── THE DROP LEDGER ──────────────────────────────────────────────────────────────────
#
# A TU that quietly does not get compiled is the worst outcome available: the program still
# builds, strictly smaller, and fails much later looking like a codegen bug. So every path a
# recoverer could NOT keep is named on stderr (`base-harness: DROPPED [kind] …`) and recorded,
# and `emit_recipe` writes the record beside the recipe as `<recipe>.drops` (`<kind>\t<msg>`
# lines, the bash twin's format). The kinds carry different certainty:
#   archive-member      the `.o` IS in the archive, so its `.c` exists somewhere and was not
#                       found: a LOST TU. FATAL in `emit_recipe`.
#   recipe-token        a `.c`-suffixed TOKEN in shell text is not necessarily a TU (`>gen.c`, a
#                       token inside an `echo`): reported loudly and counted in the summary.
#   archive-candidate   a member's source was found more than once OUTSIDE `tsrc/` (or, for a
#                       tsrc-only member, more than once inside it): the first (roots in order,
#                       then a sorted walk) is used and the others are named.
#   basename-duplicate  a path the basename dedup removed (the same FILE must reach the
#                       compiler once; `bld/x.c` and `bld/tsrc/x.c` are the case it exists for).

class DropLedger:
    """What the recoverers could not keep, in the order they found it."""

    def __init__(self, stream=None):
        self.entries = []
        self._stream = stream

    def note(self, kind, msg):
        msg = str(msg).replace("\n", " ")
        self.entries.append((kind, msg))
        print("%sDROPPED [%s] %s" % (PREFIX, kind, msg),
              file=self._stream if self._stream is not None else sys.stderr)

    def count(self, kind):
        return sum(1 for k, _m in self.entries if k == kind)

    def write(self, path):
        _write_lines(path, ["%s\t%s" % entry for entry in self.entries])


# ── RECIPE DERIVATION ────────────────────────────────────────────────────────────────
#
# `make -n <target>` prints the commands make WOULD run: upstream's own answer for a target's
# translation units, `-D` defines and `-I` dirs, re-derived on every run rather than a list this
# harness keeps by hand against a moving tree.

# A flag counts only where a TOKEN starts: at the line's start, after whitespace, or after an
# opening quote. ✔MEASURED 2026-09-21 (helper p4s4): unanchored, a path such as
# `…/C--Source-DailySoftware-…` yielded the define `ailySoftware` -- the retired shell helpers
# carried the same pattern -- and a directory named `x-Iy` would have yielded an include dir.
_DEFINE = re.compile(r"(?<![^\s'\"])-D[A-Za-z0-9_]+(?:=[^ ]*)?")
_INCLUDE = re.compile(r"(?<![^\s'\"])-I ?[^ ]+")


def _link_line(target):
    # `-o <target>` followed by a space or the END of the line: without that anchor `-o sqlite3`
    # would also match `-o sqlite3d`, and a target that is a PREFIX of another would silently
    # harvest its sibling's inputs. The target is a LITERAL (`re.escape`).
    return re.compile("-o " + re.escape(target) + "(?: |$)")


def join_continuations(text):
    """make's backslash-continuations joined so one logical command is one line (each `\\` +
    newline becomes ONE space; the recipe's own indentation survives as runs of spaces), tabs
    turned into spaces, universal newlines. A ONE-LINE recipe survives as one line (BSD sed's
    `N` at the last line is documented to quit without printing, so the old sed join could
    lose it there -- inferred, never measured on the Mac)."""
    text = (text or "").replace(CR + "\n", "\n").replace(CR, "\n")
    text = text.replace("\\\n", " ").replace("\t", " ")
    lines = text.split("\n")
    while lines and lines[-1] == "":
        lines.pop()
    return lines


def recipe_defines(lines):
    """Every `-DNAME[=VALUE]` token (the value ends at a space), `-D` removed and every `"`
    removed -- make's literal `""` makes `X=""` the EMPTY value `X=` -- unique, code-point order."""
    got = set()
    for line in lines:
        for m in _DEFINE.finditer(line):
            got.add(m.group(0)[2:].replace('"', ""))
    return sorted(got)


def recipe_includes(lines):
    """Every `-Idir` / `-I dir`, unique, code-point order. The bare `.` is dropped: it is the
    build dir, which a caller adds under its real name (a relative `.` would resolve against the
    caller's cwd, not make's)."""
    got = set()
    for line in lines:
        for m in _INCLUDE.finditer(line):
            d = m.group(0)[2:].lstrip(" ")
            if d != ".":
                got.add(d)
    return sorted(got)


def recipe_span(lines, mode, target):
    """The slice of the recipe whose tokens name the target's INPUTS.

    whole-blob  every line: right when the recipe is essentially the ONE link command.
    link-line   ONLY the lines writing `-o <target>`: right when the recipe may still carry the
                BOOTSTRAP (build-host tools such as lemon and mksourceid, whose sources must never
                be cross-compiled into a target artefact). None is `NoLinkLine`, by name.
    """
    if mode == "whole-blob":
        return list(lines)
    if mode == "link-line":
        rx = _link_line(target)
        span = [line for line in lines if rx.search(line)]
        if not span:
            raise NoLinkLine(_msg(
                "recipe derivation for '%s' found NO link line: link-line mode reads only the "
                "line that writes '-o %s' (followed by a space or the end of the line), and none "
                "of the %d recipe line(s) does." % (target, target, len(lines))))
        return span
    _usage("recipe_span: unknown mode '%s' (want whole-blob|link-line)" % mode)


def recipe_token_span(lines, target):
    """The lines whose -D/-I tokens describe OUR program: every COMPILE line (` -c `, spaces
    included so a path containing `-c` never matches) plus the link line for `target`.

    Not the TU question: upstream compiles the library and the shell with DIFFERENT define sets
    (the library carries `SQLITE_CORE`, the shell's link line does not), and DSS builds one
    program from one define list, so the answer is their UNION -- while `make -B -n` also prints
    the jimsh/lemon bootstrap, whose `-D` tokens configure a BUILD-HOST tool, never the target."""
    rx = _link_line(target)
    return [line for line in lines if " -c " in line or rx.search(line)]


def span_tus(span, build_dir, ledger):
    """One existing `.c` path per `.c` token of `span` (tokens unique, code-point order).

    An absolute token is kept when it is a file. A RELATIVE token names a file in the build dir
    -- make's cwd -- and resolves against it ONLY: the caller's cwd is never consulted, so a
    same-named file sitting there cannot be compiled in its place. A token that resolves to no
    file is a `recipe-token` drop, reported, never discarded in silence."""
    tokens = sorted({tok for line in span for tok in line.split(" ") if tok.endswith(".c")})
    out = []
    for tok in tokens:
        cand = tok if os.path.isabs(tok) else os.path.join(build_dir, tok)
        if os.path.isfile(cand):
            out.append(cand)
        else:
            ledger.note("recipe-token",
                        "the recipe span names '%s' as a .c input and it resolves to NO file "
                        "(tried '%s') %s it is NOT in the TU set" % (tok, cand, DASH))
    return out


# ── WHAT IS IN AN ARCHIVE: READ FROM ITS BYTES, NEVER ASKED OF THE HOST'S `ar` ─────────────
#
# ✔MEASURED 2026-09-23: the Mac's `ar` (cctools, BSD; `/usr/bin/ar`, no version flag) lists a
# GNU-format archive's members WITH the GNU name terminator -- `b'a.o/\n'` where GNU ar prints
# `b'a.o\n'` -- so every member failed the `.o` test below and the archive's translation units
# left the TU set IN SILENCE. What `ar t` prints is a property of the HOST; the members are a
# property of the FILE. So the file is read, by one rule on every host, and whatever this reader
# does not understand is a NAMED refusal:
#   * `!<arch>\n`, then per member a 60-byte header -- name 16, date 12, uid 6, gid 6, mode 8,
#     size 10, the magic "`\n" -- and `size` bytes of data padded to an even offset;
#   * a GNU/SysV/COFF name ends with `/`, and `/N` is the name at offset N of the long-name
#     table (the member `//`, each entry ending `/\n` -- or NUL, as some writers end it);
#   * a BSD (Apple) name is space-padded with no terminator, and `#1/N` puts the name in the
#     FIRST N bytes of the data (NUL-padded, and counted in the size);
#   * the archive's OWN bookkeeping is never a member: the symbol indexes `/`, `/SYM64/`,
#     `__.SYMDEF`, `__.SYMDEF SORTED`, `__.SYMDEF_64`, `__.SYMDEF_64 SORTED`, and the table `//`;
#   * the pad byte after the LAST member may be missing (it carries nothing); any other short
#     read is a truncation;
#   * refused by name: a file that is not an archive (it is NOT an empty one), a THIN archive
#     (its members live outside it), a truncated header or body, a header without its magic, a
#     size that is not a number, a long name the archive does not hold, an empty name.
AR_MAGIC = b"!<arch>\n"
AR_THIN_MAGIC = b"!<thin>\n"
AR_HEADER_SIZE = 60
AR_HEADER_MAGIC = b"`\n"
AR_INDEX_MEMBERS = frozenset(["/", "/SYM64/", "__.SYMDEF", "__.SYMDEF SORTED", "__.SYMDEF_64",
                              "__.SYMDEF_64 SORTED"])
AR_LONG_NAMES = "//"


def archive_members(path):
    """The member names of the archive `path`, in archive order, read from its bytes (see the
    note above); the archive's symbol indexes and long-name table are not members. Anything the
    reader cannot account for is a `RecipeRefused` naming the byte it stopped at."""
    try:
        with open(path, "rb") as fh:
            data = fh.read()
    except OSError as exc:
        _refuse("could not read the archive '%s': %s" % (path, exc))
    if data.startswith(AR_THIN_MAGIC):
        _refuse("'%s' is a THIN archive: its members live outside it, so none of them can be read "
                "from it." % path)
    if not data.startswith(AR_MAGIC):
        _refuse("'%s' is not an ar archive: it does not begin with '!<arch>\\n' %s a file that is "
                "not an archive is NOT an empty archive." % (path, DASH))
    names, table, pos = [], None, len(AR_MAGIC)
    while pos < len(data):
        head = data[pos:pos + AR_HEADER_SIZE]
        if len(head) < AR_HEADER_SIZE:
            _refuse("'%s' is TRUNCATED: the member header at byte %d holds %d byte(s), not %d."
                    % (path, pos, len(head), AR_HEADER_SIZE))
        if head[58:60] != AR_HEADER_MAGIC:
            _refuse("'%s' is malformed: the member header at byte %d does not end with the header "
                    "magic '`\\n' (it ends %r)." % (path, pos, head[58:60]))
        field = head[0:16].decode("latin-1").rstrip(" ")
        size_text = head[48:58].decode("latin-1").strip()
        if not size_text.isdigit():
            _refuse("'%s' is malformed: the member header at byte %d gives the size %r, which is "
                    "not a number." % (path, pos, size_text))
        size, start = int(size_text), pos + AR_HEADER_SIZE
        if start + size > len(data):
            _refuse("'%s' is TRUNCATED: the member at byte %d claims %d byte(s), and the file ends "
                    "%d byte(s) after its header." % (path, pos, size, len(data) - start))
        body = data[start:start + size]
        at, pos = pos, start + size + (size & 1)
        if field == AR_LONG_NAMES:
            table = body
            continue
        if field.startswith("#1/"):
            count = field[3:]
            if not count.isdigit() or int(count) > size:
                _refuse("'%s' is malformed: the member header at byte %d names '%s', but its name "
                        "does not fit in its %d byte(s)." % (path, at, field, size))
            name = body[:int(count)].split(b"\0", 1)[0].decode("utf-8", "replace")
        elif field.startswith("/") and field not in AR_INDEX_MEMBERS:
            offset = field[1:]
            if not offset.isdigit():
                _refuse("'%s' holds a member at byte %d named '%s': neither a long-name "
                        "reference ('/N') nor an index this reader knows." % (path, at, field))
            if table is None or int(offset) >= len(table):
                _refuse("'%s' names the long name at offset %s (member at byte %d), but %s."
                        % (path, offset, at, "it has no long-name table ('//') before it"
                           if table is None else "its long-name table holds only %d byte(s)"
                           % len(table)))
            entry = table[int(offset):].split(b"\n", 1)[0].split(b"\0", 1)[0]
            name = entry[:-1] if entry.endswith(b"/") else entry
            name = name.decode("utf-8", "replace")
        elif field.endswith("/") and field not in AR_INDEX_MEMBERS:
            name = field[:-1]
        else:
            name = field
        if name in AR_INDEX_MEMBERS:
            continue
        if not name:
            _refuse("'%s' holds a member with an EMPTY name at byte %d." % (path, at))
        names.append(name)
    return names


def _in_tsrc(path):
    return "tsrc" in re.split(r"[\\/]", os.path.dirname(path))


def _source_index(roots, names):
    """{basename: [path, ...]} for the wanted basenames: roots in the order given; within a
    root a top-down walk, directories and files in code-point order, a directory's own files
    before its subdirectories (the bash twin took `find | head -1`: directory-listing order)."""
    found = {name: [] for name in names}
    for root in roots:
        if not os.path.isdir(root):
            _refuse("the search root '%s' is not a directory, so no archive member can be "
                    "recovered from it." % root)
        for dirpath, dirnames, filenames in os.walk(root):
            dirnames.sort()
            for f in sorted(filenames):
                if f in found:
                    found[f].append(os.path.join(dirpath, f))
    return found


def archive_tus(archive, filter_names, roots, ledger):
    """The `.c` each `.o` member of `archive` was compiled from, searched under `roots`.

    DSS cannot consume a gcc `.a`, so the core sources compiled into it are recovered as SOURCE.
    `filter_names` keeps only the named `.o` members (None or empty: every member -- the bash
    twin's contract). A source outside any `tsrc/` directory is preferred (sqlite's amalgamation
    staging tree duplicates generated sources under `bld/tsrc/`; `bld/` is the copy make
    compiles); a member found only under `tsrc/` is still recovered. A member found NOWHERE is an
    `archive-member` drop -- a LOST TU; a member whose preferred tier (outside `tsrc/`, else
    inside it) holds more than one candidate is an `archive-candidate` note naming every one; a
    member that is not a `.o` is an `archive-not-object` note (✔MEASURED 2026-09-23: a name this
    test did not recognise used to leave in SILENCE, and on the Mac that was every member).
    An absent archive yields nothing (a caller that NAMED one is refused by `emit_recipe` before
    this runs)."""
    if not archive or not os.path.isfile(archive):
        return []
    members = []
    for m in archive_members(archive):
        if m.endswith(".o"):
            members.append(m)
        else:
            ledger.note("archive-not-object",
                        "archive '%s' has member '%s', which is not a '.o' %s no source is "
                        "recovered for it" % (archive, m, DASH))
    wanted = set(filter_names) if filter_names else None
    picked = [m for m in members if wanted is None or m in wanted]
    index = _source_index(roots, sorted({m[:-2] + ".c" for m in picked}))
    out = []
    for obj in picked:
        name = obj[:-2] + ".c"
        cands = index.get(name, [])
        tier = [c for c in cands if not _in_tsrc(c)] or cands
        if not tier:
            ledger.note("archive-member",
                        "archive '%s' has member '%s' but NO '%s' exists under any search root "
                        "(%s) %s the TU it was compiled from is LOST from the source set"
                        % (archive, obj, name, " ".join(roots), DASH))
            continue
        out.append(tier[0])
        if len(tier) > 1:
            ledger.note("archive-candidate",
                        "archive '%s' member '%s' has %d candidate sources: %s is USED, %s is "
                        "not (roots in order, then a sorted walk; narrow the search roots if "
                        "the used one is the wrong file)"
                        % (archive, obj, len(tier), tier[0], ", ".join(tier[1:])))
    return out


def dedup_by_basename(paths, ledger=None):
    """`paths` unique, then ONE path per basename -- the code-point-first -- in code-point order.
    The surviving path is a property of the SET, never of the order it arrived in or of the host
    locale. With a `ledger`, every removed path is a `basename-duplicate` drop."""
    kept, out = {}, []
    for p in sorted({p for p in paths if p}):
        b = os.path.basename(p)
        if b in kept:
            if ledger is not None:
                ledger.note("basename-duplicate",
                            "'%s' has the same basename as '%s', which is kept (the code-point-"
                            "first path): the same FILE must reach the compiler once"
                            % (p, kept[b]))
            continue
        kept[b] = p
        out.append(p)
    return out


def _is_build_host_tool(tu, build_dir):
    """`tool/` is upstream's OWN flat directory of programs built to RUN ON THE BUILD HOST
    (lemon, mkkeywordhash, mksourceid); `lempar.c` is lemon's parser TEMPLATE, not standalone C.
    Neither can ever be a TU of a target artefact. Keyed on the TU's IMMEDIATE parent, so a
    checkout sitting under some unrelated `tool/` directory is not refused wholesale."""
    if os.path.basename(tu) == "lempar.c":
        return True
    parent = os.path.dirname(tu)
    if os.path.basename(parent) != "tool":
        return False

    def norm(p):
        return os.path.normcase(os.path.normpath(os.path.abspath(p)))
    return norm(parent) != norm(build_dir)


def _switch(flag, value):
    if isinstance(value, bool):
        return value
    if value in (0, 1, "0", "1"):
        return value in (1, "1")
    _usage("emit_recipe: %s must be 0 or 1 (got '%s')" % (flag, value))


def _floor(flag, value):
    if isinstance(value, int) and not isinstance(value, bool) and value >= 0:
        return value
    if isinstance(value, str) and re.fullmatch(r"[0-9]+", value):
        return int(value)
    _usage("emit_recipe: %s must be a non-negative integer (got '%s')" % (flag, value))


def _argv_list(flag, value, allow_empty=True):
    if isinstance(value, (list, tuple)) and all(isinstance(v, str) and v for v in value):
        if value or allow_empty:
            return list(value)
    _usage("emit_recipe: %s must be a list of non-empty strings (got %r)" % (flag, value))


RecipeResult = collections.namedtuple(
    "RecipeResult", ["summary", "tus", "defines", "includes", "drops", "recipe_file"])


def emit_recipe(*, build_dir=None, make_target=None, recipe_file=None, out_tus=None,
                out_defines=None, out_includes=None, make_vars=(), search_roots=(),
                prereq_mode="whole-blob", always_make=False, token_scope="all", archive=None,
                archive_from_span=False, min_tus=0, min_defines=0, make_argv=("make",),
                err=None):
    """Derive ONE make target's recipe into three files, in the bash twin's order:
      1. `[*make_argv, "-n", ("-B"), "-o", "Makefile", make_target, *make_vars]` with cwd =
         the build dir (`-o Makefile`: a dry run never REMAKES the makefile), stdout
         and stderr (merged, raw bytes) into `recipe_file`; its exit code DELIBERATELY ignored --
         `make -n` legitimately exits non-zero on a tree whose bootstrap it would have had to
         run, and the text it printed is still what we came for; a SHORT parse is what must
         never pass, and the floors catch that on the OUTPUT;
      2. join the continuations, take the span (`NoLinkLine` when link-line mode finds none);
      3. the token lines (`all` = the whole recipe, `recipe` = compile lines + the link line);
         write the defines and includes files;
      4. a NAMED archive that does not exist is refused;
      5. the object filter (`archive_from_span`: the basenames of the span's `.o` tokens);
      6. `<recipe>.drops` truncated; TUs = span TUs + archive TUs, basename-deduplicated,
         sorted, written to `out_tus`, the drop ledger written beside the recipe;
      7. the build-host tool guard; 8. lost archive members; 9. the TU floor, then the define
         floor; 10. the summary `T: N TUs, N defines, N -I dirs (mode M)` (+ the unresolved-token
         count when there is one).
    `always_make` adds `-B`: without it `make -n` prints only what REMAINS to be done, so the
    recipe depended on the state of a build dir nobody thought of as an input (a warm tree
    printed ONE line, whose -D set lacked `SQLITE_CORE`). Refusals raise `RecipeRefused`
    (`NoLinkLine` included); malformed arguments raise `HarnessUsageError` before make runs."""
    given = dict(build_dir=build_dir, make_target=make_target, recipe_file=recipe_file,
                 out_tus=out_tus, out_defines=out_defines, out_includes=out_includes)
    for key in REQUIRED_ARGS:
        if not given[key]:
            _usage("emit_recipe: %s is required" % key)
    if prereq_mode not in PREREQ_MODES:
        _usage("emit_recipe: unknown prereq_mode '%s' (want whole-blob|link-line)" % prereq_mode)
    if token_scope not in TOKEN_SCOPES:
        _usage("emit_recipe: unknown token_scope '%s' (want all|recipe)" % token_scope)
    always_make = _switch("always_make", always_make)
    archive_from_span = _switch("archive_from_span", archive_from_span)
    min_tus = _floor("min_tus", min_tus)
    min_defines = _floor("min_defines", min_defines)
    make_vars = _argv_list("make_vars", make_vars)
    search_roots = _argv_list("search_roots", search_roots)
    make_argv = _argv_list("make_argv", make_argv, allow_empty=False)
    target = make_target

    if not os.path.isdir(build_dir):
        _refuse("recipe derivation for '%s' cannot run: the build dir '%s' is not a directory."
                % (target, build_dir))

    # ── 1. make -n ──
    # `-o Makefile`: a dry run must never REMAKE the makefile (see the module note).
    argv = (make_argv + ["-n"] + (["-B"] if always_make else []) + ["-o", "Makefile"]
            + [target] + make_vars)
    try:
        fh = open(recipe_file, "wb")
    except OSError as exc:
        _refuse("cannot write the recipe file '%s': %s" % (recipe_file, exc))
    with fh:
        try:
            subprocess.run(argv, cwd=build_dir, stdin=subprocess.DEVNULL, stdout=fh,
                           stderr=subprocess.STDOUT, env=C.child_env())
        except OSError as exc:
            why = ("recipe derivation for '%s' could not start make: %s: %s"
                   % (target, argv[0], exc))
            fh.write(("%s%s\n" % (PREFIX, why)).encode("utf-8"))
            _refuse(why)

    # ── 2. join + span ──
    lines = join_continuations(_read_text(recipe_file))
    try:
        span = recipe_span(lines, prereq_mode, target)
    except NoLinkLine as exc:
        hint = ("" if always_make else
                "\n%smake -n prints only what REMAINS to be done: a target that is already "
                "built prints no link line unless always_make is set." % PREFIX)
        raise NoLinkLine("%s See %s%s" % (exc, recipe_file, hint)) from None

    # ── 3. where the -D / -I tokens come from ──
    tokens = lines if token_scope == "all" else recipe_token_span(lines, target)
    defines = recipe_defines(tokens)
    includes = recipe_includes(tokens)
    _write_out(out_defines, defines, "out_defines")
    _write_out(out_includes, includes, "out_includes")

    # ── 4. a NAMED archive that is not there is a named failure, never a quiet zero ──
    if archive and not os.path.isfile(archive):
        _refuse("recipe derivation for '%s' was told to recover TUs from the archive '%s', "
                "which does NOT exist." % (target, archive),
                "Nothing was silently substituted for it: the core sources compiled into that "
                "archive are the bulk of this target.")

    # ── 5. the object filter: in link-line mode, recover ONLY the `.o` the link named ──
    filter_names = None
    if archive_from_span:
        filter_names = sorted({os.path.basename(tok) for line in span
                               for tok in line.split(" ") if tok.endswith(".o")})

    # ── 6. the TUs, through the drop ledger ──
    drops_path = recipe_file + ".drops"
    ledger = DropLedger(stream=err)
    _write_out(drops_path, [], "drop ledger")
    try:
        collected = span_tus(span, build_dir, ledger)
        if archive:
            for root in search_roots:
                if not os.path.isdir(root):
                    _refuse("recipe derivation for '%s': the search root '%s' is not a "
                            "directory." % (target, root))
            collected += archive_tus(archive, filter_names, search_roots, ledger)
        tus = dedup_by_basename(collected, ledger=ledger)
    finally:
        try:
            ledger.write(drops_path)
        except OSError as exc:
            _refuse("cannot write the drop ledger '%s': %s" % (drops_path, exc))
    _write_out(out_tus, tus, "out_tus")

    # ── 7. THE BUILD-HOST TOOL GUARD -- a NAMED cause, ahead of the floors ──
    host_tools = [tu for tu in tus if _is_build_host_tool(tu, build_dir)]
    if host_tools:
        _refuse("recipe derivation for '%s' harvested BUILD-HOST TOOL sources as target TUs:"
                % target,
                *["      " + tu for tu in host_tools],
                "Those are programs upstream builds to RUN ON THIS HOST (lemon/mkkeywordhash/"
                "mksourceid),",
                "or lemon's parser TEMPLATE (lempar.c), which is not standalone C. Cross-"
                "compiling any of",
                "them into a target artifact cannot work, and it fails EVERY leg at once with "
                "front-end",
                "diagnostics that point at the tool's source rather than at this derivation.",
                "CAUSE: 'make -n' prints WHAT REMAINS TO BE DONE, so a build dir whose bootstrap "
                "is not",
                "current puts those compile lines INTO the recipe %s the build directory is an "
                "input." % DASH,
                "REMEDY: derive with prereq_mode='link-line' and always_make set (deterministic, "
                "and the",
                "bootstrap is excluded structurally), or bring '%s' fully up to date before "
                "deriving." % build_dir)

    # ── 8. a LOST archive member is a named cause, ahead of the floors ──
    n_lost = ledger.count("archive-member")
    if n_lost:
        _refuse("recipe derivation for '%s' LOST %d archive member(s): the object is in '%s' but "
                "no matching .c exists under any of search_roots." % (target, n_lost, archive),
                "Each one is a translation unit that would silently NOT be compiled, producing a "
                "smaller program that fails much later at link. See %s" % drops_path)

    # ── 9. the floors: a recipe parse that breaks yields a SHORT list, not an error ──
    if len(tus) < min_tus:
        _refuse("recipe derivation for '%s' yielded only %d TUs (<%d) %s the recipe parse broke; "
                "see %s" % (target, len(tus), min_tus, DASH, recipe_file))
    if len(defines) < min_defines:
        _refuse("recipe derivation for '%s' yielded only %d defines (<%d) %s the recipe parse "
                "broke; see %s" % (target, len(defines), min_defines, DASH, recipe_file))

    # ── 10. the summary; the unresolved-token count rides on it, pass or fail ──
    n_unresolved = ledger.count("recipe-token")
    note = ""
    if n_unresolved:
        note = (" %s ★ %d .c token(s) in the recipe resolved to NO file (each named on stderr; "
                "see %s)" % (DASH, n_unresolved, drops_path))
    summary = "%s: %d TUs, %d defines, %d -I dirs (mode %s)%s" % (
        target, len(tus), len(defines), len(includes), prereq_mode, note)
    return RecipeResult(summary, tus, defines, includes, list(ledger.entries), recipe_file)


# ── WHAT DID THE COMPILER ACTUALLY WRITE? ASK IT, DO NOT GUESS ───────────────────────
#
# A driver that ASSEMBLED an artefact's name assumed one platform's spelling: DSS names a PE
# executable `<name>.exe`, and a POSIX host cross-building the pe64 leg once recorded a real,
# error-free PE32+ executable as a build FAILURE. `TargetSpec::outputExtension` owns that table
# and the compiler REPORTS every artefact it commits (`ARTIFACT_MARKER`). Two DIFFERENT paths
# for one spec in one log is an ERROR, never "last wins": that rule would hand a caller its
# SIBLING's binary the moment a second artefact shares a log. Each artefact gets its OWN log
# and its OWN --output dir; this only has to notice when that structure is broken.

ReportedArtifact = collections.namedtuple("ReportedArtifact", ["code", "path", "reported", "error"])


def _marker(spec):
    if not isinstance(spec, str) or not spec or spec != spec.strip() or len(spec.split()) != 1:
        raise HarnessUsageError("%sa target spec is ONE token with no whitespace (got %r)"
                                % (PREFIX, spec))
    return ARTIFACT_MARKER + spec + " "


def reported_artifacts(text, spec):
    """Every DISTINCT path reported for `spec` in `text`, in order: the lines that START with
    `dsscp: artifact <spec> ` once the line end (CR included) is removed."""
    marker = _marker(spec)
    seen = []
    for line in _lines(text):
        if line.startswith(marker):
            path = line[len(marker):]
            if path and marker not in path and path not in seen:
                seen.append(path)
    return seen


def _read_report(lines, spec, log):
    marker = _marker(spec)
    reported, unreadable = [], []
    for n, line in enumerate(lines, 1):
        if line.startswith(marker):
            path = line[len(marker):]
            if not path:
                unreadable.append((n, "a report naming an EMPTY path", line))
            elif marker in path:
                unreadable.append((n, "a report with the marker AGAIN inside its path", line))
            elif path not in reported:
                reported.append(path)
        elif marker in line:
            unreadable.append((n, "the marker in MID-line, which is not a report", line))
    odd = ""
    if unreadable:
        odd = ("\n      %d line(s) carry the marker without being a report (a report STARTS a "
               "line and names a path):\n" % len(unreadable)
               + "\n".join("        line %d, %s: %s" % u for u in unreadable))
    if not reported:
        return ReportedArtifact(
            1, None, [],
            "the build reported NO artefact for %s (expected a '%s%s <path>' line in %s)%s"
            % (spec, ARTIFACT_MARKER, spec, log, odd))
    if len(reported) > 1 or unreadable:
        head = ("the build log '%s' reports %d DIFFERENT artefacts for the target spec '%s':"
                % (log, len(reported), spec) if len(reported) > 1 else
                "the build log '%s' reports ONE artefact for the target spec '%s' and ALSO lines "
                "that carry its marker without being a report:" % (log, spec))
        return ReportedArtifact(
            2, None, list(reported),
            head + "\n      " + "\n      ".join(reported) + odd +
            "\n      Refusing to guess which one was meant. Each artefact must be built into "
            "its OWN\n      --output directory with its OWN compile log, so its path is ASKED of "
            "the compiler,\n      never guessed from one platform's spelling.")
    return ReportedArtifact(0, os.path.normpath(os.path.abspath(reported[0])), list(reported), None)


def reported_artifact_in(text, spec, where):
    """`reported_artifact`'s judgement of build output already IN MEMORY (`where` names it in
    the refusal). ★ THE ONE READING RULE of the line dsscp prints for an artefact: the
    benchmark's measurement core reads its builds' output through this, and the leg resolver's
    reader (`harness_legs.dss_reported_artifacts`) through `reported_artifacts` -- until
    2026-09-22 each program read the marker by its own rule (stripped lines; anywhere in a
    line), so the same log could name an artefact to one reader and not to another."""
    return _read_report(_lines(text or ""), spec, where)


def reported_artifact(log_path, spec):
    """THE path the build logged at `log_path` produced for `spec` ->
    ReportedArtifact(code, path, reported, error): code 0 exactly one artefact (`path` =
    abspath + normpath of it, never realpath), 1 none, 2 more than one DISTINCT claim. Absence
    is a real answer (code 1), never a crash."""
    marker = _marker(spec)
    try:
        text = _read_text(log_path)
    except FileNotFoundError:
        return ReportedArtifact(
            1, None, [], "the build reported NO artefact for %s (expected a '%s<path>' line in %s, "
            "which does not exist)" % (spec, marker, log_path))
    except OSError as exc:
        return ReportedArtifact(1, None, [], "the compile log %s could not be read: %s"
                                % (log_path, exc))
    return _read_report(_lines(text), spec, log_path)


# ── BUILD ONE ARTIFACT ───────────────────────────────────────────────────────────────

def generate_manifest(gen, output, artifact_name, spec, tus_file, includes_file, defines_file,
                      recipe_transform, stack_reserve, lib_argv=(), python=sys.executable,
                      tu_preludes=""):
    """The ONE manifest generator both artefacts share, called in ONE argument order:
    `<python> <gen> --tus T --includes I --defines D --target S <lib argv...> --artifact-name N
    --recipe-transform X --stack-reserve R [--tu-preludes P] --output O`. The library argv passes
    through as TOKENS, never re-spelled (a resolved library may carry `<path>=<import-name>`, a
    vocabulary this module must not know). The transform and the reserve are REQUIRED: an omitted
    flag takes the generator's pe64 defaults. `tu_preludes` is the path of a leg's declared
    `build.tuPreludes` written as JSON, or "" for a leg that declares none -- whose argv is then
    the one it always had, byte for byte. -> `Result(rc, out, err)`, the generator's output
    merged into `out`; `manifest_error(result)` states a failure."""
    named = (("gen", gen), ("output", output), ("artifact_name", artifact_name),
             ("tus_file", tus_file), ("includes_file", includes_file),
             ("defines_file", defines_file), ("python", python))
    for name, value in named:
        if not value:
            _usage("generate_manifest: %s is required" % name)
    _marker(spec)
    if recipe_transform is None or str(recipe_transform) == "":
        _usage("generate_manifest: the recipe transform must be passed EXPLICITLY (an omitted "
               "--recipe-transform takes the generator's pe64 default)")
    if stack_reserve is None or str(stack_reserve) == "":
        _usage("generate_manifest: the stack reserve must be passed EXPLICITLY (an omitted "
               "--stack-reserve takes the generator's pe64 default)")
    if not re.fullmatch(r"[0-9]+", str(stack_reserve)) or isinstance(stack_reserve, bool):
        _usage("generate_manifest: the stack reserve must be a non-negative integer (got %r)"
               % (stack_reserve,))
    argv = [python, gen, "--tus", tus_file, "--includes", includes_file,
            "--defines", defines_file, "--target", spec]
    argv += [str(a) for a in lib_argv]
    argv += ["--artifact-name", artifact_name, "--recipe-transform", str(recipe_transform),
             "--stack-reserve", str(stack_reserve)]
    if tu_preludes:
        argv += ["--tu-preludes", tu_preludes]
    argv += ["--output", output]
    return C.capture(argv, env_=C.child_env(python=True), merge=True)


def manifest_error(result):
    """None for a generator that exited 0, else `manifest generation failed: <its output>`."""
    if result.rc == 0:
        return None
    parts = [line.strip() for line in ((result.out or "") + "\n" + (result.err or "")).split("\n")
             if line.strip()]
    return "manifest generation failed: %s" % (" / ".join(parts) or "(rc %d, no output)" % result.rc)


BuildResult = collections.namedtuple(
    "BuildResult",
    ["code", "ok", "path", "err_count", "first_errors", "time_suffix", "error", "log", "exit_code"])

_CTIME = re.compile(r"compile time \S+")

# ★ THE WHOLE STREAM, FOR EVERY BUILD. dsscp caps its diagnostics RUN-WIDE (50 per code, 1000 in all:
# DiagnosticReporter::Config; each CU's diagnostics are copied into the run's reporter), so a code raised
# in many TUs hides the LAST TUs whole -- and every caller here reads the log PER TU: Step 7's build
# attribution charges each rejected TU, and the round-close recompile counts each accepted one. A count
# no real build approaches, passed as BOTH caps. The readers still NAME a stream whose cap fired anyway
# (`harness_legs.dss_stream_gaps`), so this is the request, not the proof.
DIAGNOSTIC_CAP = 1000000


def compile_time_suffix(text):
    """`"  (compile time X)"` from the LAST `compile time X` in `text`, or ""."""
    hits = _CTIME.findall(text or "")
    return "  (%s)" % hits[-1] if hits else ""


def build_artifact(dss, manifest, config, outdir, log, spec):
    """Run `<dss> --project M --config=C --output D --time --max-diagnostics N --max-per-code N`
    (N = `DIAGNOSTIC_CAP`: the WHOLE diagnostic stream, for every build), its stdout and stderr written to
    `log` as RAW BYTES, and judge the LOG -> BuildResult(code, ok, path, err_count, first_errors,
    time_suffix, error, log, exit_code):
      0 built · 1 no artefact reported · 2 more than one (see `reported_artifact`) ·
      3 `error[` diagnostics (counted by line, the first three kept) · 4 an artefact was REPORTED
      and is not a regular FILE on disk · `LAUNCH_FAILED` the compiler could not be started.
    The four failures have four different remedies, so they stay apart. `dss` is one executable
    path or an argv prefix. The compiler's exit code is RECORDED, never judged: dsscp exits 0 on
    some fatal errors, so the log is the verdict. Whether the file is EXECUTABLE is not asked
    here -- that is not target-agnostic (a static library leg's artefact is not); the caller
    that intends to exec it asks."""
    prefix = [dss] if isinstance(dss, str) else list(dss or ())
    if not prefix or not all(isinstance(a, str) and a for a in prefix):
        _usage("build_artifact: the compiler (a path or an argv prefix) is required")
    for name, value in (("manifest", manifest), ("config", config), ("outdir", outdir),
                        ("log", log)):
        if not value:
            _usage("build_artifact: %s is required" % name)
    _marker(spec)
    argv = prefix + ["--project", manifest, "--config=%s" % config, "--output", outdir, "--time",
                     "--max-diagnostics", str(DIAGNOSTIC_CAP), "--max-per-code", str(DIAGNOSTIC_CAP)]
    try:
        fh = open(log, "wb")
    except OSError as exc:
        return BuildResult(LAUNCH_FAILED, False, None, 0, [], "",
                           "could not open the compile log '%s' for writing (%s) %s the compiler "
                           "was NOT started" % (log, exc, DASH), log, None)
    with fh:
        try:
            p = subprocess.run(argv, stdin=subprocess.DEVNULL, stdout=fh,
                               stderr=subprocess.STDOUT, env=C.child_env())
        except OSError as exc:
            why = "could not start the compiler '%s': %s" % (prefix[0], exc)
            fh.write(("%s%s\n" % (PREFIX, why)).encode("utf-8"))
            return BuildResult(LAUNCH_FAILED, False, None, 0, [], "", why, log, None)
    text = _read_text(log)
    lines = _lines(text)
    tsfx = compile_time_suffix(text)
    errs = [line for line in lines if "error[" in line]
    if errs:
        return BuildResult(3, False, None, len(errs), errs[:3], tsfx,
                           "%d error[...] diagnostic(s)" % len(errs), log, p.returncode)
    rep = _read_report(lines, spec, log)
    if rep.code != 0:
        return BuildResult(rep.code, False, None, 0, [], tsfx, "0 error[...] and " + rep.error,
                           log, p.returncode)
    if not os.path.isfile(rep.path):
        what = "is not a regular file" if os.path.exists(rep.path) else "is not there"
        return BuildResult(4, False, rep.path, 0, [], tsfx,
                           "0 error[...] but the artefact the build REPORTED %s: %s"
                           % (what, rep.path), log, p.returncode)
    return BuildResult(0, True, rep.path, 0, [], tsfx, None, log, p.returncode)


# ── ARTIFACT VERDICT LEDGER ──────────────────────────────────────────────────────────
#
# One artefact on one leg gets ONE verdict, and the report must be able to prove that every
# (leg, artifact) pair it declared reached one: a ledger keyed only by leg cannot say "the
# fixture built and the CLI did not", and a ledger nobody filled in must never read as clean.

class VerdictLedger:
    def __init__(self):
        self._v = {}

    def set(self, leg, artifact, verdict, detail):
        self._v[(leg, artifact)] = (verdict, detail)

    def get(self, leg, artifact):
        """-> (verdict, detail), or None when nothing was recorded."""
        return self._v.get((leg, artifact))

    def missing(self, artifact, legs):
        """The legs (in the order given) with no verdict for `artifact`; an EMPTY verdict is no
        verdict."""
        return [leg for leg in legs if not (self._v.get((leg, artifact)) or ("",))[0]]

    def assert_complete(self, artifact, legs):
        """[] when every leg has a verdict for `artifact`; else `LedgerHole` naming the legs."""
        hole = self.missing(artifact, legs)
        if hole:
            raise LedgerHole(artifact, hole)
        return []


# ── CLI ──────────────────────────────────────────────────────────────────────────────

USAGE = """usage:
  python sqlite_base.py --self-test          (alias --selftest)
exit: 0 every arm passed · 1 an arm failed · 2 usage"""


def cli_action(args):
    """-> ("self-test",) | ("help",) | ("usage", message)."""
    if args and args[0] in ("--self-test", "--selftest"):
        return ("self-test",) if len(args) == 1 else (
            "usage", "%s%s takes no further arguments" % (PREFIX, args[0]))
    if args and args[0] in ("-h", "--help"):
        return ("help",)
    return ("usage", "sqlite_base.py is a LIBRARY: its one CLI verb is --self-test (every caller "
                     "imports emit_recipe).")


def main(argv=None):
    act = cli_action(list(sys.argv[1:] if argv is None else argv))
    if act[0] == "self-test":
        return self_test()
    if act[0] == "help":
        print(USAGE)
        return 0
    print(act[1], file=sys.stderr)
    print(USAGE, file=sys.stderr)
    return 2


# ── SELF-TEST -- red-on-disable, by construction ─────────────────────────────────────
#
# Every arm builds a fixture that DIFFERS FROM A PASSING ONE IN ONE WAY and demands a different
# answer; an arm that asserts an ABSENCE also proves, beside it, that the thing can be present.
# Labels: `shNN` = the bash twin's 41 checks, `psNN` = the PowerShell twin's 23 (their labels
# kept), `nNN` = new. Every external program is a real CHILD PROCESS: a fake make / dsscp / ar /
# generator is a Python script run by this interpreter; the REAL `make` and `ar` run where they
# exist (a named SKIP, counted, where they do not).

EXPECTED_ARMS = 138        # 41 sh + 23 ps + 74 new

_SKIP = object()

# ONE stand-in for both `make` and `dsscp`: it writes the bytes of --say to stdout (raw),
# --stderr to stderr, records the argv it was given AFTER its own flags and its cwd, and exits
# --exit. Either tool is judged by what it printed, which is exactly what this controls.
_FAKE_TOOL = r'''import json, os, sys
a = sys.argv[1:]
say = rec = errtxt = None
code = 0
while a and a[0] in ("--say", "--record", "--exit", "--stderr"):
    k, v, a = a[0], a[1], a[2:]
    if k == "--say": say = v
    elif k == "--record": rec = v
    elif k == "--exit": code = int(v)
    else: errtxt = v
if rec:
    with open(rec, "w", encoding="utf-8", newline="\n") as fh:
        json.dump({"argv": a, "cwd": os.getcwd()}, fh)
if errtxt:
    sys.stderr.write(errtxt + "\n")
    sys.stderr.flush()
if say:
    with open(say, "rb") as fh:
        sys.stdout.buffer.write(fh.read())
    sys.stdout.flush()
sys.exit(code)
'''

_ECHO_GEN = r'''import json, sys
sys.stdout.write(json.dumps(sys.argv[1:]) + "\n")
'''

_FAIL_GEN = r'''import sys
sys.stdout.write("gen: this manifest is refused\n")
sys.stdout.write("gen: second line of the reason\n")
sys.exit(4)
'''


def _put(path, data):
    parent = os.path.dirname(path)
    if parent and not os.path.isdir(parent):
        os.makedirs(parent)
    with open(path, "wb") as fh:
        fh.write(data if isinstance(data, bytes) else data.encode("utf-8"))


def _get(path):
    with open(path, "rb") as fh:
        return fh.read()


def _norm(p):
    return os.path.normpath(os.path.abspath(p))


def _raised(fn, exc_type):
    """The `exc_type` instance `fn()` raised, or None when it returned (another exception
    propagates)."""
    try:
        fn()
    except exc_type as exc:
        return exc
    return None


def _ar_member(field, body):
    """One archive member, byte by byte: the 60-byte header -- name 16, mtime 12, uid 6, gid 6,
    mode 8, size 10, the magic `\\x60\\n` -- then the body, padded to an even offset."""
    hdr = (field.ljust(16) + "0".ljust(12) + "0".ljust(6) + "0".ljust(6) + "644".ljust(8)
           + str(len(body)).ljust(10)).encode("ascii") + b"\x60\n"
    return [hdr, body] + ([b"\n"] if len(body) % 2 else [])


def _write_gnu_ar(path, members, index=False):
    """A GNU-format archive, so no fixture depends on the host `ar` to exist: `name/` in the
    header; a name too long for it (16 bytes with its `/`) in the `//` table as `/N`; and, with
    `index`, a `/` symbol index first (an empty one: four zero bytes)."""
    table, fields = b"", []
    for name, _data in members:
        if len(name) + 1 > 16:
            fields.append("/%d" % len(table))
            table += (name + "/\n").encode("utf-8")
        else:
            fields.append(name + "/")
    out = [b"!<arch>\n"]
    if index:
        out += _ar_member("/", b"\0\0\0\0")
    if table:
        out += _ar_member("//", table)
    for field, (_name, data) in zip(fields, members):
        out += _ar_member(field, data)
    _put(path, b"".join(out))


def _write_bsd_ar(path, members, index=False):
    """A BSD-format archive, the layout Apple's tools write: a space-padded name with NO
    terminator; a name longer than 16 bytes, or holding a space, as `#1/N` with the name in the
    first N bytes of the body, NUL-padded to a multiple of four; and, with `index`, a
    `__.SYMDEF SORTED` symbol index first (a name with a space, so it too goes through `#1/`)."""
    def member(name, data):
        if len(name) > 16 or " " in name:
            raw = name.encode("utf-8")
            raw += b"\0" * (-len(raw) % 4)
            return _ar_member("#1/%d" % len(raw), raw + data)
        return _ar_member(name, data)
    out = [b"!<arch>\n"]
    if index:
        out += member("__.SYMDEF SORTED", b"\0" * 8)
    for name, data in members:
        out += member(name, data)
    _put(path, b"".join(out))


def _ar_headers(path, limit=24):
    """The raw header fields of an archive -- the byte, the name field, the size field -- read
    WITHOUT interpretation, for a failing arm to print what the file actually holds."""
    try:
        data = _get(path)
    except OSError as exc:
        return ["  (cannot read %s: %s)" % (path, exc)]
    lines = ["  %d byte(s); begins %r" % (len(data), data[:8])]
    pos = 8
    while pos + 60 <= len(data) and len(lines) <= limit:
        head = data[pos:pos + 60]
        size_text = head[48:58].decode("latin-1").strip()
        lines.append("  byte %d: name field %r, size field %r, magic %r"
                     % (pos, head[0:16].decode("latin-1"), size_text, head[58:60]))
        if not size_text.isdigit():
            break
        pos += 60 + int(size_text) + (int(size_text) & 1)
    return lines


class _Arms:
    def __init__(self):
        self.passed = self.failed = self.skipped = self.ran = 0
        self.labels = set()

    def arm(self, label, fn):
        """One counted arm: `fn()` returns a bool, (bool, detail), or (_SKIP, why)."""
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

    def section(self, name, fn, fx):
        try:
            fn(self, fx)
        except Exception:
            self.failed += 1
            print("  [FAIL] section %s CRASHED (its remaining arms did not run):" % name)
            for line in traceback.format_exc().rstrip().split("\n"):
                print("         %s" % line)

    def finish(self, expected):
        if self.ran != expected:
            self.failed += 1
            print("  [FAIL] ran %d arm(s), but EXPECTED_ARMS declares %d %s an arm was added, "
                  "removed or never reached" % (self.ran, expected, DASH))
        print("\npassed=%d failed=%d skipped=%d" % (self.passed, self.failed, self.skipped))
        return 0 if self.failed == 0 else 1


def _eq(want, got):
    return want == got, "want: %r\ngot : %r" % (want, got)


# ── WHAT A FAILING ARM PRINTS ABOUT THE HOST TOOL IT JUDGED ───────────────────────────────
# ✔MEASURED 2026-09-23: four arms failed on macOS and the gate received their NAMES only, from a
# host reachable through a runner alone. So an arm that judges a host tool's output prints, when
# it fails, what it EXPECTED and what it OBSERVED, then the tool itself: the argv, the exit code,
# the raw stdout and stderr (repr, so a trailing `/`, a CR or a space shows), which program
# answered to that name, and what that program says its version is.

def _verdict(ok, want, got, facts=lambda: ()):
    """(ok, detail) for `_Arms.arm`. The detail -- EXPECTED, OBSERVED, then `facts()` -- is built
    for a FAILED arm only, so a passing run never pays for probing the host tool."""
    if ok:
        return True, ""
    return False, "\n".join(["EXPECTED: %s" % want, "OBSERVED: %s" % got] + list(facts()))


def _raw(data, cap=1500):
    s = repr(data)
    return s if len(s) <= cap else s[:cap] + "...<%d more chars>" % (len(s) - cap)


def _run_facts(argv, timeout=60):
    """One run of `argv`, as report lines: the argv, its exit code, its raw stdout and stderr."""
    lines = ["  argv   : %r" % (list(argv),)]
    try:
        p = subprocess.run(list(argv), stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                           stderr=subprocess.PIPE, timeout=timeout)
    except (OSError, subprocess.TimeoutExpired) as exc:
        return lines + ["  could not run it: %s: %s" % (type(exc).__name__, exc)]
    return lines + ["  exit   : %d" % p.returncode, "  stdout : %s" % _raw(p.stdout),
                    "  stderr : %s" % _raw(p.stderr)]


def _tool_facts(name):
    """Which program answers to `name` here, and what it says its version is. A GNU tool answers
    `--version`; a BSD one may refuse it and answer `-V`, or answer neither. Both probes print
    their exit code and first lines, so no probe's silence passes for an answer."""
    path = name if os.path.isabs(name) else shutil.which(name)
    if not path:
        return ["  tool   : %r is not on this PATH" % name]
    real = os.path.realpath(path)
    lines = ["  tool   : %s%s" % (path, "" if real == path else " -> %s" % real)]
    for flag in ("--version", "-V"):
        probe = "`%s %s`" % (os.path.basename(path), flag)
        try:
            p = subprocess.run([path, flag], stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, timeout=30)
        except (OSError, subprocess.TimeoutExpired) as exc:
            lines.append("  version: %s could not run: %s" % (probe, exc))
            continue
        said = p.stdout.decode("utf-8", "replace").strip().splitlines()[:3]
        lines.append("  version: %s exit %d: %s" % (probe, p.returncode,
                                                    " | ".join(said) or "(nothing)"))
    return lines


def _archive_facts(archive):
    """What an archive arm judged: the reader's answer (or its refusal), the file's raw headers,
    and -- as a witness only, never as the answer -- what this host's `ar t` says of it."""
    try:
        said = "archive_members -> %r" % (archive_members(archive),)
    except RecipeRefused as exc:
        said = "archive_members REFUSED: %s" % exc
    lines = ["the archive %s:" % archive, "  " + said] + _ar_headers(archive)
    if shutil.which("ar"):
        lines += ["the host's `ar t`, for comparison:"] + _run_facts(["ar", "t", archive])
        lines += _tool_facts("ar")
    return lines


class _Fx:
    """The self-test's scratch world: one temp root, the fake programs, fresh subdirectories."""

    def __init__(self, root):
        self.root = root
        self.n = 0
        self.sink = io.StringIO()
        self.fake_tool = self._script("fake_tool.py", _FAKE_TOOL)
        self.echo_gen = self._script("echo_gen.py", _ECHO_GEN)
        self.fail_gen = self._script("fail_gen.py", _FAIL_GEN)
        self.host_ar = shutil.which("ar")
        self.host_make = shutil.which("make")

    def _script(self, name, src):
        path = os.path.join(self.root, name)
        _put(path, src)
        return path

    def fresh(self, tag):
        self.n += 1
        d = os.path.join(self.root, "%03d-%s" % (self.n, tag))
        os.makedirs(d)
        return d

    def _tool(self, say, exit_code, stderr, record):
        self.n += 1
        path = os.path.join(self.root, "say-%03d.txt" % self.n)
        _put(path, say)
        argv = [sys.executable, self.fake_tool, "--say", path, "--exit", str(exit_code)]
        if stderr:
            argv += ["--stderr", stderr]
        if record:
            argv += ["--record", record]
        return tuple(argv)

    def make(self, recipe, exit_code=0, stderr=None, record=None):
        """A `make_argv` whose `make -n` prints `recipe`."""
        return self._tool(recipe, exit_code, stderr, record)

    def dss(self, say, exit_code=0, stderr=None, record=None):
        """A compiler argv prefix whose run prints `say` (str or raw bytes)."""
        return self._tool(say, exit_code, stderr, record)

    def archive(self, tag, members):
        """A fresh GNU-format archive holding `members` (names; each body is the name's bytes)."""
        path = os.path.join(self.fresh(tag), "lib.a")
        _write_gnu_ar(path, [(m, m.encode("utf-8")) for m in members])
        return path

    def emit(self, build_dir, recipe, **kw):
        """emit_recipe over a fake make printing `recipe`; outputs in a fresh directory."""
        out = self.fresh("emit")
        args = dict(build_dir=build_dir, make_target="prog",
                    recipe_file=os.path.join(out, "recipe.txt"),
                    out_tus=os.path.join(out, "tus.txt"),
                    out_defines=os.path.join(out, "defines.txt"),
                    out_includes=os.path.join(out, "includes.txt"),
                    prereq_mode="link-line", min_tus=1, min_defines=0,
                    make_argv=self.make(recipe), err=self.sink)
        args.update(kw)
        return emit_recipe(**args), out

    def cli(self, args, cwd=None, timeout=120):
        return subprocess.run([sys.executable, THIS] + list(args), cwd=cwd,
                              stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE, env=C.child_env(python=True),
                              timeout=timeout)


def _st_join(A, fx):
    blob = join_continuations("cc -c a.c \\\n  -DFOO=1 \\\n  -Ione\ncc -o prog a.o \\\n  -Itwo\n")
    A.arm("sh01 recipe_blob joins continuations into 2 lines",
          lambda: _eq(2, len([line for line in blob if line])))
    A.arm("sh02 recipe_blob puts -DFOO=1 and -Ione on ONE line",
          lambda: _eq(1, sum(1 for line in blob if re.search(r"-DFOO=1.*-Ione", line))))
    A.arm("n01 a ONE-LINE recipe survives the join (with and without its final newline)",
          lambda: _eq([["cc -o prog one.c"]] * 2,
                      [join_continuations("cc -o prog one.c\n"),
                       join_continuations("cc -o prog one.c")]))

    def crlf():
        got = join_continuations("cc -c a.c \\" + CR + "\n  -DX" + CR + "\ncc -o prog a.o"
                                 + CR + "\n")
        return (got == ["cc -c a.c    -DX", "cc -o prog a.o"] and not any(CR in g for g in got),
                "got %r" % got)
    A.arm("n02 a CRLF recipe joins its continuations and leaves NO CR inside a token", crlf)
    A.arm("n03 a TAB-separated recipe line still matches the link line (tabs become spaces)",
          lambda: _eq(1, len(recipe_span(join_continuations("cc\t-o\tprog\tone.c\n"),
                                         "link-line", "prog"))))


def _st_defines(A, fx):
    blob = join_continuations('cc -c a.c -DFOO=1 -DQ="v" -Ione\ncc -o prog a.o -Itwo\n')
    A.arm("sh03 recipe_defines strips -D and make's literal quotes",
          lambda: _eq(["FOO=1", "Q=v"], recipe_defines(blob)))
    A.arm("sh04 recipe_includes drops the bare '.'",
          lambda: _eq(["one", "two"], recipe_includes(["cc -I. -Ione -Itwo"])))
    A.arm('n04 -DX="" becomes the EMPTY value X=',
          lambda: _eq(["X="], recipe_defines(['cc -DX="" -c a.c'])))
    A.arm("n05 defines and includes are sorted by CODE POINT (a locale sort interleaves cases)",
          lambda: (recipe_defines(["cc -Db -DB -D_u -DA"]) == ["A", "B", "_u", "b"]
                   and recipe_includes(["cc -Ib -IB"]) == ["B", "b"]
                   and sorted(["b", "B"], key=str.lower) != ["B", "b"],
                   "got %r / %r" % (recipe_defines(["cc -Db -DB -D_u -DA"]),
                                    recipe_includes(["cc -Ib -IB"]))))
    A.arm("n06 '-I dir' (one space) is an include dir too",
          lambda: _eq(["/x/inc"], recipe_includes(["cc -I /x/inc -c a.c"])))
    # The NEGATIVE the old pattern failed: `-D`/`-I` inside a path token. Control in the same
    # line: a quoted flag and a separated `-I dir` still count.
    inside = 'cc -c /tmp/C--Source-DailySoftware-Ideas/x.c "-DQUOTED=1" -DREAL=1 -I/inc -I bar'
    A.arm("n06b only a token that STARTS with -D/-I is a define/include: a path holding "
          "`-DailySoftware-Ideas` yields neither (control: a quoted -D and '-I dir' still count)",
          lambda: _eq((["QUOTED=1", "REAL=1"], ["/inc", "bar"]),
                      (recipe_defines([inside]), recipe_includes([inside]))))


def _st_span(A, fx):
    b2 = join_continuations("cc -o lemon /tool/lemon.c\ncc -o prog shell.c a.o b.o\n")
    A.arm("sh05 span whole-blob sees BOTH lines",
          lambda: _eq(2, len(recipe_span(b2, "whole-blob", "prog"))))
    A.arm("sh06 span link-line sees ONLY the link",
          lambda: _eq(1, len(recipe_span(b2, "link-line", "prog"))))
    A.arm("sh07 span link-line EXCLUDES the tool source (control: whole-blob carries it)",
          lambda: (not any("lemon.c" in line for line in recipe_span(b2, "link-line", "prog"))
                   and any("lemon.c" in line for line in recipe_span(b2, "whole-blob", "prog"))))
    b3 = join_continuations("cc -o prog x.c\ncc -o progd y.c\n")
    A.arm("sh08 span link-line 'prog' does NOT match 'progd' (control: 'progd' has its line)",
          lambda: (len(recipe_span(b3, "link-line", "prog")) == 1
                   and "y.c" in recipe_span(b3, "link-line", "progd")[0]))
    A.arm("sh09   … and it picked the RIGHT one",
          lambda: "x.c" in recipe_span(b3, "link-line", "prog")[0])
    A.arm("sh10 span rejects an unknown mode",
          lambda: _raised(lambda: recipe_span(b3, "nonsense", "prog"), HarnessUsageError)
          is not None)

    def no_link():
        exc = _raised(lambda: recipe_span(b3, "link-line", "nosuch"), NoLinkLine)
        return (exc is not None and isinstance(exc, RecipeRefused) and "'nosuch'" in str(exc)
                and "NO link line" in str(exc), "raised %r" % exc)
    A.arm("n07 no link line is a NAMED refusal (NoLinkLine, exit 1), not a silent rc 2", no_link)
    A.arm("n08 the target is a LITERAL (re.escape): 'a.b' does not match '-o axb' (control: an "
          "unescaped regex would)",
          lambda: (recipe_span(["cc -o axb x.c", "cc -o a.b y.c"], "link-line", "a.b")
                   == ["cc -o a.b y.c"] and re.search("-o a.b", "cc -o axb x.c") is not None))
    A.arm("n09 a link line whose '-o <target>' ENDS the line is matched",
          lambda: _eq(["cc x.o -o prog"], recipe_span(["cc x.o -o prog"], "link-line", "prog")))


def _st_token_span(A, fx):
    b6 = join_continuations("cc -DJIM_COMPAT -o jimsh /tool/jim.c\ncc -DCORE_ONLY -c a.c\n"
                            "cc -DLINK_ONLY -o prog shell.c a.o\n")
    tok = recipe_token_span(b6, "prog")
    A.arm("sh11 token_span takes the compile line AND the link line", lambda: _eq(2, len(tok)))
    A.arm("sh12 token_span keeps the COMPILE-only define (the SQLITE_CORE case)",
          lambda: "CORE_ONLY" in recipe_defines(tok))
    A.arm("sh13 token_span keeps the LINK-only define", lambda: "LINK_ONLY" in recipe_defines(tok))
    A.arm("sh14 token_span EXCLUDES the bootstrap's foreign define",
          lambda: ("JIM_COMPAT" not in recipe_defines(tok)
                   and "JIM_COMPAT" in recipe_defines(b6)))
    A.arm("sh15   (control) whole-blob DOES let it through",
          lambda: "JIM_COMPAT" in recipe_defines(b6))


def _st_dedup(A, fx):
    o1 = dedup_by_basename(["/z/dup.c", "/a/dup.c", "/a/uniq.c"])
    o2 = dedup_by_basename(["/a/uniq.c", "/a/dup.c", "/z/dup.c"])
    A.arm("sh16 dedup is ORDER-INDEPENDENT (the anchor's whole point)", lambda: _eq(o1, o2))
    A.arm("sh17 dedup keeps one path per basename", lambda: _eq(2, len(o1)))
    A.arm("sh18 dedup keeps the SORT-FIRST duplicate",
          lambda: "/a/dup.c" in o1 and "/z/dup.c" not in o1)
    A.arm("n10 dedup order is CODE POINT, not locale: '/B/x.c' beats '/a/x.c' (control: a "
          "case-folding order picks '/a/x.c')",
          lambda: (dedup_by_basename(["/a/x.c", "/B/x.c"]) == ["/B/x.c"]
                   and sorted(["/a/x.c", "/B/x.c"], key=str.lower)[0] == "/a/x.c"))

    def collisions():
        led, clean = DropLedger(stream=fx.sink), DropLedger(stream=fx.sink)
        dedup_by_basename(["/z/dup.c", "/a/dup.c"], ledger=led)
        dedup_by_basename(["/z/one.c", "/a/two.c"], ledger=clean)
        e = led.entries
        return (len(e) == 1 and e[0][0] == "basename-duplicate" and "/z/dup.c" in e[0][1]
                and not clean.entries, "entries %r / control %r" % (e, clean.entries))
    A.arm("n11 a path the dedup REMOVES is reported (basename-duplicate); control: no collision, "
          "no entry", collisions)


def _st_reader(A, fx):
    spec = "x86_64:elf64-x86_64-linux-exec"
    other = "arm64:elf64-aarch64-linux-exec"
    mk = ARTIFACT_MARKER + spec + " "
    d = fx.fresh("reader")
    log1 = os.path.join(d, "log1")
    _put(log1, "noise\n" + mk + "/out/a\nmore noise\n")
    A.arm("sh19 reported_artifact reads the path",
          lambda: _eq(_norm("/out/a"), reported_artifact(log1, spec).path))
    A.arm("ps01 reported_artifact reads the path",
          lambda: _eq(["/out/a"], reported_artifact(log1, spec).reported))
    A.arm("ps02   … and reports Ok", lambda: _eq(0, reported_artifact(log1, spec).code))
    logs = os.path.join(d, "logS")
    _put(logs, mk + "/out dir/a\n")
    A.arm("sh20 reported_artifact keeps a path containing a space",
          lambda: _eq(_norm("/out dir/a"), reported_artifact(logs, spec).path))
    A.arm("ps03 reported_artifact keeps a path containing a space",
          lambda: _eq(["/out dir/a"], reported_artifact(logs, spec).reported))
    log2 = os.path.join(d, "log2")
    _put(log2, mk + "/out/a\n" + mk + "/out/a\n")
    A.arm("sh21 a REPEATED identical report is still one artefact",
          lambda: _eq(_norm("/out/a"), reported_artifact(log2, spec).path))
    A.arm("ps04 a REPEATED identical report collapses to ONE artefact",
          lambda: _eq(1, len(reported_artifacts(_get(log2).decode("utf-8"), spec))))
    A.arm("ps05   … and is still Ok", lambda: _eq(0, reported_artifact(log2, spec).code))
    log3 = os.path.join(d, "log3")
    _put(log3, mk + "/out/a\n" + mk + "/out/b\n")
    amb = reported_artifact(log3, spec)
    A.arm("sh22 TWO DIFFERENT artefacts for one spec is REFUSED", lambda: _eq(2, amb.code))
    A.arm("sh23   … and the refusal NAMES both paths",
          lambda: ("/out/a" in (amb.error or "") and "/out/b" in (amb.error or ""), amb.error))
    A.arm("ps06 TWO DIFFERENT artefacts for one spec is REFUSED",
          lambda: amb.code != 0 and amb.path is None)
    A.arm("ps07   … and the refusal NAMES both paths",
          lambda: ("/out/a" in (amb.error or "") and "/out/b" in (amb.error or "")
                   and "Refusing to guess" in (amb.error or ""), amb.error))
    log4 = os.path.join(d, "log4")
    _put(log4, ARTIFACT_MARKER + other + " /out/other\n")
    A.arm("sh24 a DIFFERENT spec's artefact is not returned (control: its own spec reads it)",
          lambda: (reported_artifact(log4, spec).code == 1
                   and reported_artifact(log4, other).code == 0))
    A.arm("ps08 a DIFFERENT spec's artefact is not returned",
          lambda: (reported_artifact(log4, spec).path is None
                   and reported_artifact(log4, other).path == _norm("/out/other")))
    nosuch = os.path.join(d, "nosuchlog")
    A.arm("sh25 an absent log is rc 1, not a crash",
          lambda: _eq(1, reported_artifact(nosuch, spec).code))
    A.arm("ps09 an absent log is a verdict, not a crash",
          lambda: "NO artefact" in (reported_artifact(nosuch, spec).error or ""))

    def crlf():
        logc = os.path.join(d, "logCRLF")
        _put(logc, "noise" + CR + "\n" + mk + "/out/a" + CR + "\n")
        r = reported_artifact(logc, spec)
        return (CR.encode() in _get(logc) and r.code == 0 and r.path == _norm("/out/a")
                and not any(CR in p for p in r.reported), "got %r" % (r,))
    A.arm("n12 a CRLF log yields a path with NO CR (control: the fixture really holds CR)", crlf)

    def midline_alone():
        logm = os.path.join(d, "logMID")
        _put(logm, "[12:00] " + mk + "/out/a\n")
        loga = os.path.join(d, "logANCH")
        _put(loga, mk + "/out/a\n")
        r = reported_artifact(logm, spec)
        return (r.code == 1 and "MID-line" in (r.error or "") and "[12:00]" in (r.error or "")
                and reported_artifact(loga, spec).code == 0, "got %r" % (r,))
    A.arm("n13 a marker in MID-line is not an artefact and is REPORTED (control: the same line "
          "anchored is read)", midline_alone)

    def midline_beside():
        logb = os.path.join(d, "logBESIDE")
        _put(logb, mk + "/out/a\nprogress 50%" + mk + "/out/b\n")
        r = reported_artifact(logb, spec)
        return (r.code == 2 and r.path is None and "/out/b" in (r.error or ""), "got %r" % (r,))
    A.arm("n14 a mid-line marker BESIDE a report makes the log ambiguous (code 2), never "
          "silently used or ignored", midline_beside)

    def in_memory_same_rule():
        # Every shape above, judged from TEXT and from the FILE holding the same bytes: the one
        # rule must give one answer (code, path, claims) whichever way the log arrives.
        shapes = {"one": "noise\n" + mk + "/out/a\n", "none": "noise only\n",
                  "two": mk + "/out/a\n" + mk + "/out/b\n",
                  "midline-alone": "[12:00] " + mk + "/out/a\n",
                  "midline-beside": mk + "/out/a\nprogress " + mk + "/out/b\n",
                  "crlf": mk + "/out/a" + CR + "\n",
                  # BLANKS are content: an indented marker is not a report, and a path's
                  # trailing blanks are the path's (a private rule that stripped lines --
                  # the resolver's old reader -- answered both differently).
                  "indented": "   " + mk + "/out/a\n",
                  "trailing-blanks": mk + "/out/a  \n"}
        got = {}
        for name, text in shapes.items():
            path = os.path.join(d, "same-" + name)
            _put(path, text)
            m = reported_artifact_in(text, spec, path)
            f = reported_artifact(path, spec)
            got[name] = (m.code, m.path, m.reported) == (f.code, f.path, f.reported)
        order = ("one", "none", "two", "midline-alone", "midline-beside", "crlf", "indented",
                 "trailing-blanks")
        codes = [reported_artifact_in(shapes[k], spec, "t").code for k in order]
        kept = reported_artifact_in(shapes["trailing-blanks"], spec, "t").reported
        return (all(got.values()) and codes == [0, 1, 2, 1, 2, 0, 1, 0] and kept == ["/out/a  "],
                "same %r, codes %r, trailing-blank claim %r" % (got, codes, kept))
    A.arm("n14b reported_artifact_in judges TEXT exactly as reported_artifact judges the FILE "
          "holding the same bytes (one, none, two, mid-line alone/beside, CRLF, an indented "
          "marker, a path's trailing blanks)", in_memory_same_rule)

    def empty_path():
        loge = os.path.join(d, "logEMPTY")
        _put(loge, mk.rstrip(" ") + " \n")
        r = reported_artifact(loge, spec)
        return (r.code == 1 and "EMPTY path" in (r.error or "")
                and reported_artifact(log1, spec).code == 0, "got %r" % (r,))
    A.arm("n15 a report naming an EMPTY path is not an artefact, and says so (control: the same "
          "line with a path is read)", empty_path)

    def not_realpath():
        real = fx.fresh("realdir")
        link = os.path.join(fx.root, "linkdir-%03d" % fx.n)
        try:
            os.symlink(real, link, target_is_directory=True)
        except (OSError, NotImplementedError) as exc:
            return _SKIP, "this host cannot create a directory symlink here (%s)" % exc
        logl = os.path.join(d, "logLINK")
        _put(logl, mk + os.path.join(link, "a.bin") + "\n")
        r = reported_artifact(logl, spec)
        want = _norm(os.path.join(link, "a.bin"))
        return (r.path == want and os.path.realpath(want) != want,
                "path %r, realpath %r" % (r.path, os.path.realpath(want)))
    A.arm("n16 the path is abspath+normpath, NOT realpath (control: realpath differs)",
          not_realpath)
    A.arm("n17 an empty or whitespace-carrying spec is a USAGE error",
          lambda: (_raised(lambda: reported_artifact(log1, ""), HarnessUsageError) is not None
                   and _raised(lambda: reported_artifacts("x", "a b"), HarnessUsageError)
                   is not None))


def _st_ledger(A, fx):
    led = VerdictLedger()
    led.set("legA", "sqlite3", "built", "ok")
    A.arm("sh26 verdict round-trips", lambda: _eq("built", led.get("legA", "sqlite3")[0]))
    A.arm("ps10 verdict round-trips", lambda: _eq(("built", "ok"), led.get("legA", "sqlite3")))
    A.arm("sh27 verdicts are keyed per ARTIFACT, not per leg",
          lambda: (led.get("legA", "testfixture") is None
                   and led.get("legA", "sqlite3") is not None))
    A.arm("ps11 verdicts are keyed per ARTIFACT, not per leg",
          lambda: _eq(["legA"], led.missing("testfixture", ["legA"])))
    A.arm("sh28 assert_verdicts passes when every leg has one",
          lambda: _eq([], led.assert_complete("sqlite3", ["legA"])))
    A.arm("ps12 assert_verdicts passes when every leg has one",
          lambda: _eq([], led.missing("sqlite3", ["legA"])))
    A.arm("sh29 assert_verdicts FAILS on a leg with no verdict",
          lambda: _raised(lambda: led.assert_complete("sqlite3", ["legA", "legB"]), LedgerHole)
          is not None)

    def names():
        exc = _raised(lambda: led.assert_complete("sqlite3", ["legA", "legB"]), LedgerHole)
        return (led.missing("sqlite3", ["legA", "legB"]) == ["legB"] and exc is not None
                and exc.missing == ["legB"] and "legB" in str(exc), "raised %r" % exc)
    A.arm("ps13 assert_verdicts NAMES a leg with no verdict", names)

    def empty_verdict():
        led.set("legC", "sqlite3", "", "a detail with no verdict")
        led.set("legD", "sqlite3", "built", "a verdict")
        return _eq(["legC"], led.missing("sqlite3", ["legC", "legD"]))
    A.arm("n18 an EMPTY verdict is no verdict (the bash rule; the PowerShell ledger counted the "
          "key) (control: a non-empty one counts)", empty_verdict)

    def tuple_keys():
        led.set("a/b", "c", "built", "x")
        return led.get("a", "b/c") is None and led.get("a/b", "c") is not None
    A.arm("n19 keys are (leg, artifact) TUPLES: 'a/b'+'c' is not 'a'+'b/c' (control: its own key "
          "reads)", tuple_keys)


def _st_floors(A, fx):
    t = fx.fresh("floors")
    _put(os.path.join(t, "one.c"), "")
    A.arm("sh30 emit_recipe FAILS below the TU floor",
          lambda: ("yielded only 1 TUs (<50)" in str(_raised(
              lambda: fx.emit(t, "cc -o prog one.c\n", min_tus=50), RecipeRefused))))
    box = {}

    def at_floor():
        box["res"], box["out"] = fx.emit(t, "cc -o prog one.c\n", min_tus=1, min_defines=0)
        return True
    A.arm("sh31 emit_recipe PASSES at the floor (the control)", at_floor)
    A.arm("sh32   … and resolved the relative TU against the build dir",
          lambda: _eq(os.path.join(t, "one.c") + "\n",
                      _get(os.path.join(box["out"], "tus.txt")).decode("utf-8")))
    A.arm("sh33 emit_recipe rejects a missing required argument",
          lambda: _raised(lambda: emit_recipe(build_dir=t), HarnessUsageError) is not None)

    def real_names():
        full = dict(build_dir=t, make_target="prog", recipe_file="r", out_tus="a",
                    out_defines="b", out_includes="c")
        msgs = []
        for key in REQUIRED_ARGS:
            kw = dict(full)
            kw[key] = None
            msgs.append(str(_raised(lambda: emit_recipe(**kw), HarnessUsageError)))
        want = ["%s is required" % key for key in REQUIRED_ARGS]
        stale = ("--bld ", "--target ", "--recipe ", "--out-defs ", "--out-incs ")
        old_spelling = "base-harness: dss_bh_emit_recipe: --bld is required"
        return (all(w in m for w, m in zip(want, msgs))
                and not any(s in m for s in stale for m in msgs)
                and any(s in old_spelling for s in stale), "\n".join(msgs))
    A.arm("n20 every 'is required' message names the REAL parameter (5 of the bash twin's 6 did not) "
          "(control: the stale-name needles do match the old spelling)", real_names)

    def floors_usage():
        bad = [dict(min_tus="12x"), dict(min_tus=-1), dict(min_defines="1.5"), dict(min_tus=True)]
        refused = [_raised(lambda: fx.emit(t, "cc -o prog one.c\n", **kw), HarnessUsageError)
                   for kw in bad]
        res, _o = fx.emit(t, "cc -o prog one.c\n", min_tus="1")
        return (all(r is not None for r in refused) and len(res.tus) == 1,
                "refusals %r" % refused)
    A.arm("n21 a non-integer or negative floor is a USAGE error (control: '1' is accepted)",
          floors_usage)

    def switches_usage():
        bad = [dict(always_make="yes"), dict(archive_from_span=2), dict(always_make="true")]
        refused = [_raised(lambda: fx.emit(t, "cc -o prog one.c\n", **kw), HarnessUsageError)
                   for kw in bad]
        res, _o = fx.emit(t, "cc -o prog one.c\n", always_make="1", archive_from_span="0")
        return all(r is not None for r in refused) and len(res.tus) == 1, "refusals %r" % refused
    A.arm("n22 a switch that is not 0/1 is a USAGE error (control: '1'/'0' are accepted)",
          switches_usage)

    def before_make():
        out = fx.fresh("usage-before-make")
        recipe = os.path.join(out, "recipe.txt")
        e1 = _raised(lambda: fx.emit(t, "cc -o prog one.c\n", recipe_file=recipe,
                                     prereq_mode="bogus"), HarnessUsageError)
        refused_early = e1 is not None and not os.path.exists(recipe)
        fx.emit(t, "cc -o prog one.c\n", recipe_file=recipe)
        return (refused_early and "unknown prereq_mode 'bogus'" in str(e1)
                and os.path.exists(recipe), "raised %r" % e1)
    A.arm("n23 an unknown prereq_mode is refused BEFORE make runs (no recipe file written; "
          "control: a valid call writes it)", before_make)
    A.arm("n24 an unknown token_scope is a USAGE error",
          lambda: "unknown token_scope 'nope'" in str(_raised(
              lambda: fx.emit(t, "cc -o prog one.c\n", token_scope="nope"), HarnessUsageError)))


def _st_tool_guard(A, fx):
    t = fx.fresh("tool")
    _put(os.path.join(t, "one.c"), "")
    _put(os.path.join(t, "tool", "lemon.c"), "")
    _put(os.path.join(t, "lempar.c"), "")
    A.arm("sh34 emit_recipe REFUSES a tool/ build-host source as a target TU",
          lambda: "BUILD-HOST TOOL" in str(_raised(
              lambda: fx.emit(t, "cc -o prog tool/lemon.c one.c\n"), RecipeRefused)))
    A.arm("sh35 emit_recipe REFUSES lempar.c (lemon's template, not standalone C)",
          lambda: "BUILD-HOST TOOL" in str(_raised(
              lambda: fx.emit(t, "cc -o prog lempar.c one.c\n"), RecipeRefused)))
    A.arm("sh36 emit_recipe ACCEPTS an ordinary TU (the control)",
          lambda: _eq(1, len(fx.emit(t, "cc -o prog one.c\n")[0].tus)))

    def under_tool_parent():
        bld = os.path.join(fx.fresh("x"), "tool", "sqlite", "bld")
        _put(os.path.join(bld, "one.c"), "")
        res, _o = fx.emit(bld, "cc -o prog one.c\n")
        old_rule = re.search(r"(^|/)tool/", res.tus[0].replace("\\", "/")) is not None
        return len(res.tus) == 1 and old_rule, "tus %r, old rule matched: %s" % (res.tus, old_rule)
    A.arm("n25 the tool guard does NOT refuse a checkout that merely sits under a tool/ parent "
          "(control: the old (^|/)tool/ rule matches it)", under_tool_parent)

    def build_dir_named_tool():
        bld = os.path.join(fx.fresh("y"), "tool")
        _put(os.path.join(bld, "one.c"), "")
        res, _o = fx.emit(bld, "cc -o prog one.c\n")
        return _eq([os.path.join(bld, "one.c")], res.tus)
    A.arm("n26 a build dir that is itself NAMED tool is not a build-host tool directory",
          build_dir_named_tool)


def _st_drops(A, fx):
    t = fx.fresh("drops")
    real = os.path.join(t, "real.c")
    _put(real, "")
    led = DropLedger(stream=fx.sink)
    got = span_tus([real + " missing-tu.c"], t, led)
    A.arm("sh37 span_tus still emits the token it CAN resolve", lambda: _eq([real], got))
    A.arm("sh38 span_tus REPORTS the token it cannot resolve",
          lambda: _eq(1, sum(1 for k, m in led.entries
                             if k == "recipe-token" and "missing-tu.c" in m)))
    libx = os.path.join(t, "libx.a")
    _write_gnu_ar(libx, [("lost.o", b"\x00obj")])

    def lost_reported():
        led2 = DropLedger(stream=fx.sink)
        try:
            archive_tus(libx, None, [t], led2)
            refused = None
        except RecipeRefused as exc:
            refused = exc
        hits = [m for k, m in led2.entries if k == "archive-member" and "lost.o" in m]
        got = ("RecipeRefused: %s" % refused if refused is not None else
               "%d such entr%s; every entry %r" % (len(hits), "y" if len(hits) == 1 else "ies",
                                                  led2.entries))
        return _verdict(refused is None and len(hits) == 1,
                        "exactly 1 'archive-member' entry naming lost.o", got,
                        lambda: _archive_facts(libx))
    A.arm("sh39 archive_tus REPORTS a member whose .c is nowhere", lost_reported)

    def lost_refused():
        try:
            res, _o = fx.emit(t, "cc -o prog real.c\n", archive=libx, search_roots=[t])
            got = "no refusal: emit_recipe RETURNED %r, drops %r" % (res.summary, res.drops)
        except RecipeRefused as exc:
            got = "RecipeRefused: %s" % exc
        return _verdict("LOST 1 archive member(s)" in got and got.startswith("RecipeRefused"),
                        "RecipeRefused naming 'LOST 1 archive member(s)'", got,
                        lambda: _archive_facts(libx))
    A.arm("sh40 emit_recipe FAILS on a LOST archive member", lost_refused)
    A.arm("sh41 emit_recipe FAILS on an archive that does not exist",
          lambda: "which does NOT exist" in str(_raised(
              lambda: fx.emit(t, "cc -o prog real.c\n", archive=os.path.join(t, "no-such.a"),
                              search_roots=[t]), RecipeRefused)))

    def decoy():
        bld = fx.fresh("decoy-bld")
        here = fx.fresh("decoy-cwd")
        _put(os.path.join(here, "decoy.c"), "")
        led3 = DropLedger(stream=fx.sink)
        old = os.getcwd()
        try:
            os.chdir(here)
            control = os.path.isfile("decoy.c")
            tus = span_tus(["cc -o prog decoy.c"], bld, led3)
        finally:
            os.chdir(old)
        return (control and tus == [] and led3.count("recipe-token") == 1,
                "control %s, tus %r" % (control, tus))
    A.arm("n27 a decoy .c in the PROCESS cwd is never picked up (control: it IS there)", decoy)


def _st_archive(A, fx):
    t = fx.fresh("archive")
    lib = os.path.join(t, "lib3.a")
    _write_gnu_ar(lib, [("a.o", b"A"), ("b.o", b"BB"), ("c.o", b"C"), ("notes.txt", b"n")])

    def lists(path, want):
        try:
            got = archive_members(path)
        except RecipeRefused as exc:
            got = "RecipeRefused: %s" % exc
        return _verdict(got == want, repr(want), repr(got), lambda: _archive_facts(path))
    A.arm("n28 archive_members lists a GNU-format archive's members EXACTLY, read from its bytes "
          "on EVERY host (the GNU `/` name terminator is not part of a name)",
          lambda: lists(lib, ["a.o", "b.o", "c.o", "notes.txt"]))
    bsd = os.path.join(t, "bsd.a")
    bsd_names = ["a.o", "fts3_tokenizer1.o", "with space.o", "sqlite3session.o"]
    _write_bsd_ar(bsd, [(n, n.encode("utf-8")) for n in bsd_names], index=True)
    A.arm("n68 ...a BSD-format archive (Apple's layout: `#1/N` for a name past 16 bytes or with a "
          "space, a 16-byte name bare, a `__.SYMDEF SORTED` index) EXACTLY, the index left out",
          lambda: lists(bsd, bsd_names))
    gnu_long = os.path.join(t, "gnu-long.a")
    long_names = ["fts3_tokenizer1.o", "abcdefghijklm.o", "sqlite3session.o", "a.o"]
    _write_gnu_ar(gnu_long, [(n, n.encode("utf-8")) for n in long_names], index=True)
    A.arm("n69 ...a GNU-format archive with a `/` index and a `//` long-name table (and a 15-byte "
          "name that just fits its header) EXACTLY, neither table a member",
          lambda: lists(gnu_long, long_names))

    # ✔MEASURED 2026-09-23 on the Mac: `/usr/bin/ar rc` of files that are NOT Mach-O exits 0,
    # warns "ranlib: warning: archive member 'a.o' not a mach-o file", and writes an archive that
    # holds ONLY its `__.SYMDEF SORTED` index -- the members are gone. So the witness is built the
    # way production builds one: real objects, from this host's own compiler, archived by its ar.
    def host_written():
        cc = shutil.which("gcc")
        if not fx.host_ar or not cc:
            return _SKIP, "no %s on this PATH" % ("ar" if not fx.host_ar else "gcc")
        d = fx.fresh("host-ar")
        names = ["a.o", "fts3_tokenizer1.o", "sqlite3session.o"]
        steps = []
        for n in names:
            _put(os.path.join(d, n[:-2] + ".c"), "int %s_x = 1;\n" % n[:-2])
            steps.append([cc, "-c", n[:-2] + ".c", "-o", n])
        steps.append([fx.host_ar, "rc", "host.a"] + names)
        path = os.path.join(d, "host.a")
        ran = []
        for argv in steps:
            try:
                p = subprocess.run(argv, cwd=d, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE, timeout=120)
                ran.append((argv, p.returncode, "exit %d, stdout %s, stderr %s"
                            % (p.returncode, _raw(p.stdout), _raw(p.stderr))))
            except (OSError, subprocess.TimeoutExpired) as exc:
                ran.append((argv, None, "could not run it: %s" % exc))
            if ran[-1][1] != 0:
                break

        def facts():
            return (["in %s:" % d] + ["  %r: %s" % (argv, said) for argv, _rc, said in ran]
                    + _tool_facts(cc) + _tool_facts(fx.host_ar)
                    + (_archive_facts(path) if os.path.isfile(path) else []))
        if ran[-1][1] != 0 or len(ran) != len(steps):
            return _verdict(False, "this host's gcc and ar build the archive (every step exit 0)",
                            ran[-1][2], facts)
        try:
            got = archive_members(path)
        except RecipeRefused as exc:
            got = "RecipeRefused: %s" % exc
        return _verdict(got == names, repr(names), repr(got), facts)
    A.arm("n70 ...an archive THIS HOST'S ar WROTE from objects THIS HOST'S gcc compiled (its own "
          "layout, its own symbol index, long names included) EXACTLY, the index left out -- the one "
          "arm that needs host tools, as a WITNESS of the host's format", host_written)

    def refused_with(path, needle):
        exc = _raised(lambda: archive_members(path), RecipeRefused)
        return _verdict(exc is not None and needle in str(exc), "RecipeRefused naming %r" % needle,
                        "no refusal: %r" % (archive_members(path),) if exc is None else
                        "RecipeRefused: %s" % exc, lambda: _ar_headers(path))
    junk = os.path.join(t, "junk.a")
    _put(junk, "this is not an archive\n")
    A.arm("n29 a file that is not an archive is a named refusal, NOT an empty archive",
          lambda: refused_with(junk, "NOT an empty archive"))
    cut_head = os.path.join(t, "cut-head.a")
    _put(cut_head, _get(lib)[:-3])
    cut_body = os.path.join(t, "cut-body.a")
    _put(cut_body, b"".join([b"!<arch>\n"] + _ar_member("long.o/", b"0123456789"))[:-5])

    def truncated():
        a, b = refused_with(cut_head, "is TRUNCATED"), refused_with(cut_body, "is TRUNCATED")
        return (a[0] and b[0], "cut inside the last HEADER: %s\ncut inside the last BODY: %s"
                % (a[1] or "refused as TRUNCATED", b[1] or "refused as TRUNCATED"))
    A.arm("n30 a TRUNCATED archive -- cut inside a member's header, or inside its body -- is a "
          "named refusal (control: the whole archive reads, n28)", truncated)
    A.arm("n31 an archive that cannot be READ (a directory) is a named refusal",
          lambda: refused_with(t, "could not read the archive"))
    thin = os.path.join(t, "thin.a")
    _put(thin, b"!<thin>\n")
    A.arm("n71 a THIN archive (its members live outside it) is a named refusal",
          lambda: refused_with(thin, "is a THIN archive"))

    def malformed():
        whole = _get(lib)
        cases = []
        no_magic = os.path.join(t, "no-magic.a")
        _put(no_magic, whole[:8 + 58] + b"XX" + whole[8 + 60:])
        cases.append((no_magic, "does not end with the header magic"))
        bad_size = os.path.join(t, "bad-size.a")
        _put(bad_size, whole[:8 + 48] + b"12x".ljust(10) + whole[8 + 58:])
        cases.append((bad_size, "which is not a number"))
        no_table = os.path.join(t, "no-table.a")
        _put(no_table, b"!<arch>\n" + b"".join(_ar_member("/0", b"AB")))
        cases.append((no_table, "it has no long-name table ('//') before it"))
        got = []
        for path, needle in cases:
            exc = _raised(lambda: archive_members(path), RecipeRefused)
            got.append((os.path.basename(path), exc is not None and needle in str(exc),
                        str(exc) if exc is not None else "NOT refused"))
        return _verdict(all(ok for _n, ok, _e in got), "each case refused, naming its cause",
                        "; ".join("%s: %s" % (n, "ok" if ok else e) for n, ok, e in got))
    A.arm("n72 a header without its magic, a size that is not a number, and a long name with no "
          "table are each a named refusal", malformed)
    root = os.path.join(t, "root")
    for name in ("a.c", "b.c", "c.c"):
        _put(os.path.join(root, "src", name), "")

    def not_object_noted():
        led = DropLedger(stream=fx.sink)
        got = archive_tus(lib, None, [root], led)
        notes = [m for k, m in led.entries if k == "archive-not-object"]
        want = [os.path.join(root, "src", n) for n in ("a.c", "b.c", "c.c")]
        return _verdict(len(notes) == 1 and "'notes.txt'" in notes[0] and got == want,
                        "1 'archive-not-object' entry naming notes.txt, and a.c b.c c.c recovered",
                        "entries %r; recovered %r" % (led.entries, got))
    A.arm("n73 a member that is not a .o is NOTED, never skipped in silence (control: the three "
          ".o members are recovered)", not_object_noted)

    def span_filter():
        led = DropLedger(stream=fx.sink)
        got = archive_tus(lib, ["a.o", "c.o"], [root], led)
        every = archive_tus(lib, None, [root], led)
        return (got == [os.path.join(root, "src", "a.c"), os.path.join(root, "src", "c.c")]
                and len(every) == 3, "filtered %r / every %r" % (got, every))
    A.arm("n32 the archive-from-span filter recovers ONLY the named members (control: no filter "
          "recovers all three)", span_filter)

    def tsrc():
        r2 = os.path.join(t, "r2")
        _put(os.path.join(r2, "bld", "tsrc", "x.c"), "")
        _put(os.path.join(r2, "bld", "x.c"), "")
        _put(os.path.join(r2, "bld", "tsrc", "y.c"), "")
        led = DropLedger(stream=fx.sink)
        got = archive_tus(fx.archive("xy", ["x.o", "y.o"]), None, [r2], led)
        return (got == [os.path.join(r2, "bld", "x.c"), os.path.join(r2, "bld", "tsrc", "y.c")]
                and not led.entries, "got %r, entries %r" % (got, led.entries))
    A.arm("n33 a source outside tsrc/ is PREFERRED, and a tsrc-only member is still recovered",
          tsrc)

    def second_candidate():
        r3 = os.path.join(t, "r3")
        _put(os.path.join(r3, "ext", "z.c"), "")
        _put(os.path.join(r3, "src", "z.c"), "")
        _put(os.path.join(r3, "src", "solo.c"), "")
        led = DropLedger(stream=fx.sink)
        got = archive_tus(fx.archive("z-solo", ["z.o", "solo.o"]), None,
                          [os.path.join(r3, "src"), os.path.join(r3, "ext")], led)
        notes = [m for k, m in led.entries if k == "archive-candidate"]
        return (got == [os.path.join(r3, "src", "z.c"), os.path.join(r3, "src", "solo.c")]
                and len(notes) == 1 and "z.o" in notes[0], "got %r, notes %r" % (got, notes))
    A.arm("n34 a SECOND candidate source is reported and the first root wins (control: a single "
          "candidate is silent)", second_candidate)

    def sorted_walk():
        r4 = os.path.join(t, "r4")
        _put(os.path.join(r4, "b", "w.c"), "")
        _put(os.path.join(r4, "a", "w.c"), "")
        led = DropLedger(stream=fx.sink)
        got = archive_tus(fx.archive("w", ["w.o"]), None, [r4], led)
        return _eq([os.path.join(r4, "a", "w.c")], got)
    A.arm("n35 within a root the walk is SORTED ('a/' before 'b/', whatever the creation order)",
          sorted_walk)
    A.arm("n36 a search root that does not exist is a named refusal",
          lambda: "is not a directory" in str(_raised(
              lambda: archive_tus(lib, None, [os.path.join(t, "no-root")],
                                  DropLedger(stream=fx.sink)), RecipeRefused)))

    def through_emit():
        bld = fx.fresh("emit-span")
        _put(os.path.join(bld, "shell.c"), "")
        res, _o = fx.emit(bld, "cc -o prog shell.c a.o c.o\n", archive=lib,
                          archive_from_span=True, search_roots=[root])
        want = sorted([os.path.join(bld, "shell.c"), os.path.join(root, "src", "a.c"),
                       os.path.join(root, "src", "c.c")])
        return _eq(want, res.tus)
    A.arm("n37 emit_recipe archive_from_span recovers exactly the link line's objects",
          through_emit)


def _st_make(A, fx):
    t = fx.fresh("make")
    _put(os.path.join(t, "one.c"), "")

    def argv_and_cwd():
        rec = os.path.join(fx.fresh("rec"), "argv.json")
        make_argv = fx.make("cc -o prog one.c\n", record=rec)
        res, _o = fx.emit(t, "cc -o prog one.c\n", always_make=True,
                          make_vars=["A=1", "OPTIONS=-DX"], make_argv=make_argv)
        with open(rec, encoding="utf-8") as fh:
            got = json.load(fh)
        want_argv = ["-n", "-B", "-o", "Makefile", "prog", "A=1", "OPTIONS=-DX"]
        # The cwd is judged by IDENTITY, never by spelling: the recorder reports the kernel's
        # spelling (`os.getcwd()`), and ✔MEASURED 2026-09-23 on the Mac that is `/private/var/...`
        # for the `/var/...` the driver passed -- one directory, two names.
        try:
            same = os.path.samefile(got["cwd"], t)
        except OSError as exc:
            same = "cannot tell: %s" % exc

        def facts():
            return (["the stand-in make the driver ran (exit 0 by construction):",
                     "  argv   : %r" % (list(make_argv) + list(got["argv"]),),
                     "  output : %s (the recipe file: stdout and stderr, merged)"
                     % _raw(_get(res.recipe_file)),
                     "the cwd as each side spells it, and as realpath resolves it:",
                     "  expected %r -> %r" % (t, os.path.realpath(t)),
                     "  observed %r -> %r" % (got["cwd"], os.path.realpath(got["cwd"])),
                     "  the same directory (os.path.samefile): %s" % same,
                     "context, the host make (this arm drives a stand-in; n47 drives the host's):"]
                    + _tool_facts("make"))
        return _verdict(got["argv"] == want_argv and same is True,
                        "argv %r, cwd the directory %r" % (want_argv, t),
                        "argv %r, cwd %r" % (got["argv"], got["cwd"]), facts)
    A.arm("n38 make runs as [*make_argv, -n, -B, -o Makefile, target, *vars] with cwd = the "
          "build dir (the same DIRECTORY, however the host spells it)",
          argv_and_cwd)

    def no_b():
        argvs = []
        for always in (False, True):
            rec = os.path.join(fx.fresh("rec"), "argv.json")
            fx.emit(t, "cc -o prog one.c\n", always_make=always,
                    make_argv=fx.make("cc -o prog one.c\n", record=rec))
            with open(rec, encoding="utf-8") as fh:
                argvs.append(json.load(fh)["argv"])
        return _eq([["-n", "-o", "Makefile", "prog"], ["-n", "-B", "-o", "Makefile", "prog"]],
                   argvs)
    A.arm("n39 without always_make there is NO -B (control: with it, there is)", no_b)

    def never_remakes():
        argvs = []
        for always in (False, True):
            rec = os.path.join(fx.fresh("rec"), "argv.json")
            fx.emit(t, "cc -o prog one.c\n", always_make=always,
                    make_argv=fx.make("cc -o prog one.c\n", record=rec))
            with open(rec, encoding="utf-8") as fh:
                a = json.load(fh)["argv"]
            i = a.index("-o") if "-o" in a else -1
            argvs.append(i >= 0 and a[i + 1:i + 2] == ["Makefile"] and i < a.index("prog"))
        return _eq([True, True], argvs)
    A.arm("n39b the dry run NEVER remakes the makefile: `-o Makefile` precedes the target, "
          "with and without -B", never_remakes)
    A.arm("n40 make's exit code is IGNORED: a non-zero make that printed a good recipe derives",
          lambda: _eq(1, len(fx.emit(t, "cc -o prog one.c\n",
                                     make_argv=fx.make("cc -o prog one.c\n", exit_code=2))[0].tus)))

    def stderr_merged():
        res, out = fx.emit(t, "cc -o prog one.c\n",
                           make_argv=fx.make("cc -o prog one.c\n", stderr="make: warning W1"))
        return b"make: warning W1" in _get(res.recipe_file)
    A.arm("n41 make's STDERR lands in the recipe file with its stdout", stderr_merged)
    def make_cannot_start():
        recipe = os.path.join(fx.fresh("make-start"), "recipe.txt")
        exc = _raised(lambda: fx.emit(t, "x", recipe_file=recipe,
                                      make_argv=(os.path.join(t, "no-such-make"),)),
                      RecipeRefused)
        return (exc is not None and "could not start make" in str(exc)
                and b"could not start make" in _get(recipe), "raised %r" % exc)
    A.arm("n42 a make that cannot START is a named refusal, stated in the recipe file too",
          make_cannot_start)
    A.arm("n43 a build dir that does not exist is a named refusal",
          lambda: "is not a directory" in str(_raised(
              lambda: fx.emit(os.path.join(t, "no-such-bld"), "cc -o prog one.c\n"),
              RecipeRefused)))

    def no_link_hint():
        exc = _raised(lambda: fx.emit(t, "cc -o other one.c\n"), NoLinkLine)
        return (exc is not None and "recipe.txt" in str(exc) and "unless always_make is set" in str(exc),
                "raised %r" % exc)
    A.arm("n44 emit_recipe's NoLinkLine names the recipe file and the always_make remedy",
          no_link_hint)

    def unresolved_note():
        res, _o = fx.emit(t, "cc -o prog one.c gone.c\n")
        return ("★ 1 .c token(s) in the recipe resolved to NO file" in res.summary
                and res.summary.startswith("prog: 1 TUs, 0 defines, 0 -I dirs (mode link-line)"),
                res.summary)
    A.arm("n45 the unresolved-token count rides on the summary", unresolved_note)

    def drops_file():
        res, _o = fx.emit(t, "cc -o prog one.c gone.c\n")
        text = _get(res.recipe_file + ".drops").decode("utf-8")
        return (text.startswith("recipe-token\tthe recipe span names 'gone.c'")
                and text.count("\n") == 1, text)
    A.arm("n46 <recipe>.drops holds the ledger as <kind>TAB<msg> lines", drops_file)

    def real_make():
        if not fx.host_make:
            return _SKIP, "no make on this PATH"
        bld = fx.fresh("real-make")
        _put(os.path.join(bld, "one.c"), "")
        _put(os.path.join(bld, "Makefile"), "prog: one.c\n\tcc -DREAL_MAKE -Iinc -o prog one.c\n")
        res, _o = fx.emit(bld, "", make_argv=("make",), always_make=True)
        return (res.tus == [os.path.join(bld, "one.c")] and res.defines == ["REAL_MAKE"]
                and res.includes == ["inc"], "got %r" % (res,))
    A.arm("n47 the REAL make derives a Makefile's recipe (argv order and cwd accepted by make)",
          real_make)


def _st_cli(A, fx):
    # ★ THE LIBRARY'S ONE VERB IS ITS SELF-TEST (2026-09-25): the `emit-recipe` verb left -- no program ran it,
    # every caller imports `emit_recipe` -- and with it its three arms (n48 the real make through the CLI, n49 a
    # refusal's exit code, n52 its repeatable flags). The library's own arms cover the derivation (n47 the real
    # make, sh30-sh41 the refusals). What stays is the CLI's contract.
    def cli_usage():
        gone = fx.cli(["emit-recipe", "--build-dir", "x"])
        none = fx.cli([])
        err = gone.stderr.decode("utf-8")
        return ([p.returncode for p in (gone, none)] == [2, 2] and "its one CLI verb is --self-test" in err,
                "rcs %r\n%s" % ([p.returncode for p in (gone, none)], err))
    A.arm("n50 CLI usage errors are exit 2: no verb, and the retired emit-recipe verb names the library's one "
          "verb", cli_usage)
    A.arm("n51 CLI --selftest is an alias of --self-test, and extra arguments are usage",
          lambda: (cli_action(["--selftest"]) == ("self-test",)
                   and cli_action(["--self-test"]) == ("self-test",)
                   and cli_action(["--self-test", "x"])[0] == "usage"))


def _st_build(A, fx):
    spec = "x86_64:elf64-x86_64-linux-exec"
    mk = ARTIFACT_MARKER + spec + " "
    t = fx.fresh("build")
    log = os.path.join(t, "build.log")
    real = os.path.join(t, "real-artifact.bin")
    _put(real, "x")

    def build(say, **kw):
        return build_artifact(fx.dss(say, **kw), "m.json", "debug", t, log, spec)
    r1 = build("error[F001A] something went wrong\nerror[F001A] and again\n")
    A.arm("ps14 build with diagnostics is NOT Ok", lambda: not r1.ok and r1.code == 3)
    A.arm("ps15   … and counts them", lambda: _eq(2, r1.err_count))
    r2 = build("nothing interesting happened\n")
    A.arm("ps16 0 error[ and NO artefact report is NOT Ok", lambda: not r2.ok and r2.code == 1)
    A.arm("ps17   … and says the build reported nothing",
          lambda: ("NO artefact" in (r2.error or ""), r2.error))
    r3 = build(mk + os.path.join(t, "not-written.bin") + "\n")
    A.arm("ps18 an artefact REPORTED but not on disk is NOT Ok", lambda: not r3.ok and r3.code == 4)
    A.arm("ps19   … and quotes what the compiler claimed",
          lambda: ("not-written.bin" in (r3.error or ""), r3.error))
    r4 = build("compile time 1.5s\n" + mk + real + "\n")
    A.arm("ps20 a real artefact with 0 error[ IS Ok (the control)",
          lambda: (r4.ok and r4.code == 0 and r4.path == _norm(real), "got %r" % (r4,)))
    A.arm("ps21   … and carries the compile-time suffix",
          lambda: _eq("  (compile time 1.5s)", r4.time_suffix))

    def launch_failed():
        missing = os.path.join(t, "no-such-dsscp")
        r = build_artifact(missing, "m.json", "debug", t, log, spec)
        return (r.code == LAUNCH_FAILED and not r.ok and "could not start the compiler" in r.error
                and b"could not start the compiler" in _get(log), "got %r" % (r,))
    A.arm("n53 a compiler that cannot START is LAUNCH_FAILED (not 'it said nothing'), stated in "
          "the log", launch_failed)

    def nonzero_ok():
        r = build(mk + real + "\n", exit_code=3)
        return (r.ok and r.code == 0 and r.exit_code == 3, "got %r" % (r,))
    A.arm("n54 a NON-ZERO exit with a valid artefact and 0 error[ is Ok: the log is the verdict, "
          "the exit code is recorded", nonzero_ok)

    def directory():
        r = build(mk + t + "\n")
        return (r.code == 4 and not r.ok and "not a regular file" in (r.error or ""),
                "got %r" % (r,))
    A.arm("n55 a DIRECTORY reported as the artefact is code 4 (isfile, not exists)", directory)

    def raw_bytes():
        raw = b"\xff\xfe raw line" + CR.encode() + b"\n" + (mk + real + "\n").encode()
        r = build_artifact(fx.dss(raw), "m.json", "debug", t, log, spec)
        return (_get(log) == raw and r.ok, "log %r" % _get(log)[:80])
    A.arm("n56 the log holds the compiler's RAW bytes (a non-UTF-8 byte and a CR survive)",
          raw_bytes)

    def stderr_in_log():
        r = build(mk + real + "\n", stderr="warning: from the compiler's stderr")
        return (b"from the compiler's stderr" in _get(log) and r.ok, "got %r" % (r,))
    A.arm("n57 the compiler's STDERR is captured into the log", stderr_in_log)

    def argv_shape():
        rec = os.path.join(fx.fresh("dss-rec"), "argv.json")
        build(mk + real + "\n", record=rec)
        with open(rec, encoding="utf-8") as fh:
            got = json.load(fh)["argv"]
        return _eq(["--project", "m.json", "--config=debug", "--output", t, "--time",
                    "--max-diagnostics", str(DIAGNOSTIC_CAP), "--max-per-code", str(DIAGNOSTIC_CAP)], got)
    A.arm("n58 the compiler argv is exactly --project M --config=C --output D --time and BOTH diagnostic "
          "caps raised to DIAGNOSTIC_CAP: every build asks for its WHOLE stream", argv_shape)

    def str_launch():
        r = build_artifact(sys.executable, "m.json", "debug", t, log, spec)
        return (r.code == 1 and r.exit_code not in (0, None) and len(_get(log)) > 0,
                "got %r" % (r,))
    A.arm("n59 a PATH-form compiler is launched as a real process (python refuses the flags: "
          "exit code recorded, its stderr in the log, no artefact)", str_launch)

    def case_sensitive():
        shout = build("ERROR[X] shouted but not a dsscp diagnostic\n")
        quiet = build("error[X] a real one\n")
        return (shout.err_count == 0 and quiet.err_count == 1 and quiet.code == 3,
                "ERROR[ -> %d, error[ -> %d" % (shout.err_count, quiet.err_count))
    A.arm("n60 error[ is counted case-SENSITIVELY (control: a lowercase error[ counts)",
          case_sensitive)
    A.arm("n61 first_errors keeps the FIRST three error[ lines",
          lambda: _eq(["error[E%d] n" % i for i in (1, 2, 3)],
                      build("".join("error[E%d] n\n" % i for i in range(1, 6))).first_errors))
    A.arm("n62 two artefacts reported is build code 2 (refused, no path)",
          lambda: (lambda r: (r.code == 2 and r.path is None, "got %r" % (r,)))(
              build(mk + real + "\n" + mk + os.path.join(t, "other.bin") + "\n")))


def _st_suffix(A, fx):
    A.arm("n63 compile_time_suffix takes the LAST match",
          lambda: _eq("  (compile time 2s)",
                      compile_time_suffix("dsscp: compile time 1s\nx\ndsscp: compile time 2s\n")))
    A.arm("n64 compile_time_suffix is empty when there is none",
          lambda: _eq("", compile_time_suffix("no timing here\n")))


def _st_manifest(A, fx):
    spec = "x86_64:elf64-x86_64-linux-exec"

    def gen(transform, reserve, script=None, lib_argv=(), tu_preludes=""):
        return generate_manifest(script or fx.echo_gen, "o", "sqlite3", spec, "t", "i", "d",
                                 transform, reserve, lib_argv=lib_argv, tu_preludes=tu_preludes)
    g = gen("none", 0)
    A.arm("ps22 manifest argv carries --recipe-transform none and --stack-reserve 0 EXPLICITLY "
          "(was: omits --recipe-transform when $null)",
          lambda: ("--recipe-transform none" in " ".join(json.loads(g.out))
                   and "--stack-reserve 0" in " ".join(json.loads(g.out)), g.out))
    g2 = gen("windows-selfconfig", 8388608)
    A.arm("ps23   … and carries them when they are given",
          lambda: ("--recipe-transform windows-selfconfig" in " ".join(json.loads(g2.out))
                   and "--stack-reserve 8388608" in " ".join(json.loads(g2.out)), g2.out))
    A.arm("n65 an omitted (None) transform or reserve is a USAGE error (it selected the pe64 "
          "defaults)",
          lambda: (_raised(lambda: gen(None, 0), HarnessUsageError) is not None
                   and _raised(lambda: gen("none", None), HarnessUsageError) is not None))

    def order():
        r = gen("none", 0, lib_argv=("--resolve-library", "z=/lib/libz"))
        want = ["--tus", "t", "--includes", "i", "--defines", "d", "--target", spec,
                "--resolve-library", "z=/lib/libz", "--artifact-name", "sqlite3",
                "--recipe-transform", "none", "--stack-reserve", "0", "--output", "o"]
        return _eq(want, json.loads(r.out))
    A.arm("n66 the generator argv is the bash twin's ORDER, the library argv passed through as "
          "tokens", order)

    def preludes():
        r = gen("none", 0, tu_preludes="p.json")
        got = json.loads(r.out)
        want = ["--tus", "t", "--includes", "i", "--defines", "d", "--target", spec,
                "--artifact-name", "sqlite3", "--recipe-transform", "none", "--stack-reserve", "0",
                "--tu-preludes", "p.json", "--output", "o"]
        return _eq(want, got)
    A.arm("n74 a leg's declared TU preludes reach the generator as --tu-preludes, just before "
          "--output (a leg without them: n66's argv, byte for byte)", preludes)

    def failing():
        r = gen("none", 0, script=fx.fail_gen)
        m = manifest_error(r) or ""
        return (r.rc == 4 and m.startswith("manifest generation failed: ")
                and "this manifest is refused / gen: second line" in m, m)
    A.arm("n67 a failing generator is rc != 0 and manifest_error states it with its output",
          failing)


_SECTIONS = (("join", _st_join), ("defines", _st_defines), ("span", _st_span),
             ("token-span", _st_token_span), ("dedup", _st_dedup), ("reader", _st_reader),
             ("ledger", _st_ledger), ("floors", _st_floors), ("tool-guard", _st_tool_guard),
             ("drops", _st_drops), ("archive", _st_archive), ("make", _st_make),
             ("cli", _st_cli), ("build", _st_build), ("suffix", _st_suffix),
             ("manifest", _st_manifest))


def self_test():
    print("== sqlite_base.py --self-test ==")
    A = _Arms()
    root = tempfile.mkdtemp(prefix="dss-sqlite-base-st-")
    try:
        fx = _Fx(root)
        print("   (host ar: %s; host make: %s)" % (fx.host_ar or "none", fx.host_make or "none"))
        for name, fn in _SECTIONS:
            A.section(name, fn, fx)
    finally:
        shutil.rmtree(root, ignore_errors=True)
    return A.finish(EXPECTED_ARMS)


if __name__ == "__main__":
    sys.exit(main())
