#!/usr/bin/env python3
# PURPOSE: refuse a carriage script whose repository path disagrees with the project's own declared name.
"""check-carriage-paths.py -- the MULTI-HOST CARRIAGE guard.

★★★ WHY THIS EXISTS, and it is a measured break rather than a tidiness rule.

Every script that reaches ANOTHER HOST carries a hard-coded repository directory
name: `scripts/wsl-leg/wsl-leg.sh` (`SRC`, `DST`), `scripts/remote-leg/remote-leg.sh`
(`REMOTE_DIR`, twice), `scripts/profile-compile/profile-compile-dispatch.sh`
(`RREPO`, three legs), `real-examples/c/sqlite/build-and-test.sh` and
`benchmark-speedtest1.sh` (`SRC_DIR`). Until this guard, NO `ctest` entry read any
of them.

✔MEASURED 2026-08-24 (cycle P32, D-GATE-NO-CTEST-ENTRY-SEES-THE-MULTI-HOST-CARRIAGE-SCRIPTS):
a global `dss-code-prime` -> `dsscp` rename rewrote THIRTEEN such paths at once,
pointing every remote leg at directories that exist on no host -- and the full
suite reported **1603/1603 PASS**. Exactly one of the thirteen reddened anything,
and by accident: `plan_citations_guard`, because its ceiling key stopped naming a
real document. The other twelve reddened nothing at all.

★★ THE DAMAGE IS NOT THAT THEY BREAK, IT IS THAT THEY BREAK SILENTLY. This
project's 3-leg rule (Windows ctest + WSL x86_64 + qemu arm64) is a standing
requirement before every commit, so a broken carriage does not inconvenience one
cycle -- it removes two thirds of the gate, without a diagnostic, until somebody
happens to run a leg.

★★★ THE INVARIANT, AND WHY IT IS NOT "MATCH THE CHECKOUT DIRECTORY".

A checkout may legitimately live in a differently-named directory (a fork, a
worktree, a CI runner's `s/` directory), so comparing against the local
directory's basename would red honest trees. What is NOT free to vary is
AGREEMENT: the remote checkouts are named for the PROJECT, and the project
declares its own name exactly once, in `project(<name> ...)` in the root
`CMakeLists.txt`. So the invariant is:

    every repository path a carriage script names must end in the project name
    as declared by `project()` -- and they must therefore all agree with each
    other.

That is decidable offline, on any host, with no ssh and no network, which is what
lets it be a `ctest` entry rather than a leg-only check.

★★ IT KEYS ON THE VARIABLE'S ROLE, NOT ON A LIST OF FILES. A hard-coded list of
the five known carriage scripts would cover a new one only on the day somebody
remembered to add it -- the same enumerate-don't-state-the-rule failure that
produced the break this guard exists for (that rename's protect list named five
SPELLINGS and therefore could not discover a sixth class). Instead it finds
assignments whose VARIABLE NAME says the value is a repository root, anywhere
under the scanned trees.

★★ AND IT HAS A FLOOR, because a role-keyed scan's failure mode is to find
NOTHING. Rename every variable, move the trees, break the regex, and a scan
without a floor reports success over an empty set -- the vacuous-check failure
this repository has now met several times. The floor refuses a collapsed scan
before it can be mistaken for a clean one.

USAGE
    python scripts/check-carriage-paths/check-carriage-paths.py           # verify + self-test
    python scripts/check-carriage-paths/check-carriage-paths.py --list    # show what it found

★ NO `.ps1` TWIN, DELIBERATELY. This is a `.py`, which runs unchanged on every
host this project gates on, so a PowerShell sibling would be a second
implementation of something that was never split -- the case the repository's
pairing rule explicitly exempts.
"""
import io
import os
import re
import sys

for _s in (sys.stdout, sys.stderr):
    try:
        _s.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))

SCAN_TREES = ("scripts", "real-examples", ".github")
SCAN_SUFFIXES = (".sh", ".ps1", ".py", ".json", ".yml", ".yaml", ".mjs")

# A variable whose NAME says its value is a repository root. Matched
# case-insensitively as a WHOLE name or as a trailing component, so
# `DSS_WSL_CHECKOUT` and `RepoRoot` are both caught without listing every
# prefix somebody might invent.
ROLE_NAME = re.compile(
    r"(?:^|_)(?:REPO|RREPO|REPO_ROOT|REPOROOT|REMOTE_DIR|REMOTEDIR|SRC_DIR|SRCDIR"
    r"|CHECKOUT|DSSSRC|DSS_SRC)$", re.I)
# `SRC` and `DST` are role names ONLY in a carriage context -- they are far too
# common otherwise -- so they are admitted only when the value is host-rooted
# (see HOST_ROOTED below).
WEAK_ROLE_NAME = re.compile(r"^(?:SRC|DST)$", re.I)

# NAME="value" / NAME='value' / NAME="${NAME:-value}" / $Name = "value"
ASSIGN = re.compile(
    r"""(?:^|\s|\$)(?P<name>[A-Za-z_][A-Za-z0-9_]*)\s*=\s*
        (?P<q>["'])(?P<val>[^"']*)(?P=q)""", re.X)
# The default inside a shell parameter expansion: ${VAR:-default}
DEFAULT = re.compile(r"\$\{[A-Za-z_][A-Za-z0-9_]*:-([^}]*)\}")

# A value that is rooted somewhere a CHECKOUT lives, rather than being a
# relative path inside one.
HOST_ROOTED = re.compile(r"(?:^~|^/|^\$HOME|^\$\{HOME|^%USERPROFILE%|/mnt/[a-z]/|^[A-Za-z]:[/\\])")

# `.claude/skills/<segment>` -- the OTHER half of the same break. The rename's
# citation-inventory key was rewritten to `.claude/skills/dsscp/`, which names a
# directory that does not exist, so its ceiling silently stopped applying to any
# real document.
#
# ⚠ THE FIRST DRAFT OF THIS RULE WAS WRONG AND THE TREE SAID SO IMMEDIATELY: it
# asserted that every such segment equals the PROJECT name, and then reddened
# `.claude/skills/dss-cycle` and `.claude/skills/dss-state` -- legitimate sibling
# skills that are not named for the project and never should be. The invariant
# that is actually true is weaker and more useful: **a reference must name a
# skill directory that EXISTS.** That catches the real defect exactly, generalises
# to every skill rather than one, and cannot red an honest tree.
SKILLDIR = re.compile(r"\.claude/skills/([A-Za-z0-9._-]+)")

# ★ FLOORS. Both are deliberately BELOW today's counts: a floor that tracks the
# exact number reds on every honest addition, which trains readers to raise it
# without looking. These refuse a COLLAPSE, not a change.
FLOOR_ROOTS = 6
FLOOR_SKILLDIRS = 1


def project_name(text):
    m = re.search(r"^\s*project\(\s*([A-Za-z0-9._-]+)", text, re.M)
    return m.group(1) if m else None


def last_segment(value):
    """The final bare path component, or None if the tail is interpolated.

    A value ending in `$something` names a directory this file cannot know, so
    it is not a claim this guard can check -- and pretending otherwise would be
    the guard inventing a verdict.
    """
    v = value.replace("\\", "/").rstrip("/")
    if not v:
        return None
    seg = v.rsplit("/", 1)[-1]
    if not seg or "$" in seg or "%" in seg or "{" in seg:
        return None
    return seg


def scan_text(rel, text):
    """Yield (kind, rel, name, value, segment) for every checkable claim."""
    for m in ASSIGN.finditer(text):
        name, raw = m.group("name"), m.group("val")
        d = DEFAULT.search(raw)
        value = d.group(1) if d else raw
        if "/" not in value and "\\" not in value:
            continue
        strong = bool(ROLE_NAME.search(name))
        weak = bool(WEAK_ROLE_NAME.match(name)) and bool(HOST_ROOTED.search(value))
        if not (strong or weak):
            continue
        seg = last_segment(value)
        if seg is None:
            continue
        yield ("root", rel, name, value, seg)
    for m in SKILLDIR.finditer(text):
        yield ("skilldir", rel, ".claude/skills", m.group(0), m.group(1))


def scan_tree(root):
    out = []
    for tree in SCAN_TREES:
        base = os.path.join(root, tree)
        if not os.path.isdir(base):
            continue
        for dirpath, dirnames, filenames in os.walk(base):
            dirnames[:] = [d for d in dirnames if d not in (".git", "node_modules")]
            for fn in sorted(filenames):
                if not fn.endswith(SCAN_SUFFIXES):
                    continue
                full = os.path.join(dirpath, fn)
                rel = os.path.relpath(full, root).replace("\\", "/")
                # This guard's OWN source states every spelling it refuses, so
                # scanning it would make it its own counter-example.
                if rel.startswith("scripts/check-carriage-paths/"):
                    continue
                try:
                    text = io.open(full, encoding="utf-8", newline="").read()
                except (UnicodeDecodeError, OSError):
                    continue
                out.extend(scan_text(rel, text))
    return out


# Overridable so the self-test can present a mirror world without touching the
# tree -- the same reason the sibling guards self-test against a mirror.
SKILLDIR_EXISTS = None


def skilldir_exists(seg):
    if SKILLDIR_EXISTS is not None:
        return seg in SKILLDIR_EXISTS
    return os.path.isdir(os.path.join(REPO, ".claude", "skills", seg))


def verdict(name, findings):
    """Return (rc, message). rc 0 green, 1 disagreement, 2 collapsed scan."""
    roots = [f for f in findings if f[0] == "root"]
    skilldirs = [f for f in findings if f[0] == "skilldir"]

    if len(roots) < FLOOR_ROOTS:
        return 2, ("check-carriage-paths: FAIL -- the scan COLLAPSED: found only %d "
                   "repository-root constant(s) under %s, floor is %d.\n"
                   "  A role-keyed scan that finds nothing reports success over an "
                   "empty set. Something moved, was renamed, or stopped matching;\n"
                   "  fix the scan rather than lowering the floor."
                   % (len(roots), " + ".join(SCAN_TREES), FLOOR_ROOTS))
    if len(skilldirs) < FLOOR_SKILLDIRS:
        return 2, ("check-carriage-paths: FAIL -- the scan COLLAPSED: found only %d "
                   "`.claude/skills/<name>` reference(s), floor is %d."
                   % (len(skilldirs), FLOOR_SKILLDIRS))

    lines = []
    bad_roots = [f for f in roots if f[4] != name]
    for kind, rel, var, value, seg in bad_roots:
        lines.append("    %s  %s = %s" % (rel, var, value))
        lines.append("        names `%s`, but `project(%s ...)` in CMakeLists.txt "
                     "declares `%s`" % (seg, name, name))
    bad_skills = [f for f in skilldirs if not skilldir_exists(f[4])]
    for kind, rel, var, value, seg in bad_skills:
        lines.append("    %s  %s" % (rel, value))
        lines.append("        names `.claude/skills/%s/`, which does not exist" % seg)
    if lines:
        head = ["check-carriage-paths: FAIL -- reference(s) name something that is not "
                "there:"]
        # ⚠ THE TAIL IS PER-KIND, and the first draft printed both halves
        # unconditionally -- so a pure root failure ended with a sentence about
        # skill directories that was not true of it. A diagnostic that explains
        # a failure the reader did not have is how a guard's messages stop being
        # read, which costs more than the sentence saved.
        tail = [""]
        if bad_roots:
            tail += [
                "  A repository-root constant is a path used to REACH a checkout on another",
                "  host, and nothing in ctest runs those legs -- so a wrong one is silent",
                "  until somebody needs the gate. A checkout is named for the PROJECT; the",
                "  built command's name is a different string and does not belong here.",
            ]
        if bad_skills:
            tail += [
                "  A skill-directory reference that names nothing simply stops applying,",
                "  which is how a citation ceiling can go on passing over a document it no",
                "  longer covers.",
            ]
        return 1, "\n".join(head + lines + tail)
    return 0, ("check-carriage-paths: OK (%d repository-root constant(s) name `%s`; "
               "%d skill directory reference(s) all resolve)"
               % (len(roots), name, len(skilldirs)))


# ---------------------------------------------------------------------------
# SELF-TEST. Every arm asserts the MESSAGE, not merely a non-zero exit: an
# independent audit of a sibling guard once found a floor arm passing with the
# floor DELETED, because it collapsed on a different refusal that happened to
# share an exit code.
# ---------------------------------------------------------------------------
ARMS = [
    ("GREEN-CONTROL", [
        ("scripts/x/x.sh", 'REPO_ROOT="$HOME/src/PROJ"'),
        ("scripts/y/y.sh", 'RREPO="src/Github/PROJ"'),
        ("scripts/z/z.sh", 'REMOTE_DIR="src/PROJ"'),
        ("scripts/w/w.sh", 'SRC="/mnt/c/Source/DailySoftware/PROJ"'),
        ("scripts/w/w.sh", 'DST="${DSS_WSL_CHECKOUT:-$HOME/src/PROJ}"'),
        ("real-examples/a/a.sh", 'SRC_DIR="${SRC_DIR:-$HOME/src/PROJ}"'),
        ("scripts/i/i.json", '".claude/skills/PROJ/references/repo-map.md": 2'),
    ], 0, "check-carriage-paths: OK"),
    ("ONE-ROOT-RENAMED", [
        ("scripts/x/x.sh", 'REPO_ROOT="$HOME/src/PROJ"'),
        ("scripts/y/y.sh", 'RREPO="src/Github/PROJ"'),
        ("scripts/z/z.sh", 'REMOTE_DIR="src/PROJ"'),
        ("scripts/w/w.sh", 'SRC="/mnt/c/Source/DailySoftware/wrongname"'),
        ("scripts/w/w.sh", 'DST="${DSS_WSL_CHECKOUT:-$HOME/src/PROJ}"'),
        ("real-examples/a/a.sh", 'SRC_DIR="${SRC_DIR:-$HOME/src/PROJ}"'),
        ("scripts/i/i.json", '".claude/skills/PROJ/references/repo-map.md": 2'),
    ], 1, "in CMakeLists.txt declares"),
    ("SKILLDIR-NAMES-NOTHING", [
        ("scripts/x/x.sh", 'REPO_ROOT="$HOME/src/PROJ"'),
        ("scripts/y/y.sh", 'RREPO="src/Github/PROJ"'),
        ("scripts/z/z.sh", 'REMOTE_DIR="src/PROJ"'),
        ("scripts/w/w.sh", 'SRC="/mnt/c/Source/DailySoftware/PROJ"'),
        ("scripts/w/w.sh", 'DST="${DSS_WSL_CHECKOUT:-$HOME/src/PROJ}"'),
        ("real-examples/a/a.sh", 'SRC_DIR="${SRC_DIR:-$HOME/src/PROJ}"'),
        ("scripts/i/i.json", '".claude/skills/wrongname/references/repo-map.md": 2'),
    ], 1, "which does not exist"),
    ("SCAN-COLLAPSED", [
        ("scripts/x/x.sh", 'REPO_ROOT="$HOME/src/PROJ"'),
        ("scripts/i/i.json", '".claude/skills/PROJ/references/repo-map.md": 2'),
    ], 2, "the scan COLLAPSED"),
    ("NO-SKILLDIR", [
        ("scripts/x/x.sh", 'REPO_ROOT="$HOME/src/PROJ"'),
        ("scripts/y/y.sh", 'RREPO="src/Github/PROJ"'),
        ("scripts/z/z.sh", 'REMOTE_DIR="src/PROJ"'),
        ("scripts/w/w.sh", 'SRC="/mnt/c/Source/DailySoftware/PROJ"'),
        ("scripts/w/w.sh", 'DST="${DSS_WSL_CHECKOUT:-$HOME/src/PROJ}"'),
        ("real-examples/a/a.sh", 'SRC_DIR="${SRC_DIR:-$HOME/src/PROJ}"'),
    ], 2, "`.claude/skills/<name>` reference(s)"),
    # ⚠ An INTERPOLATED tail is not a claim this guard can check, and saying so
    # is the difference between a measurement and an invented verdict. This arm
    # exists because the obvious implementation -- take the last segment and
    # compare -- would red `RREPO="$HOME/src/$name"` for no reason.
    ("INTERPOLATED-TAIL-IS-NOT-A-CLAIM", [
        ("scripts/x/x.sh", 'REPO_ROOT="$HOME/src/PROJ"'),
        ("scripts/y/y.sh", 'RREPO="src/Github/PROJ"'),
        ("scripts/z/z.sh", 'REMOTE_DIR="src/PROJ"'),
        ("scripts/w/w.sh", 'SRC="/mnt/c/Source/DailySoftware/PROJ"'),
        ("scripts/w/w.sh", 'DST="${DSS_WSL_CHECKOUT:-$HOME/src/PROJ}"'),
        ("real-examples/a/a.sh", 'SRC_DIR="${SRC_DIR:-$HOME/src/PROJ}"'),
        ("scripts/v/v.sh", 'REPO_ROOT="$HOME/src/$whatever"'),
        ("scripts/i/i.json", '".claude/skills/PROJ/references/repo-map.md": 2'),
    ], 0, "check-carriage-paths: OK"),
    # ⚠ And a RELATIVE path inside a checkout is not a checkout path. Without
    # this distinction `SRC="src/dss-config"` would red, which is how a guard
    # earns a reputation for crying wolf and then gets disabled.
    ("RELATIVE-SRC-IS-NOT-A-CHECKOUT", [
        ("scripts/x/x.sh", 'REPO_ROOT="$HOME/src/PROJ"'),
        ("scripts/y/y.sh", 'RREPO="src/Github/PROJ"'),
        ("scripts/z/z.sh", 'REMOTE_DIR="src/PROJ"'),
        ("scripts/w/w.sh", 'SRC="/mnt/c/Source/DailySoftware/PROJ"'),
        ("scripts/w/w.sh", 'DST="${DSS_WSL_CHECKOUT:-$HOME/src/PROJ}"'),
        ("real-examples/a/a.sh", 'SRC_DIR="${SRC_DIR:-$HOME/src/PROJ}"'),
        ("scripts/v/v.sh", 'SRC="src/dss-config"'),
        ("scripts/i/i.json", '".claude/skills/PROJ/references/repo-map.md": 2'),
    ], 0, "check-carriage-paths: OK"),
]


def self_test():
    global SKILLDIR_EXISTS
    name = "projname"
    ok = True
    # The mirror world: only `projname` is a real skill directory in it, so the
    # arms can vary the reference without the tree's actual skills leaking in.
    SKILLDIR_EXISTS = {name}
    for label, rows, want_rc, want_msg in ARMS:
        findings = []
        for rel, line in rows:
            findings.extend(scan_text(rel, line.replace("PROJ", name)))
        rc, msg = verdict(name, findings)
        good = (rc == want_rc and want_msg in msg)
        ok = ok and good
        print("carriage-paths: self-test arm %-33s rc=%d %s"
              % (label, rc, "as expected" if good
                 else "EXPECTED rc=%d and %r (got %r)" % (want_rc, want_msg, msg[:90])))
    SKILLDIR_EXISTS = None
    if not ok:
        print("carriage-paths: self-test FAILED -- this guard is NOT proven able to fail.")
    return ok


def main(argv):
    cml = os.path.join(REPO, "CMakeLists.txt")
    name = project_name(io.open(cml, encoding="utf-8", newline="").read())
    if not name:
        print("check-carriage-paths: FAIL -- no `project(<name> ...)` in CMakeLists.txt, "
              "so there is no declared name to check against.")
        return 2

    findings = scan_tree(REPO)
    if "--list" in argv:
        for kind, rel, var, value, seg in sorted(findings):
            print("  %-9s %-52s %s = %s" % (kind, rel, var, value))
        print("  project name: %s" % name)

    rc, msg = verdict(name, findings)
    print(msg)
    if not self_test():
        return 3
    return rc


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
