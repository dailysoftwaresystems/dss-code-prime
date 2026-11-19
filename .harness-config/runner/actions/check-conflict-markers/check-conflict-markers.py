#!/usr/bin/env python3
# PURPOSE: refuse a line that is a merge-conflict marker git writes -- seven `<`, seven vertical bars or seven `>` at its start, and seven `=` alone between an opener and its closer -- in any text file of the tree git tracks or would add.
"""check-conflict-markers.py -- NO CONFLICT LEFT IN THE TREE.

A merge, a rebase, a cherry-pick or a stash that stops on a conflict writes its markers into the file: a line
that begins with seven `<` (the opener, then the name of one side), seven `|` (the common base, in the diff3
styles), seven `=` alone (the separator) and seven `>` (the closer, then the name of the other side). A file
committed with them sometimes still compiles -- inside a comment, a string table, a document -- and is always
wrong. This guard refuses every such line in every text file of the tree: tracked, and untracked but not ignored
(what a commit could take), as the working tree holds it.

WHAT A MARKER IS: the seven characters at the start of a line, then the end of the line, a space or a CR. Seven
is git's marker size; no file of this tree sets the `conflict-marker-size` attribute that would change it.
An opener, a base and a closer are refused wherever they stand. A separator is refused only between an opener and
the closer after it in the same file: seven `=` alone is also a heading's underline in Markdown and in
reStructuredText, and outside a conflict that is what it is.

HOW: `git grep -I -n -z --untracked -E` for a line beginning with seven of one of the four characters, asked
through the tree's identity in git's unsteered environment (`owning-tree.py`) -- a superset; each hit is then
judged here (`marker_kind`, `judge`). A binary file (`-I`) is not read: a marker is text. The program's own
markers are composed at run time, so no line of it is one.

FAIL LOUD: git failing is exit 2, never "nothing found"; so is a scan that read fewer text files than its floor
(git grep reads only what git lists, and a tree it cannot list returns nothing, which is also what a clean tree
returns). The self-test proves each rule in both directions, end to end through a conflict git itself writes.

usage:
  check-conflict-markers.py               judge the tree this program lives in (ctest `conflict_markers_guard`)
  check-conflict-markers.py --self-test   prove each rule, one verdict line per arm (`self-test` step; ctest
                                          entry `harness/conflict_markers_selftest`)
exit: 0 OK . 1 a marker found . 2 cannot judge . 3 usage
"""
import importlib.util
import os
import sys
import tempfile

sys.dont_write_bytecode = True  # a by-path load must not write __pycache__ beside another action

# A cp1252 console turns a printed glyph into a traceback; reconfigure before anything can print (the property
# `guard_output_encoding_guard` ratchets for every program).
for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError, OSError):  # pragma: no cover - an odd stream
        pass

TOOL = "conflict-markers"
SELFTEST_MARKER = TOOL + " self-test: "
# ⚠ Raising this is the claim that the arms you added actually RUN.
EXPECTED_ARMS = 7
# The fewest text files a scan of THIS tree may read before its silence is taken for a clean tree.
SCAN_FLOOR = 1500
SIZE = 7
OPENER, BASE, SEPARATOR, CLOSER = "<" * SIZE, "|" * SIZE, "=" * SIZE, ">" * SIZE
NAMES = {OPENER: "an opener", BASE: "a base marker", SEPARATOR: "a separator inside a conflict",
         CLOSER: "a closer"}
# The superset git is asked: a line beginning with seven of one of the four characters.
QUERY = "^(<{%d}|[|]{%d}|={%d}|>{%d})" % (SIZE, SIZE, SIZE, SIZE)

_OT = None


class Unmeasured(Exception):
    """This guard could not answer -- exit 2, never a pass."""


def owning_tree_module():
    """`owning-tree.py`, loaded as a SIBLING by path. A missing owner is exit 2 -- never a fallback to the
    caller's cwd."""
    global _OT
    if _OT is None:
        path = os.path.join(os.path.dirname(os.path.dirname(os.path.realpath(__file__))),
                            "owning-tree", "owning-tree.py")
        if not os.path.isfile(path):
            raise Unmeasured("cannot find %s -- this guard's tree is resolved there and nowhere else" % path)
        spec = importlib.util.spec_from_file_location("dss_owning_tree", path)
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        _OT = mod
    return _OT


def marker_kind(line):
    """The marker `line` is (OPENER, BASE, SEPARATOR or CLOSER), or None: seven of one character at its start,
    then its end, a space or a CR -- a separator only alone (its end, or a CR before it)."""
    for kind in (OPENER, BASE, SEPARATOR, CLOSER):
        if line.startswith(kind):
            rest = line[SIZE:]
            if kind == SEPARATOR:
                return kind if rest in ("", "\r") else None
            return kind if rest == "" or rest[0] in " \r" else None
    return None


def judge(hits):
    """`hits` -- (path, line number, text), in each file's line order -> the offences, `path:line: what`. An
    opener, a base and a closer are each an offence; a separator only between an opener and the closer after it
    in the same file."""
    offences, inside = [], {}
    for path, line_no, text in hits:
        kind = marker_kind(text)
        if kind is None:
            continue
        if kind == OPENER:
            inside[path] = True
        elif kind == CLOSER:
            inside[path] = False
        elif kind == SEPARATOR and not inside.get(path):
            continue   # a heading's underline: outside a conflict, seven `=` alone is no marker
        offences.append("%s:%d: %s" % (path, line_no, NAMES[kind]))
    return offences


def parse_grep_n_z(out):
    """`path\\0line\\0text\\n` records -> [(path, line, text)], decoded without loss."""
    hits, pos = [], 0
    while pos < len(out):
        a = out.find(b"\0", pos)
        b = out.find(b"\0", a + 1) if a >= 0 else -1
        if a < 0 or b < 0:
            break
        c = out.find(b"\n", b + 1)
        c = len(out) if c < 0 else c
        hits.append((out[pos:a].decode("utf-8", "surrogateescape"), int(out[a + 1:b]),
                     out[b + 1:c].decode("utf-8", "surrogateescape")))
        pos = c + 1
    return hits


def git(ident, args, ok=(0,)):
    """`git <args>` asked THROUGH `ident`, in git's unsteered environment. An exit code outside `ok` is
    UNMEASURED, carrying git's own first line -- never read as "nothing found"."""
    p = owning_tree_module().tree_git(ident, args)
    if p.returncode not in ok:
        first = p.stderr.decode("utf-8", "replace").strip().split("\n")[0][:200]
        raise Unmeasured("`git %s` exited %d%s -- this guard cannot read the tree through it, so it refuses to "
                         "report a verdict" % (" ".join(args), p.returncode, (": " + first) if first else ""))
    return p.stdout


def scan(ident, floor=SCAN_FLOOR):
    """-> (offences, how many text files the scan read). Raises Unmeasured below the floor."""
    read = [ln for ln in git(ident, ["grep", "-I", "-l", "-z", "--untracked", "-e", "", "--"],
                             ok=(0, 1)).split(b"\0") if ln]
    if len(read) < floor:
        raise Unmeasured("the scan read %d text file(s), below its floor of %d: git grep reads only what git "
                         "lists, so a tree it cannot list looks clean -- this does NOT mean the tree is clean"
                         % (len(read), floor))
    hits = parse_grep_n_z(git(ident, ["grep", "-I", "-n", "-z", "--untracked", "-E", QUERY, "--"], ok=(0, 1)))
    hits.sort(key=lambda h: (h[0], h[1]))
    return judge(hits), len(read)


def verify():
    ot = owning_tree_module()
    try:
        ident = ot.identity(ot.resolve(__file__, reads_git=True))
    except ot.Refusal as exc:
        raise Unmeasured(str(exc))
    offences, read = scan(ident)
    if offences:
        print("%s: FAIL -- %d merge-conflict marker line(s); settle each conflict by hand and remove its "
              "markers:" % (TOOL, len(offences)), file=sys.stderr)
        for o in offences:
            print("  " + o, file=sys.stderr)
        return 1
    print("%s: OK (%d text file(s), tracked or untracked and not ignored, read; no conflict marker)"
          % (TOOL, read))
    return 0


# ── the self-test ───────────────────────────────────────────────────────────────────────────────

def _fixture(box, name, files):
    """A fresh repository at `box/name` holding `files` ({path: text or bytes}), none of them added."""
    ot = owning_tree_module()
    repo = os.path.join(box, name)
    for rel, data in files.items():
        full = os.path.join(repo, *rel.split("/"))
        os.makedirs(os.path.dirname(full), exist_ok=True)
        with open(full, "wb") as fh:
            fh.write(data if isinstance(data, bytes) else data.encode("utf-8"))
    os.makedirs(repo, exist_ok=True)
    ot.run_git(["init", "-q", repo], capture_output=True)
    return ot.TreeIdentity(os.path.realpath(repo), None, "", "")


def _conflicted(box):
    """A repository in which `git merge` stopped on a conflict, in the diff3 style: one file, one line two
    branches changed. -> its identity."""
    ot = owning_tree_module()
    repo = os.path.join(box, "merge")
    os.makedirs(repo)
    settings = ["-c", "user.name=fixture", "-c", "user.email=fixture@example.invalid", "-c",
                "commit.gpgsign=false", "-c", "merge.conflictStyle=diff3"]

    def run(*args, ok=(0,)):
        p = ot.run_git(["-C", repo] + settings + list(args), capture_output=True)
        if p.returncode not in ok:
            raise Unmeasured("the fixture's `git %s` exited %d: %s" % (
                " ".join(args), p.returncode, p.stderr.decode("utf-8", "replace").strip()[:160]))
    run("init", "-q", "-b", "one")
    with open(os.path.join(repo, "f.txt"), "w", encoding="utf-8", newline="\n") as fh:
        fh.write("head\nvalue = 0\ntail\n")
    run("add", "f.txt")
    run("commit", "-q", "--no-verify", "-m", "base")
    run("checkout", "-q", "-b", "two")
    with open(os.path.join(repo, "f.txt"), "w", encoding="utf-8", newline="\n") as fh:
        fh.write("head\nvalue = 2\ntail\n")
    run("commit", "-q", "--no-verify", "-am", "two")
    run("checkout", "-q", "one")
    with open(os.path.join(repo, "f.txt"), "w", encoding="utf-8", newline="\n") as fh:
        fh.write("head\nvalue = 1\ntail\n")
    run("commit", "-q", "--no-verify", "-am", "one")
    run("merge", "-q", "--no-edit", "two", ok=(1,))   # 1: the merge stopped on the conflict, as wanted
    return ot.TreeIdentity(os.path.realpath(repo), None, "", "")


def self_test():
    ot = owning_tree_module()
    ran, failed = [], []

    def arm(label, fn):
        try:
            res = fn()
            ok, detail = res if isinstance(res, tuple) else (bool(res), "")
        except Exception as exc:  # noqa: BLE001 - a crashing arm FAILS, it never aborts the rest
            ok, detail = False, "CRASHED %s: %s" % (type(exc).__name__, exc)
        ran.append(label)
        if ok:
            print("%sok      %s" % (SELFTEST_MARKER, label))
        else:
            failed.append(label)
            print("%sFAILED  %s -- %s" % (SELFTEST_MARKER, label, (detail or "no detail").replace("\n", " ")[:600]))

    def m1():
        lines = [OPENER + " HEAD", BASE + " merged common ancestors", SEPARATOR, CLOSER + " two",
                 OPENER, CLOSER + "\r", SEPARATOR + "\r", BASE + " base\r"]
        got = [marker_kind(x) for x in lines]
        want = [OPENER, BASE, SEPARATOR, CLOSER, OPENER, CLOSER, SEPARATOR, BASE]
        return got == want, "read as %r" % [NAMES.get(g, "none") for g in got]
    arm("M1 each marker as git writes it is read as one: with a side's name, alone, and with a CR before the end", m1)

    def m2():
        lines = ["<" * 8 + " x", "=" * 8, ">" * 6 + " x", "|" * 7 + "x", SEPARATOR + " x", "x" + OPENER,
                 " " + CLOSER, "<" * 3 + ">" * 4]
        got = [marker_kind(x) for x in lines]
        return got == [None] * len(lines), "read as markers: %r" % [x for x, g in zip(lines, got) if g]
    arm("M2 eight of a character, six, seven then a letter, a separator then text, and a marker not at the start "
        "are no marker", m2)

    def m3():
        heading = [("doc.md", 2, SEPARATOR)]
        conflict = [("a.c", 1, OPENER + " HEAD"), ("a.c", 3, SEPARATOR), ("a.c", 5, CLOSER + " two"),
                    ("a.c", 9, SEPARATOR)]
        h, c = judge(heading), judge(conflict)
        return (h == [] and [o.split(":")[1] for o in c] == ["1", "3", "5"],
                "heading refused as %r; conflict read as %r" % (h, c))
    arm("M3 a separator is refused only between an opener and its closer: alone, or after the closer, it is a "
        "heading's underline", m3)

    box = os.path.realpath(tempfile.mkdtemp(prefix="conflict-markers-selftest-"))
    try:
        ot.fence(box)
        # The fixtures' git reads no configuration of the host's: a global and a system file that do not exist,
        # set in this process's environment for the arms' duration (every git here is started from it).
        env = {"GIT_CONFIG_GLOBAL": os.path.join(box, "no-such-gitconfig"),
               "GIT_CONFIG_SYSTEM": os.path.join(box, "no-such-gitconfig"), "GIT_CONFIG_NOSYSTEM": "1"}
        saved = {k: os.environ.get(k) for k in env}
        os.environ.update(env)
        try:
            def m4():
                ident = _conflicted(box)
                offences, _read = scan(ident, floor=1)
                kinds = [o.split(": ", 1)[1] for o in offences]
                return (kinds == [NAMES[OPENER], NAMES[BASE], NAMES[SEPARATOR], NAMES[CLOSER]]
                        and all(o.startswith("f.txt:") for o in offences), "offences %r" % offences)
            arm("M4 a conflict git itself writes, in the diff3 style, is refused line by line: the opener, the base, "
                "the separator and the closer, each named with its path and line", m4)

            def m5():
                ident = _fixture(box, "kinds", {
                    ".gitignore": "ignored.txt\n",
                    "untracked.txt": "x\n" + OPENER + " HEAD\n",
                    "ignored.txt": OPENER + " HEAD\n",
                    "binary.dat": b"\0\1" + (OPENER + " HEAD\n").encode("ascii")})
                offences, _read = scan(ident, floor=1)
                want = "%s:%d: %s" % ("untracked.txt", 2, NAMES[OPENER])   # the offence's own format
                return offences == [want], "offences %r" % offences
            arm("M5 an untracked file git would add is read; a file git ignores and a binary file are not", m5)

            def m6():
                ident = ot.TreeIdentity(os.path.join(box, "not-a-repository"), None, "", "")
                os.makedirs(ident.root)
                try:
                    scan(ident, floor=1)
                except Unmeasured as exc:
                    return "exited" in str(exc), str(exc)[:120]
                return False, "a directory git cannot read was judged"
            arm("M6 a tree git cannot read is UNMEASURED, never clean", m6)

            def m7():
                ident = _fixture(box, "clean", {"a.md": "Title\n" + SEPARATOR + "\n", "b.txt": "fine\n"})
                offences, read = scan(ident, floor=1)
                try:
                    scan(ident, floor=3)
                    below = "accepted"
                except Unmeasured as exc:
                    below = "refused" if "below its floor" in str(exc) else str(exc)[:80]
                return (offences == [] and read == 2 and below == "refused",
                        "offences %r, read %d, a floor above it %s" % (offences, read, below))
            arm("M7 a clean tree is OK and says how many text files it read; a scan below its floor is UNMEASURED",
                m7)
        finally:
            for k, v in saved.items():
                if v is None:
                    os.environ.pop(k, None)
                else:
                    os.environ[k] = v
    except Exception as exc:  # noqa: BLE001 - a FIXTURE crash stops the arms after it, by name
        print("%sfixture construction crashed after %d arm(s): %s: %s" % (SELFTEST_MARKER, len(ran),
                                                                          type(exc).__name__, exc))
        failed.append("fixture construction")
    finally:
        ot.remove_tree(box)
    if len(ran) != EXPECTED_ARMS:
        print("%s%d arm(s) ran, %d expected -- FAIL" % (SELFTEST_MARKER, len(ran), EXPECTED_ARMS))
        return 2
    print("%s%d arm(s), %d failed%s" % (SELFTEST_MARKER, len(ran), len(failed), "" if not failed else " -- FAIL"))
    return 0 if not failed else 2


def main(argv):
    if argv == ["--self-test"]:
        return self_test()
    if argv:
        print("%s: FAIL -- unknown arguments %r (see the header: no argument, or --self-test)" % (TOOL, argv),
              file=sys.stderr)
        return 3
    try:
        return verify()
    except Unmeasured as exc:
        print("%s: FAIL -- %s" % (TOOL, exc), file=sys.stderr)
        return 2


if __name__ == "__main__":
    try:
        code = main(sys.argv[1:])
    except Exception as exc:  # noqa: BLE001 - a crash is UNMEASURED (2), never a finding (1)
        print("%s: FAIL -- crashed: %s: %s" % (TOOL, type(exc).__name__, exc), file=sys.stderr)
        code = 2
    sys.stdout.flush()
    sys.stderr.flush()
    sys.exit(code)
