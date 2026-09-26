#!/usr/bin/env python3
"""test-corpus-census.py -- corpus-census's run identity (HEAD, branch, tree state) describes the tree it lives in,
whatever git environment the caller exported.

ⓘ No `PURPOSE:` line: `check-scripts-index` lets a sibling omit the declaration, and this file is the primary's
test, not a tool of its own.

★★★ WHAT IS PINNED. ✔MEASURED 2026-09-15 (P66 lane ge) before the fix: `git()` -- the reads that stamp every census
report with `git HEAD`, `git branch` and `tree state` -- run on a tree at edfa495d holding 122 dirty paths, named
ANOTHER repository's branch and HEAD under that repository's GIT_DIR; under GIT_DIR + GIT_WORK_TREE it did the same
and read the dirty tree as CLEAN; under an absolute GIT_INDEX_FILE its `git status` failed (rc 128).

★★ HERMETIC. corpus-census.py is COPIED into a fixture DSS tree together with the files it opens at import -- its
root markers (`CMakeLists.txt`, `src/`) and `src/core/types/parse_diagnostic.cpp` for the family table -- and the
owner it asks git through; the fixture is committed on a known branch and then made DIRTY. A CHILD process imports
that copy and prints its `git()` answers: nothing compiles and no census runs. Arms:
  CONTROL                  no steering                                  -> the fixture's HEAD, branch and dirty status
  GIT_DIR                  another repository                           -> the same (negative: bare git names the other)
  GIT_DIR + GIT_WORK_TREE  another repository                           -> the same (negative: bare git reads it CLEAN)
  GIT_INDEX_FILE           another repository's index, ABSOLUTE         -> the same (negative: bare git status differs)
plus one arm that the fixture box is removed.

usage:  test-corpus-census.py
exit:   0 every arm held · 1 an arm failed · 2 cannot run
"""
from __future__ import annotations

import importlib.util
import json
import os
import shutil
import subprocess
import sys
import tempfile
sys.dont_write_bytecode = True  # a by-path load must not write __pycache__ beside another action (the rule: check-scripts-index)

for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError, OSError):  # pragma: no cover - an odd stream
        pass

HERE = os.path.dirname(os.path.realpath(__file__))
# The owner is this file's SIBLING program, and the real tree is ITS answer -- never a count
# of `dirname`s, which named `.harness-config/runner` the day this file moved out of
# `scripts/` (2026-09-18).
OWNER_PY = os.path.join(os.path.dirname(HERE), "owning-tree", "owning-tree.py")
OWN_BRANCH, OTHER_BRANCH = "ge-census-own", "ge-census-other"
EXPECTED_ARMS = 5

# Imports the COPY named on the command line and prints its run-identity answers as JSON.
_DRIVER = r'''
import importlib.util, json, sys
spec = importlib.util.spec_from_file_location("census_copy", sys.argv[1])
mod = importlib.util.module_from_spec(spec)
sys.modules["census_copy"] = mod
spec.loader.exec_module(mod)
print("IDENTITY=" + json.dumps({"root": str(mod.REPO_ROOT), "head": mod.git("rev-parse", "HEAD"),
                                "branch": mod.git("rev-parse", "--abbrev-ref", "HEAD"),
                                "status": mod.git("status", "--porcelain")}))
'''


def load_owning_tree():
    spec = importlib.util.spec_from_file_location("dss_owning_tree", OWNER_PY)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


OT = load_owning_tree()
REAL_TREE = OT.resolve(__file__)


def git(cwd, *args):
    p = OT.run_git(["-C", cwd, "-c", "user.email=ge@example.invalid", "-c", "user.name=ge",
                    "-c", "commit.gpgsign=false"] + list(args),
                   capture_output=True, text=True, encoding="utf-8", errors="replace")
    if p.returncode != 0:
        raise RuntimeError("git %s in %s failed: %s" % (" ".join(args), cwd, p.stderr.strip()))
    return p.stdout.strip()


def put(root, rel, text=None, copy_from=None):
    dest = os.path.join(root, *rel.split("/"))
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    if copy_from:
        shutil.copyfile(copy_from, dest)
    else:
        with open(dest, "w", encoding="utf-8", newline="\n") as fh:
            fh.write(text)
    return dest


def main(argv):
    if argv:
        print("test-corpus-census: takes no arguments (got %r)" % argv)
        return 2
    base = OT.git_environment()
    box = os.path.realpath(tempfile.mkdtemp(prefix="corpus-census-test-"))
    ran, failed = [], []

    def arm(ok, label, detail):
        ran.append(label)
        if not ok:
            failed.append(label)
        print("  %-4s %s%s" % ("ok" if ok else "FAIL", label, "" if ok else "   [%s]" % detail))

    try:
        own = os.path.join(box, "own")
        other = os.path.join(box, "other")
        put(own, "CMakeLists.txt", "# a fixture tree: corpus-census finds its root by this file and src/\n")
        put(own, "src/core/types/parse_diagnostic.cpp",
            copy_from=os.path.join(REAL_TREE, "src", "core", "types", "parse_diagnostic.cpp"))
        copy = put(own, ".harness-config/runner/actions/corpus-census/corpus-census.py", copy_from=os.path.join(HERE, "corpus-census.py"))
        put(own, ".harness-config/runner/actions/owning-tree/owning-tree.py",
            copy_from=OWNER_PY)
        git(own, "init", "-q")
        git(own, "checkout", "-q", "-b", OWN_BRANCH)
        git(own, "add", "-A")
        git(own, "commit", "-q", "--no-verify", "-m", "fixture own")
        put(own, "CMakeLists.txt", "# a fixture tree, now DIRTY\n")
        put(other, "other.txt", "another repository\n")
        git(other, "init", "-q")
        git(other, "checkout", "-q", "-b", OTHER_BRANCH)
        git(other, "add", "-A")
        git(other, "commit", "-q", "--no-verify", "-m", "fixture other")
        own_head = git(own, "rev-parse", "HEAD")

        def census(extra):
            env = dict(base, GIT_CEILING_DIRECTORIES=box, **extra)
            try:
                # `-B`: a child that loads by path writes no bytecode (check-scripts-index clause 12b).
                p = subprocess.run([sys.executable, "-B", "-c", _DRIVER, copy], cwd=box, env=env,
                                   capture_output=True,
                                   text=True, encoding="utf-8", errors="replace", timeout=180)
                said = (p.stdout or "") + (p.stderr or "")
            except subprocess.TimeoutExpired:
                return None, "TIMEOUT"
            for line in said.splitlines():
                if line.startswith("IDENTITY="):
                    return json.loads(line[len("IDENTITY="):]), ""
            return None, said.strip()[-240:]

        def bare(extra, *args):
            p = subprocess.run(["git", "-C", own] + list(args), env=dict(base, **extra), capture_output=True,
                               text=True, encoding="utf-8", errors="replace")
            return p.returncode, p.stdout.strip()

        control, why = census({})
        ok = (control is not None and control["head"] == own_head and control["branch"] == OWN_BRANCH
              and "CMakeLists.txt" in control["status"] and OT.same_path(control["root"], own))
        arm(ok, "CONTROL: the census copy stamps its own tree -- HEAD, branch, and its dirty status",
            "got=%r %s" % (control, why))
        if control is None:
            return 1

        other_git = os.path.join(other, ".git")
        cases = (
            ("GIT_DIR", {"GIT_DIR": other_git},
             lambda: bare({"GIT_DIR": other_git}, "rev-parse", "--abbrev-ref", "HEAD")[1] == OTHER_BRANCH),
            ("GIT_DIR + GIT_WORK_TREE", {"GIT_DIR": other_git, "GIT_WORK_TREE": other},
             lambda: bare({"GIT_DIR": other_git, "GIT_WORK_TREE": other}, "status", "--porcelain") == (0, "")),
            ("an ABSOLUTE GIT_INDEX_FILE", {"GIT_INDEX_FILE": os.path.abspath(os.path.join(other_git, "index"))},
             lambda: bare({"GIT_INDEX_FILE": os.path.abspath(os.path.join(other_git, "index"))},
                          "status", "--porcelain") != (0, control["status"])),
        )
        for label, extra, negative in cases:
            real = negative()
            got, why = census(extra)
            arm(real and got == control,
                "a caller's %s naming another repository changes none of the run-identity answers" % label,
                "negative-synthesized=%s got=%r %s" % (real, got, why))
    finally:
        box_gone = OT.remove_tree(box)
        if not box_gone:
            print("test-corpus-census: the fixture box was NOT removed: %s" % box)
    arm(box_gone, "the fixture box is removed -- git's read-only objects included", "left behind: %s" % box)

    if len(ran) != EXPECTED_ARMS:
        print("test-corpus-census: FAIL -- %d arm(s) ran, %d expected" % (len(ran), EXPECTED_ARMS))
        return 1
    if failed:
        print("test-corpus-census: FAIL -- %d of %d arm(s)" % (len(failed), len(ran)))
        return 1
    print("test-corpus-census: OK -- %d arm(s): the run identity names its own tree under GIT_DIR, "
          "GIT_DIR + GIT_WORK_TREE and an absolute GIT_INDEX_FILE" % len(ran))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
