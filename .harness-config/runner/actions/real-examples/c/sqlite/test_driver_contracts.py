#!/usr/bin/env python3
"""test_driver_contracts.py -- the SQLite corpus driver's LEG-CONTRACT suite (Step 0).

It replaced `test-driver-contracts.sh` and its Windows twin `test-driver-contracts.ps1` on
2026-09-21 (lane mig, part 4: no `.sh`/`.ps1` under the actions directory). The twins EXTRACTED
shell text out of two drivers and ran it; there is ONE Python driver now, so every contract is
pinned by CALLING the driver's own functions -- the very function objects production calls,
driving the real resume loop, the real ledger and the real gates -- and every pin is proven
non-vacuous by MUTATING a temporary COPY of the module that owns the guard (red-on-disable):

  DC-01 .. DC-22  green pins; each carries the UNION of both twins' arms for its subject, plus
                  the negatives neither twin had (report 09, section E.4);
  RD-01 .. RD-42  red arms; each MUTATES a copy of one driver module -- fail-closed: the witness
                  occurs EXACTLY once, the mutant bytes (or AST) differ, the witness is absent,
                  it parses, compiles and IMPORTS under a unique module name kept out of
                  sys.modules -- and re-runs the SAME pin, which must go red ON A CHECK THE ARM
                  NAMES. A pin that crashes against a mutant is a HARNESS failure, never a red;
                  a pin that skips makes its red arm a named skip, never a vacuous pass;
  MS-1 .. MS-10   the mutator's own self-test: a non-unique witness, a missing one, a no-op
                  transform (text and AST), a surviving witness, a syntax-breaking transform
                  and an import-breaking one are each REFUSED; a real mutation is accepted; and
                  the red verdict itself is fail-closed (vacuous, red elsewhere, red too wide
                  and crashed each FAIL; only a skipping pin skips).

Usage: `python test_driver_contracts.py` (no arguments). ASCII output, flushed per line; the
LAST line is exactly `passed=N failed=N skipped=N` -- one per check of a green pin, one per red
arm, one per mutator arm, one for the arm registry -- and the exit code is 0 only when failed=0.
`--child <scenario> <spec.json> ...` is internal: the pins whose property is "the PROCESS stops"
(DC-11, DC-18) and the one that needs a process chain (DC-20) re-enter this file in a child.

Nothing runs at import beyond `sys.dont_write_bytecode = True` (the action is
`requireInputsUnmoved`: no __pycache__ may appear beside it). Every temporary file lives under
the system temp directory and is removed with a read-only-bit-clearing handler; every child is
`sys.executable` with PYTHONDONTWRITEBYTECODE=1, stdin at EOF and a timeout. Stdlib only;
Python 3.9 syntax.

RETIRED with the second driver (report 09, section D), each property carried as named:
the cross-driver parity readings D2/C2 -> DC-06 over the one driver; the `.ps1` region-marker
arms -> there is nothing to pair; I2/G2 as twins -> DC-11; the "byte-identical in bash and
PowerShell" sentinel parity -> the literal itself, pinned behaviourally in DC-07; P6b/c/d's
PowerShell pipeline-leak shape -> DC-21 keeps the non-integer refusal it grew into; every
bash/pwsh extraction mechanism -> importing the module.
"""
from __future__ import annotations

import sys

# BEFORE any sibling import: the action is `requireInputsUnmoved`, so no __pycache__ may appear.
sys.dont_write_bytecode = True

import ast  # noqa: E402
import collections  # noqa: E402
import contextlib  # noqa: E402
import copy  # noqa: E402
import importlib  # noqa: E402
import importlib.util  # noqa: E402
import io  # noqa: E402
import json  # noqa: E402
import os  # noqa: E402
import re  # noqa: E402
import shutil  # noqa: E402
import stat  # noqa: E402
import subprocess  # noqa: E402
import tempfile  # noqa: E402
import time  # noqa: E402
import traceback  # noqa: E402
import types  # noqa: E402
import uuid  # noqa: E402

HERE = os.path.dirname(os.path.realpath(__file__))
THIS_FILE = os.path.realpath(__file__)
DASH = "\u2014"
CHILD_TIMEOUT_S = 240
RESOLVER_TIMEOUT_S = 300
READY_TIMEOUT_S = 60.0
SILENT_SENTINEL_TEXT = ("<SILENT: the fixture produced no diagnostic, no test result and no "
                        "test name>")
DECOY_CODE = ("import os, sys, time; f = open(sys.argv[1] + '.tmp', 'w'); f.write(str(os.getpid())); "
              "f.close(); os.replace(sys.argv[1] + '.tmp', sys.argv[1]); time.sleep(120)")


# ── output ───────────────────────────────────────────────────────────────────────────

class Out:
    """ASCII only (anything else escaped), flushed per line."""

    def __init__(self, stream):
        self.stream = stream

    def line(self, text=""):
        s = str(text).encode("ascii", "backslashreplace").decode("ascii")
        self.stream.write(s + "\n")
        self.stream.flush()


class Tally:
    def __init__(self):
        self.passed = self.failed = self.skipped = 0


def _short(v, n=900):
    s = v if isinstance(v, str) else repr(v)
    return s if len(s) <= n else s[:n] + "...<%d more>" % (len(s) - n)


class PinSkip(Exception):
    """The WHOLE pin cannot run on this host: a named, counted skip (never a pass)."""


class Checks:
    """One run of one pin. Every check has a short STABLE key, so a red arm can name the check it
    must turn red. `out` None = quiet: a run against a mutant, whose failures are the point."""

    def __init__(self, pin_id, out, tally):
        self.pin_id, self.out, self.tally = pin_id, out, tally
        self.results = collections.OrderedDict()
        self.skips = []

    def ck(self, key, label, ok, detail=""):
        ok = bool(ok)
        if key in self.results:
            raise AssertionError("check key %s/%s is used twice in one pin" % (self.pin_id, key))
        self.results[key] = (label, ok, detail)
        if self.out is not None:
            tag = "%s/%s %s" % (self.pin_id, key, label)
            if ok:
                self.tally.passed += 1
                self.out.line("  ok   " + tag)
            else:
                self.tally.failed += 1
                self.out.line("  FAIL " + tag)
                for dl in str(detail).splitlines()[:40]:
                    self.out.line("         " + dl)
        return ok

    def eq(self, key, label, want, got):
        return self.ck(key, label, want == got, "expected %s\ngot      %s" % (_short(want), _short(got)))

    def has(self, key, label, hay, needle):
        return self.ck(key, label, needle in str(hay), "%s\ndoes not contain %r" % (_short(str(hay)), needle))

    def lacks(self, key, label, hay, needle):
        return self.ck(key, label, needle not in str(hay), "%s\nCONTAINS %r" % (_short(str(hay)), needle))

    def skip(self, key, label, why):
        self.skips.append((key, label, why))
        if self.out is not None:
            self.tally.skipped += 1
            self.out.line("  skip %s/%s %s -- %s" % (self.pin_id, key, label, why))

    @property
    def failed_keys(self):
        return [k for k, (_l, ok, _d) in self.results.items() if not ok]


def is_refusal(exc):
    """A HarnessDie of ANY copy of sqlite_common (a mutant copy defines its own class)."""
    return any(k.__name__ in ("HarnessDie", "CloneLockBlocked") for k in type(exc).__mro__)


def refused(fn, *a, **kw):
    """-> (True, message) when `fn` REFUSES (HarnessDie), else (False, its value). Anything else
    propagates: a crash is a harness failure, never an answer."""
    try:
        return False, fn(*a, **kw)
    except Exception as exc:  # noqa: BLE001 -- re-raised unless it is a refusal
        if is_refusal(exc):
            return True, str(exc)
        raise


# ── files, temp dirs, environment ────────────────────────────────────────────────────

def read_bytes(path):
    with open(path, "rb") as fh:
        return fh.read()


def read_text(path):
    return read_bytes(path).decode("utf-8")


def write_bytes(path, data):
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    with open(path, "wb") as fh:
        fh.write(data if isinstance(data, bytes) else data.encode("utf-8"))
    return path


def write_json(path, obj):
    return write_bytes(path, json.dumps(obj, indent=1, sort_keys=True).encode("utf-8"))


def mkdtemp(tag):
    return tempfile.mkdtemp(prefix="dss-dtc-%s-" % re.sub(r"[^A-Za-z0-9]+", "", tag))


def _clear_readonly_and_retry(func, path, _exc):
    try:
        os.chmod(path, stat.S_IREAD | stat.S_IWRITE | stat.S_IEXEC)
    except OSError:
        pass
    try:
        func(path)
    except OSError:
        pass


def rmtree_hard(path):
    """Remove a temp tree, clearing read-only bits and retrying while a killed child lets go of its
    files (Windows keeps a dying image locked for a moment). -> True when gone."""
    for _ in range(80):
        if not os.path.lexists(path):
            return True
        try:
            if sys.version_info >= (3, 12):
                shutil.rmtree(path, onexc=_clear_readonly_and_retry)
            else:
                shutil.rmtree(path, onerror=_clear_readonly_and_retry)
        except OSError:
            pass
        if os.path.lexists(path):
            time.sleep(0.25)
    return not os.path.lexists(path)


@contextlib.contextmanager
def patched_environ(unset=(), **setv):
    """os.environ with `unset` removed and `setv` applied (None deletes), restored EXACTLY after."""
    saved = dict(os.environ)
    try:
        for k in unset:
            os.environ.pop(k, None)
        for k, v in setv.items():
            if v is None:
                os.environ.pop(k, None)
            else:
                os.environ[k] = str(v)
        yield
    finally:
        os.environ.clear()
        os.environ.update(saved)


def config_knobs(common_path):
    """Every environment NAME `sqlite_common.Config` reads -- its string constants shaped like one,
    read from its SOURCE, so a knob added there is scrubbed here without an edit."""
    tree = ast.parse(read_text(common_path))
    names = set()
    for node in ast.walk(tree):
        if isinstance(node, ast.ClassDef) and node.name == "Config":
            for sub in ast.walk(node):
                if isinstance(sub, ast.Constant) and isinstance(sub.value, str) \
                        and re.match(r"^[A-Z][A-Z0-9_]{2,}$", sub.value):
                    names.add(sub.value)
    if len(names) < 20:
        raise RuntimeError("found only %d environment knob(s) in sqlite_common.Config (%s): a scrubbed "
                           "environment that scrubs nothing would inherit an operator's settings"
                           % (len(names), sorted(names)))
    return sorted(names)


# ── loading a module: the shipped one, a private copy, or a mutant ────────────────────

def module_path(name):
    return os.path.join(HERE, name + ".py")


_CODE = {}


def exec_module(unique_name, source_text, source_path, real_path):
    """A module executed from `source_text` under `unique_name`, NEVER registered in sys.modules.
    `__file__` is the REAL path, so a module that locates its siblings by `__file__` still finds
    them from a mutant copy; `__dtc_source__` names the file the text came from. The compiled code
    is kept per (path, text): a private copy re-executes it rather than re-compiling 175 KB."""
    key = (source_path, source_text)
    code = _CODE.get(key)
    if code is None:
        code = _CODE[key] = compile(source_text, source_path, "exec", dont_inherit=True)
    spec = importlib.util.spec_from_loader(unique_name, loader=None, origin=source_path)
    mod = importlib.util.module_from_spec(spec)
    mod.__file__ = real_path
    mod.__dtc_source__ = source_path
    exec(code, mod.__dict__)
    return mod


def source_of(mod):
    return read_text(getattr(mod, "__dtc_source__", None) or mod.__file__)


_ALIASES = {}


def module_aliases(name):
    """{alias: sibling} for every top-level `import sqlite_x as Y` of module `name` (read from its
    source, so a new sibling dependency is followed without an edit here)."""
    if name not in _ALIASES:
        out = {}
        for node in ast.parse(read_text(module_path(name))).body:
            if isinstance(node, ast.Import):
                for a in node.names:
                    if a.name.startswith("sqlite_") and a.asname:
                        out[a.asname] = a.name
        _ALIASES[name] = out
    return _ALIASES[name]


Mutant = collections.namedtuple("Mutant", ["name", "mod", "path", "source"])


class Mods:
    """The modules one pin run sees: the SHIPPED ones, except the ONE a red arm mutated."""

    def __init__(self, mutant=None):
        self.mutant = mutant

    def overridden(self, name):
        return self.mutant is not None and self.mutant.name == name

    def mod(self, name):
        return self.mutant.mod if self.overridden(name) else importlib.import_module(name)

    def fresh(self, name, **attrs):
        """A PRIVATE copy of `name` (of the mutant, when that is the one mutated), its sibling
        aliases re-bound to the mutant where it names one, then `attrs` set -- so a pin may replace
        a collaborator without touching the module every other pin uses."""
        if self.overridden(name):
            src, spath = self.mutant.source, self.mutant.path
        else:
            spath = module_path(name)
            src = read_text(spath)
        m = exec_module("dtc_%s_%s" % (name, uuid.uuid4().hex[:10]), src, spath, module_path(name))
        for alias, dep in module_aliases(name).items():
            if self.overridden(dep):
                setattr(m, alias, self.mutant.mod)
        for k, v in attrs.items():
            setattr(m, k, v)
        return m

    def use(self, name):
        """`name` as the pin must see it: the shipped module, or a private copy when a sibling it
        imports is the one mutated."""
        if self.overridden(name):
            return self.mutant.mod
        if any(self.overridden(dep) for dep in module_aliases(name).values()):
            return self.fresh(name)
        return importlib.import_module(name)

    def override_spec(self):
        return None if self.mutant is None else {"name": self.mutant.name, "path": self.mutant.path}


# ── the fail-closed mutator ──────────────────────────────────────────────────────────

class MutationRefused(Exception):
    """A mutant that would prove nothing. Always a FAILURE of its arm, never a skip."""


def make_mutant(name, tag, workdir, witness, old=None, new=None, transform=None, ast_witness=None):
    """A mutant of module `name`, refused unless it is a mutant at all:
      (0) the witness occurs EXACTLY once in the shipped module (text; or `ast_witness(tree)`),
          and a text `old` occurs exactly once too;
      (1) the mutant differs -- its BYTES (text mode) or its AST (a `transform(tree)`);
      (2) the witness is ABSENT from the mutant;
      (3) it parses and compiles;
      (4) it IMPORTS under a unique module name, kept out of sys.modules.
    The mutant's text is written under `workdir` (never beside the driver)."""
    path = module_path(name)
    raw = read_bytes(path)
    src = raw.decode("utf-8")
    mpath = os.path.join(workdir, "%s.%s.mutant.py" % (name, re.sub(r"[^A-Za-z0-9]+", "", tag)))
    if transform is None:
        n = src.count(witness)
        if n != 1:
            raise MutationRefused("the WITNESS occurs %d time(s) in %s.py, needs exactly 1: %r. A "
                                  "non-unique witness cannot tell 'the guard survived' from 'a copy of "
                                  "the text survived'; a missing one targets code that no longer exists."
                                  % (n, name, witness))
        old = witness if old is None else old
        k = src.count(old)
        if k != 1:
            raise MutationRefused("the text to replace occurs %d time(s) in %s.py, needs exactly 1: %r"
                                  % (k, name, old))
        mutant = src.replace(old, new, 1)
        if mutant.encode("utf-8") == raw:
            raise MutationRefused("THE MUTATION DID NOT LAND: the mutant is byte-identical to %s.py. "
                                  "Refusing to report a red-on-disable over an unmodified file." % name)
        if witness in mutant:
            raise MutationRefused("the mutation changed %s.py but the WITNESS SURVIVES: %r -- something "
                                  "else was edited; the guard under test is still present."
                                  % (name, witness))
    else:
        tree = ast.parse(src, filename=path)
        n = ast_witness(tree)
        if n != 1:
            raise MutationRefused("the AST witness (%s) matches %d node(s) in %s.py, needs exactly 1"
                                  % (witness, n, name))
        mtree = transform(copy.deepcopy(tree))
        mutant = ast.unparse(ast.fix_missing_locations(mtree)) + "\n"
        if ast.dump(ast.parse(mutant)) == ast.dump(tree):
            raise MutationRefused("THE MUTATION DID NOT LAND: the mutant's AST is identical to %s.py's "
                                  "(only its formatting changed)." % name)
        if ast_witness(ast.parse(mutant)) != 0:
            raise MutationRefused("the AST witness (%s) SURVIVES the transform of %s.py" % (witness, name))
    try:
        ast.parse(mutant, filename=mpath)
        _CODE[(mpath, mutant)] = compile(mutant, mpath, "exec", dont_inherit=True)
    except SyntaxError as exc:
        raise MutationRefused("the mutant does not PARSE (%s), so any red it produced would be a syntax "
                              "error rather than the missing guard. Narrow the mutation." % exc)
    write_bytes(mpath, mutant.encode("utf-8"))
    unique = "dtc_mutant_%s_%s" % (name, uuid.uuid4().hex[:10])
    try:
        mod = exec_module(unique, mutant, mpath, path)
    except Exception as exc:  # noqa: BLE001 -- any import failure refuses the mutant
        raise MutationRefused("the mutant does not IMPORT (%s: %s), so no pin could exercise it"
                              % (type(exc).__name__, exc))
    if unique in sys.modules:
        raise MutationRefused("the mutant leaked into sys.modules as %s" % unique)
    return Mutant(name, mod, mpath, mutant)


# ── resolvers: the REAL one (memoised), a scripted one, and an in-process one ──────────

def memo_resolver(suite):
    C = suite.C

    class MemoResolver(C.Resolver):
        """The REAL resolver (harness_legs.py through its verb CLI), each distinct question asked
        ONCE per suite run: the verbs this suite asks are pure functions of their argv, and a red
        arm re-asks exactly what its green pin asked. Asked under the scrubbed environment."""

        def __init__(self):
            C.Resolver.__init__(self, suite.host, suite.arch)
            self.memo, self.asked = {}, []

        def call(self, args, catalogue=True, timeout=None, input_text=None, env_overrides=None):
            key = (tuple(str(a) for a in args), bool(catalogue), input_text,
                   tuple(sorted((env_overrides or {}).items())))
            self.asked.append(list(key[0]))
            if key not in self.memo:
                with patched_environ(unset=suite.knobs):
                    self.memo[key] = C.Resolver.call(self, args, catalogue=catalogue,
                                                     timeout=timeout or RESOLVER_TIMEOUT_S,
                                                     input_text=input_text, env_overrides=env_overrides)
            return self.memo[key]

    return MemoResolver()


class FakeResolver:
    """Answers ONLY what its pin scripted (`answer(args)` -> Result, or None = rc 64, loud) and
    records every question."""

    def __init__(self, answer, C, host="linux", arch="x86_64"):
        self.answer, self.C, self.host, self.arch = answer, C, host, arch
        self.asked = []

    @property
    def host_args(self):
        return ["--host-os", self.host, "--host-arch", self.arch]

    def call(self, args, catalogue=True, timeout=None, input_text=None, env_overrides=None):
        args = [str(a) for a in args]
        self.asked.append(args)
        got = self.answer(args)
        if got is None:
            return self.C.Result(64, "", "the contract suite's fake resolver was not scripted for: %s"
                                 % " ".join(args))
        return got


def deterministic_translator(calls):
    """report 09 E.7's injected translator: `X:\\a\\b` -> `/mnt/x/a/b`, the same on every host."""
    def translate(argv, timeout=None):
        calls.append(list(argv))
        raw = str(argv[-1])
        m = re.match(r"^([A-Za-z]):[\\/](.*)$", raw)
        if not m:
            return 1, "", "the contract suite's translator takes a drive path, got %r" % raw
        return 0, "/mnt/%s/%s\n" % (m.group(1).lower(), m.group(2).replace("\\", "/")), ""
    return translate


class InProcResolver:
    """harness_legs.py's REAL verb logic, run IN THIS PROCESS with its path translator INJECTED, so
    a launcher in another namespace (windows-to-wsl) is exercised on every host. It answers
    exactly what the CLI would; only the translator's spawn is replaced."""

    def __init__(self, suite, translator, host="windows", arch="x86_64"):
        self.suite, self.translator, self.host, self.arch = suite, translator, host, arch
        self.asked = []

    @property
    def host_args(self):
        return ["--host-os", self.host, "--host-arch", self.arch]

    def call(self, args, catalogue=True, timeout=None, input_text=None, env_overrides=None):
        hl = self.suite.hl
        args = [str(a) for a in args]
        self.asked.append(args)
        argv = (["--catalogue", os.path.join(HERE, "legs.json")] if catalogue else []) + args
        saved = hl._run_translator
        hl._run_translator = self.translator
        o, e = io.StringIO(), io.StringIO()
        try:
            with contextlib.redirect_stdout(o), contextlib.redirect_stderr(e):
                try:
                    rc = hl.main(argv)
                except SystemExit as exc:
                    rc = exc.code if isinstance(exc.code, int) else (0 if exc.code is None else 2)
        finally:
            hl._run_translator = saved
        return self.suite.C.Result(rc if isinstance(rc, int) else 0, o.getvalue(), e.getvalue())


class Proxy:
    """A module stand-in: every attribute is the wrapped module's, except the ones named here."""

    def __init__(self, wrapped, **over):
        self.__dict__["_wrapped"] = wrapped
        self.__dict__.update(over)

    def __getattr__(self, name):
        return getattr(self.__dict__["_wrapped"], name)


# ── what every pin run shares ────────────────────────────────────────────────────────

class Suite:
    """The SHIPPED modules, the REAL resolver (each question asked once) and what it answered."""

    def __init__(self):
        if HERE not in sys.path:
            sys.path.insert(0, HERE)
        self.C = importlib.import_module("sqlite_common")
        self.host, self.arch = self.C.host_os(), self.C.host_arch()
        self._cache = {}
        self.real = memo_resolver(self)

    def once(self, key, fn):
        if key not in self._cache:
            self._cache[key] = fn()
        return self._cache[key]

    @property
    def knobs(self):
        return self.once("knobs", lambda: config_knobs(module_path("sqlite_common")))

    @property
    def vocab(self):
        return self.once("vocab", lambda: list(
            importlib.import_module("build_and_test").read_vocabulary(self.real)))

    @property
    def classes_text(self):
        return self.once("classes", lambda: self.real.call(["--verdict-classes"]).out)

    def plan(self, host, arch, launchers):
        return self.once(("plan", host, arch, launchers), lambda: self.real.json(
            ["--plan", "--environment-probes", "skip", "--host-os", host, "--host-arch", arch,
             "--launchers-available", launchers, "--format", "json"],
            "the %s/%s leg plan" % (host, arch)))

    @property
    def stage_build(self):
        return self.once("stage-build", lambda: self.real.json(
            ["--stage-build", "--format", "json"], "the stage-build declaration"))

    @property
    def hl(self):
        path = os.path.join(HERE, "harness_legs.py")
        return self.once("hl", lambda: exec_module("dtc_harness_legs_%s" % uuid.uuid4().hex[:8],
                                                   read_text(path), path, path))


class Ctx:
    """One pin run: the suite, the modules it sees, its own temp dir."""

    def __init__(self, suite, mods, tmp):
        self.suite, self.M, self.tmp = suite, mods, tmp
        self.C = suite.C

    def log(self):
        return self.C.Log(io.StringIO())

    def sub(self, name):
        p = os.path.join(self.tmp, "%s-%s" % (name, uuid.uuid4().hex[:6]))
        os.makedirs(p)
        return p


def text(log):
    return log.stream.getvalue()


def warn_lines(log):
    return [ln for ln in text(log).splitlines() if ln.startswith(" ! ")]


def leg_d(label="elf64-x86_64", fmt="elf64-x86_64-linux-exec", os_key="linux", mode="native",
          launcher=(), spec=None, run_extra=None, build_extra=None, **top):
    """A resolved-plan leg object as `--plan --format json` shapes one (the fields the driver reads)."""
    parts = fmt.split("-")
    arch = parts[1] if len(parts) >= 4 else "x86_64"
    d = {"label": label, "spec": spec or "%s:%s" % (arch, fmt), "format": fmt, "targetArch": arch,
         "run": {"mode": mode, "launcher": list(launcher), "verdict": "", "detail": "",
                 "pathTranslation": "none", "envTransfer": "inherit", "env": {}},
         "build": {"configStageKey": os_key, "libraries": {"provider": "pinned-archive"}},
         "confoundsByName": [], "confoundsByEvidence": [], "executionEvidence": [],
         "abortConfounds": [], "confoundGating": "probed", "runDirectoryGating": "not-required",
         "confoundReport": ["contract-suite plan: this leg declares no confound row"],
         "confoundRows": []}
    d["run"].update(run_extra or {})
    d["build"].update(build_extra or {})
    d.update(top)
    return d


def make_run(x, log, out_dir):
    """A real `sqlite_common.Run`, its Config read under the SCRUBBED environment (every knob Config
    reads removed), so an operator's DSS_* setting cannot change what a pin measures."""
    C = x.C
    with patched_environ(unset=x.suite.knobs):
        cfg = C.Config()
    run = C.Run(cfg, log=log)
    os.makedirs(out_dir, exist_ok=True)
    run.out_dir = out_dir
    run.registry_glob = os.path.join(out_dir, "no-registry-in-the-contract-suite-*.md")
    return run


def fake_procs(x, sweep=None):
    """A recording stand-in for sqlite_procs (the real Sweep/Enumeration types): nothing is swept.
    `sweep(P, why, launcher_prefix)` -> the Sweep one sweep answers (default: it LOOKED and found
    nothing), so a pin can script what a sweep learnt while no process is ever touched."""
    P = importlib.import_module("sqlite_procs")
    calls = []

    def stop_our_fixtures(fixture_path, why, launcher_prefix=None, settle_s=20, log=None, enumerator=None,
                          sleep=None):
        calls.append(("stop", str(fixture_path), str(why), list(launcher_prefix or [])))
        if sweep is not None:
            return sweep(P, str(why), list(launcher_prefix or []))
        return P.Sweep(True, "the contract suite's recording procs")

    def our_fixture_pids(fixture_path, launcher_prefix=None, enumerator=None):
        calls.append(("pids", str(fixture_path), list(launcher_prefix or [])))
        return P.Enumeration([], True, "the contract suite's recording procs")

    def kill_tree(proc):
        calls.append(("kill_tree", getattr(proc, "pid", None)))

    return types.SimpleNamespace(stop_our_fixtures=stop_our_fixtures, our_fixture_pids=our_fixture_pids,
                                 kill_tree=kill_tree, Sweep=P.Sweep, Enumeration=P.Enumeration,
                                 calls=calls)


# ── children ─────────────────────────────────────────────────────────────────────────

def child_env(x, extra=None):
    e = dict((k, v) for k, v in os.environ.items() if k not in set(x.suite.knobs))
    e.update(PYTHONDONTWRITEBYTECODE="1", PYTHONUTF8="1", PYTHONIOENCODING="utf-8")
    for k, v in (extra or {}).items():
        if v is None:
            e.pop(k, None)
        else:
            e[k] = str(v)
    return e


def start_children(jobs):
    """Start every (key, argv, env) at once -> the running Popens, for `collect_children`."""
    procs = collections.OrderedDict()
    for key, argv, env in jobs:
        procs[key] = subprocess.Popen(argv, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                      stderr=subprocess.STDOUT, env=env)
    return procs


def run_children(jobs, timeout=CHILD_TIMEOUT_S):
    """Start every (key, argv, env) at once -> {key: (rc, merged output)}."""
    return collect_children(start_children(jobs), timeout)


def collect_children(procs, timeout=CHILD_TIMEOUT_S):
    """-> {key: (rc, merged output)}. A child that outlives `timeout` is killed (it is ours) and
    answers rc None."""
    deadline = time.monotonic() + timeout
    res = {}
    for key, p in procs.items():
        try:
            outb, _ = p.communicate(timeout=max(1.0, deadline - time.monotonic()))
            res[key] = (p.returncode, outb.decode("utf-8", "replace"))
        except subprocess.TimeoutExpired:
            p.kill()
            outb, _ = p.communicate()
            res[key] = (None, outb.decode("utf-8", "replace") + "\n<killed after %ss>" % timeout)
    return res


def await_ready(path, proc, seconds=READY_TIMEOUT_S):
    """The readiness handshake: the child writes its OWN pid, atomically. -> pid, or None."""
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        try:
            with open(path, "r", encoding="ascii") as fh:
                return int(fh.read().strip())
        except (OSError, ValueError):
            pass
        if proc.poll() is not None:
            return None
        time.sleep(0.05)
    return None


def reap(proc):
    if proc is None:
        return
    if proc.poll() is None:
        try:
            proc.kill()
        except OSError:
            pass
    try:
        proc.wait(timeout=30)
    except subprocess.TimeoutExpired:
        pass


def last_json_line(out):
    for line in reversed((out or "").splitlines()):
        line = line.strip()
        if line.startswith("{"):
            try:
                return json.loads(line)
            except ValueError:
                return None
    return None


# ═══════════════════════════════════════════════════════════════════════════════════════
# DC-01 -- the CLOSED verdict vocabulary, read by the driver's own reader
# ═══════════════════════════════════════════════════════════════════════════════════════

def pin_dc01(t, x):
    bt = x.M.mod("build_and_test")
    C = x.C
    toks = bt.read_vocabulary(x.suite.real)
    t.ck("V01", "the driver's OWN reader answers the vocabulary from its owner (--verdict-vocabulary)",
         len(toks) > 0, repr(toks))
    t.eq("V02", "every token it read is CLEAN (no CR, LF, TAB or space)", [],
         [tk for tk in toks if any(ch in tk for ch in "\r\n\t ")])

    def fake(outtext, rc=0, err=""):
        return FakeResolver(lambda a: C.Result(rc, outtext, err) if a == ["--verdict-vocabulary"] else None, C)
    crlf = bt.read_vocabulary(fake("ran\r\npoisoned\r\n"))
    t.eq("V03", "a resolver writing CRLF (Windows text-mode stdout) reads as clean tokens, on EVERY host",
         ["ran", "poisoned"], crlf)
    t.eq("V04", "...and so does a lone CR", ["ran", "poisoned"], bt.read_vocabulary(fake("ran\rpoisoned\r")))
    r, led = refused(C.Ledger, crlf, x.log())
    t.ck("V05", "...so the Ledger built from them KNOWS 'ran' (a kept CR made every legitimate token "
         "unknown)", (not r) and led.known("ran"), led)
    r, msg = refused(bt.read_vocabulary, fake("", 2, "harness_legs.py: FATAL: pinned failure"))
    t.ck("V06", "a resolver that FAILS is refused, never read as an empty vocabulary", r, msg)
    t.has("V07", "...naming its exit code", msg, "rc=2")
    empty = bt.read_vocabulary(fake("\r\n\n  \n"))
    r2, msg2 = refused(C.Ledger, empty, x.log())
    t.ck("V08", "an EMPTY answer yields no token, and the Ledger REFUSES an empty closed vocabulary",
         empty == [] and r2, (empty, msg2))
    t.has("V09", "...saying what it could not state", msg2, "CLOSED verdict vocabulary")
    need = ["ran", "poisoned", "skipped-by-runOn", "skipped-build-input-missing",
            "skipped-launcher-prerequisite-missing", "not-selected-by-runner"]
    t.eq("V10", "the real vocabulary carries every token the pins below record", [],
         [n for n in need if n not in toks])


# ═══════════════════════════════════════════════════════════════════════════════════════
# DC-02 -- both recorders are guarded by the closed vocabulary (the union of the A arms)
# ═══════════════════════════════════════════════════════════════════════════════════════

MAC_DETAIL = ("host darwin/arm64 cannot run x86_64:macho64-x86_64-darwin-exec natively; declared "
              "launcher 'arch -x86_64' is available")


def pin_dc02(t, x):
    C = x.M.mod("sqlite_common")
    vocab = x.suite.vocab

    def leg(label, **kw):
        return C.Leg(leg_d(label, **kw))
    log = C.Log(io.StringIO())
    led = C.Ledger(vocab, log)
    alpha = leg("alpha")
    led.set_leg(alpha, "skipped-by-runOn", "runOn excludes this host")
    t.eq("A01", "a CLASSIFIED leg verdict is recorded verbatim", ("skipped-by-runOn", "runOn excludes this host"),
         (alpha.verdict, alpha.verdict_detail))
    t.eq("A02", "...and costs no unclassified count", [], list(led.unclassified))
    mac = leg("macho64-x86_64", mode="launched", launcher=["arch", "-x86_64"])
    led.set_leg(mac, "", MAC_DETAIL)
    t.eq("A03", "an EMPTY leg token becomes poisoned", "poisoned", mac.verdict)
    t.eq("A04", "...is counted, by name", ["macho64-x86_64"], list(led.unclassified))
    t.ck("A05", "...its detail starts `HARNESS DEFECT:`", mac.verdict_detail.startswith("HARNESS DEFECT:"),
         mac.verdict_detail)
    t.has("A06", "...and it warns loudly", text(log), "HARNESS DEFECT")
    t.has("A07", "...naming the resolved run plan (mode and declared launcher)", text(log),
          "declared launcher 'arch -x86_64'")
    beta = leg("beta")
    led.set_leg(beta, "skipped-because-i-said-so", "made up")
    t.eq("A08", "a leg token OUTSIDE the closed vocabulary is poisoned", "poisoned", beta.verdict)

    log2 = C.Log(io.StringIO())
    led2 = C.Ledger(vocab, log2)
    gamma = leg("gamma")
    led2.unit_not_run(gamma, "skipped-by-runOn", "runOn excludes this host")
    t.eq("U01", "a CLASSIFIED unit not-run is recorded verbatim, em dash and all",
         "not run [skipped-by-runOn] %s runOn excludes this host" % DASH, gamma.unit_verdict)
    mac2 = leg("macho64-x86_64", mode="launched", launcher=["arch", "-x86_64"])
    led2.unit_not_run(mac2, "", MAC_DETAIL)
    t.ck("U02", "an EMPTY unit token never writes `not run []`: it becomes `not run [poisoned] -- HARNESS "
         "DEFECT: ...`", mac2.unit_verdict.startswith("not run [poisoned] %s HARNESS DEFECT: " % DASH)
         and not mac2.unit_verdict.startswith("not run []"), mac2.unit_verdict)
    t.eq("U03", "...is counted once, by name", ["macho64-x86_64"], list(led2.unclassified))
    t.eq("U04", "...and poisons the LEG verdict too (the stricter rule)", "poisoned", mac2.verdict)
    t.has("U05", "...warning HARNESS DEFECT", text(log2), "HARNESS DEFECT")
    t.has("U06", "...naming the resolved run plan", text(log2), "declared launcher 'arch -x86_64'")
    t.has("U07", "...and saying the WHOLE unit corpus did not run", text(log2), "ENTIRE unit corpus did not run")
    delta = leg("delta")
    led2.unit_not_run(delta, "skipped-because-i-said-so", "made up")
    t.ck("U08", "an OFF-vocabulary unit token is poisoned too",
         delta.unit_verdict.startswith("not run [poisoned] %s HARNESS DEFECT: " % DASH), delta.unit_verdict)

    log3 = C.Log(io.StringIO())
    led3 = C.Ledger(vocab, log3)
    eps = leg("eps")
    led3.set_leg(eps, "", "x")
    led3.unit_not_run(eps, "", "y")
    t.eq("U09", "both recorders poisoning ONE leg count it ONCE", ["eps"], list(led3.unclassified))
    zeta = leg("zeta", run_extra={"verdict": "skipped-by-runOn"})
    led3.marks_missing(zeta, "this leg is NOT built on this host", "no libtcl here")
    t.eq("M01", "a missing declared input is skipped-build-input-missing", "skipped-build-input-missing",
         zeta.verdict)
    t.has("M02", "...KEEPING the displaced run verdict in the detail", zeta.verdict_detail,
          "[and this host could not RUN it either: skipped-by-runOn]")
    eta = leg("eta")
    led3.marks_harness_defect(eta, "poisoned", "provider 'x' has no dispatch arm")
    t.eq("M03", "a harness defect is recorded poisoned", "poisoned", eta.verdict)
    t.has("M04", "...and says the run CANNOT exit 0", text(log3), "CANNOT exit 0")
    r, msg = refused(C.Ledger, [], C.Log(io.StringIO()))
    t.ck("E01", "a Ledger over an EMPTY vocabulary REFUSES", r, msg)
    t.eq("E02", "the ledger's dash is the em dash U+2014 (Step 9 reads it byte for byte)", DASH, C.DASH)


# ═══════════════════════════════════════════════════════════════════════════════════════
# DC-03 -- ONE run decision
# ═══════════════════════════════════════════════════════════════════════════════════════

def pin_dc03(t, x):
    C = x.M.mod("sqlite_common")

    def mk(mode, launcher=()):
        return C.Leg(leg_d("x", mode=mode, launcher=launcher))
    t.eq("R01", "run decision: native -> runnable", False, C.run_is_skipped(mk("native")))
    t.eq("R02", "run decision: launched (with a launcher) -> runnable", False,
         C.run_is_skipped(mk("launched", ["arch", "-x86_64"])))
    t.eq("R03", "run decision: skip -> skipped", True, C.run_is_skipped(mk("skip")))
    r, msg = refused(C.run_is_skipped, mk("launched", []))
    t.ck("R04", "a 'launched' leg with an EMPTY launcher argv REFUSES", r, msg)
    t.has("R05", "...naming the contradiction", msg, "EMPTY launcher argv")
    r, msg = refused(C.run_is_skipped, mk("teleport"))
    t.ck("R06", "an UNKNOWN run mode REFUSES (never read as runnable or skipped)", r, msg)
    t.has("R07", "...naming the mode", msg, "unknown run mode 'teleport'")
    r, msg = refused(C.run_is_skipped, mk(""))
    t.ck("R08", "an EMPTY run mode refuses too, spelled <empty>", r and "<empty>" in str(msg), msg)


# ═══════════════════════════════════════════════════════════════════════════════════════
# DC-04 -- the corpus-entry gates: three legitimate not-runs, and NO control-compiler gate
# ═══════════════════════════════════════════════════════════════════════════════════════

def pin_dc04(t, x):
    U = x.M.use("sqlite_units")
    C = x.C
    log = x.log()
    led = C.Ledger(x.suite.vocab, log)
    run = types.SimpleNamespace(log=log, ledger=led)
    mac = C.Leg(leg_d("macho64-x86_64", fmt="macho64-x86_64-darwin-exec", os_key="darwin", mode="launched",
                      launcher=["arch", "-x86_64"]))
    mac.tcl_lib, mac.fixture_built, mac.fixture, mac.cc = "/cache/libtcl8.6.dylib", True, "/out/testfixture", []
    t.eq("E01", "a BUILT, launchable leg with NO control compiler REACHES the corpus", True,
         U.corpus_entry(run, mac))
    t.has("E02", "...the missing control compiler is an info line, never a gate", text(log), "no CONTROL compiler")
    nolibs = C.Leg(leg_d("nolibs"))
    led.set_leg(nolibs, "skipped-build-input-missing", "no libtcl here")
    t.eq("E03", "no libraries -> NOT entered", False, U.corpus_entry(run, nolibs))
    t.ck("E04", "...its unit ledger names its own token",
         nolibs.unit_verdict.startswith("not run [skipped-build-input-missing] %s " % DASH), nolibs.unit_verdict)
    nofix = C.Leg(leg_d("nofix"))
    nofix.tcl_lib = "/cache/libtcl8.6.so"
    led.set_leg(nofix, "poisoned", "compile failed")
    t.eq("E05", "no fixture -> NOT entered", False, U.corpus_entry(run, nofix))
    t.eq("E06", "...recorded as poisoned: step 7 did not produce a fixture",
         "not run [poisoned] %s step 7 did not produce a fixture" % DASH, nofix.unit_verdict)
    noexec = C.Leg(leg_d("noexec", mode="skip",
                         run_extra={"verdict": "skipped-by-runOn", "detail": "runOn excludes this host"}))
    noexec.tcl_lib, noexec.fixture_built, noexec.fixture = "/cache/libtcl8.6.so", True, "/out/testfixture"
    t.eq("E07", "a host that cannot EXECUTE it -> NOT entered", False, U.corpus_entry(run, noexec))
    t.ck("E08", "...the unit ledger carries the run verdict",
         noexec.unit_verdict.startswith("not run [skipped-by-runOn] %s runOn excludes this host" % DASH),
         noexec.unit_verdict)
    t.eq("E09", "...and so does the LEG verdict", "skipped-by-runOn", noexec.verdict)
    t.has("E10", "...saying the fixture DID build", noexec.verdict_detail, "BUILT OK")
    withcc = C.Leg(leg_d("withcc"))
    withcc.tcl_lib, withcc.fixture_built, withcc.fixture, withcc.cc = "/l/libtcl.so", True, "/o/tf", ["gcc"]
    t.eq("E11", "a leg WITH a control compiler reaches it too (the control)", True, U.corpus_entry(run, withcc))
    t.eq("E12", "none of the legitimate not-runs was unclassified", [], list(led.unclassified))


# ═══════════════════════════════════════════════════════════════════════════════════════
# DC-05 -- the segment parser (the .sh D arms, the .ps1 C arms over CRLF, and the edges)
# ═══════════════════════════════════════════════════════════════════════════════════════

PRECOND_LINES = ["Can't find a usable init.tcl in the following directories: /opt/local/lib/tcl8.6 "
                 "/opt/local/lib/tcl8.6 ...",
                 "This probably means that Tcl wasn't installed properly.",
                 '    (procedure "tclInit" line 61)', "    invoked from within",
                 '"interp create tinterp"', '    (procedure "slave_test_script" line 4)']
HEALTHY_LINES = ["select1-1.1... Ok", "select1-1.2... Ok", "Time: select1.test 42 ms",
                 "misc7-7.0... Ok", "Time: misc7.test 11 ms",
                 "0 errors out of 192 tests on host Darwin 64-bit"]
CRASH_LINES = ["select1-1.1... Ok", "Time: select1.test 42 ms",
               "swarmvtabfault-1.1-oom-persistent.143...", "child process exited abnormally",
               '    (procedure "do_test" line 12)']
INERT_LINES = ["select1-1.1... Ok", "select1-1.2... Ok", "select1.test-closeallfiles... Ok",
               "select1.test-sharedcachesetting... Ok", "Time: select1.test 42 ms",
               "fts5aa.test-closeallfiles... Ok", "fts5aa.test-sharedcachesetting... Ok",
               "Time: fts5aa.test 2 ms", "! wherelimit-1.1 expected: [1]",
               "! wherelimit-1.1 got: [0]", "wherelimit.test-closeallfiles... Ok",
               "wherelimit.test-sharedcachesetting... Ok", "Time: wherelimit.test 7 ms",
               "0 errors out of 8 tests on host Linux 64-bit"]
D1 = "Can't find a usable init.tcl in the following directories: /opt/local/lib/tcl8.6 ..."
D2 = "child process exited abnormally"


def facts_tuple(f):
    return (tuple(f.files), tuple(f.inert), tuple(f.failures), f.summary, f.errors, f.total, f.permutation,
            f.last_test, f.ok, f.fail_markers, f.first_diag, tuple(f.blamed), f.gave_up)


def pin_dc05(t, x):
    K = x.M.mod("sqlite_corpus")

    def parse(name, lines, eol="\n"):
        p = write_bytes(os.path.join(x.tmp, name), (eol.join(lines) + eol).encode("utf-8"))
        return K.parse_segment(p)
    p = parse("precond.log", PRECOND_LINES)
    t.eq("D01", "precondition log: the diagnostic is captured VERBATIM", PRECOND_LINES[0], p.first_diag)
    t.eq("D02", "precondition log: ZERO files completed", 0, p.n_files)
    t.eq("D03", "precondition log: no summary line", "", p.summary)
    h = parse("healthy.log", HEALTHY_LINES)
    t.eq("D04", "healthy log: NO diagnostic is invented", "", h.first_diag)
    t.eq("D05", "healthy log: files counted", 2, h.n_files)
    t.eq("D06", "healthy log: last file", "misc7.test", h.last_file)
    t.eq("D07", "healthy log: the summary is the WHOLE line, host suffix and all",
         "0 errors out of 192 tests on host Darwin 64-bit", h.summary)
    t.eq("D08", "healthy log: ' Ok' tally", 3, h.ok)
    c = parse("crash.log", CRASH_LINES)
    t.eq("D09", "crash log: files completed > 0", 1, c.n_files)
    t.eq("D10", "crash log: the last test is named", "swarmvtabfault-1.1-oom-persistent.143", c.last_test)
    t.eq("D11", "crash log: a diagnostic is captured too", D2, c.first_diag)
    i = parse("inert.log", INERT_LINES)
    t.eq("D12", "inert log: three files completed", 3, i.n_files)
    t.eq("D13", "inert log: exactly ONE asserted nothing", 1, i.n_inert)
    t.eq("D14", "inert log: and it is the one that ran nothing, BY NAME", ["fts5aa.test"], list(i.inert))
    t.eq("D15", "inert log: a file that only FAILED is not inert (its marker counted)", 1, i.fail_markers)
    t.eq("D16", "healthy log: nothing is inert", 0, h.n_inert)
    pc = parse("precond-crlf.log", PRECOND_LINES[:5], "\r\n")
    t.eq("C01", "CRLF precondition log: the diagnostic is verbatim, no CR", PRECOND_LINES[0], pc.first_diag)
    t.eq("C02", "CRLF precondition log: ZERO files completed", 0, pc.n_files)
    t.eq("C03", "CRLF precondition log: no summary line", "", pc.summary)
    hc = parse("healthy-crlf.log", HEALTHY_LINES, "\r\n")
    t.eq("C04", "CRLF healthy log: NO diagnostic is invented", "", hc.first_diag)
    t.eq("C05", "CRLF healthy log: files counted", 2, hc.n_files)
    t.eq("C06", "CRLF healthy log: last file", "misc7.test", hc.last_file)
    t.eq("C07", "CRLF healthy log: the summary is the WHOLE line (no CR)",
         "0 errors out of 192 tests on host Darwin 64-bit", hc.summary)
    t.eq("C08", "CRLF healthy log: ' Ok' tally", 3, hc.ok)
    cc = parse("crash-crlf.log", CRASH_LINES, "\r\n")
    t.eq("C09", "CRLF crash log: files completed > 0", 1, cc.n_files)
    t.eq("C10", "CRLF crash log: the last test is named", "swarmvtabfault-1.1-oom-persistent.143", cc.last_test)
    t.eq("C11", "CRLF crash log: a diagnostic is captured too", D2, cc.first_diag)
    ic = parse("inert-crlf.log", INERT_LINES, "\r\n")
    t.eq("C12", "a CRLF log parses to EXACTLY the facts of its LF twin", facts_tuple(i), facts_tuple(ic))
    for key, n, cut in (("X01", 400, False), ("X02", 401, True)):
        raw = ("E" * (n - 2)) + "\tx"
        got = parse("diag-%d.log" % n, [raw]).first_diag
        flat = raw.replace("\t", " ")
        want = flat[:400] + " ...[truncated]" if cut else flat
        t.eq(key, "a %d-character diagnostic is %s (tabs -> spaces)" % (
            n, "capped at 400 + the ASCII suffix ' ...[truncated]'" if cut else "kept whole"), want, got)
    t.ck("X03", "the truncation suffix is ASCII (decided: the .ps1's; the .sh's U+2026 could split a "
         "UTF-8 sequence)", getattr(K, "TRUNCATION_SUFFIX", "").isascii()
         and K.TRUNCATION_SUFFIX == " ...[truncated]", repr(getattr(K, "TRUNCATION_SUFFIX", None)))


# ═══════════════════════════════════════════════════════════════════════════════════════
# DC-06 -- the declared capability set reaches EVERY stage build site: a spawn spy + a census
# ═══════════════════════════════════════════════════════════════════════════════════════

class _StageStop(Exception):
    """Raised by the spy at the LAST derivation: every spawn of Step 4 has been seen."""


def stage_census(src):
    """The AST census of sqlite_stage's own source: every make spawn, every configure argv, every
    run_configure call and every recipe derivation -> a dict of findings."""
    tree = ast.parse(src)
    parents = {}
    for node in ast.walk(tree):
        for ch in ast.iter_child_nodes(node):
            parents[ch] = node

    def enclosing(node):
        while node in parents:
            node = parents[node]
            if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
                return node
        return None

    def const(n):
        return n.value if isinstance(n, ast.Constant) and isinstance(n.value, str) else None

    def is_options(e):
        s = const(e)
        if s is not None:
            return s.startswith("OPTIONS=")
        return isinstance(e, ast.BinOp) and isinstance(e.op, ast.Add) \
            and (const(e.left) or "").startswith("OPTIONS=")

    def cname(c):
        f = c.func
        return f.id if isinstance(f, ast.Name) else (f.attr if isinstance(f, ast.Attribute) else "")

    def configure_path(e):
        return isinstance(e, ast.Call) and cname(e) in ("_j", "join") and bool(e.args) \
            and const(e.args[-1]) == "configure"
    make_sites, cfg_argvs, rc_calls, derivs = [], [], [], []
    for node in ast.walk(tree):
        if isinstance(node, ast.Call):
            nm = cname(node)
            a0 = node.args[0] if node.args else None
            if isinstance(a0, (ast.List, ast.Tuple)) and a0.elts and const(a0.elts[0]) == "make":
                make_sites.append({"line": node.lineno, "helper": nm == "run_to_log",
                                   "options": any(is_options(e) for e in a0.elts)})
            if nm == "run_configure":
                fn = enclosing(node)
                ok = len(node.args) >= 4 and isinstance(node.args[3], ast.Name) \
                    and node.args[3].id == "configure_args"
                flags_first = fn is not None and any(
                    isinstance(s, ast.AugAssign) and isinstance(s.target, ast.Name)
                    and s.target.id == "configure_args" and isinstance(s.value, ast.Attribute)
                    and s.value.attr == "configure_flags" and s.lineno < node.lineno
                    for s in ast.walk(fn))
                rc_calls.append({"line": node.lineno, "passes": ok, "flags_first": flags_first})
            if nm == "emit_recipe":
                kw = dict((k.arg, k.value) for k in node.keywords if k.arg)
                mv = kw.get("make_vars")
                derivs.append({"line": node.lineno, "options": isinstance(mv, (ast.Tuple, ast.List))
                               and any(is_options(e) for e in mv.elts)})
        if isinstance(node, (ast.List, ast.Tuple)) and node.elts and configure_path(node.elts[0]):
            fn = enclosing(node)
            cfg_argvs.append(fn.name if fn is not None else "<module>")
    rc_fns = [n for n in ast.walk(tree) if isinstance(n, ast.FunctionDef) and n.name == "run_configure"]
    helper = bool(rc_fns) and any(isinstance(c, ast.Call) and cname(c) == "run_to_log"
                                  for c in ast.walk(rc_fns[0]))
    return {"make": make_sites, "configure_argvs": cfg_argvs, "run_configure_spawns": helper,
            "rc_calls": rc_calls, "derivations": derivs}


def pin_dc06(t, x):
    C = x.C
    Sg = x.M.fresh("sqlite_stage")
    sb = x.suite.stage_build
    flags, mo, req = list(sb["configureFlags"]), sb.get("makeOptions", ""), list(sb["requiredDefines"])
    t.ck("S00", "the real stage-build declaration carries configure flags and required defines to check",
         bool(flags) and bool(req), sb)
    root = x.sub("stage")
    clone = os.path.join(root, "sqlite")
    write_bytes(os.path.join(clone, "configure"), b"#!/bin/sh\nexit 0\n")
    os.chmod(os.path.join(clone, "configure"), 0o755)
    write_bytes(os.path.join(clone, "test", "veryquick.test"), b"# the contract suite's tier\n")
    tclcfg = write_bytes(os.path.join(root, "fake-tclConfig.sh"), b"TCL_LIBS='-lpinlib'\n")
    spawns, derivations = [], []

    def spy_run_to_log(argv, cwd, log_path, env):
        argv = [str(a) for a in argv]
        spawns.append((argv, str(cwd)))
        write_bytes(log_path, b"the contract suite's spawn spy\n")
        if argv[0].replace("\\", "/").endswith("/configure"):
            ld = [a[len("LDFLAGS="):] for a in argv if a.startswith("LDFLAGS=")]
            write_bytes(os.path.join(cwd, "Makefile"), (
                "CC = cc\nOPT_FEATURE_FLAGS = %s\nTCL_CONFIG_SH = %s\nLDFLAGS.configure = %s\n"
                % (" ".join("-D" + d for d in req), tclcfg, " ".join(ld))).encode("utf-8"))
            write_bytes(os.path.join(cwd, "sqlite_cfg.h"), b"#define HAVE_CONTRACT_SUITE 1\n")
            return 0
        if argv[0] == "make":
            for target in ("testfixture", "sqlite3d"):
                if target in argv:
                    write_bytes(os.path.join(cwd, target), b"#!/bin/sh\necho spy\n")
                    os.chmod(os.path.join(cwd, target), 0o755)
            return 0
        return 127

    class RecipeRefused(Exception):
        pass

    class HarnessUsageError(Exception):
        pass

    def emit_recipe(**kw):
        derivations.append(dict(kw))
        bld, target = kw["build_dir"], kw["make_target"]
        tus = [clone + "/src/main.c"] + ([bld + "/shell.c"] if target == "sqlite3d" else [])
        defs = req + (["SQLITE_CORE"] if target == "sqlite3d" else [])
        for key, lines in (("out_tus", tus), ("out_defines", defs), ("out_includes", [clone + "/src"])):
            write_bytes(kw[key], "".join(ln + "\n" for ln in lines).encode("utf-8"))
        write_bytes(kw["recipe_file"], b"the contract suite's make -n\n")
        if target == "sqlite3d":
            raise _StageStop()
        return types.SimpleNamespace(summary="%s: the contract suite's spy" % target)
    base = types.SimpleNamespace(RecipeRefused=RecipeRefused, HarnessUsageError=HarnessUsageError,
                                 emit_recipe=emit_recipe)
    coh = types.SimpleNamespace(run_check=lambda dirs, checkout=None, require_cli=False, label=None,
                                out=None, err=None: 0)
    siblings = {"sqlite_base": base, "sqlite_coherence": coh}
    Sg._sibling = lambda name, what: siblings[name]
    Sg.run_to_log = spy_run_to_log
    Sg.discover_roots = lambda ctx: None
    Sg.tcl_inventory = lambda ctx: []
    Sg.ensure_tclsh = lambda ctx: ("/opt/pin/bin/tclsh8.6", "/opt/pin/lib/tclConfig.sh")
    Sg.ensure_dev_headers = lambda ctx: None
    Sg.tclsh_version = lambda ctx, interpreter="tclsh": "8.6"
    Sg.tcl_config_values = lambda ctx, cfg_path, names: dict(
        (n, "-lpinlib" if n == "TCL_LIBS" else "") for n in names)
    Sg.probe_link_l = lambda ctx, probe_cc, args: "-L/opt/pin/lib" in list(args)
    Sg.libdir_for = lambda ctx, name, probe_cc: "/opt/pin/lib"
    cfg = Sg.StageConfig(sqlite_dir=clone, out_dir=os.path.join(root, "out"), stage_build=sb, jobs=2,
                         tcl_version="8.6", host_os="linux", environ=dict(os.environ),
                         pkg_install=lambda a, b=None: None)
    log = C.Log(io.StringIO())
    stopped, died = False, ""
    try:
        Sg.stage(cfg, log=log, lock=types.SimpleNamespace(notes=[]))
    except _StageStop:
        stopped = True
    except Exception as exc:  # noqa: BLE001 -- a refusal is an observation; anything else re-raised
        if not is_refusal(exc):
            raise
        died = str(exc)
    t.ck("S01", "the spy saw the WHOLE Step-4 spawn sequence (the stage reached the CLI derivation)",
         stopped, died or text(log)[-1500:])
    configures = [a for a, _cwd in spawns if a[0].replace("\\", "/").endswith("/configure")]
    makes = [a for a, _cwd in spawns if a[0] == "make"]
    t.eq("S02", "configure was spawned twice through the one helper (the configure, the LDFLAGS repair)",
         2, len(configures))
    t.eq("S03", "EVERY configure carries EVERY declared configureFlags entry (legs.json stageBuild)", [],
         [(k, f) for k, a in enumerate(configures) for f in flags if f not in a])
    t.ck("S04", "the LDFLAGS re-run carries the repair too", len(configures) == 2
         and "LDFLAGS=-L/opt/pin/lib" in configures[1], configures)
    t.eq("S05", "the three stage make targets were spawned, EACH carrying OPTIONS=<declared makeOptions>",
         [("sqlite3.c", True), ("testfixture", True), ("sqlite3d", True)],
         [(a[2] if len(a) > 2 and a[1] == "-s" else a[1], ("OPTIONS=" + mo) in a) for a in makes])
    t.eq("S06", "both recipe derivations pass OPTIONS=<makeOptions> as a make variable",
         [("testfixture", True), ("sqlite3d", True)],
         [(d["make_target"], ("OPTIONS=" + mo) in tuple(d.get("make_vars") or ())) for d in derivations])
    cen = stage_census(source_of(Sg))
    t.ck("C01", "census: the matcher still FINDS make spawn sites (it can see its subject)",
         len(cen["make"]) >= 1, cen["make"])
    t.eq("C02", "census: every make spawn goes through run_to_log, the ONE spawn helper", [],
         [m["line"] for m in cen["make"] if not m["helper"]])
    t.eq("C03", "census: every make spawn carries OPTIONS=", [],
         [m["line"] for m in cen["make"] if not m["options"]])
    t.ck("C04", "census: every configure argv is built inside run_configure, which spawns through run_to_log",
         len(cen["configure_argvs"]) >= 1 and set(cen["configure_argvs"]) == {"run_configure"}
         and cen["run_configure_spawns"], cen)
    t.ck("C05", "census: every run_configure call passes configure_args, which carries cfg.configure_flags "
         "before it", len(cen["rc_calls"]) >= 1 and all(c["passes"] and c["flags_first"] for c in cen["rc_calls"]),
         cen["rc_calls"])
    t.ck("C06", "census: every recipe derivation passes OPTIONS= in make_vars (and there is one)",
         len(cen["derivations"]) >= 1 and all(d["options"] for d in cen["derivations"]), cen["derivations"])


# ═══════════════════════════════════════════════════════════════════════════════════════
# DC-07 -- the precondition discriminator, and the REAL resume loop driven by scripted logs
# ═══════════════════════════════════════════════════════════════════════════════════════

def drive_corpus(x, U, tag, write_segment, patterns=(), leg_over=None, answer=None, kentry=()):
    """Drive `sqlite_units.run_corpus` -- the REAL resume engine and verdict -- with an INJECTED
    segment runner that writes a scripted log per segment. `kentry` is the plan's kernel entry
    (it only arms the in-kernel sweep, which the caller's procs stand-in answers). -> what it did."""
    C = x.C
    root = x.sub("corpus-" + tag)
    testdir = os.path.join(root, "test")
    write_bytes(os.path.join(testdir, "veryquick.test"), b"run_test_suite veryquick\n")
    write_bytes(os.path.join(testdir, "permutations.test"), b"# the contract suite's permutations\n")
    corpus = ["f%02d.test" % k for k in range(1, 21)]
    log = C.Log(io.StringIO())
    run = make_run(x, log, os.path.join(root, "out"))
    run.ledger = C.Ledger(x.suite.vocab, log)
    run.resolver = FakeResolver(answer or (lambda a: C.Result(0, "", "") if a[:1] == ["--registry-controls"]
                                           else None), C)
    d = leg_d("elf64-x86_64")
    d.update(leg_over or {})
    leg = C.Leg(d)
    leg.fixture = os.path.join(root, "bin", "testfixture")
    leg.tcl_lib = os.path.join(root, "lib", "libtcl8.6.so")
    leg.z_lib = os.path.join(root, "lib", "libz.so.1")
    leg.fixture_built = True
    ctx = {"test_file": os.path.join(testdir, "veryquick.test"), "testdir": testdir, "corpus": corpus,
           "tier_perms": ["veryquick"], "prefixes": [], "witnesses": [],
           "base_env": {"PATH": os.environ.get("PATH", ""),
                        "SQLITE_TEST_PATTERN_LIST": "stale-selection-in-the-driver-environment",
                        "LD_LIBRARY_PATH": "/host/lib"}}
    rundir = os.path.join(root, "run")
    os.makedirs(rundir)
    plan = {"launcher": [], "launcherPath": "", "kernelEntryArgv": list(kentry)}
    segs = []

    def runner(argv, cwd, env, seglog, stall_s, cap_s, settle_s, kill_tree, sweep, any_left):
        k = len(segs)
        segs.append({"argv": [str(a) for a in argv], "env": dict(env), "log": seglog})
        write_bytes(seglog, write_segment(k))
        return U.L.SegmentResult(1, "", 0.0)
    U.run_corpus(run, leg, ctx, rundir, plan, leg.fixture, list(kentry), list(patterns), runner=runner)
    verdict = leg.unit_verdict or ""
    lt = text(log)
    if verdict.startswith("FAIL:PRECONDITION FAILURE"):
        stop = "PRECONDITION"
    elif "RESUME BUDGET EXHAUSTED" in lt:
        stop = "BUDGET-EXHAUSTED"
    else:
        stop = "OTHER: " + verdict[:120]
    rep = leg.unit_report if isinstance(leg.unit_report, dict) else {}
    return types.SimpleNamespace(leg=leg, run=run, segs=segs, log=lt, stop=stop, corpus=corpus,
                                 testdir=testdir, segments=rep.get("segments"), resumes=rep.get("resumes"),
                                 budget=run.cfg.max_resumes)


def pin_dc07(t, x):
    K = x.M.mod("sqlite_corpus")

    def takes(files_done, diag, prev_sig, ok=0, fx=0, last=""):
        facts = K.SegmentFacts(first_diag=diag, files=["f%d.test" % k for k in range(files_done)], ok=ok,
                               fail_markers=fx, last_test=last)
        return "PRECONDITION" if K.is_precondition_failure(prev_sig, facts) else "RESUME"
    silent = K.zero_progress_signature(K.SegmentFacts())
    t.eq("E01", "zero progress twice, IDENTICAL diagnostic -> PRECONDITION", "PRECONDITION", takes(0, D1, D1))
    t.eq("E02", "the FIRST such abort -> RESUME", "RESUME", takes(0, D1, ""))
    t.eq("E03", "zero progress, a DIFFERENT diagnostic -> RESUME", "RESUME", takes(0, D2, D1))
    t.eq("E04", "a crash AFTER completing files -> RESUME", "RESUME", takes(7, D1, D1))
    t.eq("E05", "one file completed, same diagnostic -> RESUME", "RESUME", takes(1, D1, D1))
    t.eq("E06", "zero progress, NO OUTPUT, the first time -> RESUME", "RESUME", takes(0, "", ""))
    t.eq("E07", "SILENCE TWICE -> PRECONDITION", "PRECONDITION", takes(0, "", silent))
    t.eq("E08", "no diagnostic but ' Ok' lines -> RESUME", "RESUME", takes(0, "", silent, ok=5))
    t.eq("E09", "no diagnostic but a FAILURE marker -> RESUME", "RESUME", takes(0, "", silent, fx=2))
    t.eq("E10", "no diagnostic but a test NAME -> RESUME", "RESUME", takes(0, "", silent, last="select1-1.1"))
    t.eq("E11", "two diagnostics differing only in CASE -> RESUME (ordinal, never case-folded)", "RESUME",
         takes(0, "Cannot Open Libtcl", "cannot open libtcl"))
    t.eq("E12", "a silent segment signs as the exact ASCII sentinel (the literal both twins shared)",
         SILENT_SENTINEL_TEXT, silent)
    U = x.M.fresh("sqlite_units", P=fake_procs(x))
    s = drive_corpus(x, U, "silent", lambda k: b"")
    t.ck("L00", "the resume budget is the driver's own (DSS_MAX_RESUMES unset: its default) and can be spent",
         isinstance(s.budget, int) and s.budget >= 1, s.budget)
    t.eq("L01", "TWO SILENT SEGMENTS through the REAL loop: stops after ONE resume, as PRECONDITION",
         (2, 1, "PRECONDITION", 2), (s.segments, s.resumes, s.stop, len(s.segs)))
    t.eq("L02", "...and the silent segment's log really was ZERO BYTES", 0,
         os.path.getsize(s.segs[0]["log"]) if s.segs else -1)
    t.ck("L03", "the tier segment ran WITHOUT SQLITE_TEST_PATTERN_LIST though the driver's env had one",
         bool(s.segs) and "SQLITE_TEST_PATTERN_LIST" not in s.segs[0]["env"], s.segs[:1])
    want_after = " ".join(s.corpus[1:])
    t.ck("L04", "the resume segment runs `permutations.test veryquick` over the files AFTER its boundary",
         len(s.segs) >= 2 and s.segs[1]["env"].get("SQLITE_TEST_PATTERN_LIST") == want_after
         and s.segs[1]["argv"][-2:] == [os.path.join(s.testdir, "permutations.test"), "veryquick"],
         s.segs[1:2])
    fp = [c[2] for c in U.P.calls if c[0] == "stop"]
    t.eq("L05", "the recording procs saw the pre-corpus sweep and one after every segment",
         ["pre-corpus", "after segment 1", "after segment 2"], fp)
    same = drive_corpus(x, x.M.fresh("sqlite_units", P=fake_procs(x)), "same",
                        lambda k: (D1 + "\n").encode("utf-8"))
    t.eq("L06", "two segments with the SAME diagnostic: the same answer", (2, 1, "PRECONDITION"),
         (same.segments, same.resumes, same.stop))
    diff = drive_corpus(x, x.M.fresh("sqlite_units", P=fake_procs(x)), "diff",
                        lambda k: ("child process exited abnormally in file %d\n" % (k + 1)).encode("utf-8"))
    t.eq("L07", "DIFFERENT diagnostics every time: the WHOLE budget is spent (the resilience rule)",
         (diff.budget + 1, diff.budget, "BUDGET-EXHAUSTED"), (diff.segments, diff.resumes, diff.stop))


# ═══════════════════════════════════════════════════════════════════════════════════════
# DC-08 -- the acquisition contract: required, optional, libraries, empty
# ═══════════════════════════════════════════════════════════════════════════════════════

def pin_dc08(t, x):
    libs = x.M.mod("sqlite_libs")
    C = x.C
    acq = x.sub("acq")
    script = os.path.join(acq, "tcl8.6")
    os.makedirs(script)
    tcl = write_bytes(os.path.join(acq, "libtcl8.6.dylib"), b"x")
    z = write_bytes(os.path.join(acq, "libz.1.dylib"), b"x")
    hl = x.suite.hl

    def record(plan_over=None, drop=(), libraries=None):
        plan_ = {"leg": "macho64-arm64", "targetArch": "arm64", "cacheDir": acq, "scriptLibraryDir": script}
        plan_.update(plan_over or {})
        rec = hl.acquisition_record(plan_, libraries=libraries if libraries is not None else [
            {"as": "libtcl8.6.dylib", "path": tcl}, {"as": "libz.1.dylib", "path": z}],
            from_cache=True, remediated=[])
        for k in drop:
            rec.pop(k, None)
        return json.dumps(rec)

    def mk_run(answer_text, rc=0, err="acquire: stderr kept verbatim\n"):
        log = x.log()
        run = types.SimpleNamespace(out_dir=x.sub("acq-out"), log=log, ledger=C.Ledger(x.suite.vocab, log),
                                    cfg=types.SimpleNamespace(host_libdir="", tcl_dll="", zlib_dll=""),
                                    stage=None)
        run.resolver = FakeResolver(lambda a: C.Result(rc, answer_text, err) if a[:1] == ["--acquire"]
                                    else None, C)
        return run

    def mk_leg():
        return C.Leg(leg_d("macho64-arm64", fmt="macho64-arm64-darwin-exec", os_key="darwin", build_extra={
            "libraries": {"provider": "pinned-archive", "tclNames": ["libtcl8.6.dylib"],
                          "zNames": ["libz.1.dylib"]}}))
    run = mk_run(record())
    report, stage, rc, why, logp = libs.acquire(run, mk_leg())
    t.ck("A01", "a REAL acquisition record (harness_legs.acquisition_record) is read: its REQUIRED cacheDir",
         report is not None and report.get("cacheDir") == acq and rc == 0 and stage == "--acquire",
         (stage, rc, why))
    t.eq("A02", "...the resolver's stderr is kept verbatim in the leg's own acquire log",
         "acquire: stderr kept verbatim\n", read_text(logp) if logp and os.path.isfile(logp) else None)
    run, leg = mk_run(record()), mk_leg()
    libs.resolve_leg(run, leg, "host-system pinned-archive search-paths")
    t.eq("A03", "resolve_leg: the acquired cache directory is recorded", acq, leg.acq_dir)
    t.eq("A04", "...the libraries as (as, path) pairs, in the report's order",
         [("libtcl8.6.dylib", tcl), ("libz.1.dylib", z)], leg.acq_libs)
    t.eq("A05", "...the OPTIONAL script library is read", script, leg.tcl_script_dir)
    t.eq("A06", "...and (tcl, z) are picked out of the cache by the leg's OWN declared names",
         (os.path.abspath(tcl), os.path.abspath(z)), (leg.tcl_lib, leg.z_lib))
    run, leg = mk_run(record(plan_over={"scriptLibraryDir": ""})), mk_leg()
    libs.resolve_leg(run, leg, "")
    t.ck("A07", "an ABSENT optional script library yields '' and is WARNED, never assumed benign",
         leg.tcl_script_dir == "" and "stages NO Tcl script library" in text(run.log), text(run.log)[-800:])
    rep, stage, rc, why, _ = libs.acquire(mk_run(record(drop=["cacheDir"])), mk_leg())
    t.ck("A08", "a report MISSING the required cacheDir is REFUSED (a read failure, rc 1), never read as ''",
         rep is None and stage == "reading the --acquire report" and rc == 1 and "could not be read" in why,
         (rep, stage, rc, why))
    rep, stage, rc, why, _ = libs.acquire(mk_run(record(plan_over={"cacheDir": ""})), mk_leg())
    t.ck("A09", "an EMPTY required cacheDir is refused the same way", rep is None and rc == 1, (rep, stage, rc))
    rep, stage, rc, why, _ = libs.acquire(mk_run(record(drop=["libraries"])), mk_leg())
    t.ck("A10", "a report MISSING its libraries is refused", rep is None and rc == 1, (rep, stage, rc))
    rep, stage, rc, why, _ = libs.acquire(mk_run("", rc=4, err="--acquire: digest mismatch (pinned)\n"), mk_leg())
    t.eq("A11", "a failing --acquire answers (None, '--acquire', its rc, its stderr)",
         (None, "--acquire", 4, "--acquire: digest mismatch (pinned)"), (rep, stage, rc, why))
    rep, stage, rc, why, _ = libs.acquire(mk_run("this is not JSON"), mk_leg())
    t.ck("A12", "rc 0 with a report that is not JSON is refused", rep is None and rc == 1, (rep, stage, rc))
    run, leg = mk_run("", rc=4, err="--acquire: digest mismatch (pinned)\n"), mk_leg()
    libs.resolve_leg(run, leg, "")
    t.ck("A13", "a refused acquisition costs the leg its build: skipped-build-input-missing, naming the "
         "provider", leg.verdict == "skipped-build-input-missing" and "pinned-archive" in leg.verdict_detail,
         (leg.verdict, leg.verdict_detail))
    t.eq("A14", "the optional key is the contract's name (no reader takes a key NAME, so the .ps1's "
         "empty-key refusal has no subject left)", "scriptLibraryDir", getattr(libs, "ACQ_SCRIPT_LIBRARY_KEY", None))


# ═══════════════════════════════════════════════════════════════════════════════════════
# DC-09 -- the loader variable and separator are TARGET-keyed, over two REAL plans
# ═══════════════════════════════════════════════════════════════════════════════════════

LOADER_WANT = {
    "darwin": {"elf64-x86_64": ("LD_LIBRARY_PATH", ":"), "elf64-arm64": ("LD_LIBRARY_PATH", ":"),
               "pe64-x86_64": None, "macho64-arm64": ("DYLD_LIBRARY_PATH", ":"),
               "macho64-x86_64": ("DYLD_LIBRARY_PATH", ":")},
    "windows": {"elf64-x86_64": ("LD_LIBRARY_PATH", ":"), "elf64-arm64": ("LD_LIBRARY_PATH", ":"),
                "pe64-x86_64": ("PATH", ";"), "macho64-arm64": ("DYLD_LIBRARY_PATH", ":"),
                "macho64-x86_64": ("DYLD_LIBRARY_PATH", ":")},
}
PLANS = (("darwin", "arm64", "arch"), ("windows", "x86_64", "wsl.exe"))


def loader_contract(leg):
    """The stated table (decision 6), for a leg the catalogue grows beyond the five named above."""
    parts = (leg.format or "").split("-")
    fmt_os = parts[-2] if len(parts) >= 3 else ""
    if fmt_os == "linux":
        return ("LD_LIBRARY_PATH", ":")
    if fmt_os == "darwin":
        return ("DYLD_LIBRARY_PATH", ":")
    if fmt_os == "windows":
        return ("PATH", ";") if leg.run_mode == "native" else None
    return "<no stated answer for target OS %r>" % fmt_os


def pin_dc09(t, x):
    L = x.M.mod("sqlite_launch")
    C = x.C
    modes = {}
    for host, arch, launchers in PLANS:
        plan = x.suite.plan(host, arch, launchers)
        legs = [C.Leg(copy.deepcopy(d)) for d in plan.get("legs") or []]
        t.ck("P-%s" % host, "the REAL %s/%s plan (--environment-probes skip, --launchers-available %s) carries "
             "at least 5 legs" % (host, arch, launchers), len(legs) >= 5, [lg.label for lg in legs])
        for lg in legs:
            modes[(host, lg.label)] = lg.run_mode
            want = LOADER_WANT[host].get(lg.label, loader_contract(lg))
            r, got = refused(L.loader_spec, lg)
            t.eq("V-%s-%s" % (host, lg.label), "%s plan: %s (%s, run %s) -> %s" % (
                host, lg.label, lg.format, lg.run_mode, want), want, got if not r else "REFUSED: " + got)
    t.eq("F01", "the pe64 fork, both sides: native on the windows plan (PATH ';'), not native on the "
         "darwin plan (none)", ("native", True), (modes.get(("windows", "pe64-x86_64")),
                                                modes.get(("darwin", "pe64-x86_64")) not in (None, "native")))
    base = [d for d in x.suite.plan("windows", "x86_64", "wsl.exe")["legs"] if d["label"] == "elf64-x86_64"]
    if not base:
        t.ck("X00", "the windows plan carries elf64-x86_64 to vary", False)
        return

    def variant(stage_key=None, fmt=None):
        d = copy.deepcopy(base[0])
        if stage_key is not None:
            d.setdefault("build", {})["configStageKey"] = stage_key
        if fmt is not None:
            d["format"] = fmt
        return C.Leg(d)
    r, msg = refused(L.loader_spec, variant(stage_key="darwin"))
    t.ck("X01", "a plan whose configStageKey (darwin) contradicts its ELF format REFUSES", r, msg)
    t.has("X02", "...naming the contradiction", msg, "disagrees with itself")
    r, msg = refused(L.loader_spec, variant(fmt="macho64-arm64-darwin-exec"))
    t.ck("X03", "...and so does the .ps1's variant (a darwin format under a linux key)",
         r and "disagrees with itself" in str(msg), msg)
    r, msg = refused(L.loader_spec, variant(stage_key=""))
    t.ck("X04", "an EMPTY target OS REFUSES", r and "no target OS" in str(msg), msg)
    r, msg = refused(L.loader_spec, variant(stage_key="plan9", fmt="elf64-x86_64-plan9-exec"))
    t.ck("X05", "an UNKNOWN target OS REFUSES (a new OS must DECLARE its answer)",
         r and "no declared runtime-loader search variable" in str(msg), msg)
    t.eq("X06", "a consistent leg's target OS is read from the plan", "linux", L.target_os(variant()))


# ═══════════════════════════════════════════════════════════════════════════════════════
# DC-10 -- the confound supply is PER LEG, from the declaration, gated twice
# ═══════════════════════════════════════════════════════════════════════════════════════

def pin_dc10(t, x):
    V = x.M.mod("sqlite_verdicts")
    C = x.C

    def mk(label, patterns, gating="probed", rdg="not-required", declared=True):
        d = leg_d(label, confoundGating=gating, runDirectoryGating=rdg, confoundsByName=list(patterns))
        if not declared:
            for k in ("confoundsByName", "confoundsByEvidence", "executionEvidence"):
                d.pop(k, None)
        return C.Leg(d)
    own = ["^walsetlk-", "^busy2-"]
    t.eq("I01", "a leg gets ITS OWN declared patterns", own, V.confound_supply(mk("elf64-x86_64", own), None))
    t.eq("I02", "...and the LABEL does not change the answer (the same declaration under two labels: a "
         "label-keyed supply cannot answer both)", [own, own],
         [V.confound_supply(mk("elf64-x86_64", own), None), V.confound_supply(mk("pe64-x86_64", own), None)])
    arm = V.confound_supply(mk("elf64-arm64", ["^busy2-", "emulated:^writecrash-"]), None)
    t.eq("I03", "a DIFFERENT declaration gives a DIFFERENT set", ["^busy2-", "emulated:^writecrash-"], arm)
    t.eq("I04", "a leg declaring [] inherits NOTHING", [], V.confound_supply(mk("pe64-x86_64", []), None))
    t.ck("I05", "the `emulated:` scope survives the supply", "emulated:^writecrash-" in arm, arm)
    odd = ["^vtabH-3\\.1$", "^a b-1$", "C:\\\\x"]
    t.eq("I06", "patterns with a space, a `$` and a backslash survive verbatim", odd,
         V.confound_supply(mk("elf64-x86_64", odd), None))
    t.eq("O01", "the operator override reaches EVERY leg (a [] leg included)", ["^op-1", "^op-2"],
         V.confound_supply(mk("pe64-x86_64", []), ["^op-1", "^op-2"]))
    t.eq("O02", "...replacing a leg's own list", ["^op-1"], V.confound_supply(mk("elf64-x86_64", own), ["^op-1"]))
    r, msg = refused(V.confound_supply, mk("nodecl", [], declared=False), None)
    t.ck("U01", "an UNDECLARED leg REFUSES rather than answering []", r, msg)
    t.has("U02", "...naming the transport defect", msg, "transport defect")
    r, msg = refused(V.confound_supply, mk("elf64-x86_64", ["^busy2-"], gating="unprobed"), None)
    t.ck("G01", "an UNPROBED plan REFUSES rather than serving its ungated list", r, msg)
    t.has("G02", "...naming the gating it got", msg, "confoundGating='unprobed'")
    t.has("G03", "...and how to resolve a measured plan", msg, "--environment-probes skip")
    r, msg = refused(V.confound_supply, mk("elf64-x86_64", ["^busy2-"], gating="injected"), None)
    t.ck("G04", "an INJECTED plan (verdicts read from a file) REFUSES too", r and "confoundGating='injected'" in
         str(msg), msg)
    r, msg = refused(V.confound_supply, mk("pe64-x86_64", ["^vtabH-3\\.1$"], rdg="unmeasured"), None)
    t.ck("G05", "an UNMEASURED run directory REFUSES rather than serving its uncorroborated list", r, msg)
    t.has("G06", "...naming the gating it got", msg, "runDirectoryGating='unmeasured'")
    t.has("G07", "...and the call that would measure it", msg, "--corroborate-run-dir")
    t.eq("G08", "a 'measured' run-directory gating is ACCEPTED", ["^vtabH-3\\.1$"],
         V.confound_supply(mk("pe64-x86_64", ["^vtabH-3\\.1$"], rdg="measured"), None))
    r, msg = refused(V.confound_supply, mk("pe64-x86_64", ["^x"], rdg="whenever"), None)
    t.ck("G09", "a run-directory gating outside the vocabulary REFUSES", r and "runDirectoryGating='whenever'"
         in str(msg), msg)


# ═══════════════════════════════════════════════════════════════════════════════════════
# DC-11 -- the supply's refusal STOPS THE DRIVER at the real call site (a child process)
# ═══════════════════════════════════════════════════════════════════════════════════════

def pin_dc11(t, x):
    jobs = []
    for key, gating, rdg in (("unprobed", "unprobed", "not-required"), ("probed", "probed", "not-required"),
                             ("unmeasured", "probed", "unmeasured")):
        tmp = x.sub("supply-" + key)
        spec = write_json(os.path.join(tmp, "spec.json"), {"tmp": tmp, "gating": gating, "rdg": rdg,
                                                             "vocab": x.suite.vocab,
                                                             "override": x.M.override_spec()})
        jobs.append((key, [sys.executable, THIS_FILE, "--child", "supply", spec], child_env(x)))
    res = run_children(jobs)
    rc, out = res["unprobed"]
    t.eq("C01", "an UNPROBED plan STOPS the driver at the real call site (sqlite_units.unit_leg; rc)", 1, rc)
    t.has("C02", "...having said why", out, "confoundGating='unprobed'")
    t.lacks("C03", "...and the statement AFTER the call site never ran", out, "REACHED-NEXT-STATEMENT")
    rc, out = res["probed"]
    t.eq("C04", "a PROBED plan runs on through the call site (rc)", 0, rc)
    t.has("C05", "...reaching the next statement with the leg's pattern", out, "REACHED-NEXT-STATEMENT size=1")
    rc, out = res["unmeasured"]
    t.eq("C06", "an UNMEASURED run directory STOPS the driver at the real call site (rc)", 1, rc)
    t.has("C07", "...having said why", out, "runDirectoryGating='unmeasured'")
    t.lacks("C08", "...and the statement AFTER the call site never ran", out, "REACHED-NEXT-STATEMENT")


# ═══════════════════════════════════════════════════════════════════════════════════════
# DC-12 -- a failed run-directory operation is a VERDICT, never a silent fallback
# ═══════════════════════════════════════════════════════════════════════════════════════

def pin_dc12(t, x):
    L = x.M.mod("sqlite_launch")
    t.eq("J01", "an EMPTY prefix is a real answer (the driver's own filesystem): ok, no reason",
         (True, ""), L.run_dir_argv([], ["/x"], "do nothing"))
    marker = os.path.join(x.tmp, "spawned-anyway")
    got = L.run_dir_argv([], [sys.executable, "-c", "open(%r, 'w').close()" % marker], "do nothing")
    t.ck("J02", "an EMPTY prefix with TWO arguments still spawns NOTHING (the .ps1 would have run rest[0])",
         got == (True, "") and not os.path.exists(marker), (got, os.path.exists(marker)))
    ok, why = L.run_dir_argv([sys.executable, "-c", "raise SystemExit(3)"], ["x"], "prepare the run directory")
    t.eq("J03", "a FAILING prefix is REPORTED, never swallowed", False, ok)
    t.has("J04", "...naming what could not be done", why,
          "could not prepare the run directory in the launcher's own filesystem")
    t.has("J05", "...and the exit code", why, "exited 3")
    t.eq("J06", "a prefix that succeeds is ok", (True, ""),
         L.run_dir_argv([sys.executable, "-c", "pass"], ["x"], "create it"))
    t.eq("J07", "a None prefix is the empty answer too", (True, ""), L.run_dir_argv(None, ["x"], "do nothing"))


# ═══════════════════════════════════════════════════════════════════════════════════════
# DC-13 -- a launched leg's run environment ARRIVES (carrier, loader path, translation)
# ═══════════════════════════════════════════════════════════════════════════════════════

def pin_dc13(t, x):
    L = x.M.mod("sqlite_launch")
    C = x.C
    by = dict((d["label"], d) for d in x.suite.plan("windows", "x86_64", "wsl.exe").get("legs") or [])
    if "elf64-arm64" not in by or "pe64-x86_64" not in by:
        t.ck("D00", "the windows-host plan carries elf64-arm64 and pe64-x86_64", False, sorted(by))
        return
    arm, pe = C.Leg(copy.deepcopy(by["elf64-arm64"])), C.Leg(copy.deepcopy(by["pe64-x86_64"]))
    t.eq("D01", "the arm64 leg is LAUNCHED into another path namespace", "windows-to-wsl", L.path_verb(arm))
    t.eq("D02", "...and does NOT inherit this driver's environment", "wslenv", L.env_verb(arm))
    t.eq("D03", "...and the CATALOGUE declares its launcher variable", {"QEMU_LD_PREFIX": "/usr/aarch64-linux-gnu"},
         L.declared_env(arm))
    t.eq("D04", "the pe64 leg runs NATIVELY, in this driver's namespace", ("native", "none"),
         (pe.run_mode, L.path_verb(pe)))
    R = InProcResolver(x.suite, deterministic_translator([]))
    got = L.loader_search_path(R, pe, ["C:\\dss\\pin\\z\\lib"], {"PATH": "C:\\host\\bin"}, x.log())
    t.eq("N01", "native pe64: its directory, the TARGET separator ';', then this driver's PATH",
         ("PATH", "C:\\dss\\pin\\z\\lib;C:\\host\\bin"), got)
    dirs = ["C:\\dss\\pin\\tcl\\lib", "C:\\dss\\pin\\z\\lib"]
    host_side = "C:\\dss\\pin\\host-side\\lib"
    log2 = x.log()
    got = L.loader_search_path(R, arm, dirs, {"LD_LIBRARY_PATH": host_side}, log2)
    want_val = ":".join(L.launch_path(R, "windows-to-wsl", d) for d in dirs)
    val = got[1] if got else ""
    t.eq("T01", "launched arm64 (injected translator): every directory in the LAUNCHER's namespace, joined "
         "by the TARGET's ':'", ("LD_LIBRARY_PATH", want_val), got)
    t.lacks("T02", "...so the value carries no Windows list separator", val, ";")
    t.lacks("T03", "...and no Windows path spelling", val, "\\")
    t.ck("T04", "...it is absolute in the launcher's namespace", val.startswith("/"), val)
    t.lacks("T05", "...and the HOST value already in the variable does NOT cross", val, "host-side")
    t.has("T06", "...which is said out loud, never dropped silently", text(log2), "is NOT carried into the launcher's")
    base = {"PATH": "C:\\host\\bin", "LD_LIBRARY_PATH": host_side, "WSLENV": "OPERATOR_VAR/u",
            "SQLITE_TEST_PATTERN_LIST": "stale-selection", "QUICKTEST_OMIT": "stale-omit"}
    base_before, environ_before = dict(base), dict(os.environ)
    r, env = refused(L.leg_launch_env, R, arm, base, dirs, tcl_library="C:\\dss\\pin\\tcl\\lib\\tcl8.6",
                     extra={"SQLITE_TEST_PATTERN_LIST": None, "QUICKTEST_OMIT": "a.test,b.test"}, log=x.log())
    if r:
        env = {"<refused>": env}
    carrier = str(env.get("WSLENV", ""))

    def named(n):
        return any(e == n or e.startswith(n + "/") for e in carrier.split(":"))
    t.eq("B01", "the builder applies the CATALOGUE-declared launcher variable", "/usr/aarch64-linux-gnu",
         env.get("QEMU_LD_PREFIX"))
    t.eq("B02", "...the loader variable holds the launcher-namespace value", want_val, env.get("LD_LIBRARY_PATH"))
    t.ck("B03", "...the carrier NAMES the declared launcher variable", named("QEMU_LD_PREFIX"), carrier)
    t.ck("B04", "...and NAMES the loader search variable", named("LD_LIBRARY_PATH"), carrier)
    t.eq("B05", "a DRIVER-PATH forward (TCL_LIBRARY) crosses TRANSLATED",
         L.launch_path(R, "windows-to-wsl", "C:\\dss\\pin\\tcl\\lib\\tcl8.6"), env.get("TCL_LIBRARY"))
    t.ck("B06", "...and is NAMED in the carrier", named("TCL_LIBRARY"), carrier)
    t.ck("B07", "an OPAQUE forward that is SET (QUICKTEST_OMIT) is named, with the segment's value",
         named("QUICKTEST_OMIT") and env.get("QUICKTEST_OMIT") == "a.test,b.test", (carrier, env.get("QUICKTEST_OMIT")))
    t.ck("B08", "a forward the segment DELETED (SQLITE_TEST_PATTERN_LIST) is gone AND never named",
         "SQLITE_TEST_PATTERN_LIST" not in env and not named("SQLITE_TEST_PATTERN_LIST"), carrier)
    t.has("B09", "the operator's own carrier value is merged, never clobbered", carrier, "OPERATOR_VAR/u")
    t.eq("B10", "the base dict handed in is unchanged", base_before, base)
    t.ck("B11", "os.environ is unchanged afterwards (a returned dict replaced the .ps1's Push/Pop)",
         dict(os.environ) == environ_before, "os.environ changed")
    t.eq("B12", "every other base variable crosses unchanged", "C:\\host\\bin", env.get("PATH"))
    Rr = x.suite.real
    env4 = {"QUICKTEST_OMIT": "a,b", "TCL_LIBRARY": "opt/tcl/lib/tcl8.6"}
    r4, got4 = refused(L.carrier_assignments, Rr, "wslenv", "none", env4, L.FORWARD_PLAIN, L.FORWARD_PATHS, [], "")
    t.eq("K01", "exact carrier output over the REAL resolver, in order: the driver path ASSIGNED, then the "
         "carrier naming the set forwards", [("TCL_LIBRARY", "opt/tcl/lib/tcl8.6"),
                                             ("WSLENV", "QUICKTEST_OMIT:TCL_LIBRARY")],
         got4 if not r4 else "REFUSED: %s" % got4)
    t.lacks("K02", "an UNSET forward (SQLITE_TEST_PATTERN_LIST) is never named", repr(got4), "SQLITE_TEST_PATTERN_LIST")
    r5, got5 = refused(L.carrier_assignments, Rr, "wslenv", "none", dict(env4, SQLITE_TEST_PATTERN_LIST=""),
                       L.FORWARD_PLAIN, L.FORWARD_PATHS, [], "")
    t.eq("K03", "an EMPTY value is treated as unset (it would arrive empty-but-existing)", (r4, got4), (r5, got5))
    r6, got6 = refused(L.carrier_assignments, Rr, "wslenv", "none", dict(env4, QEMU_LD_PREFIX="usr/aarch64"),
                       L.FORWARD_PLAIN, L.FORWARD_PATHS, ["QEMU_LD_PREFIX"], "")
    t.has("K04", "a CATALOGUE-declared launcher variable crosses too", repr(got6), "QEMU_LD_PREFIX")
    t.eq("K05", "a launcher that INHERITS carries nothing", [],
         L.carrier_assignments(Rr, "inherit", "none", env4, L.FORWARD_PLAIN, L.FORWARD_PATHS, [], ""))
    before = len(Rr.asked)
    got7 = L.carrier_assignments(Rr, "wslenv", "none", {}, L.FORWARD_PLAIN, L.FORWARD_PATHS, ["QEMU_LD_PREFIX"], "")
    t.eq("K06", "nothing SET means nothing carried -- the resolver is not even asked", ([], 0),
         (got7, len(Rr.asked) - before))
    t.eq("K07", "wslenv's carrier variable comes from the resolver's --env-transfers", "WSLENV",
         L.carrier_name(Rr, "wslenv"))
    t.eq("K08", "inherit has no carrier", "", L.carrier_name(Rr, "inherit"))
    r9, m9 = refused(L.carrier_name, Rr, "teleport")
    t.ck("K09", "a verb the resolver does not declare is REFUSED, never read as inherit",
         r9 and "declares no carrier variable" in str(m9), m9)
    spec = x.suite.hl.PATH_TRANSLATIONS.get("windows-to-wsl", {})
    valid, xl = spec.get("validHostOs", ""), list(spec.get("translator") or [])
    if x.suite.host != valid:
        t.skip("RT1", "the REAL windows-to-wsl translator on the launched leg",
               "that verb's translator exists only on a '%s' host (harness_legs PATH_TRANSLATIONS validHostOs); "
               "this host is '%s'" % (valid, x.suite.host))
    elif not xl or not shutil.which(xl[0]):
        t.skip("RT1", "the REAL windows-to-wsl translator on the launched leg", "%s is not on PATH" % (xl[:1] or "?"))
    elif not Rr.call(["--path-translation", "windows-to-wsl", "--translate-path",
                      "C:\\dss\\pin\\probe"]).out.strip().startswith("/"):
        probe = Rr.call(["--path-translation", "windows-to-wsl", "--translate-path", "C:\\dss\\pin\\probe"])
        t.skip("RT1", "the REAL windows-to-wsl translator on the launched leg",
               "%s is on PATH but translates nothing on this host (rc=%d: %s) -- no usable WSL distribution"
               % (xl[0], probe.rc, " ".join((probe.err or probe.out).split())[:300]))
    else:
        r10, got10 = refused(L.loader_search_path, Rr, arm, dirs, {}, x.log())
        parts = got10[1].split(":") if (not r10 and got10) else []
        t.ck("RT1", "the REAL translator (%s) spells both directories for the launcher: absolute, no backslash, "
             "':'-joined, each the same directory" % " ".join(xl),
             len(parts) == 2 and all(p.startswith("/") and "\\" not in p for p in parts)
             and parts[0].endswith("/dss/pin/tcl/lib") and parts[1].endswith("/dss/pin/z/lib"), got10)


# ═══════════════════════════════════════════════════════════════════════════════════════
# DC-14 -- the smoke gate spawns with the ONE shared environment builder
# ═══════════════════════════════════════════════════════════════════════════════════════

def drive_smoke(x, rc):
    """Drive `sqlite_smoke.step7c` on one launched arm64 leg with the builder and the spawn SPIED
    (the gate is never really run). -> (leg, run, spawns, built, log text)."""
    C = x.C
    smoke = x.M.fresh("sqlite_smoke")
    root = x.sub("smoke-%d" % rc)
    bld = os.path.join(root, "bld")
    write_bytes(os.path.join(bld, "sqlite3.h"), b'#define SQLITE_VERSION        "3.99.0"\n'
                b'#define SQLITE_SOURCE_ID      "2026-09-21 00:00:00 the-contract-suite"\n')
    log = C.Log(io.StringIO())
    run = make_run(x, log, os.path.join(root, "out"))
    run.stage = {"bld": bld, "reference_cli": "", "reference_cli_posix": "",
                 "reference_cli_why": "no reference CLI in this pin"}
    run.resolver = FakeResolver(lambda a: C.Result(0, "aarch64\telf64\tlinux\n", "")
                                if a[:1] == ["--identify-binary"] else None, C)
    leg = C.Leg(leg_d("elf64-arm64", fmt="elf64-aarch64-linux-exec", mode="launched", launcher=["qemu-aarch64"],
                      run_extra={"env": {"QEMU_LD_PREFIX": "/usr/aarch64-linux-gnu"}}))
    leg.cli_bin = os.path.join(root, "elf64-arm64", "sqlite3")
    leg.z_lib_any = os.path.join(root, "zlib", "libz.so.1")
    leg.tcl_script_dir = os.path.join(root, "tcl", "tcl8.6")
    run.legs = [leg]
    real_L, real_C = smoke.L, smoke.C
    built, spawns = [], []

    def spy_builder(*a, **kw):
        env = real_L.leg_launch_env(*a, **kw)
        built.append({"leg": a[1].label if len(a) > 1 else "?", "env": dict(env)})
        return env

    def spy_capture(argv, cwd=None, env_=None, timeout=None, input_text=None, merge=False):
        spawns.append({"argv": [str(v) for v in argv], "env": dict(env_ or {})})
        return real_C.Result(rc, "the contract suite's smoke output\n", "")
    smoke.L = Proxy(real_L, leg_launch_env=spy_builder)
    smoke.C = Proxy(real_C, capture=spy_capture)
    smoke.step7c(run)
    return leg, run, spawns, built, text(log)


def pin_dc14(t, x):
    C = x.C
    leg, run, spawns, built, _lt = drive_smoke(x, 0)
    t.ck("W01", "the smoke gate is spawned exactly once, with THIS interpreter and cli-smoke.py",
         len(spawns) == 1 and spawns[0]["argv"][:2] == [sys.executable, C.CLI_SMOKE], spawns)
    env = spawns[0]["env"] if spawns else {}
    benv = built[0]["env"] if built else {}
    flags = C.child_env({}, base={}, python=True)
    t.eq("W02", "the spawn's environment IS the one the shared builder returned, plus exactly the Python "
         "child's own flags (%s)" % ", ".join(sorted(flags)),
         ([], dict(flags)), ([k for k, v in benv.items() if k not in flags and env.get(k) != v]
                             if benv else ["<no builder call>"], dict((k, env.get(k)) for k in flags)))
    t.eq("W03", "...so the launcher's DECLARED QEMU_LD_PREFIX reached the gate", "/usr/aarch64-linux-gnu",
         env.get("QEMU_LD_PREFIX"))
    t.ck("W04", "...the loader variable carries the leg's zlib directory",
         os.path.dirname(leg.z_lib_any) in str(env.get("LD_LIBRARY_PATH", "")), env.get("LD_LIBRARY_PATH"))
    t.eq("W05", "...and TCL_LIBRARY is the leg's acquired script library", leg.tcl_script_dir, env.get("TCL_LIBRARY"))
    t.eq("W06", "the builder was called ONCE, for this leg", ["elf64-arm64"], [b["leg"] for b in built])
    sm, un, la = x.M.mod("sqlite_smoke"), x.M.mod("sqlite_units"), x.M.mod("sqlite_launch")
    t.ck("W07", "the smoke step and the corpus reach the ONE builder (sqlite_smoke.L.leg_launch_env is "
         "sqlite_units.L.leg_launch_env is sqlite_launch.leg_launch_env)",
         sm.L.leg_launch_env is un.L.leg_launch_env is la.leg_launch_env)
    seen = []

    def spy2(*a, **kw):
        env2 = la.leg_launch_env(*a, **kw)
        seen.append(dict(env2))
        return env2
    U = x.M.fresh("sqlite_units", L=Proxy(la, leg_launch_env=spy2), P=fake_procs(x))
    log = x.log()
    run2 = make_run(x, log, x.sub("seg-out"))
    run2.resolver = FakeResolver(lambda a: None, C)
    got = []

    def runner(argv, cwd, env2, seglog, *rest):
        got.append(dict(env2))
        write_bytes(seglog, b"")
        return la.SegmentResult(0, "", 0.0)
    seg = U.Segment("tier", "", "veryquick.test", None, "/x/veryquick.test", "")
    U.run_one_segment(run2, leg, seg, os.path.join(x.tmp, "seg.log"), x.tmp, ["qemu-aarch64"], "/x/testfixture",
                      [], [os.path.dirname(leg.z_lib_any)], {"PATH": "/usr/bin"}, None, U.LegRun(), runner=runner)
    t.ck("W08", "the corpus's per-segment spawn gets EXACTLY the environment the same builder returned",
         len(seen) == 1 and got == seen, (len(seen), len(got)))


# ═══════════════════════════════════════════════════════════════════════════════════════
# DC-15 -- the launcher-prerequisite gate, strict mode, the Step-9 ledger, the smoke rc table
# ═══════════════════════════════════════════════════════════════════════════════════════

UNMET_REPORT = {"label": "lau", "ok": False, "verdict": "skipped-launcher-prerequisite-missing",
                "missing": [{"kind": "command", "path": "qemu-aarch64", "provides": "PIN-PROVIDES",
                             "why": "PIN-WHY", "install": "PIN-INSTALL",
                             "probe": ["wsl.exe", "-e", "sh", "-lc", "command -v qemu-aarch64"]}],
                "uncovered": ["PIN-UNCOVERED-ROW"]}


def drive_step9(x, report_mod, lau_verdict, strict, nat_verdict="ran"):
    C = x.C
    log = C.Log(io.StringIO())
    run = make_run(x, log, x.sub("step9-out"))
    run.cfg.strict = strict
    run.ledger = C.Ledger(x.suite.vocab, log)
    classes = x.suite.classes_text
    run.resolver = FakeResolver(lambda a: C.Result(0, classes, "") if a == ["--verdict-classes"] else (
        C.Result(0, "", "") if a[:1] == ["--oracle-report"] else None), C)
    nat = C.Leg(leg_d("nat"))
    nat.verdict, nat.verdict_detail, nat.fixture_built = nat_verdict, "the contract suite's leg", True
    nat.cli_bin, nat.smoke_verdict, nat.unit_verdict = "/pin/nat/sqlite3", "PASS (14/14)", "PASS (pin)"
    lau = C.Leg(leg_d("lau", mode="skip"))
    lau.verdict, lau.verdict_detail, lau.fixture_built = lau_verdict, "the contract suite's leg", True
    lau.cli_bin, lau.smoke_verdict = "/pin/lau/sqlite3", "built, NOT RUN here"
    run.legs = [nat, lau]
    run.stage = {"fixture_recipe": {"tus": "", "defines": ""}, "cli_recipe": {"tus": "", "defines": ""},
                 "reference_cli": "", "reference_fixture": "", "sqlite_head": "pin"}
    run.compiler = types.SimpleNamespace(path="/pin/dsscp", build_type_note="", built="2026-09-21",
                                         origin="the contract suite")
    run.provenance = {"head_short": "pin", "diverge_note": ""}
    run.currency_note, run.sqlite_dir_posix, run.stage_dir = "pin", "/pin/sqlite", ""
    run.artifacts = types.SimpleNamespace(get=lambda label, art: ("built", "the contract suite"))
    rc = report_mod.step9(run)
    return rc, text(log)


def pin_dc15(t, x):
    C = x.C
    bt = x.M.mod("build_and_test")
    U = x.M.use("sqlite_units")
    mode = {"m": "met"}
    asked = []

    def answer(a):
        if a[:1] != ["--check-launcher"]:
            return None
        asked.append(a[1] if len(a) > 1 else "?")
        m = mode["m"]
        if m == "met":
            return C.Result(0, json.dumps({"label": "lau", "ok": True, "verdict": "", "missing": [],
                                           "uncovered": []}), "")
        if m == "unmet":
            return C.Result(3, json.dumps(UNMET_REPORT), "")
        if m == "garbled":
            return C.Result(3, "this report is not JSON {", "")
        return C.Result(2, "", "harness_legs.py: FATAL: the pin asked for an unreadable outcome")

    def gate(m):
        mode["m"] = m
        log = C.Log(io.StringIO())
        run = make_run(x, log, x.sub("gate-out"))
        run.ledger = C.Ledger(x.suite.vocab, log)
        run.resolver = FakeResolver(answer, C)
        lau = C.Leg(leg_d("lau", fmt="elf64-aarch64-linux-exec", mode="launched",
                          launcher=["wsl.exe", "-e", "qemu-aarch64"]))
        lau.tcl_lib, lau.fixture_built, lau.fixture, lau.cc = "/cache/libtcl8.6.so", True, "/out/testfixture", []
        nat = C.Leg(leg_d("nat"))
        nat.tcl_lib, nat.fixture_built, nat.fixture = "/cache/libtcl8.6.so", True, "/out/testfixture"
        run.legs = [lau, nat]
        bt.launcher_prereq_gate(run)
        return run, lau, nat, log
    run, lau, nat, log = gate("met")
    t.eq("K01", "a MET prerequisite leaves the plan's run mode and verdict alone (checked FIRST)",
         ("launched", "", []), (lau.run_mode, lau.verdict, list(run.ledger.unclassified)))
    t.eq("K02", "...and the leg REACHES the corpus", True, U.corpus_entry(run, lau))
    run, lau, nat, log = gate("unmet")
    t.eq("K03", "an UNMET prerequisite: the leg's verdict is the closed-vocabulary token",
         "skipped-launcher-prerequisite-missing", lau.verdict)
    t.eq("K04", "...the RUN verdict too, so both artefacts read the same answer",
         "skipped-launcher-prerequisite-missing", lau.run.get("verdict"))
    t.eq("K05", "...the run mode is downgraded to skip", "skip", lau.run_mode)
    t.eq("K06", "...so the corpus is NOT entered", False, U.corpus_entry(run, lau))
    t.ck("K07", "...and the unit ledger says so under that same token",
         lau.unit_verdict.startswith("not run [skipped-launcher-prerequisite-missing] %s " % DASH), lau.unit_verdict)
    t.ck("K08", "...a token the closed vocabulary knows, NOT rejected by the guard",
         run.ledger.known("skipped-launcher-prerequisite-missing") and list(run.ledger.unclassified) == [],
         run.ledger.unclassified)
    w = text(log)
    t.has("K09", "the remedy row: MISSING [command] <path>", w, "MISSING [command] qemu-aarch64")
    t.has("K10", "...what it PROVIDES", w, "provides: PIN-PROVIDES")
    t.has("K11", "...WHY it is declared", w, "why     : PIN-WHY")
    t.has("K12", "...HOW TO INSTALL it", w, "install : PIN-INSTALL")
    t.has("K13", "...and what was probed", w, "probed  : wsl.exe -e sh -lc command -v qemu-aarch64")
    t.eq("K14", "launcher_prereq_rows formats every row, UNCOVERED included",
         ["MISSING [command] qemu-aarch64", "      provides: PIN-PROVIDES", "      why     : PIN-WHY",
          "      install : PIN-INSTALL", "      probed  : wsl.exe -e sh -lc command -v qemu-aarch64",
          "UNCOVERED PIN-UNCOVERED-ROW"], bt.launcher_prereq_rows(UNMET_REPORT))
    run, lau, nat, log = gate("boom")
    t.eq("K15", "an rc the driver has no arm for is POISONED, never passed", ("poisoned", "skip"),
         (lau.verdict, lau.run_mode))
    t.eq("K16", "...and the leg does not reach the corpus", False, U.corpus_entry(run, lau))
    run, lau, nat, log = gate("garbled")
    t.eq("K17", "rc 3 with an UNPARSEABLE report is poisoned (the .ps1's union), not unmet", "poisoned", lau.verdict)
    t.has("K18", "...saying the report is not JSON", text(log), "not JSON")
    t.ck("K19", "a NATIVE leg is never probed, and keeps its run mode", "nat" not in asked and nat.run_mode == "native",
         asked)
    REP = x.M.use("sqlite_report")
    rc, lt = drive_step9(x, REP, "skipped-launcher-prerequisite-missing", False)
    t.ck("S01", "by default an ENVIRONMENTAL skip WARNS and the run survives (rc 0)",
         rc == 0 and "ENVIRONMENTAL reason" in lt, (rc, lt[-1500:]))
    rc, lt = drive_step9(x, REP, "skipped-launcher-prerequisite-missing", True)
    t.ck("S02", "under DSS_STRICT_ARM_VERDICTS=1 it is a HARD FAILURE, naming the variable",
         rc == 1 and "DSS_STRICT_ARM_VERDICTS=1" in lt, (rc, lt[-1500:]))
    rc, lt = drive_step9(x, REP, "skipped-by-runOn", True)
    t.eq("S03", "a STRUCTURAL skip is NOT fatal even under strict", 0, rc)
    rc, lt = drive_step9(x, REP, "skipped-launcher-prerequisite-missing", False, nat_verdict="skipped-by-runOn")
    t.ck("S04", "ZERO verified legs plus an environmental skip fails the run (the .ps1's rule, the union)",
         rc == 1 and "NO declared leg reached a VERIFIED verdict" in lt, (rc, lt[-1500:]))
    V = x.M.mod("sqlite_verdicts")
    classes = V.verdict_classes(x.suite.real)
    t.eq("G00", "the classes are READ from the resolver (--verdict-classes): the token is environmental",
         "environmental", classes.get("skipped-launcher-prerequisite-missing"))

    def two(v):
        a, b = C.Leg(leg_d("nat")), C.Leg(leg_d("lau"))
        a.verdict, b.verdict = "ran", v
        return [a, b]
    lc = V.ledger_counts(two("skipped-launcher-prerequisite-missing"), classes)
    t.eq("G01", "the ledger counts the token as ENVIRONMENTAL, with no accounting hole and nothing bogus",
         (1, [], [], True), (lc.environmental, list(lc.bogus), list(lc.unnamed), lc.accounted == lc.total))
    t.has("G02", "...and the counts line NAMES the class", V.counts_line(lc), "1 launcher-prerequisite-missing")
    lc2 = V.ledger_counts(two("skipped-because-i-said-so"), classes)
    t.eq("G03", "an OFF-vocabulary token is still filed as bogus, and the ledger does not add up",
         (["lau=skipped-because-i-said-so"], False), (list(lc2.bogus), lc2.accounted == lc2.total))
    rc, lt = drive_step9(x, REP, "skipped-because-i-said-so", False)
    t.ck("G04", "...which Step 9 announces as a LEDGER ACCOUNTING HOLE and fails",
         rc == 1 and "LEDGER ACCOUNTING HOLE" in lt, (rc, lt[-1200:]))
    rows = {}
    for code in (0, 1, 2, 3, 4, 9):
        lg, rn, _sp, _b, _l = drive_smoke(x, code)
        rows[code] = (lg.smoke_verdict or "", rn.counts["smoke"])
    v0, n0 = rows[0]
    t.ck("Q0", "smoke rc 0 passes and costs the run nothing", "PASS (14/14)" in v0 and n0 == 0, rows[0])
    v1, n1 = rows[1]
    t.ck("Q1", "smoke rc 1 IS charged to DSS (never an unknown rc) and reds the run",
         "CHARGED TO DSS" in v1 and "UNKNOWN rc" not in v1 and n1 == 1, rows[1])
    v2, n2 = rows[2]
    t.ck("Q2", "smoke rc 2 is OUR argv defect, not charged, pointing at the smoke log, and reds the run",
         "HARNESS ARGV DEFECT" in v2 and "CHARGED TO DSS" not in v2 and "smoke.log" in v2 and n2 == 1, rows[2])
    v3, n3 = rows[3]
    t.ck("Q3", "smoke rc 3 is NOT DSS (the gcc reference fails identically), not charged, and reds the run",
         "NOT DSS" in v3 and "CHARGED TO DSS" not in v3 and n3 == 1, rows[3])
    v4, n4 = rows[4]
    t.ck("Q4", "smoke rc 4 is NOT A VERDICT, not charged, and reds the run",
         "NOT A VERDICT" in v4 and "CHARGED TO DSS" not in v4 and n4 == 1, rows[4])
    v9, n9 = rows[9]
    t.ck("Q9", "an rc with no arm says UNKNOWN rc=9, blames nobody, and reds the run",
         "UNKNOWN rc=9" in v9 and "CHARGED TO DSS" not in v9 and n9 == 1, rows[9])


# ═══════════════════════════════════════════════════════════════════════════════════════
# DC-16 -- the smoke argv: MEASURED targets, the CATALOGUE's launcher, no host identity
# ═══════════════════════════════════════════════════════════════════════════════════════

FORBIDDEN_ATTRS = {("sys", "platform"), ("os", "name"), ("platform", "system"), ("platform", "machine"),
                   ("platform", "uname"), ("platform", "platform")}
FORBIDDEN_NAMES = {"host", "host_os", "host_arch", "needs_wsl", "host_is_wsl", "HostNeedsWsl", "HOST_OS",
                   "PosixSide"}


def host_identity_refs(node):
    hits = []
    for n in ast.walk(node):
        if isinstance(n, ast.Attribute):
            if isinstance(n.value, ast.Name) and (n.value.id, n.attr) in FORBIDDEN_ATTRS:
                hits.append("%s.%s" % (n.value.id, n.attr))
            elif n.attr in FORBIDDEN_NAMES:
                hits.append("." + n.attr)
        elif isinstance(n, ast.Name) and n.id in FORBIDDEN_NAMES:
            hits.append(n.id)
        elif isinstance(n, ast.arg) and n.arg in FORBIDDEN_NAMES:
            hits.append(n.arg)
    return hits


def pin_dc16(t, x):
    smoke = x.M.mod("sqlite_smoke")
    C = x.C
    xl = deterministic_translator([])

    def answer(a):
        if a[:2] == ["--path-translation", "windows-to-wsl"] and "--translate-path" in a:
            rc, out, err = xl(["wslpath", a[-1]])
            return C.Result(rc, out, err)
        return None
    R = FakeResolver(answer, C)
    leg = C.Leg(leg_d("macho64-x86_64", fmt="macho64-x86_64-darwin-exec", os_key="darwin", mode="launched",
                      launcher=["arch", "-x86_64"], run_extra={"pathTranslation": "windows-to-wsl"}))
    leg.cli_bin = "C:\\out\\macho64-x86_64\\sqlite3"
    measured = "x86_64:macho64:darwin"
    ref_l = ["qemu-x86_64", "-L", "/usr/x86_64-linux-gnu"]
    argv = smoke.smoke_argv(R, leg, measured, "3.99.0", "srcid", "C:\\out\\smoke", "C:\\out\\smoke\\result.json",
                            "/ref/sqlite3d", "x86_64:elf64:linux", ref_l)

    def after(flag):
        return argv[argv.index(flag) + 1] if flag in argv and argv.index(flag) + 1 < len(argv) else "<absent>"
    t.eq("A01", "the gate is THIS interpreter running cli-smoke.py", [sys.executable, C.CLI_SMOKE], argv[:2])
    t.eq("A02", "--leg-spec carries the leg's DECLARED spec", leg.spec, after("--leg-spec"))
    t.eq("A03", "--cli-target carries the MEASURED target (never the declaration again)", measured, after("--cli-target"))
    t.eq("A04", "--cli is the CLI spelled for the LAUNCHER (translated)", "/mnt/c/out/macho64-x86_64/sqlite3",
         after("--cli"))
    t.ck("A05", "every leg launcher token is `--launcher=<t>` (the `=` form survives a leading dash)",
         "--launcher=arch" in argv and "--launcher=-x86_64" in argv, argv)
    t.eq("A06", "--reference and --reference-target carry the reference and its MEASURED target",
         ("/ref/sqlite3d", "x86_64:elf64:linux"), (after("--reference"), after("--reference-target")))
    t.eq("A07", "every reference launcher token as --reference-launcher=<t>, in order",
         ["--reference-launcher=%s" % v for v in ref_l], [a for a in argv if a.startswith("--reference-launcher=")])
    bare = smoke.smoke_argv(R, leg, measured, "3.99.0", "srcid", "w", "r.json", "", "", [])
    t.eq("A08", "with NO reference, no --reference* argument at all", [],
         [a for a in bare if a.startswith("--reference")])
    lft = {"rc": 0}

    def answer2(a):
        if a[:1] == ["--identify-binary"]:
            return C.Result(0, "x86_64\telf64\tlinux\n", "") if lft.get("identify", True) else \
                C.Result(1, "", "cannot identify")
        if a[:1] == ["--launcher-for-target"]:
            if lft["rc"] == 0:
                return C.Result(0, "qemu-x86_64 -L /usr/x86_64-linux-gnu\n", "catalogue answer")
            return C.Result(lft["rc"], "", "the pinned launcher answer rc %d" % lft["rc"])
        return None

    def refrun():
        R2 = FakeResolver(answer2, C)
        run = types.SimpleNamespace(log=x.log(), resolver=R2, posix=types.SimpleNamespace(needs_wsl=True),
                                    stage={"reference_cli": "C:\\ref\\sqlite3d", "reference_cli_posix": "/ref/sqlite3d",
                                           "reference_cli_why": ""})
        return run, R2
    run, R2 = refrun()
    got = smoke.reference_cli(run)
    t.eq("F01", "the reference's target is MEASURED from its own header (--identify-binary)", "x86_64:elf64:linux",
         got[1])
    t.ck("F02", "the launcher is asked of the CATALOGUE for that MEASURED target (--launcher-for-target)",
         any(a[:2] == ["--launcher-for-target", "x86_64:elf64:linux"] for a in R2.asked), R2.asked)
    t.eq("F03", "...and the reference launcher IS the catalogue's answer", ref_l, got[2])
    t.eq("F04", "the gate is handed the reference in the POSIX-side spelling", "/ref/sqlite3d", got[0])
    lft["rc"] = 3
    run, R2 = refrun()
    got = smoke.reference_cli(run)
    t.ck("F05", "a reference this host cannot EXECUTE (rc 3) is DROPPED, and said", got == ("", "", [])
         and "cannot EXECUTE" in text(run.log), (got, text(run.log)[-600:]))
    lft["rc"] = 2
    run, R2 = refrun()
    got = smoke.reference_cli(run)
    t.ck("F06", "a malformed-triple answer (rc 2) is OUR defect: dropped, and said", got == ("", "", [])
         and "OUR defect" in text(run.log), (got, text(run.log)[-600:]))
    lft["rc"], lft["identify"] = 0, False
    run, R2 = refrun()
    got = smoke.reference_cli(run)
    t.ck("F07", "a reference that cannot be IDENTIFIED is dropped, never passed with a guessed target",
         got == ("", "", []) and "could not be IDENTIFIED" in text(run.log), (got, text(run.log)[-600:]))
    probe = ast.parse("def f(run):\n    return sys.platform, os.name, run.posix.needs_wsl\n")
    t.eq("H00", "control: the host-identity detector SEES host identity where it is", 3, len(host_identity_refs(probe)))
    tree = ast.parse(source_of(smoke))
    fns = dict((n.name, n) for n in tree.body if isinstance(n, ast.FunctionDef))
    for key, name in (("H01", "smoke_argv"), ("H02", "reference_cli"), ("H03", "launcher_for_target"),
                      ("H04", "identify_binary"), ("H05", "step7c")):
        t.ck(key, "%s exists and references NO host identity (sys.platform, os.name, platform.*, host, "
             "needs_wsl ...)" % name, name in fns and not host_identity_refs(fns[name]),
             host_identity_refs(fns[name]) if name in fns else "%s not found" % name)


# ═══════════════════════════════════════════════════════════════════════════════════════
# DC-17 -- WHY a failure was excused is PRINTED; an empty account is refused
# ═══════════════════════════════════════════════════════════════════════════════════════

def pin_dc17(t, x):
    V = x.M.mod("sqlite_verdicts")
    log = x.log()
    r, _v = refused(V.print_confound_report, "elf64-x86_64",
                    "probe clock-realtime-steps = ABSENT\n\nrow INACTIVE: ^walsetlk-", log)
    t.eq("R01", "a report is printed line by line", False, r)
    t.eq("R02", "...the probe line and the INACTIVE row; the BLANK line is skipped (exactly 2 lines)",
         ["   probe clock-realtime-steps = ABSENT", "   row INACTIVE: ^walsetlk-"], text(log).splitlines())
    r3, m3 = refused(V.print_confound_report, "elf64-x86_64", "   ", x.log())
    t.ck("R03", "a whitespace-only report REFUSES rather than printing nothing", r3, m3)
    t.has("R04", "...naming what is missing", m3, "EMPTY confound report")
    r5, m5 = refused(V.print_confound_report, "elf64-x86_64", "", x.log())
    t.ck("R05", "an EMPTY report refuses too", r5 and "EMPTY confound report" in str(m5), m5)
    log6 = x.log()
    refused(V.print_confound_report, "elf64-x86_64", "a\r\n\r\nb\r\n", log6)
    t.eq("R06", "a CRLF report: one trailing CR stripped per line, blank lines skipped", ["   a", "   b"],
         text(log6).splitlines())


# ═══════════════════════════════════════════════════════════════════════════════════════
# DC-18 -- the compiler is PROVED current before the run is spent (children: the run stops)
# ═══════════════════════════════════════════════════════════════════════════════════════

STALE_LINE = "error[C_InvalidSemantics]: unknown key 'restrictMarker' in 'declarations[0]'"


def fake_dsscp(dirpath, name, line):
    """A fake compiler: `.cmd` on Windows (✔MEASURED 2026-08-31: an extensionless `#!/bin/sh` file
    is WinError 193 there), `#!/bin/sh` elsewhere. ASCII, no cmd metacharacters in `line`."""
    if os.name == "nt":
        p = os.path.join(dirpath, name + ".cmd")
        body = "@echo off\r\n" + ("echo %s\r\n" % line if line else "") + "exit /b 0\r\n"
        write_bytes(p, body.encode("ascii"))
    else:
        p = os.path.join(dirpath, name)
        body = "#!/bin/sh\n" + ("echo \"%s\"\n" % line if line else "") + "exit 0\n"
        write_bytes(p, body.encode("ascii"))
        os.chmod(p, 0o755)
    return p


def pin_dc18(t, x):
    root = x.sub("currency")
    stale = fake_dsscp(root, "stale-dsscp", STALE_LINE)
    ok = fake_dsscp(root, "current-dsscp", "")
    cfgroot = os.path.join(root, "cfgroot")
    os.makedirs(os.path.join(cfgroot, "src", "dss-config"))
    noroot = os.path.join(root, "no-config-root")
    os.makedirs(noroot)
    tree = os.path.join(root, "a-tree-that-produced-this-binary")
    os.makedirs(tree)
    repo = os.path.join(root, "repo")
    os.makedirs(repo)
    core = os.path.join(HERE, "speedtest1_bench.py")
    stub = write_bytes(os.path.join(root, "rc2-core.py"),
                       b'import sys\nprint("usage: ...", file=sys.stderr)\nsys.exit(2)\n')
    specs = ["x86_64:elf64-x86_64-linux-exec", "x86_64:elf64-x86_64-linux-exec", "x86_64:pe64-x86_64-windows-exec"]
    legs = [["elf64-x86_64", "x86_64:elf64-x86_64-linux-exec"], ["pe64-x86_64", "x86_64:pe64-x86_64-windows-exec"]]
    # "The run STOPS at Step 5" is a property of a PROCESS: build_and_test.run_all's own call site runs
    # in a child per arm. The classification arms call assert_current in THIS process meanwhile (its
    # pre-flights are real child processes either way; a refusal is the HarnessDie it raises).
    jobs = []
    for key, dss, extra in (("site-stale", stale, None), ("site-current", ok, None),
                            ("site-skip", stale, {"SKIP_DSS_BUILD": "1"})):
        spec = write_json(os.path.join(root, key + ".json"), {
            "dsscp": dss, "config_root": cfgroot, "legs": legs, "tree": tree, "repo_root": repo,
            "override": x.M.override_spec()})
        jobs.append((key, [sys.executable, THIS_FILE, "--child", "currency", spec], child_env(x, extra)))
    running = start_children(jobs)
    CMP = x.M.mod("sqlite_compiler")
    res = {}
    try:
        for key, dss, cfg, cr in (("call-stale", stale, cfgroot, core), ("call-current", ok, cfgroot, core),
                                  ("call-noroot", ok, noroot, core), ("call-rc2", ok, cfgroot, stub),
                                  ("call-nocore", ok, cfgroot, os.path.join(root, "no-such-core.py"))):
            comp = CMP.Compiler(path=dss, type="Release", source="the contract suite", detail="", tree=tree,
                                origin="LOCATED under an eligible build root %s NOT built by this run" % DASH,
                                built="2026-08-28 09:30:36", build_type_note="")
            r, v = refused(CMP.assert_current, cr, comp, cfg, specs, CMP.rebuild_command(comp, repo))
            res[key] = (1, "DIE: %s" % v) if r else (0, "REACHED-NEXT-STATEMENT ok=[%s]" % v)
    finally:
        res.update(collect_children(running))
    rc, out = res["site-stale"]
    t.eq("O01", "a STALE compiler STOPS the driver at Step 5 (build_and_test.run_all's own call site; rc)", 1, rc)
    t.has("O02", "...naming the BINARY", out, stale)
    t.has("O03", "...naming WHEN it was built", out, "built     : 2026-08-28 09:30:36")
    t.has("O04", "...naming how it was obtained", out, "NOT built by this run")
    t.has("O05", "...naming the REBUILD command for the tree THIS binary came from", out, "REBUILD IT: cmake --build " + tree)
    t.has("O06", "...quoting the compiler's own diagnostic", out, "unknown key 'restrictMarker'")
    t.lacks("O07", "...and the statement AFTER Step 5 never ran", out, "REACHED-NEXT-STATEMENT")
    rc, out = res["site-current"]
    t.eq("O08", "a CURRENT compiler is let through Step 5 (rc)", 0, rc)
    t.has("O09", "...reaching the next statement having proved BOTH selected targets, in order", out,
          "x86_64:elf64-x86_64-linux-exec, x86_64:pe64-x86_64-windows-exec")
    rc, out = res["site-skip"]
    t.eq("O10", "SKIP_DSS_BUILD=1 does NOT exempt a stale binary (rc)", 1, rc)
    t.ck("O11", "...and the refusal says a reused binary is not exempt, naming SKIP_DSS_BUILD=1",
         "DOES NOT EXEMPT IT FROM THIS CHECK" in out and "SKIP_DSS_BUILD=1" in out, out[-1500:])
    t.lacks("O12", "...and the statement AFTER Step 5 never ran", out, "REACHED-NEXT-STATEMENT")
    rc, out = res["call-stale"]
    t.ck("O13", "only rc 1 of the pre-flight ACCUSES: THE COMPILER CANNOT COMPILE THREE LINES",
         rc == 1 and "CANNOT COMPILE THREE LINES" in out, (rc, out[-1200:]))
    rc, out = res["call-current"]
    t.ck("O14", "one probe per DISTINCT target, deduplicated and in order",
         rc == 0 and "ok=[x86_64:elf64-x86_64-linux-exec, x86_64:pe64-x86_64-windows-exec]" in out, (rc, out[-1200:]))
    rc, out = res["call-noroot"]
    t.eq("O15", "a check that CANNOT RUN (no src/dss-config) still REFUSES (the run stops)", 1, rc)
    t.has("O16", "...saying the compiler was NOT judged", out, "COULD NOT RUN")
    t.has("O17", "...and saying so about staleness explicitly", out, "NOTHING above says that binary is stale")
    t.lacks("O18", "...and it does NOT tell the operator to rebuild", out, "REBUILD IT:")
    rc, out = res["call-rc2"]
    t.eq("O19", "an UNEXPECTED exit code of the pre-flight REFUSES too (the run stops)", 1, rc)
    t.has("O20", "...reported as COULD NOT RUN, naming the code", out,
          "COULD NOT RUN for target x86_64:elf64-x86_64-linux-exec (exit 2)")
    t.lacks("O21", "...and NOT as an accusation against the compiler", out, "CANNOT COMPILE THREE LINES")
    rc, out = res["call-nocore"]
    t.ck("O22", "a MISSING pre-flight core REFUSES as CANNOT RUN, never as a stale binary",
         rc == 1 and "CANNOT RUN" in out and "REBUILD IT:" not in out, (rc, out[-1200:]))


# ═══════════════════════════════════════════════════════════════════════════════════════
# DC-19 -- execution evidence: no monitor without an armed row; the attribution is FOLDED
# ═══════════════════════════════════════════════════════════════════════════════════════

FOLD_LOG = ["select1-1.0... Ok", "Time: select1.test 3 ms", "! walsetlk-2.2.3 expected: [1]",
            "! walsetlk-2.2.3 got: [0]", "! select1-1.1 expected: [2]", "! select1-1.1 got: [3]",
            "! zipfile-25.0 expected: [x]", "! zipfile-25.0 got: [y]", "Time: walsetlk.test 5 ms",
            "3 errors out of 100 tests on host the-contract-suite 64-bit"]


def attribution_answer(C):
    def answer(a):
        if a[:1] == ["--attribute-unit-failures"]:
            if "--failure=explode-1.1" in a:
                return C.Result(3, "", "the resolver refused on purpose")
            return C.Result(0, "EXCUSED\twalsetlk-2.2.3\nGENUINE\tselect1-1.1\nREPORT\t[elf64-x86_64] per-failure "
                               "clock attribution: walsetlk-2.2.3 EXCUSED by ^walsetlk- (stub)\n", "")
        if a[:1] == ["--execution-monitor-argv"]:
            return C.Result(9, "", "no execution monitor in the contract suite")
        if a[:1] == ["--registry-controls"]:
            return C.Result(0, "", "")
        return None
    return answer


def pin_dc19(t, x):
    L = x.M.mod("sqlite_launch")
    C = x.C
    noarm = C.Leg(leg_d("pe64-x86_64", fmt="pe64-x86_64-windows-exec", os_key="windows"))
    armed = C.Leg(leg_d("elf64-x86_64", confoundsByEvidence=["^walsetlk-"], executionEvidence=["clock-realtime-steps"]))
    keep = write_bytes(os.path.join(x.tmp, "untouched.log"), b"keep me\n")
    R = FakeResolver(lambda a: None, C)
    log = x.log()
    mons = L.evidence_start(R, noarm, keep, 0, False, log)
    t.eq("M01", "a leg with no armed row starts NO monitor", [], list(mons))
    t.eq("M02", "...its segment log is NOT emptied", b"keep me\n", read_bytes(keep))
    t.eq("M03", "...and nothing is asked of the resolver, nothing warned", ([], []), (R.asked, warn_lines(log)))
    keep2 = write_bytes(os.path.join(x.tmp, "override.log"), b"keep me too\n")
    R2, log2 = FakeResolver(lambda a: None, C), x.log()
    mons2 = L.evidence_start(R2, armed, keep2, 0, True, log2)
    t.eq("M04", "the operator override starts NO monitor even for an ARMED leg", [], list(mons2))
    t.eq("M05", "...leaves that log untouched and asks the resolver nothing", (b"keep me too\n", []),
         (read_bytes(keep2), R2.asked))
    log3 = x.log()
    r6, _v = refused(L.evidence_stop, "pe64-x86_64", [], log3)
    t.eq("M06", "stopping an EMPTY monitor list is a no-op (no error, nothing said)", (False, ""), (r6, text(log3)))
    R4 = FakeResolver(lambda a: None, C)
    got = L.evidence_attribute(R4, noarm, "native", ["x.log"], [], ["walsetlk-2.2.3"], False, x.log())
    t.eq("M07", "attribution with no armed row excuses NOTHING and asks nothing", (set(), []), (got, R4.asked))
    got = L.evidence_attribute(R4, armed, "native", ["x.log"], [], ["walsetlk-2.2.3"], True, x.log())
    t.eq("M08", "the override attributes nothing and asks nothing", (set(), []), (got, R4.asked))
    R5, log5 = FakeResolver(attribution_answer(C), C), x.log()
    got = L.evidence_attribute(R5, armed, "native", ["seg0.log"], [], ["walsetlk-2.2.3", "select1-1.1"], False, log5)
    t.eq("M09", "the resolver's EXCUSED names are returned, and its REPORT line printed",
         ({"walsetlk-2.2.3"}, True), (got, "EXCUSED by ^walsetlk-" in text(log5)))
    R6, log6 = FakeResolver(attribution_answer(C), C), x.log()
    got = L.evidence_attribute(R6, armed, "native", ["seg0.log"], [], ["walsetlk-2.2.3", "explode-1.1"], False, log6)
    t.eq("M10", "an attribution that could NOT run excuses nothing and warns so", (set(), True),
         (got, "could NOT run" in text(log6)))
    over = {"confoundsByEvidence": ["^walsetlk-"], "executionEvidence": ["clock-realtime-steps"]}
    f = drive_corpus(x, x.M.fresh("sqlite_units", P=fake_procs(x)), "fold",
                     lambda k: ("\n".join(FOLD_LOG) + "\n").encode("utf-8"), patterns=["^zipfile-"], leg_over=over,
                     answer=attribution_answer(C))
    v = f.leg.unit_verdict or ""
    t.ck("F01", "the fold (REAL loop, judge_leg): an EXCUSED name LEAVES the genuine list",
         v.startswith("FAIL:1 genuine unit failure(s): select1-1.1"), v[:300])
    t.has("F02", "...and JOINS the confounds", f.log, "(+2 known confound(s) ignored: zipfile-25.0 walsetlk-2.2.3)")
    t.ck("F03", "...and is named as excused PER FAILURE", any("excused PER FAILURE" in ln and "walsetlk-2.2.3" in ln
                                                               for ln in f.log.splitlines()), f.log[-1500:])
    bad = FOLD_LOG[:2] + ["! walsetlk-2.2.3 expected: [1]", "! walsetlk-2.2.3 got: [0]",
                          "! explode-1.1 expected: [2]", "! explode-1.1 got: [3]"] + FOLD_LOG[6:]
    g = drive_corpus(x, x.M.fresh("sqlite_units", P=fake_procs(x)), "fold-failed",
                     lambda k: ("\n".join(bad) + "\n").encode("utf-8"), patterns=["^zipfile-"], leg_over=over,
                     answer=attribution_answer(C))
    gv = g.leg.unit_verdict or ""
    t.ck("F04", "a FAILED attribution folds nothing: both failures stay GENUINE",
         gv.startswith("FAIL:2 genuine unit failure(s): explode-1.1 walsetlk-2.2.3"), gv[:300])
    t.lacks("F05", "...it excuses nothing", g.log, "excused PER FAILURE")
    t.has("F06", "...and warns that it could NOT run", g.log, "could NOT run")


# ═══════════════════════════════════════════════════════════════════════════════════════
# DC-20 -- the leftover-fixture sweep: a real decoy, self/ancestor exclusion, UNVERIFIED
# ═══════════════════════════════════════════════════════════════════════════════════════

def start_native_decoy(x, tag):
    """A sleeping decoy carrying a UNIQUE needle: its command line (POSIX), or -- because the
    Windows sweep matches the IMAGE path -- a copy of sys.executable (and its DLLs, PYTHONHOME set)
    at a unique path. -> (Popen, needle, readiness file, why-not): Popen None only when THIS
    interpreter cannot be copied at all (an app-execution alias is not an image)."""
    ddir = os.path.join(x.tmp, "dss-decoy-" + tag)
    os.makedirs(ddir)
    ready = os.path.join(x.tmp, "dss-decoy-%s.ready" % tag)
    if os.name == "nt":
        image = os.path.join(ddir, "testfixture.exe")
        try:
            shutil.copy2(sys.executable, image)
            exe_dir = os.path.dirname(os.path.realpath(sys.executable))
            for n in os.listdir(exe_dir):
                if n.lower().endswith(".dll"):
                    shutil.copy2(os.path.join(exe_dir, n), os.path.join(ddir, n))
        except OSError as exc:
            return None, image, ready, "this interpreter (%s) cannot be copied to a unique image path: %s" % (
                sys.executable, exc)
        if os.path.getsize(image) == 0:
            return None, image, ready, ("this interpreter (%s) is an app-execution alias, not a copyable image"
                                        % sys.executable)
        argv, needle, env = [image, "-c", DECOY_CODE, ready], image, child_env(x, {"PYTHONHOME": sys.base_prefix})
    else:
        needle = os.path.join(ddir, "testfixture")
        argv, env = [sys.executable, "-c", DECOY_CODE, ready, needle], child_env(x)
    p = subprocess.Popen(argv, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, env=env)
    return p, needle, ready, ""


def pin_dc20(t, x):
    P = x.M.mod("sqlite_procs")

    def broken(prefix):
        return P.Enumeration([], False, "injected: the contract suite's enumerator cannot run")
    en = P.our_fixture_pids("/x/testfixture", enumerator=broken)
    t.ck("U01", "an enumeration that cannot run is passed through UNVERIFIED, with its reason",
         not en.verified and "injected" in en.why, repr(en))
    log = x.log()
    sw = P.stop_our_fixtures("/x/testfixture", "contract-suite", log=log, enumerator=broken)
    t.ck("U02", "...stop_our_fixtures kills nothing and WARNS 'UNVERIFIED' with the reason (never 'none found')",
         list(sw) == [] and not sw.verified and "UNVERIFIED" in text(log) and "injected" in text(log), text(log))
    t.ck("U03", "an EMPTY fixture path is UNVERIFIED (never 'matches everything')", not P.our_fixture_pids("").verified)
    real = P.our_fixture_pids(os.path.join(x.tmp, "not-running", "testfixture"))
    if not real.verified:
        # The rest of this pin LOOKS at processes; a host that cannot look cannot run it -- and a red arm
        # of this pin then skips by name instead of reading as VACUOUS (PinSkip, not a sub-arm skip).
        raise PinSkip("this host's process enumeration cannot run (%s), so the decoy and process-chain arms "
                      "cannot look" % real.why)
    tag = uuid.uuid4().hex[:12]
    proc, needle, ready, why_not = start_native_decoy(x, tag)
    try:
        pid = await_ready(ready, proc) if proc is not None else None
        if proc is None:
            t.skip("D01", "the native decoy arms (found by its image path, killed, gone)", why_not)
        else:
            t.ck("D01", "the decoy is ALIVE (readiness handshake: its own pid) before the sweep runs",
                 pid is not None and proc.poll() is None, "decoy rc=%r" % proc.poll())
        if pid is not None:
            found = P.our_fixture_pids(needle)
            t.ck("D02", "the sweep FINDS it by its %s" % ("IMAGE path" if os.name == "nt" else "command line"),
                 found.verified and len(found.procs) >= 1, repr(found)[:900])
            t.eq("D03", "...and the pid found is the decoy's", [pid], [r[0] for r in found.procs])
            other = P.our_fixture_pids(os.path.join(x.tmp, "dss-decoy-unrelated", os.path.basename(needle)))
            t.ck("D04", "an unrelated path matches NOTHING (D02 is its positive)", other.verified and not other.procs,
                 repr(other)[:900])
            klog = x.log()
            killed = P.stop_our_fixtures(needle, "contract-suite", settle_s=0, log=klog)
            if pid in list(killed):     # only a kill the sweep CLAIMS is waited for; else D07 reds at once
                try:
                    proc.wait(timeout=30)
                except subprocess.TimeoutExpired:
                    pass
            t.eq("D05", "stop_our_fixtures kills it and returns its pid", [pid], list(killed))
            t.ck("D06", "...the kill is REPORTED as LEFTOVER FIXTURE, with the pid",
                 "LEFTOVER FIXTURE: contract-suite" in text(klog) and str(pid) in text(klog), text(klog))
            t.ck("D07", "...and the decoy is dead", proc.poll() is not None)
            again = P.our_fixture_pids(needle)
            t.ck("D08", "a second sweep finds nothing", again.verified and not again.procs, repr(again)[:900])
    finally:
        reap(proc)
    chain = x.sub("chain")
    marker = os.path.join(chain, "dss-chain-%s" % tag, "testfixture")
    spec = write_json(os.path.join(chain, "spec.json"), {"tmp": chain, "override": x.M.override_spec()})
    res = run_children([("chain", [sys.executable, THIS_FILE, "--child", "sweep-parent", spec, marker], child_env(x))])
    rc, out = res["chain"]
    got = last_json_line(out)
    if not got:
        t.ck("A00", "the process chain (parent -> probe + sibling) answered", False, out[-2000:])
        return
    t.eq("A01", "control: the probe child AND its parent both match the needle in the raw listing",
         sorted([got["me"], got["parent"]]), sorted(got["raw"]))
    t.ck("A02", "the probe's sweep FINDS a same-needle process that is not its ancestor (a sibling): it can see",
         got["verified"] and got["sibling"] in got["hits"], got)
    t.ck("A03", "...and spares the probe itself", got["me"] not in got["hits"], got)
    t.ck("A04", "...and spares its ancestors (its parent, and this suite)",
         got["parent"] not in got["hits"] and os.getpid() not in got["hits"], got)


# ═══════════════════════════════════════════════════════════════════════════════════════
# DC-21 -- a LOCATED compiler is REFRESHED, not trusted (Step 5, with an injected builder)
# ═══════════════════════════════════════════════════════════════════════════════════════

def fake_tree(root, rel, btype):
    tree = os.path.join(root, *rel.split("/"))
    write_bytes(os.path.join(tree, "CMakeCache.txt"),
                ("CMAKE_BUILD_TYPE:STRING=%s\nCMAKE_GENERATOR:INTERNAL=Ninja\n" % btype).encode("ascii"))
    binary = write_bytes(os.path.join(tree, "bin", "dss", "dsscp"), b"not really a compiler\n")
    return tree, binary


def pin_dc21(t, x):
    CMP = x.M.mod("sqlite_compiler")
    seen = []

    def ok_build(tree, jobs):
        seen.append((tree, jobs))
        return 0
    ret = CMP.refresh_located("T:/rel", "T:/rel/bin/dss/dsscp.exe", "2026-08-31 23:25:16", 7, ok_build)
    t.eq("P1", "the LOCATED tree is what gets rebuilt, with this run's job count", [("T:/rel", 7)], seen)
    t.eq("P2", "...and the refresh reports back the tree it built", "T:/rel", ret)
    r, msg = refused(CMP.refresh_located, "T:/rel", "T:/rel/bin/dss/dsscp.exe", "2026-08-31", 7, lambda tr, j: 1)
    t.ck("P3", "a FAILED rebuild REFUSES, never falls back to the located binary", r, msg)
    t.has("P4", "...saying which defect a fallback would reinstate", msg, "older than the sources it compiles")
    r, msg = refused(CMP.refresh_located, "", "T:/somewhere/dsscp.exe", "2026-08-31", 7, ok_build)
    t.ck("P5", "a binary with NO build tree REFUSES, and is not silently reused", r, msg)
    t.has("P6", "...and the two refusals are told apart", msg, "build TREE could not be determined")
    r, msg = refused(CMP.refresh_located, "T:/rel", "T:/rel/dsscp", "w", 7, lambda tr, j: "0")
    t.ck("P6b", "a builder answering no EXIT CODE (a string) is refused in its own words", r and
         "did not return an EXIT CODE" in str(msg), msg)
    r, msg = refused(CMP.refresh_located, "T:/rel", "T:/rel/dsscp", "w", 7, lambda tr, j: True)
    t.ck("P6c", "...and so is a boolean (True is not an exit code)", r and "did not return an EXIT CODE" in str(msg), msg)
    repo = x.sub("repo")
    tree, binary = fake_tree(repo, "build/rel", "Release")
    spy = []
    with patched_environ(unset=x.suite.knobs):
        r, comp = refused(CMP.obtain, repo, 7, False, log=x.log(),
                          invoke_build=lambda tr, j: (spy.append((os.path.normcase(tr), j)), 0)[1])
    t.eq("P7", "Step 5 rebuilds the LOCATED Release tree with the run's job count (the call site CALLS the "
         "refresh)", [(os.path.normcase(os.path.abspath(tree)), 7)], spy)
    t.eq("P8", "...and the origin says the run rebuilt it", "LOCATED under an eligible build root, then REBUILT by "
         "this run (incremental)", getattr(comp, "origin", comp))
    with patched_environ(unset=x.suite.knobs):
        r, msg = refused(CMP.obtain, repo, 7, False, log=x.log(), invoke_build=lambda tr, j: 3)
    t.ck("P9", "a failed refresh through Step 5 is FATAL", r and "older than the sources it compiles" in str(msg), msg)
    spy2 = []
    with patched_environ(unset=x.suite.knobs, SKIP_DSS_BUILD="1"):
        r, comp = refused(CMP.obtain, repo, 7, False, log=x.log(),
                          invoke_build=lambda tr, j: (spy2.append(tr), 0)[1])
    t.eq("P10", "SKIP_DSS_BUILD=1 reuses the located Release binary and NEVER builds",
         ([], "REUSED under SKIP_DSS_BUILD=1 %s NOT built by this run" % DASH), (spy2, getattr(comp, "origin", comp)))
    with patched_environ(unset=x.suite.knobs, DSS_BIN=os.path.join(repo, "no-such-dsscp")):
        r, msg = refused(CMP.obtain, repo, 7, False, log=x.log(), invoke_build=ok_build)
    t.ck("P11", "DSS_BIN naming a missing file is REFUSED (never replaced by a searched binary)",
         r and "does not name an existing file" in str(msg), msg)
    spy3 = []
    with patched_environ(unset=x.suite.knobs, DSS_BIN=binary):
        r, comp = refused(CMP.obtain, repo, 7, False, log=x.log(), invoke_build=lambda tr, j: (spy3.append(tr), 0)[1])
    t.eq("P12", "DSS_BIN naming a binary uses it and never builds",
         ([], "named by DSS_BIN %s NOT built by this run" % DASH), (spy3, getattr(comp, "origin", comp)))
    repo2 = x.sub("repo-debug")
    fake_tree(repo2, "build/rel", "Debug")
    with patched_environ(unset=x.suite.knobs, SKIP_DSS_BUILD="1"):
        r, msg = refused(CMP.obtain, repo2, 7, False, log=x.log(), invoke_build=ok_build)
    t.ck("P13", "a Debug-only tree under SKIP_DSS_BUILD=1 is REFUSED: only a Release compiler is eligible",
         r and "rejected on BUILD TYPE" in str(msg), msg)
    repo3 = x.sub("repo-variant")
    vtree, _vb = fake_tree(repo3, "build/x86_64-mingw-release", "Release")
    fake_tree(repo3, "out/rel", "Release")
    cands, _searched = CMP.find_candidates(repo3)
    t.eq("P14", "a Release tree under ANY build/<name> (DssHarness's variant layout) is found; one outside "
         "build/ is not", [os.path.normcase(os.path.abspath(vtree))],
         [os.path.normcase(c.tree) for c in cands])


# ═══════════════════════════════════════════════════════════════════════════════════════
# DC-22 -- what a leftover-fixture sweep LEARNT reaches the leg's verdict (the REAL run_corpus)
# ═══════════════════════════════════════════════════════════════════════════════════════

SWEEP_KENTRY = ["wsl.exe", "-e"]    # a kernel entry that is NEVER spawned: the procs stand-in answers
SWEEP_KILLED, SWEEP_STUCK = 5151, 4242
SWEEP_STUCK_HOW = "it outlived TERM, the grace and KILL (the contract suite's injection)"
SWEEP_BLIND_WHY = "the contract suite's in-kernel enumerator could not run"


def scripted_sweep(stuck=False, blind=False, kill_at=None):
    """-> a `sweep` for fake_procs. On this host: an UNKILLABLE leftover at every sweep (`stuck`)
    and one leftover killed by the sweep named `kill_at`; inside the fixture's kernel: a sweep that
    could not LOOK (`blind`). Each fact travels in the real `Sweep` type, as sqlite_procs reports it."""
    def sweep(P, why, prefix):
        if prefix:
            return P.Sweep(False, SWEEP_BLIND_WHY) if blind else P.Sweep(True, "a clean in-kernel sweep")
        sw = P.Sweep(True, "a scripted host sweep")
        if why == kill_at:
            sw.append(SWEEP_KILLED)
        if stuck:
            sw.failed.append((SWEEP_STUCK, SWEEP_STUCK_HOW))
        return sw
    return sweep


def pin_dc22(t, x):
    def drive(tag, sweep):
        procs = fake_procs(x, sweep=sweep)
        s = drive_corpus(x, x.M.fresh("sqlite_units", P=procs), tag, lambda k: b"", kentry=SWEEP_KENTRY)
        rep = s.leg.unit_report if isinstance(s.leg.unit_report, dict) else {}
        asked = sorted((c[2], c[3]) for c in procs.calls if c[0] == "stop")
        return s, [str(h) for h in rep.get("hygiene") or []], s.leg.unit_verdict or "", asked

    def naming(pid, entries):
        return [h for h in entries if re.search(r"(?<![0-9])%d(?![0-9])" % pid, h)]
    both = sorted((p, k) for p in ("pre-corpus", "after segment 1", "after segment 2") for k in ([], SWEEP_KENTRY))
    s, hyg, verdict, asked = drive("facts", scripted_sweep(stuck=True, blind=True, kill_at="after segment 1"))
    t.eq("H01", "the REAL run_corpus reached its subject: two silent segments (a PRECONDITION stop), and BOTH "
         "sweeps (this host; the fixture's kernel) asked pre-corpus and after each segment",
         (2, "PRECONDITION", both), (s.segments, s.stop, asked))
    stuck = [h for h in naming(SWEEP_STUCK, hyg) if SWEEP_STUCK_HOW in h]
    t.ck("H02", "an UNKILLABLE leftover reaches unit_report['hygiene'] with its pid and why the kill did not take",
         stuck, hyg)
    t.ck("H03", "...and the leg's VERDICT: its [PROCESS HYGIENE suffix carries that record",
         stuck and "[PROCESS HYGIENE" in verdict and all(h in verdict for h in stuck), verdict)
    blind = [h for h in hyg if SWEEP_BLIND_WHY in h]
    t.ck("H04", "a BLIND in-kernel sweep reaches unit_report['hygiene'] with its reason and where it looked -- "
         "ONCE across the three blind sweeps (pre-corpus and two segments)",
         len(blind) == 1 and " ".join(SWEEP_KENTRY) in blind[0], hyg)
    t.ck("H05", "...and the verdict carries it, once", verdict.count(SWEEP_BLIND_WHY) == 1, verdict)
    killed = naming(SWEEP_KILLED, hyg)
    t.ck("H06", "a KILLED leftover reaches both as well (the record the flattening kept)",
         killed and all(h in verdict for h in killed), hyg)
    s2, hyg2, verdict2, asked2 = drive("clean", scripted_sweep())
    t.eq("H07", "NEGATIVE: clean sweeps (each LOOKED; nothing killed, nothing failed) add NO hygiene -- on the "
         "same two-segment drive, both sweeps asked at every point", (2, "PRECONDITION", both, []),
         (s2.segments, s2.stop, asked2, hyg2))
    t.ck("H08", "NEGATIVE: ...and the verdict has no [PROCESS HYGIENE suffix",
         verdict2.startswith("FAIL:PRECONDITION") and "[PROCESS HYGIENE" not in verdict2, verdict2)


# ═══════════════════════════════════════════════════════════════════════════════════════
# child scenarios (`--child <scenario> <spec.json> ...`)
# ═══════════════════════════════════════════════════════════════════════════════════════

def child_supply(spec, _spec_path, _rest):
    """DC-11: drive the REAL call site (sqlite_units.unit_leg) up to the confound supply; the
    statement after it prints a marker. A refusal is handled as the driver's main() does: exit 1."""
    import sqlite_common as C
    import sqlite_units as U
    tmp = spec["tmp"]
    log = C.Log(sys.stdout)
    run = C.Run(C.Config(), log=log)
    run.out_dir = os.path.join(tmp, "out")
    run.ledger = C.Ledger(spec["vocab"], log)
    run.compiler = types.SimpleNamespace(path=os.path.join(tmp, "dsscp"))
    run.stage = {"src": os.path.join(tmp, "sqlite", "src"), "bld": os.path.join(tmp, "sqlite", "bld")}
    run.loadext_builder = "dss"
    rdg = spec["rdg"]

    def answer(a):
        if a[:1] == ["--run-dir-plan"]:
            return C.Result(0, json.dumps({"runFilesystem": "driver", "launcherPath": "", "launcher": [],
                                           "rmTreeArgv": [], "mkdirArgv": [], "copyArgv": [],
                                           "kernelEntryArgv": []}), "")
        if a[:1] == ["--build-loadext-helper"]:
            dest = a[a.index("--dest-dir") + 1]
            return C.Result(0, json.dumps({"staged": os.path.join(dest, "libtestloadext.so"),
                                           "detail": "staged by the contract suite's fake resolver",
                                           "crossCheck": ""}), "")
        if a[:1] == ["--corroborate-run-dir"]:
            sup = [a[k + 1] for k, v in enumerate(a) if v == "--supplied"]
            return C.Result(0, json.dumps({"confounds": sup, "abortConfounds": [], "runDirectoryGating": rdg,
                                           "reportText": "row ^busy2-: corroboration answered by the contract "
                                                         "suite (%s)" % rdg}), "")
        return None
    run.resolver = FakeResolver(answer, C)
    d = leg_d("someleg", confoundGating=spec["gating"], confoundsByName=["^busy2-"],
              confoundReport=["row ^busy2-: ACTIVE (the contract suite's plan)"], confoundRows=[{"pattern": "^busy2-"}])
    leg = C.Leg(d)
    leg.tcl_lib = os.path.join(tmp, "lib", "libtcl8.6.so")
    leg.fixture_built, leg.fixture, leg.cc = True, os.path.join(tmp, "testfixture"), []

    def reached(run_, leg_, ctx_, rundir, plan, launch_bin, kentry, patterns, runner=None):
        print("REACHED-NEXT-STATEMENT size=%d" % len(patterns), flush=True)
    U.run_corpus = reached
    try:
        U.unit_leg(run, leg, {})
    except C.HarnessDie as exc:
        print("DIE: %s" % exc, flush=True)
        return exc.exit_code
    return 0


def child_currency(spec, _spec_path, _rest):
    """DC-18: build_and_test.run_all's OWN Step-5 call site, the steps before it stubbed (Step 6's
    first statement prints the marker), a refusal handled as the driver's main() does: exit 1."""
    import sqlite_common as C
    import sqlite_compiler as CMP
    comp = CMP.Compiler(path=spec["dsscp"], type="Release", source="the contract suite", detail="",
                        tree=spec["tree"], origin="LOCATED under an eligible build root %s NOT built by this run"
                        % DASH, built="2026-08-28 09:30:36", build_type_note="")
    try:
        import build_and_test as BT
        import sqlite_build as BLD
        log = C.Log(sys.stdout)
        run = C.Run(C.Config(), log=log)
        run.repo_root = spec["repo_root"]
        run.legs = [C.Leg(leg_d(lbl, spec=sp)) for lbl, sp in spec["legs"]]
        for step in ("step0", "step1", "step2", "step34"):
            setattr(BT, step, lambda run_: None)
        CMP.obtain = lambda *a, **k: comp
        CMP.pin_config_root = lambda repo_root, log=None: spec["config_root"]

        def reached(run_):
            print("REACHED-NEXT-STATEMENT ok=[%s]" % run_.currency_note, flush=True)
            raise SystemExit(0)
        BLD.stage_headers = reached
        BT.run_all(run)
        print("run_all RETURNED without reaching Step 6", flush=True)
        return 5
    except C.HarnessDie as exc:
        print("DIE: %s" % exc, flush=True)
        return exc.exit_code


def child_sweep_parent(spec, spec_path, rest):
    """DC-20: start a same-needle SIBLING and the PROBE; relay the probe's answer."""
    marker = rest[0]
    ready = os.path.join(spec["tmp"], "sibling.ready")
    env = dict(os.environ)
    s = subprocess.Popen([sys.executable, "-c", DECOY_CODE, ready, marker], stdin=subprocess.DEVNULL,
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, env=env)
    try:
        s_pid = await_ready(ready, s)
        if s_pid is None:
            print("SIBLING-DID-NOT-START rc=%r" % s.poll(), flush=True)
            return 3
        r = subprocess.run([sys.executable, THIS_FILE, "--child", "sweep-probe", spec_path, marker, str(s_pid)],
                           stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env,
                           timeout=CHILD_TIMEOUT_S)
        sys.stdout.write(r.stdout.decode("utf-8", "replace"))
        sys.stdout.flush()
        return r.returncode
    finally:
        reap(s)


def child_sweep_probe(spec, _spec_path, rest):
    """DC-20: from inside a chain whose members all carry the needle, ask the sweep."""
    marker, s_pid = rest[0], int(rest[1])
    import sqlite_procs as P
    me, parent = os.getpid(), os.getppid()
    en = P.enumerate_processes()
    rows = dict((pid, txt) for pid, _pp, txt in en.procs)
    if os.name == "nt":
        needle = rows.get(me, "")

        def same(txt):
            return bool(txt) and bool(needle) and os.path.normcase(os.path.normpath(txt)) == \
                os.path.normcase(os.path.normpath(needle))
    else:
        needle = marker

        def same(txt):
            return needle in txt
    raw = sorted(p for p in (me, parent) if p in rows and same(rows[p]))
    found = P.our_fixture_pids(needle)
    print(json.dumps({"me": me, "parent": parent, "raw": raw, "hits": [r[0] for r in found.procs],
                      "verified": bool(found.verified), "sibling": s_pid, "needle": needle, "why": found.why}),
          flush=True)
    return 0


CHILD_SCENARIOS = {"supply": child_supply, "currency": child_currency, "sweep-parent": child_sweep_parent,
                   "sweep-probe": child_sweep_probe}


def child_main(args):
    if len(args) < 2 or args[0] not in CHILD_SCENARIOS:
        sys.stderr.write("usage: test_driver_contracts.py --child {%s} <spec.json> ...\n"
                         % ",".join(sorted(CHILD_SCENARIOS)))
        return 2
    with open(args[1], "r", encoding="utf-8") as fh:
        spec = json.load(fh)
    if HERE not in sys.path:
        sys.path.insert(0, HERE)
    ov = spec.get("override")
    if ov:
        # A child is a throwaway process: the mutant REPLACES the module under its own name, so every
        # import of it -- the call sites' included -- binds the mutant.
        sys.modules[ov["name"]] = exec_module(ov["name"], read_text(ov["path"]), ov["path"], module_path(ov["name"]))
    return CHILD_SCENARIOS[args[0]](spec, args[1], args[2:])


# ═══════════════════════════════════════════════════════════════════════════════════════
# the pin registry
# ═══════════════════════════════════════════════════════════════════════════════════════

PinSpec = collections.namedtuple("PinSpec", ["id", "title", "fn"])
PINS = (
    PinSpec("DC-01", "the CLOSED verdict vocabulary, read by the driver's own reader", pin_dc01),
    PinSpec("DC-02", "both verdict recorders are guarded by the closed vocabulary", pin_dc02),
    PinSpec("DC-03", "ONE run decision (skip / native / launched)", pin_dc03),
    PinSpec("DC-04", "the corpus-entry gates (no control-compiler gate)", pin_dc04),
    PinSpec("DC-05", "the segment parser", pin_dc05),
    PinSpec("DC-06", "the declared capability set reaches every stage build site", pin_dc06),
    PinSpec("DC-07", "the precondition discriminator and the REAL resume loop", pin_dc07),
    PinSpec("DC-08", "the acquisition contract", pin_dc08),
    PinSpec("DC-09", "the loader variable and separator are TARGET-keyed", pin_dc09),
    PinSpec("DC-10", "the confound supply is PER LEG and gated twice", pin_dc10),
    PinSpec("DC-11", "the supply's refusal STOPS the driver (child processes)", pin_dc11),
    PinSpec("DC-12", "a failed run-directory operation is a VERDICT", pin_dc12),
    PinSpec("DC-13", "a launched leg's run environment ARRIVES", pin_dc13),
    PinSpec("DC-14", "the smoke gate spawns with the ONE shared environment builder", pin_dc14),
    PinSpec("DC-15", "the launcher-prerequisite gate, strict mode, the ledger, the smoke rc table", pin_dc15),
    PinSpec("DC-16", "the smoke argv: MEASURED targets, the CATALOGUE's launcher, no host identity", pin_dc16),
    PinSpec("DC-17", "WHY a failure was excused is PRINTED", pin_dc17),
    PinSpec("DC-18", "the compiler is PROVED current before the run is spent (child processes)", pin_dc18),
    PinSpec("DC-19", "execution evidence: monitors, attribution and the fold", pin_dc19),
    PinSpec("DC-20", "the leftover-fixture sweep (decoy, ancestors, UNVERIFIED)", pin_dc20),
    PinSpec("DC-21", "a LOCATED compiler is REFRESHED, not trusted", pin_dc21),
    PinSpec("DC-22", "what a leftover sweep LEARNT reaches the leg's verdict (the REAL run_corpus)", pin_dc22),
)
PIN_BY_ID = dict((p.id, p) for p in PINS)


# ═══════════════════════════════════════════════════════════════════════════════════════
# the red-arm registry (report 09 E.5 + the gap arms of C.2), as DATA
# ═══════════════════════════════════════════════════════════════════════════════════════

def _is_gating_refusal(node):
    return (isinstance(node, ast.If) and isinstance(node.test, ast.Compare) and isinstance(node.test.left, ast.Name)
            and node.test.left.id == "gating" and len(node.test.ops) == 1 and isinstance(node.test.ops[0], ast.NotEq)
            and isinstance(node.test.comparators[0], ast.Constant) and node.test.comparators[0].value == "probed")


def gating_refusals(tree):
    """AST witness: the `if gating != "probed": ...` statements in confound_supply's body."""
    return sum(1 for fn in ast.walk(tree) if isinstance(fn, ast.FunctionDef) and fn.name == "confound_supply"
               for st in fn.body if _is_gating_refusal(st))


def drop_gating_refusal(tree):
    for fn in ast.walk(tree):
        if isinstance(fn, ast.FunctionDef) and fn.name == "confound_supply":
            fn.body = [st for st in fn.body if not _is_gating_refusal(st)]
    return tree


Red = collections.namedtuple("Red", ["id", "pin", "module", "source", "title", "witness", "old", "new", "expect",
                                     "stay_green", "transform", "ast_witness"])


def red(id_, pin, module, source, title, witness, new=None, old=None, expect=(), stay_green=(), transform=None,
        ast_witness=None):
    return Red(id_, pin, module, source, title, witness, old, new, tuple(expect), tuple(stay_green), transform,
               ast_witness)


_LEDGER_UNIT_GUARD = (
    '        if not token:\n'
    '            why = "this driver recorded a NOT-RUN with an EMPTY verdict token"\n'
    '        elif not self.known(token):\n'
    '            why = ("this driver recorded a NOT-RUN with the token \'%s\', which is OUTSIDE the "\n'
    '                   "closed vocabulary (%s)" % (token, " ".join(self.vocabulary)))\n'
    '        else:\n'
    '            leg.unit_verdict = "not run [%s] %s %s" % (token, DASH, detail)\n'
    '            return\n')
_LEDGER_LEG_GUARD = (
    '        if not token:\n'
    '            why = "this driver recorded a verdict with an EMPTY token"\n'
    '        elif not self.known(token):\n'
    '            why = ("this driver recorded the verdict token \'%s\', which is OUTSIDE the closed "\n'
    '                   "vocabulary (%s)" % (token, " ".join(self.vocabulary)))\n'
    '        else:\n'
    '            leg.verdict, leg.verdict_detail = token, detail\n'
    '            return\n')
_CONTROL_CC_INFO = (
    '    if not leg.cc:\n'
    '        log.info("[%s] no CONTROL compiler on this host \u2014 the corpus RUNS anyway (the loadext helper "\n'
    '                 "comes from DSS); only the helper\'s cross-check against a second toolchain is lost."\n'
    '                 % leg.label)\n')
_RUN_DIR_FAIL = (
    '    return False, ("could not %s in the launcher\'s own filesystem \u2014 `%s` exited %d: %s"\n'
    '                   % (what, " ".join(argv), r.rc, " | ".join(r.out.strip().splitlines()) or\n'
    '                      "<no diagnostic>"))\n')
_STEP5_CALL = ('    proved = CMP.assert_current(C.BENCH_CORE, run.compiler, run.config_root, specs,\n'
               '                                CMP.rebuild_command(run.compiler, run.repo_root))\n')
_SMOKE_ENV = ('        env = L.leg_launch_env(run.resolver, leg, base, loader_dirs,\n'
              '                               tcl_library=leg.tcl_script_dir or None, log=log)\n')
# `_sweep`'s records of a kill that did NOT take and of a sweep that could not LOOK; deleting them
# (its `for pid in sw:` kill record kept) is the flattening that once lost both from the verdict.
_SWEEP_FACTS = ('        for pid, how in sw.failed:\n'
                '            lr.hygiene.append("%s \u2014 FAILED to kill leftover pid %s %s (%s): it may still hold this "\n'
                '                              "leg\'s files" % (why, pid, where, how))\n'
                '        blind = "leftover-fixture sweep UNVERIFIED %s" % where\n'
                '        if not sw.verified and not any(h.startswith(blind) for h in lr.hygiene):\n'
                '            lr.hygiene.append("%s (%s; first at %s) \u2014 a stray fixture would go unnoticed"\n'
                '                              % (blind, sw.why, why))\n')

REDS = (
    red("RD-01a", "DC-02", "sqlite_common", "H1", "remove unit_not_run's closed-vocabulary guard",
        'why = "this driver recorded a NOT-RUN with an EMPTY verdict token"', old=_LEDGER_UNIT_GUARD,
        new='        leg.unit_verdict = "not run [%s] %s %s" % (token, DASH, detail)\n        return\n',
        expect=("U02",)),
    red("RD-01b", "DC-02", "sqlite_common", "F1", "remove set_leg's closed-vocabulary guard",
        'why = "this driver recorded a verdict with an EMPTY token"', old=_LEDGER_LEG_GUARD,
        new='        leg.verdict, leg.verdict_detail = token, detail\n        return\n', expect=("A03",)),
    red("RD-02", "DC-04", "sqlite_units", "H2", "restore the control-compiler gate",
        "no CONTROL compiler on this host", old=_CONTROL_CC_INFO, new="    if not leg.cc:\n        return False\n",
        expect=("E01",)),
    red("RD-03", "DC-05", "sqlite_corpus", "H3/F2", "remove the first-diagnostic capture",
        "diag = d[:DIAG_LIMIT] + TRUNCATION_SUFFIX if len(d) > DIAG_LIMIT else d",
        old="                diag = d[:DIAG_LIMIT] + TRUNCATION_SUFFIX if len(d) > DIAG_LIMIT else d\n",
        new='                diag = ""\n', expect=("D01",)),
    red("RD-04", "DC-05", "sqlite_corpus", "H9", "count the harness's teardown results as coverage",
        "first[0].endswith(TEARDOWN_TAILS)",
        old="                if not (first and first[0].endswith(TEARDOWN_TAILS)):\n                    pend += 1\n",
        new="                pend += 1\n", expect=("D14",)),
    red("RD-05", "DC-07", "sqlite_corpus", "H4/F17", "weaken the discriminator to 'zero files'",
        'sig == (prev_zero_sig or "")',
        old='    return facts.n_files == 0 and sig != "" and sig == (prev_zero_sig or "")\n',
        new="    return facts.n_files == 0\n", expect=("E02", "L07")),
    red("RD-06", "DC-07", "sqlite_corpus", "H4b/F18", "make a SILENT crash unsignable again",
        "return SILENT_SENTINEL", old="        return SILENT_SENTINEL\n", new='        return ""\n',
        expect=("E07", "L01"), stay_green=("E01", "E03", "L06")),
    red("RD-07", "DC-09", "sqlite_launch", "H5/F3", "the darwin loader row names LD_LIBRARY_PATH",
        'return ("DYLD_LIBRARY_PATH", ":")', new='return ("LD_LIBRARY_PATH", ":")',
        expect=("V-darwin-macho64-arm64",)),
    red("RD-08", "DC-13", "sqlite_launch", "F7", "join the loader search path with the HOST separator",
        "(name, sep.join(parts))", new="(name, os.pathsep.join(parts))", expect=("N01", "T01")),
    red("RD-09", "DC-13", "sqlite_launch", "F9", "leave the loader search path in the HOST namespace",
        "t = launch_path(resolver, verb, d)", new="t = d", expect=("T01",)),
    red("RD-10", "DC-13", "sqlite_launch", "F10", "keep the loader variable out of the carrier",
        "names = list(declared) + ([loader_name] if loader_name else [])", new="names = list(declared)",
        expect=("B04",)),
    red("RD-11", "DC-13", "sqlite_launch", "H8", "forward a DRIVER PATH by name, untranslated",
        'call.append("--forward-path=%s=%s" % (n, v))', new='call += ["--forward", n]', expect=("K01",)),
    red("RD-12a", "DC-10", "sqlite_verdicts", "H6", "restore ONE global confound list for every leg",
        'return [str(p) for p in d.get("confoundsByName") or []]',
        new='return ["^walsetlk-", "^busy2-", "^zipfile-25.0$"]', expect=("I04",)),
    red("RD-12b", "DC-10", "sqlite_verdicts", "F4", "key the supply on the leg LABEL",
        'return [str(p) for p in d.get("confoundsByName") or []]',
        new='return ["^walsetlk-", "^busy2-"] if leg.label == "pe64-x86_64" else []', expect=("I02",)),
    red("RD-13", "DC-10", "sqlite_verdicts", "H17", "AST: delete the confoundGating refusal statement",
        "AST: the `if gating != 'probed'` statement of confound_supply", transform=drop_gating_refusal,
        ast_witness=gating_refusals, expect=("G01",)),
    red("RD-14", "DC-11", "sqlite_units", "H19 (replaced)", "swallow the supply's refusal at the call site",
        "\n    patterns = V.confound_supply(leg, override)\n",
        new="\n    try:\n        patterns = V.confound_supply(leg, override)\n    except C.HarnessDie:\n"
            "        patterns = []\n", expect=("C03",)),
    red("RD-15", "DC-11", "sqlite_verdicts", "F19", "downgrade the gating refusal to a note",
        "C.die(\"[%s] the resolved leg plan says confoundGating='%s', not 'probed'.",
        new="C.LOG.info(\"[%s] the resolved leg plan says confoundGating='%s', not 'probed'.", expect=("C03",)),
    red("RD-16", "DC-12", "sqlite_launch", "H7/F5", "make a failed run-dir operation report success",
        "return False, (\"could not %s in the launcher's own filesystem", old=_RUN_DIR_FAIL,
        new='    return True, ""\n', expect=("J03",)),
    red("RD-17", "DC-14", "sqlite_smoke", "F8", "spawn the smoke gate with none of the leg's environment",
        "env = L.leg_launch_env(run.resolver, leg, base, loader_dirs,", old=_SMOKE_ENV,
        new="        env = dict(base)\n", expect=("W03",)),
    red("RD-18a", "DC-15", "build_and_test", "H11", "answer an UNMET launcher prerequisite as met",
        "if r.rc == 0:", new="if r.rc in (0, 3):", expect=("K03",)),
    red("RD-18b", "DC-15", "build_and_test", "F11", "remove the unmet arm (unmet becomes poisoned)",
        "if r.rc == 3 and isinstance(report, dict):", new="if False:", expect=("K03",)),
    red("RD-19", "DC-15", "sqlite_report", "H12/F12", "declassify the launcher skip as environmental",
        "classes.get(lg.verdict) == ENVIRONMENTAL]",
        new='classes.get(lg.verdict) == ENVIRONMENTAL and lg.verdict != "skipped-launcher-prerequisite-missing"]',
        expect=("S02",)),
    red("RD-20", "DC-15", "sqlite_smoke", "H13/F13", "take the smoke gate's CHARGED-TO-DSS arm away",
        '    1: ("FAIL \u2014 CHARGED TO DSS', new='    11: ("FAIL \u2014 CHARGED TO DSS', expect=("Q1",)),
    red("RD-21", "DC-15", "sqlite_verdicts", "H14/F14", "drop the token from the ledger's vocabulary",
        "count = collections.OrderedDict((v, 0) for v in classes)",
        new='count = collections.OrderedDict((v, 0) for v in classes if v != "skipped-launcher-prerequisite-missing")',
        expect=("G01",)),
    red("RD-22", "DC-16", "sqlite_smoke", "H15/F15", "pass the declared spec as the measured target",
        '"--cli-target", cli_target,', new='"--cli-target", leg.spec,', expect=("A03",)),
    red("RD-23a", "DC-16", "sqlite_smoke", "H16", "resolve NO launcher for the reference",
        "rc, argv, why = launcher_for_target(run.resolver, triple)", new='rc, argv, why = 0, [], ""',
        expect=("F03",)),
    red("RD-23b", "DC-16", "sqlite_smoke", "F16", "pick the reference launcher from the HOST identity",
        "rc, argv, why = launcher_for_target(run.resolver, triple)",
        new='rc, argv, why = 0, (["wsl.exe", "-e"] if run.posix.needs_wsl else []), ""', expect=("F03", "H02")),
    red("RD-24", "DC-17", "sqlite_verdicts", "H18", "remove the empty-report refusal",
        'if not (report_text or "").strip():', new="if False:", expect=("R03",)),
    red("RD-25", "DC-18", "build_and_test", "H20/F20", "stop calling the currency check at Step 5",
        "proved = CMP.assert_current(C.BENCH_CORE, run.compiler, run.config_root, specs,", old=_STEP5_CALL,
        new='    proved = "(not checked)"\n', expect=("O01",)),
    red("RD-26", "DC-18", "build_and_test", "H21/F21", "let SKIP_DSS_BUILD=1 bypass the currency check",
        "proved = CMP.assert_current(C.BENCH_CORE, run.compiler, run.config_root, specs,", old=_STEP5_CALL,
        new='    proved = ("(not checked under SKIP_DSS_BUILD=1)" if C.env("SKIP_DSS_BUILD") == "1" else '
            'CMP.assert_current(C.BENCH_CORE, run.compiler, run.config_root, specs, '
            'CMP.rebuild_command(run.compiler, run.repo_root)))\n', expect=("O10",)),
    red("RD-27", "DC-18", "sqlite_compiler", "H22/F22", "report an unrunnable check as a stale binary",
        "if r.rc != 1:", new="if False:", expect=("O17", "O20")),
    red("RD-28", "DC-19", "sqlite_units", "HP", "drop the fold of the resolver's EXCUSED names",
        "        real = keep\n", new="        pass\n", expect=("F01",)),
    red("RD-29", "DC-19", "sqlite_launch", "FP (replaced)", "remove the no-armed-row early return",
        '    if override_active or not [p for p in (leg.d.get("confoundsByEvidence") or []) if p]:\n        return []\n',
        new="", expect=("M03", "M05")),
    red("RD-30", "DC-20", "sqlite_procs", "F6", "blind the sweep's matcher",
        "row[0] not in skip and match(row[2])", new="row[0] not in skip and False", expect=("D02", "A02")),
    red("RD-31", "DC-21", "sqlite_compiler", "F23", "let a failed rebuild fall back to the located binary",
        "    if rc != 0:\n", new="    if False:\n", expect=("P3",)),
    red("RD-32", "DC-06", "sqlite_stage", "H10", "build the reference testfixture without the declared OPTIONS",
        '"testfixture", "USE_AMALGAMATION=0", "OPTIONS=" + mo', new='"testfixture", "USE_AMALGAMATION=0"',
        expect=("S05", "C03")),
    red("RD-32b", "DC-06", "sqlite_stage", "new (E.5)", "configure without the declared capability flags",
        "configure_args += cfg.configure_flags", new="configure_args += []", expect=("S03",)),
    red("RD-32c", "DC-06", "sqlite_stage", "new", "derive the testfixture recipe without OPTIONS",
        'make_vars=("USE_AMALGAMATION=0", "OPTIONS=" + mo)', new='make_vars=("USE_AMALGAMATION=0",)',
        expect=("S06", "C06")),
    red("RD-33", "DC-10", "sqlite_verdicts", "gap (C.2)", "remove the runDirectoryGating refusal",
        'if rdg not in ("not-required", "measured"):', new="if False:", expect=("G05",)),
    red("RD-34", "DC-10", "sqlite_verdicts", "gap (C.2)", "remove the undeclared-leg refusal",
        "if missing:", new="if False:", expect=("U01",)),
    red("RD-35", "DC-10", "sqlite_verdicts", "gap (C.2)", "ignore the operator override",
        "if override is not None:", new="if False:", expect=("O01",)),
    red("RD-36", "DC-03", "sqlite_common", "gap (C.2)", "remove the empty-launcher refusal",
        "if not leg.launcher:", new="if False:", expect=("R04",)),
    red("RD-37", "DC-09", "sqlite_launch", "gap (C.2)", "remove the loader's target-OS cross-check",
        "if os_ != fmt_os:", new="if False:", expect=("X01",)),
    red("RD-38a", "DC-08", "sqlite_libs", "gap (C.2)", "read the REQUIRED cacheDir as optional",
        'cache_dir = report["cacheDir"]', new='cache_dir = report.get("cacheDir") or "<no cacheDir in the report>"',
        expect=("A08",)),
    red("RD-38b", "DC-08", "sqlite_libs", "gap (C.2)", "accept an EMPTY required value",
        'raise KeyError("cacheDir/libraries empty")',
        old='        if not cache_dir or not libraries:\n            raise KeyError("cacheDir/libraries empty")\n',
        new="", expect=("A09",)),
    red("RD-39", "DC-01", "build_and_test", "gap (C.2)", "keep the CR on every vocabulary token",
        'return [ln.strip() for ln in r.out.replace("\\r", "\\n").split("\\n") if ln.strip()]',
        new='return [ln for ln in r.out.split("\\n") if ln.strip()]', expect=("V03",)),
    red("RD-40", "DC-15", "sqlite_smoke", "gap (C.2)", "take the smoke gate's NOT-DSS arm away",
        '    3: ("FAIL \u2014 NOT DSS', new='    13: ("FAIL \u2014 NOT DSS', expect=("Q3",)),
    red("RD-41", "DC-22", "sqlite_units", "new (sweep facts)", "flatten each Sweep to its kills again",
        "for pid, how in sw.failed:", old=_SWEEP_FACTS, new="", expect=("H02", "H03", "H04", "H05"),
        stay_green=("H01", "H06", "H07", "H08")),
    red("RD-42", "DC-22", "sqlite_units", "new (sweep facts)", "record a blind sweep at EVERY sweep, not once",
        "not any(h.startswith(blind) for h in lr.hygiene)",
        old="        if not sw.verified and not any(h.startswith(blind) for h in lr.hygiene):\n",
        new="        if not sw.verified:\n", expect=("H04", "H05"),
        stay_green=("H01", "H02", "H03", "H06", "H07", "H08")),
)


# ═══════════════════════════════════════════════════════════════════════════════════════
# the mutator's own self-test
# ═══════════════════════════════════════════════════════════════════════════════════════

def _refused_with(fn, needle):
    try:
        got = fn()
    except MutationRefused as exc:
        return needle in str(exc), "refused: %s" % exc
    return False, "ACCEPTED (it should have been refused): %s" % (got.path,)


def ms_nonunique(suite, work):
    return _refused_with(lambda: make_mutant("sqlite_verdicts", "ms1", work, "\n    return ", old="    if missing:\n",
                                             new="    if False:\n"), "occurs")


def ms_missing(suite, work):
    return _refused_with(lambda: make_mutant("sqlite_verdicts", "ms2", work, "a witness that is nowhere in the driver",
                                             old="    if missing:\n", new="    if False:\n"), "occurs 0 time")


def ms_noop(suite, work):
    return _refused_with(lambda: make_mutant("sqlite_verdicts", "ms3", work, "if missing:", old="    if missing:\n",
                                             new="    if missing:\n"), "DID NOT LAND")


def ms_survives(suite, work):
    return _refused_with(lambda: make_mutant("sqlite_verdicts", "ms4", work, "if missing:",
                                             old="def leg_mode(leg):\n",
                                             new="def leg_mode(leg):  # touched by the mutator self-test\n"),
                         "SURVIVES")


def ms_syntax(suite, work):
    return _refused_with(lambda: make_mutant("sqlite_verdicts", "ms5", work, "def leg_mode(leg):",
                                             old="def leg_mode(leg):\n", new="def leg_mode(leg)\n"), "PARSE")


def ms_import(suite, work):
    return _refused_with(lambda: make_mutant("sqlite_verdicts", "ms6", work, "import sqlite_common as C",
                                             old="import sqlite_common as C\n",
                                             new="import sqlite_common_absent_for_the_mutator_self_test as C\n"),
                         "IMPORT")


def ms_ast_noop(suite, work):
    return _refused_with(lambda: make_mutant("sqlite_verdicts", "ms7", work, "AST no-op", transform=lambda tr: tr,
                                             ast_witness=gating_refusals), "DID NOT LAND")


def ms_positive(suite, work):
    m = make_mutant("sqlite_verdicts", "ms8", work, 'if not (report_text or "").strip():', new="if False:")
    real = importlib.import_module("sqlite_verdicts")
    r_real, _a = refused(real.print_confound_report, "x", "   ", suite.C.Log(io.StringIO()))
    r_mut, _b = refused(m.mod.print_confound_report, "x", "   ", suite.C.Log(io.StringIO()))
    ok = (m.mod is not real and m.mod.__name__ not in sys.modules and r_real and not r_mut
          and not os.path.realpath(m.path).startswith(os.path.realpath(HERE) + os.sep))
    return ok, "mutant %s at %s: real refuses=%s, mutant refuses=%s" % (m.mod.__name__, m.path, r_real, r_mut)


def ms_red_verdict(suite, work):
    """The red verdict itself, over synthetic pin results: only 'went red on a NAMED check, and the
    stay-green checks stayed green' passes; a vacuous run, a red elsewhere, a red too wide and a crash
    each FAIL; a pin skip is a SKIP."""
    arm = red("RD-T", "DC-01", "sqlite_common", "self-test", "a synthetic arm", "w", new="v", expect=("A",),
              stay_green=("B",))

    def ran(*failing):
        t = Checks("DC-01", None, Tally())
        for k in ("A", "B", "C"):
            t.ck(k, k, k not in failing)
        return t
    got = [judge_red(arm, ran())[0], judge_red(arm, ran("C"))[0], judge_red(arm, ran("A", "B"))[0],
           judge_red(arm, ran("A", "C"))[0], judge_red(arm, ran("A"), crash="Traceback ...")[0],
           judge_red(arm, ran(), skip_why="no such host facility")[0]]
    want = ["fail", "fail", "fail", "pass", "fail", "skip"]
    return got == want, "verdicts for (vacuous, wrong check, too wide, named, crash, skip): %s" % got


def ms_ast_positive(suite, work):
    m = make_mutant("sqlite_verdicts", "ms9", work, "AST gating refusal", transform=drop_gating_refusal,
                    ast_witness=gating_refusals)
    C = suite.C
    leg = C.Leg(leg_d("x", confoundGating="unprobed", confoundsByName=["^a"]))
    r, got = refused(m.mod.confound_supply, leg, None)
    ok = gating_refusals(ast.parse(m.source)) == 0 and not r and got == ["^a"]
    return ok, "AST mutant accepted; unprobed supply refused=%s answer=%r" % (r, got)


MUTATOR_ARMS = (
    ("MS-1", "a witness that occurs MORE than once is refused", ms_nonunique),
    ("MS-2", "a witness that is not in the module is refused", ms_missing),
    ("MS-3", "a no-op transform is refused (the mutant must differ)", ms_noop),
    ("MS-4", "a transform that leaves the witness in place is refused", ms_survives),
    ("MS-5", "a syntax-breaking transform is refused", ms_syntax),
    ("MS-6", "a mutant that cannot IMPORT is refused", ms_import),
    ("MS-7", "an AST transform that changes nothing is refused (formatting is not a mutation)", ms_ast_noop),
    ("MS-8", "a real text mutation is ACCEPTED: unique module name, out of sys.modules, written outside the "
             "driver's directory, and its behaviour changed", ms_positive),
    ("MS-9", "a real AST mutation is ACCEPTED and removes exactly its witness", ms_ast_positive),
    ("MS-10", "the red verdict is fail-closed: vacuous, red on another check, red too wide and crashed "
              "each FAIL; only a pin skip skips", ms_red_verdict),
)

# Every arm this file registers: 22 pins + 49 red arms + 10 mutator arms. A registry that no longer
# adds up to this -- an arm deleted, or one added without this line -- is a FAILURE.
DECLARED_TOTAL = 81


# ═══════════════════════════════════════════════════════════════════════════════════════
# the runner
# ═══════════════════════════════════════════════════════════════════════════════════════

def _fail(out, tally, headline, detail=""):
    tally.failed += 1
    out.line("  FAIL " + headline)
    for dl in str(detail).splitlines()[-60:]:
        out.line("         " + dl)


def _tail(buf, n=25):
    lines = buf.getvalue().splitlines()[-n:]
    return ("\n  driver output (last %d line(s)):\n" % len(lines) + "\n".join("    " + ln for ln in lines)) if lines else ""


def run_green(pin, suite, out, tally):
    out.line("-- %s %s" % (pin.id, pin.title))
    t = Checks(pin.id, out, tally)
    tmp = mkdtemp(pin.id)
    buf = io.StringIO()
    try:
        try:
            with contextlib.redirect_stdout(buf), contextlib.redirect_stderr(buf):
                pin.fn(t, Ctx(suite, Mods(None), tmp))
        except PinSkip as exc:
            t.skip("REST", "the rest of the pin" if t.results else "the whole pin", str(exc))
        except Exception:  # noqa: BLE001 -- a crash is a harness FAILURE, reported with its traceback
            _fail(out, tally, "%s PIN CRASHED against the SHIPPED driver" % pin.id, traceback.format_exc() + _tail(buf))
            return
        if not t.results and not t.skips:
            _fail(out, tally, "%s asserted NOTHING -- a pin that checks nothing proves nothing" % pin.id)
        elif t.failed_keys:
            out.line("         (%s: %d check(s) failed against the SHIPPED driver)%s"
                     % (pin.id, len(t.failed_keys), _tail(buf, 15)))
    finally:
        rmtree_hard(tmp)


def judge_red(r, t, crash=None, skip_why=None):
    """-> (kind, headline, detail) for ONE red arm, fail-closed: 'skip' only when its pin skipped;
    otherwise 'fail' unless the pin went red ON A CHECK THE ARM NAMES and every stay-green check
    stayed green. A crash is a FAILURE (a harness defect), never a red."""
    label = "%s [%s, %s] %s" % (r.id, r.module, r.source, r.title)
    if skip_why is not None:
        return "skip", "%s -- its pin %s skips on this host: %s" % (label, r.pin, skip_why), ""
    if crash is not None:
        return "fail", "%s -- PIN %s CRASHED against the mutant (a harness failure, never a red)" % (label, r.pin), crash
    failed = t.failed_keys
    hit = [k for k in r.expect if k in failed]
    broken = [k for k in r.stay_green if k not in t.results or not t.results[k][1]]
    if not failed:
        return "fail", ("%s -- VACUOUS: pin %s stayed GREEN against a module whose guard was REMOVED"
                        % (label, r.pin)), "either the pin does not exercise the guard, or the guard is not what makes it pass"
    if not hit:
        return "fail", ("%s -- red, but NOT for the reason it names: none of %s went red"
                        % (label, ", ".join("%s/%s" % (r.pin, k) for k in r.expect))), "went red: %s" % ", ".join(failed)
    if broken:
        return "fail", ("%s -- red too WIDELY: %s must stay green (only the named reason may go red)"
                        % (label, ", ".join("%s/%s" % (r.pin, k) for k in broken))), "went red: %s" % ", ".join(failed)
    return "pass", ("%s -- red-on-disable CONFIRMED on %s (%d check(s) went red: %s)"
                    % (label, ", ".join("%s/%s" % (r.pin, k) for k in hit), len(failed), ", ".join(failed))), ""


def run_red(r, suite, out, tally):
    work = mkdtemp("mut" + r.id)
    tmp = None
    try:
        try:
            m = make_mutant(r.module, r.id, work, r.witness, old=r.old, new=r.new, transform=r.transform,
                            ast_witness=r.ast_witness)
        except MutationRefused as exc:
            _fail(out, tally, "%s [%s, %s] %s -- the MUTATION was REFUSED, so nothing was demonstrated"
                  % (r.id, r.module, r.source, r.title), str(exc))
            return
        t = Checks(r.pin, None, tally)
        tmp = mkdtemp(r.id)
        buf = io.StringIO()
        crash = skip_why = None
        try:
            with contextlib.redirect_stdout(buf), contextlib.redirect_stderr(buf):
                PIN_BY_ID[r.pin].fn(t, Ctx(suite, Mods(m), tmp))
        except PinSkip as exc:
            skip_why = str(exc)
        except Exception:  # noqa: BLE001 -- a crash against a mutant is a HARNESS failure, never a red
            crash = traceback.format_exc() + _tail(buf)
        kind, headline, detail = judge_red(r, t, crash, skip_why)
        if kind == "pass":
            tally.passed += 1
            out.line("  ok   " + headline)
        elif kind == "skip":
            tally.skipped += 1
            out.line("  skip " + headline)
        else:
            _fail(out, tally, headline, detail)
    finally:
        rmtree_hard(work)
        if tmp:
            rmtree_hard(tmp)


def run_mutator_arm(arm, suite, out, tally):
    key, title, fn = arm
    work = mkdtemp(key)
    try:
        try:
            ok, detail = fn(suite, work)
        except Exception:  # noqa: BLE001
            ok, detail = False, "CRASHED:\n" + traceback.format_exc()
        if ok:
            tally.passed += 1
            out.line("  ok   %s %s" % (key, title))
        else:
            _fail(out, tally, "%s %s" % (key, title), detail)
    finally:
        rmtree_hard(work)


def run_suite(out):
    tally = Tally()
    t0 = time.monotonic()
    try:
        suite = Suite()
    except Exception:  # noqa: BLE001 -- nothing can be pinned without the driver's own modules
        _fail(out, tally, "the driver's modules could not be loaded from %s" % HERE, traceback.format_exc())
        out.line("passed=%d failed=%d skipped=%d" % (tally.passed, tally.failed, tally.skipped))
        return 1
    out.line("test_driver_contracts.py -- pinning the ONE Python driver in %s (host %s/%s, Python %s)"
             % (HERE, suite.host, suite.arch, sys.version.split()[0]))
    spent = []

    def timed(name, fn, *a):
        s = time.monotonic()
        fn(*a)
        spent.append((time.monotonic() - s, name))
    for pin in PINS:
        timed(pin.id, run_green, pin, suite, out, tally)
    out.line("-- RD  red-on-disable: each mutation proven to LAND, then its pin must go red on the check it names")
    for r in REDS:
        timed(r.id, run_red, r, suite, out, tally)
    out.line("-- MS  the mutator's own self-test")
    for arm in MUTATOR_ARMS:
        timed(arm[0], run_mutator_arm, arm, suite, out, tally)
    registered = len(PINS) + len(REDS) + len(MUTATOR_ARMS)
    ids = [p.id for p in PINS] + [r.id for r in REDS] + [a[0] for a in MUTATOR_ARMS]
    unknown_pins = sorted(set(r.pin for r in REDS) - set(PIN_BY_ID))
    if registered == DECLARED_TOTAL and len(set(ids)) == len(ids) and not unknown_pins:
        tally.passed += 1
        out.line("  ok   REGISTRY %d arm(s) registered and run, as declared (%d pins, %d red arms, %d mutator arms)"
                 % (registered, len(PINS), len(REDS), len(MUTATOR_ARMS)))
    else:
        _fail(out, tally, "REGISTRY %d arm(s) registered, %d declared; duplicate ids: %s; red arms naming no pin: %s"
              % (registered, DECLARED_TOTAL, sorted(set(i for i in ids if ids.count(i) > 1)), unknown_pins))
    out.line("elapsed %.1fs; slowest: %s" % (time.monotonic() - t0, ", ".join(
        "%s %.1fs" % (name, dt) for dt, name in sorted(spent, reverse=True)[:6])))
    out.line("passed=%d failed=%d skipped=%d" % (tally.passed, tally.failed, tally.skipped))
    return 0 if tally.failed == 0 else 1


def main(argv=None):
    args = list(sys.argv[1:] if argv is None else argv)
    if args[:1] == ["--child"]:
        return child_main(args[1:])
    if args in (["-h"], ["--help"]):
        print(__doc__)
        return 0
    if args:
        sys.stderr.write("test_driver_contracts.py takes no arguments (got: %s)\n" % " ".join(args))
        return 2
    return run_suite(Out(sys.stdout))


if __name__ == "__main__":
    sys.exit(main())
