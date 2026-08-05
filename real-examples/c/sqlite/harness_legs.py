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
#
# ★ `pinned-archive` IS THE GENERAL FORM OF `ubuntu-ports-arm64`
# (D-HARNESS-LIBRARY-ACQUISITION-BUILT-FOR-ONE-LEG-IN-ONE-DRIVER). Operator
# principle, 2026-08-04: "we should be able to build macho on linux. ANY LEG MUST
# BE ABLE TO BUILD TO ANY LEG." The drivers already AGREE in design — every leg's
# plan line reads "build here", with only the RUN gated — so a leg that cannot
# BUILD violates the harness's own contract. The mechanism was never the problem:
# `build-and-test.sh` has downloaded Ubuntu ports `.deb`s for the arm64 leg since
# TF-C68. What it lacked was (a) generality — one hand-written provider serving
# one leg — and (b) a second implementation, because `build-and-test.ps1` could
# not acquire AT ALL ("library provider '$provider' is NOT IMPLEMENTED by
# build-and-test.ps1"), which is this project's canonical silent-harness-bug shape.
#
# So `pinned-archive` is DECLARED (url + sha256 + members, in legs.json) and
# IMPLEMENTED HERE, once, in the file both drivers already hard-require — exactly
# the argument this module's header makes for putting the leg DECISION here. A
# driver calls `--acquire <leg>` and reads the resulting JSON; neither driver
# knows what HTTP is. That is what makes the capability-pair gap unrepeatable
# rather than merely repaired.
LIBRARY_PROVIDERS = {"host-system", "ubuntu-ports-arm64", "search-paths",
                     "pinned-archive"}
RECIPE_TRANSFORMS = {"none", "windows-selfconfig"}

# The archive kinds `--acquire` can open, and the `tarfile` mode that opens each.
# CLOSED on purpose: a `.deb` (an `ar` archive) and a `.pkg.tar.zst` (needs zstd,
# which is not in the stdlib before 3.14) are NOT here, so a catalogue that
# declares one fails the lint LOUDLY instead of failing at download time on some
# other machine, months later.
ARCHIVE_FORMATS = {
    "tar.bz2": "r:bz2",
    "tar.gz":  "r:gz",
    "tar.xz":  "r:xz",
}

# Mach-O `cpu_type_t` per TARGET ARCH — the key that picks a slice out of a
# universal archive. Keyed on the leg's own target arch, never on the host: a
# universal file contains both slices wherever it is sitting, and which one THIS
# LEG needs is a property of the leg.
MACHO_CPU_TYPES = {
    "x86_64": 0x01000007,
    "arm64":  0x0100000C,
}

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
#   validHostOs  the only host OS on which this namespace exists, so the lint can
#                CHECK a declaration instead of trusting it. "" = any.
PATH_TRANSLATIONS = {
    "none": {
        "translator": [],
        "sourceShape": "",
        "validHostOs": "",
    },
    "windows-to-wsl": {
        # `wslpath` is the translator because the parts that bite are its job:
        # the drive-letter -> mount-point mapping is configurable (/etc/wsl.conf
        # `root=`), UNC paths are a different mapping again, and case is not
        # ours to guess. Hand-rolled `C:\` -> `/mnt/c/` string surgery gets all
        # three wrong quietly.
        #
        # ★★ `-e` IS PART OF THE TRANSLATOR, NOT DECORATION —
        # D-TOOLS-WSL-EXE-WITHOUT-DASH-E-RUNS-A-LOCAL-SHELL. `wsl.exe <cmd>`
        # without it does not run <cmd>: WSL reconstructs a command LINE and
        # feeds it to the distro's DEFAULT SHELL, which strips quoting and
        # expands before the named binary is reached.
        #
        # ⚠ THIS ENTRY CARRIED A MISATTRIBUTED ROOT CAUSE UNTIL 2026-08-04, and
        # the correction is written down because a workaround stood on it. The
        # old comment said `wsl.exe wslpath -a -u 'C:\a\b'` prints
        # `wslpath: C:ab` because "the backslashes are eaten before wslpath ever
        # sees them", and normalised `\` -> `/` here to route around it. The
        # SYMPTOM was real; the CAUSE was not wslpath and not backslashes — it
        # was the hidden local shell, which eats `\` as its own escape character.
        # ✔RE-MEASURED 2026-08-04 through the REAL call path (python
        # subprocess.run, this host), one variable changed:
        #     with    -e :  'C:\a\b' -> rc=0 /mnt/c/a/b
        #     without -e :  'C:\a\b' -> rc=1 `wslpath: C:ab`
        # and the give-away that a naive fix would have missed: WITHOUT `-e`,
        # `'C:\Program Files\Git\x'` SUCCEEDS, because the space makes python
        # quote the argument and quoted backslashes survive the shell. So the
        # old defect fired only on paths with no spaces — which is why a
        # separator workaround looked like it worked.
        # ⇒ the separator normalisation is GONE, not demoted to belt-and-braces:
        # with `-e` the path reaches wslpath exactly as this driver holds it
        # (backslashes, spaces, trailing separator — all rc=0, MEASURED), and
        # re-spelling a path before handing it to the tool whose entire job is
        # to own that path's spelling is the same string surgery the paragraph
        # above refuses. Do not re-add it: if a translation ever fails again,
        # the input the tool saw must be the input this driver had.
        "translator": ["wsl.exe", "-e", "wslpath", "-a", "-u"],
        "sourceShape": "windows-drive",
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


# ── THE TARGET C COMPILER — A NAME IS NOT A DECLARATION OF TARGET ───────────
#
# D-HARNESS-LOADEXT-HELPER-TARGET-BLINDNESS-NOW-ABORTS-THE-RUN, and the anchor it
# grew out of, D-HARNESS-ARM64-LEG-HOST-ARCH-HELPER-SO.
#
# ✔MEASURED 2026-08-05, a full build-and-test.sh run on a WSL/Ubuntu x86_64 host:
# both elf legs went GREEN (6 err / 331,351 and 5 err / 331,355, all known
# confounds) and the run then DIED in /usr/bin/ld —
#   "relocation R_X86_64_PC32 against symbol `sqlite3_api' can not be used when
#    making a shared object; recompile with -fPIC" ... "final link failed"
# — while building the pe64 leg's loadext helper. pe64's units never ran.
#
# THE CAUSE WAS THE CATALOGUE, NOT THE GUARD. build-and-test.sh:2615 and :3804
# both state, emphatically and correctly, that this harness must never fall back
# to the host compiler, because "a HOST-arch extension the $leg fixture cannot
# load would false-red every loadext-* test as a genuine DSS failure". Neither
# guard fired: legs.json declared pe64's targetCc candidates as
# ["x86_64-w64-mingw32-gcc", "gcc"], the mingw cross-compiler was absent
# (✔MEASURED on that host), and so the resolver picked plain host `gcc`
# LEGITIMATELY — the config had already licensed the fallback the guards forbid.
#
# ★ AND THE FIX IS NOT "DELETE gcc FROM THAT LIST". ✔MEASURED 2026-08-05 on this
# project's Windows box: `gcc -dumpmachine` there prints `x86_64-w64-mingw32`,
# i.e. on a native Windows host the bare name `gcc` IS the pe64 leg's correct
# compiler (Git for Windows and Strawberry both ship mingw gcc under it). Deleting
# it would host-lock the leg in the other direction, which is the same defect
# wearing a different hat (D-HARNESS-CROSS-HOST-ANY-TARGET).
#
# So the candidate list stays a list of names to TRY, and ACCEPTANCE is decided by
# asking the compiler what it targets and comparing that against the leg's own
# declared `spec`. That is a property of the LEG (its target), checked against a
# property of the CANDIDATE (its own answer) — no host test, no format-name branch,
# and no new catalogue key, because the leg already declares its target.
#
# ★★ IT GENERALISES BEYOND pe64, WHICH IS THE ARGUMENT FOR DOING IT THIS WAY. The
# elf64-x86_64 leg declares candidates ["cc", "gcc", "clang"]; on the project's
# NATIVE arm64 Linux VPS those resolve to an aarch64 compiler, which is exactly
# the wrong-arch helper D-HARNESS-ARM64-LEG-HOST-ARCH-HELPER-SO is named after.
# The same one check refuses it, on a host nobody had to enumerate here.

# `-dumpmachine` is THE question, asked ONE way. DOCUMENTED: GCC and Clang both
# implement it and print the target triple on a single line.
# ✔MEASURED 2026-08-05: gcc -> `x86_64-linux-gnu` (WSL) and `x86_64-w64-mingw32`
# (Windows/Strawberry), aarch64-linux-gnu-gcc -> `aarch64-linux-gnu`.
# ⚠ Apple clang is DOCUMENTED, NOT MEASURED — this project's Mac was asleep and
# unreachable when this landed, and waking a personal machine needs the operator.
# If a Mac ever rejects its own `clang` here, the diagnostic names this exact
# probe and the leg degrades to `skipped-build-input-missing` (loud, and STILL
# BUILT) — never to a silently wrong-target helper, which is the outcome that
# reads as a DSS miscompile.
CC_TARGET_MACHINE_FLAG = "-dumpmachine"


def cc_machine_argv(cc):
    """The argv that asks a compiler what it targets. Named ONCE, here, so the
    two drivers cannot come to ask the question two different ways."""
    return [cc, CC_TARGET_MACHINE_FLAG]


def machine_target_os(token):
    """The OS one TOKEN of a target triple names, in this file's own OS
    vocabulary, or "" when it names none.

    Matching is by PREFIX against OS_ALIASES, longest alias first, because a
    triple's OS component routinely carries a version or a flavour suffix that
    an exact lookup would miss: `mingw32`, `darwin24.4.0`, `macosx14.0`. Longest
    first so a short alias can never shadow a longer one that also matches.
    Vendor tokens (`pc`, `apple`, `w64`, `unknown`) and ABI tokens (`gnu`,
    `musl`, `eabi`) match nothing, which is what makes the scan safe to run over
    every token rather than guessing which position the OS is in — triples come
    both 3-part (`x86_64-linux-gnu`) and 4-part (`x86_64-pc-linux-gnu`)."""
    key = (token or "").strip().lower()
    if not key:
        return ""
    for alias in sorted(OS_ALIASES, key=len, reverse=True):
        if key.startswith(alias):
            return OS_ALIASES[alias]
    return ""


def machine_matches_spec(machine, spec):
    """Does a compiler reporting MACHINE produce objects for SPEC's target?

    Returns (ok, reason) — the reason is populated on BOTH outcomes so a driver
    can log why it accepted as readily as why it refused. PURE: no filesystem, no
    process, no host, so the whole rule is unit-testable (see self_test)."""
    first = ((machine or "").strip().splitlines() or [""])[0].strip()
    if not first:
        return (False, "printed no target triple at all")
    want_arch = canon_arch(spec_target_arch(spec))
    want_os = spec_target_os(spec)
    tokens = [t for t in first.split("-") if t]
    got_arch = canon_arch(tokens[0]) if tokens else ""
    if got_arch != want_arch:
        return (False, "targets arch '%s' (triple '%s'); this leg needs '%s'"
                       % (got_arch or "<unreadable>", first, want_arch))
    got_os = ""
    for tok in tokens[1:]:
        got_os = machine_target_os(tok)
        if got_os:
            break
    if not got_os:
        return (False, "triple '%s' names no OS this catalogue recognises "
                       "(known: %s); this leg needs '%s'"
                       % (first, ", ".join(sorted(set(OS_ALIASES.values()))),
                          want_os or "<unreadable from the leg's spec>"))
    if got_os != want_os:
        return (False, "targets OS '%s' (triple '%s'); this leg needs '%s'"
                       % (got_os, first, want_os or "<unreadable from the leg's spec>"))
    return (True, "triple '%s' targets %s/%s" % (first, got_arch, got_os))


def _run_machine_probe(argv):
    """(rc, stdout). rc is taken DIRECTLY off the process, never after a pipe."""
    import subprocess  # local, matching _run_translator: this resolver spawns
                       # almost nothing and the import stays next to the spawn.
    try:
        proc = subprocess.run(argv, capture_output=True, text=True)
    except OSError as exc:
        return 127, "%s" % exc
    return proc.returncode, proc.stdout


def resolve_target_cc(leg, runner=None, which=None):
    """The FIRST declared candidate that is BOTH present and proves it targets
    this leg. Returns (cc, machine, rejections) with cc == "" when none does.

    ★ AN UNVERIFIABLE CANDIDATE IS REFUSED, NOT ASSUMED. A compiler whose
    `-dumpmachine` fails cannot say what it produces, and "accept it anyway" is
    precisely the silent fallback that cost a run above. The refusal is loud, it
    names the probe, and it costs the leg its RUN — never its BUILD."""
    runner = runner or _run_machine_probe
    which = which or shutil.which
    spec = leg.get("spec", "")
    rejections = []
    for cc in leg.get("build", {}).get("targetCc", {}).get("candidates", []):
        found = which(cc)
        if not found:
            rejections.append("%s: not on PATH" % cc)
            continue
        argv = cc_machine_argv(cc)
        rc, out = runner(argv)
        if rc != 0:
            rejections.append(
                "%s (%s): `%s` exited %d, so it cannot state its target; REFUSED "
                "rather than assumed — output: %r"
                % (cc, found, " ".join(argv), rc, (out or "").strip()[:200]))
            continue
        ok, why = machine_matches_spec(out, spec)
        if not ok:
            rejections.append("%s (%s): %s" % (cc, found, why))
            continue
        return (cc, ((out or "").strip().splitlines() or [""])[0].strip(),
                rejections)
    return ("", "", rejections)


# ── The helper extension the loadext corpus dlopen()s, PER TARGET ───────────
#
# ✔MEASURED 2026-08-05 from the staged upstream tree, sqlite `test/loadext.test`
# lines 24-30, quoted structurally rather than verbatim:
#
#     if {$::tcl_platform(platform) eq "windows"} -> ./testloadext.dll
#     else                                        -> ./libtestloadext.so
#
# and `$::tcl_platform(os) eq "Darwin"` changes only the COMPILER FLAGS
# (`-dynamiclib`), never the name — so a Darwin fixture looks for the `.so`
# spelling too. DOCUMENTED (Tcl manual): `tcl_platform(platform)` is `windows` on
# a Windows Tcl and `unix` everywhere else this harness targets.
#
# ★ WHY THIS IS A TARGET FACT AND NOT A DRIVER CONSTANT. build-and-test.sh:3801
# hardcoded `libtestloadext.so` for EVERY leg. A pe64 fixture therefore looked for
# `./testloadext.dll`, did not find it, and fell through to loadext.test's own
# `exec gcc` self-build — the exact fallback the pre-staging exists to prevent.
# The name is declared per leg (`build.loadExtHelperName`) and the lint checks the
# declaration against the target OS derived from the leg's spec, the same
# declare-then-cross-check discipline `zconfGuards` / POSIX_ONLY_ZCONF_GUARDS use.
LOADEXT_HELPER_NAME_BY_TARGET_OS = {
    "windows": "testloadext.dll",
    "linux": "libtestloadext.so",
    "darwin": "libtestloadext.so",
}


def loadext_helper_name(leg):
    """The declared file name, verbatim. No default and no derivation here: an
    omitted declaration is a LINT finding, not something this quietly fills in."""
    return str(leg.get("build", {}).get("loadExtHelperName", ""))


def leg_by_label(legs, label, where=""):
    """ONE leg, by label, or a LegError naming every label there is. One copy,
    because two subcommands looking a leg up two ways is how their diagnostics
    come to disagree about what the catalogue contains."""
    for leg in legs:
        if leg.get("label") == label:
            return leg
    raise LegError("no leg labelled %r in %s (declared: %s)"
                   % (label, where or CATALOGUE,
                      ", ".join(l.get("label", "?") for l in legs)))


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


# ── Declared library acquisition ────────────────────────────────────────────
#
# D-HARNESS-LIBRARY-ACQUISITION-BUILT-FOR-ONE-LEG-IN-ONE-DRIVER. A leg whose
# target libraries this machine does not carry ACQUIRES them, from a source the
# CATALOGUE declares, against a checksum the catalogue PINS.
#
# THE FOUR RULES, all of them load-bearing:
#
#  1. PINNED CHECKSUM. A build that fetches third-party binaries is a
#     supply-chain surface. Every archive declares its sha256 and is verified
#     BEFORE anything is extracted from it — and the download cache is
#     CONTENT-ADDRESSED (`downloads/<sha256>.<ext>`), so "is the cached copy the
#     thing we pinned?" is answered by re-hashing, not by trusting a file name.
#  2. FAIL LOUD, NEVER A SILENT FALLBACK. A checksum mismatch, an absent member,
#     a missing slice, an unreachable host with a cold cache — each raises with
#     the URL, the expected and actual digest, and what was being looked for.
#     Nothing here EVER degrades to "use whatever is already on disk".
#  3. OFFLINE WORKS. The network is touched only when the content-addressed
#     download is absent. A populated cache + `--offline` completes with no
#     round-trip at all, which is what makes this usable on a gate machine.
#  4. TARGET-KEYED, NEVER HOST-KEYED. The route is a property of the LEG's
#     target. The only host input is WHERE this machine keeps caches
#     (`cache_root`), which is the same kind of fact as `searchPaths`.
#
# ★ AND THE PART THAT IS NOT OBVIOUS: `importName`. An acquired library is a
# STAND-IN — we read its export surface, but the target machine will load ITS
# OWN copy from ITS OWN prefix. The embedded identity (Mach-O LC_ID_DYLIB / ELF
# DT_SONAME / PE export DllName) is therefore a fact about the PACKAGER, not
# about the target: MacPorts' libtcl8.6.dylib says `/opt/local/lib/...`, and a
# binary that records that demands MacPorts on the target Mac. So every acquired
# member DECLARES the identity to record, the lint REQUIRES it, and `--acquire`
# reports the declared name beside the embedded one it is displacing, so a build
# log always states the substitution instead of hiding it.


def cache_root(explicit=None):
    """Where THIS MACHINE keeps caches. A host fact, in the same category as
    `searchPaths` — it decides nothing about which legs exist. Precedence:
    an explicit --cache-root, then DSS_HARNESS_CACHE_ROOT, then the
    `~/.cache/dss-code-prime` the pe64 leg's declared search paths already
    name, spelled through the variable that exists on this host."""
    if explicit:
        return os.path.abspath(explicit)
    env = os.environ.get("DSS_HARNESS_CACHE_ROOT")
    if env:
        return os.path.abspath(env)
    home = os.environ.get("HOME") or os.environ.get("USERPROFILE")
    if not home:
        raise LegError(
            "cannot locate a cache root: neither DSS_HARNESS_CACHE_ROOT nor "
            "HOME nor USERPROFILE is set. The acquisition cache lives OUTSIDE "
            "the repository on purpose (a downloaded third-party binary is not "
            "a source artefact), so there is no in-tree fallback to take.")
    return os.path.join(os.path.abspath(home), ".cache", "dss-code-prime")


def leg_cache_dir(leg, root):
    """The directory a leg's acquired libraries are materialised into. DERIVED
    from the leg label rather than declared: one fewer thing a catalogue can get
    wrong, and two legs can never collide on it."""
    return os.path.join(root, "harness-libs", leg["label"])


def _sha256_file(path):
    import hashlib
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def _fat_slices(buf):
    """[(cputype, offset, size)] for a Mach-O universal archive, or [] if `buf`
    is not one. The fat header is BIG-ENDIAN by definition (both magics), which
    is why the struct format is fixed and not host-derived."""
    import struct
    if len(buf) < 8:
        return []
    magic, count = struct.unpack(">II", buf[:8])
    if magic == 0xCAFEBABE:          # fat_arch (32-bit offsets)
        out, off = [], 8
        for _ in range(count):
            cputype, _sub, offset, size, _align = struct.unpack(">IIIII", buf[off:off + 20])
            out.append((cputype, offset, size))
            off += 20
        return out
    if magic == 0xCAFEBABF:          # fat_arch_64
        out, off = [], 8
        for _ in range(count):
            cputype, _sub, offset, size, _align, _res = struct.unpack(">IIQQII", buf[off:off + 32])
            out.append((cputype, offset, size))
            off += 32
        return out
    return []


def _macho_install_name(buf):
    """The LC_ID_DYLIB install name of a THIN 64-bit little-endian Mach-O, or ""
    when there is none / this is not one. Read for REPORTING only: it is the
    identity `importName` displaces, and a build log that does not print it
    cannot show the substitution actually happened."""
    import struct
    if len(buf) < 32:
        return ""
    magic, = struct.unpack("<I", buf[:4])
    if magic != 0xFEEDFACF:
        return ""
    ncmds, = struct.unpack("<I", buf[16:20])
    p = 32
    for _ in range(ncmds):
        if p + 8 > len(buf):
            return ""
        cmd, cmdsize = struct.unpack("<II", buf[p:p + 8])
        if cmdsize < 8:
            return ""
        if cmd == 0xD:               # LC_ID_DYLIB
            nameoff, = struct.unpack("<I", buf[p + 8:p + 12])
            return buf[p + nameoff:p + cmdsize].split(b"\0")[0].decode("utf-8", "replace")
        p += cmdsize
    return ""


def _thin_for_arch(blob, target_arch, where):
    """The slice of `blob` this leg's TARGET needs.

    A universal archive is not a library a cross-compiler can be handed: DSS's
    Mach-O reader recognises the THIN 64-bit magic (0xFEEDFACF) and nothing else
    (`src/ffi/binary_readers/macho_reader.cpp` — there is no 0xCAFEBABE arm), and
    even a reader that DID accept one would have to be told which slice, which
    `readImports(path, reporter)` has no parameter for. Slicing here is not a
    workaround for that: a LEG's build input is a library FOR THAT LEG'S TARGET,
    and handing macho64-arm64 a file that also contains x86_64 code makes "which
    architecture did this leg actually resolve against?" unanswerable from the
    input. `lipo -thin` exists for exactly this reason — and it is the very
    remediation DSS's own dispatcher names in its `D-FF1-MACHO-FAT` arm, so
    slicing here is the CONTRACT that anchor states, not a way around it."""
    slices = _fat_slices(blob)
    if not slices:
        return blob, False
    want = MACHO_CPU_TYPES.get(target_arch)
    if want is None:
        raise LegError(
            "%s is a Mach-O universal archive with %d slice(s), but this "
            "resolver has no cpu_type for target arch %r (known: %s) — it "
            "cannot pick the right one, and picking the wrong one would produce "
            "a build that resolves against a foreign architecture"
            % (where, len(slices), target_arch, ", ".join(sorted(MACHO_CPU_TYPES))))
    for cputype, offset, size in slices:
        if cputype == want:
            if offset + size > len(blob):
                raise LegError("%s: the %s slice runs past the end of the file "
                               "(offset %d + size %d > %d)"
                               % (where, target_arch, offset, size, len(blob)))
            return blob[offset:offset + size], True
    raise LegError(
        "%s is a universal archive that does NOT contain a %s slice (has: %s). "
        "The declaration says this leg's library comes from here; it does not."
        % (where, target_arch,
           ", ".join(sorted(_arch_name(c) for c, _o, _s in slices))))


def _arch_name(cputype):
    for name, value in MACHO_CPU_TYPES.items():
        if value == cputype:
            return name
    return "cpu_type=0x%08X" % cputype


def _download(url, dest, timeout=120):
    """Fetch `url` to `dest`. Deliberately the ONLY network call in this file, so
    `--offline` has exactly one thing to refuse."""
    import urllib.request
    # ★ THE PART FILE IS PER-PROCESS, NOT PER-DESTINATION. The download cache is
    # SHARED between legs (two macho legs slice the same universal archive) and
    # this project's rule is that legs and drivers may run CONCURRENTLY — a fixed
    # `<dest>.part` would let two processes interleave writes into one file and
    # then both rename it, producing a digest mismatch that looks like a supply-
    # chain event instead of a race. `os.replace` is atomic, so a unique temp per
    # process makes the last writer win with a WHOLE file, and the digest check
    # still guards the content.
    tmp = "%s.%d.part" % (dest, os.getpid())
    try:
        with urllib.request.urlopen(url, timeout=timeout) as resp, \
                open(tmp, "wb") as out:
            shutil.copyfileobj(resp, out)
    except Exception as exc:            # noqa: BLE001 — re-raised as LegError
        try:
            os.remove(tmp)
        except OSError:
            pass
        raise LegError("download failed: %s\n  %s: %s"
                       % (url, type(exc).__name__, exc))
    os.replace(tmp, dest)


def acquire_plan(leg, root):
    """What `--acquire` WOULD do, as data: the declared archives, the cache paths
    they land in, and the (as-name -> importName) map. Pure — no filesystem, no
    network — so the self-test can assert the plan on any machine."""
    libs = leg.get("build", {}).get("libraries", {})
    acq = libs.get("acquire", {})
    target_arch = spec_target_arch(leg.get("spec", ""))
    cdir = leg_cache_dir(leg, root)
    ddir = os.path.join(root, "harness-libs", "downloads")
    archives = []
    for a in acq.get("archives", []):
        fmt = a.get("archiveFormat", "")
        ext = fmt if fmt else "bin"
        archives.append({
            "url": a.get("url", ""),
            "sha256": a.get("sha256", ""),
            "archiveFormat": fmt,
            "download": os.path.join(ddir, "%s.%s" % (a.get("sha256", "nohash"), ext)),
            "members": [{
                "member": m.get("member", ""),
                "as": m.get("as", ""),
                "universal": bool(m.get("universal", False)),
                "importName": m.get("importName", ""),
                "path": os.path.join(cdir, m.get("as", "")),
            } for m in a.get("members", [])],
        })
    return {
        "leg": leg.get("label", ""),
        "targetArch": target_arch,
        "cacheDir": cdir,
        "downloadDir": ddir,
        "archives": archives,
    }


# Bumped when the MATERIALISATION changes shape (a new slicing rule, a new
# archive kind, a new stamp field). A stamp written by an older resolver is
# re-materialised rather than trusted — the alternative is a stale cache that
# looks current.
#   1 -> 2: the stamp records each MATERIALISED file's own sha256. Pinning only
#           the archive left a hole one run wide: after the first acquisition the
#           extracted libraries were trusted by stamp alone, so a cache whose
#           libtcl8.6.dylib had been swapped would be handed to the compiler
#           without a murmur. The digest pin has to survive extraction or it only
#           protects the download.
ACQUIRE_STAMP_VERSION = 2


def acquire(leg, root, offline=False, downloader=_download):
    """Materialise a `pinned-archive` leg's declared libraries and return the
    report. Raises LegError — loudly, naming the URL/digest/member — on any
    failure. `downloader` is injected so the self-test can assert the OFFLINE and
    CACHE-HIT paths without a network (and prove they take no round-trip)."""
    import json as _json
    import tarfile

    libs = leg.get("build", {}).get("libraries", {})
    if libs.get("provider") != "pinned-archive":
        raise LegError("leg '%s' declares provider %r, not 'pinned-archive' — "
                       "--acquire is the implementation of ONE declared route, "
                       "not a general 'go and find it somewhere' verb"
                       % (leg.get("label", "?"), libs.get("provider")))
    plan_ = acquire_plan(leg, root)
    cdir, ddir = plan_["cacheDir"], plan_["downloadDir"]
    stamp_path = os.path.join(cdir, ".acquired.json")

    want_stamp = {
        "stampVersion": ACQUIRE_STAMP_VERSION,
        "targetArch": plan_["targetArch"],
        "archives": [{"sha256": a["sha256"], "url": a["url"],
                      "members": [{"member": m["member"], "as": m["as"],
                                   "universal": m["universal"],
                                   "importName": m["importName"]}
                                  for m in a["members"]]}
                     for a in plan_["archives"]],
    }
    # The DECLARATION half of the stamp must match exactly; the `materialised`
    # half is the digest of each extracted file, checked below.
    stamp = None
    if os.path.isfile(stamp_path):
        try:
            with open(stamp_path, "r", encoding="utf-8") as f:
                stamp = _json.load(f)
        except (OSError, ValueError):
            stamp = None
    fresh = bool(stamp) and stamp.get("declaration") == want_stamp
    # Why a re-materialisation happened, so a build log never has to guess. A
    # non-empty list is not an error: the content is restored FROM THE
    # DIGEST-VERIFIED ARCHIVE, which is the pinned thing. It is reported because
    # a cache that silently repaired itself is a fact worth seeing.
    remediated = []
    if fresh:
        have = stamp.get("materialised", {})
        for a in plan_["archives"]:
            for m in a["members"]:
                if not os.path.isfile(m["path"]):
                    remediated.append("%s: absent" % m["as"])
                    fresh = False
                elif _sha256_file(m["path"]) != have.get(m["as"]):
                    # ★ THE PIN HAS TO SURVIVE EXTRACTION. Verifying only the
                    # archive protects the download and nothing after it: from
                    # the second run on, the compiler is handed the EXTRACTED
                    # file, and if that is not what came out of the pinned
                    # archive then the pin bought nothing.
                    remediated.append("%s: content does not match the digest "
                                      "recorded when it was extracted" % m["as"])
                    fresh = False

    if not fresh:
        # EVERY declaration is validated, and OFFLINE refuses, BEFORE anything is
        # created or fetched. A run that cannot possibly succeed must not first
        # leave a half-built cache tree behind it, and an unopenable
        # archiveFormat should not be discovered after a 5 MB download.
        for a in plan_["archives"]:
            if a["archiveFormat"] not in ARCHIVE_FORMATS:
                raise LegError(
                    "leg '%s': archive %s declares archiveFormat %r, which this "
                    "resolver cannot open (known: %s)"
                    % (plan_["leg"], a["url"], a["archiveFormat"],
                       ", ".join(sorted(ARCHIVE_FORMATS))))
            if offline and not os.path.isfile(a["download"]):
                raise LegError(
                    "leg '%s': --offline, but the pinned archive is not in the "
                    "cache.\n  need : %s\n  from : %s\n  Run once without "
                    "--offline to populate it; this refuses rather than falling "
                    "back to whatever else is on this machine."
                    % (plan_["leg"], a["download"], a["url"]))
        # ✔MEASURED, and reported independently by BOTH driver implementations:
        # an unusable cache root (a path whose parent is a regular file, a
        # read-only volume) escaped as a raw Python traceback with rc=1 instead
        # of this file's own `harness_legs.py: FATAL: …` shape — so the driver
        # dutifully quoted a stack trace at the operator. Loud, but below the bar
        # the rest of this module holds itself to: every refusal names the thing
        # that failed and what to do about it.
        try:
            os.makedirs(ddir, exist_ok=True)
            os.makedirs(cdir, exist_ok=True)
        except OSError as exc:
            raise LegError(
                "leg '%s': the acquisition cache root is not usable.\n  root : "
                "%s\n  error: %s\n  The cache lives OUTSIDE the repository by "
                "design; point DSS_HARNESS_CACHE_ROOT (or --cache-root) at a "
                "writable directory." % (plan_["leg"], root, exc))
        for a in plan_["archives"]:
            mode = ARCHIVE_FORMATS[a["archiveFormat"]]
            dl = a["download"]
            if os.path.isfile(dl):
                got = _sha256_file(dl)
                if got != a["sha256"]:
                    # A content-addressed name that does not match its content is
                    # a corrupt or tampered cache, never something to reuse.
                    os.remove(dl)
                    if offline:
                        raise LegError(
                            "leg '%s': --offline, and the cached archive is "
                            "CORRUPT (its content does not hash to the name it "
                            "is filed under).\n  file     : %s\n  expected : "
                            "%s\n  actual   : %s\n  It has been removed; re-run "
                            "with network access."
                            % (plan_["leg"], dl, a["sha256"], got))
            if not os.path.isfile(dl):
                downloader(a["url"], dl)
                got = _sha256_file(dl)
                if got != a["sha256"]:
                    os.remove(dl)
                    raise LegError(
                        "CHECKSUM MISMATCH — refusing to use this download.\n"
                        "  url      : %s\n  expected : %s\n  actual   : %s\n"
                        "  The catalogue PINS the digest precisely so a changed "
                        "or substituted third-party binary stops the build "
                        "instead of entering it."
                        % (a["url"], a["sha256"], got))
            with tarfile.open(dl, mode) as tf:
                for m in a["members"]:
                    blob = _extract_member(tf, m["member"], a["url"])
                    where = "%s :: %s" % (os.path.basename(a["url"]), m["member"])
                    if m["universal"]:
                        blob, was_fat = _thin_for_arch(blob, plan_["targetArch"], where)
                        if not was_fat:
                            raise LegError(
                                "%s is declared `universal: true` but is NOT a "
                                "Mach-O universal archive. The declaration and "
                                "the file disagree; slicing silently skipped "
                                "would hand this leg a library of unknown "
                                "architecture." % where)
                    elif _fat_slices(blob):
                        raise LegError(
                            "%s IS a Mach-O universal archive but is not "
                            "declared `universal: true`. DSS's Mach-O reader "
                            "accepts only a thin image, so this would fail at "
                            "--resolve-library time with a format error that "
                            "says nothing about the declaration." % where)
                    dest = m["path"]
                    if os.path.exists(dest):
                        os.chmod(dest, 0o644)
                        os.remove(dest)
                    with open(dest, "wb") as f:
                        f.write(blob)
        with open(stamp_path, "w", encoding="utf-8") as f:
            _json.dump({"declaration": want_stamp,
                        "materialised": {m["as"]: _sha256_file(m["path"])
                                         for a in plan_["archives"]
                                         for m in a["members"]}},
                       f, indent=1, sort_keys=True)

    libraries = []
    for a in plan_["archives"]:
        for m in a["members"]:
            with open(m["path"], "rb") as f:
                head = f.read(1 << 16)
            libraries.append({
                "as": m["as"],
                "path": m["path"],
                "importName": m["importName"],
                # The identity being DISPLACED, so a build log states the
                # substitution instead of hiding it.
                "embeddedIdentity": _macho_install_name(head),
                "sourceUrl": a["url"],
                "archiveSha256": a["sha256"],
                "fileSha256": _sha256_file(m["path"]),
            })
    return {
        "leg": plan_["leg"],
        "targetArch": plan_["targetArch"],
        "cacheDir": cdir,
        "fromCache": fresh,
        "remediated": remediated,
        "libraries": libraries,
    }


def _extract_member(tf, name, url):
    """One member's BYTES, following an intra-archive symlink.

    Symlinks are not incidental: MacPorts ships `libz.1.dylib -> libz.1.3.2.dylib`,
    and a symlink extracted as a symlink is (a) not creatable on Windows without
    privilege and (b) not a library DSS can read. Materialising the LINK TARGET
    under the declared `as` name is what makes one declaration work on every
    host."""
    seen = []
    cur = name
    for _ in range(8):
        try:
            member = tf.getmember(cur)
        except KeyError:
            # Archives commonly prefix `./`; try that spelling before failing.
            try:
                member = tf.getmember("./" + cur)
            except KeyError:
                raise LegError(
                    "member not found in %s: %r%s\n  A declared member that is "
                    "not in the archive is a stale declaration, not something to "
                    "search around for."
                    % (os.path.basename(url), name,
                       ("  (followed: %s)" % " -> ".join(seen)) if seen else ""))
        if member.issym() or member.islnk():
            seen.append(cur)
            target = member.linkname
            cur = target if target.startswith("/") else \
                os.path.normpath(os.path.join(os.path.dirname(cur), target)).replace("\\", "/")
            continue
        f = tf.extractfile(member)
        if f is None:
            raise LegError("member %r in %s is not a regular file"
                           % (cur, os.path.basename(url)))
        return f.read()
    raise LegError("member %r in %s: symlink chain too deep (%s)"
                   % (name, os.path.basename(url), " -> ".join(seen)))


# ── The DSS argv for one resolved library ───────────────────────────────────
#
# ★ NAMED IN ONE FILE, NOT IN TWO DRIVERS — the same argument `--translate-path`
# makes for `wslpath`. A driver that spelled the compiler flag itself would be a
# capability that can exist in one driver and not the other, which is this
# project's canonical silent-harness-bug shape and the very defect
# D-HARNESS-LIBRARY-ACQUISITION-BUILT-FOR-ONE-LEG-IN-ONE-DRIVER names.
#
# ⚠ THE OVERRIDE IS NOT OPTIONAL WHERE IT IS DECLARED. A leg whose library is an
# ACQUIRED STAND-IN and whose compiler cannot record the declared identity must
# NOT be built: the artefact would link clean and fail at dyld/loader time on the
# target machine, which is the one failure this host cannot observe. So
# `resolve_library_argv` REFUSES rather than dropping the override.
DSS_RESOLVE_LIBRARY_FLAG = "--resolve-library"
# The override is a SUFFIX on the flag's value, not a second flag:
# `--resolve-library <path>[=<import-name>]`, split by the compiler on the LAST
# `=`. This marker is what `--help` must show for the capability to be present;
# probing for the bare flag name would find the pre-override compiler too, which
# is exactly the silent-drop this refuses to allow.
DSS_IMPORT_NAME_HELP_MARKER = "--resolve-library <path>[=<import-name>]"


def dss_supports_import_name(dss_path, runner=None):
    """Does this compiler accept the import-name override? Probed from its own
    --help, never assumed — the compiler and the harness land in different
    commits, and a driver that assumed the newer compiler would silently emit an
    artefact with the packager's install name baked in."""
    argv = [dss_path, "--help"]
    if runner is not None:
        return DSS_IMPORT_NAME_HELP_MARKER in runner(argv)
    import subprocess
    try:
        out = subprocess.run(argv, stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT, timeout=60)
    except (OSError, subprocess.SubprocessError) as exc:
        raise LegError("cannot probe %r for --help: %s" % (dss_path, exc))
    return DSS_IMPORT_NAME_HELP_MARKER in out.stdout.decode("utf-8", "replace")


def resolve_library_argv(path, import_name="", supported=True):
    """The argv tokens that hand DSS one resolved library. Without an override
    that is the plain `--resolve-library <path>` every leg has always used, so a
    provider that needs no override emits byte-identical arguments to before."""
    if not import_name:
        return [DSS_RESOLVE_LIBRARY_FLAG, path]
    if not supported:
        raise LegError(
            "this compiler does not accept `%s`, but the leg DECLARES the "
            "runtime identity %r for %s.\n  Dropping the override is not an "
            "option: the binary would record the packager's own install name "
            "(the acquired library is a STAND-IN, not the copy the target "
            "machine loads), link clean here, and fail at LOAD time on a machine "
            "this host cannot observe.\n  Build a compiler that carries it, or "
            "change the leg's provider."
            % (DSS_IMPORT_NAME_HELP_MARKER, import_name, path))
    if "=" in path:
        # The compiler splits the value on its LAST `=`, so a path containing one
        # cannot carry an override on the command line at all — it would silently
        # truncate the path and record a nonsense identity. REFUSE, with the way
        # out: this is a property of where the cache lives, and DSS_HARNESS_CACHE_
        # ROOT moves it. (The project manifest's object form has no separator and
        # is the other escape hatch, for a driver that builds via --project.)
        raise LegError(
            "the resolved library path contains '=', which `%s` cannot express: "
            "the compiler splits the value on its LAST '=', so this path would "
            "be truncated and the recorded identity would be wrong.\n  path : "
            "%s\n  want : %s\n  Move the acquisition cache somewhere without an "
            "'=' (DSS_HARNESS_CACHE_ROOT), or build this leg through a project "
            "manifest, whose object form needs no separator."
            % (DSS_IMPORT_NAME_HELP_MARKER, path, import_name))
    return [DSS_RESOLVE_LIBRARY_FLAG, "%s=%s" % (path, import_name)]


def acquired_import_names(leg):
    """(tclImportName, zImportName) for a leg, matched from its acquire members'
    `as` names against its own declared tclNames/zNames. Computed HERE so the two
    drivers never each decide which acquired file is the Tcl one."""
    libs = leg.get("build", {}).get("libraries", {})
    tcl_names = set(libs.get("tclNames", []))
    z_names = set(libs.get("zNames", []))
    tcl = z = ""
    for a in libs.get("acquire", {}).get("archives", []):
        for m in a.get("members", []):
            if m.get("as") in tcl_names:
                tcl = m.get("importName", "")
            elif m.get("as") in z_names:
                z = m.get("importName", "")
    return tcl, z


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
    # VERBATIM. The path the translator sees is the path this driver holds —
    # no separator normalisation, no re-spelling of any kind (see the
    # `windows-to-wsl` comment: that workaround existed only to route around a
    # hidden local shell, and the shell is gone from the translator argv).
    argv = list(spec["translator"]) + [raw]
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
    # The RUNTIME IDENTITY each acquired library must be recorded under, resolved
    # to the (tcl, z) pair the drivers actually pass to DSS. Host-free: it is a
    # property of the TARGET's library layout, and it is "" for every provider
    # that hands over a library already carrying the right embedded identity.
    tcl_import, z_import = acquired_import_names(leg)
    libs["tclImportName"] = tcl_import
    libs["zImportName"] = z_import
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
        # The FILE NAME sqlite's own test/loadext.test looks for on THIS leg's
        # target (see LOADEXT_HELPER_NAME_BY_TARGET_OS). Emitted so the driver
        # never spells it, and never spells it once for five different targets.
        put("LEG_LOADEXT_NAME", b.get("loadExtHelperName", ""))
        put("LEG_LIB_PROVIDER", libs.get("provider", ""))
        put("LEG_LIB_TCL_NAMES", " ".join(libs.get("tclNames", [])))
        put("LEG_LIB_Z_NAMES", " ".join(libs.get("zNames", [])))
        put("LEG_LIB_PATHS", "\n".join(libs.get("searchPaths", [])))
        # The runtime identity DSS must record for each acquired library, when
        # the file we can READ is a stand-in whose embedded identity belongs to
        # its packager. "" = the library's own identity is the right one. The
        # driver passes it to DSS; it never invents one.
        put("LEG_LIB_TCL_IMPORT_NAME", libs.get("tclImportName", ""))
        put("LEG_LIB_Z_IMPORT_NAME", libs.get("zImportName", ""))
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

_SHA256_RE = re.compile(r"^[0-9a-f]{64}$")


def _lint_acquire(label, spec, libs):
    """The `acquire` block's own defects. Split out because there are enough of
    them that inlining would bury the rest of the leg lint — and because every
    one of these is a rule that, unchecked, ships a build that fetches an
    unverified binary or records a wrong runtime identity."""
    out = []
    provider = libs.get("provider")
    acq = libs.get("acquire")
    if provider == "pinned-archive":
        if not acq:
            out.append("leg '%s': provider 'pinned-archive' declares no "
                       "`acquire` block — the provider IS the declaration; "
                       "without it there is nothing to fetch" % label)
            return out
    elif acq:
        out.append("leg '%s': provider %r ignores `acquire`, but one is declared "
                   "— a reader would think it is used" % (label, provider))
        return out
    else:
        return out

    archives = acq.get("archives")
    if not isinstance(archives, list) or not archives:
        out.append("leg '%s': acquire.archives is empty" % label)
        return out
    tcl_names = set(libs.get("tclNames", []))
    z_names = set(libs.get("zNames", []))
    saw_tcl = saw_z = False
    for a in archives:
        url = a.get("url", "")
        who = "leg '%s' archive %r" % (label, url or "<no url>")
        if not url:
            out.append("%s: no url" % who)
        elif not url.startswith("https://"):
            # A pinned digest makes the TRANSPORT less critical, but plain http
            # also gives an attacker the URL-shape and the timing, and every
            # source this project uses offers TLS. Requiring it costs nothing.
            out.append("%s: url is not https:// — the digest pin authenticates "
                       "the CONTENT, TLS authenticates the SOURCE, and both are "
                       "cheap" % who)
        digest = a.get("sha256", "")
        if not _SHA256_RE.match(digest or ""):
            out.append("%s: sha256 %r is not 64 lowercase hex digits. A build "
                       "that fetches a third-party binary MUST pin it — this is "
                       "the whole supply-chain surface of the harness" % (who, digest))
        fmt = a.get("archiveFormat", "")
        if fmt not in ARCHIVE_FORMATS:
            out.append("%s: archiveFormat %r is not one this resolver can open "
                       "(known: %s). Declaring an unopenable kind fails on some "
                       "other machine, months later, instead of here"
                       % (who, fmt, ", ".join(sorted(ARCHIVE_FORMATS))))
        members = a.get("members")
        if not isinstance(members, list) or not members:
            out.append("%s: no members — an archive nothing is taken from is a "
                       "download for its own sake" % who)
            continue
        for m in members:
            as_name = m.get("as", "")
            if not m.get("member"):
                out.append("%s: a member declares no `member` path" % who)
            if not as_name:
                out.append("%s: a member declares no `as` name — the name it is "
                           "materialised under is what tclNames/zNames match" % who)
                continue
            if "universal" not in m:
                out.append("%s member %r: no `universal` key. Whether the file is "
                           "a Mach-O universal archive that must be sliced to this "
                           "leg's arch is a CLAIM about the artefact, and `false` "
                           "is as much a claim as `true` — a reader must not have "
                           "to know a default" % (who, as_name))
            elif not isinstance(m["universal"], bool):
                out.append("%s member %r: `universal` is %r, not a JSON boolean"
                           % (who, as_name, m["universal"]))
            imp = m.get("importName", "")
            if not imp:
                out.append(
                    "%s member %r: no `importName`. An ACQUIRED library is a "
                    "STAND-IN — the target machine loads its own copy — so the "
                    "identity to record (DT_NEEDED / LC_LOAD_DYLIB / import DLL "
                    "name) is a fact about the TARGET, never about whoever "
                    "packaged the download. Leaving it out is how a binary comes "
                    "to demand /opt/local on a Mac that has no MacPorts."
                    % (who, as_name))
            elif imp.strip() != imp:
                # `not imp.strip()` is deliberately NOT tested here: an
                # all-whitespace importName is already caught by `not imp` being
                # false and this branch reporting the whitespace. Testing it
                # again would be a dead condition that also makes the rule above
                # look optional — and it made a red-on-disable measurement lie
                # once already (disabling `if not imp:` still refused, through
                # this branch, with a message about whitespace on an EMPTY
                # string, so the mutation looked like a passing test).
                out.append("%s member %r: importName %r has leading/trailing "
                           "whitespace" % (who, as_name, imp))
            if as_name in tcl_names:
                saw_tcl = True
            elif as_name in z_names:
                saw_z = True
            else:
                out.append(
                    "%s member %r: the `as` name matches neither this leg's "
                    "tclNames (%s) nor its zNames (%s), so nothing will ever "
                    "resolve to it — an acquired file no search can find is a "
                    "download that silently does nothing"
                    % (who, as_name, ", ".join(sorted(tcl_names)) or "<none>",
                       ", ".join(sorted(z_names)) or "<none>"))
    if not saw_tcl:
        out.append("leg '%s': provider 'pinned-archive' acquires no member "
                   "matching tclNames (%s)" % (label, ", ".join(sorted(tcl_names))))
    if not saw_z:
        out.append("leg '%s': provider 'pinned-archive' acquires no member "
                   "matching zNames (%s)" % (label, ", ".join(sorted(z_names))))
    # Slicing needs a cpu_type for the leg's own arch, and the failure mode of
    # not having one is "cannot pick a slice" at acquisition time on a machine
    # that may be nowhere near a developer.
    arch = spec_target_arch(spec)
    if any(m.get("universal") for a in archives for m in a.get("members", [])) \
            and arch not in MACHO_CPU_TYPES:
        out.append("leg '%s': declares a `universal` member but its target arch "
                   "%r has no cpu_type in this resolver (known: %s)"
                   % (label, arch, ", ".join(sorted(MACHO_CPU_TYPES))))
    return out


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
        findings.extend(_lint_acquire(label, spec, libs))
        if not build.get("targetCc", {}).get("candidates"):
            findings.append("leg '%s': no targetCc candidates — the corpus's "
                            "dlopen()ed helper extension could not be built for it"
                            % label)
        if not build.get("sharedLibFlags"):
            findings.append("leg '%s': no sharedLibFlags" % label)
        # ── the helper extension's FILE NAME, per target ─────────────────────
        # DECLARED, then cross-checked against the target OS the spec already
        # names — the same discipline as POSIX_ONLY_ZCONF_GUARDS above, and for
        # the same reason: the value is DERIVABLE, so a declaration that
        # disagrees with the target is a defect a lint can catch host-free
        # instead of a corpus quietly building its own helper hours later
        # (D-HARNESS-LOADEXT-HELPER-TARGET-BLINDNESS-NOW-ABORTS-THE-RUN).
        declared_helper = build.get("loadExtHelperName")
        want_helper = LOADEXT_HELPER_NAME_BY_TARGET_OS.get(target_os)
        if not declared_helper:
            findings.append(
                "leg '%s': no build.loadExtHelperName — every leg declares the "
                "file name sqlite's test/loadext.test looks for on ITS target "
                "(MEASURED at test/loadext.test:26-29: '%s' on a Windows Tcl, "
                "'%s' elsewhere). Omitting it is how one POSIX spelling came to "
                "be staged for a Windows leg, whose fixture then never found it "
                "and self-built one with a hardcoded `gcc`."
                % (label, LOADEXT_HELPER_NAME_BY_TARGET_OS["windows"],
                   LOADEXT_HELPER_NAME_BY_TARGET_OS["linux"]))
        elif target_os in TARGET_OS_NAMES and declared_helper != want_helper:
            findings.append(
                "leg '%s': targets OS '%s' but declares loadExtHelperName %r; "
                "sqlite's test/loadext.test looks for %r there. A helper staged "
                "under any other name is invisible to the corpus, which then "
                "falls back to building its own with a hardcoded compiler."
                % (label, target_os, declared_helper, want_helper))
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

    # VERBATIM, and this replaced a check that asserted the OPPOSITE. Until
    # 2026-08-04 this verb re-spelled `\` as `/` before calling wslpath, to route
    # around `wslpath: C:ab` — a symptom misattributed to wslpath when its cause
    # was the local shell `wsl.exe` runs without `-e`. The pin now holds the
    # property that made the workaround unnecessary: what the driver holds is
    # what the tool is asked about.
    check("windows-to-wsl hands the translator the path VERBATIM",
          translate_path("windows-to-wsl", "C:\\a\\b", runner=_ok_runner)
          == "/mnt/c/a/b" and seen and seen[0][-1] == "C:\\a\\b",
          "translator saw %r" % (seen[0] if seen else None))
    check("windows-to-wsl invokes the DECLARED translator argv",
          bool(seen) and seen[0][:-1]
          == PATH_TRANSLATIONS["windows-to-wsl"]["translator"],
          "argv=%r" % (seen[0] if seen else None))
    # D-TOOLS-WSL-EXE-WITHOUT-DASH-E-RUNS-A-LOCAL-SHELL, pinned where the argv is
    # DECLARED. `wsl.exe <cmd>` and `wsl.exe -- <cmd>` both hand the line to the
    # distro's default shell first; only `-e`/`--exec` reaches the binary. ✔BOTH
    # MEASURED 2026-08-04: `wsl.exe -- /nope` answers `/bin/bash: line 1: /nope`
    # while `wsl.exe -e /nope` answers `execvpe(/nope) failed`, and `wsl.exe --`
    # expands a matching glob argument into two.
    for verb, spec in sorted(PATH_TRANSLATIONS.items()):
        argv = spec["translator"]
        if not argv or os.path.basename(argv[0]).lower() not in ("wsl", "wsl.exe"):
            continue
        check("pathTranslation %r runs wsl.exe with -e, not through a local "
              "shell" % verb,
              len(argv) > 1 and argv[1] in ("-e", "--exec"),
              "translator argv=%r — without -e the path is parsed by WSL's "
              "default shell before wslpath sees it, and `\\` is that shell's "
              "escape character" % (argv,))
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
          len(statements) == 1 + len(labels) * 25,
          "got %d statements for %d legs" % (len(statements), len(labels)))
    check("the sh emitter carries the helper extension's target-keyed name",
          "LEG_LOADEXT_NAME[" in sh,
          "without it build-and-test.sh spells one POSIX file name for five "
          "targets, which is D-HARNESS-LOADEXT-HELPER-TARGET-BLINDNESS-NOW-"
          "ABORTS-THE-RUN's second half")
    check("the sh emitter carries the launcher's path namespace",
          "LEG_PATH_TRANSLATION[" in sh and "LEG_PATH_TRANSLATOR[" in sh,
          "build-and-test.sh cannot translate a launcher's paths without it")
    check("the sh emitter carries the launcher's environment namespace",
          "LEG_ENV_TRANSFER[" in sh,
          "build-and-test.sh cannot forward its run environment without it")
    check("the sh emitter carries each leg's declared runtime identity",
          "LEG_LIB_TCL_IMPORT_NAME[" in sh and "LEG_LIB_Z_IMPORT_NAME[" in sh,
          "a driver cannot record the target's library identity without it")
    for stmt in statements:
        check("the sh emitter emits assignments only",
              ASSIGNMENT_RE.match(stmt) is not None, stmt)

    # ── Declared library acquisition ────────────────────────────────────────
    # D-HARNESS-LIBRARY-ACQUISITION-BUILT-FOR-ONE-LEG-IN-ONE-DRIVER. Everything
    # here is PURE — `acquire_plan` touches neither the network nor the disk —
    # so the properties hold on a gate machine with no connectivity at all. The
    # end-to-end fetch is exercised by the drivers, which is the right place for
    # it: a unit test that downloaded 5 MB would be a unit test that fails when
    # MacPorts is slow.
    acquiring = [leg for leg in legs
                 if leg["build"]["libraries"].get("provider") == "pinned-archive"]
    check("some leg actually declares the acquisition route",
          bool(acquiring),
          "the whole mechanism would be untested by these assertions otherwise")
    # A cache root that does not exist, so a machine whose REAL cache happens to
    # be populated still exercises the cold-cache refusal. Nothing creates it:
    # `acquire` validates and refuses before it makes a single directory, which
    # is itself one of the properties asserted below.
    #
    # ★ UNIQUE PER RUN, and that is not tidiness. A fixed name made this
    # self-test HISTORY-DEPENDENT: a red-on-disable experiment that deliberately
    # broke the offline refusal created the directory, and every later run then
    # failed the non-vacuity guard below for a reason that had nothing to do with
    # the code under test. A fixture whose outcome depends on what ran before it
    # is a fixture that will eventually be "fixed" by deleting the guard.
    import tempfile
    import uuid
    root = os.path.join(tempfile.gettempdir(),
                        "dss-harness-legs-selftest-%d-%s"
                        % (os.getpid(), uuid.uuid4().hex))
    check("the self-test's cache root really is absent", not os.path.exists(root),
          "%s exists — the cold-cache assertions below would be vacuous" % root)
    seen_digests = {}
    for leg in acquiring:
        lbl = leg["label"]
        ap = acquire_plan(leg, root)
        check("acquire plan targets the LEG's arch, not the host's (%s)" % lbl,
              ap["targetArch"] == spec_target_arch(leg["spec"]),
              "plan says %r" % ap["targetArch"])
        check("the acquisition cache is OUTSIDE the repository (%s)" % lbl,
              HERE not in ap["cacheDir"] and ap["cacheDir"].startswith(root),
              ap["cacheDir"])
        check("two legs cannot collide on one cache dir (%s)" % lbl,
              lbl in ap["cacheDir"], ap["cacheDir"])
        for a in ap["archives"]:
            check("every acquired archive is content-addressed by its digest (%s)"
                  % lbl, a["sha256"] in a["download"], a["download"])
            check("a pinned digest is 64 hex chars (%s)" % lbl,
                  _SHA256_RE.match(a["sha256"]) is not None, a["sha256"])
            seen_digests.setdefault(a["sha256"], set()).add(a["url"])
            for m in a["members"]:
                check("every acquired member declares the identity to RECORD "
                      "(%s :: %s)" % (lbl, m["as"]),
                      bool(m["importName"]),
                      "an acquired library is a stand-in; its embedded identity "
                      "belongs to the packager, not to the target")
                check("an acquired member materialises inside its leg's cache "
                      "dir (%s :: %s)" % (lbl, m["as"]),
                      m["path"].startswith(ap["cacheDir"]), m["path"])
    # A digest reused ACROSS legs is correct and deliberate — the macho archives
    # are universal, so one download serves both legs and the second is a cache
    # hit. A digest naming two different URLs is not: content addressing would
    # then be a lie, and whichever URL was fetched first would win silently.
    check("one digest never names two different URLs",
          all(len(u) == 1 for u in seen_digests.values()),
          "%r" % {d: sorted(u) for d, u in seen_digests.items() if len(u) > 1})
    declared_archives = sum(len(acquire_plan(l, root)["archives"]) for l in acquiring)
    check("a universal archive is DOWNLOADED ONCE and shared by the legs slicing it",
          len(seen_digests) < declared_archives if len(acquiring) > 1 else True,
          "%d distinct digests across %d declared archives — each leg carrying "
          "its own private copy would double the fetch"
          % (len(seen_digests), declared_archives))
    for leg in acquiring:
        tcl_i, z_i = acquired_import_names(leg)
        check("the resolver decides WHICH acquired file is the Tcl one (%s)"
              % leg["label"], bool(tcl_i) and bool(z_i),
              "tcl=%r z=%r — a driver must never make this match itself"
              % (tcl_i, z_i))
    # A provider with no acquisition route must produce no acquisition plan, or
    # a driver would call --acquire on a leg that declares nothing.
    for leg in legs:
        if leg["build"]["libraries"].get("provider") == "pinned-archive":
            continue
        check("a non-acquiring leg declares no archives (%s)" % leg["label"],
              not acquire_plan(leg, root)["archives"])
        tcl_i, z_i = acquired_import_names(leg)
        check("a non-acquiring leg overrides no identity (%s)" % leg["label"],
              not tcl_i and not z_i,
              "a host-supplied library already carries the right one")

    # `--offline` must REFUSE, never fall back. Asserted against a cache root
    # that cannot exist, so a machine that happens to have the real cache
    # populated still exercises the refusal.
    # `_forbidden_downloader` raises a NON-LegError, and `_raises` only counts a
    # LegError — so this single assertion covers both halves at once: it is TRUE
    # only if acquisition refused loudly AND never reached for the network.
    if acquiring:
        check("--offline with a cold cache REFUSES loudly and takes no "
              "round-trip",
              _raises(lambda: acquire(acquiring[0], root, offline=True,
                                      downloader=_forbidden_downloader)))
        check("--acquire on a leg that declares no route is a refusal, not a "
              "search",
              _raises(lambda: acquire(
                  [l for l in legs
                   if l["build"]["libraries"].get("provider") != "pinned-archive"][0],
                  root, offline=True, downloader=_forbidden_downloader)))

    # ── The DSS argv for one resolved library ───────────────────────────────
    check("no override => the argv every leg has always used",
          resolve_library_argv("/tmp/libz.so.1") == ["--resolve-library", "/tmp/libz.so.1"])
    check("an override is passed through the one named flag",
          resolve_library_argv("/tmp/libz.1.dylib", "@loader_path/libz.1.dylib")
          == [DSS_RESOLVE_LIBRARY_FLAG,
              "/tmp/libz.1.dylib=@loader_path/libz.1.dylib"])
    check("a path the override spelling CANNOT express is a refusal, not a "
          "truncation",
          _raises(lambda: resolve_library_argv("/tmp/a=b/libz.1.dylib",
                                               "@loader_path/libz.1.dylib")),
          "the compiler splits on the LAST '=', so this path would be silently "
          "cut and the recorded identity would be nonsense")
    check("a path containing '=' is fine when nothing is overridden",
          resolve_library_argv("/tmp/a=b/libz.so.1")
          == [DSS_RESOLVE_LIBRARY_FLAG, "/tmp/a=b/libz.so.1"],
          "there is no separator to be confused by when there is no suffix")
    check("a compiler without the flag REFUSES rather than dropping the override",
          _raises(lambda: resolve_library_argv("/tmp/x.dylib", "@loader_path/x.dylib",
                                               supported=False)),
          "dropping it would bake the packager's install name into the artefact "
          "and fail at LOAD time on a machine this host cannot observe")
    check("an unsupported compiler is irrelevant when nothing is overridden",
          resolve_library_argv("/tmp/x.so", "", supported=False)
          == ["--resolve-library", "/tmp/x.so"])
    # ★ The probe must distinguish a compiler that has the OVERRIDE from one that
    # merely has `--resolve-library` — every DSS ever built has the latter, so a
    # probe for the bare flag name would report support that is not there and
    # bake the packager's install name into the artefact.
    check("override support is PROBED from the compiler's own --help",
          dss_supports_import_name(
              "dss", runner=lambda a: DSS_IMPORT_NAME_HELP_MARKER)
          and not dss_supports_import_name(
              "dss", runner=lambda a: "  --resolve-library <path>  read a binary"),
          "assuming it would silently emit an artefact with the wrong identity")

    # ── the target C compiler: a NAME is not a declaration of TARGET ─────────
    # D-HARNESS-LOADEXT-HELPER-TARGET-BLINDNESS-NOW-ABORTS-THE-RUN. Every triple
    # below is either ✔MEASURED on a machine this project owns (marked) or a
    # DOCUMENTED spelling of the same shape; the rule under test is pure, so it
    # is tested as a table rather than by running a compiler.
    check("the probe argv is spelled ONE way",
          cc_machine_argv("gcc") == ["gcc", "-dumpmachine"])
    for triple, spec, want in [
        # ✔MEASURED 2026-08-05, WSL/Ubuntu x86_64 — `gcc -dumpmachine`.
        ("x86_64-linux-gnu", "x86_64:elf64-x86_64-linux-exec", True),
        # ★ THE DEFECT ITSELF: that same host gcc, offered for the pe64 leg.
        ("x86_64-linux-gnu", "x86_64:pe64-x86_64-windows-exec", False),
        # ✔MEASURED 2026-08-05, this project's Windows box — `gcc -dumpmachine`
        # AND `x86_64-w64-mingw32-gcc -dumpmachine` both print this, which is why
        # the bare name `gcc` must stay a legitimate pe64 candidate.
        ("x86_64-w64-mingw32", "x86_64:pe64-x86_64-windows-exec", True),
        ("x86_64-w64-mingw32", "x86_64:elf64-x86_64-linux-exec", False),
        # ✔MEASURED 2026-08-05, WSL — `aarch64-linux-gnu-gcc -dumpmachine`.
        ("aarch64-linux-gnu", "arm64:elf64-aarch64-linux-exec", True),
        # ★ D-HARNESS-ARM64-LEG-HOST-ARCH-HELPER-SO, both directions: the arm64
        # host's native gcc offered for the x86_64 leg, and vice versa.
        ("aarch64-linux-gnu", "x86_64:elf64-x86_64-linux-exec", False),
        ("x86_64-linux-gnu", "arm64:elf64-aarch64-linux-exec", False),
        # DOCUMENTED clang spellings — 4-part triples, and an OS token carrying a
        # version suffix, which is why machine_target_os matches by prefix.
        ("arm64-apple-darwin24.4.0", "arm64:macho64-arm64-darwin-exec", True),
        ("x86_64-apple-darwin24.4.0", "x86_64:macho64-x86_64-darwin-exec", True),
        ("arm64-apple-darwin24.4.0", "x86_64:macho64-x86_64-darwin-exec", False),
        ("x86_64-pc-linux-gnu", "x86_64:elf64-x86_64-linux-exec", True),
        ("x86_64-alpine-linux-musl", "x86_64:elf64-x86_64-linux-exec", True),
        ("x86_64-pc-windows-msvc", "x86_64:pe64-x86_64-windows-exec", True),
        # A triple that names no OS at all (bare metal) matches nothing here.
        ("arm-none-eabi", "arm64:elf64-aarch64-linux-exec", False),
        # An i686 compiler is not an x86_64 one, whatever its OS says.
        ("i686-linux-gnu", "x86_64:elf64-x86_64-linux-exec", False),
    ]:
        got, why = machine_matches_spec(triple, spec)
        check("`%s` %s build %s" % (triple, "CAN" if want else "CANNOT", spec),
              got == want, "reason given: %s" % why)
    check("a compiler that printed NOTHING is refused, not read as a match",
          machine_matches_spec("", "x86_64:elf64-x86_64-linux-exec")[0] is False)
    check("both outcomes carry a reason",
          all(machine_matches_spec(t, s)[1]
              for t, s in [("x86_64-linux-gnu", "x86_64:elf64-x86_64-linux-exec"),
                           ("x86_64-linux-gnu", "x86_64:pe64-x86_64-windows-exec")]))

    # resolve_target_cc, with the host injected. `which` and the probe are both
    # stubs, so these cells are reproducible on every machine — including the
    # WSL cell that produced the measured failure this whole change came from.
    _pe = leg_by_label(legs, "pe64-x86_64", path)
    _elf64 = leg_by_label(legs, "elf64-x86_64", path)

    def _host(present, machines):
        """(which, runner) for a pretend host."""
        return ((lambda cc: ("/usr/bin/" + cc) if cc in present else None),
                (lambda argv: (0, machines[argv[0]]) if argv[0] in machines
                              else (1, "unrecognised option '-dumpmachine'")))

    _w, _r = _host({"gcc", "cc"}, {"gcc": "x86_64-linux-gnu\n",
                                   "cc": "x86_64-linux-gnu\n"})
    cc, machine, rej = resolve_target_cc(_pe, runner=_r, which=_w)
    check("★ THE MEASURED FAILURE, REPRODUCED: a Linux host gcc is NOT accepted "
          "for the pe64 leg", cc == "",
          "accepted %r (%s) — this is the fallback that produced "
          "'relocation R_X86_64_PC32 ... recompile with -fPIC' and killed a run "
          "after two legs had gone green" % (cc, machine))
    check("...and the refusal names every candidate it looked at",
          len(rej) == len(_pe["build"]["targetCc"]["candidates"]),
          "rejections=%r" % (rej,))
    check("...naming the absent cross-compiler by name",
          any("x86_64-w64-mingw32-gcc" in r for r in rej), "%r" % (rej,))

    _w, _r = _host({"gcc"}, {"gcc": "x86_64-w64-mingw32\n"})
    cc, machine, _ = resolve_target_cc(_pe, runner=_r, which=_w)
    check("a WINDOWS host's bare `gcc` IS the pe64 leg's compiler",
          (cc, machine) == ("gcc", "x86_64-w64-mingw32"),
          "got %r/%r — deleting 'gcc' from the candidates would host-lock this "
          "leg in the other direction" % (cc, machine))

    _w, _r = _host({"x86_64-w64-mingw32-gcc", "gcc"},
                   {"x86_64-w64-mingw32-gcc": "x86_64-w64-mingw32\n",
                    "gcc": "x86_64-linux-gnu\n"})
    cc, _, _ = resolve_target_cc(_pe, runner=_r, which=_w)
    check("the cross-compiler wins when both are present",
          cc == "x86_64-w64-mingw32-gcc", "got %r" % cc)

    _w, _r = _host({"cc", "gcc", "clang"}, {"cc": "aarch64-linux-gnu\n",
                                            "gcc": "aarch64-linux-gnu\n",
                                            "clang": "aarch64-linux-gnu\n"})
    cc, _, _ = resolve_target_cc(_elf64, runner=_r, which=_w)
    check("an arm64 Linux host's native cc is NOT accepted for the x86_64 leg",
          cc == "",
          "D-HARNESS-ARM64-LEG-HOST-ARCH-HELPER-SO — got %r" % cc)

    _w, _r = _host({"cc"}, {})   # present, but the probe fails
    cc, _, rej = resolve_target_cc(_elf64, runner=_r, which=_w)
    check("a compiler that cannot STATE its target is refused, not assumed",
          cc == "" and any("cannot state its target" in r for r in rej),
          "rejections=%r" % (rej,))

    _w, _r = _host(set(), {})
    cc, _, rej = resolve_target_cc(_elf64, runner=_r, which=_w)
    check("nothing on PATH is a refusal that says so",
          cc == "" and all("not on PATH" in r for r in rej), "%r" % (rej,))

    # ── the helper extension's name is a TARGET fact ─────────────────────────
    for leg in legs:
        declared = loadext_helper_name(leg)
        want = LOADEXT_HELPER_NAME_BY_TARGET_OS.get(spec_target_os(leg["spec"]))
        check("leg '%s' declares the helper name its target's Tcl looks for"
              % leg["label"], declared == want,
              "declared %r, sqlite's test/loadext.test wants %r on target OS %r"
              % (declared, want, spec_target_os(leg["spec"])))
    check("the two spellings are the two sqlite has",
          sorted(set(LOADEXT_HELPER_NAME_BY_TARGET_OS.values()))
          == ["libtestloadext.so", "testloadext.dll"])

    # RED ON DISABLE: the lint must actually catch a wrong declaration, so it is
    # fed one. A mutated COPY on disk — never the shipped catalogue.
    import tempfile as _tf
    _mut_dir = _tf.mkdtemp(prefix="dss-legs-lint-")
    try:
        with open(path, "r", encoding="utf-8") as _f:
            _doc = json.load(_f)
        for _variant, _mutate in (
            ("a POSIX helper name on the Windows leg",
             lambda l: l["build"].update(loadExtHelperName="libtestloadext.so")),
            ("no helper name at all",
             lambda l: l["build"].pop("loadExtHelperName", None)),
        ):
            _copy = json.loads(json.dumps(_doc))
            for _l in _copy["legs"]:
                if spec_target_os(_l["spec"]) == "windows":
                    _mutate(_l)
            _p = os.path.join(_mut_dir, "legs.json")
            with open(_p, "w", encoding="utf-8") as _f:
                json.dump(_copy, _f)
            _found = [f for f in lint(_p) if "loadExtHelperName" in f]
            check("the lint REDS on %s" % _variant, bool(_found),
                  "lint said nothing about it — the check is dead config")
    finally:
        shutil.rmtree(_mut_dir, ignore_errors=True)

    out.write("passed=%d failed=%d\n" % (passed, failed))
    return 0 if failed == 0 else 1


def _forbidden_downloader(url, dest, timeout=120):
    """Injected where a network call would be a DEFECT. Raises something that is
    NOT a LegError, so a test asserting `LegError` cannot pass by accident when
    the code under test does reach for the network."""
    raise AssertionError("network round-trip taken where none is permitted: %s" % url)


# ── CLI ─────────────────────────────────────────────────────────────────────

def main(argv=None):
    # ✔MEASURED 2026-08-04, and it is PRE-EXISTING (reproduced at HEAD, rc=1):
    # `harness_legs.py --help` CRASHES on a Windows console. Python picks the
    # console's cp1252 for stdout, this file's prose is full of `→`/`★`/`—`, and
    # argparse's write raises UnicodeEncodeError before a single line of help
    # appears. The same mechanism was quietly degrading every diagnostic: the
    # `CHECKSUM MISMATCH — refusing` refusal printed as `CHECKSUM MISMATCH ?
    # refusing`. A tool whose --help cannot run on one of its two supported hosts
    # is a tool nobody reads the help of, so this is fixed rather than worked
    # around by de-punctuating the prose. `errors="replace"` keeps the old
    # degrade-don't-die behaviour for anything a terminal still cannot render.
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except (AttributeError, ValueError, OSError):
            pass   # a redirected/wrapped stream that cannot be reconfigured
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
    p.add_argument("--resolve-target-cc", default=None, metavar="LABEL",
                   help="pick LABEL's target C compiler — the FIRST declared "
                        "targetCc candidate that is on PATH AND proves, via "
                        "`" + CC_TARGET_MACHINE_FLAG + "`, that it targets this "
                        "leg. Prints '<cc>\\t<triple>' on stdout; the per-"
                        "candidate ladder always goes to stderr. Exits 3 when "
                        "none qualifies — a candidate that cannot state its "
                        "target is REFUSED, never assumed, because the compiler "
                        "builds this leg's dlopen()ed loadext helper and a "
                        "wrong-target one false-reds every loadext-* unit.")
    p.add_argument("--acquire", default=None, metavar="LABEL",
                   help="materialise LABEL's declared `pinned-archive` libraries "
                        "(download -> verify pinned sha256 -> extract -> slice to "
                        "this leg's arch) into the cache and print the result as "
                        "JSON. IDEMPOTENT and OFFLINE-CAPABLE: a populated cache "
                        "makes no network call. Implemented HERE, once, so both "
                        "drivers acquire identically.")
    p.add_argument("--acquire-plan", default=None, metavar="LABEL",
                   help="print, as JSON, what --acquire WOULD do for LABEL — the "
                        "archives, digests and cache paths — touching neither the "
                        "network nor the filesystem")
    p.add_argument("--cache-root", default=None, metavar="DIR",
                   help="where acquired libraries are cached (default: "
                        "$DSS_HARNESS_CACHE_ROOT, else ~/.cache/dss-code-prime). "
                        "OUTSIDE the repository, always.")
    p.add_argument("--offline", action="store_true",
                   help="refuse to reach the network: --acquire completes from "
                        "the cache or FAILS naming what is missing. It never "
                        "falls back to whatever else is on the machine.")
    p.add_argument("--resolve-library-argv", default=None, metavar="PATH",
                   help="print, one token per line, the argv that hands DSS the "
                        "library at PATH — including the import-name override "
                        "when --import-name is given. THIS is what keeps the "
                        "compiler flag named in one file instead of two drivers.")
    p.add_argument("--import-name", default="", metavar="NAME",
                   help="the runtime identity to record for --resolve-library-argv "
                        "(a leg plan's LEG_LIB_TCL_IMPORT_NAME / _Z_IMPORT_NAME). "
                        "Empty = the library's own embedded identity is correct.")
    p.add_argument("--dss", default="", metavar="PATH",
                   help="the compiler --resolve-library-argv is being built for. "
                        "Given with a non-empty --import-name, its --help is "
                        "PROBED for override support and a compiler without it is "
                        "a LOUD refusal, never a silently dropped override.")
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
            or args.env_transfers or args.env_transfer
            or args.acquire or args.acquire_plan or args.resolve_library_argv
            or args.resolve_target_cc):
        p.error("one of --verdict-vocabulary / --plan / --header-stages / --lint "
                "/ --self-test / --path-translations / --translate-path / "
                "--assert-translated / --env-transfers / --env-transfer / "
                "--acquire / --acquire-plan / --resolve-library-argv / "
                "--resolve-target-cc is required")
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
        if args.resolve_library_argv:
            supported = True
            if args.import_name and args.dss:
                supported = dss_supports_import_name(args.dss)
            for tok in resolve_library_argv(args.resolve_library_argv,
                                            args.import_name, supported):
                sys.stdout.write("%s\n" % tok)
            return 0
        if args.resolve_target_cc:
            leg = leg_by_label(load_catalogue(args.catalogue),
                               args.resolve_target_cc, args.catalogue)
            cc, machine, rejections = resolve_target_cc(leg)
            # The LADDER goes to stderr on BOTH outcomes: on success it says which
            # candidates were passed over and why, and on failure it IS the
            # diagnostic the driver puts in the leg's verdict. stdout carries only
            # the answer, so a caller can read it with a plain command
            # substitution and never has to parse prose out of it.
            for line in rejections:
                sys.stderr.write("  rejected %s\n" % line)
            if not cc:
                sys.stderr.write(
                    "no declared targetCc candidate for leg '%s' (%s) both exists "
                    "on this host AND targets %s. NOT falling back to whatever "
                    "compiler is here: it would build this leg's %s for the WRONG "
                    "target, the fixture could not load it, and every loadext-* "
                    "unit would false-red as a genuine DSS failure "
                    "[D-HARNESS-ARM64-LEG-HOST-ARCH-HELPER-SO].\n"
                    % (leg.get("label"), leg.get("spec"), leg.get("spec"),
                       loadext_helper_name(leg) or "loadext helper"))
                return 3
            sys.stdout.write("%s\t%s\n" % (cc, machine))
            return 0
        if args.acquire or args.acquire_plan:
            label = args.acquire or args.acquire_plan
            legs = load_catalogue(args.catalogue)
            root = cache_root(args.cache_root)
            leg = leg_by_label(legs, label, args.catalogue)
            result = (acquire_plan(leg, root) if args.acquire_plan
                      else acquire(leg, root, offline=args.offline))
            sys.stdout.write(json.dumps(result, indent=1, sort_keys=True) + "\n")
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
