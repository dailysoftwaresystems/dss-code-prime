#!/usr/bin/env python3
"""test-macos-leg.py -- both macos-leg twins drive the tree THEY live in, whatever the caller's cwd or git environment,
and send leg-tree.sh to the Mac on the carriage's STDIN, never on its command line.

ⓘ No `PURPOSE:` line: `check-scripts-index` lets a sibling omit the declaration, and this file is the primary's test,
not a tool of its own.

★★★ WHAT IS PINNED, EACH MEASURED BEFORE ITS FIX.
  * CWD (P66 lane rr): `macos-leg.sh` took its default source from `$(pwd)` and `macos-leg.ps1` from
    `(Get-Location).Path`, each beside a cwd-relative carriage, so a driver run by path from another checkout's root
    pushed and tested THAT checkout through THAT checkout's carriage. Same class as
    [[D-SCRIPT-LANE-WORKTREE-REPO-ROOT-IS-CWD-KEYED]].
  * GIT ENVIRONMENT (P66 lane ge): ✔MEASURED 2026-09-15 from copies against this stub carriage, under another
    repository's GIT_DIR, and under GIT_DIR + GIT_WORK_TREE, `macos-leg.ps1` pushed its own tree and PREPARED the Mac's
    clone on THAT repository's branch and commit; an absolute GIT_INDEX_FILE changed nothing. The `.sh` twin was
    already immune through `leg_tree_driver_identity`.
  * TRANSPORT (P66 lane ge): both twins handed the carriage leg-tree.sh's TEXT as one command-line argument.
    ✔MEASURED 2026-09-15: a NATIVE ssh.exe refuses an argument of 32,700 characters (the `.sh` twin's 33,901-byte
    helper was already over it wherever `ssh` resolves to one), and the `.ps1` twin fit only by stripping comments,
    with 21,428 characters of headroom left. Now the command is `sh -c '<loader>' leg-tree <bytes> <verb> '<arg>'...`
    and the helper's exact bytes go on the carriage's stdin; `leg_tree_remote_command` in leg-tree.sh owns it.

★★ HOW, WITHOUT A MAC. Each twin is COPIED byte for byte into a throwaway fixture tree whose `scripts/ssh-macos/`
carriage is a STUB: it records every command it is handed, saves the bytes on its stdin, and never opens a
connection. Every run ends at the missing witness -- the stub never answers the remote body -- which is the driver
refusing, as it must with no Mac behind the carriage, and that refusal runs the restore. Arms, per twin:
  cwd        CONTROL own tree; cwd inside ANOTHER repository holding its own carriage (so a cwd-keyed driver has a
             wrong tree it CAN reach); cwd inside NO repository; an EXPLICIT --src / -Src that must win
  git env    a caller's GIT_DIR, GIT_DIR + GIT_WORK_TREE, and an absolute GIT_INDEX_FILE naming the other repository
             -> still prepares its own branch at its own commit (each steering negative proven by a bare git first)
  transport  the prepare command is the owner's shape and carries no helper text; its stdin is the helper byte for
             byte; and that exact command, run by `sh` with that exact stdin, runs the verb (rc 2 at the missing
             destination) and removes its temp file
  size       with the helper padded by 150 KB of CODE lines (past every measured ceiling, and past the old `.ps1`
             comment strip), the command and stdin still hold
plus, with both twins, the `.ps1` twin sends the `.sh` twin's command byte for byte; and the fixture box is removed.

⚠ SAFETY IS STRUCTURAL, NOT HOPED FOR: the subject is always the COPY inside the fixture box, its carriage is the stub
inside the box, the destination is a path that exists nowhere, and this driver refuses to start a run whose subject
resolves outside the box.

usage:  test-macos-leg.py --bash <bash> [--pwsh <pwsh>]      (bash always: it runs the recorded commands)
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
TWINS = {"sh": "macos-leg.sh", "ps1": "macos-leg.ps1"}
ARMS_PER_TWIN = 11
# ⓘ NOT named `DST`: `carriage_paths_guard` reads a `SRC`/`DST` assignment in a leg's directory as
# a repository-root CLAIM that must name the project, and this value deliberately names nothing.
NOWHERE = "/rr-macos-leg-test-destination-that-exists-nowhere"
DISPATCH_MARK = "# ── dispatch, so this file is BOTH"
COMMAND_RE = re.compile(r"^sh -c '(?P<loader>[^']*)' leg-tree (?P<bytes>[0-9]+) (?P<verb>[a-z_]+)"
                        r"(?P<args>(?: '(?:[^']|'\\'')*')*)$")
ARG_RE = re.compile(r" '((?:[^']|'\\'')*)'")

STUB_SH = r"""#!/usr/bin/env bash
# A STUB carriage: records what the driver asked for, saves its stdin, and never opens a connection.
dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
log="$dir/calls.log"
n=0; [ -f "$log" ] && n=$(wc -l < "$log" | tr -d ' ')
case "${1:-}" in
    --push) printf 'PUSH-WHOAMI=%s\n' "$(cat "${3:-/nonexistent}/WHOAMI" 2>/dev/null || echo '<unreadable>')" >> "$log" ;;
    'bash -s') cat >/dev/null; printf 'BODY\n' >> "$log" ;;
    *) printf '%s' "${1:-}" > "$dir/cmd-$n.txt"; cat > "$dir/stdin-$n.bin"; printf 'CMD %s\n' "$n" >> "$log" ;;
esac
exit 0
"""

STUB_PS1 = r"""param([string]$Command = '', [string]$PushSource = '', [string]$PushDest = '', [switch]$Prune)
# A STUB carriage: records what the driver asked for, saves its stdin, and never opens a connection.
$log = Join-Path $PSScriptRoot 'calls.log'
if ($PushSource) {
    $who = try { (Get-Content -Raw -LiteralPath (Join-Path $PushSource 'WHOAMI')).Trim() } catch { '<unreadable>' }
    Add-Content -LiteralPath $log -Value "PUSH-WHOAMI=$who"
    exit 0
}
# ⚠ NO `$input` ANYWHERE IN THIS FILE: a -File script that merely MENTIONS it has its whole redirected stdin
# drained by pwsh before the script runs, so a native child then reads nothing (✔measured, see the CMD branch).
if ($Command -eq 'bash -s') {
    & $env:MACOS_LEG_STUB_PYTHON -c 'import sys; sys.stdin.buffer.read()'
    Add-Content -LiteralPath $log -Value 'BODY'
    exit 0
}
$n = @(Get-Content -LiteralPath $log -ErrorAction SilentlyContinue).Count
[IO.File]::WriteAllText((Join-Path $PSScriptRoot "cmd-$n.txt"), $Command)
# The bytes are read by a NATIVE child inheriting this process's stdin -- exactly how the real carriage's
# `& ssh` receives them. (An in-process [Console]::OpenStandardInput() read 0 bytes here: pwsh's own host
# does not hand a -File script its redirected stdin that way, so it would measure the stub, not the driver.)
& $env:MACOS_LEG_STUB_PYTHON -c 'import sys; open(sys.argv[1], "wb").write(sys.stdin.buffer.read())' (Join-Path $PSScriptRoot "stdin-$n.bin")
Add-Content -LiteralPath $log -Value "CMD $n"
exit 0
"""


def load_owning_tree():
    path = os.path.join(REAL_TREE, "scripts", "owning-tree", "owning-tree.py")
    spec = importlib.util.spec_from_file_location("dss_owning_tree", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


OT = load_owning_tree()


def git(cwd, *args):
    p = OT.run_git(["-C", cwd, "-c", "user.email=rr@example.invalid", "-c", "user.name=rr test",
                    "-c", "commit.gpgsign=false"] + list(args),
                   capture_output=True, text=True, encoding="utf-8", errors="replace")
    if p.returncode != 0:
        raise RuntimeError("git %s in %s failed: %s" % (" ".join(args), cwd, p.stderr.strip()))
    return p.stdout.strip()


def write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(text)


def padded_helper():
    """leg-tree.sh with 150 KB of CODE lines before its dispatch -- code, so no comment strip could drop them."""
    with open(LEG_TREE_SH, "rb") as fh:
        text = fh.read().decode("utf-8")
    at = text.index(DISPATCH_MARK)
    pad = "".join(": %s\n" % ("x" * 1000) for _ in range(150))
    return text[:at] + pad + text[at:]


def make_repo(path, whoami, branch, with_driver, helper_text=None):
    """A git repository on `branch` holding `WHOAMI`; with the leg's files and stub carriages if asked."""
    write(os.path.join(path, "WHOAMI"), whoami + "\n")
    if with_driver:
        # The driver AND everything a driver reads from its tree -- so a cwd-keyed driver standing here
        # would find a complete, reachable wrong tree.
        for name in TWINS.values():
            dest = os.path.join(path, "scripts", "macos-leg", name)
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            shutil.copyfile(os.path.join(HERE, name), dest)
        for rel in (("scripts", "repo-tree", "repo-tree.ps1"),):
            dest = os.path.join(path, *rel)
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            shutil.copyfile(os.path.join(REAL_TREE, *rel), dest)
        dest = os.path.join(path, "scripts", "leg-tree", "leg-tree.sh")
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        if helper_text is None:
            shutil.copyfile(LEG_TREE_SH, dest)
        else:
            with open(dest, "w", encoding="utf-8", newline="\n") as fh:
                fh.write(helper_text)
    write(os.path.join(path, "scripts", "ssh-macos", "ssh-macos.sh"), STUB_SH)
    write(os.path.join(path, "scripts", "ssh-macos", "ssh-macos.ps1"), STUB_PS1)
    git(path, "init", "-q")
    git(path, "checkout", "-q", "-b", branch)
    git(path, "add", "-A")
    git(path, "commit", "-q", "--no-verify", "-m", "fixture " + whoami)
    return git(path, "rev-parse", "HEAD")


def carriage_dir(repo):
    return os.path.join(repo, "scripts", "ssh-macos")


def records(repo):
    """[(kind, value)] from a stub's log: ('PUSH', whoami) · ('BODY', '') · ('CMD', (command, stdin bytes))."""
    d = carriage_dir(repo)
    log = os.path.join(d, "calls.log")
    if not os.path.isfile(log):
        return []
    out = []
    with open(log, encoding="utf-8", errors="replace") as fh:
        for line in (l.strip() for l in fh):
            if line.startswith("PUSH-WHOAMI="):
                out.append(("PUSH", line.split("=", 1)[1]))
            elif line == "BODY":
                out.append(("BODY", ""))
            elif line.startswith("CMD "):
                n = line.split(" ", 1)[1]
                try:
                    with open(os.path.join(d, "cmd-%s.txt" % n), "rb") as fc:
                        cmd = fc.read().decode("utf-8", "replace")
                    with open(os.path.join(d, "stdin-%s.bin" % n), "rb") as fs:
                        data = fs.read()
                except OSError:
                    cmd, data = "<unreadable>", b""
                out.append(("CMD", (cmd, data)))
    return out


def clear(*repos):
    for r in repos:
        d = carriage_dir(r)
        for name in os.listdir(d):
            if name == "calls.log" or name.startswith("cmd-") or name.startswith("stdin-"):
                os.remove(os.path.join(d, name))


def parse(cmd):
    """-> (loader, bytes, verb, [args]) for the owner's command shape, or None."""
    m = COMMAND_RE.match(cmd)
    if not m:
        return None
    args = [a.replace("'\\''", "'") for a in ARG_RE.findall(m.group("args"))]
    return m.group("loader"), int(m.group("bytes")), m.group("verb"), args


def verb_calls(recs, verb):
    return [(cmd, data, parse(cmd)) for kind, value in recs if kind == "CMD"
            for cmd, data in [value] if parse(cmd) and parse(cmd)[2] == verb]


def loader_of(helper_bytes):
    for line in helper_bytes.decode("utf-8").splitlines():
        if line.startswith("LEG_TREE_REMOTE_LOADER='") and line.endswith("'"):
            return line[len("LEG_TREE_REMOTE_LOADER='"):-1]
    return None


def verdict(recs, whoami, branch, sha):
    """The tree PUSHED, the branch and commit PREPARED, and a RESTORE that names the destination.

    ★ The restore half is not decoration: every run here ends refused (no witness can come back from a stub), and a
    refusal that does not restore leaves a real Mac's clone dirty. It also proves the verbs carry their ARGUMENTS.
    """
    pushed = [v for k, v in recs if k == "PUSH"]
    prepared = verb_calls(recs, "prepare")
    restored = verb_calls(recs, "restore")
    ok = (pushed == [whoami] and len(prepared) == 1 and prepared[0][2][3] == [NOWHERE, branch, sha]
          and len(restored) >= 1 and restored[0][2][3][:1] == [NOWHERE])
    return ok, "pushed=%r prepared=%r restored=%r other-cmds=%r" % (
        pushed, [p[2][3] for p in prepared], [r[2][3] for r in restored],
        [v[0][:60] for k, v in recs if k == "CMD" and not parse(v[0])])


def main(argv):
    shells = {}
    i = 0
    while i < len(argv):
        if argv[i] in ("--bash", "--pwsh") and i + 1 < len(argv):
            shells["sh" if argv[i] == "--bash" else "ps1"] = argv[i + 1]
            i += 2
            continue
        print("test-macos-leg: unknown argument %r" % argv[i])
        return 2
    if "sh" not in shells:
        print("test-macos-leg: CANNOT RUN -- --bash is required (it drives the .sh twin and runs the recorded "
              "commands of both)")
        return 2
    bash = shells["sh"]

    base = OT.git_environment()
    base.pop("DSS_LEG_GUARDS", None)
    box = os.path.realpath(tempfile.mkdtemp(prefix="macos-leg-test-"))
    ran, failed = [], []

    def arm(ok, label, detail):
        ran.append(label)
        if not ok:
            failed.append(label)
        print("  %-4s %s%s" % ("ok" if ok else "FAIL", label, "" if ok else "   [%s]" % detail))

    try:
        own = os.path.join(box, "own")
        foreign = os.path.join(box, "foreign")
        explicit = os.path.join(box, "explicit")
        big = os.path.join(box, "big")
        nonrepo = os.path.join(box, "nonrepo")
        tmp = os.path.join(box, "tmp")
        own_sha = make_repo(own, "own", "rr-fixture-own", with_driver=True)
        make_repo(foreign, "foreign", "rr-fixture-foreign", with_driver=True)
        explicit_sha = make_repo(explicit, "explicit", "rr-fixture-explicit", with_driver=False)
        big_text = padded_helper()
        big_sha = make_repo(big, "big", "rr-fixture-big", with_driver=True, helper_text=big_text)
        os.makedirs(nonrepo)
        os.makedirs(tmp)
        with open(os.path.join(own, "scripts", "leg-tree", "leg-tree.sh"), "rb") as fh:
            own_helper = fh.read()
        with open(os.path.join(big, "scripts", "leg-tree", "leg-tree.sh"), "rb") as fh:
            big_helper = fh.read()
        loader = loader_of(own_helper)
        if loader is None:
            print("test-macos-leg: CANNOT RUN -- leg-tree.sh holds no LEG_TREE_REMOTE_LOADER line")
            return 2
        foreign_git = os.path.join(foreign, ".git")
        control_prepare = {}

        def drive(twin, subject, cwd, extra_args=(), steer=None):
            env = dict(base, DSS_MACOS_LEG_DIR=NOWHERE, TMPDIR=tmp, TEMP=tmp, TMP=tmp,
                       GIT_CEILING_DIRECTORIES=box, MACOS_LEG_STUB_PYTHON=sys.executable, **(steer or {}))
            shell = shells[twin]
            if os.path.isabs(shell):
                env["PATH"] = os.path.dirname(shell) + os.pathsep + env.get("PATH", "")
            cmd = ([shell, subject] if twin == "sh"
                   else [shell, "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", subject]) + list(extra_args)
            try:
                p = subprocess.run(cmd, cwd=cwd, env=env, stdin=subprocess.DEVNULL, capture_output=True,
                                   text=True, encoding="utf-8", errors="replace", timeout=240)
                return ((p.stdout or "") + (p.stderr or "")).strip().splitlines()[-1:]
            except subprocess.TimeoutExpired:
                return ["TIMEOUT"]

        def run_recorded(cmd, data):
            """The recorded command, run by `sh` (through bash) with the recorded stdin, in the fixture's tmp."""
            held = os.path.join(box, "recorded-stdin.bin")
            with open(held, "wb") as fh:
                fh.write(data)
            before = set(os.listdir(tmp))
            env = dict(base, TMPDIR=tmp.replace("\\", "/"))
            if os.path.isabs(bash):
                env["PATH"] = os.path.dirname(bash) + os.pathsep + env.get("PATH", "")
            with open(held, "rb") as fh:
                p = subprocess.run([bash, "-c", cmd], cwd=box, env=env, stdin=fh, capture_output=True,
                                   timeout=180)
            said = (p.stdout + p.stderr).decode("utf-8", "replace")
            return p.returncode, said, sorted(set(os.listdir(tmp)) - before)

        for twin in sorted(shells):
            name = TWINS[twin]
            subject = os.path.join(own, "scripts", "macos-leg", name)
            if not os.path.realpath(subject).startswith(box + os.sep):
                print("test-macos-leg: REFUSING -- the subject %s is not inside the fixture box" % subject)
                return 2

            # ── cwd ────────────────────────────────────────────────────────────────
            for label, cwd, extra, whoami, branch, sha in (
                    ("CONTROL cwd = its own tree", own, [], "own", "rr-fixture-own", own_sha),
                    ("cwd inside ANOTHER repository holding its own carriage", foreign, [], "own",
                     "rr-fixture-own", own_sha),
                    ("cwd inside NO repository", nonrepo, [], "own", "rr-fixture-own", own_sha),
                    ("an EXPLICIT source still wins", foreign, ["--src" if twin == "sh" else "-Src", explicit],
                     "explicit", "rr-fixture-explicit", explicit_sha)):
                clear(own, foreign)
                tail = drive(twin, subject, cwd, extra)
                recs = records(own) + records(foreign)
                ok, detail = verdict(recs, whoami, branch, sha)
                if label.startswith("CONTROL"):
                    control_prepare[twin] = verb_calls(recs, "prepare")
                arm(ok, "%s: %s -> pushes and prepares %s, restores on refusal" % (name, label, whoami),
                    "%s; driver said %r" % (detail, tail))

            # ── the caller's git environment ───────────────────────────────────────
            for label, steer in (("GIT_DIR", {"GIT_DIR": foreign_git}),
                                 ("GIT_DIR + GIT_WORK_TREE", {"GIT_DIR": foreign_git, "GIT_WORK_TREE": foreign}),
                                 ("an ABSOLUTE GIT_INDEX_FILE",
                                  {"GIT_INDEX_FILE": os.path.abspath(os.path.join(foreign_git, "index"))})):
                neg = subprocess.run(["git", "-C", own, "rev-parse", "--abbrev-ref", "HEAD"],
                                     env=dict(base, **steer), capture_output=True, text=True,
                                     encoding="utf-8", errors="replace").stdout.strip()
                expect_steered = "GIT_INDEX_FILE" not in label
                clear(own, foreign)
                tail = drive(twin, subject, own, steer=steer)
                ok, detail = verdict(records(own) + records(foreign), "own", "rr-fixture-own", own_sha)
                arm(ok and (neg == "rr-fixture-foreign") == expect_steered,
                    "%s: a caller's %s naming another repository -> still prepares its own branch at its own "
                    "commit" % (name, label),
                    "bare-git-branch=%r %s; driver said %r" % (neg, detail, tail))

            # ── the transport, read off the CONTROL run ─────────────────────────────
            calls = control_prepare.get(twin) or []
            cmd, data, parsed = calls[0] if len(calls) == 1 else ("", b"", None)
            arm(parsed is not None and parsed[0] == loader and parsed[1] == len(own_helper)
                and len(cmd) < 2048 and "leg_tree_git_unsteered" not in cmd,
                "%s: prepare's command is the owner's `sh -c '<loader>' leg-tree <bytes> prepare ...`, under 2 KB, "
                "and carries none of the helper's text" % name,
                "command(%d bytes)=%r" % (len(cmd), cmd[:160]))
            arm(parsed is not None and data == own_helper,
                "%s: prepare's STDIN is leg-tree.sh byte for byte" % name,
                "stdin=%d bytes, helper=%d bytes" % (len(data), len(own_helper)))
            if parsed is not None:
                rc, said, left = run_recorded(cmd, data)
            else:
                rc, said, left = None, "no prepare command was recorded", []
            arm(rc == 2 and ("no such directory: %s" % NOWHERE) in said and not left,
                "%s: that command, run by `sh` with that stdin, runs prepare (rc 2 at the missing destination) "
                "and leaves no temp file" % name,
                "rc=%s left=%r said=%r" % (rc, left, said.strip()[-200:]))

            # ── size independence ──────────────────────────────────────────────────
            clear(big)
            tail = drive(twin, os.path.join(big, "scripts", "macos-leg", name), big)
            calls = verb_calls(records(big), "prepare")
            cmd, data, parsed = calls[0] if len(calls) == 1 else ("", b"", None)
            arm(parsed is not None and parsed[1] == len(big_helper) > 140000 and len(cmd) < 2048
                and parsed[3] == [NOWHERE, "rr-fixture-big", big_sha] and data == big_helper,
                "%s: with the helper padded to %d bytes of code, the command stays small and the stdin exact"
                % (name, len(big_helper)),
                "command(%d bytes)=%r stdin=%d bytes; driver said %r" % (len(cmd), cmd[:120], len(data), tail))

        if len(shells) == 2:
            a = [c[0] for c in control_prepare.get("sh") or []]
            b = [c[0] for c in control_prepare.get("ps1") or []]
            arm(len(a) == 1 and a == b,
                "the .ps1 twin sends the .sh twin's prepare command byte for byte",
                "sh=%r ps1=%r" % ([x[:80] for x in a], [x[:80] for x in b]))
    finally:
        box_gone = OT.remove_tree(box)
    arm(box_gone, "the fixture box is removed -- git's read-only objects included", "left behind: %s" % box)

    want = ARMS_PER_TWIN * len(shells) + (1 if len(shells) == 2 else 0) + 1
    if len(ran) != want:
        print("test-macos-leg: FAIL -- %d arm(s) ran, %d expected" % (len(ran), want))
        return 1
    if failed:
        print("test-macos-leg: FAIL -- %d of %d arm(s)" % (len(failed), len(ran)))
        return 1
    print("test-macos-leg: OK -- %d arm(s) over %s" % (len(ran), ", ".join(TWINS[t] for t in sorted(shells))))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
