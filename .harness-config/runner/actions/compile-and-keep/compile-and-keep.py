# PURPOSE: compile sources carried in the request with the dsscp a leg itself BUILT, for a stated target and each stated pipeline, and KEEP the images beside a report of every compile (exit code, diagnostics, the image's name, size and md5), so an image one machine cannot build is built on a leg that can and pulled back.
"""compile-and-keep.py -- build images on the leg that CAN build them, and keep them.

WHY IT EXISTS (2026-10-08, cycle P69): DSS builds any target on any host, so a compiler built on one leg makes
images for every other. Nothing in the harness KEPT such an image: the in-process corpus runner judges an image
it cannot run in a temporary directory, the command-line runner removes its scratch on green, `compile-bench`
removes each run directory, and `probe-reference-cc` runs a REFERENCE compiler by bare name and keeps text. So a
fix whose compiler could not be rebuilt on the one machine that runs its images could not be proved that day,
although a leg that could build it was standing by. This action is that carriage: the leg builds its own dsscp
(DssHarness builds a leg before a step that names its product), this program compiles the request's sources with
it, and `dssharness sync --legs <leg> --pull <each kept file>` brings the images to the machine that runs them.

THE SOURCES travel in the request, never as files in the tree: `--sources-b64`, the base64 (standard or URL-safe
alphabet, padding optional) of a UTF-8 JSON array of `{"name", "src", "ext"}` objects, each optionally with
`"language"`. A source placed in the tree would have to be synced there, which overwrites the ONE copy of the
repository a host keeps, under whatever lane is using it. The bytes may be that JSON text or a zlib stream of it
(told apart by the first byte: a JSON array opens with `[`), because a run's inputs reach DssHarness on one Windows
command line and a handful of real sources do not fit it plain. THE PACK STEP WRITES that value from files, on
the machine that holds them (`dssharness run compile-and-keep --legs <that machine's leg> --manual-step pack
--input from=<paths>`): `--from` is a comma-separated list of files and directories, a directory standing for
every file directly inside it and a relative path taken from the tree; each file is carried as name = its stem,
ext = its extension. The value is written alone on one line of `--out`, the step's kept output, having been read
back through the same decoder a run uses. Each source is compiled alone: `<name>.<ext>` is one translation unit
and one image.

THE COMPILER is `--dsscp`, the path of the file the leg's own build made (the action passes `{product}`). It is
never looked up: a bare program name, which only PATH could resolve, is refused, and so is a path that is not a
file. Its documents are named, not discovered: every compile runs with `DSS_CONFIG_ROOT` = `--tree`, the tree
the compiler was built from, so neither the working directory nor an installed copy can answer instead.

THE TARGET is `--target`, the compiler's own `<targetName>:<formatName>` spec, passed to it verbatim and never
composed here. THE LANGUAGE is `--language`, likewise verbatim (one source may name its own).

THE PIPELINES are `--configs`, a comma-separated list of arm names. `baseline` is the arm the two corpus runners
call by that name: NO `--config` token at all, the compiler's own default, which is a shipped behaviour of its
own and not `--config=debug` spelled implicitly. Every other name is passed as `--config=<name>` and judged by
the compiler. Every source is compiled once per arm.

THE KIND is `--kind`, optional: the artifact profile the images must be (the product's own word for an artifact's
kind; `src/core/types/artifact_profile.hpp` is its registry). The kind of image a target makes is fixed by the
FORMAT its spec names, so this option selects nothing: it is CHECKED, by membership, against the
`artifactProfiles` the spec's format document declares under `--tree`, before anything is compiled. A format
that declares none (a relocatable object) is compiled with no kind stated, and a kind stated for it is refused.

THE REQUEST IS PUT TO THE COMPILER BEFORE ANY SOURCE IS. The compiler exits 1 for a source that does not compile
and 1 for a target it does not know, so an exit code cannot tell a refused request from a failed compile, and a
list of diagnostic codes would be this program's guess at the compiler's vocabulary. Instead each (language,
arm) is asked once for a source that does NOT EXIST: a compiler that accepts the request gets as far as that
source and says it cannot open it, naming it; one that never names it refused the request before any source, and
its own words are this program's refusal. (MEASURED 2026-10-08 on ten requests: every accepted one named the
absent file, every refused one -- an unknown target, format, language or pipeline, a mismatched pair, a spec
with no `:` -- did not.)

WHAT IS KEPT, under `--step-dir` (the step's own directory, `{stepBuild}`):
    kept/compile-and-keep.txt            the report, as printed
    kept/images/<name>/<arm>/<file>      every file that compile left, as the compiler named it
`kept` is the step's declared output, so `persist` keeps it. `sync --pull` carries FILES, each named, never a
directory (MEASURED 2026-10-08: "... is a directory in ..., and only files cross, each named: name the files in
it"), so the report's last line but one IS the command that brings every kept file back, whole, for the leg it
ran on: `--leg` and `--kept-at` (where this leg's kept outputs go) are what it is written from, and a run with
`-v` shows it as the step prints it. The run's `--json` names the same files as keptOutputs. The sources are
written under `sources/` beside `kept`, which is not kept: the report carries each source's size and md5.
EVERY path this program writes is made by ONE function, which refuses a name that is not one entry directly
inside its parent, under its own spelling -- `..`, `.`, a separator, an absolute path, a drive, a device.

THE REPORT, per source and arm: the source's size and md5, the compile's exit code, what the compiler printed,
and every file it left with its size and md5 (or that it left none). A compile that FAILS is a measurement, and
so is one that exits 0 and leaves nothing: the run goes on, the report says so, and the step passes -- a failed
step keeps nothing, and a report is what the caller came for. The last line counts them:
`compile-and-keep: OK compiles=<n> built=<b> failed=<f> empty=<e> images=<i>`.
Every line printed or kept passes THE ONE REDACTOR (`redact/redact.py`, loaded by path from its sibling).

EXIT CODES -- 0 means every compile RAN; each refusal is one line, `compile-and-keep: REFUSED (exit <n>) - ...`
(the compiler's own words under it, for 4), printed before any source is compiled and before anything is kept:
    2   the request is malformed (an empty option, an empty or repeated pipeline name, a pipeline name that is
        not a plain word, an empty path for the pack), the step's directory already holds a run, or this host's
        account cannot be named for redaction
    3   no built dsscp: none named, a bare program name, a path that is not a file, or a file that cannot be
        started
    4   the built compiler refuses the request (the language, the target, or a pipeline), or its answer to the
        control cannot be read (it exits 0 for a source that is not there, or the control source exists)
    5   the sources do not decode: not base64, not a JSON array (plain or zlib), a malformed, repeated, empty or
        oversized source, a source name or extension that is not a plain word, more characters than a run's
        inputs carry -- and, for the pack, a file that cannot be read, is not UTF-8 text or has no name or no
        extension, and a directory that holds no file
    6   a path would leave the step's directory: a source name, an extension or a pipeline name that is not one
        entry directly inside its parent
    7   the stated kind is not one the target's format document serves, or that document cannot be read
An argument error argparse itself reports is also exit 2.

`--selftest` holds every refusal above and every counted outcome against a stand-in compiler it writes itself,
so it needs no build and runs anywhere (the action's manual `self-test` step). With `--tree`, `--dsscp`,
`--live-example` and `--live-target` it adds the LIVE arms, with the compiler a build made: it reads a corpus
example's own manifest (language, source, the artifact its target row names, the shipped pipelines) and compiles
it for that target under every arm, puts an unknown target and an unknown pipeline to the real compiler, and
states the kind the target's real format document declares -- which is what holds this program's reading of the
compiler's command line, of its answers to the control and of the documents' own key, on every leg of the gate
(ctest: `harness/compile_and_keep_selftest`). The arms are counted against an exact ratchet per block, and an arm
whose subject refuses or crashes FAILS BY NAME: the arms after it still run.
"""
import argparse
import base64
import binascii
import contextlib
import hashlib
import importlib.util
import io
import json
import os
import re
import shutil
import subprocess
import sys
sys.dont_write_bytecode = True  # this program loads redact.py by path; no __pycache__ beside a sibling action
import tempfile
import zlib

# What this program prints is a compiler's output, so any character can reach the pipe: both streams are UTF-8
# from IMPORT on (argument errors and --help print before main()), the repository's rule for every action.
for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError, OSError):
        pass

EXIT_REQUEST = 2
EXIT_NO_COMPILER = 3
EXIT_COMPILER_REFUSES = 4
EXIT_SOURCES = 5
EXIT_PATH = 6
EXIT_KIND = 7

# The arm that passes NO `--config` token: the corpus runners' own name for it.
BASELINE = "baseline"
# The step directory's two entries: what is kept (the step's declared output) and the scratch that is not.
KEPT = "kept"
SOURCES = "sources"
IMAGES = "images"
REPORT = "compile-and-keep.txt"
# The source the control request names. It must not exist; a compiler that accepts a request names it.
CONTROL = "compile-and-keep-absent-control"
ITEM_KEYS = ("name", "src", "ext", "language")
# A plain word: what a source name, an extension and a pipeline name are, once they are known to be one entry
# inside their parent. It keeps a line break or a quote out of the report and off the compiler's command line.
WORD = re.compile(r"^[A-Za-z0-9_](?:[A-Za-z0-9_.+-]{0,62}[A-Za-z0-9_+-])?$")
MAX_SOURCES = 32
MAX_SOURCE_BYTES = 256 * 1024
# One Windows command line is 32767 characters and carries every input of the run (the probe's own bound).
MAX_SOURCES_B64 = 24000
MAX_UNPACKED = 2 * MAX_SOURCES * MAX_SOURCE_BYTES
MAX_STREAM_CHARS = 200000
# Below the runner's stall bound: this program prints each compile as it ends, and nothing while one runs.
COMPILE_SECONDS = 300
CONTROL_SECONDS = 120
SAID_LINES = 12


class Refusal(Exception):
    def __init__(self, code, text):
        super().__init__(text)
        self.code = code
        self.text = text


_REDACT = []


def redact_module():
    """`.harness-config/runner/actions/redact/redact.py` -- the one owner of the redaction rule, loaded ONCE by
    path from this file's sibling directory. It FAILS LOUD when absent rather than falling back to a local copy."""
    if not _REDACT:
        path = os.path.join(os.path.dirname(os.path.dirname(os.path.realpath(__file__))), "redact", "redact.py")
        if not os.path.isfile(path):
            raise SystemExit("compile-and-keep: cannot find %s -- the redaction rule lives there and nowhere else"
                             % path)
        spec = importlib.util.spec_from_file_location("dss_redact", path)
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        _REDACT.append(mod)
    return _REDACT[0]


def redactor(tree, places=None):
    """The one redactor for `tree`, with `places` ({label: directory}) marked like it. A host whose account
    cannot be named is a Refusal -- a line and an exit code, never a traceback."""
    mod = redact_module()
    try:
        return mod.redactor(tree, places=places)
    except mod.Unredactable as exc:
        raise Refusal(EXIT_REQUEST, str(exc))


try:
    hashlib.md5(b"", usedforsecurity=False)
    MD5_FLAGS = {"usedforsecurity": False}  # a fingerprint, never a secret: a FIPS host must not refuse it
except TypeError:                           # a Python older than the keyword
    MD5_FLAGS = {}


def md5_of(data):
    return hashlib.md5(data, **MD5_FLAGS).hexdigest()


def file_md5(path):
    """(size, md5) of a file, read in pieces: a compiler's binary can be large."""
    digest, size = hashlib.md5(b"", **MD5_FLAGS), 0
    with io.open(path, "rb") as f:
        for piece in iter(lambda: f.read(1 << 20), b""):
            digest.update(piece)
            size += len(piece)
    return size, digest.hexdigest()


def unpack_sources(b64):
    """-> the request's sources, each {"name", "src" (bytes), "ext", "language" (or None)}. Anything that does not
    decode to exactly that is refused WHOLE, before a compiler is asked anything."""
    if not b64:
        raise Refusal(EXIT_SOURCES, "no sources were given: --sources-b64 is empty")
    if len(b64) > MAX_SOURCES_B64:
        raise Refusal(EXIT_SOURCES, "--sources-b64 is %d characters, over the %d a run's inputs can carry on one "
                                    "command line: send the sources over two runs" % (len(b64), MAX_SOURCES_B64))
    text = b64.strip().replace("-", "+").replace("_", "/")  # the URL-safe alphabet, strictly, as the standard one
    try:
        raw = base64.b64decode((text + "=" * (-len(text) % 4)).encode("ascii"), validate=True)
    except (binascii.Error, ValueError, UnicodeEncodeError) as e:
        raise Refusal(EXIT_SOURCES, "--sources-b64 does not decode: it is not base64 (%s)" % e)
    if raw.lstrip(b" \t\r\n")[:1] != b"[":
        inflater = zlib.decompressobj()
        try:
            plain = inflater.decompress(raw, MAX_UNPACKED + 1)
        except zlib.error as e:
            raise Refusal(EXIT_SOURCES, "--sources-b64 does not decode: its bytes are neither a JSON array nor a "
                                        "zlib stream of one (%s)" % e)
        if len(plain) > MAX_UNPACKED or not inflater.eof:
            raise Refusal(EXIT_SOURCES, "--sources-b64 does not decode: its zlib stream is cut short, or inflates "
                                        "past the %d bytes a request may hold" % MAX_UNPACKED)
        raw = plain
    try:
        doc = json.loads(raw.decode("utf-8"))
    except (ValueError, UnicodeDecodeError) as e:
        raise Refusal(EXIT_SOURCES, "--sources-b64 does not decode to a UTF-8 JSON array: %s" % e)
    if not isinstance(doc, list) or not doc:
        raise Refusal(EXIT_SOURCES, "--sources-b64 must decode to a non-empty JSON array of {name, src, ext} "
                                    "objects (each optionally with language)")
    if len(doc) > MAX_SOURCES:
        raise Refusal(EXIT_SOURCES, "the request holds %d sources, over the %d one run compiles"
                      % (len(doc), MAX_SOURCES))
    items, seen = [], set()
    for index, item in enumerate(doc):
        if not isinstance(item, dict):
            raise Refusal(EXIT_SOURCES, "source %d is not an object" % index)
        unknown = sorted(set(item) - set(ITEM_KEYS))
        if unknown:
            raise Refusal(EXIT_SOURCES, "source %d carries unknown key(s) %s; the keys are %s"
                          % (index, unknown, ", ".join(ITEM_KEYS)))
        for key in ITEM_KEYS:
            if (key != "language" or key in item) and not isinstance(item.get(key), str):
                raise Refusal(EXIT_SOURCES, "source %d: %s must be a string" % (index, key))
        name = item["name"]
        try:
            data = item["src"].encode("utf-8")
        except UnicodeEncodeError as e:
            raise Refusal(EXIT_SOURCES, "source %r: its text is not text UTF-8 can carry (%s)" % (name, e))
        if not data:
            raise Refusal(EXIT_SOURCES, "source %r has no text" % name)
        if len(data) > MAX_SOURCE_BYTES:
            raise Refusal(EXIT_SOURCES, "source %r is %d bytes, over the %d-byte limit"
                          % (name, len(data), MAX_SOURCE_BYTES))
        # Without case: two names that differ by case alone share one directory where the file system ignores it,
        # and a request must mean the same on every host.
        if name.casefold() in seen:
            raise Refusal(EXIT_SOURCES, "the source name %r appears twice (names are compared without case)" % name)
        seen.add(name.casefold())
        items.append({"name": name, "src": data, "ext": item["ext"], "language": item.get("language")})
    return items


def packed_from(tree, origin):
    """The files `--from` names, in order: a comma-separated list of files and directories, a directory standing for
    every file directly inside it (by name), a relative path taken from the tree."""
    paths = []
    for entry in (origin or "").split(","):
        if not entry:
            raise Refusal(EXIT_REQUEST, "--from must name the source files to carry, comma-separated and none of "
                                        "them empty (%r)" % origin)
        path = entry if os.path.isabs(entry) else os.path.join(tree, entry)
        if os.path.isdir(path):
            inside = sorted(name for name in os.listdir(path) if os.path.isfile(os.path.join(path, name)))
            if not inside:
                raise Refusal(EXIT_SOURCES, "%s holds no file to carry" % path)
            paths.extend(os.path.join(path, name) for name in inside)
        else:
            paths.append(path)
    return paths


def pack_sources(paths):
    """The `--sources-b64` value for these files: name = the stem, ext = the extension, the JSON array compressed.
    It is read back through unpack_sources before it is returned, so a value a run would refuse is refused here."""
    doc = []
    for path in paths:
        stem, ext = os.path.splitext(os.path.basename(path))
        if not stem or len(ext) < 2:
            raise Refusal(EXIT_SOURCES, "%s: a source file is carried as <name>.<ext>, and this one has no "
                                        "name or no extension" % path)
        try:
            with io.open(path, "rb") as f:
                src = f.read().decode("utf-8")
        except OSError as e:
            raise Refusal(EXIT_SOURCES, "%s cannot be read: %s" % (path, e))
        except UnicodeDecodeError as e:
            raise Refusal(EXIT_SOURCES, "%s is not UTF-8 text, and the sources travel as JSON text: %s" % (path, e))
        doc.append({"name": stem, "src": src, "ext": ext[1:]})
    packed = zlib.compress(json.dumps(doc, ensure_ascii=False).encode("utf-8"), 9)
    value = base64.urlsafe_b64encode(packed).decode("ascii").rstrip("=")
    unpack_sources(value)
    return value


def split_arms(value):
    """The pipeline names of `--configs`, in the order given."""
    arms = (value or "").split(",")       # no name at all splits to one empty name
    if "" in arms:
        raise Refusal(EXIT_REQUEST, "--configs must name the pipelines to build, comma-separated and none of them "
                                    "empty (%r); `%s` is the arm that passes no --config at all" % (value, BASELINE))
    if len({arm.casefold() for arm in arms}) != len(arms):
        raise Refusal(EXIT_REQUEST, "--configs names a pipeline twice: %r" % value)
    return arms


def pipeline_args(arm):
    """What an arm adds to the compiler's command line: nothing for the baseline, else its one token."""
    return [] if arm == BASELINE else ["--config=" + arm]


def built_compiler(path):
    """The compiler's command, [the real path of the file `--dsscp` names] -- the file a build made, or a refusal.
    Never a lookup: a bare name could only be resolved by PATH, and that would measure whatever is installed."""
    if not path:
        raise Refusal(EXIT_NO_COMPILER, "no built dsscp was named: --dsscp is empty")
    if os.path.basename(path) == path:
        raise Refusal(EXIT_NO_COMPILER, "--dsscp %r is a bare program name, which only PATH could resolve: this "
                                        "action runs the compiler the leg BUILT, by its path in the leg's build "
                                        "directory, never one found on PATH" % path)
    if not os.path.isfile(path):
        raise Refusal(EXIT_NO_COMPILER, "no built dsscp on this leg: %s is not a file. Build the leg first "
                                        "(`dssharness build --legs <leg>`); through its runner this action is "
                                        "handed the leg's own product, which DssHarness builds before the step"
                      % path)
    return [os.path.realpath(path)]


def child(parent, name, what, where="the step's directory"):
    """`parent/name` -- THE ONE DOOR for every path this program writes or reads by a caller's word. Refused unless
    it is ONE entry directly inside `parent`, under its own spelling. The two halves hold different names: `..`,
    `.`, an empty name, a separator and an absolute or drive-relative path resolve under ANOTHER NAME; an entry
    that is a link resolves under its own name SOMEWHERE ELSE."""
    path = os.path.join(parent, name)
    real = os.path.realpath(path)
    if os.path.dirname(real) != os.path.realpath(parent) or os.path.basename(real) != name:
        raise Refusal(EXIT_PATH, "%s %r would leave %s: it does not name one entry directly inside %s"
                      % (what, name, where, parent))
    return path


def plain_word(value, what, code):
    if not WORD.match(value):
        raise Refusal(code, "%s %r is not a plain word (%s)" % (what, value, WORD.pattern))


def run_compiler(argv, cwd, env, seconds):
    """(exit code or a mark, stdout, stderr), each stream capped. A compiler that cannot be STARTED is a refusal:
    nothing was measured, and the system's own words say why."""
    try:
        p = subprocess.run(argv, cwd=cwd, env=env, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                           stderr=subprocess.PIPE, timeout=seconds)
        rc, out, err = p.returncode, p.stdout, p.stderr
    except subprocess.TimeoutExpired as e:
        rc, out, err = "timeout-%ds" % seconds, e.stdout or b"", e.stderr or b""
    except OSError as e:
        raise Refusal(EXIT_NO_COMPILER, "the built dsscp at %s cannot be started: %s" % (argv[0], e))

    def text(b):
        # One line ending in every report: a compiler on Windows ends its lines CR LF.
        s = b.decode("utf-8", errors="replace").replace("\x00", "").replace("\r\n", "\n").replace("\r", "\n")
        return s if len(s) <= MAX_STREAM_CHARS else s[:MAX_STREAM_CHARS] + "\n... %d more character(s) not kept" % (
            len(s) - MAX_STREAM_CHARS)
    return rc, text(out), text(err)


def said(out, err):
    """The first lines of what a compiler printed, for a refusal that quotes it."""
    lines = [ln for ln in (err + "\n" + out).split("\n") if ln.strip()]
    more = ["... %d more line(s)" % (len(lines) - SAID_LINES)] if len(lines) > SAID_LINES else []
    return "\n".join(lines[:SAID_LINES] + more) if lines else "(it printed nothing)"


def control(compiler, env, scratch, language, target, arm, ext):
    """Put the REQUEST to the compiler before any source: the same command line for a source that does not exist.
    Returns the exit code of an accepted request; a request the compiler does not accept is a Refusal carrying its
    own words. Accepted = the compiler got as far as the source, and said so by naming it."""
    absent = CONTROL + "." + ext
    if os.path.lexists(os.path.join(scratch, absent)):
        raise Refusal(EXIT_COMPILER_REFUSES, "the control source %s exists in %s, so asking for it proves nothing"
                      % (absent, scratch))
    argv = (compiler + ["--compile", absent, "--language", language, "--target", target] + pipeline_args(arm)
            + ["--output", os.path.join(scratch, CONTROL + "-output")])
    rc, out, err = run_compiler(argv, scratch, env, CONTROL_SECONDS)
    asked = "language %r, target %r, pipeline %s" % (language, target,
                                                      "`%s` (no --config)" % arm if arm == BASELINE
                                                      else "`--config=%s`" % arm)
    if rc == 0:
        raise Refusal(EXIT_COMPILER_REFUSES, "the built compiler exited 0 for a source that does not exist (%s), so "
                                             "its answer to a request cannot be told from its answer to a source"
                      % asked)
    if absent in out + err:
        return rc
    raise Refusal(EXIT_COMPILER_REFUSES, "the built compiler refuses the request before it reads a source (%s; "
                                         "exit %s). Its own words:\n%s" % (asked, rc, said(out, err)))


def served_kinds(tree, target):
    """(the format document's file name, the artifact profiles it declares or None when it declares none), read
    from the tree the compiler's documents are named in. The format is whatever follows the spec's first `:`."""
    formats = os.path.join(tree, "src", "dss-config", "object-formats")
    name = target.split(":", 1)[-1] + ".format.json"
    path = child(formats, name, "the target's format", "the formats directory")
    try:
        with io.open(path, encoding="utf-8") as f:
            doc = json.load(f)
    except (OSError, ValueError) as e:
        raise Refusal(EXIT_KIND, "the format document of the target %r cannot be read (%s): %s" % (target, path, e))
    kinds = doc.get("artifactProfiles") if isinstance(doc, dict) else None
    if kinds is not None and (not isinstance(kinds, list) or any(not isinstance(k, str) for k in kinds)):
        raise Refusal(EXIT_KIND, "%s: `artifactProfiles` is not a list of names" % path)
    return name, kinds


def check_kind(tree, target, kind):
    """The report's line for the kind, or a Refusal when a stated kind is not one the format serves."""
    try:
        name, kinds = served_kinds(tree, target)
    except Refusal as e:
        if kind:
            raise Refusal(EXIT_KIND, e.text)
        return "kind: not stated, and the format document was not read (%s)" % e.text
    serves = "serves the artifact profile(s) %s" % ", ".join(kinds) if kinds else "declares no artifact profile"
    if not kind:
        return "kind: not stated; %s %s" % (name, serves)
    if not kinds or kind not in kinds:
        raise Refusal(EXIT_KIND, "the kind %r is not one the target makes: %s %s (its `artifactProfiles`)%s"
                      % (kind, name, serves, "" if kinds else ", so no kind can be stated for it"))
    return "kind: %s, one of the profile(s) %s serves (%s)" % (kind, name, ", ".join(kinds))


def listing(directory):
    """[(name relative to `directory`, size, md5)] for every file under it, sorted; [] when it is not there."""
    found = []
    for base, dirs, files in os.walk(directory):
        dirs.sort()
        for name in sorted(files):
            path = os.path.join(base, name)
            found.append((os.path.relpath(path, directory).replace(os.sep, "/"),) + file_md5(path))
    return found


def compile_and_keep(tree, compiler, sources_b64, language, target, configs, kind, step_dir, emit,
                     compile_seconds=COMPILE_SECONDS):
    """Compile every source under every arm and keep what each compile left. `emit(line)` takes each report line
    as it is known. Returns (the kept directory, the counts); raises Refusal before any source is compiled and
    before anything is kept."""
    for value, option in ((tree, "--tree"), (language, "--language"), (target, "--target"), (step_dir, "--step-dir")):
        if not value:
            raise Refusal(EXIT_REQUEST, "%s is empty" % option)
    arms = split_arms(configs)
    items = unpack_sources(sources_b64)
    # THE PLAN: every path, made through the one door, before a directory exists.
    scratch = child(step_dir, SOURCES, "the scratch directory")
    kept = child(step_dir, KEPT, "the kept directory")
    images = child(kept, IMAGES, "the image directory")
    rows = []
    for item in items:
        source_dir = child(scratch, item["name"], "the source name")
        source = child(source_dir, item["name"] + "." + item["ext"], "the source file name")
        item_dir = child(images, item["name"], "the source name")
        rows.extend((item, arm, source_dir, source, item_dir, child(item_dir, arm, "the pipeline name"))
                    for arm in arms)
    for item in items:
        plain_word(item["name"], "the source name", EXIT_SOURCES)
        plain_word(item["ext"], "the extension of source %r" % item["name"], EXIT_SOURCES)
    for arm in arms:
        plain_word(arm, "the pipeline name", EXIT_REQUEST)
    for used in (scratch, kept):
        if os.path.lexists(used):
            raise Refusal(EXIT_REQUEST, "%s exists already: a step's directory is this run's alone" % used)
    env = dict(os.environ, DSS_CONFIG_ROOT=tree)
    os.makedirs(scratch)
    # THE REQUEST, put to the compiler before any source (see the header): once per (language, arm).
    accepted = []
    for item in items:
        for arm in arms:
            asked = (item["language"] or language, arm)
            if asked not in [a[:2] for a in accepted]:
                accepted.append(asked + (control(compiler, env, scratch, asked[0], target, arm, item["ext"]),))
    kind_line = check_kind(tree, target, kind)
    emit("compile-and-keep: compiler %s (the executable: %d bytes, md5 %s)"
         % ((compiler[-1],) + file_md5(compiler[-1])))
    emit("compile-and-keep: documents DSS_CONFIG_ROOT=%s" % tree)
    emit("compile-and-keep: target %s; %s" % (target, kind_line))
    emit("compile-and-keep: pipelines %s" % ", ".join(
        "%s (%s)" % (arm, " ".join(pipeline_args(arm)) or "no --config token") for arm in arms))
    for asked_language, arm, rc in accepted:
        emit("compile-and-keep: request accepted for language %s, pipeline %s (the control exited %s naming its "
             "absent source)" % (asked_language, arm, rc))
    os.makedirs(images)
    counts = {"compiles": 0, "built": 0, "failed": 0, "empty": 0, "images": 0}
    for item, arm, source_dir, source, item_dir, out_dir in rows:
        if not os.path.isdir(source_dir):
            os.makedirs(source_dir)
            with io.open(source, "wb") as f:
                f.write(item["src"])
        os.makedirs(item_dir, exist_ok=True)
        argv = (compiler + ["--compile", os.path.basename(source), "--language", item["language"] or language,
                            "--target", target] + pipeline_args(arm) + ["--output", out_dir])
        rc, out, err = run_compiler(argv, source_dir, env, compile_seconds)
        files = listing(out_dir)
        emit("=== %s [%s] source %s, %d byte(s), md5 %s" % (item["name"], arm, os.path.basename(source),
                                                            len(item["src"]), md5_of(item["src"])))
        emit("== compile: exit %s" % rc)
        for stream, body in (("stdout", out), ("stderr", err)):
            if body.strip():
                emit("-- %s:" % stream)
                for line in body.rstrip("\n").split("\n"):
                    emit(line)
        for name, size, digest in files:
            emit("== image %s: %d byte(s), md5 %s" % (name, size, digest))
        if not files:
            emit("== image: NONE (the compile exited %s and left no file)" % rc)
        counts["compiles"] += 1
        counts["images"] += len(files)
        counts["built" if rc == 0 and files else "empty" if rc == 0 else "failed"] += 1
    return kept, counts


def closing_line(counts):
    return ("compile-and-keep: OK compiles=%(compiles)d built=%(built)d failed=%(failed)d empty=%(empty)d "
            "images=%(images)d" % counts)


def bring_back(tree, leg, kept_at, kept):
    """The ONE line that brings every kept file back, or None when the run does not say where its kept files go.
    `kept_at` is where this leg's kept outputs of the step go; `sync --pull` takes each FILE by its path relative
    to the tree. Kept files that are not inside the tree are said, never named by a path --pull would refuse."""
    if not (leg and kept_at):
        return None
    try:
        base = os.path.relpath(os.path.join(kept_at, KEPT), tree).replace(os.sep, "/")
    except ValueError:                      # another drive
        base = "../"
    if base == ".." or base.startswith("../"):
        return ("compile-and-keep: the kept files go to %s, which is not inside the tree, so no `sync --pull` can "
                "name them" % kept_at)
    names = [REPORT] + [IMAGES + "/" + name for name, _, _ in listing(os.path.join(kept, IMAGES))]
    return ("compile-and-keep: bring these %d file(s) back with: dssharness sync --legs %s %s"
            % (len(names), leg, " ".join("--pull %s/%s" % (base, name) for name in names)))


def run(a, compiler=None, out=None, places=None):
    """One request, end to end -> its exit code. The report is printed as it is made and kept when it is whole."""
    out = out or sys.stdout
    lines = []
    try:
        shown = redactor(a.tree or None, places)
    except Refusal as e:
        out.write("compile-and-keep: REFUSED (exit %d) - %s\n" % (e.code, e.text))
        return e.code

    def emit(line):
        lines.append(shown(line))
        out.write(lines[-1] + "\n")
        out.flush()

    try:
        kept, counts = compile_and_keep(a.tree, compiler or built_compiler(a.dsscp), a.sources_b64, a.language,
                                        a.target, a.configs, a.kind, a.step_dir, emit)
    except Refusal as e:
        out.write(shown("compile-and-keep: REFUSED (exit %d) - %s" % (e.code, e.text)) + "\n")
        return e.code
    back = bring_back(a.tree, a.leg, a.kept_at, kept)
    if back:
        emit(back)
    emit(closing_line(counts))
    with io.open(os.path.join(kept, REPORT), "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")
    return 0


# ── THE SELF-TEST ─────────────────────────────────────────────────────────────────────────────────────────────
# The stand-in compiler: it answers this program's command line the way the measured compiler does -- a request it
# does not know is refused before any source and without naming one, a source that is not there is named -- and
# what it does with a source is written in the source's own text. It records every call it gets.
STAND_IN = r'''
import hashlib, json, os, sys, time
a = sys.argv[1:]
with open(os.environ["COMPILE_AND_KEEP_CALLS"], "a", encoding="utf-8") as f:
    f.write(json.dumps({"argv": a, "cwd": os.getcwd(), "root": os.environ.get("DSS_CONFIG_ROOT")}) + "\n")
value = lambda flag: a[a.index(flag) + 1]
src, target, out = value("--compile"), value("--target"), value("--output")
config = [x.split("=", 1)[1] for x in a if x.startswith("--config=")]
if config == ["unknownpipeline"]:
    sys.stderr.write("error: --config: not a recognized configuration\n")
    sys.exit(2)
if target == "unknown:fmt-exec":
    sys.stderr.write("error[NoSuchTarget]: no shipped target config found\n")
    sys.exit(1)
if target == "blind:fmt-exec":
    sys.exit(0)
if not os.path.isfile(src):
    sys.stderr.write("error: failed to open '%s' for reading\n" % src)
    sys.exit(1)
text = open(src, "rb").read()
if b"FAILS" in text:
    sys.stdout.buffer.write(b"note: one line on stdout\r\n")
    sys.stdout.flush()
    sys.stderr.write("error: does not compile, in %s\n" % os.path.abspath(src))
    sys.exit(1)
if b"LEAVES-NOTHING" in text:
    sys.exit(0)
if b"HANGS" in text:
    time.sleep(60)
stem = os.path.splitext(os.path.basename(src))[0]
os.makedirs(out)
for name in [stem + ".img"] + ([stem + ".map"] if b"TWO-FILES" in text else []):
    with open(os.path.join(out, name), "wb") as f:
        f.write(("%s|%s|%s|%s" % (name, target, config, hashlib.md5(text).hexdigest())).encode())
sys.stderr.write("artifact %s %s\n" % (target, os.path.join(out, stem + ".img")))
'''

# ★ AN EXACT RATCHET PER BLOCK: an arm lost from a block, or a block that silently stopped running, is a failure,
# never a smaller green.
EXPECTED_FIXED = 107
EXPECTED_LIVE = 5


def b64json(doc):
    return base64.urlsafe_b64encode(json.dumps(doc).encode("utf-8")).decode("ascii").rstrip("=")


def selftest(tree=None, dsscp=None, live_example=None, live_target=None):
    failures, ran = [0], {"fixed": 0, "live": 0}
    block = ["fixed"]
    holder = tempfile.TemporaryDirectory(prefix="dss-compile-and-keep-")
    box = os.path.realpath(holder.name)
    fake_tree = os.path.join(box, "tree")
    formats = os.path.join(fake_tree, "src", "dss-config", "object-formats")
    os.makedirs(formats)
    for name, doc in (("fmt-exec", {"artifactProfiles": ["cli"]}), ("fmt-lib", {"artifactProfiles": ["lib", "module"]}),
                      ("fmt-obj", {}), ("fmt-bad", {"artifactProfiles": "cli"})):
        with io.open(os.path.join(formats, name + ".format.json"), "w", encoding="utf-8") as f:
            json.dump(doc, f)
    stand_in = os.path.join(box, "stand-in.py")
    with io.open(stand_in, "w", encoding="utf-8", newline="\n") as f:
        f.write(STAND_IN)
    calls_path = os.path.join(box, "calls.jsonl")
    os.environ["COMPILE_AND_KEEP_CALLS"] = calls_path
    fake = [sys.executable, stand_in]
    places = {"<scratch>": box}
    shown = redactor(fake_tree, places)
    serial = [0]

    last = ["(none)"]

    def check(name, ok, detail=""):
        ran[block[0]] += 1
        last[0] = name
        print("compile-and-keep selftest: %-4s %s" % ("ok" if ok else "FAIL", name)
              + ("" if ok or not detail else "\n" + shown(str(detail))))
        failures[0] += 0 if ok else 1

    def calls():
        if not os.path.isfile(calls_path):
            return []
        with io.open(calls_path, encoding="utf-8") as f:
            return [json.loads(line) for line in f if line.strip()]

    def request(sources, target="arch:fmt-exec", configs="baseline,opt", kind="", language="lang", compiler=fake,
                seconds=COMPILE_SECONDS, the_tree=fake_tree, step=None):
        """One request against the stand-in -> (exit code, report lines, the step directory, the calls it made)."""
        serial[0] += 1
        step = step or os.path.join(box, "step-%03d" % serial[0])
        os.makedirs(step, exist_ok=True)
        if os.path.isfile(calls_path):
            os.remove(calls_path)
        lines = []
        try:
            kept, counts = compile_and_keep(the_tree, compiler, sources if isinstance(sources, str) else b64json(sources),
                                            language, target, configs, kind, step, lines.append, seconds)
            lines.append(closing_line(counts))
            rc = 0
        except Refusal as e:
            lines.append("REFUSED (exit %d) - %s" % (e.code, e.text))
            rc = e.code
        except Exception as e:      # a crash is THIS arm's failure, by name, and the arms after it still run
            lines.append("CRASHED - %r" % (e,))
            rc = "crashed"
        return rc, lines, step, calls()

    def attempt(call, *args):
        """What a call returns, or its refusal or its crash as text: an arm fails by NAME, whatever its subject does."""
        try:
            return call(*args)
        except Refusal as e:
            return "REFUSED (exit %d) - %s" % (e.code, e.text)
        except Exception as e:
            return "CRASHED - %r" % (e,)

    def link(target, at):
        """A directory link at `at`: a symbolic link, or a junction where Windows withholds the privilege."""
        try:
            os.symlink(target, at, target_is_directory=True)
        except OSError:
            subprocess.run(["cmd", "/c", "mklink", "/J", at, target], stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL)

    def refused(result, code, *wanted):
        rc, lines, step, made = result
        return rc == code and all(w in lines[-1] for w in wanted)

    def untouched(result):
        """A refused request wrote nothing: its step directory is as empty as it was made."""
        return os.listdir(result[2]) == []

    one = [{"name": "alpha", "src": "int alpha;\n", "ext": "x"}]
    two = one + [{"name": "beta", "src": "int beta;\n", "ext": "y"}]
    try:
        # ── A. what a good request does ──
        good = request(two)
        rc, lines, step, made = good
        kept = os.path.join(step, KEPT)
        expect = [("alpha", "baseline", []), ("alpha", "opt", ["opt"]), ("beta", "baseline", []), ("beta", "opt", ["opt"])]
        check("two sources under two pipelines: four compiles, four images, the closing line counts them",
              rc == 0 and lines[-1] == "compile-and-keep: OK compiles=4 built=4 failed=0 empty=0 images=4", lines)
        check("each image is kept at kept/images/<name>/<arm>/ under the name the compiler gave it",
              all(os.path.isfile(os.path.join(kept, IMAGES, n, arm, n + ".img")) for n, arm, _ in expect),
              listing(kept))
        images = {name: (size, digest) for name, size, digest in listing(os.path.join(kept, IMAGES))}
        check("the report gives every image's size and md5 as the kept file's own",
              len(images) == 4 and all("== image %s: %d byte(s), md5 %s" % (os.path.basename(n), s, d) in lines
                                       for n, (s, d) in images.items()), lines)
        check("the report gives every source's size and md5",
              "=== alpha [baseline] source alpha.x, 11 byte(s), md5 %s" % md5_of(b"int alpha;\n") in lines, lines)
        compiles = [c for c in made if not c["argv"][1].startswith(CONTROL)]
        check("the baseline arm passes NO --config token; a named arm passes exactly --config=<name>",
              [[x for x in c["argv"] if x.startswith("--config")] for c in compiles]
              == [[], ["--config=opt"], [], ["--config=opt"]], compiles)
        check("every compile is `--compile <name>.<ext> --language <l> --target <spec> [--config=<arm>] --output <dir>`",
              [c["argv"] for c in compiles]
              == [["--compile", n + "." + e, "--language", "lang", "--target", "arch:fmt-exec"]
                  + (["--config=opt"] if arm == "opt" else []) + ["--output", os.path.join(kept, IMAGES, n, arm)]
                  for n, e in (("alpha", "x"), ("beta", "y")) for arm in ("baseline", "opt")], compiles)
        check("every call of the compiler names the tree as DSS_CONFIG_ROOT",
              bool(made) and all(c["root"] == fake_tree for c in made), made)
        check("a source is compiled from its own directory, by its file name alone",
              all(c["cwd"] == os.path.join(step, SOURCES, c["argv"][1].split(".")[0]) for c in compiles), compiles)
        check("the request is put to the compiler ONCE per (language, arm), before the first source",
              [c["argv"][1] for c in made[:2]] == [CONTROL + ".x"] * 2 and len(made) == 6
              and [[x for x in c["argv"] if x.startswith("--config")] for c in made[:2]] == [[], ["--config=opt"]],
              made)
        check("the report says which requests the compiler accepted",
              sum("request accepted for language lang" in ln for ln in lines) == 2, lines)
        check("the sources are written beside the kept directory, never inside it",
              sorted(os.listdir(step)) == [KEPT, SOURCES] and os.path.isdir(kept)
              and sorted(os.listdir(kept)) == [IMAGES], (os.listdir(step), listing(kept)))
        own = request(one + [{"name": "gamma", "src": "g\n", "ext": "x", "language": "other"}], configs="baseline")
        check("a source may name its own language, and that request is put to the compiler too",
              own[0] == 0 and [(c["argv"][1], c["argv"][3]) for c in own[3]]
              == [(CONTROL + ".x", "lang"), (CONTROL + ".x", "other"), ("alpha.x", "lang"), ("gamma.x", "other")],
              own[3])

        # ── B. outcomes that are measurements, never refusals ──
        mixed = request([{"name": "bad", "src": "FAILS\n", "ext": "x"}, {"name": "void", "src": "LEAVES-NOTHING\n", "ext": "x"},
                         {"name": "pair", "src": "TWO-FILES\n", "ext": "x"}], configs="baseline")
        check("a compile that fails is reported and the run goes on: counted failed, its exit code kept",
              mixed[0] == 0 and mixed[1][-1] == "compile-and-keep: OK compiles=3 built=1 failed=1 empty=1 images=2"
              and "== compile: exit 1" in mixed[1], mixed[1])
        check("what a failing compile printed is kept, each stream under its name",
              "== compile: exit 1" in mixed[1] and ["-- stdout:", "note: one line on stdout", "-- stderr:"]
              == mixed[1][mixed[1].index("== compile: exit 1") + 1:][:3], mixed[1])
        check("a failing compile's entry says it left no file",
              "== image: NONE (the compile exited 1 and left no file)" in mixed[1], mixed[1])
        check("a compile that exits 0 and leaves nothing is counted empty, and says so",
              "== image: NONE (the compile exited 0 and left no file)" in mixed[1], mixed[1])
        check("every file a compile leaves is listed, not the first alone",
              sum(ln.startswith("== image pair.") for ln in mixed[1]) == 2, mixed[1])
        slow = request([{"name": "slow", "src": "HANGS\n", "ext": "x"}, {"name": "after", "src": "a\n", "ext": "x"}],
                       configs="baseline", seconds=2)
        check("a compile that outlives its bound is ended, reported and counted failed; the next one still runs",
              slow[0] == 0 and "== compile: exit timeout-2s" in slow[1]
              and slow[1][-1] == "compile-and-keep: OK compiles=2 built=1 failed=1 empty=0 images=1", slow[1])

        # ── C. the sources that do not decode (exit 5), each before the compiler is asked anything ──
        plain_items = attempt(unpack_sources, b64json(two))
        packed = base64.urlsafe_b64encode(zlib.compress(json.dumps(two).encode("utf-8"))).decode("ascii")
        check("a zlib stream of the JSON array decodes to the same sources",
              isinstance(plain_items, list) and len(plain_items) == 2 and attempt(unpack_sources, packed) == plain_items,
              attempt(unpack_sources, packed))
        # A text whose two spellings DIFFER: a run of `?` and a run of `>` put both characters the alphabets disagree
        # on into it at any alignment, and its length is no multiple of three, so the standard spelling is padded.
        tricky = next(text for text in (json.dumps([{"name": "t", "src": "?????>>>>>" + "a" * extra, "ext": "x"}])
                                        .encode("utf-8") for extra in range(3)) if len(text) % 3)
        url_safe = base64.urlsafe_b64encode(tricky).decode("ascii").rstrip("=")
        standard = base64.b64encode(tricky).decode("ascii")
        check("the standard alphabet with padding and the URL-safe one without decode alike",
              "-" in url_safe and "_" in url_safe and "+" in standard and "/" in standard and standard.endswith("=")
              and isinstance(attempt(unpack_sources, url_safe), list)
              and attempt(unpack_sources, url_safe) == attempt(unpack_sources, standard),
              (url_safe, attempt(unpack_sources, url_safe), attempt(unpack_sources, standard)))
        undecodable = [
            ("no sources at all", "", "is empty"),
            ("text that is not base64", "not base64!!", "not base64"),
            ("base64 of bytes that are neither JSON nor zlib", base64.b64encode(b"\x00\x01\x02\x03").decode(),
             "neither a JSON array nor a zlib stream"),
            ("a zlib stream cut short", base64.b64encode(zlib.compress(json.dumps(two).encode())[:-6]).decode(),
             "cut short"),
            ("a zlib stream that inflates past the bound",
             base64.b64encode(zlib.compress(b" " * (MAX_UNPACKED + 2))).decode(), "inflates past"),
            ("JSON that is not an array", b64json({"name": "a"}), "neither a JSON array"),
            ("a JSON array cut short", base64.b64encode(b'[{"name": "a"').decode(), "to a UTF-8 JSON array"),
            ("an empty array", b64json([]), "non-empty JSON array"),
            ("an array of something else", b64json(["text"]), "is not an object"),
            ("a source with an unknown key", b64json([dict(one[0], flags="-O2")]), "unknown key(s) ['flags']"),
            ("a source with no extension stated", b64json([{"name": "a", "src": "x"}]), "ext must be a string"),
            ("a source whose text is not a string", b64json([{"name": "a", "src": 7, "ext": "x"}]),
             "src must be a string"),
            ("a source whose language is not a string", b64json([dict(one[0], language=7)]),
             "language must be a string"),
            ("a source with no text", b64json([{"name": "a", "src": "", "ext": "x"}]), "has no text"),
            ("a source over the size limit", base64.b64encode(zlib.compress(json.dumps(
                [{"name": "a", "src": "x" * (MAX_SOURCE_BYTES + 1), "ext": "x"}]).encode())).decode(), "over the"),
            ("a name twice, differing by case alone", b64json(one + [dict(one[0], name="ALPHA")]), "appears twice"),
            ("more sources than one run compiles",
             base64.b64encode(zlib.compress(json.dumps([dict(one[0], name="n%d" % i) for i in range(MAX_SOURCES + 1)])
                                            .encode())).decode(), "over the %d one run compiles" % MAX_SOURCES),
            ("more characters than a run's inputs carry", "A" * (MAX_SOURCES_B64 + 1), "over the %d" % MAX_SOURCES_B64),
            ("text UTF-8 cannot carry", base64.b64encode(b'[{"name":"a","src":"\\ud800","ext":"x"}]').decode(),
             "not text UTF-8 can carry"),
        ]
        for name, value, want in undecodable:
            result = request(value)
            check("REFUSED, exit 5, nothing asked of the compiler, nothing written: " + name,
                  refused(result, EXIT_SOURCES, want) and result[3] == [] and untouched(result), result[1])

        # ── D. a path that would leave the step's directory (exit 6) ──
        leaving = [
            ("a source named `..`", [dict(one[0], name="..")], "baseline", "the source name '..'"),
            ("a source named `.`", [dict(one[0], name=".")], "baseline", "the source name '.'"),
            ("a source name that climbs out of the step", [dict(one[0], name="../../out")], "baseline",
             "the source name"),
            ("a source name with a separator", [dict(one[0], name="in/side")], "baseline", "the source name"),
            ("a source name that ends in a separator", [dict(one[0], name="alpha/")], "baseline",
             "the source name 'alpha/'"),
            ("a source named by an absolute path", [dict(one[0], name=os.path.join(box, "elsewhere"))], "baseline",
             "the source name"),
            ("an extension that climbs out", [dict(one[0], ext="x/../../../out")], "baseline", "the source file name"),
            ("a pipeline named `..`", one, "baseline,..", "the pipeline name '..'"),
            ("a pipeline name with a separator", one, "a/b", "the pipeline name 'a/b'"),
        ]
        for name, sources, configs, want in leaving:
            result = request(sources, configs=configs)
            check("REFUSED, exit 6, nothing asked of the compiler, nothing written: " + name,
                  refused(result, EXIT_PATH, want, "would leave the step's directory") and result[3] == []
                  and untouched(result), result[1])
        check("nothing was written beside the step directories by any of them",
              {e for e in os.listdir(box) if not e.startswith("step-")} == {"stand-in.py", "tree"}, os.listdir(box))
        try:
            child(box, "tree", "an entry")
            inside = True
        except Refusal:
            inside = False
        check("the door lets through a plain entry of its parent", inside)
        # A name that holds no separator and still resolves elsewhere UNDER ITS OWN NAME: an entry that is a link.
        linked_step = os.path.join(box, "step-linked")
        os.makedirs(os.path.join(linked_step, SOURCES))
        os.makedirs(os.path.join(box, "step-link-target", "linked"))
        link(os.path.join(box, "step-link-target", "linked"), os.path.join(linked_step, SOURCES, "linked"))
        result = request([dict(one[0], name="linked")], configs="baseline", step=linked_step)
        check("REFUSED, exit 6, nothing asked of the compiler: a source name that is a link to a directory of the same "
              "name elsewhere", os.path.isdir(os.path.join(linked_step, SOURCES, "linked"))
              and refused(result, EXIT_PATH, "the source name 'linked'", "would leave the step's directory")
              and result[3] == [], result[1])

        # ── E. a malformed request (exit 2) ──
        malformed = [
            ("no pipeline named", dict(configs=""), "--configs must name"),
            ("an empty pipeline name among them", dict(configs="baseline,,opt"), "--configs must name"),
            ("a pipeline named twice", dict(configs="opt,baseline,OPT"), "names a pipeline twice"),
            ("a pipeline name that is not a plain word", dict(configs="baseline,o pt"), "is not a plain word"),
            ("no language", dict(language=""), "--language is empty"),
            ("no target", dict(target=""), "--target is empty"),
        ]
        for name, over, want in malformed:
            result = request(one, **over)
            check("REFUSED, exit 2, nothing asked of the compiler: " + name,
                  refused(result, EXIT_REQUEST, want) and result[3] == [] and untouched(result), result[1])
        for name, item in (("a source name that is not a plain word", dict(one[0], name="two words")),
                           ("a source name ending in a dot", dict(one[0], name="alpha.")),
                           ("an extension that is not a plain word", dict(one[0], ext="x y"))):
            result = request([item])
            check("REFUSED, exit 5, nothing asked of the compiler: " + name,
                  refused(result, EXIT_SOURCES, "is not a plain word") and result[3] == [] and untouched(result),
                  result[1])
        reused = request(one, configs="baseline")
        again = request(one, configs="baseline", step=reused[2])
        check("a step directory that holds a run already is refused, its kept report untouched",
              refused(again, EXIT_REQUEST, "exists already") and again[3] == [], again[1])

        # ── F. the built compiler (exit 3) ──
        for name, value, want in (("none named", "", "--dsscp is empty"),
                                  ("a bare program name", "dsscp", "is a bare program name"),
                                  ("a path that is not there", os.path.join(box, "absent", "dsscp"), "is not a file"),
                                  ("a directory", fake_tree, "is not a file")):
            try:
                built_compiler(value)
                answer = "accepted"
            except Refusal as e:
                answer = (e.code, e.text)
            check("REFUSED, exit 3: " + name, answer != "accepted" and answer[0] == EXIT_NO_COMPILER
                  and want in answer[1], answer)
        check("a file a build made is taken by its real path, alone on the command line",
              attempt(built_compiler, os.path.join(fake_tree, os.pardir, "stand-in.py")) == [stand_in],
              attempt(built_compiler, os.path.join(fake_tree, os.pardir, "stand-in.py")))
        inert = os.path.join(box, "inert-compiler")
        with io.open(inert, "w", encoding="utf-8") as f:
            f.write("not a program\n")
        unstartable = request(one, compiler=[inert])
        check("REFUSED, exit 3, in the system's own words, nothing kept: a compiler that cannot be started",
              refused(unstartable, EXIT_NO_COMPILER, "cannot be started: ") and not os.path.exists(
                  os.path.join(unstartable[2], KEPT)), unstartable[1])

        # ── G. a request the built compiler refuses (exit 4), before any source ──
        for name, over, want in (
                ("a target it does not know", dict(target="unknown:fmt-exec"),
                 ("refuses the request before it reads a source", "target 'unknown:fmt-exec'", "exit 1",
                  "error[NoSuchTarget]: no shipped target config found")),
                ("a pipeline it does not know", dict(configs="baseline,unknownpipeline"),
                 ("refuses the request before it reads a source", "`--config=unknownpipeline`", "exit 2",
                  "error: --config: not a recognized configuration")),
                ("a compiler that exits 0 for a source that is not there", dict(target="blind:fmt-exec"),
                 ("exited 0 for a source that does not exist",))):
            result = request(two, **over)
            check("REFUSED, exit 4, in the compiler's own words, no source compiled, nothing kept: " + name,
                  refused(result, EXIT_COMPILER_REFUSES, *want)
                  and all(c["argv"][1].startswith(CONTROL) for c in result[3])
                  and not os.path.exists(os.path.join(result[2], KEPT)), result[1])
        planted = os.path.join(box, "step-planted")
        os.makedirs(os.path.join(planted, SOURCES))
        with io.open(os.path.join(planted, SOURCES, CONTROL + ".x"), "w") as f:
            f.write("x\n")
        answer = attempt(control, fake, dict(os.environ, DSS_CONFIG_ROOT=fake_tree), os.path.join(planted, SOURCES),
                         "lang", "arch:fmt-exec", "baseline", "x")
        check("the control refuses to ask for a source that exists",
              isinstance(answer, str) and answer.startswith("REFUSED (exit 4) - ") and "proves nothing" in answer,
              answer)

        # ── H. the kind (exit 7) ──
        served = request(one, configs="baseline", kind="cli")
        check("a kind the target's format serves is accepted, and the report says which document said so",
              served[0] == 0 and "compile-and-keep: target arch:fmt-exec; kind: cli, one of the profile(s) "
                                 "fmt-exec.format.json serves (cli)" in served[1], served[1])
        unstated = request(one, configs="baseline", target="arch:fmt-lib")
        check("with no kind stated the report still says what the format serves",
              unstated[0] == 0 and "compile-and-keep: target arch:fmt-lib; kind: not stated; fmt-lib.format.json "
                                   "serves the artifact profile(s) lib, module" in unstated[1], unstated[1])
        plain = request(one, configs="baseline", target="arch:fmt-obj")
        check("a format that declares no profile is compiled with no kind stated",
              plain[0] == 0 and any("fmt-obj.format.json declares no artifact profile" in ln for ln in plain[1]),
              plain[1])
        for name, over, want in (
                ("a kind the format does not serve", dict(kind="lib"),
                 ("the kind 'lib' is not one the target makes", "fmt-exec.format.json serves the artifact profile(s) cli")),
                ("a kind stated for a format that declares none", dict(kind="cli", target="arch:fmt-obj"),
                 ("declares no artifact profile", "so no kind can be stated for it")),
                ("a kind where the format document is not there", dict(kind="cli", target="arch:fmt-absent"),
                 ("cannot be read",)),
                ("a kind where the document's list is not a list", dict(kind="cli", target="arch:fmt-bad"),
                 ("`artifactProfiles` is not a list of names",)),
                ("a kind where the spec's format is not one entry of the formats directory",
                 dict(kind="cli", target="arch:sub/fmt-exec"), ("would leave the formats directory",))):
            result = request(one, configs="baseline", **over)
            check("REFUSED, exit 7, no source compiled, nothing kept: " + name,
                  refused(result, EXIT_KIND, *want) and all(c["argv"][1].startswith(CONTROL) for c in result[3])
                  and not os.path.exists(os.path.join(result[2], KEPT)), result[1])
        unread = request(one, configs="baseline", target="arch:fmt-absent")
        check("with no kind stated a format document that is not there is said, and the compile still runs",
              unread[0] == 0 and any("kind: not stated, and the format document was not read" in ln for ln in unread[1]),
              unread[1])

        # ── I. the whole program: its printed report, its kept one, redaction, its exit codes ──
        def whole(sources_b64, target="arch:fmt-exec", configs="baseline", kind="", leg="", kept_at=""):
            serial[0] += 1
            step = os.path.join(box, "step-%03d" % serial[0])
            os.makedirs(step)
            said_ = io.StringIO()
            a = argparse.Namespace(tree=fake_tree, dsscp="", sources_b64=sources_b64, language="lang", target=target,
                                   configs=configs, kind=kind, step_dir=step, leg=leg, kept_at=kept_at)
            try:
                rc = run(a, compiler=fake, out=said_, places=places)
            except Exception as e:  # as in request(): a crash is this arm's failure, by name
                rc = "crashed: %r" % (e,)
            return rc, said_.getvalue(), step

        rc, printed, step = whole(b64json([{"name": "bad", "src": "FAILS\n", "ext": "x"}] + one))
        report = os.path.join(step, KEPT, REPORT)
        check("the kept report is what was printed, to the line, and ends with the closing line",
              rc == 0 and os.path.isfile(report) and io.open(report, encoding="utf-8").read() == printed
              and printed.rstrip("\n").split("\n")[-1].startswith("compile-and-keep: OK compiles=2 built=1 failed=1 "),
              printed)
        check("a path the compiler prints is redacted in the report: the scratch is a mark, never its real place",
              "<scratch>" in printed and box not in printed and box.replace(os.sep, "/") not in printed, printed)
        check("a run that is not told where its kept files go prints no command for them",
              "bring these" not in printed and "sync --pull" not in printed, printed)
        rc, printed, step = whole(b64json(two), configs="baseline,opt", leg="some-leg",
                                  kept_at=os.path.join(fake_tree, "held", "here"))
        told = printed.rstrip("\n").split("\n")
        check("told the leg and where its kept files go, the report's last line but one names EVERY kept file for "
              "`sync --pull`, relative to the tree",
              rc == 0 and told[-1].startswith("compile-and-keep: OK compiles=4 ") and told[-2]
              == "compile-and-keep: bring these 5 file(s) back with: dssharness sync --legs some-leg "
                 + " ".join("--pull held/here/kept/" + name for name in
                            ["compile-and-keep.txt", "images/alpha/baseline/alpha.img", "images/alpha/opt/alpha.img",
                             "images/beta/baseline/beta.img", "images/beta/opt/beta.img"])
              and io.open(os.path.join(step, KEPT, REPORT), encoding="utf-8").read() == printed, printed)
        rc, printed, step = whole(b64json(one), leg="some-leg", kept_at=os.path.join(box, "not-the-tree"))
        check("kept files that are not inside the tree are SAID, never named by a path --pull would refuse",
              rc == 0 and "which is not inside the tree, so no `sync --pull` can name them" in printed
              and "--pull " not in printed, printed)
        rc, printed, step = whole("not base64!!")
        check("a refusal is ONE line naming its exit code, the program exits with it, and no report is kept",
              rc == EXIT_SOURCES and printed.startswith("compile-and-keep: REFUSED (exit 5) - ")
              and printed.count("\n") == 1 and os.listdir(step) == [], printed)
        rc, printed, step = whole(b64json(one), target="unknown:fmt-exec")
        check("the compiler's words reach the printed refusal",
              rc == EXIT_COMPILER_REFUSES and "error[NoSuchTarget]: no shipped target config found" in printed, printed)
        the_redactor = redact_module()
        kept_rule = the_redactor.redactor

        def nameless(*args, **kwargs):
            raise the_redactor.Unredactable("this host's account cannot be named")

        the_redactor.redactor = nameless
        try:
            rc, printed, step = whole(b64json(one))
        finally:
            the_redactor.redactor = kept_rule
        check("a host whose account cannot be named is REFUSED (exit 2), one line, nothing compiled or written",
              rc == EXIT_REQUEST and printed == "compile-and-keep: REFUSED (exit 2) - this host's account cannot be "
                                                "named\n" and os.listdir(step) == [], printed)
        with contextlib.redirect_stdout(io.StringIO()) as shown_:
            rc = main(["compile-and-keep.py", "--tree=" + fake_tree, "--dsscp=dsscp", "--sources-b64=" + b64json(one),
                       "--language=lang", "--target=arch:fmt-exec", "--configs=baseline",
                       "--step-dir=" + os.path.join(box, "step-main"), "--leg=some-leg",
                       "--kept-at=" + os.path.join(fake_tree, "held")])
        check("through main(): a bare compiler name exits 3 with its one line",
              rc == EXIT_NO_COMPILER and shown_.getvalue().startswith("compile-and-keep: REFUSED (exit 3) - "),
              shown_.getvalue())

        # ── J. the pack step ──
        files = os.path.join(box, "pack")
        os.makedirs(os.path.join(files, "nested"))
        for name, text in (("one.x", "first\n"), ("two.y", "second é\n")):
            with io.open(os.path.join(files, name), "w", encoding="utf-8", newline="\n") as f:
                f.write(text)
        both = [os.path.join(files, "one.x"), os.path.join(files, "two.y")]
        value = attempt(pack_sources, both)
        carried_items = attempt(unpack_sources, value)
        check("the pack carries each file as its stem, its extension and its bytes, and a run decodes it",
              isinstance(carried_items, list) and [(i["name"], i["ext"], i["src"]) for i in carried_items]
              == [("one", "x", b"first\n"), ("two", "y", "second é\n".encode("utf-8"))]
              and re.match(r"^[A-Za-z0-9_-]+$", value) is not None, (value, carried_items))
        check("a directory stands for every FILE directly inside it, by name; a relative path is taken from the tree",
              attempt(packed_from, box, "pack") == both and attempt(packed_from, box, files + ",pack/one.x")
              == both + [os.path.join(box, "pack/one.x")], attempt(packed_from, box, "pack"))

        def packing(call, *args):
            answer = attempt(call, *args)
            return answer if isinstance(answer, str) and answer.startswith(("REFUSED", "CRASHED")) else "packed"

        with io.open(os.path.join(box, "latin.x"), "wb") as f:
            f.write(b"caf\xe9\n")
        for name, paths, want in (("a file with no extension", [os.path.join(files, "one")], "no name or no extension"),
                                  ("a file that is not there", [os.path.join(files, "absent.x")], "cannot be read"),
                                  ("a file that is not UTF-8 text", [os.path.join(box, "latin.x")], "is not UTF-8 text"),
                                  ("two files of one stem", [os.path.join(files, "one.x")] * 2, "appears twice")):
            answer = packing(pack_sources, paths)
            check("the pack REFUSES (exit 5) " + name, answer.startswith("REFUSED (exit 5) - ") and want in answer,
                  answer)
        for name, origin, code, want in (("an empty path among them", "pack,,pack/one.x", 2, "none of them empty"),
                                         ("no path at all", "", 2, "none of them empty"),
                                         ("a directory that holds no file", "pack/nested", 5, "holds no file to carry")):
            answer = packing(packed_from, box, origin)
            check("the pack REFUSES (exit %d) %s" % (code, name), answer.startswith("REFUSED (exit %d) - " % code)
                  and want in answer, answer)
        carried = os.path.join(box, "carried.txt")
        with contextlib.redirect_stdout(io.StringIO()) as shown_:
            rc = main(["compile-and-keep.py", "--pack", "--tree=" + box, "--from=pack", "--out=" + carried])
        check("through main(): the pack writes the value ALONE on one line and says what it carried",
              rc == 0 and io.open(carried, encoding="ascii").read() == value + "\n" and shown_.getvalue()
              == "compile-and-keep: PACKED 2 source(s) into %d character(s): one.x, two.y\n" % len(value),
              shown_.getvalue())
        refused_out = os.path.join(box, "not-carried.txt")
        with contextlib.redirect_stdout(io.StringIO()) as shown_:
            rc = main(["compile-and-keep.py", "--pack", "--tree=" + box, "--from=pack/absent.x", "--out=" + refused_out])
        check("through main(): a pack that is refused exits with its code, one line, and writes no value",
              rc == EXIT_SOURCES and shown_.getvalue().startswith("compile-and-keep: REFUSED (exit 5) - ")
              and not os.path.exists(refused_out), shown_.getvalue())

        # ── K. the action file says what this program does ──
        here = os.path.dirname(os.path.realpath(__file__))
        with io.open(os.path.join(here, "compile-and-keep.yml"), encoding="utf-8") as f:
            yml = f.read()
        steps = re.split(r"(?m)^  - name: ", yml)[1:]
        by_name = {s.split("\n", 1)[0].strip(): s for s in steps}
        compile_step, self_step = by_name.get("compile", ""), by_name.get("self-test", "")
        run_line = next((ln.strip() for ln in compile_step.split("\n")
                         if ln.strip().startswith("python3 ./compile-and-keep.py ")), "")
        loose = re.findall(r"(--[a-z0-9-]+)(?=\s|$)", run_line)
        check("the .yml spells every option --name=value", bool(run_line) and not loose, "loose options: %r" % loose)
        check("the compile step is handed the leg's own product, its own step directory, its leg and where that "
              "leg's kept outputs of THIS step go",
              '--dsscp="{product}"' in run_line and '--step-dir="{stepBuild}"' in run_line
              and '--tree="{treeDir}"' in run_line and '--leg="{leg}"' in run_line
              and '--kept-at="{actionArtifacts}/compile"' in run_line, run_line)
        check("the step's declared output is the directory this program keeps",
              re.search(r"(?m)^    outputs: \[%s\]$" % KEPT, compile_step) is not None
              and re.search(r"(?m)^    persist: true$", compile_step) is not None, compile_step)
        check("the self-test step names no product, so it needs no build",
              "--selftest" in self_step and "{product}" not in self_step and "{buildDir}" not in self_step
              and re.search(r"(?m)^    manual: true$", self_step) is not None, self_step)
        check("every option the .yml passes is one the program takes",
              set(re.findall(r"(--[a-z0-9-]+)=", run_line))
              == {o for action in build_parser()._actions for o in action.option_strings} - {"-h", "--help"},
              run_line)
        pack_step = by_name.get("pack", "")
        pack_line = next((ln.strip() for ln in pack_step.split("\n")
                          if ln.strip().startswith("python3 ./compile-and-keep.py ")), "")
        check("the pack step is manual, names no product, and keeps the one file the program is told to write",
              re.search(r"(?m)^    manual: true$", pack_step) is not None and "{product}" not in pack_step
              and "{buildDir}" not in pack_step and '--out="{stepBuild}/sources_b64.txt"' in pack_line
              and re.search(r"(?m)^    outputs: \[sources_b64\.txt\]$", pack_step) is not None
              and re.search(r"(?m)^    persist: true$", pack_step) is not None, pack_step)
        check("the pack step's options are the pack's own, each spelled --name=value after the one verb",
              re.findall(r"(--[a-z0-9-]+)(?=\s|$)", pack_line) == ["--pack"]
              and set(re.findall(r"(--[a-z0-9-]+)=", pack_line)) | {"--pack"}
              == {o for action in pack_parser()._actions for o in action.option_strings} - {"-h", "--help"},
              pack_line)

        # ── L. the LIVE arms: the compiler a build made, a corpus example's own manifest ──
        if dsscp is not None:
            block[0] = "live"
            live(check, request, tree, dsscp, live_example, live_target)
    except Exception as e:      # never a traceback in place of a verdict: the crash is a failure, and it says where
        failures[0] += 1
        print("compile-and-keep selftest: FAIL an arm CRASHED the self-test, the one after %r: %s"
              % (last[0], shown(repr(e))))
    finally:
        os.environ.pop("COMPILE_AND_KEEP_CALLS", None)
        holder.cleanup()
    expected = {"fixed": EXPECTED_FIXED, "live": EXPECTED_LIVE if dsscp is not None else 0}
    miscount = {b: (ran[b], expected[b]) for b in ran if ran[b] != expected[b]}
    if miscount:
        failures[0] += 1
        print("compile-and-keep selftest: ARM COUNT per block (ran, expected) %r -- the EXPECTED_* constants are "
              "the ratchet" % miscount)
    total = sum(ran.values())
    note = ("; %d of them live, with the compiler a build made" % ran["live"] if dsscp is not None
            else "; the live arms are ctest's: no built compiler was named")
    print("compile-and-keep selftest: %s" % ("OK (%d arm(s)%s)" % (total, note) if failures[0] == 0
                                             else "FAIL - %d of %d arm(s)" % (failures[0], total)))
    return 1 if failures[0] else 0


def live(check, request, tree, dsscp, example, spec):
    """The live arms. Everything they compile is read from the example's own manifest: the language, the source,
    the artifact its row for `spec` names, and the shipped pipelines."""
    with io.open(os.path.join(tree, example, "expected.json"), encoding="utf-8") as f:
        manifest = json.load(f)
    rows = [t for t in manifest["targets"] if t["spec"] == spec]
    pipelines = [p["shippedPipeline"] for p in manifest.get("optimizedPipelines", [])]
    artifact = rows[0]["artifact"] if len(rows) == 1 else None
    stem, ext = os.path.splitext(manifest["source"])
    with io.open(os.path.join(tree, example, manifest["source"]), "rb") as f:
        source = [{"name": stem, "src": f.read().decode("utf-8"), "ext": ext[1:]}]
    arms = [BASELINE] + pipelines
    common = dict(language=manifest["language"], compiler=built_compiler(dsscp), the_tree=tree)
    built = request(source, target=spec, configs=",".join(arms), **common)
    check("live: the leg's own compiler builds the example for the target its manifest names, under the baseline "
          "and every shipped pipeline",
          artifact is not None and len(pipelines) >= 1 and built[0] == 0
          and built[1][-1].startswith("compile-and-keep: OK compiles=%d built=%d failed=0 empty=0 "
                                      % (len(arms), len(arms))), (spec, manifest.get("targets"), pipelines, built[1]))
    check("live: each arm's image is the artifact the manifest names, and nothing else was left",
          all([n for n, _, _ in listing(os.path.join(built[2], KEPT, IMAGES, stem, arm))] == [artifact]
              for arm in arms), listing(os.path.join(built[2], KEPT)))
    unknown = request(source, target="compile-and-keep-no-such-target:compile-and-keep-no-such-format",
                      configs=BASELINE, **common)
    check("live: a target no document names is REFUSED by the compiler's own answer (exit 4), nothing kept",
          unknown[0] == EXIT_COMPILER_REFUSES and "refuses the request before it reads a source" in unknown[1][-1]
          and not os.path.exists(os.path.join(unknown[2], KEPT)), unknown[1])
    pipeline = request(source, target=spec, configs="compile-and-keep-no-such-pipeline", **common)
    check("live: a pipeline the compiler does not know is REFUSED by its own answer (exit 4), nothing kept",
          pipeline[0] == EXIT_COMPILER_REFUSES and "`--config=compile-and-keep-no-such-pipeline`" in pipeline[1][-1]
          and not os.path.exists(os.path.join(pipeline[2], KEPT)), pipeline[1])
    try:
        kinds = served_kinds(tree, spec)[1]
    except Refusal as e:
        kinds = e.text
    stated = request(source, target=spec, configs=BASELINE, kind=kinds[0] if isinstance(kinds, list) and kinds else "",
                     **common)
    check("live: the kind the target's own format document declares is read from it, and accepted",
          isinstance(kinds, list) and len(kinds) >= 1 and stated[0] == 0
          and any("kind: %s, one of the profile(s) " % kinds[0] in ln for ln in stated[1]), (kinds, stated[1]))


def build_parser():
    ap = argparse.ArgumentParser(description="Compile the request's sources with the dsscp a leg built, for a stated "
                                             "target and each stated pipeline, and keep the images and a report.",
                                 allow_abbrev=False)
    ap.add_argument("--tree", required=True)
    ap.add_argument("--dsscp", required=True)
    ap.add_argument("--sources-b64", required=True)
    ap.add_argument("--language", required=True)
    ap.add_argument("--target", required=True)
    ap.add_argument("--configs", required=True)
    ap.add_argument("--kind", default="")
    ap.add_argument("--step-dir", required=True)
    ap.add_argument("--leg", required=True)
    ap.add_argument("--kept-at", required=True)
    return ap


def pack_parser():
    ap = argparse.ArgumentParser(description="The pack step: write the --sources-b64 value for source files.",
                                 allow_abbrev=False)
    ap.add_argument("--pack", action="store_true", required=True)
    ap.add_argument("--tree", required=True)
    ap.add_argument("--from", dest="origin", required=True)
    ap.add_argument("--out", required=True)
    return ap


def pack(a, out=None):
    """The pack step -> its exit code: the value alone on one line of `--out`, and one line saying what it holds."""
    out = out or sys.stdout
    shown = str
    try:
        shown = redactor(a.tree)
        paths = packed_from(a.tree, a.origin)
        value = pack_sources(paths)
    except Refusal as e:
        out.write(shown("compile-and-keep: REFUSED (exit %d) - %s" % (e.code, e.text)) + "\n")
        return e.code
    with io.open(a.out, "w", encoding="ascii", newline="\n") as f:
        f.write(value + "\n")
    out.write("compile-and-keep: PACKED %d source(s) into %d character(s): %s\n"
              % (len(paths), len(value), ", ".join(os.path.basename(path) for path in paths)))
    return 0


def selftest_parser():
    ap = argparse.ArgumentParser(description="The self-test; with all four options, its live arms too.",
                                 allow_abbrev=False)
    ap.add_argument("--selftest", action="store_true", required=True)
    for option in ("--tree", "--dsscp", "--live-example", "--live-target"):
        ap.add_argument(option)
    return ap


def main(argv):
    if argv[1:2] == ["--selftest"]:
        a = selftest_parser().parse_args(argv[1:])
        given = [v for v in (a.tree, a.dsscp, a.live_example, a.live_target) if v]
        if len(given) not in (0, 4):
            print("compile-and-keep: USAGE - --selftest takes --tree, --dsscp, --live-example and --live-target "
                  "together (the live arms), or none of them")
            return EXIT_REQUEST
        return selftest(a.tree, a.dsscp, a.live_example, a.live_target) if given else selftest()
    if argv[1:2] == ["--pack"]:
        return pack(pack_parser().parse_args(argv[1:]))
    return run(build_parser().parse_args(argv[1:]))


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
