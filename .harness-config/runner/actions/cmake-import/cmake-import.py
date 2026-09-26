#!/usr/bin/env python3
"""cmake-import -- convert a CMake project into a DSS `.dss-project.json` manifest.

It runs `cmake` with CMAKE_EXPORT_COMPILE_COMMANDS=ON into a throwaway build
directory, then aggregates the compile_commands.json CMake wrote, across EVERY
translation unit, into the manifest the compiler consumes via `dsscp --project`:
  * sources  -- each entry's `file`       (forward-slash, deduped, sorted)
  * includes -- every -I / -isystem dir   (resolved vs the entry `directory`;
                deduped, sorted)
  * defines  -- every -D NAME[=VALUE]     (the -D stripped, deduped, sorted)
All other compiler flags (-c, -o, -O, -g, warnings, -std=...) are ignored --
DSS derives those itself.

Every path is written RELATIVE TO THE MANIFEST'S OWN DIRECTORY when it lies under
that directory, and ABSOLUTE otherwise (never with `..`). DSS resolves a manifest's
relative paths against the directory the manifest is in, so the manifest names the
same files from any working directory. Write it at the project root and it stays
relocatable with the project; written anywhere else, its paths are absolute.

Usage:
  python3 .harness-config/runner/actions/cmake-import/cmake-import.py <root-cmake-dir> <output-project-file> [options]
  python3 .harness-config/runner/actions/cmake-import/cmake-import.py --self-test
  python3 .harness-config/runner/actions/cmake-import/cmake-import.py --prove-any-cwd <dsscp>

Positional (both required):
  <root-cmake-dir>       CMake project root (must contain CMakeLists.txt)
  <output-project-file>  path of the .dss-project.json to write

Options (a value flag also takes the `--flag=value` form; `--` ends the options):
  --target <spec>        DSS "<targetName>:<formatName>" (repeatable).
                         Default = the host-native spec.
  --language <name>      DSS language name.        Default: c
  --profile <name>       DSS artifactProfile.      Default: cli
  --artifact-name <name> binary base name (no path separators).
                         Default: the root dir's basename (sanitized).
  --compile-commands <path>
                         transform an EXISTING compile_commands.json instead of
                         configuring; the root still names the project (its
                         basename is the default artifact name).
  --self-test            prove the refusals, the scratch rules, the path base and
                         the generator ladder against synthesized inputs; exit 0
                         only if every arm passes.
  --prove-any-cwd <dsscp>
                         import the bundled example into a manifest OUTSIDE its
                         root and build it with <dsscp> from the root, from a
                         directory of decoy look-alikes and from an empty one;
                         exit 0 only if all three build and the root-relative
                         manifest this tool once wrote fails from the last two.
  -h, --help             show this help and exit.

Requirements: CMake, with a generator that emits compile_commands.json. Ninja
and Unix Makefiles do; Visual Studio and Xcode do not. The default generator is
tried first, then Ninja, then Unix Makefiles, each in a fresh build directory.

NOTE: compile_commands.json is COMPILE-only -- it carries no link libraries,
so `resolveLibraries` is never emitted. If your project links external
libraries, add a `resolveLibraries` array to the manifest by hand.

Exit codes: 0 the manifest was written (or every self-test arm passed, or the
any-cwd proof held) · 1 refused -- a bad argument, a missing input, no
compile_commands.json, a transform error -- or a self-test arm or the proof
failed · 130 interrupted (Ctrl-C) · 143 terminated (SIGTERM, or SIGHUP where the
host has it).
"""

# ── ONE PROGRAM, AND WHAT RETIRED WITH THE TWO WRAPPERS ─────────────────────────
# Until 2026-09-21 this file was only the TRANSFORM, and `cmake-import.sh` and
# `cmake-import.ps1` were wrappers that parsed the command line, picked the host
# target, ran `cmake` and then started this file. The operator's ruling -- no
# `.sh`/`.ps1` under `.harness-config/runner/actions`, "they are specific per OS"
# -- folds the wrapper logic in here, so ONE program runs unchanged on every host
# and the "two wrappers, byte-identical by construction" argument has no subject
# left. Where the twins disagreed, the stricter rule won and is stated where it
# lives: `--` ends the options (the .sh honoured it), a lone `-` is refused (the
# .ps1 took it as a positional), flags are case-sensitive and never abbreviated
# (the .ps1 matched `--Target`; neither accepted `--lang`), the host is the
# OPERATING SYSTEM's view (the .ps1's), and the scratch cleanup survives SIGTERM
# and SIGHUP (only the .sh's did). Interpreter discovery, `cygpath`, `mktemp`,
# the awk help and `$LASTEXITCODE` retired with the files that needed them.
# ⚠ THIS FILE CARRIES NO PURPOSE DECLARATION, DELIBERATELY: `check-scripts-index`
# uses this action as its self-test subject, and that subject needs a program
# beside the action file that leaves the declaration to `cmake-import.yml`.

import contextlib
import hashlib
import io
import json
import os
import platform
import shutil
import signal
import subprocess
import sys
import tempfile

# ── OUTPUT ENCODING, AT IMPORT ───────────────────────────────────────────────────
# ✔MEASURED 2026-08-23 (CPython 3.14.3, Windows, both streams pipes -- how the
# action runner and ctest start every program): stdout comes up cp1252, so a
# printed path or CMake log line holding a character outside cp1252 would raise
# `UnicodeEncodeError` inside the report. Reconfigured here, before anything can
# print, so `--help`, every refusal and the summary are covered.
for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError, OSError):   # pragma: no cover - an odd stream
        pass


# ⚠ FROM HERE TO `aggregate` IS THE TRANSFORM THE MANIFEST BYTES DEPEND ON. It is
# unchanged from the day it was the wrappers' shared transform, so a manifest this
# program writes AT THE PROJECT ROOT is byte-identical to the one `cmake-import.sh`
# wrote for the same project. Changing any of it changes the manifests every existing
# import produced. ⓘ What changed on 2026-09-21 is the BASE the paths are made
# relative to -- the manifest's directory, no longer the project root (`main`): the
# two coincide exactly when the manifest is written at the root.

# ─────────────────────────────────────────────────────────────────────────────
# path canonicalization (pure string ops — no filesystem access)
# ─────────────────────────────────────────────────────────────────────────────
def is_alpha(c):
    o = ord(c)
    return (97 <= o <= 122) or (65 <= o <= 90)


def is_absolute(p):
    if p.startswith("/"):
        return True
    if len(p) >= 2 and p[1] == ":" and is_alpha(p[0]):
        return True
    return False


def collapse(p):
    """Lexical path normalization on a forward-slash string."""
    drive = ""
    root = ""
    rest = p
    if rest.startswith("/"):
        root = "/"; rest = rest[1:]
    elif len(rest) >= 2 and rest[1] == ":" and is_alpha(rest[0]):
        drive = rest[0:2]; rest = rest[2:]
        if rest.startswith("/"):
            root = "/"; rest = rest[1:]
    out = []
    for seg in rest.split("/"):
        if seg == "" or seg == ".":
            continue
        if seg == "..":
            if out and out[-1] != "..":
                out.pop()
            elif root == "":
                out.append("..")
            continue
        out.append(seg)
    return drive + root + "/".join(out)


def canon(raw, directory):
    """Make `raw` absolute (relative to `directory`) with forward slashes."""
    p = raw.replace("\\", "/")
    if not is_absolute(p):
        d = directory.replace("\\", "/")
        if d and not d.endswith("/"):
            d += "/"
        p = d + p
    return collapse(p)


# ─────────────────────────────────────────────────────────────────────────────
# command-string tokenizer
# ─────────────────────────────────────────────────────────────────────────────
def tokenize(s):
    """Minimal shell-like tokenizer for a compile_commands.json `command`.

    Whitespace separates tokens; "..." and '...' spans preserve inner spaces.
    Backslash is handled ONLY for an escaped double-quote (\\" -> literal ")
    and, inside a "..." span, \\\\ -> literal \\ ; every other backslash stays
    literal, so Windows paths (C:\\Users\\..., a lone backslash before a letter)
    survive. This matches how CMake/Ninja shell-escape string-valued -D
    defines: -DVER=\\"1.2.3\\" -> the define VER="1.2.3", and
    -DMSG="\\"hi there\\"" -> MSG="hi there". Single-quoted spans are fully
    literal (POSIX).
    """
    toks = []
    cur = None
    quote = None                       # None, '"', or "'"
    i, n = 0, len(s)
    while i < n:
        c = s[i]
        if quote == '"':
            if c == "\\" and i + 1 < n and s[i + 1] in ('"', "\\"):
                cur += s[i + 1]; i += 2; continue
            if c == '"':
                quote = None; i += 1; continue
            cur += c; i += 1; continue
        if quote == "'":
            if c == "'":
                quote = None; i += 1; continue
            cur += c; i += 1; continue
        # not inside a quote span
        if c in " \t\r\n":
            if cur is not None:
                toks.append(cur); cur = None
            i += 1; continue
        if c == "\\" and i + 1 < n and s[i + 1] == '"':
            if cur is None: cur = ""
            cur += '"'; i += 2; continue
        if c == '"':
            if cur is None: cur = ""
            quote = '"'; i += 1; continue
        if c == "'":
            if cur is None: cur = ""
            quote = "'"; i += 1; continue
        if cur is None: cur = ""
        cur += c
        i += 1
    if cur is not None:
        toks.append(cur)
    return toks


def relativize(abs_path, base):
    """Lexical: `abs_path` relative to `base` when under it, else `abs_path`.

    Both are already normalized forward-slash strings. `abs == base` -> "." ;
    `abs` under `base` -> the remainder (e.g. "src/main.c") ; otherwise the
    absolute path is kept (e.g. a toolchain -isystem dir outside the project).
    Pure string op — no filesystem access.
    """
    if base.endswith("/"):
        base = base[:-1]                 # tolerate a root-ish base
    if abs_path == base:
        return "."
    prefix = base + "/"
    if abs_path.startswith(prefix):
        return abs_path[len(prefix):]
    return abs_path


def sanitize_name(s):
    """Reduce a raw string to a bare file name ([A-Za-z0-9._-], else '_')."""
    out = []
    for c in s:
        o = ord(c)
        if (97 <= o <= 122) or (65 <= o <= 90) or (48 <= o <= 57) or c in "._-":
            out.append(c)
        else:
            out.append("_")
    return "".join(out)


ISYS = "-isystem"


# ─────────────────────────────────────────────────────────────────────────────
def aggregate(entries):
    """Return (sorted_sources, sorted_includes, sorted_defines)."""
    sources, includes, defines = set(), set(), set()
    for e in entries:
        if not isinstance(e, dict):
            continue
        directory = e.get("directory", "") or ""
        args = e.get("arguments")
        if isinstance(args, list):
            toks = [str(x) for x in args]
        else:
            toks = tokenize(e.get("command", "") or "")

        f = e.get("file", "") or ""
        if f:
            sources.add(canon(f, directory))

        j, m = 0, len(toks)
        while j < m:
            t = toks[j]
            if t == "-I":
                if j + 1 < m:
                    includes.add(canon(toks[j + 1], directory)); j += 2; continue
            elif t.startswith("-I") and len(t) > 2:
                includes.add(canon(t[2:], directory)); j += 1; continue
            elif t == ISYS:
                if j + 1 < m:
                    includes.add(canon(toks[j + 1], directory)); j += 2; continue
            elif t.startswith(ISYS) and len(t) > len(ISYS):
                includes.add(canon(t[len(ISYS):], directory)); j += 1; continue
            elif t == "-D":
                if j + 1 < m:
                    defines.add(toks[j + 1]); j += 2; continue
            elif t.startswith("-D") and len(t) > 2:
                defines.add(t[2:]); j += 1; continue
            j += 1

    return sorted(sources), sorted(includes), sorted(defines)


# ═════════════════════════════════════════════════════════════════════════════
# THE PROGRAM -- what the retired wrappers did, in the order they did it:
# parse (C1-C7), validate (C8-C9), pick the host target (C11), make the scratch
# (C12), configure (C14-C15), transform (T1-T8), clean up on every exit (C13).
# ═════════════════════════════════════════════════════════════════════════════

PROG = "cmake-import.py"

# The value flags. Each also takes the `--flag=value` form. Nothing is matched by
# prefix or case-folded: `--lang` and `--Target` are unknown flags, because a flag
# that "probably meant" another one is a command line nobody wrote.
VALUE_FLAGS = ("--target", "--language", "--profile", "--artifact-name",
               "--compile-commands", "--prove-any-cwd")

# The host-native default target (C11). Format names are the shipped
# `src/dss-config/object-formats/*.format.json` stems; target names are the shipped
# `targets/*.target.json` `target.name` values. ⓘ `compile-bench.py` carries the
# same five rows (`HOST_TARGETS`) for the same question -- a host gaining a row has
# to gain it in both.
HOST_SPECS = {
    ("Linux", "x86_64"): "x86_64:elf64-x86_64-linux-exec",
    ("Linux", "arm64"): "arm64:elf64-aarch64-linux-exec",
    ("macOS", "arm64"): "arm64:macho64-arm64-darwin-exec",
    ("macOS", "x86_64"): "x86_64:macho64-x86_64-darwin-exec",
    ("Windows", "x86_64"): "x86_64:pe64-x86_64-windows-exec",
}
MACHINE_ALIASES = {"x86_64": "x86_64", "amd64": "x86_64",
                   "aarch64": "arm64", "arm64": "arm64"}

# The generator ladder (C14): the default generator first -- the one the user's own
# CMake would pick -- then the two known to write compile_commands.json. "" = no -G.
GENERATORS = ("", "Ninja", "Unix Makefiles")

SCRATCH_PREFIX = ".dss-cmake-import-build."
LOG_NAME = ".cmake-import.log"
RULE = "-" * 64

# The signals whose arrival still removes the scratch (C13). Ctrl-C needs no handler:
# Python raises KeyboardInterrupt on SIGINT already.
TRAPPED_SIGNALS = ("SIGTERM", "SIGHUP")


class Refusal(Exception):
    """Printed as `cmake-import.py: error: <message>`; the exit code is 1."""


class _Terminated(BaseException):
    """SIGTERM or SIGHUP arrived. A BaseException, so no `except Exception` on the way
    out can swallow it before `main`'s cleanup and its exit code 143."""


class Options(object):
    """What the command line asked for -- see `parse_cli`."""

    def __init__(self):
        self.root = None
        self.output = None
        self.targets = []
        self.language = "c"
        self.profile = "cli"
        self.artifact = ""
        self.compile_commands = None
        self.help = False
        self.self_test = False
        self.prove_any_cwd = None      # the dsscp `--prove-any-cwd` builds with


def _flush():
    """Flush both streams, so a child writing to the same pipe cannot overtake them."""
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.flush()
        except (AttributeError, ValueError, OSError):
            pass


# ── C1-C7: the command line ─────────────────────────────────────────────────────
def parse_cli(argv):
    """argv -> Options, refusing (Refusal) the way the retired wrappers did.

    Arguments are read in order, so `-h` answers at once unless an unknown flag came
    before it. `--` ends the options: everything after it is positional, a leading
    `-` included. A lone `-` is an unknown flag, not a positional (the .sh's rule; the
    .ps1 took it as a path). An EMPTY `--artifact-name` means "derive it", as in both
    twins; an empty `--target` is refused -- neither twin checked, and it would write
    a manifest naming the target "".
    """
    o = Options()
    positionals = []
    i, n = 0, len(argv)
    while i < n:
        a = argv[i]
        if a in ("-h", "--help"):
            o.help = True
            return o
        if a == "--self-test":
            o.self_test = True
            i += 1
            continue
        if a == "--":
            positionals.extend(argv[i + 1:])
            break
        flag = value = None
        for f in VALUE_FLAGS:
            if a == f:
                if i + 1 >= n:
                    raise Refusal("%s requires a value" % f)
                flag, value = f, argv[i + 1]
                i += 2
                break
            if a.startswith(f + "="):
                flag, value = f, a[len(f) + 1:]
                i += 1
                break
        if flag is None:
            if a.startswith("-"):
                raise Refusal("unknown flag '%s' (see --help)" % a)
            positionals.append(a)
            i += 1
        elif flag == "--target":
            o.targets.append(value)
        elif flag == "--language":
            o.language = value
        elif flag == "--profile":
            o.profile = value
        elif flag == "--artifact-name":
            o.artifact = value
        elif flag == "--prove-any-cwd":
            o.prove_any_cwd = value
        else:
            o.compile_commands = value

    if o.self_test:
        if n != 1:
            raise Refusal("--self-test takes no other argument (see --help)")
        return o
    if o.prove_any_cwd is not None:
        if argv not in (["--prove-any-cwd", o.prove_any_cwd],
                        ["--prove-any-cwd=" + o.prove_any_cwd]):
            raise Refusal("--prove-any-cwd takes no other argument (see --help)")
        if not o.prove_any_cwd:
            raise Refusal("--prove-any-cwd must be non-empty")
        return o
    if len(positionals) < 2:
        raise Refusal("expected 2 positional arguments <root-cmake-dir> "
                      "<output-project-file>; got %d (see --help)" % len(positionals))
    if len(positionals) > 2:
        raise Refusal("too many positional arguments (%d); expected exactly "
                      "<root-cmake-dir> <output-project-file> (see --help)"
                      % len(positionals))
    o.root, o.output = positionals
    if not o.language:
        raise Refusal("--language must be non-empty")
    if not o.profile:
        raise Refusal("--profile must be non-empty")
    if any(not t for t in o.targets):
        raise Refusal("--target must be non-empty")
    if o.compile_commands is not None and not o.compile_commands:
        raise Refusal("--compile-commands must be non-empty")
    if "/" in o.artifact or "\\" in o.artifact:
        raise Refusal("--artifact-name must be a bare file name (no '/' or '\\'): '%s'"
                      % o.artifact)
    return o


# ── C8-C9: the inputs and the tool ──────────────────────────────────────────────
def _filesystem_spelling(path):
    """`path` as the filesystem stores it, when that differs from `path` ONLY in case.

    ✔MEASURED 2026-09-21 (Windows 11, CMake 4.3.2): given `-S` in the wrong case -- a
    lower-case `c:\\source\\...` typed at a prompt -- CMake wrote every path in
    compile_commands.json, and CMAKE_HOME_DIRECTORY, in the case the filesystem stores.
    A base typed that way (then the root, now the manifest's directory) matched none of
    them and the manifest came out with ABSOLUTE paths; the retired .sh wrote the same
    absolute manifest for the same input.
    `realpath` names the stored case, but it also follows links, which CMake does not:
    a realpath that is a DIFFERENT path, rather than this path in another case, is not
    taken, so a symlinked root keeps its own spelling. Where `normcase` does not fold
    case (POSIX) only an identical realpath matches, so nothing is respelled -- and
    CMake respells nothing there either.
    """
    real = os.path.realpath(path)
    if os.path.normcase(real) == os.path.normcase(path):
        return real
    return path


def validate_inputs(root, direct, cmake=None):
    """-> (the root as an absolute path, the cmake argv prefix or None when `direct`).

    ★ The root is `os.path.abspath`, respelled only in case (`_filesystem_spelling`) and
    never resolved through a link: the SAME string is handed to `cmake -S` and named
    for the default artifact, so a symlinked root keeps its own name. (The emitted
    paths are made relative to the MANIFEST's directory, spelled the same way -- see
    `main`.) A `direct` (`--compile-commands`) run configures nothing, so it needs
    neither a CMakeLists.txt nor cmake -- only the root it names. `cmake` replaces the
    PATH lookup with an argv prefix; only the self-test passes one.
    """
    if not os.path.isdir(root):
        raise Refusal("root directory not found: '%s'" % root)
    root_abs = _filesystem_spelling(os.path.abspath(root))
    if direct:
        return root_abs, None
    if not os.path.isfile(os.path.join(root, "CMakeLists.txt")):
        raise Refusal("no CMakeLists.txt in root directory: '%s'" % root)
    if cmake is None:
        found = shutil.which("cmake")
        if not found:
            raise Refusal("cmake not found on PATH — install CMake and retry")
        cmake = [os.path.abspath(found)]
    return root_abs, list(cmake)


# ── C11: the host-native target ─────────────────────────────────────────────────
def _darwin_os_machine():
    """`uname -m`, except under Rosetta 2: an x86_64 interpreter on an arm64 Mac reads
    `x86_64` from uname while the operating system is arm64, and
    `sysctl.proc_translated` is 1 exactly then (the key does not exist on an Intel Mac)."""
    raw = platform.machine()
    if raw == "x86_64":
        try:
            p = subprocess.run(["/usr/sbin/sysctl", "-n", "sysctl.proc_translated"],
                               stdin=subprocess.DEVNULL, capture_output=True, text=True)
        except OSError:
            return raw
        if p.returncode == 0 and p.stdout.strip() == "1":
            return "arm64"
    return raw


def detect_host_spec(system=None, environ=None, machine=None):
    """The host-native `<target>:<format>` spec, from the OPERATING SYSTEM's view of the
    machine -- never the process's, which an emulated or 32-bit interpreter misreports.

    On Windows `PROCESSOR_ARCHITEW6432` wins over `PROCESSOR_ARCHITECTURE`: Windows sets
    it for a process whose architecture is not the system's, and there it names the
    system's (the retired .ps1 read the same fact as `OSArchitecture`). On macOS a
    Rosetta-translated interpreter is seen through (`_darwin_os_machine`). An
    MSYS/MinGW/Cygwin Python reports its own system name and is a Windows host too.
    `system`, `environ` and `machine` default to this host; the self-test passes its own.
    """
    system = platform.system() if system is None else system
    environ = os.environ if environ is None else environ
    if system in ("Windows", "Windows_NT") or system.startswith(("MINGW", "MSYS", "CYGWIN")):
        label = "Windows"
        raw = (environ.get("PROCESSOR_ARCHITEW6432") or environ.get("PROCESSOR_ARCHITECTURE")
               or (platform.machine() if machine is None else machine))
    elif system == "Linux":
        label, raw = "Linux", platform.machine() if machine is None else machine
    elif system == "Darwin":
        label, raw = "macOS", _darwin_os_machine() if machine is None else machine
    else:
        raise Refusal("unrecognized host OS '%s' — pass --target explicitly" % system)
    spec = HOST_SPECS.get((label, MACHINE_ALIASES.get(raw.lower())))
    if spec is None:
        raise Refusal("unsupported %s architecture '%s' — pass --target explicitly"
                      % (label, raw))
    return spec


# ── C12: the scratch build root ─────────────────────────────────────────────────
def make_scratch(root_abs):
    """A NEW directory `<root>/.dss-cmake-import-build.<random>`, created atomically.

    ★ UNIQUE PER RUN -- do NOT "simplify" it back to a constant name. Two concurrent
    imports of the SAME project (a CI matrix, a parallel test suite, two people on one
    host) would otherwise share one directory, and each run's cleanup would delete the
    other's in-flight CMake output. A pid suffix would not be enough either: pids
    recycle, and a killed run leaves its directory behind. `tempfile.mkdtemp` creates
    with an exclusive `mkdir` and draws another name when one is taken, so an existing
    directory -- someone else's run, or a leftover -- is never handed back. That is the
    .sh's `mktemp -d` and the .ps1's verified `New-Item` without `-Force` in one call.
    """
    try:
        return tempfile.mkdtemp(prefix=SCRATCH_PREFIX, dir=root_abs)
    except OSError as exc:
        raise Refusal("could not create a scratch build directory under '%s' (%s)"
                      % (root_abs, exc))


# ── C14-C15: the configure, down the generator ladder ───────────────────────────
def configure(cmake, root_abs, scratch):
    """Configure `root_abs` once per generator until one writes compile_commands.json.

    Each attempt gets its OWN fresh `attemptN` directory, because CMake cannot switch
    generators inside an existing build tree -- and a never-reused name means nothing
    here ever has to be deleted mid-run. Success is cmake's exit code 0 AND the file:
    Visual Studio and Xcode exit 0 without writing it. The log is rewritten per attempt,
    so the refusal shows what the LAST generator said. `cmake` is an argv prefix.
    """
    log = os.path.join(scratch, LOG_NAME)
    for n, gen in enumerate(GENERATORS, 1):
        build = os.path.join(scratch, "attempt%d" % n)
        try:
            os.mkdir(build)
        except OSError as exc:
            raise Refusal("could not create the build directory '%s' (%s)" % (build, exc))
        argv = list(cmake) + ["-S", root_abs, "-B", build,
                              "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON"]
        if gen:
            argv += ["-G", gen]
        try:
            fh = open(log, "wb")
        except OSError as exc:
            raise Refusal("could not write the CMake log '%s' (%s)" % (log, exc))
        with fh:
            _flush()
            try:
                rc = subprocess.run(argv, stdin=subprocess.DEVNULL, stdout=fh,
                                    stderr=subprocess.STDOUT).returncode
            except OSError as exc:
                raise Refusal("could not start cmake '%s' (%s)" % (argv[0], exc))
        ccjson = os.path.join(build, "compile_commands.json")
        if rc == 0 and os.path.isfile(ccjson):
            return ccjson

    text = ""
    if os.path.isfile(log):
        with open(log, "rb") as fh:
            # CRLF folded to LF: this text is written through a text stream, which puts
            # the host's line ending back -- a raw CRLF would come out doubled on Windows.
            text = fh.read().decode("utf-8", "replace").replace("\r\n", "\n")
        if text and not text.endswith("\n"):
            text += "\n"
    raise Refusal("CMake did not produce compile_commands.json.\n"
                  "Tried the default generator, Ninja, and Unix Makefiles.\n"
                  "Use a generator that supports CMAKE_EXPORT_COMPILE_COMMANDS\n"
                  "(Ninja or Unix Makefiles). Last CMake output:\n"
                  + RULE + "\n" + text + RULE)


# ── T1-T8: the transform ────────────────────────────────────────────────────────
def emit_manifest(compile_commands, output, targets, language, profile, artifact,
                  relative_to):
    """Aggregate `compile_commands` and write the manifest to `output`. Returns 0.

    Everything this writes -- the key order, `indent=2`, `ensure_ascii=False`, UTF-8,
    LF, one trailing newline, the ordinal sorts -- is what the wrappers' shared
    transform wrote, and the two summary lines are its two lines. `relative_to` lists
    the spellings of the MANIFEST's directory (as stored, and as typed when those
    differ): a path under ANY of them is emitted relative to it, because CMake writes
    the stored case while a tool that copies its caller's spelling writes the typed
    one; every other path is emitted absolute.
    """
    try:
        with open(compile_commands, "r", encoding="utf-8") as f:
            entries = json.load(f)
    except FileNotFoundError:
        raise Refusal("compile_commands.json not found: %s" % compile_commands)
    except Exception as e:
        raise Refusal("could not read compile_commands.json: %s" % e)

    if not isinstance(entries, list):
        raise Refusal("compile_commands.json is not a JSON array")

    sources, includes, defines = aggregate(entries)

    # ★ Emit a path RELATIVE TO THE MANIFEST'S DIRECTORY when it lives under it, and
    # ABSOLUTE otherwise -- never with `..`. DSS resolves a manifest's relative paths
    # against the directory the manifest is in, so either spelling names the same file
    # from any working directory. ⚠ Until 2026-09-21 the base was the PROJECT ROOT,
    # which is right only for a manifest written AT the root: this program's own runner
    # writes its manifest into the step's build directory, where root-relative names
    # resolved to nothing (or, from a directory holding look-alikes, to the wrong
    # files). Re-sort the final (relativized) strings.
    if relative_to:
        bases = [collapse(b.replace("\\", "/")) for b in relative_to]

        def under_root(p):
            for base in bases:
                q = relativize(p, base)
                if q != p:
                    return q
            return p

        sources = sorted({under_root(s) for s in sources})
        includes = sorted({under_root(s) for s in includes})

    if not sources:
        raise Refusal("no source files found in compile_commands.json "
                      "(nothing to import)")

    # ── dedup-preserve-order for targets ───────────────────────────────────
    seen = set(); unique_targets = []
    for t in targets:
        if t not in seen:
            seen.add(t); unique_targets.append(t)

    # ── build manifest (fixed key order) ───────────────────────────────────
    manifest = {}
    manifest["language"] = language
    manifest["artifactProfile"] = profile
    manifest["targets"] = unique_targets
    if artifact:
        manifest["artifactName"] = artifact
    manifest["sources"] = sources
    if includes:
        manifest["includes"] = includes
    if defines:
        manifest["defines"] = defines

    text = json.dumps(manifest, indent=2, ensure_ascii=False)
    try:
        with open(output, "wb") as f:
            f.write(text.encode("utf-8"))
            f.write(b"\n")
    except OSError as e:
        raise Refusal("could not write %s: %s" % (output, e))

    # ── summary ────────────────────────────────────────────────────────────
    sys.stdout.write("cmake-import: %d sources, %d includes, %d defines -> %s\n"
                     % (len(sources), len(includes), len(defines), output))
    sys.stdout.write("note: link libraries are NOT captured from "
                     "compile_commands.json (compile-only). Add a "
                     "\"resolveLibraries\" array by hand if this project links "
                     "external libraries.\n")
    return 0


# ── C13: cleanup on every exit ──────────────────────────────────────────────────
def _remove_tree(path):
    """Remove `path` and everything under it, read-only files included. True when gone.

    Windows refuses to unlink a read-only file -- what the retired .ps1's `-Force` was
    for -- and a configure that clones through FetchContent leaves git's read-only
    object files in the build tree. The same rule as `owning-tree.py`'s `remove_tree`,
    kept here rather than imported: the cleanup path is the last place a missing
    sibling may be allowed to fail.
    """
    def _writable_then_retry(func, p, _exc):
        try:
            os.chmod(p, 0o700)
            func(p)
        except OSError:
            pass

    if os.path.lexists(path):
        try:
            if sys.version_info >= (3, 12):
                shutil.rmtree(path, onexc=_writable_then_retry)
            else:  # pragma: no cover - a pre-3.12 interpreter
                shutil.rmtree(path, onerror=_writable_then_retry)
        except OSError:
            pass
    return not os.path.lexists(path)


def _on_terminate(signum, _frame):
    raise _Terminated(signum)


def _signals(names):
    return [getattr(signal, name) for name in names if hasattr(signal, name)]


def _set_handlers(sigs, handler):
    """Install `handler` on each signal in `sigs` -> {signal: the handler it replaced}.
    A signal this process cannot trap here (not the main thread, or refused by the
    host) is left as it is."""
    held = {}
    for sig in sigs:
        try:
            held[sig] = signal.signal(sig, handler)
        except (ValueError, OSError, RuntimeError):
            pass
    return held


def _restore_handlers(held):
    for sig, handler in held.items():
        try:
            signal.signal(sig, signal.SIG_DFL if handler is None else handler)
        except (ValueError, OSError, RuntimeError):
            pass


def main(argv=None, cmake=None):
    """Run the command line `argv` (default: this process's). Returns the exit code.

    ★ THE SCRATCH ROOT IS REMOVED ON EVERY WAY OUT: success, a refusal, a crash,
    Ctrl-C (exit 130), and SIGTERM or SIGHUP (exit 143) -- how CI cancels a job. A child
    cmake running at that moment is killed and waited for by `subprocess.run` before
    the exception reaches the cleanup. The retired .ps1 lost SIGTERM/SIGHUP on POSIX
    hosts; the .sh trapped both, and so does this. While the directory is removed the
    three signals are ignored, so a second one cannot cut the cleanup short. A
    directory that cannot be removed is NAMED on stderr, never left silently in the
    user's project. `cmake` is the self-test's stand-in (see `validate_inputs`).
    """
    argv = sys.argv[1:] if argv is None else list(argv)
    held = _set_handlers(_signals(TRAPPED_SIGNALS), _on_terminate)
    scratch = None
    try:
        try:
            o = parse_cli(argv)
            if o.help:
                sys.stdout.write(__doc__ or "cmake-import: no help text (python -OO)\n")
                return 0
            if o.self_test:
                return self_test()
            if o.prove_any_cwd is not None:
                return prove_any_cwd(o.prove_any_cwd)
            direct = o.compile_commands is not None
            root_abs, cmake_cmd = validate_inputs(o.root, direct, cmake)
            # The base every emitted path is made relative to: the MANIFEST's directory,
            # as stored and as typed (the same case-only respelling the root gets).
            out_typed = os.path.dirname(os.path.abspath(o.output))
            out_dir = _filesystem_spelling(out_typed)
            spellings = [out_dir] + ([out_typed] if out_typed != out_dir else [])
            targets = o.targets or [detect_host_spec()]
            artifact = o.artifact or sanitize_name(os.path.basename(root_abs))
            if direct:
                ccjson = o.compile_commands
            else:
                scratch = make_scratch(root_abs)
                # Named once, so a run that fails or is interrupted is debuggable even
                # though the directory's name differs every time.
                sys.stderr.write("%s: scratch build dir: %s\n" % (PROG, scratch))
                ccjson = configure(cmake_cmd, root_abs, scratch)
            return emit_manifest(ccjson, o.output, targets, o.language, o.profile,
                                 artifact, spellings)
        except Refusal as exc:
            _flush()
            sys.stderr.write("%s: error: %s\n" % (PROG, exc))
            return 1
        except KeyboardInterrupt:
            return 130
        except _Terminated:
            return 143
    finally:
        quiet = _set_handlers(_signals(("SIGINT",) + TRAPPED_SIGNALS), signal.SIG_IGN)
        try:
            if scratch is not None and not _remove_tree(scratch):
                sys.stderr.write("%s: warning: could not remove the scratch build "
                                 "directory '%s' -- remove it by hand\n" % (PROG, scratch))
        finally:
            _restore_handlers(quiet)
            _restore_handlers(held)


# ═════════════════════════════════════════════════════════════════════════════
# --prove-any-cwd <dsscp>
# ═════════════════════════════════════════════════════════════════════════════
# ★ THE PIN THE PATH BASE ANSWERS TO (ctest `harness/cmake_import_any_cwd`). A manifest
# this program writes OUTSIDE the project root must build the SAME files from ANY working
# directory: the root, a directory holding a look-alike of every root-relative name (each
# an `#error`, so reading one fails the build), and an empty directory -- and the three
# builds must make byte-identical artefacts. The subject is the bundled example, copied
# into a temp box so the action directory is never written.
# ★★ IT SYNTHESIZES ITS NEGATIVE: the ROOT-RELATIVE manifest this program wrote until
# 2026-09-21, at the same place, must FAIL from the decoy and from the empty directory.
# Both hold whichever base a compiler resolves relative names against (the process's
# working directory, where the decoy's `#error` is read; or the manifest's own, where the
# names miss), so the proof does not depend on which one the compiler under test
# implements. A TRAP CONTROL proves the decoy is a trap at all: a manifest naming the
# decoy's files must fail AND print the decoy's mark, so a positive build from the decoy
# directory that had read one could not have passed.
# ⓘ The compile_commands.json is WRITTEN, not configured: the entries CMake writes for
# `example/CMakeLists.txt` (two TUs, its include dir, SCALE=2 and DEMO_BUILD). So the pin
# needs no CMake and no C compiler on the host; the generator ladder has its own arms.

EXAMPLE_DIR = os.path.join(os.path.dirname(os.path.realpath(__file__)), "example")
_EXAMPLE_TUS = ("src/main.c", "src/util.c")
_EXAMPLE_LOOKALIKES = _EXAMPLE_TUS + ("include/mathlib.h",)
_DECOY_MARK = "DECOY-READ"


def _example_entries(proj, build):
    """The compile_commands.json entries CMake writes for the bundled example."""
    p, b = proj.replace("\\", "/"), build.replace("\\", "/")
    return [{"directory": b, "file": p + "/" + tu,
             "arguments": ["cc", "-DSCALE=2", "-DDEMO_BUILD", "-I" + p + "/include", "-c",
                           p + "/" + tu]} for tu in _EXAMPLE_TUS]


def _dsscp_build(dsscp, manifest, cwd, out_dir):
    """Build `manifest` with `dsscp` from `cwd` -> (exit code or None, merged output,
    {each file made, relative to `out_dir` with forward slashes: the md5 of its bytes})."""
    try:
        p = subprocess.run([dsscp, "--project", manifest, "--output", out_dir], cwd=cwd,
                           stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, timeout=600)
    except (OSError, subprocess.TimeoutExpired) as exc:
        return None, "could not run %s: %s" % (dsscp, exc), {}
    made = {}
    for dirpath, _dirs, files in os.walk(out_dir):
        for f in files:
            path = os.path.join(dirpath, f)
            made[os.path.relpath(path, out_dir).replace("\\", "/")] = \
                hashlib.md5(_read_bytes(path)).hexdigest()
    return p.returncode, p.stdout.decode("utf-8", "replace"), made


def _last_lines(text, n=4):
    return "\n        ".join(text.strip().splitlines()[-n:])


def prove_any_cwd(dsscp):
    """Run the proof described above. Returns 0 only when every case held."""
    dsscp = os.path.abspath(dsscp)
    if not os.path.isfile(dsscp):
        raise Refusal("--prove-any-cwd: no dsscp at '%s'" % dsscp)
    if not os.path.isfile(os.path.join(EXAMPLE_DIR, "CMakeLists.txt")):
        raise Refusal("--prove-any-cwd: the bundled example is missing: '%s'" % EXAMPLE_DIR)
    box = os.path.realpath(tempfile.mkdtemp(prefix="cmake-import-any-cwd-"))
    failed = []

    def case(label, ok, detail):
        sys.stdout.write("  %s %s\n" % ("ok  " if ok else "FAIL", label))
        if not ok:
            failed.append(label)
            sys.stdout.write("        %s\n" % detail)
        _flush()

    try:
        proj = os.path.join(box, "proj")
        shutil.copytree(EXAMPLE_DIR, proj)
        ccdir = os.path.join(box, "cc")
        os.makedirs(os.path.join(ccdir, "build"))
        cc = os.path.join(ccdir, "compile_commands.json")
        _write(cc, json.dumps(_example_entries(proj, os.path.join(ccdir, "build")), indent=1))
        outside = os.path.join(box, "out", "deep")
        os.makedirs(outside)
        manifest = os.path.join(outside, "example.dss-project.json")
        rc = main(["--compile-commands", cc, "--target", detect_host_spec(), proj, manifest])
        if rc != 0:
            case("the example imports into a manifest outside its root", False, "exit %d" % rc)
            return 1
        m = json.loads(_read_bytes(manifest).decode("utf-8"))
        p = proj.replace("\\", "/")
        want = ([p + "/" + tu for tu in _EXAMPLE_TUS], [p + "/include"])
        got = (m.get("sources"), m.get("includes"))
        case("a manifest written outside the root names every path ABSOLUTE", got == want,
             "sources/includes %r, expected %r" % (got, want))

        decoy, empty = os.path.join(box, "decoy"), os.path.join(box, "empty")
        for rel in _EXAMPLE_LOOKALIKES:
            path = os.path.join(decoy, *rel.split("/"))
            os.makedirs(os.path.dirname(path), exist_ok=True)
            _write(path, "#error %s %s: a manifest path resolved against the working "
                         "directory\n" % (_DECOY_MARK, rel))
        os.makedirs(empty)
        builds = []
        for i, (name, cwd) in enumerate((("the project root", proj),
                                         ("a directory of decoy look-alikes", decoy),
                                         ("an empty directory", empty))):
            rc, text, made = _dsscp_build(dsscp, manifest, cwd,
                                          os.path.join(box, "art", "absolute-%d" % i))
            case("it builds from %s" % name, rc == 0 and bool(made) and _DECOY_MARK not in text,
                 "exit %r, %d file(s) made, decoy read: %s\n        %s"
                 % (rc, len(made), _DECOY_MARK in text, _last_lines(text)))
            builds.append((name, made))
        # "The same files both times", read literally: every build made the same files with
        # the same bytes. ✔MEASURED 2026-09-21 that dsscp's output does not depend on its
        # working directory (pe64, elf64 and macho64, byte-identical from four directories).
        case("the three builds made BYTE-IDENTICAL artefacts",
             all(made == builds[0][1] for _name, made in builds) and bool(builds[0][1]),
             "\n        ".join("%s: %r" % (name, made) for name, made in builds))

        d = decoy.replace("\\", "/")
        trap = os.path.join(outside, "decoy.dss-project.json")
        _write(trap, json.dumps(dict(m, sources=[d + "/" + tu for tu in _EXAMPLE_TUS],
                                     includes=[d + "/include"]), indent=2) + "\n")
        rc, text, _made = _dsscp_build(dsscp, trap, empty, os.path.join(box, "art", "trap"))
        case("TRAP CONTROL: a manifest naming the decoy's files fails and prints its mark",
             rc not in (0, None) and _DECOY_MARK in text,
             "exit %r, mark printed: %s -- the decoy is no trap, so a build from its directory "
             "proves nothing\n        %s" % (rc, _DECOY_MARK in text, _last_lines(text)))

        neg = os.path.join(outside, "root-relative.dss-project.json")
        _write(neg, json.dumps(dict(m, sources=list(_EXAMPLE_TUS), includes=["include"]),
                               indent=2) + "\n")
        for i, (name, cwd) in enumerate((("the decoy directory", decoy),
                                         ("the empty directory", empty))):
            rc, text, _made = _dsscp_build(dsscp, neg, cwd,
                                           os.path.join(box, "art", "root-relative-%d" % i))
            # Failing is not enough: it must fail on the SOURCE it cannot name, never on
            # something else (a missing config fails every build, the positive ones too).
            case("NEGATIVE CONTROL: the root-relative manifest fails from %s, on its source"
                 % name, rc not in (0, None) and "main.c" in text,
                 "exit %r, names main.c: %s -- a manifest naming its files relative to the ROOT "
                 "did not fail on them from outside the root, so the positive cases above prove "
                 "nothing\n        %s" % (rc, "main.c" in text, _last_lines(text)))
    finally:
        if not _remove_tree(box):
            sys.stderr.write("%s: warning: could not remove the proof box '%s'\n" % (PROG, box))
    if failed:
        sys.stdout.write("cmake-import: any-cwd proof FAILED -- %d case(s): %s\n"
                         % (len(failed), "; ".join(failed)))
        return 1
    sys.stdout.write("cmake-import: any-cwd proof OK -- a manifest written outside the root "
                     "built the same files from the root, a decoy directory and an empty one\n")
    return 0


# ═════════════════════════════════════════════════════════════════════════════
# --self-test
# ═════════════════════════════════════════════════════════════════════════════
# ★ THE WRAPPERS HAD ZERO ARMS; the only behavioural witness of this program was the
# runner step, whose one leg is Linux. Every arm below is a SYNTHESIZED negative with a
# control beside it, asserts the exact message, and is reported by name; the arm count
# is pinned, so an arm that silently stops running is itself a failure.
# ⚠ Raising EXPECTED_ARMS is the claim that the arms you added actually RUN.
EXPECTED_ARMS = 20

# A stand-in for `cmake`, started as `[sys.executable, <this file>]` so the ladder runs
# on every host with no CMake and no shell. It reads its behaviour per generator from
# the `plan.json` beside it -- `ok` writes a compile_commands.json naming the `-S`
# root's `src/main.c` with one include and one define, `rc0-no-file` exits 0 having
# written nothing (what Visual Studio and Xcode do), anything else exits 1 -- and
# appends every invocation (generator, `-S`, `-B`, and whether `-B` was a fresh empty
# directory) to the plan's `record` file.
_FAKE_CMAKE = r'''
import json, os, sys
here = os.path.dirname(os.path.abspath(__file__))
with open(os.path.join(here, "plan.json"), encoding="utf-8") as fh:
    plan = json.load(fh)
args = sys.argv[1:]
def opt(name):
    return args[args.index(name) + 1] if name in args else ""
src, build, gen = opt("-S"), opt("-B"), opt("-G")
key = gen or "<default>"
with open(plan["record"], "a", encoding="utf-8") as fh:
    fh.write(json.dumps({"gen": gen, "src": src, "build": build,
                         "fresh": os.path.isdir(build) and not os.listdir(build)}) + "\n")
how = plan["generators"].get(key, "fail")
print("fake-cmake: generator=%s behaviour=%s" % (key, how))
sys.stdout.flush()
if how == "ok":
    root = src.replace("\\", "/")
    entry = {"directory": build, "file": root + "/src/main.c",
             "arguments": ["cc", "-I" + root + "/include", "-DSCALE=2", "-c",
                           root + "/src/main.c"]}
    with open(os.path.join(build, "compile_commands.json"), "w", encoding="utf-8") as fh:
        json.dump([entry], fh)
    sys.exit(0)
if how == "rc0-no-file":
    sys.exit(0)
sys.stderr.write("fake-cmake: this generator refuses (%s)\n" % key)
sys.exit(1)
'''

# What the ladder arm's project must become. Written out, never derived through
# `emit_manifest`: an oracle computed by the code it judges agrees with any bug.
_LADDER_EXPECTED = (
    '{\n'
    '  "language": "c",\n'
    '  "artifactProfile": "cli",\n'
    '  "targets": [\n'
    '    "x86_64:elf64-x86_64-linux-exec"\n'
    '  ],\n'
    '  "artifactName": "my_proj",\n'
    '  "sources": [\n'
    '    "src/main.c"\n'
    '  ],\n'
    '  "includes": [\n'
    '    "include"\n'
    '  ],\n'
    '  "defines": [\n'
    '    "SCALE=2"\n'
    '  ]\n'
    '}\n')

# ...and what the direct-transform control's compile_commands.json must become: both
# entry forms, a quoted include holding a space, an escaped string define, a separated
# `-D`, an absolute `-isystem` outside the root, a relative `file`, a duplicate TU, and
# a repeated target.
_DIRECT_EXPECTED = (
    '{\n'
    '  "language": "c",\n'
    '  "artifactProfile": "cli",\n'
    '  "targets": [\n'
    '    "x86_64:elf64-x86_64-linux-exec",\n'
    '    "arm64:elf64-aarch64-linux-exec"\n'
    '  ],\n'
    '  "artifactName": "direct_root",\n'
    '  "sources": [\n'
    '    "src/a.c",\n'
    '    "src/b.c"\n'
    '  ],\n'
    '  "includes": [\n'
    '    "/opt/toolchain/include",\n'
    '    "include"\n'
    '  ],\n'
    '  "defines": [\n'
    '    "DEMO",\n'
    '    "MODE=fast",\n'
    '    "VER=\\"1.2.3\\""\n'
    '  ]\n'
    '}\n')

_ELF = "x86_64:elf64-x86_64-linux-exec"
_PE = "x86_64:pe64-x86_64-windows-exec"

# The contract the arms hold the program to, WRITTEN OUT rather than read from the
# constants they judge: a mutated PROG, SCRATCH_PREFIX or RULE would otherwise agree
# with itself. ✔MEASURED the class once already -- see `_arm_cleanup_after_exception`.
_NAME = "cmake-import.py"
_SCRATCH = ".dss-cmake-import-build."
_RULE64 = "-" * 64
# What `_project` puts in a root, so a leftover of ANY name is seen, not only one
# spelled with the scratch prefix.
_PROJECT_ENTRIES = ["CMakeLists.txt", "include", "src"]


class _ArmFailed(Exception):
    pass


def _check(cond, why):
    if not cond:
        raise _ArmFailed(why)


def _refusal_text(fn, *args):
    try:
        fn(*args)
    except Refusal as exc:
        return str(exc)
    raise _ArmFailed("%s%r did not refuse" % (fn.__name__, args))


def _expect_refusal(message, fn, *args):
    got = _refusal_text(fn, *args)
    _check(got == message, "%s%r refused with\n  %r\nexpected\n  %r"
           % (fn.__name__, args, got, message))


def _run_main(argv, cmake=None):
    """`main(argv)` with both streams captured -> (exit code, stdout, stderr)."""
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        rc = main(argv, cmake=cmake)
    return rc, out.getvalue(), err.getvalue()


def _write(path, text):
    with io.open(path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(text)


def _read_bytes(path):
    with open(path, "rb") as fh:
        return fh.read()


def _project(box, name):
    """A CMake project root holding what the stand-in names: src/main.c and include/."""
    root = os.path.join(box, name)
    os.makedirs(os.path.join(root, "src"))
    os.makedirs(os.path.join(root, "include"))
    _write(os.path.join(root, "CMakeLists.txt"), "project(p C)\nadd_executable(p src/main.c)\n")
    _write(os.path.join(root, "src", "main.c"), "int main(void) { return 0; }\n")
    return root


def _fake_cmake(box, name, generators):
    """-> (the stand-in's argv prefix, its record file)."""
    d = os.path.join(box, name)
    os.makedirs(d)
    record = os.path.join(d, "record.jsonl")
    _write(os.path.join(d, "fake-cmake.py"), _FAKE_CMAKE)
    _write(os.path.join(d, "plan.json"),
           json.dumps({"record": record, "generators": generators}))
    return [sys.executable, os.path.join(d, "fake-cmake.py")], record


def _calls(record):
    if not os.path.isfile(record):
        return []
    with io.open(record, encoding="utf-8") as fh:
        return [json.loads(line) for line in fh if line.strip()]


def _same(a, b):
    return os.path.normcase(os.path.abspath(a)) == os.path.normcase(os.path.abspath(b))


def _arm_unknown_flag(box):
    _expect_refusal("unknown flag '--bogus' (see --help)", parse_cli, ["--bogus", "r", "o"])
    _expect_refusal("unknown flag '-x' (see --help)", parse_cli, ["r", "o", "--target=t", "-x"])
    # The control: every flag, in both spellings, is accepted and lands where it says.
    o = parse_cli(["--target", "a:b", "--target=c:d", "--language", "cpp", "--profile=lib",
                   "--artifact-name", "app", "--compile-commands=cc.json", "r", "o"])
    got = (o.targets, o.language, o.profile, o.artifact, o.compile_commands, o.root, o.output)
    _check(got == (["a:b", "c:d"], "cpp", "lib", "app", "cc.json", "r", "o"),
           "the control command line parsed as %r" % (got,))


def _arm_abbreviation_and_case(box):
    _expect_refusal("unknown flag '--lang' (see --help)", parse_cli, ["--lang", "c", "r", "o"])
    _expect_refusal("unknown flag '--lang=c' (see --help)", parse_cli, ["--lang=c", "r", "o"])
    _expect_refusal("unknown flag '--Target' (see --help)", parse_cli, ["--Target", "x", "r", "o"])
    _expect_refusal("unknown flag '-H' (see --help)", parse_cli, ["-H"])
    _expect_refusal("unknown flag '--HELP' (see --help)", parse_cli, ["--HELP"])


def _arm_lone_dash(box):
    _expect_refusal("unknown flag '-' (see --help)", parse_cli, ["-", "o"])
    # The control: after `--` a leading dash is a positional, the lone one included.
    o = parse_cli(["--", "-", "--target"])
    _check((o.root, o.output, o.targets) == ("-", "--target", []),
           "`-- - --target` parsed as root=%r output=%r targets=%r"
           % (o.root, o.output, o.targets))


def _arm_positional_count(box):
    want = ("expected 2 positional arguments <root-cmake-dir> <output-project-file>; "
            "got %d (see --help)")
    _expect_refusal(want % 0, parse_cli, [])
    _expect_refusal(want % 1, parse_cli, ["r"])
    _expect_refusal("too many positional arguments (3); expected exactly <root-cmake-dir> "
                    "<output-project-file> (see --help)", parse_cli, ["a", "b", "c"])
    _expect_refusal("--self-test takes no other argument (see --help)",
                    parse_cli, ["--self-test", "r"])
    o = parse_cli(["r", "o"])
    _check((o.root, o.output) == ("r", "o"), "two positionals parsed as %r"
           % ((o.root, o.output),))


def _arm_value_required(box):
    _expect_refusal("--target requires a value", parse_cli, ["r", "o", "--target"])
    _expect_refusal("--artifact-name requires a value", parse_cli, ["--artifact-name"])
    _expect_refusal("--compile-commands requires a value",
                    parse_cli, ["r", "o", "--compile-commands"])


def _arm_empty_values(box):
    _expect_refusal("--language must be non-empty", parse_cli, ["--language=", "r", "o"])
    _expect_refusal("--profile must be non-empty", parse_cli, ["--profile", "", "r", "o"])
    _expect_refusal("--target must be non-empty", parse_cli, ["--target=", "r", "o"])
    _expect_refusal("--compile-commands must be non-empty",
                    parse_cli, ["--compile-commands=", "r", "o"])
    # The control: an EMPTY artifact name is "derive it from the root", as in both twins.
    o = parse_cli(["--artifact-name=", "r", "o"])
    _check(o.artifact == "", "--artifact-name= parsed as %r" % o.artifact)


def _arm_artifact_name(box):
    _expect_refusal("--artifact-name must be a bare file name (no '/' or '\\'): 'a/b'",
                    parse_cli, ["--artifact-name", "a/b", "r", "o"])
    _expect_refusal("--artifact-name must be a bare file name (no '/' or '\\'): 'a\\b'",
                    parse_cli, ["--artifact-name=a\\b", "r", "o"])
    o = parse_cli(["--artifact-name", "app.v2", "r", "o"])
    _check(o.artifact == "app.v2", "a bare artifact name parsed as %r" % o.artifact)


def _arm_help(box):
    for argv in (["-h"], ["--help"], ["r", "o", "-h"], ["--target", "t", "--help"]):
        rc, out, err = _run_main(argv)
        _check(rc == 0 and out == __doc__ and err == "",
               "main(%r) -> exit %d, %d bytes of stdout (help is %d), stderr %r"
               % (argv, rc, len(out), len(__doc__ or ""), err))
    _check("<root-cmake-dir> <output-project-file>" in (__doc__ or "")
           and "--self-test" in (__doc__ or ""), "the help text lost its usage lines")


def _arm_host_spec(box):
    cases = [
        (("Windows", {"PROCESSOR_ARCHITECTURE": "AMD64"}, None), _PE),
        (("MINGW64_NT-10.0-26200", {"PROCESSOR_ARCHITECTURE": "AMD64"}, None), _PE),
        # The OS view: a 32-bit interpreter on 64-bit Windows reads x86 for itself.
        (("Windows", {"PROCESSOR_ARCHITECTURE": "x86", "PROCESSOR_ARCHITEW6432": "AMD64"},
          None), _PE),
        (("Linux", {}, "x86_64"), _ELF),
        (("Linux", {}, "aarch64"), "arm64:elf64-aarch64-linux-exec"),
        (("Darwin", {}, "arm64"), "arm64:macho64-arm64-darwin-exec"),
        (("Darwin", {}, "x86_64"), "x86_64:macho64-x86_64-darwin-exec"),
    ]
    for (system, env, machine), want in cases:
        got = detect_host_spec(system, env, machine)
        _check(got == want, "detect_host_spec(%r, %r, %r) = %r, expected %r"
               % (system, env, machine, got, want))
    # ...and an x64 interpreter emulated on ARM64 Windows is refused -- never handed a
    # pe64 x86_64 target because its own view says AMD64.
    _expect_refusal("unsupported Windows architecture 'ARM64' — pass --target explicitly",
                    detect_host_spec, "Windows",
                    {"PROCESSOR_ARCHITECTURE": "AMD64", "PROCESSOR_ARCHITEW6432": "ARM64"})
    _expect_refusal("unsupported Linux architecture 'riscv64' — pass --target explicitly",
                    detect_host_spec, "Linux", {}, "riscv64")
    _expect_refusal("unrecognized host OS 'Plan9' — pass --target explicitly",
                    detect_host_spec, "Plan9", {}, "x86_64")


def _arm_missing_cmakelists(box):
    root = os.path.join(box, "no-cmakelists")
    os.makedirs(root)
    absent = os.path.join(box, "absent")
    _expect_refusal("root directory not found: '%s'" % absent,
                    validate_inputs, absent, False, None)
    _expect_refusal("no CMakeLists.txt in root directory: '%s'" % root,
                    validate_inputs, root, False, None)
    cmake, record = _fake_cmake(box, "fake", {"<default>": "ok"})
    out = os.path.join(box, "refused.dss-project.json")
    rc, so, se = _run_main([root, out, "--target", _ELF], cmake=cmake)
    _check(rc == 1 and so == "" and se == "%s: error: no CMakeLists.txt in root directory: "
           "'%s'\n" % (_NAME, root), "the refused run: exit %d, stdout %r, stderr %r"
           % (rc, so, se))
    _check(os.listdir(root) == [] and _calls(record) == [] and not os.path.exists(out),
           "a refused run left %r in the root, started cmake %d time(s), wrote=%s"
           % (os.listdir(root), len(_calls(record)), os.path.exists(out)))
    # The control: the same root with a CMakeLists.txt is accepted.
    _write(os.path.join(root, "CMakeLists.txt"), "project(p C)\n")
    got = validate_inputs(root, False, cmake)
    _check(_same(got[0], root) and got[1] == cmake, "validate_inputs -> %r" % (got,))


def _arm_bad_compile_commands(box):
    out = os.path.join(box, "t2.dss-project.json")
    args = (out, [_ELF], "c", "cli", "x", [box])
    missing = os.path.join(box, "missing.json")
    _expect_refusal("compile_commands.json not found: %s" % missing,
                    emit_manifest, missing, *args)
    garbled = os.path.join(box, "garbled.json")
    _write(garbled, "[{")
    got = _refusal_text(emit_manifest, garbled, *args)
    _check(got.startswith("could not read compile_commands.json: "),
           "unparsable JSON refused with %r" % got)
    obj = os.path.join(box, "object.json")
    _write(obj, '{"file": "a.c"}')
    _expect_refusal("compile_commands.json is not a JSON array", emit_manifest, obj, *args)
    _check(not os.path.exists(out), "a refused transform wrote %s" % out)
    rc, so, se = _run_main(["--compile-commands", obj, "--target", _ELF, box, out])
    _check(rc == 1 and so == "" and se == "%s: error: compile_commands.json is not a JSON "
           "array\n" % _NAME, "through the command line: exit %d, stdout %r, stderr %r"
           % (rc, so, se))


def _arm_no_sources(box):
    out = os.path.join(box, "t5.dss-project.json")
    for n, text in enumerate(("[]", '[{"directory": "/w", "command": "cc -c -DX=1"}]',
                              '["not an entry", 7]')):
        cc = os.path.join(box, "t5-%d.json" % n)
        _write(cc, text)
        _expect_refusal("no source files found in compile_commands.json (nothing to import)",
                        emit_manifest, cc, out, [_ELF], "c", "cli", "x", [box])
    _check(not os.path.exists(out), "a transform with no sources wrote %s" % out)


def _arm_direct_transform(box):
    root = os.path.join(box, "direct root")
    os.makedirs(root)                     # no CMakeLists.txt: a direct run configures nothing
    r = root.replace("\\", "/")
    entries = [
        {"directory": r + "/build", "file": "../src/b.c",
         "arguments": ["cc", "-I", "../include", "-isystem/opt/toolchain/include",
                       "-D", "MODE=fast", "-c", "../src/b.c"]},
        {"directory": r + "/build", "file": r + "/src/a.c",
         "command": 'cc -I"%s/include" -DVER=\\"1.2.3\\" -DDEMO -o a.o -c %s/src/a.c'
                    % (r, r)},
        {"directory": r + "/build", "file": r + "/src/a.c", "command": "cc -c %s/src/a.c" % r},
    ]
    cc = os.path.join(box, "compile_commands.json")
    _write(cc, json.dumps(entries))
    # The manifest is written AT THE ROOT, so its directory is the base and the bytes are
    # the ones the retired wrappers wrote for the same project.
    out = os.path.join(root, "direct.dss-project.json")
    cmake, record = _fake_cmake(box, "fake", {"<default>": "ok"})
    rc, so, se = _run_main(["--compile-commands", cc, "--target", _ELF, "--target",
                            "arm64:elf64-aarch64-linux-exec", "--target=" + _ELF, root, out],
                           cmake=cmake)
    _check(rc == 0 and se == "", "exit %d, stderr %r" % (rc, se))
    got = _read_bytes(out)
    _check(got == _DIRECT_EXPECTED.encode("utf-8"),
           "the manifest bytes differ from the written-out expectation:\n%s"
           % got.decode("utf-8", "replace"))
    _check(so.startswith("cmake-import: 2 sources, 2 includes, 3 defines -> %s\nnote: "
                         % out), "the summary reads %r" % so)
    _check(_calls(record) == [] and os.listdir(root) == ["direct.dss-project.json"],
           "a --compile-commands run started cmake %d time(s) / left %r in the root"
           % (len(_calls(record)), os.listdir(root)))


def _arm_root_spelling(box):
    """The spelling of the root and of the manifest's directory (the ✔MEASURED wrong-case
    defect): respelled in case only, never through a link, and a path under EITHER
    spelling of the manifest's directory is emitted relative."""
    real = os.path.join(box, "CaseRoot")
    os.makedirs(os.path.join(real, "src"))
    _check(_filesystem_spelling(real) == real,
           "a correctly spelled root was respelled: %r" % _filesystem_spelling(real))
    checked = []
    link = os.path.join(box, "LinkRoot")
    try:
        os.symlink(real, link, target_is_directory=True)
    except (OSError, NotImplementedError, AttributeError):
        link = None                          # Windows without the symlink privilege
    if link is not None:
        checked.append("link")
        _check(_filesystem_spelling(link) == link,
               "the symlinked root %r was resolved to %r -- CMake keeps a link's "
               "spelling, so every path would come out absolute"
               % (link, _filesystem_spelling(link)))
    wrong = os.path.join(box, "caseroot")
    if os.path.normcase("A") == os.path.normcase("a") and os.path.isdir(wrong):
        checked.append("case")
        _check(_filesystem_spelling(wrong) == real,
               "the wrong-case root %r was kept as typed (%r), not respelled as stored"
               % (wrong, _filesystem_spelling(wrong)))
        # End to end: one source in the stored spelling (what CMake writes), one in the
        # typed spelling (what a tool copying its caller's cwd writes). Both are the root.
        entries = [
            {"directory": real, "file": real.replace("\\", "/") + "/src/a.c",
             "command": "cc -c a.c"},
            {"directory": wrong, "file": wrong.replace("\\", "/") + "/src/b.c",
             "command": "cc -c b.c"},
        ]
        cc = os.path.join(box, "compile_commands.json")
        _write(cc, json.dumps(entries))
        # Written INTO the root as typed, so the manifest's directory has both spellings.
        out = os.path.join(wrong, "case.dss-project.json")
        rc, so, se = _run_main(["--compile-commands", cc, "--target", _ELF, wrong, out])
        _check(rc == 0, "exit %d, stderr %r" % (rc, se))
        manifest = json.loads(_read_bytes(out).decode("utf-8"))
        _check(manifest.get("sources") == ["src/a.c", "src/b.c"]
               and manifest.get("artifactName") == "CaseRoot",
               "the root typed as %r produced %r" % (wrong, manifest))
    _check(checked, "this host offers neither a symlink nor a case-folding filesystem, "
           "so nothing about the root's spelling was checked")


def _arm_scratch_collision(box):
    root = os.path.join(box, "collide")
    taken = os.path.join(root, _SCRATCH + "taken0")
    os.makedirs(taken)
    sentinel = os.path.join(taken, "another-run-in-flight.txt")
    _write(sentinel, "not yours\n")
    if not hasattr(tempfile, "_get_candidate_names"):
        raise _ArmFailed("this Python's tempfile has no _get_candidate_names, so the "
                         "collision cannot be forced here -- rewrite the arm, never drop it")
    drawn = []

    def names():
        for name in ("taken0", "fresh0"):
            drawn.append(name)
            yield name

    real = tempfile._get_candidate_names
    tempfile._get_candidate_names = names
    try:
        got = make_scratch(root)
    finally:
        tempfile._get_candidate_names = real
    _check(drawn == ["taken0", "fresh0"],
           "make_scratch drew %r: the taken name was never tried, so this arm proved "
           "nothing (does make_scratch still create through tempfile.mkdtemp?)" % drawn)
    _check(_same(got, os.path.join(root, _SCRATCH + "fresh0")),
           "make_scratch returned %s, expected the name AFTER the taken one" % got)
    _check(os.listdir(taken) == ["another-run-in-flight.txt"] and os.path.isfile(sentinel),
           "the pre-existing directory was reused or touched: %r" % os.listdir(taken))
    _check(os.path.isdir(got) and os.listdir(got) == [],
           "the new scratch root is not a fresh empty directory")


def _arm_cleanup_after_exception(box):
    root = _project(box, "cleanup")
    cmake, record = _fake_cmake(box, "fake", {"<default>": "ok"})
    # The signals are NAMED HERE, never read from TRAPPED_SIGNALS: an arm whose cases
    # came from the constant it judges would run no signal case at all once that
    # constant was emptied -- ✔MEASURED, that mutant passed this arm until it said so.
    must_trap = ("SIGTERM", "SIGHUP")
    trapped = _signals(must_trap)
    before = dict((sig, signal.getsignal(sig)) for sig in trapped)
    cases = [("RuntimeError", "raised"), ("KeyboardInterrupt", 130)]
    cases += [(name, 143) for name in must_trap if hasattr(signal, name)]
    _check(any(name == "SIGTERM" for name, _ in cases),
           "this host has no SIGTERM, so the signal half of the cleanup went unproved")
    real = globals()["configure"]
    for label, want in cases:
        seen = []

        def crash(cmake_argv, root_abs, scratch, label=label):
            seen.append(scratch)
            _write(os.path.join(scratch, "half-configured.txt"), "partial\n")
            if label == "RuntimeError":
                raise RuntimeError("a synthetic crash")
            if label == "KeyboardInterrupt":
                raise KeyboardInterrupt()
            sig = getattr(signal, label)
            if signal.getsignal(sig) is not _on_terminate:
                raise _ArmFailed("main() holds a scratch root with %s at %r: that signal "
                                 "would end the run and leak the directory"
                                 % (label, signal.getsignal(sig)))
            signal.raise_signal(sig)
            raise _ArmFailed("%s was raised and the run carried on" % label)

        globals()["configure"] = crash
        try:
            try:
                rc = _run_main([root, os.path.join(box, "never.json"), "--target", _ELF],
                               cmake=cmake)[0]
            except RuntimeError:
                rc = "raised"
            except (KeyboardInterrupt, _Terminated) as exc:
                raise _ArmFailed("main() let %s escape instead of returning its exit code"
                                 % type(exc).__name__)
        finally:
            globals()["configure"] = real
        _check(rc == want, "%s: main() -> %r, expected %r" % (label, rc, want))
        _check(len(seen) == 1 and not os.path.exists(seen[0])
               and sorted(os.listdir(root)) == _PROJECT_ENTRIES,
               "%s: the scratch root survived: %r" % (label, sorted(os.listdir(root))))
        after = dict((sig, signal.getsignal(sig)) for sig in trapped)
        _check(after == before, "%s: main() did not restore the signal handlers it replaced "
               "(%r, was %r)" % (label, after, before))

    # A scratch root that CANNOT be removed is named on stderr, never left in silence.
    real_remove = globals()["_remove_tree"]
    globals()["_remove_tree"] = lambda path: False
    try:
        out = os.path.join(box, "kept.dss-project.json")
        rc, so, se = _run_main([root, out, "--target", _ELF], cmake=cmake)
    finally:
        globals()["_remove_tree"] = real_remove
    kept = [n for n in os.listdir(root) if n not in _PROJECT_ENTRIES]
    try:
        _check(rc == 0 and len(kept) == 1, "the unremovable-scratch run: exit %d, left %r"
               % (rc, kept))
        scratch = os.path.join(root, kept[0])
        _check(se.endswith("%s: warning: could not remove the scratch build directory '%s' "
                           "-- remove it by hand\n" % (_NAME, scratch)),
               "a scratch root that could not be removed was not named on stderr: %r" % se)
    finally:
        for n in kept:
            real_remove(os.path.join(root, n))


def _arm_generator_ladder(box):
    root = _project(box, "my proj")
    cmake, record = _fake_cmake(box, "fake", {"<default>": "rc0-no-file", "Ninja": "ok",
                                              "Unix Makefiles": "ok"})
    out = os.path.join(root, "ladder.dss-project.json")      # at the root: relative paths
    rc, so, se = _run_main(["--target", _ELF, "--target=" + _ELF, root, out], cmake=cmake)
    calls = _calls(record)
    _check(rc == 0, "exit %d; stderr:\n%s" % (rc, se))
    _check([c["gen"] for c in calls] == ["", "Ninja"],
           "generators tried: %r -- expected the default, then Ninja, and nothing after "
           "(a default that exits 0 WITHOUT compile_commands.json is not a success)"
           % [c["gen"] for c in calls])
    scratch = os.path.dirname(calls[0]["build"])
    _check([os.path.basename(c["build"]) for c in calls] == ["attempt1", "attempt2"]
           and all(c["fresh"] for c in calls)
           and all(_same(os.path.dirname(c["build"]), scratch) for c in calls),
           "the attempts did not each get a fresh directory of ONE scratch root: %r" % calls)
    _check(_same(os.path.dirname(scratch), root)
           and os.path.basename(scratch).startswith(_SCRATCH)
           and all(_same(c["src"], root) for c in calls),
           "cmake was not configuring the project root from a %s* scratch in it: %r"
           % (_SCRATCH, calls))
    got = _read_bytes(out)
    _check(got == _LADDER_EXPECTED.encode("utf-8"),
           "the manifest bytes differ from the written-out expectation:\n%s"
           % got.decode("utf-8", "replace"))
    _check(so.startswith("cmake-import: 1 sources, 1 includes, 1 defines -> %s\n" % out),
           "the summary reads %r" % so)
    _check(se == "%s: scratch build dir: %s\n" % (_NAME, scratch),
           "stderr did not name the scratch root once and only that: %r" % se)
    _check(not os.path.exists(scratch)
           and sorted(os.listdir(root)) == sorted(_PROJECT_ENTRIES + ["ladder.dss-project.json"]),
           "the scratch root survived a successful run: %r" % sorted(os.listdir(root)))


def _arm_ladder_exhausted(box):
    root = _project(box, "exhausted")
    cmake, record = _fake_cmake(box, "fake", {"<default>": "rc0-no-file", "Ninja": "fail",
                                              "Unix Makefiles": "fail"})
    out = os.path.join(box, "exhausted.dss-project.json")
    rc, so, se = _run_main([root, out, "--target", _ELF], cmake=cmake)
    calls = _calls(record)
    _check(rc == 1, "exit %d" % rc)
    _check([c["gen"] for c in calls] == ["", "Ninja", "Unix Makefiles"],
           "generators tried: %r" % [c["gen"] for c in calls])
    scratch = os.path.dirname(calls[0]["build"])
    want = ("%s: scratch build dir: %s\n"
            "%s: error: CMake did not produce compile_commands.json.\n"
            "Tried the default generator, Ninja, and Unix Makefiles.\n"
            "Use a generator that supports CMAKE_EXPORT_COMPILE_COMMANDS\n"
            "(Ninja or Unix Makefiles). Last CMake output:\n"
            "%s\n"
            "fake-cmake: generator=Unix Makefiles behaviour=fail\n"
            "fake-cmake: this generator refuses (Unix Makefiles)\n"
            "%s\n" % (_NAME, scratch, _NAME, _RULE64, _RULE64))
    _check(se == want, "stderr is not the wrappers' failure block around the LAST "
           "attempt's log:\n%s" % se)
    _check(so == "" and not os.path.exists(out), "a failed configure wrote %r / %s"
           % (so, out))
    _check(not os.path.exists(scratch) and sorted(os.listdir(root)) == _PROJECT_ENTRIES,
           "the scratch root survived a refused run: %r" % sorted(os.listdir(root)))


def _arm_manifest_directory_is_the_base(box):
    """★ The base is the MANIFEST's directory. At the root every project path is relative
    (the control: the old bytes); in a subdirectory of the root only what lies under that
    subdirectory is; outside the root EVERY path is absolute -- the root-relative names
    this program wrote until 2026-09-21 would resolve, beside that manifest, to nothing.
    Never a `..`, and a path on another drive is absolute wherever the manifest is."""
    root = os.path.join(box, "proj")
    os.makedirs(os.path.join(root, "dss"))
    os.makedirs(os.path.join(box, "out", "deep"))
    r = root.replace("\\", "/")
    entries = [{"directory": r + "/build", "file": r + "/src/main.c",
                "arguments": ["cc", "-I" + r, "-I" + r + "/include", "-I" + r + "/dss/gen",
                              "-isystem", "/opt/toolchain/include", "-IZ:/elsewhere", "-DX",
                              "-c", r + "/src/main.c"]}]
    cc = os.path.join(box, "compile_commands.json")
    _write(cc, json.dumps(entries))
    foreign = ["/opt/toolchain/include", "Z:/elsewhere"]
    cases = [
        ("at the root", os.path.join(root, "at-root.dss-project.json"),
         ["src/main.c"], [".", "/opt/toolchain/include", "Z:/elsewhere", "dss/gen", "include"]),
        ("in a subdirectory of the root", os.path.join(root, "dss", "in-sub.dss-project.json"),
         [r + "/src/main.c"], sorted(foreign + [r, r + "/include", "gen"])),
        ("outside the root", os.path.join(box, "out", "deep", "outside.dss-project.json"),
         [r + "/src/main.c"], sorted(foreign + [r, r + "/include", r + "/dss/gen"])),
    ]
    for label, out, want_src, want_inc in cases:
        rc, _so, se = _run_main(["--compile-commands", cc, "--target", _ELF, root, out])
        _check(rc == 0, "%s: exit %d, stderr %r" % (label, rc, se))
        m = json.loads(_read_bytes(out).decode("utf-8"))
        got = (m.get("sources"), m.get("includes"))
        _check(got == (want_src, want_inc), "the manifest %s wrote sources/includes\n  %r\n"
               "expected\n  %r" % (label, got, (want_src, want_inc)))
        _check(not any(".." in p.split("/") for p in got[0] + got[1]),
               "the manifest %s wrote a `..` path: %r" % (label, got))
    # The synthesized NEGATIVE, spelled out: under the old base (the ROOT) the manifest
    # outside the root would have named `src/main.c` -- resolved beside it, nothing.
    _check("src/main.c" not in got[0], "the manifest outside the root still names its "
           "source relative to the ROOT: %r" % (got[0],))


def _arm_prove_any_cwd_usage(box):
    _expect_refusal("--prove-any-cwd requires a value", parse_cli, ["--prove-any-cwd"])
    _expect_refusal("--prove-any-cwd takes no other argument (see --help)",
                    parse_cli, ["--prove-any-cwd", "d", "r"])
    _expect_refusal("--prove-any-cwd takes no other argument (see --help)",
                    parse_cli, ["r", "o", "--prove-any-cwd=d"])
    _expect_refusal("--prove-any-cwd must be non-empty", parse_cli, ["--prove-any-cwd="])
    _expect_refusal("--self-test takes no other argument (see --help)",
                    parse_cli, ["--self-test", "--prove-any-cwd", "d"])
    # The control: both spellings are accepted alone.
    for argv in (["--prove-any-cwd", "d"], ["--prove-any-cwd=d"]):
        o = parse_cli(argv)
        _check((o.prove_any_cwd, o.self_test, o.root) == ("d", False, None),
               "%r parsed as prove=%r self_test=%r root=%r"
               % (argv, o.prove_any_cwd, o.self_test, o.root))
    # A dsscp that is not there is refused BY NAME before anything is built.
    absent = os.path.join(box, "no-such-dsscp")
    rc, so, se = _run_main(["--prove-any-cwd", absent])
    _check(rc == 1 and so == "" and se == "%s: error: --prove-any-cwd: no dsscp at '%s'\n"
           % (_NAME, absent), "an absent dsscp: exit %d, stdout %r, stderr %r" % (rc, so, se))


_ARMS = (
    ("unknown-flag", _arm_unknown_flag),
    ("abbreviation-and-case", _arm_abbreviation_and_case),
    ("lone-dash", _arm_lone_dash),
    ("positional-count", _arm_positional_count),
    ("value-required", _arm_value_required),
    ("empty-values", _arm_empty_values),
    ("artifact-name", _arm_artifact_name),
    ("help", _arm_help),
    ("host-spec-os-view", _arm_host_spec),
    ("missing-cmakelists", _arm_missing_cmakelists),
    ("bad-compile-commands", _arm_bad_compile_commands),
    ("no-sources", _arm_no_sources),
    ("direct-transform-control", _arm_direct_transform),
    ("root-spelling", _arm_root_spelling),
    ("scratch-collision-never-reused", _arm_scratch_collision),
    ("cleanup-after-exception", _arm_cleanup_after_exception),
    ("generator-ladder", _arm_generator_ladder),
    ("ladder-exhausted", _arm_ladder_exhausted),
    ("manifest-directory-is-the-base", _arm_manifest_directory_is_the_base),
    ("prove-any-cwd-usage", _arm_prove_any_cwd_usage),
)


def self_test():
    """Run every arm in its own directory of one temp box. 0 only if all EXPECTED_ARMS pass.

    The box is taken through `realpath` once, so every path an arm builds is already
    spelled as stored and no arm depends on how the host spells its temp directory.
    """
    box = os.path.realpath(tempfile.mkdtemp(prefix="cmake-import-selftest-"))
    failed = []
    ran = 0
    try:
        for n, (name, arm) in enumerate(_ARMS, 1):
            ran += 1
            sub = os.path.join(box, "arm%02d" % n)
            os.makedirs(sub)
            try:
                arm(sub)
            except Exception as exc:
                failed.append(name)
                why = str(exc) if isinstance(exc, _ArmFailed) else "%s: %s" % (
                    type(exc).__name__, exc)
                sys.stdout.write("  FAIL %2d %s\n        %s\n"
                                 % (n, name, why.replace("\n", "\n        ")))
            else:
                sys.stdout.write("  ok   %2d %s\n" % (n, name))
    finally:
        if not _remove_tree(box):
            sys.stderr.write("%s: warning: could not remove the self-test box '%s'\n"
                             % (PROG, box))
    if ran != EXPECTED_ARMS:
        sys.stdout.write("cmake-import: self-test FAILED -- %d arm(s) ran, EXPECTED_ARMS "
                         "says %d\n" % (ran, EXPECTED_ARMS))
        return 1
    if failed:
        sys.stdout.write("cmake-import: self-test FAILED -- %d of %d arm(s): %s\n"
                         % (len(failed), ran, ", ".join(failed)))
        return 1
    sys.stdout.write("cmake-import: self-test OK -- %d/%d arms\n" % (ran, EXPECTED_ARMS))
    return 0


if __name__ == "__main__":
    sys.exit(main())
