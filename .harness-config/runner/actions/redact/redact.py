# PURPOSE: filter text so no tree path, home, account, host name or address leaves the machine that printed it -- the one redactor every action loads and every session pipes its output through.
"""redact.py -- the ONE redactor.

WHY ONE (2026-09-30, cycle P69, lane hm). Two programs redacted, and they had already DIVERGED on the same fact
three ways: read-leg-path's home rule let an account hold a blank (`C:\\Users\\Ann Lee`) where the session filter's
stopped at it; the session filter's name boundary counted `_` as part of a word, so `<account>_x` came back
UNMASKED; and read-leg-path matched names case-sensitively on Windows, where accounts and machine names are not.
One rule stated twice is two rules. This file is the only statement of what must never leave a machine; the
readers that print from a leg's host (`read-leg-path`, `probe-reference-cc`) load it by path, and a session pipes
its output through it.

USE
  python redact.py [FILE ...]   filter FILEs, or standard input LINE BY LINE, flushed (a monitor pipes `tail -f`
                                through it; a whole-stdin read would print nothing until the stream ends)
  python redact.py --self-test  the arms, each by name, counted against EXPECTED_ARMS
  loaded by path                `redactor(tree)` -> apply(text); `Unredactable` when the account has no name;
                                `lazy_redactor(tree, places)` -> shown(text), built on first use and WITHHELD when
                                the account has no name, and `cut(shown, text, n)`: what a program prints of a
                                failing arm's detail
⚠ DISPLAY ONLY. Never write a file -- a row cell, a note, a source -- from what this prints: a mask is not the text.
The filter is not a runner step, and cannot be one: a step reads no standard input (✔MEASURED 2026-09-30, run
20260930-164007-b87cbb24 -- `echo <marker> | dssharness run <runner>` handed the step 0 bytes).

THE RULES, in the order they apply
  1. NUL bytes are dropped (a ctest log on Windows carries them).
  2. SECRET PATHS become `<secret-path>`, whole: a path through a `.ssh`, `sshItems`, `.secrets` or `.env`
     component (`.env.local` too), a path whose last component is a `*.key`, `*.pem` or `id_*` file, and anywhere a
     `*.pem` file or an OpenSSH key file's name (`id_ed25519`, `id_rsa_vps.pub`). An option glued to one (`-i/x/.ssh/k`)
     keeps its letter.
  3. PATHS: the tree (given by the caller) becomes `<tree>`, the home directory `~`, and each place the caller names
     its label (`<temp>`, a program's scratch), in every slash spelling -- and case-insensitively where the host's
     file names are (Windows).
  4. A HOME BY ITS SHAPE, whatever account it names: the component after any `home` or `Users` component (any case,
     either slash, doubled backslashes) becomes `<user>` -- `/home/<n>`, `C:\\Users\\<n>`, `/mnt/c/Users/<n>`,
     `\\\\wsl.localhost\\<distro>\\home\\<n>`, Wine's `Z:\\home\\<n>`, `file:///home/<n>`. An account may hold an
     apostrophe inside a word (`O'Brien`). Several blank-separated words are one account only where they are ONE
     path component -- a Windows profile (`Users`) followed by a separator, or a quoted path, the words ending at a
     separator or at its closing quote (`C:\\Users\\Ann Lee\\x`, `"/home/Ann Lee"`) -- and then only within 20
     characters (a Windows account name's limit, before a `.<suffix>`) and with no contraction among them. Anywhere
     else the first word is the account and a prose line after a home keeps its words (the P69 reviews: the rule
     swallowed the rest of a line, and then every word up to a later slash, `don't` included).
  5. A STRANGER'S HOME by the tilde form, `~<n>/...` or `~<n>` at a token's start, becomes `~<user>`; `~/` stays,
     and an 8.3 short name (`PROGRA~1`) is never one (a letter or digit precedes its tilde).
  6. `<user>@<host>` for ANY account, case-insensitive, becomes `<user>@<host>` -- except an assembler symbol's
     suffix or a bare VERSION (`tcl-tk@8`, `python@3.12`, `actions/checkout@v4`), and only where either is the WHOLE
     token after `@` (`puts@plt`, `x@GOTPCREL(%rip)`, `_g@PAGEOFF`): a host that merely begins like one
     (`gotham.example`, `page7.example`, `8.example`) is masked.
  7. `<name>.local` (an mDNS name) as a whole token becomes `<host>.local`, in any case -- never a file name that
     goes on (`settings.local.json`), and never `hosts.local`, DssHarness's own configuration path for this machine.
  8. AN SSH-FAMILY COMMAND -- `ssh`, `scp`, `sftp`, `rsync`, `mosh`, as a whole token, a path to one, `.exe` too:
     the values of `-i` (`<key-path>`), `-l` (`<user>`; scp's and sftp's `-l` is a bandwidth), `-F` (`<path>`),
     `-J`/`-W` (`<host>`), `-L`/`-R` (`<forward>`) and `-o` -- every `-o` setting but a harmless one's (BatchMode,
     ConnectTimeout, ...): `User=<user>`, `HostName=`/`ProxyJump=<host>`, `IdentityFile=`/`CertificateFile=
     <key-path>`, any other `<redacted>` -- attached (`-lacct`, `-oUser=x`) or separate; and the DESTINATION: for
     ssh, sftp and mosh the first operand (`[user@]host`, an `ssh://` URL), for scp and rsync every `[user@]host:path`
     and `host::module` operand and `scp://`/`rsync://` URL. A quoted value is read as a command of its own (rsync's
     `-e "ssh -i k"`, `-e'ssh -l x'` glued to its option), and so is a shell variable's (`GIT_SSH_COMMAND="ssh -l
     x"`, `RSYNC_RSH='ssh -i k'`). A destination that is a declared ssh ITEM's name (`ssh arm64-vps: ...`, as
     DssHarness names a host) is kept: config.json declares it, and it holds no connection data; so is an English
     function word (`over ssh and ...`: prose). `ssh:` (a message's prefix) is not a command.
  9. OpenSSH's own messages: the host in `connect to host X`, `Could not resolve hostname X`, `Connection to X
     closed`, `Connection closed|reset by [authenticating|invalid user U] X port`, `Permanently added 'X'`,
     `authenticity of host 'X'`, `Host key for X has changed`, `Authenticated to X`, `Connected to X`, `Connecting to
     X`, becomes `<host>` (and U `<user>`) -- an English function word there is prose (`Connecting to the database`).
     An IPv6 address there is masked FIRST, as rule 10 masks it (`connect to host <ip> port 22`): two of these
     forms stop at a colon, and cut at its first group an address printed every group after it (2026-10-07).
 10. An IPv6 address -- one Python's `ipaddress` accepts, holding a digit, so `a::b` and `std::vector` stay -- with
     its zone, or with the dotted quad it may end in (`::ffff:203.0.113.5`, taken whole), and an IPv4 address,
     joined by `_` too (`ip_10.1.2.3`), become `<ip>`; an IPv4 never where it is a C standard section (`C 6.7.2.5`,
     `C23 6.7.2.5`, `§6.7.2.5`, `section 6.7.2.5`, `6.7.2.5p3`): a row cell once stored `C <ip>'s` copied from a
     display.
 11. An `ls -l` line's owner and group become `<user>` and `<group>`, whoever they are.
 12. A UNC SERVER, `\\\\<server>\\...` or `//<server>/...` at a token's start, becomes `<host>` -- except WSL's own
     (`wsl.localhost`, `wsl$`) and `localhost`; a URL's `scheme://` authority is not one.
 13. HOST NAMES -- THIS machine's (its full name, its first label, COMPUTERNAME) and the DECLARED remote hosts' --
     in a HOST POSITION, in ANY case on EVERY host, because a DNS name is case-insensitive (RFC 4343): after `@`,
     before `.<label>` (the name AND every label after it -- the one position the exemptions below reach: a host
     named like the project's prefix met its own `dss.mir` there, the P69 re-review), after `//` or `\\\\` (a UNC
     path or a URL's authority), before `:<port>` / `:/` / `:~`; and anywhere on a line that runs an ssh-family
     command.
 14. ACCOUNT names as bare WORDS, never kept: THIS machine's (getpass, the home's last component,
     USERNAME/USER/LOGNAME), in the host's own case rule (insensitive on Windows, sensitive elsewhere: an account
     `ubuntu` must not mask `Ubuntu 24.04`), and the DECLARED remote accounts in their own case (a POSIX account).
     A word is wherever the name is not part of a longer run of ASCII letters or digits, so `_`, `-`, `.`, `@`, a
     slash, `:` and blanks all end one (`13.3.0-6ubuntu2` and `dsscp` keep the words they are part of).
 15. HOST names as bare WORDS -- this machine's in its case rule (a host `dss` must not mask every `DSS_`
     identifier on a POSIX host), the declared ones in any case -- with two exemptions:
     (::) a name followed by `::` is a C++ scope (`dss::Tree`), not a host (the P69 review);
     ★ (K) only where the caller named a tree: a host whose short name is also a word of the project's own
     vocabulary (the arm64 VPS is named like this project's prefix, and read back from it
     `bin/<name>/<name>_examples_runner` and `<name>-config` hid which compiler and configuration ran) is KEPT where
     the tree itself says so -- (K1) as a whole PATH COMPONENT whose neighbouring component pair exists in the tree
     (`bin/<name>/...` when `bin/<name>` is a directory there), (K2) inside a compound token (joined by `-`, `_`
     or `.`) that is itself a file or directory name in the tree, or (K3) inside a DOTTED compound the tree's own
     TEXT spells, exactly (`dss.mir`, `DSS.DevOps` in its sources and documents) -- never a known full host name or a
     name under one's domain, and never from the redactor's own fixtures, a build's or a run's output, or a secret
     path (TreeText). Everything else stays masked: a bare word, every host position above but the dotted one, a
     compound the tree does not hold (`<name>-mac`). Without a tree nothing is kept by (K): over-masking stays the
     failure direction. The tree's names and its text are each walked once per redactor, only when an occurrence asks
     (a host name at all; a dotted compound around one), bounded by VOCAB_MAX_ENTRIES and VOCAB_MAX_SECONDS, and by
     TEXT_MAX_SECONDS -- a walk cut short keeps FEWER, never more.
THE DECLARED REMOTE NAMES are the connection data DssHarness keeps per ssh host in
`.harness-config/sshItems/<item>/.env` (`dssharness help secrets`): its USER, and its ADDRESS with the address's
first label (an IP address is not keyed: rule 10 masks every one). They are read from the caller's tree, from this
file's own tree, and from the main checkout of either when it is a git worktree -- NEVER printed: no arm, refusal or
detail names one, and the live arm reports only counts. An `.env` that exists but cannot be read is a refusal
(`Unredactable`, naming the item): what it holds could not be masked.
WHAT THIS DOES NOT COVER -- it masks NAMES, PATHS and ADDRESSES by their shape and by the names it knows:
  * a secret printed bare (a password, a token, a key's CONTENT) has no shape and is not masked: never print one;
  * a stranger's host name in prose, outside every position above, is not masked;
  * a stranger's account of several blank-separated words is masked whole only where rule 4 bounds them;
  * a WSL distribution's own account, on the machine that hosts it: the WSL legs run as an account no ssh item
    declares and this machine's names do not include, so a bare one in relayed WSL output (`whoami`, `id`, `ps`, an
    owner outside an `ls -l` line) is not masked -- its home is, by its shape (rule 4);
  * a remote host's MACHINE name where its declared ADDRESS is an IP (the arm64 VPS): no name is declared to key, so
    a bare one is not masked -- except by the redactor running ON that host, where it is this machine's own;
  * a bare `name.key` with no directory is not masked (it reads as an attribute in code);
  * a name shorter than three characters is not keyed: it would shred ordinary text;
  * a name its printer CUT before this filter saw it: a prefix of an account or host word is no name it knows (a
    home path cut short stays masked by its shape, rule 4). So text is redacted WHOLE and only then cut -- `cut`
    below, which the self-tests of `anchors`, `anchor-rows` and the speedtest1 benchmark print their details through
    -- and a session's pipeline puts this filter before any `cut` or `head -c`.
"""
import getpass
import io
import ipaddress
import os
import re
import socket
import subprocess
import sys
import tempfile
import threading
import time

sys.dont_write_bytecode = True  # loaded by path by other actions: never a __pycache__ in an action's directory

# What this program prints is another program's output, so any character can reach the pipe: both streams are UTF-8
# from IMPORT on, the repository's rule for every action.
for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError, OSError):
        pass

MIN_NAME = 3
VOCAB_MAX_ENTRIES = 600000
VOCAB_MAX_SECONDS = 30.0
# Rule 15's (K3): where the tree's own TEXT is read, and what bounds the read.
TEXT_ROOTS = ("src", "tests", "examples", "integrated_tests", "cmake", "docs", ".claude", ".plans", ".github",
              ".harness-config/runner/actions")
TEXT_SKIP = frozenset((".git", "build", "artifacts", "runs", "__pycache__", "node_modules", "sshItems", "wslDistros",
                       ".secrets"))
# The redactor's own directory: its self-test SPELLS host-shaped names on purpose, to mask them.
TEXT_SKIP_PATHS = (".harness-config/runner/actions/redact",)
TEXT_MAX_FILE_BYTES = 4 * 1024 * 1024
TEXT_MAX_SECONDS = 10.0
NAME_CHARS = "A-Za-z0-9"
TOKEN_CHARS = "A-Za-z0-9._-"

# Rule 2 -- secret paths. A path run stops at a blank, a quote, `=` and the shell's punctuation, so
# `IdentityFile=<path>` keeps its key.
_PATH_RUN = re.compile(r"[^\s\"'<>|,;=()\[\]{}*?]+")
_SECRET_COMPONENT = re.compile(r"(?i)(?:^|[\\/:])(?:\.ssh|sshitems|\.secrets|\.env(?:\.[A-Za-z0-9_-]+)*)(?=[\\/]|$)")
# an `id_*` file as a path's last component is a key's name when it has no extension but `.pub` (`id_table.cpp` is
# not one)
_KEY_FILE_LAST = re.compile(r"(?i)[\\/](?:[^\\/]*\.(?:key|pem)|id_[A-Za-z0-9_-]+(?:\.pub)?)$")
_KEY_FILE_BARE = re.compile(r"(?i)^(?:[^\\/]+\.pem|id_(?:rsa|dsa|ecdsa|ed25519|xmss)(?:[-_]sk)?(?:[._-][A-Za-z0-9._-]*)?)$")
_GLUED_OPTION = re.compile(r"^(-[A-Za-z])(?=.)")

# Rule 4 -- a home by its shape. Group 1 (a separator, `home` or `Users` in any case -- group 2 -- and the
# separator(s) after it) is kept; group 3, the account's words, become `<user>` as `_home_shape` decides. The account
# takes every character Windows allows in a user name (so every POSIX name too) and an apostrophe only INSIDE a word;
# it never takes `<`, so `<user>` is never taken for a name a second time.
_SEP = r"(?:\\|/)"
_ACCOUNT_CHAR = r"[^\\/\s\"'<>|:;,=+*?\[\]]"
_ACCOUNT_WORD = _ACCOUNT_CHAR + r"+(?:'" + _ACCOUNT_CHAR + r"+)*"
HOME_SHAPE = re.compile(r"(?i)(" + _SEP + r"(home|users)" + _SEP + r"+)(" + _ACCOUNT_WORD + r"(?: " + _ACCOUNT_WORD
                        + r")*)")
# A Windows account name is at most 20 characters (its profile directory may add `.<domain>` or `.<serial>`); an
# English contraction is a prose word, never part of a name.
ACCOUNT_MAX = 20
_CONTRACTION = re.compile(r"(?i)(?:n't|'s|'re|'ll|'ve|'d|'m)$")
# Rule 5 -- a stranger's home by the tilde form.
TILDE_HOME = re.compile(r"(?<![A-Za-z0-9_.~-])~([A-Za-z_][A-Za-z0-9._-]*)")
# Rule 6 -- user@host for any account; an assembler symbol's suffix is not a host, but only as the WHOLE token after
# `@` (ELF x86-64 and AArch64, Mach-O, COFF relocation operators).
_ASM_SUFFIXES = ("plt", "plt32", "got", "gotpcrel", "gotpcrelx", "rex_gotpcrelx", "gotoff", "gotplt", "gottpoff",
                 "gotntpoff", "gotpage", "gotpageoff", "got_prel", "gotrel", "tpoff", "ntpoff", "dtpoff",
                 "indntpoff", "dtprel", "tprel", "tlsgd", "tlsld", "tlsldm", "tlsdesc", "tlscall", "tlvp",
                 "tlvppage", "tlvppageoff", "page", "pageoff", "secrel", "secrel32", "secrel7", "imgrel", "imagebase",
                 "abs", "abs8", "size", "pcrel")
# A host part that is a bare VERSION (`tcl-tk@8`, `python@3.12`, a workflow's `actions/checkout@v4`) is no host
# either: a package's or an action's pinned release.
USER_AT_HOST = re.compile(r"(?i)\b[a-z_][a-z0-9_.-]*@(?!(?:%s|v?\d+(?:\.\d+){0,2})(?![a-z0-9._-]))"
                          r"(?:\[[0-9a-f:.%%a-z]+\]|[a-z0-9][a-z0-9.-]*\b)"
                          % "|".join(sorted(_ASM_SUFFIXES, key=len, reverse=True)))
# Rule 7 -- an mDNS name: a whole token ending `.local` -- never a FILE name that goes on (`settings.local.json`), and
# never `hosts.local`, DssHarness's own configuration path for this machine's host section (`dssharness help
# admission`), which the tree's documents name.
DOT_LOCAL = re.compile(r"(?i)(?<![A-Za-z0-9._-])(?!hosts\.local\b)[A-Za-z0-9][A-Za-z0-9._-]*\.local\b"
                       r"(?!\.[A-Za-z0-9])")
# Rule 8 -- an ssh-family command.
_SSH_FIND = re.compile(r"(?i)(?<![A-Za-z0-9_-])(?:ssh|scp|sftp|rsync|mosh)(?![A-Za-z0-9_-])")
_SSH_PROGRAM = re.compile(r"(?i)^(?:.*[\\/])?(ssh|scp|sftp|rsync|mosh)(?:\.exe)?$")
# The option letters that take a value, per program (ssh(1), scp(1), sftp(1), mosh(1), rsync(1)); rsync's -i -l -o
# -F -J are flags, and its remote shell comes through -e or --rsh.
_SSH_VALUE_LETTERS = {"ssh": set("BbcDEeFIiJLlmOoPpQRSWw"), "scp": set("cDFiJloPSX"), "sftp": set("BbcDFiJloPRSX"),
                      "mosh": set("p"), "rsync": set("eBTfM")}
_SSH_MASKED_LETTERS = {
    "ssh": {"i": "<key-path>", "l": "<user>", "F": "<path>", "J": "<host>", "W": "<host>", "L": "<forward>",
            "R": "<forward>"},
    "scp": {"i": "<key-path>", "F": "<path>", "J": "<host>"},
    "sftp": {"i": "<key-path>", "F": "<path>", "J": "<host>"},
    "mosh": {}, "rsync": {}}
_SSH_O_PROGRAMS = ("ssh", "scp", "sftp")
_SSH_O_HARMLESS = frozenset(k.lower() for k in (
    "AddressFamily", "BatchMode", "CheckHostIP", "Ciphers", "ClearAllForwardings", "Compression",
    "ConnectionAttempts", "ConnectTimeout", "ControlMaster", "ControlPersist", "EscapeChar", "ExitOnForwardFailure",
    "ForwardAgent", "ForwardX11", "GSSAPIAuthentication", "HashKnownHosts", "HostbasedAuthentication",
    "HostKeyAlgorithms", "IdentitiesOnly", "IPQoS", "KbdInteractiveAuthentication", "KexAlgorithms", "LogLevel",
    "MACs", "NumberOfPasswordPrompts", "PasswordAuthentication", "Port", "PreferredAuthentications",
    "PubkeyAcceptedAlgorithms", "PubkeyAcceptedKeyTypes", "PubkeyAuthentication", "RequestTTY",
    "ServerAliveCountMax", "ServerAliveInterval", "StrictHostKeyChecking", "TCPKeepAlive", "UpdateHostKeys",
    "VisualHostKey"))
_SSH_O_MASK = {"user": "<user>", "hostname": "<host>", "proxyjump": "<host>", "identityfile": "<key-path>",
               "certificatefile": "<key-path>"}
_SSH_URL = re.compile(r"(?i)^(?:ssh|sftp|scp|rsync)://")
_SSH_AUTHORITY = re.compile(r"^(?:([^@\s/\\<>]+)@)?(\[[^\]]+\]|[A-Za-z0-9][A-Za-z0-9._-]*)(.*)$")
_SHELL_OPERATORS = ("&&", "||", ";", "|", "&")
# A shell variable ASSIGNED a command (`GIT_SSH_COMMAND=...`, `RSYNC_RSH=...`): its value is read as one.
_ASSIGNMENT = re.compile(r"^([A-Za-z_][A-Za-z0-9_]*=)(.*)$", re.S)
# English function words: never a host, so after `ssh` in prose (`over ssh and ...`) and in an OpenSSH message's host
# position (`Connecting to the database`) one is a word, not a destination (the P69 re-review's NIT 3).
_PROSE_WORDS = frozenset((
    "a", "an", "and", "are", "as", "at", "be", "been", "but", "by", "for", "from", "if", "in", "into", "is", "it",
    "its", "nor", "not", "of", "on", "or", "over", "so", "than", "that", "the", "their", "then", "there", "these",
    "this", "those", "through", "to", "under", "until", "via", "was", "were", "when", "where", "which", "while",
    "with", "without", "yet"))
# Rule 9 -- OpenSSH's own messages; group `host` (and `user`) is masked.
SSH_MESSAGES = tuple(re.compile(p) for p in (
    r"(?i)\bconnect to (?:host|address) (?P<host>\[[^\]]+\]|[^\s:]+)",
    r"(?i)\bCould not resolve hostname (?P<host>[^\s:]+)",
    r"(?i)\bConnection to (?P<host>\S+?)(?= closed\b)",
    r"(?i)\bConnection (?:closed|reset|timed out) by (?:(?:authenticating|invalid) user (?P<user>\S+) )?"
    r"(?P<host>\S+)(?= port\b)",
    r"(?i)\bPermanently added '(?P<host>[^']+)'",
    r"(?i)\bauthenticity of host '(?P<host>[^']+)'",
    r"(?i)\bHost key for (?P<host>\S+) has changed",
    r"(?i)\bAuthenticated to (?P<host>[^\s(]+)",
    r"(?i)\bConnected to (?P<host>[^\s(]+?)(?=\.?(?:\s|$))",
    r"(?i)\bConnecting to (?P<host>[^\s(\[]+?)(?=\.{0,3}(?:\s|$))"))
# Rule 10 -- addresses. An IPv6 candidate is validated, and must hold a digit; an IPv4 address is one unless it is a
# C standard section, and `_` joins it to nothing. An IPv6 address may END in a dotted quad (RFC 4291 2.2:
# `::ffff:203.0.113.5`, `64:ff9b::192.0.2.33`), taken whole -- the P69 re-review: cut at its first dot, the hex part
# was masked and three octets printed -- and a candidate is never one a dotted quad goes on from.
IPV6_CANDIDATE = re.compile(r"(?<![0-9A-Za-z_:.])(?:[0-9A-Fa-f]{0,4}(?::[0-9A-Fa-f]{0,4}){1,6}:(?:\d{1,3}\.){3}\d{1,3}"
                            r"|[0-9A-Fa-f]{0,4}(?::[0-9A-Fa-f]{0,4}){2,7})(?:%[0-9A-Za-z_.-]+)?"
                            r"(?![0-9A-Za-z_:]|\.\d)")
IPV4 = re.compile(r"(?<!\bC )(?<!\bC\d\d )(?<!§)(?<!§ )(?<![Ss]ection )(?<![0-9A-Za-z])(?<!\d\.)"
                  r"(?:\d{1,3}\.){3}\d{1,3}(?![0-9A-Za-z]|\.\d)")
# Rule 11 -- an `ls -l` line: mode, links, OWNER, GROUP, then a size.
LS_LONG = re.compile(r"(?m)^(\s*[-bcdlpsD][-rwxsStTlL]{9}[.+@*]?\s+\d+\s+)([^\s<]\S*)(\s+)([^\s<]\S*)(?=\s+\d)")
# Rule 12 -- a UNC server at a token's start (never a URL's authority, never a doubled slash inside a path).
UNC_SERVER = re.compile(r"(?<![0-9A-Za-z_:\\/.$])(\\\\|//)(?!(?i:wsl\.localhost|wsl\$|localhost)(?=[\\/]))"
                        r"([0-9A-Za-z][0-9A-Za-z._$-]*)(?=[\\/])")


class Unredactable(Exception):
    """This host's account cannot be named, or a declared host's connection data cannot be read, so what would be
    printed cannot be redacted: a refusal, never a silent pass."""


def this_os_family():
    return "windows" if os.name == "nt" else "posix"


def account_names(home):
    """This process's account names: `getpass` (LOGNAME, USER, LNAME, USERNAME, then the password database), the
    home's last component -- an account the process table cannot name still owns a home -- and the environment's
    USERNAME, USER and LOGNAME. Empty only when every one is silent."""
    names = set()
    try:
        names.add(getpass.getuser())
    except Exception:  # noqa: BLE001 - getpass raises OSError, KeyError or ImportError by platform
        pass
    if home:
        names.add(os.path.basename(home.rstrip("/\\")))
    for var in ("USERNAME", "USER", "LOGNAME"):
        names.add(os.environ.get(var, ""))
    return {n for n in names if n}


def host_names():
    """This machine's names: its network name, that name's first label, and COMPUTERNAME / HOSTNAME."""
    names = set()
    try:
        full = socket.gethostname()
    except OSError:
        full = ""
    names.update((full, full.split(".")[0]))
    for var in ("COMPUTERNAME", "HOSTNAME"):
        value = os.environ.get(var, "")
        names.update((value, value.split(".")[0]))
    return {n for n in names if n}


# ──────────────────────────────── the declared remote names ────────────────────────────────

def own_tree():
    """The tree this file lives in (`<tree>/.harness-config/runner/actions/redact/redact.py`), or None."""
    config = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.realpath(__file__)))))
    return os.path.dirname(config) if os.path.basename(config) == ".harness-config" else None


def main_checkout_of(tree):
    """The main checkout a git WORKTREE belongs to -- its `.git` FILE names `<main>/.git/worktrees/<name>` -- or
    None (a main checkout, a plain copy, anything unreadable)."""
    dotgit = os.path.join(tree, ".git")
    if not os.path.isfile(dotgit):
        return None
    try:
        with io.open(dotgit, encoding="utf-8", errors="replace") as fh:
            first = fh.readline().strip()
    except OSError:
        return None
    if not first.lower().startswith("gitdir:"):
        return None
    gitdir = first[len("gitdir:"):].strip().replace("\\", "/")
    at = gitdir.lower().find("/.git/worktrees/")
    return gitdir[:at] if at > 0 else None


def declared_trees(tree=None):
    """The trees whose ssh items name the declared remote hosts: the caller's, this file's own, and the main checkout
    of either -- each once."""
    seen, out = set(), []
    for t in (tree, own_tree()):
        for u in ((t, main_checkout_of(t)) if t else ()):
            if u and os.path.isdir(u):
                key = os.path.normcase(os.path.realpath(u))
                if key not in seen:
                    seen.add(key)
                    out.append(u)
    return out


def _read_env(path):
    values = {}
    with io.open(path, encoding="utf-8-sig") as fh:  # a byte-order mark must not hide the first key
        for line in fh:
            line = line.strip()
            if line.startswith("export "):
                line = line[len("export "):].lstrip()
            if not line or line.startswith("#"):
                continue
            key, eq, value = line.partition("=")
            if eq:
                values[key.strip()] = value.strip().strip("\"'")
    return values


def declared_names(trees):
    """-> {"accounts", "hosts", "items"}: every ssh item's USER, its ADDRESS and that address's first label (an IP
    address is not keyed: rule 10 masks every address) under each tree's `.harness-config/sshItems/<item>/.env`, and
    the item names (config.json declares those: they are not secret). NOTHING here prints a value. An `.env` that
    exists but cannot be read raises `Unredactable` naming its item."""
    accounts, hosts, items = set(), set(), set()
    for t in trees:
        base = os.path.join(t, ".harness-config", "sshItems")
        try:
            entries = sorted(os.listdir(base))
        except OSError:
            continue
        for name in entries:
            if not os.path.isdir(os.path.join(base, name)):
                continue
            items.add(name)
            env = os.path.join(base, name, ".env")
            if not os.path.lexists(env):
                continue
            try:
                values = _read_env(env)
            except (OSError, UnicodeError) as exc:
                raise Unredactable("the connection data DssHarness keeps for the ssh host '%s' cannot be read (%s), "
                                   "so the names it holds could not be masked" % (name, type(exc).__name__))
            if values.get("USER"):
                accounts.add(values["USER"])
            address = values.get("ADDRESS", "").strip("[]")
            if address:
                try:
                    ipaddress.ip_address(address.split("%", 1)[0])
                except ValueError:
                    hosts.update((address, address.split(".")[0]))
    return {"accounts": accounts, "hosts": hosts, "items": items}


# ──────────────────────────────── the tree's vocabulary (rule 15's exemption K) ────────────────────────────────

class TreeVocabulary:
    """The file and directory NAMES under a tree and every (parent, child) name PAIR, `.git` left out -- what the
    tree itself says (rule 15's exemption). Walked once; bounded, so a walk cut short keeps fewer names, never
    more. `fold` compares names as the host's file system does (case-insensitively on Windows)."""

    def __init__(self, tree, os_family, max_entries=VOCAB_MAX_ENTRIES, max_seconds=VOCAB_MAX_SECONDS):
        self.fold = (lambda s: s.casefold()) if os_family == "windows" else (lambda s: s)
        self.names, self.pairs = set(), set()
        self.entries, self.complete = 0, True
        start = time.monotonic()
        for dirpath, dirs, files in os.walk(tree):
            dirs[:] = [d for d in dirs if d != ".git"]
            parent = self.fold(os.path.basename(dirpath.rstrip("/\\")))
            for name in dirs + files:
                folded = self.fold(name)
                self.names.add(folded)
                self.pairs.add((parent, folded))
                self.entries += 1
            if self.entries >= max_entries or time.monotonic() - start >= max_seconds:
                self.complete = False
                break
        self.seconds = time.monotonic() - start

    def has_name(self, name):
        return self.fold(name) in self.names

    def has_pair(self, parent, child):
        return (self.fold(parent), self.fold(child)) in self.pairs


class TreeText:
    """The DOTTED compounds the tree's own TEXT spells around a word of `names` (a host's names): every maximal run
    of `A-Za-z0-9._-` holding a `.` and one of them as a whole word, exactly as spelled -- rule 15's (K3). Read from
    the files at the tree's top and under TEXT_ROOTS -- its sources, tests, examples, plans, skills, documents and
    actions -- never a build's, a run's, a fetch's or a secret's directory (TEXT_SKIP), never the redactor's own
    (TEXT_SKIP_PATHS: its fixtures spell host names to mask them), never a secret path (rule 2's), a file over
    TEXT_MAX_FILE_BYTES or one holding a NUL. Walked once; bounded by TEXT_MAX_SECONDS, so a walk cut short keeps
    FEWER compounds, never more."""

    def __init__(self, tree, names, max_seconds=None):
        max_seconds = TEXT_MAX_SECONDS if max_seconds is None else max_seconds
        self.compounds, self.files, self.complete = set(), 0, True
        words = sorted({n for n in names if n}, key=len, reverse=True)
        start = time.monotonic()
        if not words:
            self.seconds = 0.0
            return
        word = re.compile(r"(?i)(?<![A-Za-z0-9])(?:%s)(?![A-Za-z0-9])" % "|".join(re.escape(w) for w in words))
        run = re.compile(r"[%s]" % TOKEN_CHARS)
        skip = {os.path.normcase(os.path.join(tree, *p.split("/"))) for p in TEXT_SKIP_PATHS}
        paths = [os.path.join(tree, f) for f in sorted(os.listdir(tree))
                 if f not in TEXT_SKIP and os.path.isfile(os.path.join(tree, f))]
        for root in TEXT_ROOTS:
            top = os.path.join(tree, *root.split("/"))
            for dirpath, dirs, files in os.walk(top):
                dirs[:] = sorted(d for d in dirs if d not in TEXT_SKIP
                                 and os.path.normcase(os.path.join(dirpath, d)) not in skip)
                paths += [os.path.join(dirpath, f) for f in sorted(files)]
        paths = [p for p in paths if not (_SECRET_COMPONENT.search(p) or _KEY_FILE_LAST.search(p))]
        for path in paths:
            if time.monotonic() - start >= max_seconds:
                self.complete = False
                break
            try:
                if os.path.getsize(path) > TEXT_MAX_FILE_BYTES:
                    continue
                with io.open(path, "rb") as fh:
                    data = fh.read()
            except OSError:
                continue
            if b"\x00" in data:
                continue
            self.files += 1
            body = data.decode("utf-8", "replace")
            for m in word.finditer(body):
                left, right = m.start(), m.end()
                while left > 0 and run.match(body[left - 1]):
                    left -= 1
                while right < len(body) and run.match(body[right]):
                    right += 1
                token = body[left:right].rstrip(".")
                if "." in token:
                    self.compounds.add(token)
        self.seconds = time.monotonic() - start

    def has(self, token):
        return token in self.compounds


def _word(value, flags=0):
    return re.compile("(?<![%s])%s(?![%s])" % (NAME_CHARS, re.escape(value), NAME_CHARS), flags)


def _token_around(text, start, end):
    """The maximal compound token (`A-Za-z0-9._-`) around text[start:end], its trailing dots left out ->
    (left, right, token)."""
    left = start
    while left > 0 and re.match("[%s]" % TOKEN_CHARS, text[left - 1]):
        left -= 1
    right = end
    while right < len(text) and re.match("[%s]" % TOKEN_CHARS, text[right]):
        right += 1
    return left, right, text[left:right].rstrip(".")


def _kept_by_vocabulary(text, start, end, vocab, spelled=None, known=()):
    """Rule 15's exemption K for ONE occurrence of a host name at text[start:end]: (K2) the compound token around it
    is a name in the tree, (K3) it is a DOTTED compound the tree's own text spells (`spelled`, a TreeText factory)
    and no known full host name or name under one of their domains (`known`, lower case), or (K1) it is a whole path
    component whose neighbouring pair exists in the tree."""
    left, right, token = _token_around(text, start, end)
    if (left, right) != (start, end) and token != text[start:end]:
        if vocab.has_name(token):
            return True                                                   # (K2) a compound the tree names
        low = token.lower()
        if "." not in token or spelled is None or any(low == k or low.endswith("." + k) for k in known):
            return False
        return spelled().has(token)                                       # (K3) a dotted compound the tree spells
    seps = ("/", "\\")  # a TUPLE: `"" in "/\\"` is True, and a name at the text's edge is not in a path
    before = text[left - 1] if left > 0 else ""
    after = text[right] if right < len(text) else ""
    if before not in seps and after not in seps:
        return False                                                      # a bare word: never kept
    if left >= 2 and text[left - 2] in seps and before in seps:
        return False                                                      # `//name`, `\\name`: a UNC host
    name = text[start:end]
    if before in seps:
        p_end = left - 1
        p_start = p_end
        while p_start > 0 and re.match("[%s]" % TOKEN_CHARS, text[p_start - 1]):
            p_start -= 1
        if p_start < p_end and vocab.has_pair(text[p_start:p_end], name):
            return True                                                   # (K1) `<parent>/<name>` exists
    if after in seps:
        n_start = right + 1
        n_end = n_start
        while n_end < len(text) and re.match("[%s]" % TOKEN_CHARS, text[n_end]):
            n_end += 1
        if n_start < n_end and vocab.has_pair(name, text[n_start:n_end].rstrip(".")):
            return True                                                   # (K1) `<name>/<child>` exists
    return False


# ──────────────────────────────── the rules' helpers ────────────────────────────────

def _mask_secret_paths(text):
    """Rule 2."""
    def one(m):
        tok = m.group(0)
        core = tok.rstrip(".:")
        tail = tok[len(core):]
        glued = _GLUED_OPTION.match(core)
        head, path = (glued.group(1), core[2:]) if glued else ("", core)  # `-i/x/.ssh/k` keeps its `-i`
        if path and (_SECRET_COMPONENT.search(path) or _KEY_FILE_LAST.search(path) or _KEY_FILE_BARE.match(path)):
            return head + "<secret-path>" + tail
        return tok
    return _PATH_RUN.sub(one, text)


def _home_shape(m):
    """Rule 4, one home. ONE word is the account, unless several are -- which a POSIX name never is (it holds no
    blank) -- so several are taken only where they are bounded as ONE path component: a Windows profile (`Users`)
    followed by a separator, or a QUOTED path, where the words end at a separator or at the closing quote. Even then
    only within ACCOUNT_MAX characters (before a `.<suffix>`) and with no contraction among them. Otherwise the first
    word alone is the account and the rest of the line keeps its words (the P69 re-review: `cd /home/<n> and don't
    touch src/x` lost every word up to `x`)."""
    root, kind, run = m.group(1), m.group(2).lower(), m.group(3)
    words = run.split(" ")
    if len(words) == 1:
        return root + "<user>"
    s = m.string
    after = s[m.end():m.end() + 1]
    i = m.start()
    while i > 0 and not s[i - 1].isspace():
        i -= 1
    opening = next((c for c in s[i:m.start()] if c in "\"'"), None)
    seps = ("\\", "/")
    bounded = (opening is not None and (after == opening or after in seps)) or (kind == "users" and after in seps)
    if bounded and len(run.split(".", 1)[0]) <= ACCOUNT_MAX and not any(_CONTRACTION.search(w) for w in words):
        return root + "<user>"
    return root + "<user>" + run[len(words[0]):]


def _tokens(s):
    """(start, end) of each blank-separated token of `s`; a quoted run (`"..."`, `'...'`) stays inside its token. A
    quote opens a run only at a token's start, after `=`, or after a glued short option (`-e'ssh -l x'`) -- an
    apostrophe inside a word (`can't`) is a letter."""
    out, i, n = [], 0, len(s)
    while i < n:
        while i < n and s[i].isspace():
            i += 1
        if i >= n:
            break
        start, quote = i, None
        while i < n and (quote or not s[i].isspace()):
            c = s[i]
            if quote:
                if c == quote:
                    quote = None
            elif c in "\"'" and (i == start or s[i - 1] == "="
                                 or (i == start + 2 and s[start] == "-" and s[start + 1].isalpha())):
                quote = c
            i += 1
        out.append((start, i))
    return out


def _unquote(tok):
    """-> (lead, core, trail): a token's opening quote and its closing quote and punctuation, apart from its core."""
    m = re.match(r"^([\"']?)(.*?)([\"']?[,;)]*)$", tok, re.S)
    return m.group(1), m.group(2), m.group(3)


def _is_mask(value):
    return bool(re.match(r"<[a-z-]+>", value))


def _mask_o(value):
    key, eq, rest = value.partition("=")
    if not eq:
        key, _sp, rest = value.partition(" ")
        eq = " " if _sp else ""
    if key.strip().lower() in _SSH_O_HARMLESS:
        return value
    if not eq:
        return "<redacted>"
    return key + eq + _SSH_O_MASK.get(key.strip().lower(), "<redacted>")


def _mask_option_value(program, letter, value):
    if _is_mask(value):
        return value
    if letter == "o" and program in _SSH_O_PROGRAMS:
        return _mask_o(value)
    return _SSH_MASKED_LETTERS[program].get(letter, value)


def _mask_destination(core, items, operand_with_colon):
    if not core or core.startswith("<"):
        return core
    if (core[:-1] if core.endswith(":") else core) in items:
        return core                                          # a declared item's name: DssHarness's label for it
    url = _SSH_URL.match(core)
    if url:                                                  # `ssh://[user@]host[:port]`, `rsync://host/module`
        m = _SSH_AUTHORITY.match(core[url.end():])
        if not m:
            return core                                      # its authority is already a mask, or none
        user, host, rest = m.groups()
        return url.group(0) + ("<user>@" if user else "") + "<host>" + rest
    m = _SSH_AUTHORITY.match(core)
    if not m:
        return core
    user, host, rest = m.groups()
    if rest.startswith("://"):
        return core                                          # another scheme's URL: not a destination
    if operand_with_colon:
        if not rest.startswith(":"):
            return core                                      # a local path
        if len(host) == 1 and rest[1:2] in ("\\", "/"):
            return core                                      # a drive letter, `C:\x`
        if not user and rest == ":":
            return core                                      # `rsync error:` and its kind: a message, not a host
    elif rest and not rest.startswith((":", "/")):
        return core
    return ("<user>@" if user else "") + "<host>" + rest


def _scan_ssh(line, toks, i, program, items, out):
    """Mask the options and destinations of ONE ssh-family command whose operands start at token `i`; -> the index
    of the first token it did not read."""
    letters = _SSH_VALUE_LETTERS[program]
    pending, options_done = None, False
    while i < len(toks):
        s, e = toks[i]
        tok = line[s:e]
        lead, core, trail = _unquote(tok)
        if core in _SHELL_OPERATORS:
            return i + 1
        if pending is not None:
            new = _mask_option_value(program, pending, core)
            if new == core and lead and _SSH_FIND.search(core):
                new = _mask_ssh_commands(core, items)       # a quoted command: rsync's `-e "ssh -i k"`
            if new != core:
                out.append((s, e, lead + new + trail))
            pending = None
            i += 1
            continue
        if not options_done and core == "--":
            options_done = True
            i += 1
            continue
        if not options_done and core.startswith("--") and len(core) > 2:
            name, eq, value = core[2:].partition("=")
            if eq and _SSH_FIND.search(value):
                new = _mask_ssh_commands(value, items)      # `--rsh="ssh -i k"`, `--ssh=...`
                if new != value:
                    out.append((s, e, lead + "--" + name + "=" + new + trail))
            i += 1
            continue
        if not options_done and core.startswith("-") and len(core) > 1 and not core.startswith("<"):
            for j in range(1, len(core)):
                if core[j] in letters:
                    rest = core[j + 1:]
                    if rest:
                        q_lead, q_core, q_trail = _unquote(rest)
                        new = _mask_option_value(program, core[j], q_core)
                        if new == q_core and q_lead and _SSH_FIND.search(q_core):
                            new = _mask_ssh_commands(q_core, items)   # glued and quoted: rsync's `-e'ssh -l x'`
                        if new != q_core:
                            out.append((s, e, lead + core[:j + 1] + q_lead + new + q_trail + trail))
                    else:
                        pending = core[j]
                    break
            i += 1
            continue
        if program in ("ssh", "sftp", "mosh") and core.lower() in _PROSE_WORDS:
            return i + 1                                    # `over ssh and ...`: prose, not a destination
        if program in ("ssh", "sftp", "mosh"):
            new = _mask_destination(core, items, operand_with_colon=False)
            if new != core:
                out.append((s, e, lead + new + trail))
            return i + 1                                    # what follows is the remote command
        new = _mask_destination(core, items, operand_with_colon=True)
        if new != core:
            out.append((s, e, lead + new + trail))
        i += 1
    return i


def _mask_ssh_commands(line, items):
    """Rule 8, over one line."""
    if not _SSH_FIND.search(line):
        return line
    toks = _tokens(line)
    out = []
    i = 0
    while i < len(toks):
        s, e = toks[i]
        lead, core, trail = _unquote(line[s:e])
        if lead and _SSH_FIND.search(core) and not _SSH_PROGRAM.match(core):
            new = _mask_ssh_commands(core, items)           # a quoted command of its own
            if new != core:
                out.append((s, e, lead + new + trail))
            i += 1
            continue
        assign = _ASSIGNMENT.match(core)
        if assign and _SSH_FIND.search(assign.group(2)):
            v_lead, v_core, v_trail = _unquote(assign.group(2))
            new = _mask_ssh_commands(v_core, items)         # `GIT_SSH_COMMAND="ssh -l x"`, `RSYNC_RSH='ssh -i k'`
            if new != v_core:
                out.append((s, e, lead + assign.group(1) + v_lead + new + v_trail + trail))
            i += 1
            continue
        m = _SSH_PROGRAM.match(core)
        if m and not trail.startswith(":"):
            i = _scan_ssh(line, toks, i + 1, m.group(1).lower(), items, out)
            continue
        i += 1
    for s, e, new in sorted(out, reverse=True):
        line = line[:s] + new + line[e:]
    return line


def _mask_ssh_messages(text, items):
    """Rule 9. A declared item's name (DssHarness's label for a host) is kept, as in rule 8, and so is a prose word
    (`Connecting to the database`)."""
    for rx in SSH_MESSAGES:
        def one(m, rx=rx):
            whole, base = m.group(0), m.start()
            spans = [(m.start(g), m.end(g), "<%s>" % g) for g in ("user", "host")
                     if g in rx.groupindex and m.group(g) is not None and not m.group(g).startswith("<")
                     and m.group(g) not in items and m.group(g).rstrip(".").lower() not in _PROSE_WORDS]
            for s, e, mask in sorted(spans, reverse=True):
                whole = whole[:s - base] + mask + whole[e - base:]
            return whole
        text = rx.sub(one, text)
    return text


def _mask_ipv6(text):
    """Rule 10's IPv6 half."""
    def one(m):
        cand = m.group(0)
        addr = cand.split("%", 1)[0]
        if not any(ch.isdigit() for ch in addr):
            return cand
        try:
            ipaddress.IPv6Address(addr)
        except ValueError:
            return cand
        return "<ip>"
    return IPV6_CANDIDATE.sub(one, text)


def redactor(tree=None, user=None, host=None, home=None, os_family=None, vocabulary=None, declared=None,
             places=None, spelled=None):
    """-> apply(text). `tree` (optional) becomes `<tree>` and is the only source of rule 15's exemption K; `user`,
    `host` and `home` default to this process's names (a test passes its own, so what it asserts never depends on
    the host it runs on); `os_family` ("windows" or "posix") defaults to this host's; `vocabulary` replaces the
    tree walk and `spelled` the tree-TEXT walk (a test's); `declared` ({"accounts", "hosts", "items"}) replaces the
    declared remote names read from the trees' ssh items (a test's); `places` ({label: directory}) marks more
    directories like the tree -- `<temp>` for the system temp directory, a program's own scratch -- in every slash
    spelling and their real path's, the longest first (rule 3). Raises `Unredactable` when no account can be named, or
    a declared host's connection data cannot be read."""
    os_family = os_family or this_os_family()
    if home is None:
        home = os.path.expanduser("~")
    accounts = {user} if user else account_names(home)
    accounts = {a for a in accounts if len(a) >= MIN_NAME}
    if not accounts:
        raise Unredactable("this host's account cannot be named (no getpass answer, no home directory, no USERNAME "
                           "or USER), so what would be printed cannot be redacted")
    if declared is None:
        declared = declared_names(declared_trees(tree))
    remote_accounts = {a for a in declared.get("accounts", ()) if len(a) >= MIN_NAME}
    remote_hosts = {h for h in declared.get("hosts", ()) if len(h) >= MIN_NAME}
    items = set(declared.get("items", ()))
    hosts = {host, host.split(".")[0]} if host else host_names()
    hosts = {h for h in hosts if len(h) >= MIN_NAME}
    every_host = sorted(hosts | remote_hosts, key=len, reverse=True)       # the full name before its label
    word_flags = re.IGNORECASE if os_family == "windows" else 0
    path_pairs = []
    marks = [(tree, "<tree>", False), (home, "~", False)]
    marks += [(path, label, True) for label, path in sorted((places or {}).items())]
    for value, mark, real in marks:
        if value and len(value) >= MIN_NAME:
            forms = {value, os.path.realpath(value)} if real else {value}
            for form in forms:
                for spelling in {form, form.replace("\\", "/"), form.replace("/", "\\")}:
                    path_pairs.append((re.compile(re.escape(spelling), word_flags), len(spelling), mark))
    path_pairs.sort(key=lambda p: p[1], reverse=True)  # the tree, or a place, before the home it sits in
    # Rule 13's positions, per host: after `@`, the DOTTED name (the name and every label after it), after `//`, and
    # before `:<port>`. The dotted one alone takes the exemptions (it is the one a tree's vocabulary spells:
    # `dss.mir`); `known` is every known FULL name and its domain, which (K3) never keeps.
    host_positions = []
    for h in every_host:
        e = re.escape(h)
        host_positions += [(re.compile(r"(?i)(?<=@)%s(?![A-Za-z0-9-])" % e), 0),
                           (re.compile(r"(?i)(?<![A-Za-z0-9-])%s(?:\.[A-Za-z0-9-]+)+" % e), len(h)),
                           (re.compile(r"(?i)(?<=[\\/]{2})%s(?![A-Za-z0-9-])" % e), 0),
                           (re.compile(r"(?i)(?<![A-Za-z0-9-])%s(?=:[0-9/\\~])" % e), 0)]
    known = set()
    for h in every_host:
        if "." in h:
            known.update((h.lower(), h.lower().split(".", 1)[1]))
    host_anywhere = [re.compile(r"(?i)(?<![A-Za-z0-9])%s(?![A-Za-z0-9])" % re.escape(h)) for h in every_host]
    host_words = ([_word(h, word_flags) for h in sorted(hosts, key=len, reverse=True)]
                  + [_word(h, re.IGNORECASE) for h in sorted(remote_hosts - hosts, key=len, reverse=True)])
    account_words = ([_word(a, word_flags) for a in sorted(accounts, key=len, reverse=True)]
                     + [_word(a) for a in sorted(remote_accounts - accounts, key=len, reverse=True)])
    state = {"vocab": vocabulary, "text": spelled}

    def vocab():
        if state["vocab"] is None and tree and os.path.isdir(tree):
            state["vocab"] = TreeVocabulary(tree, os_family)
        return state["vocab"]

    def text_vocab():                                                      # asked only where a tree was named
        if state["text"] is None:
            state["text"] = TreeText(tree, every_host)
        return state["text"]

    def keep_host(t, start, end):
        if t.startswith("::", end) and not t.startswith(":::", end):
            return True                                                    # (::) a C++ scope, not a host
        v = vocab() if tree else None
        return v is not None and _kept_by_vocabulary(t, start, end, v, text_vocab, known)   # (K)

    def apply(text):
        text = text.replace("\x00", "")                                    # rule 1
        text = _mask_secret_paths(text)                                    # rule 2
        for rx, _n, mark in path_pairs:                                    # rule 3
            text = rx.sub(lambda _m, mark=mark: mark, text)
        text = HOME_SHAPE.sub(_home_shape, text)                           # rule 4
        text = TILDE_HOME.sub("~<user>", text)                             # rule 5
        text = USER_AT_HOST.sub("<user>@<host>", text)                     # rule 6
        text = DOT_LOCAL.sub("<host>.local", text)                         # rule 7
        if _SSH_FIND.search(text):                                         # rule 8
            text = "\n".join(_mask_ssh_commands(ln, items) for ln in text.split("\n"))
        # rule 10's IPv6 half runs FIRST: rule 9's `connect to host X` and `Could not resolve hostname X` stop at
        # a colon, so an IPv6 address there was cut at its first group, masked `<host>`, the rest printed.
        text = _mask_ipv6(text)                                            # rule 10, IPv6
        text = _mask_ssh_messages(text, items)                             # rule 9
        text = IPV4.sub("<ip>", text)
        text = LS_LONG.sub(lambda m: m.group(1) + "<user>" + m.group(3) + "<group>", text)   # rule 11
        text = UNC_SERVER.sub(lambda m: m.group(1) + "<host>", text)      # rule 12
        for rx, name_len in host_positions:                                # rule 13
            if not name_len:
                text = rx.sub("<host>", text)
            elif rx.search(text):                                          # the dotted name: the exemptions
                text = rx.sub(lambda m, t=text, n=name_len: m.group(0) if keep_host(t, m.start(), m.start() + n)
                              else "<host>", text)
        if host_anywhere and _SSH_FIND.search(text):
            text = "\n".join(_mask_ssh_line(ln, host_anywhere) for ln in text.split("\n"))
        for rx in account_words:                                           # rule 14: never kept
            text = rx.sub("<user>", text)
        for rx in host_words:                                              # rule 15: the exemptions
            if not rx.search(text):
                continue
            text = rx.sub(lambda m, t=text: m.group(0) if keep_host(t, m.start(), m.end()) else "<host>", text)
        return text
    # The tree walks this redactor made for rule 15 -- its names, and its text -- or None for one it did not make, so
    # a caller can say what the exemption consulted and what it cost (read-leg-path prints both on its OK line).
    apply.vocabulary = lambda: state["vocab"]
    apply.text_vocabulary = lambda: state["text"]
    return apply


WITHHELD = "<detail withheld: this host's account cannot be named>"


def lazy_redactor(tree=None, places=None, **names):
    """-> shown(text): THE redactor (`tree`, `places` and any of `redactor`'s other arguments, a test's `names`),
    built on FIRST use, over the WHOLE text -- or WITHHELD for every text when this host's account cannot be named.
    The one implementation the programs that print a failing arm's detail load (anchors, anchor-rows, the speedtest1
    benchmark, the pragma census): until 2026-10-06 each held a copy, three of them with a `<temp>` rule this file
    lacked (the P69 re-review). `shown.redactor` holds what was built, so a test may put a stand-in in its place."""
    built = []

    def shown(text):
        if not built:
            try:
                built.append(redactor(tree=tree, places=places, **names))
            except Unredactable:
                built.append(lambda _t: WITHHELD)
        return built[0]("" if text is None else str(text))
    shown.redactor = built
    return shown


def cut(shown, text, n):
    """`shown(text)` -- the WHOLE text redacted -- then its first n characters (n > 0) or its last -n (n < 0). Never
    the other order: a cut taken first can split a name the rules would have masked whole, leaving a piece of it no
    rule knows (the P69 review's MINOR 5)."""
    whole = shown(text)
    return whole[:n] if n > 0 else whole[n:]


def _mask_ssh_line(line, host_anywhere):
    if not _SSH_FIND.search(line):
        return line
    for rx in host_anywhere:
        line = rx.sub("<host>", line)
    return line


# ──────────────────────────────── the self-test ────────────────────────────────

# ★ AN EXACT RATCHET: the self-test counts the arms it ran and fails on any other number, so an arm deleted -- or a
# block that silently stopped running -- is a red, not a smaller green. The count is host-independent: every arm
# below is synthetic except "this host's and the declared names never survive", which is ONE arm however many texts
# it checks.
EXPECTED_ARMS = 242


def self_test():
    ran, failed = [0], [0]

    def arm(label, ok, detail=""):
        ran[0] += 1
        if not ok:
            failed[0] += 1
        print("redact self-test: %-58s %s" % (label[:58], "ok" if ok else "FAIL" + ((": " + detail) if detail else "")))

    def same(label, red, text, want):
        got = red(text)
        arm(label, got == want, "%r -> %r, want %r" % (text, got, want))

    none = {"accounts": (), "hosts": (), "items": ()}   # synthetic arms never depend on this host's ssh items
    posix = redactor(user="lnxacct", host="h9.example", home="/home/lnxacct", os_family="posix", declared=none)
    win = redactor(user="WinAcct", host="WINBOX", home="C:\\Users\\WinAcct", os_family="windows", declared=none)
    # ── rule 1, NUL; rule 3, paths ──
    same("NUL bytes are dropped", posix, "a\x00b", "ab")
    tree_red = redactor(tree="/srv/trees/wt1", user="lnxacct", host="h9.example", home="/home/lnxacct",
                        os_family="posix", declared=none)
    same("the tree path is <tree>", tree_red, "at /srv/trees/wt1/src/a.cpp", "at <tree>/src/a.cpp")
    same("the home path is ~", posix, "cd /home/lnxacct/src", "cd ~/src")
    same("a Windows home, any slash, any case, is ~", win, "c:/users/winacct/x and C:\\Users\\WinAcct\\y",
         "~/x and ~\\y")
    # ── rule 2, secret paths (the P69 review's MAJOR 1) ──
    for text, want in (
            ("key at /srv/k/.ssh/id_ed25519 ok", "key at <secret-path> ok"),
            ("read .harness-config/sshItems/vps7/.env now", "read <secret-path> now"),
            ("C:\\keys\\vps7.pem", "<secret-path>"),
            ("cert7.pem.", "<secret-path>."),
            ("/opt/keys/deploy7.key", "<secret-path>"),
            ("logs/id_custom7", "<secret-path>"),
            ("found id_ed25519_vps7.pub here", "found <secret-path> here"),
            ("tok in .secrets/token7.txt", "tok in <secret-path>"),
            ("cfg .env.local", "cfg <secret-path>"),
            ("IdentityFile=/srv/k/.ssh/k7", "IdentityFile=<secret-path>"),
            ("-i/srv/k/.ssh/k7", "-i<secret-path>")):
        same("secret path %r" % text, posix, text, want)
    for text in ("self.key = 1", "the id_counter field", "x.environment", "my.envy/x"):
        same("not a secret path: %r" % text, posix, text, text)
    # ── rule 4, a home by its shape, for accounts this redactor was never told about ──
    for text, want in (
            ("C:\\Users\\otheracct\\AppData\\Local\\Temp\\x", "C:\\Users\\<user>\\AppData\\Local\\Temp\\x"),
            ("path C:\\\\Users\\\\otheracct\\\\src", "path C:\\\\Users\\\\<user>\\\\src"),
            ("c:/users/otheracct/x", "c:/users/<user>/x"),
            ("D:\\Users\\Ann Lee\\AppData", "D:\\Users\\<user>\\AppData"),
            ("/mnt/c/Users/otheracct/AppData", "/mnt/c/Users/<user>/AppData"),
            ("/mnt/d/users/Ann Lee/x", "/mnt/d/users/<user>/x"),
            ("cd /c/Users/otheracct/src", "cd /c/Users/<user>/src"),
            ("/Users/macacct/Library/x", "/Users/<user>/Library/x"),
            ("cd /home/otheracct/src", "cd /home/<user>/src"),
            ("HOME=/home/ab", "HOME=/home/<user>"),
            ("gcc -I/home/otheracct/include", "gcc -I/home/<user>/include"),
            ("'/home/otheracct'", "'/home/<user>'"),
            ("\"/home/Ann Lee\"", "\"/home/<user>\""),
            ("//wsl.localhost/Ubuntu/home/otheracct/p44", "//wsl.localhost/Ubuntu/home/<user>/p44"),
            ("\\\\wsl.localhost\\Ubuntu\\home\\otheracct\\x", "\\\\wsl.localhost\\Ubuntu\\home\\<user>\\x"),
            ("\\\\wsl$\\Ubuntu\\home\\otheracct", "\\\\wsl$\\Ubuntu\\home\\<user>"),
            ("Z:\\home\\otheracct\\test", "Z:\\home\\<user>\\test"),
            ("Z:/home/otheracct/src", "Z:/home/<user>/src"),
            ("/cygdrive/c/Users/otheracct/x", "/cygdrive/c/Users/<user>/x"),
            ("\\\\fileserver\\home\\otheracct\\docs", "\\\\<host>\\home\\<user>\\docs"),
            ("file:///home/otheracct/a.c", "file:///home/<user>/a.c"),
            ("/HOME/Otheracct/x", "/HOME/<user>/x"),
            ("C:\\Users\\O'Brien\\x", "C:\\Users\\<user>\\x"),
            ("C:\\Users\\O'Brien", "C:\\Users\\<user>"),
            ("docs/home/guide.md", "docs/home/<user>"),             # OVER-masked: the permitted direction
            ("files in /home/otheracct and more words here", "files in /home/<user> and more words here"),
            # the P69 re-review's MINOR 3: several words are one account only where they are ONE path component
            ("cd /home/otheracct and don't touch src/x", "cd /home/<user> and don't touch src/x"),
            ("C:\\Users\\otheracct and don't touch src\\x", "C:\\Users\\<user> and don't touch src\\x"),
            ("C:\\Users\\otheracct and many more words\\x", "C:\\Users\\<user> and many more words\\x"),
            ("echo /home/otheracct and fix\"", "echo /home/<user> and fix\""),
            ("C:\\Users\\Ann Lee.CORP\\x", "C:\\Users\\<user>\\x"),
            ("'/home/Ann Lee'", "'/home/<user>'")):
        same("home shape %r" % text, posix, text, want)
    for text in ("the Users guide", "home/x", "/homework/x", "/users-guide/x", "/myhome/x", "~/src/a.cpp",
                 "/home/<user>/src"):
        same("not a home: %r" % text, posix, text, text)
    # ── rule 5, the tilde form (a round-12 audit item) ──
    same("~name/ is a stranger's home", posix, "ls ~otheracct/src", "ls ~<user>/src")
    same("~name alone is a stranger's home", posix, "cd ~otheracct", "cd ~<user>")
    same("~/ is not a stranger's home", posix, "cd ~/src && x=~/y", "cd ~/src && x=~/y")
    same("an 8.3 short name is not a home", posix, "C:\\PROGRA~1\\X and DSS-SC~1", "C:\\PROGRA~1\\X and DSS-SC~1")
    # ── rule 6, user@host for any account (a round-12 audit item), any case; the assembler suffixes, exactly ──
    same("user@host of an unknown account", posix, "ssh otheracct@build7:22", "ssh <user>@<host>:22")
    same("USER@HOST in capitals", posix, "Otheracct@Build7.Example", "<user>@<host>")
    for sym in ("puts@plt", "x@GOTPCREL(%rip)", "_g@PAGEOFF", "v@tlsgd", "f@SECREL32"):
        same("an assembler suffix is kept: %s" % sym, posix, "call %s" % sym, "call %s" % sym)
    for text in ("bob7@gotham.example", "ann7@page7.example", "eve7@absent.example", "joe7@got.example",
                 "kim7@plt-host.example", "pat7@8.example"):
        same("a host beginning like a suffix: %s" % text, posix, text, "<user>@<host>")
    same("user@[ipv6]", posix, "acct7@[2001:db8::7]", "<user>@<host>")
    # the P69 re-review's NIT 2: a bare version after `@` is a pinned release, not a host
    for text in ("brew install tcl-tk@8 llvm@17 python@3.12", "uses: actions/checkout@v4"):
        same("a version is kept: %r" % text, posix, text, text)
    # ── rule 7, .local ──
    same("an mDNS name is masked", posix, "reach buildmac.local now", "reach <host>.local now")
    same("an mDNS name in capitals is masked", posix, "Buildmac.LOCAL", "<host>.local")
    for text in ("the hosts.local section", "edit .claude/settings.local.json"):
        same("not an mDNS name: %r" % text, posix, text, text)
    # ── rule 8, an ssh-family command's options and destination (the P69 review's MAJOR 1) ──
    for text, want in (
            ("ssh -i /opt/k/k1 vps7 uptime", "ssh -i <key-path> <host> uptime"),
            ("ssh -i/opt/k/k1 vps7", "ssh -i<key-path> <host>"),
            ("ssh -l remoteacct vps7", "ssh -l <user> <host>"),
            ("ssh -lremoteacct vps7", "ssh -l<user> <host>"),
            ("ssh -F /opt/cfg/c7 vps7", "ssh -F <path> <host>"),
            ("ssh -J jump7 vps7", "ssh -J <host> <host>"),
            ("ssh -o User=remoteacct -o HostName=vps7.example -o IdentityFile=/opt/k1 -o ProxyJump=jump7 alias7",
             "ssh -o User=<user> -o HostName=<host> -o IdentityFile=<key-path> -o ProxyJump=<host> <host>"),
            ("ssh -oUser=remoteacct alias7", "ssh -oUser=<user> <host>"),
            ("ssh -o \"User remoteacct\" alias7", "ssh -o \"User <user>\" <host>"),
            ("ssh -o ProxyCommand=nc7 alias7", "ssh -o ProxyCommand=<redacted> <host>"),
            ("ssh -o BatchMode=yes -o ConnectTimeout=25 alias7 true",
             "ssh -o BatchMode=yes -o ConnectTimeout=25 <host> true"),
            ("ssh -vtt -p 22 -L 8080:inner7:80 alias7", "ssh -vtt -p 22 -L <forward> <host>"),
            ("ssh -W inner7:22 jump7", "ssh -W <host> <host>"),
            ("ssh ssh://remoteacct@vps7:2222", "ssh ssh://<user>@<host>:2222"),
            ("C:\\Windows\\System32\\OpenSSH\\ssh.exe -l remoteacct alias7 uname",
             "C:\\Windows\\System32\\OpenSSH\\ssh.exe -l <user> <host> uname"),
            ("scp -i /opt/k1 -P 22 -l 800 notes.txt remoteacct@vps7:/srv/x",
             "scp -i <key-path> -P 22 -l 800 notes.txt <user>@<host>:/srv/x"),
            ("scp vps7:/srv/a C:\\local\\b", "scp <host>:/srv/a C:\\local\\b"),
            ("rsync -avz -e \"ssh -i /opt/k1 -l remoteacct\" src/ vps7:dst/",
             "rsync -avz -e \"ssh -i <key-path> -l <user>\" src/ <host>:dst/"),
            ("rsync --rsh=\"ssh -i /opt/k1\" src/ vps7::mod7", "rsync --rsh=\"ssh -i <key-path>\" src/ <host>::mod7"),
            ("rsync -av rsync://vps7/mod7 dst/", "rsync -av rsync://<host>/mod7 dst/"),
            ("sftp -F /opt/cfg/c7 vps7", "sftp -F <path> <host>"),
            ("mosh vps7", "mosh <host>"),
            ("ssh jump7 ssh -l remoteacct inner7 uptime", "ssh <host> ssh -l <user> <host> uptime"),
            ("x && ssh vps7 ls; ssh -i /opt/k1 vps8", "x && ssh <host> ls; ssh -i <key-path> <host>"),
            # the P69 re-review's NIT 1: a variable that holds a command, and a quote glued to its option
            ("GIT_SSH_COMMAND=\"ssh -l remoteacct -i /opt/k1\" git fetch",
             "GIT_SSH_COMMAND=\"ssh -l <user> -i <key-path>\" git fetch"),
            ("export RSYNC_RSH='ssh -i /opt/k1 -l remoteacct'", "export RSYNC_RSH='ssh -i <key-path> -l <user>'"),
            ("rsync -e'ssh -l remoteacct' src/ vps7:dst/", "rsync -e'ssh -l <user>' src/ <host>:dst/")):
        same("ssh command %r" % text, posix, text, want)
    # the P69 re-review's NIT 3: an English function word after `ssh`, or in a message's host position, is prose
    for text in ("copied over ssh and verified", "Connecting to the database", "Connected to the network."):
        same("prose is not a host: %r" % text, posix, text, text)
    items = redactor(user="lnxacct", host="h9.example", home="/home/lnxacct", os_family="posix",
                     declared={"accounts": (), "hosts": (), "items": ("item7",)})
    same("a declared item's name is kept: ssh item7:", items, "ssh item7: copy synced", "ssh item7: copy synced")
    for text in ("ssh: connect to nothing", "rsync error: some files were not transferred", "the ssh_config file",
                 "openssh-client", "~/.ssh is gone"):
        same("not an ssh command: %r" % text, posix, text,
             text if ".ssh" not in text else "<secret-path> is gone")
    # ── rule 9, OpenSSH's own messages ──
    for text, want in (
            ("ssh: connect to host vps7.example.net port 22: Connection timed out",
             "ssh: connect to host <host> port 22: Connection timed out"),
            # an IPv6 address, taken whole: read up to its first colon it printed every group after the first
            ("ssh: connect to host 2001:db8::7 port 22: Connection timed out",
             "ssh: connect to host <ip> port 22: Connection timed out"),
            ("ssh: connect to host fe80::1234:5678:9abc:def0%8 port 22: Connection timed out",
             "ssh: connect to host <ip> port 22: Connection timed out"),
            ("ssh: connect to address 2001:db8:0:0:0:0:0:7 port 22: Connection refused",
             "ssh: connect to address <ip> port 22: Connection refused"),
            ("ssh: Could not resolve hostname vps7: Name or service not known",
             "ssh: Could not resolve hostname <host>: Name or service not known"),
            ("Connection to vps7.example.net closed.", "Connection to <host> closed."),
            ("Connection closed by 10.0.0.9 port 22", "Connection closed by <host> port 22"),
            ("Connection closed by authenticating user remoteacct vps7 port 22 [preauth]",
             "Connection closed by authenticating user <user> <host> port 22 [preauth]"),
            ("Warning: Permanently added 'vps7.example.net' (ED25519) to the list of known hosts.",
             "Warning: Permanently added '<host>' (ED25519) to the list of known hosts."),
            ("The authenticity of host 'vps7 (10.0.0.9)' can't be established.",
             "The authenticity of host '<host>' can't be established."),
            ("Host key for vps7 has changed and you have requested strict checking.",
             "Host key for <host> has changed and you have requested strict checking."),
            ("Authenticated to vps7 ([10.0.0.9]:22) using \"publickey\".",
             "Authenticated to <host> ([<ip>]:22) using \"publickey\"."),
            ("Connected to vps7.", "Connected to <host>."),
            ("Connecting to vps7...", "Connecting to <host>...")):
        same("ssh message %r" % text[:40], posix, text, want)
    # ── rule 10, addresses ──
    same("an IPv4 address is masked", posix, "connect 10.1.22.3:22", "connect <ip>:22")
    same("an IPv4 address joined by _ is masked", posix, "ip_10.1.22.3_x", "ip_<ip>_x")
    for text in ("C 6.7.2.5's rule", "C23 6.7.2.5", "§6.7.2.5", "§ 6.7.2.5", "section 6.7.2.5", "6.7.2.5p3"):
        same("a C section is kept: %r" % text, posix, text, text)
    for text, want in (("addr fe80::1%eth0 up", "addr <ip> up"), ("[2001:db8::7]:22", "[<ip>]:22"),
                       ("2001:0db8:0000:0000:0000:ff00:0042:8329", "<ip>"), ("lo ::1", "lo <ip>"),
                       # the P69 re-review's MINOR 6: an IPv4 tail is part of the address, never three octets
                       ("::ffff:203.0.113.5", "<ip>"), ("nat64 64:ff9b::192.0.2.33 up", "nat64 <ip> up"),
                       ("[::ffff:203.0.113.5]:22", "[<ip>]:22"), ("::ffff:203.0.113.5:22", "::ffff:<ip>:22")):
        same("an IPv6 address is masked: %r" % text, posix, text, want)
    for text in ("std::vector<int>", "dss::Tree", "a::b", "at 12:34:56", "cafe::beef"):
        same("not an IPv6 address: %r" % text, posix, text, text)
    # ── rule 11, an `ls -l` line ──
    same("ls -l: owner and group", posix, "-rw-r--r-- 1 remoteacct staffgrp 4096 Oct  1 12:00 notes.txt",
         "-rw-r--r-- 1 <user> <group> 4096 Oct  1 12:00 notes.txt")
    same("ls -l (macOS @): owner and group", posix, "drwxr-xr-x@ 3 macacct staff 96 Oct  1 12:00 d",
         "drwxr-xr-x@ 3 <user> <group> 96 Oct  1 12:00 d")
    same("not an ls -l line", posix, "-rw is a flag 1 of 2", "-rw is a flag 1 of 2")
    # ── rule 12, a UNC server ──
    for text, want in (("\\\\fileserver7\\share\\x", "\\\\<host>\\share\\x"), ("//fileserver7/share", "//<host>/share"),
                       ("net use \\\\nas7\\pub", "net use \\\\<host>\\pub")):
        same("a UNC server is masked: %r" % text, posix, text, want)
    for text in ("https://www.example.org/download.html", "\\\\wsl.localhost\\Ubuntu\\x", "\\\\wsl$\\Ubuntu\\x",
                 "\\\\?\\C:\\x", "a//b/c", "\\\\localhost\\c$\\x"):
        same("not a UNC server: %r" % text, posix, text, text)
    # ── rules 13-15: this host's names ──
    dss = redactor(user="ubuntu", host="dss.example", home="/home/ubuntu", os_family="posix", declared=none)
    same("a name inside a version is KEPT", dss, "gcc 13.3.0-6ubuntu2", "gcc 13.3.0-6ubuntu2")
    same("a name inside a program name is KEPT", dss, "dsscp.exe --version", "dsscp.exe --version")
    for text in ("ubuntu_x", "ubuntu-mac", "ubuntu.local", "ubuntu@h", "/home/ubuntu/", "C:\\Users\\ubuntu\\",
                 "dss_x", "dss-mac", "dss.local", "x@dss.example:", "ssh dss"):
        got = dss(text)
        arm("no leak in %r" % text, "ubuntu" not in got and "dss" not in got and any(
            m in got for m in ("<user>", "<host>", "~")), "%r -> %r" % (text, got))
    # host POSITIONS are case-insensitive on EVERY host (RFC 4343); bare words follow the host's own case rule
    same("posix: a bare word in another case is kept", dss, "DSS_CONFIG_ROOT is set", "DSS_CONFIG_ROOT is set")
    same("posix: @HOST in another case is masked", dss, "root@DSS:~$ ls", "<user>@<host>:~$ ls")
    same("posix: HOST.domain, every label, is masked", dss, "ping DSS.Example.Org", "ping <host>")
    same("posix: a UNC host in another case is masked", dss, "\\\\DSS\\share", "\\\\<host>\\share")
    same("posix: a URL host in another case is masked", dss, "http://Dss:8080/", "http://<host>:8080/")
    same("posix: an ssh line masks the host in any case", dss, "ssh -p 22 Dss uptime", "ssh -p 22 <host> uptime")
    same("posix: host:port in another case is masked", dss, "DSS:22 refused", "<host>:22 refused")
    same("a name followed by :: is a C++ scope", dss, "dss::Tree and dss::detail::x", "dss::Tree and dss::detail::x")
    same("...but not on an rsync line", dss, "rsync src/ dss::mod7", "rsync src/ <host>::mod7")
    same("every label after this host's own", dss, "dss.corp.example.com", "<host>")
    same("windows: a bare host word in another case is masked", win, "on winbox today", "on <host> today")
    same("windows: a bare account word in another case is masked", win, "owner WINACCT", "owner <user>")
    same("posix: a bare account word in another case is kept", posix, "LNXACCT is not the account",
         "LNXACCT is not the account")
    # ── the DECLARED remote names, from a synthetic tree's ssh items (the P69 review's MAJOR 1) ──
    with tempfile.TemporaryDirectory() as tmp:
        main = os.path.realpath(os.path.join(tmp, "main"))
        os.makedirs(os.path.join(main, ".harness-config", "sshItems", "item7"))
        os.makedirs(os.path.join(main, ".harness-config", "sshItems", "item8"))
        with io.open(os.path.join(main, ".harness-config", "sshItems", "item7", ".env"), "w", encoding="utf-8") as f:
            f.write("ADDRESS=vps7.example.net\nUSER=remoteacct\nPORT=22\n")
        with io.open(os.path.join(main, ".harness-config", "sshItems", "item8", ".env"), "w", encoding="utf-8") as f:
            f.write("\ufeffexport ADDRESS='10.0.0.8'\nUSER=\"macacct\"\n")
        wt = os.path.join(tmp, "wt")
        os.makedirs(os.path.join(wt, ".harness-config"))
        with io.open(os.path.join(wt, ".git"), "w", encoding="utf-8") as f:
            f.write("gitdir: %s/.git/worktrees/wt\n" % main.replace("\\", "/"))
        # `declared_trees` also names THIS file's own tree and its main checkout, whose items are REAL: this arm
        # checks only that the worktree's main checkout is among the trees, and prints only a count; the names are
        # then read from the two synthetic trees alone, so a failure's detail can hold nothing but synthetic names.
        keys = [os.path.normcase(os.path.realpath(t)) for t in declared_trees(wt)]
        arm("a worktree's main checkout is a declared tree", os.path.normcase(main) in keys
            and os.path.normcase(os.path.realpath(wt)) in keys, "%d tree(s)" % len(keys))
        names = declared_names([wt, main])
        arm("the items' USER, ADDRESS and first label are read", sorted(names["accounts"]) == ["macacct", "remoteacct"]
            and sorted(names["hosts"]) == ["vps7", "vps7.example.net"] and names["items"] == {"item7", "item8"},
            "%r" % {k: sorted(v) for k, v in names.items()})
        decl = redactor(tree=wt, user="lnxacct", host="h9.example", home="/home/lnxacct", os_family="posix",
                        declared=names)
        for text, want in (("remoteacct logged in", "<user> logged in"), ("macacct owns it", "<user> owns it"),
                           ("vps7 is up", "<host> is up"), ("VPS7.EXAMPLE.NET answered", "<host> answered"),
                           ("Remoteacct is another word", "Remoteacct is another word"),
                           ("ssh item7: copy synced", "ssh item7: copy synced")):
            same("declared: %r" % text, decl, text, want)
        os.remove(os.path.join(main, ".harness-config", "sshItems", "item8", ".env"))
        os.makedirs(os.path.join(main, ".harness-config", "sshItems", "item8", ".env"))   # exists, cannot be read
        try:
            declared_names([wt, main])
            arm("an unreadable .env is refused, naming its item", False, "the names were read")
        except Unredactable as exc:
            arm("an unreadable .env is refused, naming its item", "'item8'" in str(exc) and "10.0.0.8" not in str(exc),
                str(exc))
    # ── rule 15's exemption K, pinned with a SYNTHETIC host over a synthetic tree (the round-13 short-name row) ──
    with tempfile.TemporaryDirectory() as tmp:
        tree = os.path.realpath(tmp)
        os.makedirs(os.path.join(tree, "build", "v1", "bin", "syn"))
        os.makedirs(os.path.join(tree, "src", "syn-config"))
        io.open(os.path.join(tree, "build", "v1", "bin", "syn", "syn_examples_runner"), "w").close()
        syn = redactor(tree=tree, user="ubuntu", host="syn.example", home="/home/ubuntu", os_family="posix",
                       declared=none)
        for text in ("bin/syn/syn_examples_runner", "loading src/syn-config/x.json", "running syn_examples_runner",
                     "<tree>/build/v1/bin/syn/"):
            same("KEPT, the tree names it: %r" % text, syn, text, text)
        for text, want in (("ubuntu@syn:~$ ls", "<user>@<host>:~$ ls"), ("Linux syn 6.8.0", "Linux <host> 6.8.0"),
                           ("ssh syn", "ssh <host>"), ("syn.local", "<host>.local"), ("syn-mac", "<host>-mac"),
                           ("x@syn.example:", "<user>@<host>:"), ("//syn/share", "//<host>/share"),
                           ("http://syn:8080/", "http://<host>:8080/"), ("the syn prefix", "the <host> prefix"),
                           ("lib/syn/x", "lib/<host>/x"), ("syn/nothing-here", "<host>/nothing-here")):
            same("MASKED, a host position or no tree fact: %r" % text, syn, text, want)
        no_tree = redactor(user="ubuntu", host="syn.example", home="/home/ubuntu", os_family="posix", declared=none)
        same("without a tree nothing is kept", no_tree, "bin/syn/syn_examples_runner",
             "bin/<host>/<host>_examples_runner")
        same("the account is never kept, tree or not",
             redactor(tree=tree, user="syn", host="h9.example", home="/home/syn", os_family="posix", declared=none),
             "bin/syn/syn_examples_runner", "bin/<user>/<user>_examples_runner")
        bounded = TreeVocabulary(tree, "posix", max_entries=1)
        arm("a walk cut short keeps fewer names, never more", not bounded.complete and bounded.entries <= 3
            and not bounded.has_name("syn_examples_runner"), "entries=%d" % bounded.entries)
        # ── (K3), the dotted compounds the tree's own TEXT spells (the P69 re-review's MINOR 4: on a host named like
        #    the project's prefix, the dotted host position masked its `dss.mir`) ──
        def put(rel, body):
            path = os.path.join(tree, *rel.split("/"))
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with io.open(path, "w", encoding="utf-8") as fh:
                fh.write(body)
        put("src/notes.md", "the syn.mir dialect, SYN.DevOps, syn.macho_arm64_exit, dss.mir, syn.example, "
                            "syn.lab.example, mail.syn.example\n")
        put("build/v1/log.txt", "syn.secret ran\n")                                   # a build's output
        put(".harness-config/runner/actions/redact/fixture.txt", "syn.fixture\n")     # the redactor's own
        put("src/.env", "syn.envtoken\n")                                             # a secret path
        fresh = redactor(tree=tree, user="ubuntu", host="syn.example", home="/home/ubuntu", os_family="posix",
                         declared=none)
        before = fresh("Linux syn 6.8.0")
        walked_early = fresh.text_vocabulary()
        after = fresh("the syn.mir dialect")
        text_vocab = fresh.text_vocabulary()
        arm("the tree's TEXT is walked only when a dotted compound asks", before == "Linux <host> 6.8.0"
            and walked_early is None and after == "the syn.mir dialect" and text_vocab is not None
            and text_vocab.files >= 1, "%r %r %r" % (before, after, walked_early))
        for text in ("SYN.DevOps", "ran syn.macho_arm64_exit"):
            same("KEPT, the tree's text spells it: %r" % text, fresh, text, text)
        for text, want in (("syn.corp is up", "<host> is up"), ("syn.devops", "<host>"),
                           ("syn.secret ran", "<host> ran"), ("syn.fixture", "<host>"), ("syn.envtoken", "<host>")):
            same("MASKED, a compound the tree's text does not give: %r" % text, fresh, text, want)
        # (K3) never keeps a KNOWN name, though the tree's text spells all three: the host's full name (a whole word,
        # which rule 15 masks before K is asked), a name under its domain, and one ending in the full name -- the last
        # two are K3's own refusal, which the first cannot witness.
        for text, want in (("ping syn.example now", "ping <host> now"), ("ping syn.lab.example now", "ping <host> now"),
                           ("ping mail.syn.example now", "ping mail.<host> now")):
            same("MASKED, a known name the tree's text spells: %r" % text, fresh, text, want)
        short = TreeText(tree, {"syn"}, max_seconds=0)
        arm("a text walk cut short keeps fewer compounds, never more", not short.complete and not short.has("syn.mir"),
            "complete=%r" % short.complete)
        dss_host = redactor(tree=tree, user="ubuntu", host="dss", home="/home/ubuntu", os_family="posix",
                            declared=none)
        same("on a host named `dss`, the tree's dss.mir is KEPT", dss_host, "emits dss.mir", "emits dss.mir")
        same("...and a dss.<label> the tree does not spell is masked", dss_host, "dss.corp is up", "<host> is up")
    # ── the refusal: an account nobody can name ──
    real_getuser, saved_env = getpass.getuser, {v: os.environ.get(v) for v in ("USERNAME", "USER", "LOGNAME")}

    def refuse():
        raise OSError("synthetic: no account in the process table")
    getpass.getuser = refuse
    for v in saved_env:
        os.environ.pop(v, None)
    try:
        # a BARE word: only the home's last component can name the account to rule 14 (the P69 review's NIT 2 --
        # a `<user>@<host>` text would be masked by rule 6 whoever the account is)
        got = redactor(host="h9.example", home="/home/fbacct", os_family="posix", declared=none)("fbacct ran today")
        arm("no getpass: the home names the account", got == "<user> ran today", repr(got))
        try:
            redactor(host="h9.example", home="", os_family="posix", declared=none)
            arm("no account at all is refused", False, "a redactor was built that cannot redact the account")
        except Unredactable:
            arm("no account at all is refused", True)
        withheld = lazy_redactor(host="h9.example", home="", os_family="posix", declared=none)("10.1.2.3 ran")
        arm("a detail is WITHHELD whole when no account can be named", withheld == WITHHELD, repr(withheld))
    finally:
        getpass.getuser = real_getuser
        for v, value in saved_env.items():
            if value is not None:
                os.environ[v] = value
    # ── a failing arm's detail, as a program prints it: the places, then the WHOLE text, then the cut (the P69
    #    re-review's NIT 10: three programs each held a copy of this, with a `<temp>` rule this file lacked) ──
    placed = redactor(user="lnxacct", host="h9.example", home="/home/lnxacct", os_family="posix", declared=none,
                      places={"<temp>": "/srv/tmp7", "<probe>": "/home/lnxacct/scratch7"})
    same("a place is marked like the tree, in every slash spelling", placed, "at /srv/tmp7/a and \\srv\\tmp7\\b",
         "at <temp>/a and <temp>\\b")
    same("...the longest first, so a place inside the home keeps its own mark", placed,
         "ran /home/lnxacct/scratch7/x from /home/lnxacct", "ran <probe>/x from ~")
    shown = lazy_redactor()
    shown.redactor[:] = [lambda t: t.replace("zqxacct", "<user>")]   # a stand-in: this proves the ORDER, not the rules
    cuts = (cut(shown, "x" * 20 + " zqxacct ran", 24), cut(shown, "ran by zqxacct", -5))
    arm("a detail is redacted WHOLE, then cut: no piece of a name survives either end",
        not any(piece in "|".join(cuts) for piece in ("zqx", "qxa", "xac", "acc", "cct")) and cut(shown, None, 3) == "",
        repr(cuts))
    # ── this host's own names and the DECLARED ones, on whichever leg runs this: ONE arm, and a failure prints only
    #    counts and lengths -- never a name ──
    me = redactor()
    home = os.path.expanduser("~")
    real = declared_names(declared_trees(None))
    names = sorted({a for a in account_names(home) if len(a) >= MIN_NAME}
                   | {h for h in host_names() if len(h) >= MIN_NAME}
                   | {n for n in real["accounts"] | real["hosts"] if len(n) >= MIN_NAME}, key=len, reverse=True)
    texts = [home + "/x", home.replace("/", "\\") + "\\x"]
    for n in names:
        texts += ["/home/%s/x" % n, "C:\\Users\\%s\\x" % n, "%s@h.example" % n, "x@%s:" % n, "ssh %s" % n,
                  "on %s today" % n]
    survivors = []
    for text in texts:
        got = me(text).replace("<user>", "").replace("<host>", "")
        if home in got or any(_word(n, re.IGNORECASE if this_os_family() == "windows" else 0).search(got)
                              for n in names):
            survivors.append(len(got))
    arm("names never survive (%d, %d declared, %d texts)"
        % (len(names), len(real["accounts"]) + len(real["hosts"]), len(texts)),
        bool(names) and not survivors, "a real name survived in %d text(s) (lengths %r)" % (len(survivors), survivors))
    # ── the filter, through the REAL input path: a child fed a pipe, one line answered before the stream ends ──
    child = subprocess.Popen([sys.executable, "-B", os.path.realpath(__file__)], stdin=subprocess.PIPE,
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    child.stdin.write(b"connect 10.9.8.7 now\x00\n")
    child.stdin.flush()
    first = {}
    reader = threading.Thread(target=lambda: first.setdefault("line", child.stdout.readline()))
    reader.start()
    reader.join(20)
    streamed = first.get("line", b"").decode("utf-8", "replace")
    child.stdin.write(b"second x@y.example\n")
    child.stdin.close()
    rest = child.stdout.read().decode("utf-8", "replace")
    child.wait(30)
    arm("the filter answers each line before the stream ends", streamed.rstrip("\r\n") == "connect <ip> now",
        repr(streamed))
    arm("the filter redacts every line and exits 0", rest.strip() == "second <user>@<host>" and child.returncode == 0,
        "%r rc=%r" % (rest, child.returncode))
    with tempfile.TemporaryDirectory() as tmp:
        f = os.path.join(tmp, "log.txt")
        with io.open(f, "w", encoding="utf-8") as fh:
            fh.write("ip 172.16.0.1\n")
        out = subprocess.run([sys.executable, "-B", os.path.realpath(__file__), f], stdout=subprocess.PIPE,
                             stderr=subprocess.PIPE, timeout=60)
        arm("the filter reads a file argument", out.stdout.decode("utf-8", "replace").strip() == "ip <ip>"
            and out.returncode == 0, repr(out.stdout))
    total = ran[0]
    ok = failed[0] == 0 and total == EXPECTED_ARMS
    if total != EXPECTED_ARMS:
        print("redact self-test: ARM COUNT %d, expected %d -- an arm was added or lost; EXPECTED_ARMS is the "
              "ratchet" % (total, EXPECTED_ARMS))
    print("redact self-test: %d arm(s), %d failed%s" % (total, failed[0], "" if ok else " -- FAIL"))
    return 0 if ok else 1


def main(argv):
    if argv == ["--self-test"]:
        return self_test()
    if any(a.startswith("-") for a in argv):
        print("redact: USAGE -- redact.py [FILE ...] (standard input when none) | --self-test; unknown option %r"
              % next(a for a in argv if a.startswith("-")), file=sys.stderr)
        return 2
    try:
        red = redactor()
    except Unredactable as exc:
        print("redact: REFUSED -- %s" % exc, file=sys.stderr)
        return 2
    if argv:
        for path in argv:
            with io.open(path, "rb") as fh:
                sys.stdout.write(red(fh.read().decode("utf-8", errors="replace")))
        return 0
    sys.stdin.reconfigure(encoding="utf-8", errors="replace")
    for line in sys.stdin:  # line by line, flushed: every rule is line-local, so streaming changes no output
        sys.stdout.write(red(line))
        sys.stdout.flush()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
