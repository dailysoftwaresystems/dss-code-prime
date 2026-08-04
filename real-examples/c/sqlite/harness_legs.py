#!/usr/bin/env python3
"""THE SQLITE HARNESS LEG RESOLVER — one host-independent answer, two drivers.

D-HARNESS-CROSS-HOST-ANY-TARGET item (2): "de-host-lock the harness so any
(host, target) pair builds".

WHAT THIS REPLACES.  `build-and-test.sh` built its leg list out of `uname`
(`add_leg host $(host_target_spec)`, plus an arm64 leg only when the host was
Linux/x86_64 — so pe64 and macho were unreachable from it), and
`build-and-test.ps1` opened with `if (-not $IsWindows) { Die ... }` over a single
hardcoded `$Spec = 'x86_64:pe64-x86_64-windows-exec'`.  Both scripts therefore
answered "which targets does this harness build?" with "whatever this machine
is", which is the exact inverse of the requirement.

THE SPLIT THIS FILE ENFORCES.  Two questions that used to be one:

  * CAN THIS HOST **BUILD** TARGET T?  — ALWAYS YES, and it is not a host
    question at all.  DSS selects its target from config; a leg's build is
    attempted on every host, unconditionally.  A build can still FAIL (a
    compile error → `poisoned`) or be unable to START because a DECLARED input
    is absent from the machine (→ `skipped-build-input-missing`), but neither
    outcome is ever inferred from the host's identity: both are OBSERVED, and
    both are reported by name.
  * CAN THIS HOST **EXECUTE** T's ARTIFACT? — the one legitimate host question.
    Answered from the leg's own declaration: natively when the host OS is in
    `runOn` and the host arch is the target's, otherwise through a DECLARED
    launcher (qemu for a cross-arch host, Wine for a cross-OS one), otherwise
    not at all — and "not at all" is a NAMED verdict, never silence.

THE VERDICT VOCABULARY IS NOT THIS FILE'S.  It is the closed set in
`tests/test_support/arm_verdict_ledger.hpp`, reused verbatim so the sqlite
harness and the two examples-corpus harnesses classify a non-run the same way
and a reader can grep one set of names across all three.  `--verdict-vocabulary`
prints it, and `tests/harness/test_sqlite_harness_legs.cpp` asserts the printed
list is EXACTLY `armVerdictName()` over `kAllArmVerdicts`, in order — so the
duplication of those strings into Python cannot drift without a red test.

PYTHON, AND WHY.  The decision must have ONE implementation: this project has
already paid for the alternative (build-and-test.ps1's header records a
capability that existed in the .sh and not in the .ps1, so a CI driver exporting
DSS_COMMIT got an assertion on one leg and silence on the other — "a capability
in one driver and not the other is a silent harness bug").  Python is the only
language both drivers already hard-require (the .sh does `ensure_cmd python3`
before generating a manifest; the .ps1 gates on python3 in its Step 1), and the
repo already uses the python-core/thin-shell-wrapper shape in scripts/.

USAGE
  harness_legs.py --verdict-vocabulary
  harness_legs.py --plan [--host-os OS] [--host-arch ARCH] [--format json|sh]
                         [--launchers-available a,b,... | --launchers-none]
  harness_legs.py --header-stages
  harness_legs.py --path-translations
  harness_legs.py --path-translation VERB --translate-path PATH [--translate-path …]
  harness_legs.py --path-translation VERB --assert-translated ARG [--assert-translated …]
  harness_legs.py --env-transfers
  harness_legs.py --env-transfer VERB --forward NAME [--forward NAME …] [--carrier-current V]
  harness_legs.py --lint
  harness_legs.py --self-test
"""

import argparse
import json
import os
import platform
import re
import shlex
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
CATALOGUE = os.path.join(HERE, "legs.json")

# ── The verdict vocabulary ──────────────────────────────────────────────────
# ★ MIRROR, NOT A SOURCE. The authority is `ArmVerdict` in
# tests/test_support/arm_verdict_ledger.hpp; this list must equal
# armVerdictName() over kAllArmVerdicts, IN ORDER.
# tests/harness/test_sqlite_harness_legs.cpp fails if it does not.
VERDICTS = [
    "ran",                            # verified: built, spawned, asserted
    "expect-error-asserted",          # verified: a declared-failure arm
    "skipped-by-runOn",               # structural: host OS cannot spawn it and
                                      #   no launcher is declared for this host
    "skipped-no-emulator-declared",   # structural: host OS matches, arch does
                                      #   not, and no launcher is declared
    "skipped-emulator-missing",       # environmental: launcher declared, absent
    "skipped-build-input-missing",    # environmental: a DECLARED build input
                                      #   (a resolve-library binary, a target
                                      #   compiler) is absent from this machine
    "not-selected-by-runner",         # harness limitation
    "poisoned",                       # failure, already reported loudly
]

# Closed vocabularies for the catalogue's own keys. Anything outside them is a
# LOUD lint failure rather than a silently-ignored typo.
LIBRARY_PROVIDERS = {"host-system", "ubuntu-ports-arm64", "search-paths"}
RECIPE_TRANSFORMS = {"none", "windows-selfconfig"}

# ── Launcher path translation ───────────────────────────────────────────────
#
# D-HARNESS-NO-WSL-LAUNCHER-FOR-ELF-ON-WINDOWS. A launcher does not always share
# a path NAMESPACE with the driver that spawns it. Wine on Linux does (`wine
# /home/me/x.exe` — the argument is the driver's own path, verbatim); `wsl.exe`
# on Windows does NOT (the driver holds `C:\...\testfixture`, the callee needs
# `/mnt/c/.../testfixture`). Handing an untranslated path to a launcher of the
# second kind does not fail as a path error: the callee opens a RELATIVE file
# named `C:\...`, does not find it, and the run reads as a broken binary.
#
# So the namespace is DECLARED, per launcher, from this closed vocabulary — never
# inferred from the host, and never from the launcher's name. Two launchers on the
# same host can want different answers (a future ssh launcher on Windows would
# need a third verb), and the same launcher on two hosts wants the same one.
#
# Each verb owns everything its translation needs, as DATA:
#   translator   the argv that performs it, with the path appended. [] = identity.
#   sourceShape  the FOREIGN-PATH predicate (below), used three ways: an input
#                MUST match it (else it is not a path in the namespace we
#                translate from), an output must NOT, and no argument handed to
#                a launcher may still match it at spawn time.
#   separator    [from, to] applied before the translator runs. [] = untouched.
#   validHostOs  the only host OS on which this namespace exists, so the lint can
#                CHECK a declaration instead of trusting it. "" = any.
PATH_TRANSLATIONS = {
    "none": {
        "translator": [],
        "sourceShape": "",
        "separator": [],
        "validHostOs": "",
    },
    "windows-to-wsl": {
        # `wslpath` is the translator because the parts that bite are its job:
        # the drive-letter -> mount-point mapping is configurable (/etc/wsl.conf
        # `root=`), UNC paths are a different mapping again, and case is not
        # ours to guess. Hand-rolled `C:\` -> `/mnt/c/` string surgery gets all
        # three wrong quietly.
        "translator": ["wsl.exe", "wslpath", "-a", "-u"],
        "sourceShape": "windows-drive",
        # ✔MEASURED 2026-08-04 on this host: `wsl.exe wslpath -a -u 'C:\a\b'`
        # exits 1 and prints `wslpath: C:ab` — the backslashes are eaten before
        # wslpath ever sees them. The forward-slash spelling of the SAME path
        # exits 0 and yields /mnt/c/a/b. Normalising the separator is not path
        # surgery: `/` is accepted by every Windows path API and `\` is never a
        # filename character there, so this is a re-spelling of the same string,
        # and everything that actually varies is still wslpath's to decide.
        "separator": ["\\", "/"],
        "validHostOs": "windows",
    },
}


def _looks_like_windows_drive_path(arg):
    """`C:\\x`, `c:/x`, `--out=D:\\y`, or a `\\\\server\\share` UNC.

    Scans the WHOLE argument rather than only its head: a path that arrives
    embedded in a `--flag=<path>` is exactly as untranslated as a bare one, and
    the point of this predicate is that nothing untranslated reaches a launcher.
    A false positive here stops the run with a nameable reason, which is the
    direction a harness should fail in."""
    for i in range(max(0, len(arg) - 2)):
        if arg[i].isalpha() and arg[i + 1] == ":" and arg[i + 2] in "\\/":
            return True
    return arg.startswith("\\\\")


# shape name -> predicate. A `sourceShape` naming no entry here is a LegError,
# not a silently-true test.
FOREIGN_PATH_SHAPES = {
    "windows-drive": _looks_like_windows_drive_path,
}


# ── Launcher environment transfer ───────────────────────────────────────────
#
# THE SECOND HALF OF THE SAME FORK, and it was found by MEASURING the first.
# A launcher that lives in another OS namespace does not inherit the driver's
# environment any more than it understands the driver's paths.
#
# ✔MEASURED 2026-08-04 on this host: with `SQLITE_TEST_PATTERN_LIST` and
# `QUICKTEST_OMIT` set in the Windows parent, `wsl.exe -- sh -c 'echo …'` prints
# BOTH as EMPTY. With `WSLENV` naming them, both arrive intact. The consequence
# was observed live, not argued: the corpus resume engine passes its file
# selection through SQLITE_TEST_PATTERN_LIST, so under a wsl.exe launcher the
# resume silently re-ran the corpus FROM THE BEGINNING instead of from the abort
# point — a harness that looks like it is working and is not.
#
# ⚠ AND THE SHARP EDGE, MEASURED THE HARD WAY THE SAME DAY: naming an UNSET
# variable in the carrier does NOT leave it unset on the other side — it arrives
# EMPTY BUT EXISTING. sqlite's permutations.test asks `info exists
# ::env(SQLITE_TEST_PATTERN_LIST)`, so an empty-but-existing value is an EMPTY
# FILE LIST, not "no filter": the tier selected ZERO files, tester.tcl still
# finalised and printed `0 errors out of 1 tests`, and the driver called the run
# GREEN. THIS FUNCTION DOES NOT KNOW WHICH VARIABLES ARE SET — the caller does,
# and both drivers filter to the ones that are, per segment, before calling.
#
# Same shape as the path namespace, for the same reason: DECLARED per launcher,
# closed vocabulary, unknown verb is a LOUD lint failure.
#   nameCarrier  the variable whose VALUE is the list of names to forward.
#                "" = the child simply inherits and nothing is needed.
#   separator    how that list is joined.
#   validHostOs  the only host OS the mechanism exists on, so the lint can CHECK.
ENV_TRANSFERS = {
    "inherit": {"nameCarrier": "", "separator": "", "validHostOs": ""},
    "wslenv": {"nameCarrier": "WSLENV", "separator": ":", "validHostOs": "windows"},
}


def env_transfer(verb):
    """The declared verb's spec, or a LegError — never a permissive default.
    Defaulting to `inherit` for an unknown verb is exactly the silent-empty-
    environment failure this vocabulary exists to prevent."""
    spec = ENV_TRANSFERS.get(verb)
    if spec is None:
        raise LegError(
            "unknown envTransfer %r (known: %s). A launcher declares how the "
            "driver's run environment reaches the process it spawns; an "
            "unrecognised verb cannot be silently treated as 'inherit', because "
            "'inherit' is itself a claim — that the child sees this driver's "
            "environment block."
            % (verb, ", ".join(sorted(ENV_TRANSFERS))))
    return spec


def env_carrier_assignments(verb, names, current=""):
    """The extra `NAME=VALUE` assignments the driver must make so that `names`
    are visible to the launched process. Empty for a verb that inherits.

    `current` is any value the carrier already holds, so an operator's own
    setting survives — the merge, and the separator it uses, belong here rather
    than in two drivers."""
    spec = env_transfer(verb)
    carrier = spec["nameCarrier"]
    if not carrier:
        return []
    wanted = [n for n in names if n]
    if not wanted:
        return []
    sep = spec["separator"]
    have = [x for x in (current or "").split(sep) if x]
    merged = list(have)
    for n in wanted:
        # `WSLENV` entries may carry a `/u`-style suffix; compare on the NAME.
        if not any(h == n or h.startswith(n + "/") for h in merged):
            merged.append(n)
    return ["%s=%s" % (carrier, sep.join(merged))]

# The zconf.h guards `./configure` may have baked into the DERIVING host's zlib
# header, and which each leg therefore has to declare for ITS OWN target. Closed,
# for the same reason every other vocabulary here is: a typo'd guard name would
# otherwise be a declaration that silently does nothing.
# ★ These are NAMES OF MACROS IN A THIRD-PARTY HEADER, listed here only so a
# declaration can be validated. stage-zinc.py is what applies them; nothing in
# this file knows what either macro MEANS.
ZCONF_GUARDS = ("Z_HAVE_UNISTD_H", "Z_HAVE_STDARG_H")

# The one guard whose correct value is DERIVABLE from the target, so the lint can
# check a declaration instead of trusting it: <unistd.h> is POSIX, and a Windows
# target does not have it. (Z_HAVE_STDARG_H is not in here because <stdarg.h> is
# C-standard — every target has it, so there is nothing to cross-check.)
POSIX_ONLY_ZCONF_GUARDS = ("Z_HAVE_UNISTD_H",)
TARGET_OS_NAMES = ("linux", "windows", "darwin")

# Host identity, in the SAME spellings the corpus manifests use for `runOn` and
# the same arch spellings the shipped *.target.json files use as their `name`.
# (`currentHostOs()` / `currentHostArch()` in arm_verdict_ledger.hpp are the C++
# twins; `uname -s` says "Darwin" and build-and-test.sh historically said
# "macos" — both normalize to `darwin` here, once, at the boundary.)
OS_ALIASES = {
    "linux": "linux", "wsl": "linux",
    "darwin": "darwin", "macos": "darwin", "mac": "darwin", "osx": "darwin",
    "windows": "windows", "win32": "windows", "mingw": "windows",
    "cygwin": "windows", "msys": "windows",
}
ARCH_ALIASES = {
    "x86_64": "x86_64", "amd64": "x86_64", "x64": "x86_64",
    "arm64": "arm64", "aarch64": "arm64",
}


class LegError(Exception):
    """A catalogue defect. Always fatal — never downgraded to a warning."""


def canon_os(name):
    key = (name or "").strip().lower()
    return OS_ALIASES.get(key, key or "unknown")


def canon_arch(name):
    key = (name or "").strip().lower()
    return ARCH_ALIASES.get(key, key or "unknown")


def detect_host_os():
    return canon_os(platform.system())


def detect_host_arch():
    return canon_arch(platform.machine())


def spec_target_arch(spec):
    """'arm64:elf64-aarch64-linux-exec' -> 'arm64'. The C++ twin is
    `specTargetArch()` in arm_verdict_ledger.hpp."""
    return spec.split(":", 1)[0] if ":" in spec else spec


def spec_format(spec):
    return spec.split(":", 1)[1] if ":" in spec else ""


def spec_target_os(spec):
    """'x86_64:pe64-x86_64-windows-exec' -> 'windows'.

    The shipped `*.format.json` names are `<container><bits>-<arch>-<os>-<kind>`,
    so the OS is the second-to-last token. Returns "" when the format name is not
    that shape — the caller LINTS that, rather than this guessing."""
    parts = spec_format(spec).split("-")
    return parts[-2] if len(parts) >= 3 else ""


# ── Staged third-party headers, per TARGET ──────────────────────────────────
#
# D-HARNESS-SQLITE-STAGE-ZCONF-IS-PE-SHAPED. The staged zlib header carries the
# DERIVING host's ./configure probe results; a leg declares what ITS target's
# answers are (`build.zconfGuards`), and the drivers stage one zinc/ per
# `recipeTransform`. The KEY is the transform name and nothing else, so the
# mapping is a property of the catalogue rather than of a branch in a driver.

def header_stage_key(leg):
    # `str()` because this becomes a DIRECTORY NAME in both drivers: a catalogue
    # that wrote a number here must not produce a path spelled differently in
    # python, bash and PowerShell.
    return str(leg.get("build", {}).get("recipeTransform", "none"))


def zconf_guards(leg):
    return dict(leg.get("build", {}).get("zconfGuards", {}))


def header_stages(legs):
    """key -> guards, in first-declared order.

    RAISES on a conflict: two legs sharing a recipeTransform but declaring
    DIFFERENT guards cannot share one staged zinc/, and silently picking either
    one would put a leg back where this whole anchor started — compiling against
    a header configured for somebody else's target."""
    stages, owner = {}, {}
    for leg in legs:
        key = header_stage_key(leg)
        guards = zconf_guards(leg)
        if key in stages and stages[key] != guards:
            raise LegError(
                "legs '%s' and '%s' both declare recipeTransform '%s' — so they "
                "share ONE staged zinc/ — but declare DIFFERENT zconfGuards "
                "(%r vs %r). One stage cannot be two headers; give them "
                "different recipeTransforms or reconcile the guards."
                % (owner[key], leg.get("label"), key, stages[key], guards))
        stages.setdefault(key, guards)
        owner.setdefault(key, leg.get("label"))
    return stages


# ── Catalogue ───────────────────────────────────────────────────────────────

def load_catalogue(path=CATALOGUE):
    if not os.path.isfile(path):
        raise LegError("leg catalogue not found: %s" % path)
    with open(path, "r", encoding="utf-8") as f:
        try:
            doc = json.load(f)
        except ValueError as exc:
            raise LegError("leg catalogue is not valid JSON (%s): %s" % (path, exc))
    if doc.get("dssHarnessLegsVersion") != 1:
        raise LegError("leg catalogue %s: unsupported dssHarnessLegsVersion %r "
                       "(this resolver understands 1)"
                       % (path, doc.get("dssHarnessLegsVersion")))
    legs = doc.get("legs")
    if not isinstance(legs, list) or not legs:
        raise LegError("leg catalogue %s declares no legs" % path)
    return legs


def expand_path(raw):
    """Expand `${env:NAME}` in a declared search path. An UNSET variable drops
    the whole candidate (returns None) rather than leaving a literal `${env:...}`
    on the search list, which would read as a real directory in a log."""
    out = raw
    while "${env:" in out:
        start = out.index("${env:")
        end = out.find("}", start)
        if end < 0:
            raise LegError("unterminated ${env:...} in search path %r" % raw)
        name = out[start + 6:end]
        value = os.environ.get(name)
        if not value:
            return None
        out = out[:start] + value + out[end + 1:]
    return out


def launcher_for(leg, host_os, host_arch):
    """The declared launcher entry for this (hostOs, hostArch), or None.
    `*` matches any. Order is significant: the first match wins, so a catalogue
    can put a specific entry ahead of a wildcard."""
    for entry in leg.get("launchers", []):
        want_os = entry.get("hostOs", "*")
        want_arch = entry.get("hostArch", "*")
        if want_os not in ("*", host_os):
            continue
        if want_arch not in ("*", host_arch):
            continue
        return entry
    return None


def launcher_available(command, available):
    """Is the launcher's argv[0] usable? `available` is None to consult PATH, or
    an explicit set (tests pin it so a plan is reproducible on any machine)."""
    exe = command[0]
    if available is not None:
        return exe in available
    return shutil.which(exe) is not None


# ── Path translation ────────────────────────────────────────────────────────

def path_translation(verb):
    """The declared verb's spec, or a LegError. Never a permissive default: an
    unknown verb that silently meant "none" would pass every path through
    untranslated, which is the exact failure this vocabulary exists to prevent."""
    spec = PATH_TRANSLATIONS.get(verb)
    if spec is None:
        raise LegError(
            "unknown pathTranslation %r (known: %s). A launcher declares the "
            "PATH NAMESPACE its argv lives in; an unrecognised verb cannot be "
            "silently treated as 'none', because 'none' is itself a claim — that "
            "the launcher takes this driver's paths verbatim."
            % (verb, ", ".join(sorted(PATH_TRANSLATIONS))))
    return spec


def looks_untranslated(verb, arg):
    """Does `arg` still look like a path in the namespace `verb` translates FROM?
    Always False for a verb that translates nothing."""
    shape = path_translation(verb)["sourceShape"]
    if not shape:
        return False
    predicate = FOREIGN_PATH_SHAPES.get(shape)
    if predicate is None:
        raise LegError("pathTranslation %r names foreign-path shape %r, which "
                       "nothing implements" % (verb, shape))
    return predicate(arg)


def translate_path(verb, raw, runner=None):
    """`raw`, spelled the way the launcher declaring `verb` addresses it.

    Fails loud on every step: an input that is not in the source namespace, a
    translator that is absent, a non-zero rc, empty output, or output that STILL
    looks like a source-namespace path. `runner` is injected by the self-test so
    the contract can be exercised without the translator being installed."""
    spec = path_translation(verb)
    if not spec["translator"]:
        return raw
    if not looks_untranslated(verb, raw):
        raise LegError(
            "pathTranslation '%s' was asked to translate %r, which is not a "
            "path in the namespace it translates FROM (%s). Translating it "
            "would resolve it against the translator's own working directory "
            "and produce a plausible-looking wrong answer."
            % (verb, raw, spec["sourceShape"]))
    text = raw
    sep = spec["separator"]
    if sep:
        text = text.replace(sep[0], sep[1])
    argv = list(spec["translator"]) + [text]
    if runner is None:
        runner = _run_translator
    rc, out, err = runner(argv)
    out = out.strip()
    if rc != 0 or not out:
        raise LegError(
            "pathTranslation '%s' FAILED for %r: `%s` exited %s%s. The launcher "
            "that declared this verb cannot be given a path at all, so the run "
            "must stop rather than hand it one the callee will misread as a "
            "relative filename."
            % (verb, raw, " ".join(argv), rc,
               (" — " + (err.strip() or out)) if (err.strip() or out) else ""))
    if looks_untranslated(verb, out):
        raise LegError(
            "pathTranslation '%s' returned %r for %r, which is STILL a %s path "
            "— the translator ran but did not translate."
            % (verb, out, raw, spec["sourceShape"]))
    return out


def _run_translator(argv):
    """(rc, stdout, stderr). rc is taken DIRECTLY off the process."""
    import subprocess  # local: nothing else in this resolver spawns anything.
    try:
        proc = subprocess.run(argv, capture_output=True, text=True)
    except OSError as exc:
        return 127, "", "%s" % exc
    return proc.returncode, proc.stdout, proc.stderr


def assert_translated(verb, args):
    """Every argument about to be handed to a launcher is in ITS namespace.

    The net under "translate at construction": a caller that adds a new
    path-valued argument and forgets to translate it gets a named refusal here
    instead of a test failure three hours in that looks like a fixture bug."""
    for i, arg in enumerate(args):
        if looks_untranslated(verb, arg):
            raise LegError(
                "argument %d of the launcher command line is still a %s path: "
                "%r. This launcher declares pathTranslation '%s', so every PATH "
                "in its argv must be translated first — a callee in the other "
                "namespace does not report this as a bad path, it opens a "
                "relative file by that name, misses, and the failure reads as a "
                "broken binary."
                % (i, path_translation(verb)["sourceShape"], arg, verb))


# ── The one decision ────────────────────────────────────────────────────────

def plan_leg(leg, host_os, host_arch, available):
    """Resolve ONE leg against ONE host.

    Build is unconditional. Run is answered from the leg's declaration.
    Returns a dict; `run.verdict` is populated ONLY for a non-run (it is one of
    the skip names) — a planned run has verdict None because the verdict is the
    OUTCOME (`ran` / `poisoned`) and only the driver can know it.
    """
    spec = leg["spec"]
    arch = spec_target_arch(spec)
    run_on = leg.get("runOn", [])
    os_ok = host_os in run_on
    arch_ok = host_arch == arch

    # `pathTranslation` is ALWAYS present and ALWAYS a declared verb — "none" for
    # a native run, because "this driver's paths are the ones the callee sees" is
    # a claim worth stating rather than an absence a driver has to interpret.
    run = {"mode": None, "launcher": [], "env": {}, "verdict": None, "detail": "",
           "pathTranslation": "none", "pathTranslator": [],
           "envTransfer": "inherit"}

    if os_ok and arch_ok:
        run["mode"] = "native"
        run["detail"] = ("host %s/%s matches runOn=[%s] and target arch %s"
                         % (host_os, host_arch, ",".join(run_on), arch))
    else:
        entry = launcher_for(leg, host_os, host_arch)
        if entry is not None:
            command = list(entry.get("command", []))
            if not command:
                raise LegError("leg '%s' declares a launcher for (%s, %s) with an "
                               "EMPTY command — absence of an entry is how this "
                               "catalogue spells 'no launcher exists'; an empty "
                               "command is neither a declaration nor a denial"
                               % (leg["label"], entry.get("hostOs"),
                                  entry.get("hostArch")))
            # The launcher's PATH NAMESPACE, declared by the entry. A verb the
            # resolver does not know raises rather than degrading to "none":
            # "none" means the launcher takes this driver's paths VERBATIM, and
            # guessing that for a launcher that does not is the whole defect.
            verb = entry.get("pathTranslation", "none")
            xlate = path_translation(verb)
            translator = list(xlate["translator"])
            # The launcher's ENVIRONMENT namespace, declared beside its path
            # namespace. Same refusal on an unknown verb, same reason.
            env_verb = entry.get("envTransfer", "inherit")
            env_transfer(env_verb)
            # A launcher is USABLE only if its translator is too. They are one
            # capability: `wsl.exe` present with no distro behind it resolves
            # neither, and the honest verdict for that machine is the
            # environmental skip, not a run that dies on its first path.
            needed = [command] + ([translator] if translator else [])
            missing = [c for c in needed if not launcher_available(c, available)]
            if not missing:
                run["mode"] = "launched"
                run["launcher"] = command
                run["env"] = dict(entry.get("env", {}))
                run["pathTranslation"] = verb
                run["pathTranslator"] = translator
                run["envTransfer"] = env_verb
                run["detail"] = ("host %s/%s cannot run %s natively; declared "
                                 "launcher '%s' is available%s"
                                 % (host_os, host_arch, spec, " ".join(command),
                                    "" if not translator else
                                    " (paths translated into its namespace by "
                                    "'%s', pathTranslation '%s')"
                                    % (" ".join(translator), verb)))
            else:
                run["mode"] = "skip"
                run["verdict"] = "skipped-emulator-missing"
                run["detail"] = ("declared launcher '%s' for host %s/%s is not "
                                 "usable: %s not on PATH — install it (or set "
                                 "DSS_STRICT_ARM_VERDICTS=1 to make this a hard "
                                 "failure)"
                                 % (" ".join(command), host_os, host_arch,
                                    " and ".join(c[0] for c in missing)))
        elif os_ok:
            # Same OS, different arch, nothing declared. The corpus's
            # SkippedNoEmulatorDeclared, meaning-for-meaning.
            run["mode"] = "skip"
            run["verdict"] = "skipped-no-emulator-declared"
            run["detail"] = ("host arch %s differs from target arch %s and the "
                             "catalogue declares no launcher for (%s, %s)"
                             % (host_arch, arch, host_os, host_arch))
        else:
            run["mode"] = "skip"
            run["verdict"] = "skipped-by-runOn"
            run["detail"] = ("runOn=[%s] excludes host OS %s and no launcher is "
                             "declared for (%s, %s)"
                             % (",".join(run_on), host_os, host_os, host_arch))

    build = dict(leg.get("build", {}))
    libs = dict(build.get("libraries", {}))
    paths = []
    for raw in libs.get("searchPaths", []):
        expanded = expand_path(raw)
        if expanded:
            paths.append(expanded)
    libs["searchPaths"] = paths
    build["libraries"] = libs

    return {
        "label": leg["label"],
        "spec": spec,
        "targetArch": arch,
        "format": spec_format(spec),
        "runOn": list(run_on),
        # ★ UNCONDITIONAL. This key exists so that a reader (and a diff) can see
        # that no host makes it false. If you ever find yourself writing
        # `"attempt": <expression>` here, you are re-locking the harness.
        # `headerStageKey` is DERIVED here so a driver never has to spell the
        # recipeTransform -> zinc/ mapping itself; it reads the key and joins it
        # onto the stage root. Host-free, like everything else in this dict.
        "build": dict(build, attempt=True, headerStageKey=header_stage_key(leg)),
        "run": run,
    }


def plan(host_os, host_arch, available, path=CATALOGUE):
    legs = load_catalogue(path)
    return {
        "host": {"os": host_os, "arch": host_arch},
        "legs": [plan_leg(leg, host_os, host_arch, available) for leg in legs],
    }


# ── Emitters ────────────────────────────────────────────────────────────────

def emit_sh(resolved):
    """Shell assignments for build-and-test.sh's `eval`. The caller has already
    `declare -A`'d every array. Everything is shlex-quoted, and the emitter
    writes ONLY assignments — no commands — so an eval of this text cannot do
    anything but populate variables."""
    out = []
    q = shlex.quote
    labels = [leg["label"] for leg in resolved["legs"]]
    out.append("LEG_ORDER=(%s)" % " ".join(q(x) for x in labels))
    for leg in resolved["legs"]:
        lbl = leg["label"]
        b = leg["build"]
        libs = b.get("libraries", {})
        run = leg["run"]

        def put(name, value):
            out.append("%s[%s]=%s" % (name, q(lbl), q(value)))

        put("LEG_SPEC", leg["spec"])
        put("LEG_FORMAT", leg["format"])
        put("LEG_ARCH", leg["targetArch"])
        put("LEG_RUN_MODE", run["mode"])
        put("LEG_RUN_VERDICT", run["verdict"] or "")
        put("LEG_RUN_DETAIL", run["detail"])
        put("LEG_LAUNCH", " ".join(q(x) for x in run["launcher"]))
        put("LEG_LAUNCH_ENV",
            " ".join("%s=%s" % (k, q(v)) for k, v in sorted(run["env"].items())))
        # The launcher's PATH NAMESPACE + the argv that maps into it. The driver
        # never performs the translation itself — it calls this resolver back
        # (`--translate-path`), so `wslpath` is named in exactly one file. The
        # translator argv is emitted anyway so a build log can state, per leg,
        # HOW its paths were translated.
        put("LEG_PATH_TRANSLATION", run["pathTranslation"])
        put("LEG_PATH_TRANSLATOR", " ".join(q(x) for x in run["pathTranslator"]))
        # How the driver's run environment reaches the launched process. Same
        # story as the path namespace: the driver reads the VERB and asks this
        # resolver to turn it into assignments (`--env-transfer`).
        put("LEG_ENV_TRANSFER", run["envTransfer"])
        put("LEG_RECIPE_TRANSFORM", b.get("recipeTransform", "none"))
        # The leg's OWN staged-header directory name + the guards that made it.
        # The driver joins the key onto its zinc/ root; it never decides the
        # mapping and never reads the guards (stage-zinc.py applies those) — the
        # guard string is emitted so a build log states, per leg, WHICH header
        # configuration that leg was compiled against.
        put("LEG_HEADER_STAGE_KEY", b.get("headerStageKey", "none"))
        put("LEG_ZCONF_GUARDS",
            " ".join("%s=%d" % (k, 1 if v else 0)
                     for k, v in sorted(b.get("zconfGuards", {}).items())))
        put("LEG_STACK_RESERVE", str(b.get("stackReserveBytes", 0)))
        put("LEG_SHARED_FLAGS", " ".join(b.get("sharedLibFlags", [])))
        put("LEG_CC_CANDIDATES", " ".join(b.get("targetCc", {}).get("candidates", [])))
        put("LEG_CC_PKG", b.get("targetCc", {}).get("package", ""))
        put("LEG_LIB_PROVIDER", libs.get("provider", ""))
        put("LEG_LIB_TCL_NAMES", " ".join(libs.get("tclNames", [])))
        put("LEG_LIB_Z_NAMES", " ".join(libs.get("zNames", [])))
        put("LEG_LIB_PATHS", "\n".join(libs.get("searchPaths", [])))
    return "\n".join(out) + "\n"


# A statement the sh emitter is allowed to produce: NAME=… or NAME[key]=… .
ASSIGNMENT_RE = re.compile(r"^[A-Z][A-Z0-9_]*(\[[^\]]*\])?=")


def sh_statements(text):
    """Split emitted shell text into STATEMENTS, honouring shlex.quote's single
    quotes. A value may legitimately contain newlines (search paths are
    newline-separated so a path with spaces survives), so a naive splitlines()
    would see a quoted path as its own statement — which is precisely the
    misreading that would let a real injected command hide behind one."""
    statements, current, in_quote = [], [], False
    for ch in text:
        if ch == "'":
            in_quote = not in_quote
            current.append(ch)
        elif ch == "\n" and not in_quote:
            statements.append("".join(current))
            current = []
        else:
            current.append(ch)
    tail = "".join(current)
    if tail:
        statements.append(tail)
    if in_quote:
        raise LegError("the sh emitter produced an unterminated quote")
    return [s for s in statements if s]


# ── Lint ────────────────────────────────────────────────────────────────────

def lint(path=CATALOGUE):
    """Catalogue defects, host-independently. Returns a list of strings."""
    findings = []
    legs = load_catalogue(path)
    seen_labels, seen_specs = {}, {}
    # (targetArch, hostOs, hostArch) -> set of launcher spellings. One triple
    # must have ONE vocabulary, for the same reason lintDeclaredEmulators gives:
    # with two candidates, an omitting leg cannot be told what it is missing.
    vocab = {}
    for leg in legs:
        for key in ("label", "spec", "runOn", "launchers", "build"):
            if key not in leg:
                findings.append("leg %r: missing required key '%s'"
                                % (leg.get("label", "<unlabelled>"), key))
        label = leg.get("label", "<unlabelled>")
        spec = leg.get("spec", "")
        if ":" not in spec:
            findings.append("leg '%s': spec %r is not <targetName>:<formatName>"
                            % (label, spec))
        if label in seen_labels:
            findings.append("duplicate leg label '%s'" % label)
        seen_labels[label] = True
        if spec in seen_specs:
            findings.append("legs '%s' and '%s' declare the same spec '%s'"
                            % (seen_specs[spec], label, spec))
        seen_specs[spec] = label
        if not leg.get("runOn"):
            findings.append("leg '%s': runOn is empty — no host could ever run "
                            "it and nothing says why" % label)
        arch = spec_target_arch(spec)
        for entry in leg.get("launchers", []):
            if "hostOs" not in entry or "hostArch" not in entry:
                findings.append("leg '%s': a launcher entry omits hostOs/hostArch"
                                % label)
                continue
            cmd = entry.get("command", [])
            if not isinstance(cmd, list) or not cmd or not all(cmd):
                findings.append("leg '%s': launcher for (%s, %s) has an empty or "
                                "malformed command %r — absence of an entry is "
                                "how 'no launcher exists' is spelled"
                                % (label, entry["hostOs"], entry["hostArch"], cmd))
                continue
            # ── the launcher's PATH NAMESPACE ──────────────────────────────
            # Declared on EVERY entry, explicitly, for the same reason every
            # zconf guard is: a reader must never have to know a default, and
            # "this launcher takes my paths verbatim" is a claim, not a silence.
            if "pathTranslation" not in entry:
                findings.append(
                    "leg '%s': launcher for (%s, %s) declares no "
                    "pathTranslation. Every launcher states the PATH NAMESPACE "
                    "its argv lives in — 'none' when it takes this driver's "
                    "paths verbatim (Wine, qemu), a named verb when it does not "
                    "(known: %s). Omitting it is how a launcher comes to be "
                    "handed a path its callee reads as a relative filename."
                    % (label, entry["hostOs"], entry["hostArch"],
                       ", ".join(sorted(PATH_TRANSLATIONS))))
            else:
                verb = entry["pathTranslation"]
                if verb not in PATH_TRANSLATIONS:
                    findings.append(
                        "leg '%s': launcher for (%s, %s) declares unknown "
                        "pathTranslation %r (known: %s) — a verb nothing "
                        "implements is a declaration that reads as "
                        "configuration and is not"
                        % (label, entry["hostOs"], entry["hostArch"], verb,
                           ", ".join(sorted(PATH_TRANSLATIONS))))
                else:
                    # DERIVABLE, so it is checked rather than trusted: a path
                    # namespace belongs to a host OS. `windows-to-wsl` on a
                    # Linux host would translate a path shape that host cannot
                    # produce, using a tool it does not have.
                    want_os = PATH_TRANSLATIONS[verb]["validHostOs"]
                    if want_os and entry["hostOs"] != want_os:
                        findings.append(
                            "leg '%s': launcher for (%s, %s) declares "
                            "pathTranslation '%s', whose source namespace only "
                            "exists on a '%s' host — so on this host there is "
                            "no such path to translate and no translator to do "
                            "it with"
                            % (label, entry["hostOs"], entry["hostArch"], verb,
                               want_os))
            # ── the launcher's ENVIRONMENT namespace ───────────────────────
            # Declared on EVERY entry too. A launcher whose child cannot see the
            # driver's run environment does not fail — it runs with an EMPTY one,
            # which is how a resume engine came to re-run a whole corpus.
            if "envTransfer" not in entry:
                findings.append(
                    "leg '%s': launcher for (%s, %s) declares no envTransfer. "
                    "Every launcher states how the driver's run environment "
                    "reaches the process it spawns — 'inherit' when the child "
                    "gets this driver's environment block (qemu, Wine, arch), a "
                    "named verb when it does not (known: %s)."
                    % (label, entry["hostOs"], entry["hostArch"],
                       ", ".join(sorted(ENV_TRANSFERS))))
            else:
                everb = entry["envTransfer"]
                if everb not in ENV_TRANSFERS:
                    findings.append(
                        "leg '%s': launcher for (%s, %s) declares unknown "
                        "envTransfer %r (known: %s)"
                        % (label, entry["hostOs"], entry["hostArch"], everb,
                           ", ".join(sorted(ENV_TRANSFERS))))
                else:
                    want_os = ENV_TRANSFERS[everb]["validHostOs"]
                    if want_os and entry["hostOs"] != want_os:
                        findings.append(
                            "leg '%s': launcher for (%s, %s) declares "
                            "envTransfer '%s', whose carrier only exists on a "
                            "'%s' host"
                            % (label, entry["hostOs"], entry["hostArch"], everb,
                               want_os))
            if entry["hostOs"] in leg.get("runOn", []) and entry["hostArch"] == arch:
                findings.append("leg '%s': launcher declared for (%s, %s), which "
                                "is this leg's NATIVE host — dead config: the "
                                "resolver never consults it" % (label, entry["hostOs"],
                                                                entry["hostArch"]))
            key = (arch, entry["hostOs"], entry["hostArch"])
            vocab.setdefault(key, set()).add(" ".join(cmd))
        build = leg.get("build", {})
        transform = build.get("recipeTransform")
        if transform not in RECIPE_TRANSFORMS:
            findings.append("leg '%s': unknown recipeTransform %r (known: %s)"
                            % (label, transform, ", ".join(sorted(RECIPE_TRANSFORMS))))
        # ── the staged zlib header this leg compiles against ──────────────────
        # Every guard, declared explicitly, with a value that is a BOOLEAN and
        # not a string: `"false"` is truthy in every language a driver here is
        # written in, and a guard that silently reads as "on" is exactly the
        # defect this key exists to end.
        guards = build.get("zconfGuards")
        target_os = spec_target_os(spec)
        if target_os not in TARGET_OS_NAMES:
            findings.append("leg '%s': cannot derive a target OS from spec %r "
                            "(format names are <container><bits>-<arch>-<os>-<kind>; "
                            "got %r) — so the staged-header declaration below "
                            "cannot be cross-checked against the target"
                            % (label, spec, target_os))
        if not isinstance(guards, dict):
            findings.append("leg '%s': missing build.zconfGuards — every leg must "
                            "declare, for ITS OWN target, the value of each zconf.h "
                            "guard ./configure may have set on the DERIVING host "
                            "(%s). Omitting it is how one pe-shaped zinc/ came to "
                            "serve every leg." % (label, ", ".join(ZCONF_GUARDS)))
        else:
            for name in sorted(set(guards) - set(ZCONF_GUARDS)):
                findings.append("leg '%s': unknown zconf guard %r (known: %s) — a "
                                "guard nothing applies is a declaration that reads "
                                "as configuration and is not"
                                % (label, name, ", ".join(ZCONF_GUARDS)))
            for name in ZCONF_GUARDS:
                if name not in guards:
                    findings.append("leg '%s': build.zconfGuards omits %r — every "
                                    "guard is declared for every leg, so a reader "
                                    "never has to know a default" % (label, name))
                elif not isinstance(guards[name], bool):
                    findings.append("leg '%s': zconf guard %r is %r, not a JSON "
                                    "boolean — a string is truthy in bash, "
                                    "PowerShell and python alike"
                                    % (label, name, guards[name]))
            for name in POSIX_ONLY_ZCONF_GUARDS:
                want = target_os in ("linux", "darwin")
                if target_os in TARGET_OS_NAMES and guards.get(name) is not want:
                    findings.append(
                        "leg '%s': targets OS '%s' but declares %s=%r. That guard "
                        "governs `#include <unistd.h>`, which exists on POSIX and "
                        "not on Windows, so its correct value is DERIVABLE from the "
                        "target (%r) — a declaration that disagrees with the target "
                        "would stage a header configured for a different machine, "
                        "which is the whole defect this key closes."
                        % (label, target_os, name, guards.get(name), want))
        libs = build.get("libraries", {})
        provider = libs.get("provider")
        if provider not in LIBRARY_PROVIDERS:
            findings.append("leg '%s': unknown library provider %r (known: %s)"
                            % (label, provider, ", ".join(sorted(LIBRARY_PROVIDERS))))
        if provider == "search-paths" and not libs.get("searchPaths"):
            findings.append("leg '%s': provider 'search-paths' with no searchPaths"
                            % label)
        if provider != "search-paths" and libs.get("searchPaths"):
            findings.append("leg '%s': provider %r ignores searchPaths, but some "
                            "are declared — a reader would think they are used"
                            % (label, provider))
        if not libs.get("tclNames") or not libs.get("zNames"):
            findings.append("leg '%s': libraries declare no tclNames/zNames" % label)
        if not build.get("targetCc", {}).get("candidates"):
            findings.append("leg '%s': no targetCc candidates — the corpus's "
                            "dlopen()ed helper extension could not be built for it"
                            % label)
        if not build.get("sharedLibFlags"):
            findings.append("leg '%s': no sharedLibFlags" % label)
    # One staged zinc/ per recipeTransform is only sound if every leg sharing a
    # transform wants the SAME header. header_stages() raises on a conflict; here
    # that is a finding rather than a crash, so `--lint` reports it beside the
    # others instead of dying on the first one.
    try:
        header_stages(legs)
    except LegError as exc:
        findings.append("%s" % exc)
    for key, spellings in sorted(vocab.items()):
        if len(spellings) > 1:
            findings.append("(targetArch=%s, hostOs=%s, hostArch=%s) has %d "
                            "launcher spellings: %s — one triple must have one "
                            "vocabulary" % (key[0], key[1], key[2], len(spellings),
                                            ", ".join(sorted(spellings))))
    return findings


# ── Self-test ───────────────────────────────────────────────────────────────
#
# Runs at the START of both drivers (their Step 0), for the same reason the
# confound-classifier self-test does: a defect in the leg plan is otherwise
# invisible until a leg silently fails to appear, and by then hours have been
# spent. The load-bearing assertion is HOST-INVARIANCE OF THE BUILD SET.

# Every host this project has ever run a leg on, plus two it has not (the
# unknown-arch and unknown-OS rows). A host the resolver does not recognise must
# still produce a full build set — falling back to "build nothing" would be the
# host-locking defect wearing a different hat.
SELF_TEST_HOSTS = [
    ("linux", "x86_64"), ("linux", "arm64"),
    ("windows", "x86_64"), ("windows", "arm64"),
    ("darwin", "arm64"), ("darwin", "x86_64"),
    ("unknown", "x86_64"), ("linux", "riscv64"), ("unknown", "unknown"),
]

# The RUN oracle: hand-written, per (host, leg), so the self-test is not the
# resolver's ladder restated. `native` / `launched:<cmd>` / a verdict name.
# ★ Derived by reading legs.json BY HAND. If you change a leg's runOn or its
# launchers, this table must be updated deliberately — that friction is the
# point: a silent change to who can execute what is exactly the class of edit
# that must not sail through.
RUN_ORACLE = {
    ("linux", "x86_64"): {
        "elf64-x86_64": "native",
        "elf64-arm64": "launched:qemu-aarch64",
        "pe64-x86_64": "launched:wine",
        "macho64-arm64": "skipped-by-runOn",
        "macho64-x86_64": "skipped-by-runOn",
    },
    ("linux", "arm64"): {
        "elf64-x86_64": "launched:qemu-x86_64",
        "elf64-arm64": "native",
        "pe64-x86_64": "skipped-by-runOn",
        "macho64-arm64": "skipped-by-runOn",
        "macho64-x86_64": "skipped-by-runOn",
    },
    ("windows", "x86_64"): {
        # D-HARNESS-NO-WSL-LAUNCHER-FOR-ELF-ON-WINDOWS: this row used to read
        # `skipped-by-runOn` for elf64-x86_64 while the driver had a working
        # Linux testfixture sitting on disk beside the verdict.
        "elf64-x86_64": "launched:wsl.exe",
        "elf64-arm64": "skipped-by-runOn",
        "pe64-x86_64": "native",
        "macho64-arm64": "skipped-by-runOn",
        "macho64-x86_64": "skipped-by-runOn",
    },
    ("windows", "arm64"): {
        # WSL2 runs the HOST's architecture, so an arm64 Windows box reaches the
        # arm64 Linux leg and not the x86_64 one.
        "elf64-x86_64": "skipped-by-runOn",
        "elf64-arm64": "launched:wsl.exe",
        # Same OS, different arch, no launcher declared for (windows, arm64).
        "pe64-x86_64": "skipped-no-emulator-declared",
        "macho64-arm64": "skipped-by-runOn",
        "macho64-x86_64": "skipped-by-runOn",
    },
    ("darwin", "arm64"): {
        "elf64-x86_64": "skipped-by-runOn",
        "elf64-arm64": "skipped-by-runOn",
        "pe64-x86_64": "skipped-by-runOn",
        "macho64-arm64": "native",
        "macho64-x86_64": "launched:arch",
    },
    ("darwin", "x86_64"): {
        "elf64-x86_64": "skipped-by-runOn",
        "elf64-arm64": "skipped-by-runOn",
        "pe64-x86_64": "launched:wine",
        # No off-Mac-arch launcher for an arm64 Mach-O, and none exists.
        "macho64-arm64": "skipped-no-emulator-declared",
        "macho64-x86_64": "native",
    },
    ("unknown", "x86_64"): {
        "elf64-x86_64": "skipped-by-runOn",
        "elf64-arm64": "skipped-by-runOn",
        "pe64-x86_64": "skipped-by-runOn",
        "macho64-arm64": "skipped-by-runOn",
        "macho64-x86_64": "skipped-by-runOn",
    },
    ("linux", "riscv64"): {
        "elf64-x86_64": "skipped-no-emulator-declared",
        "elf64-arm64": "skipped-no-emulator-declared",
        "pe64-x86_64": "skipped-by-runOn",
        "macho64-arm64": "skipped-by-runOn",
        "macho64-x86_64": "skipped-by-runOn",
    },
    ("unknown", "unknown"): {
        "elf64-x86_64": "skipped-by-runOn",
        "elf64-arm64": "skipped-by-runOn",
        "pe64-x86_64": "skipped-by-runOn",
        "macho64-arm64": "skipped-by-runOn",
        "macho64-x86_64": "skipped-by-runOn",
    },
}


def _raises(thunk):
    """Did `thunk` refuse LOUDLY? Only a LegError counts — an AttributeError or a
    TypeError is a bug in the resolver, not the refusal under test."""
    try:
        thunk()
    except LegError:
        return True
    return False


def _forbidden_runner(argv):
    """A translator that must never be reached. Raises a non-LegError on purpose,
    so `_raises` cannot mistake "it spawned something" for "it refused"."""
    raise AssertionError("a translator was invoked when none should have been: %r"
                         % (argv,))


def self_test(path=CATALOGUE, out=sys.stdout):
    passed = failed = 0

    def check(name, ok, detail=""):
        nonlocal passed, failed
        if ok:
            passed += 1
        else:
            failed += 1
            out.write("FAIL: %s%s\n" % (name, ("\n      " + detail) if detail else ""))

    findings = lint(path)
    check("the leg catalogue lints clean", not findings, "\n      ".join(findings))

    legs = load_catalogue(path)
    labels = [leg["label"] for leg in legs]

    # ★ THE LOAD-BEARING ASSERTION. Every host, every launcher availability,
    # the SAME build set in the SAME order. This is D-HARNESS-CROSS-HOST-ANY-
    # TARGET item (2) expressed as an executable property: if a future edit
    # makes the leg list depend on the host in ANY way, this reds.
    for host in SELF_TEST_HOSTS:
        for available in (None, set(),
                          {"qemu-aarch64", "qemu-x86_64", "wine", "arch", "wsl.exe"}):
            resolved = plan(host[0], host[1], available, path)
            got = [leg["label"] for leg in resolved["legs"]]
            check("build set is host-invariant on %s/%s (launchers=%s)"
                  % (host[0], host[1],
                     "PATH" if available is None else sorted(available)),
                  got == labels,
                  "expected %r, got %r" % (labels, got))
            check("every leg's build is attempted on %s/%s" % host,
                  all(leg["build"]["attempt"] for leg in resolved["legs"]))
            for leg in resolved["legs"]:
                v = leg["run"]["verdict"]
                check("run verdict on %s/%s for %s is in the closed vocabulary"
                      % (host[0], host[1], leg["label"]),
                      v is None or v in VERDICTS, "got %r" % v)
                check("a planned run carries no verdict, a skip carries one "
                      "(%s/%s %s)" % (host[0], host[1], leg["label"]),
                      (leg["run"]["mode"] == "skip") == (v is not None),
                      "mode=%r verdict=%r" % (leg["run"]["mode"], v))
                check("every leg outcome carries a reason (%s/%s %s)"
                      % (host[0], host[1], leg["label"]),
                      bool(leg["run"]["detail"]))

    # The RUN oracle — hand-written expectations, all launchers present.
    every = {"qemu-aarch64", "qemu-x86_64", "wine", "arch", "wsl.exe"}
    for host, expectations in RUN_ORACLE.items():
        resolved = plan(host[0], host[1], every, path)
        check("the run oracle covers every declared leg on %s/%s" % host,
              sorted(expectations) == sorted(labels),
              "oracle=%r legs=%r" % (sorted(expectations), sorted(labels)))
        for leg in resolved["legs"]:
            want = expectations.get(leg["label"])
            run = leg["run"]
            if want == "native":
                got = run["mode"]
            elif want and want.startswith("launched:"):
                got = "launched:" + (run["launcher"][0] if run["launcher"] else "")
            else:
                got = run["verdict"]
            check("run plan on %s/%s for %s" % (host[0], host[1], leg["label"]),
                  got == want, "expected %r, got %r (mode=%r)"
                  % (want, got, run["mode"]))

    # An UNAVAILABLE launcher must degrade to the environmental skip and to
    # nothing else — in particular it must not change the build set (asserted
    # above) and must not silently become a structural skip, which strict mode
    # would then refuse to act on.
    for host, expectations in RUN_ORACLE.items():
        resolved = plan(host[0], host[1], set(), path)
        for leg in resolved["legs"]:
            if not (expectations.get(leg["label"]) or "").startswith("launched:"):
                continue
            check("a missing launcher is ENVIRONMENTAL on %s/%s for %s"
                  % (host[0], host[1], leg["label"]),
                  leg["run"]["verdict"] == "skipped-emulator-missing",
                  "got %r" % leg["run"]["verdict"])

    # ── the launcher's PATH NAMESPACE ────────────────────────────────────────
    # D-HARNESS-NO-WSL-LAUNCHER-FOR-ELF-ON-WINDOWS. Three properties, and the
    # third is the one that would have caught the original defect: the verb is
    # the LAUNCHER's, so it must be the same verb whichever leg declared that
    # launcher and whichever host is asking.
    for host in SELF_TEST_HOSTS:
        resolved = plan(host[0], host[1], every, path)
        for leg in resolved["legs"]:
            run = leg["run"]
            verb = run.get("pathTranslation")
            check("run plan carries a DECLARED pathTranslation (%s/%s %s)"
                  % (host[0], host[1], leg["label"]),
                  verb in PATH_TRANSLATIONS, "got %r" % verb)
            check("a non-launched run translates nothing (%s/%s %s)"
                  % (host[0], host[1], leg["label"]),
                  run["mode"] == "launched" or verb == "none",
                  "mode=%r verb=%r" % (run["mode"], verb))
            check("the translator argv IS the verb's (%s/%s %s)"
                  % (host[0], host[1], leg["label"]),
                  run.get("pathTranslator")
                  == (PATH_TRANSLATIONS[verb]["translator"]
                      if run["mode"] == "launched" else []),
                  "verb=%r translator=%r" % (verb, run.get("pathTranslator")))
    # A launcher that needs a translator is UNUSABLE without it, and the verdict
    # for that is the environmental skip — not a run that dies on its first path.
    translating = [(h, leg["label"])
                   for h in SELF_TEST_HOSTS
                   for leg in plan(h[0], h[1], every, path)["legs"]
                   if leg["run"].get("pathTranslator")]
    check("at least one host/leg cell exercises a translating launcher",
          bool(translating),
          "no leg in this catalogue declares a pathTranslation with a "
          "translator, so every assertion above is vacuous")
    for host, label in translating:
        # Everything present EXCEPT the translator's own argv[0].
        # ⚠ HONEST LABEL: for every verb this catalogue ships, the translator's
        # argv[0] IS the launcher's argv[0] (`wsl.exe`), so removing one removes
        # both and this cannot separate "launcher missing" from "translator
        # missing". What it DOES pin is that a translating launcher whose tool is
        # absent degrades to the ENVIRONMENTAL skip rather than to a planned run
        # that dies on its first path. The separation becomes testable the day a
        # verb names a translator distinct from its launcher.
        translator_exes = {t["translator"][0] for t in PATH_TRANSLATIONS.values()
                           if t["translator"]}
        without = every - translator_exes
        for leg in plan(host[0], host[1], without, path)["legs"]:
            if leg["label"] != label:
                continue
            check("a translating launcher whose TOOLING is absent is "
                  "environmental, not a silent run (%s/%s %s)"
                  % (host[0], host[1], label),
                  leg["run"]["verdict"] == "skipped-emulator-missing",
                  "got mode=%r verdict=%r"
                  % (leg["run"]["mode"], leg["run"]["verdict"]))

    # The translation CONTRACT, exercised with an injected translator so it holds
    # on a machine that has none. Each case is a way the mechanism can go wrong
    # QUIETLY, which is why each one raises instead of returning something.
    check("an unknown pathTranslation verb raises rather than meaning 'none'",
          _raises(lambda: translate_path("windows-to-posix", "C:/x")))
    check("'none' is the identity",
          translate_path("none", "C:\\a\\b") == "C:\\a\\b")
    check("'none' never calls a translator",
          translate_path("none", "/x", runner=_forbidden_runner) == "/x")
    seen = []

    def _ok_runner(argv):
        seen.append(list(argv))
        return 0, "/mnt/c/a/b\n", ""

    check("windows-to-wsl normalises the separator BEFORE the translator",
          translate_path("windows-to-wsl", "C:\\a\\b", runner=_ok_runner)
          == "/mnt/c/a/b" and seen and seen[0][-1] == "C:/a/b",
          "translator saw %r" % (seen[0] if seen else None))
    check("windows-to-wsl invokes the DECLARED translator argv",
          bool(seen) and seen[0][:-1]
          == PATH_TRANSLATIONS["windows-to-wsl"]["translator"],
          "argv=%r" % (seen[0] if seen else None))
    check("a translator that exits non-zero is FATAL, not a passthrough",
          _raises(lambda: translate_path("windows-to-wsl", "C:/a",
                                         runner=lambda a: (1, "", "boom"))))
    check("a translator that prints NOTHING is FATAL",
          _raises(lambda: translate_path("windows-to-wsl", "C:/a",
                                         runner=lambda a: (0, "  \n", ""))))
    check("a translator that returns the SOURCE spelling is FATAL",
          _raises(lambda: translate_path("windows-to-wsl", "C:/a",
                                         runner=lambda a: (0, "C:/a\n", ""))))
    check("a path NOT in the source namespace is FATAL, never resolved "
          "against the translator's own cwd",
          _raises(lambda: translate_path("windows-to-wsl", "relative/x",
                                         runner=_forbidden_runner)))
    # The net under "translate at construction".
    check("assert_translated passes a fully-translated argv",
          not _raises(lambda: assert_translated(
              "windows-to-wsl", ["/mnt/c/f/testfixture", "/mnt/c/t/x.test",
                                 "--start=full:"])))
    for bad in ("C:\\t\\x.test", "c:/t/x.test", "--testdir=D:\\t",
                "\\\\server\\share\\x"):
        check("assert_translated CATCHES an untranslated %r" % bad,
              _raises(lambda b=bad: assert_translated("windows-to-wsl",
                                                      ["/mnt/c/f", b])))
    check("assert_translated is a no-op for a non-translating verb",
          not _raises(lambda: assert_translated("none", ["C:\\t\\x.test"])))

    # ── the launcher's ENVIRONMENT namespace ─────────────────────────────────
    # The half that was found by measuring the first: a wsl.exe-launched fixture
    # saw an EMPTY SQLITE_TEST_PATTERN_LIST, so the corpus resume engine re-ran
    # the whole corpus instead of the tail after the abort.
    for host in SELF_TEST_HOSTS:
        for leg in plan(host[0], host[1], every, path)["legs"]:
            run = leg["run"]
            everb = run.get("envTransfer")
            check("run plan carries a DECLARED envTransfer (%s/%s %s)"
                  % (host[0], host[1], leg["label"]),
                  everb in ENV_TRANSFERS, "got %r" % everb)
            check("a non-launched run inherits (%s/%s %s)"
                  % (host[0], host[1], leg["label"]),
                  run["mode"] == "launched" or everb == "inherit",
                  "mode=%r verb=%r" % (run["mode"], everb))
    check("an unknown envTransfer verb raises rather than meaning 'inherit'",
          _raises(lambda: env_transfer("copy-the-block")))
    check("'inherit' needs no assignments",
          env_carrier_assignments("inherit", ["A", "B"]) == [])
    check("'wslenv' names each forwarded variable in its carrier",
          env_carrier_assignments("wslenv", ["A", "B"]) == ["WSLENV=A:B"])
    check("'wslenv' with nothing to forward assigns nothing",
          env_carrier_assignments("wslenv", []) == [])
    check("an operator's existing carrier value is MERGED, not clobbered",
          env_carrier_assignments("wslenv", ["B"], "A/u") == ["WSLENV=A/u:B"])
    check("a variable already carried (even with a /flag) is not duplicated",
          env_carrier_assignments("wslenv", ["A"], "A/u") == ["WSLENV=A/u"])
    check("an unknown envTransfer verb raises from the assignment path too",
          _raises(lambda: env_carrier_assignments("nope", ["A"])))
    # At least one leg/host cell must actually need a carrier, or every
    # assertion above is about a mechanism this catalogue never reaches.
    check("some declared launcher needs a non-inherit envTransfer",
          any(leg["run"].get("envTransfer") not in (None, "inherit")
              for h in SELF_TEST_HOSTS
              for leg in plan(h[0], h[1], every, path)["legs"]))

    # ── the staged-header plan ───────────────────────────────────────────────
    # HOST-INVARIANT for exactly the reason the build set is: which zlib header a
    # leg compiles against is a fact about that leg's TARGET. If this ever starts
    # varying by host, a leg is being configured by the machine again.
    stages = header_stages(legs)
    check("one staged header stage per distinct recipeTransform",
          sorted(stages) == sorted({leg["build"]["recipeTransform"] for leg in legs}),
          "stages=%r transforms=%r" % (sorted(stages),
                                       sorted({leg["build"]["recipeTransform"] for leg in legs})))
    check("more than one header stage is declared", len(stages) > 1,
          "only %r — with a single stage this whole mechanism is untested by the "
          "catalogue it ships with" % sorted(stages))
    for host in SELF_TEST_HOSTS:
        resolved = plan(host[0], host[1], set(), path)
        for leg in resolved["legs"]:
            key = leg["build"]["headerStageKey"]
            check("header stage key is host-invariant on %s/%s for %s"
                  % (host[0], host[1], leg["label"]),
                  key == leg["build"]["recipeTransform"],
                  "key=%r transform=%r" % (key, leg["build"]["recipeTransform"]))
            check("the leg's header stage exists in the stage plan (%s/%s %s)"
                  % (host[0], host[1], leg["label"]), key in stages)
            check("the leg's declared guards ARE its stage's guards (%s/%s %s)"
                  % (host[0], host[1], leg["label"]),
                  stages.get(key) == leg["build"].get("zconfGuards"),
                  "stage=%r leg=%r" % (stages.get(key),
                                       leg["build"].get("zconfGuards")))
    # Two legs with DIFFERENT transforms must not share a stage — otherwise the
    # per-target staging is per-target in name only.
    by_key = {}
    for leg in legs:
        by_key.setdefault(header_stage_key(leg), set()).add(leg["build"]["recipeTransform"])
    for key, transforms in sorted(by_key.items()):
        check("stage '%s' serves exactly one recipeTransform" % key,
              len(transforms) == 1, "serves %r" % sorted(transforms))

    # The sh emitter must round-trip every leg, and must emit assignments ONLY —
    # build-and-test.sh `eval`s this text, so a line that is not an assignment is
    # a command it would execute.
    sh = emit_sh(plan("linux", "x86_64", every, path))
    check("the sh emitter names every leg", all(lbl in sh for lbl in labels))
    statements = sh_statements(sh)
    check("the sh emitter emitted one statement per leg field",
          len(statements) == 1 + len(labels) * 22,
          "got %d statements for %d legs" % (len(statements), len(labels)))
    check("the sh emitter carries the launcher's path namespace",
          "LEG_PATH_TRANSLATION[" in sh and "LEG_PATH_TRANSLATOR[" in sh,
          "build-and-test.sh cannot translate a launcher's paths without it")
    check("the sh emitter carries the launcher's environment namespace",
          "LEG_ENV_TRANSFER[" in sh,
          "build-and-test.sh cannot forward its run environment without it")
    for stmt in statements:
        check("the sh emitter emits assignments only",
              ASSIGNMENT_RE.match(stmt) is not None, stmt)

    out.write("passed=%d failed=%d\n" % (passed, failed))
    return 0 if failed == 0 else 1


# ── CLI ─────────────────────────────────────────────────────────────────────

def main(argv=None):
    p = argparse.ArgumentParser(prog="harness_legs.py", description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--catalogue", default=CATALOGUE)
    p.add_argument("--verdict-vocabulary", action="store_true",
                   help="print the closed verdict names, one per line, in order")
    p.add_argument("--plan", action="store_true")
    p.add_argument("--header-stages", action="store_true",
                   help="print the distinct staged-header directories the drivers "
                        "must materialise: '<key>\\t<GUARD>=<0|1> ...', one per "
                        "line. HOST-FREE — a leg's header configuration is a fact "
                        "about its TARGET.")
    p.add_argument("--path-translations", action="store_true",
                   help="print the closed path-translation vocabulary: "
                        "'<verb>\\t<translator argv>', one per line")
    p.add_argument("--path-translation", default=None,
                   help="the verb --translate-path / --assert-translated act "
                        "under (a leg plan's run.pathTranslation)")
    p.add_argument("--translate-path", action="append", default=None,
                   metavar="PATH",
                   help="print PATH spelled the way the launcher declaring "
                        "--path-translation addresses it. Repeatable; one line "
                        "of output per PATH, in order. THIS is what keeps the "
                        "translator named in one file instead of two drivers.")
    p.add_argument("--assert-translated", action="append", default=None,
                   metavar="ARG",
                   help="verify no ARG is still a path in the namespace "
                        "--path-translation translates FROM. Silent on success; "
                        "FATAL naming the argument otherwise. ★ USE THE `=` "
                        "FORM (--assert-translated=ARG): a real launcher argv "
                        "contains things like `--start=full:`, and the "
                        "space-separated form would have argparse read that as "
                        "an option. The space form still works for a value that "
                        "does not begin with `-`, and misuse is a LOUD argparse "
                        "error rather than a skipped check.")
    p.add_argument("--env-transfers", action="store_true",
                   help="print the closed environment-transfer vocabulary: "
                        "'<verb>\\t<carrier variable>', one per line")
    p.add_argument("--env-transfer", default=None, metavar="VERB",
                   help="resolve --forward NAMEs under this verb (a leg plan's "
                        "run.envTransfer) and print the extra NAME=VALUE "
                        "assignments the driver must make. Prints NOTHING for a "
                        "verb whose child simply inherits.")
    p.add_argument("--forward", action="append", default=None, metavar="NAME",
                   help="an environment variable the launched process must see. "
                        "Repeatable. ⚠ NAMESPACE-NEUTRAL VALUES ONLY: forwarding "
                        "a variable whose value is a HOST path (PATH, "
                        "TCL_LIBRARY) hands the child a path it cannot resolve.")
    p.add_argument("--carrier-current", default="", metavar="VALUE",
                   help="the carrier variable's existing value, so an operator's "
                        "own setting is merged rather than clobbered")
    p.add_argument("--lint", action="store_true")
    p.add_argument("--self-test", action="store_true")
    p.add_argument("--host-os", default=None)
    p.add_argument("--host-arch", default=None)
    p.add_argument("--format", default="json", choices=("json", "sh"))
    p.add_argument("--launchers-available", default=None,
                   help="comma-separated launcher commands to treat as present "
                        "(tests pin this so a plan is reproducible)")
    p.add_argument("--launchers-none", action="store_true",
                   help="treat every declared launcher as absent")
    args = p.parse_args(argv)

    if not (args.verdict_vocabulary or args.plan or args.lint or args.self_test
            or args.header_stages or args.path_translations
            or args.translate_path or args.assert_translated
            or args.env_transfers or args.env_transfer):
        p.error("one of --verdict-vocabulary / --plan / --header-stages / --lint "
                "/ --self-test / --path-translations / --translate-path / "
                "--assert-translated / --env-transfers / --env-transfer is "
                "required")
    if (args.translate_path or args.assert_translated) and not args.path_translation:
        p.error("--translate-path / --assert-translated require "
                "--path-translation <verb> — the namespace is the launcher's "
                "DECLARATION, never something this tool infers")

    try:
        if args.verdict_vocabulary:
            sys.stdout.write("\n".join(VERDICTS) + "\n")
            return 0
        if args.path_translations:
            for verb in sorted(PATH_TRANSLATIONS):
                sys.stdout.write("%s\t%s\n" % (
                    verb, " ".join(PATH_TRANSLATIONS[verb]["translator"])))
            return 0
        if args.translate_path:
            for raw in args.translate_path:
                sys.stdout.write("%s\n"
                                 % translate_path(args.path_translation, raw))
            return 0
        if args.assert_translated:
            assert_translated(args.path_translation, args.assert_translated)
            return 0
        if args.env_transfers:
            for verb in sorted(ENV_TRANSFERS):
                sys.stdout.write("%s\t%s\n"
                                 % (verb, ENV_TRANSFERS[verb]["nameCarrier"]))
            return 0
        if args.env_transfer:
            for line in env_carrier_assignments(args.env_transfer,
                                                args.forward or [],
                                                args.carrier_current):
                sys.stdout.write("%s\n" % line)
            return 0
        if args.header_stages:
            for key, guards in header_stages(load_catalogue(args.catalogue)).items():
                sys.stdout.write("%s\t%s\n" % (key, " ".join(
                    "%s=%d" % (n, 1 if v else 0) for n, v in sorted(guards.items()))))
            return 0
        if args.lint:
            findings = lint(args.catalogue)
            for f in findings:
                sys.stdout.write("LINT: %s\n" % f)
            sys.stdout.write("findings=%d\n" % len(findings))
            return 1 if findings else 0
        if args.self_test:
            return self_test(args.catalogue)

        if args.launchers_none and args.launchers_available is not None:
            p.error("--launchers-none and --launchers-available are exclusive")
        available = None
        if args.launchers_none:
            available = set()
        elif args.launchers_available is not None:
            available = {x for x in args.launchers_available.split(",") if x}
        host_os = canon_os(args.host_os) if args.host_os else detect_host_os()
        host_arch = canon_arch(args.host_arch) if args.host_arch else detect_host_arch()
        resolved = plan(host_os, host_arch, available, args.catalogue)
        if args.format == "sh":
            sys.stdout.write(emit_sh(resolved))
        else:
            json.dump(resolved, sys.stdout, indent=2)
            sys.stdout.write("\n")
        return 0
    except LegError as exc:
        # FAIL LOUD. A driver that cannot resolve its legs must stop, not guess.
        sys.stderr.write("harness_legs.py: FATAL: %s\n" % exc)
        return 2


if __name__ == "__main__":
    sys.exit(main())
