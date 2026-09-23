# PURPOSE: print the tail of a file, or the newest entries of a directory, inside a leg's tree on the host that leg runs on, redacted and kept, so a remote step's log can be read without a raw ssh session.
"""read-leg-path.py -- read ONE path inside a leg's tree, on the host the leg runs on.

WHY IT EXISTS (✔MEASURED 2026-09-23, P68 round 8): DssHarness 0.5.8 keeps a remote leg's logs on
that leg's host. A failed macOS build reported only "build exited 1"; `build -v` from the
orchestrating host streams no remote child output; and `host-exec -- build` on the Mac refuses,
because a host does not know it IS the leg's host (connection data is, correctly, never synced).
The one sanctioned way to run code on a leg's host is a runner, so reading a remote log is one.

WHAT IT READS: `--path`, relative to the leg's tree (`--tree`, filled in from `{treeDir}`):
  * a FILE -> its last `--lines` lines, or only the lines matching `--match` (a Python regular
    expression, searched per line; `.` keeps every non-empty line);
  * a DIRECTORY -> its newest `--lines` entries, one per line: modified time, size, name
    (a trailing `/` marks a directory).

WHAT IT REFUSES, by name, exit 2, printing nothing read from the path:
  * an absolute `--path`, and any path that resolves OUTSIDE the tree (`..`, a link leaving it);
  * connection data and secrets, because this program PRINTS what it reads: a path with a
    component named `.secrets`, `sshItems`, `.env` or `.ssh`, and a file named `.key`, `*.key`,
    `*.pem` or `id_*`;
  * a path that does not exist, and an unreadable `--match`.

REDACTION, applied to every line printed, names included: the tree's absolute path becomes
`<tree>`, the home directory `~`, the user name `<user>` and the host's name `<host>` (each only
when at least three characters long, so a one-letter name cannot shred the text). What leaves the
host names the repository's files, never the machine or the account.

OUTPUT: the lines, then `read-leg-path: OK <n> line(s) of <path>` -- on stdout, and in `--out`,
which the runner keeps, so `dssharness sync --pull` can bring it back.

`--selftest` runs the refusal and redaction arms against a scratch tree and exits non-zero if
any arm fails; each arm prints its name and verdict.
"""
import argparse
import getpass
import io
import os
import re
import socket
import sys
import tempfile
import time

# What this program prints is another program's log, so any character can reach the pipe: both streams are UTF-8
# from IMPORT on (argument errors and --help print before main()), the repository's rule for every guard and action.
for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError, OSError):
        pass

DENIED_COMPONENTS = {".secrets", "sshitems", ".env", ".ssh"}
MAX_LINES = 5000


def denied(rel):
    parts = [p for p in re.split(r"[\\/]+", rel) if p not in ("", ".")]
    for p in parts:
        if p.lower() in DENIED_COMPONENTS:
            return "a component named %r (connection data or secrets)" % p
    name = parts[-1].lower() if parts else ""
    if name == ".key" or name.endswith(".key") or name.endswith(".pem") or name.startswith("id_"):
        return "a key file name %r" % parts[-1]
    return None


def redactor(tree):
    pairs = []
    home = os.path.expanduser("~")
    try:
        user = getpass.getuser()
    except Exception:  # an account the process table cannot name is simply not redacted by name
        user = ""
    host = socket.gethostname()
    for value, mark in ((tree, "<tree>"), (home, "~")):
        if value and len(value) >= 3:
            pairs.append((value, mark))
            pairs.append((value.replace("\\", "/"), mark))
            pairs.append((value.replace("/", "\\"), mark))
    for value, mark in ((host, "<host>"), (host.split(".")[0], "<host>"), (user, "<user>")):
        if value and len(value) >= 3:
            pairs.append((value, mark))
    pairs.sort(key=lambda p: len(p[0]), reverse=True)  # longest first: the tree before the home it sits in

    def apply(text):
        for value, mark in pairs:
            text = text.replace(value, mark)
        return text
    return apply


def read(tree, rel, lines, match):
    """Return (exit code, output lines). Refusals return code 2 and a single explanatory line."""
    if not rel:
        return 2, ["read-leg-path: REFUSED - --path is empty"]
    if os.path.isabs(rel) or re.match(r"^[A-Za-z]:", rel) or rel.startswith(("/", "\\")):
        return 2, ["read-leg-path: REFUSED - --path must be relative to the leg's tree, not absolute"]
    why = denied(rel)
    if why:
        return 2, ["read-leg-path: REFUSED - --path names %s; this program prints what it reads" % why]
    root = os.path.realpath(tree)
    target = os.path.realpath(os.path.join(root, rel))
    if target != root and not target.startswith(root + os.sep):
        return 2, ["read-leg-path: REFUSED - --path resolves outside the leg's tree"]
    rel_resolved = os.path.relpath(target, root)
    why = denied(rel_resolved)
    if why:  # a link inside the tree may still land on connection data
        return 2, ["read-leg-path: REFUSED - --path resolves to %s" % why]
    if not os.path.exists(target):
        return 2, ["read-leg-path: REFUSED - no such path in the leg's tree: %s" % rel]
    try:
        pattern = re.compile(match)
    except re.error as e:
        return 2, ["read-leg-path: REFUSED - --match is not a regular expression: %s" % e]
    red = redactor(root)
    out = []
    if os.path.isdir(target):
        entries = []
        for name in os.listdir(target):
            p = os.path.join(target, name)
            try:
                st = os.stat(p)
            except OSError:
                continue
            entries.append((st.st_mtime, st.st_size, name + ("/" if os.path.isdir(p) else "")))
        entries.sort(reverse=True)
        for mtime, size, name in entries[:lines]:
            out.append(red("%s %12d  %s" % (time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(mtime)), size, name)))
        what = "%d of %d entr%s of %s/" % (len(out), len(entries), "y" if len(entries) == 1 else "ies", rel)
    else:
        with io.open(target, "rb") as f:
            text = f.read().decode("utf-8", errors="replace").replace("\x00", "")
        kept = [ln for ln in text.splitlines() if ln.strip() and pattern.search(ln)]
        out = [red(ln) for ln in kept[-lines:]]
        what = "%d line(s) of %s" % (len(out), rel)
    out.append("read-leg-path: OK %s" % red(what))
    return 0, out


def selftest():
    failures = 0
    with tempfile.TemporaryDirectory() as tmp:
        tree = os.path.realpath(tmp)
        os.makedirs(os.path.join(tree, "logs"))
        os.makedirs(os.path.join(tree, ".harness-config", "sshItems", "mac"))
        with io.open(os.path.join(tree, "logs", "build.log"), "w", encoding="utf-8") as f:
            f.write("first\nFAILED: step\nat %s/src/a.cpp\nlast\n" % tree)
        with io.open(os.path.join(tree, ".harness-config", "sshItems", "mac", ".env"), "w", encoding="utf-8") as f:
            f.write("ADDRESS=secret\n")
        arms = [
            ("absolute path refused", "/etc/passwd", 2, "not absolute"),
            ("parent escape refused", "../outside", 2, "outside the leg's tree"),
            ("connection data refused", ".harness-config/sshItems/mac/.env", 2, "connection data"),
            ("key name refused", "logs/id_ed25519", 2, "key file"),
            ("missing path refused", "logs/none.log", 2, "no such path"),
            ("tail of a file", "logs/build.log", 0, "OK 4 line(s)"),
            ("the tree path is redacted", "logs/build.log", 0, "<tree>/src/a.cpp"),
            ("a directory is listed", "logs", 0, "build.log"),
        ]
        for name, rel, want_rc, want_text in arms:
            rc, lines = read(tree, rel, 200, ".")
            body = "\n".join(lines)
            ok = rc == want_rc and want_text in body and (want_rc != 0 or "secret" not in body)
            if name == "the tree path is redacted":
                ok = ok and tree not in body
            print("read-leg-path selftest: %-28s %s" % (name, "ok" if ok else "FAIL (rc=%d)\n%s" % (rc, body)))
            failures += 0 if ok else 1
        rc, lines = read(tree, "logs/build.log", 200, "FAILED")
        ok = rc == 0 and lines[0] == "FAILED: step" and "OK 1 line(s)" in lines[-1]
        print("read-leg-path selftest: %-28s %s" % ("--match filters lines", "ok" if ok else "FAIL"))
        failures += 0 if ok else 1
    print("read-leg-path selftest: %s" % ("OK" if failures == 0 else "FAIL - %d arm(s)" % failures))
    return 1 if failures else 0


def main(argv):
    if argv[1:] == ["--selftest"]:
        return selftest()
    ap = argparse.ArgumentParser(description="Read one path inside a leg's tree, redacted.")
    ap.add_argument("--tree", required=True)
    ap.add_argument("--path", required=True)
    ap.add_argument("--lines", type=int, default=200)
    ap.add_argument("--match", default=".")
    ap.add_argument("--out", required=True)
    a = ap.parse_args(argv[1:])
    if not 1 <= a.lines <= MAX_LINES:
        print("read-leg-path: REFUSED - --lines must be between 1 and %d" % MAX_LINES)
        return 2
    rc, lines = read(a.tree, a.path, a.lines, a.match)
    text = "\n".join(lines) + "\n"
    sys.stdout.write(text)
    if rc == 0:
        os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
        with io.open(a.out, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
    return rc


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
