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
  * a path that does not exist, and an unreadable `--match`;
  * a host whose account cannot be named (no `getpass` answer and no home directory), because its
    name could then not be redacted.

REDACTION, applied to every line printed, names included: the tree's absolute path becomes
`<tree>`, the home directory `~`, the user name `<user>` and the host's name `<host>` (each only
when at least three characters long, so a one-letter name cannot shred the text). What leaves the
host names the repository's files, never the machine or the account. ★ A PATH is replaced wherever
it occurs; a NAME wherever it is not part of a longer run of ASCII letters or digits -- so `_`, `-`,
`.`, `@`, `/`, a backslash, `:` and blanks all end one, and `<name>_x`, `<name>-mac`, `<name>.local`,
`<name>@h`, `/home/<name>/` and `C:\\Users\\<name>\\` are all redacted, exactly as the substring rule
redacted them -- because a name is also a piece of other words: ✔MEASURED 2026-09-24, on the arm64
VPS a gcc version string came back as `13.3.0-6<user>2`, the account's name cut out of a word. The
self-test pins both directions: the words a name is part of survive, and no shape the substring rule
caught leaks. probe-reference-cc loads this redactor by path and adds nothing to the rule.
★ A HOME DIRECTORY IS ALSO REDACTED BY ITS SHAPE, whatever account it names, at any length: the path
component after ANY component named `home` or `Users` (any case, either slash, doubled backslashes)
becomes `<user>` -- one rule for every spelling a home takes on some host: `/home/<name>`,
`/Users/<name>`, `C:\\Users\\<name>`, `/mnt/c/Users/<name>`, `/c/Users/<name>`,
`/cygdrive/c/Users/<name>`, `\\\\wsl.localhost\\<distro>\\home\\<name>` and `\\\\wsl$\\...`, Wine's
`Z:\\home\\<name>`, a share's `\\\\server\\home\\<name>`, `file:///home/<name>`, `-I/home/<name>`.
✔MEASURED 2026-09-25, twice: read on the WSL leg, a tracked file naming this machine's Windows home
kept the Windows account name three times out of three (the WSL host knows its own account, which is
another one); then the list of prefixes that fix enumerated missed the next ones -- the WSL home as
Windows names it and Wine's -- which a tracked file also holds. So the host's own names are the first
rule, never the only one, and the shape is keyed on the COMPONENT, not on a list of prefixes. The
repository tracks no directory named `home` or `Users` (✔MEASURED 2026-09-25, `git ls-files`), so
the rule costs it nothing; should one appear, its children are over-redacted in what this prints,
and a word that is not an account (`/Users/Shared`) is too -- over-redacting is the direction a
redactor may fail in.

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


# The characters that make a NAME part of a LONGER run -- ASCII letters and digits only: a name inside
# `13.3.0-6ubuntu2` or `dsscp` is a piece of another word, not the account or the machine, while `_`, `-`,
# `.`, `@`, `/`, a backslash, `:` and blanks all end one, so every shape the substring rule caught is still
# caught.
NAME_CHARS = "A-Za-z0-9"

# A HOME DIRECTORY BY ITS SHAPE (see REDACTION above): group 1 -- a separator, `home` or `Users` in any case, and the
# separator(s) after it -- is kept, and group 2 -- the account -- becomes `<user>`. The account takes every character
# Windows allows in a user name (so every POSIX name too): not a separator, a blank, `"` `'` `<` `>` `|` `:` `;` `,`
# `=` `+` `*` `?` `[` `]`, except that a blank or an apostrophe BETWEEN two of those characters belongs to it
# (`Ann Lee`, `O'Brien`) while a trailing quote does not. It never takes `<`, so `<user>` is never taken for a name a
# second time.
_SEP = r"(?:\\|/)"
_ACCOUNT_CHAR = r"[^\\/\s\"'<>|:;,=+*?\[\]]"
_ACCOUNT = _ACCOUNT_CHAR + r"+(?:[ ']" + _ACCOUNT_CHAR + r"+)*"
HOME_SHAPE = re.compile(r"(?i)(" + _SEP + r"(?:home|users)" + _SEP + r"+)(" + _ACCOUNT + r")")


class Unredactable(Exception):
    """This host's account cannot be named, so what this program prints cannot be redacted: a refusal, never a
    silent pass."""


def account_name(home):
    """This process's account: `getpass` (which reads LOGNAME, USER, LNAME and USERNAME, then the password
    database), else the last component of `home` -- an account the process table cannot name still owns a home.
    '' only when both are silent. ✔FOUND 2026-09-25 (the round-12 audit): a `getpass` failure used to leave the
    account unredacted by name, silently, and the own-host self-test arm then checked nothing."""
    try:
        name = getpass.getuser()
    except Exception:
        name = ""
    if not name and home:
        name = os.path.basename(home.rstrip("/\\"))
    return name


def redactor(tree, user=None, host=None, home=None):
    """-> apply(text). `user`, `host` and `home` default to this process's account, machine and home directory; a
    test passes its own, so what it asserts never depends on the host it happens to run on. Raises `Unredactable`
    when the account cannot be named."""
    pairs = []
    if home is None:
        home = os.path.expanduser("~")
    if user is None:
        user = account_name(home)
        if not user:
            raise Unredactable("this host's account cannot be named (getpass failed and there is no home directory), "
                               "so what this program prints cannot be redacted")
    if host is None:
        host = socket.gethostname()
    for value, mark in ((tree, "<tree>"), (home, "~")):
        if value and len(value) >= 3:
            pairs.append((value, mark))
            pairs.append((value.replace("\\", "/"), mark))
            pairs.append((value.replace("/", "\\"), mark))
    pairs.sort(key=lambda p: len(p[0]), reverse=True)  # longest first: the tree before the home it sits in
    names = sorted({(value, mark) for value, mark in ((host, "<host>"), (host.split(".")[0], "<host>"),
                                                      (user, "<user>")) if value and len(value) >= 3},
                   key=lambda p: len(p[0]), reverse=True)  # the full host name before its first label
    words = [(re.compile("(?<![%s])%s(?![%s])" % (NAME_CHARS, re.escape(value), NAME_CHARS)), mark)
             for value, mark in names]

    def apply(text):
        for value, mark in pairs:   # a path, wherever it occurs
            text = text.replace(value, mark)
        text = HOME_SHAPE.sub(lambda m: m.group(1) + "<user>", text)  # a home by its shape, whatever account
        for rx, mark in words:      # a name, only as a word or a path component
            text = rx.sub(mark, text)
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
    try:
        red = redactor(root)
    except Unredactable as e:
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
        # A home of an account this host has never heard of, as a tracked file carries one (the WSL case below).
        with io.open(os.path.join(tree, "logs", "homes.log"), "w", encoding="utf-8") as f:
            f.write("//     input : C:\\Users\\winacct\\AppData\\Local\\Temp\\DSS-SC~1\n"
                    "cd /mnt/c/Users/winacct/src && gcc -I/home/lnxacct/include a.c\n"
                    "//     unc   : //wsl.localhost/Ubuntu/home/lnxacct/p44_unc_inc\n"
                    "wine: Z:\\home\\lnxacct\\test\n")
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
        # ★ EVERY NAME SYNTHESIZED, THE HOME INCLUDED -- and on purpose names that COLLIDE: an account named like a
        # distro word, its home the home rule's own, and a host named like this project's prefix. ✔MEASURED
        # 2026-09-25: with the home taken from the process, the arm64 VPS -- whose account IS that distro word --
        # ran the `/home/<name>/` arm red, and only there.
        red = redactor(tree, user="ubuntu", host="dss.example", home="/home/ubuntu")
        # KEPT: a name that is part of a longer run of letters or digits is another word.
        for label, text in (("a name inside a version is KEPT", "gcc 13.3.0-6ubuntu2"),
                            ("a name inside a program name is KEPT", "dsscp.exe --version")):
            got = red(text)
            ok = got == text
            print("read-leg-path selftest: %-28s %s" % (label, "ok" if ok else "FAIL: %r -> %r" % (text, got)))
            failures += 0 if ok else 1
        # NO LEAK: every shape the old substring rule redacted is still redacted.
        for text in ("ubuntu_x", "ubuntu-mac", "ubuntu.local", "ubuntu@h", "/home/ubuntu/", "C:\\Users\\ubuntu\\",
                     "dss_x", "dss-mac", "dss.local", "x@dss.example:", "ssh dss"):
            got = red(text)
            ok = "ubuntu" not in got and "dss" not in got and ("<user>" in got or "<host>" in got or "~" in got)
            print("read-leg-path selftest: %-28s %s" % ("no leak in %r" % text,
                                                     "ok" if ok else "FAIL: %r -> %r" % (text, got)))
            failures += 0 if ok else 1
        # COLLISIONS, the same on every leg: the synthesized home is the home rule's, and a host named like the
        # project's prefix over-redacts that vocabulary -- the direction a redactor may fail in -- never letting it by.
        for label, text, want in (("the synthesized home is ~", "/home/ubuntu/src/a.cpp", "~/src/a.cpp"),
                                  ("a host named like the prefix", "bin/dss/dss_examples_runner",
                                   "bin/<host>/<host>_examples_runner")):
            got = red(text)
            ok = got == want
            print("read-leg-path selftest: %-28s %s" % (label, "ok" if ok else "FAIL: %r -> %r" % (text, got)))
            failures += 0 if ok else 1
        # HOME SHAPES, for accounts this redactor was never told about.
        stranger = redactor(tree, user="someone", host="h.example", home="/home/someone")
        for text, want in (
                ("C:\\Users\\winacct\\AppData\\Local\\Temp\\x", "C:\\Users\\<user>\\AppData\\Local\\Temp\\x"),
                ("path C:\\\\Users\\\\winacct\\\\src", "path C:\\\\Users\\\\<user>\\\\src"),
                ("c:/users/winacct/x", "c:/users/<user>/x"),
                ("D:\\Users\\Ann Lee\\AppData", "D:\\Users\\<user>\\AppData"),
                ("/mnt/c/Users/winacct/AppData", "/mnt/c/Users/<user>/AppData"),
                ("/mnt/d/users/Ann Lee/x", "/mnt/d/users/<user>/x"),
                ("cd /c/Users/winacct/src", "cd /c/Users/<user>/src"),
                ("/Users/macacct/Library/x", "/Users/<user>/Library/x"),
                ("cd /home/lnxacct/src", "cd /home/<user>/src"),
                ("HOME=/home/ab", "HOME=/home/<user>"),
                ("gcc -I/home/lnxacct/include", "gcc -I/home/<user>/include"),
                ("'/home/lnxacct'", "'/home/<user>'"),
                # ✔MEASURED 2026-09-25 (the round-12 audit): the WSL home as Windows names it, and Wine's, were kept
                # by the list of prefixes the first fix enumerated -- and a tracked file holds both.
                ("//wsl.localhost/Ubuntu/home/lnxacct/p44", "//wsl.localhost/Ubuntu/home/<user>/p44"),
                ("\\\\wsl.localhost\\Ubuntu\\home\\lnxacct\\x", "\\\\wsl.localhost\\Ubuntu\\home\\<user>\\x"),
                ("\\\\wsl$\\Ubuntu\\home\\lnxacct", "\\\\wsl$\\Ubuntu\\home\\<user>"),
                ("C:\\wsl.localhost\\Ubuntu\\home\\lnxacct\\x", "C:\\wsl.localhost\\Ubuntu\\home\\<user>\\x"),
                ("Z:\\home\\lnxacct\\test", "Z:\\home\\<user>\\test"),
                ("Z:/home/lnxacct/src", "Z:/home/<user>/src"),
                ("/cygdrive/c/Users/winacct/x", "/cygdrive/c/Users/<user>/x"),
                ("\\\\fileserver\\home\\lnxacct\\docs", "\\\\fileserver\\home\\<user>\\docs"),
                ("file:///home/lnxacct/a.c", "file:///home/<user>/a.c"),
                ("/HOME/Lnxacct/x", "/HOME/<user>/x"),
                ("C:\\Users\\O'Brien\\x", "C:\\Users\\<user>\\x"),
                # OVER-REDACTED, the permitted direction: the repository tracks no `home` or `Users` directory.
                ("docs/home/guide.md", "docs/home/<user>")):
            got = stranger(text)
            ok = got == want
            print("read-leg-path selftest: %-28s %s" % ("home shape %r" % text, "ok" if ok else "FAIL: -> %r" % got))
            failures += 0 if ok else 1
        for text in ("the Users guide", "home/x", "/homework/x", "/users-guide/x", "/myhome/x",
                     "~/src/a.cpp", "/home/<user>/src"):
            got = stranger(text)
            ok = got == text
            print("read-leg-path selftest: %-28s %s" % ("not a home: %r" % text, "ok" if ok else "FAIL: -> %r" % got))
            failures += 0 if ok else 1
        # THROUGH THE REAL INPUT PATH: a stranger's homes in a file, read by read(), which takes this host's names.
        rc, lines = read(tree, "logs/homes.log", 200, ".")
        body = "\n".join(lines)
        ok = (rc == 0 and "acct" not in body and "C:\\Users\\<user>\\AppData" in body
              and "/mnt/c/Users/<user>/src" in body and "-I/home/<user>/include" in body
              and "//wsl.localhost/Ubuntu/home/<user>/p44_unc_inc" in body and "Z:\\home\\<user>\\test" in body)
        print("read-leg-path selftest: %-28s %s" % ("a stranger's home in a file", "ok" if ok else "FAIL (rc=%d)\n%s"
                                                     % (rc, body)))
        failures += 0 if ok else 1
        # THIS HOST'S OWN NAMES, on whichever leg runs this: its home, and its account in every home shape, never
        # survive. A failure prints only a length: printing what survived would publish the name.
        me = redactor(tree)
        home = os.path.expanduser("~")
        account = account_name(home)
        ok = len(account) >= 1
        print("read-leg-path selftest: %-28s %s" % ("this host's account is named", "ok" if ok else
                                                 "FAIL: the account cannot be named, so no arm below checks it"))
        failures += 0 if ok else 1
        # The ACCOUNT FALLBACK and the REFUSAL, synthesized: getpass made to fail, then the home taken away too.
        real_getuser = getpass.getuser

        def refuse():
            raise OSError("synthetic: no account in the process table")
        getpass.getuser = refuse
        try:
            try:
                got = redactor(tree, host="h.example", home="/home/fbacct")("fbacct@h.example ran")
            except Unredactable:
                got = "REFUSED: the home did not name the account"
            ok = "fbacct" not in got and "<user>" in got
            print("read-leg-path selftest: %-28s %s" % ("no getpass: the home names it", "ok" if ok else
                                                     "FAIL: -> %r" % got))
            failures += 0 if ok else 1
            try:
                redactor(tree, host="h.example", home="")
                ok = False
            except Unredactable:
                ok = True
            print("read-leg-path selftest: %-28s %s" % ("no account at all is refused", "ok" if ok else
                                                     "FAIL: a redactor was built that cannot redact the account"))
            failures += 0 if ok else 1
            real_account_name = globals()["account_name"]
            globals()["account_name"] = lambda _home: ""
            try:
                rc, lines = read(tree, "logs/build.log", 200, ".")
            finally:
                globals()["account_name"] = real_account_name
            ok = rc == 2 and len(lines) == 1 and "account cannot be named" in lines[0]
            print("read-leg-path selftest: %-28s %s" % ("read() refuses it, prints none", "ok" if ok else
                                                     "FAIL (rc=%d, %d line(s))" % (rc, len(lines))))
            failures += 0 if ok else 1
        finally:
            getpass.getuser = real_getuser
        texts = [home + "/x", home.replace("/", "\\") + "\\x"]
        if len(account) >= 3:
            texts += ["/home/%s/x" % account, "C:\\Users\\%s\\x" % account, "/Users/%s/x" % account,
                      "/mnt/c/Users/%s/x" % account, "//wsl.localhost/Ubuntu/home/%s/x" % account,
                      "\\\\wsl$\\Ubuntu\\home\\%s\\x" % account, "Z:\\home\\%s\\x" % account]
        gone = re.compile("(?<![A-Za-z0-9])%s(?![A-Za-z0-9])" % re.escape(account)) if account else None
        for text in texts:
            got = me(text)
            unmarked = got.replace("<user>", "").replace("<host>", "").replace("<tree>", "")  # an account named `user`
            ok = home not in got and not (gone and gone.search(unmarked))
            print("read-leg-path selftest: %-28s %s" % ("this host's home and account", "ok" if ok else
                                                     "FAIL: a real name survived (%d characters kept)" % len(got)))
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
