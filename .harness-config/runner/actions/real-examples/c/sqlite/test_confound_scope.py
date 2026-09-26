#!/usr/bin/env python3
"""test_confound_scope.py -- the Step-0 BEHAVIOURAL self-test of the SQLite corpus harness's
late-stage decisions, run against the ONE Python driver (`build_and_test.py` + its flat
`sqlite_*.py` modules) by IMPORTING its functions by name.

It replaces the twins `test-confound-scope.sh` + `test-confound-scope.ps1` (lane mig, part 4,
2026-09-21), which EXTRACTED each old driver's shell text between `dss:` sentinels and executed
it. There is one driver now, so a renamed or missing function is a named FAIL here -- stronger
than the substring the twins matched -- and a capability "in one driver and not the other" has
nothing left to check (the 102 retired arms are declared as data below, with what covers them).

WHAT IS TESTED (158 arms; the old -> new map is the registry itself: every arm names the old
`S*` (.sh) / `P*` (.ps1) arms it carries, and the run checks that the carried + retired sets are
exactly the twins' 322 arms):
  CL  the confound CLASSIFIER -- scope (`native:`/`emulated:`), permutation prefixes, the case
      policy, the scoped-excusal warning, and the SHIPPED elf64-arm64 writecrash row, planned;
  PV  checkout PROVENANCE against REAL git repositories (UNKNOWN / DETACHED-HEAD / CR-only);
  GT  the Step-2 SOURCE GATE (absent / populated non-checkout / clone / a `.git` FILE);
  LX  the LOADEXT helper's rc contract, the verdict it records, the leg loop, Step 9's red;
  CF  the per-target sqlite_cfg.h (the REAL stage-zinc.py over a synthesised header);
  LA  the launcher argv `=` form (the REAL cli-smoke.py parser);
  SM  the CLI smoke gate's MEASURED targets, reference launcher and rc table;
  LP  the launcher-prerequisite gate;  RF  the run-fidelity selector;
  BA  build attribution + oracle status;  RD  run-directory corroboration;
  EV  execution-evidence monitors (a REAL monitor process);  SU  the confound SUPPLY.

SEAMS: a RECORDING fake resolver (anything with `.call(args) -> sqlite_common.Result`), the real
`sqlite_common.Resolver` where the TRANSPORT or the SHIPPED catalogue is the subject, real git in
throwaway repositories, `sqlite_common.Log(stream=StringIO)` for the log, `HarnessDie` for a
refusal. Nothing reaches the network; every file is written under one `tempfile.mkdtemp` root,
removed at the end (read-only git objects included). Git runs HERMETIC: no system or global
config, `core.autocrlf=false`, a ceiling at the temp root, no GIT_DIR-style variables.

Usage: python test_confound_scope.py        (no arguments; -h prints this text)
Output: a line per arm -- `ok`, `FAIL` (with the reason) or `SKIP` (a gate this host lacks,
named and counted) -- ASCII only, flushed per line, then EXACTLY `passed=N failed=N skipped=N`
as the LAST line. Exit 0 only when failed=0. `build_and_test.py` Step 0 and the ctest entry
`harness/sqlite_driver_selftest` read that last line.
"""
from __future__ import annotations

import ast
import collections
import contextlib
import inspect
import io
import json
import os
import re
import shlex
import shutil
import stat
import subprocess
import sys
import tempfile
import textwrap
import time
import traceback
import types

# The action is `requireInputsUnmoved`: no `__pycache__` may appear beside the driver.
sys.dont_write_bytecode = True

HERE = os.path.dirname(os.path.realpath(__file__))

# The em dash the driver's verdict lines carry (spelled by code point: this file stays ASCII).
EM = chr(0x2014)

# -- the registry --------------------------------------------------------------------------

TOTAL = 158
FAMILIES = (("CL", 28), ("PV", 31), ("GT", 10), ("LX", 32), ("CF", 6), ("LA", 5), ("SM", 13),
            ("LP", 3), ("RF", 2), ("BA", 3), ("RD", 1), ("EV", 5), ("SU", 19))
SECTIONS = {
    "CL": "CL: the confound CLASSIFIER (sqlite_verdicts.classify / leg_mode / warn_scoped)",
    "PV": "PV: checkout PROVENANCE (build_and_test.provenance / source_gate), real git",
    "GT": "GT: the Step-2 SOURCE GATE (build_and_test.source_gate), real git",
    "LX": "LX: the LOADEXT helper (sqlite_units.stage_loadext / loadext_verdict / carry_helper)",
    "CF": "CF: the per-target sqlite_cfg.h (sqlite_build.stage_headers / write_include_lists)",
    "LA": "LA: the launcher argv FORM (sqlite_smoke.smoke_argv + the real cli-smoke.py)",
    "SM": "SM: the CLI smoke gate's MEASURED targets and rc table (sqlite_smoke, sqlite_report)",
    "LP": "LP: the launcher-prerequisite gate (build_and_test.launcher_prereq_gate)",
    "RF": "RF: the run-fidelity selector (build_and_test.select_legs)",
    "BA": "BA: build attribution + oracle status (sqlite_build, sqlite_report)",
    "RD": "RD: run-directory corroboration (sqlite_units.corroborate)",
    "EV": "EV: execution evidence (sqlite_launch.evidence_*, sqlite_units.run_one_segment)",
    "SU": "SU: the confound SUPPLY (sqlite_verdicts.confound_supply)",
}

Arm = collections.namedtuple("Arm", ["id", "label", "old", "gates", "fn"])
ARMS = []


def arm(arm_id, label, old="", gates=""):
    """Register one arm: its id, what it asserts, the OLD twin arms it carries (space-separated;
    empty = a NEW arm neither twin had) and the host gates it needs."""
    def register(fn):
        ARMS.append(Arm(arm_id, label, tuple(old.split()), tuple(gates.split()), fn))
        return fn
    return register


# A gate is something a host may genuinely lack. Every arm carrying it is SKIPPED, by name,
# when it is absent -- the skip count is the number of registered arms, never a typed figure.
GATES = {
    "git": (lambda: shutil.which("git") is not None,
            "no `git` on PATH: these fixtures are REAL repositories (a stub would test the stub)"),
}

# The twins' own arms (report 10 section B): per block, how many. .sh = 182, .ps1 = 140.
OLD_SH = (("S1", 20), ("S2", 14), ("S3", 12), ("S4", 28), ("S5", 6), ("S6", 4), ("S7", 19),
          ("S8", 65), ("S9", 14))
OLD_PS = (("P1", 11), ("P2", 18), ("P3", 9), ("P4", 5), ("P5", 8), ("P6", 6), ("P7", 72),
          ("P8", 11))
OLD_TOTALS = (182, 140)

# The 102 old arms that RETIRE (report 10 section D), and what now proves each property.
RETIRED = (
    ("parity: the .sh reading the .ps1's text -- one driver now; each capability has its "
     "behavioural arm (LP, SM, RD, BA, EV, RF, LX, CF, LA)",
     "S8.01 S8.02 S8.03 S8.04 S8.05 S8.06 S8.07 S8.08 S8.09 S8.10 S8.11 S8.12 S8.13 S8.14 "
     "S8.15 S8.16 S8.17 S8.18 S8.19 S8.20 S8.21 S8.22 S8.23 S8.24 S8.25 S8.26 S8.27 S8.28 "
     "S8.29 S8.30 S8.31 S8.33 S8.35 S8.37 S8.39 S8.41 S8.43 S8.45 S8.47 S8.49 S8.51 S8.53 "
     "S8.56 S8.57 S8.58 S8.59 S8.61 S8.63 S8.65"),
    ("parity: the .ps1 reading the .sh's text -- same reason",
     "P5.06 P5.07 P5.08 P6.04 P7.02 P7.04 P7.06 P7.08 P7.11 P7.12 P7.14 P7.16 P7.19 P7.20 "
     "P7.22 P7.24 P7.26 P7.28 P7.30 P7.32 P7.34 P7.36 P7.38 P7.40 P7.42 P7.44 P7.46 P7.48 "
     "P7.50 P7.52 P7.54 P7.56 P7.58 P7.60 P7.63 P7.64 P7.68 P7.70 P7.72"),
    ("own-driver `dss:` region markers -- extraction boundaries; the functions are IMPORTED "
     "by name now, and a missing one fails loudly",
     "S7.01 S7.02 S7.03 S7.05 S7.09 S7.14 P7.01 P7.03 P7.05 P7.07 P7.21 P7.33"),
    ("shell-dialect mechanisms -- S4.27 (errexit-safe rc capture) is LX26-LX30's behaviour; "
     "P7.65 (the awk-range control) is SM09's own negative",
     "S4.27 P7.65"),
)


class ArmFailure(Exception):
    """An assertion of an arm did not hold."""


class FixtureError(Exception):
    """A shared fixture could not be built; every arm resting on it FAILS with the reason."""


# -- output --------------------------------------------------------------------------------

def _ascii(text):
    return str(text).encode("ascii", "backslashreplace").decode("ascii")


def out(line=""):
    sys.stdout.write(_ascii(line) + "\n")
    sys.stdout.flush()


def _short(value, limit=700):
    s = value if isinstance(value, str) else repr(value)
    return s if len(s) <= limit else s[:limit] + " ...[+%d chars]" % (len(s) - limit)


def exc_text(exc):
    frames = traceback.extract_tb(exc.__traceback__)
    where = ""
    if frames:
        f = frames[-1]
        where = " (raised in %s, %s(), line %d)" % (os.path.basename(f.filename), f.name, f.lineno)
    return "%s: %s%s" % (type(exc).__name__, exc, where)


# -- assertions ----------------------------------------------------------------------------

def expect(cond, what):
    if not cond:
        raise ArmFailure(what)


def eq(got, want, what):
    if got != want:
        raise ArmFailure("%s\nwant: %s\ngot : %s" % (what, _short(want), _short(got)))


def has(hay, needle, what):
    if needle not in hay:
        raise ArmFailure("%s\nwant (contained): %s\nin: %s" % (what, _short(needle), _short(hay)))


def lacks(hay, needle, what):
    if needle in hay:
        raise ArmFailure("%s\nmust NOT contain: %s\nin: %s" % (what, _short(needle), _short(hay)))


def refused(fn, *args, **kwargs):
    """The HarnessDie message of `fn(...)`; FAIL when it returns normally."""
    try:
        fn(*args, **kwargs)
    except X.C.HarnessDie as exc:
        return str(exc)
    raise ArmFailure("expected a HarnessDie refusal from %s(); it returned normally"
                     % getattr(fn, "__name__", fn))


def after(argv, opt):
    """The element right after `opt`, which must occur EXACTLY once in `argv`."""
    n = argv.count(opt)
    if n != 1:
        raise ArmFailure("%r occurs %d time(s) in the argv, expected exactly once:\n%s"
                         % (opt, n, _short(argv)))
    i = argv.index(opt)
    if i + 1 >= len(argv):
        raise ArmFailure("%r is the LAST argv element, with no value:\n%s" % (opt, _short(argv)))
    return argv[i + 1]


def adjacent(argv, opt, value, what):
    eq(after(argv, opt), value, "%s (the element after %s)" % (what, opt))


def all_after(argv, opt):
    return [argv[i + 1] for i, a in enumerate(argv[:-1]) if a == opt]


def samepath(a, b):
    return os.path.normcase(os.path.normpath(str(a))) == os.path.normcase(os.path.normpath(str(b)))


# -- the suite's shared state --------------------------------------------------------------

X = types.SimpleNamespace(C=None, V=None, BT=None, U=None, L=None, BLD=None, SMK=None, REP=None,
                          B=None, K=None, root="", cache={}, memo={}, seq=0)

DRIVER_MODULES = (("C", "sqlite_common"), ("V", "sqlite_verdicts"), ("BT", "build_and_test"),
                  ("L", "sqlite_launch"), ("K", "sqlite_corpus"), ("U", "sqlite_units"),
                  ("B", "sqlite_base"), ("BLD", "sqlite_build"), ("SMK", "sqlite_smoke"),
                  ("REP", "sqlite_report"))

# Every driver function an arm calls, resolved BY NAME before any arm runs (a missing one is a
# named FAIL, and each arm calling it fails with the AttributeError).
NEEDED = {
    "sqlite_common": ("Config", "Run", "Leg", "Ledger", "Log", "Resolver", "Result", "HarnessDie",
                      "PosixSide", "run_is_skipped", "child_env", "capture", "LEGS_JSON", "CLI_SMOKE"),
    "sqlite_verdicts": ("classify", "warn_scoped", "leg_mode", "confound_supply", "split_scope"),
    "build_and_test": ("provenance", "source_gate", "_dir_has_entries", "launcher_prereq_gate",
                       "launcher_prereq_rows", "select_legs", "read_vocabulary", "run_all",
                       "step1"),
    "sqlite_launch": ("evidence_start", "evidence_stop", "evidence_attribute",
                      "run_dir_corroboration", "SegmentResult"),
    "sqlite_units": ("stage_loadext", "loadext_verdict", "carry_helper", "poison_unrun",
                     "corroborate", "run_one_segment", "unit_leg", "judge_leg", "step8",
                     "Segment", "LegRun"),
    "sqlite_build": ("stage_headers", "write_include_lists", "attribute_build", "build_oracle",
                     "step7"),
    "sqlite_smoke": ("smoke_argv", "identify_binary", "launcher_for_target", "reference_cli",
                     "smoke_rc_verdict", "step7c"),
    "sqlite_report": ("oracle_lines", "step9"),
    "sqlite_base": ("VerdictLedger",),
}


def fixture(name, build):
    """A shared fixture, built ONCE on first use. A build that raised is remembered, and every
    arm that asks for it FAILS with the original reason (never a silent pass)."""
    if name not in X.cache:
        try:
            X.cache[name] = (True, build())
        except Exception as exc:  # noqa: BLE001 -- recorded and re-raised per dependent arm
            X.cache[name] = (False, exc_text(exc))
    ok, value = X.cache[name]
    if not ok:
        raise FixtureError("the shared fixture '%s' could not be built: %s" % (_short(name), value))
    return value


def tmpdir(*parts):
    """A fresh directory under the suite's temp root."""
    X.seq += 1
    d = os.path.join(X.root, *(list(parts) + ["%03d" % X.seq]))
    os.makedirs(d)
    return d


def write(path, data):
    parent = os.path.dirname(path)
    if parent:
        os.makedirs(parent, exist_ok=True)
    with open(path, "wb") as fh:
        fh.write(data if isinstance(data, bytes) else data.encode("utf-8"))
    return path


def read(path):
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        return fh.read()


# -- environment ---------------------------------------------------------------------------

@contextlib.contextmanager
def env_patch(values=None, unset=()):
    """Set (None = delete) and unset environment names for the block; restore after."""
    values = dict(values or {})
    names = set(unset) | set(values)
    saved = {k: os.environ.get(k) for k in names}
    try:
        for k in unset:
            os.environ.pop(k, None)
        for k, v in values.items():
            if v is None:
                os.environ.pop(k, None)
            else:
                os.environ[k] = v
        yield
    finally:
        for k, v in saved.items():
            if v is None:
                os.environ.pop(k, None)
            else:
                os.environ[k] = v


def config_env_names():
    """Every environment name `sqlite_common.Config` reads, taken from its own source (the
    upper-case string constants of the class), plus every `DSS_*` -- so a knob the operator
    exported for a REAL run can never change what these arms see."""
    names = set()
    src = textwrap.dedent(inspect.getsource(X.C.Config))
    for node in ast.walk(ast.parse(src)):
        if isinstance(node, ast.Constant) and isinstance(node.value, str) \
                and re.match(r"^[A-Z][A-Z0-9_]+$", node.value):
            names.add(node.value)
    names |= {k for k in os.environ if k.startswith("DSS_")}
    return names


GIT_HOSTILE = ("GIT_DIR", "GIT_WORK_TREE", "GIT_INDEX_FILE", "GIT_OBJECT_DIRECTORY",
               "GIT_ALTERNATE_OBJECT_DIRECTORIES", "GIT_COMMON_DIR", "GIT_NAMESPACE", "GIT_PREFIX",
               "GIT_CONFIG_PARAMETERS", "GIT_CONFIG", "GIT_CEILING_DIRECTORIES",
               "GIT_DISCOVERY_ACROSS_FILESYSTEM")


@contextlib.contextmanager
def suite_environment(root):
    """The whole run's environment: the operator's harness knobs removed, git HERMETIC (no
    system/global config, CRLF left alone, no hooks, no signing, no prompt, a discovery ceiling
    at the temp root), and no bytecode from any Python child."""
    empty_cfg = write(os.path.join(root, "hermetic-gitconfig"), b"")
    no_hooks = os.path.join(root, "no-git-hooks")
    no_template = os.path.join(root, "no-git-template")
    for d in (no_hooks, no_template):
        os.makedirs(d, exist_ok=True)
    # NOTE: every value NON-EMPTY: on Windows an empty environment value DELETES the name, and git
    # then refuses the whole GIT_CONFIG_COUNT block (MEASURED) -- so an empty template DIRECTORY,
    # never `init.templateDir=`.
    git_cfg = (("core.autocrlf", "false"), ("core.safecrlf", "false"), ("commit.gpgsign", "false"),
               ("tag.gpgsign", "false"), ("init.templateDir", no_template), ("core.hooksPath", no_hooks),
               ("core.fsmonitor", "false"), ("advice.detachedHead", "false"))
    values = {"GIT_CONFIG_NOSYSTEM": "1", "GIT_CONFIG_GLOBAL": empty_cfg,
              "GIT_TERMINAL_PROMPT": "0", "GIT_CEILING_DIRECTORIES": root,
              "GIT_AUTHOR_NAME": "dss-selftest", "GIT_AUTHOR_EMAIL": "selftest@dss.invalid",
              "GIT_COMMITTER_NAME": "dss-selftest", "GIT_COMMITTER_EMAIL": "selftest@dss.invalid",
              "GIT_CONFIG_COUNT": str(len(git_cfg)), "PYTHONDONTWRITEBYTECODE": "1"}
    for i, (k, v) in enumerate(git_cfg):
        values["GIT_CONFIG_KEY_%d" % i] = k
        values["GIT_CONFIG_VALUE_%d" % i] = v
    stale = [k for k in os.environ if k.startswith("GIT_CONFIG_KEY_") or k.startswith("GIT_CONFIG_VALUE_")]
    with env_patch(values, unset=set(config_env_names()) | set(GIT_HOSTILE) | set(stale)):
        yield


def make_config(**env):
    """A `sqlite_common.Config` read under a controlled environment (only `env` set)."""
    with env_patch(env):
        return X.C.Config()


def new_log():
    return X.C.Log(stream=io.StringIO())


def log_text(log):
    return log.stream.getvalue()


def warnings(log):
    """The warn lines of a captured log, without their ` ! ` marker."""
    return [ln[3:] for ln in log_text(log).splitlines() if ln.startswith(" ! ")]


def new_run(log=None, **env):
    log = log or new_log()
    run = X.C.Run(make_config(**env), log)
    run.driver_tree = None
    return run


def R(rc=0, out="", err=""):
    return X.C.Result(rc, out, err)


class FakeResolver:
    """A RECORDING stand-in for `sqlite_common.Resolver`: each call's argv is kept, and the
    answer is looked up by the call's VERB (its first argument) -- a Result, a callable of the
    argv returning one, or an exception to raise. An undeclared verb answers rc 99."""

    def __init__(self, answers=None, host="linux", arch="x86_64"):
        self.answers, self.calls = dict(answers or {}), []
        self.host, self.arch = host, arch

    @property
    def host_args(self):
        return ["--host-os", self.host, "--host-arch", self.arch]

    def call(self, args, catalogue=True, timeout=None, input_text=None, env_overrides=None):
        args = [str(a) for a in args]
        self.calls.append(args)
        ans = self.answers.get(args[0]) if args else None
        if callable(ans):
            ans = ans(args)
        if isinstance(ans, BaseException):
            raise ans
        if ans is None:
            return R(99, "", "fake resolver: no answer declared for %r" % (args[:1],))
        return ans

    def json(self, args, what, catalogue=True, ok=(0,)):
        r = self.call(args, catalogue=catalogue)
        if r.rc not in ok:
            X.C.die("%s: the resolver FAILED (rc=%d)" % (what, r.rc))
        return json.loads(r.out)

    def asked(self, verb):
        return [c for c in self.calls if c and c[0] == verb]


class Recording:
    """The REAL resolver, with every call recorded."""

    def __init__(self, real):
        self.real, self.calls = real, []

    @property
    def host_args(self):
        return self.real.host_args

    def call(self, args, **kw):
        self.calls.append([str(a) for a in args])
        return self.real.call(args, **kw)

    def json(self, args, what, catalogue=True, ok=(0,)):
        self.calls.append([str(a) for a in args])
        return self.real.json(args, what, catalogue=catalogue, ok=ok)

    def asked(self, verb):
        return [c for c in self.calls if c and c[0] == verb]


class Memo:
    """The REAL resolver for a verb whose answer is a CLOSED VOCABULARY (constant for the run):
    asked ONCE per suite, the same Result served to every identical later call. Only for such
    verbs -- a spawn of the 1.2 MB resolver costs a second on a loaded host."""

    def __init__(self, real):
        self.real = real

    @property
    def host_args(self):
        return self.real.host_args

    def call(self, args, **kw):
        key = (self.real.catalogue, self.real.host, self.real.arch, tuple(str(a) for a in args))
        if key not in X.memo:
            X.memo[key] = self.real.call(args, **kw)
        return X.memo[key]

    def json(self, args, what, catalogue=True, ok=(0,)):
        return self.real.json(args, what, catalogue=catalogue, ok=ok)


# -- the SHIPPED catalogue, planned by the REAL resolver -----------------------------------

LAUNCHERS_LINUX = "qemu-aarch64,wine"


def real_resolver(host=None, arch=None, catalogue=None):
    return X.C.Resolver(host or X.C.host_os(), arch or X.C.host_arch(),
                        catalogue=catalogue or X.C.LEGS_JSON)


def plan(host, arch, available, catalogue=None):
    """The resolver's plan of `catalogue` (the shipped legs.json by default) for a STATED host,
    probes skipped and launchers pinned, so the answer is the same on every machine."""
    def build():
        res = real_resolver(host, arch, catalogue)
        args = ["--plan"] + res.host_args + ["--environment-probes", "skip", "--format", "json"]
        args += ["--launchers-available", available] if available else ["--launchers-none"]
        p = res.json(args, "the %s/%s leg plan" % (host, arch))
        return collections.OrderedDict((d["label"], d) for d in p["legs"])
    return fixture(("plan", host, arch, available, catalogue), build)


def plan_leg(label, host="linux", arch="x86_64", available=LAUNCHERS_LINUX, catalogue=None):
    """A `sqlite_common.Leg` over a DEEP COPY of one planned leg (an arm may mutate it)."""
    return X.C.Leg(json.loads(json.dumps(plan(host, arch, available, catalogue)[label])))


def plan_legs(host="linux", arch="x86_64", available=LAUNCHERS_LINUX, catalogue=None):
    return [X.C.Leg(json.loads(json.dumps(d)))
            for d in plan(host, arch, available, catalogue).values()]


def unscoped_catalogue():
    """The shipped legs.json with `scope` REMOVED from the elf64-arm64 `^writecrash-` row (the
    negative CL28 and SU18 synthesise by removal from the shipped data)."""
    def build():
        with open(X.C.LEGS_JSON, "r", encoding="utf-8") as fh:
            cat = json.load(fh)
        n = 0
        for leg in cat.get("legs", []):
            if leg.get("label") != "elf64-arm64":
                continue
            for row in leg.get("confounds", []):
                if row.get("pattern") == "^writecrash-" and row.get("scope") == "emulated":
                    del row["scope"]
                    n += 1
        if n != 1:
            raise FixtureError("expected exactly ONE shipped elf64-arm64 `^writecrash-` row scoped "
                               "`emulated`, found %d: the shipped data these arms rest on changed" % n)
        path = os.path.join(tmpdir("catalogue"), "legs.json")
        with open(path, "w", encoding="utf-8", newline="\n") as fh:
            json.dump(cat, fh, indent=1)
        return path
    return fixture("unscoped_catalogue", build)


def vocabulary():
    return fixture("vocabulary", lambda: X.BT.read_vocabulary(real_resolver()))


def classes_text():
    def build():
        r = real_resolver().call(["--verdict-classes"])
        if r.rc != 0 or not r.out.strip():
            raise FixtureError("harness_legs.py --verdict-classes exited %d: %s" % (r.rc, r.err[:300]))
        return r.out
    return fixture("verdict_classes", build)


def plan_stages(kind):
    """{stage key: [name, ...]} read off the SHIPPED plan's legs (the JSON verb, the contract):
    `header` = each leg's zconfGuards by its headerStageKey, `config` = its configureAnswers by
    its configStageKey -- the declarations stage-zinc.py stages one directory per key from."""
    key_field, names_field = {"header": ("headerStageKey", "zconfGuards"),
                              "config": ("configStageKey", "configureAnswers")}[kind]
    table = collections.OrderedDict()
    for d in plan("linux", "x86_64", LAUNCHERS_LINUX).values():
        b = d.get("build") or {}
        if b.get(key_field):
            table.setdefault(b[key_field], set()).update((b.get(names_field) or {}).keys())
    if not table or not all(table.values()):
        raise FixtureError("the shipped plan declares no %s stages (or one with no names): %r" % (kind, table))
    return collections.OrderedDict((k, sorted(v)) for k, v in table.items())


def ledger(run):
    run.ledger = X.C.Ledger(vocabulary(), run.log)
    return run.ledger


def pin_leg(label, confounds=(), gating="probed", rdg="not-required", run_mode="native",
            launcher=(), omit=()):
    """A plan-SHAPED leg built by hand (the twins' fixture legs)."""
    d = {"label": label, "spec": "x86_64:elf64-x86_64-linux-exec",
         "format": "elf64-x86_64-linux-exec", "confoundsByName": list(confounds),
         "confoundsByEvidence": [], "executionEvidence": [], "confoundGating": gating,
         "runDirectoryGating": rdg, "run": {"mode": run_mode, "launcher": list(launcher)}}
    for k in omit:
        d.pop(k, None)
    return X.C.Leg(d)


# =======================================================================================
# CL -- the classifier
# =======================================================================================

P_TIER = ("no_mutex_try.", "memsubsys1.", "memsubsys2.", "mm-")
F0 = ("writecrash-1.1.1", "walsetlk-2.1.3", "zipfile-25.0", "sometest-9.9")
P_SH = ("^walsetlk-", "^zipfile-25\\.0$", "^recoverfault", "emulated:^writecrash-")
P_PS = ("^walsetlk-", "^zipfile-25\\.0$", "emulated:^writecrash-")
QUAL = ("memsubsys1.walsetlk-2.2.6", "no_mutex_try.busy2-2.2.3",
        "memsubsys2.recoverfault-1-oom-persistent.515", "memsubsys1.zipfile-25.0",
        "no_mutex_try.walsetlk_recover-1.2", "memsubsys2.realbug-1.1")
P_QUAL = ("^walsetlk-", "^busy2-", "^zipfile-25\\.0$", "^recoverfault")
W_NAMES = ("no_mutex_try.walsetlk_recover-1.2", "walsetlk_recover-1.3.(36244809)",
           "memsubsys2.realbug-1.1")
P_W = ("^walsetlk-", "^walsetlk_recover-", "^recoverfault")
MM_NAMES = ("mm-zipfile-25.0", "mm-walsetlk-2.1.3", "mm-backup4-3.3")
P_MM = ("^walsetlk-", "^zipfile-25\\.0$")


def cls(failures, patterns, mode, prefixes=P_TIER):
    return X.V.classify(list(failures), list(patterns), list(prefixes), mode)


def mode_of(run_mode, launcher=()):
    return X.V.leg_mode(pin_leg("mode-probe", run_mode=run_mode, launcher=launcher))


@arm("CL01", "launched leg: an `emulated:` pattern EXCUSES writecrash", "S1.01")
def _cl01():
    mode = mode_of("launched", ["qemu-aarch64"])
    eq(mode, "emulated", "leg_mode() of a LAUNCHED leg")
    c = cls(F0, P_SH, mode)
    has(c.confound, "writecrash-1.1.1", "writecrash must be excused on a launched leg")
    eq(c.confound, ["walsetlk-2.1.3", "writecrash-1.1.1", "zipfile-25.0"],
       "the excused names, deduplicated, in code-point order")


@arm("CL02", "...and NAMES it as scope-excused", "S1.02")
def _cl02():
    eq(cls(F0, P_SH, mode_of("launched", ["qemu-aarch64"])).scoped, ["writecrash-1.1.1"],
       "the scope-excused names")


@arm("CL03", "...while the genuine failure stays REAL", "S1.03")
def _cl03():
    eq(cls(F0, P_SH, mode_of("launched", ["qemu-aarch64"])).real, ["sometest-9.9"], "the real names")


@arm("CL04", "native leg: an `emulated:` pattern does NOT excuse (order + dedup contract)",
     "S1.04 P1.01")
def _cl04():
    eq(mode_of("native"), "native", "leg_mode() of a NATIVE leg")
    eq(mode_of(""), "native", "leg_mode() of a leg with NO run mode (the .ps1 fixture left it unset)")
    eq(cls(F0, P_SH, "native").real, ["sometest-9.9", "writecrash-1.1.1"],
       "real on native, the .sh fixture (code-point order)")
    eq(cls(F0, P_PS, "native").real, ["sometest-9.9", "writecrash-1.1.1"],
       "real on native, the .ps1 fixture")
    eq(cls(F0 + ("writecrash-1.1.1", ""), P_SH, "native").real, ["sometest-9.9", "writecrash-1.1.1"],
       "a duplicated name is reported ONCE, an empty one never")


@arm("CL05", "native leg: NO scope excusals are recorded", "S1.05 P1.04")
def _cl05():
    eq(cls(F0, P_SH, "native").scoped, [], "scoped, the .sh fixture")
    eq(cls(F0, P_PS, "native").scoped, [], "scoped, the .ps1 fixture")


@arm("CL06", "native leg: BARE patterns still excuse", "S1.06 P1.02")
def _cl06():
    eq(cls(F0, P_SH, "native").confound, ["walsetlk-2.1.3", "zipfile-25.0"], "confound, the .sh fixture")
    eq(cls(F0, P_PS, "native").confound, ["walsetlk-2.1.3", "zipfile-25.0"], "confound, the .ps1 fixture")


@arm("CL07", "native leg: the genuine failure stays REAL", "P1.03")
def _cl07():
    has(cls(F0, P_PS, "native").real, "sometest-9.9", "sometest-9.9 on a native leg")


@arm("CL08", "native leg: a `native:` pattern EXCUSES", "P1.05")
def _cl08():
    has(cls(F0, ["native:^writecrash-"], "native").confound, "writecrash-1.1.1",
        "`native:^writecrash-` on a native leg")


@arm("CL09", "...and NAMES it as scope-excused", "P1.06")
def _cl09():
    eq(cls(F0, ["native:^writecrash-"], "native").scoped, ["writecrash-1.1.1"], "the scope-excused names")


@arm("CL10", "launched leg: a `native:` pattern does NOT excuse (CL08's negative)")
def _cl10():
    c = cls(F0, ["native:^writecrash-"], mode_of("launched", ["qemu-aarch64"]))
    has(c.real, "writecrash-1.1.1", "a `native:` row must be inert on a launched leg")
    eq(c.scoped, [], "nothing is scope-excused")


@arm("CL11", "red-on-disable: the SAME regex without its scope leaks onto native", "S1.07 P1.07")
def _cl11():
    scoped = cls(F0, P_SH, "native")
    bare = cls(F0, P_SH[:3] + ("^writecrash-",), "native")
    has(scoped.real, "writecrash-1.1.1", "control: the scoped row does not excuse on native")
    has(bare.confound, "writecrash-1.1.1", "the unscoped row (.sh fixture) DOES excuse on native")
    has(cls(F0, ["^writecrash-"], "native").confound, "writecrash-1.1.1",
        "the unscoped row alone (.ps1 fixture) DOES excuse on native")


@arm("CL12", "a permutation-qualified walsetlk name is excused", "S1.08")
def _cl12():
    has(cls(QUAL, P_QUAL, "native").confound, "memsubsys1.walsetlk-2.2.6", "memsubsys1.walsetlk-2.2.6")


@arm("CL13", "a permutation-qualified busy2 name is excused", "S1.09")
def _cl13():
    has(cls(QUAL, P_QUAL, "native").confound, "no_mutex_try.busy2-2.2.3", "no_mutex_try.busy2-2.2.3")


@arm("CL14", "a permutation-qualified recoverfault name is excused", "S1.10")
def _cl14():
    has(cls(QUAL, P_QUAL, "native").confound, "memsubsys2.recoverfault-1-oom-persistent.515",
        "memsubsys2.recoverfault-1-oom-persistent.515")


@arm("CL15", "a permutation-qualified zipfile name is excused under the anchored `$`", "S1.11")
def _cl15():
    has(cls(QUAL, P_QUAL, "native").confound, "memsubsys1.zipfile-25.0", "memsubsys1.zipfile-25.0")


@arm("CL16", "walsetlk_recover is NOT swept in by `^walsetlk-` (family resemblance)", "S1.12")
def _cl16():
    has(cls(QUAL, P_QUAL, "native").real, "no_mutex_try.walsetlk_recover-1.2",
        "no_mutex_try.walsetlk_recover-1.2 has its OWN row; `^walsetlk-` must not reach it")


@arm("CL17", "a permutation-qualified genuine failure stays REAL", "S1.13")
def _cl17():
    has(cls(QUAL, P_QUAL, "native").real, "memsubsys2.realbug-1.1", "memsubsys2.realbug-1.1")


@arm("CL18", "walsetlk_recover is excused by ITS OWN row", "S1.14")
def _cl18():
    has(cls(W_NAMES, P_W, "native").confound, "no_mutex_try.walsetlk_recover-1.2",
        "no_mutex_try.walsetlk_recover-1.2 under `^walsetlk_recover-`")


@arm("CL19", "...the (n)-suffixed form too", "S1.15")
def _cl19():
    has(cls(W_NAMES, P_W, "native").confound, "walsetlk_recover-1.3.(36244809)",
        "walsetlk_recover-1.3.(36244809)")


@arm("CL20", "...while an unrelated failure stays REAL", "S1.16")
def _cl20():
    has(cls(W_NAMES, P_W, "native").real, "memsubsys2.realbug-1.1", "memsubsys2.realbug-1.1")


@arm("CL21", "an `mm-` zipfile name is excused", "S1.17 P1.08")
def _cl21():
    has(cls(MM_NAMES, P_MM, "native").confound, "mm-zipfile-25.0", "mm-zipfile-25.0")


@arm("CL22", "an `mm-` walsetlk name is excused", "S1.18 P1.09")
def _cl22():
    has(cls(MM_NAMES, P_MM, "native").confound, "mm-walsetlk-2.1.3", "mm-walsetlk-2.1.3")


@arm("CL23", "an `mm-` name no pattern matches stays REAL", "S1.19 P1.10")
def _cl23():
    has(cls(MM_NAMES, P_MM, "native").real, "mm-backup4-3.3", "mm-backup4-3.3")


@arm("CL24", "red-on-disable: with NO prefixes, dotted-qualified confounds read as REAL", "S1.20")
def _cl24():
    c = cls(QUAL, P_QUAL, "native", prefixes=())
    eq(c.real[:1], ["memsubsys1.walsetlk-2.2.6"], "the first real name without prefixes")
    eq(c.confound, [], "nothing excused without the prefixes")
    expect(cls(QUAL, P_QUAL, "native").confound, "control: WITH the prefixes the same names are excused")


@arm("CL25", "red-on-disable: with NO prefixes, `mm-` confounds read as REAL", "P1.11")
def _cl25():
    has(cls(MM_NAMES, P_MM, "native", prefixes=()).real, "mm-zipfile-25.0",
        "mm-zipfile-25.0 without the `mm-` prefix")


@arm("CL26", "case policy: `^WALSETLK-` excuses nothing; `NATIVE:`/`EMULATED:` are not scopes")
def _cl26():
    eq(cls(["walsetlk-2.1.3"], ["^WALSETLK-"], "native").real, ["walsetlk-2.1.3"],
       "an upper-case pattern must not excuse a lower-case name (re.search is case-SENSITIVE; the "
       ".ps1's case-folding excused MORE)")
    c = cls(["writecrash-1.1.1"], ["NATIVE:^writecrash-"], "native")
    eq((c.real, c.scoped), (["writecrash-1.1.1"], []), "`NATIVE:` is not the `native:` scope")
    c = cls(["writecrash-1.1.1"], ["EMULATED:^writecrash-"], "emulated")
    eq((c.real, c.scoped), (["writecrash-1.1.1"], []), "`EMULATED:` is not the `emulated:` scope")
    c = cls(["writecrash-1.1.1"], ["native:^writecrash-"], "native")
    eq((c.confound, c.scoped), (["writecrash-1.1.1"], ["writecrash-1.1.1"]),
       "control: the lower-case scope IS honoured")


@arm("CL27", "the scoped-excusal WARN names the count, the mode and the truncation caveat")
def _cl27():
    log = new_log()
    X.V.warn_scoped("elf64-arm64", cls(F0, P_SH, "emulated").scoped, "emulated", log)
    w = warnings(log)
    eq(len(w), 3, "the scoped-excusal warning is three lines")
    has(w[0], "[elf64-arm64] 1 failure(s) excused ONLY because this leg runs 'emulated': "
        "writecrash-1.1.1", "the leg, the count, the mode and the name")
    has(w[1], "NOT evidence of correctness on a native run", "the coverage statement")
    has(w[2], "TRUNCATE", "the truncation caveat")
    has(w[2], "coverage there is partial", "the truncation caveat")
    log2 = new_log()
    X.V.warn_scoped("leg-x", ["a-1", "b-2"], "native", log2)
    has(warnings(log2)[0], "2 failure(s) excused ONLY because this leg runs 'native': a-1 b-2",
        "the count is measured, not fixed")
    log3 = new_log()
    X.V.warn_scoped("leg-x", [], "emulated", log3)
    eq(log_text(log3), "", "nothing scope-excused -> nothing said")


@arm("CL28", "SHIPPED elf64-arm64 writecrash row, planned: excused launched, real native; "
     "unscoped copy LEAKS")
def _cl28():
    names = ["writecrash-1.1.1", "sometest-9.9"]
    launched = plan_leg("elf64-arm64")
    eq(launched.run_mode, "launched", "the linux/x86_64 plan runs elf64-arm64 under its launcher")
    has(launched.d["confoundsByName"], "emulated:^writecrash-", "the shipped plan's scoped wire")
    c = cls(names, launched.d["confoundsByName"], X.V.leg_mode(launched))
    eq((c.confound, c.scoped, c.real), (["writecrash-1.1.1"], ["writecrash-1.1.1"], ["sometest-9.9"]),
       "launched: excused, scope-excused, and the genuine one real")
    native = plan_leg("elf64-arm64", "linux", "arm64", "")
    eq(native.run_mode, "native", "the linux/arm64 plan runs elf64-arm64 natively")
    eq(cls(names, native.d["confoundsByName"], X.V.leg_mode(native)).real,
       ["sometest-9.9", "writecrash-1.1.1"], "native: the emulated-earned row excuses nothing")
    leak = plan_leg("elf64-arm64", "linux", "arm64", "", catalogue=unscoped_catalogue())
    has(leak.d["confoundsByName"], "^writecrash-", "with `scope` removed the wire is bare")
    eq(cls(names, leak.d["confoundsByName"], X.V.leg_mode(leak)).confound, ["writecrash-1.1.1"],
       "NEGATIVE by removal: the unscoped row LEAKS onto native -- so the shipped `scope` is what "
       "prevents it")


# =======================================================================================
# PV + GT -- provenance and the source gate, against REAL repositories
# =======================================================================================

# A file name git QUOTES in porcelain and name-only output (core.quotePath: a non-ASCII byte),
# spelled by code point so this file stays ASCII.
QUOTED = "caf" + chr(0xE9) + ".c"


class GitWorld:
    """Throwaway repositories, each built on first use. Every one is a FIXTURE the arms only
    read; a state an arm needs is its own copy."""

    def __init__(self, root):
        self.root = root
        self.made = {}

    def git(self, args, check=True):
        p = subprocess.run(["git"] + [str(a) for a in args], stdin=subprocess.DEVNULL,
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                           encoding="utf-8", errors="replace", timeout=120)
        if check and p.returncode != 0:
            raise FixtureError("git %s exited %d: %s" % (" ".join(str(a) for a in args), p.returncode,
                                                         (p.stderr or p.stdout).strip()[:400]))
        return p

    def out(self, *args):
        return self.git(args).stdout.strip()

    def raw(self, *args):
        p = self.git(args, check=False)
        return p.returncode, p.stdout.strip()

    def repo(self, name):
        if name not in self.made:
            self.made[name] = getattr(self, "_" + name.replace("-", "_"))()
        return self.made[name]

    def _p(self, name):
        return os.path.join(self.root, name)

    def _init(self, name, files):
        d = self._p(name)
        self.git(["init", "-q", d])
        for rel, data in files:
            write(os.path.join(d, rel), data)
        self.git(["-C", d, "add"] + [rel for rel, _ in files])
        self.git(["-C", d, "commit", "-q", "--no-verify", "-m", "seed"])
        return d

    def info(self, d, short=False, **extra):
        """HEAD's full sha and branch in ONE git process (`rev-parse HEAD --abbrev-ref HEAD` prints
        both, git 2.43 and 2.55 alike -- MEASURED), the short sha only when an arm needs it: every
        spawn counts on a loaded Windows host."""
        long_, branch = self.out("-C", d, "rev-parse", "HEAD", "--abbrev-ref", "HEAD").splitlines()
        i = {"path": d, "long": long_, "branch": branch}
        if short:
            i["short"] = self.out("-C", d, "rev-parse", "--short", "HEAD")
        i.update(extra)
        return i

    def _copy(self, src, name):
        d = self._p(name)
        shutil.copytree(self.repo(src)["path"], d, symlinks=True)
        return d

    def _clean(self):
        return self.info(self._init("clean", [("tracked.c", b"tracked\n")]), short=True)

    def _untracked(self):
        d = self._copy("clean", "untracked")
        write(os.path.join(d, "added.c"), b"added\n")
        return {"path": d}

    def _dirty(self):
        d = self._copy("clean", "dirty")
        write(os.path.join(d, "added.c"), b"added\n")
        write(os.path.join(d, "tracked.c"), b"tracked\nmore\n")
        return {"path": d}

    def _second(self):
        d = self._copy("clean", "second")
        write(os.path.join(d, "tracked.c"), b"second\n")
        self.git(["-C", d, "commit", "-q", "--no-verify", "-am", "second"])
        return self.info(d, first_long=self.repo("clean")["long"])

    def _detached(self):
        d = self._copy("clean", "detached")
        self.git(["-C", d, "checkout", "-q", "--detach", "HEAD"])
        return {"path": d}

    def _nested(self):
        host = self._copy("clean", "nested-host")
        n = os.path.join(host, "not-a-checkout")
        write(os.path.join(n, "src.c"), b"x\n")
        clean = self.repo("clean")
        return {"path": n, "host_short": clean["short"], "host_branch": clean["branch"]}

    def _crlf(self):
        d = self._init("crlf", [("lf.c", b"a\nb\n"), ("other.c", b"x\n")])
        write(os.path.join(d, "lf.c"), b"a\r\nb\r\n")     # CR-only: not a semantic change
        write(os.path.join(d, "other.c"), b"x\ny\n")      # a real change
        return {"path": d}

    def _crlf_only(self):
        d = self._copy("crlf", "crlf-only")
        write(os.path.join(d, "other.c"), b"x\n")         # back to HEAD: only the CR-only one left
        return {"path": d}

    def _quoted(self):
        d = self._init("quoted", [(QUOTED, b"v1\n")])
        write(os.path.join(d, QUOTED), b"v1\nv2 -- a real change\n")
        return {"path": d}

    def _badgit(self):
        d = self._p("badgit")
        write(os.path.join(d, "src.c"), b"x\n")
        write(os.path.join(d, ".git"), b"")               # a `.git` that points nowhere
        return {"path": d}

    def _origin(self):
        d = self._init("origin", [("f.c", b"v1\n")])
        head = self.info(d)                  # the default branch is READ, never assumed
        self.git(["-C", d, "checkout", "-q", "-b", "feature/probe"])
        write(os.path.join(d, "f.c"), b"v1\nv2\n")
        self.git(["-C", d, "commit", "-q", "--no-verify", "-am", "feature"])
        feature_long = self.out("-C", d, "rev-parse", "HEAD")
        self.git(["-C", d, "checkout", "-q", head["branch"]])
        return {"path": d, "default": head["branch"], "default_long": head["long"],
                "feature_long": feature_long}

    def _worktree(self):
        d = self._p("worktree")
        self.git(["clone", "-q", self.repo("origin")["path"], d])
        write(os.path.join(d, "f.c"), b"v1\ndirty\n")
        write(os.path.join(d, "extra.c"), b"new\n")
        return {"path": d}

    def _gitfile(self):
        base = self._p("gitfile-base")
        self.git(["clone", "-q", self.repo("origin")["path"], base])
        d = self._p("gitfile-worktree")
        self.git(["-C", base, "worktree", "add", "-q", "-b", "selftest-worktree", d])
        return self.info(d)


def gw():
    return fixture("gitworld", lambda: GitWorld(tmpdir("git")))


def repo(name):
    return fixture(("repo", name), lambda: gw().repo(name))


def prov(name):
    return fixture(("provenance", name), lambda: X.BT.provenance(repo(name)["path"]))


def plain_dir(kind):
    """Non-repository directories: `plain` (a file), `empty`, `hidden` (a dotfile only)."""
    def build():
        d = tmpdir("fs", kind)
        if kind == "plain":
            write(os.path.join(d, "file.c"), b"one\n")
        elif kind == "hidden":
            write(os.path.join(d, ".hidden"), b"hi\n")
        return d
    return fixture(("fs", kind), build)


GateOutcome = collections.namedtuple("GateOutcome", ["run", "log", "died"])


def gate_run(repo_root, env=None, **cfg_fields):
    """`build_and_test.source_gate` on `repo_root`; the Config is read under `env` and then
    `cfg_fields` are set on it. -> GateOutcome(run, log, died-message-or-None)."""
    log = new_log()
    with env_patch(env or {}):
        run = X.C.Run(X.C.Config(), log)
    run.repo_root = repo_root
    run.driver_tree = None
    for k, v in cfg_fields.items():
        setattr(run.cfg, k, v)
    try:
        X.BT.source_gate(run)
        return GateOutcome(run, log, None)
    except X.C.HarnessDie as exc:
        return GateOutcome(run, log, str(exc))


def gate_ok(outcome, what):
    if outcome.died is not None:
        raise ArmFailure("%s: the gate REFUSED: %s" % (what, _short(outcome.died)))
    has(log_text(outcome.log), "dsscp checkout ready", "%s: the gate reaches its end" % what)


@arm("PV01", "HEAD is the real short sha", "S2.01 P2.01", "git")
def _pv01():
    eq(prov("clean")["head_short"], repo("clean")["short"], "head_short")


@arm("PV02", "the branch is the real branch", "S2.02 P2.02", "git")
def _pv02():
    eq(prov("clean")["branch"], repo("clean")["branch"], "branch")


@arm("PV03", "a clean tree diverges by 0", "S2.03", "git")
def _pv03():
    eq(prov("clean")["diverge"], "0", "diverge")


@arm("PV04", "a clean tree gets NO divergence note", "S2.04 P2.03", "git")
def _pv04():
    eq(prov("clean")["diverge_note"], "", "diverge_note")


@arm("PV05", "a clean tree does not refuse (the gate reaches its end)", "P2.04", "git")
def _pv05():
    g = gate_run(repo("clean")["path"])
    gate_ok(g, "a clean checkout")
    eq(g.run.provenance.get("head_long"), repo("clean")["long"], "the provenance the gate recorded")


@arm("PV06", "a non-repo HEAD says UNKNOWN(no .git ...), never empty", "S2.05")
def _pv06():
    pv = X.BT.provenance(plain_dir("plain"))
    expect(pv["head_short"].startswith("UNKNOWN(no .git"), "head_short = %r" % pv["head_short"])
    eq((pv["head_long"], pv["branch"]), ("", "UNKNOWN"), "head_long / branch of a non-checkout")


@arm("PV07", "a non-repo divergence is UNCOMPUTABLE (\"\"), distinct from 0", "S2.06")
def _pv07():
    pv = X.BT.provenance(plain_dir("plain"))
    eq(pv["diverge"], "", "diverge of a non-checkout")
    has(pv["diverge_note"], "UNVERIFIED", "the note says it could not be measured")


@arm("PV08", "NEGATIVE: a bare rev-parse on a non-checkout yields EMPTY", "S2.13", "git")
def _pv08():
    rc, out_ = gw().raw("-C", plain_dir("plain"), "rev-parse", "--short", "HEAD")
    eq(out_, "", "raw `git rev-parse --short HEAD` stdout outside any repository")
    expect(rc != 0, "raw rev-parse must FAIL there (rc=%d) -- else the fixture sits inside a repo" % rc)


@arm("PV09", "a non-checkout NESTED in a repo: head UNKNOWN(no .git ...)", "P2.14", "git")
def _pv09():
    expect(prov("nested")["head_short"].startswith("UNKNOWN(no .git"),
           "head_short = %r" % prov("nested")["head_short"])


@arm("PV10", "...its divergence is UNVERIFIED, never reported clean", "P2.15", "git")
def _pv10():
    has(prov("nested")["diverge_note"], "UNVERIFIED", "diverge_note")
    eq(prov("nested")["diverge"], "", "diverge")


@arm("PV11", "NEGATIVE: a bare rev-parse there reports the ENCLOSING repo's HEAD", "P2.16", "git")
def _pv11():
    n = repo("nested")
    rc, out_ = gw().raw("-C", n["path"], "rev-parse", "--short", "HEAD")
    eq((rc, out_), (0, n["host_short"]), "raw rev-parse inside the nested non-checkout")


@arm("PV12", "a nested non-checkout's branch is UNKNOWN, not the enclosing branch", "", "git")
def _pv12():
    n = repo("nested")
    eq(prov("nested")["branch"], "UNKNOWN", "branch")
    rc, out_ = gw().raw("-C", n["path"], "rev-parse", "--abbrev-ref", "HEAD")
    eq((rc, out_), (0, n["host_branch"]), "NEGATIVE: raw git names the ENCLOSING branch there")


@arm("PV13", "dir_has_entries: a populated directory", "S2.07")
def _pv13():
    eq(X.BT._dir_has_entries(plain_dir("plain")), True, "populated")


@arm("PV14", "dir_has_entries: an EMPTY (or absent) directory", "S2.08")
def _pv14():
    eq(X.BT._dir_has_entries(plain_dir("empty")), False, "empty")
    eq(X.BT._dir_has_entries(os.path.join(plain_dir("empty"), "absent")), False, "absent")


@arm("PV15", "dir_has_entries: a DOTFILE-only directory (and the gate refuses it)", "S2.09")
def _pv15():
    d = plain_dir("hidden")
    eq(X.BT._dir_has_entries(d), True, "dotfile-only")
    g = gate_run(d, allow_fresh_clone=True)
    expect(g.died is not None, "a dotfile-only non-checkout must be REFUSED, never cloned into")
    has(g.died, "is NOT a git checkout", "the refusal")


@arm("PV16", "an UNTRACKED file counts as divergence", "S2.10", "git")
def _pv16():
    eq(prov("untracked")["diverge"], "1", "diverge")


@arm("PV17", "modified + untracked = 2", "S2.11", "git")
def _pv17():
    eq(prov("dirty")["diverge"], "2", "diverge")


@arm("PV18", "the note carries the count", "S2.12 P2.12", "git")
def _pv18():
    has(prov("dirty")["diverge_note"], "+2 file(s) differ from HEAD", "diverge_note")


@arm("PV19", "NEGATIVE: `status -uno` UNDERCOUNTS (1 vs 2)", "S2.14", "git")
def _pv19():
    p = gw().git(["-C", repo("dirty")["path"], "status", "--porcelain", "-uno"])
    eq(len([ln for ln in p.stdout.splitlines() if ln.strip()]), 1, "raw `status --porcelain -uno` lines")


@arm("PV20", "the divergence is WARNED about, not merely noted", "S3.08 P2.13", "git")
def _pv20():
    g = gate_run(repo("dirty")["path"])
    gate_ok(g, "a dirty checkout")
    expect([w for w in warnings(g.log) if "differ from HEAD" in w],
           "a WARN line naming the divergence; warnings: %r" % warnings(g.log))


@arm("PV21", "a CR-only (LF->CRLF) change is not counted and the note says so -- and ONLY those", "", "git")
def _pv21():
    # Every finding is collected, so one run names every way the exclusion is wrong on this host.
    bad = []
    p = gw().git(["-C", repo("crlf")["path"], "status", "--porcelain"])
    if len([ln for ln in p.stdout.splitlines() if ln.strip()]) != 2:
        bad.append("fixture NEGATIVE: raw `status --porcelain` must list BOTH files (the CR-only one "
                   "included): %r" % p.stdout)
    pv = prov("crlf")
    if pv["diverge"] != "1":
        bad.append("one CR-only + one REAL change: diverge must count only the real one -- want '1', "
                   "got %r (note %r)" % (pv["diverge"], pv["diverge_note"]))
    for needle in ("+1 file(s) differ from HEAD", "1 CR-only difference(s) excluded"):
        if needle not in pv["diverge_note"]:
            bad.append("the note must say %r: got %r" % (needle, pv["diverge_note"]))
    only = prov("crlf-only")
    if (only["diverge"], only["diverge_note"]) != ("0", ""):
        bad.append("a tree whose ONLY change is CR-only is clean: got diverge=%r note=%r"
                   % (only["diverge"], only["diverge_note"]))
    raw = gw().git(["-C", repo("quoted")["path"], "status", "--porcelain"]).stdout
    if '"' not in raw:
        bad.append("fixture NEGATIVE: git must QUOTE %r in porcelain output here: %r" % (QUOTED, raw))
    q = prov("quoted")
    if (q["diverge"], "CR-only" in q["diverge_note"]) != ("1", False):
        bad.append("a REAL change to a tracked file whose name git QUOTES must be counted, never excluded "
                   "as CR-only: got diverge=%r note=%r" % (q["diverge"], q["diverge_note"]))
    if bad:
        raise ArmFailure("\n".join(bad))


@arm("PV22", "a detached HEAD maps to DETACHED-HEAD", "P2.17", "git")
def _pv22():
    eq(prov("detached")["branch"], "DETACHED-HEAD", "branch")


@arm("PV23", "NEGATIVE: raw git prints the literal `HEAD` there", "P2.18", "git")
def _pv23():
    eq(gw().raw("-C", repo("detached")["path"], "rev-parse", "--abbrev-ref", "HEAD"), (0, "HEAD"),
       "raw abbrev-ref on a detached HEAD")


@arm("PV24", "a DSS_COMMIT matching HEAD is VERIFIED", "S3.11 P2.05", "git")
def _pv24():
    c = repo("clean")
    g = gate_run(c["path"], dss_commit=c["long"])
    gate_ok(g, "DSS_COMMIT = HEAD")
    has(log_text(g.log), "DSS_COMMIT verified: HEAD is %s" % c["long"], "the verification line")


@arm("PV25", "an ABBREVIATED DSS_COMMIT resolves and verifies", "P2.06", "git")
def _pv25():
    c = repo("clean")
    g = gate_run(c["path"], dss_commit=c["short"])
    gate_ok(g, "DSS_COMMIT = the short sha")
    has(log_text(g.log), "DSS_COMMIT verified: HEAD is %s" % c["long"], "resolved to the FULL sha")


@arm("PV26", "NEGATIVE: a bare string compare would REJECT that abbreviation", "P2.07", "git")
def _pv26():
    c = repo("clean")
    expect(c["short"] != c["long"] and c["long"].startswith(c["short"]),
           "short %r must differ from long %r while prefixing it" % (c["short"], c["long"]))


@arm("PV27", "a DSS_COMMIT that is not here REFUSES", "S3.12 P2.08", "git")
def _pv27():
    g = gate_run(repo("clean")["path"], dss_commit="deadbeef" * 5)
    expect(g.died is not None, "an unknown DSS_COMMIT must be refused")
    has(g.died, "does not resolve to a commit", "the refusal")


@arm("PV28", "a resolvable DSS_COMMIT that is NOT HEAD refuses", "P2.09", "git")
def _pv28():
    s = repo("second")
    g = gate_run(s["path"], dss_commit=s["first_long"])
    expect(g.died is not None, "a DSS_COMMIT that is not HEAD must be refused")
    has(g.died, "The run would have validated the checkout", "the refusal")


@arm("PV29", "a DSS_BRANCH mismatch refuses", "S3.10 P2.10", "git")
def _pv29():
    g = gate_run(repo("clean")["path"], dss_branch="not-this-branch")
    expect(g.died is not None, "a DSS_BRANCH mismatch must be refused")
    has(g.died, "DSS_BRANCH='not-this-branch'", "the refusal names the value")


@arm("PV30", "a DSS_BRANCH matching the checkout proceeds", "P2.11", "git")
def _pv30():
    c = repo("clean")
    gate_ok(gate_run(c["path"], dss_branch=c["branch"]), "DSS_BRANCH = the checkout's branch")


@arm("PV31", "a DSS_COMMIT on a NON-checkout is refused, never verified", "", "git")
def _pv31():
    for label, d in (("a populated non-checkout", plain_dir("plain")),
                     ("a directory whose `.git` points nowhere", repo("badgit")["path"])):
        g = gate_run(d, dss_commit=repo("clean")["long"])
        expect(g.died is not None, "%s with DSS_COMMIT must be refused" % label)
        lacks(log_text(g.log), "DSS_COMMIT verified", "%s: nothing may be verified" % label)


@arm("GT01", "(a) an ABSENT directory refuses instead of cloning", "S3.01")
def _gt01():
    d = os.path.join(tmpdir("gate"), "nope")
    g = gate_run(d, env={"DSS_ALLOW_FRESH_CLONE": "yes"})
    eq(g.run.cfg.allow_fresh_clone, False, "only the literal `1` opts in (`yes` does not)")
    expect(g.died is not None, "an absent SRC_DIR without the opt-in must be refused")
    has(g.died, "will NOT clone one silently", "the refusal")


@arm("GT02", "(a) ...and the gate does not fall through", "S3.02")
def _gt02():
    d = os.path.join(tmpdir("gate"), "nope")
    g = gate_run(d)
    expect(g.died is not None, "refused")
    eq(g.run.provenance, {}, "no provenance was recorded")
    lacks(log_text(g.log), "dsscp checkout ready", "the gate's end was not reached")
    eq(os.path.lexists(d), False, "nothing was created at SRC_DIR")


@arm("GT03", "(c) a populated NON-checkout is refused, even with the opt-in", "S3.03")
def _gt03():
    d = tmpdir("gate", "rsynced")
    write(os.path.join(d, "src.c"), b"x\n")
    g = gate_run(d, allow_fresh_clone=True, dss_repo_url=os.path.join(X.root, "nowhere"))
    expect(g.died is not None, "refused")
    has(g.died, "is NOT a git checkout", "the refusal names the rsync shape")
    eq((sorted(os.listdir(d)), read(os.path.join(d, "src.c"))), (["src.c"], "x\n"),
       "the tree is untouched (never cloned over)")


def gate_clone(tag, **env):
    o = repo("origin")
    dest = os.path.join(tmpdir("gate", tag), "cloned")
    values = {"DSS_ALLOW_FRESH_CLONE": "1", "DSS_REPO_URL": o["path"]}
    values.update(env)
    return gate_run(dest, env=values), dest


def gt_clone():
    return fixture("gt_clone", lambda: gate_clone("clone", DSS_BRANCH="feature/probe"))


@arm("GT04", "an opt-in fresh clone reaches the end", "S3.04", "git")
def _gt04():
    g, dest = gt_clone()
    gate_ok(g, "DSS_ALLOW_FRESH_CLONE=1")
    expect(os.path.isdir(os.path.join(dest, ".git")), "the clone exists at SRC_DIR")


@arm("GT05", "...and lands on DSS_BRANCH", "S3.05", "git")
def _gt05():
    g, dest = gt_clone()
    gate_ok(g, "DSS_ALLOW_FRESH_CLONE=1 DSS_BRANCH=feature/probe")
    eq(g.run.provenance.get("branch"), "feature/probe", "the recorded branch")
    eq(gw().raw("-C", dest, "rev-parse", "--abbrev-ref", "HEAD"), (0, "feature/probe"), "the clone's HEAD")


@arm("GT06", "NEGATIVE: a bare clone lands on the DEFAULT branch, not the feature", "S3.06", "git")
def _gt06():
    o = repo("origin")
    d = os.path.join(tmpdir("gate", "oldway"), "cloned")
    gw().git(["clone", "-q", o["path"], d])
    rc, br = gw().raw("-C", d, "rev-parse", "--abbrev-ref", "HEAD")
    eq((rc, br), (0, o["default"]), "a bare clone's branch")
    expect(br != "feature/probe", "so GT05 is not restating git's default")


def gt_dirty():
    """ONE gate over the dirty clone (the twins ran it once and read both properties off it)."""
    return fixture("gt_dirty", lambda: gate_run(repo("worktree")["path"]))


@arm("GT07", "(b) a DIRTY tree still runs (never a hard failure)", "S3.07", "git")
def _gt07():
    gate_ok(gt_dirty(), "a dirty clone")


@arm("GT08", "(b) the banner carries the count", "S3.09", "git")
def _gt08():
    g = gt_dirty()
    gate_ok(g, "a dirty clone")
    eq(g.run.provenance.get("diverge"), "2", "the recorded divergence")
    expect([ln for ln in log_text(g.log).splitlines() if " at " in ln and "+2 file(s) differ from HEAD" in ln],
           "the `at <sha> on <branch>` banner carries `+2 file(s) differ from HEAD`")


@arm("GT09", "a `.git` FILE (a git worktree) is accepted as a checkout", "", "git")
def _gt09():
    w = repo("gitfile")
    gitent = os.path.join(w["path"], ".git")
    expect(os.path.isfile(gitent) and not os.path.isdir(gitent),
           "fixture: `git worktree add` writes a `.git` FILE")
    expect(X.BT._dir_has_entries(w["path"]),
           "NEGATIVE: a directory-only `.git` test would see a POPULATED NON-checkout and refuse it")
    g = gate_run(w["path"])
    gate_ok(g, "a git worktree")
    eq((g.run.provenance.get("head_long"), g.run.provenance.get("branch")),
       (w["long"], "selftest-worktree"), "the worktree's own HEAD and branch")


@arm("GT10", "the fresh-clone path still VERIFIES DSS_COMMIT", "", "git")
def _gt10():
    o = repo("origin")
    good, _ = gate_clone("commit-ok", DSS_BRANCH="feature/probe", DSS_COMMIT=o["feature_long"])
    gate_ok(good, "clone + DSS_COMMIT = the feature tip")
    has(log_text(good.log), "DSS_COMMIT verified: HEAD is %s" % o["feature_long"], "verified")
    bad, _ = gate_clone("commit-bad", DSS_BRANCH="feature/probe", DSS_COMMIT=o["default_long"])
    expect(bad.died is not None, "clone + a DSS_COMMIT that is not the cloned HEAD must be refused")
    has(bad.died, "The run would have validated the checkout", "the refusal")


# =======================================================================================
# LX -- the loadext helper
# =======================================================================================

RUNDIR_PLAN = {"runFilesystem": "driver", "launcherPath": "", "launcher": [], "rmTreeArgv": [],
               "mkdirArgv": [], "copyArgv": [], "kernelEntryArgv": []}
NO_CONTROL = "NO CONTROL ON THIS HOST: nothing here targets it"
P0016 = "the dss build FAILED: error[P0016] got quote include not found"
ENV_DETAIL = "DSS_LOADEXT_HELPER=reference was requested, and no candidate"
FATAL_ERR = "harness_legs.py: FATAL: no leg labelled 'nope'\n"


def helper_report(detail="a detail line", cross="", staged="/staged/testloadext.dll", klass=""):
    return json.dumps({"verdictClass": klass, "detail": detail, "crossCheck": cross,
                       "staged": staged}) + "\n"


def lx_run(answers, resolver=None, **env):
    run = new_run(**env)
    run.out_dir = tmpdir("lx", "out")
    run.stage = {"src": tmpdir("lx", "sqlite-src"), "bld": tmpdir("lx", "bld")}
    run.compiler = types.SimpleNamespace(path="/nonexistent/dsscp")
    run.loadext_builder = "dss"
    ledger(run)
    run.resolver = resolver or FakeResolver(answers)
    return run


def lx_leg(shape):
    if shape == "pe":
        leg = pin_leg("pe-shaped")
        leg.spec = "x86_64:pe64-x86_64-windows-exec"
        leg.cc, leg.cc_machine = ["mingw-gcc"], "x86_64-w64-mingw32"
        leg.build = {"loadExtHelperName": "testloadext.dll"}
    else:
        leg = pin_leg("posix-shaped")
        leg.cc, leg.cc_machine = [], ""
        leg.build = {"loadExtHelperName": "libtestloadext.so"}
    leg.fixture = "/built/%s/testfixture" % leg.label
    return leg


Staged = collections.namedtuple("Staged", ["run", "leg", "rundir", "res", "argv"])


def stage(shape, rc, out_, err="", resolver=None, leg=None, **env):
    run = lx_run({"--build-loadext-helper": R(rc, out_, err)}, resolver=resolver, **env)
    leg = leg or lx_leg(shape)
    rundir = os.path.join(run.out_dir, leg.label, "run")
    res = X.U.stage_loadext(run, leg, rundir)
    calls = getattr(run.resolver, "calls", [])
    return Staged(run, leg, rundir, res, calls[-1] if calls else [])


def lx_pe():
    return fixture("lx_pe", lambda: stage("pe", 0, helper_report(detail="testloadext.dll, built by dss")))


def lx_posix():
    return fixture("lx_posix", lambda: stage("posix", 0, helper_report(cross=NO_CONTROL)))


@arm("LX01", "it asks for THIS leg: `--build-loadext-helper <label>`, adjacent", "S4.01")
def _lx01():
    s = lx_pe()
    eq(s.argv[:2], ["--build-loadext-helper", "pe-shaped"], "the verb and the leg's label")


@arm("LX02", "...passing the resolved builder", "S4.02")
def _lx02():
    adjacent(lx_pe().argv, "--helper-builder", "dss", "the builder")


@arm("LX03", "...the DSS binary", "S4.03")
def _lx03():
    adjacent(lx_pe().argv, "--dss", "/nonexistent/dsscp", "the compiler")


@arm("LX04", "...the sqlite src root (for sqlite3ext.h)", "S4.04")
def _lx04():
    s = lx_pe()
    adjacent(s.argv, "--sqlite-src", s.run.stage["src"], "the src root")


@arm("LX05", "...the BUILD root (for the generated sqlite3.h)", "S4.05")
def _lx05():
    s = lx_pe()
    adjacent(s.argv, "--sqlite-bld", s.run.stage["bld"], "the build root")


@arm("LX06", "...the run's testdir as the destination (created)", "S4.06 P3.04")
def _lx06():
    s = lx_pe()
    dest = os.path.join(s.rundir, "testdir")
    adjacent(s.argv, "--dest-dir", dest, "the destination")
    expect(os.path.isdir(dest), "the destination testdir exists before the helper is asked for")


@arm("LX07", "...and the driver's own build config (DSS_CONFIG)", "S4.07")
def _lx07():
    adjacent(lx_pe().argv, "--dss-config", "release", "the default config")
    s = stage("pe", 0, helper_report(), DSS_CONFIG="debug")
    adjacent(s.argv, "--dss-config", "debug", "DSS_CONFIG=debug")


@arm("LX08", "the VERIFIED control compiler is passed through", "S4.08 P3.03")
def _lx08():
    adjacent(lx_pe().argv, "--reference-cc", "mingw-gcc", "the control compiler")
    leg = lx_leg("pe")
    leg.cc = ["clang", "-arch", "x86_64"]
    s = stage("pe", 0, helper_report(), leg=leg)
    adjacent(s.argv, "--reference-cc", "clang\t-arch\tx86_64", "a multi-token control is ONE TAB-joined argument")


@arm("LX09", "...with the triple it reported", "S4.09")
def _lx09():
    adjacent(lx_pe().argv, "--reference-machine", "x86_64-w64-mingw32", "the control's triple")


@arm("LX10", "rc 0 is a staged helper", "S4.10 P3.01")
def _lx10():
    eq(lx_pe().res[0], 0, "code")


@arm("LX11", "...and it carries the staged path", "P3.02")
def _lx11():
    eq(lx_pe().res[3], "/staged/testloadext.dll", "staged")


@arm("LX12", "an ABSENT control does not stop the leg", "S4.11")
def _lx12():
    eq(lx_posix().res[0], 0, "code with no control compiler")


@arm("LX13", "...and the cross-check line says so out loud", "S4.12")
def _lx13():
    s = lx_posix()
    eq(s.res[2], NO_CONTROL, "the cross-check returned")
    has(log_text(s.run.log), NO_CONTROL, "the cross-check logged")


@arm("LX14", "an EMPTY control is passed as an EMPTY ARGUMENT, not omitted", "S4.13")
def _lx14():
    adjacent(lx_posix().argv, "--reference-cc", "", "the empty control")
    adjacent(lx_posix().argv, "--reference-machine", "", "the empty triple")


def lx_rc3():
    return fixture("lx_rc3", lambda: stage("pe", 3, helper_report(detail=P0016, klass="poisoned")))


def lx_rc4():
    return fixture("lx_rc4", lambda: stage("posix", 4, helper_report(
        detail=ENV_DETAIL, klass="skipped-build-input-missing")))


def lx_fatal():
    return fixture("lx_fatal", lambda: stage("pe", 2, "", FATAL_ERR))


@arm("LX15", "rc 3 is POISONED (returned, never raised)", "S4.14 P3.05")
def _lx15():
    eq(lx_rc3().res[0], 1, "code")


@arm("LX16", "...carrying the compiler's own diagnostic", "S4.15 P3.06")
def _lx16():
    has(lx_rc3().res[1], "P0016", "why")


@arm("LX17", "rc 4 is ENVIRONMENTAL, not a failure", "S4.16 P3.07")
def _lx17():
    eq(lx_rc4().res[0], 2, "code")


@arm("LX18", "...and says the DEFAULT would have worked", "S4.17")
def _lx18():
    has(lx_rc4().res[1], "DSS_LOADEXT_HELPER=reference", "why")


@arm("LX19", "an UNRECOGNISED rc with a valid report is poisoned", "P3.08")
def _lx19():
    s = stage("pe", 9, helper_report(klass="something-new", detail="an unknown class"))
    eq(s.res[0], 1, "code")
    has(s.res[1], "exited 9", "why names the rc")


@arm("LX20", "UNREADABLE output is poisoned, never a quiet success", "S4.18 P3.09")
def _lx20():
    eq(lx_fatal().res[0], 1, "code")


@arm("LX21", "...and the refusal QUOTES what it could not read", "S4.19 P3.09")
def _lx21():
    has(lx_fatal().res[1], "FATAL", "why")


@arm("LX22", "rc 0 with a NON-JSON report is poisoned (the .sh called it staged)")
def _lx22():
    s = stage("pe", 0, "staged, probably\n")
    eq((s.res[0], s.res[3]), (1, ""), "(code, staged)")


STUB_RESOLVER_PY = r'''
import json, sys
sys.stderr.write("harness_legs.py: note: a diagnostic on stderr that is NOT the report\n")
sys.stderr.flush()
sys.stdout.write(json.dumps({"verdictClass": "", "detail": "staged by the stub", "crossCheck": "",
                             "staged": "/staged/testloadext.dll"}) + "\n")
sys.exit(0)
'''


@arm("LX23", "stderr noise does not poison a valid stdout report (the REAL Resolver transport)")
def _lx23():
    stub = write(os.path.join(tmpdir("lx", "stub"), "stub_resolver.py"), STUB_RESOLVER_PY)
    real = X.C.Resolver(X.C.host_os(), X.C.host_arch(), catalogue=os.devnull, script=stub)
    s = stage("pe", 0, "", resolver=Recording(real))
    eq((s.res[0], s.res[3]), (0, "/staged/testloadext.dll"), "(code, staged) through the real transport")
    merged = X.C.capture([sys.executable, stub], env_=X.C.child_env(python=True), timeout=120, merge=True)
    try:
        json.loads(merged.out)
        parsed = True
    except ValueError:
        parsed = False
    eq(parsed, False, "NEGATIVE: the SAME output with the streams merged is not a report")


@arm("LX24", "staging NEVER raises: every outcome is returned as (code, why, cross, staged)", "S4.20")
def _lx24():
    table = ((0, helper_report(), "", 0), (0, "not json\n", "", 1), (0, "[1, 2]\n", "", 1),
             (0, "", "", 1), (3, helper_report(klass="poisoned"), "", 1),
             (4, helper_report(klass="skipped-build-input-missing"), "", 2),
             (4, "not json\n", "", 1), (9, helper_report(), "", 1), (2, "", FATAL_ERR, 1),
             (127, "", "cannot start harness_legs.py", 1), (124, "", "timed out after 900s", 1))
    for rc, out_, err, want in table:
        s = stage("pe", rc, out_, err)
        expect(isinstance(s.res, tuple) and len(s.res) == 4, "a 4-tuple for rc %d: %r" % (rc, s.res))
        eq(s.res[0], want, "the code for rc %d / output %r" % (rc, out_[:20]))
    run = lx_run({"--build-loadext-helper": R(0, helper_report())})
    blocker = write(os.path.join(run.out_dir, "a-file-not-a-dir"), b"x")
    res = X.U.stage_loadext(run, lx_leg("pe"), blocker)
    eq(res[0], 1, "a run directory that cannot be created is poisoned, returned")
    has(res[1], "could not create", "why")


@arm("LX25", "the verdict step NEVER raises (codes 0/1/2/other)", "S4.21 P4.05")
def _lx25():
    for code, want in ((0, True), (1, False), (2, False), (7, False), (-3, False)):
        run = lx_run({})
        got = X.U.loadext_verdict(run, lx_leg("pe"), code, "why %d" % code)
        eq(got, want, "loadext_verdict(code=%d)" % code)


def verdict_after(code, leg=None):
    run = lx_run({})
    leg = leg or lx_leg("pe")
    before = run.counts["staging"]
    ok = X.U.loadext_verdict(run, leg, code, "the reason for code %d" % code)
    return types.SimpleNamespace(run=run, leg=leg, ok=ok, counted=run.counts["staging"] - before)


@arm("LX26", "poisoned is RECORDED (leg verdict + its units not run)", "S4.22 P4.01")
def _lx26():
    v = verdict_after(1)
    eq(v.leg.verdict, "poisoned", "the leg verdict")
    has(v.leg.verdict_detail, "testloadext.dll", "the detail names the declared helper")
    expect(v.leg.unit_verdict.startswith("not run [poisoned]"), "unit_verdict = %r" % v.leg.unit_verdict)


def lx_step8():
    """`sqlite_units.step8` over TWO runnable legs: A's helper build fails (rc 3), B's control
    arm is absent (rc 4). Nothing here spawns a fixture: both legs stop at their helper."""
    def build():
        run = new_run()
        run.out_dir = tmpdir("s8", "out")
        testdir = tmpdir("s8", "test")
        write(os.path.join(testdir, "veryquick.test"), "run_test_suite veryquick\n")
        run.stage = {"testdir": testdir, "src": tmpdir("s8", "src"), "bld": tmpdir("s8", "bld")}
        run.stage_build = {}
        run.clone_lock = None
        run.compiler = types.SimpleNamespace(path="/nonexistent/dsscp")
        run.loadext_builder = "dss"
        ledger(run)
        a, b = plan_leg("elf64-x86_64"), plan_leg("elf64-arm64")
        for leg in (a, b):
            leg.tcl_lib, leg.z_lib = "/built/libtcl8.6.so", "/built/libz.so"
            leg.fixture, leg.fixture_built = "/built/%s/testfixture" % leg.label, True
        run.legs = [a, b]

        def helper(args):
            if args[1] == a.label:
                return R(3, helper_report(detail=P0016, klass="poisoned"))
            return R(4, helper_report(detail=ENV_DETAIL, klass="skipped-build-input-missing"))
        run.resolver = FakeResolver({"--run-dir-plan": R(0, json.dumps(RUNDIR_PLAN)),
                                     "--build-loadext-helper": helper})
        X.U.step8(run)
        return types.SimpleNamespace(run=run, a=a, b=b)
    return fixture("lx_step8", build)


@arm("LX27", "the leg loop CONTINUES past a poisoned leg (step8, two legs)", "S4.23 P4.03")
def _lx27():
    s = lx_step8()
    eq([c[1] for c in s.run.resolver.asked("--build-loadext-helper")], [s.a.label, s.b.label],
       "both legs asked for their helper, in order")
    eq(s.run.resolver.asked("--corroborate-run-dir"), [], "neither leg's corpus started")


@arm("LX28", "poisoned is COUNTED toward the run's red", "S4.24 P4.04")
def _lx28():
    s = lx_step8()
    eq(s.a.verdict, "poisoned", "leg A's verdict")
    eq(s.run.counts["staging"], 1, "staging failures after step8 (A only)")
    eq(verdict_after(1).counted, 1, "one poisoned leg counts exactly once")


@arm("LX29", "environmental -> skipped-build-input-missing, KEEPING the displaced run verdict",
     "S4.25 P4.02")
def _lx29():
    eq(lx_step8().b.verdict, "skipped-build-input-missing", "leg B's verdict in the step8 drive")
    leg = lx_leg("posix")
    leg.run = dict(leg.run, verdict="skipped-emulator-missing")
    v = verdict_after(2, leg)
    eq(v.leg.verdict, "skipped-build-input-missing", "the leg verdict")
    has(v.leg.verdict_detail, "[and this host could not RUN it either: skipped-emulator-missing]",
        "the displaced run verdict is kept in the detail")
    expect(v.leg.unit_verdict.startswith("not run [skipped-build-input-missing]"),
           "unit_verdict = %r" % v.leg.unit_verdict)


@arm("LX30", "environmental is NOT counted as a stage failure", "S4.26")
def _lx30():
    eq(verdict_after(2).counted, 0, "code 2 adds nothing to the staging count")
    s = lx_step8()
    eq((s.b.verdict, s.run.counts["staging"]), ("skipped-build-input-missing", 1),
       "step8: B environmental, and the count is A's alone")


def green_run(staging=0):
    """A Step-9 state in which everything is GREEN except, optionally, `staging` failures."""
    run = new_run()
    run.out_dir = tmpdir("s9", "out")
    d = tmpdir("s9", "recipe")
    tus = write(os.path.join(d, "tus.txt"), "a.c\nb.c\n")
    defs = write(os.path.join(d, "defines.txt"), "-DX=1\n")
    run.stage = {"sqlite_head": "0123456789", "fixture_recipe": {"tus": tus, "defines": defs},
                 "cli_recipe": {"tus": tus, "defines": defs}, "reference_cli": "",
                 "reference_cli_why": "no reference in this fixture", "reference_fixture": "",
                 "reference_fixture_why": "no reference in this fixture"}
    run.compiler = types.SimpleNamespace(path="/built/dsscp", build_type_note="", built="now",
                                         origin="the self-test fixture")
    run.provenance = {"head_short": "abc1234", "diverge_note": ""}
    run.currency_note = "proved by the self-test fixture"
    run.sqlite_dir_posix = "/src/sqlite"
    ledger(run)
    leg = plan_leg("elf64-x86_64")
    leg.fixture_built, leg.cli_bin = True, "/built/sqlite3"
    leg.smoke_verdict, leg.unit_verdict, leg.unit_report = "PASS (14/14)", "PASS (0 errors out of 9 tests)", {}
    run.legs = [leg]
    run.ledger.set_leg(leg, "ran", leg.unit_verdict)
    run.artifacts = X.B.VerdictLedger()
    run.artifacts.set(leg.label, "sqlite3", "built", "sqlite3 -> /built/sqlite3")
    run.resolver = FakeResolver({"--oracle-report": R(0, "[elf64-x86_64] ORACLE: self-test\n"),
                                 "--verdict-classes": R(0, classes_text())})
    run.counts["staging"] = staging
    return run


@arm("LX31", "Step 9 exits NONZERO when stage failures > 0 (and 0 when not)", "S4.28")
def _lx31():
    green = green_run(0)
    eq(X.REP.step9(green), 0, "control: the same run with no staging failure exits 0")
    red = green_run(1)
    eq(X.REP.step9(red), 1, "one staging failure")
    expect([w for w in warnings(red.log)
            if "1 leg(s) BUILT their testfixture but could not stage the loadext helper" in w],
           "the reason names the staging count; warnings: %r" % warnings(red.log)[-6:])


@arm("LX32", "a failed copy into the launcher's filesystem is poisoned and counted")
def _lx32():
    def carried(copy_argv, launcher_path="/launcher/run"):
        run = lx_run({})
        leg = lx_leg("posix")
        before = run.counts["staging"]
        plan_ = dict(RUNDIR_PLAN, launcherPath=launcher_path, copyArgv=copy_argv)
        ok = X.U.carry_helper(run, leg, plan_, "/staged/libtestloadext.so")
        return ok, run.counts["staging"] - before, leg
    ok, counted, leg = carried([sys.executable, "-c", "import sys; sys.exit(3)"])
    eq((ok, counted, leg.verdict), (False, 1, "poisoned"), "(may run, counted, verdict) on a failed copy")
    expect(leg.unit_verdict.startswith("not run [poisoned]") and "transfer FAILED" in leg.unit_verdict,
           "unit_verdict = %r" % leg.unit_verdict)
    ok, counted, leg = carried([sys.executable, "-c", "import sys; sys.exit(0)"])
    eq((ok, counted, leg.verdict), (True, 0, ""), "control: a copy that succeeds")
    ok, counted, _leg = carried([sys.executable, "-c", "import sys; sys.exit(3)"], launcher_path="")
    eq((ok, counted), (True, 0), "no launcher filesystem -> nothing to carry, nothing spawned")


# =======================================================================================
# CF -- the per-target sqlite_cfg.h
# =======================================================================================

CFG_MARKER = "/* test_confound_scope fixture: the DERIVING host's sqlite_cfg.h */"


def cf_stage(good):
    """`sqlite_build.stage_headers` running the REAL stage-zinc.py against the SHIPPED catalogue,
    over a zconf.h/sqlite_cfg.h synthesised from the stages the resolver declares."""
    base = tmpdir("cf", "good" if good else "bad")
    zinc_src = os.path.join(base, "zlib-src")
    guards = sorted({g for names in plan_stages("header").values() for g in names})
    write(os.path.join(zinc_src, "zlib.h"), "/* zlib.h fixture */\n#include \"zconf.h\"\n")
    write(os.path.join(zinc_src, "zconf.h"), "/* zconf.h fixture */\n" + "".join(
        "#if 1    /* was set to #if 1 by ./configure */\n#  define %s\n#endif\n" % g for g in guards))
    answers = sorted({a for names in plan_stages("config").values() for a in names})
    body = CFG_MARKER + "\n" + ("".join("#define %s 1\n" % a for a in answers) if good
                                else "/* this header carries NONE of the declared answers */\n")
    cfg_h = write(os.path.join(base, "bld", "sqlite_cfg.h"), body)
    run = new_run()
    run.stage = {"zinc_src": zinc_src, "sqlite_cfg_h": cfg_h}
    existed = os.path.isfile(cfg_h)
    died = None
    try:
        X.BLD.stage_headers(run)
    except X.C.HarnessDie as exc:
        died = str(exc)
    return types.SimpleNamespace(run=run, base=base, cfg_h=cfg_h, existed=existed,
                                 exists_after=os.path.isfile(cfg_h), died=died)


def cf_ok():
    s = fixture("cf_ok", lambda: cf_stage(True))
    if s.died:
        raise FixtureError("stage_headers refused a well-formed header: %s" % _short(s.died))
    return s


def cf_lists():
    def build():
        ok = cf_ok()
        d = tmpdir("cf", "lists")
        run = new_run()
        run.host = "linux"   # the list's HEAD is host-free; a darwin host only appends its SDK LAST
        run.out_dir = os.path.join(d, "out")
        os.makedirs(run.out_dir)
        run.stage = {"fixture_recipe": {"includes": write(os.path.join(d, "fx-inc.txt"),
                                                          "/recipe/inc-a\n/recipe/inc-b\n")},
                     "cli_recipe": {"includes": write(os.path.join(d, "cli-inc.txt"), "/recipe/cli-inc\n")},
                     "tcl_inc": "/staged/tcl/include", "bld": "/staged/bld"}
        orphan = pin_leg("no-staged-config")
        orphan.build = {"headerStageKey": "none", "configStageKey": "plan9"}
        run.legs = plan_legs() + [orphan]
        run.zinc_stage_dirs = dict(ok.run.zinc_stage_dirs)
        run.cfg_stage_dirs = dict(ok.run.cfg_stage_dirs)
        X.BLD.write_include_lists(run)
        return types.SimpleNamespace(run=run, orphan=orphan)
    return fixture("cf_lists", build)


def list_lines(path):
    return [ln for ln in read(path).splitlines() if ln.strip()]


@arm("CF01", "stage-zinc.py is asked for a per-target sqlite_cfg.h (every declared target OS)",
     "S5.01 P5.01")
def _cf01():
    ok = cf_ok()
    eq(sorted(ok.run.cfg_stage_dirs), sorted(plan_stages("config")),
       "one staged sqlite_cfg.h per target OS the catalogue declares")
    for key, d in sorted(ok.run.cfg_stage_dirs.items()):
        has(read(os.path.join(d, "sqlite_cfg.h")), CFG_MARKER,
            "the %s copy was derived from the header the driver named" % key)


@arm("CF02", "...writing it into ITS OWN cfg/ root", "S5.02 P5.02")
def _cf02():
    ok = cf_ok()
    root = os.path.join(ok.base, "cfg")
    for key, d in sorted(ok.run.cfg_stage_dirs.items()):
        expect(samepath(os.path.dirname(d), root) and os.path.basename(d) == key,
               "the %s stage %r must be <stage>/cfg/%s" % (key, d, key))


@arm("CF03", "...and REFUSES a run in which none was produced (the deriving copy kept)", "S5.03 P5.03")
def _cf03():
    bad = fixture("cf_bad", lambda: cf_stage(False))
    expect(bad.died is not None, "a header without the declared answers must be refused")
    has(bad.died, "produced NO per-target sqlite_cfg.h", "the refusal")
    eq((bad.exists_after, bad.run.cfg_stage_dirs), (True, {}),
       "on refusal nothing is recorded and the deriving host's copy is not removed")


@arm("CF04", "the cfg dir is FIRST on every leg's FIXTURE include list", "S5.04 P5.04")
def _cf04():
    s = cf_lists()
    for leg in s.run.legs[:-1]:
        lines = list_lines(leg.inc_file)
        eq(lines[:3], [s.run.cfg_stage_dirs[leg.build["configStageKey"]], "/recipe/inc-a", "/recipe/inc-b"],
           "%s: the head of the fixture include list" % leg.label)
        expect(lines.index("/staged/bld") > 0, "%s: the build dir comes AFTER the cfg dir" % leg.label)
    eq(s.orphan.inc_file, "", "a leg whose config stage was not produced gets NO list (no fallback)")


@arm("CF05", "the cfg dir is FIRST on every leg's CLI include list", "S5.05 P5.04")
def _cf05():
    s = cf_lists()
    for leg in s.run.legs[:-1]:
        lines = list_lines(leg.cli_inc_file)
        eq(lines[:2], [s.run.cfg_stage_dirs[leg.build["configStageKey"]], "/recipe/cli-inc"],
           "%s: the head of the CLI include list" % leg.label)
    eq(s.orphan.cli_inc_file, "", "no CLI list without its own config stage")


@arm("CF06", "the DERIVING host's copy is REMOVED (it existed before the call)", "S5.06 P5.05")
def _cf06():
    ok = cf_ok()
    eq((ok.existed, ok.exists_after), (True, False), "(existed before, exists after)")


# =======================================================================================
# LA -- the launcher argv form
# =======================================================================================

def la_leg():
    """The SHIPPED macho64-x86_64 leg as a darwin/arm64 host plans it (launcher `arch -x86_64`)."""
    leg = plan_leg("macho64-x86_64", "darwin", "arm64", "arch")
    leg.cli_bin = os.path.join(fixture("la_dir", lambda: tmpdir("la")), "sqlite3")
    return leg


def la_argv(leg, cli_target, ref="", ref_target="", ref_launch=()):
    d = fixture("la_dir", lambda: tmpdir("la"))
    return X.SMK.smoke_argv(FakeResolver({}), leg, cli_target, "3.99.0", "2099-01-01 selftest", d,
                            os.path.join(d, "result.json"), ref, ref_target, list(ref_launch))


def smoke_run(args):
    return X.C.capture([sys.executable, X.C.CLI_SMOKE] + list(args),
                       env_=X.C.child_env(python=True), timeout=180, merge=True)


@arm("LA01", "`--launcher=<tok>` for the DASH-leading token the SHIPPED plan declares", "S6.01 P6.01")
def _la01():
    leg = la_leg()
    dash = [t for t in leg.launcher if t.startswith("-")]
    expect(dash, "the shipped darwin/arm64 macho64-x86_64 launcher must carry a DASH-leading token, "
           "or this arm proves nothing: %r" % (leg.launcher,))
    argv = la_argv(leg, "x86_64:macho64:darwin")
    eq([a for a in argv if a.startswith("--launcher")], ["--launcher=%s" % t for t in leg.launcher],
       "every launcher token, in order, in the `=` form")
    letters = [t for t in leg.launcher if not t.startswith("-")]
    r = smoke_run([x for t in letters for x in ("--launcher", t)])
    has(r.out, "the following arguments are required: --cli",
        "NEGATIVE by removal: with its dash token removed the shipped launcher is accepted in the "
        "SPACE form too -- so only the declared dash token makes this arm discriminate")


@arm("LA02", "`--reference-launcher=<tok>` too (the sibling that actually broke)", "P6.02")
def _la02():
    argv = la_argv(la_leg(), "x86_64:macho64:darwin", "/ref/sqlite3", "x86_64:elf64:linux", ["wsl.exe", "-e"])
    eq([a for a in argv if a.startswith("--reference-launcher")],
       ["--reference-launcher=wsl.exe", "--reference-launcher=-e"], "the reference launcher tokens")
    adjacent(argv, "--reference", "/ref/sqlite3", "the reference")
    none = la_argv(la_leg(), "x86_64:macho64:darwin")
    eq([a for a in none if a.startswith("--reference")], [], "no reference -> no reference arguments")


@arm("LA03", "no bare option element is followed by a separate token (the argv list)", "S6.02 P6.03")
def _la03():
    argv = la_argv(la_leg(), "x86_64:macho64:darwin", "/ref/sqlite3", "x86_64:elf64:linux", ["wsl.exe", "-e"])
    eq([a for a in argv if a in ("--launcher", "--reference-launcher")], [],
       "bare `--launcher` / `--reference-launcher` elements")


@arm("LA04", "the REAL cli-smoke.py REFUSES the space form with the shipped tokens", "S6.03 P6.05")
def _la04():
    r = smoke_run([x for t in la_leg().launcher for x in ("--launcher", t)])
    has(r.out, "argument --launcher: expected one argument", "argparse's refusal")


@arm("LA05", "...while the `=` form (and the driver's WHOLE argv) gets past token parsing",
     "S6.04 P6.06")
def _la05():
    leg = la_leg()
    r = smoke_run(["--launcher=%s" % t for t in leg.launcher])
    has(r.out, "the following arguments are required: --cli", "the `=` form reaches the required-arg check")
    argv = la_argv(leg, "arm64:macho64:darwin")   # a WRONG target: the gate stops before running anything
    r = X.C.capture(argv, env_=X.C.child_env(python=True), timeout=180, merge=True)
    eq(r.rc, 4, "the driver's argv reaches the gate's wrong-target precondition (rc 4, not argv rc 2)")
    has(r.out, "subject        : %s %s" % (" ".join(leg.launcher), leg.cli_bin),
        "the gate consumed every launcher token as a launcher token")


# =======================================================================================
# SM -- the CLI smoke gate's measured targets and rc table
# =======================================================================================

REF_TRIPLE = "x86_64:elf64:linux"
WRONG_TRIPLE = "arm64:elf64:linux"


def sm_files():
    def build():
        d = tmpdir("sm")
        bld = os.path.join(d, "bld")
        write(os.path.join(bld, "sqlite3.h"), '#define SQLITE_VERSION        "3.99.0"\n'
              '#define SQLITE_SOURCE_ID      "2099-01-01 00:00:00 selftest"\n')
        return types.SimpleNamespace(d=d, bld=bld, ref=write(os.path.join(d, "ref-sqlite3"), b"ref"),
                                     cli=write(os.path.join(d, "leg-sqlite3"), b"cli"),
                                     fixture_ref=write(os.path.join(d, "ref-testfixture"), b"fx"))
    return fixture("sm_files", build)


def identify_answers(table):
    def answer(args):
        for path, triple in table.items():
            if samepath(args[1], path):
                return R(0, triple.replace(":", "\t") + "\n") if triple else R(3, "", "unknown bytes")
        return R(3, "", "no such binary in the fixture")
    return answer


def ref_cli(launcher_answer, identify=None, host="linux", needs_wsl=False):
    f = sm_files()
    run = new_run()
    run.host, run.posix = host, X.C.PosixSide("windows" if needs_wsl else "linux")
    run.stage = {"reference_cli": f.ref, "reference_cli_posix": "", "reference_cli_why": ""}
    run.resolver = FakeResolver({"--identify-binary": identify or identify_answers({f.ref: REF_TRIPLE}),
                                 "--launcher-for-target": launcher_answer})
    return X.SMK.reference_cli(run), run


def sm_step7c():
    """The REAL Step 7c over one native leg, the REAL cli-smoke.py: the leg's CLI measures as a
    WRONG target on purpose, so the gate stops at its first precondition and executes nothing."""
    def build():
        f = sm_files()
        run = new_run()
        run.out_dir = tmpdir("sm", "out")
        run.stage = {"bld": f.bld, "reference_cli": f.ref, "reference_cli_posix": "", "reference_cli_why": ""}
        leg = plan_leg("elf64-x86_64")
        leg.cli_bin = f.cli
        run.legs = [leg]
        run.artifacts = X.B.VerdictLedger()
        run.resolver = FakeResolver({"--identify-binary": identify_answers({f.ref: REF_TRIPLE, f.cli: WRONG_TRIPLE}),
                                     "--launcher-for-target": R(0, "", "runs natively on this host")})
        X.SMK.step7c(run)
        result = os.path.join(run.leg_out(leg), "cli-smoke", "result.json")
        return types.SimpleNamespace(run=run, leg=leg, result=json.loads(read(result)))
    return fixture("sm_step7c", build)


@arm("SM01", "the REFERENCE binary is identified by measurement", "S8.38 S8.40 P7.45 P7.47")
def _sm01():
    (path, triple, _launch), run = ref_cli(R(0, "", "native"))
    eq([c[1] for c in run.resolver.asked("--identify-binary")], [sm_files().ref], "identified: the reference")
    eq((path, triple), (sm_files().ref, REF_TRIPLE), "the reference and its MEASURED triple")
    (path, triple, launch), run = ref_cli(R(0, "", "native"), identify=identify_answers({sm_files().ref: ""}))
    eq((path, triple, launch), ("", "", []), "NEGATIVE: an unidentifiable reference is DROPPED, never guessed")


@arm("SM02", "the leg's CLI is identified by measurement (and that triple reaches the gate)",
     "S8.42 P7.49")
def _sm02():
    s = sm_step7c()
    has([c[1] for c in s.run.resolver.asked("--identify-binary")], s.leg.cli_bin, "the leg's CLI was identified")
    eq(s.result.get("cliTarget"), WRONG_TRIPLE, "the gate received the MEASURED triple as --cli-target")


@arm("SM03", "TAB fields joined with `:`; an EMPTY answer is refused, never fabricated", "S8.52 P7.59")
def _sm03():
    fake = FakeResolver({"--identify-binary": R(0, "arm64\telf64\tlinux\n")})
    eq(X.SMK.identify_binary(fake, "/x/sqlite3"), ("arm64:elf64:linux", ""), "the joined triple")
    fake = FakeResolver({"--identify-binary": R(0, "\n")})
    t, why = X.SMK.identify_binary(fake, "/x/sqlite3")
    eq(t, "", "no triple from an empty answer")
    has(why, "printed NOTHING", "the refusal")
    fake = FakeResolver({"--identify-binary": R(3, "", "not an executable I know")})
    t, why = X.SMK.identify_binary(fake, "/x/sqlite3")
    eq(t, "", "no triple from rc 3")
    has(why, "could not identify", "the refusal")


@arm("SM04", "`--leg-spec` carries the leg's DECLARED spec", "S8.44 P7.51")
def _sm04():
    leg = la_leg()
    adjacent(la_argv(leg, WRONG_TRIPLE), "--leg-spec", leg.spec, "the declared spec")
    s = sm_step7c()
    eq(s.result.get("legSpec"), s.leg.spec, "the gate received the declared spec")


@arm("SM05", "`--cli-target` carries the MEASURED triple, not the declaration", "S8.46 P7.53")
def _sm05():
    leg = la_leg()
    argv = la_argv(leg, WRONG_TRIPLE)
    adjacent(argv, "--cli-target", WRONG_TRIPLE, "the measured triple")
    expect(after(argv, "--cli-target") != leg.spec, "the declaration is not what is passed")


@arm("SM06", "`--reference-target` carries the reference's MEASURED triple", "S8.48 P7.55")
def _sm06():
    adjacent(la_argv(la_leg(), WRONG_TRIPLE, "/ref/sqlite3", REF_TRIPLE), "--reference-target", REF_TRIPLE,
             "the reference triple")
    eq(sm_step7c().result.get("referenceTarget"), REF_TRIPLE, "the gate received the measured reference triple")


@arm("SM07", "the reference's launcher comes from --launcher-for-target on the MEASURED triple",
     "S8.50 P7.57")
def _sm07():
    (path, triple, launch), run = ref_cli(R(0, "arch -x86_64\n", "declared for that target here"))
    calls = run.resolver.asked("--launcher-for-target")
    eq(calls, [["--launcher-for-target", REF_TRIPLE] + run.resolver.host_args], "the request")
    eq(launch, ["arch", "-x86_64"], "the launcher the catalogue declared")
    (path, triple, launch), _run = ref_cli(R(3, "", "this host cannot run that target"))
    eq((path, triple, launch), ("", "", []), "NEGATIVE: a reference this host cannot run is DROPPED")


@arm("SM08", "the oracle report gets the MEASURED reference triple (set and passed)",
     "S8.54 S8.55 P7.61 P7.62")
def _sm08():
    f = sm_files()

    def lines_for(ref, identify, report=None):
        run = new_run()
        run.stage = {"reference_fixture": ref, "reference_fixture_why": "none in this fixture"}
        run.legs = [plan_leg("elf64-x86_64"), plan_leg("elf64-arm64")]
        run.resolver = FakeResolver({"--identify-binary": identify,
                                     "--oracle-report": report or R(0, "ORACLE: self-test\n")})
        reasons = []
        X.REP.oracle_lines(run, reasons)
        return run, reasons
    run, reasons = lines_for(f.fixture_ref, identify_answers({f.fixture_ref: REF_TRIPLE}))
    calls = run.resolver.asked("--oracle-report")
    eq([c[1] for c in calls], ["elf64-x86_64", "elf64-arm64"], "one report per leg")
    for c in calls:
        adjacent(c, "--reference-target", REF_TRIPLE, "%s: the MEASURED triple" % c[1])
        adjacent(c, "--reference-path", f.fixture_ref, "%s: the reference" % c[1])
    eq(reasons, [], "no failure reason")
    run, _ = lines_for(os.path.join(f.d, "absent"), identify_answers({}))
    for c in run.resolver.asked("--oracle-report"):
        eq((after(c, "--reference-target"), after(c, "--reference-path")), ("", ""),
           "NEGATIVE: no reference -> no target, never a guessed one")
    run, _ = lines_for(f.fixture_ref, identify_answers({f.fixture_ref: ""}))
    for c in run.resolver.asked("--oracle-report"):
        eq(after(c, "--reference-target"), "", "an unidentifiable reference passes NO target")
    _run, reasons = lines_for(f.fixture_ref, identify_answers({f.fixture_ref: REF_TRIPLE}),
                              report=lambda a: R(2, "", "bad argv") if a[1] == "elf64-arm64" else R(0, "ok\n"))
    expect(len(reasons) == 1 and "elf64-arm64" in reasons[0], "a failed report is a reason: %r" % reasons)


@arm("SM09", "the reference launcher does NOT change when only host-identity inputs change", "P7.66")
def _sm09():
    answer = R(0, "wsl.exe -e\n", "declared for that target on this host")
    results = []
    for host, needs_wsl in (("windows", True), ("linux", False), ("darwin", False)):
        (path, triple, launch), run = ref_cli(answer, host=host, needs_wsl=needs_wsl)
        results.append((path, triple, launch))
        for c in run.resolver.asked("--launcher-for-target"):
            eq(c[2:], run.resolver.host_args, "the host facts asked are the RESOLVER's, not the run's")
    eq(len(set(json.dumps(r) for r in results)), 1, "one answer across host identities: %r" % results)
    eq(results[0][2], ["wsl.exe", "-e"], "...the resolver's")
    (_p, _t, launch), _run = ref_cli(R(0, "", "native here"))
    eq(launch, [], "NEGATIVE: a different resolver answer DOES change it (the check is not vacuous)")


@arm("SM10", "rc 4 is NOT A VERDICT (unattributable) -- also from the REAL gate", "S8.60 P7.67")
def _sm10():
    v, w = X.SMK.smoke_rc_verdict(4, "/o/result.json", "/o/smoke.log")
    eq(v, "FAIL %s NOT A VERDICT (unattributable); see /o/result.json" % EM, "the verdict line")
    has(w, "NOT A VERDICT", "the warning")
    s = sm_step7c()
    expect(s.leg.smoke_verdict.startswith("FAIL %s NOT A VERDICT (unattributable)" % EM),
           "step7c + the real gate: %r" % s.leg.smoke_verdict)
    eq(s.run.counts["smoke"], 1, "the red is counted (it is still red)")


@arm("SM11", "rc 2 is a HARNESS ARGV DEFECT pointing at the smoke LOG", "S8.62 P7.69")
def _sm11():
    v, w = X.SMK.smoke_rc_verdict(2, "/o/result.json", "/o/smoke.log")
    eq(v, "FAIL %s HARNESS ARGV DEFECT (the gate rejected its own arguments); see /o/smoke.log" % EM,
       "the verdict line (the gate wrote no result, so the LOG)")
    has(w, "our defect, not the compiler's", "the warning")


@arm("SM12", "rc 1 is CHARGED TO DSS against a MATCHED control", "S8.64 P7.71")
def _sm12():
    v, _w = X.SMK.smoke_rc_verdict(1, "/o/result.json", "/o/smoke.log")
    eq(v, "FAIL %s CHARGED TO DSS (a MATCHED gcc control passes the assertions this leg fails); "
       "see /o/result.json" % EM, "the verdict line")


@arm("SM13", "rc 0 PASS, rc 3 NOT DSS, an unknown rc is a DRIVER defect (never charged)")
def _sm13():
    eq(X.SMK.smoke_rc_verdict(0, "/o/r.json", "/o/s.log"), ("PASS (14/14)", None), "rc 0")
    v, w = X.SMK.smoke_rc_verdict(3, "/o/r.json", "/o/s.log")
    eq(v, "FAIL %s NOT DSS (the gcc reference fails identically); see /o/r.json" % EM, "rc 3")
    has(w, "DSS is NOT implicated", "rc 3's warning")
    v, w = X.SMK.smoke_rc_verdict(7, "/o/r.json", "/o/s.log")
    has(v, "UNKNOWN rc=7", "rc 7")
    has(w, "NOT charged to DSS", "rc 7's warning")


# =======================================================================================
# LP -- the launcher-prerequisite gate;  RF -- the run-fidelity selector
# =======================================================================================

MISSING_REPORT = {"ok": False, "verdict": "skipped-launcher-prerequisite-missing",
                  "missing": [{"kind": "command", "path": "qemu-aarch64", "provides": "the emulator",
                               "why": "declared by the launcher", "install": "apt-get install qemu-user-static"}],
                  "uncovered": []}


def lp_run(answer):
    run = new_run()
    run.legs = plan_legs()
    ledger(run)
    run.resolver = FakeResolver({"--check-launcher": answer})
    X.BT.launcher_prereq_gate(run)
    return run, {lg.label: lg for lg in run.legs}


@arm("LP01", "`--check-launcher` runs for every LAUNCHED leg, and only those", "S8.32 P7.39")
def _lp01():
    run, legs = lp_run(R(0, "{}\n"))
    launched = [lg.label for lg in plan_legs() if lg.run_mode == "launched"]
    expect(launched, "the shipped linux/x86_64 plan launches at least one leg")
    eq(run.resolver.asked("--check-launcher"),
       [["--check-launcher", lab] + run.resolver.host_args for lab in launched], "the requests")
    eq({lab: (lg.run_mode, lg.verdict) for lab, lg in legs.items()},
       {lg.label: (lg.run_mode, lg.verdict) for lg in plan_legs()}, "every prerequisite met -> nothing changes")


@arm("LP02", "rc 3 + a report -> skipped-launcher-prerequisite-missing; unreadable -> poisoned",
     "S8.34 P7.41")
def _lp02():
    def answer(args):
        return R(3, json.dumps(MISSING_REPORT) + "\n") if args[1] == "elf64-arm64" else R(3, "not json\n")
    run, legs = lp_run(answer)
    a = legs["elf64-arm64"]
    eq((a.verdict, a.run_mode, a.run.get("verdict")),
       ("skipped-launcher-prerequisite-missing", "skip", "skipped-launcher-prerequisite-missing"),
       "the unmet leg: its verdict, and its run is now SKIPPED")
    has(log_text(run.log), "MISSING [command] qemu-aarch64", "the missing row is named")
    eq(X.BT.launcher_prereq_rows(MISSING_REPORT)[:2],
       ["MISSING [command] qemu-aarch64", "      provides: the emulator"], "the report's rows")
    eq(legs["pe64-x86_64"].verdict, "poisoned", "an rc-3 report that is NOT JSON is poisoned")
    _run, legs = lp_run(R(5, "", "boom"))
    eq(legs["elf64-arm64"].verdict, "poisoned", "an unrecognised rc is poisoned, never assumed benign")


@arm("LP03", "the check comes BEFORE Step 7b (run_all's order, and its effect downstream)",
     "S8.36 P7.43")
def _lp03():
    names = call_names(X.BT.run_all)
    for anchor in ("BLD.step7", "BLD.step7b"):
        expect("step1" in names and anchor in names and names.index("step1") < names.index(anchor),
               "run_all must call step1 before %s: %r" % (anchor, names))
    has(call_names(X.BT.step1), "launcher_prereq_gate", "step1 runs the launcher-prerequisite gate")
    _run, legs = lp_run(lambda a: R(3, json.dumps(MISSING_REPORT)) if a[1] == "elf64-arm64" else R(0, "{}"))
    eq(X.C.run_is_skipped(legs["elf64-arm64"]), True, "the gated leg is SKIPPED by every later step")


def call_names(fn):
    tree = ast.parse(textwrap.dedent(inspect.getsource(fn)))
    calls = sorted((n for n in ast.walk(tree) if isinstance(n, ast.Call)),
                   key=lambda n: (n.lineno, n.col_offset))
    return [ast.unparse(n.func) for n in calls]


def rf_select(fidelity, legs_filter=None):
    env = {"DSS_RUN_FIDELITY": fidelity}
    if legs_filter:
        env["DSS_LEGS"] = legs_filter
    with env_patch(env):
        run = X.C.Run(X.C.Config(), new_log())
        run.legs = plan_legs()
        ledger(run)
        run.resolver = Recording(Memo(real_resolver()))
        try:
            X.BT.select_legs(run)
            return run, None
        except X.C.HarnessDie as exc:
            return run, str(exc)


@arm("RF01", "DSS_RUN_FIDELITY is honoured (validated against the resolver's vocabulary)",
     "S7.10 P7.35")
def _rf01():
    run, died = rf_select("native")
    eq(died, None, "DSS_RUN_FIDELITY=native")
    eq(run.cfg.fidelity_filter, ["native"], "Config parsed the filter")
    eq(len(run.resolver.asked("--run-fidelities")), 1, "the resolver was asked for its vocabulary")
    eq([lg.label for lg in run.selected()], [lg.label for lg in plan_legs() if lg.fidelity == "native"],
       "only the native-fidelity legs stay selected")
    for lg in run.legs:
        if not lg.selected:
            eq(lg.verdict, "not-selected-by-runner", "%s: the deselected leg's verdict" % lg.label)
            has(lg.verdict_detail, "DSS_RUN_FIDELITY='native'", "%s: the detail names the switch" % lg.label)
    run, died = rf_select("native,emulated")
    eq(sorted(lg.fidelity for lg in run.selected()), ["emulated", "native"], "a two-value filter")


@arm("RF02", "an UNKNOWN value is refused against the resolver's RUN_FIDELITIES", "S7.11 P7.37")
def _rf02():
    _run, died = rf_select("bogus")
    expect(died is not None, "DSS_RUN_FIDELITY=bogus must be refused")
    has(died, "'bogus'", "the refusal names the value")
    known = [ln.strip() for ln in Memo(real_resolver()).call(["--run-fidelities"]).out.splitlines()
             if ln.strip()]
    has(died, "Known: %s" % " ".join(known), "...and the resolver's own vocabulary")
    _run, died = rf_select("NATIVE")
    expect(died is not None, "the vocabulary is case-SENSITIVE: `NATIVE` is refused")
    _run, died = rf_select("emulated", legs_filter="elf64-x86_64")
    expect(died is not None and "selected NO leg" in died, "a filter selecting nothing is refused: %r" % died)


# =======================================================================================
# BA -- build attribution and the oracle status;  RD -- corroboration
# =======================================================================================

def ba_run(answers):
    run = new_run()
    run.out_dir = tmpdir("ba", "out")
    run.resolver = FakeResolver(answers)
    return run


ATTRIBUTION = {"report": ["[pe] attribution: 1 of 2 rejected TU(s) charged to DSS"],
               "chargedToDss": ["/x/src/alpha.c"], "tus": ["/x/src/alpha.c", "/x/src/beta.c"]}


@arm("BA01", "a build failure ASKS whose failure it is (--attribute-build)", "S7.04 P7.15")
def _ba01():
    run = ba_run({"--attribute-build": R(3, json.dumps(ATTRIBUTION) + "\n")})
    leg = plan_leg("elf64-x86_64")
    summary = X.BLD.attribute_build(run, leg, "/o/compile.log", "/o/leg.dss-project.json", run.out_dir)
    eq(summary, "1 of 2 rejected TU(s) charged to DSS: alpha.c", "the ledger summary")
    c = run.resolver.asked("--attribute-build")[0]
    eq(c[:2], ["--attribute-build", leg.label], "the verb and the leg")
    adjacent(c, "--compile-log", "/o/compile.log", "dss's own log")
    adjacent(c, "--manifest", "/o/leg.dss-project.json", "the manifest")
    run = ba_run({"--attribute-build": R(5, "", "cannot attribute")})
    eq(X.BLD.attribute_build(run, leg, "/o/c.log", "/o/m.json", run.out_dir), "",
       "NEGATIVE: an unusable answer excuses nothing")
    has(call_names(X.BLD.step7), "attribute_build", "Step 7 asks it on a failed build")


@arm("BA02", "the oracle's STATUS goes to the attribution, on every outcome", "S7.12 P7.17")
def _ba02():
    for rc, rec, status in ((3, {"status": "build-failed"}, "build-failed"),
                            (4, {"status": "no-reference-compiler"}, "no-reference-compiler"),
                            (0, {"status": "built", "path": "/o/ref", "cc": "gcc", "triple": "x"}, "built")):
        run = ba_run({"--build-reference-oracle": R(rc, json.dumps(rec) + "\n"),
                      "--attribute-build": R(3, json.dumps(ATTRIBUTION) + "\n")})
        leg = plan_leg("elf64-x86_64")
        X.BLD.build_oracle(run, leg, "/o/leg.dss-project.json", run.out_dir)
        eq(leg.oracle.get("status"), status, "rc %d: the recorded oracle status" % rc)
        X.BLD.attribute_build(run, leg, "/o/compile.log", "/o/leg.dss-project.json", run.out_dir)
        c = run.resolver.asked("--attribute-build")[0]
        adjacent(c, "--oracle-status", status, "rc %d: the status handed to the attributor" % rc)
        adjacent(c, "--oracle-log", os.path.join(run.out_dir, "reference-oracle.log"), "the oracle's log")


@arm("BA03", "the oracle's STATUS goes to the oracle REPORT too (per leg)", "S7.13 P7.18")
def _ba03():
    run = new_run()
    run.stage = {"reference_fixture": "", "reference_fixture_why": "none here"}
    a, b = plan_leg("elf64-x86_64"), plan_leg("elf64-arm64")
    a.oracle = {"status": "build-failed", "log": "/o/a.log", "path": "", "cc": "", "triple": ""}
    b.oracle = {"status": "built", "log": "/o/b.log", "path": "/o/b-ref", "cc": "gcc", "triple": "aarch64-linux-gnu"}
    run.legs = [a, b]
    run.resolver = FakeResolver({"--oracle-report": R(0, "ORACLE: self-test\n")})
    X.REP.oracle_lines(run, [])
    calls = {c[1]: c for c in run.resolver.asked("--oracle-report")}
    adjacent(calls[a.label], "--oracle-status", "build-failed", "leg A's status")
    adjacent(calls[b.label], "--oracle-status", "built", "leg B's status")
    adjacent(calls[b.label], "--leg-oracle", "/o/b-ref", "leg B's own oracle")
    adjacent(calls[b.label], "--leg-oracle-triple", "aarch64-linux-gnu", "leg B's oracle triple")


class StopHere(Exception):
    """Raised by the fake resolver to stop a REAL flow at the call under test."""


@arm("RD01", "corroboration is called with THIS leg + THIS run dir; its survivors become the supply",
     "S7.06 S7.07 P7.09 P7.10")
def _rd01():
    run = new_run()
    run.out_dir = tmpdir("rd", "out")
    run.stage = {"src": tmpdir("rd", "src"), "bld": tmpdir("rd", "bld")}
    run.compiler = types.SimpleNamespace(path="/nonexistent/dsscp")
    run.loadext_builder = "dss"
    ledger(run)
    leg = plan_leg("elf64-x86_64")
    leg.tcl_lib, leg.z_lib, leg.fixture, leg.fixture_built = "/b/libtcl.so", "/b/libz.so", "/b/testfixture", True
    run.legs = [leg]
    run.resolver = FakeResolver({"--run-dir-plan": R(0, json.dumps(RUNDIR_PLAN)),
                                 "--build-loadext-helper": R(0, helper_report()),
                                 "--corroborate-run-dir": StopHere("stopped at the corroboration call")})
    try:
        X.U.unit_leg(run, leg, {})
        raise ArmFailure("unit_leg never asked for the corroboration")
    except StopHere:
        pass
    call = run.resolver.asked("--corroborate-run-dir")[0]
    eq(call[1], leg.label, "the leg")
    adjacent(call, "--driver-run-dir", os.path.join(run.leg_out(leg), "run"), "THIS leg's run directory")
    eq(all_after(call, "--supplied"), list(leg.d["confoundsByName"]), "the plan's by-name supply is handed over")
    verbs = [c[0] for c in run.resolver.calls]
    expect(verbs.index("--build-loadext-helper") < verbs.index("--corroborate-run-dir"),
           "the run directory is populated (helper staged) before it is corroborated: %r" % verbs)
    pe = plan_leg("pe64-x86_64")
    # A PROBED plan cannot be produced hermetically; the gating under test here is the OTHER one.
    pe.d["confoundGating"] = "probed"
    has(refused(X.V.confound_supply, pe, None), "runDirectoryGating='unmeasured'",
        "before: the plan's own run-directory gating is refused")
    corr = {"confounds": ["^sessionnoact-4\\.3$"], "abortConfounds": ["^veryquick/nolock\\.test$"],
            "runDirectoryGating": "measured", "reportText": "[pe64-x86_64] corroborated by the self-test"}
    run2 = new_run()
    run2.resolver = FakeResolver({"--corroborate-run-dir": R(0, json.dumps(corr))})
    X.U.corroborate(run2, pe, "/o/pe64-x86_64/run")
    eq(X.V.confound_supply(pe, None), ["^sessionnoact-4\\.3$"], "after: the SURVIVORS are the supply")
    eq((pe.d["abortConfounds"], pe.d["runDirectoryGating"]), (["^veryquick/nolock\\.test$"], "measured"),
       "the abort rows and the gating are replaced too")


# =======================================================================================
# EV -- execution evidence
# =======================================================================================

MONITOR_PY = r'''
import os, sys, time
timeline, stop_file, done = sys.argv[1], sys.argv[2], sys.argv[3]
with open(timeline, "w") as fh:
    fh.write('{"kind": "armed"}\n')
deadline = time.time() + 60
while time.time() < deadline:
    if os.path.exists(stop_file):
        with open(done, "w") as fh:
            fh.write("stopped\n")
        sys.exit(0)
    time.sleep(0.02)
sys.exit(3)
'''


def armed_leg(label):
    leg = plan_leg(label)
    leg.d["confoundsByEvidence"] = ["^walsetlk-"]
    leg.d["executionEvidence"] = ["clock-realtime-steps"]
    return leg


def monitor_answer():
    d = tmpdir("ev", "monitor")
    script = write(os.path.join(d, "monitor.py"), MONITOR_PY)
    paths = types.SimpleNamespace(timeline=os.path.join(d, "timeline.jsonl"), stop=os.path.join(d, "stop"),
                                  done=os.path.join(d, "done"))
    plan_ = {"argv": [sys.executable, "-B", script, paths.timeline, paths.stop, paths.done],
             "timeline": paths.timeline, "stopFile": paths.stop, "armWithinSeconds": 30,
             "stopWithinSeconds": 30}
    return R(0, json.dumps(plan_)), paths


def ev_segment(runner):
    """`sqlite_units.run_one_segment` with a REAL monitor process and an injected runner."""
    answer, paths = monitor_answer()
    run = new_run()
    run.resolver = FakeResolver({"--execution-monitor-argv": answer})
    leg = armed_leg("elf64-x86_64")
    d = tmpdir("ev", "segment")
    seglog = write(os.path.join(d, "corpus.log"), b"stale bytes from a previous segment\n")
    seg = X.U.Segment("tier", "", "veryquick.test", None, "/corpus/veryquick.test", "")
    lr = X.U.LegRun()
    raised = None
    try:
        res = X.U.run_one_segment(run, leg, seg, seglog, d, [], "/built/testfixture", [],
                                  [os.path.join(d, "lib")], dict(os.environ), None, lr, runner=runner)
    except Exception as exc:  # noqa: BLE001 -- the arm asserts on it
        res, raised = None, exc
    return types.SimpleNamespace(run=run, seglog=seglog, paths=paths, res=res, raised=raised)


def ev_normal():
    def build():
        seen = {}

        def runner(argv, cwd, env, log_path, stall, cap, settle, kill_tree, sweep, any_left):
            seen.update(argv=list(argv), log_path=log_path, size=os.path.getsize(log_path),
                        timeline=read(paths_box[0].timeline) if os.path.exists(paths_box[0].timeline) else "",
                        done=os.path.exists(paths_box[0].done))
            return X.L.SegmentResult(0, "", 0.0)
        paths_box = []
        answer, paths = monitor_answer()
        paths_box.append(paths)
        run = new_run()
        run.resolver = FakeResolver({"--execution-monitor-argv": answer})
        leg = armed_leg("elf64-x86_64")
        d = tmpdir("ev", "normal")
        seglog = write(os.path.join(d, "corpus.log"), b"stale bytes from a previous segment\n")
        seg = X.U.Segment("tier", "", "veryquick.test", None, "/corpus/veryquick.test", "")
        res = X.U.run_one_segment(run, leg, seg, seglog, d, [], "/built/testfixture", [],
                                  [os.path.join(d, "lib")], dict(os.environ), None, X.U.LegRun(),
                                  runner=runner)
        return types.SimpleNamespace(run=run, seglog=seglog, paths=paths, res=res, seen=seen)
    return fixture("ev_normal", build)


@arm("EV01", "the monitor plan is REQUESTED for this segment's log (--execution-monitor-argv)",
     "S7.15 P7.23")
def _ev01():
    leg = armed_leg("elf64-x86_64")
    fake = FakeResolver({"--execution-monitor-argv": R(3, "", "no monitor for this probe here")})
    log = new_log()
    seglog = write(os.path.join(tmpdir("ev", "ev01"), "corpus.log"), b"")
    eq(X.L.evidence_start(fake, leg, seglog, 0, False, log), [], "a monitor that cannot be planned arms nothing")
    calls = fake.asked("--execution-monitor-argv")
    eq(len(calls), 1, "one request per declared evidence probe")
    adjacent(calls[0], "--evidence-probe", "clock-realtime-steps", "the probe")
    adjacent(calls[0], "--watch-log", seglog, "THIS segment's log")
    adjacent(calls[0], "--run-filesystem", "driver", "the leg's run filesystem")
    adjacent(calls[0], "--segment-cap-seconds", "0", "the segment cap")
    has(" ".join(warnings(log)), "stays GENUINE", "a monitor that could not be planned un-excuses, out loud")
    for why, lg, override in (("no armed row", plan_leg("elf64-x86_64"), False), ("operator override", leg, True)):
        f2 = FakeResolver({})
        eq(X.L.evidence_start(f2, lg, seglog, 0, override, new_log()), [], "%s: no monitor" % why)
        eq(f2.calls, [], "%s: nothing requested" % why)


@arm("EV02", "the monitor is ARMED on this segment's EMPTIED log before the fixture starts",
     "S7.16 P7.25")
def _ev02():
    s = ev_normal()
    call = s.run.resolver.asked("--execution-monitor-argv")[0]
    adjacent(call, "--watch-log", s.seglog, "the monitor watches THIS segment's log")
    eq(s.seen.get("log_path"), s.seglog, "the fixture writes the same log")
    eq(s.seen.get("size"), 0, "the stale log was EMPTIED before the monitor armed")
    has(s.seen.get("timeline", ""), X.L.ARMED_MARK, "the monitor had ARMED when the fixture started")
    eq(s.seen.get("done"), False, "the monitor was still running during the fixture")
    eq(s.res.rc, 0, "the runner's result is returned")


@arm("EV03", "the monitor is STOPPED after the fixture -- even when the fixture RAISES", "S7.17 P7.27")
def _ev03():
    s = ev_normal()
    expect(os.path.exists(s.paths.done), "normal path: the monitor saw its stop file")
    eq(os.path.exists(s.paths.stop), False, "normal path: the stop file is cleaned up")

    def boom(*_a, **_k):
        raise RuntimeError("the fixture exploded")
    e = ev_segment(boom)
    expect(isinstance(e.raised, RuntimeError), "the fixture's exception propagates: %r" % (e.raised,))
    expect(os.path.exists(e.paths.done), "raising path: the monitor was STOPPED anyway")
    eq(os.path.exists(e.paths.stop), False, "raising path: the stop file is cleaned up")


@arm("EV04", "the per-failure attribution is REQUESTED (--attribute-unit-failures)", "S7.18 P7.29")
def _ev04():
    leg = armed_leg("elf64-x86_64")
    fake = FakeResolver({"--attribute-unit-failures": R(0, "EXCUSED\twalsetlk-2.1.3\nGENUINE\tsometest-9.9\n"
                                                             "REPORT\t[x] a clock step inside walsetlk-2.1.3\n")})
    log = new_log()
    got = X.L.evidence_attribute(fake, leg, "native", ["/o/corpus.log"], ["mm-"],
                                 ["walsetlk-2.1.3", "sometest-9.9"], False, log)
    eq(got, {"walsetlk-2.1.3"}, "the EXCUSED names")
    c = fake.asked("--attribute-unit-failures")[0]
    eq(c[:2], ["--attribute-unit-failures", leg.label], "the verb and the leg")
    for opt in ("--evidence-pattern=^walsetlk-", "--segment-log=/o/corpus.log", "--tier-prefix=mm-",
                "--failure=walsetlk-2.1.3", "--failure=sometest-9.9"):
        has(c, opt, "the request")
    has(log_text(log), "a clock step inside walsetlk-2.1.3", "the resolver's account is relayed")
    f2 = FakeResolver({"--attribute-unit-failures": R(2, "", "broken")})
    eq(X.L.evidence_attribute(f2, leg, "native", [], [], ["walsetlk-2.1.3"], False, new_log()), set(),
       "a failed attribution excuses NOTHING")
    f3 = FakeResolver({})
    eq(X.L.evidence_attribute(f3, leg, "native", [], [], ["walsetlk-2.1.3"], True, new_log()), set(),
       "override active: nothing asked, nothing excused")
    eq(f3.calls, [], "override active: no request")


def judge_mode(label):
    run = new_run()
    run.out_dir = tmpdir("ev", "judge")
    ledger(run)
    run.resolver = FakeResolver({"--attribute-unit-failures": R(0, "EXCUSED\twalsetlk-2.1.3\n"),
                                 "--registry-controls": R(0, "")})
    leg = armed_leg(label)
    lr = X.U.LegRun()
    lr.failures = ["walsetlk-2.1.3", "sometest-9.9"]
    X.U.judge_leg(run, leg, {"prefixes": list(P_TIER), "witnesses": []}, lr, [],
                  os.path.join(run.out_dir, "corpus.log"), os.path.join(run.out_dir, "corpus-units.txt"))
    calls = run.resolver.asked("--attribute-unit-failures")
    expect(calls, "the classifier asked for the per-failure attribution")
    return after(calls[0], "--leg-mode"), leg


@arm("EV05", "the classifier passes ITS scope mode to the attribution", "S7.19 P7.31")
def _ev05():
    mode, leg = judge_mode("elf64-arm64")
    eq((mode, X.V.leg_mode(leg)), ("emulated", "emulated"), "a launched leg")
    mode, leg = judge_mode("elf64-x86_64")
    eq((mode, X.V.leg_mode(leg)), ("native", "native"), "a native leg")


# =======================================================================================
# SU -- the supply
# =======================================================================================

SU_X86 = ("^walsetlk-", "^busy2-", "^zipfile-25\\.0$")
SU_ARM = ("^busy2-", "emulated:^writecrash-")
OVERRIDE = "^operator-1 ^operator-2"


def supply(leg, override=None):
    return X.V.confound_supply(leg, override)


@arm("SU01", "a leg gets ITS OWN declared patterns", "S9.01 P8.01")
def _su01():
    eq(supply(pin_leg("elf64-x86_64", SU_X86)), list(SU_X86), "the supply")


@arm("SU02", "...a DIFFERENT leg gets a DIFFERENT set", "S9.02 P8.03")
def _su02():
    eq(supply(pin_leg("elf64-arm64", SU_ARM)), list(SU_ARM), "the supply")


@arm("SU03", "a leg declaring [] inherits NOTHING", "S9.03 P8.02")
def _su03():
    eq(supply(pin_leg("pe64-x86_64", ())), [], "the supply")


@arm("SU04", "the `emulated:` scope SURVIVES the supply", "S9.04 P8.04")
def _su04():
    has(supply(pin_leg("elf64-arm64", SU_ARM)), "emulated:^writecrash-", "the supply")


@arm("SU05", "the operator override (DSS_CONFOUNDS, read by Config) reaches EVERY leg", "S9.05 P8.05")
def _su05():
    ov = make_config(DSS_CONFOUNDS=OVERRIDE).confounds_override
    eq(ov, ["^operator-1", "^operator-2"], "Config's override")
    for leg in (pin_leg("pe64-x86_64", ()), pin_leg("elf64-x86_64", SU_X86), pin_leg("elf64-arm64", SU_ARM)):
        eq(supply(leg, ov), ["^operator-1", "^operator-2"], "the override on %s" % leg.label)
    eq(make_config(DSS_CONFOUNDS="  ^op[e]r*\t\t^x?  ").confounds_override, ["^op[e]r*", "^x?"],
       "split on whitespace, each token VERBATIM (no glob expansion)")
    eq(make_config(DSS_CONFOUNDS="   ").confounds_override, None, "an all-blank override is NO override")


@arm("SU06", "...and REPLACES the earned set, never merges", "S9.06")
def _su06():
    got = supply(pin_leg("elf64-x86_64", SU_X86), ["^operator-1", "^operator-2"])
    eq(got, ["^operator-1", "^operator-2"], "the supply under an override")


def undeclared():
    return pin_leg("macho64-arm64", (), omit=("confoundsByName",))


@arm("SU07", "an UNDECLARED leg REFUSES (HarnessDie) rather than answering []", "S9.07")
def _su07():
    refused(supply, undeclared())


@arm("SU08", "...naming the transport defect", "S9.08 P8.06")
def _su08():
    msg = refused(supply, undeclared())
    has(msg, "transport defect", "the refusal")
    has(msg, "confoundsByName", "...and the missing field")


@arm("SU09", "an UNPROBED plan REFUSES rather than serving its ungated list", "S9.09")
def _su09():
    refused(supply, pin_leg("unprobedleg", ("^busy2-",), gating="unprobed"))


@arm("SU10", "...naming the gating it actually got", "S9.10 P8.07")
def _su10():
    has(refused(supply, pin_leg("unprobedleg", ("^busy2-",), gating="unprobed")),
        "confoundGating='unprobed'", "the refusal")


@arm("SU11", "...and saying how to resolve a measured plan", "P8.08")
def _su11():
    has(refused(supply, pin_leg("unprobedleg", ("^busy2-",), gating="unprobed")),
        "--environment-probes skip", "the remedy")


@arm("SU12", "`injected` and UNSET confound gatings are refused too")
def _su12():
    has(refused(supply, pin_leg("injectedleg", ("^busy2-",), gating="injected")),
        "confoundGating='injected'", "injected")
    has(refused(supply, pin_leg("unsetleg", ("^busy2-",), omit=("confoundGating",))),
        "confoundGating='<unset>'", "unset")
    has(refused(supply, pin_leg("unsetrdg", ("^busy2-",), omit=("runDirectoryGating",))),
        "runDirectoryGating='<unset>'", "an unset run-directory gating")


def uncorroborated():
    return pin_leg("uncorroboratedleg", ("^vtabH-3\\.1$",), rdg="unmeasured")


@arm("SU13", "an UNMEASURED run directory REFUSES rather than serving its list", "S9.11 S7.08 P7.13")
def _su13():
    refused(supply, uncorroborated())


@arm("SU14", "...naming the gating it actually got", "S9.12 P8.09")
def _su14():
    has(refused(supply, uncorroborated()), "runDirectoryGating='unmeasured'", "the refusal")


@arm("SU15", "...and the call that would measure it", "S9.13 P8.10")
def _su15():
    has(refused(supply, uncorroborated()), "--corroborate-run-dir", "the remedy")


@arm("SU16", "a `not-required` run-directory gating is ACCEPTED", "S9.14")
def _su16():
    has(supply(pin_leg("elf64-x86_64", SU_X86, rdg="not-required")), "^walsetlk-", "the supply")


@arm("SU17", "a `measured` run-directory gating is ACCEPTED", "P8.11")
def _su17():
    has(supply(pin_leg("pe64-x86_64", ("^vtabH-3\\.1$",), rdg="measured")), "^vtabH-3\\.1$", "the supply")


def usable(d):
    """A planned leg dict with its gatings made usable -- what a probed plan whose run directory
    was corroborated carries (neither can be produced hermetically)."""
    d = json.loads(json.dumps(d))
    d["confoundGating"] = "probed"
    if d.get("runDirectoryGating") == "unmeasured":
        d["runDirectoryGating"] = "measured"
    return X.C.Leg(d)


@arm("SU18", "SHIPPED catalogue: each planned leg supplies ITS OWN by-name wires "
     "(removal changes only that leg)")
def _su18():
    shipped = plan("linux", "x86_64", LAUNCHERS_LINUX)
    with open(X.C.LEGS_JSON, "r", encoding="utf-8") as fh:
        cat = json.load(fh)
    rows = {lg["label"]: [r.get("pattern") for r in lg.get("confounds", [])] for lg in cat["legs"]}
    eq(sorted(shipped), sorted(rows), "the plan covers every declared leg")
    supplies = {}
    for label, d in shipped.items():
        got = supply(usable(d))
        eq(got, list(d["confoundsByName"]), "%s: the supply is the plan's own by-name list" % label)
        for wire in got:
            has(rows[label], X.V.split_scope(wire)[1], "%s: %r comes from ITS OWN rows" % (label, wire))
        supplies[label] = got
    has(supplies["elf64-arm64"], "emulated:^writecrash-", "elf64-arm64 supplies its scoped row")
    eq([w for w in supplies["elf64-x86_64"] if "writecrash" in w], [], "elf64-x86_64 never inherits it")
    # The negative on the linux/arm64 pair (the plans CL28 also reads): the same catalogue with
    # ONE row's `scope` removed changes the supply of that row's leg and of no other.
    before = plan("linux", "arm64", "")
    after_ = plan("linux", "arm64", "", catalogue=unscoped_catalogue())
    eq(sorted(lab for lab in before if supply(usable(after_[lab])) != supply(usable(before[lab]))),
       ["elf64-arm64"], "NEGATIVE by removal: only the leg whose row lost its scope supplies differently")


@arm("SU19", "the operator override takes PRECEDENCE over every refusal")
def _su19():
    ov = ["^operator-1"]
    for why, leg in (("undeclared", undeclared()),
                     ("unprobed", pin_leg("u", ("^busy2-",), gating="unprobed")),
                     ("unmeasured", uncorroborated())):
        eq(supply(leg, ov), ov, "%s leg under an override" % why)


# =======================================================================================
# the run
# =======================================================================================

def old_universe():
    ids = []
    for blocks in (OLD_SH, OLD_PS):
        for block, n in blocks:
            ids += ["%s.%02d" % (block, i) for i in range(1, n + 1)]
    return ids


def registry_problems():
    """The registry's own accounting: TOTAL, the id families, and the old-arm map."""
    bad = []
    ids = [a.id for a in ARMS]
    if len(ARMS) != TOTAL:
        bad.append("the registry holds %d arm(s) but TOTAL declares %d" % (len(ARMS), TOTAL))
    want = ["%s%02d" % (fam, i) for fam, n in FAMILIES for i in range(1, n + 1)]
    if sum(n for _f, n in FAMILIES) != TOTAL:
        bad.append("FAMILIES sum to %d, not TOTAL %d" % (sum(n for _f, n in FAMILIES), TOTAL))
    dup = sorted({i for i in ids if ids.count(i) > 1})
    if dup:
        bad.append("duplicate arm id(s): %s" % " ".join(dup))
    if sorted(ids) != sorted(want):
        bad.append("arm ids differ from the declared families: missing %s; unexpected %s"
                   % (sorted(set(want) - set(ids)), sorted(set(ids) - set(want))))
    universe = old_universe()
    if (sum(n for _b, n in OLD_SH), sum(n for _b, n in OLD_PS)) != OLD_TOTALS:
        bad.append("the old blocks do not sum to the twins' declared totals %s" % (OLD_TOTALS,))
    carried = [o for a in ARMS for o in a.old]
    retired = [o for _why, ids_ in RETIRED for o in ids_.split()]
    for name, seq in (("carried", carried), ("retired", retired)):
        stray = sorted(set(seq) - set(universe))
        if stray:
            bad.append("%s ids that are no old arm: %s" % (name, " ".join(stray)))
    if len(retired) != len(set(retired)):
        bad.append("a retired id is listed twice")
    both = sorted(set(carried) & set(retired))
    if both:
        bad.append("ids both carried and retired: %s" % " ".join(both))
    lost = sorted(set(universe) - set(carried) - set(retired))
    if lost:
        bad.append("old arms neither carried nor retired: %s" % " ".join(lost))
    for a in ARMS:
        for g in a.gates:
            if g not in GATES:
                bad.append("%s names an undeclared gate %r" % (a.id, g))
    return bad, len(set(carried)), len(retired), len(universe)


def bind_modules():
    """Import every driver module and resolve every name an arm calls. -> the problems."""
    import importlib
    if HERE not in sys.path:
        sys.path.insert(0, HERE)
    bad = []
    for attr, name in DRIVER_MODULES:
        try:
            setattr(X, attr, importlib.import_module(name))
        except Exception as exc:  # noqa: BLE001 -- a module that cannot import is a named FAIL
            bad.append("cannot import %s: %s" % (name, exc_text(exc)))
    for attr, name in DRIVER_MODULES:
        mod = getattr(X, attr)
        for fn in NEEDED.get(name, ()):
            if mod is not None and not hasattr(mod, fn):
                bad.append("%s has no `%s` (an arm calls it by that name)" % (name, fn))
    return bad


def _rm_readonly(func, path, _info):
    try:
        os.chmod(path, stat.S_IWRITE | stat.S_IREAD | stat.S_IEXEC)
    except OSError:
        pass
    func(path)


def remove_tree(path):
    for attempt in range(6):
        if not os.path.lexists(path):
            return True
        try:
            if sys.version_info >= (3, 12):
                shutil.rmtree(path, onexc=_rm_readonly)
            else:
                shutil.rmtree(path, onerror=_rm_readonly)
        except OSError:
            time.sleep(0.25 * (attempt + 1))
    return not os.path.lexists(path)


def run_arms(counts):
    section = None
    for a in ARMS:
        if a.id[:2] != section:
            section = a.id[:2]
            out("--- %s ---" % SECTIONS.get(section, section))
        tag = "  [%s]" % " ".join(a.old) if a.old else "  [new]"
        absent = [g for g in a.gates if not GATES[g][0]()]
        if absent:
            counts["skipped"] += 1
            out("  SKIP %s %s -- %s" % (a.id, a.label, GATES[absent[0]][1]))
            continue
        sink = io.StringIO()
        failure = None
        try:
            with contextlib.redirect_stdout(sink), contextlib.redirect_stderr(sink):
                a.fn()
        except (ArmFailure, FixtureError) as exc:
            failure = str(exc)
        except (Exception, SystemExit) as exc:  # noqa: BLE001 -- a raising arm is a FAIL, named
            failure = "the arm RAISED %s" % exc_text(exc)
        if failure is None:
            counts["passed"] += 1
            out("  ok   %s %s%s" % (a.id, a.label, tag))
        else:
            counts["failed"] += 1
            out("  FAIL %s %s%s" % (a.id, a.label, tag))
            for line in failure.splitlines()[:40]:
                out("       %s" % line)
    for g, (probe, why) in sorted(GATES.items()):
        n = len([a for a in ARMS if g in a.gates])
        if not probe():
            out("  (gate `%s` absent on this host: %d arm(s) SKIPPED -- %s)" % (g, n, why))


def main(argv=None):
    args = list(sys.argv[1:] if argv is None else argv)
    if args in (["-h"], ["--help"]):
        out(__doc__)
        return 0
    if args:
        sys.stderr.write(_ascii("test_confound_scope.py takes no arguments (-h prints usage); got: %s\n"
                                % " ".join(args)))
        return 2
    t0 = time.monotonic()
    counts = collections.Counter(passed=0, failed=0, skipped=0)
    out("test_confound_scope.py: %d arm(s) over the Python driver in %s" % (TOTAL, HERE))
    root = os.path.realpath(tempfile.mkdtemp(prefix="dss-confscope-"))
    X.root, X.cache, X.memo, X.seq = root, {}, {}, 0
    saved_log_stream = None
    try:
        problems = bind_modules()
        bad, n_carried, n_retired, n_old = registry_problems()
        problems += bad
        for p in problems:
            counts["failed"] += 1
            out("  FAIL meta: %s" % p)
        out("  meta: registry %d arm(s); old arms %d = %d carried + %d retired%s"
            % (len(ARMS), n_old, n_carried, n_retired, "" if not problems else " -- SEE THE FAILS ABOVE"))
        if X.C is not None:
            # Driver code that logs through the module default must never reach this stream.
            saved_log_stream, X.C.LOG.stream = X.C.LOG.stream, io.StringIO()
            with suite_environment(root):
                run_arms(counts)
        else:
            counts["failed"] += 1
            out("  FAIL meta: sqlite_common did not import, so no arm could run")
    finally:
        if saved_log_stream is not None:
            X.C.LOG.stream = saved_log_stream
        if not remove_tree(root):
            out("  note: could not remove the temp root %s" % root)
    out("elapsed %.1fs" % (time.monotonic() - t0))
    out("passed=%d failed=%d skipped=%d" % (counts["passed"], counts["failed"], counts["skipped"]))
    return 0 if counts["failed"] == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
