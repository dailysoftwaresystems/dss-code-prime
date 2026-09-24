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
it (and the link's target), and the first lines of its `--version`.

FLAGS: `--flags`, a comma-separated list; each item is percent-decoded, so a comma or a space inside
ONE flag is written `%2C` or `%20` (`-Wl%2C-z%2Cnoexecstack`). `-o` is refused: this program owns
every output path.

DUMP (object mode only): `--dump`, a comma-separated argv, percent-decoded like the flags, whose first
item is one of DUMP_TOOLS; the object's path is appended as its last argument.

REFUSED by name, exit 2, before anything is compiled: an empty or undecodable source, a decoded
source over MAX_SOURCE_BYTES, a compiler or dump tool spelled as a path, a dump tool outside
DUMP_TOOLS, a `--dump` outside object mode, `-o` among the flags, an unknown mode or extension.
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
objects, each `{"name", "src"}` plus any of `ext`, `cc`, `flags`, `mode`, `dump` (strings, the same
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

`--selftest` runs the refusal, decoding, batch and redaction arms, and one real compile-and-run when
the host's PATH has a `cc`, `gcc` or `clang` (it says which, or that it found none).
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
DUMP_TOOLS = ("objdump", "llvm-objdump", "otool", "nm", "llvm-nm", "readelf", "llvm-readelf", "size", "llvm-size")
MAX_SOURCE_BYTES = 256 * 1024
MAX_STREAM_CHARS = 200000
BARE_NAME = re.compile(r"^[A-Za-z0-9_.+-]+$")
COMPILE_SECONDS = 180
RUN_SECONDS = 60
MAX_BATCH = 32
MAX_BATCH_B64 = 24000
ITEM_NAME = re.compile(r"^[A-Za-z0-9_.-]{1,64}$")
ITEM_KEYS = ("name", "src", "ext", "cc", "flags", "mode", "dump", "src_ref")


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


def decode_source(b64):
    if not b64:
        raise Refusal("--src-b64 is empty")
    text = b64.strip().replace("-", "+").replace("_", "/")  # the URL-safe alphabet, strictly, as the standard one
    padded = text + "=" * (-len(text) % 4)
    try:
        data = base64.b64decode(padded.encode("ascii"), validate=True)
    except (binascii.Error, ValueError, UnicodeEncodeError) as e:
        raise Refusal("--src-b64 is not base64: %s" % e)
    if not data:
        raise Refusal("--src-b64 decodes to an empty source")
    if len(data) > MAX_SOURCE_BYTES:
        raise Refusal("the decoded source is %d bytes, over the %d-byte limit" % (len(data), MAX_SOURCE_BYTES))
    return data


def split_list(value):
    """A comma-separated list, each item percent-decoded; empty items dropped."""
    return [unquote(item) for item in (value or "").split(",") if item != ""]


def check_flags(flags):
    for f in flags:
        if f == "-o" or (f.startswith("-o") and not f.startswith("-objc")):
            raise Refusal("--flags names %r; this program owns every output path" % f)


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


def block(lines, title, rc, out, err):
    lines.append("== %s: exit %s" % (title, rc))
    for name, body in (("stdout", out), ("stderr", err)):
        if body.strip():
            lines.append("-- %s:" % name)
            lines.extend(body.rstrip("\n").split("\n"))


def probe(tree, src_b64, ext, cc, flags_value, mode, dump_value, scratch_parent=None):
    """Return (exit code, report lines). A refusal is exit 2 with one explanatory line."""
    try:
        if mode not in MODES:
            raise Refusal("--mode %r is not one of %s" % (mode, ", ".join(MODES)))
        if ext not in EXTENSIONS:
            raise Refusal("--ext %r is not one of %s" % (ext, ", ".join(EXTENSIONS)))
        data = decode_source(src_b64)
        flags = split_list(flags_value)
        check_flags(flags)
        dump = split_list(dump_value)
        if dump and mode != "object":
            raise Refusal("--dump applies only to --mode object")
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
    rc, out, err = run_cmd([cc_path, "--version"], scratch, 30)
    version = (out.strip() or err.strip()).replace("\r", "").split("\n")[:3]
    lines.append("version (exit %s): %s" % (rc, " | ".join(version)))
    lines.append("source: probe.%s, %d byte(s)" % (ext, len(data)))

    witness = ["mode=" + mode]
    if mode == "stdout":
        argv = [cc_path] + flags + [src]
        c_rc, out, err = run_cmd(argv, scratch, COMPILE_SECONDS)
        block(lines, "compile " + " ".join([cc] + flags + ["probe." + ext]), c_rc, out, err)
        witness.append("compile=%s" % c_rc)
    else:
        product = os.path.join(scratch, {"object": "probe.o", "asm": "probe.s"}.get(mode, "probe.exe"))
        stage = {"object": ["-c"], "asm": ["-S"]}.get(mode, [])
        argv = [cc_path] + stage + flags + [src, "-o", product]
        shown = [cc] + stage + flags + ["probe." + ext, "-o", os.path.basename(product)]
        c_rc, out, err = run_cmd(argv, scratch, COMPILE_SECONDS)
        block(lines, "compile " + " ".join(shown), c_rc, out, err)
        witness.append("compile=%s" % c_rc)
        if c_rc == 0 and mode == "asm":
            with io.open(product, "rb") as f:
                asm = f.read().decode("utf-8", errors="replace")
            kept = asm.rstrip("\n").split("\n")
            lines.append("== %s (%d line(s)%s):" % (os.path.basename(product), len(kept),
                                                    ", the first 5000 kept" if len(kept) > 5000 else ""))
            lines.extend(kept[:5000])
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
                                                or not isinstance(v, str) or not v for k, v in sources.items()):
            raise Refusal("a batch's `sources` must map names matching %s to non-empty source text"
                          % ITEM_NAME.pattern)
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
        if not isinstance(item.get("src"), str) or not item["src"]:
            raise Refusal("batch item %r has no source text" % name)
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
                               item.get("flags", ""), item.get("mode", "run"), item.get("dump", ""), scratch_parent)
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
    return probe(a.tree, a.src_b64, a.ext, a.cc, a.flags, a.mode, a.dump)


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
            ("dump outside object mode refused", dict(dump_value="nm"), "only to --mode object"),
            ("unlisted dump tool refused", dict(mode="object", dump_value="rm,-rf"), "is not one of"),
        ]
        for name, over, want in refusals:
            kw = dict(tree=tree, src_b64=good, ext="c", cc="cc", flags_value="", mode="run", dump_value="")
            kw.update(over)
            rc, lines = probe(**kw)
            arm(name, rc == 2 and want in lines[0], "\n".join(lines))
        rc, lines = probe(tree, good, "c", "no-such-cc-for-probe", "", "run", "")
        arm("an absent compiler is REPORTED, not refused", rc == 0 and lines[-1].endswith("compiler=absent")
            and "not on this host's PATH" in lines[0], "\n".join(lines))
        arm("flags are percent-decoded", split_list("-Wl%2C-z%2Cnoexecstack,-DX=a%20b,,-O2")
            == ["-Wl,-z,noexecstack", "-DX=a b", "-O2"])
        arm("URL-safe base64 decodes", decode_source(base64.urlsafe_b64encode(b"\xfb\xff int").decode("ascii")
                                                     .rstrip("=")) == b"\xfb\xff int")
        red = redactor(tree, os.path.join(tree, "scratch"))
        native = os.path.join(tree, "scratch", "probe.c") + " and " + os.path.join(tree, "src")
        for spelling, text in (("native", native), ("forward-slash", native.replace("\\", "/"))):
            want = "<probe>%sprobe.c and <tree>%ssrc" % ((os.sep, os.sep) if spelling == "native" else ("/", "/"))
            arm("scratch and tree redacted (%s)" % spelling, red(text) == want, red(text))
        cc = next((c for c in ("cc", "gcc", "clang") if shutil.which(c)), None)
        if cc is None:
            print("probe-reference-cc selftest: %-36s %s" % ("real compile and run", "not run - this host's PATH has "
                                                             "no cc, gcc or clang"))
        else:
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
            ("src and src_ref together are refused",
             b64json({"sources": {"s": "x"}, "items": [{"name": "a", "src": "x", "src_ref": "s"}]}), "both src"),
        ]
        for name, value, want in batch_refusals:
            rc, lines = run_batch(tree, value)
            arm(name, rc == 2 and want in lines[0], "\n".join(lines))
        rc, lines = run_batch(tree, b64json({"sources": {"prog": "int main(void) { return 42; }\n"}, "items": [
            {"name": "v1", "src_ref": "prog", "cc": "no-such-cc-for-probe", "flags": "-O0"},
            {"name": "v2", "src_ref": "prog", "cc": "no-such-cc-for-probe", "flags": "-O2"}]}))
        arm("a batch object shares one source across items", rc == 0
            and [ln for ln in lines if ln.startswith("=== probe ")] == ["=== probe v1", "=== probe v2"]
            and lines[-1] == "probe-reference-cc: OK batch=2 measured=2 refused=0", "\n".join(lines))
        for label, argv in (("both sources", ["--src-b64=" + good, "--batch-b64=" + good]), ("no source", [])):
            rc, lines = run(build_parser().parse_args(["--tree=" + tree, "--out=o"] + argv))
            arm("exactly one source (%s refused)" % label, rc == 2 and "exactly one of" in lines[0],
                "\n".join(lines))
    # A flag list, a URL-safe base64 source and a dump argv can each BEGIN with `-`, which argparse reads as another
    # option unless it is spelled `--name=value` -- so the runner's own .yml must spell every option that way.
    a = build_parser().parse_args(["--tree=t", "--src-b64=-AB_", "--flags=-std=c2x,-dM", "--dump=-x", "--out=o"])
    arm("option values may begin with '-'", (a.src_b64, a.flags, a.dump) == ("-AB_", "-std=c2x,-dM", "-x"))
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
