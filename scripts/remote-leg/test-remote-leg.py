#!/usr/bin/env python3
"""test-remote-leg.py -- remote-leg.sh sends leg-tree.sh to its carriage on STDIN, never on the command line: for
the prepare, and for the restore its EXIT trap runs.

ⓘ No `PURPOSE:` line: `check-scripts-index` lets a sibling omit the declaration, and this file is the primary's test,
not a tool of its own.

★★★ WHAT IS PINNED. remote-leg.sh passed `"$(cat leg-tree.sh)"` plus a verb line as ONE carriage argument.
✔MEASURED 2026-09-15 (P66 lane ge) inside WSL, where this driver runs for both carriages: one argument of 131,072
bytes fails at the local execve (MAX_ARG_STRLEN, rc 126) and 131,071 passes -- so a leg worked only while the helper
and its verb line stayed under that, a dependence on the helper's SIZE. Now the carriage is handed
`sh -c '<loader>' leg-tree <bytes> <verb> '<arg>'...` (built by `leg_tree_remote_command` in leg-tree.sh) with the
helper's exact bytes on its stdin. ✔MEASURED the same day on the real arm64 VPS: the loader ran prepare and restore on
a throwaway clone under the host's dash, refused a stream cut short (rc 71), and took a 1.3 MB helper.

★★ HERMETIC. remote-leg.sh is COPIED, with every file it opens from its own tree (leg-tree.sh, carriage-excludes.py
and the owning-tree owner that loads), into a fixture repository whose `scripts/ssh-arm64-vps/ssh-arm64-vps.sh` is a
STUB. The stub records each command and saves the bytes on its stdin, answers the reachability and lock probes,
accepts the rsync, and FAILS the configure (exit 20) -- so the leg dies there and its EXIT trap runs the restore. No
host is reached, nothing is built. Arms:
  (1) prepare's command is the owner's shape, loader and byte count included, under 2 KB, with no helper text
  (2) prepare's stdin is leg-tree.sh, byte for byte
  (3) the EXIT trap's restore is sent the same way, with the same bytes and the leg's commit
  (4) that prepare command, run by `sh` with that stdin, runs the verb (rc 2 at the missing clone), no temp file left
  (5) SIZE: with the helper padded by 150 KB of code lines, (1) and (2) still hold
  (6) the fixture box is removed

usage:  test-remote-leg.py --bash <bash>
exit:   0 every arm held · 1 an arm failed · 2 cannot run
"""
from __future__ import annotations

import importlib.util
import os
import re
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
LEG_TREE_SH = os.path.join(REAL_TREE, "scripts", "leg-tree", "leg-tree.sh")
REMOTE_DIR = "src/Github/dss-code-prime"          # remote-leg.sh's arm64-vps row
BRANCH = "ge-remote-leg-fixture"
EXPECTED_ARMS = 6
DISPATCH_MARK = "# ── dispatch, so this file is BOTH"
COMMAND_RE = re.compile(r"^sh -c '(?P<loader>[^']*)' leg-tree (?P<bytes>[0-9]+) (?P<verb>[a-z_]+)"
                        r"(?P<args>(?: '(?:[^']|'\\'')*')*)$")
ARG_RE = re.compile(r" '((?:[^']|'\\'')*)'")

STUB = r"""#!/usr/bin/env bash
# A STUB arm64-vps carriage: records what remote-leg.sh asks, saves stdin for leg-tree commands, reaches no host.
dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
log="$dir/calls.log"
n=0; [ -f "$log" ] && n=$(wc -l < "$log" | tr -d ' ')
if [ "${1:-}" = "--rsync" ]; then printf 'RSYNC %s\n' "$n" >> "$log"; exit 0; fi
printf '%s' "${1:-}" > "$dir/cmd-$n.txt"
case "${1:-}" in
    "sh -c '"*)    cat > "$dir/stdin-$n.bin"; printf 'LEGTREE %s\n' "$n" >> "$log"; exit 0 ;;
    'uname -sm')   printf 'UNAME %s\n' "$n" >> "$log"; echo 'Linux aarch64'; exit 0 ;;
    *LOCK-TAKEN*)  printf 'LOCK %s\n' "$n" >> "$log"; echo LOCK-TAKEN; exit 0 ;;
    *'cmake -S'*)  printf 'CONFIGURE %s\n' "$n" >> "$log"; exit 20 ;;
    *)             printf 'CMD %s\n' "$n" >> "$log"; exit 0 ;;
esac
"""


def load_owning_tree():
    path = os.path.join(REAL_TREE, "scripts", "owning-tree", "owning-tree.py")
    spec = importlib.util.spec_from_file_location("dss_owning_tree", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


OT = load_owning_tree()


def git(cwd, *args):
    p = OT.run_git(["-C", cwd, "-c", "user.email=ge@example.invalid", "-c", "user.name=ge",
                    "-c", "commit.gpgsign=false"] + list(args),
                   capture_output=True, text=True, encoding="utf-8", errors="replace")
    if p.returncode != 0:
        raise RuntimeError("git %s in %s failed: %s" % (" ".join(args), cwd, p.stderr.strip()))
    return p.stdout.strip()


def place(root, rel, data):
    dest = os.path.join(root, *rel.split("/"))
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    with open(dest, "wb") as fh:
        fh.write(data)


def read(path):
    with open(path, "rb") as fh:
        return fh.read()


def padded(helper):
    text = helper.decode("utf-8")
    at = text.index(DISPATCH_MARK)
    return (text[:at] + "".join(": %s\n" % ("x" * 1000) for _ in range(150)) + text[at:]).encode("utf-8")


def make_fixture(root, helper):
    # `.gitignore` too: carriage-excludes derives the rsync excludes from it and REFUSES (rc 3, "the exclude
    # FLOOR is breached") a tree that does not ignore build/, .secrets/, .worktrees/ and the rest -- ✔measured
    # on the first run of this test, where the leg died there instead of at the stubbed configure.
    for rel in ("scripts/remote-leg/remote-leg.sh", "scripts/carriage-excludes/carriage-excludes.py",
                "scripts/owning-tree/owning-tree.py", ".gitignore"):
        place(root, rel, read(os.path.join(REAL_TREE, *rel.split("/"))))
    place(root, "scripts/leg-tree/leg-tree.sh", helper)
    place(root, "scripts/ssh-arm64-vps/ssh-arm64-vps.sh", STUB.encode("utf-8"))
    place(root, "WHOAMI", b"remote-leg fixture\n")
    git(root, "init", "-q")
    git(root, "checkout", "-q", "-b", BRANCH)
    git(root, "add", "-A")
    git(root, "commit", "-q", "--no-verify", "-m", "fixture")
    return git(root, "rev-parse", "HEAD")


def legtree_calls(root):
    d = os.path.join(root, "scripts", "ssh-arm64-vps")
    log = os.path.join(d, "calls.log")
    out = []
    if not os.path.isfile(log):
        return out, []
    with open(log, encoding="utf-8", errors="replace") as fh:
        kinds = [l.split() for l in fh if l.strip()]
    for kind, n in kinds:
        if kind != "LEGTREE":
            continue
        cmd = read(os.path.join(d, "cmd-%s.txt" % n)).decode("utf-8", "replace")
        data = read(os.path.join(d, "stdin-%s.bin" % n))
        m = COMMAND_RE.match(cmd)
        parsed = None if not m else (m.group("loader"), int(m.group("bytes")), m.group("verb"),
                                     [a.replace("'\\''", "'") for a in ARG_RE.findall(m.group("args"))])
        out.append((cmd, data, parsed))
    return out, [k for k, _n in kinds]


def loader_of(helper):
    for line in helper.decode("utf-8").splitlines():
        if line.startswith("LEG_TREE_REMOTE_LOADER='") and line.endswith("'"):
            return line[len("LEG_TREE_REMOTE_LOADER='"):-1]
    return None


def main(argv):
    if len(argv) != 2 or argv[0] != "--bash":
        print("test-remote-leg: CANNOT RUN -- usage: test-remote-leg.py --bash <bash>")
        return 2
    bash = argv[1]
    base = OT.git_environment()
    base.pop("DSS_LEG_GUARDS", None)
    base.pop("DSS_REMOTE_LEG_JOBS", None)
    box = os.path.realpath(tempfile.mkdtemp(prefix="remote-leg-test-"))
    ran, failed = [], []

    def arm(ok, label, detail):
        ran.append(label)
        if not ok:
            failed.append(label)
        print("  %-4s %s%s" % ("ok" if ok else "FAIL", label, "" if ok else "   [%s]" % detail))

    def drive(root):
        env = dict(base, GIT_CEILING_DIRECTORIES=box)
        path = [os.path.dirname(sys.executable)]
        if os.path.isabs(bash):
            path.insert(0, os.path.dirname(bash))
        env["PATH"] = os.pathsep.join(path + [env.get("PATH", "")])
        try:
            p = subprocess.run([bash, os.path.join(root, "scripts", "remote-leg", "remote-leg.sh"),
                                "--carriage", "arm64-vps", "--mode", "full"],
                               cwd=box, env=env, stdin=subprocess.DEVNULL, capture_output=True, text=True,
                               encoding="utf-8", errors="replace", timeout=300)
            return p.returncode, ((p.stdout or "") + (p.stderr or "")).strip()
        except subprocess.TimeoutExpired:
            return None, "TIMEOUT"

    try:
        helper = read(LEG_TREE_SH)
        loader = loader_of(helper)
        if loader is None:
            print("test-remote-leg: CANNOT RUN -- leg-tree.sh holds no LEG_TREE_REMOTE_LOADER line")
            return 2
        own = os.path.join(box, "own")
        sha = make_fixture(own, helper)
        rc, said = drive(own)
        calls, kinds = legtree_calls(own)
        prepare = [c for c in calls if c[2] and c[2][2] == "prepare"]
        restore = [c for c in calls if c[2] and c[2][2] == "restore"]
        story = "rc=%s kinds=%r tail=%r" % (rc, kinds, said.splitlines()[-2:])

        cmd, data, parsed = prepare[0] if len(prepare) == 1 else ("", b"", None)
        arm(parsed is not None and parsed[0] == loader and parsed[1] == len(helper)
            and parsed[3] == [REMOTE_DIR, BRANCH, sha] and len(cmd) < 2048 and "leg_tree_git_unsteered" not in cmd,
            "(1) prepare's command is the owner's `sh -c '<loader>' leg-tree <bytes> prepare '<dir>' '<branch>' "
            "'<sha>'`, under 2 KB, with none of the helper's text",
            "%s command(%d bytes)=%r" % (story, len(cmd), cmd[:140]))
        arm(parsed is not None and data == helper, "(2) prepare's STDIN is leg-tree.sh, byte for byte",
            "%s stdin=%d helper=%d" % (story, len(data), len(helper)))
        arm(len(restore) == 1 and restore[0][2][3] == [REMOTE_DIR, sha] and restore[0][1] == helper
            and "CONFIGURE" in kinds and kinds.index("CONFIGURE") < kinds.index("LEGTREE", kinds.index("CONFIGURE")),
            "(3) the restore the EXIT trap runs after the failed configure is sent the same way, same bytes, the "
            "leg's commit", "%s restores=%r" % (story, [r[2][3] if r[2] else r[0][:60] for r in restore]))

        if parsed is not None:
            tmp = os.path.join(box, "tmp")
            os.makedirs(tmp, exist_ok=True)
            held = os.path.join(box, "held-stdin.bin")
            place(box, "held-stdin.bin", data)
            env = dict(base, TMPDIR=tmp.replace("\\", "/"))
            if os.path.isabs(bash):
                env["PATH"] = os.path.dirname(bash) + os.pathsep + env.get("PATH", "")
            with open(held, "rb") as fh:
                p = subprocess.run([bash, "-c", cmd], cwd=box, env=env, stdin=fh, capture_output=True, timeout=180)
            out = (p.stdout + p.stderr).decode("utf-8", "replace")
            left = os.listdir(tmp)
            arm(p.returncode == 2 and ("no such directory: %s" % REMOTE_DIR) in out and not left,
                "(4) that command, run by `sh` with that stdin, runs prepare (rc 2 at the missing clone) and leaves "
                "no temp file", "rc=%s left=%r said=%r" % (p.returncode, left, out.strip()[-200:]))
        else:
            arm(False, "(4) that command, run by `sh` with that stdin, runs prepare (rc 2 at the missing clone) and "
                       "leaves no temp file", "no prepare command was recorded: %s" % story)

        big_helper = padded(helper)
        big = os.path.join(box, "big")
        big_sha = make_fixture(big, big_helper)
        rc, said = drive(big)
        calls, kinds = legtree_calls(big)
        prepare = [c for c in calls if c[2] and c[2][2] == "prepare"]
        cmd, data, parsed = prepare[0] if len(prepare) == 1 else ("", b"", None)
        arm(parsed is not None and parsed[1] == len(big_helper) > 140000 and len(cmd) < 2048
            and parsed[3] == [REMOTE_DIR, BRANCH, big_sha] and data == big_helper,
            "(5) SIZE: with the helper padded to %d bytes of code, the command stays small and the stdin exact"
            % len(big_helper),
            "rc=%s kinds=%r command(%d bytes)=%r stdin=%d tail=%r" % (rc, kinds, len(cmd), cmd[:100], len(data),
                                                                       said.splitlines()[-2:]))
    finally:
        box_gone = OT.remove_tree(box)
    arm(box_gone, "(6) the fixture box is removed -- git's read-only objects included", "left behind: %s" % box)

    if len(ran) != EXPECTED_ARMS:
        print("test-remote-leg: FAIL -- %d arm(s) ran, %d expected" % (len(ran), EXPECTED_ARMS))
        return 1
    if failed:
        print("test-remote-leg: FAIL -- %d of %d arm(s)" % (len(failed), len(ran)))
        return 1
    print("test-remote-leg: OK -- %d arm(s): prepare and the trap's restore reach the carriage on stdin, whatever "
          "the helper's size" % len(ran))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
