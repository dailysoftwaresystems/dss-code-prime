#!/usr/bin/env python3
"""test-check-ci-legs.py -- both check-ci-legs twins ask git AND gh about the tree they live in, whatever git
environment the caller exported, and refuse a caller's GH_REPO.

ⓘ No `PURPOSE:` line: `check-scripts-index` lets a sibling omit the declaration, and this file is the primary's
test, not a tool of its own.

★★★ WHAT IS PINNED. ✔MEASURED 2026-09-15 (P66 lane ge) before the fix, with a recording stub gh: under another
repository's GIT_DIR, or GIT_DIR + GIT_WORK_TREE, BOTH twins read THAT repository's branch and asked gh about it,
and handed gh an environment in which gh's own git saw THAT repository's origin -- even with the branch given. The
real gh 2.89.0 then answered HTTP 404 for `repos/:owner/:repo`, where it answers the right repository without the
variables. A fix that routed only the tool's own git call would have left the second channel open.

★★ HERMETIC: no network and no gh authentication. Each twin is COPIED, with the owner it loads
(`scripts/leg-tree/leg-tree.sh` or `scripts/repo-tree/repo-tree.ps1`), into a fixture repository on a known branch
with a known origin. A STUB gh leads PATH; for every call it records its arguments and what git answers in ITS OWN
environment -- the top level and the origin, which is exactly what gh reads to find `:owner/:repo`. Arms, per twin:
  CONTROL                 no steering                               -> the fixture's branch; gh's git = the fixture
  GIT_DIR                 another repository                        -> the same   (negative: a bare git names the other)
  GIT_DIR + GIT_WORK_TREE another repository                        -> the same   (negative proven the same way)
  GIT_INDEX_FILE          another repository's index, ABSOLUTE      -> the same   (✔measured not to steer these reads;
                                                                                    pinned so it stays that way)
  --branch GIVEN          under GIT_DIR                             -> gh's git STILL sees the fixture -- the channel a
                                                                       git-only fix leaves open
  GH_REPO                 set by the caller                         -> REFUSED, exit 2, naming it, and gh never ran
plus one arm that the fixture box is removed (a box left behind is a loud failure, not a leak).

usage:  test-check-ci-legs.py [--bash <bash>] [--pwsh <pwsh>]      (at least one twin)
exit:   0 every arm held · 1 an arm failed · 2 cannot run
"""
from __future__ import annotations

import importlib.util
import os
import shutil
import subprocess
import sys
import tempfile

for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError, OSError):  # pragma: no cover - an odd stream
        pass

HERE = os.path.dirname(os.path.realpath(__file__))
REAL_TREE = os.path.dirname(os.path.dirname(HERE))
TWINS = {"sh": "check-ci-legs.sh", "ps1": "check-ci-legs.ps1"}
OWNERS = {"sh": ("scripts", "leg-tree", "leg-tree.sh"), "ps1": ("scripts", "repo-tree", "repo-tree.ps1")}
OWN_BRANCH, OTHER_BRANCH, GIVEN_BRANCH = "ge-ci-own", "ge-ci-other", "ge-ci-given"
OWN_ORIGIN = "https://github.com/ge-fixture/ci-legs-own.git"
OTHER_ORIGIN = "https://github.com/ge-fixture/ci-legs-other.git"
ARMS_PER_TWIN = 6

STUB_SH = r"""#!/usr/bin/env bash
# A STUB gh: records its arguments and what git answers in ITS environment, then answers with canned rows.
log=${CI_LEGS_STUB_LOG:?}
# ONE record per call: the jobs query's `--jq` program spans lines, so the arguments are flattened.
printf 'CALL\x1f%s\x1f%s\x1f%s\n' "$(printf '%s' "$*" | tr '\r\n' '  ')" \
    "$(git rev-parse --show-toplevel 2>/dev/null)" "$(git remote get-url origin 2>/dev/null)" >> "$log"
case "${1:-} ${2:-}" in
    "run list") echo 4242 ;;
    api\ *) case "$2" in
                */jobs*) printf 'NO-MATRIX\tlabel-check=skipped\n' ;;
                *)       printf 'deadbeef\tstub\t2026-09-15T00:00:00Z\tsuccess\n' ;;
            esac ;;
    *) exit 9 ;;
esac
"""

STUB_PS1 = r"""# A STUB gh: records its arguments and what git answers in ITS environment, then answers with canned rows.
$top = (& git rev-parse --show-toplevel 2>$null | Select-Object -First 1)
$origin = (& git remote get-url origin 2>$null | Select-Object -First 1)
# ONE record per call: the jobs query's `--jq` program spans lines, so the arguments are flattened.
Add-Content -LiteralPath $env:CI_LEGS_STUB_LOG -Value ("CALL" + [char]0x1f + (($args -join ' ') -replace "[`r`n]", ' ') + [char]0x1f + $top + [char]0x1f + $origin)
if ($args.Count -ge 2 -and $args[0] -eq 'run' -and $args[1] -eq 'list') { '4242'; exit 0 }
if ($args.Count -ge 2 -and $args[0] -eq 'api') {
    if ($args[1] -like '*/jobs*') { "NO-MATRIX`tlabel-check=skipped" } else { "deadbeef`tstub`t2026-09-15T00:00:00Z`tsuccess" }
    exit 0
}
exit 9
"""


def load_owning_tree():
    path = os.path.join(REAL_TREE, "scripts", "owning-tree", "owning-tree.py")
    spec = importlib.util.spec_from_file_location("dss_owning_tree", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


OT = load_owning_tree()


def write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(text)


def git(cwd, *args):
    p = OT.run_git(["-C", cwd, "-c", "user.email=ge@example.invalid", "-c", "user.name=ge",
                    "-c", "commit.gpgsign=false"] + list(args),
                   capture_output=True, text=True, encoding="utf-8", errors="replace")
    if p.returncode != 0:
        raise RuntimeError("git %s in %s failed: %s" % (" ".join(args), cwd, p.stderr.strip()))
    return p.stdout.strip()


def make_repo(path, branch, origin, with_tool):
    write(os.path.join(path, "WHOAMI"), branch + "\n")
    if with_tool:
        for name in TWINS.values():
            dest = os.path.join(path, "scripts", "check-ci-legs", name)
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            shutil.copyfile(os.path.join(HERE, name), dest)
        for parts in OWNERS.values():
            dest = os.path.join(path, *parts)
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            shutil.copyfile(os.path.join(REAL_TREE, *parts), dest)
    git(path, "init", "-q")
    git(path, "checkout", "-q", "-b", branch)
    git(path, "add", "-A")
    git(path, "commit", "-q", "--no-verify", "-m", "fixture " + branch)
    git(path, "remote", "add", "origin", origin)


def calls(log):
    if not os.path.isfile(log):
        return []
    with open(log, encoding="utf-8", errors="replace") as fh:
        rows = [l.rstrip("\r\n").split("\x1f") for l in fh if l.startswith("CALL")]
    return [r[1:] + [""] * (4 - len(r)) for r in rows]


def same(a, b):
    return bool(a) and bool(b) and OT.same_path(a, b)


def main(argv):
    shells = {}
    i = 0
    while i < len(argv):
        if argv[i] in ("--bash", "--pwsh") and i + 1 < len(argv):
            shells["sh" if argv[i] == "--bash" else "ps1"] = argv[i + 1]
            i += 2
            continue
        print("test-check-ci-legs: unknown argument %r" % argv[i])
        return 2
    if not shells:
        print("test-check-ci-legs: CANNOT RUN -- name at least one twin's interpreter (--bash / --pwsh)")
        return 2

    base = OT.git_environment()
    for k in ("GH_REPO", "GH_HOST"):
        base.pop(k, None)
    box = os.path.realpath(tempfile.mkdtemp(prefix="check-ci-legs-test-"))
    ran, failed = [], []

    def arm(ok, label, detail):
        ran.append(label)
        if not ok:
            failed.append(label)
        print("  %-4s %s%s" % ("ok" if ok else "FAIL", label, "" if ok else "   [%s]" % detail))

    try:
        own = os.path.join(box, "own")
        other = os.path.join(box, "other")
        make_repo(own, OWN_BRANCH, OWN_ORIGIN, with_tool=True)
        make_repo(other, OTHER_BRANCH, OTHER_ORIGIN, with_tool=False)
        stubs = {"sh": os.path.join(box, "stub-sh"), "ps1": os.path.join(box, "stub-ps1")}
        write(os.path.join(stubs["sh"], "gh"), STUB_SH)
        os.chmod(os.path.join(stubs["sh"], "gh"), 0o755)
        write(os.path.join(stubs["ps1"], "gh.ps1"), STUB_PS1)
        steer_dir = {"GIT_DIR": os.path.join(other, ".git")}
        steer_both = {"GIT_DIR": os.path.join(other, ".git"), "GIT_WORK_TREE": other}
        steer_index = {"GIT_INDEX_FILE": os.path.abspath(os.path.join(other, ".git", "index"))}

        def bare_branch(extra):
            p = subprocess.run(["git", "-C", own, "rev-parse", "--abbrev-ref", "HEAD"], env=dict(base, **extra),
                               capture_output=True, text=True, encoding="utf-8", errors="replace")
            return p.stdout.strip()

        for twin, shell in sorted(shells.items()):
            subject = os.path.join(own, "scripts", "check-ci-legs", TWINS[twin])
            if not os.path.realpath(subject).startswith(box + os.sep):
                print("test-check-ci-legs: REFUSING -- the subject %s is not inside the fixture box" % subject)
                return 2
            n = [0]

            def run(extra, branch_arg=None):
                n[0] += 1
                log = os.path.join(box, "%s-%d.log" % (twin, n[0]))
                env = dict(base, CI_LEGS_STUB_LOG=log, GIT_CEILING_DIRECTORIES=box, **extra)
                shell_dir = os.path.dirname(shell) if os.path.isabs(shell) else ""
                env["PATH"] = os.pathsep.join([stubs[twin]] + ([shell_dir] if shell_dir else [])
                                              + [env.get("PATH", "")])
                cmd = ([shell, subject] if twin == "sh"
                       else [shell, "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", subject])
                if branch_arg:
                    cmd += ["--branch" if twin == "sh" else "-Branch", branch_arg]
                try:
                    p = subprocess.run(cmd, cwd=box, env=env, capture_output=True, text=True, encoding="utf-8",
                                       errors="replace", timeout=180)
                    said = (p.stdout or "") + (p.stderr or "")
                    rc = p.returncode
                except subprocess.TimeoutExpired:
                    said, rc = "TIMEOUT", None
                return rc, said, calls(log)

            def answered_for_own(rc, rows, branch):
                listed = [r for r in rows if r[0].startswith("run list")]
                return (rc == 0 and len(rows) >= 3 and len(listed) == 1
                        and ("--branch %s " % branch) in (listed[0][0] + " ")
                        and all(same(r[1], own) and r[2] == OWN_ORIGIN for r in rows))

            def show(rc, said, rows):
                return "rc=%s calls=%r said=%r" % (rc, [(r[0][:40], r[1][-24:], r[2][-26:]) for r in rows][:3],
                                                  said.strip().splitlines()[-1:] if said.strip() else [])

            name = TWINS[twin]
            rc, said, rows = run({})
            arm(answered_for_own(rc, rows, OWN_BRANCH),
                "%s: CONTROL -> asks gh about its own branch, and gh's git sees its own tree and origin" % name,
                show(rc, said, rows))

            for label, extra in (("GIT_DIR", steer_dir), ("GIT_DIR + GIT_WORK_TREE", steer_both)):
                neg = bare_branch(extra)
                rc, said, rows = run(extra)
                arm(neg == OTHER_BRANCH and answered_for_own(rc, rows, OWN_BRANCH),
                    "%s: a caller's %s naming another repository reaches neither its git nor gh" % (name, label),
                    "negative (bare git branch)=%r %s" % (neg, show(rc, said, rows)))

            rc, said, rows = run(steer_index)
            arm(answered_for_own(rc, rows, OWN_BRANCH),
                "%s: a caller's ABSOLUTE GIT_INDEX_FILE from another repository changes no answer" % name,
                show(rc, said, rows))

            rc, said, rows = run(steer_dir, branch_arg=GIVEN_BRANCH)
            arm(answered_for_own(rc, rows, GIVEN_BRANCH),
                "%s: with the branch GIVEN, a caller's GIT_DIR still does not reach gh's own git" % name,
                show(rc, said, rows))

            rc, said, rows = run({"GH_REPO": "ge-fixture/ci-legs-other"})
            arm(rc == 2 and "GH_REPO" in said and "ge-fixture/ci-legs-other" in said and not rows,
                "%s: a caller's GH_REPO is REFUSED (exit 2, named) and gh never runs" % name,
                show(rc, said, rows))
    finally:
        box_gone = OT.remove_tree(box)
    arm(box_gone, "the fixture box is removed -- git's read-only objects included", "left behind: %s" % box)

    want = ARMS_PER_TWIN * len(shells) + 1
    if len(ran) != want:
        print("test-check-ci-legs: FAIL -- %d arm(s) ran, %d expected" % (len(ran), want))
        return 1
    if failed:
        print("test-check-ci-legs: FAIL -- %d of %d arm(s)" % (len(failed), len(ran)))
        return 1
    print("test-check-ci-legs: OK -- %d arm(s) over %s" % (len(ran), ", ".join(TWINS[t] for t in sorted(shells))))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
