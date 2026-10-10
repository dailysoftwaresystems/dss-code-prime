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
Every option is spelled `--name=value` on the step's run line: a `--match` expression may begin with `-`, and
argparse reads `--match -x` as a second option (exit 2) -- ✔FOUND 2026-09-30 (cycle P69), the self-test pins it.

WHAT IT REFUSES, by name, exit 2, printing nothing read from the path:
  * an absolute `--path`, and any path that resolves OUTSIDE the tree (`..`, a link leaving it);
  * connection data and secrets, because this program PRINTS what it reads: a path with a
    component named `.secrets`, `sshItems`, `.env` or `.ssh`, and a file named `.key`, `*.key`,
    `*.pem` or `id_*`;
  * a path that does not exist, and an unreadable `--match`;
  * a host whose account cannot be named, because its name could then not be redacted.

REDACTION: every line printed, names included, passes through the ONE redactor,
`.harness-config/runner/actions/redact/redact.py`, loaded by path -- the tree becomes `<tree>`, the home `~`, this
host's account `<user>` and its name `<host>`, a home by its shape `<user>` whatever account it names, and the
foreign shapes (`<user>@<host>`, a key's path, an ssh command's connection values, addresses, UNC servers, ...)
are masked too, because a host's copy holds tracked files naming OTHER machines (✔MEASURED 2026-09-25: read on the
WSL leg, a tracked file kept this machine's Windows account three times out of three). The rules, their order, their
limits and the exemptions -- a host named like a word the leg's own tree holds, a C++ scope -- are that file's,
stated once; nothing here restates them.

OUTPUT: the lines, then `read-leg-path: OK <n> line(s) of <path>` -- on stdout, and in `--out`,
which the runner keeps, so `dssharness sync --pull` can bring it back.

`--selftest` runs the refusal, reading and redaction-through-the-reader arms against a scratch tree, counted
against EXPECTED_ARMS, and exits non-zero if any fails or the count differs; each arm prints its name and verdict.
"""
import argparse
import importlib.util
import io
import os
import re
import sys
import tempfile
import time

sys.dont_write_bytecode = True  # this program loads redact.py by path: no __pycache__ beside another action

# What this program prints is another program's log, so any character can reach the pipe: both streams are UTF-8
# from IMPORT on (argument errors and --help print before main()), the repository's rule for every guard and action.
for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError, OSError):
        pass

DENIED_COMPONENTS = {".secrets", "sshitems", ".env", ".ssh"}
MAX_LINES = 5000
_REDACT = []


def redact_module():
    """`.harness-config/runner/actions/redact/redact.py`, the one owner of the redaction rule, loaded ONCE by path
    from this file's sibling directory (a hyphen-free name, but an action's program is never on sys.path). It FAILS
    LOUD when absent rather than falling back to a local copy: a second spelling of what must never leave a host is
    the drift one owner exists to end."""
    if not _REDACT:
        path = os.path.join(os.path.dirname(os.path.dirname(os.path.realpath(__file__))), "redact", "redact.py")
        if not os.path.isfile(path):
            raise SystemExit("read-leg-path: cannot find %s -- the redaction rule lives there and nowhere else" % path)
        spec = importlib.util.spec_from_file_location("dss_redact", path)
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        _REDACT.append(mod)
    return _REDACT[0]


def denied(rel):
    parts = [p for p in re.split(r"[\\/]+", rel) if p not in ("", ".")]
    for p in parts:
        if p.lower() in DENIED_COMPONENTS:
            return "a component named %r (connection data or secrets)" % p
    name = parts[-1].lower() if parts else ""
    if name == ".key" or name.endswith(".key") or name.endswith(".pem") or name.startswith("id_"):
        return "a key file name %r" % parts[-1]
    return None


def read(tree, rel, lines, match, red=None):
    """Return (exit code, output lines). Refusals return code 2 and a single explanatory line. `red` replaces this
    host's redactor (a test's, over the same tree); the OK line says when the redactor walked the tree's vocabulary
    for its one exemption, how many names it read and in how long."""
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
    mod = redact_module()
    try:
        red = red or mod.redactor(root)
    except mod.Unredactable as e:
        return 2, ["read-leg-path: REFUSED - %s" % e]
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
    vocab = red.vocabulary() if hasattr(red, "vocabulary") else None
    walked = "" if vocab is None else " (tree vocabulary: %d name(s) in %.2f s%s)" % (
        len(vocab.names), vocab.seconds, "" if vocab.complete else ", cut short -- fewer names kept, never more")
    out.append("read-leg-path: OK %s%s" % (red(what), walked))
    return 0, out


# ★ AN EXACT RATCHET (the round-12 audit: this self-test counted failures and never how many arms ran, so a deleted
# arm passed). Every arm below is host-independent; the one that reads this host's own names is ONE arm.
EXPECTED_ARMS = 17


def selftest():
    ran, failed = [0], [0]

    def arm(label, ok, detail=""):
        ran[0] += 1
        failed[0] += 0 if ok else 1
        print("read-leg-path selftest: %-40s %s" % (label, "ok" if ok else "FAIL" + (("\n" + detail) if detail
                                                                                      else "")))

    with tempfile.TemporaryDirectory() as tmp:
        tree = os.path.realpath(tmp)
        os.makedirs(os.path.join(tree, "logs"))
        os.makedirs(os.path.join(tree, ".harness-config", "sshItems", "mac"))
        with io.open(os.path.join(tree, "logs", "build.log"), "w", encoding="utf-8") as f:
            f.write("first\nFAILED: step\nat %s/src/a.cpp\n-x marks a line\nlast\n" % tree)
        with io.open(os.path.join(tree, ".harness-config", "sshItems", "mac", ".env"), "w", encoding="utf-8") as f:
            f.write("ADDRESS=secret\n")
        # A home of an account this host has never heard of, as a tracked file carries one (the WSL case), and the
        # foreign shapes a host copy's tracked files can hold.
        with io.open(os.path.join(tree, "logs", "homes.log"), "w", encoding="utf-8") as f:
            f.write("//     input : C:\\Users\\winacct\\AppData\\Local\\Temp\\DSS-SC~1\n"
                    "cd /mnt/c/Users/winacct/src && gcc -I/home/lnxacct/include a.c\n"
                    "//     unc   : //wsl.localhost/Ubuntu/home/lnxacct/p44_unc_inc\n"
                    "wine: Z:\\home\\lnxacct\\test\n"
                    "ssh lnxacct@buildbox.local 10.20.30.40\n")
        arms = [
            ("absolute path refused", "/etc/passwd", 2, "not absolute"),
            ("parent escape refused", "../outside", 2, "outside the leg's tree"),
            ("connection data refused", ".harness-config/sshItems/mac/.env", 2, "connection data"),
            ("key name refused", "logs/id_ed25519", 2, "key file"),
            ("missing path refused", "logs/none.log", 2, "no such path"),
            ("tail of a file", "logs/build.log", 0, "OK 5 line(s)"),
            ("the tree path is redacted", "logs/build.log", 0, "<tree>/src/a.cpp"),
            ("a directory is listed", "logs", 0, "build.log"),
        ]
        for name, rel, want_rc, want_text in arms:
            rc, lines = read(tree, rel, 200, ".")
            body = "\n".join(lines)
            ok = rc == want_rc and want_text in body and (want_rc != 0 or "secret" not in body)
            if name == "the tree path is redacted":
                ok = ok and tree not in body
            arm(name, ok, "rc=%d\n%s" % (rc, body))
        # THROUGH THE REAL INPUT PATH: a stranger's homes and the foreign shapes in a file, read by read(), which
        # takes this host's names and the one redactor's rules.
        rc, lines = read(tree, "logs/homes.log", 200, ".")
        body = "\n".join(lines)
        arm("a stranger's home in a file", rc == 0 and "acct" not in body
            and "C:\\Users\\<user>\\AppData" in body and "/mnt/c/Users/<user>/src" in body
            and "-I/home/<user>/include" in body and "//wsl.localhost/Ubuntu/home/<user>/p44_unc_inc" in body
            and "Z:\\home\\<user>\\test" in body, "rc=%d\n%s" % (rc, body))
        arm("a foreign host and address in a file", rc == 0 and "ssh <user>@<host> <ip>" in body
            and "buildbox" not in body and "10.20" not in body, body)
        rc, lines = read(tree, "logs/build.log", 200, "FAILED")
        arm("--match filters lines", rc == 0 and lines[0] == "FAILED: step" and "OK 1 line(s)" in lines[-1],
            "\n".join(lines))
        # THE ONE EXEMPTION, reached through the reader, with a SYNTHETIC host over this scratch tree: a path the
        # tree holds keeps the name, and the OK line says the vocabulary was walked; a text without the name walks
        # nothing and says nothing.
        os.makedirs(os.path.join(tree, "build", "v1", "bin", "syn"))
        io.open(os.path.join(tree, "build", "v1", "bin", "syn", "syn_examples_runner"), "w").close()
        with io.open(os.path.join(tree, "logs", "syn.log"), "w", encoding="utf-8") as f:
            f.write("ran build/v1/bin/syn/syn_examples_runner on syn\n")
        synthetic = redact_module().redactor(tree, user="ubuntu", host="syn.example", home="/home/ubuntu",
                                             os_family="posix")
        rc, lines = read(tree, "logs/syn.log", 200, ".", red=synthetic)
        arm("a tree-held name is kept, the walk reported", rc == 0
            and lines[0] == "ran build/v1/bin/syn/syn_examples_runner on <host>"
            and "(tree vocabulary: " in lines[-1], "\n".join(lines))
        rc, lines = read(tree, "logs/build.log", 200, ".", red=redact_module().redactor(
            tree, user="ubuntu", host="syn.example", home="/home/ubuntu", os_family="posix"))
        arm("no host name, no walk, no report", rc == 0 and "tree vocabulary" not in lines[-1], lines[-1])
        # A HOST WHOSE ACCOUNT NOBODY CAN NAME: read() refuses and prints nothing it read.
        mod = redact_module()
        real = mod.account_names
        saved = {v: os.environ.pop(v, None) for v in ("USERNAME", "USER", "LOGNAME")}
        real_getuser = mod.getpass.getuser

        def refuse():
            raise OSError("synthetic: no account in the process table")
        mod.account_names = lambda _home: set()
        mod.getpass.getuser = refuse
        try:
            rc, lines = read(tree, "logs/build.log", 200, ".")
        finally:
            mod.account_names = real
            mod.getpass.getuser = real_getuser
            for v, value in saved.items():
                if value is not None:
                    os.environ[v] = value
        arm("read() refuses an unnameable account", rc == 2 and len(lines) == 1
            and "account cannot be named" in lines[0], "rc=%d, %d line(s)" % (rc, len(lines)))
        # THIS HOST'S OWN HOME, whichever leg runs this: ONE arm, and a failure prints only a length.
        home = os.path.expanduser("~")
        with io.open(os.path.join(tree, "logs", "mine.log"), "w", encoding="utf-8") as f:
            f.write("%s/x\n%s\\x\n" % (home, home.replace("/", "\\")))
        rc, lines = read(tree, "logs/mine.log", 200, ".")
        arm("this host's home never survives", rc == 0 and not any(home in ln for ln in lines),
            "a real home survived (%d line(s))" % len(lines))
    # A --match that BEGINS WITH `-`, spelled as the step spells it, reaches the program as its value.
    a = build_parser().parse_args(["--tree=t", "--path=p", "--match=-x marks", "--lines=3", "--out=o"])
    arm("a --match beginning with '-' is a value", a.match == "-x marks" and a.lines == 3, repr(vars(a)))
    yml = os.path.join(os.path.dirname(os.path.realpath(__file__)), "read-leg-path.yml")
    with io.open(yml, encoding="utf-8") as f:
        run_line = next((ln for ln in f if "read-leg-path.py" in ln and "--tree" in ln), "")
    loose = re.findall(r"(--[a-z0-9-]+)(?=\s)", run_line)
    arm("the .yml spells every option --name=value", bool(run_line) and not loose, "loose options: %r" % loose)
    total = ran[0]
    ok = failed[0] == 0 and total == EXPECTED_ARMS
    if total != EXPECTED_ARMS:
        print("read-leg-path selftest: ARM COUNT %d, expected %d -- EXPECTED_ARMS is the ratchet"
              % (total, EXPECTED_ARMS))
    print("read-leg-path selftest: %s" % ("OK (%d arm(s))" % total if ok else "FAIL - %d of %d arm(s)"
                                          % (failed[0], total)))
    return 0 if ok else 1


def build_parser():
    ap = argparse.ArgumentParser(description="Read one path inside a leg's tree, redacted.", allow_abbrev=False)
    ap.add_argument("--tree", required=True)
    ap.add_argument("--path", required=True)
    ap.add_argument("--lines", type=int, default=200)
    ap.add_argument("--match", default=".")
    ap.add_argument("--out", required=True)
    return ap


def main(argv):
    if argv[1:] == ["--selftest"]:
        return selftest()
    a = build_parser().parse_args(argv[1:])
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
