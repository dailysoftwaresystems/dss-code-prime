#!/usr/bin/env python3
"""sqlite_corpus.py -- the Step-8 ENGINE of the SQLite corpus harness: pure functions over
sqlite's own test tree and over ONE segment log.

It replaces the `dss:corpus-engine` region the two old drivers mirrored (`build-and-test.sh`
and its Windows twin `build-and-test.ps1`, lane mig, part 4, 2026-09-21): the corpus file list,
the tier's permutation sequence, the per-suite test-name prefixes, the segment parser and its
fact alphabet, the zero-progress signature, the aborting-file resolver, the ordinal file-list
helpers and the digit grouper. There is ONE implementation now, so the old differential
battery (`harness_legs.py --check-regions`) has no twin to compare; its inputs, the two answers
it STATED and the six answers the old bash and PowerShell copies AGREED on (MEASURED before
they were deleted) are this module's `--self-test`.

The RESUME DECISION (the strictly advancing boundary, the two inserted segments, the refusal
reasons) is deliberately NOT here: it lives in `sqlite_units.py`, on top of these primitives,
so there is one copy of the decision.

THE FACT ALPHABET of one segment (the old .sh parser's letters, kept so a reader of either can
map one to the other):
  A first_diag     the first non-blank line that is NOT the fixture doing its job
  B abort_file     the innermost `(file "<p>" line N)` frame of the LAST traceback block
                   (`blamed` keeps one frame per block, in order)
  F files          completed test files, in order
  I inert          completed files that asserted NOTHING (only the two teardown results)
  X failures       failing test names in stream order, duplicates kept (every consumer dedups)
  S summary        the WHOLE summary line;  E errors / C total out of it (None without S)
  P permutation    T last_test    G gave_up (`*** Giving up`: tester.tcl's --maxerror cap)
  N n_files   D last_file   M n_inert   K ok (` Ok` lines)   Q fail_markers (`! <n> expected:`)
  derived_count    K + Q + 1 -- an ABORTED segment's test count (it printed no summary)

Nothing runs at import. `python3 sqlite_corpus.py --self-test` prints `passed=N failed=N
skipped=N` last and exits 0 only when nothing failed.
"""
from __future__ import annotations

import os
import re
import sys

for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError, OSError):  # pragma: no cover - an odd stream
        pass
# The action is `requireInputsUnmoved`: no `__pycache__` may appear beside its programs, and
# the sibling import below would write one before its own guard ran.
sys.dont_write_bytecode = True

import sqlite_common as C  # noqa: E402

# ★ THE SILENT-CRASH SENTINEL, DEFINED ONCE. The zero-progress signature of a segment that said
# NOTHING at all, so two consecutive silent segments compare EQUAL and the leg stops after ONE
# resume instead of burning the whole budget on a fixture that never started (MEASURED on a
# native elf64-x86_64 leg whose every corpus log was 0 bytes). ASCII on purpose: it is compared
# byte for byte and must never carry an encoding question.
SILENT_SENTINEL = "<SILENT: the fixture produced no diagnostic, no test result and no test name>"

# The first-diagnostic cap. ⚠DECISION: the suffix is the ASCII ' ...[truncated]' (the old
# PowerShell twin's); the bash twin appended U+2026 and counted BYTES under LC_ALL=C, so it could
# cut a UTF-8 sequence in half. Here the cap counts characters of the decoded line.
DIAG_LIMIT = 400
TRUNCATION_SUFFIX = " ...[truncated]"

# The two results tester.tcl emits for EVERY file. A file whose only results are these asserted
# nothing (it returned at an `ifcapable` gate). ⚠ The names end in `...` with NO space before it,
# so the match is on the WHOLE first field's tail -- an anchored `-closeallfiles$` never fires
# (MEASURED: a first cut reported 0 inert files over a corpus with 362).
TEARDOWN_TAILS = (".test-closeallfiles...", ".test-sharedcachesetting...")

_RE_BLANKS = re.compile(r"[ \t]+")
_RE_NONBLANK = re.compile(r"[^ \t]")
_RE_TESTNAME = re.compile(r"[^ \t]+\.\.\.")
_RE_SUMMARY = re.compile(r"([0-9]+) errors? out of ([0-9]+) tests")
_RE_BLAME = re.compile(r'\(file "([^"]*)" line [0-9]+\)')
_RE_FAILS_HEAD = re.compile(r"!?Failures on these tests:[ \t]*")
_RE_BANG = re.compile(r"! [^ ]+ (expected|got):")
_RE_PERM_LEAD = re.compile(r'[ \t]*"?')
_RE_PERM = re.compile(r"(?:run_test_suite|run_tests)[ \t]+([A-Za-z_][A-Za-z0-9_]*)")

_RE_EXCLUDE_OPEN = re.compile(
    r"[ \t]*set[ \t]+alltests[ \t]+\[test_set[ \t]+\$alltests[ \t]+-exclude[ \t]*\{")
_RE_EXCLUDE_SPLIT = re.compile(r"[ \t{}\]]+")
_RE_EXCLUDE_CLOSE = re.compile(r"\}[ \t]*\]")
# ⚠DECISION (the PowerShell twin's boundary): `run_test_suite` must not be the tail of a longer
# word -- `my_run_test_suite x` is not a call. Both twins agreed on every measured input.
_RE_RUN_SUITE = re.compile(r"(?<![\w-])run_test_suite[ \t]+([A-Za-z_][A-Za-z0-9_]*)")
_RE_SUITE_LINE = re.compile(r'[ \t]*test_suite[ \t]+"')
_RE_SUITE_NAME = re.compile(r'test_suite[ \t]+"([^"]*)"')
_RE_SUITE_PREFIX = re.compile(r'-prefix[ \t]+"([^"]*)"')


def _strip_eol(raw):
    """ONE line of a binary read: the LF removed, then ONE trailing CR (a CRLF log from a fixture
    running on Windows). A lone CR elsewhere does NOT end a line -- the awk record discipline."""
    if raw.endswith(b"\n"):
        raw = raw[:-1]
    if raw.endswith(b"\r"):
        raw = raw[:-1]
    return raw


def _text_lines(path):
    """Every line of `path`, split on LF only, one trailing CR removed, bytes decoded leniently."""
    with open(path, "rb") as fh:
        for raw in fh:
            yield _strip_eol(raw).decode("utf-8", "replace")


def _fields(line):
    """awk's default field split: runs of blanks, leading and trailing ones ignored."""
    return [f for f in _RE_BLANKS.split(line) if f]


# ── sqlite's own test tree, read as DATA ─────────────────────────────────────────────

def corpus_exclusions(permutations_test):
    """(names, found): the `.test` names permutations.test's own
    `set alltests [test_set $alltests -exclude { … }]` block removes from $alltests, and whether
    that block was found. The opening line's own words are NOT read (both old twins skipped it).
    ⚠DECISION: a CRLF permutations.test is read like an LF one (the bash twin kept the CR, so
    `x.test\\r` never matched and the exclusion silently widened)."""
    names, found, inside = set(), False, False
    for line in _text_lines(permutations_test):
        if not inside:
            if _RE_EXCLUDE_OPEN.match(line):
                inside = found = True
            continue
        for word in _RE_EXCLUDE_SPLIT.split(line):
            if word.endswith(".test"):
                names.add(word)
        if _RE_EXCLUDE_CLOSE.search(line):
            break
    return names, found


def corpus_files(testdir, log=C.LOG):
    """sqlite's own $alltests: every `*.test` basename in `testdir` (a leading dot is never
    matched, as by the shell and Tcl globs) MINUS the driver scripts permutations.test excludes by
    name, in CODE-POINT order -- the order run_tests uses (`lsort`, -ascii) and the order every
    ordinal helper below compares in. A parse miss only WIDENS the list (it is a superset filter),
    and it is WARNED, never silent. A missing corpus directory is refused."""
    if not os.path.isdir(testdir):
        C.die("the corpus directory %s does not exist (or is not a directory), so the .test corpus "
              "list cannot be read." % testdir)
    perms = os.path.join(testdir, "permutations.test")
    skip = set()
    if os.path.exists(perms):
        try:
            skip, found = corpus_exclusions(perms)
        except OSError as exc:
            skip, found = set(), False
            if log is not None:
                log.warn("could not read %s (%s)." % (perms, exc))
        if not found and log is not None:
            log.warn("could not read sqlite's $alltests exclude list from %s — using the raw *.test "
                     "glob (a superset; still correct)." % perms)
    names = []
    for name in os.listdir(testdir):
        if name.startswith(".") or not name.endswith(".test") or name in skip:
            continue
        if os.path.exists(os.path.join(testdir, name)):
            names.append(name)
    return sorted(names)


def tier_permutations(tierfile):
    """The tier script's permutation sequence, in order: the name after each `run_test_suite`
    (all.test names 27, veryquick/quick/full one). One per line, the first on it. ⚠ A commented
    `# run_test_suite x` line IS counted -- both old copies did, and their agreed answer is a
    stated expectation of the self-test (no shipped tier script carries one, MEASURED)."""
    try:
        lines = list(_text_lines(tierfile))
    except OSError as exc:
        C.die("could not read the tier script %s: %s" % (tierfile, exc))
    out = []
    for line in lines:
        m = _RE_RUN_SUITE.search(line)
        if m:
            out.append(m.group(1))
    return out


def tier_prefixes(permutations_test):
    """The TEST-NAME PREFIX each suite stamps onto its test names, read as DATA out of
    permutations.test -- it is NOT derivable from the suite name: the default is `<name>.`, `mmap`
    declares `-prefix "mm-"`, and veryquick/quick/full declare `-prefix ""` (none). One non-empty
    prefix per suite, LONGEST FIRST (a longer prefix wins over one that is merely its head); ties
    in reverse code-point order (the bash twin's `sort -rn` tie-break). A declaration must START
    its line (a commented one is not one). A missing file has no prefixes (a DSS_TEST_FILE outside
    sqlite's tree has none); an unreadable one is refused."""
    if not os.path.exists(permutations_test):
        return []
    try:
        lines = list(_text_lines(permutations_test))
    except OSError as exc:
        C.die("could not read %s: %s" % (permutations_test, exc))
    found = set()
    for line in lines:
        if not _RE_SUITE_LINE.match(line):
            continue
        m = _RE_SUITE_NAME.search(line)
        if not m:
            continue
        mp = _RE_SUITE_PREFIX.search(line)
        prefix = mp.group(1) if mp else m.group(1) + "."
        if prefix:
            found.add(prefix)
    return sorted(found, key=lambda p: (len(p), p), reverse=True)


# ── one segment log ─────────────────────────────────────────────────────────────────

class SegmentFacts:
    """What ONE streaming pass over a segment log found (see the fact alphabet above)."""

    __slots__ = ("first_diag", "blamed", "files", "inert", "gave_up", "failures", "summary",
                 "errors", "total", "permutation", "last_test", "ok", "fail_markers")

    def __init__(self, first_diag="", blamed=None, files=None, inert=None, gave_up=False,
                 failures=None, summary="", errors=None, total=None, permutation="", last_test="",
                 ok=0, fail_markers=0):
        self.first_diag = first_diag            # A
        self.blamed = list(blamed or [])        # every B, in order
        self.files = list(files or [])          # F
        self.inert = list(inert or [])          # I
        self.gave_up = bool(gave_up)            # G
        self.failures = list(failures or [])    # X
        self.summary = summary                  # S
        self.errors = errors                    # E
        self.total = total                      # C
        self.permutation = permutation          # P
        self.last_test = last_test              # T
        self.ok = ok                            # K
        self.fail_markers = fail_markers        # Q

    @property
    def abort_file(self):
        """B: the frame of the LAST traceback, i.e. the one the process died in ("" when none)."""
        return self.blamed[-1] if self.blamed else ""

    @property
    def n_files(self):
        return len(self.files)

    @property
    def last_file(self):
        return self.files[-1] if self.files else ""

    @property
    def n_inert(self):
        return len(self.inert)

    @property
    def derived_count(self):
        return self.ok + self.fail_markers + 1

    def __repr__(self):
        return ("SegmentFacts(N=%d D=%r M=%d K=%d Q=%d S=%r T=%r P=%r B=%r G=%r A=%r X=%d)"
                % (self.n_files, self.last_file, self.n_inert, self.ok, self.fail_markers,
                   self.summary, self.last_test, self.permutation, self.abort_file, self.gave_up,
                   self.first_diag, len(self.failures)))


def parse_segment(log_path):
    """ONE streaming pass over a segment log -> SegmentFacts. The logs reach 150 MB / 3.6M lines,
    so the file is read in BINARY, line by line (LF only; one trailing CR removed; a lone CR does
    not end a line), each line decoded with errors replaced. The rules and their ORDER are the old
    bash parser's (a rule that consumed a line hid it from every rule below it):
      A  first non-blank line that is not `Time: `, not `… Ok`, not `<name>...`, not a summary;
         tabs -> spaces, capped at DIAG_LIMIT characters + TRUNCATION_SUFFIX
      B  a `Time: `/` Ok`/`<name>...` line ends a traceback block; the FIRST `(file "…" line N)`
         of a block is its frame (Tcl prints errorInfo innermost-first); an empty one still ends
         the block's turn
      F/I  `Time: <f> <n> ms` (exactly four fields, the fourth `ms`); inert when nothing counted
         since the previous file; consumed
      G  `*** Giving up` (consumed)   X  `!Failures on these tests: a b` (leading bang optional;
         consumed)
      K  every line ending ` Ok`; it counts toward the file unless its first field is a teardown
      Q  `! <n> expected:` counts a failure (and toward the file); `! <n> expected:|got:` records
         X <n> (consumed)
      S/E/C  the LAST line carrying `<n> errors out of <m> tests` (the whole line; consumed)
      P  `run_test_suite <p>` / `run_tests <p>` after optional blanks and ONE quote (consumed)
      T  the text before the first `...` of a `<name>...` line
    A missing or unreadable log is refused (the segment runner always creates it)."""
    diag = ""
    blamed, files, inert, failures = [], [], [], []
    summary, nerr, ntest = "", None, None
    perm = last_test = ""
    gave_up = False
    ok = fx = pend = 0
    tbseen = False
    try:
        fh = open(log_path, "rb")
    except OSError as exc:
        C.die("could not read the segment log %s: %s" % (log_path, exc))
    with fh:
        for raw in fh:
            line = _strip_eol(raw).decode("utf-8", "replace")
            is_time = line.startswith("Time: ")
            is_ok = line.endswith(" Ok")
            is_test = "..." in line and _RE_TESTNAME.match(line) is not None
            if not diag and not is_time and not is_ok and not is_test \
                    and _RE_NONBLANK.search(line) and not _RE_SUMMARY.search(line):
                d = line.replace("\t", " ")
                diag = d[:DIAG_LIMIT] + TRUNCATION_SUFFIX if len(d) > DIAG_LIMIT else d
            if is_time or is_ok or is_test:
                tbseen = False
            if '(file "' in line and not tbseen:
                m = _RE_BLAME.search(line)
                if m:
                    tbseen = True
                    b = m.group(1).replace("\t", " ")
                    if b:
                        blamed.append(b)
            if is_time:
                f = _fields(line)
                if len(f) == 4 and f[3] == "ms":
                    files.append(f[1])
                    if pend == 0:
                        inert.append(f[1])
                    pend = 0
                    continue
            if line.startswith("*** Giving up"):
                gave_up = True
                continue
            if line.startswith("Failures on these tests:") \
                    or line.startswith("!Failures on these tests:"):
                rest = line[_RE_FAILS_HEAD.match(line).end():]
                failures.extend(n for n in _RE_BLANKS.split(rest) if n)
                continue
            if is_ok:
                ok += 1
                first = _fields(line)
                if not (first and first[0].endswith(TEARDOWN_TAILS)):
                    pend += 1
            if line.startswith("! "):
                m = _RE_BANG.match(line)
                if m:
                    if m.group(1) == "expected":
                        fx += 1
                        pend += 1
                    f = _fields(line)
                    failures.append(f[1] if len(f) > 1 else "")
                    continue
            if " out of " in line:
                m = _RE_SUMMARY.search(line)
                if m:
                    summary, nerr, ntest = line, int(m.group(1)), int(m.group(2))
                    continue
            if "run_test" in line:
                rest = line[_RE_PERM_LEAD.match(line).end():]
                m = _RE_PERM.match(rest)
                if m:
                    perm = m.group(1)
                    continue
            if is_test:
                last_test = line.split("...", 1)[0]
    return SegmentFacts(first_diag=diag, blamed=blamed, files=files, inert=inert, gave_up=gave_up,
                        failures=failures, summary=summary,
                        errors=nerr if summary else None, total=ntest if summary else None,
                        permutation=perm, last_test=last_test, ok=ok, fail_markers=fx)


def zero_progress_signature(facts):
    """What must CHANGE between two consecutive zero-file segments for another resume to be worth
    attempting: the first diagnostic when there is one; SILENT_SENTINEL when the parse found
    NOTHING (no diagnostic, no ` Ok`, no failure marker, no test name); otherwise "" -- a segment
    that ran tests without completing a file made progress and must stay resumable."""
    if facts.first_diag:
        return facts.first_diag
    if not facts.ok and not facts.fail_markers and not facts.last_test:
        return SILENT_SENTINEL
    return ""


def is_precondition_failure(prev_zero_sig, facts):
    """The PRECONDITION discriminator: this segment completed ZERO files AND its signature is
    non-empty AND equal -- ORDINAL, case included -- to the previous zero-file segment's. A genuine
    crash moves (the boundary strictly advances), so it dies differently and stays resumable."""
    sig = zero_progress_signature(facts)
    return facts.n_files == 0 and sig != "" and sig == (prev_zero_sig or "")


# ── naming the aborting file, and the ordinal helpers ─────────────────────────────────

def resolve_abort_file(name, corpus):
    """Which corpus FILE a qualified test NAME (the T fact) or a traceback SOURCE PATH (the B fact)
    names: the directory prefix is dropped first, on EITHER separator (the list is basenames, so
    whose namespace the prefix belongs to is irrelevant -- one wine log carries `Z:/…` and `/…`
    spellings of the same tree); then the corpus stem that occurs RIGHTMOST on `.`/`-`/string
    boundaries wins, a tie going to the LONGER stem.
    `inmemory_journal.swarmvtabfault-1.1-oom-persistent.143` -> swarmvtabfault.test (not
    swarmvtab.test: the `f` after it is no delimiter). "" when nothing matches. An empty stem
    never matches (the awk twin would have looped on one)."""
    if not name:
        return ""
    base = name.rsplit("/", 1)[-1].rsplit("\\", 1)[-1]
    if not base:
        return ""
    size = len(base)
    best_at, best_len, best_file = -1, -1, ""
    for f in corpus:
        stem = f[:-5] if f.endswith(".test") else f
        if not stem:
            continue
        span = len(stem)
        at = -1
        i = base.find(stem)
        while i >= 0:
            before = "." if i == 0 else base[i - 1]
            after = "." if i + span >= size else base[i + span]
            if before in ".-" and after in ".-":
                at = i
            i = base.find(stem, i + 1)
        if at >= 0 and (at > best_at or (at == best_at and span > best_len)):
            best_at, best_len, best_file = at, span, f
    return best_file


def files_after(boundary, corpus):
    """Every corpus basename ordinally AFTER `boundary` ('' = all of them) -- the
    SQLITE_TEST_PATTERN_LIST superset sqlite intersects with the permutation's own -files."""
    b = boundary or ""
    return [f for f in corpus if f > b]


def first_file_after(boundary, corpus):
    """The FIRST basename (in list order) ordinally after `boundary`, or None."""
    b = boundary or ""
    for f in corpus:
        if f > b:
            return f
    return None


def group_digits(n):
    """Thousands separators, locale-free: 4200000 -> '4,200,000'."""
    return "{:,}".format(int(n))


# ── self-test ────────────────────────────────────────────────────────────────────────

class _Checks:
    def __init__(self, out=None):
        self.out = out or sys.stdout
        self.passed = self.failed = self.skipped = 0

    def section(self, title):
        print("-- %s" % title, file=self.out)

    def check(self, label, ok, detail=""):
        if ok:
            self.passed += 1
            print("  ok   %s" % label, file=self.out)
        else:
            self.failed += 1
            print("  FAIL %s%s" % (label, ("\n       " + detail) if detail else ""), file=self.out)

    def eq(self, label, want, got):
        self.check(label, want == got, "expected %r\n       got      %r" % (want, got))

    def skip(self, label, why):
        self.skipped += 1
        print("  skip %s — %s" % (label, why), file=self.out)


class _CaptureLog:
    def __init__(self):
        self.lines = []

    def step(self, msg):
        self.lines.append(("step", str(msg)))

    def info(self, msg):
        self.lines.append(("info", str(msg)))

    def ok(self, msg):
        self.lines.append(("ok", str(msg)))

    def warn(self, msg):
        self.lines.append(("warn", str(msg)))

    def text(self, kind="warn"):
        return "\n".join(t for k, t in self.lines if k == kind)


def _write(path, data):
    if isinstance(data, str):
        data = data.encode("utf-8")
    with open(path, "wb") as fh:
        fh.write(data)
    return path


# The inputs of the old differential battery (harness_legs.py `_mirror_write_fixtures`), byte for
# byte, and the answers it recorded. `MIRROR_*_MEASURED` were NOT stated by that battery: they are
# the output the old bash copy (Git Bash on Windows AND GNU bash in WSL) and the old PowerShell
# copy AGREED on over these inputs, MEASURED 2026-09-21 before the copies were deleted.
_MIRROR_CORPUS_NAMES = ["alter.test", "wal2.test", "swarmvtab.test", "swarmvtabfault.test",
                        "zipfile.test", "walsetlk.test", "all.test", "permutations.test",
                        "veryquick.test"]
_MIRROR_PERMS = ("set alltests [test_set $alltests -exclude {\n"
                 "  all.test permutations.test\n"
                 "  veryquick.test\n"
                 "}]\n"
                 'test_suite "veryquick" -prefix "" -description {\n'
                 "}\n"
                 'test_suite "mmap" -prefix "mm-" -description {\n'
                 "}\n"
                 'test_suite "inmemory_journal" -description {\n'
                 "}\n")
_MIRROR_TIER = ("run_test_suite veryquick\n"
                "  run_test_suite inmemory_journal\n"
                "# run_test_suite mmap\n")
_MIRROR_LIST = ["alter.test", "swarmvtab.test", "swarmvtabfault.test", "symlink.test",
                "symlink2.test", "wal2.test", "walsetlk.test", "zipfile.test"]
_MIRROR_NAMES = ["inmemory_journal.swarmvtabfault-1.1-oom-persistent.143", "mm-wal2-3.3",
                 "walsetlk-2.1.3", "nothing-matches-here.1", "symlink.test-sharedcachesetting",
                 "Z:/home/rafael/src/sqlite/test/symlink2.test",
                 "/home/rafael/src/sqlite/test/symlink2.test",
                 "Z:\\home\\rafael\\src\\sqlite\\test\\symlink2.test", "symlink2.test"]
_MIRROR_SEGMENT_LOG = (b"Can't find a usable init.tcl in the following directories:\n"
                       b"alter-1.1... Ok\n"
                       b"Time: alter.test 12 ms\n"
                       b"wal2-2.1... Ok\r\n"
                       b"Time: wal2.test 34 ms\n"
                       b'"run_test_suite inmemory_journal"\n'
                       b"! walsetlk-2.1.3 expected: [1]\n"
                       b"! walsetlk-2.1.3 got: [0]\n"
                       b"!Failures on these tests: walsetlk-2.1.3 zipfile-25.0\n"
                       b'    (file "/opt/x/wal2.test" line 12)\n'
                       b'    (file "/opt/x/permutations.test" line 99)\n'
                       b"Time: zipfile.test 7 ms\n"
                       b'    (file "Z:\\opt\\x\\zipfile.test" line 7)\n'
                       b"swarmvtab.test-closeallfiles... Ok\n"
                       b"swarmvtab.test-sharedcachesetting... Ok\n"
                       b"Time: swarmvtab.test 2 ms\n"
                       b"*** Giving up...\n"
                       b"2 errors out of 41 tests on somehost Linux 64-bit\n")
# A segment log this harness really produced and then could not read (pe64-x86_64 under wine,
# re-entering symlink2.test; it died before the first `name...` line). Verbatim, CRLF re-joined.
_REAL_ABORT_SEGMENT_LOG = [
    b'Z:\\home\\rafael\\src\\dss-code-prime\\build\\real-examples\\c\\sqlite\\pe64-x86_64\\pe64-x86_64-windows-exec\\testfixture.exe: Z:\\home\\rafael\\src\\sqlite\\test\\lnk220.sym: File Not Found',
    b'    while executing',
    b'"exec -- $::env(ComSpec) /c del [file nativename $link]"',
    b'    (procedure "deleteWin32Symlink" line 2)',
    b'    invoked from within',
    b'"deleteWin32Symlink $link"',
    b'    (procedure "canCreateWin32Symlink" line 6)',
    b'    invoked from within',
    b'"canCreateWin32Symlink"',
    b'    (file "Z:/home/rafael/src/sqlite/test/symlink2.test" line 48)',
    b'    invoked from within',
    b'"source Z:/home/rafael/src/sqlite/test/symlink2.test"',
    b'    invoked from within',
    b'"interp eval tinterp $script"',
    b'    (procedure "slave_test_script" line 30)',
    b'    invoked from within',
    b'"slave_test_script [list source $zFile] "',
    b'    invoked from within',
    b'"time { slave_test_script [list source $zFile] }"',
    b'    (procedure "slave_test_file" line 23)',
    b'    invoked from within',
    b'"slave_test_file $file"',
    b'    (procedure "run_tests" line 36)',
    b'    invoked from within',
    b'"run_tests veryquick -presql {} -files {shared3.test func7.test upfrom4.test Z:/home/rafael/src/sqlite/test/../ext/fts5/test/fts5misc.test vacuum5.test..."',
    b'    ("eval" body line 1)',
    b'    invoked from within',
    b'"eval [list run_tests $suite] $S $extra"',
    b'    (procedure "main" line 34)',
    b'    invoked from within',
    b'"main $argv"',
    b'    (file "/home/rafael/src/sqlite/test/permutations.test" line 1270)',
    b'    invoked from within',
    b'"source $argv0"',
    b'    invoked from within',
    b'"if {[llength $argv]>=1} {',
    b'set new [list]',
    b'foreach arg $argv {',
    b'if {[string match -* $arg] || [file exists $arg]} {',
    b'lappend new $arg',
    b'} else {',
    b'set once 0',
    b'..."',
]
_MIRROR_CORPUS_FILES_MEASURED = ["alter.test", "swarmvtab.test", "swarmvtabfault.test", "wal2.test",
                                 "walsetlk.test", "zipfile.test"]
_MIRROR_TIER_PERMUTATIONS_MEASURED = ["veryquick", "inmemory_journal", "mmap"]
_MIRROR_TIER_PREFIXES_MEASURED = ["inmemory_journal.", "mm-"]
_MIRROR_RESOLVE_MEASURED = [
    "inmemory_journal.swarmvtabfault-1.1-oom-persistent.143\tswarmvtabfault.test",
    "mm-wal2-3.3\twal2.test", "walsetlk-2.1.3\twalsetlk.test", "nothing-matches-here.1",
    "symlink.test-sharedcachesetting\tsymlink.test",
    "Z:/home/rafael/src/sqlite/test/symlink2.test\tsymlink2.test",
    "/home/rafael/src/sqlite/test/symlink2.test\tsymlink2.test",
    "Z:\\home\\rafael\\src\\sqlite\\test\\symlink2.test\tsymlink2.test",
    "symlink2.test\tsymlink2.test"]
_MIRROR_FILES_AFTER_MEASURED = ["swarmvtabfault.test", "symlink.test", "symlink2.test", "wal2.test",
                                "walsetlk.test", "zipfile.test"]
_MIRROR_PARSE_MEASURED = [
    "alter.test", "wal2.test", "zipfile.test", "swarmvtab.test", "I swarmvtab.test",
    "X walsetlk-2.1.3", "X zipfile-25.0", "B /opt/x/wal2.test", "B Z:\\opt\\x\\zipfile.test",
    "S 2 errors out of 41 tests on somehost Linux 64-bit", "E 2", "C 41", "P inmemory_journal",
    "T swarmvtab.test-sharedcachesetting", "G 1", "N 4", "M 1", "D swarmvtab.test", "K 4", "Q 1",
    "A Can't find a usable init.tcl in the following directories:"]
# The battery's own STATED answers. The sentinel is assembled from two halves so the module
# carries its literal exactly ONCE (the constant above).
_SENTINEL_EXPECTED = "<SILENT: the fixture produced no diagnostic," + \
    " no test result and no test name>"
_MIRROR_ABORT_EXPECTED = ["B Z:/home/rafael/src/sqlite/test/symlink2.test", "T", "N 0",
                          "FILE symlink2.test"]
_MIRROR_ZERO_EXPECTED = ["1 boom: cannot open libtcl", "2 " + _SENTINEL_EXPECTED, "3", "4", "5",
                         "6 boom"]


def _project_parse(facts):
    """The old battery's projection of one parse into the fact alphabet (lines right-stripped,
    as the battery normalised both arms)."""
    rows = list(facts.files)
    rows += ["I %s" % f for f in facts.inert]
    rows += ["X %s" % x for x in sorted(set(facts.failures))]
    rows += ["B %s" % b for b in facts.blamed]
    rows += ["S %s" % facts.summary,
             "E %s" % ("" if facts.errors is None else facts.errors),
             "C %s" % ("" if facts.total is None else facts.total),
             "P %s" % facts.permutation, "T %s" % facts.last_test,
             "G %s" % ("1" if facts.gave_up else ""), "N %d" % facts.n_files,
             "M %d" % facts.n_inert, "D %s" % facts.last_file, "K %d" % facts.ok,
             "Q %d" % facts.fail_markers, "A %s" % facts.first_diag]
    return [r.rstrip() for r in rows]


def _selftest_mirror(t, work):
    t.section("MC  the old differential battery's cases, answers stated or MEASURED from both old "
              "copies")
    corpus_dir = os.path.join(work, "corpus")
    os.makedirs(corpus_dir)
    for name in _MIRROR_CORPUS_NAMES:
        _write(os.path.join(corpus_dir, name), "# %s\n" % name)
    perms = _write(os.path.join(corpus_dir, "permutations.test"), _MIRROR_PERMS)
    tier = _write(os.path.join(work, "tier.test"), _MIRROR_TIER)
    seglog = _write(os.path.join(work, "segment.log"), _MIRROR_SEGMENT_LOG)
    abortlog = _write(os.path.join(work, "abort-segment.log"),
                      b"\r\n".join(_REAL_ABORT_SEGMENT_LOG) + b"\r\n")
    cap = _CaptureLog()
    t.eq("MC1 corpus-files", _MIRROR_CORPUS_FILES_MEASURED, corpus_files(corpus_dir, log=cap))
    t.eq("MC1b ...and the exclusion block was found, so nothing was warned", "", cap.text())
    t.eq("MC2 tier-permutations", _MIRROR_TIER_PERMUTATIONS_MEASURED, tier_permutations(tier))
    t.eq("MC3 tier-prefixes", _MIRROR_TIER_PREFIXES_MEASURED, tier_prefixes(perms))
    t.eq("MC4 resolve-abort-file",
         _MIRROR_RESOLVE_MEASURED,
         [("%s\t%s" % (n, resolve_abort_file(n, _MIRROR_LIST))).rstrip() for n in _MIRROR_NAMES])
    fx = parse_segment(abortlog)
    t.eq("MC5 abort-file-from-traceback (STATED): the real wine log names symlink2.test",
         _MIRROR_ABORT_EXPECTED,
         [("B %s" % b).rstrip() for b in fx.blamed]
         + [("T %s" % fx.last_test).rstrip(), "N %d" % fx.n_files,
            ("FILE %s" % resolve_abort_file(fx.abort_file, _MIRROR_LIST)).rstrip()])
    rows = [("boom: cannot open libtcl", 0, 0, ""), ("", 0, 0, ""), ("", 5, 0, ""), ("", 0, 2, ""),
            ("", 0, 0, "select1-1.1"), ("boom", 9, 9, "x")]
    t.eq("MC6 zero-progress-signature (STATED)", _MIRROR_ZERO_EXPECTED,
         [("%d %s" % (i + 1, zero_progress_signature(
             SegmentFacts(first_diag=a, ok=k, fail_markers=q, last_test=n)))).rstrip()
          for i, (a, k, q, n) in enumerate(rows)])
    t.eq("MC7 files-after", _MIRROR_FILES_AFTER_MEASURED, files_after("swarmvtab.test", _MIRROR_LIST))
    t.eq("MC8 parse-segment (every fact, the CRLF line and the two traceback blocks included)",
         _MIRROR_PARSE_MEASURED, _project_parse(parse_segment(seglog)))


_PRECOND_LINES = ["Can't find a usable init.tcl in the following directories: /opt/local/lib/tcl8.6 "
                  "/opt/local/lib/tcl8.6 ...",
                  "This probably means that Tcl wasn't installed properly.",
                  '    (procedure "tclInit" line 61)', "    invoked from within",
                  '"interp create tinterp"', '    (procedure "slave_test_script" line 4)']
_HEALTHY_LINES = ["select1-1.1... Ok", "select1-1.2... Ok", "Time: select1.test 42 ms",
                  "misc7-7.0... Ok", "Time: misc7.test 11 ms",
                  "0 errors out of 192 tests on host Darwin 64-bit"]
_CRASH_LINES = ["select1-1.1... Ok", "Time: select1.test 42 ms",
                "swarmvtabfault-1.1-oom-persistent.143...", "child process exited abnormally",
                '    (procedure "do_test" line 12)']
_INERT_LINES = ["select1-1.1... Ok", "select1-1.2... Ok", "select1.test-closeallfiles... Ok",
                "select1.test-sharedcachesetting... Ok", "Time: select1.test 42 ms",
                "fts5aa.test-closeallfiles... Ok", "fts5aa.test-sharedcachesetting... Ok",
                "Time: fts5aa.test 2 ms", "! wherelimit-1.1 expected: [1]",
                "! wherelimit-1.1 got: [0]", "wherelimit.test-closeallfiles... Ok",
                "wherelimit.test-sharedcachesetting... Ok", "Time: wherelimit.test 7 ms",
                "0 errors out of 8 tests on host Linux 64-bit"]
_D1 = "Can't find a usable init.tcl in the following directories: /opt/local/lib/tcl8.6 ..."
_D2 = "child process exited abnormally"


def _log_of(work, name, lines, eol="\n"):
    return _write(os.path.join(work, name), eol.join(lines) + eol)


def _selftest_parser_pins(t, work):
    t.section("D   the bash twin's parser pin (precondition / healthy / crash / inert logs, LF)")
    p = parse_segment(_log_of(work, "precond.log", _PRECOND_LINES))
    t.eq("D01 precondition log: the diagnostic is captured verbatim", _PRECOND_LINES[0], p.first_diag)
    t.eq("D02 precondition log: ZERO files completed", 0, p.n_files)
    t.eq("D03 precondition log: no summary line", "", p.summary)
    h = parse_segment(_log_of(work, "healthy.log", _HEALTHY_LINES))
    t.eq("D04 healthy log: NO diagnostic is invented", "", h.first_diag)
    t.eq("D05 healthy log: files counted", 2, h.n_files)
    t.eq("D06 healthy log: last file", "misc7.test", h.last_file)
    t.eq("D07 healthy log: the summary is the WHOLE line, host suffix and all",
         "0 errors out of 192 tests on host Darwin 64-bit", h.summary)
    t.eq("D08 healthy log: ' Ok' tally", 3, h.ok)
    c = parse_segment(_log_of(work, "crash.log", _CRASH_LINES))
    t.eq("D09 crash log: files completed > 0", 1, c.n_files)
    t.eq("D10 crash log: the last test is named", "swarmvtabfault-1.1-oom-persistent.143", c.last_test)
    t.eq("D11 crash log: a diagnostic is captured too", _D2, c.first_diag)
    i = parse_segment(_log_of(work, "inert.log", _INERT_LINES))
    t.eq("D12 inert log: three files completed", 3, i.n_files)
    t.eq("D13 inert log: exactly ONE asserted nothing", 1, i.n_inert)
    t.eq("D14 inert log: and it is the one that ran nothing (BY NAME)", ["fts5aa.test"], i.inert)
    t.eq("D15 inert log: a file that only FAILED is not inert (its marker counted)", 1, i.fail_markers)
    t.eq("D16 healthy log: nothing is inert", 0, h.n_inert)

    t.section("C   the PowerShell twin's parser pin, over CRLF logs (what its Set-Content wrote)")
    p = parse_segment(_log_of(work, "precond-crlf.log", _PRECOND_LINES[:5], "\r\n"))
    t.eq("C01 precondition log: the diagnostic is captured verbatim (no CR)", _PRECOND_LINES[0],
         p.first_diag)
    t.eq("C02 precondition log: ZERO files completed", 0, p.n_files)
    t.eq("C03 precondition log: no summary line", "", p.summary)
    h = parse_segment(_log_of(work, "healthy-crlf.log", _HEALTHY_LINES, "\r\n"))
    t.eq("C04 healthy log: NO diagnostic is invented", "", h.first_diag)
    t.eq("C05 healthy log: files counted", 2, h.n_files)
    t.eq("C06 healthy log: last file", "misc7.test", h.last_file)
    t.eq("C07 healthy log: summary is the WHOLE line, not just the counts",
         "0 errors out of 192 tests on host Darwin 64-bit", h.summary)
    t.eq("C08 healthy log: ' Ok' tally", 3, h.ok)
    c = parse_segment(_log_of(work, "crash-crlf.log", _CRASH_LINES, "\r\n"))
    t.eq("C09 crash log: files completed > 0", 1, c.n_files)
    t.eq("C10 crash log: the last test is named", "swarmvtabfault-1.1-oom-persistent.143", c.last_test)
    t.eq("C11 crash log: a diagnostic is captured too", _D2, c.first_diag)


def _takes(files_done, diag, prev_sig, ok=0, fx=0, last=""):
    """One decision of the discriminator over facts shaped like a parse's (the SHIPPED signature
    derivation computes this segment's side; only the previous carry is handed in)."""
    facts = SegmentFacts(first_diag=diag, files=["f%d.test" % k for k in range(files_done)], ok=ok,
                         fail_markers=fx, last_test=last)
    return "PRECONDITION" if is_precondition_failure(prev_sig, facts) else "RESUME"


def _selftest_discriminator(t, work):
    t.section("E   the precondition discriminator (both twins' single-decision arms + the case arm)")
    silent = zero_progress_signature(SegmentFacts())
    t.eq("E01 zero progress twice, IDENTICAL diagnostic -> PRECONDITION", "PRECONDITION",
         _takes(0, _D1, _D1))
    t.eq("E02 the FIRST such abort -> RESUME", "RESUME", _takes(0, _D1, ""))
    t.eq("E03 zero progress, DIFFERENT diagnostic -> RESUME", "RESUME", _takes(0, _D2, _D1))
    t.eq("E04 a crash AFTER completing files -> RESUME", "RESUME", _takes(7, _D1, _D1))
    t.eq("E05 one file completed, same diagnostic -> RESUME", "RESUME", _takes(1, _D1, _D1))
    t.eq("E06 zero progress, NO OUTPUT, first time -> RESUME", "RESUME", _takes(0, "", ""))
    t.eq("E07 SILENCE TWICE -> PRECONDITION", "PRECONDITION", _takes(0, "", silent))
    t.eq("E08 no diagnostic but ' Ok' lines -> RESUME", "RESUME", _takes(0, "", silent, ok=5))
    t.eq("E09 no diagnostic but a FAILURE marker -> RESUME", "RESUME", _takes(0, "", silent, fx=2))
    t.eq("E10 no diagnostic but a test NAME -> RESUME", "RESUME",
         _takes(0, "", silent, last="select1-1.1"))
    t.eq("E11 two diagnostics differing only in CASE -> RESUME (ordinal, never case-folded)",
         "RESUME", _takes(0, "Cannot Open Libtcl", "cannot open libtcl"))
    empty = _write(os.path.join(work, "silent.log"), b"")
    t.eq("E12 the silent fixture's log really is ZERO BYTES", 0, os.path.getsize(empty))
    s1 = parse_segment(empty)
    t.eq("E13 a parsed ZERO-BYTE log signs as the sentinel", _SENTINEL_EXPECTED,
         zero_progress_signature(s1))
    same = parse_segment(_write(os.path.join(work, "same.log"), _D1 + "\n"))
    t.eq("E14 a parsed one-line talking log signs as its diagnostic", _D1,
         zero_progress_signature(same))
    # The carry, pairwise (the resume LOOP and its budget are sqlite_units.py's): the first silent
    # segment resumes, the second is the precondition failure.
    first = is_precondition_failure("", s1)
    carry = zero_progress_signature(s1) if s1.n_files == 0 else ""
    second = is_precondition_failure(carry, parse_segment(empty))
    t.eq("E15 two parsed SILENT segments in a row: resume, then PRECONDITION", [False, True],
         [first, second])
    verdicts, prev = [], ""
    for k in range(1, 13):
        fk = parse_segment(_write(os.path.join(work, "diff.%d.log" % k),
                                  "child process exited abnormally in file %d\n" % k))
        verdicts.append(is_precondition_failure(prev, fk))
        prev = zero_progress_signature(fk) if fk.n_files == 0 else ""
    t.eq("E16 twelve parsed segments dying DIFFERENTLY: never a precondition failure",
         [False] * 12, verdicts)
    t.eq("E17 ...while twelve IDENTICAL ones would be (the E16 instrument can say yes)",
         [False] + [True] * 11,
         [is_precondition_failure("" if k == 0 else _D1, same) for k in range(12)])
    t.check("E18 the sentinel is ASCII and byte-exact", SILENT_SENTINEL == _SENTINEL_EXPECTED
            and SILENT_SENTINEL.isascii(), repr(SILENT_SENTINEL))


def _selftest_parser_edges(t, work):
    t.section("X   parser edges: line endings, truncation, rule order")
    lf = parse_segment(_log_of(work, "h-lf.log", _INERT_LINES))
    crlf = parse_segment(_log_of(work, "h-crlf.log", _INERT_LINES, "\r\n"))
    t.eq("X01 a CRLF log parses to the SAME facts as its LF twin", _project_parse(lf),
         _project_parse(crlf))
    lone = parse_segment(_write(os.path.join(work, "lone-cr.log"),
                                b"select1-1.1... Ok\rTime: select1.test 1 ms\n"))
    t.eq("X02 a lone CR does NOT end a line (no file completed, no ' Ok' at its end)",
         (0, 0, "select1-1.1"), (lone.n_files, lone.ok, lone.last_test))
    split = parse_segment(_write(os.path.join(work, "lone-cr-control.log"),
                                 b"select1-1.1... Ok\nTime: select1.test 1 ms\n"))
    t.eq("X03 ...control: the same bytes with LF complete the file", (1, 1),
         (split.n_files, split.ok))
    two = parse_segment(_write(os.path.join(work, "cr-cr.log"), b"select1-1.1... Ok\r\r\n"))
    one = parse_segment(_write(os.path.join(work, "cr.log"), b"select1-1.1... Ok\r\n"))
    t.eq("X04 exactly ONE trailing CR is removed (a second one stays, so no ' Ok')", (0, 1),
         (two.ok, one.ok))
    for n, want_cut in ((DIAG_LIMIT, False), (DIAG_LIMIT + 1, True), (DIAG_LIMIT + 50, True)):
        text = ("E" * (n - 3)) + "\tx!"
        got = parse_segment(_write(os.path.join(work, "diag-%d.log" % n), text + "\n")).first_diag
        flat = text.replace("\t", " ")
        want = flat[:DIAG_LIMIT] + TRUNCATION_SUFFIX if want_cut else flat
        t.eq("X05 a %d-character diagnostic is %s (tabs -> spaces, ASCII suffix)"
             % (n, "capped at %d" % DIAG_LIMIT if want_cut else "kept whole"), want, got)
    t.check("X06 the truncation suffix is ASCII", TRUNCATION_SUFFIX.isascii(), repr(TRUNCATION_SUFFIX))
    s = parse_segment(_log_of(work, "sums.log", ["1 error out of 5 tests", "x",
                                                 "3 errors out of 70 tests on h"]))
    t.eq("X07 the LAST summary line wins, singular `error` read too",
         ("3 errors out of 70 tests on h", 3, 70), (s.summary, s.errors, s.total))
    t.eq("X08 no summary line -> errors and total are None (not 0)", (None, None),
         (lone.errors, lone.total))
    g = parse_segment(_log_of(work, "gave.log", ["*** Giving up...", "0 errors out of 1 tests"]))
    t.eq("X09 `*** Giving up` is recorded, and a summary after it still read", (True, 1),
         (g.gave_up, g.total))
    t.eq("X10 ...and a log without it says so", False, s.gave_up)
    f = parse_segment(_log_of(work, "fails.log", ["Failures on these tests:  a-1\tb-2  ",
                                                  "!Failures on these tests: c-3",
                                                  "! d-4 expected: [1]", "! d-4 got: [2]"]))
    t.eq("X11 failure lists (with/without the bang, blanks/tabs) and `!` markers, in order",
         ["a-1", "b-2", "c-3", "d-4", "d-4"], f.failures)
    t.eq("X12 ...one expected: per failed test is the failure tally", 1, f.fail_markers)
    b = parse_segment(_log_of(work, "blame.log", [
        '    (file "" line 3)', '    (file "/x/second.test" line 9)', "Time: a.test 1 ms",
        '    (file "/x/third.test" line 1)', "b-1... Ok", '    (file "/x/fourth.test" line 2)',
        '    (file "/x/fifth.test" line 2)']))
    t.eq("X13 one frame per traceback block; an EMPTY frame still spends its block's turn; a "
         "Time/Ok line opens a new block", ["/x/third.test", "/x/fourth.test"], b.blamed)
    t.eq("X14 abort_file is the LAST block's frame", "/x/fourth.test", b.abort_file)
    pm = parse_segment(_log_of(work, "perm.log", ['  "run_test_suite inmemory_journal"',
                                                  "run_tests veryquick -presql {} -files {a.test}"]))
    t.eq("X15 the permutation, quote abutting or run_tests form; the last wins", "veryquick",
         pm.permutation)
    tm = parse_segment(_log_of(work, "time.log", ["x-1... Ok", "Time: a.test abc ms",
                                                  "Time: b.test 1 ms extra", "Time: c.test 12 s",
                                                  "Time: 3 errors out of 9 tests"]))
    t.eq("X16 `Time:` needs EXACTLY four fields ending `ms` (the third is not checked)",
         ["a.test"], tm.files)
    t.eq("X17 a `Time:` line that completes no file still reaches the later rules (here the "
         "summary rule)", "Time: 3 errors out of 9 tests", tm.summary)
    bad = parse_segment(_write(os.path.join(work, "bytes.log"),
                               b"\xff\xfe broken \x00 bytes\nselect1-1.1... Ok\n"))
    t.eq("X18 undecodable bytes and NUL are replaced, never fatal", ("\ufffd\ufffd broken \x00 bytes",
                                                                    1), (bad.first_diag, bad.ok))
    t.eq("X19 derived_count is K + Q + 1", lf.ok + lf.fail_markers + 1, lf.derived_count)
    t.eq("X20 a summary-looking diagnostic is not a diagnostic, a ` Ok` line is not either", "",
         parse_segment(_log_of(work, "nodiag.log", ["0 errors out of 3 tests", "a... Ok",
                                                    "Time: a.test 1 ms"])).first_diag)
    try:
        parse_segment(os.path.join(work, "no-such.log"))
        refused = False
    except C.HarnessDie:
        refused = True
    t.check("X21 a MISSING segment log is refused, never parsed as an empty one", refused)


def _selftest_resolver_and_lists(t):
    t.section("R   the aborting-file resolver")
    t.eq("R01 an empty name resolves to nothing", "", resolve_abort_file("", _MIRROR_LIST))
    t.eq("R02 a bare directory resolves to nothing", "", resolve_abort_file("/x/y/", _MIRROR_LIST))
    t.eq("R03 RIGHTMOST wins over longest", "bar.test",
         resolve_abort_file("x-foo.bar-1", ["foo.test", "bar.test"]))
    t.eq("R04 a tie on position goes to the LONGER stem", "a-b.test",
         resolve_abort_file("a-b.1", ["a.test", "a-b.test"]))
    t.eq("R05 a stem followed by a non-delimiter is not a match", "",
         resolve_abort_file("walsetlkx-1", ["walsetlk.test"]))
    t.eq("R06 an EMPTY corpus entry is skipped (the awk twin would have looped on it)", "wal2.test",
         resolve_abort_file("wal2-1.1", ["", ".test", "wal2.test"]))
    t.eq("R07 the last occurrence of a stem is the one that counts", "mm.test",
         resolve_abort_file("mm-x.mm-2", ["mm.test", "x.test"]))
    t.section("L   ordinal helpers and the digit grouper")
    t.eq("L01 files_after('') is the whole list", _MIRROR_LIST, files_after("", _MIRROR_LIST))
    t.eq("L02 files_after past the end is empty", [], files_after("zz.test", _MIRROR_LIST))
    t.eq("L03 ordinal: symlink.test sorts BEFORE symlink2.test ('.' < '2')", ["symlink2.test"],
         files_after("symlink.test", ["symlink.test", "symlink2.test"]))
    t.eq("L04 first_file_after names the next file", "symlink2.test",
         first_file_after("symlink.test", _MIRROR_LIST))
    t.eq("L05 first_file_after past the end is None", None, first_file_after("zz", _MIRROR_LIST))
    t.eq("L06 first_file_after('') is the first file", "alter.test", first_file_after("", _MIRROR_LIST))
    t.eq("L07 group_digits", ["0", "999", "1,000", "12,345", "4,200,000", "1,234,567,890"],
         [group_digits(n) for n in (0, 999, 1000, 12345, 4200000, 1234567890)])


def _selftest_tree_readers(t, work):
    t.section("T   reading sqlite's own tree")
    bare = os.path.join(work, "bare")
    os.makedirs(bare)
    for name in ("b.test", "a.test", ".hidden.test", "UPPER.TEST", "notes.txt"):
        _write(os.path.join(bare, name), "#\n")
    cap = _CaptureLog()
    t.eq("T01 no permutations.test: every *.test, code-point order, no dot-file, case-sensitive "
         "suffix", ["a.test", "b.test"], corpus_files(bare, log=cap))
    t.eq("T02 ...and no warning (there is no exclude list to miss)", "", cap.text())
    nob = os.path.join(work, "noblock")
    os.makedirs(nob)
    _write(os.path.join(nob, "x.test"), "#\n")
    _write(os.path.join(nob, "permutations.test"), "test_suite \"x\" -description {\n}\n")
    cap = _CaptureLog()
    t.eq("T03 a permutations.test with no exclude block: the raw glob (a superset)",
         ["permutations.test", "x.test"], corpus_files(nob, log=cap))
    t.check("T04 ...and that miss is WARNED, never silent", "exclude list" in cap.text(), cap.text())
    crlf = os.path.join(work, "crlfperms")
    os.makedirs(crlf)
    for name in ("a.test", "all.test", "permutations.test"):
        _write(os.path.join(crlf, name), "#\n")
    _write(os.path.join(crlf, "permutations.test"),
           "set alltests [test_set $alltests -exclude { a.test\r\n  all.test\r\n"
           "  permutations.test\r\n}]\r\n")
    t.eq("T05 a CRLF exclude block is read; the OPENING line's own words are not (a.test stays)",
         ["a.test"], corpus_files(crlf, log=_CaptureLog()))
    try:
        corpus_files(os.path.join(work, "no-such-dir"), log=_CaptureLog())
        refused = False
    except C.HarnessDie:
        refused = True
    t.check("T06 a missing corpus directory is refused", refused)
    pf = _write(os.path.join(work, "perms-ties.test"),
                'test_suite "abd" -description {\n'
                'test_suite "abc" -description {\n'
                '  test_suite "zz" -prefix "zz." -description {\n'
                '# test_suite "commented" -prefix "cm-" {\n'
                'test_suite "abc" -description {\n'
                'test_suite "none" -prefix "" -description {\n')
    t.eq("T07 prefixes: longest first, ties in reverse code-point order, deduplicated, "
         "indented read, commented and empty ones not", ["abd.", "abc.", "zz."], tier_prefixes(pf))
    t.eq("T08 a missing permutations.test has no prefixes", [],
         tier_prefixes(os.path.join(work, "absent.test")))
    tf = _write(os.path.join(work, "tier-edges.test"),
                "ifcapable rbu { run_test_suite rbu }\n"
                "my_run_test_suite notme\n"
                "run_test_suite-x notme\n"
                "    run_test_suite autovacuum_crash\r\n")
    t.eq("T09 permutations: anywhere on the line, not the tail of a longer word, CR tolerated",
         ["rbu", "autovacuum_crash"], tier_permutations(tf))


def self_test():
    import shutil
    import tempfile
    import traceback
    t = _Checks()
    work = tempfile.mkdtemp(prefix="dss-corpus-selftest-")
    try:
        for name, fn in (("mirror", lambda: _selftest_mirror(t, os.path.join(work, "mirror"))),
                         ("parser pins", lambda: _selftest_parser_pins(t, work)),
                         ("discriminator", lambda: _selftest_discriminator(t, work)),
                         ("parser edges", lambda: _selftest_parser_edges(t, work)),
                         ("resolver", lambda: _selftest_resolver_and_lists(t)),
                         ("tree readers", lambda: _selftest_tree_readers(t, work))):
            try:
                if name == "mirror":
                    os.makedirs(os.path.join(work, "mirror"))
                fn()
            except Exception:  # noqa: BLE001 -- a crashed group is a FAILURE, never a skip
                t.check("group '%s' ran to completion" % name, False, traceback.format_exc())
    finally:
        shutil.rmtree(work, ignore_errors=True)
    print("passed=%d failed=%d skipped=%d" % (t.passed, t.failed, t.skipped))
    return 0 if t.failed == 0 else 1


def main(argv=None):
    args = list(sys.argv[1:] if argv is None else argv)
    if args == ["--self-test"]:
        return self_test()
    if args in (["-h"], ["--help"]):
        print(__doc__)
        return 0
    print("usage: sqlite_corpus.py --self-test   (a library of the sqlite harness; it has no other "
          "verb)", file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main())
