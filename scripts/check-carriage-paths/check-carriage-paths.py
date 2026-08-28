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

# ★★ WHERE A CHECKOUT IS FETCHED FROM, which is the same role as where one
# LIVES wearing a different spelling -- and this guard was blind to it.
# D-CARRIAGE-REPO-URL-ROLE-IS-INVISIBLE-TO-THE-PATHS-GUARD: `build-and-test.sh`
# defaulted `DSS_REPO_URL` to `dailysoftwaresystems/dsscp.git` when the
# repository is `dss-code-prime`, so a fresh clone on a host without $SRC_DIR
# cloned nothing -- and ROLE_NAME is `$`-anchored on REPO/SRC_DIR/CHECKOUT, so a
# name ending in `_URL` was never scanned at all.
#
# ★★★ THE RULE IS AGREEMENT AT THE CLONE SITE, NOT A NAME LIST, and that choice
# is the whole design. The obvious fix -- admit URL-shaped role names and
# require an our-org repository to be named for the project -- is WRONG, and the
# row that opened this says so: it would red `homebrew-dsscp`, `scoop-dsscp` and
# `dsscp-nix`, which are package repositories legitimately named for the COMMAND
# under the width ruling. Requiring a URL to name THIS project cannot tell those
# apart from the defect.
#
# What actually caught the bug was that THE FILE DISAGREED WITH ITSELF: the URL
# said `dsscp` while the `SRC_DIR` it is cloned INTO said `dss-code-prime`, one
# line apart. That is mechanical, local, and needs no allowlist:
#
#   a URL cloned into a REPOSITORY-ROOT CONSTANT must name the same repository
#   as that constant.
#
# It is ORG-BLIND and PROJECT-BLIND, so it checks the upstream sqlite clone on
# exactly the same terms as ours.
#
# ⚠⚠ THE QUALIFIER ON THE TARGET IS LOAD-BEARING AND WAS MEASURED, NOT
# REASONED. The first draft of this rule checked EVERY clone site, and running
# it against the tree produced ELEVEN findings of which ZERO were defects:
# `clone_or_update has always taken a third argument` and similar PROSE inside
# comments; the function's own `clone_or_update <url> <dir>` signature line;
# and -- the one that actually refutes the general form -- `test-confound-scope`
# legitimately cloning `$ORIGIN` into `cloned-oldway` and `worktree`. Cloning a
# repository into a directory with a different name is ORDINARY GIT USAGE, so
# "URL name == target name" is simply not true in general.
#
# What IS true is narrower and is this guard's own subject: a repository-root
# constant is a variable whose NAME already claims to hold a checkout of a
# particular repository, so filling one from a URL that names a DIFFERENT
# repository is a self-contradiction no matter whose repository it is. Requiring
# the target to be a `$VAR` matching ROLE_NAME drops all eleven false positives
# -- prose has no `$VAR` target, and `tap`/`worktree`/`cloned-oldway` are not
# role names -- while keeping the defect this row was opened for.
CLONE_SITE = re.compile(
    r"""(?:clone_or_update|git\s+clone(?:\s+-[^\s]*(?:\s+[^\s"']+)?)*)\s+
        (?P<url>"[^"]*"|'[^']*'|[^\s"']+)\s+
        (?P<dir>"[^"]*"|'[^']*'|[^\s"']+)""", re.X)
# `$VAR` / `${VAR}` / `"$VAR"` -- a clone-site argument that defers to an
# assignment elsewhere in the same file.
VAR_REF = re.compile(r"^\$\{?([A-Za-z_][A-Za-z0-9_]*)\}?$")

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
# ★ The clone-site scan needs a floor for the same reason the others do, and
# more urgently: it is the arm that was ADDED because this guard had been
# reporting green over a claim it never looked at. A scan that silently stops
# matching restores exactly that blindness, and reports OK while doing it.
FLOOR_CLONEPAIRS = 1


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


def repo_identity(value):
    """The repository a path or URL names, or None if it makes no claim.

    The last segment with any `.git` suffix removed, so that
    `git@github.com:org/dss-code-prime.git` and `$HOME/src/dss-code-prime`
    reduce to the same string and can be compared at all.
    """
    seg = last_segment(value)
    if seg is None:
        return None
    return seg[:-4] if seg.endswith(".git") else seg


def scan_text(rel, text):
    """Yield (kind, rel, name, value, segment) for every checkable claim."""
    assigns = {}
    for m in ASSIGN.finditer(text):
        name, raw = m.group("name"), m.group("val")
        d = DEFAULT.search(raw)
        value = d.group(1) if d else raw
        # ⚠ RECORDED BEFORE THE ROLE FILTER, and deliberately: the clone-site
        # check resolves whatever variable the site names, and `DSS_REPO_URL`
        # is exactly the kind of name no role pattern admits. Narrowing this
        # map to role names would rebuild the blind spot inside its own fix.
        assigns.setdefault(name, value)
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

    def unquote(expr):
        e = expr.strip()
        if len(e) >= 2 and e[0] == e[-1] and e[0] in "\"'":
            e = e[1:-1]
        return e

    def resolve(expr):
        e = unquote(expr)
        ref = VAR_REF.match(e)
        if ref:
            if ref.group(1) not in assigns:
                return None          # names something this file never sets
            e = assigns[ref.group(1)]
            d = DEFAULT.search(e)
            if d:
                e = d.group(1)
        return repo_identity(e)

    for m in CLONE_SITE.finditer(text):
        # THE TARGET MUST BE A REPOSITORY-ROOT CONSTANT -- see the header. This
        # single condition is what separates a checkable claim from prose and
        # from an ordinary clone-into-a-differently-named-directory.
        dst = VAR_REF.match(unquote(m.group("dir")))
        if dst is None or not ROLE_NAME.search(dst.group(1)):
            continue
        url_repo = resolve(m.group("url"))
        dir_repo = resolve(m.group("dir"))
        if url_repo is None or dir_repo is None:
            continue                 # not a claim this guard can check
        yield ("clonepair", rel,
               "%s -> %s" % (m.group("url"), m.group("dir")),
               "%s vs %s" % (url_repo, dir_repo),
               "OK" if url_repo == dir_repo else "MISMATCH")

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
    clonepairs = [f for f in findings if f[0] == "clonepair"]

    if len(clonepairs) < FLOOR_CLONEPAIRS:
        return 2, ("check-carriage-paths: FAIL -- the scan COLLAPSED: found only %d "
                   "clone site(s) pairing a repository URL with the directory it is\n"
                   "  cloned into, floor is %d. This arm exists because a wrong URL was "
                   "invisible here once already; a scan that matches nothing\n"
                   "  reports OK over the same blind spot. Fix the scan rather than "
                   "lowering the floor."
                   % (len(clonepairs), FLOOR_CLONEPAIRS))
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
    bad_pairs = [f for f in clonepairs if f[4] == "MISMATCH"]
    for kind, rel, var, value, seg in bad_pairs:
        lines.append("    %s  %s" % (rel, var))
        lines.append("        clones `%s` into a directory named for `%s`"
                     % tuple(value.split(" vs ")))
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
        if bad_pairs:
            tail += [
                "  A clone URL and the directory it is cloned INTO name the same repository",
                "  or one of them is wrong. Nothing in ctest takes the fresh-clone arm, so a",
                "  wrong URL clones nothing while every tier that skipped that arm reports",
                "  green. This is org-blind on purpose: it is not asking whether the repo is",
                "  ours, only whether the file agrees with itself.",
            ]
        if bad_skills:
            tail += [
                "  A skill-directory reference that names nothing simply stops applying,",
                "  which is how a citation ceiling can go on passing over a document it no",
                "  longer covers.",
            ]
        return 1, "\n".join(head + lines + tail)
    return 0, ("check-carriage-paths: OK (%d repository-root constant(s) name `%s`; "
               "%d clone site(s) agree with the directory they clone into; "
               "%d skill directory reference(s) all resolve)"
               % (len(roots), name, len(clonepairs), len(skilldirs)))


# ---------------------------------------------------------------------------
# SELF-TEST. Every arm asserts the MESSAGE, not merely a non-zero exit: an
# independent audit of a sibling guard once found a floor arm passing with the
# floor DELETED, because it collapsed on a different refusal that happened to
# share an exit code.
# ---------------------------------------------------------------------------
#
# ★ EVERY ARM CARRIES A CLONE SITE, spelled once here rather than seven times.
# Without one, the new FLOOR_CLONEPAIRS collapses the arm -- and a collapse
# standing in for the verdict an arm was written to prove is precisely the
# "passed for the wrong reason" failure this header warns about. It brings its
# OWN checkout variable so it is self-contained and does not perturb the root
# counts the other arms depend on beyond a constant +1.
CARRIAGE_BASE = [
    ("real-examples/clone/c.sh", 'DSS_REPO_URL="git@github.com:org/PROJ.git"'),
    ("real-examples/clone/c.sh", 'DSS_CHECKOUT="$HOME/src/PROJ"'),
    ("real-examples/clone/c.sh", 'clone_or_update "$DSS_REPO_URL" "$DSS_CHECKOUT" ""'),
]

ARMS = [
    ("GREEN-CONTROL", [
        ("scripts/x/x.sh", 'REPO_ROOT="$HOME/src/PROJ"'),
        ("scripts/y/y.sh", 'RREPO="src/Github/PROJ"'),
        ("scripts/z/z.sh", 'REMOTE_DIR="src/PROJ"'),
        ("scripts/w/w.sh", 'SRC="/mnt/c/Source/DailySoftware/PROJ"'),
        ("scripts/w/w.sh", 'DST="${DSS_WSL_CHECKOUT:-$HOME/src/PROJ}"'),
        ("real-examples/a/a.sh", 'SRC_DIR="${SRC_DIR:-$HOME/src/PROJ}"'),
        ("scripts/i/i.json", '".claude/skills/PROJ/references/repo-map.md": 2'),
    ] + CARRIAGE_BASE, 0, "check-carriage-paths: OK"),
    ("ONE-ROOT-RENAMED", [
        ("scripts/x/x.sh", 'REPO_ROOT="$HOME/src/PROJ"'),
        ("scripts/y/y.sh", 'RREPO="src/Github/PROJ"'),
        ("scripts/z/z.sh", 'REMOTE_DIR="src/PROJ"'),
        ("scripts/w/w.sh", 'SRC="/mnt/c/Source/DailySoftware/wrongname"'),
        ("scripts/w/w.sh", 'DST="${DSS_WSL_CHECKOUT:-$HOME/src/PROJ}"'),
        ("real-examples/a/a.sh", 'SRC_DIR="${SRC_DIR:-$HOME/src/PROJ}"'),
        ("scripts/i/i.json", '".claude/skills/PROJ/references/repo-map.md": 2'),
    ] + CARRIAGE_BASE, 1, "in CMakeLists.txt declares"),
    ("SKILLDIR-NAMES-NOTHING", [
        ("scripts/x/x.sh", 'REPO_ROOT="$HOME/src/PROJ"'),
        ("scripts/y/y.sh", 'RREPO="src/Github/PROJ"'),
        ("scripts/z/z.sh", 'REMOTE_DIR="src/PROJ"'),
        ("scripts/w/w.sh", 'SRC="/mnt/c/Source/DailySoftware/PROJ"'),
        ("scripts/w/w.sh", 'DST="${DSS_WSL_CHECKOUT:-$HOME/src/PROJ}"'),
        ("real-examples/a/a.sh", 'SRC_DIR="${SRC_DIR:-$HOME/src/PROJ}"'),
        ("scripts/i/i.json", '".claude/skills/wrongname/references/repo-map.md": 2'),
    ] + CARRIAGE_BASE, 1, "which does not exist"),
    ("SCAN-COLLAPSED", [
        ("scripts/x/x.sh", 'REPO_ROOT="$HOME/src/PROJ"'),
        ("scripts/i/i.json", '".claude/skills/PROJ/references/repo-map.md": 2'),
    # ⚠ The expected message names the ROOT floor specifically. Both collapse
    # arms print "the scan COLLAPSED", so matching that alone would let this arm
    # pass on the clone-site floor tripping instead -- one arm standing in for
    # another, which is the exact defect the header describes.
    ] + CARRIAGE_BASE, 2, "repository-root constant(s) under"),
    ("NO-SKILLDIR", [
        ("scripts/x/x.sh", 'REPO_ROOT="$HOME/src/PROJ"'),
        ("scripts/y/y.sh", 'RREPO="src/Github/PROJ"'),
        ("scripts/z/z.sh", 'REMOTE_DIR="src/PROJ"'),
        ("scripts/w/w.sh", 'SRC="/mnt/c/Source/DailySoftware/PROJ"'),
        ("scripts/w/w.sh", 'DST="${DSS_WSL_CHECKOUT:-$HOME/src/PROJ}"'),
        ("real-examples/a/a.sh", 'SRC_DIR="${SRC_DIR:-$HOME/src/PROJ}"'),
    ] + CARRIAGE_BASE, 2, "`.claude/skills/<name>` reference(s)"),
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
    ] + CARRIAGE_BASE, 0, "check-carriage-paths: OK"),
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
    ] + CARRIAGE_BASE, 0, "check-carriage-paths: OK"),
    # ★ THE ARM THIS WHOLE ADDITION EXISTS FOR — the real defect, reproduced.
    # `DSS_REPO_URL` named `dsscp` while the directory it is cloned into named
    # the project, one line apart, and no role pattern admitted a name ending
    # in `_URL` so nothing looked.
    ("CLONE-URL-DISAGREES-WITH-ITS-CHECKOUT", [
        ("scripts/x/x.sh", 'REPO_ROOT="$HOME/src/PROJ"'),
        ("scripts/y/y.sh", 'RREPO="src/Github/PROJ"'),
        ("scripts/z/z.sh", 'REMOTE_DIR="src/PROJ"'),
        ("scripts/w/w.sh", 'SRC="/mnt/c/Source/DailySoftware/PROJ"'),
        ("scripts/w/w.sh", 'DST="${DSS_WSL_CHECKOUT:-$HOME/src/PROJ}"'),
        ("real-examples/a/a.sh", 'SRC_DIR="${SRC_DIR:-$HOME/src/PROJ}"'),
        ("real-examples/a/a.sh", 'DSS_REPO_URL="git@github.com:org/dsscp.git"'),
        ("real-examples/a/a.sh", 'clone_or_update "$DSS_REPO_URL" "$SRC_DIR" ""'),
        ("scripts/i/i.json", '".claude/skills/PROJ/references/repo-map.md": 2'),
    ], 1, "into a directory named for"),
    # ⚠ A URL this file never assigns, or an interpolated one, is NOT a claim --
    # `${TAP_REPO}` in the package-publish workflow is exactly that, and reading
    # a verdict out of it would red three legitimate package repositories. Here
    # the arm still has to reach a verdict, so a checkable pair rides along.
    ("UNRESOLVABLE-CLONE-URL-IS-NOT-A-CLAIM", [
        ("scripts/x/x.sh", 'REPO_ROOT="$HOME/src/PROJ"'),
        ("scripts/y/y.sh", 'RREPO="src/Github/PROJ"'),
        ("scripts/z/z.sh", 'REMOTE_DIR="src/PROJ"'),
        ("scripts/w/w.sh", 'SRC="/mnt/c/Source/DailySoftware/PROJ"'),
        ("scripts/w/w.sh", 'DST="${DSS_WSL_CHECKOUT:-$HOME/src/PROJ}"'),
        ("real-examples/a/a.sh", 'SRC_DIR="${SRC_DIR:-$HOME/src/PROJ}"'),
        ("scripts/p/p.yml", 'git clone --depth 1 "https://github.com/${TAP_REPO}.git" tap'),
        ("scripts/i/i.json", '".claude/skills/PROJ/references/repo-map.md": 2'),
    ] + CARRIAGE_BASE, 0, "check-carriage-paths: OK"),
    # And the floor for the new scan, proven to fail on its own message rather
    # than borrowing the root floor's.
    ("CLONE-SCAN-COLLAPSED", [
        ("scripts/x/x.sh", 'REPO_ROOT="$HOME/src/PROJ"'),
        ("scripts/y/y.sh", 'RREPO="src/Github/PROJ"'),
        ("scripts/z/z.sh", 'REMOTE_DIR="src/PROJ"'),
        ("scripts/w/w.sh", 'SRC="/mnt/c/Source/DailySoftware/PROJ"'),
        ("scripts/w/w.sh", 'DST="${DSS_WSL_CHECKOUT:-$HOME/src/PROJ}"'),
        ("real-examples/a/a.sh", 'SRC_DIR="${SRC_DIR:-$HOME/src/PROJ}"'),
        ("scripts/i/i.json", '".claude/skills/PROJ/references/repo-map.md": 2'),
    ], 2, "clone site(s) pairing a repository URL"),
]


def self_test():
    global SKILLDIR_EXISTS
    name = "projname"
    ok = True
    # The mirror world: only `projname` is a real skill directory in it, so the
    # arms can vary the reference without the tree's actual skills leaking in.
    SKILLDIR_EXISTS = {name}
    for label, rows, want_rc, want_msg in ARMS:
        # ⚠ ROWS ARE GROUPED BY FILE AND JOINED, not scanned one at a time.
        # The clone-site check resolves a `$VAR` against assignments elsewhere
        # in the SAME file, so a per-line scan could never see them together --
        # and per-file is what `scan_tree` really does, so this is the mirror
        # world getting closer to the tree rather than further from it.
        by_file = {}
        for rel, line in rows:
            by_file.setdefault(rel, []).append(line.replace("PROJ", name))
        findings = []
        for rel, lines in by_file.items():
            findings.extend(scan_text(rel, "\n".join(lines)))
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
