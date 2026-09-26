# PURPOSE: compile, and optionally run or dump, one probe with the reference compiler of the host a leg runs on, and keep the verdict, the diagnostics and the result redacted, so a reference is MEASURED on a host no session reaches directly.
"""probe-reference-cc.py -- measure a REFERENCE compiler on the host a leg runs on.

WHY IT EXISTS (2026-09-23, P68 round 9): the bar measures every reference compiler separately, per
target, and a Mach-O row's reference is Apple's clang with the SDK's own headers -- a toolchain that
exists only on the Mac. DssHarness reaches a leg's host only through a runner, and no runner compiled a
probe there, so lanes took Mach-O values as DOCUMENTED (Apple's published headers) where the bar asks
for MEASURED.

THE PROBE: `--src-b64`, the probe's source text in base64 (standard or URL-safe alphabet, padding
optional). It is carried as an input, not as a file, for two reasons: a runner input cannot hold a
space or a newline, and a probe file placed in the tree would have to be synced there, which
overwrites that host's ONE copy of the repository (`dssharness sync` keeps one per host) under
whatever lane is using it.

MODES (`--mode`):
  run      compile and link, then run the program: its stdout, stderr and exit code (the default);
  link     compile and link, without running;
  object   compile to an object (`-c`), then run the `--dump` tool on it when one is named;
  asm      compile to assembly (`-S`), and keep the assembly the compiler wrote;
  stdout   run the compiler with no output file added, and keep what it prints (`-E`, `-dM,-E`).

THE COMPILER: `--cc`, a bare program name looked up on the host's PATH (default `cc`), never a path,
so the probe measures the host's own toolchain. Its identity is recorded first: where PATH resolves
it (and the link's target), and the first lines of what it says it is (`--version`, or MSVC's banner).

THE COMMAND-LINE DIALECT: a compiler speaks either gcc's command line (`gnu`: `-c`, `-S`, `-o <path>`)
or MSVC's (`msvc`: `/c`, `/FA`, `/Fe<path>` `/Fo<path>` `/Fa<path>`). DIALECT_OF names the compilers that speak
MSVC's (`cl`, `clang-cl`); every other name speaks gcc's. `--dialect` overrides it for a driver under
another name. The msvc spellings, each MSVC's documented option: run/link = `cl /nologo ... probe.c
/Feprobe.exe`; object = `/c ... /Foprobe.obj`; asm = `/c /FA ... /Faprobe.asm /Foprobe.obj` (an
assembly-only listing); stdout = the caller's `/E` or `/EP`. The IDENTITY is the program's, not the
dialect's: cl has no `--version`, so it is run with no arguments and its banner is kept (BANNER_ONLY);
clang-cl answers `--version`. cl needs the Visual Studio developer environment, which DssHarness sets up itself
for a leg whose toolchain declares it (the msvc toolchain's `developerEnvironment`, the windows-x86_64-release
leg) -- ✔MEASURED 2026-09-24: `dssharness run` on that leg enters it, and cl 19.51 resolves on PATH.

FLAGS: `--flags`, a comma-separated list; each item is percent-decoded, so a comma or a space inside
ONE flag is written `%2C` or `%20` (`-Wl%2C-z%2Cnoexecstack`). An option that places a product this
program reads is refused, because this program owns those paths: gnu's `-o`; msvc's `/Fe` `/Fo` `/Fa`
and its deprecated `/o` (either prefix, `/` or `-`), and a linker `/OUT:`. MSVC's `/FA` (capital A, a
listing's TYPE) places nothing and is allowed, as are `/openmp` and `/options:strict`. An msvc `/link`
tail is moved AFTER the source and the output paths, where MSVC's /link reference requires it.

DUMP (object, link and run modes): `--dump`, a comma-separated argv, percent-decoded like the flags, whose
first item is one of DUMP_TOOLS; the PRODUCT's path is appended as its last argument -- the object in object
mode, the LINKED IMAGE in link and run modes (after the run, in run mode). ✔ADDED 2026-09-24: lane xa had to
measure Apple ld's output with probes that read their own image, because only an object could be dumped.

REFUSED by name, exit 2, before anything is compiled: an undecodable source, an EMPTY one outside stdout
mode, a decoded source over MAX_SOURCE_BYTES, a compiler or dump tool spelled as a path, a dump tool
outside DUMP_TOOLS, a `--dump` in asm or stdout mode (there is no product to dump), an option placing a
product among the flags, an unknown mode, extension or dialect, and an assembly source (`s`, `S`) for the
msvc dialect (cl does not assemble). ★ AN EMPTY SOURCE IS A PROBE IN STDOUT MODE: an empty translation unit under
`-dM,-E` prints exactly the compiler's predefined macros. A single probe cannot carry one -- an empty
`--src-b64` is no source at all -- so it travels as a batch item's `"src": ""` (or a shared source
`""`), and an item that is empty in any other mode is refused as that item.
A compiler or dump tool that PATH does not find is NOT refused: "this host has no clang" is a
measurement of the host, reported as `compiler=absent` / `dump=absent` (a refused step keeps no
report, so a refusal would leave only "probe exited 2" on the orchestrating side).

REDACTION: every line printed or kept passes through read-leg-path's redactor -- the ONE owner of
that rule, loaded by path from its sibling directory -- which turns the tree into `<tree>`, the home
directory into `~`, the user into `<user>` and the host's name into `<host>`; this program adds its
own scratch directory, as `<probe>`, in both of the spellings a symbolic link can give it
(`/var/...` and `/private/var/...` on macOS).

THE VERDICT: a probe that fails to COMPILE is still a measurement, and the report says so; this
program's exit code says only whether it could measure (0) or refused (2). The report is the
compiler's identity, each command with its exit code and output, then the witness line
`probe-reference-cc: OK mode=<mode> compile=<rc>[ run=<rc>][ dump=<rc>]` -- on stdout and in `--out`,
which the runner keeps, so `dssharness sync --pull` brings it back.

A BATCH (`--batch-b64`, in place of `--src-b64`): the base64 of a UTF-8 JSON array of up to MAX_BATCH
objects, each `{"name", "src"}` plus any of `ext`, `cc`, `flags`, `mode`, `dump`, `dialect` (strings, the same
rules as the options). Every item runs in turn inside ONE run, so ONE sync and ONE pull serve them all
-- MEASURED 2026-09-23: the Mac fell asleep between two single-probe runs a minute apart, and its
keep-awake lives only as long as a run. Each item's report follows a `=== probe <name>` line; an item
refused for a malformed field is reported there and the batch goes on; a malformed BATCH (not base64,
not a JSON array, an unknown key, a name twice) is refused whole. SHARED SOURCES: the batch may instead
be an OBJECT `{"sources": {"<key>": "<text>", ...}, "items": [...]}` whose items name a source with
`src_ref` in place of `src` -- one program measured under many flag sets (arches x optimisation
levels) is then carried ONCE, not once per item (MEASURED 2026-09-23: four programs x four variants
repeated the text sixteen times, past the Windows command line). The last line is
`probe-reference-cc: OK batch=<n> measured=<m> refused=<r>`. The batch's base64 is bounded by
MAX_BATCH_B64, because a run's inputs reach the orchestrating host's DssHarness on one Windows
command line (32767 characters).

`--selftest` runs the refusal, decoding, batch and redaction arms and each dialect's spelling of every
mode as a pure argv, then the REAL arms: with the host's `cc`, `gcc` or `clang` and its `nm` or
`llvm-nm` everywhere, and with `cl` and `dumpbin` inside a Visual Studio developer environment
(`VCToolsInstallDir` set). A real arm's tool that PATH does not find where it is expected is a named
FAILURE, never a skip; outside a developer environment the cl arms say, by name, that they are not
that host's. The same self-test runs as this action's manual step on any leg the runner declares:
`dssharness run probe-reference-cc --legs <leg> --manual-step self-test`.
"""
import argparse
import base64
import binascii
import importlib.util
import io
import json
import os
import re
import shutil
import subprocess
import sys
sys.dont_write_bytecode = True  # this program loads read-leg-path.py by path; no __pycache__ beside a sibling action
import tempfile
from urllib.parse import unquote

# What this program prints is a compiler's and a probe's output, so any character can reach the pipe: both streams
# are UTF-8 from IMPORT on (argument errors and --help print before main()), the repository's rule for every action.
for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError, OSError):
        pass

MODES = ("run", "link", "object", "asm", "stdout")
EXTENSIONS = ("c", "s", "S", "cpp")
DUMP_TOOLS = ("objdump", "llvm-objdump", "otool", "nm", "llvm-nm", "readelf", "llvm-readelf", "size", "llvm-size",
              "dumpbin")
DUMP_MODES = ("object", "link", "run")   # the modes that leave a product to dump: an object, or the linked image
# The command-line dialect each compiler speaks: MSVC's for these names, gcc's for every other.
DIALECTS = ("gnu", "msvc")
DIALECT_OF = {"cl": "msvc", "clang-cl": "msvc"}
# The extensions each dialect compiles: cl compiles C and C++ and does not assemble (ml64 is MSVC's assembler).
DIALECT_EXTENSIONS = {"gnu": EXTENSIONS, "msvc": ("c", "cpp")}
# The compilers with no `--version`: run with no arguments, cl prints its banner and a usage line.
BANNER_ONLY = ("cl",)
# The msvc options that place a product this program reads, either prefix: /Fe the image, /Fo the object, /Fa the
# listing (a lowercase `a`: /FA, capital, is the listing's TYPE); cl's DEPRECATED /o, which its reference no longer
# lists but cl still honours (✔MEASURED 2026-09-25, cl 19.51.36260: `cl probe.c -o probe.exe` warns D9035, exits 0
# and links the image at that path) -- every documented cl option that begins with a lowercase o is /openmp or
# /options:, so those two stay allowed; and the linker's /OUT: (linker options are case-insensitive).
MSVC_PRODUCT_PATH = re.compile(r"^[/-](F[eoa]|o(?!penmp|ptions:))")
MSVC_LINK_OUT = re.compile(r"^[/-]OUT:", re.IGNORECASE)
MAX_SOURCE_BYTES = 256 * 1024
MAX_STREAM_CHARS = 200000
BARE_NAME = re.compile(r"^[A-Za-z0-9_.+-]+$")
COMPILE_SECONDS = 180
RUN_SECONDS = 60
MAX_BATCH = 32
MAX_BATCH_B64 = 24000
ITEM_NAME = re.compile(r"^[A-Za-z0-9_.-]{1,64}$")
ITEM_KEYS = ("name", "src", "ext", "cc", "flags", "mode", "dump", "dialect", "src_ref")


class Refusal(Exception):
    pass


def _read_leg_path():
    """`.harness-config/runner/actions/read-leg-path/read-leg-path.py` -- the one owner of the redaction rule.

    Loaded by path from this file's sibling directory (a hyphen is not a module name). It FAILS LOUD when
    absent rather than falling back to a local copy: a second spelling of what must never leave a host is
    the drift one owner exists to end.
    """
    path = os.path.join(os.path.dirname(os.path.dirname(os.path.realpath(__file__))),
                        "read-leg-path", "read-leg-path.py")
    if not os.path.isfile(path):
        raise SystemExit("probe-reference-cc: cannot find %s -- the redaction rule lives there and nowhere else"
                         % path)
    spec = importlib.util.spec_from_file_location("dss_read_leg_path", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def redactor(tree, scratch):
    base = _read_leg_path().redactor(tree)
    spellings = sorted({scratch, os.path.realpath(scratch)}, key=len, reverse=True)

    def apply(text):
        for s in spellings:  # the scratch sits outside the tree, under the system temp directory
            if s and len(s) >= 3:
                text = text.replace(s, "<probe>").replace(s.replace("\\", "/"), "<probe>")
        return base(text)
    return apply


def decode_source(b64, allow_empty=False):
    """The probe's bytes. EMPTY is a source only where `allow_empty` says so: stdout mode, where an empty
    translation unit is the predefined-macro probe (`-dM,-E`); anywhere else it is a refusal."""
    if not b64:
        if allow_empty:
            return b""
        raise Refusal("the source is empty: an empty translation unit is a probe only in stdout mode "
                      "(-dM,-E)")
    text = b64.strip().replace("-", "+").replace("_", "/")  # the URL-safe alphabet, strictly, as the standard one
    padded = text + "=" * (-len(text) % 4)
    try:
        data = base64.b64decode(padded.encode("ascii"), validate=True)
    except (binascii.Error, ValueError, UnicodeEncodeError) as e:
        raise Refusal("--src-b64 is not base64: %s" % e)
    if not data and not allow_empty:
        raise Refusal("--src-b64 decodes to an empty source")
    if len(data) > MAX_SOURCE_BYTES:
        raise Refusal("the decoded source is %d bytes, over the %d-byte limit" % (len(data), MAX_SOURCE_BYTES))
    return data


def split_list(value):
    """A comma-separated list, each item percent-decoded; empty items dropped."""
    return [unquote(item) for item in (value or "").split(",") if item != ""]


def program_key(cc):
    """The name a compiler is KNOWN by in DIALECT_OF and BANNER_ONLY: on Windows a program name is case-insensitive
    and PATHEXT supplies `.exe`, so `CL` and `cl.exe` there are the same `cl`; and a distribution's version suffix
    names the same driver (✔MEASURED 2026-09-25: the WSL host's /usr/bin has `clang-cl-19` and no `clang-cl`)."""
    key = os.path.normcase(cc)
    if os.name == "nt" and key.endswith(".exe"):
        key = key[:-4]
    return re.sub(r"-[0-9]+(\.[0-9]+)*$", "", key)


def check_flags(flags, dialect="gnu"):
    """REFUSE an option that places a product this program reads -- the object, the assembly, the linked image --
    because this program owns those paths: gnu's `-o`; msvc's `/Fe` `/Fo` `/Fa`, its deprecated `/o`, and a linker
    `/OUT:`."""
    for f in flags:
        if dialect == "msvc":
            places_a_product = bool(MSVC_PRODUCT_PATH.match(f) or MSVC_LINK_OUT.match(f))
        else:
            places_a_product = f == "-o" or (f.startswith("-o") and not f.startswith("-objc"))
        if places_a_product:
            raise Refusal("--flags names %r; this program owns every output path" % f)


def dialect_for(cc, override=""):
    """The command-line dialect `cc` speaks: `override` when given (REFUSED unless one of DIALECTS), else
    DIALECT_OF's entry for the compiler's name, else gnu."""
    if override:
        if override not in DIALECTS:
            raise Refusal("--dialect %r is not one of %s" % (override, ", ".join(DIALECTS)))
        return override
    return DIALECT_OF.get(program_key(cc), "gnu")


def identity_argv(cc, cc_path):
    """What asks the compiler what it is: `--version`, except for a compiler in BANNER_ONLY, which is run with no
    arguments at all and answers with its banner. A property of the PROGRAM, not of its dialect: clang-cl speaks
    MSVC's command line and still answers `--version`."""
    return [cc_path] if program_key(cc) in BANNER_ONLY else [cc_path, "--version"]


def split_link(flags):
    """msvc: (the compiler's flags, the `/link` tail). MSVC's `/link` "and its linker options must appear after
    any file names and CL options" (MSVC's /link reference), so the tail goes LAST, after the source and the
    output paths; gnu has no such tail."""
    for i, f in enumerate(flags):
        if f in ("/link", "-link"):
            return flags[:i], flags[i:]
    return flags, []


def compile_argv(dialect, cc, cc_path, mode, flags, src, scratch):
    """(argv, the argv as the report shows it, the product's path or None) for one compile. A PURE function of its
    inputs, so the self-test pins every dialect's spelling of every mode without a compiler.
    msvc spells an output path `/Fe<path>` `/Fo<path>` `/Fa<path>`, with no colon and no space: the one form MSVC's
    reference documents for all three (`/Fe:` and `/Fo:` also exist; `/Fa:` is not documented)."""
    base = os.path.basename(src)
    if dialect == "msvc":
        head, link = split_link(flags)
        outputs = {"asm": [("/Fa", "probe.asm"), ("/Fo", "probe.obj")], "object": [("/Fo", "probe.obj")],
                   "stdout": []}.get(mode, [("/Fe", "probe.exe")])
        stage = {"asm": ["/c", "/FA"], "object": ["/c"]}.get(mode, [])
        argv = ([cc_path, "/nologo"] + stage + head + [src] + [opt + os.path.join(scratch, name)
                                                                for opt, name in outputs] + link)
        shown = [cc, "/nologo"] + stage + head + [base] + [opt + name for opt, name in outputs] + link
        return argv, shown, os.path.join(scratch, outputs[0][1]) if outputs else None
    if mode == "stdout":
        return [cc_path] + flags + [src], [cc] + flags + [base], None
    product = os.path.join(scratch, {"object": "probe.o", "asm": "probe.s"}.get(mode, "probe.exe"))
    stage = {"object": ["-c"], "asm": ["-S"]}.get(mode, [])
    return ([cc_path] + stage + flags + [src, "-o", product],
            [cc] + stage + flags + [base, "-o", os.path.basename(product)], product)


def find_tool(name, what):
    """The tool's path on this host's PATH, or None. A name that is not a bare program name is REFUSED: that is a
    malformed request. A bare name PATH does not find is a MEASUREMENT of the host -- "this host has no clang" -- so
    it is reported, never refused (✔MEASURED 2026-09-23: the arm64 VPS answered `clang` with nothing, and a refusal
    left only "probe exited 2" on this side, because a failed step keeps no report)."""
    if not BARE_NAME.match(name or ""):
        raise Refusal("%s %r must be a bare program name, never a path" % (what, name))
    return shutil.which(name)


def run_cmd(argv, cwd, seconds):
    """(exit code or a timeout mark, stdout, stderr), each stream capped."""
    try:
        p = subprocess.run(argv, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=seconds)
        rc = p.returncode
        out, err = p.stdout, p.stderr
    except subprocess.TimeoutExpired as e:
        rc = "timeout-%ds" % seconds
        out, err = e.stdout or b"", e.stderr or b""
    except OSError as e:
        return "not-started (%s)" % e, "", ""

    def text(b):
        s = b.decode("utf-8", errors="replace").replace("\x00", "")
        return s if len(s) <= MAX_STREAM_CHARS else s[:MAX_STREAM_CHARS] + "\n... %d more character(s) not kept" % (
            len(s) - MAX_STREAM_CHARS)
    return rc, text(out), text(err)


def kept_listing(product):
    """(report lines, a witness mark or None) for the assembly a compile wrote at `product`. A compiler that exits 0
    and writes no listing is a MEASUREMENT (`asm=absent`), not a crash that loses the report and the batch; a NUL is
    removed as run_cmd removes it from a stream, because a "listing" can be binary (✔MEASURED 2026-09-25: cl spoken
    to in gcc's dialect linked an IMAGE at `-o probe.s`, and its NULs reached the report)."""
    name = os.path.basename(product)
    if not os.path.isfile(product):
        return ["== %s: the compiler exited 0 and wrote no such file" % name], "asm=absent"
    with io.open(product, "rb") as f:
        asm = f.read().decode("utf-8", errors="replace").replace("\x00", "")
    kept = asm.rstrip("\n").split("\n")
    return (["== %s (%d line(s)%s):" % (name, len(kept), ", the first 5000 kept" if len(kept) > 5000 else "")]
            + kept[:5000]), None


def block(lines, title, rc, out, err):
    lines.append("== %s: exit %s" % (title, rc))
    for name, body in (("stdout", out), ("stderr", err)):
        if body.strip():
            lines.append("-- %s:" % name)
            lines.extend(body.rstrip("\n").split("\n"))


def probe(tree, src_b64, ext, cc, flags_value, mode, dump_value, scratch_parent=None, dialect_value=""):
    """Return (exit code, report lines). A refusal is exit 2 with one explanatory line."""
    try:
        if mode not in MODES:
            raise Refusal("--mode %r is not one of %s" % (mode, ", ".join(MODES)))
        if ext not in EXTENSIONS:
            raise Refusal("--ext %r is not one of %s" % (ext, ", ".join(EXTENSIONS)))
        dialect = dialect_for(cc, dialect_value)
        if ext not in DIALECT_EXTENSIONS[dialect]:
            raise Refusal("--ext %r: the %s dialect compiles only %s (cl does not assemble; ml64 is MSVC's "
                          "assembler)" % (ext, dialect, ", ".join(DIALECT_EXTENSIONS[dialect])))
        data = decode_source(src_b64, allow_empty=(mode == "stdout"))
        flags = split_list(flags_value)
        check_flags(flags, dialect)
        dump = split_list(dump_value)
        if dump and mode not in DUMP_MODES:
            raise Refusal("--dump applies to --mode %s: an object or a linked image" % ", ".join(DUMP_MODES))
        if dump and dump[0] not in DUMP_TOOLS:
            raise Refusal("--dump tool %r is not one of %s" % (dump[0], ", ".join(DUMP_TOOLS)))
        cc_path = find_tool(cc, "--cc")
        dump_path = find_tool(dump[0], "--dump tool") if dump else None
    except Refusal as e:
        return 2, ["probe-reference-cc: REFUSED - %s" % e]
    if cc_path is None:
        return 0, ["compiler: %s is not on this host's PATH; nothing was compiled" % cc,
                   "probe-reference-cc: OK mode=%s compiler=absent" % mode]

    scratch = tempfile.mkdtemp(prefix="dss-probe-reference-cc-", dir=scratch_parent)
    red = redactor(tree, scratch)
    lines = []
    src = os.path.join(scratch, "probe." + ext)
    with io.open(src, "wb") as f:
        f.write(data)
    real_cc = os.path.realpath(cc_path)
    lines.append("compiler: %s -> %s%s" % (cc, cc_path, "" if real_cc == cc_path else " -> " + real_cc))
    rc, out, err = run_cmd(identity_argv(cc, cc_path), scratch, 30)
    # A banner-only compiler writes its banner to stderr and a usage line to stdout (✔MEASURED 2026-09-25, cl 19.51).
    said = err if program_key(cc) in BANNER_ONLY else (out.strip() or err)
    version = [ln.strip() for ln in said.replace("\r", "").split("\n") if ln.strip()][:3]
    lines.append("version (exit %s): %s" % (rc, " | ".join(version)))
    lines.append("dialect: %s" % dialect)
    lines.append("source: probe.%s, %d byte(s)" % (ext, len(data)))

    witness = ["mode=" + mode]
    argv, shown, product = compile_argv(dialect, cc, cc_path, mode, flags, src, scratch)
    c_rc, out, err = run_cmd(argv, scratch, COMPILE_SECONDS)
    block(lines, "compile " + " ".join(shown), c_rc, out, err)
    witness.append("compile=%s" % c_rc)
    if mode != "stdout":
        if c_rc == 0 and mode == "asm":
            listing, mark = kept_listing(product)
            lines.extend(listing)
            if mark:
                witness.append(mark)
        if c_rc == 0 and mode == "run":
            r_rc, out, err = run_cmd([product], scratch, RUN_SECONDS)
            block(lines, "run ./%s" % os.path.basename(product), r_rc, out, err)
            witness.append("run=%s" % r_rc)
        if c_rc == 0 and dump and dump_path is None:
            lines.append("== dump: %s is not on this host's PATH; the object was not dumped" % dump[0])
            witness.append("dump=absent")
        elif c_rc == 0 and dump:
            d_rc, out, err = run_cmd([dump_path] + dump[1:] + [product], scratch, RUN_SECONDS)
            block(lines, "dump " + " ".join(dump + [os.path.basename(product)]), d_rc, out, err)
            witness.append("dump=%s" % d_rc)
    lines.append("probe-reference-cc: OK %s" % " ".join(witness))
    shutil.rmtree(scratch, ignore_errors=True)
    return 0, [red(ln) for ln in lines]


def _b64(text):
    return base64.b64encode(text.encode("utf-8")).decode("ascii")


def decode_batch(b64):
    """-> the batch's items. A malformed BATCH is refused whole, before anything is compiled."""
    if len(b64) > MAX_BATCH_B64:
        raise Refusal("--batch-b64 is %d characters, over the %d a run's inputs can carry"
                      % (len(b64), MAX_BATCH_B64))
    text = b64.strip().replace("-", "+").replace("_", "/")
    try:
        raw = base64.b64decode((text + "=" * (-len(text) % 4)).encode("ascii"), validate=True)
    except (binascii.Error, ValueError, UnicodeEncodeError) as e:
        raise Refusal("--batch-b64 is not base64: %s" % e)
    try:
        doc = json.loads(raw.decode("utf-8"))
    except (ValueError, UnicodeDecodeError) as e:
        raise Refusal("--batch-b64 is not a UTF-8 JSON array: %s" % e)
    sources = {}
    if isinstance(doc, dict):
        unknown = sorted(set(doc) - {"sources", "items"})
        if unknown:
            raise Refusal("a batch object carries unknown key(s) %s; the keys are sources, items" % unknown)
        sources = doc.get("sources", {})
        if not isinstance(sources, dict) or any(not isinstance(k, str) or not ITEM_NAME.match(k)
                                                or not isinstance(v, str) for k, v in sources.items()):
            raise Refusal("a batch's `sources` must map names matching %s to source text (\"\" is the empty "
                          "translation unit, a stdout-mode probe)" % ITEM_NAME.pattern)
        items = doc.get("items")
    else:
        items = doc
    if not isinstance(items, list) or not items:
        raise Refusal("--batch-b64 must decode to a non-empty JSON array")
    if len(items) > MAX_BATCH:
        raise Refusal("the batch holds %d items, over the %d allowed" % (len(items), MAX_BATCH))
    seen = set()
    for i, item in enumerate(items):
        if not isinstance(item, dict):
            raise Refusal("batch item %d is not an object" % i)
        unknown = sorted(set(item) - set(ITEM_KEYS))
        if unknown:
            raise Refusal("batch item %d carries unknown key(s) %s; the keys are %s"
                          % (i, unknown, ", ".join(ITEM_KEYS)))
        name = item.get("name")
        if not isinstance(name, str) or not ITEM_NAME.match(name):
            raise Refusal("batch item %d's name %r must match %s" % (i, name, ITEM_NAME.pattern))
        if name in seen:
            raise Refusal("batch item name %r appears twice" % name)
        seen.add(name)
        if "src" in item and "src_ref" in item:
            raise Refusal("batch item %r gives both src and src_ref" % name)
        if "src_ref" in item:
            if item["src_ref"] not in sources:
                raise Refusal("batch item %r: src_ref %r names no source in the batch" % (name, item["src_ref"]))
            item["src"] = sources[item["src_ref"]]
        if not isinstance(item.get("src"), str):
            raise Refusal("batch item %r has no source text (\"\" is the empty translation unit)" % name)
        for key in ITEM_KEYS[2:-1]:
            if key in item and not isinstance(item[key], str):
                raise Refusal("batch item %r: %s must be a string" % (name, key))
    return items


def run_batch(tree, batch_b64, scratch_parent=None):
    """Every item in turn, each under its `=== probe <name>` line. Return (exit code, report lines)."""
    try:
        items = decode_batch(batch_b64)
    except Refusal as e:
        return 2, ["probe-reference-cc: REFUSED - %s" % e]
    lines, measured = [], 0
    for item in items:
        rc, item_lines = probe(tree, _b64(item["src"]), item.get("ext", "c"), item.get("cc", "cc"),
                               item.get("flags", ""), item.get("mode", "run"), item.get("dump", ""), scratch_parent,
                               item.get("dialect", ""))
        lines.append("=== probe %s" % item["name"])
        lines.extend(item_lines)
        measured += 1 if rc == 0 else 0
    lines.append("probe-reference-cc: OK batch=%d measured=%d refused=%d"
                 % (len(items), measured, len(items) - measured))
    return 0, lines


def run(a):
    """The parsed options -> (exit code, report lines). Exactly one of the two sources."""
    if bool(a.src_b64) == bool(a.batch_b64):
        return 2, ["probe-reference-cc: REFUSED - give exactly one of --src-b64 (one probe) and --batch-b64 "
                   "(a batch)"]
    if a.batch_b64:
        return run_batch(a.tree, a.batch_b64)
    return probe(a.tree, a.src_b64, a.ext, a.cc, a.flags, a.mode, a.dump, dialect_value=a.dialect)


def selftest():
    failures = 0

    def arm(name, ok, detail=""):
        nonlocal failures
        print("probe-reference-cc selftest: %-36s %s" % (name, "ok" if ok else "FAIL" + (("\n" + detail)
                                                                                          if detail else "")))
        failures += 0 if ok else 1

    good = _b64("int main(void) { return 42; }\n")
    with tempfile.TemporaryDirectory() as tmp:
        tree = os.path.realpath(tmp)
        refusals = [
            ("empty source refused", dict(src_b64=""), "is empty"),
            ("non-base64 source refused", dict(src_b64="not base64!!"), "not base64"),
            ("oversized source refused", dict(src_b64=_b64("x" * (MAX_SOURCE_BYTES + 1))), "over the"),
            ("unknown mode refused", dict(mode="execute"), "--mode"),
            ("unknown extension refused", dict(ext="py"), "--ext"),
            ("compiler path refused", dict(cc="/usr/bin/cc"), "bare program name"),
            ("-o among flags refused", dict(flags_value="-O2,-o,x"), "owns every output path"),
            ("-oFILE among flags refused", dict(flags_value="-ox"), "owns every output path"),
            ("dump in asm mode refused", dict(mode="asm", dump_value="nm"), "applies to --mode object, link"),
            ("dump in stdout mode refused", dict(mode="stdout", dump_value="nm"), "applies to --mode object, link"),
            ("an empty source outside stdout mode refused", dict(src_b64="", mode="link"), "is empty"),
            ("unlisted dump tool refused", dict(mode="object", dump_value="rm,-rf"), "is not one of"),
        ]
        for name, over, want in refusals:
            kw = dict(tree=tree, src_b64=good, ext="c", cc="cc", flags_value="", mode="run", dump_value="")
            kw.update(over)
            rc, lines = probe(**kw)
            arm(name, rc == 2 and want in lines[0], "\n".join(lines))
        # ── THE MSVC DIALECT: each mode's spelling pinned as a pure argv (no cl needed); refusals by MESSAGE ──
        arm("cl and clang-cl speak msvc, the rest gnu",
            [dialect_for(c) for c in ("cl", "clang-cl", "clang-cl-19", "gcc", "cc", "clang", "gcc-13")]
            == ["msvc", "msvc", "msvc", "gnu", "gnu", "gnu", "gnu"])
        arm("--dialect overrides the compiler's name", dialect_for("gcc", "msvc") == "msvc"
            and dialect_for("cl", "gnu") == "gnu")
        if os.name == "nt":
            arm("a Windows program name is case-insensitive", dialect_for("CL.EXE") == "msvc"
                and identity_argv("Cl.exe", "P") == ["P"])
        rc, lines = probe(tree, good, "c", "cl", "", "run", "", dialect_value="borland")
        arm("an unknown dialect is refused", rc == 2 and "--dialect 'borland' is not one of gnu, msvc" in lines[0],
            "\n".join(lines))
        sc = os.path.join(tree, "sc")
        src = os.path.join(sc, "probe.c")
        exe, obj, lst = (os.path.join(sc, n) for n in ("probe.exe", "probe.obj", "probe.asm"))
        for mode, want in (("run", ["CL", "/nologo", "/O2", src, "/Fe" + exe]),
                           ("link", ["CL", "/nologo", "/O2", src, "/Fe" + exe]),
                           ("object", ["CL", "/nologo", "/c", "/O2", src, "/Fo" + obj]),
                           ("asm", ["CL", "/nologo", "/c", "/FA", "/O2", src, "/Fa" + lst, "/Fo" + obj]),
                           ("stdout", ["CL", "/nologo", "/O2", src])):
            got = compile_argv("msvc", "cl", "CL", mode, ["/O2"], src, sc)
            arm("msvc %s spelling" % mode, got[0] == want and got[2] == {"run": exe, "link": exe, "object": obj,
                                                                        "asm": lst}.get(mode), repr(got))
        got = compile_argv("msvc", "cl", "CL", "run", ["/O2", "/link", "/STACK:1048576"], src, sc)
        arm("msvc /link tail goes after the source and /Fe", got[0] == ["CL", "/nologo", "/O2", src, "/Fe" + exe,
                                                                         "/link", "/STACK:1048576"]
            and got[1] == ["cl", "/nologo", "/O2", "probe.c", "/Feprobe.exe", "/link", "/STACK:1048576"], repr(got))
        arm("identity: cl alone (its banner), the rest --version",
            (identity_argv("cl", "P"), identity_argv("clang-cl", "P"), identity_argv("gcc", "P"))
            == (["P"], ["P", "--version"], ["P", "--version"]))
        arm("gnu spellings unchanged",
            [compile_argv("gnu", "cc", "CC", m, ["-O2"], src, sc)[0] for m in ("run", "object", "asm", "stdout")]
            == [["CC", "-O2", src, "-o", exe], ["CC", "-c", "-O2", src, "-o", os.path.join(sc, "probe.o")],
                ["CC", "-S", "-O2", src, "-o", os.path.join(sc, "probe.s")], ["CC", "-O2", src]])
        for name, flags_value in (("msvc /Fe:x refused", "/Fe:x"), ("msvc -Fox refused", "-Fox.obj"),
                                  ("msvc /Fa<path> refused", "/Fax.asm"), ("msvc /link /OUT: refused",
                                                                           "/link,/OUT:x.exe"),
                                  # `/Out:`, not `/out:`: the deprecated-/o rule refuses `/out:` on its own.
                                  ("msvc /link /Out: refused (any case)", "/link,/Out:x.exe"),
                                  ("msvc deprecated -o <path> refused", "-o,x.exe"),
                                  ("msvc deprecated /o<path> refused", "/ox.exe")):
            rc, lines = probe(tree, good, "c", "cl", flags_value, "run", "")
            arm(name, rc == 2 and "owns every output path" in lines[0], "\n".join(lines))
        for ext in ("s", "S"):
            rc, lines = probe(tree, good, ext, "cl", "", "object", "")
            arm("msvc refuses an assembly source (.%s)" % ext, rc == 2 and "cl does not assemble" in lines[0],
                "\n".join(lines))
        try:
            check_flags(["/FAs", "/FAcs", "/Fdx.pdb", "/W4", "/std:c11", "/Od", "/O2", "/openmp", "/openmp:llvm",
                         "-openmp", "/options:strict", "/link", "/STACK:1048576"], "msvc")
            arm("msvc: /FA, /O*, /openmp, /options: and non-product outputs allowed", True)
        except Refusal as e:
            arm("msvc: /FA, /O*, /openmp, /options: and non-product outputs allowed", False, str(e))
        rc, lines = probe(tree, good, "c", "no-such-cc-for-probe", "", "run", "")
        arm("an absent compiler is REPORTED, not refused", rc == 0 and lines[-1].endswith("compiler=absent")
            and "not on this host's PATH" in lines[0], "\n".join(lines))
        rc, lines = probe(tree, "", "c", "no-such-cc-for-probe", "-dM,-E", "stdout", "")
        arm("an EMPTY source is a stdout-mode probe", rc == 0 and lines[-1].endswith("compiler=absent"),
            "\n".join(lines))
        arm("flags are percent-decoded", split_list("-Wl%2C-z%2Cnoexecstack,-DX=a%20b,,-O2")
            == ["-Wl,-z,noexecstack", "-DX=a b", "-O2"])
        arm("a listing never written is asm=absent, not a crash",
            kept_listing(os.path.join(tree, "never-written.s"))
            == (["== never-written.s: the compiler exited 0 and wrote no such file"], "asm=absent"))
        with io.open(os.path.join(tree, "binary.s"), "wb") as f:
            f.write(b"MZ\x00\x00a\x00b\nc\n")
        arm("a binary listing keeps no NUL", kept_listing(os.path.join(tree, "binary.s"))
            == (["== binary.s (2 line(s)):", "MZab", "c"], None), repr(kept_listing(os.path.join(tree, "binary.s"))))
        arm("URL-safe base64 decodes", decode_source(base64.urlsafe_b64encode(b"\xfb\xff int").decode("ascii")
                                                     .rstrip("=")) == b"\xfb\xff int")
        red = redactor(tree, os.path.join(tree, "scratch"))
        native = os.path.join(tree, "scratch", "probe.c") + " and " + os.path.join(tree, "src")
        for spelling, text in (("native", native), ("forward-slash", native.replace("\\", "/"))):
            want = "<probe>%sprobe.c and <tree>%ssrc" % ((os.sep, os.sep) if spelling == "native" else ("/", "/"))
            arm("scratch and tree redacted (%s)" % spelling, red(text) == want, red(text))
        cc = next((c for c in ("cc", "gcc", "clang") if shutil.which(c)), None)
        # ★ A LEG WITHOUT A REFERENCE COMPILER IS A NAMED FAILURE, never a skip: the arms below are the only
        # ones that run a real compiler (and the only ones that read a real linked image), and a gate leg that
        # skipped them would pass having proved nothing about what this program exists to measure.
        arm("a reference compiler is on this host's PATH", cc is not None,
            "none of cc, gcc or clang resolves on this host's PATH")
        if cc is not None:
            rc, lines = probe(tree, good, "c", cc, "", "run", "", scratch_parent=tree)
            body = "\n".join(lines)
            arm("real compile and run (%s)" % cc, rc == 0 and "compile=0" in lines[-1] and "run=42" in lines[-1]
                and tree not in body, body)
            leftovers = [n for n in os.listdir(tree) if n.startswith("dss-probe-reference-cc-")]
            arm("the scratch directory is removed", not leftovers, repr(leftovers))
            rc, lines = probe(tree, _b64("int main(void) { return undeclared; }\n"), "c", cc, "", "run", "",
                              scratch_parent=tree)
            arm("a refused compile is still a measurement", rc == 0 and "run=" not in lines[-1]
                and "compile=0" not in lines[-1], "\n".join(lines))
            saved = DUMP_TOOLS
            globals()["DUMP_TOOLS"] = saved + ("no-such-dump-for-probe",)
            try:
                rc, lines = probe(tree, good, "c", cc, "", "object", "no-such-dump-for-probe", scratch_parent=tree)
            finally:
                globals()["DUMP_TOOLS"] = saved
            arm("an absent dump tool is REPORTED, not refused", rc == 0 and "compile=0" in lines[-1]
                and lines[-1].endswith("dump=absent"), "\n".join(lines))
            globals()["DUMP_TOOLS"] = saved + ("no-such-dump-for-probe",)
            try:
                rc, lines = probe(tree, good, "c", cc, "", "link", "no-such-dump-for-probe", scratch_parent=tree)
            finally:
                globals()["DUMP_TOOLS"] = saved
            arm("a dump in LINK mode reads the linked image", rc == 0 and "compile=0" in lines[-1]
                and lines[-1].endswith("dump=absent") and "run=" not in lines[-1], "\n".join(lines))
            rc, lines = probe(tree, _b64(""), "c", cc, "-dM,-E", "stdout", "", scratch_parent=tree)
            arm("an EMPTY translation unit prints the predefined macros (%s)" % cc, rc == 0
                and "compile=0" in lines[-1] and any(ln.startswith("#define ") for ln in lines),
                "\n".join(lines[-12:]))
            dumper = next((d for d in ("nm", "llvm-nm") if shutil.which(d)), None)
            arm("a dump tool is on this host's PATH", dumper is not None,
                "neither nm nor llvm-nm resolves on this host's PATH")
            if dumper:
                rc, lines = probe(tree, good, "c", cc, "", "run", dumper, scratch_parent=tree)
                arm("run mode runs, then dumps the linked image (%s)" % dumper, rc == 0 and "run=42" in lines[-1]
                    and "dump=0" in lines[-1] and any("main" in ln for ln in lines), "\n".join(lines[-12:]))
        # ── REAL cl. ★ INSIDE A VISUAL STUDIO DEVELOPER ENVIRONMENT (vcvars sets VCToolsInstallDir) cl IS the
        # reference compiler, so a cl or a dumpbin that PATH does not find there is a named FAILURE, never a skip,
        # for the reason above; outside one no cl is expected, and the arms say so by name.
        if not os.environ.get("VCToolsInstallDir"):
            print("probe-reference-cc selftest: %-36s %s" % ("real cl arms", "not this host's: no Visual Studio "
                                                             "developer environment (VCToolsInstallDir unset)"))
        else:
            arm("cl is on PATH in the developer environment", shutil.which("cl") is not None,
                "VCToolsInstallDir is set but cl does not resolve on PATH")
            arm("dumpbin is on PATH in the developer environment", shutil.which("dumpbin") is not None,
                "VCToolsInstallDir is set but dumpbin does not resolve on PATH")
            rc, lines = probe(tree, good, "c", "cl", "", "run", "", scratch_parent=tree)
            body = "\n".join(lines)
            arm("real cl compile and run", rc == 0 and "compile=0" in lines[-1] and "run=42" in lines[-1]
                and "dialect: msvc" in lines and tree not in body, body)
            # Exit 0 is part of the pin: cl run with NO arguments exits 0, while `cl --version` prints the same banner,
            # warns D9002 and exits 2 (✔MEASURED 2026-09-25, cl 19.51.36260), so only the exit tells them apart.
            arm("real cl identity is its banner (exit 0)", any(re.match(r"version \(exit 0\): Microsoft \(R\) C/C\+\+ "
                                                                        r"Optimizing Compiler Version [0-9]", ln)
                                                               for ln in lines), body)
            rc, lines = probe(tree, good, "c", "cl", "/O2,/link,/STACK:1048576", "run", "", scratch_parent=tree)
            arm("real cl: a /link tail reaches the linker", rc == 0 and "run=42" in lines[-1], "\n".join(lines))
            rc, lines = probe(tree, _b64("int main(void) { return undeclared; }\n"), "c", "cl", "", "run", "",
                              scratch_parent=tree)
            arm("real cl: a refused compile is a measurement", rc == 0 and "run=" not in lines[-1]
                and "compile=0" not in lines[-1] and any("C2065" in ln for ln in lines), "\n".join(lines))
            rc, lines = probe(tree, good, "c", "cl", "", "object", "dumpbin,/SYMBOLS", scratch_parent=tree)
            arm("real cl object, dumped by dumpbin", rc == 0 and "dump=0" in lines[-1]
                and any(re.search(r"External\s+\|\s+main\b", ln) for ln in lines), "\n".join(lines))
            rc, lines = probe(tree, good, "c", "cl", "", "link", "dumpbin,/HEADERS", scratch_parent=tree)
            arm("real cl link, the IMAGE dumped", rc == 0 and "dump=0" in lines[-1] and "run=" not in lines[-1]
                and any("EXECUTABLE IMAGE" in ln for ln in lines), "\n".join(lines[-20:]))
            rc, lines = probe(tree, good, "c", "cl", "/O2", "asm", "", scratch_parent=tree)
            arm("real cl assembly listing", rc == 0 and any(re.match(r"main\s+PROC\b", ln) for ln in lines),
                "\n".join(lines))
            rc, lines = probe(tree, _b64("int v = _MSC_VER;\n"), "c", "cl", "/EP", "stdout", "", scratch_parent=tree)
            arm("real cl /EP expands _MSC_VER", rc == 0 and any(re.match(r"int v = [0-9]+;", ln) for ln in lines),
                "\n".join(lines))
            leftovers = [n for n in os.listdir(tree) if n.startswith("dss-probe-reference-cc-")]
            arm("real cl: every scratch directory is removed", not leftovers, repr(leftovers))
        def b64json(obj):
            return base64.urlsafe_b64encode(json.dumps(obj).encode("utf-8")).decode("ascii").rstrip("=")
        # The refused item comes FIRST, so a batch that stopped at a refusal would lose the item after it.
        rc, lines = run_batch(tree, b64json([
            {"name": "bad-flags", "src": "int x;\n", "flags": "-o,x"},
            {"name": "absent", "src": "int main(void) { return 42; }\n", "cc": "no-such-cc-for-probe"}]))
        heads = [ln for ln in lines if ln.startswith("=== probe ")]
        arm("a batch runs every item; a refused item goes on", rc == 0
            and heads == ["=== probe bad-flags", "=== probe absent"]
            and lines[-1] == "probe-reference-cc: OK batch=2 measured=1 refused=1", "\n".join(lines))
        batch_refusals = [
            ("a batch not JSON is refused whole", base64.b64encode(b"not json").decode("ascii"),
             "not a UTF-8 JSON array"),
            ("a batch name twice is refused", b64json([{"name": "a", "src": "x"}, {"name": "a", "src": "y"}]),
             "appears twice"),
            ("an unknown batch key is refused", b64json([{"name": "a", "src": "x", "cflags": "-O2"}]),
             "unknown key"),
            ("a batch item name must be bare", b64json([{"name": "../a", "src": "x"}]), "must match"),
            ("an oversized batch is refused", "A" * (MAX_BATCH_B64 + 1), "over the"),
            ("a src_ref naming no source is refused",
             b64json({"sources": {"s": "x"}, "items": [{"name": "a", "src_ref": "t"}]}), "names no source"),
            ("a source that is not text is refused", b64json({"sources": {"s": 1}, "items": [{"name": "a", "src_ref": "s"}]}),
             "source text"),
            ("src and src_ref together are refused",
             b64json({"sources": {"s": "x"}, "items": [{"name": "a", "src": "x", "src_ref": "s"}]}), "both src"),
            ("a batch item's dialect must be a string", b64json([{"name": "a", "src": "x", "dialect": 1}]),
             "dialect must be a string"),
        ]
        for name, value, want in batch_refusals:
            rc, lines = run_batch(tree, value)
            arm(name, rc == 2 and want in lines[0], "\n".join(lines))
        # The msvc-refused item comes FIRST: its refusal is its own, and the item after it still runs.
        rc, lines = run_batch(tree, b64json([
            {"name": "msvc-refused", "src": "int x;\n", "cc": "cl", "flags": "/Fex.exe"},
            {"name": "dialect-override", "src": "int x;\n", "cc": "no-such-cc-for-probe", "dialect": "msvc"}]))
        arm("a batch item carries its own dialect", rc == 0
            and lines[-1] == "probe-reference-cc: OK batch=2 measured=1 refused=1"
            and any("owns every output path" in ln for ln in lines), "\n".join(lines))
        rc, lines = run_batch(tree, b64json({"sources": {"prog": "int main(void) { return 42; }\n"}, "items": [
            {"name": "v1", "src_ref": "prog", "cc": "no-such-cc-for-probe", "flags": "-O0"},
            {"name": "v2", "src_ref": "prog", "cc": "no-such-cc-for-probe", "flags": "-O2"}]}))
        rc2, lines2 = run_batch(tree, b64json({"sources": {"empty": ""}, "items": [
            {"name": "dM", "src_ref": "empty", "mode": "stdout", "flags": "-dM,-E", "cc": "no-such-cc-for-probe"},
            {"name": "link-empty", "src": "", "mode": "link", "cc": "no-such-cc-for-probe"}]}))
        arm("a batch's empty source: measured in stdout mode, refused as ITS item elsewhere", rc2 == 0
            and lines2[-1] == "probe-reference-cc: OK batch=2 measured=1 refused=1"
            and any("is empty" in ln for ln in lines2), "\n".join(lines2))
        arm("a batch object shares one source across items", rc == 0
            and [ln for ln in lines if ln.startswith("=== probe ")] == ["=== probe v1", "=== probe v2"]
            and lines[-1] == "probe-reference-cc: OK batch=2 measured=2 refused=0", "\n".join(lines))
        for label, argv in (("both sources", ["--src-b64=" + good, "--batch-b64=" + good]), ("no source", [])):
            rc, lines = run(build_parser().parse_args(["--tree=" + tree, "--out=o"] + argv))
            arm("exactly one source (%s refused)" % label, rc == 2 and "exactly one of" in lines[0],
                "\n".join(lines))
    # A flag list, a URL-safe base64 source and a dump argv can each BEGIN with `-`, which argparse reads as another
    # option unless it is spelled `--name=value` -- so the runner's own .yml must spell every option that way.
    a = build_parser().parse_args(["--tree=t", "--src-b64=-AB_", "--flags=-std=c2x,-dM", "--dump=-x", "--out=o",
                                   "--dialect=msvc"])
    arm("option values may begin with '-'", (a.src_b64, a.flags, a.dump, a.dialect)
        == ("-AB_", "-std=c2x,-dM", "-x", "msvc"))
    yml = os.path.join(os.path.dirname(os.path.realpath(__file__)), "probe-reference-cc.yml")
    with io.open(yml, encoding="utf-8") as f:
        run_line = next((ln for ln in f if "probe-reference-cc.py" in ln and "--tree" in ln), "")
    loose = re.findall(r"(--[a-z0-9-]+)(?=\s)", run_line)
    arm("the .yml spells every option --name=value", bool(run_line) and not loose, "loose options: %r" % loose)
    print("probe-reference-cc selftest: %s" % ("OK" if failures == 0 else "FAIL - %d arm(s)" % failures))
    return 1 if failures else 0


def build_parser():
    ap = argparse.ArgumentParser(description="Compile, and optionally run or dump, one probe with this host's "
                                             "reference compiler; print and keep the report, redacted.")
    ap.add_argument("--tree", required=True)
    ap.add_argument("--src-b64", default="")
    ap.add_argument("--batch-b64", default="")
    ap.add_argument("--ext", default="c")
    ap.add_argument("--cc", default="cc")
    ap.add_argument("--flags", default="")
    ap.add_argument("--mode", default="run")
    ap.add_argument("--dump", default="")
    ap.add_argument("--dialect", default="")
    ap.add_argument("--out", required=True)
    return ap


def main(argv):
    if argv[1:] == ["--selftest"]:
        return selftest()
    a = build_parser().parse_args(argv[1:])
    rc, lines = run(a)
    text = "\n".join(lines) + "\n"
    sys.stdout.write(text)
    if rc == 0:
        os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
        with io.open(a.out, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
    return rc


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
