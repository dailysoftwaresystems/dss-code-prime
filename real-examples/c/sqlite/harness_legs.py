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
  harness_legs.py --config-stages
  harness_legs.py --path-translations
  harness_legs.py --path-translation VERB --translate-path PATH [--translate-path …]
  harness_legs.py --path-translation VERB --assert-translated ARG [--assert-translated …]
  harness_legs.py --env-transfers
  harness_legs.py --env-transfer VERB --forward NAME [--forward NAME …] [--carrier-current V]
  harness_legs.py --check-launcher LABEL [--artifact PATH]
  harness_legs.py --identify-binary PATH
  harness_legs.py --launcher-for-target ARCH:CONTAINER:TARGETOS
  harness_legs.py --lint
  harness_legs.py --self-test
"""

import argparse
import hashlib
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
    "skipped-launcher-prerequisite-missing",
                                      # environmental: the launcher itself is
                                      #   PRESENT and looks fine, and something
                                      #   it DECLARED it needs beyond argv[0]
                                      #   (a sysroot, an ELF interpreter inside
                                      #   it, a program inside the distro it
                                      #   crosses into) is absent
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
LIBRARY_PROVIDERS = {"host-system", "search-paths", "pinned-archive"}
RECIPE_TRANSFORMS = {"none", "windows-selfconfig"}

# The archive kinds `--acquire` can open, and the `tarfile` mode that opens each.
# CLOSED on purpose: a `.pkg.tar.zst` (needs zstd, which is not in the stdlib
# before 3.14) is NOT here, so a catalogue that declares one fails the lint
# LOUDLY instead of failing at download time on some other machine, months later.
#
# ★ `deb` IS AN `ar` ARCHIVE WHOSE PAYLOAD IS A TAR, and it is opened by this
# module rather than by `dpkg-deb` — the tool the old `ubuntu-ports-arm64`
# provider shelled out to and which no Windows or macOS host has. The `ar` header
# is 60 fixed bytes; the payload member is `data.tar.<ext>`, and the <ext> decides
# which of the tar modes above opens it. ⚠ THE ZSTD WALL IS REAL AND MEASURED
# (2026-08-06): EVERY Ubuntu .deb — amd64 and arm64, jammy through current —
# ships `data.tar.zst`, so the "Ubuntu amd64 archive, literal sibling of the
# arm64 provider" is NOT convertible with the stdlib. Debian bookworm's are
# `data.tar.xz` and are. A `data.tar.zst` therefore gets a refusal that NAMES the
# wall instead of a stack trace from tarfile.
ARCHIVE_FORMATS = {
    "tar.bz2": "r:bz2",
    "tar.gz":  "r:gz",
    "tar.xz":  "r:xz",
    "deb":     "ar+data.tar",
}

# The inner-tar suffixes a `.deb` payload may carry, and the mode that opens each
# — the SAME modes as above, which is the point: `deb` adds a container, not a
# compressor.
DEB_PAYLOAD_MODES = {
    "data.tar.bz2": "r:bz2",
    "data.tar.gz":  "r:gz",
    "data.tar.xz":  "r:xz",
}

# ── WHICH COPY OF AN ACQUIRED LIBRARY ACTUALLY RUNS ─────────────────────────
#
# D-HARNESS-ACQUIRED-TCL-DYLIB-HAS-NO-SCRIPT-LIBRARY. Acquisition obtains a
# library's CODE. It does not obtain the RUNTIME DATA that library needs, and
# NOTHING AT BUILD TIME CAN SEE THE DIFFERENCE: the link succeeds, the binary
# runs, and it dies only on the code path that touches the data. Tcl (its script
# library), ICU (its data bundle) and tzdata all carry a baked-in directory path
# that acquisition silently leaves dangling.
#
# Whether that matters for a given member is decided by ONE question — WHICH COPY
# OF THE LIBRARY DOES THE ARTEFACT ACTUALLY LOAD? — so it is DECLARED per member
# rather than guessed:
#
#   staged-beside-artefact  the file we acquired IS the file that runs (the
#                           driver stages it next to the binary; Mach-O
#                           `@loader_path`, ELF LD_LIBRARY_PATH, the Windows
#                           app-directory search). ⇒ every runtime data
#                           directory it bakes in MUST be staged too, or the
#                           artefact dies at the first code path that reads it.
#   target-supplies-its-own the target machine loads ITS OWN copy by name, and
#                           our copy is a BUILD-TIME STAND-IN read for its export
#                           table. ⇒ its baked-in directories belong to that
#                           machine and staging ours would be the wrong data.
RUNTIME_COPIES = {"staged-beside-artefact", "target-supplies-its-own"}

# An `importName` that resolves NEXT TO THE LOADING BINARY rather than by name or
# at an absolute path. Declared as a closed set so `runtimeCopy` can be
# CROSS-CHECKED against it instead of taken on trust.
IMPORT_NAME_BESIDE_PREFIXES = ("@loader_path/", "@executable_path/", "$ORIGIN/")

# What an absolute path baked into an acquired library IS. Both are claims a
# reader can check, and `inert` costs a `$why` — "nothing reads this" is exactly
# the sentence that was wrong about `/opt/local/lib/tcl8.6`.
EMBEDDED_PATH_KINDS = {"runtime-data", "inert"}

# What a staged data directory is FOR. `tclScriptLibrary` is singled out because
# a driver has to point `TCL_LIBRARY` at it; everything else is `generic` and is
# staged without any driver having to know what it is.
DATA_DIR_ROLES = {"tclScriptLibrary", "generic"}

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


# ── WHAT A FORWARDED VARIABLE'S VALUE MEANS ON THE OTHER SIDE ───────────────
#
# THE HALF `envTransfer` DOES NOT COVER, AND IT IS A DIFFERENT QUESTION.
# `envTransfer` answers WHETHER a variable crosses the launcher's environment
# boundary. It says NOTHING about whether the VALUE still means the same thing
# once it has crossed — and for a variable holding a path, it does not.
# [D-HARNESS-PS1-TCL-LIBRARY-NOT-FORWARDED-ACROSS-THE-WSL-BOUNDARY.]
#
# ✔MEASURED (TF-C123): `build-and-test.ps1` sets `TCL_LIBRARY` for a leg whose
# Tcl came from acquisition, and TCL_LIBRARY was NOT in the forward set — so a
# `wsl.exe`-launched leg did not carry it and Tcl could not find `init.tcl`.
# ⚠ AND THE OBVIOUS FIX IS A SECOND, QUIETER DEFECT: the value is a HOST path
# (`C:\…`), so merely NAMING it in the carrier hands a Windows path to a Linux
# process. Tcl then fails to find `init.tcl` again and the driver blames the
# acquisition instead of the boundary — the same symptom, a wrong diagnosis, and
# no signal that anything was translated or not.
#
# So a forwarded variable is not just a NAME: it is a name plus a claim about
# the NAMESPACE its value lives in. The claim is DECLARED, per variable, in one
# place, and there is no default — an undeclared name is REFUSED, because
# "nobody classified this yet" and "this value is namespace-neutral" are the two
# answers that must never look alike. That is the whole point: the next
# path-valued variable someone adds to a driver's forward list cannot be
# forwarded raw by accident, because it cannot be forwarded at all until its
# kind is written down here.
#
#   opaque       the value means the same thing in every namespace — a Tcl glob
#                list, a comma-separated file list, a flag. Crosses verbatim.
#   driver-path  the value is a path IN THIS DRIVER'S namespace. It MUST be put
#                through the launcher's declared `pathTranslation` before it
#                crosses; for a launcher declaring `none` that is the identity,
#                which is correct and still goes through the same door.
#
# A variable whose value the CATALOGUE declared for a specific launcher
# (`launchers[].env`, e.g. QEMU_LD_PREFIX) is a third case and is NOT in this
# table: it was written FOR that launcher, so it is already in the launcher's
# namespace by construction. Those cross through `declared` below, which is a
# separate argument precisely so that "the catalogue authored this value" is a
# statement a reader can see, not an omission they have to infer.
LAUNCH_FORWARD_KINDS = {
    "SQLITE_TEST_PATTERN_LIST": "opaque",
    "QUICKTEST_OMIT": "opaque",
    "TCL_LIBRARY": "driver-path",
}


def forward_kind(name):
    """The declared kind of a forwarded variable, or a LegError. Never a
    permissive default — see the note above: an unclassified variable that
    silently meant `opaque` is exactly the raw-path forward this exists to
    stop."""
    kind = LAUNCH_FORWARD_KINDS.get(name)
    if kind is None:
        raise LegError(
            "%r is not a DECLARED forwardable variable (declared: %s). A "
            "variable crossing a launcher's environment boundary must state "
            "the namespace its VALUE lives in: 'opaque' (means the same thing "
            "on both sides) or 'driver-path' (a path in THIS driver's "
            "namespace, which must go through the launcher's declared "
            "pathTranslation first). There is no default, because forwarding a "
            "HOST path verbatim does not fail as a path error — the callee "
            "opens a relative file of that name, misses, and the run reads as a "
            "broken binary. Add it to LAUNCH_FORWARD_KINDS with its kind, or "
            "pass it as a catalogue-declared launcher variable if legs.json "
            "authored its value for this launcher."
            % (name, ", ".join("%s=%s" % (k, v)
                               for k, v in sorted(LAUNCH_FORWARD_KINDS.items()))))
    return kind


def launch_forward_assignments(env_verb, path_verb, forwards, declared=(),
                               current="", runner=None):
    """Every `NAME=VALUE` a driver must apply so that the process its launcher
    spawns sees the run environment MEANING what this driver meant.

    `forwards` is [(name, value)] — the driver-set variables that are actually
    SET right now (the caller filters; see env_carrier_assignments' note on why
    an unset name in the carrier is a false green). `declared` is the names the
    CATALOGUE declared for this launcher, already in its namespace.

    Returns the value re-assignments FIRST (so a driver applying them in order
    has the translated value in place before the carrier names it), then the
    carrier. Empty for a verb whose child inherits — a native run stays
    byte-for-byte itself.

    `runner` is passed through to translate_path so the self-test can exercise
    the contract on a host with no translator installed."""
    # THE VERB FIRST, before any name is looked at: an unknown envTransfer is
    # fatal on its own terms, and diagnosing it as "undeclared variable" would
    # name the wrong thing.
    spec = env_transfer(env_verb)
    names = [n for n, _ in forwards if n] + [n for n in declared if n]
    if not spec["nameCarrier"] or not names:
        return []
    out = []
    for name, value in forwards:
        if not name:
            continue
        if forward_kind(name) != "driver-path":
            continue
        # A path-valued variable with no value would cross as EMPTY-BUT-EXISTING,
        # which is the failure mode that made the carrier filter load-bearing.
        if not value:
            raise LegError(
                "%r is declared 'driver-path' but was forwarded with no value. "
                "Only a variable that is actually SET may be carried: naming an "
                "unset one materialises it on the other side as EMPTY-BUT-"
                "EXISTING, which reads as a real setting rather than as absence."
                % name)
        # THE GUARD THE ANCHOR ASKS FOR, and it is here rather than at the call
        # site so that neither driver can be the one that forgets: a path-valued
        # variable may not cross a boundary whose translation nobody declared.
        if not path_verb:
            raise LegError(
                "%r is declared 'driver-path', so its value (%r) is in THIS "
                "driver's namespace — but no pathTranslation was declared for "
                "the launcher it is crossing to. A path forwarded without a "
                "declared translation is not a path error on the other side: "
                "the callee opens a relative file of that name and the run "
                "reads as a broken binary. Declare the launcher's "
                "pathTranslation (legs.json) and pass it here."
                % (name, value))
        out.append("%s=%s" % (name, translate_path(path_verb, value,
                                                   runner=runner)))
    return out + env_carrier_assignments(env_verb, names, current)


# ── Launcher run FILESYSTEM ─────────────────────────────────────────────────
#
# THE THIRD NAMESPACE, AND IT WAS FOUND THE SAME WAY THE FIRST TWO WERE — BY
# MEASURING. [D-HARNESS-WSL-LAUNCHED-LEG-RUNDIR-IS-DRVFS.]
#
# `pathTranslation` says how a launcher SPELLS a path and `envTransfer` says
# whether it sees this driver's environment. Neither says anything about the
# FILESYSTEM the launched process actually writes its databases onto — and a
# corpus whose subject is a database engine is, more than anything else, a test
# of a filesystem's semantics.
#
# ✔MEASURED 2026-08-06 on this host, ONE process, TWO directories:
#     /mnt/c/…  fs=v9fs      chmod 644 -> 777   chmod 400 -> 555
#     /tmp      fs=ext2/ext3 chmod 644 -> 644   chmod 400 -> 400
# `/mnt/c` is mounted `9p … aname=drvfs;…` with NO `metadata` option, so DrvFs
# derives the whole POSIX mode from the Windows read-only ATTRIBUTE alone. Every
# sqlite unit that asserts anything about file permissions therefore fails, and
# ✔MEASURED BY A 2x2 MATCHED CONTROL ({DSS fixture, gcc reference} x {DrvFs, ext4})
# all 60 of them fail IDENTICALLY with the gcc reference on DrvFs and VANISH on
# ext4 — wal2 20, zipfile 12, e_walauto 10, journal3 8, attach 2, tkt3457 1, and
# a pager4 ABORT. Zero of them are DSS-attributable.
#
# ⛔ AND THEY MUST NOT BECOME CONFOUNDS. The mechanism is known, it is OURS, and
# it is fixable: a confound row for them would permanently launder a harness
# misconfiguration into "expected", using the very mechanism `confounds` below
# exists to keep honest.
#
# So a launcher entry declares WHICH FILESYSTEM its leg's run directory lives in,
# in the same shape and for the same reason as its two siblings: closed
# vocabulary, required on every entry, no default, unknown verb is a LOUD refusal.
#   root            where run directories go in the LAUNCHER's own filesystem.
#                   "" = the launcher shares this driver's filesystem and the
#                   driver's own run directory IS the run directory.
#   workingDirArgv  how the launcher is told to start its child in a directory,
#                   as a template over `{dir}`. SPLICED IMMEDIATELY AFTER THE
#                   LAUNCHER'S PROGRAM NAME (argv[0]) — where a program's own
#                   options go, and before any option that introduces the child
#                   command (`-e`). `run_dir_plan` performs the splice so that
#                   neither driver knows the rule and the resolved argv can be
#                   asserted whole.  ✔MEASURED: `wsl.exe --cd /tmp -e pwd` -> /tmp.
#   mkdirArgv       argv PREFIX that creates a directory (+ parents) there.
#   rmTreeArgv      argv PREFIX that removes a tree there.
#   copyArgv        argv PREFIX that copies <src> <dst> INTO that filesystem. The
#                   source is spelled in the launcher's own path namespace, i.e.
#                   it has already been through `pathTranslation`.
#                   ⚠ EVERY ONE OF THESE IS A REAL argv, NEVER A SHELL STRING —
#                   `wsl.exe -e` exists precisely so no local shell re-parses it
#                   (D-TOOLS-WSL-EXE-WITHOUT-DASH-E-RUNS-A-LOCAL-SHELL), and a
#                   `sh -c` form here would hand that property straight back.
#   probeArgv       how to ASK, in the launcher's own filesystem, whether one of
#                   its declared `requires` rows is satisfied — one template per
#                   requirement kind, over `{path}`. `{}` means the launcher
#                   shares this process's filesystem and the probe is answered
#                   IN-PROCESS (os.path.isfile / isdir / shutil.which) with NO
#                   SPAWN AT ALL. See `requirement_probe_argv`.
#   validHostOs     the only host OS the mechanism exists on, so the lint CHECKS.
#   kernelEntryArgv argv PREFIX that runs an arbitrary program IN THIS VERB'S
#                   KERNEL. `[]` is the claim that goes with sharesDriverKernel
#                   True: this process is already in that kernel, so entering it
#                   is a no-op and anything asked here is asked in-process.
#                   ⚠ THIS IS THE KERNEL BOUNDARY, NOT THE LEG'S LAUNCHER. For
#                   elf64-arm64 from Windows the launcher is `wsl.exe -e
#                   qemu-aarch64`, and qemu is USER-MODE translation inside the
#                   very kernel `wsl.exe -e` already entered — it changes the
#                   INSTRUCTION SET, not the clock, the scheduler or the
#                   filesystem. So the entry mechanism is the whole of what an
#                   environment probe must go through, and going through the full
#                   launcher argv would additionally demand an aarch64 python3
#                   that does not exist — a probe that could not run, in order to
#                   measure something qemu cannot change.
#                   ★ ASSERTED, NOT ASSUMED: --self-test checks this argv is
#                   EQUAL TO the LONGEST COMMON PREFIX of every other argv
#                   template on the same entry (`argv_common_prefix`), so the
#                   boundary is spelled once and the table cannot drift into two
#                   answers about where its own kernel begins.
#                   ⚠ EQUAL TO, NOT "IS A PREFIX OF" — that weaker test is what
#                   this check used to be, and `['wsl.exe']` PASSED it, i.e. it
#                   would have accepted an entry mechanism with `-e` DROPPED
#                   (D-TOOLS-WSL-EXE-WITHOUT-DASH-E-RUNS-A-LOCAL-SHELL).
#                   ⚠ AND IT MUST AGREE WITH sharesDriverKernel: non-empty here
#                   ⟺ False there. See assert_kernel_declaration_coherent, which
#                   refuses the pair at the accessor rather than in prose.
#   kernelProbeInterpreter
#                   the program, IN THAT KERNEL, that runs this script there. ""
#                   for an in-process verb. Separate from kernelEntryArgv because
#                   they are two different facts and they fail differently: one is
#                   "the kernel could not be entered", the other is "it was, and
#                   nothing there can run this script".
#   kernelEntryPathTranslation
#                   the PATH_TRANSLATIONS verb that spells a DRIVER path for that
#                   kernel. `pathTranslation` on a launcher answers this for the
#                   launcher's argv; nothing answered it for the FILESYSTEM, and
#                   an environment probe crossing the boundary has no launcher.
RUN_FILESYSTEMS = {
    "driver": {
        "root": "",
        "workingDirArgv": [],
        "mkdirArgv": [],
        "rmTreeArgv": [],
        "copyArgv": [],
        # {} is NOT "unimplemented" — it is the claim that goes with `root: ""`:
        # this launcher's filesystem IS this process's, so the question is
        # answered by asking THIS python, and spawning anything to ask it would
        # be a round-trip whose answer could differ from the caller's own view.
        "probeArgv": {},
        "validHostOs": "",
        # ★ DOES THE LAUNCHED PROCESS SHARE THIS DRIVER'S KERNEL?
        # ANCHOR, ONE LINE, DO NOT WRAP (the registry guard matches the whole name):
        # D-HARNESS-ENVIRONMENT-PROBE-MEASURES-THE-DRIVERS-KERNEL-NOT-THE-LAUNCHED-ONE
        # `driver` is one machine, one kernel, one clock — Wine,
        # qemu-user and `arch -x86_64` are all in-process translation, so an
        # environment probe run by this driver is measuring the very environment
        # the fixture will get. Declared as a FIELD, not decided by looking at the
        # verb's NAME, so a new launcher has to state it.
        "sharesDriverKernel": True,
        # [] / "" / "none" are the three halves of ONE claim: this process is
        # already in that kernel. An environment probe is therefore answered by
        # THIS python, and spawning a copy of this script to ask would be a
        # round-trip whose answer could differ from the caller's own view — the
        # same reason `probeArgv` is {} here.
        "kernelEntryArgv": [],
        "kernelProbeInterpreter": "",
        "kernelEntryPathTranslation": "none",
    },
    "wsl-linux": {
        # /tmp, not $HOME: ✔MEASURED ext4 with 749 G free, and it is the one
        # directory a distro guarantees is writable without knowing whose distro
        # it is. NOT a tmpfs here (`stat -f -c %T /tmp` -> ext2/ext3), which
        # matters because the corpus writes multi-gigabyte databases.
        "root": "/tmp/dss-sqlite-harness",
        "workingDirArgv": ["--cd", "{dir}"],
        "mkdirArgv": ["wsl.exe", "-e", "mkdir", "-p"],
        "rmTreeArgv": ["wsl.exe", "-e", "rm", "-rf"],
        "copyArgv": ["wsl.exe", "-e", "cp", "-f"],
        # ⚠ THE `command` PROBE IS THE ONE PLACE A SHELL APPEARS IN THIS TABLE,
        # AND IT IS AGAINST THE TABLE'S OWN RULE ABOVE. READ THIS BEFORE
        # "FIXING" IT INTO A STRING.
        #
        # It is UNAVOIDABLE: "is <name> executable in this launcher's PATH?" is
        # answered by `command -v`, which is a SHELL BUILTIN — there is no
        # `/usr/bin/command`, and `which`/`type` are not guaranteed to exist as
        # binaries on a minimal distro (which is exactly the kind of distro this
        # probe was written to catch). So a shell is the instrument, not a
        # convenience.
        #
        # It is SAFE for one reason and one reason only: the path is passed as a
        # POSITIONAL ARGUMENT (`--` then `{path}`, read back as `$1`) and is
        # NEVER interpolated into the script text. The script is a fixed,
        # closed literal; nothing a catalogue can write reaches the parser.
        # Interpolating `{path}` into the `sh -c` string would hand back exactly
        # the property `wsl.exe -e` exists to guarantee
        # (D-TOOLS-WSL-EXE-WITHOUT-DASH-E-RUNS-A-LOCAL-SHELL) — a launcher argv
        # nobody can prove was the argv that ran.
        "probeArgv": {
            "file":      ["wsl.exe", "-e", "test", "-f", "{path}"],
            "directory": ["wsl.exe", "-e", "test", "-d", "{path}"],
            "command":   ["wsl.exe", "-e", "sh", "-c",
                          'command -v "$1" >/dev/null', "--", "{path}"],
        },
        "validHostOs": "windows",
        # ⚠ FALSE, AND IT IS THE ONE THAT MATTERS FOR THE CLOCK PROBE. This launcher
        # crosses into a DIFFERENT KERNEL — and the measured clock defect
        # (D-ENV-WSL2-CLOCK-REALTIME-STEPS-34S) is that kernel's, not Windows'.
        # ✔MEASURED 2026-08-12 on this host, minutes apart, one probe: the WINDOWS
        # clock is ABSENT (0 steps, 0.0000 s drift over 20.025 s / 80 samples) while
        # the WSL2 clock is PRESENT (8 steps, worst per-tick drift 36.549 s, spread
        # 36.549 s over 20.01 s / 80 samples).
        # ⇒ SO THE PROBE IS RUN *THERE*, THROUGH THE FIELDS BELOW. It used to be run
        # here and force-discarded for this leg — safe (an environment failure read
        # as GENUINE and got investigated) but lossy, and the loss was measured: a
        # Windows-driven corpus at 52cf784d reported 4 UNCLASSIFIED walsetlk reds on
        # elf64-x86_64 and 3 on elf64-arm64 that the arm64 VPS ran green from the
        # same DSS commit against the same upstream tree.
        "sharesDriverKernel": False,
        # `wsl.exe -e` and nothing more: the KERNEL boundary, which is exactly what
        # an environment measurement has to cross. `-e` is load-bearing here for the
        # same reason it is in every other argv on this entry
        # (D-TOOLS-WSL-EXE-WITHOUT-DASH-E-RUNS-A-LOCAL-SHELL): without it WSL
        # rebuilds a command LINE and feeds it to the distro's default shell.
        "kernelEntryArgv": ["wsl.exe", "-e"],
        # ⓘ THE DISTRO'S python3, NOT THIS DRIVER'S. Its ABSENCE is an ordinary,
        # expected answer on a minimal distro and it is handled as one: the
        # measurement is `unreachable`, every verdict for that kernel is
        # INDETERMINATE, and the rows go INACTIVE. Never `absent`.
        "kernelProbeInterpreter": "python3",
        # This script and the catalogue live on the DRIVER's filesystem, which this
        # kernel reaches only as /mnt/<drive>. `wslpath` owns that spelling.
        "kernelEntryPathTranslation": "windows-to-wsl",
    },
}


# ── WHICH KERNEL IS BEING MEASURED, AND HOW MANY TIMES ──────────────────────
#
# ANCHOR, ONE LINE, DO NOT WRAP:
# D-HARNESS-ENVIRONMENT-PROBE-MEASURES-THE-DRIVERS-KERNEL-NOT-THE-LAUNCHED-ONE
#
# The IDENTITY of a kernel, so a measurement can be taken ONCE and reused by every
# leg that executes in it. DERIVED from `sharesDriverKernel` rather than declared
# as a fourth field, and that is deliberate: `sharesDriverKernel: True` already
# MEANS "this is the driver's kernel", so a separate name for it could only ever
# agree or be wrong. Two distinct verbs that both answer False are treated as two
# kernels — which costs one extra sample if they are secretly the same box and can
# never produce a verdict about the wrong one.
def probe_kernel(fs_verb):
    """The kernel namespace a leg with this `runFilesystem` executes in."""
    return "driver" if fs_shares_driver_kernel(fs_verb) else fs_verb


def fs_shares_driver_kernel(verb):
    """Does a leg launched through this FILESYSTEM verb execute against THIS
    driver's kernel? THE ONE READER OF THAT FIELD.

    ★ ONE READER ON PURPOSE. "Which drawer does this leg open" (probe_kernel)
    and "does a measurement of this machine apply to it" are the same question
    asked twice, and two subscripts of the same key are two places for the next
    author to answer it differently.

    Never a bare subscript: a KeyError here would be a python traceback in the
    one function whose job is to say WHICH MACHINE a verdict describes."""
    return run_filesystem(verb)["sharesDriverKernel"]


def probe_kernel_names():
    """Every kernel namespace the declared table can resolve to. Always a SUBSET
    of RUN_FILESYSTEMS' own keys — asserted by --self-test — which is what lets a
    namespace be looked up in that table for its entry mechanism."""
    return {probe_kernel(v) for v in RUN_FILESYSTEMS}


def run_filesystem(verb):
    """The declared verb's spec, or a LegError — never a permissive default.

    Defaulting to `driver` for an unknown verb is exactly the defect this
    vocabulary exists to prevent: `driver` is itself a CLAIM — that the launched
    process writes onto the same filesystem this driver does — and it was the
    unstated, unexamined assumption that put a Linux sqlite corpus onto DrvFs.

    ★ AND THE SPEC IS CHECKED, NOT MERELY FOUND. This is the single accessor
    every consumer goes through, which makes it the one place a MALFORMED verb
    can be made UNUSABLE rather than usable-and-wrong."""
    spec = RUN_FILESYSTEMS.get(verb)
    if spec is None:
        raise LegError(
            "unknown runFilesystem %r (known: %s). A launcher declares which "
            "FILESYSTEM its leg's run directory lives in; an unrecognised verb "
            "cannot be silently treated as 'driver', because 'driver' asserts "
            "that the launched process writes onto this driver's own filesystem "
            "— and a filesystem that only APPROXIMATES POSIX semantics (DrvFs "
            "derives every mode bit from one Windows attribute) fails a database "
            "engine's corpus without failing anything this harness can see."
            % (verb, ", ".join(sorted(RUN_FILESYSTEMS))))
    assert_kernel_declaration_coherent(verb, spec)
    return spec


# ── THE TWO FIELDS THAT MUST AGREE, AND THE COMBINATION THAT RESTORES THE
#    DEFECT ────────────────────────────────────────────────────────────────
#
# ANCHOR, ONE LINE, DO NOT WRAP:
# D-HARNESS-ENVIRONMENT-PROBE-MEASURES-THE-DRIVERS-KERNEL-NOT-THE-LAUNCHED-ONE
#
# `sharesDriverKernel` picks WHICH DRAWER a leg reads (probe_kernel).
# `kernelEntryArgv` picks HOW a measurement gets there (kernel_probe_argv, which
# reads an EMPTY one as "already in that kernel, so measure IN THIS PROCESS").
# Nothing bound them, and the unbound pair is not a hypothetical — it is exactly
# what a NEW launcher looks like when its author answers the first question and
# forgets the second:
#
#   ⛔ sharesDriverKernel False + kernelEntryArgv []  — ✔MEASURED: `outcome:
#   in-process`, `why: "measured in this process, which is this driver's own
#   kernel"`, FILED UNDER THE FOREIGN KERNEL'S NAME. `in-process` is in
#   KERNEL_OUTCOMES_IN_FORCE, so both ELF legs HONOUR it. That is this anchor's
#   defect restored verbatim — a measurement of the driver's machine excusing
#   failures produced somewhere nobody measured — and the record contradicts
#   itself inside its own `why` string while nothing refuses.
#
# ⛔ AND IT IS NOT FIXED BY DERIVING ONE FROM THE OTHER. `sharesDriverKernel :=
# not kernelEntryArgv` RELOCATES the defect instead of removing it: the author
# who forgot the entry mechanism would then be silently DECLARED to share this
# driver's kernel — the same wrong answer, arrived at more quietly, with no
# second field left to disagree with it. The two fields answer two different
# questions, and the danger is precisely the combination "elsewhere, with
# nowhere to go". ⇒ THE CONJUNCTION IS VALIDATED AND A MALFORMED VERB IS REFUSED
# BY NAME, at the accessor, where it cannot be bypassed.
def assert_kernel_declaration_coherent(verb, spec):
    """`sharesDriverKernel is False` ⟺ `kernelEntryArgv` is non-empty."""
    for field in ("sharesDriverKernel", "kernelEntryArgv"):
        if field not in spec:
            raise LegError(
                "runFilesystem %r declares no `%s`. Every verb states BOTH: "
                "which kernel its legs execute in, and how a measurement gets "
                "there. `True` and `[]` are themselves CLAIMS — they were the "
                "unexamined ones — so a missing field is a new launcher nobody "
                "answered the question for, never a yes."
                % (verb, field))
    shares = spec["sharesDriverKernel"]
    entry = list(spec["kernelEntryArgv"])
    if bool(shares) == (not entry):
        return
    raise LegError(
        "runFilesystem %r is MALFORMED: `sharesDriverKernel` is %r while "
        "`kernelEntryArgv` is %r. Those two must agree — a verb that does NOT "
        "share this driver's kernel states how that kernel is ENTERED, and a "
        "verb that DOES share it states `[]` because this process is already "
        "there. False with an EMPTY entry argv is the dangerous half: the "
        "measurement is then taken IN THIS PROCESS and stamped `in-process` "
        "(an outcome that is IN FORCE) while being filed under '%s' — an "
        "answer about the driver's own machine wearing another kernel's name, "
        "which is this anchor's defect exactly. True with a NON-EMPTY one is "
        "the mirror: a kernel this process claims to be in AND a mechanism for "
        "entering it, one of which is a lie and neither of which may be "
        "preferred. Answer BOTH questions for this verb. "
        "[D-HARNESS-ENVIRONMENT-PROBE-MEASURES-THE-DRIVERS-KERNEL-NOT-THE-"
        "LAUNCHED-ONE]"
        % (verb, shares, entry, verb))


def argv_common_prefix(argvs):
    """The LONGEST common prefix of some argv templates; `[]` for none.

    ★ LONGEST, NOT "A PREFIX", AND THE DIFFERENCE IS THE WHOLE INSTRUMENT. The
    check this serves used to ask `o[:len(entry)] == entry` for every other argv
    on a RUN_FILESYSTEMS entry, which the candidate `['wsl.exe']` PASSES — i.e.
    it would have accepted an entry mechanism that DROPPED `-e`, which is
    D-TOOLS-WSL-EXE-WITHOUT-DASH-E-RUNS-A-LOCAL-SHELL. "Is a prefix" is true of
    every truncation; only "is the longest one they all share" pins a boundary."""
    lists = [list(a) for a in argvs]
    if not lists:
        return []
    out = []
    for i in range(min(len(a) for a in lists)):
        head = lists[0][i]
        if any(a[i] != head for a in lists):
            break
        out.append(head)
    return out


def splice_working_dir(command, verb, directory):
    """The launcher argv that starts its child in `directory`.

    The splice point is stated once, here: immediately after the launcher's
    PROGRAM NAME. `wsl.exe -e` is program + the option that introduces the child
    command, and `--cd` must precede it. Returns `command` unchanged for a verb
    with no working-directory option (every `driver` launcher — there the OS
    process's own working directory is the run directory)."""
    tmpl = run_filesystem(verb)["workingDirArgv"]
    if not tmpl:
        return list(command)
    if not command:
        raise LegError(
            "runFilesystem %r needs to splice a working-directory option into an "
            "EMPTY launcher argv. A launcher whose child must start elsewhere is "
            "by definition a launcher." % verb)
    opts = [x.replace("{dir}", directory) for x in tmpl]
    return [command[0]] + opts + list(command[1:])


def launcher_run_dir(verb, label, driver_run_dir):
    """The run directory as the LAUNCHER addresses it, or "" for `driver`.

    The leg label is in the name so a log line and an `ls` are readable; the
    digest of the DRIVER's own run directory is in it so two checkouts, or two
    output roots, cannot collide inside one shared /tmp. Deterministic, so a
    re-run reuses (and re-wipes) the same directory rather than littering."""
    root = run_filesystem(verb)["root"]
    if not root:
        return ""
    if not driver_run_dir:
        raise LegError(
            "runFilesystem %r needs this driver's own run directory to derive a "
            "collision-free name in %s, and none was supplied" % (verb, root))
    digest = hashlib.sha256(driver_run_dir.encode("utf-8")).hexdigest()[:12]
    return "%s/%s-%s" % (root, label, digest)


# ── WHAT A LAUNCHER NEEDS BEYOND ITS OWN argv[0] ────────────────────────────
#
# THE FOURTH THING A LAUNCHER ENTRY DECLARES, AND IT WAS FOUND THE WAY THE OTHER
# THREE WERE — BY MEASURING A RUN THAT READ AS A COMPILER DEFECT AND WAS NOT ONE.
#
# ✔MEASURED on this project's NATIVE arm64 VPS, leg elf64-x86_64 under
# `qemu-x86_64`: THREE corpus segments aborted with
#     libgcc_s.so.1 must be installed for pthread_exit to work
#     qemu: uncaught target signal 6 (Aborted)
# one of them AFTER that segment's summary had already printed — i.e. at fixture
# EXIT, which is where `pthread_exit` runs. That sentence is GLIBC'S. It is not
# sqlite's and it is not DSS's, and no part of this harness could say so, because
# the ONLY gate a launcher has ever passed is `launcher_available` — a
# `shutil.which(command[0])` and nothing else.
#
# ★ AND THE SAME HOLE, ONE LAUNCHER OVER, IS WORSE. The elf64-arm64 leg's Windows
# launcher is `["wsl.exe", "-e", "qemu-aarch64"]`. `shutil.which` confirms
# **wsl.exe** — a Windows binary that is present on every machine with WSL — and
# NEVER ASKS whether `qemu-aarch64` exists inside the distro. A box with WSL and
# no qemu therefore passes the gate, every unit exits 255 with no diagnostic, and
# 14 of them were charged to DSS before anyone read the loader's message.
#
# ⇒ A LAUNCHER DECLARES WHAT IT NEEDS BEYOND argv[0], AND EVERY ROW SHOWS ITS
# WORK. `requires: []` is a CLAIM — "this launcher needs nothing but its own
# program" — in exactly the way `confounds: []` is a claim about earned excuses,
# so the key is REQUIRED on every entry and a missing one must be indistinguish-
# able from nothing. All five fields are required; none is defaulted:
#
#   kind      file | directory | command — the closed vocabulary below. HOW each
#             kind is asked is a property of the launcher's `runFilesystem`
#             (RUN_FILESYSTEMS[verb]["probeArgv"]), never of this row.
#   path      what to look for. `${NAME}` expands over THIS ENTRY'S OWN `env` map
#             and over NOTHING ELSE — see expand_launcher_requirement_path.
#   provides  what the run loses without it, in a reader's terms.
#   why       THE EVIDENCE. This is `confounds`' `earnedOn`/`mechanism` discipline
#             applied here: the lint REFUSES a row that does not show its work,
#             because a prerequisite nobody can justify is a prerequisite the next
#             person deletes.
#   install   THE REMEDY. A diagnostic without one is a diagnostic nobody acts on
#             — the whole cost of the two failures above was the hours between
#             "255" and "install qemu-user-static / libc6-arm64-cross".
LAUNCHER_REQUIREMENT_KINDS = {"file", "directory", "command"}

# All five, on every row. Listed as data so the lint's refusal can NAME the
# missing one instead of saying "malformed".
LAUNCHER_REQUIREMENT_KEYS = ("kind", "path", "provides", "why", "install")

# A POSIX environment-variable name. `launchers[].env` keys are handed to a
# process's environment block verbatim, and a key that is not a name is not a
# variable — it is a value nothing can ever read.
_LAUNCHER_ENV_NAME_RE = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")

# `${NAME}` as it appears in a requires path. Deliberately NOT the `${env:NAME}`
# spelling `expand_path` uses for library search paths: that one reads the
# PROCESS environment, this one must never, and two mechanisms that read
# different things must not look alike.
_LAUNCHER_REQUIRE_VAR_RE = re.compile(r"\$\{([^}]*)\}")


def launcher_requirement_vars(raw):
    """Every `${NAME}` a requires path references, in order of appearance."""
    return _LAUNCHER_REQUIRE_VAR_RE.findall(raw or "")


def expand_launcher_requirement_path(raw, env, where=""):
    """`${NAME}` expanded over THIS ENTRY'S OWN `env` — and over nothing else.

    ⚠ THE PROCESS ENVIRONMENT IS NOT CONSULTED, ON PURPOSE, and this is the whole
    point of the function existing rather than a call to os.path.expandvars.
    `QEMU_LD_PREFIX` is a variable this harness SETS for the launcher (it is in
    the entry's `env`), and it is also a variable an operator's shell may already
    hold with a different value. If a host's ambient value could silently decide
    what got checked, then two machines with identical catalogues would be
    checking different files and the check would be unreproducible — which is the
    same defect `plan_leg`'s injected `available` exists to prevent one layer up.

    An undeclared `${NAME}` is REFUSED BY NAME, never expanded to empty: an empty
    expansion turns `${QEMU_LD_PREFIX}/lib/ld-linux-aarch64.so.1` into an
    absolute path that exists on any Linux box, i.e. a check that passes for the
    wrong reason."""
    out, at = [], 0
    for m in _LAUNCHER_REQUIRE_VAR_RE.finditer(raw or ""):
        name = m.group(1)
        if name not in env:
            raise LegError(
                "%srequires path %r references ${%s}, which this launcher entry's "
                "own `env` does not declare (it declares: %s). A requirement's "
                "path expands over the entry's OWN env map and over NOTHING else "
                "— never the process environment — so that the same catalogue "
                "checks the same file on every machine. Declare %s in this "
                "entry's `env`, or spell the path literally."
                % (("leg '%s': " % where) if where else "", raw, name,
                   ", ".join(sorted(env)) or "nothing", name))
        value = env[name]
        if not isinstance(value, str) or not value:
            raise LegError(
                "%srequires path %r references ${%s}, which this entry's `env` "
                "declares as %r. An empty or non-string value would expand to "
                "nothing and leave a path that resolves somewhere else entirely."
                % (("leg '%s': " % where) if where else "", raw, name, value))
        out.append(raw[at:m.start()])
        out.append(value)
        at = m.end()
    out.append((raw or "")[at:])
    return "".join(out)


def requirement_probe_argv(fs_verb, kind, path):
    """The argv that ASKS whether one requirement is satisfied, or [] when the
    launcher shares this process's filesystem and the answer is in-process.

    The templates live on RUN_FILESYSTEMS because WHICH FILESYSTEM the question
    is about is a property of the launcher, not of the thing being looked for:
    `qemu-aarch64` means one file on a Linux driver and a completely different
    one inside WSL, and the ONLY thing that distinguishes those two questions is
    the launcher's declared `runFilesystem`."""
    if kind not in LAUNCHER_REQUIREMENT_KINDS:
        raise LegError(
            "unknown launcher requirement kind %r (known: %s). A kind decides "
            "HOW the machine is asked; an unrecognised one cannot be guessed at, "
            "because every wrong guess answers a question nobody asked."
            % (kind, ", ".join(sorted(LAUNCHER_REQUIREMENT_KINDS))))
    templates = run_filesystem(fs_verb)["probeArgv"]
    if not templates:
        return []
    tmpl = templates.get(kind)
    if tmpl is None:
        raise LegError(
            "runFilesystem %r implements no probe for requirement kind %r "
            "(it implements: %s). A kind with no probe on some filesystem is a "
            "requirement that is silently never checked there."
            % (fs_verb, kind, ", ".join(sorted(templates))))
    return [x.replace("{path}", path) for x in tmpl]


def _env_value_is_path(value):
    """Does this `launchers[].env` VALUE denote a filesystem location?

    ⚠ A DIFFERENT QUESTION FROM `FOREIGN_PATH_SHAPES`, and the two are used for
    two different rules, so they are kept apart. `sourceShape` answers "is this a
    path in the namespace this launcher translates FROM" — it is how a DRIVER
    path caught crossing to a foreign launcher is refused. This answers the
    broader "is this a path at all", which is what decides whether a declared
    variable owes a `requires` row: `/usr/aarch64-linux-gnu` is a path and it is
    NOT a windows-drive path, so the narrow predicate would call it a non-path
    and let the very variable this whole section is about go unchecked."""
    if not isinstance(value, str) or not value:
        return False
    return (value.startswith("/") or value.startswith("\\")
            or _looks_like_windows_drive_path(value))


def launcher_env_findings(label, entry):
    """Everything wrong with ONE launcher entry's `env`, as findings.

    `env` was optional and validated NOWHERE until requires paths started
    expanding over it (it was read once, at plan_leg, and copied through). A map
    that decides what a check looks at is load-bearing, so it is validated with
    the same required-no-default discipline as its three sibling keys."""
    where = "leg '%s': launcher for (%s, %s)" % (
        label, entry.get("hostOs"), entry.get("hostArch"))
    out = []
    if "env" not in entry:
        out.append(
            "%s declares no `env`. Every launcher states the environment its own "
            "declaration authors for the process it spawns — `{}` when it authors "
            "none, which is the common answer. Required, because a missing key "
            "cannot be told from an empty one, and `requires` paths expand over "
            "this map: an absent map and a map with nothing in it must not read "
            "the same way to the thing that expands them." % where)
        return out
    env = entry["env"]
    if not isinstance(env, dict):
        out.append("%s declares an `env` that is not an object, got %r"
                   % (where, type(env).__name__))
        return out
    for name in sorted(env):
        value = env[name]
        if not _LAUNCHER_ENV_NAME_RE.match(name or ""):
            out.append(
                "%s declares env key %r, which is not an environment VARIABLE "
                "NAME ([A-Za-z_][A-Za-z0-9_]*). A key that is not a name cannot "
                "be read by the process it is set for." % (where, name))
            continue
        if not isinstance(value, str) or not value:
            out.append(
                "%s declares env %s=%r. A declared variable's value must be a "
                "non-empty string: an empty one arrives at the launched process "
                "as EMPTY-BUT-EXISTING, which reads as a real setting rather "
                "than as absence — the same false-green shape that made the "
                "WSLENV carrier filter load-bearing." % (where, name, value))
            continue
        # ── the value's NAMESPACE, for a launcher that has another one ──────
        verb = entry.get("pathTranslation", "none")
        shape = PATH_TRANSLATIONS.get(verb, {}).get("sourceShape", "")
        predicate = FOREIGN_PATH_SHAPES.get(shape)
        if predicate is not None and predicate(value):
            out.append(
                "%s declares env %s=%r, which is a '%s' path — THIS DRIVER'S "
                "namespace — on a launcher whose pathTranslation is '%s'. The "
                "value crosses the boundary VERBATIM (a WSLENV carrier forwards "
                "bytes, it does not translate them), so the launched process "
                "receives a path from a namespace it cannot resolve, opens a "
                "relative file of that name, misses, and the run reads as a "
                "broken binary rather than as a misdeclared variable."
                % (where, name, value, shape, verb))
            continue
        # ── a path-valued variable owes a requires row ──────────────────────
        # Otherwise the declaration is a value nobody ever looks at: the run sets
        # QEMU_LD_PREFIX to a sysroot that is not on the machine, the loader
        # resolves nothing, and the failure surfaces as the target program's exit
        # code. Requiring the row is what turns that into a named prerequisite.
        if _env_value_is_path(value):
            referenced = any(
                name in launcher_requirement_vars(row.get("path", ""))
                for row in entry.get("requires", [])
                if isinstance(row, dict))
            if not referenced:
                out.append(
                    "%s declares the PATH-VALUED env %s=%r and NOTHING in its "
                    "`requires` list references ${%s}. A launcher that points a program at a "
                    "directory must state that the directory has to BE there: "
                    "without a row nothing ever looks, the loader silently "
                    "resolves nothing, and the run fails as the target program's "
                    "exit code instead of as a named missing prerequisite."
                    % (where, name, value, name))
    return out


def launcher_requires_findings(label, entry):
    """Everything wrong with ONE launcher entry's `requires`, as findings.

    THE RULES ARE WRITTEN ONCE, HERE. `resolve_launcher_requirements` raises the
    first of these rather than re-deciding anything, so the lint's enumeration
    and the resolver's refusal cannot drift into disagreeing about what a valid
    row is."""
    where = "leg '%s': launcher for (%s, %s)" % (
        label, entry.get("hostOs"), entry.get("hostArch"))
    if "requires" not in entry:
        return ["%s declares no `requires`. Every launcher states what it needs "
                "BEYOND its own argv[0] — `[]` when it needs nothing, which is a "
                "CLAIM and not a silence. Required, because a missing key cannot "
                "be told from an empty one, and the difference is the whole "
                "defect: `shutil.which('wsl.exe')` says yes on a box with no "
                "distro, no qemu inside it and no sysroot, and every unit then "
                "exits 255 and is charged to the compiler." % where]
    rows = entry["requires"]
    if not isinstance(rows, list):
        return ["%s declares a `requires` that is not a list, got %r"
                % (where, type(rows).__name__)]
    out, seen = [], set()
    env = entry.get("env")
    if not isinstance(env, dict):
        env = {}
    for row in rows:
        if not isinstance(row, dict):
            out.append("%s declares a requirement that is not an object: %r — "
                       "every row carries its kind, its path and its evidence"
                       % (where, row))
            continue
        for key in LAUNCHER_REQUIREMENT_KEYS:
            value = row.get(key)
            if not isinstance(value, str) or not value.strip():
                out.append(
                    "%s declares a requirement with no `%s` (%r). All five of %s "
                    "are required and none is defaulted: `why` is the row's "
                    "EVIDENCE and `install` is its REMEDY, and a prerequisite "
                    "that states neither is one a reader cannot act on and the "
                    "next person deletes."
                    % (where, key, row.get("path", row),
                       ", ".join(LAUNCHER_REQUIREMENT_KEYS)))
        kind = row.get("kind")
        if isinstance(kind, str) and kind and kind not in LAUNCHER_REQUIREMENT_KINDS:
            out.append(
                "%s declares a requirement of unknown `kind` %r (known: %s). A "
                "kind decides how the machine is asked; nothing implements this "
                "one, so the row would read as configuration and check nothing."
                % (where, kind, ", ".join(sorted(LAUNCHER_REQUIREMENT_KINDS))))
        raw = row.get("path")
        if isinstance(raw, str) and raw:
            if raw in seen:
                out.append("%s declares the requirement path %r twice"
                           % (where, raw))
            seen.add(raw)
            try:
                expand_launcher_requirement_path(raw, env, label)
            except LegError as exc:
                out.append("%s: %s" % (where, exc))
        if isinstance(kind, str) and kind in LAUNCHER_REQUIREMENT_KINDS:
            # A kind nothing can ASK about on this launcher's filesystem is a
            # row that is silently never checked there — the same shape as the
            # defect, one level up.
            try:
                requirement_probe_argv(entry.get("runFilesystem", "driver"),
                                       kind, "/probe")
            except LegError as exc:
                out.append("%s: %s" % (where, exc))
    return out


def resolve_launcher_requirements(entry, where=""):
    """This entry's `requires`, RESOLVED: paths expanded over the entry's own
    env, probe argv built from the entry's own runFilesystem.

    PURE — no filesystem, no spawn, no os.environ — so `plan_leg` can emit the
    rows and a plan stays reproducible on any machine. Executing them is a
    separate act with its own subcommand (`--check-launcher`)."""
    findings = launcher_requires_findings(where or "<leg>", entry)
    if findings:
        raise LegError(findings[0])
    fs_verb = entry.get("runFilesystem", "driver")
    out = []
    for row in entry.get("requires", []):
        path = expand_launcher_requirement_path(row["path"], entry.get("env", {}),
                                                where)
        out.append({
            "kind": row["kind"],
            "path": path,
            "provides": row["provides"],
            "why": row["why"],
            "install": row["install"],
            "probe": requirement_probe_argv(fs_verb, row["kind"], path),
        })
    return out


# ── EARNED CONFOUNDS ARE A PROPERTY OF THE LEG, NOT OF THE DRIVER ───────────
#
# [D-HARNESS-CONFOUND-LEDGER-IS-PER-DRIVER-NOT-PER-LEG,
#  D-HARNESS-SQLITE-CONFOUNDS-NOT-DECLARED-PER-LEG,
#  D-SQLITE-CONFOUND-LIST-DRIVER-ASYMMETRY.]
#
# A "confound" is a unit failure this harness has PROVEN is not the compiler's —
# by a matched control, on a named leg, on a named date. Which failures are
# excused therefore decides every verdict this harness renders, and until this
# key existed the answer depended on WHICH DRIVER YOU RAN:
#   · build-and-test.sh read ONE global `DSS_CONFOUNDS` and applied it to EVERY
#     leg, including legs where nothing had ever been measured;
#   · build-and-test.ps1 returned an earned list for `pe64-x86_64` and `@()` for
#     everything else — and ALL SIX of its patterns had been earned on LINUX
#     x86_64, so the Windows driver handed a Linux-earned list to the one leg
#     that could not use it and withheld it from the legs that earned it.
# ✔MEASURED consequence: the SAME elf64-x86_64 artefact's `zipfile-25.0` was a
# "known non-DSS confound" under one driver and a "genuine failure" under the
# other, in the same project on the same day. No two legs' genuine-failure counts
# were comparable, and the genuine-failure count is what every verdict rests on.
#
# ★ SO THE DECLARATION MOVED HERE AND CARRIES ITS EVIDENCE. Every pattern names
# the leg and host its control was run on, the date, and the mechanism. A pattern
# with no provenance cannot be added — the lint refuses it — because "we have
# always excused this one" is precisely how a real defect becomes furniture.
# ★★ ABSENCE IS A CLAIM TOO: a leg declaring `"confounds": []` is stating that
# nothing has ever been earned there and that every failure counts. The key is
# REQUIRED on every leg so that silence cannot mean "nobody filled this in".
#
# ── `scope` IS LEGACY: A RUN MODE IS NOT A HOST ─────────────────────────────
#
# [D-HARNESS-CONFOUND-SCOPE-IS-A-RUN-MODE-NOT-A-HOST.]
#
# `scope` matched a pattern against the leg's RUN MODE — `any` / `native` /
# `emulated`. It is the wrong axis for any row whose mechanism is a property of
# the MACHINE, and the catalogue could not say so, so it said nothing:
#
#   · `^walsetlk-`, `^walsetlk_recover-` and `^busy2-` are excused because THIS
#     BOX'S CLOCK_REALTIME steps ~34.47 s every ~5 s. There is no `scope` that
#     spells "on a host whose clock does that", so all three sat at `scope: any`
#     — which excuses the family on EVERY machine, including the arm64 VPS where
#     the clock has never been shown to step. A GENUINE walsetlk regression there
#     would have been swallowed in silence.
#   · The condition was ALREADY WRITTEN DOWN. legs[1]'s `$confoundsComment` says
#     the clock rows are declared because the mechanism is "a property of the
#     HOST'S CLOCK, which both Linux legs share WHEN THIS HARNESS IS DRIVEN FROM
#     THIS BOX". A load-bearing condition, recorded in prose, where nothing reads
#     it — the same failure as `earnedOn`, one field along.
#
# ⇒ THE CONDITION BECOMES A MEASUREMENT. A row may declare
# `requires: [<probe name>]`, and it is honoured only when every named probe
# MEASURES its defect as PRESENT on the machine this run is happening on. `[]` is
# the unconditional claim and is the common answer.
#
# ★ WHY THIS IS NOT HOST-KEYING (and legs.json's `$noHostKeyingComment` now says
# it positively): A MEASURED PROPERTY OF THE RUNNING ENVIRONMENT IS ADMISSIBLE; A
# NAME FOR THE ENVIRONMENT NEVER IS. `if host == 'wsl'` is forbidden because a
# name is not evidence and it cannot be wrong out loud. "This clock steps 34.5 s,
# here are the 4 steps I watched it take" is evidence, it is checkable, and it is
# false on a healthy box — which is the entire point.
#
# ⛔ WHY `scope: emulated` WAS REJECTED AS THE FIX FOR THE CLOCK ROWS. It is a
# PROXY, and this project already owns the receipts:
# D-TEST-PE64-CONFOUND-PIN-WEAKENED-BY-ITS-OWN-SUBJECT records this same pin
# being re-cut TWICE, each time by the case it was meant to judge. It is also
# provably wrong in BOTH directions: `emulated` would excuse walsetlk under Wine
# on a healthy-clock box (false excusal), and the `^writecrash-` row below already
# documents the proxy breaking the other way on an arm64 Windows host. A proxy
# with two recorded failures is not a candidate.
#
# `scope` is therefore LEGACY, not an alternative: it survives ONLY on rows whose
# real mechanism has no probe yet, each of which must NAME ITS BLOCKER
# (`scopeLegacyBlocker`). `scope: any` is REFUSED outright — that claim is now
# spelled `requires: []`. The axis retires when the last row leaves it, the way
# `copy-relocation` was retired: deleted, not left inert.
#   native     excused only when THIS HOST executes the artefact directly.
#   emulated   excused only when it goes through a declared launcher. The name is
#              the operator-facing DSS_CONFOUNDS vocabulary and is deliberately
#              NOT renamed to `launched`: renaming it would silently un-excuse
#              every `emulated:` pattern an operator has ever typed.
CONFOUND_SCOPES = ("native", "emulated")

# Retired from CONFOUND_SCOPES, and NAMED so the refusal can explain itself
# instead of reporting "unknown scope" for a value that used to be the default.
RETIRED_CONFOUND_SCOPES = ("any",)

# Required on EVERY declared pattern, all non-empty. These are the four questions
# a reader must be able to answer without leaving the file: what does it match,
# where was it proven, when, and by what mechanism + which anchor holds the long
# form. A confound is an ASSERTION THAT THE COMPILER IS INNOCENT; it has to show
# its work.
CONFOUND_PROVENANCE_KEYS = ("earnedOn", "earnedAt", "mechanism", "anchor")

# ── WHAT A CONFOUND ROW MATCHES ─────────────────────────────────────────────
#
# ANCHOR, ONE LINE, DO NOT WRAP: D-HARNESS-ABORT-HAS-NO-EARNED-CONFOUND-VOCABULARY
#
# ★★ ONE LEDGER, TWO MATCH KINDS — NOT TWO LEDGERS. Every row here answers one
# question, "is this failure the compiler's?", and a second list answering it
# would be a second thing to keep earned, lint and read. What differs is only
# the NAME the failure arrives under: a unit failure arrives as a test name, and
# an ABORT kills the fixture mid-file so there is no unit name at all — only
# `permutation/file`. That was the gap: a proven-upstream abort could not be
# recorded in ANY form, so the driver's "a run with aborts is NEVER green" rule
# convicted the compiler of an environment fault it had measured to be innocent
# of.
#
# ★ AND THE GUARD IS NOT REMOVED, IT IS MADE CONDITIONAL ON PROVENANCE. An
# `abort-file` row carries the SAME mandatory earnedOn/earnedAt/mechanism/anchor
# as any other, enforced by the same lint, and an UNEARNED abort still fails the
# run exactly as before. "Proven" means earned; nothing else buys the exemption.
CONFOUND_MATCH_KINDS = {
    "unit": "the row's pattern is matched against a FAILING UNIT's test name "
            "(the historical and default behaviour)",
    "abort-file": "the row's pattern is matched against a fixture ABORT, spelled "
                  "`permutation/file` (e.g. `^veryquick/nolock\\.test$`) — the "
                  "only name an abort has, because it dies before the unit that "
                  "killed it can be reported",
    "build-tu": "the row's pattern is matched against a TRANSLATION UNIT PATH "
                "(e.g. `ext/misc/fileio\\.c$`) — the only name a BUILD failure "
                "has, because it dies before any unit exists to be named. "
                "★ UNLIKE EVERY OTHER KIND, THE ROW ALONE EXCUSES NOTHING: it is "
                "honoured only when THIS RUN's same-platform reference oracle, "
                "given this leg's own manifest, also rejected that TU. See "
                "attribute_build_failure "
                "[D-HARNESS-BUILD-FAILURE-HAS-NO-PER-TU-ATTRIBUTION]",
}
CONFOUND_MATCH_DEFAULT = "unit"


def confound_match_kind(row):
    """One row's match kind, defaulted and CHECKED. A typo must not silently
    become the default: a row meant to excuse an abort that quietly became a
    unit-name row would match nothing and the abort would still convict."""
    kind = row.get("matches", CONFOUND_MATCH_DEFAULT)
    if kind not in CONFOUND_MATCH_KINDS:
        raise LegError(
            "confound %r declares matches=%r (known: %s). An unrecognised match "
            "kind cannot be defaulted to `unit`: a mistyped abort row would then "
            "match no unit name, excuse nothing, and the abort it was written "
            "for would still be charged to the compiler."
            % (row.get("pattern"), kind, ", ".join(sorted(CONFOUND_MATCH_KINDS))))
    return kind


def confound_scope_prefix(scope):
    """The `native:`/`emulated:` prefix a scope becomes on the wire, so the two
    drivers' long-standing pattern grammar is produced in ONE place instead of
    being re-spelled in each of them.

    `scope` is LEGACY (see the block above). This function is what a row still on
    the axis passes through; a row that has migrated never reaches it."""
    if scope in RETIRED_CONFOUND_SCOPES:
        raise LegError(
            "confound scope %r is RETIRED. It meant 'excused however this leg "
            "runs', which is now spelled `requires: []` — an explicit claim that "
            "the excusal depends on nothing measurable. The `scope` axis survives "
            "only for rows whose real mechanism has no probe yet, and each of "
            "those must name its blocker. "
            "[D-HARNESS-CONFOUND-SCOPE-IS-A-RUN-MODE-NOT-A-HOST]" % (scope,))
    if scope not in CONFOUND_SCOPES:
        raise LegError(
            "unknown confound scope %r (known: %s). A scope decides whether a "
            "pattern excuses a failure at all; an unrecognised one cannot be "
            "treated as unconditional, because unconditional is the widest "
            "possible excusal." % (scope, ", ".join(CONFOUND_SCOPES)))
    return scope + ":"


# ── WHICH RUN MODES A `scope`d PATTERN CAN EVER MATCH ───────────────────────
#
# [D-HARNESS-CONFOUND-SCOPE-IS-A-RUN-MODE-NOT-A-HOST.]
#
# `scope` is LEGACY, but while it survives the harness has to be able to SAY —
# per leg, per run — whether a scoped row can match anything here at all. It
# could not, and the report said the opposite: ✔MEASURED at 0ecec160 with
# `--host-os linux --host-arch arm64 --launchers-none` (run mode `native`),
# `confound rows ACTIVE (4 of 7): ... emulated:^writecrash- ...` with the reason
# "unconditional (`requires: []`) ... nothing this harness measures per run".
# Two claims, both false: on a native run the matcher never applies an
# `emulated:` pattern, so the row is neither unconditional nor in force. The
# direction was safe; the ACCOUNT — whose entire job is to say why a failure was
# excused — overstated the excusal set.
#
# ⚠ THE MATCHING ITSELF STAYS IN THE DRIVERS' MATCHER, ITS ONE OWNER. This table
# is NOT a second implementation of it: the wire pattern keeps its prefix and is
# still supplied verbatim, because a planner that ALSO decided scope matches
# would make two places answer one question — the defect
# D-HARNESS-CONFOUND-LEDGER-IS-PER-DRIVER-NOT-PER-LEG already cost this project
# once, one axis along. What this table buys is HONEST REPORTING: the account
# says a scoped row is SUPPLIED but not in force here, and names the run mode
# that would put it in force.
RUN_MODES = ("native", "launched", "skip")
# ── RUN FIDELITY — does the artefact execute on ITS OWN INSTRUCTION SET? ─────
# [D-HARNESS-RUN-FIDELITY-IS-COMPUTED-BUT-NEITHER-RECORDED-NOR-SELECTABLE]
# A SEPARATE axis from RUN_MODES, and separate because it answers a different
# question. `mode` says HOW the artefact is reached (directly / through a
# declared launcher / not at all); `fidelity` says WHAT KIND OF EVIDENCE the run
# produces. They are independent: two `launched` runs of the SAME leg can differ
# here, which is the whole reason this exists.
#   native          this host's own kernel, this host's own ISA.
#   foreign-kernel  same ISA, another kernel — wsl.exe on an arm64 Windows box
#                   runs aarch64 code on aarch64 silicon. REAL hardware.
#   emulated        the ISA is translated — qemu-user, Rosetta, box64.
# ⛔ NOT SPELLED AFTER A TOOL. `qemu` in the name would be wrong for Rosetta and
# for every emulator not yet written; the property is the ISA crossing, and it is
# read from data `plan_leg` already computes rather than from a launcher's name
# — the proxy this project has already refused once for confound scopes
# [D-HARNESS-CONFOUND-SCOPE-IS-A-RUN-MODE-NOT-A-HOST].
RUN_FIDELITIES = ("native", "foreign-kernel", "emulated")
CONFOUND_SCOPE_RUN_MODES = {
    "native": ("native",),
    # `emulated` is the operator-facing DSS_CONFOUNDS spelling and is
    # deliberately NOT renamed to `launched` (see CONFOUND_SCOPES); the mapping
    # to the run mode it means lives here, declared, rather than in a reader's
    # head.
    "emulated": ("launched",),
}


def confound_scope_run_modes(scope):
    """The run modes in which a `scope`d pattern can match at all, or a
    LegError. Never a permissive default: an unknown scope answered with "every
    mode" would report a dead row as in force, which is the direction that
    overstates what was excused."""
    modes = CONFOUND_SCOPE_RUN_MODES.get(scope)
    if modes is None:
        raise LegError(
            "confound scope %r declares no run modes it can match (known: %s). "
            "A scope is a claim about HOW the leg runs; one whose modes nobody "
            "declared cannot be reported as in force, and cannot be reported as "
            "dead either." % (scope, ", ".join(sorted(CONFOUND_SCOPE_RUN_MODES))))
    return modes


# ── THE ENVIRONMENT-DEFECT PROBE REGISTRY ───────────────────────────────────
#
# [D-HARNESS-CONFOUND-SCOPE-IS-A-RUN-MODE-NOT-A-HOST.]
#
# A confound row's `requires` names PROBES, and a probe is a MEASURED PROCEDURE
# that answers one question about the machine this run is happening on:
#
#   present         the defect is HERE — I watched it happen, and here is what I saw
#   absent          I looked and it did not happen
#   indeterminate   I could not look (no instrument, the measurement was cut short)
#
# ★★ THE ERRORS ARE NOT SYMMETRIC, AND THE WHOLE DESIGN FOLLOWS FROM THAT.
#
#   FALSE NEGATIVE — `absent` on a box that HAS the defect: the confound rows go
#   INACTIVE, environment failures are reported as genuine reds, somebody
#   investigates and finds the clock. NOISY BUT SAFE.
#
#   FALSE POSITIVE — `present` on a HEALTHY box: a real DSS miscompile is
#   SILENTLY EXCUSED. SILENT AND DANGEROUS — it is the exact failure this whole
#   mechanism exists to remove, so a mechanism that can commit it is worse than
#   none.
#
# ⇒ EVERY PROBE IS BIASED TOWARD `absent`. `indeterminate` is honoured as
# `absent` (never as `present`), the thresholds must be CLEARED rather than
# approached, and a probe that raises is `indeterminate` and says so.
#
# ★ THE ENGINE HAS NO PROBE-NAME BRANCH. A registry entry names a VERB; the
# engine looks the verb up in this closed table and calls its `measure`. Adding
# `clock-realtime-steps-on-the-vps` needs NO code. Adding a new KIND of
# measurement adds a verb here — a vocabulary extension, exactly like
# RUN_FILESYSTEMS / PATH_TRANSLATIONS / ENV_TRANSFERS — and the lint then refuses
# any registry entry naming a verb this table does not have.
PROBE_VERDICTS = ("present", "absent", "indeterminate")


def probe_verdict_honours(verdict):
    """Does this verdict permit a row that REQUIRES the probe to be honoured?

    ONLY `present`. `indeterminate` is deliberately NOT a maybe: a row honoured
    on "I could not measure" is a row honoured on nothing, which is the state
    `scope: any` was already in."""
    if verdict not in PROBE_VERDICTS:
        raise LegError(
            "unknown probe verdict %r (known: %s). A verdict decides whether a "
            "failure is excused; an unrecognised one cannot be read as "
            "'present', because 'present' is the direction that hides a real "
            "compiler defect." % (verdict, ", ".join(PROBE_VERDICTS)))
    return verdict == "present"


# The "nothing was injected" sentinel. `None` cannot do this job: `awake=None`
# is a REAL fixture — "this interpreter has no monotonic clock" — and the arm it
# selects (INDETERMINATE) is one the self-test has to be able to drive. Reusing
# None for both would make that arm permanently unreachable from a test, which is
# how a defensive branch comes to be shipped unexercised.
_PROBE_DEFAULT = object()


# ── THERE ARE TWO MONOTONIC CLOCKS AND THE NAME DOES NOT SAY WHICH ──────────
#
# [D-HARNESS-PROBE-READS-A-HOST-SUSPEND-AS-A-WALL-CLOCK-STEP]
#
# Every modern host keeps two, and they answer different questions:
#
#   AWAKE       time the machine spent RUNNING. Stops dead in a suspend.
#   CONTINUOUS  time that ELAPSED. Counts the suspend, because the suspend
#               really did happen.
#
# ⚠ WHICH SPELLING IS WHICH IS OPPOSITE ON THE TWO PLATFORMS THIS PROJECT RUNS,
# so `CLOCK_MONOTONIC` may not be read as either one — getting it backwards
# inverts the whole instrument:
#
#   ✔MEASURED 2026-08-13, macOS 26.5.2 / Darwin 25.5.0 arm64, one process, one
#     breath: CLOCK_MONOTONIC 172000.715 s − CLOCK_UPTIME_RAW 27339.289 s =
#     144661.427 s of host suspend this laptop has already served since boot. So
#     DARWIN'S `CLOCK_MONOTONIC` IS THE CONTINUOUS ONE and CLOCK_UPTIME_RAW is
#     the awake one. `time.monotonic()` there is `mach_absolute_time()` and
#     tracks the AWAKE clock (0.510054 s over a 0.5 s sleep, exactly
#     CLOCK_UPTIME_RAW's 0.510054 s).
#   ✔MEASURED the same day, Ubuntu 24.04 under WSL2 / CPython 3.12.3:
#     CLOCK_BOOTTIME (id 7) and CLOCK_MONOTONIC (id 1) are distinct ids reading
#     91644.442742 and 91644.442731 s — this VM has recorded ZERO suspend, so
#     their DIFFERENCE is unproven ON IT; Linux's documented split is the mirror
#     image of Darwin's (CLOCK_MONOTONIC excludes suspend, CLOCK_BOOTTIME
#     includes it).
#   ✔MEASURED the same day, Windows 11 / CPython 3.14.3: `time.clock_gettime`
#     DOES NOT EXIST and not one CLOCK_* id is exposed. `time.monotonic()` is
#     QueryPerformanceCounter, whose behaviour across modern standby is
#     UNMEASURED — inducing modern standby from a test run is not practical on
#     the operator's own box, so the last arm below says so instead of guessing.
#
# ★ THE CHOICE IS THEREFORE DERIVED FROM THE VOCABULARY THE PLATFORM PUBLISHES,
# never from its name: a platform that can tell the two states apart exposes a
# SECOND id for whichever one its `CLOCK_MONOTONIC` is not. Linux's is awake, so
# it adds CLOCK_BOOTTIME for elapsed; Darwin's is elapsed, so it adds
# CLOCK_UPTIME_RAW for awake. A platform that publishes neither has not claimed
# it can distinguish them, and gets an answer that SAYS the reference is
# unmeasured rather than a silent guess in the dangerous direction.
def _resolve_continuous_clock():
    """(read, name, recordedSuspendSeconds) for the monotonic clock that KEEPS
    COUNTING while this host is suspended.

    `recordedSuspendSeconds` is the host's OWN corroboration of the choice — a
    continuous clock minus an awake one, both read here and both taken from the
    SAME ADJUSTMENT DOMAIN (see the comment at the difference below; on Darwin
    that is the `*_RAW` pair, which is NOT the pair used for the reading), i.e.
    the suspend this machine has already served since boot. `0.0` means it has not suspended yet
    and the choice is unproven ON IT; `None` means the platform exposes no second
    id to difference against and the fallback is in force. It is EVIDENCE and
    never a gate: a probe that behaved differently on a machine that happened to
    have napped would be two instruments wearing one name.

    The pair is differenced through `clock_gettime` rather than against
    `time.monotonic()`, whose ABSOLUTE value is not on the same epoch — ✔MEASURED
    on Darwin, `time.monotonic()` read 0.017 s in the very process where
    CLOCK_UPTIME_RAW read 27339.289 s."""
    import time as _time
    gettime = getattr(_time, "clock_gettime", None)
    mono = getattr(_time, "CLOCK_MONOTONIC", None)
    mono_raw = getattr(_time, "CLOCK_MONOTONIC_RAW", None)
    boot = getattr(_time, "CLOCK_BOOTTIME", None)
    uptime = getattr(_time, "CLOCK_UPTIME_RAW", None)
    candidate = None
    if gettime is not None and mono is not None:
        if boot is not None:
            candidate = (boot, mono, "CLOCK_BOOTTIME")
        elif uptime is not None:
            candidate = (mono, uptime, "CLOCK_MONOTONIC")
    if candidate is not None:
        continuous_id, awake_id, name = candidate
        # ★ THE CORROBORATION IS DIFFERENCED WITHIN ONE ADJUSTMENT DOMAIN, which
        # is NOT always the pair used for the reading above. Suspend is not the
        # only thing separating two monotonic ids: `CLOCK_MONOTONIC` is
        # ADJUSTABLE (NTP slews it) while the `*_RAW` ids never are, so a
        # cross-domain difference carries suspend PLUS an adjustment term whose
        # sign is unbounded.
        #   Linux  — CLOCK_BOOTTIME − CLOCK_MONOTONIC: both adjusted, and by the
        #            SAME adjustment, so the difference is suspend alone.
        #   Darwin — CLOCK_MONOTONIC − CLOCK_UPTIME_RAW mixes an ADJUSTED clock
        #            with a RAW one. Use the raw pair instead.
        # ⚠ MEASURED on the macos-latest CI runner, which is what caught this:
        # the mixed pair reported **-0.496 s of recorded suspend** on a freshly
        # booted VM that had never slept — ~650 ppm of slew over 764 s of uptime.
        # It went unnoticed for as long as it did because the host it was written
        # on carried 144661 s of real suspend, which buries a sub-second term.
        # The self-test's `recordedSuspend >= 0` check stays STRICT, because with
        # one domain on both sides the ordering is a genuine invariant again.
        corroboration = (continuous_id, awake_id)
        if boot is None and uptime is not None and mono_raw is not None:
            corroboration = (mono_raw, uptime)
        try:
            recorded = round(gettime(corroboration[0])
                             - gettime(corroboration[1]), 3)
        except (OSError, ValueError):
            # An id the platform NAMES but cannot SERVE is not a clock. Falling
            # through is the safe direction: the fallback reports itself as
            # unmeasured, where using a half-working id would not.
            pass
        else:
            return (lambda: gettime(continuous_id)), name, recorded
    return _time.monotonic, "time.monotonic()", None


def _measure_wall_clock_step(config, clock=None, awake=_PROBE_DEFAULT,
                             reference=_PROBE_DEFAULT, sleeper=None):
    """Does this machine's WALL CLOCK step, relative to REAL ELAPSED TIME?

    THE INSTRUMENT IS THE DIFFERENCE OF TWO CLOCKS, and that choice is the
    noise immunity. Sampling CLOCK_REALTIME alone would have to assume the sleep
    interval was honoured, so a descheduled process, a loaded box or a slow
    filesystem would all read as "the clock jumped". Subtracting the reference
    delta cancels every one of those: a scheduling delay inflates BOTH deltas
    equally and the difference stays ~0. What survives is a wall clock that
    moved when real time did not — which is the defect, and nothing else.

    ★ THE REFERENCE IS THE CONTINUOUS CLOCK, NOT THE AWAKE ONE, and that is the
    whole of D-HARNESS-PROBE-READS-A-HOST-SUSPEND-AS-A-WALL-CLOCK-STEP. A host
    suspend IS real elapsed time and a healthy wall clock is SUPPOSED to advance
    across it, so differencing against an AWAKE clock scores a nap as a
    wall-clock step of exactly the nap's length. ✔MEASURED 2026-08-13 on a
    HEALTHY Mac whose clock was verified tracking: a 32.9 s nap produced `worst
    per-tick drift 32.8999 s and offset spread 32.9004 s (need 5.000 s)` — it
    CLEARED BOTH magnitude thresholds, and only `minStepsRequired: 2` stood
    between it and a forged `present`, the false positive this registry calls
    SILENT AND DANGEROUS. Against the continuous clock that same nap scores ~0,
    while a genuine CLOCK_REALTIME step still scores its FULL magnitude because
    the reference did not move: strictly sharper, nothing lost.

    ⚠ TWO clocks, and they are NOT interchangeable. The WINDOW is bounded on the
    AWAKE clock so a nap cannot eat the sample — the sample count, and therefore
    all the detecting power, is exactly what it was before this change — while
    the DRIFT is differenced against the CONTINUOUS one. `windowSeconds` is
    consequently AWAKE seconds and may be far short of the wall time the run
    took; that is the instrument working, not the defect.

    ✔THE SIGNATURE THIS IS CALIBRATED AGAINST (D-ENV-WSL2-CLOCK-REALTIME-STEPS-
    34S, MEASURED 2026-08-01, two independent instruments): CLOCK_REALTIME
    oscillates between two values ~34.47 s apart, flipping every ~5 s; 49 and 48
    steps observed; total spread 35.164 s. So a 20 s sample at 250 ms sees ~4
    flips, and `minStepSeconds: 5` sits ~7x below the real magnitude and ~4
    orders of magnitude above scheduler noise. That defect moves CLOCK_REALTIME
    against BOTH monotonic clocks, so this change leaves it fully visible.

    The clocks are INJECTED so the self-test drives every arm — present, absent,
    indeterminate, the nap, and the boundary either side of each threshold — on
    any host, in milliseconds, with no dependence on the machine running it."""
    import time as _time
    clock = clock or _time.time
    if awake is _PROBE_DEFAULT:
        awake = getattr(_time, "monotonic", None)
    if reference is _PROBE_DEFAULT:
        reference, ref_name, ref_suspend = _resolve_continuous_clock()
        ref_says = (
            ("%s, which has already recorded %.3f s of host suspend on this "
             "machine" % (ref_name, ref_suspend)) if ref_suspend is not None
            else ("%s -- this interpreter exposes no clock id known to count a "
                  "host suspend, so a suspend here is UNMEASURED" % ref_name))
    else:
        ref_name, ref_suspend, ref_says = (
            "injected", None, "the injected reference clock")
    sleeper = sleeper or _time.sleep
    if awake is None:
        # NOT a failure of the box: a failure of the INSTRUMENT. Reported as
        # indeterminate (⇒ honoured as absent) rather than guessed either way.
        return "indeterminate", ("no monotonic clock on this interpreter, so a "
                                 "wall-clock step cannot be told from a "
                                 "scheduling delay"), {}
    window = float(config["sampleSeconds"])
    interval = float(config["sampleIntervalMs"]) / 1000.0
    min_step = float(config["minStepSeconds"])
    min_steps = int(config["minStepsRequired"])
    steps, worst, samples = 0, 0.0, 0
    lo = hi = None
    a0 = awake()
    prev_w, prev_r = clock(), reference()
    try:
        while awake() - a0 < window:
            sleeper(interval)
            w, r = clock(), reference()
            samples += 1
            # The wall clock's own drift over this tick, with real ELAPSED time
            # taken out. |.| because the measured defect steps BOTH ways.
            drift = abs((w - prev_w) - (r - prev_r))
            if drift > worst:
                worst = drift
            if drift >= min_step:
                steps += 1
            # The oscillation AMPLITUDE, elapsed time removed, so a clock that
            # merely runs fast is not mistaken for one that jumps.
            offset = w - r
            lo = offset if lo is None else min(lo, offset)
            hi = offset if hi is None else max(hi, offset)
            prev_w, prev_r = w, r
    except Exception as exc:                                # noqa: BLE001
        # A clock that cannot be read is an unmeasured machine, not a healthy one
        # and not a broken one. Said out loud, honoured as absent.
        return "indeterminate", ("the sample was cut short by %s: %s"
                                 % (type(exc).__name__, exc)), {
            "samples": samples, "steps": steps}
    spread = 0.0 if lo is None else hi - lo
    evidence = {"samples": samples, "steps": steps,
                "worstDriftSeconds": round(worst, 4),
                "spreadSeconds": round(spread, 4),
                "windowSeconds": round(awake() - a0, 3),
                # WHICH clock the numbers above were differenced against, and
                # what this host's own suspend history says about that choice.
                # A drift figure whose reference is unnamed is the ambiguity
                # that produced the forgery in the first place.
                "referenceClock": ref_name,
                "referenceRecordedSuspendSeconds": ref_suspend}
    if samples < 2:
        return "indeterminate", ("only %d sample(s) in %.1f s — a step needs at "
                                 "least two intervals to be visible at all"
                                 % (samples, window)), evidence
    # BOTH thresholds, and both must be CLEARED. Either alone is weaker than the
    # measured signature: a single big drift could be one suspend/resume, and a
    # spread with no per-tick step is a clock that drifts rather than jumps.
    if steps >= min_steps and spread >= min_step:
        return "present", (
            "%d step(s) >= %d required; worst per-tick wall-vs-reference drift "
            "%.3f s and total offset spread %.3f s, both >= %.3f s, over %.1f s "
            "awake / %d samples against %s" % (steps, min_steps, worst, spread,
                                               min_step,
                                               evidence["windowSeconds"],
                                               samples, ref_says)), evidence
    return "absent", (
        "%d step(s) (need %d) with worst per-tick drift %.4f s and offset spread "
        "%.4f s (need %.3f s) over %.1f s awake / %d samples: this machine's wall "
        "clock tracked %s"
        % (steps, min_steps, worst, spread, min_step, evidence["windowSeconds"],
           samples, ref_says)), evidence


# THE CLOSED VERB TABLE. `configKeys` closes the config so a typo'd threshold is
# a LOUD refusal and not a silently-ignored key; `floors` is the one thing config
# may NOT weaken.
#
# ★★ WHY THE FLOORS ARE IN CODE AND THE THRESHOLDS ARE IN CONFIG. The operator's
# rule is that tightening a threshold must be an edit, not a code change — so
# `sampleSeconds`, `minStepSeconds` and friends live in legs.json. But a
# threshold that config can LOOSEN without limit is a guard that gets re-cut to
# fit whatever case is in front of it, which this project has already paid for
# twice (D-TEST-PE64-CONFOUND-PIN-WEAKENED-BY-ITS-OWN-SUBJECT). So config may
# only move a threshold in the SAFE direction: the floors below are the weakest
# configuration that can still support a `present`, and the lint refuses anything
# looser, naming the asymmetry. `sampleSeconds >= 15` and `minStepsRequired >= 2`
# are the operator's stated floor — never a single pair of readings.
ENVIRONMENT_PROBE_VERBS = {
    "wall-clock-step": {
        "measure": _measure_wall_clock_step,
        "asks": "does this machine's CLOCK_REALTIME jump relative to REAL "
                "ELAPSED TIME — the monotonic clock that counts a host suspend, "
                "not the one that stops with the machine?",
        "configKeys": ("sampleSeconds", "sampleIntervalMs", "minStepSeconds",
                       "minStepsRequired"),
        "floors": {"sampleSeconds": 15.0, "minStepsRequired": 2,
                   "minStepSeconds": 1.0},
        # Config may only ever RAISE these. `sampleIntervalMs` has no floor: a
        # shorter interval takes MORE samples in the same window, which can only
        # make the measurement finer.
        "raiseOnly": ("sampleSeconds", "minStepsRequired", "minStepSeconds"),
    },
}

# Required on every registry entry, all non-empty — the same discipline
# CONFOUND_PROVENANCE_KEYS imposes on a confound row, for the same reason. A
# probe decides whether a failure is excused; it has to say what it measures,
# what a `present` would mean, and which anchor holds the long form.
PROBE_DECLARATION_KEYS = ("verb", "measures", "presentMeans", "anchor")


def environment_probes(catalogue_doc):
    """The declared registry, or {} — and `{}` is legal: a catalogue in which no
    row is conditional needs no probes. A row that NAMES a probe the registry
    does not declare is refused by the lint, so an empty registry cannot silently
    disable a gate."""
    probes = catalogue_doc.get("environmentProbes", {})
    if not isinstance(probes, dict):
        raise LegError("`environmentProbes` must be an object, got %r"
                       % type(probes).__name__)
    return probes


def probe_verb(name):
    """The declared verb's spec, or a LegError — never a permissive default, for
    the same reason run_filesystem() has none: an unknown verb treated as
    anything at all is a probe whose answer nobody chose."""
    spec = ENVIRONMENT_PROBE_VERBS.get(name)
    if spec is None:
        raise LegError(
            "unknown environment-probe verb %r (known: %s). A probe's verb IS "
            "its measured procedure; there is no default, because the only two "
            "candidates for one are 'assume the defect is here' (which hides "
            "compiler bugs) and 'assume it is not' (which is what an ABSENT "
            "verdict already says out loud)."
            % (name, ", ".join(sorted(ENVIRONMENT_PROBE_VERBS))))
    return spec


# ── WHERE A VERDICT CAME FROM ───────────────────────────────────────────────
#
# ANCHOR, ONE LINE, DO NOT WRAP:
# D-HARNESS-PROBE-VERDICTS-FLAG-INJECTS-AN-UNVALIDATED-PRESENT
#
# ★★★ THE NEW DOOR WAS QUIETER THAN THE OLD ONE, IN THE DANGEROUS DIRECTION.
# `--probe-verdicts FILE` accepted any JSON object, checked only that it was a
# dict, stamped the plan `confoundGating: probed` and honoured whatever it said.
# ✔MEASURED at 0ecec160: a hand-written
# `{"clock-realtime-steps": {"verdict": "present", "why": "I said so", ...}}`
# produced `gating=probed`, all 7 rows ACTIVE, and a report line reading
# `clock-realtime-steps = PRESENT   [wall-clock-step: I said so]` — nothing in
# the plan or the log distinguished it from a measurement. Contrast the OPERATOR
# override DSS_CONFOUNDS, which both drivers announce per leg as
# `[operator DSS_CONFOUNDS - applied to EVERY leg]`.
#
# ⇒ TWO THINGS, AND NEITHER ALONE IS ENOUGH.
#   1. EVERY INJECTED ENTRY IS VALIDATED against the declared registry (the probe
#      NAME must be declared, the `verb` must match ITS declaration, the verdict
#      must be in the closed set, the evidence must be a map) with a named
#      LegError instead of a python traceback from three frames deeper.
#      ✔MEASURED before this: `{"clock-realtime-steps": "present"}` raised
#      `ValueError: dictionary update sequence element #0 has length 1` and
#      `{"clock-realtime-steps": {"why": "..."}}` raised `KeyError: 'verdict'`.
#   2. IT IS HONOURED, AND IT IS LOUD, AND IT CANNOT RUN A CORPUS. An injected
#      verdict still decides rows — the flag exists so a caller and the pins can
#      drive the honouring path without sampling a clock for 20 s — but the plan
#      is stamped `confoundGating: injected`, which is NOT the one gating both
#      drivers accept, so a verdict file captured on the WSL2 box and replayed on
#      the arm64 VPS STOPS THE DRIVER instead of silently restoring the blind
#      spot this gate closed. Every report line derived from one says INJECTED in
#      words, so a log reader can never mistake it for a measurement.
PROBE_SOURCE_MEASURED = "measured"
PROBE_SOURCE_INJECTED = "injected"
PROBE_SOURCES = (PROBE_SOURCE_MEASURED, PROBE_SOURCE_INJECTED)

# Required on every INJECTED entry, all present. `evidence` is required too: a
# verdict with no evidence beside it is the `earnedOn` defect wearing a JSON key,
# and `{}` is a legal answer for the indeterminate arms that have none.
INJECTED_VERDICT_KEYS = ("verdict", "why", "verb", "evidence")


def _validated_verdict_entry(name, got, probes, source_label):
    """ONE verdict entry, checked against the declared registry, or a LegError.

    ★ ONE VALIDATOR, TWO DOORS. The injection flag and the reader of a verdict map
    that arrived over a pipe from another kernel ask exactly the same questions,
    and two copies of these clauses is the shape this harness already has a name
    for: a rule enforced on one path while its sibling shrugs."""
    if name not in probes:
        raise LegError(
            "%s carries a verdict for '%s', which the catalogue's "
            "`environmentProbes` registry does not declare (declared: %s). "
            "A verdict for an undeclared probe can gate nothing, and "
            "accepting it silently would let a typo'd name look like a "
            "measurement that happened. See anchor, ONE LINE, DO NOT WRAP: "
            "D-HARNESS-PROBE-VERDICTS-FLAG-INJECTS-AN-UNVALIDATED-PRESENT"
            % (source_label, name, ", ".join(sorted(probes)) or "<none>"))
    if not isinstance(got, dict):
        raise LegError(
            "%s: the verdict for '%s' is %s, not the object "
            "--probe-environment prints ({verdict, why, verb, evidence}). "
            "A bare string cannot say WHAT was measured or HOW."
            % (source_label, name, type(got).__name__))
    missing = [k for k in INJECTED_VERDICT_KEYS if k not in got]
    if missing:
        raise LegError(
            "%s: the verdict for '%s' omits %s. Every key is required: a "
            "verdict decides whether a failing test is excused, so it states "
            "the answer, the evidence for it, and the verb that produced it. "
            "[D-HARNESS-PROBE-VERDICTS-FLAG-INJECTS-AN-UNVALIDATED-PRESENT]"
            % (source_label, name, ", ".join("`%s`" % k for k in missing)))
    # The verdict word itself, through the same closed check the measuring
    # path uses — so an invented verdict cannot arrive by flag either.
    probe_verdict_honours(got["verdict"])
    if not str(got["why"]).strip():
        raise LegError(
            "%s: the verdict for '%s' carries an EMPTY `why`. A verdict with "
            "no stated evidence is the `earnedOn` defect wearing a JSON key."
            % (source_label, name))
    declared_verb = probes[name].get("verb", "")
    if got["verb"] != declared_verb:
        raise LegError(
            "%s: the verdict for '%s' says verb %r, but the registry declares "
            "that probe's verb as %r. A verdict is the answer of ONE measured "
            "procedure; one carrying another procedure's name is either a "
            "replay of a different probe or a hand-edit, and neither may "
            "decide an excusal."
            % (source_label, name, got["verb"], declared_verb))
    if not isinstance(got["evidence"], dict):
        raise LegError(
            "%s: the verdict for '%s' carries `evidence` of type %s rather "
            "than an object. `{}` is the legal answer when there is none."
            % (source_label, name, type(got["evidence"]).__name__))
    return dict(got)


def validate_probe_verdicts(verdicts, catalogue_doc, source_label):
    """Injected verdicts, PER KERNEL, validated against the declared registry and
    stamped `outcome: injected` — or a named LegError.

    Returns NEW objects; the caller's is never mutated, so a caller that also
    wants the raw file contents still has them.

    ★★ THE FILE NOW NAMES THE KERNEL EACH VERDICT IS ABOUT, and that is the whole
    subject of this cycle rather than a formality: a flat `{probe: verdict}` file
    silently asserted that a run has ONE environment, so a verdict captured on a
    Windows host was applied to a leg whose fixture executes in WSL2. The flat
    shape is REFUSED by name (see below) instead of being read as the driver's,
    because reading it as anything is a guess about which machine it describes.

    ★ WHY EVERY CLAUSE IS HERE RATHER THAN "trust the caller, it is a test flag":
    the flag is reachable from any shell, its effect is to EXCUSE FAILING TESTS,
    and the failure mode is silent. A validated refusal costs a line; an
    unvalidated `present` costs a real compiler defect."""
    probes = environment_probes(catalogue_doc)
    if not isinstance(verdicts, dict):
        raise LegError(
            "%s is not a JSON object mapping a KERNEL to the verdicts measured in "
            "it (got %s). A verdict map decides which failing tests are excused; "
            "a value of another shape cannot be read as one."
            % (source_label, type(verdicts).__name__))
    out = {}
    for kernel in sorted(verdicts):
        if kernel in probes:
            raise LegError(
                "%s is keyed on PROBE NAMES ('%s' is a declared probe), which is "
                "the flat shape this harness used before it could measure more "
                "than one kernel. It is refused rather than read as the driver's, "
                "because which KERNEL a verdict describes is exactly what decides "
                "whether it may excuse a leg: wrap it as {\"<kernel>\": {...}}, "
                "where <kernel> is one of %s. "
                "[D-HARNESS-ENVIRONMENT-PROBE-MEASURES-THE-DRIVERS-KERNEL-NOT-THE-"
                "LAUNCHED-ONE]"
                % (source_label, kernel, ", ".join(sorted(RUN_FILESYSTEMS))))
        if kernel not in probe_kernel_names():
            raise LegError(
                "%s carries verdicts for kernel '%s', which is not a kernel any "
                "declared runFilesystem resolves to (they resolve to: %s). No leg "
                "looks in that drawer, so those verdicts could only ever decide "
                "nothing - silently, which is the direction that hides a "
                "measurement nobody applied."
                % (source_label, kernel, ", ".join(sorted(probe_kernel_names()))))
        got = verdicts[kernel]
        if not isinstance(got, dict):
            raise LegError(
                "%s: the verdicts for kernel '%s' are %s, not the object "
                "--probe-environment prints." % (source_label, kernel,
                                                 type(got).__name__))
        entries = {}
        for name in sorted(got):
            entry = _validated_verdict_entry(
                name, got[name], probes, "%s [kernel '%s']"
                % (source_label, kernel))
            # ★ STAMPED HERE, AND NOT TAKEN FROM THE FILE. A file that said
            # `"source": "measured"` would otherwise be able to launder itself
            # into looking like this process's own measurement, which is the
            # whole hazard.
            entry["source"] = PROBE_SOURCE_INJECTED
            entries[name] = entry
        out[kernel] = kernel_measurement(
            kernel, "injected",
            "read from %s, which attributes these verdicts to kernel '%s'"
            % (source_label, kernel), entries)
    return out


def probe_verdict_source(entry):
    """The declared source of one verdict, or a LegError — never a default.

    An entry with no source is a producer this function has not been taught
    about; reading it as `measured` would be the permissive answer, and reading
    it as `injected` would slander a real measurement. Both doors stamp it."""
    src = entry.get("source")
    if src not in PROBE_SOURCES:
        raise LegError(
            "a probe verdict carries source %r (known: %s). Every producer of a "
            "verdict stamps where it came from — run_environment_probes measures, "
            "validate_probe_verdicts injects — because the report must never read "
            "the same for a measurement and for a file. "
            "[D-HARNESS-PROBE-VERDICTS-FLAG-INJECTS-AN-UNVALIDATED-PRESENT]"
            % (src, ", ".join(PROBE_SOURCES)))
    return src


# ── HOW A PLAN'S GATING WAS ARRIVED AT, AND WHETHER IT MAY RUN A CORPUS ─────
#
# Closed, and each entry says whether a DRIVER may run on it. Exactly one may.
# ⚠ THE DRIVERS SPELL THE TEST AS `== 'probed'` — i.e. "the only usable one" —
# and --self-test asserts that exactly one gating is usable and that it is
# spelled `probed`, so adding a usable gating here without touching both drivers
# reds instead of silently letting a corpus run on it.
CONFOUND_GATINGS = {
    "probed": {
        "usable": True,
        "means": "this process MEASURED the declared probes on this machine",
    },
    "unprobed": {
        "usable": False,
        "means": "nothing was measured; every conditional row is INACTIVE. "
                 "Fail-safe, but the excusals it withholds would surface as "
                 "GENUINE reds and read as compiler regressions",
    },
    "injected": {
        "usable": False,
        "means": "the verdicts were READ FROM A FILE (--probe-verdicts), not "
                 "measured here. Validated and honoured for resolution, so a "
                 "caller and the pins can drive the honouring path, but never "
                 "fit to excuse a failure on THIS machine: the file may have "
                 "been captured on another one",
    },
}


def confound_gating(kernel_measurements):
    """The one word a driver checks. `None` -> unprobed; otherwise the gating
    named by the SOURCE the verdicts carry, ACROSS EVERY KERNEL.

    ★ DERIVED FROM THE VERDICTS THEMSELVES rather than from which CLI flag ran,
    so a second injection door cannot arrive later and be stamped `probed` by
    forgetting a line at the call site.

    ⓘ AN `unreachable` KERNEL IS STILL `probed`, and that is the fail-toward-
    reporting rule, not an oversight: this plan DID measure, in the declared way,
    and the answer for that kernel is INDETERMINATE — which honours nothing.
    Stamping it `unprobed` would make both drivers REFUSE TO RUN AT ALL on a host
    whose distro has no python3, turning "we could not excuse anything here" into
    "no corpus runs today"."""
    if kernel_measurements is None:
        return "unprobed"
    if not isinstance(kernel_measurements, dict):
        raise LegError(
            "probe verdicts of type %s cannot be gated on; an unprobed "
            "resolution is spelled `None`." % type(kernel_measurements).__name__)
    # ⓘ `_measurement_filed_as`, not `_measurement_verdicts`: this walks the WHOLE
    # map, so it is the earliest place a measurement whose self-label disagrees
    # with the key it was filed under can be refused — before any leg opens any
    # drawer, and for every kernel rather than only the one leg's.
    sources = {probe_verdict_source(v)
               for k, m in kernel_measurements.items()
               for v in _measurement_filed_as(k, m).values()}
    if PROBE_SOURCE_INJECTED in sources:
        # ANY injected verdict taints the whole plan's gating. A plan that mixed
        # one measured and one injected verdict and called itself `probed` would
        # be the quiet door again, wearing a majority vote.
        return "injected"
    return "probed"


def run_environment_probes(catalogue_doc, only=None, **instruments):
    """{name: {verdict, why, verb, evidence}} — the machine ANSWERING.

    Deliberately a separate act from `plan_leg`, which stays PURE: the same split
    as `check_launcher` (a plan is the same on every host; a probe is the host
    speaking). Verdicts are then INJECTED back into the plan, exactly as launcher
    availability is.

    `only` restricts the run to the probe names actually required by some row, so
    a catalogue whose rows are all unconditional pays nothing. `instruments` are
    forwarded to the verb's `measure` so the self-test can drive every arm."""
    probes = environment_probes(catalogue_doc)
    out = {}
    for name in sorted(probes):
        if only is not None and name not in only:
            continue
        spec = probes[name]
        verb = spec.get("verb", "")
        impl = probe_verb(verb)
        config = spec.get("config", {})
        try:
            verdict, why, evidence = impl["measure"](config, **instruments)
        except LegError:
            raise
        except Exception as exc:                             # noqa: BLE001
            # The verb itself misbehaved. INDETERMINATE (⇒ absent), named, never
            # swallowed into a green.
            verdict, why, evidence = ("indeterminate",
                                      "probe verb '%s' raised %s: %s"
                                      % (verb, type(exc).__name__, exc), {})
        if verdict not in PROBE_VERDICTS:
            raise LegError(
                "environment probe '%s' (verb '%s') answered %r, which is not "
                "one of %s. A probe that invents a verdict cannot be gated on."
                % (name, verb, verdict, ", ".join(PROBE_VERDICTS)))
        out[name] = {"verdict": verdict, "why": why, "verb": verb,
                     "evidence": evidence,
                     "anchor": spec.get("anchor", ""),
                     "config": dict(config),
                     # ★ WHERE THIS ANSWER CAME FROM, ON THE RECORD.
                     # [D-HARNESS-PROBE-VERDICTS-FLAG-INJECTS-AN-UNVALIDATED-PRESENT]
                     # A verdict this process MEASURED and a verdict handed to it
                     # in a file are not the same evidence, and the report must
                     # never read the same for both. Stamped at BOTH doors — see
                     # validate_probe_verdicts for the other one — because a
                     # field that only one producer sets is a field a reader
                     # cannot trust the absence of.
                     "source": PROBE_SOURCE_MEASURED}
    return out


def required_probe_names(legs):
    """Every probe name any row on any leg requires — the `only` set above."""
    names = set()
    for leg in legs:
        for row in leg.get("confounds", []):
            for nm in row.get("requires", []):
                names.add(nm)
    return names


# ── MEASURING THE KERNEL THE FIXTURE ACTUALLY RUNS IN ───────────────────────
#
# ANCHOR, ONE LINE, DO NOT WRAP:
# D-HARNESS-ENVIRONMENT-PROBE-MEASURES-THE-DRIVERS-KERNEL-NOT-THE-LAUNCHED-ONE
#
# ★★★ ONE MEASUREMENT PER KERNEL, FILED UNDER THAT KERNEL'S NAME. The verdict map
# used to be a single flat `{probe: verdict}` for the whole run, which silently
# asserted that a run has ONE environment. It does not: a Windows driver resolves
# elf64-x86_64 and elf64-arm64 into the WSL2 kernel and pe64-x86_64 into its own,
# and the clock defect lives in exactly one of the two.
#
# ⇒ THE UNIT OF MEASUREMENT IS A KERNEL, and a leg reads the answer filed under
# ITS kernel. A cross-kernel `present` can no longer reach a native leg because it
# is not in that leg's drawer at all — the force-to-indeterminate filter this
# anchor's V1 added is not a second mechanism to keep in step, it is a case that
# stopped existing.
#
# ★ AND THE COST MODEL IS THE REASON IT IS KEYED ON THE KERNEL RATHER THAN ON THE
# LEG: the two ELF legs on a Windows host share one kernel and therefore ONE 20 s
# sample. Keying on the leg would have sampled the same clock twice to get the
# same answer.
KERNEL_PROBE_OUTCOMES = {
    "in-process": "measured by THIS process, which is already in that kernel",
    "entered": "measured INSIDE that kernel, through its declared "
               "`kernelEntryArgv`",
    # ⚠ NOT A VERDICT. `unreachable` says the INSTRUMENT did not run; the verdicts
    # filed with it are INDETERMINATE, which probe_verdict_honours treats as
    # absent, so rows go INACTIVE and the failures they would have excused are
    # reported as GENUINE. "We could not measure" must never become "we measured
    # absent" (it would look like a clean bill) and must never become "present"
    # (it would excuse a real miscompile).
    "unreachable": "the kernel could not be entered, could not run this script, "
                   "or did not answer with a verdict map",
    "injected": "READ FROM A FILE (--probe-verdicts) and attributed to that "
                "kernel by the file, not by any measurement",
}
KERNEL_MEASUREMENT_KEYS = ("kernel", "outcome", "why", "verdicts")

# The outcomes under which the verdicts filed for a kernel are allowed to decide
# that kernel's legs. `unreachable` is the one that is not — and it is excluded
# HERE, in a named tuple, rather than by an `!= "unreachable"` at each reader,
# because a fifth outcome added later must force this line to be re-answered.
KERNEL_OUTCOMES_IN_FORCE = ("in-process", "entered", "injected")


def kernel_measurement(kernel, outcome, why, verdicts):
    """One kernel's answer, with HOW it was obtained beside it — never bare
    verdicts. The outcome is checked against the closed table here, at the one
    constructor, so an invented one cannot reach a decision."""
    if outcome not in KERNEL_PROBE_OUTCOMES:
        raise LegError(
            "unknown kernel-probe outcome %r for kernel %r (known: %s). HOW a "
            "verdict was obtained decides whether it may excuse a failing test, "
            "so an unrecognised outcome cannot be treated as any of them."
            % (outcome, kernel, ", ".join(sorted(KERNEL_PROBE_OUTCOMES))))
    if not str(why).strip():
        raise LegError(
            "the measurement for kernel %r carries an EMPTY `why`. Every outcome "
            "owes its evidence: `unreachable` owes the failure, and a successful "
            "one owes the argv or the words 'in this process'." % kernel)
    return {"kernel": kernel, "outcome": outcome, "why": why,
            "verdicts": verdicts}


def _measurement_verdicts(measurement):
    """The verdicts inside ONE kernel's measurement — with the wrapper CHECKED.

    The same net `_checked_probe_gate` casts one level up: a consumer handed a
    bare `{probe: verdict}` where a kernel measurement belongs would read the
    probe NAMES as kernel names and silently decide nothing, which is the exact
    shape of the defect this cycle removed."""
    if (not isinstance(measurement, dict)
            or any(k not in measurement for k in KERNEL_MEASUREMENT_KEYS)):
        raise LegError(
            "a kernel's entry is %r rather than the object kernel_measurement() "
            "returns (fields: %s). Verdicts are filed PER KERNEL now, and a bare "
            "verdict map here would be read as a kernel whose name is a probe's. "
            "[D-HARNESS-ENVIRONMENT-PROBE-MEASURES-THE-DRIVERS-KERNEL-NOT-THE-"
            "LAUNCHED-ONE]"
            % (sorted(measurement) if isinstance(measurement, dict)
               else type(measurement).__name__,
               ", ".join(KERNEL_MEASUREMENT_KEYS)))
    verdicts = measurement["verdicts"]
    if not isinstance(verdicts, dict):
        raise LegError("kernel %r filed verdicts of type %s"
                       % (measurement.get("kernel"), type(verdicts).__name__))
    return verdicts


def _measurement_filed_as(kernel, measurement):
    """The verdicts inside the measurement filed under `kernel` — with the
    wrapper AND ITS SELF-LABEL checked.

    ★★ THE `kernel` FIELD DECIDES SOMETHING HERE, AND UNTIL THIS IT DECIDED
    NOTHING. It is REQUIRED by KERNEL_MEASUREMENT_KEYS and was read only inside
    an error string, so ✔MEASURED: a measurement self-labelled `"wsl-linux"`
    filed under the map key `"driver"` was ACCEPTED AND HONOURED
    (`gate.kernel='driver' applies=True verdict='present'`) — a verdict about
    the WSL2 kernel deciding a leg that executes on this driver's machine, i.e.
    this anchor's own defect with the two kernels swapped.

    A declared field whose only consumer PRINTS it is this project's recurring
    shape (`sharesDriverKernel` read only by the report; an abort fingerprint
    living in prose while the matcher read the path). The map KEY and the FIELD
    are one claim stated twice, and stating a claim twice buys nothing at all
    unless disagreeing is REFUSED.

    ANCHOR, ONE LINE, DO NOT WRAP:
    D-HARNESS-ENVIRONMENT-PROBE-MEASURES-THE-DRIVERS-KERNEL-NOT-THE-LAUNCHED-ONE
    """
    verdicts = _measurement_verdicts(measurement)
    got = measurement["kernel"]
    if got != kernel:
        raise LegError(
            "the measurement FILED UNDER kernel '%s' says it is ABOUT kernel "
            "%r. A verdict's entire meaning is which machine it describes, so "
            "the two spellings of that cannot be allowed to disagree and "
            "neither can be preferred: believing the KEY would apply %r's "
            "answer to '%s''s legs, and believing the FIELD would file it "
            "where no leg ever looks — one excuses failures produced on a "
            "machine nobody measured, the other hides a measurement nobody "
            "applied. [D-HARNESS-ENVIRONMENT-PROBE-MEASURES-THE-DRIVERS-"
            "KERNEL-NOT-THE-LAUNCHED-ONE]"
            % (kernel, got, got, kernel))
    return verdicts


def kernel_probe_needs(resolved_legs):
    """{kernel: sorted(probe names)} — which kernels this resolution must measure,
    and which probes in each.

    ★ READ OFF THE *RESOLVED* PLAN, NEVER RE-DERIVED. Which kernel a leg executes
    in is the output of launcher resolution, and a second implementation of that
    here is precisely how one ledger becomes two that disagree
    (D-HARNESS-CONFOUND-LEDGER-IS-PER-DRIVER-NOT-PER-LEG, one axis along). The
    caller resolves the plan once with NO verdicts — pure, cheap, and unaffected
    by verdicts, which only ever decide confound rows — and hands the legs here.

    A leg with no conditional row contributes NOTHING, so a catalogue whose rows
    are all unconditional pays nothing at all, and a host on which only the
    launched legs are conditional never samples its own clock."""
    needs = {}
    for leg in resolved_legs:
        names = set()
        for row in leg.get("confoundRows", []):
            names.update(row.get("requires", []))
        if not names:
            continue
        # Through the named accessors, never a bare subscript: a leg that
        # reached here without a resolved `run` is a transport defect between
        # the resolver and this function, and a KeyError traceback names
        # neither the leg nor what was missing.
        kernel = probe_kernel(run_filesystem_verb(leg.get("run")))
        needs.setdefault(kernel, set()).update(names)
    return {k: sorted(v) for k, v in needs.items()}


# ── WHICH LEGS AN INVOCATION READS, AND THEREFORE WHETHER IT MEASURES AT ALL ─
#
# ★ THE RULE: AN INVOCATION MEASURES ONLY WHEN SOME LEG WHOSE DECISIONS IT WILL
# ACTUALLY READ DECLARES A CONDITIONAL ROW.
#
# ⛔ THE REGRESSION THAT ASKED FOR IT, ✔MEASURED on a Windows host:
# `--classify-abort pe64-x86_64 --abort veryquick/nolock.test --abort-log …`
# took 20.957 s (20.616 s re-measured), ALL of it the `wsl.exe -e python3 …
# --probe-environment` child measuring the WSL2 kernel — a kernel pe64-x86_64
# does NOT execute in (its kernel is `driver`), whose drawer its gate therefore
# never opens, and none of whose verdicts anything on that path reads: the
# catalogue's only abort row is `requires: []`, so ZERO verdicts are consulted.
# Before verdicts were per-kernel the same call paid an IN-PROCESS 20 s sample,
# which cannot hang; after, it is a FOREIGN-KERNEL SPAWN bounded at 120 s + 4x
# the window, PER ABORT, in a LOOP over aborts (build-and-test.sh:6487,
# build-and-test.ps1:5558) — on the ABORT path, i.e. the path that runs when
# something has already gone wrong. Cost and robustness, both the wrong way.
#
# ⚠⚠ AND HERE IS THE LIMIT, STATED SO THE NEXT AUTHOR DOES NOT FIND IT BY
# BREAKING IT: THIS DECIDES *WHETHER* TO MEASURE. IT NEVER DECIDES *WHICH
# KERNELS*. `plan()` resolves EVERY leg eagerly, so a leg whose rows require a
# probe whose drawer is missing from the map hits leg_confound_decisions' loud
# "carries NO verdict for it" refusal and stops the run. ⇒ MEASURE ALL OF THE
# PLAN'S KERNELS OR NONE OF THEM, NEVER A SUBSET. "None" is safe because it is a
# DECLARED state: `verdicts=None` is an unprobed resolution, every conditional
# row goes INACTIVE, the plan is stamped `confoundGating: unprobed`, and nothing
# raises — which is exactly why correctness is unchanged for an invocation whose
# consulted leg has no conditional row to make inactive.
#
# ⛔ AND IT IS NOT DONE BY ROUTING THE CHEAP PATH THROUGH `--probe-verdicts`:
# that stamps the gating `injected`, which BOTH drivers refuse BY NAME
# (build-and-test.ps1:341 — "a verdict captured on another box would excuse a
# real miscompile HERE, in silence"). That refusal is correct, and it cannot
# presently tell "mine, this run" from "somebody's file".
def consulted_legs(resolved_legs, labels):
    """The legs whose confound decisions THIS invocation will READ.

    `labels` is None for an invocation that emits the WHOLE plan — every leg's
    decisions cross the wire to the driver, so every leg is consulted — and a
    collection of labels for one that answers about named legs only
    (`--classify-abort LABEL` consults exactly that leg, and reads its
    `confoundDecisions` alone).

    PURE, deliberately: the whole decision is a function of the resolved plan,
    so --self-test pins both directions without spawning anything."""
    if labels is None:
        return list(resolved_legs)
    by_label = {}
    for leg in resolved_legs:
        by_label[leg.get("label", "")] = leg
    out = []
    for label in sorted(labels):
        if label not in by_label:
            raise LegError(
                "this invocation would consult leg %r, which this host's plan "
                "does not contain (planned: %s). Whether an environment "
                "measurement is needed at all is read off the rows the "
                "CONSULTED legs declare, and a leg that is not in the plan "
                "declares nothing — reading that as 'nothing to measure here' "
                "would be a decision taken about a leg that does not exist."
                % (label, ", ".join(sorted(by_label)) or "<none>"))
        out.append(by_label[label])
    return out


def invocation_probe_needs(resolved_legs, labels):
    """{kernel: [probe names]} that the CONSULTED legs require.

    THE DECISION INPUT, NOT THE MEASUREMENT INPUT — read the second ⚠⚠ above.
    Empty means nothing this invocation will read is gated on a measurement, so
    the correct and cheapest answer is to take none; non-empty means take the
    measurement for EVERY kernel in the plan (`kernel_probe_needs`)."""
    return kernel_probe_needs(consulted_legs(resolved_legs, labels))


def resolved_kernel_measurements(resolved_legs, labels, measure):
    """The measurements this invocation resolves with — `None` for one that
    needs none.

    ★ THE WIRING, NOT JUST THE PREDICATE, AND THAT IS THE WHOLE POINT OF THE
    FUNCTION EXISTING. A pure "does it need to measure" that the CLI then
    consults with an `if` leaves the `if` itself unpinned: delete it and the
    predicate's own pins stay green while every invocation pays a foreign-kernel
    spawn again. `measure` is INJECTED — exactly as `runner` and `translator`
    are one function along — so --self-test drives this decision, observes
    whether the measurement was taken AND WHAT IT WAS ASKED FOR, and never
    spawns anything.

    ⚠⚠ AND IT ASKS FOR `kernel_probe_needs(resolved_legs)` — THE WHOLE PLAN'S
    KERNELS — not the consulted legs'. Read the second ⚠⚠ on consulted_legs
    before changing that line: plan() decides every leg eagerly, so a subset is
    the one variation that turns a cheap answer into a loud refusal."""
    if not invocation_probe_needs(resolved_legs, labels):
        return None
    return measure(kernel_probe_needs(resolved_legs))


def kernel_probe_argv(fs_verb, script, catalogue, only, translator=None):
    """The argv that measures `only` INSIDE this verb's kernel, or [] when that
    kernel is this process's own.

    ★ IT IS THIS SCRIPT, RE-ENTERED WITH `--probe-environment`, WHICH IS AN
    IN-PROCESS-ONLY INSTRUMENT. That is what makes recursion structurally
    impossible rather than merely unlikely: the child measures where it is and
    never orchestrates. It is also the instrument an operator can run by hand and
    compare against, byte for byte.

    ⚠ NO ENVIRONMENT CROSSES WITH IT, and none needs to: every input is an
    ARGUMENT. A launcher in another OS namespace does not inherit this driver's
    environment block (that is what `envTransfer` exists to say), so a probe that
    read a variable would read an empty one — silently, which is how the corpus
    resume engine once re-ran itself from the beginning."""
    spec = run_filesystem(fs_verb)
    entry = list(spec["kernelEntryArgv"])
    if not entry:
        return []
    interp = spec["kernelProbeInterpreter"]
    if not interp:
        raise LegError(
            "runFilesystem %r declares a kernelEntryArgv (%s) but no "
            "`kernelProbeInterpreter`. Entering a kernel and having nothing there "
            "to run this script with are two different failures, and a verb that "
            "states one without the other cannot report either."
            % (fs_verb, " ".join(entry)))
    xlate = spec["kernelEntryPathTranslation"]
    argv = entry + [interp, translate_path(xlate, script, translator),
                    "--probe-environment",
                    "--catalogue", translate_path(xlate, catalogue, translator)]
    for name in only:
        argv += ["--probe-only", name]
    # THE SAME NET THE LAUNCHER ARGV GETS. A path left in this driver's namespace
    # does not fail as a path error in the other kernel: python opens a relative
    # file by that name, misses, and the failure reads as a broken script.
    assert_translated(xlate, argv)
    return argv


# ── EVERY CHILD THIS RESOLVER SPAWNS: BOUNDED, AND DECODED ──────────────────
#
# Three functions spawn ON THE PLAN-RESOLUTION PATH — `_run_kernel_probe` (the
# environment measurement), `_run_translator` (`wslpath`, and every launcher
# `probeArgv`) and `_run_machine_probe` (`cc -dumpmachine`). WHAT A FAILURE MEANS
# differs at each of them and must stay theirs to say; these two properties do
# not differ and are therefore owned HERE, once, so a fourth resolution-path
# spawn gets them by construction instead of by somebody remembering.
#
# ⓘ AND THIS IS NOT EVERY SPAWN IN THE FILE, WHICH IS WHY THE SCOPE IS NAMED.
# Outside the resolution path this module also starts: `_run_capture` (the
# loadext-helper BUILD — a compiler run, deliberately unbounded, and carrying
# the same undecodable-output exposure described below), `_mirror_run` (a
# Popen, twin-parity self-test infrastructure), `dss_supports_import_name`
# (already bounded at 60 s, and it decodes bytes itself) and the
# `--build-reference-oracle` compile (bounded by nothing on purpose: it builds
# a whole reference fixture). None of those runs during `--plan`, so none can
# hang it; a claim here that this were the only spawn would be false.
#
# ⚠ (1) A DEADLINE IS COMPULSORY. There is no unbounded call to make by accident:
# `_captured` takes the budget as a required argument.
#
# ⚠ (2) A CHILD WHOSE OUTPUT CANNOT BE DECODED RETURNS SUCCESS AND NO OUTPUT.
# ✔MEASURED 2026-08-12 on this host (Windows, cp1252, python 3.14.3): a child
# writing bytes the locale codec cannot decode does NOT make
# `subprocess.run(..., text=True)` raise. The reader THREAD dies with its own
# UnicodeDecodeError, on its own stack, and run() returns `rc=0, stdout=None` —
# the exact shape of a successful call. `json.loads(None)` then raises
# TypeError, which is neither LegError nor OSError, so it walked past BOTH
# handlers in measure_kernel_environments and killed `--plan`: one non-UTF-8 byte
# printed by a distro ahead of its JSON took down step 1 of every corpus run.
# ⇒ THE ENCODING IS NAMED — never the host's locale, which is not the child's and
# never was — undecodable bytes become U+FFFD, and a captured stream is NEVER
# None. That is what makes parse_kernel_probe_output's promise of a named refusal
# for "a distro that printed a warning before the JSON" true for a warning in ANY
# encoding rather than only an ASCII one.
CHILD_OUTPUT_ENCODING = "utf-8"
CHILD_OUTPUT_ERRORS = "replace"


def _captured(argv, timeout):
    """(rc, stdout, stderr) for one child. rc is taken DIRECTLY off the process,
    never after a pipe, and both streams are always `str`.

    ⚠ rc IS `None` WHEN, AND ONLY WHEN, the child outlived `timeout` and was
    killed. No real process exits `None`, so a deadline cannot be mistaken for
    an exit status, and every caller is FORCED to state what a deadline means to
    it — `unreachable` for a measurement, a named refusal for a path
    translation. Returning some conventional number here instead would have made
    that one decision for all three."""
    import subprocess   # local: the spawn and the import stay together
    try:
        proc = subprocess.run(argv, capture_output=True, text=True,
                              encoding=CHILD_OUTPUT_ENCODING,
                              errors=CHILD_OUTPUT_ERRORS, timeout=timeout)
    except subprocess.TimeoutExpired:
        return None, "", ""
    except OSError as exc:
        return 127, "", "%s" % exc
    # NEVER None PAST THIS LINE. `errors=replace` means the reader thread cannot
    # die any more, and this is the belt to that brace: a stream that somehow
    # arrived absent becomes "" — a value every consumer here already handles —
    # rather than a None that reaches json.loads three frames later.
    return (proc.returncode,
            proc.stdout if proc.stdout is not None else "",
            proc.stderr if proc.stderr is not None else "")


# ⚠ A SPAWN THAT CAN HANG IS A PLAN THAT CAN HANG, AND THE PLAN IS STEP 1 OF EVERY
# CORPUS RUN. `wsl.exe` on a wedged or cold-booting distro can block indefinitely,
# and before this the whole harness had no spawn on the resolution path to hang. So
# the child is BOUNDED, and a timeout is `unreachable` like every other way the
# kernel can fail to answer — never a verdict, never a hang.
# ★ THE BUDGET IS DERIVED FROM THE DECLARED SAMPLE WINDOWS, not a magic number: the
# child is doing exactly the work this catalogue asked for, so tightening a probe's
# `sampleSeconds` tightens its deadline automatically. The allowance on top is for
# ENTERING the kernel (a cold WSL distro is slow) and is deliberately generous —
# this bound exists to stop a hang, not to police a slow machine.
KERNEL_PROBE_ENTRY_ALLOWANCE_SECONDS = 120.0
KERNEL_PROBE_SAMPLE_SLACK = 4.0

# ★ AND THE OTHER TWO SPAWNS GET THE SAME ALLOWANCE, DERIVED, NOT REINVENTED.
# `wsl.exe -e wslpath -a -u <path>`, `wsl.exe -e test -f <path>` and `cc
# -dumpmachine` SAMPLE NOTHING: all any of them does is start a program (for the
# first two, by ENTERING a kernel) and run one trivial command — precisely the
# work the allowance above was made generous for. So this is that constant at a
# ZERO sample window, identically `kernel_probe_budget_seconds(doc, [])`, which
# --self-test asserts. Deriving it means a cold-boot allowance re-tuned once is
# re-tuned for every spawn on the resolution path, rather than one of them
# silently keeping the old number.
RESOLVER_SPAWN_BUDGET_SECONDS = KERNEL_PROBE_ENTRY_ALLOWANCE_SECONDS


def kernel_probe_budget_seconds(catalogue_doc, names):
    """How long the child may take, in seconds, for the probes it was asked."""
    probes = environment_probes(catalogue_doc)
    window = 0.0
    for nm in names:
        cfg = probes.get(nm, {}).get("config", {})
        try:
            window += float(cfg.get("sampleSeconds", 0.0) or 0.0)
        except (TypeError, ValueError):
            # A malformed config is the lint's problem, not this function's; it
            # must not become an UNBOUNDED wait.
            pass
    return KERNEL_PROBE_ENTRY_ALLOWANCE_SECONDS + KERNEL_PROBE_SAMPLE_SLACK * window


def _run_kernel_probe(argv, timeout):
    """(rc, stdout, stderr). rc DIRECTLY off the process, never after a pipe.

    A DEADLINE HERE IS `unreachable`, NOT A VERDICT: rc 124, the conventional
    `timeout(1)` code, so a reader who greps for it finds the same number the
    shell would have reported, and measure_kernel_environments' `rc != 0` arm
    files INDETERMINATE for every probe this kernel owed."""
    rc, sout, serr = _captured(argv, timeout)
    if rc is None:
        # %g, not %.0f: the number printed is the number applied. `%.0f` renders
        # every sub-second deadline as "0 s", which is a diagnostic that
        # contradicts its own budget the moment anyone tightens one.
        return 124, "", "no answer within %g s; the child was killed" % timeout
    return rc, sout, serr


def parse_kernel_probe_output(text, catalogue_doc, only, where):
    """The child's stdout, VALIDATED into a verdict map — or a LegError naming
    what was wrong with it.

    Validated with the SAME clauses the injection door uses, and for a stronger
    reason: this output arrives over a pipe from another kernel, so a distro that
    printed a warning before the JSON, or a python that died half way, must be a
    named refusal and never a partially-read map. `source` is REQUIRED to be
    `measured` rather than overwritten — a child that somehow answered from a file
    is not a measurement of that kernel, and quietly re-stamping it would recreate
    D-HARNESS-PROBE-VERDICTS-FLAG-INJECTS-AN-UNVALIDATED-PRESENT one process out.

    ⚠ AND THE PROMISE ABOVE IS UNCONDITIONAL, NOT "while the warning is ASCII".
    `json.loads(None)` raises TypeError — neither LegError nor OSError, so it
    escaped BOTH of measure_kernel_environments' handlers and killed `--plan`.
    _captured no longer produces a None (see CHILD_OUTPUT_ENCODING), and this is
    the net under ANY other producer, including an injected runner: a non-string
    here is a named transport refusal, never a python traceback."""
    if not isinstance(text, str):
        raise LegError(
            "%s produced %s where its stdout belongs. A captured stream is "
            "text or it is nothing that can be parsed; `json.loads` on it "
            "raises TypeError, which is not a LegError and not an OSError, so "
            "it escapes the two handlers that turn a failed measurement into "
            "an `unreachable` kernel and takes the whole plan down instead. "
            "✔MEASURED: subprocess.run(text=True) returns rc=0 with stdout "
            "None when the locale codec cannot decode what the child wrote."
            % (where, type(text).__name__))
    try:
        got = json.loads(text)
    except ValueError as exc:
        raise LegError("%s did not answer with JSON (%s). First 200 bytes: %r"
                       % (where, exc, text[:200]))
    if not isinstance(got, dict):
        raise LegError("%s answered with %s, not the object --probe-environment "
                       "prints" % (where, type(got).__name__))
    probes = environment_probes(catalogue_doc)
    out = {}
    for name in sorted(got):
        entry = _validated_verdict_entry(name, got[name], probes, where)
        if entry.get("source") != PROBE_SOURCE_MEASURED:
            raise LegError(
                "%s: the verdict for '%s' carries source %r rather than %r. Only a "
                "MEASUREMENT taken in that kernel may decide its legs' excusals."
                % (where, name, entry.get("source"), PROBE_SOURCE_MEASURED))
        out[name] = entry
    missing = [n for n in only if n not in out]
    if missing:
        raise LegError(
            "%s answered for %s but was asked for %s. A kernel that skipped a "
            "probe has not measured it, and the absent name would raise as a "
            "transport defect at the leg that requires it."
            % (where, ", ".join(sorted(out)) or "<nothing>", ", ".join(missing)))
    return out


def measure_kernel_environments(catalogue_doc, needs, script, catalogue,
                                runner=None, translator=None, **instruments):
    """{kernel: kernel_measurement} — every kernel this resolution needs, each
    sampled ONCE.

    ⚠ EVERY FAILURE PATH LANDS ON `unreachable` WITH INDETERMINATE VERDICTS, and
    that asymmetry is the whole safety argument (see `$environmentProbesComment`).
    A kernel we could not reach must not read as a healthy one — nor as a sick
    one. `instruments` are forwarded to the in-process verb so the self-test can
    drive every arm without owning a broken clock."""
    if runner is None:
        runner = _run_kernel_probe
    out = {}
    for kernel in sorted(needs):
        only = list(needs[kernel])

        def _unreachable(why, _kernel=kernel, _only=only):
            """One exit for every way a kernel can fail to answer, so a new one
            cannot be added that forgets to file INDETERMINATE verdicts and thus
            raises at the first leg that requires the probe."""
            text = _ascii_snippet(why, 400)
            return kernel_measurement(
                _kernel, "unreachable", text,
                _indeterminate_verdicts(
                    catalogue_doc, _only,
                    "NOT MEASURED in kernel '%s': %s" % (_kernel, text)))

        # ⚠ NO `if kernel == "driver"` BRANCH, DELIBERATELY. A kernel namespace is
        # always a declared RUN_FILESYSTEMS verb (--self-test asserts it), so the
        # in-process case is the one whose DECLARED `kernelEntryArgv` is empty —
        # read from the table, never recognised by name. A name branch here would
        # be the same defect as `if host == 'wsl'`, one table along.
        # ⚠ AND BUILDING THE ARGV IS ITSELF A THING THAT CAN FAIL IN THE OTHER
        # NAMESPACE: `wslpath` runs inside the distro, so a machine with wsl.exe
        # and no working distro fails HERE, before any probe. That is a kernel we
        # could not reach, not a fatal.
        try:
            argv = kernel_probe_argv(kernel, script, catalogue, only, translator)
        except LegError as exc:
            out[kernel] = _unreachable(
                "kernel '%s' could not even be addressed: %s" % (kernel, exc))
            continue
        if not argv:
            out[kernel] = kernel_measurement(
                kernel, "in-process",
                "measured in this process, which is this driver's own kernel",
                run_environment_probes(catalogue_doc, only=set(only),
                                       **instruments))
            continue
        # ⚠ AND A CALLER'S INSTRUMENTS CANNOT SILENTLY NOT APPLY. They are injected
        # CLOCKS: they reach the in-process verb and there is no way to send them
        # into another kernel. Dropping them quietly would make a self-test that
        # thinks it is driving a stepping clock actually measure the real one —
        # a test that passes for the wrong reason, in the file whose subject is
        # measurements attributed to the wrong machine.
        if instruments:
            raise LegError(
                "kernel '%s' must be ENTERED to be measured (%s), so the injected "
                "instrument(s) %s cannot reach it. A caller driving a probe's arms "
                "must name a kernel this process is already in."
                % (kernel, " ".join(argv[:3]),
                   ", ".join(sorted(instruments))))
        where = "the environment probe in kernel '%s' (`%s`)" % (
            kernel, " ".join(argv))
        try:
            rc, sout, serr = runner(
                argv, kernel_probe_budget_seconds(catalogue_doc, only))
        except OSError as exc:                                   # noqa: BLE001
            rc, sout, serr = 127, "", "%s" % exc
        if rc != 0:
            said = (serr or "").strip() or (sout or "").strip()
            out[kernel] = _unreachable(
                "%s exited %s%s" % (where, rc,
                                    (" - " + said) if said else
                                    " and said nothing"))
            continue
        try:
            verdicts = parse_kernel_probe_output(sout, catalogue_doc, only, where)
        except LegError as exc:
            out[kernel] = _unreachable("%s" % exc)
            continue
        out[kernel] = kernel_measurement(
            kernel, "entered",
            _ascii_snippet("measured inside kernel '%s' by `%s`"
                           % (kernel, " ".join(argv)), 400),
            verdicts)
    return out


def _ascii_snippet(text, limit):
    """`text` collapsed to one line of printable ASCII, truncated to `limit`.

    ★ NOT COSMETIC. Everything here ends up in the confound report, which
    confound_report_lines REFUSES if it is not ASCII — deliberately, because the
    twin-parity proof compares those lines BYTE FOR BYTE across a bash arm and a
    PowerShell arm. A distro that answered in UTF-8, or a path with an accent,
    would otherwise turn "we could not measure that kernel" (recoverable, and the
    whole point of the unreachable outcome) into a fatal at the generator."""
    flat = " ".join(str(text).split())
    safe = "".join(c if 32 <= ord(c) <= 126 else "?" for c in flat)
    if not safe:
        # kernel_measurement REFUSES an empty `why`, and it is right to: an
        # outcome with no evidence is the `earnedOn` defect. "it said nothing" is
        # itself the evidence here.
        return "<the kernel gave no printable detail>"
    return safe[:limit] + ("..." if len(safe) > limit else "")


def _indeterminate_verdicts(catalogue_doc, names, why):
    """The answer for a kernel that could not be asked: one INDETERMINATE per
    probe the legs there require, carrying the failure as its evidence.

    ★ THE ENTRIES EXIST RATHER THAN BEING OMITTED. A missing verdict for a
    required probe is a LOUD transport defect at the leg (and must stay one), so
    an unreachable kernel that filed nothing would STOP THE RUN instead of
    reporting it. The harness must survive an environment it cannot measure."""
    probes = environment_probes(catalogue_doc)
    return {nm: {"verdict": "indeterminate", "why": why,
                 "verb": probes.get(nm, {}).get("verb", ""),
                 "evidence": {}, "anchor": probes.get(nm, {}).get("anchor", ""),
                 "config": dict(probes.get(nm, {}).get("config", {})),
                 "source": PROBE_SOURCE_MEASURED}
            for nm in names}


# ── WHOSE KERNEL DID THE PROBE MEASURE? THE *DECISION*, NOT THE PRINTOUT ────
#
# ANCHOR, ONE LINE, DO NOT WRAP:
# D-HARNESS-ENVIRONMENT-PROBE-MEASURES-THE-DRIVERS-KERNEL-NOT-THE-LAUNCHED-ONE
#
# ★★★ IT WAS PROSE THAT CHANGED NOTHING — THE `earnedOn` DEFECT, ONE FIELD ALONG.
# The caveat confound_report_lines prints says, in words, "rows go INACTIVE and
# such a failure is reported as GENUINE". The DECISION function had never heard of
# `run`, `runFilesystem` or `sharesDriverKernel`. ✔MEASURED at 0ecec160 with
# `--host-os windows --host-arch x86_64 --launchers-available wsl.exe` and a
# `present` verdict: the caveat printed, and THE NEXT LINE printed `confound rows
# ACTIVE (7 of 7)`. The caveat fired on `sharesDriverKernel == False` alone,
# unconditional on the verdict, so it was false whenever it mattered.
#
# ⛔ THE FAILURE IT LEAVES OPEN, WHICH IS NOT HYPOTHETICAL: a Windows host whose
# OWN CLOCK_REALTIME steps (VM checkpoint/migration, a time-sync storm, chrony
# `makestep`) driving the ELF legs through `wsl.exe`. The probe samples the
# WINDOWS clock, answers `present`, and every ^walsetlk-/^busy2- failure produced
# inside the WSL2 kernel is silently excused — including a genuine WAL
# blocking-lock miscompile, which the ^walsetlk- row's own mechanism text says
# must stay red.
#
# ⇒ V1 FILTERED THE VERDICTS BEFORE ANY ROW WAS DECIDED: a leg whose launcher did
# not share this driver's kernel had every verdict forced to `indeterminate`. That
# made the unsafe direction impossible and it is still the behaviour whenever the
# right kernel cannot be measured — but it was CONSERVATIVE, and the cost was
# measured: a Windows-driven corpus at 52cf784d charged 4 walsetlk reds on
# elf64-x86_64 and 3 on elf64-arm64 to DSS that the arm64 VPS ran GREEN from the
# same commit against the same upstream tree.
#
# ★★★ V2 REMOVES THE FILTER BY REMOVING ITS SUBJECT. Verdicts are measured PER
# KERNEL and filed under that kernel's name, and a leg reads the drawer for the
# kernel IT executes in. There is no longer such a thing as "a verdict measured
# here, reaching a leg that runs there" to be filtered out — the wrong kernel's
# answer is not in the leg's drawer at all. What survives is the case where the
# right kernel could not be measured, and that is `outcome: unreachable` with
# INDETERMINATE verdicts, which honours nothing and SAYS SO.
#
# ★ READ FROM THE DECLARED TABLE VIA THE LEG'S RESOLVED LAUNCHER, NEVER FROM THE
# VERB'S NAME. `sharesDriverKernel` is a field on RUN_FILESYSTEMS because Wine,
# qemu-user and `arch -x86_64` are in-process translation on ONE kernel while
# `wsl.exe` crosses into another, and no spelling of a verb tells you which.
#
# ⓘ AND IT HAS EXACTLY ONE READER: `fs_shares_driver_kernel`. An earlier text
# here claimed it was "read TWICE, by two consumers that must agree — probe_kernel
# and this gate"; THE GATE NEVER READ IT. What the gate had was a call to
# `run_shares_driver_kernel(run)` with the return value DISCARDED, whose only
# surviving role was to raise — a validating side effect wearing the name of a
# question. The validation is real and is kept; it now happens under the names of
# the things it actually checks (`run_filesystem_verb` for the run's key,
# `run_filesystem` for the verb's declaration), and the ONE consumer of the
# answer is `probe_kernel`, which is what picks the drawer.


def run_filesystem_verb(run):
    """The resolved run's `runFilesystem`, or a LegError — never a default.

    ★ NO PERMISSIVE DEFAULT, and that stopped being cosmetic the moment this key
    began deciding WHICH KERNEL'S measurement a leg reads.
    `run.get("runFilesystem", "driver")` defaulted to "shares this kernel, apply
    the verdict, print no caveat" — the direction that excuses a real
    miscompile. plan_leg seeds the key on EVERY resolved run ('driver' for a
    native one), so an absent one is a transport defect, never a native run."""
    if not isinstance(run, dict):
        raise LegError(
            "a leg's resolved `run` is %r rather than a run plan. WHICH KERNEL "
            "the fixture executes in decides whether an environment "
            "measurement applies to this leg at all, so there is nothing to "
            "default it to." % type(run).__name__)
    if "runFilesystem" not in run:
        raise LegError(
            "the resolved run plan carries no `runFilesystem`. plan_leg seeds it "
            "on every run ('driver' for a native one), so its absence is a "
            "transport defect between the resolver and its consumers — and "
            "reading it as 'driver' would apply a measurement of THIS machine to "
            "a leg that may execute in another kernel. "
            "[D-HARNESS-ENVIRONMENT-PROBE-MEASURES-THE-DRIVERS-KERNEL-NOT-THE-"
            "LAUNCHED-ONE]")
    return run["runFilesystem"]


def run_mode(run):
    """The resolved run's mode, checked against the closed vocabulary. Required
    for the same reason `runFilesystem` is: the mode decides whether a `scope`d
    row can match here, and an unknown mode reported as matching would overstate
    what was excused."""
    if not isinstance(run, dict) or "mode" not in run:
        raise LegError(
            "the resolved run plan carries no `mode`. plan_leg sets it on every "
            "path (native / launched / skip), so its absence is a transport "
            "defect.")
    mode = run["mode"]
    if mode not in RUN_MODES:
        raise LegError("unknown run mode %r (known: %s)"
                       % (mode, ", ".join(RUN_MODES)))
    return mode


# The fields of the object below. Consumers CHECK for them, so handing the raw
# verdict map to `leg_confound_decisions` or `confound_report_lines` is a loud
# refusal rather than a silent return to the unfiltered behaviour — which is
# exactly how V1 shipped: one function knew about the launcher and the other did
# not, and nothing said so.
PROBE_GATE_KEYS = ("verdicts", "appliesToThisLeg", "why", "runFilesystem",
                   "runMode", "gating", "kernel", "kernelOutcome")


def probe_gate(kernel_measurements, run):
    """THE MEASUREMENT IN FORCE FOR ONE LEG, and the run it was judged against.

    Built ONCE per leg and handed to every consumer, so "which verdicts decide
    this leg's rows" has exactly one owner.

    ★ THE LEG'S OWN KERNEL DECIDES WHICH DRAWER IS OPENED. `kernel_measurements`
    is `None` for an unprobed resolution and otherwise {kernel: measurement}; this
    leg reads the one filed under `probe_kernel(run["runFilesystem"])` and CANNOT
    see any other, which is what makes a verdict about the wrong machine
    structurally unable to excuse a failure here rather than filtered out by a
    rule someone has to remember to apply."""
    mode = run_mode(run)
    verb = run_filesystem_verb(run)   # named refusal on absence, never 'driver'
    # ⓘ AND THIS IS THE READ OF `sharesDriverKernel`: probe_kernel goes through
    # fs_shares_driver_kernel, which goes through run_filesystem, which refuses a
    # verb whose kernel declaration is missing or self-contradictory. One call,
    # every check, and the ANSWER is used rather than discarded.
    kernel = probe_kernel(verb)
    gate = {"verdicts": None, "appliesToThisLeg": False, "why": "",
            "runFilesystem": verb, "runMode": mode, "kernel": kernel,
            "kernelOutcome": "",
            "gating": confound_gating(kernel_measurements)}
    if kernel_measurements is None:
        gate["why"] = ("no environment probe was run for this plan, so nothing "
                       "was measured in kernel '%s' or anywhere else" % kernel)
        return gate
    if not isinstance(kernel_measurements, dict):
        raise LegError(
            "probe verdicts of type %s cannot gate a leg's confounds. An "
            "unprobed resolution is legal and is spelled `None`."
            % type(kernel_measurements).__name__)
    mine = kernel_measurements.get(kernel)
    if mine is None:
        # ⓘ NOT AN ERROR HERE, AND NOT A LICENCE EITHER. A leg with no conditional
        # row needs no measurement at all and this is its ordinary state; a leg
        # that DOES require one gets the LOUD "carries NO verdict for it" refusal
        # from leg_confound_decisions, which is the right place for it because
        # only that function knows which probes this leg's rows actually name.
        gate["verdicts"] = {}
        gate["why"] = ("this resolution measured no kernel '%s', which is where "
                       "this leg's fixture executes" % kernel)
        return gate
    verdicts = _measurement_filed_as(kernel, mine)
    outcome = mine["outcome"]
    if outcome not in KERNEL_PROBE_OUTCOMES:
        raise LegError("kernel '%s' filed unknown outcome %r" % (kernel, outcome))
    gate["kernelOutcome"] = outcome
    gate["appliesToThisLeg"] = outcome in KERNEL_OUTCOMES_IN_FORCE
    if not gate["appliesToThisLeg"]:
        gate["why"] = mine["why"]
    out = {}
    for name, got in verdicts.items():
        if not isinstance(got, dict) or "verdict" not in got:
            raise LegError(
                "the verdict for probe '%s' in kernel '%s' is %r and carries no "
                "`verdict`. Every producer of a verdict map emits the full entry "
                "({verdict, why, verb, evidence, source}); a partial one here is "
                "a transport defect, and a missing verdict cannot be read as "
                "either answer. See anchor, ONE LINE, DO NOT WRAP: "
                "D-HARNESS-PROBE-VERDICTS-FLAG-INJECTS-AN-UNVALIDATED-PRESENT"
                % (name, kernel,
                   got if not isinstance(got, dict) else sorted(got)))
        probe_verdict_source(got)
        # ⚠ BELT AND BRACES, AND IT IS NOT REDUNDANT. measure_kernel_environments
        # already files INDETERMINATE for an unreachable kernel; this makes the
        # rule true of ANY producer, including a future one that files a real
        # verdict beside a failed outcome. A verdict from an outcome that is not
        # in force must never decide a row, whoever wrote it.
        entry = dict(got)
        if not gate["appliesToThisLeg"]:
            entry["verdict"] = "indeterminate"
        out[name] = entry
    gate["verdicts"] = out
    return gate


def _checked_probe_gate(gate, who):
    """The one place a consumer asserts it was handed a GATE and not a raw
    verdict map. Named `who` so the refusal says which consumer was miscalled."""
    if not isinstance(gate, dict) or any(k not in gate for k in PROBE_GATE_KEYS):
        raise LegError(
            "%s was handed %r instead of the object probe_gate() returns "
            "(fields: %s). The gate is what selects the measurement taken in THIS "
            "LEG'S kernel out of the per-kernel map, so a consumer reading a raw "
            "verdict map would read another machine's answer — which is the "
            "defect this object exists to make impossible. "
            "[D-HARNESS-ENVIRONMENT-PROBE-MEASURES-THE-DRIVERS-"
            "KERNEL-NOT-THE-LAUNCHED-ONE]"
            % (who, sorted(gate) if isinstance(gate, dict) else type(gate).__name__,
               ", ".join(PROBE_GATE_KEYS)))
    return gate


def leg_confound_decisions(leg, gate):
    """[{pattern, wire, active, reason, requires, scope}] — every declared row,
    WITH ITS VERDICT, in declaration order.

    ★ EVERY ROW APPEARS, active or not. An inactive row that vanished from this
    list would be indistinguishable from a row nobody declared, and "which rows
    were INACTIVE" is precisely what a reader needs to understand a red.

    `gate` is what probe_gate() returned for THIS LEG — never a raw verdict map.
    A gate whose `verdicts` is None means NO PROBE WAS RUN for this plan; every
    conditional row is then INACTIVE — the fail-safe direction — and the plan says
    `confoundGating: unprobed` so a driver refuses to use it rather than quietly
    under-excusing.

    ⓘ A CROSS-KERNEL VERDICT IS NOT A CASE HERE, and saying it was is the text
    this docstring used to carry. Verdicts are filed PER KERNEL and the gate
    opens only the drawer for the kernel THIS LEG executes in, so a verdict
    measured elsewhere cannot reach this function at all — there is nothing left
    to force. What CAN arrive is a gate whose `appliesToThisLeg` is False, i.e.
    the leg's OWN kernel could not be measured (`unreachable`): every verdict
    filed beside that outcome has already been forced to `indeterminate`, so it
    honours nothing and the rows go INACTIVE."""
    gate = _checked_probe_gate(gate, "leg_confound_decisions")
    probe_verdicts = gate["verdicts"]
    label = leg.get("label", "<unlabelled>")
    if "confounds" not in leg:
        raise LegError(
            "leg '%s' declares no `confounds`. Every leg states which unit "
            "failures have been PROVEN non-DSS on it — `[]` when none ever have, "
            "which is the common and correct answer. It is required because a "
            "missing key cannot be told from an empty one, and the difference "
            "decides whether a failure is reported as a compiler defect." % label)
    out = []
    for row in leg["confounds"]:
        pattern = row["pattern"]
        scope = row.get("scope", "")
        wire = (confound_scope_prefix(scope) if scope else "") + pattern
        # ── CAN A `scope`d PATTERN MATCH ANYTHING ON THIS RUN AT ALL? ────────
        # The matcher (both drivers) is still the only thing that APPLIES the
        # prefix; this only decides what the account is allowed to SAY. A row
        # whose scope excludes this run's mode is SUPPLIED and NOT IN FORCE, and
        # calling that "unconditional" overstated the excusal set.
        # [D-HARNESS-CONFOUND-SCOPE-IS-A-RUN-MODE-NOT-A-HOST]
        scope_modes = confound_scope_run_modes(scope) if scope else ()
        scoped_out = bool(scope) and gate["runMode"] not in scope_modes
        scope_clause = ""
        if scope:
            scope_clause = (
                ". SCOPED `%s:`, and this leg's run mode is `%s`: the matcher "
                "applies a pattern prefixed `%s:` only on a run in mode %s, so "
                "this row is supplied and %s"
                % (scope, gate["runMode"], scope, "/".join(scope_modes),
                   "NOT IN FORCE HERE" if scoped_out else "IN FORCE here"))

        def _decision(active, reason, _requires=(), _pattern=pattern, _wire=wire,
                      _scope=scope, _scoped_out=scoped_out,
                      _clause=scope_clause, _matches=confound_match_kind(row),
                      _row=row):
            """One row's verdict. `active` stays the SUPPLY question — is this
            pattern handed to the matcher — and `scopedOut` is the separate,
            separately-reported question of whether the matcher can apply it on
            this run. Two questions, two fields; collapsing them is what produced
            a row reported ACTIVE with the reason "unconditional"."""
            return {"pattern": _pattern, "wire": _wire, "scope": _scope,
                    "requires": list(_requires), "active": active,
                    "matches": _matches, "row": _row,
                    "scopedOut": _scoped_out, "reason": reason + _clause}

        requires = list(row.get("requires", []))
        if "requires" not in row:
            raise LegError(
                "leg '%s': confound %r declares no `requires`. It is REQUIRED on "
                "every row and `[]` is the ordinary answer — 'this excusal "
                "depends on nothing this harness can measure'. It is not "
                "defaulted, because a missing key could not be told from an "
                "empty one, and the difference is whether a family is excused on "
                "a machine that has never been shown to have the defect. "
                "[D-HARNESS-CONFOUND-SCOPE-IS-A-RUN-MODE-NOT-A-HOST]"
                % (label, pattern))
        if not requires:
            # ⚠ THE WORD "unconditional" IS RESERVED FOR A ROW THAT REALLY IS ONE.
            # A row carrying a `scope` is conditional on the RUN MODE, so saying
            # "unconditional ... nothing this harness measures per run" about it
            # was two false claims in the sentence a reader uses to judge an
            # excusal. [D-HARNESS-CONFOUND-SCOPE-IS-A-RUN-MODE-NOT-A-HOST]
            out.append(_decision(
                True,
                ("requires no environment probe (`requires: []`): this excusal "
                 "rests on its own earned control"
                 if scope else
                 "unconditional (`requires: []`): this excusal rests on its own "
                 "earned control and on nothing this harness measures per run")))
            continue
        if probe_verdicts is None:
            out.append(_decision(
                False,
                "requires %s, and NO environment probe was run for this plan; a "
                "conditional row is never honoured on an unmeasured machine"
                % ", ".join(requires), requires))
            continue
        blocking, holding = [], []
        # ⚠ DEFENDED RATHER THAN ASSUMED, and red-on-disable is what asked for it.
        # The `probe_verdicts is None` branch above is the only thing that keeps this
        # loop from dereferencing None, and when that branch was removed to prove the
        # guard bites, the answer was `AttributeError: 'NoneType' object has no
        # attribute 'get'` — a python message about a python rule, in the one place
        # whose whole job is to say what this harness believes about a machine. A
        # None deref must never be a diagnostic.
        if not isinstance(probe_verdicts, dict):
            raise LegError(
                "leg '%s': confound %r requires environment probe(s) %s, and the "
                "probe verdicts handed to this resolution are %r rather than a "
                "map. An unprobed resolution is legal — it drops every conditional "
                "row — but it is handled by declaring `probe_verdicts=None`, not "
                "by arriving here. [D-HARNESS-CONFOUND-SCOPE-IS-A-RUN-MODE-NOT-A-"
                "HOST]" % (label, pattern, ", ".join(requires),
                           type(probe_verdicts).__name__))
        for nm in requires:
            got = probe_verdicts.get(nm)
            if got is None:
                raise LegError(
                    "leg '%s': confound %r requires environment probe '%s', and "
                    "this run carries NO verdict for it. That is a transport "
                    "defect between the probe run and the plan, not a machine "
                    "without the defect — and guessing either way is the whole "
                    "hazard: guess 'present' and a real miscompile is excused in "
                    "silence." % (label, pattern, nm))
            if probe_verdict_honours(got["verdict"]):
                holding.append("%s: %s" % (nm, got["verdict"]))
            else:
                blocking.append("%s: %s (%s)"
                                % (nm, got["verdict"], got.get("why", "")))
        if blocking:
            out.append(_decision(
                False,
                "NOT honoured here: %s. A failure matching this pattern will be "
                "reported as GENUINE." % "; ".join(blocking), requires))
        else:
            out.append(_decision(True, "honoured: %s" % "; ".join(holding),
                                 requires))
    return out


def leg_confounds(leg, gate):
    """This leg's HONOURED confound patterns, in wire form (`emulated:^re`).

    The gate is applied HERE, once, in the one place both drivers read from — so
    a probe cannot be honoured by one driver and not the other, which is the same
    per-driver asymmetry D-HARNESS-CONFOUND-LEDGER-IS-PER-DRIVER-NOT-PER-LEG was
    about, one axis along."""
    return [d["wire"] for d in leg_confound_decisions(leg, gate)
            if d["active"] and d["matches"] == "unit"]


def leg_abort_confounds(leg, gate):
    """This leg's HONOURED ABORT patterns, in wire form — the same ledger, read
    through the other match kind. Separate ACCESSOR, not a separate list: the
    rows live beside every other confound and are earned by the same lint.

    A UNIT matcher must never see these and an ABORT matcher must never see the
    unit rows, or a row written for one name space would silently excuse a
    failure in the other."""
    return [d["wire"] for d in leg_confound_decisions(leg, gate)
            if d["active"] and d["matches"] == "abort-file"]


# ── WHAT AN ABORT'S DIAGNOSTIC *IS* ─────────────────────────────────────────
#
# ANCHOR, ONE LINE, DO NOT WRAP:
# D-HARNESS-ABORT-CONFOUND-KEYED-ON-LOCATION-NOT-IDENTITY
#
# ★★★ THE FIRST CUT OF THIS MECHANISM MATCHED ONLY `permutation/file`, AND THAT
# EXCUSED A LOCATION RATHER THAN A FAILURE. Any abort anywhere inside
# `nolock.test` — a codegen crash at nolock-3.2, a stack overflow, a wild store —
# matched the row earned for a Windows lock-violation and was silently forgiven.
# That is the exact class the confound ledger exists to prevent, wearing a new
# costume [D-TEST-PE64-CONFOUND-PIN-WEAKENED-BY-ITS-OWN-SUBJECT].
# ★★ AND THE INFORMATION WAS ALREADY IN THE ROW: its `mechanism` field records
# the fingerprint (`error copying "test.db" to "sv_test.db": permission denied`,
# gle=33) and the MATCHER USED NONE OF IT — this project's recurring failure mode
# by name, "a comment that records the full fact while the code uses half of it"
# (unistd.json's alias set; UCRT-P5's `_setjmp`). So the code now uses the whole
# fact: a row must ALSO constrain the DIAGNOSTIC, and `abortDiagnostic` is
# REQUIRED, not optional — an optional field is one the next row omits.
#
# ⓘ WHY THE LAST-EMITTED TEST IS *NOT* ALSO CONSTRAINED, stated rather than
# dropped in silence. It was considered and REFUSED as brittle: this harness
# pulls upstream sqlite on every run, so `nolock-5.1` is a COORDINATE, not an
# identity — an upstream renumbering would stop the row matching and turn an
# earned confound into a red that reads as a compiler regression, which is the
# failure direction that costs a day. The DIAGNOSTIC is the identity; the test
# number is where it happened to happen. If a future row genuinely needs the
# test name to disambiguate two failures with the same text in one file, add it
# THEN, with that case as the evidence.


def abort_diagnostic_text(log_text):
    """The fatal tail of an ABORTED segment's log — everything the fixture
    printed AFTER it last did its job — or "" when there is none.

    PURE: takes text, returns text, so the rule is unit-testable without a
    corpus. This is deliberately NOT `Read-CorpusSegment`'s `Diagnostic`/`A`
    fact, which is the FIRST non-fixture line and exists for the ZERO-PROGRESS
    path. An abort like nolock's happens after 23 passing tests, so its fatal
    text is at the END; keying on the first line would match a banner.

    ★ "" IS A REAL ANSWER AND IT MEANS NO MATCH. A silent zero-byte crash yields
    no diagnostic, and absence of evidence must never satisfy a matcher."""
    fixture_noise = re.compile(
        r"^\s*(Time:\s|Memory used:|Page-cache used:|Scratch used:|"
        r"Malloc count:|SQLite \d|\d+ errors out of \d+ tests)"
        r"|\.\.\.\s*Ok\s*$|^\S+\.test-\S*\.\.\.")
    lines = (log_text or "").splitlines()
    last_working = -1
    for i, ln in enumerate(lines):
        if fixture_noise.search(ln):
            last_working = i
    tail = [ln.rstrip() for ln in lines[last_working + 1:] if ln.strip()]
    return "\n".join(tail)


def classify_abort(leg, gate, abort_name, diagnostic):
    """The leg-and-gate front door. The decision itself is
    classify_abort_decisions, so the CLI can hand it the decisions the RESOLVED
    PLAN already carries and the two can never be two matchers."""
    return classify_abort_decisions(leg_confound_decisions(leg, gate),
                                    leg.get("label"), abort_name, diagnostic)


def classify_abort_decisions(decisions, label, abort_name, diagnostic):
    """(row, why) for the FIRST active `abort-file` row whose file pattern AND
    diagnostic pattern BOTH match, else (None, why-not).

    ★ ONE MATCHER, BOTH DRIVERS. The unit matcher is per-driver because the two
    receive their patterns through genuinely different transports; an abort
    arrives as one short string in both, so there is no such excuse here and a
    second regex implementation would be a second thing to get wrong.
    A `scope`d row is applied only when the run mode allows it, exactly as the
    unit matcher does — `scopedOut` is honoured, not just reported.

    ★★ THE CONJUNCTION IS THE WHOLE POINT: location AND identity. See the
    header above for why matching the file alone was a defect."""
    if not (diagnostic or "").strip():
        # FAIL TOWARD REPORTING. No extractable diagnostic (a silent, zero-byte
        # crash is a real case this harness has already hit once) means the
        # abort cannot be IDENTIFIED, and an unidentified abort is a compiler
        # suspect. It is never excused for being unreadable.
        return (None,
                "abort %r on leg '%s' produced NO extractable diagnostic, so it "
                "cannot be identified — it stays UNEARNED and fails the leg. "
                "Absence of evidence never satisfies a matcher "
                "[D-HARNESS-ABORT-CONFOUND-KEYED-ON-LOCATION-NOT-IDENTITY]"
                % (abort_name, label))
    file_hits = []
    for d in decisions:
        if not d["active"] or d["matches"] != "abort-file" or d["scopedOut"]:
            continue
        if not re.search(d["pattern"], abort_name or ""):
            continue
        want = d["row"].get("abortDiagnostic", "")
        if want and re.search(want, diagnostic):
            return (d, "matched the earned abort row %r AND its diagnostic %r"
                       % (d["pattern"], want))
        file_hits.append((d["pattern"], want))
    if file_hits:
        # ★ THE MESSAGE A TRIAGER NEEDS, and the one this defect would have
        # denied them: the row for this FILE exists and this is NOT its failure.
        return (None,
                "abort %r on leg '%s' is in a file that HAS an earned row (%s), "
                "but its diagnostic does NOT match that row's — this is a "
                "DIFFERENT failure in the same file and it is charged to the "
                "compiler. Earned diagnostic: %s. Observed: %r"
                % (abort_name, label,
                   ", ".join(p for p, _ in file_hits),
                   "; ".join(w for _, w in file_hits),
                   diagnostic.strip()[:400]))
    return (None, "no ACTIVE `matches: abort-file` row on leg '%s' matches %r, "
                  "so this abort is UNEARNED and still fails the leg — an "
                  "unproven abort is a compiler suspect until a row shows its "
                  "work [D-HARNESS-ABORT-HAS-NO-EARNED-CONFOUND-VOCABULARY]"
                  % (label, abort_name))


def confound_report_lines(label, decisions, gate):
    """THE LINES BOTH DRIVERS PRINT, generated ONCE here.

    ★★ `earnedOn` FAILED BECAUSE IT IS PROSE NOTHING READS. A probe result
    nobody SEES is the same failure with extra steps, so this is not optional
    output: every run states, per leg, which probes ran, each verdict WITH ITS
    MEASURED EVIDENCE, and which rows are consequently ACTIVE vs INACTIVE. A run
    whose report cannot say why a failure was excused has not earned the
    exclusion.

    Generated here rather than in each driver so the two cannot drift into
    printing different accounts of the same decision — and so the differential
    battery has one answer to compare.

    ★★ AND IT REPORTS WHAT HAPPENED, NOT WHAT WOULD BE SAFE. Every line here is
    derived from the SAME gate that decided the rows, so the account cannot
    contradict the decision the way the old caveat did (it announced rows INACTIVE
    on the line before `ACTIVE (7 of 7)`). Three facts each get their own words:
    what the probe MEASURED, whether that measurement was APPLIED to this leg, and
    whether it was MEASURED HERE at all or read from a file."""
    gate = _checked_probe_gate(gate, "confound_report_lines")
    probe_verdicts = gate["verdicts"]
    lines = []
    used = sorted({nm for d in decisions for nm in d["requires"]})
    if not used:
        lines.append("[%s] environment probes: NONE REQUIRED - every declared "
                     "confound row is unconditional (`requires: []`)" % label)
    elif probe_verdicts is None:
        lines.append("[%s] environment probes: NOT RUN for this plan, so all %d "
                     "conditional row(s) are INACTIVE" % (label, len(used)))
    else:
        # ★★★ WHICH KERNEL THE NUMBERS BELOW ARE ABOUT, ONCE, BEFORE THEM.
        # A verdict with no machine attached is what this whole anchor is about:
        # the report used to print `clock-realtime-steps = ABSENT` on a leg whose
        # fixture executed in a kernel measured PRESENT the same minute, and
        # nothing in the line said which of the two it meant.
        # ANCHOR, ONE LINE, DO NOT WRAP:
        # D-HARNESS-ENVIRONMENT-PROBE-MEASURES-THE-DRIVERS-KERNEL-NOT-THE-LAUNCHED-ONE
        lines.append(
            "[%s] environment probes: this leg's fixture executes in kernel '%s' "
            "(runFilesystem '%s'), and the verdicts below were %s"
            % (label, gate["kernel"], gate["runFilesystem"],
               KERNEL_PROBE_OUTCOMES.get(gate["kernelOutcome"],
                                         "obtained in an unstated way")))
        for nm in used:
            got = probe_verdicts.get(nm, {})
            # ★★ WHERE THE ANSWER CAME FROM, IN WORDS, ON EVERY DERIVED LINE.
            # An injected verdict that read like a measurement is
            # D-HARNESS-PROBE-VERDICTS-FLAG-INJECTS-AN-UNVALIDATED-PRESENT.
            injected = probe_verdict_source(got) == PROBE_SOURCE_INJECTED
            lines.append(
                "[%s] environment probe %s = %s%s   [%s: %s]"
                % (label, nm, str(got.get("verdict", "?")).upper(),
                   ("   [INJECTED by --probe-verdicts, NOT MEASURED ON THIS "
                    "MACHINE - this plan is confoundGating '%s' and NO driver "
                    "will run a corpus on it]" % gate["gating"]) if injected
                   else "",
                   got.get("verb", "?"), got.get("why", "")))
        # ★★ THE CAVEAT NOW FIRES ON THE MEASUREMENT HAVING FAILED, NOT ON THE
        # LAUNCHER BEING FOREIGN. Its old condition was `sharesDriverKernel ==
        # False`, i.e. "this leg runs somewhere else" — which was the right thing
        # to say only while the probe could not GO there. Leaving it in place now
        # would be a new instance of the very defect it was written for: text that
        # was true when it was written and is read as a fact about today's run.
        # ⇒ WHAT IT SAYS TODAY: we tried to measure THIS leg's own kernel and could
        # not, so the verdicts above are INDETERMINATE, the rows are INACTIVE, and
        # a matching failure is reported as GENUINE.
        # ANCHOR, ONE LINE, DO NOT WRAP:
        # D-HARNESS-ENVIRONMENT-PROBE-MEASURES-THE-DRIVERS-KERNEL-NOT-THE-LAUNCHED-ONE
        if not gate["appliesToThisLeg"]:
            lines.append(
                "[%s] CAVEAT: NOT MEASURED IN KERNEL '%s' - %s. Every verdict "
                "above is therefore INDETERMINATE, which is honoured as ABSENT, "
                "so every conditional row below is INACTIVE and such a failure is "
                "reported as GENUINE. That is the safe direction and not a clean "
                "bill: it is the absence of a measurement, never a measurement of "
                "absence." % (label, gate["kernel"], gate["why"]))
    active = [d for d in decisions if d["active"]]
    inactive = [d for d in decisions if not d["active"]]
    lines.append("[%s] confound rows ACTIVE (%d of %d): %s"
                 % (label, len(active), len(decisions),
                    " ".join(d["wire"] for d in active) or "<none>"))
    # ★ A SUPPLIED ROW THE MATCHER CANNOT APPLY ON THIS RUN GETS ITS OWN LINE.
    # It is in the ACTIVE list because it IS handed to the matcher, and the matcher
    # is the one owner of scope matching — but a reader counting the excusal set
    # from the line above would over-count it, and the row's reason used to say
    # "unconditional". [D-HARNESS-CONFOUND-SCOPE-IS-A-RUN-MODE-NOT-A-HOST]
    for d in active:
        if d.get("scopedOut"):
            lines.append("[%s] confound row SCOPED OUT: %s - %s"
                         % (label, d["wire"], d["reason"]))
    for d in inactive:
        lines.append("[%s] confound row INACTIVE: %s - %s"
                     % (label, d["wire"], d["reason"]))
    # ⚠ ASCII ONLY, ASSERTED RATHER THAN INTENDED. These lines cross into a bash
    # variable and a PowerShell string through two different file readers, and the
    # differential battery compares them BYTE FOR BYTE — so one em-dash would put
    # an encoding question inside the one value the twin-parity proof rests on
    # (the same reason the zero-progress sentinel is ASCII). Caught HERE, at the
    # generator, because the alternative is finding it as a mysterious mirror
    # failure on whichever host has the other default codepage.
    for line in lines:
        bad = [c for c in line if ord(c) > 126]
        if bad:
            raise LegError(
                "the confound report for leg '%s' contains non-ASCII character(s) "
                "%r. This text is compared byte-for-byte between a bash arm and a "
                "PowerShell arm; a non-ASCII character makes the twin-parity proof "
                "a test of two codepages instead of two implementations. Line: %s"
                % (label, sorted(set(bad)), line))
    return lines

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

# ── THE DERIVING HOST'S `configure` ANSWERS ARE NOT THIS TARGET'S ───────────
#
# D-HARNESS-MACHO-LEG-INHERITS-THE-DERIVING-LINUX-HOSTS-CONFIGURE-PROBES.
#
# EXACTLY THE ZCONF_GUARDS DEFECT, ONE LAYER UP, and it is worth stating in the
# same shape because the remedy is the same shape. The recipe is derived by
# running `make -n` on the DERIVING host, and it carries `_HAVE_SQLITE_CONFIG_H`
# — the ONLY host-probe define left on the command line — plus an include path to
# that host's own build dir. So `sqliteInt.h`'s `#include "sqlite_cfg.h"` pulls in
# the DERIVING host's `./configure` answers WHOLESALE and applies them to whatever
# target the leg names.
#
# ✔MEASURED, THE WHOLE CHAIN: the deriving Linux `sqlite_cfg.h` sets
# `HAVE_PREAD64`/`HAVE_PWRITE64`; `os_unix.c` reads
# `#if defined(HAVE_PREAD64) && defined(HAVE_PWRITE64)` -> `USE_PREAD64` -> the
# `osPread64`/`osPwrite64` macros, whose casts are typed with `off64_t` — a type
# Darwin does not have. `off64_t` + `pread64` + `pwrite64` are ONE defect, not
# three. ⛔ There is NO `HAVE_OFF64_T` macro anywhere in sqlite; an earlier
# `$recipeTransformComment` in legs.json named one, and it never existed.
#
# ★ THE CLASS IS CLOSED, AND IT WAS CLOSED BY MACHINE DIFF rather than by reading
# the header and reasoning about it: the deriving Linux `sqlite_cfg.h` against the
# Mac's OWN configure-generated one, EXACTLY THREE LINES DIFFER —
# `HAVE_PREAD64`, `HAVE_PWRITE64`, `HAVE_MALLOC_H`. All ~49 other answers
# (`HAVE_FDATASYNC`, `HAVE_GMTIME_R`, `HAVE_LOCALTIME_R`, `HAVE_USLEEP`,
# `HAVE_UTIME`, `HAVE_ISNAN`, `HAVE_NANOSLEEP`, `HAVE_REALPATH`, `HAVE_DLOPEN`,
# `HAVE_ZLIB`, the `HAVE_INT*_T` family, `SIZEOF_OFF_T 8`, …) are BYTE-IDENTICAL.
# That is why this vocabulary is three names and not fifty: a leg declares only
# what `configure` actually VARIES across the targets this harness builds, and
# the deriving host answers the rest — correctly, because the answers agree.
#
# ★ AND IT IS NOT A `-D` PROBLEM. A define can be dropped from a command line; a
# `#define` inside an included header cannot. `windows-selfconfig` drops
# `_HAVE_SQLITE_CONFIG_H` entirely and lets sqlite self-configure from
# `SQLITE_OS_WIN` — see the REJECTED-ALTERNATIVES note on `configure_stages()` for
# why the same blunt move is the WRONG answer on Darwin.
#
# Each entry names the target OSes on which the answer is TRUE, so a declaration
# can be CROSS-CHECKED against the leg's own spec instead of trusted. The `why`
# is the evidence, not a rationalisation: it is what a reader needs to judge
# whether the derivation is still right when a sixth leg arrives.
CONFIGURE_ANSWERS = {
    "HAVE_PREAD64": {
        "trueOnTargetOs": ("linux",),
        "why": "pread64() is the glibc LFS64 spelling. ✔MEASURED on the "
               "operator's Mac (macOS 26.5.2, arm64) by sqlite's OWN configure "
               "methodology — compile AND link a TU calling it against "
               "/usr/bin/cc: LINK-FAIL (plain `pread` LINK-OK). Windows has no "
               "such libc name either.",
    },
    "HAVE_PWRITE64": {
        "trueOnTargetOs": ("linux",),
        "why": "The write half of the same glibc LFS64 pair, and it must be "
               "declared separately because os_unix.c requires BOTH before it "
               "defines USE_PREAD64. ✔MEASURED on that same Mac: LINK-FAIL "
               "(plain `pwrite` LINK-OK).",
    },
    "HAVE_MALLOC_H": {
        "trueOnTargetOs": ("linux", "windows"),
        "why": "<malloc.h> is a glibc header and a Microsoft CRT header; Darwin "
               "ships <malloc/malloc.h> and no <malloc.h>. ✔MEASURED on the "
               "operator's Mac: HDR-MISS (every other probed header, including "
               "<sys/sysctl.h> and <zlib.h>, resolved). ✔MEASURED on this "
               "project's Windows box: the UCRT ships malloc.h under the Windows "
               "SDK's ucrt/ include dir. ⚠ LATENT, NOT LIVE: mem1.c reads "
               "`#if HAVE_MALLOC_H && HAVE_MALLOC_USABLE_SIZE`, and "
               "HAVE_MALLOC_USABLE_SIZE is absent on BOTH the deriving host and "
               "the Mac — so this row has never fired. It is declared anyway "
               "because it is the same leak, and 'it happens not to fire today' "
               "is not a reason to ship a wrong answer.",
    },
}
CONFIGURE_ANSWER_NAMES = tuple(sorted(CONFIGURE_ANSWERS))


def configure_answer_for_target_os(name, target_os):
    """The value `configure` WOULD produce for `name` on `target_os` — the
    derivation the lint checks a declaration against. Raises on an unknown
    symbol, because a name nothing derives cannot be cross-checked at all."""
    entry = CONFIGURE_ANSWERS.get(name)
    if entry is None:
        raise LegError("unknown configure answer %r (known: %s)"
                       % (name, ", ".join(CONFIGURE_ANSWER_NAMES)))
    return target_os in entry["trueOnTargetOs"]

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
    """(rc, stdout). rc is taken DIRECTLY off the process, never after a pipe.

    Bounded and decoded through the same `_captured` every other spawn here uses
    — a compiler that never returns is the same hazard as a distro that never
    returns, and leaving ONE of three siblings unbounded is the shape this
    project already has a name for. A deadline is rc 124 with the reason as the
    output, which resolve_target_cc already reports as "cannot state its target;
    REFUSED rather than assumed" — the right direction for a compiler that will
    not answer."""
    rc, out, _err = _captured(argv, RESOLVER_SPAWN_BUDGET_SECONDS)
    if rc is None:
        return 124, ("no answer within %g s; the probe was killed"
                     % RESOLVER_SPAWN_BUDGET_SECONDS)
    return rc, out


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


# ── THE ATTRIBUTION ORACLE, PER LEG ─────────────────────────────────────────
#
# ANCHOR, ONE LINE, DO NOT WRAP: D-HARNESS-PE64-HAS-NO-SAME-PLATFORM-ORACLE
#
# ★★★ THE DEFECT THIS CLOSES WAS AN OUTPUT LINE, NOT A MISSING FEATURE. The
# harness builds ONE reference testfixture per run — with the DERIVING host's
# gcc, so on every host this project uses it is an ELF Linux binary — and then
# Step 9 printed a single `oracle : <path>` line for the WHOLE run. For the
# `elf64-x86_64` leg that line is very nearly a matched control. For `pe64` it
# names a binary of a DIFFERENT PLATFORM that cannot be run against a pe64-only
# failure at all. ⇒ THE LINE READ AS AN AVAILABLE CONTROL AND WAS NOT ONE, which
# is strictly worse than printing nothing: an absent control is an honest gap, a
# claimed-and-unheld one retires the reader's suspicion.
#
# ★ THE ORACLE IS THEREFORE A PER-LEG FACT AND IS DERIVED, NEVER ASSERTED. Its
# two inputs are both MEASURED: the reference binary's own target (read out of
# its header by `--identify-binary`) and the leg's DECLARED `spec`. No host is
# consulted at any point — the same rule the loadext resolver above runs under.
#
# ★ AND A LEG CAN NOW HAVE ITS OWN. `--build-reference-oracle` compiles the
# leg's OWN manifest — the same `.dss-project.json` DSS consumed, so the TU set,
# the include roots, the defines and the libraries are one declaration read
# twice — with the compiler `resolve_target_cc` already verifies against that
# leg's target. That is what an oracle IS: one input, two compilers. It also
# happens to be the only shape that CAN work cross-target, because upstream's
# autotools Makefile is configured for the deriving host and cannot emit a
# foreign-target fixture.

# The verdict vocabulary. Closed, because "the oracle is sort of available" is
# the state this whole anchor is about.
# ANCHOR, ONE LINE, DO NOT WRAP: D-HARNESS-FAILING-REFERENCE-ORACLE-COLLAPSES-TO-NO-ORACLE
#
# ★★★ `--build-reference-oracle` HAS ALWAYS REPORTED WHICH OF THESE HAPPENED, AND
# THE CLASSIFIER THREW IT AWAY. The statuses below are that command's own return
# vocabulary; `attribute_build_failure` already reads them, and this constant now
# OWNS them so the two readers cannot drift. A second spelling of a fact that has
# an owner is the defect this file's own header argues against.
ORACLE_STATUS_BUILT = "built"
ORACLE_STATUS_BUILD_FAILED = "build-failed"
ORACLE_STATUS_NO_COMPILER = "no-reference-compiler"
ORACLE_STATUS_NOT_CALLED = ""
ORACLE_STATUSES = (ORACLE_STATUS_BUILT, ORACLE_STATUS_BUILD_FAILED,
                   ORACLE_STATUS_NO_COMPILER, ORACLE_STATUS_NOT_CALLED)
# The two that mean the control RAN. Named once, read by the classifier AND by
# attribute_build_failure, because "did the control run?" is one question.
ORACLE_STATUSES_ATTEMPTED = (ORACLE_STATUS_BUILT, ORACLE_STATUS_BUILD_FAILED)

ORACLE_CLASSES = {
    "same-platform": "a reference binary built for THIS LEG'S OWN TARGET — run "
                     "it on the same input to attribute a failure",
    "cross-platform": "a reference binary exists, but for a DIFFERENT PLATFORM "
                      "— it is NOT an oracle for this leg",
    # ★★ THE THREE STATES `absent` USED TO SPELL AS ONE. They differ in what the
    # READER should do next, which is the only test that matters for a verdict
    # line: fix the harness's reader / accept there is no control on this host /
    # go find out why it was never attempted. Collapsing them cost cycles P13 and
    # P14, which spent two full passes hunting a dss miscompile that was upstream,
    # because "the leg reports NO ORACLE" read as "no control was possible" when
    # the truth was "the control ran, failed, and its log is on disk".
    "build-failed": "the leg's OWN same-platform reference compiler RAN AND "
                    "FAILED to build this leg's manifest — a control was "
                    "attempted, its log exists, and reading it is the next step",
    "no-reference-compiler": "no declared targetCc candidate both exists on this "
                             "host AND targets this leg, so no same-platform "
                             "control can be built HERE — an environment limit, "
                             "not a finding about this leg",
    "absent": "no reference binary survived this run at all",
}

# The fallback control, named once. The row that opened this anchor insisted the
# weaker form be STATED rather than left implicit: TF-C124 attributed
# `win32longpath-1.3` on pe64 with no same-platform oracle, by holding the
# COMPILER constant and varying only the RUNTIME (the same dss-built
# testfixture.exe: `Ok` on real Windows, failing under wine). That is a
# legitimate control and a report that omits it understates what the harness can
# still do.
ORACLE_FALLBACK_CONTROL = (
    "FALLBACK CONTROL (weaker, but real): hold the COMPILER constant and vary "
    "the RUNTIME — run this leg's OWN dss-built artefact under a second runtime "
    "for the same target. A difference is then the runtime's, not the "
    "compiler's. This is how win32longpath-1.3 was attributed on pe64 with no "
    "same-platform oracle [D-HARNESS-PE64-HAS-NO-SAME-PLATFORM-ORACLE]."
)


def oracle_class_for_leg(leg, ref_target, ref_path, oracle_status=""):
    """(class, why) — is the run's reference binary an oracle for THIS leg?

    PURE: no filesystem, no process, no host. `ref_target` is the triple
    `--identify-binary` MEASURED off the reference's own header; `ref_path` is
    empty when no reference survived the run. The comparison is the same
    arch+OS rule `machine_matches_spec` applies to a compiler's `-dumpmachine`,
    because "does this binary belong to that target" and "does this compiler
    produce that target" are one question asked of two artefacts.

    `oracle_status` is `--build-reference-oracle`'s own reported status for THIS
    leg, verbatim from ORACLE_STATUSES. It only ever refines the no-binary case:
    a binary that exists is classified by its TARGET and nothing else, because a
    status cannot make a cross-platform binary into an oracle.
    [D-HARNESS-FAILING-REFERENCE-ORACLE-COLLAPSES-TO-NO-ORACLE]

    ⚠ An UNRECOGNISED status RAISES rather than degrading to `absent`. A verdict
    line is exactly where a typo'd flag value must not buy silence: it would
    print the most pessimistic class and read as a measured fact."""
    if oracle_status not in ORACLE_STATUSES:
        raise LegError(
            "oracle status %r is not one this catalogue declares (known: %s). "
            "The status is `--build-reference-oracle`'s OWN return vocabulary "
            "and this classifier will not guess at a value it has never heard "
            "of — a wrong verdict line is worse than a refused one."
            % (oracle_status, ", ".join(repr(s) for s in ORACLE_STATUSES)))
    if not ref_path:
        if oracle_status == ORACLE_STATUS_BUILD_FAILED:
            return ("build-failed",
                    "this leg's own same-platform reference compiler RAN and "
                    "FAILED — the control was ATTEMPTED and its build log is on "
                    "disk, so this is a lead, not a dead end")
        if oracle_status == ORACLE_STATUS_NO_COMPILER:
            return ("no-reference-compiler",
                    "no declared targetCc candidate both exists on this host and "
                    "targets this leg, so no same-platform control can be built "
                    "HERE — the leg is fine; this host cannot check it")
        return ("absent", "no reference binary was produced or preserved")
    if not ref_target:
        return ("absent",
                "a reference binary exists at %s but its target could not be "
                "MEASURED, so whether it is an oracle for this leg is unknown — "
                "and an unknown control is not a control" % ref_path)
    # --identify-binary answers `arch<TAB>format`, joined with ':' by the
    # drivers — the same `<arch>:<format>` shape a leg's `spec` is written in, so
    # ONE pair of readers answers for both artefacts and neither side is parsed
    # by a rule written for the other.
    spec = leg.get("spec", "")
    want = (canon_arch(spec_target_arch(spec)), canon_os(spec_target_os(spec)))
    got = (canon_arch(spec_target_arch(ref_target)),
           canon_os(spec_target_os(ref_target)))
    if "" in (spec_target_os(spec), spec_target_os(ref_target)):
        return ("absent",
                "the reference (%s) or this leg (%s) does not name an OS in the "
                "shape this catalogue reads, so whether they share a platform "
                "cannot be DECIDED — and an undecided control is not a control"
                % (ref_target, spec or "<undeclared>"))
    if want == got:
        return ("same-platform",
                "the reference targets %s/%s, which is this leg's own target (%s)"
                % (got[0], got[1], spec))
    return ("cross-platform",
            "the reference targets %s/%s (%s); this leg targets %s/%s (%s)"
            % (got[0], got[1], ref_target, want[0], want[1], spec))


def oracle_report_lines(leg, ref_target, ref_path, leg_oracle=None,
                        resolver=None, oracle_status=""):
    """The per-leg oracle verdict, as the lines a driver PRINTS verbatim.

    ONE implementation of the prose, called by both drivers — the same argument
    the confound report makes for living here. A leg with no oracle says so IN
    THOSE TERMS and names the fallback; it never gets a line that could be read
    as a held control.

    `leg_oracle` is the leg's OWN same-platform reference when one was built:
    {"path", "cc", "triple"}. `resolver` is resolve_target_cc's whole answer
    (cc, machine, rejections), so a leg with no oracle also says WHY — and the
    two whys are DIFFERENT facts that must not be collapsed: "this host owns no
    compiler for this target" is the environment's limit, while "a qualifying
    compiler is right here and the oracle still was not built" is OUR defect and
    names a log to read. A report that printed one sentence for both would send
    the reader to install a toolchain they already have."""
    label = leg.get("label", "<unlabelled>")
    spec = leg.get("spec", "<undeclared>")
    lines = []
    if leg_oracle and leg_oracle.get("path"):
        lines.append("%s: SAME-PLATFORM — %s" % (label, leg_oracle["path"]))
        lines.append("    built for this leg's own target %s by %s (%s), from "
                     "this leg's OWN manifest — the same sources, includes and "
                     "defines dss compiled. Run it on the failing input to "
                     "ATTRIBUTE: it fails too => upstream; it passes => dss."
                     % (spec, leg_oracle.get("cc", "<unnamed cc>"),
                        leg_oracle.get("triple", "<unmeasured triple>")))
        return lines
    cls, why = oracle_class_for_leg(leg, ref_target, ref_path,
                                    oracle_status)
    if cls == "same-platform":
        lines.append("%s: SAME-PLATFORM (the run reference) — %s"
                     % (label, ref_path))
        lines.append("    %s. Run it on the failing input to ATTRIBUTE." % why)
        return lines
    # ── the two states that must NEVER read as an available control ──────────
    lines.append("%s: NO ORACLE — %s" % (label, ORACLE_CLASSES[cls]))
    lines.append("    %s." % why)
    lines.append("    A %s-only failure CANNOT be attributed by this harness's "
                 "documented method (run the reference on the same input), "
                 "because there is nothing of this platform to run."
                 % label)
    if resolver is not None:
        cc, machine, rejections = resolver
        if cc:
            lines.append(
                "    ⚠ AND THAT IS THIS HARNESS'S GAP, NOT THIS HOST'S: `%s` "
                "(%s) is on PATH and PROVES it targets %s, so a same-platform "
                "oracle COULD have been built from this leg's own manifest and "
                "was not. Read the leg's reference-oracle log."
                % (cc, machine, spec))
        else:
            lines.append(
                "    No declared compiler on this host targets %s, so none can "
                "be built here: %s"
                % (spec, "; ".join(rejections) or "no candidates declared"))
    lines.append("    " + ORACLE_FALLBACK_CONTROL)
    return lines


# A binary's FILE NAME is a target fact, exactly as `loadExtHelperName` is — the
# reason that key is declared per leg rather than spelled in a driver. Kept as a
# target-keyed table here (rather than a sixth legs.json key to keep in step)
# because unlike the loadext helper this name is never read by upstream's test
# suite: it is ours, and only the suffix is the target's business.
REFERENCE_ORACLE_NAME_BY_TARGET_OS = {
    "windows": "reference-testfixture.exe",
    "linux": "reference-testfixture",
    "darwin": "reference-testfixture",
}


def reference_oracle_name(leg):
    """This leg's same-platform oracle's file name, keyed on its TARGET's OS —
    never on the host's. Raises rather than guessing: a leg whose spec names an
    OS this table does not know would otherwise get a Windows binary with no
    suffix, which is a file Windows will not exec."""
    os_name = spec_target_os(leg.get("spec", ""))
    if os_name not in REFERENCE_ORACLE_NAME_BY_TARGET_OS:
        raise LegError(
            "leg '%s' targets OS %r, which REFERENCE_ORACLE_NAME_BY_TARGET_OS "
            "does not name (known: %s). The oracle's file name is a TARGET "
            "fact and this catalogue will not guess one."
            % (leg.get("label"), os_name,
               ", ".join(sorted(REFERENCE_ORACLE_NAME_BY_TARGET_OS))))
    return REFERENCE_ORACLE_NAME_BY_TARGET_OS[os_name]


def reference_oracle_argv(cc, manifest, output, link_flags):
    """The one command that builds a leg's same-platform reference, composed
    from the leg's OWN manifest. PURE — returns argv, spawns nothing, so the
    composition is unit-testable without a compiler on the host.

    `link_flags` is the leg's DECLARED `build.referenceLinkFlags`: the TARGET's
    system libraries (`-lm`, `-ldl`, `-lpthread` on a POSIX target; nothing on a
    Windows one, where mingw links them itself). It is a property of the target,
    declared per leg, never sniffed from the host."""
    argv = [cc, "-o", output]
    for d in manifest.get("defines", []):
        argv.append("-D%s" % d)
    for inc in manifest.get("includes", []):
        argv.append("-I%s" % inc)
    argv.extend(manifest.get("sources", []))
    # A resolveLibraries entry is either a bare path or {"path", "importName"}.
    # The reference links against the SAME binaries dss resolved against, which
    # is what makes it a control rather than a differently-configured build.
    for lib in manifest.get("resolveLibraries", []):
        argv.append(lib["path"] if isinstance(lib, dict) else lib)
    argv.extend(link_flags or [])
    return argv


# ── PER-TU BUILD ATTRIBUTION, DRIVEN BY THE LEG'S OWN ORACLE ────────────────
#
# ANCHOR, ONE LINE, DO NOT WRAP: D-HARNESS-BUILD-FAILURE-HAS-NO-PER-TU-ATTRIBUTION
#
# ★★★ THE GAP. `--build-reference-oracle` already compiles this leg's OWN
# manifest with this leg's OWN same-platform compiler — the control is BUILT and
# its diagnostics are already on disk. But a control that FAILED collapsed to
# `absent` in oracle_class_for_leg, so a leg whose reference agreed with dss
# printed `NO ORACLE — no reference binary was produced or preserved`. That reads
# as *the control is missing*, when what actually happened is *the control ran and
# agreed with us*. Those are opposite facts and the report gave them one sentence.
# The consequence is the one this whole file exists to prevent: a real dss
# regression and an upstream defect are INDISTINGUISHABLE in the leg verdict.
#
# ★★ AND THE FIX IS NOT A SECOND LEDGER. The `confounds` ledger already answers
# "is this failure the compiler's?" for two name spaces (a unit's test NAME, an
# abort's `permutation/file`), and its own header says why there must not be a
# third list: "What differs is only the NAME the failure arrives under." A build
# failure arrives under the name of a TRANSLATION UNIT. So this is a third
# `matches` kind on the SAME rows, earned by the SAME mandatory provenance and
# checked by the SAME lint — not a parallel mechanism.
#
# ★★★ WHAT MAKES A `build-tu` ROW DIFFERENT FROM EVERY OTHER CONFOUND ROW, AND
# IT IS THE POINT: A ROW ALONE EXCUSES NOTHING. Every other row is a declaration
# that a human earned once and the harness then trusts. A `build-tu` row is a
# declaration that THIS RUN MUST RE-EARN, because the control is rebuilt every
# run from the same manifest dss consumes. The row says WHICH TU; the run's own
# reference compiler says WHETHER. A row whose TU the reference compiled CLEANLY
# is reported as UNCORROBORATED and excuses nothing — which is also how a stale
# row (upstream fixed it) announces itself instead of rotting into furniture.
# This is why `upstreamSubjects: []` is legal here while `abortDiagnostic: ""` is
# not: the abort row's identity constraint has to be written down because nothing
# measures it, and this row's identity constraint IS a measurement.
#
# ⚠ WHAT THIS COMPARISON CAN AND CANNOT DISTINGUISH — stated here because a
# reader deciding whether to trust an amnesty needs it before the code:
#   CAN   — a TU the reference REJECTS from a TU the reference ACCEPTS. That is
#           the whole verdict, and it is a measured rc + located diagnostics.
#   CAN   — a dss error naming an identifier the reference NEVER named anywhere
#           in that TU. That is the residue, and it is what keeps a dss
#           regression visible inside an upstream-broken TU.
#   CANNOT— align two compilers' ERROR RECOVERY. ✔MEASURED 2026-08-18 on this
#           leg: gcc abandoned `fileTimeToUnixTime` at its parameter list
#           (fileio.c:296) and never reached `ULARGE_INTEGER` at :299/:301; dss
#           recovered and named it. Same root cause, one extra name. A residue
#           entry is therefore a QUESTION, not a conviction, and clearing it
#           costs a declared `upstreamSubjects` entry carrying its measurement.
#   CANNOT— attribute a dss diagnostic that names no identifier at all (a
#           cascade message such as "arrow operator '->' pointee is not a
#           composite type" — 44 of the 105 on this leg). There is nothing to
#           compare, so they are COUNTED AND PRINTED as unattributable-by-name
#           and they neither grant nor deny amnesty. A dss regression that
#           manifests ONLY as extra cascade messages inside an already-broken TU
#           is the blind spot, and it is exactly this size.
BUILD_ATTRIBUTIONS = {
    "upstream": "the leg's OWN same-platform reference compiler, given this "
                "leg's OWN manifest, ALSO rejected this TU, an earned "
                "`matches: build-tu` row names it, and every identifier dss "
                "named the reference named too",
    "dss": "dss rejected this TU and the reference did not — or did, and dss "
           "named something the reference never did. Charged to dss.",
    "unattributable": "no control was available for this TU (no oracle was "
                      "attempted, or the TU is not in the manifest the oracle "
                      "compiled), so nothing about it has been measured",
}

# A dss diagnostic head: `error[S0006]: [target=<spec>] <subject>`. The target
# tag is OPTIONAL in this pattern on purpose — it is emitted by the project
# driver and not by every front end, and a parser that REQUIRED it would silently
# find zero diagnostics in a log shape that is otherwise perfectly readable.
# Finding zero diagnostics is the one outcome this whole mechanism must never
# reach quietly, because "dss said nothing" and "I could not read what dss said"
# produce the same empty set and opposite conclusions.
DSS_DIAGNOSTIC_HEAD = re.compile(
    r"^(error|warning|info)\[([A-Za-z0-9_]+)\]:\s*(?:\[target=[^\]]*\]\s*)?(.*)$")
# `  --> <path>:<line>:<col>`. GREEDY on the path because a Windows path carries
# its own colon (`C:/...`) — the trailing two numeric groups are what anchor it.
DSS_DIAGNOSTIC_LOC = re.compile(r"^\s*-->\s*(.+):(\d+):(\d+)\s*$")

# The GNU/clang diagnostic line, in both the with-column and without-column
# spellings. `note:` is captured too — not to attribute anything, but so that a
# log FULL of notes and nothing else cannot be mistaken for an unreadable log.
REFERENCE_DIAGNOSTIC = re.compile(
    r"^(.+?):(\d+):(?:(\d+):)?\s*(error|warning|note):\s*(.*)$")
# gcc and clang both append a SPELLING SUGGESTION to a diagnostic. A suggestion
# names something the compiler is NOT complaining about, and this comparison asks
# "did the reference NAME this identifier here?" — so a suggestion left in would
# corroborate a dss error about a name the reference merely proposed. Stripped
# before extraction, in the direction that makes amnesty HARDER.
REFERENCE_SUGGESTION = re.compile(r"[;,]?\s*did you mean\s*'[^']*'\s*\??")
# What a diagnostic NAMES: the single-quoted tokens, split into C identifiers.
# Both compilers quote the subject; neither prints a bare identifier that is not
# quoted somewhere in the same message.
_QUOTED = re.compile(r"'([^']*)'")
_IDENTIFIER = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
# A dss subject is an IDENTIFIER when it is exactly one, or a HEADER NAME when it
# looks like one (`unistd.h` — the subject of a `file not found`). Anything else
# is prose and is unattributable BY NAME; see the CANNOT list above.
_BARE_IDENTIFIER = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")
_HEADER_NAME = re.compile(r"^[A-Za-z0-9_./+-]+\.(?:h|hh|hpp|inc)$")


def normalise_tu_path(path):
    """One spelling for one file. Backslashes to forward slashes and no case
    folding: the two logs are produced by two different compilers on the same
    host, and only the separator has ever differed between them. Case folding
    would silently merge two real files on a case-sensitive target."""
    return (path or "").strip().replace("\\", "/")


def dss_build_diagnostics(log_text):
    """Every dss diagnostic in a build log, as
    [{severity, code, subject, file, line, col}] in log order.

    PURE — takes text, returns rows — so the parser is unit-testable without a
    compiler, which is the same argument reference_oracle_argv makes for
    composing argv rather than spawning."""
    lines = (log_text or "").splitlines()
    out = []
    for i, line in enumerate(lines):
        m = DSS_DIAGNOSTIC_HEAD.match(line)
        if not m:
            continue
        loc = DSS_DIAGNOSTIC_LOC.match(lines[i + 1]) if i + 1 < len(lines) else None
        out.append({
            "severity": m.group(1), "code": m.group(2),
            "subject": m.group(3).strip(),
            "file": normalise_tu_path(loc.group(1)) if loc else "",
            "line": int(loc.group(2)) if loc else 0,
            "col": int(loc.group(3)) if loc else 0,
        })
    return out


def dss_subject_identifier(subject):
    """The NAME a dss diagnostic is about, or "" when it names none.

    "" is a real answer and it means *this diagnostic cannot be compared*, never
    *this diagnostic is fine*. Every caller here treats it as its own class."""
    s = (subject or "").strip()
    if _BARE_IDENTIFIER.match(s) or _HEADER_NAME.match(s):
        return s
    return ""


def reference_build_diagnostics(log_text):
    """Every GNU/clang diagnostic in a reference log, as
    [{file, line, col, severity, message, names}] in log order.

    `names` is every identifier the message QUOTES, with any spelling suggestion
    removed first. It is deliberately gathered from warnings as well as errors:
    C23 makes an implicit function declaration an ERROR, and the reference here
    is a gnu17-default gcc that reports the identical construct as a WARNING —
    ✔MEASURED 2026-08-18 on this leg, 16 such names in one TU. Reading only the
    reference's errors would charge every one of them to dss.
    ⚠ THE COST, and it is real: a name the reference merely WARNED about
    corroborates a dss ERROR about that name. A construct the reference accepts
    with a warning and dss rejects outright is therefore invisible to the residue
    check. That is a deliberate trade against the 16 false accusations, and it is
    why `severity` is kept on every row: a caller that wants the strict answer
    can filter, and the report PRINTS which severity corroborated each name."""
    out = []
    for line in (log_text or "").splitlines():
        m = REFERENCE_DIAGNOSTIC.match(line)
        if not m:
            continue
        message = m.group(5)
        names = []
        for quoted in _QUOTED.findall(REFERENCE_SUGGESTION.sub("", message)):
            for tok in _IDENTIFIER.findall(quoted):
                if tok not in names:
                    names.append(tok)
        out.append({
            "file": normalise_tu_path(m.group(1)),
            "line": int(m.group(2)), "col": int(m.group(3) or 0),
            "severity": m.group(4), "message": message.strip(), "names": names,
        })
    return out


def build_tu_row_findings(label, row):
    """Everything WRONG with one `matches: build-tu` row, as lint findings.

    A FUNCTION rather than a block inside lint() so the self-test can drive the
    real rule on a real row instead of re-typing its conditions — the same
    argument classify_abort_decisions makes for being callable without a
    catalogue. [D-HARNESS-BUILD-FAILURE-HAS-NO-PER-TU-ATTRIBUTION]"""
    pat = row.get("pattern")
    findings = []
    # ★ REQUIRED AS A KEY, and `[]` is the ordinary answer — the same
    # missing-is-not-empty discipline `requires` carries, and for the identical
    # reason: a missing key cannot be told from an empty one, and the difference
    # is whether a name the reference NEVER mentioned is being waved through.
    subs = row.get("upstreamSubjects")
    if subs is None:
        findings.append(
            "leg '%s': confound %r declares matches='build-tu' but no "
            "'upstreamSubjects'. It is REQUIRED and `[]` is the ordinary answer "
            "— 'the reference names everything dss names in this TU'. A "
            "non-empty list is the RESIDUE this row waves through, and every "
            "entry has to be justified in `mechanism`: it is a name the leg's "
            "own reference compiler never mentioned." % (label, pat))
    elif (not isinstance(subs, list)
          or not all(isinstance(s, str) and s.strip() for s in subs)):
        findings.append(
            "leg '%s': confound %r declares `upstreamSubjects` %r — it must be "
            "a list of non-empty identifier strings" % (label, pat, subs))
    if not str(pat).endswith("$"):
        # A TU pattern is matched with re.search against a FULL path, so an
        # unanchored tail matches every longer sibling: `fileio\.c` also matches
        # `fileio_extra.c`. An over-wide attribution row is silent by
        # construction — it grants amnesty to a file nobody measured.
        findings.append(
            "leg '%s': confound %r declares matches='build-tu' but its pattern "
            "is not anchored at the end ('$'). TU patterns are searched against "
            "a full path, so an unanchored tail silently attributes every "
            "longer sibling path as well." % (label, pat))
    return findings


def build_tu_rows(decisions):
    """The ACTIVE `matches: build-tu` rows out of a leg's confound decisions —
    the same accessor shape as leg_confounds / leg_abort_confounds, and for the
    same reason: a matcher written for one name space must never be handed
    another's rows."""
    return [d for d in decisions
            if d["active"] and d["matches"] == "build-tu" and not d["scopedOut"]]


def attribute_build_failure(dss_log_text, reference_log_text, oracle_status,
                            manifest_sources, decisions, label="<unlabelled>"):
    """WHOSE failure is this build? One answer per TU dss rejected.

    `oracle_status` is `--build-reference-oracle`'s own reported status verbatim
    (`built` / `build-failed` / `no-reference-compiler` / "" when it was never
    called). ONLY `built` and `build-failed` mean the control RAN; every other
    value grants nothing to anything, which is the whole discipline — a missing
    control is not a silent control.

    `manifest_sources` is the manifest's `sources` list — the SAME file dss
    consumed and the oracle compiled. A TU dss names that is not in it was never
    put to the reference, so it cannot be attributed however the logs read.

    Returns a report dict. Never raises for a LOG it cannot read: an unreadable
    log is a REPORTED finding (`parserGap`), because raising would take the whole
    leg's account down with it and the harness must survive everything."""
    attempted = oracle_status in ORACLE_STATUSES_ATTEMPTED
    sources = {normalise_tu_path(s) for s in (manifest_sources or [])}
    dss_rows = [r for r in dss_build_diagnostics(dss_log_text)
                if r["severity"] == "error"]
    ref_rows = reference_build_diagnostics(reference_log_text)

    # ★ A LOG THAT PARSED TO NOTHING IS A FINDING, NOT AN ANSWER. The reference
    # is whichever compiler the leg DECLARED and this host verified; a leg that
    # one day declares an MSVC-shaped `targetCc` would produce a log this reader
    # finds zero diagnostics in — and zero reference diagnostics silently denies
    # every amnesty AND hides that the reader is the reason. It fails in the safe
    # direction and it still has to say so out loud.
    parser_gap = ""
    if oracle_status == ORACLE_STATUS_BUILD_FAILED and not ref_rows:
        parser_gap = (
            "the reference build FAILED and this harness parsed ZERO diagnostics "
            "out of its log. Nothing can be attributed and the shape of that log "
            "is not one REFERENCE_DIAGNOSTIC reads — read it by hand and widen "
            "the reader; every TU below is charged to dss meanwhile")

    ref_by_tu = {}
    for r in ref_rows:
        bucket = ref_by_tu.setdefault(r["file"], {"error": 0, "warning": 0,
                                                  "names": {}, "firstError": ""})
        if r["severity"] in ("error", "warning"):
            bucket[r["severity"]] += 1
            for n in r["names"]:
                bucket["names"].setdefault(n, r["severity"])
        if r["severity"] == "error" and not bucket["firstError"]:
            bucket["firstError"] = "%s:%d:%d: error: %s" % (
                r["file"], r["line"], r["col"], r["message"])

    tus = []
    for tu in sorted({r["file"] for r in dss_rows if r["file"]}):
        mine = [r for r in dss_rows if r["file"] == tu]
        named, cascade = [], 0
        for r in mine:
            ident = dss_subject_identifier(r["subject"])
            if ident:
                if ident not in named:
                    named.append(ident)
            else:
                cascade += 1
        ref = ref_by_tu.get(tu, {"error": 0, "warning": 0, "names": {},
                                 "firstError": ""})
        row = next((d for d in build_tu_rows(decisions)
                    if re.search(d["pattern"], tu)), None)
        allowed = list(row["row"].get("upstreamSubjects", [])) if row else []
        residue = [n for n in named
                   if n not in ref["names"] and n not in allowed]
        excused = [n for n in named if n not in ref["names"] and n in allowed]
        # ⚠ DEFENDED RATHER THAN ASSUMED, and red-on-disable is what asked for it.
        # ✔MEASURED 2026-08-18: when the `row is None` arm below was deleted to
        # prove that pin bites, the two arms after it dereferenced `row["pattern"]`
        # and the answer was `TypeError: 'NoneType' object is not subscriptable` —
        # a python message about a python rule, from the one function whose whole
        # job is to say whose defect this is. A None deref must never be a
        # diagnostic, so the name is resolved ONCE, here, and reads `<no row>`.
        row_pattern = row["pattern"] if row else "<no row>"

        if not attempted or tu not in sources:
            verdict, why = "unattributable", (
                "no oracle was attempted for this leg (status %r)" % oracle_status
                if not attempted else
                "this TU is not among the %d sources the oracle compiled, so the "
                "reference was never asked about it" % len(sources))
        elif ref["error"] < 1:
            verdict, why = "dss", (
                "the reference compiled this TU with %d error(s) and %d "
                "warning(s) — it ACCEPTED what dss rejected"
                % (ref["error"], ref["warning"]))
        elif row is None:
            verdict, why = "dss", (
                "the reference ALSO rejected this TU (%d error(s)), but NO "
                "`matches: build-tu` row on leg '%s' names it. An unearned "
                "attribution is not an attribution: write the row, with its "
                "measurement, and this run will corroborate it. First reference "
                "error: %s" % (ref["error"], label, ref["firstError"]))
        elif residue:
            verdict, why = "dss", (
                "the reference ALSO rejected this TU (%d error(s)) and row %r "
                "names it, but dss named %d identifier(s) the reference named "
                "NOWHERE in this TU: %s. Each is either a dss defect hiding "
                "inside an upstream-broken TU, or an error-recovery difference "
                "that must be declared in that row's `upstreamSubjects` with its "
                "measurement." % (ref["error"], row_pattern, len(residue),
                                  ", ".join(residue)))
        else:
            verdict, why = "upstream", (
                "the reference REJECTED this TU too — %d error(s), %d "
                "warning(s), first: %s — row %r names it, and every one of the "
                "%d identifier(s) dss named was named by the reference%s"
                % (ref["error"], ref["warning"], ref["firstError"],
                   row_pattern, len(named),
                   "" if not excused else
                   " or is a declared recovery difference (%s)"
                   % ", ".join(excused)))
        tus.append({
            "tu": tu, "attribution": verdict, "why": why,
            "dssErrors": len(mine), "dssNamed": named,
            "dssCascade": cascade,
            "referenceErrors": ref["error"], "referenceWarnings": ref["warning"],
            "referenceFirstError": ref["firstError"],
            "corroborated": [n for n in named if n in ref["names"]],
            "corroboratedBy": {n: ref["names"][n] for n in named
                               if n in ref["names"]},
            "excusedByDeclaration": excused,
            "residue": residue,
            "row": row["pattern"] if row else "",
        })

    charged = [t["tu"] for t in tus if t["attribution"] != "upstream"]
    return {
        "leg": label, "oracleStatus": oracle_status,
        "oracleAttempted": attempted, "parserGap": parser_gap,
        "dssErrors": len(dss_rows),
        "dssErrorsWithNoLocation": len([r for r in dss_rows if not r["file"]]),
        "referenceErrors": len([r for r in ref_rows if r["severity"] == "error"]),
        "tus": tus, "chargedToDss": charged,
        "verdictClass": "dss" if (charged or parser_gap) else "upstream",
    }


def build_attribution_report_lines(report):
    """The lines a driver PRINTS verbatim — one implementation, both drivers, the
    same argument oracle_report_lines and confound_report_lines make.

    ★ EVERY TU APPEARS, whoever it is charged to. An upstream-attributed TU that
    vanished from this list would be indistinguishable from a TU that compiled,
    and "the build failed and here is why it is not ours" is a claim that has to
    be readable, not merely acted on. Silence is the failure mode."""
    label = report.get("leg", "<unlabelled>")
    lines = []
    if not report.get("tus"):
        lines.append("[%s] build attribution: NO dss error carries a source "
                     "location — nothing to attribute" % label)
        return lines
    up = [t for t in report["tus"] if t["attribution"] == "upstream"]
    lines.append(
        "[%s] build attribution: %d TU(s) rejected by dss — %d UPSTREAM "
        "(the leg's own same-platform reference rejects them too), %d charged "
        "to DSS. Oracle status: %s."
        % (label, len(report["tus"]), len(up),
           len(report["tus"]) - len(up), report["oracleStatus"] or "<not run>"))
    if report.get("parserGap"):
        lines.append("[%s]   HARNESS GAP: %s" % (label, report["parserGap"]))
    for t in report["tus"]:
        lines.append("[%s]   %s  %s  (%d dss error(s), %d of them naming no "
                     "identifier)"
                     % (label, t["attribution"].upper(), t["tu"],
                        t["dssErrors"], t["dssCascade"]))
        lines.append("[%s]     %s" % (label, t["why"]))
        if t["dssCascade"]:
            # NAMED EVERY TIME, including on an UPSTREAM TU — this is the blind
            # spot stated in the header, and a blind spot nobody is reminded of
            # is a blind spot that grows.
            lines.append(
                "[%s]     %d dss error(s) in this TU name no identifier and are "
                "NOT individually attributable by this comparison; they neither "
                "granted nor denied the verdict above."
                % (label, t["dssCascade"]))
    return lines


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


# ── WHO BUILDS THE HELPER: DSS ITSELF, ON ANY HOST ──────────────────────────
#
# D-HARNESS-CROSS-HOST-ANY-TARGET. Operator, 2026-08-05: "why do we need mingw?
# since dss code prime should not have dependencies?" and "I installed mingw in
# wsl, but we should NOT depend on a tool".
#
# ★ THE DEFECT THE PREVIOUS CYCLE LEFT BEHIND, and it is a DIFFERENT one from the
# bug it fixed. `resolve_target_cc` above is correct as far as it goes — a
# compiler's NAME is not a declaration of its target, and asking `-dumpmachine`
# is the right way to find out. But the shape it fixed the bug INTO made every
# leg's RUN conditional on this host owning a THIRD-PARTY CROSS-COMPILER for that
# leg's target: a mingw `gcc` for pe64, `aarch64-linux-gnu-gcc` for the arm64
# leg, an Apple `clang` for the two macho legs. A leg with no such compiler
# recorded `skipped-build-input-missing` and its ~330,000 units did not run. That
# is host-dependence in its plainest form, and it contradicts the project's hard
# requirement that ANY target build inside ANY host.
#
# ⇒ THE COMPILER THIS REPOSITORY SHIPS BUILDS IT. ✔MEASURED 2026-08-05 on this
# project's WINDOWS box — ONE host, all five legs, `--compile
# <sqlite>/src/test_loadext.c` at the leg's own declared `sharedLibFormat`, rc=0
# and zero `error[`/`error:` on every one:
#
#     pe64-x86_64-windows-dll      ->  test_loadext.dll      8,704 bytes
#     elf64-x86_64-linux-dyn       ->  test_loadext.so      13,912 bytes
#     elf64-aarch64-linux-dyn      ->  test_loadext.so      13,912 bytes
#     macho64-arm64-darwin-dylib   ->  test_loadext.dylib   67,426 bytes
#     macho64-x86_64-darwin-dylib  ->  test_loadext.dylib   17,890 bytes
#
# and — the part that makes this a result rather than a claim — the three a
# machine here can LOAD were loaded THROUGH SQLITE'S OWN LOADER, which is what
# test/loadext.test does: `sqlite3_enable_load_extension` +
# `sqlite3_load_extension(db, <artefact>, "testloadext_init", &err)` +
# `SELECT half(9.0)` returned 4.500 for the pe64 DLL natively on Windows, for the
# elf64-x86_64 .so inside WSL, and for the elf64-aarch64 .so under
# `qemu-aarch64`. The host program in each case was built by the REFERENCE
# compiler against upstream's own `sqlite3.c`, so the ONLY thing under test was
# the DSS artefact. The two macho legs emit cleanly and cannot be loaded off a
# Mac — the same structural fact their `launchers` already declare, not a new gap.
#
# ⚠ WHY test_loadext.c IS BUILDABLE AT ALL WITHOUT LINKING sqlite: it includes
# `sqlite3ext.h` and uses SQLITE_EXTENSION_INIT1, so every `sqlite3_*` call is a
# MACRO through the `sqlite3_api` function-pointer table the loader passes in.
# ✔MEASURED from the emitted ELF's `.dynsym`: the only undefined symbols are
# `<string.h>` names against DT_NEEDED `libc.so.6`, and the exports are exactly
# `testloadext_init`, `testbrokenext_init`, `sqlite3_api`. There is no sqlite
# library to resolve against and therefore no per-leg library input to find.
#
# ──────────────────────────────────────────────────────────────────────────────
# ★★ THE DESIGN QUESTION, ANSWERED RATHER THAN ASSUMED: DSS IS NOW TESTING DSS.
#
# The fixture is DSS-built and now the extension it dlopen()s is DSS-built too.
# Two distinct hazards, and they are NOT the same size:
#
#   (a) FALSE RED — DSS's shared-library emission is broken, so the ~16 loadext-*
#       units fail for a reason that is not what the corpus is testing. This is
#       real but benign in kind: it is still a GENUINE DSS defect, merely
#       mis-attributed. The cure is attribution, and the report below names the
#       builder of the staged artefact so a reader never has to guess.
#   (b) FALSE GREEN — a defect SHARED by the fixture and the helper cancels out.
#       Both compile the same `sqlite3ext.h`, so a wrong `sqlite3_api_routines`
#       layout would agree with itself and `half()` would work. This is the
#       dangerous one, and no amount of inspecting the artefact catches it.
#
# The reference-compiler build was, implicitly, the control against (b). But it
# was never a MATCHED control: it was the only arm that ever ran, and an arm that
# is never compared proves nothing. Making it MANDATORY is what produced the host
# dependence; deleting it would drop the only instrument that can answer "was
# that red the fixture or the helper?".
#
# ⇒ WHAT IS IMPLEMENTED. DSS is the PRIMARY and the DEFAULT — always available,
# on every host, so no leg's coverage depends on what a machine happens to carry.
# The reference compiler stays as an OPTIONAL CONTROL with two jobs:
#
#   1. PASSIVE, whenever a VERIFIED target compiler is present (the
#      `-dumpmachine` check above still decides that, unchanged): the same source
#      is ALSO built with it, into a sibling directory, and both artefacts are
#      reported side by side. It never gates and it never stages. Cost is one
#      sub-second compile; what it buys is that the control artefact is ON DISK
#      beside the primary when a loadext-* red has to be triaged.
#   2. ACTIVE, when the operator asks for it (`DSS_LOADEXT_HELPER=reference`):
#      the control artefact is the one STAGED, so the corpus itself becomes the
#      differential and hazard (b) is answerable by RUNNING it, not by argument.
#      That is the only form of a matched control that can actually detect a
#      cancelling pair, and it is one environment variable away at all times.
#
# ⇒ WHAT IS DELIBERATELY NOT IMPLEMENTED: an AUTOMATIC fallback to the reference
# compiler when the DSS build fails. A DSS emission failure is a compiler defect
# and this harness exists to surface compiler defects; quietly building the
# artefact another way would hide exactly the finding. A failed primary build is
# `poisoned`, with the compiler's own diagnostics quoted. The operator switch is
# the explicit, visible way to take the other path.
#
# ⇒ AND WHAT REPLACES THE INSPECTION THE CONTROL CANNOT DO. "It compiled" is not
# "it is a loadable shared library": an empty file, or an `-exec` image emitted
# under a `.dll` name, would satisfy a returncode and fail every loadext-* unit
# hours later. So the staged artefact's OWN HEADER is read (see
# `binary_shared_lib_shape`) and must say both (i) the container this leg's
# declared sharedLibFormat names and (ii) SHARED LIBRARY — ET_DYN / MH_DYLIB /
# IMAGE_FILE_DLL. Host-free, no external tool, and it is the one check that would
# have caught the failure mode the reference build was silently standing in for.

# The `--language` the helper is compiled as. The SAME name the fixture's own
# manifest generator writes (gen-pe64-manifest.py, `"language": "c-subset"`), so
# the helper and the fixture cannot come to be parsed by two different front ends.
LOADEXT_HELPER_LANGUAGE = "c-subset"

# The file `--compile` is pointed at, relative to the staged sqlite tree's `src/`.
# Named here rather than in two drivers for the same reason the helper's OUTPUT
# name is declared per leg: one spelling, one place to change it.
LOADEXT_HELPER_SOURCE = "test_loadext.c"

# WHO may build it. Closed, because an unrecognised value must not silently mean
# "the default" — the whole point of the switch is to make the differential
# runnable, and a typo that quietly staged the primary would report a control
# that never happened.
LOADEXT_HELPER_BUILDERS = ("dss", "reference")
LOADEXT_HELPER_BUILDER_ENV = "DSS_LOADEXT_HELPER"

# The SHARED-LIBRARY kind each object-format CONTAINER spells, so a leg's declared
# `sharedLibFormat` can be CHECKED rather than trusted — the same declare-then-
# cross-check discipline as POSIX_ONLY_ZCONF_GUARDS and
# LOADEXT_HELPER_NAME_BY_TARGET_OS above.
# ✔MEASURED 2026-08-05 from the shipped `src/dss-config/object-formats/`: each
# container ships exactly one shared-library kind and it is spelled differently
# in each — `elf64-{x86_64,aarch64}-linux-dyn`, `pe64-x86_64-windows-dll`,
# `macho64-{x86_64,arm64}-darwin-dylib`. Keyed on the CONTAINER, never on the
# host and never on the arch: `elf64-aarch64-linux-dyn` and
# `elf64-x86_64-linux-dyn` differ only in the arch token the spec already carries.
SHARED_LIB_KIND_BY_CONTAINER = {
    "elf64": "dyn",
    "pe64": "dll",
    "macho64": "dylib",
}


def spec_format_container(spec):
    """'x86_64:pe64-x86_64-windows-exec' -> 'pe64'. The container is the FIRST
    token of the format name, the same decomposition `spec_target_os` uses from
    the other end."""
    parts = spec_format(spec).split("-")
    return parts[0] if parts and parts[0] else ""


def derived_shared_lib_format(spec):
    """The shared-library format name DERIVABLE from a leg's exec spec, or "".

    Format names are `<container><bits>-<arch>-<os>-<kind>`, so the shared-library
    sibling of a leg's own format is that name with its KIND token replaced by the
    container's shared-library kind. Pure, and used ONLY by the lint — the value a
    driver passes to `--target` is the leg's DECLARATION, never this."""
    fmt = spec_format(spec)
    parts = fmt.split("-")
    if len(parts) < 4:
        return ""
    kind = SHARED_LIB_KIND_BY_CONTAINER.get(spec_format_container(spec))
    if not kind:
        return ""
    return "-".join(parts[:-1] + [kind])


def shared_lib_format(leg):
    """The declared object format the loadext helper is emitted in, verbatim.

    No default and no derivation, for the reason the whole catalogue is written
    this way: the exact string handed to `--target` is the thing that has cost
    this project failed invocations, so a reader of legs.json must be able to SEE
    it. The lint cross-checks it against `derived_shared_lib_format`."""
    return str(leg.get("build", {}).get("sharedLibFormat", ""))


def shared_lib_spec(leg):
    """The combined `<arch>:<format>` argument. ★ ONE ARG, NOT TWO — there is no
    separate `--object-format` flag — and the arch and the format spell arm64
    DIFFERENTLY (`arm64:elf64-aarch64-linux-dyn`). Spelled HERE so neither driver
    ever assembles it, which is where that trap has bitten before."""
    return "%s:%s" % (spec_target_arch(leg.get("spec", "")), shared_lib_format(leg))


def loadext_helper_builder(raw, env=None):
    """Which builder STAGES the helper: the operator's choice, or 'dss'.

    An unrecognised value RAISES. Defaulting a typo'd `DSS_LOADEXT_HELPER` back to
    'dss' would report a control run that never happened, which is worse than no
    control at all."""
    value = (raw if raw is not None
             else (env if env is not None else os.environ).get(
                 LOADEXT_HELPER_BUILDER_ENV, ""))
    value = (value or "").strip().lower()
    if not value:
        return LOADEXT_HELPER_BUILDERS[0]
    if value not in LOADEXT_HELPER_BUILDERS:
        raise LegError(
            "unknown loadext helper builder %r (known: %s). This selects WHICH "
            "artefact is staged for sqlite's test/loadext.test to dlopen(): "
            "'dss' (the default — the compiler this repository ships, available "
            "on every host) or 'reference' (the leg's VERIFIED target C "
            "compiler, the control arm, which exists only where such a compiler "
            "is installed). A value nothing implements must not be read as the "
            "default: it would report a control that never ran."
            % (value, ", ".join(LOADEXT_HELPER_BUILDERS)))
    return value


def loadext_helper_dss_argv(leg, dss, source, include_dirs, outdir, config):
    """The EXACT argv that makes DSS emit this leg's helper. Pure.

    ★ `--output` IS A DIRECTORY, not a file — DSS writes `<outdir>/<stem><ext>`
    and NAMES it itself (✔MEASURED 2026-08-05: `test_loadext.dll` / `.so` /
    `.dylib`, i.e. the SOURCE stem, never the leg's declared helper name). The
    caller reads the path the build REPORTED and copies it to the declared name;
    it never predicts either. Pointing `--output` at a file path is how a
    one-byte 'artefact' turns out to be a directory being stat-ed.
    ★ `--include-dir`, not `-I`. ★ `--config=<v>`, joined, matching the shape
    base-harness.sh's `dss_bh_build_artifact` already uses."""
    argv = [dss, "--compile", source,
            "--language", LOADEXT_HELPER_LANGUAGE,
            "--target", shared_lib_spec(leg)]
    for d in include_dirs:
        argv += ["--include-dir", d]
    argv += ["--output", outdir, "--config=%s" % config]
    return argv


def loadext_helper_reference_argv(leg, cc, source, include_dirs, out_path):
    """The control arm's argv — the leg's own declared `sharedLibFlags` plus the
    same two include roots. Pure. Byte-for-byte the invocation build-and-test.sh's
    `stage_loadext_extension` has always made, kept identical on purpose: a
    control whose command changed at the same time as the thing it controls is
    not a control."""
    argv = [cc] + list(leg.get("build", {}).get("sharedLibFlags", []))
    for d in include_dirs:
        argv.append("-I%s" % d)
    argv += ["-o", out_path, source]
    return argv


# ── "it compiled" is NOT "it is a loadable shared library" ───────────────────
#
# The failure mode this rules out, named by the operator: an empty or wrong-KIND
# artefact that nevertheless "succeeds". A zero-byte file, or an executable image
# emitted under a `.dll` name, satisfies a returncode and then fails every
# loadext-* unit hours later with `no such function: half` — which reads exactly
# like a DSS miscompile in the FIXTURE. So the artefact's own header is read.
#
# The three checks are the three containers' own answers to the same question,
# taken from each format's specification (DOCUMENTED: ELF gABI `e_type`;
# PE/COFF `IMAGE_FILE_HEADER.Characteristics`; Mach-O `mach_header_64.filetype`),
# and each was ✔MEASURED against the artefacts listed at the top of this section.
# No external tool is consulted — `file`/`objdump`/`otool` would be exactly the
# third-party host dependence this whole change removes.
MH_MAGIC_64 = 0xFEEDFACF
MH_DYLIB = 0x6
ET_DYN = 3
IMAGE_FILE_DLL = 0x2000


def binary_shared_lib_shape(blob):
    """(container, isSharedLib, detail) read from a binary's OWN header.

    `container` is this catalogue's container vocabulary (`elf64`/`pe64`/
    `macho64`) or "" when the bytes are none of them. Pure — it takes the bytes,
    not a path — so the self-test can assert every arm without producing five
    real binaries on whatever machine it happens to run on."""
    import struct
    if not blob:
        return ("", False, "the file is EMPTY (0 bytes)")
    if blob[:4] == b"\x7fELF":
        if len(blob) < 20:
            return ("", False, "truncated ELF header (%d bytes)" % len(blob))
        if blob[4] != 2:
            return ("", False, "ELF but not 64-bit (EI_CLASS=%d)" % blob[4])
        etype, = struct.unpack("<H", blob[16:18])
        return ("elf64", etype == ET_DYN,
                "ELF64 e_type=%d (%s)"
                % (etype, "ET_DYN — a shared object" if etype == ET_DYN
                   else "NOT ET_DYN — not a shared object"))
    if blob[:2] == b"MZ":
        if len(blob) < 0x40:
            return ("", False, "truncated DOS header (%d bytes)" % len(blob))
        e_lfanew, = struct.unpack("<I", blob[0x3C:0x40])
        if e_lfanew + 24 > len(blob) or blob[e_lfanew:e_lfanew + 4] != b"PE\0\0":
            return ("", False, "an MZ image whose e_lfanew (0x%X) does not point "
                               "at a PE signature" % e_lfanew)
        chars, = struct.unpack("<H", blob[e_lfanew + 22:e_lfanew + 24])
        return ("pe64", bool(chars & IMAGE_FILE_DLL),
                "PE Characteristics=0x%04X (%s)"
                % (chars, "IMAGE_FILE_DLL set" if chars & IMAGE_FILE_DLL
                   else "IMAGE_FILE_DLL NOT set — this is not a DLL"))
    if len(blob) >= 16:
        magic, = struct.unpack("<I", blob[:4])
        if magic == MH_MAGIC_64:
            filetype, = struct.unpack("<I", blob[12:16])
            return ("macho64", filetype == MH_DYLIB,
                    "Mach-O 64 filetype=%d (%s)"
                    % (filetype, "MH_DYLIB" if filetype == MH_DYLIB
                       else "NOT MH_DYLIB"))
    if _fat_slices(blob):
        return ("", False, "a Mach-O FAT/universal archive, not a single-arch "
                           "shared library [D-FF1-MACHO-FAT]")
    return ("", False, "unrecognised: first 4 bytes %r" % (blob[:4],))


# ── WHICH TARGET A BINARY IS FOR, READ OUT OF THE BINARY ────────────────────
#
# THE SIBLING OF `binary_shared_lib_shape`, AND IT ANSWERS THE OTHER HALF OF THE
# SAME QUESTION. That one asks "is this the KIND of image the leg declared"; this
# one asks "is this the TARGET the leg declared", which is what a smoke gate has
# to know before it can attribute a failure to anything: a binary that will not
# run because it is for another machine is not a compiler defect, and the two are
# indistinguishable from an exit code alone.
#
# NO EXTERNAL TOOL. `file`, `objdump -f`, `lipo -info` and `dumpbin /headers`
# each answer this on exactly one host, in a different vocabulary, and one of
# them is the tool that reported success over a file that no longer existed. The
# bytes are here; they are the same bytes on every host.
#
# ★ e_machine / IMAGE_FILE_HEADER.Machine / cputype are DOCUMENTED (ELF gABI;
# PE/COFF §3.3.1; Mach-O <mach/machine.h>) and each was cross-checked against
# this catalogue's own declarations: MACHO_CPU_TYPES already existed for slicing
# universal archives and is INVERTED here rather than re-tabulated.
EM_X86_64 = 0x3E
EM_AARCH64 = 0xB7
ELF_MACHINES = {EM_X86_64: "x86_64", EM_AARCH64: "arm64"}
IMAGE_FILE_MACHINE_AMD64 = 0x8664
IMAGE_FILE_MACHINE_ARM64 = 0xAA64
PE_MACHINES = {IMAGE_FILE_MACHINE_AMD64: "x86_64",
               IMAGE_FILE_MACHINE_ARM64: "arm64"}

# EI_OSABI -> the target OS name this catalogue uses, over THE VALUES THIS
# CATALOGUE'S OWN TARGETS PRODUCE AND NO OTHERS.
#
# ⚠ AND IT IS DELIBERATELY NOT A DEFAULT. ELF's identity does not carry an
# operating system the way PE and Mach-O do — EI_OSABI is nearly always 0, and 0
# means "System V", not "Linux". So the honest reading of an unknown value is a
# REFUSAL, for the same reason `run_filesystem()` refuses an unknown verb: this
# harness builds Linux ELF today, and the day it builds a FreeBSD one (EI_OSABI
# 9) the wrong answer must be a loud one rather than a silent "linux" that sends
# a driver looking for the wrong launcher.
#
# ✔MEASURED (this project's WSL/Ubuntu host): `/bin/ls` and `/usr/bin/qemu-aarch64`
# both carry EI_OSABI=0 (`readelf -h` agrees: "UNIX - System V"), and every
# shipped `src/dss-config/object-formats/elf64-*.format.json` declares
# `"osabi": "sysv"`, which src/link/format/elf_backend.cpp:170 encodes as 0. So 0
# is what BOTH compilers in every matched control emit. 3 (ELFOSABI_GNU /
# ELFOSABI_LINUX) is the other value a GNU toolchain produces — it is stamped on
# an image using GNU IFUNC relocations — and the reference fixture is exactly the
# kind of binary that can carry it, so it is mapped rather than left to refuse a
# control run.
ELF_OSABI_TARGET_OS = {0: "linux", 3: "linux"}


def binary_target_identity(blob):
    """(arch, container, targetOs) read from a binary's OWN header.

    PURE — takes BYTES, not a path — so every arm is asserted on any machine,
    exactly as `binary_shared_lib_shape` is. Raises LegError on bytes it cannot
    read: an unidentifiable binary must never be reported as some default
    target, because the only consumer is a gate deciding whether a failure to
    run is attributable to this compiler."""
    import struct
    if not blob:
        raise LegError("cannot identify the target of an EMPTY file (0 bytes)")
    if blob[:4] == b"\x7fELF":
        if len(blob) < 20:
            raise LegError("truncated ELF header (%d bytes) — cannot read "
                           "e_machine" % len(blob))
        if blob[4] != 2:
            raise LegError("ELF but not 64-bit (EI_CLASS=%d); this catalogue "
                           "declares only 64-bit targets" % blob[4])
        end = "<" if blob[5] == 1 else ">"
        machine, = struct.unpack(end + "H", blob[18:20])
        arch = ELF_MACHINES.get(machine)
        if arch is None:
            raise LegError(
                "ELF64 e_machine=0x%02X names no architecture this catalogue "
                "targets (known: %s). Refused rather than guessed: the caller is "
                "deciding whether a binary that would not run is this compiler's "
                "fault." % (machine, ", ".join(
                    "0x%02X=%s" % (k, v) for k, v in sorted(ELF_MACHINES.items()))))
        osabi = blob[7]
        target_os = ELF_OSABI_TARGET_OS.get(osabi)
        if target_os is None:
            raise LegError(
                "ELF64 EI_OSABI=%d names no target OS this catalogue builds for "
                "(known: %s). NOT defaulted to 'linux': ELF's identity does not "
                "carry an OS the way PE and Mach-O do, so a value nobody has "
                "mapped is a target nobody has declared, and answering 'linux' "
                "would send a caller looking for the wrong launcher."
                % (osabi, ", ".join("%d=%s" % (k, v) for k, v
                                    in sorted(ELF_OSABI_TARGET_OS.items()))))
        return (arch, "elf64", target_os)
    if blob[:2] == b"MZ":
        if len(blob) < 0x40:
            raise LegError("truncated DOS header (%d bytes)" % len(blob))
        e_lfanew, = struct.unpack("<I", blob[0x3C:0x40])
        if e_lfanew + 6 > len(blob) or blob[e_lfanew:e_lfanew + 4] != b"PE\0\0":
            raise LegError("an MZ image whose e_lfanew (0x%X) does not point at "
                           "a PE signature" % e_lfanew)
        machine, = struct.unpack("<H", blob[e_lfanew + 4:e_lfanew + 6])
        arch = PE_MACHINES.get(machine)
        if arch is None:
            raise LegError(
                "PE IMAGE_FILE_HEADER.Machine=0x%04X names no architecture this "
                "catalogue targets (known: %s)"
                % (machine, ", ".join("0x%04X=%s" % (k, v) for k, v
                                      in sorted(PE_MACHINES.items()))))
        # PE carries no OS field: the container IS the OS contract (a PE image is
        # loaded by the Windows loader, and this catalogue's only pe64 leg is a
        # `-windows-` one). Stated here rather than derived from a table of one.
        return (arch, "pe64", "windows")
    if len(blob) >= 16:
        magic, = struct.unpack("<I", blob[:4])
        if magic == MH_MAGIC_64:
            cputype, = struct.unpack("<I", blob[4:8])
            # MACHO_CPU_TYPES is the arch -> cpu_type map universal-archive
            # slicing already uses. INVERTED, never re-tabulated: two tables of
            # the same fact drift, and this one is load-bearing in both
            # directions.
            for name, code in sorted(MACHO_CPU_TYPES.items()):
                if code == cputype:
                    return (name, "macho64", "darwin")
            raise LegError(
                "Mach-O cpu_type=0x%08X names no architecture this catalogue "
                "targets (known: %s)"
                % (cputype, ", ".join("%s=0x%08X" % (k, v) for k, v
                                      in sorted(MACHO_CPU_TYPES.items()))))
    if _fat_slices(blob):
        raise LegError("a Mach-O FAT/universal archive carries SEVERAL targets; "
                       "the leg's own slice must be selected before one can be "
                       "named [D-FF1-MACHO-FAT]")
    raise LegError("unrecognised object file: first 4 bytes %r" % (blob[:4],))


# ── WHAT AN ARTEFACT ASKS THE LAUNCHER FOR, AT LOAD TIME ────────────────────
#
# The VALIDATOR half of `launchers[].requires` (see that section): after a leg
# builds, the artefact itself states what it will demand of whatever runs it, and
# every one of those demands must be covered by something the launcher DECLARED.
#
# ★★ AND THE ONE THAT PROVES THE LIST CANNOT BE DERIVED: `libgcc_s.so.1` IS
# INVISIBLE TO THIS CHECK BY CONSTRUCTION. It is in no DT_NEEDED and no PT_INTERP
# — glibc `dlopen()`s it lazily, from inside `pthread_exit`, at process TEARDOWN
# — which is exactly why the VPS abort printed AFTER a segment summary. A
# derived list would have been complete, correct, and would still have missed the
# thing that killed three segments. That is the standing argument for
# `requires` being DECLARED prose with evidence rather than a computed set, and
# this function's job is only to prove the declared list is not SMALLER than what
# the binary can be seen to ask for.
PT_INTERP = 3
PT_DYNAMIC = 2
DT_NEEDED = 1


def elf_runtime_dependencies(blob):
    """(PT_INTERP path, [DT_NEEDED sonames]) for an ELF64 image, or ("", []).

    Same reading discipline as `_elf_exports` directly below: endianness from
    EI_DATA and never from the host, offsets bounds-checked against the blob, no
    external tool. PT_INTERP comes from the PROGRAM headers because that is where
    the KERNEL reads it from — a `.interp` section can be stripped while the
    segment stays, and the kernel does not consult sections."""
    import struct
    if blob[:4] != b"\x7fELF" or len(blob) < 64 or blob[4] != 2:
        return ("", [])
    end = "<" if blob[5] == 1 else ">"
    e_phoff, = struct.unpack(end + "Q", blob[0x20:0x28])
    e_phentsize, e_phnum = struct.unpack(end + "HH", blob[0x36:0x3A])
    interp, needed = "", []
    dyn_spans = []
    if e_phoff and e_phentsize >= 56:
        for i in range(e_phnum):
            o = e_phoff + i * e_phentsize
            if o + 56 > len(blob):
                break
            p_type, = struct.unpack(end + "I", blob[o:o + 4])
            p_offset, = struct.unpack(end + "Q", blob[o + 8:o + 16])
            p_filesz, = struct.unpack(end + "Q", blob[o + 32:o + 40])
            if p_type == PT_INTERP and p_offset + p_filesz <= len(blob):
                interp = blob[p_offset:p_offset + p_filesz].split(b"\0")[0] \
                    .decode("ascii", "replace")
            elif p_type == PT_DYNAMIC:
                dyn_spans.append((p_offset, p_filesz))
    # DT_NEEDED through the SECTION headers, the same route _elf_exports takes,
    # because .dynamic's sh_link names its string table directly while PT_DYNAMIC
    # would have to be walked for DT_STRTAB and that address then mapped back
    # through the program headers to a file offset.
    e_shoff, = struct.unpack(end + "Q", blob[0x28:0x30])
    e_shentsize, e_shnum = struct.unpack(end + "HH", blob[0x3A:0x3E])
    if e_shoff and e_shentsize >= 64 and e_shnum:
        secs = []
        for i in range(e_shnum):
            o = e_shoff + i * e_shentsize
            if o + 64 > len(blob):
                secs = []
                break
            _nm, stype, _fl, _ad, off, size, link, _in, _al, entsz = struct.unpack(
                end + "IIQQQQIIQQ", blob[o:o + 64])
            secs.append((stype, off, size, link, entsz))
        for stype, off, size, link, _entsz in secs:
            if stype != SHT_DYNAMIC:
                continue
            if link >= len(secs) or secs[link][0] != SHT_STRTAB:
                continue
            _t, soff, ssize, _l, _e = secs[link]
            tab = blob[soff:soff + ssize]
            p = off
            while p + 16 <= off + size and p + 16 <= len(blob):
                tag, val = struct.unpack(end + "qQ", blob[p:p + 16])
                if tag == DT_NULL:
                    break
                if tag == DT_NEEDED and val < len(tab):
                    stop = tab.find(b"\0", val)
                    name = tab[val:stop if stop >= 0 else len(tab)]
                    if name:
                        needed.append(name.decode("ascii", "replace"))
                p += 16
    return (interp, needed)


def dependency_coverage_findings(label, rows, interp, needed, staged=()):
    """Every load-time demand of an artefact that NO declared requires row covers.

    THE TWO HALVES ARE HELD TO DIFFERENT STANDARDS, ON PURPOSE:

      * PT_INTERP must be covered by an explicit `file` ROW. It is the one
        dependency named by ABSOLUTE PATH inside the image, the one a launcher's
        sysroot silently re-roots (qemu prepends QEMU_LD_PREFIX), and the one
        whose absence produces no diagnostic at all — the kernel simply refuses
        the exec and the harness sees rc=255. A directory row does not cover it:
        "the sysroot exists" and "the loader is inside it" are two different
        facts, and it was the second one that was false.
      * A DT_NEEDED soname is resolved by NAME through a search path, so it is
        covered by a file row of that name, by any declared DIRECTORY (a sysroot
        supplies the platform's libraries), or by a library this harness STAGES
        beside the artefact.

    Returns findings, so the caller decides whether they are a lint result or a
    run-time refusal."""
    files = [r["path"] for r in rows if r.get("kind") == "file"]
    dirs = [r["path"] for r in rows if r.get("kind") == "directory"]
    staged = {os.path.basename(s) for s in staged if s}
    out = []
    if interp:
        if not any(p == interp or p.endswith(interp) for p in files):
            out.append(
                "leg '%s': the artefact's PT_INTERP is %s and NO declared "
                "`requires` row of kind 'file' covers it (declared files: %s). "
                "That is the file whose absence produced rc=255 with no "
                "diagnostic and 14 units charged to this compiler. A sysroot "
                "DIRECTORY row does not cover it: a launcher re-roots the "
                "interpreter path into its sysroot, so 'the sysroot is there' "
                "and 'the loader is inside it' are two separate facts and it was "
                "the second one that was false."
                % (label, interp, ", ".join(files) or "none"))
    for soname in needed:
        base = os.path.basename(soname)
        if base in staged:
            continue
        if any(os.path.basename(p) == base for p in files):
            continue
        if dirs:
            continue
        out.append(
            "leg '%s': the artefact declares DT_NEEDED %s, and this launcher "
            "declares neither a `requires` row naming it nor any directory that "
            "could supply it, and this harness does not stage it. Something has "
            "to provide it or the artefact will not load."
            % (label, soname))
    return out


def verify_shared_lib(path, want_container):
    """(ok, detail) for an artefact that claims to be `want_container`'s shared
    library. Reads the file; everything it decides comes from
    `binary_shared_lib_shape`."""
    try:
        with open(path, "rb") as f:
            blob = f.read()
    except OSError as exc:
        return (False, "could not be read back: %s" % exc)
    container, is_shared, detail = binary_shared_lib_shape(blob)
    if not container:
        return (False, "%d bytes, and it is not a recognised object file — %s"
                       % (len(blob), detail))
    if container != want_container:
        return (False, "%d bytes, but it is a %s image and this leg's declared "
                       "sharedLibFormat is a %s one — %s"
                       % (len(blob), container, want_container, detail))
    if not is_shared:
        return (False, "%d bytes of %s, but NOT a shared library — %s. sqlite's "
                       "test/loadext.test dlopen()s this file; an image that is "
                       "not loadable fails every loadext-* unit with `no such "
                       "function: half`, which reads exactly like a DSS "
                       "miscompile in the FIXTURE."
                       % (len(blob), container, detail))
    return (True, "%d bytes, %s" % (len(blob), detail))


# ── Tcl HEADER-vs-LIBRARY coherence, PER LEG ────────────────────────────────
# [D-HARNESS-TCL-HEADER-IS-HOST-CHOSEN-WHILE-EVERY-LEG-LIBRARY-IS-PINNED]
#
# THE DEFECT THIS EXISTS FOR (✔MEASURED 2026-08-06, first native macOS run):
# the drivers pick the Tcl HEADER from the HOST (tclsh on PATH -> its
# tclConfig.sh -> TCL_INCLUDE_SPEC) while EVERY leg's Tcl LIBRARY is pinned by
# its provider. On a Mac whose default Homebrew tcl-tk is 9.0.3 the fixture
# compiled against a 9.0 header and linked an 8.6 library, and sqlite's
# tclsqlite.c gates live code on TCL_MAJOR_VERSION>8 — so the build died with
# four K_SymbolUndefined (Tcl_GetBool, Tcl_GetBoolFromObj, Tcl_GetBytesFromObj,
# Tcl_GetChild) that a human had to reverse-engineer back to a version skew.
# On Linux the host tclsh is 8.6, so header and library agreed BY ACCIDENT OF
# THE HOST — which is why hundreds of green runs never saw it.
#
# The drivers already had THREE Tcl coherence checks (interpreter-vs-staging,
# header-vs-tclConfig, recipe-vs-staging) and ALL THREE ARE HOST-SCOPED. This is
# the missing FOURTH one, and the only PER-LEG one: it compares the staged
# header against the library each leg will actually LINK.
#
# ★ WHY IT IS HERE AND NOT IN THE DRIVERS. Both drivers hard-require this module
# already, and library ACQUISITION was centralised here for exactly this reason
# (D-HARNESS-LIBRARY-ACQUISITION-BUILT-FOR-ONE-LEG-IN-ONE-DRIVER: a capability
# in one driver and not the other is a recurring defect class in this harness).
# ONE implementation, two callers.
#
# ★★ A FILE NAME IS NOT A MEASUREMENT. `libtcl8.6.so` is what somebody called
# the file; the whole defect above is a name being trusted. Nothing here reads
# the path it was handed except to report it. Two INDEPENDENT instruments are
# taken out of the BYTES, and either one may veto:
#
#   A. STRUCTURAL — the library's own export table, read from the container's
#      own tables (ELF .dynsym, Mach-O LC_SYMTAB, PE export directory). Tcl 9.0
#      exports four names 8.6 does not, and they are precisely the four the
#      fixture referenced when this defect fired. Presence of ALL of them says
#      "major 9"; absence of ALL says "major 8"; a MIX says NOTHING (a Tcl 8.7
#      is exactly that mix) and is reported as undetermined rather than guessed.
#   B. SELF-DECLARED — the identity the binary stamps on ITSELF and the loader
#      actually uses: ELF DT_SONAME, Mach-O LC_ID_DYLIB, the PE export
#      directory's Name. Still a name, but the BINARY'S name for itself, not the
#      filesystem's, and it carries the MINOR that (A) cannot.
#
# Each instrument is compared against the staged header independently. A
# disagreement from EITHER is fatal; "cannot determine" only happens when BOTH
# are silent, and that is a WARN-and-skip, never a silent pass.
#
# ✔MEASURED 2026-08-06 — the readers below, on FIVE real Tcl libraries covering
# all three containers and two architectures, every one reporting 887/894/883
# exported names, `Tcl_CreateInterp` present, ZERO of the 9.0 markers:
#   /usr/lib/x86_64-linux-gnu/libtcl8.6.so            ELF64 x86_64  DT_SONAME    libtcl8.6.so
#   ~/.cache/dss-code-prime/arm64libs/libtcl8.6.so    ELF64 aarch64 DT_SONAME    libtcl8.6.so
#   harness-libs/macho64-arm64/libtcl8.6.dylib        Mach-O arm64  LC_ID_DYLIB  /opt/local/lib/libtcl8.6.dylib
#   harness-libs/macho64-x86_64/libtcl8.6.dylib       Mach-O x86_64 LC_ID_DYLIB  /opt/local/lib/libtcl8.6.dylib
#   C:/Program Files/Git/mingw64/bin/tcl86.dll        PE64          export Name  tcl86.dll
# No Tcl 9 library was available on this machine, so the "major 9" arm is
# exercised by the self-test against SYNTHESISED images of all three containers
# rather than a measured one. That the four names exist in 9.0 and not in 8.6 is
# DOCUMENTED (Tcl 9 API) and INFERRED from upstream sqlite calling them under
# `TCL_MAJOR_VERSION>8`; that they are absent from 8.6 is MEASURED, five times.

# The names Tcl 9.0 exports and Tcl 8.6 does not. Deliberately the SAME four the
# fixture failed on: if this set ever stops discriminating, it stops
# discriminating on the exact symbols the defect was made of.
TCL9_ONLY_EXPORTS = ("Tcl_GetBool", "Tcl_GetBoolFromObj",
                     "Tcl_GetBytesFromObj", "Tcl_GetChild")
# Present in EVERY Tcl since 7.x. Its absence does not mean "an old Tcl", it
# means "this is not a Tcl library" — a resolved path that is something else
# entirely, which is a different and worse fact than a version skew.
TCL_SENTINEL_EXPORT = "Tcl_CreateInterp"

SHT_STRTAB = 3
SHT_DYNAMIC = 6
SHT_DYNSYM = 11
DT_NULL = 0
DT_SONAME = 14
STB_GLOBAL = 1
STB_WEAK = 2
SHN_UNDEF = 0
LC_SYMTAB = 0x2
LC_ID_DYLIB = 0xD
N_STAB = 0xE0
N_EXT = 0x01
N_TYPE = 0x0E
N_SECT = 0x0E


def _elf_exports(blob):
    """(exported names, DT_SONAME) for an ELF64 shared object, or (None, why).

    Read through the SECTION headers rather than PT_DYNAMIC's hash tables: a
    library on disk has them, and .dynsym's sh_size/sh_entsize gives the symbol
    count directly, where DT_GNU_HASH would have to be walked to recover it.
    Endianness comes from EI_DATA, never from the host — this file is read on a
    machine that may not share the target's byte order."""
    import struct
    if blob[:4] != b"\x7fELF" or len(blob) < 64:
        return None, "not an ELF image"
    if blob[4] != 2:
        return None, "ELF but not 64-bit (EI_CLASS=%d)" % blob[4]
    end = "<" if blob[5] == 1 else ">"
    e_shoff, = struct.unpack(end + "Q", blob[0x28:0x30])
    e_shentsize, e_shnum = struct.unpack(end + "HH", blob[0x3A:0x3E])
    if not e_shoff or e_shentsize < 64 or not e_shnum:
        return None, "no section header table (stripped?)"
    secs = []
    for i in range(e_shnum):
        o = e_shoff + i * e_shentsize
        if o + 64 > len(blob):
            return None, "the section header table runs past the end of the file"
        _nm, stype, _fl, _ad, off, size, link, _in, _al, entsz = struct.unpack(
            end + "IIQQQQIIQQ", blob[o:o + 64])
        secs.append((stype, off, size, link, entsz))

    def _strtab(link):
        if link >= len(secs) or secs[link][0] != SHT_STRTAB:
            return None
        _t, off, size, _l, _e = secs[link]
        return blob[off:off + size]

    def _at(tab, idx):
        stop = tab.find(b"\0", idx)
        return tab[idx:stop if stop >= 0 else len(tab)]

    names, soname, saw_dynsym = set(), "", False
    for stype, off, size, link, entsz in secs:
        if stype == SHT_DYNSYM and entsz >= 24:
            tab = _strtab(link)
            if tab is None:
                continue
            saw_dynsym = True
            for k in range(size // entsz):
                so = off + k * entsz
                if so + 24 > len(blob):
                    break
                st_name, st_info, _oth, st_shndx = struct.unpack(
                    end + "IBBH", blob[so:so + 8])
                if st_shndx == SHN_UNDEF:
                    continue          # imported, not exported
                if (st_info >> 4) not in (STB_GLOBAL, STB_WEAK):
                    continue          # local
                nm = _at(tab, st_name)
                if nm:
                    names.add(nm.decode("ascii", "replace"))
        elif stype == SHT_DYNAMIC:
            tab = _strtab(link)
            if tab is None:
                continue
            p = off
            while p + 16 <= off + size and p + 16 <= len(blob):
                tag, val = struct.unpack(end + "qQ", blob[p:p + 16])
                if tag == DT_NULL:
                    break
                if tag == DT_SONAME:
                    soname = _at(tab, val).decode("ascii", "replace")
                p += 16
    if not saw_dynsym:
        return None, "ELF64 with no .dynsym — it exports nothing readable"
    return names, soname


def _macho_exports(blob):
    """(exported names, LC_ID_DYLIB install name) for a THIN 64-bit little-endian
    Mach-O, or (None, why). Names are de-underscored: Mach-O's asm-level `_Tcl_x`
    is the C symbol `Tcl_x`, and every caller here speaks C."""
    import struct
    if len(blob) < 32:
        return None, "too small to be a Mach-O image"
    magic, = struct.unpack("<I", blob[:4])
    if magic != MH_MAGIC_64:
        if _fat_slices(blob):
            return None, ("a Mach-O FAT/universal archive — the leg's own slice "
                          "must be selected before this can be read "
                          "[D-FF1-MACHO-FAT]")
        return None, "not a thin 64-bit little-endian Mach-O"
    ncmds, = struct.unpack("<I", blob[16:20])
    names, ident, saw_symtab = set(), "", False
    p = 32
    for _ in range(ncmds):
        if p + 8 > len(blob):
            break
        cmd, cmdsize = struct.unpack("<II", blob[p:p + 8])
        if cmdsize < 8:
            break
        if cmd == LC_SYMTAB and p + 24 <= len(blob):
            symoff, nsyms, stroff, strsize = struct.unpack(
                "<IIII", blob[p + 8:p + 24])
            tab = blob[stroff:stroff + strsize]
            saw_symtab = True
            for k in range(nsyms):
                so = symoff + k * 16
                if so + 16 > len(blob):
                    break
                n_strx, n_type, _sect, _desc = struct.unpack(
                    "<IBBH", blob[so:so + 8])
                if n_type & N_STAB:
                    continue          # a debug entry, not a symbol
                if not n_type & N_EXT:
                    continue          # not external
                if (n_type & N_TYPE) != N_SECT:
                    continue          # undefined/absolute/indirect, not defined here
                stop = tab.find(b"\0", n_strx)
                nm = tab[n_strx:stop if stop >= 0 else len(tab)]
                if nm:
                    s = nm.decode("ascii", "replace")
                    names.add(s[1:] if s.startswith("_") else s)
        elif cmd == LC_ID_DYLIB and p + 12 <= len(blob):
            nameoff, = struct.unpack("<I", blob[p + 8:p + 12])
            ident = blob[p + nameoff:p + cmdsize].split(b"\0")[0].decode(
                "utf-8", "replace")
        p += cmdsize
    if not saw_symtab:
        return None, "Mach-O with no LC_SYMTAB — it exports nothing readable"
    return names, ident


def _pe_exports(blob):
    """(exported names, the export directory's own Name) for a PE32+ image, or
    (None, why). The Name field is the DLL's self-declared identity — the name
    an importer records — and it is NOT required to equal the file name."""
    import struct
    if blob[:2] != b"MZ" or len(blob) < 0x40:
        return None, "not an MZ/PE image"
    e_lfanew, = struct.unpack("<I", blob[0x3C:0x40])
    if e_lfanew + 24 > len(blob) or blob[e_lfanew:e_lfanew + 4] != b"PE\0\0":
        return None, "an MZ image whose e_lfanew does not point at a PE signature"
    nsec, = struct.unpack("<H", blob[e_lfanew + 6:e_lfanew + 8])
    opt_size, = struct.unpack("<H", blob[e_lfanew + 20:e_lfanew + 22])
    opt = e_lfanew + 24
    if opt + 2 > len(blob):
        return None, "truncated optional header"
    magic, = struct.unpack("<H", blob[opt:opt + 2])
    if magic != 0x20B:
        return None, "PE but not PE32+ (optional header magic 0x%04X)" % magic
    ddir = opt + 112                  # PE32+ data directories start here
    if ddir + 8 > len(blob):
        return None, "truncated data directories"
    exp_rva, _exp_size = struct.unpack("<II", blob[ddir:ddir + 8])
    if not exp_rva:
        return None, "PE32+ with an empty export data directory — it exports nothing"
    secs = []
    for i in range(nsec):
        o = opt + opt_size + i * 40
        if o + 40 > len(blob):
            return None, "the section table runs past the end of the file"
        vsize, vaddr, rawsize, rawptr = struct.unpack("<IIII", blob[o + 8:o + 24])
        secs.append((vaddr, max(vsize, rawsize), rawptr))

    def _off(rva):
        for vaddr, vspan, rawptr in secs:
            if vaddr <= rva < vaddr + vspan:
                return rawptr + (rva - vaddr)
        return None

    def _cstr(rva):
        o = _off(rva)
        if o is None or o >= len(blob):
            return ""
        stop = blob.find(b"\0", o)
        return blob[o:stop if stop >= 0 else len(blob)].decode("ascii", "replace")

    eo = _off(exp_rva)
    if eo is None or eo + 40 > len(blob):
        return None, "the export directory's RVA maps into no section"
    name_rva, _base, _nfun, nnames, _afun, anames, _aord = struct.unpack(
        "<IIIIIII", blob[eo + 12:eo + 40])
    ident = _cstr(name_rva) if name_rva else ""
    names = set()
    ao = _off(anames) if anames else None
    if ao is not None:
        for k in range(nnames):
            o = ao + 4 * k
            if o + 4 > len(blob):
                break
            rva, = struct.unpack("<I", blob[o:o + 4])
            s = _cstr(rva)
            if s:
                names.add(s)
    return names, ident


def library_exports(blob):
    """(exported names, self-declared identity, why-not) for a shared library, in
    whichever of the three containers it is. `names` is None when nothing could
    be read, and `why_not` then says what stopped it — the caller must treat that
    as "cannot determine", never as "exports nothing"."""
    for reader in (_elf_exports, _macho_exports, _pe_exports):
        names, extra = reader(blob)
        if names is not None:
            return names, extra, ""
    # Report through the container the bytes actually claim to be, so the reason
    # names the format the reader gave up on rather than the last one tried.
    container, _shared, detail = binary_shared_lib_shape(blob)
    if container == "elf64":
        return None, "", _elf_exports(blob)[1]
    if container == "macho64":
        return None, "", _macho_exports(blob)[1]
    if container == "pe64":
        return None, "", _pe_exports(blob)[1]
    return None, "", detail


# The version shapes a Tcl library stamps on ITSELF. A CLOSED vocabulary, keyed
# on the identity the binary declares (DT_SONAME / LC_ID_DYLIB / PE export Name)
# — never on the path it was found at. Anything outside it yields no version at
# all, which is the honest answer and lets instrument (A) stand alone.
_TCL_IDENT_SHAPES = (
    # libtcl8.6.so · libtcl8.6.so.0 · libtcl9.0.dylib
    re.compile(r"^libtcl(\d+)\.(\d+)\.(?:so|dylib)(?:\.\d+)*$"),
    # tcl86.dll · tcl90.dll · tcl86t.dll — Windows spells the version with no
    # separator, and a trailing `t` marks a THREADED build (the convention
    # msys2, conda-forge and ActiveState all use; sqlite's testfixture wants
    # exactly that build). ⚠ THE SUFFIX WAS MISSING AND IT COST A MEASUREMENT:
    # the pe64 leg's acquired `tcl86t.dll` matched nothing, so instrument B went
    # SILENT and the per-leg coherence check degraded from "8.6" to a bare major
    # "8" without saying it had — the quiet half-failure this pair of
    # instruments exists to avoid. ✔MEASURED 2026-08-06 on the acquired DLL.
    re.compile(r"^tcl(\d)(\d)t?\.dll$", re.IGNORECASE),
)


def tcl_identity_version(ident):
    """"X.Y" parsed out of a library's SELF-DECLARED identity, or "".

    Only the basename is considered: LC_ID_DYLIB is an absolute install path
    (`/opt/local/lib/libtcl8.6.dylib`) while DT_SONAME and the PE export Name are
    bare."""
    base = (ident or "").replace("\\", "/").rsplit("/", 1)[-1]
    for shape in _TCL_IDENT_SHAPES:
        m = shape.match(base)
        if m:
            return "%s.%s" % (m.group(1), m.group(2))
    return ""


def tcl_library_facts(blob):
    """Everything measurable about a Tcl library's version, from its BYTES.

    Returns a dict:
      symbolMajor   "8" | "9" | ""   — instrument (A), the export table
      identity      the binary's self-declared name (reported even when it
                    carries no version, because it is the loader's view)
      identityVersion "X.Y" | ""     — instrument (B)
      version       the best single answer for a message ("" when neither
                    instrument spoke)
      method        prose naming WHICH instruments spoke
      undetermined  why nothing could be measured ("" when something could)
    """
    names, ident, why = library_exports(blob)
    facts = {"symbolMajor": "", "identity": ident, "identityVersion": "",
             "version": "", "method": "", "undetermined": ""}
    if names is None:
        facts["undetermined"] = ("its export table could not be read: %s"
                                 % (why or "unrecognised container"))
        return facts
    if TCL_SENTINEL_EXPORT not in names:
        facts["undetermined"] = (
            "it exports %d name(s) but not %s, so it is not a Tcl library at all "
            "— the leg resolved something else under a Tcl file name"
            % (len(names), TCL_SENTINEL_EXPORT))
        return facts
    facts["identityVersion"] = tcl_identity_version(ident)
    present = [n for n in TCL9_ONLY_EXPORTS if n in names]
    if len(present) == len(TCL9_ONLY_EXPORTS):
        facts["symbolMajor"] = "9"
    elif not present:
        facts["symbolMajor"] = "8"
    # else: a MIX. Tcl 8.7 is exactly that. Say nothing rather than guess.
    spoke = []
    if facts["symbolMajor"]:
        spoke.append("exported-symbol markers -> major %s" % facts["symbolMajor"])
    if facts["identityVersion"]:
        spoke.append("self-declared identity %r -> %s"
                     % (ident, facts["identityVersion"]))
    facts["method"] = "; ".join(spoke)
    facts["version"] = facts["identityVersion"] or facts["symbolMajor"]
    if not spoke:
        facts["undetermined"] = (
            "it is a Tcl library (%d exports) but neither instrument could pin "
            "its version: %d of the %d Tcl-9-only markers are present (%s), "
            "which no single release explains, and its self-declared identity "
            "%r carries no version"
            % (len(names), len(present), len(TCL9_ONLY_EXPORTS),
               ", ".join(present) or "none", ident))
    return facts


def tcl_library_facts_at(path):
    """`tcl_library_facts` for a file. An unreadable file is UNDETERMINED, not a
    pass: the driver has already decided this path is the leg's library."""
    try:
        with open(path, "rb") as f:
            blob = f.read()
    except OSError as exc:
        return {"symbolMajor": "", "identity": "", "identityVersion": "",
                "version": "", "method": "",
                "undetermined": "it could not be read: %s" % exc}
    return tcl_library_facts(blob)


_TCL_H_VERSION_RE = re.compile(
    r"^[ \t]*#[ \t]*define[ \t]+TCL_VERSION[ \t]+\"([0-9][0-9.]*)\"", re.MULTILINE)


def tcl_header_version(path):
    """The "X.Y" a staged tcl.h DECLARES, or "".

    Tolerates Tcl 9's indented `#   define` as well as 8.6's `#define`, and is
    the SAME fact build-and-test.sh's `tcl_h_version` reads with sed — spelled
    once here so both drivers ask the same question of the same bytes."""
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            text = f.read()
    except OSError:
        return ""
    m = _TCL_H_VERSION_RE.search(text)
    return m.group(1) if m else ""


def tcl_coherence(header_version, entries):
    """THE FOURTH, PER-LEG Tcl CHECK. Pure — takes the staged header's version
    and [(label, path, facts)], returns (ok, report lines, warnings, fatal).

    Two ways to be incoherent, and the CROSS-LEG one is judged first because it
    has the better diagnostic:
      1. Two legs resolve DIFFERENT Tcl versions. The headers are staged ONCE for
         every leg, so no single header can serve both — the run is structurally
         incoherent whatever the header says.
      2. A leg's library disagrees with the staged header. EITHER instrument may
         veto: the marker set vetoes on the MAJOR (an API generation the library
         does not have), the self-declared identity vetoes on the full X.Y.
    Never a warning. A warn here ships a binary that links clean and misbehaves,
    which is the exact class this harness exists to prevent."""
    lines, warnings = [], []
    for label, path, facts in entries:
        lines.append("%s\t%s\t%s\t%s"
                     % (label, facts["version"] or "?",
                        facts["method"] or facts["undetermined"], _fwd(path)))
        if facts["undetermined"]:
            warnings.append(
                "[%s] the Tcl version of %s could NOT be determined — %s. This "
                "leg is NOT checked against the staged header; if it fails to "
                "link on Tcl symbols, a header/library version skew is the first "
                "thing to rule out."
                % (label, _fwd(path), facts["undetermined"]))
    known = [(label, path, facts) for label, path, facts in entries
             if facts["version"]]
    versions = sorted({facts["version"] for _l, _p, facts in known})
    if len(versions) > 1:
        return (False, lines, warnings,
                "the selected legs resolve %d DIFFERENT Tcl versions (%s), and "
                "the Tcl headers are staged ONCE for every leg — so no header "
                "can be correct for all of them. This run is structurally "
                "incoherent and would compile at least one leg against a Tcl it "
                "does not link.\n%s\n      Fix the legs' libraries so they agree, "
                "or select only the legs that do."
                % (len(versions), ", ".join(versions),
                   "\n".join("        %-16s %s   (%s)"
                             % (label, facts["version"], _fwd(path))
                             for label, path, facts in known)))
    header_major = (header_version or "").split(".")[0]
    for label, path, facts in known:
        why = ""
        if (facts["symbolMajor"] and header_major
                and facts["symbolMajor"] != header_major):
            why = ("its export table has the Tcl %s API generation (%s) while "
                   "the staged header declares TCL_VERSION \"%s\""
                   % (facts["symbolMajor"],
                      "all %d of %s are exported"
                      % (len(TCL9_ONLY_EXPORTS), ", ".join(TCL9_ONLY_EXPORTS))
                      if facts["symbolMajor"] == "9" else
                      "none of %s is exported" % ", ".join(TCL9_ONLY_EXPORTS),
                      header_version))
        elif (facts["identityVersion"] and header_version
                and facts["identityVersion"] != header_version):
            why = ("it declares itself %r (Tcl %s) while the staged header "
                   "declares TCL_VERSION \"%s\""
                   % (facts["identity"], facts["identityVersion"],
                      header_version))
        if why:
            return (False, lines, warnings,
                    "Tcl HEADER/LIBRARY SKEW on leg '%s' — %s.\n"
                    "        leg library : %s\n"
                    "        staged header: TCL_VERSION \"%s\"\n"
                    "      The header decides WHICH Tcl symbols the fixture "
                    "REFERENCES (sqlite's tclsqlite.c gates live code on "
                    "TCL_MAJOR_VERSION>8); the library decides which it can "
                    "RESOLVE. Building anyway produces undefined-symbol errors "
                    "that read like a compiler defect.\n"
                    "      Fix: DSS_TCL_VERSION=%s (stage the header this leg's "
                    "PINNED library matches), then re-run. Pinning the header to "
                    "the library is correct; pinning the library to this host is "
                    "not — every leg's library is target-keyed and this host is "
                    "not a target."
                    % (label, why, _fwd(path), header_version,
                       facts["version"]))
    return (True, lines, warnings, "")


def _fwd(path):
    """A path spelled with forward slashes, on every host.

    NOT cosmetic. Two drivers consume these paths: build-and-test.ps1 on Windows
    and build-and-test.sh on POSIX. `os.path.join` on Windows produces a MIXED
    spelling (`C:/a/b\\c`) when its first component came from a caller that used
    forward slashes, and this project has already been bitten by a path whose
    spelling differed between the tool that produced it and the tool that
    consumed it. Windows accepts `/` in every API and in PowerShell; POSIX is
    unaffected because there is nothing to replace. It is also the spelling
    dss-code-prime itself prints (base-harness.ps1: "the compiler prints forward
    slashes on every host"), so the report and the compiler's own log agree."""
    return (path or "").replace("\\", "/")


def _run_capture(argv, cwd=None):
    """(rc, combined output). rc is taken DIRECTLY off the process, never after a
    pipe. Kept next to its callers for the same reason `_run_machine_probe` is.

    ⚠ DELIBERATELY UNBOUNDED, and that is a DECISION rather than an oversight:
    this runs a COMPILER, and a compile has no honest deadline — a cold cache, a
    loaded machine or a big TU are all legitimately slow, so any number here would
    eventually fail a build that was merely slow and report it as a hang. `make`
    does not time out `cc` either. It is safe to leave unbounded for the reason
    the spawn-contract comment above states: `_run_capture` never runs during
    `--plan`, so it cannot hang step 1 of a corpus run.
    [D-HARNESS-PLAN-RESOLUTION-SPAWN-IS-UNBOUNDED]

    ★ BUT THE ENCODING IS NAMED, because that exposure is NOT a judgement call.
    Without it a compiler diagnostic containing one byte the host locale cannot
    decode kills the reader thread, and `(proc.stdout or "")` then turns the lost
    stream into `""` — so a FAILED build reports its rc with NO reason attached,
    which reads as a build that said nothing. A compiler is exactly the child most
    likely to emit a non-ASCII byte: localised messages, and source text quoted
    back in a caret line. [D-HARNESS-CHILD-OUTPUT-UNDECODABLE-CRASHES-THE-RESOLVER]"""
    import subprocess
    try:
        proc = subprocess.run(argv, capture_output=True, text=True, cwd=cwd,
                              encoding=CHILD_OUTPUT_ENCODING,
                              errors=CHILD_OUTPUT_ERRORS)
    except OSError as exc:
        return 127, "%s: %s" % (type(exc).__name__, exc)
    return proc.returncode, (proc.stdout or "") + (proc.stderr or "")


# The line dss-code-prime prints for each artefact it emitted. ✔MEASURED
# 2026-08-05: `dss-code-prime: artifact x86_64:pe64-x86_64-windows-dll <path>`.
# Matched here the SAME way base-harness.sh's `dss_bh_reported_artifacts` and
# base-harness.ps1's `Get-DssReportedArtifacts` match it — by the literal
# `dss-code-prime: artifact <spec> ` prefix, taking DISTINCT paths, and REFUSING
# to guess when there is more than one. Duplicated here rather than reached for
# because those two live in base-harness.{sh,ps1}, which this cycle does not own;
# the shapes must not diverge, and the self-test pins this one against the exact
# prefix both of them grep for.
DSS_ARTIFACT_LINE_PREFIX = "dss-code-prime: artifact "


def dss_reported_artifacts(log_text, spec):
    """Every DISTINCT artefact path the log reports for `spec`, in order."""
    needle = "%s%s " % (DSS_ARTIFACT_LINE_PREFIX, spec)
    out = []
    for line in (log_text or "").splitlines():
        line = line.strip()
        if line.startswith(needle):
            path = line[len(needle):].strip()
            if path and path not in out:
                out.append(path)
    return out


def dss_log_errors(log_text, limit=8):
    """The diagnostic lines a caller should quote. BOTH spellings: dss-code-prime
    prints `error[CODE]` for its own diagnostics, and a front-end message can
    read `error:` — grepping only one of them has cost this project a diagnosis
    before."""
    hits = []
    for line in (log_text or "").splitlines():
        if "error[" in line or "error:" in line:
            hits.append(line.strip())
            if len(hits) >= limit:
                break
    return hits


def build_loadext_helper(leg, dss, sqlite_src, sqlite_bld, dest_dir, work_dir,
                         dss_config="release", builder=None, reference_cc="",
                         reference_machine="", runner=None):
    """Build + STAGE this leg's loadext helper, and report BOTH arms.

    Returns a report dict (see `verdictClass`). Raises LegError only for a
    catalogue/usage defect — every BUILD outcome is a reported verdict, because
    this is called from inside a per-leg loop and one leg's failure must never
    cost the other legs theirs [the harness must SURVIVE everything].

    `verdictClass` is from the drivers' closed vocabulary and there are exactly
    three outcomes:
      ""                            staged; the run proceeds.
      "poisoned"                    a REAL failure — the primary build produced
                                    no loadable library. Not environmental: the
                                    compiler under test did not do its job, and
                                    the run must not exit 0 on it.
      "skipped-build-input-missing" ENVIRONMENTAL — the operator asked for the
                                    REFERENCE arm and this machine has no
                                    verified target compiler for the leg. Nothing
                                    is wrong with DSS or with the corpus; a
                                    different machine, same catalogue, runs it.
                                    It may exit 0, and it says loudly that the
                                    default (`dss`) would have worked here.
    """
    runner = runner or _run_capture
    builder = loadext_helper_builder(builder)
    label = leg.get("label", "?")
    name = loadext_helper_name(leg)
    source = _fwd(os.path.join(sqlite_src, LOADEXT_HELPER_SOURCE))
    includes = [_fwd(sqlite_src), _fwd(sqlite_bld)]
    container = spec_format_container(leg.get("spec", ""))
    report = {
        "leg": label,
        "builder": builder,
        "helperName": name,
        "source": source,
        "sharedLibSpec": shared_lib_spec(leg),
        "staged": "",
        "primary": None,
        "control": None,
        "crossCheck": "",
        "verdictClass": "poisoned",
        "detail": "",
    }

    if not name:
        report["detail"] = (
            "this leg declares no build.loadExtHelperName, so the harness does "
            "not know what file sqlite's test/loadext.test will look for on %s. "
            "Staging it under a guessed name is invisible to the corpus, which "
            "then builds its own with a hardcoded compiler. Declare it in "
            "legs.json (harness_legs.py --lint checks it against the target OS)."
            % leg.get("spec", "?"))
        return report
    if not shared_lib_format(leg):
        report["detail"] = (
            "this leg declares no build.sharedLibFormat, so there is no object "
            "format to emit the helper in. It is the `--target <arch>:<format>` "
            "argument DSS is given; declare it in legs.json (harness_legs.py "
            "--lint checks it against the leg's own spec).")
        return report
    if not os.path.isfile(source):
        report["detail"] = ("sqlite extension source not found: %s — the staged "
                            "tree is incomplete, so no leg's helper can be built "
                            "from it." % source)
        return report

    os.makedirs(work_dir, exist_ok=True)
    os.makedirs(dest_dir, exist_ok=True)

    # ── the DSS arm — always attempted, on every host ────────────────────────
    def _dss_arm():
        outdir = _fwd(os.path.join(work_dir, "dss"))
        log_path = _fwd(os.path.join(work_dir, "loadext-helper-dss.log"))
        os.makedirs(outdir, exist_ok=True)
        argv = loadext_helper_dss_argv(leg, dss, source, includes, outdir,
                                       dss_config)
        rc, out = runner(argv)
        try:
            with open(log_path, "w", encoding="utf-8", errors="replace") as f:
                f.write(" ".join(argv) + "\n\n" + (out or ""))
        except OSError:
            pass
        arm = {"builder": "dss", "available": True, "ok": False, "rc": rc,
               "argv": argv, "log": log_path, "artifact": "", "bytes": 0,
               "errors": dss_log_errors(out), "why": ""}
        # ★ dss-code-prime EXITS 0 EVEN ON FATAL ERRORS — the verdict comes from
        # `error[`/`error:` in the output PLUS the artefact the build itself
        # REPORTED, never from the process exit status. Same rule, same order, as
        # base-harness.{sh,ps1}.
        if arm["errors"]:
            arm["why"] = ("the build emitted %d diagnostic(s); first: %s"
                          % (len(arm["errors"]), arm["errors"][0]))
            return arm
        hits = dss_reported_artifacts(out, shared_lib_spec(leg))
        if len(hits) > 1:
            arm["why"] = ("the build reported %d DIFFERENT artefacts for %s (%s) "
                          "— refusing to guess which was meant"
                          % (len(hits), shared_lib_spec(leg), ", ".join(hits)))
            return arm
        if not hits:
            arm["why"] = ("0 diagnostics and the build reported NO artefact for "
                          "%s (expected a '%s%s <path>' line)"
                          % (shared_lib_spec(leg), DSS_ARTIFACT_LINE_PREFIX,
                             shared_lib_spec(leg)))
            return arm
        arm["artifact"] = _fwd(hits[0])
        if not os.path.isfile(hits[0]):
            arm["why"] = ("0 diagnostics but the artefact the build REPORTED is "
                          "not on disk: %s" % hits[0])
            return arm
        arm["bytes"] = os.path.getsize(hits[0])
        arm["ok"] = True
        return arm

    # ── the reference arm — the CONTROL, only where it exists ────────────────
    def _reference_arm():
        arm = {"builder": "reference", "available": bool(reference_cc),
               "ok": False, "rc": None, "argv": [], "log": "", "artifact": "",
               "bytes": 0, "errors": [], "cc": reference_cc,
               "machine": reference_machine, "why": ""}
        if not reference_cc:
            arm["why"] = (
                "no declared targetCc candidate on this machine both EXISTS and "
                "proves (via `%s`) that it targets %s. That is the ENTIRE reason "
                "this arm is a control and not the default: requiring it here is "
                "what made a leg's coverage depend on the host owning a "
                "third-party cross-compiler."
                % (CC_TARGET_MACHINE_FLAG, leg.get("spec", "?")))
            return arm
        outdir = _fwd(os.path.join(work_dir, "reference"))
        os.makedirs(outdir, exist_ok=True)
        dst = _fwd(os.path.join(outdir, name))
        log_path = _fwd(os.path.join(work_dir, "loadext-helper-reference.log"))
        argv = loadext_helper_reference_argv(leg, reference_cc, source, includes,
                                             dst)
        arm["argv"] = argv
        rc, out = runner(argv)
        arm["rc"] = rc
        arm["log"] = log_path
        try:
            with open(log_path, "w", encoding="utf-8", errors="replace") as f:
                f.write(" ".join(argv) + "\n\n" + (out or ""))
        except OSError:
            pass
        if rc != 0:
            arm["errors"] = [l for l in (out or "").splitlines() if l.strip()][:8]
            arm["why"] = ("`%s` exited %d; first line: %s"
                          % (reference_cc, rc,
                             arm["errors"][0] if arm["errors"] else "<empty log>"))
            return arm
        if not os.path.isfile(dst):
            arm["why"] = "the compiler exited 0 but wrote no %s" % dst
            return arm
        arm["artifact"] = dst
        arm["bytes"] = os.path.getsize(dst)
        arm["ok"] = True
        return arm

    if builder == "dss":
        primary = _dss_arm()
        control = _reference_arm()
    else:
        primary = _reference_arm()
        # The control is DSS, and it is built even when the operator has asked
        # for the reference arm: this direction is the one that ANSWERS the
        # cancelling-defect question, so both artefacts must exist to compare.
        control = _dss_arm()
    report["primary"] = primary
    report["control"] = control

    if not primary["ok"]:
        if builder == "reference" and not primary["available"]:
            report["verdictClass"] = "skipped-build-input-missing"
            report["detail"] = (
                "%s=reference was requested, and %s. The DEFAULT builder ('dss') "
                "needs nothing from this machine and would have staged this "
                "leg's helper here — so this is an operator-selected control arm "
                "that this host cannot provide, not a defect."
                % (LOADEXT_HELPER_BUILDER_ENV, primary["why"]))
            return report
        report["verdictClass"] = "poisoned"
        report["detail"] = (
            "the %s build of this leg's loadext helper (%s) FAILED: %s  "
            "[command: %s]  [full diagnostics: %s]  NOT falling back to the "
            "other builder: %s"
            % (primary["builder"], name, primary["why"],
               " ".join(primary["argv"]) or "<none>", primary["log"] or "<none>",
               "a DSS emission failure is a compiler defect and this harness "
               "exists to surface them — building the artefact another way would "
               "hide the finding" if primary["builder"] == "dss" else
               "the operator explicitly selected this arm; silently substituting "
               "the other one would report a control that never ran"))
        return report

    # ── stage it under the name THIS LEG'S TARGET looks for ──────────────────
    staged = _fwd(os.path.join(dest_dir, name))
    try:
        shutil.copyfile(primary["artifact"], staged)
    except OSError as exc:
        report["verdictClass"] = "poisoned"
        report["detail"] = ("the %s build produced %s but it could not be staged "
                            "as %s: %s" % (primary["builder"],
                                           primary["artifact"], staged, exc))
        return report
    report["staged"] = staged

    ok, why = verify_shared_lib(staged, container)
    if not ok:
        report["verdictClass"] = "poisoned"
        report["detail"] = (
            "the %s build of %s reported success, but the artefact it produced "
            "is not a loadable %s shared library: %s  [command: %s]"
            % (primary["builder"], name, container, why,
               " ".join(primary["argv"])))
        return report

    report["verdictClass"] = ""
    # The one-line ledger detail. It names the BUILDER and the TARGET SPEC, which
    # together are the whole provenance question a loadext-* red raises; the full
    # argv is in the report and in the log, so this does not paste a 12-token
    # command line into a per-leg summary.
    report["detail"] = ("%s, built for %s by %s%s — verified: %s"
                        % (name, shared_lib_spec(leg), primary["builder"],
                           (" (%s, which reports '%s')"
                            % (reference_cc, reference_machine or "<unrecorded>"))
                           if primary["builder"] == "reference" else "",
                           why))
    # THE CROSS-CHECK LINE. Reported on every outcome so a build log always
    # states what the control did — including "there was none here", which is a
    # fact about the machine and not a silence.
    if control["ok"]:
        delta = control["bytes"] - primary["bytes"]
        report["crossCheck"] = (
            "CONTROL PRESENT: the same source also built by %s (%s) -> %s, %d "
            "bytes (%+d vs the staged %s artefact's %d). The staged file is the "
            "%s one; the control sits beside it so a loadext-* red can be "
            "re-run against it (%s=%s)."
            % (control["builder"], control.get("cc") or "dss",
               control["artifact"], control["bytes"], delta, primary["builder"],
               primary["bytes"], primary["builder"], LOADEXT_HELPER_BUILDER_ENV,
               "reference" if primary["builder"] == "dss" else "dss"))
    elif not control["available"]:
        report["crossCheck"] = (
            "NO CONTROL ON THIS HOST: %s The staged helper is the %s one and it "
            "was verified structurally (%s); nothing about this leg's coverage "
            "depends on the missing compiler."
            % (control["why"], primary["builder"], why))
    else:
        report["crossCheck"] = (
            "CONTROL ATTEMPTED AND FAILED (this does NOT gate the run): %s  "
            "[%s]  A control that cannot build the same source is itself worth "
            "seeing — but the staged helper is the %s one and it verified: %s"
            % (control["why"], control["log"], primary["builder"], why))
    return report


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


# ── The sqlite CONFIGURE header, per TARGET ─────────────────────────────────
#
# D-HARNESS-MACHO-LEG-INHERITS-THE-DERIVING-LINUX-HOSTS-CONFIGURE-PROBES. The
# staged `sqlite_cfg.h` twin of `header_stages()` above: a leg declares what ITS
# target's answers are (`build.configureAnswers`) and the drivers stage one
# configured header per distinct TARGET OS.
#
# ★ THE KEY IS THE TARGET OS, NOT THE recipeTransform, and the difference is the
# whole reason this is a second stage family rather than another file in the zinc
# one. Four legs share `recipeTransform: "none"` — two Linux and two Darwin — and
# their configure answers DIFFER. Keying this on the transform would hand the
# Darwin legs the Linux header again, which is the defect.
#
# ⛔ REJECTED ALTERNATIVES, recorded because both are the obvious thing to try:
#
#   (a) DERIVE THE RECIPE PER TARGET, on a machine of that target (run sqlite's
#       ./configure on a Mac for the Darwin legs). This is what upstream's build
#       system expects and it is the reason a native build never has this bug.
#       It is REFUSED here because it breaks the harness's hard requirement —
#       "build ANY target inside ANY host" [D-HARNESS-CROSS-HOST-ANY-TARGET]:
#       it makes the Darwin legs unbuildable on the four hosts out of five that
#       are not a Mac, which is exactly the host-lock this catalogue exists to
#       end.
#
#   (b) A BLUNT `darwin-selfconfig` RECIPE TRANSFORM that simply drops
#       `_HAVE_SQLITE_CONFIG_H`, mirroring `windows-selfconfig`. ✔MEASURED: it
#       WORKS — the macho64-arm64 CLI's `off64_t`/`pread64`/`pwrite64` errors all
#       disappear. It is still the wrong answer: it discards ~49 CORRECT answers
#       to fix 3 wrong ones, and ~10 of the 49 have real consequences.
#       `HAVE_GMTIME_R`/`HAVE_LOCALTIME_R` falling away makes date.c use the
#       NON-REENTRANT `gmtime`/`localtime` in a SQLITE_THREADSAFE=1 build;
#       `HAVE_FDATASYNC` falling away makes os_unix.c `#define fdatasync fsync`;
#       `HAVE_USLEEP` falling away drops sqlite3_sleep to 1-SECOND granularity,
#       which the timing-sensitive corpus WILL notice. The same blunt drop is
#       acceptable on WINDOWS only because `SQLITE_OS_WIN` re-configures the whole
#       os_win.c path and none of those Unix answers is consulted there.
#
# What is staged instead is a header that answers all ~49 identical rows with the
# deriving host's answers and the ones that VARY with the leg's own declaration.

def configure_stage_key(leg):
    """The staged-configure-header directory name for this leg: its TARGET OS.

    Raises rather than defaulting. A leg whose spec does not name an OS this
    resolver knows has no derivable configure answers either, so a silent key
    (`""`, `"unknown"`) would put its header wherever the driver's path join
    happened to land and compile it against somebody else's answers."""
    spec = leg.get("spec", "")
    target_os = spec_target_os(spec)
    if target_os not in TARGET_OS_NAMES:
        raise LegError(
            "leg '%s': cannot derive a target OS from spec %r (format names are "
            "<container><bits>-<arch>-<os>-<kind>; got %r, known: %s) — so there "
            "is no target this leg's sqlite configure answers could be staged "
            "for, and compiling it against the DERIVING host's answers is the "
            "defect this key closes"
            % (leg.get("label", "?"), spec, target_os, ", ".join(TARGET_OS_NAMES)))
    return target_os


def configure_answers(leg):
    return dict(leg.get("build", {}).get("configureAnswers", {}))


def configure_stages(legs):
    """key -> answers, in first-declared order.

    RAISES on a conflict, exactly as `header_stages` does: two legs targeting one
    OS share ONE staged `sqlite_cfg.h`, and silently picking either declaration
    would put a leg back where this anchor started — compiled against a
    configuration measured on somebody else's machine."""
    stages, owner = {}, {}
    for leg in legs:
        key = configure_stage_key(leg)
        answers = configure_answers(leg)
        if key in stages and stages[key] != answers:
            raise LegError(
                "legs '%s' and '%s' both target OS '%s' — so they share ONE "
                "staged sqlite_cfg.h — but declare DIFFERENT configureAnswers "
                "(%r vs %r). One stage cannot be two headers; reconcile them, "
                "because at most one of the two can be a fact about that target."
                % (owner[key], leg.get("label"), key, stages[key], answers))
        stages.setdefault(key, answers)
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


def load_catalogue_doc(path=CATALOGUE):
    """The WHOLE catalogue document, for the run-wide blocks that are not legs
    (`stageBuild`, `environmentProbes`). Version-checked through load_catalogue
    first so a run-wide block can never be read out of a catalogue this resolver
    does not understand."""
    load_catalogue(path)
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


# ── THE STAGE BUILD CONFIGURATION — one declaration, both drivers ────────────
# WHICH sqlite the corpus tests is a property of the RUN, not of a leg: one
# staged tree feeds all five legs, so the capability set cannot live per-leg and
# must not live per-driver. It had already gone wrong the per-driver way — the
# CLI recipe carried -DSQLITE_ENABLE_FTS4 -DSQLITE_ENABLE_RTREE and the
# testfixture recipe, built from the same tree in the same run, carried neither.
#
# ★ THE CHARSET CHECKS ARE NOT PEDANTRY. These values are interpolated into a
#   `./configure` argv and a `make VAR=…` command line by two drivers, one of
#   which reaches them through a PowerShell here-string. A value carrying a
#   quote or a space would either be silently split into two flags or would
#   escape its quoting entirely, and the failure would present as "the capability
#   did not take" — indistinguishable from upstream retiring the flag. So the
#   shapes are refused HERE, once, where both drivers inherit the refusal.
_STAGE_FLAG_RE = re.compile(r"^--[A-Za-z0-9][A-Za-z0-9._-]*$")
_STAGE_DEFINE_RE = re.compile(r"^SQLITE_[A-Z0-9_]+$")
_STAGE_NAME_RE = re.compile(r"^[A-Za-z0-9_]+$")
_STAGE_FILE_RE = re.compile(r"^[A-Za-z0-9_.-]+$")


def stage_build(path=CATALOGUE):
    """The declared sqlite stage build configuration. Raises LegError rather
    than returning a default: a MISSING declaration must never read as "no
    extensions were wanted", which is precisely the state that let 362 of 1,241
    corpus files complete without asserting anything."""
    with open(path, "r", encoding="utf-8") as f:
        doc = json.load(f)
    sb = doc.get("stageBuild")
    if not isinstance(sb, dict):
        raise LegError(
            "leg catalogue %s declares no `stageBuild` block. It is REQUIRED, "
            "and it is required precisely because its absence is silent: the "
            "tree still configures, still builds and still reports every file "
            "as completed, while a third of them return at their first "
            "`ifcapable` gate having asserted nothing "
            "[D-HARNESS-CORPUS-FILES-COMPLETE-WITHOUT-ASSERTING-BECAUSE-"
            "CAPABILITIES-ARE-OFF]." % path)

    def _list(key, pattern, what):
        raw = sb.get(key)
        if not isinstance(raw, list) or not raw:
            raise LegError("stageBuild.%s must be a non-empty list of %s"
                           % (key, what))
        for item in raw:
            if not isinstance(item, str) or not pattern.match(item):
                raise LegError(
                    "stageBuild.%s: %r is not a well-formed %s. Both drivers "
                    "interpolate these into a shell command line, so a value "
                    "carrying whitespace or quoting would be split or would "
                    "escape its quotes, and the result would look exactly like "
                    "the capability having no effect." % (key, item, what))
        if len(set(raw)) != len(raw):
            raise LegError("stageBuild.%s repeats an entry" % key)
        return list(raw)

    flags = _list("configureFlags", _STAGE_FLAG_RE, "configure flag (--name)")
    defines = _list("optionDefines", _STAGE_DEFINE_RE, "bare SQLITE_* macro name")
    required = _list("requiredDefines", _STAGE_DEFINE_RE, "bare SQLITE_* macro name")
    # Every optionDefine is by construction something we asked for by name, so
    # it must also be something we verify arrived. Anything else would let the
    # one mechanism with no configure-side confirmation go unchecked.
    missing = [d for d in defines if d not in required]
    if missing:
        raise LegError(
            "stageBuild: %s appears in optionDefines but not in requiredDefines. "
            "optionDefines is the mechanism with NO configure-side "
            "confirmation — it is passed straight through as `make OPTIONS=…` — "
            "so it is the one that most needs the derived-recipe assertion."
            % ", ".join(sorted(missing)))
    wit = sb.get("capabilityWitnesses")
    if not isinstance(wit, dict) or not wit:
        raise LegError("stageBuild.capabilityWitnesses must be a non-empty "
                       "object of <ifcapable name>: {file, define}")
    witnesses = {}
    for cap, entry in sorted(wit.items()):
        if not _STAGE_NAME_RE.match(cap or ""):
            raise LegError("stageBuild.capabilityWitnesses: %r is not an "
                           "`ifcapable` capability name" % cap)
        if not isinstance(entry, dict):
            raise LegError("stageBuild.capabilityWitnesses[%s] must be an "
                           "object {file, define}" % cap)
        stem, define = entry.get("file"), entry.get("define")
        if not isinstance(stem, str) or not _STAGE_FILE_RE.match(stem):
            raise LegError("stageBuild.capabilityWitnesses[%s].file: %r is not "
                           "a test file stem (no directory, no extension)"
                           % (cap, stem))
        if not isinstance(define, str) or not _STAGE_DEFINE_RE.match(define):
            raise LegError("stageBuild.capabilityWitnesses[%s].define: %r is "
                           "not a bare SQLITE_* macro name" % (cap, define))
        # The pairing is DECLARED, never inferred from the capability name:
        # `mem5` ↔ SQLITE_ENABLE_MEMSYS5 share no substring, so every naming
        # heuristic has to be weakened to accept it — at which point it stops
        # catching the case it was written for.
        if define not in required:
            raise LegError(
                "stageBuild.capabilityWitnesses[%s] expects %s, which is not in "
                "requiredDefines. Its gate could then only ever be red: the "
                "witness is waiting on a capability this configuration never "
                "turns on." % (cap, define))
        witnesses[cap] = {"file": stem, "define": define}
    wit = witnesses
    return {
        "configureFlags": flags,
        "optionDefines": defines,
        "requiredDefines": sorted(required),
        "capabilityWitnesses": dict(wit),
        # The single string both drivers hand to `make`. Assembled HERE so the
        # `-D` prefix is applied in exactly one place: a driver that spelled it
        # itself could disagree, and `make OPTIONS=SQLITE_ENABLE_STAT4` (no -D)
        # is accepted by make, reaches the compiler as a bare token, and is
        # ignored — a capability lost with no error anywhere.
        "makeOptions": " ".join("-D" + d for d in defines),
    }


def stage_build_sh(sb):
    """The stage build configuration as shell assignments. build-and-test.sh
    `eval`s this text, so it must be assignments and NOTHING else — asserted by
    self_test through sh_statements(), the same guarantee emit_sh() carries.
    A separate function rather than inline in main() so the test can execute the
    SHIPPED emitter instead of re-typing what it believes the emitter does."""
    return ("DSS_STAGE_CONFIGURE_FLAGS=%s\n"
            "DSS_STAGE_MAKE_OPTIONS=%s\n"
            "DSS_STAGE_REQUIRED_DEFINES=%s\n"
            "DSS_STAGE_WITNESSES=%s\n" % (
                # shlex.quote on every value, even though stage_build() has
                # already refused everything that would need quoting. The
                # validation is the guarantee; the quoting is what keeps a FUTURE
                # loosening of it from becoming a shell injection into a driver.
                shlex.quote(" ".join(sb["configureFlags"])),
                shlex.quote(sb["makeOptions"]),
                shlex.quote(" ".join(sb["requiredDefines"])),
                shlex.quote(" ".join("%s=%s" % (c, w["file"]) for c, w
                                     in sorted(sb["capabilityWitnesses"].items())))))


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


# ── THE PATHS A LIBRARY BAKES IN ────────────────────────────────────────────
#
# An ABSOLUTE PATH WITH AT LEAST TWO COMPONENTS, POSIX or Windows-drive spelled,
# not preceded by another path character (so `abc/def/ghi` inside a longer word
# does not produce a phantom `/def/ghi`). ✔MEASURED 2026-08-06 across the six
# libraries this catalogue stages: 14 tokens in each MacPorts libtcl slice, 2 in
# each libz, 14 in Debian's libtcl8.6.so, and ZERO in conda-forge's tcl86t.dll
# and zlib.dll — i.e. the grammar is tight enough that the declaration is a
# readable list rather than a wall of noise, and loose enough to have caught the
# one path that mattered (`/opt/local/lib/tcl8.6`).
#
# ⚠ THIS IS A STRING SCAN, DELIBERATELY, AND NOT A SECTION WALK. A data path can
# be built at run time from pieces, and it can live in __TEXT, __DATA or a
# packager's config blob; a scan that only understood one container's string
# section would be a check that passes for the wrong reason on the next format.
# Its cost is the false positives above, and every one of them is DECLARED once
# against a PINNED digest, so the list can never drift under the declaration.
_ABS_PATH_TOKEN_RE = re.compile(
    rb"(?<![A-Za-z0-9_+.\-\\/])(?:[A-Za-z]:[\\/]|/)"
    rb"[A-Za-z0-9_+.-]+(?:[\\/][A-Za-z0-9_+.-]+)+")


def embedded_absolute_paths(blob):
    """Every absolute path token a library carries, sorted. Bytes in, strings
    out — the caller compares them against the DECLARATION."""
    return sorted({m.group().decode("utf-8", "replace")
                   for m in _ABS_PATH_TOKEN_RE.finditer(blob)})


def loader_dependency_paths(blob):
    """The paths a library declares as LIBRARIES FOR THE LOADER, as opposed to
    data it reads itself.

    These are subtracted from the audit above because they are not a data
    directory by any reading — they are the loader's business, and a build that
    demanded they be declared as data would be demanding nonsense.

    Mach-O is the only container that spells them as PATHS: LC_ID_DYLIB,
    LC_LOAD*_DYLIB and LC_RPATH. An ELF's DT_NEEDED is a bare soname and a PE's
    import table a bare DLL name, so neither can produce an absolute-path token
    at all — and an ELF DT_RPATH/DT_RUNPATH, which CAN, is deliberately NOT
    excused here: it is a path the packager chose, exactly the kind of thing this
    audit exists to put in front of a reader."""
    import struct
    out = []
    if len(blob) < 32:
        return out
    magic, = struct.unpack("<I", blob[:4])
    if magic != 0xFEEDFACF:
        return out
    ncmds, = struct.unpack("<I", blob[16:20])
    p = 32
    #     LC_ID_DYLIB  LC_LOAD_DYLIB  LC_LOAD_WEAK  LC_REEXPORT  LC_LAZY  LC_UPWARD
    dylib = (0xD, 0xC, 0x80000018, 0x8000001F, 0x20, 0x80000023)
    for _ in range(ncmds):
        if p + 8 > len(blob):
            break
        cmd, cmdsize = struct.unpack("<II", blob[p:p + 8])
        if cmdsize < 8 or p + cmdsize > len(blob):
            break
        if cmd in dylib or cmd in (0x1C, 0x8000001C):      # + LC_RPATH
            nameoff, = struct.unpack("<I", blob[p + 8:p + 12])
            if 8 <= nameoff < cmdsize:
                out.append(blob[p + nameoff:p + cmdsize]
                           .split(b"\0")[0].decode("utf-8", "replace"))
        p += cmdsize
    return out


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
    they land in, the (as-name -> importName) map and the staged data
    directories. Pure — no filesystem, no network — so the self-test can assert
    the plan on any machine, and so the FAILURE path can answer with the same
    fields the success path does."""
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
                "runtimeCopy": m.get("runtimeCopy", ""),
                "embeddedPaths": [{"path": e.get("path", ""),
                                   "kind": e.get("kind", ""),
                                   "role": e.get("role", "")}
                                  for e in m.get("embeddedPaths", [])],
                "dataDirs": [{"member": d.get("member", ""),
                              "as": d.get("as", ""),
                              "role": d.get("role", ""),
                              "path": os.path.join(cdir, d.get("as", ""))}
                             for d in m.get("dataDirs", [])],
                "path": os.path.join(cdir, m.get("as", "")),
            } for m in a.get("members", [])],
        })
    return {
        "leg": leg.get("label", ""),
        "targetArch": target_arch,
        "cacheDir": cdir,
        "downloadDir": ddir,
        "archives": archives,
        # ── THE CONTRACT FIELD ──────────────────────────────────────────────
        # Where Tcl's SCRIPT LIBRARY (init.tcl and friends) was staged for this
        # leg, or "" when the leg stages none. A driver reads THIS and sets
        # `TCL_LIBRARY`; it never joins a path itself, for the same reason it
        # never spells `--resolve-library` itself.
        #
        # ★ IT IS COMPUTED HERE, IN THE PURE PLAN, AND THE RESULT COPIES IT —
        # so the success return and the failure return CANNOT carry different
        # field sets (D-HARNESS-PINNED-ARCHIVE-FAILURE-RETURN-OMITS-ACQUIRED:
        # "a function whose SUCCESS return and FAILURE return carry different
        # field sets is a silent-omission generator"). There is one record
        # shape, `acquisition_record`, and one place the value comes from.
        "scriptLibraryDir": _script_library_dir(archives),
    }


def _script_library_dir(archives):
    """The staged Tcl script library among a plan's data directories, or "".

    Refuses on TWO of them rather than picking: `TCL_LIBRARY` is one directory,
    so a leg that staged two script libraries has no answer and the driver must
    not be handed whichever came first."""
    found = [d["path"] for a in archives for m in a["members"]
             for d in m["dataDirs"] if d["role"] == "tclScriptLibrary"]
    if len(found) > 1:
        raise LegError(
            "this leg stages %d Tcl script libraries (%s), but TCL_LIBRARY names "
            "exactly one directory. Declare `role: tclScriptLibrary` on the one "
            "the fixture must use and `generic` on the rest."
            % (len(found), ", ".join(found)))
    return found[0] if found else ""


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
#   2 -> 3: staged DATA DIRECTORIES (Tcl's script library) and the per-member
#           `runtimeCopy` / `embeddedPaths` declarations join the stamp. A cache
#           written before them holds the libraries but not the scripts, and a
#           stamp that did not mention them would call it fresh.
ACQUIRE_STAMP_VERSION = 3


def acquisition_record(plan_, libraries=(), from_cache=False, remediated=(),
                       loader_dependencies=(), error=""):
    """THE acquisition result — ONE record shape, built in ONE place.

    D-HARNESS-PINNED-ARCHIVE-FAILURE-RETURN-OMITS-ACQUIRED closed a bug where a
    failure return omitted a field its success twin carried, and its own closing
    note names the durable fix: "make the acquisition result ONE record type
    populated on both paths, so a field cannot be forgotten on the branch nobody
    exercises". This is that type. `--acquire` prints it on success AND on
    failure (with `error` set and the rc still naming the failure), so a driver
    that needs `scriptLibraryDir` most — because acquisition just failed and it
    is about to report why — is not handed a different shape."""
    return {
        "leg": plan_["leg"],
        "targetArch": plan_["targetArch"],
        "cacheDir": plan_["cacheDir"],
        "scriptLibraryDir": plan_["scriptLibraryDir"],
        "libraries": list(libraries),
        "fromCache": bool(from_cache),
        "remediated": list(remediated),
        # The library paths each staged library hands to the LOADER, reported
        # rather than audited (see loader_dependency_paths). ⚠ It is worth
        # READING: MacPorts' libtcl8.6.dylib carries LC_LOAD_DYLIB
        # `/opt/local/lib/libz.1.dylib`, a prefix no target Mac has, and it loads
        # only because dyld falls back to /usr/lib.
        "loaderDependencies": list(loader_dependencies),
        "error": error,
    }


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
                                   "importName": m["importName"],
                                   "runtimeCopy": m["runtimeCopy"],
                                   "embeddedPaths": m["embeddedPaths"],
                                   "dataDirs": [{"member": d["member"],
                                                 "as": d["as"],
                                                 "role": d["role"]}
                                                for d in m["dataDirs"]]}
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
                for d in m["dataDirs"]:
                    # A STAGED DIRECTORY IS PINNED THE SAME WAY A FILE IS — by
                    # ONE digest over the whole tree (every relative path and
                    # every file's own sha256). Anything added, removed or
                    # edited under it changes that digest, so the script library
                    # cannot rot into something the pinned library was never
                    # shipped with.
                    if not os.path.isdir(d["path"]):
                        remediated.append("%s/: absent" % d["as"])
                        fresh = False
                    elif _sha256_tree(d["path"]) != have.get(d["as"] + "/"):
                        remediated.append(
                            "%s/: contents do not match the digest recorded "
                            "when they were extracted" % d["as"])
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
            with _open_archive(dl, a["archiveFormat"], a["url"]) as tf:
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
                    for d in m["dataDirs"]:
                        _extract_tree(tf, d["member"], d["path"], a["url"])
        with open(stamp_path, "w", encoding="utf-8") as f:
            _json.dump({"declaration": want_stamp,
                        "materialised": dict(
                            [(m["as"], _sha256_file(m["path"]))
                             for a in plan_["archives"] for m in a["members"]]
                            + [(d["as"] + "/", _sha256_tree(d["path"]))
                               for a in plan_["archives"] for m in a["members"]
                               for d in m["dataDirs"]])},
                       f, indent=1, sort_keys=True)

    libraries = []
    loader_deps = []
    for a in plan_["archives"]:
        for m in a["members"]:
            with open(m["path"], "rb") as f:
                blob = f.read()
            _audit_embedded_paths(plan_["leg"], m, blob)
            for dep in loader_dependency_paths(blob):
                loader_deps.append("%s -> %s" % (m["as"], dep))
            libraries.append({
                "as": m["as"],
                "path": m["path"],
                "importName": m["importName"],
                "runtimeCopy": m["runtimeCopy"],
                # The identity being DISPLACED, so a build log states the
                # substitution instead of hiding it.
                "embeddedIdentity": _macho_install_name(blob[:1 << 16]),
                "sourceUrl": a["url"],
                "archiveSha256": a["sha256"],
                "fileSha256": _sha256_file(m["path"]),
                # The runtime data this library needs, and where it was put.
                # Reported per library because "which staged directory belongs
                # to which library?" has to have an answer in the build log.
                "dataDirs": [{"as": d["as"], "role": d["role"],
                              "path": d["path"],
                              "treeSha256": _sha256_tree(d["path"])}
                             for d in m["dataDirs"]],
            })
    return acquisition_record(plan_, libraries=libraries, from_cache=fresh,
                              remediated=remediated,
                              loader_dependencies=loader_deps)


def _audit_embedded_paths(label, member, blob):
    """★ THE GUARD THIS ANCHOR EXISTS FOR — D-HARNESS-ACQUIRED-TCL-DYLIB-HAS-NO-
    SCRIPT-LIBRARY.

    A `.dylib`/`.so`/`.dll` is not always self-contained. Tcl bakes in its script
    library, ICU its data bundle, tzdata its zone directory — and acquisition
    obtains the CODE and silently leaves the path dangling. Nothing at build time
    sees it: the link succeeds, the binary runs, and it dies only on the code
    path that touches the data. It cost this project its first Mach-O unit
    corpus, and the failure named no file.

    So every absolute path a staged library carries is put in front of a reader
    ONCE, against a PINNED digest, and after that the check is mechanical:

      · a path in the bytes that the declaration does not mention  -> REFUSE
      · a path the declaration mentions that is not in the bytes   -> REFUSE
      · a `runtime-data` path with no staged directory             -> REFUSE

    The second rule is what stops the declaration going vacuous: a list of paths
    nobody checks against the file is a list that will eventually describe a
    library we no longer ship.

    ⚠ IT APPLIES ONLY WHERE THE STAGED COPY IS THE ONE THAT RUNS. When the target
    supplies its own copy, our file is a build-time stand-in read for its export
    table and NEVER LOADED — its baked-in directories belong to that machine, and
    staging ours would ship the wrong data with a straight face."""
    if member["runtimeCopy"] != "staged-beside-artefact":
        return
    who = "leg '%s' :: %s" % (label, member["as"])
    declared = {e["path"]: e for e in member["embeddedPaths"]}
    excused = set(loader_dependency_paths(blob))
    found = [p for p in embedded_absolute_paths(blob) if p not in excused]

    undeclared = [p for p in found if p not in declared]
    if undeclared:
        raise LegError(
            "%s bakes in %d absolute path(s) the catalogue does not declare:\n"
            "    %s\n"
            "  This library is STAGED BESIDE THE ARTEFACT, so the copy that runs "
            "is the copy we acquired — and a directory it reads at run time that "
            "nobody staged is a failure NOTHING AT BUILD TIME CAN SEE (the link "
            "succeeds, the binary runs, and it dies only on the code path that "
            "touches the data).\n"
            "  Declare each one under `embeddedPaths` as `runtime-data` (with a "
            "`dataDirs` entry that stages it) or as `inert` (with a `$why` "
            "saying what makes it unread). Both are claims a reviewer can check; "
            "silence is not."
            % (who, len(undeclared), "\n    ".join(undeclared)))

    missing = [p for p in declared if p not in found]
    if missing:
        raise LegError(
            "%s declares %d embedded path(s) that are NOT in the file:\n    %s\n"
            "  The declaration describes a library this leg no longer stages. A "
            "path list nobody checks against the bytes is how this guard would "
            "come to pass for the wrong reason."
            % (who, len(missing), "\n    ".join(sorted(missing))))

    staged_roles = {d["role"] for d in member["dataDirs"]}
    for path, entry in sorted(declared.items()):
        if entry["kind"] != "runtime-data":
            continue
        if entry["role"] not in staged_roles:
            raise LegError(
                "%s bakes in the RUNTIME DATA directory %s (role %r) and this "
                "leg stages no directory for it.\n  The acquired library is the "
                "one that runs, so that path must resolve on the TARGET — and it "
                "is the packager's prefix, which the target does not have. Add a "
                "`dataDirs` entry with role %r."
                % (who, path, entry["role"], entry["role"]))


def _open_archive(path, fmt, url):
    """The declared archive, opened as a tar. ONE function, so `deb` is a
    CONTAINER this module understands rather than a second extraction path.

    ⚠ `dpkg-deb` is deliberately not used: it is the tool the old
    `ubuntu-ports-arm64` provider shelled out to, and no Windows or macOS host
    has it — which is precisely how "any host builds any target" came to hold
    only on Linux."""
    import io
    import tarfile
    if fmt not in ARCHIVE_FORMATS:
        raise LegError("archive %s declares archiveFormat %r, which this "
                       "resolver cannot open (known: %s)"
                       % (url, fmt, ", ".join(sorted(ARCHIVE_FORMATS))))
    if fmt != "deb":
        return tarfile.open(path, ARCHIVE_FORMATS[fmt])
    with open(path, "rb") as f:
        blob = f.read()
    if blob[:8] != b"!<arch>\n":
        raise LegError("%s is declared `deb` but does not start with the `ar` "
                       "magic `!<arch>` — the declaration and the file disagree "
                       "about what was downloaded." % url)
    off, seen = 8, []
    while off + 60 <= len(blob):
        header = blob[off:off + 60]
        name = header[0:16].decode("ascii", "replace").strip().rstrip("/")
        try:
            size = int(header[48:58].decode("ascii", "replace").strip())
        except ValueError:
            raise LegError("%s: malformed `ar` member header at offset %d"
                           % (url, off))
        body = off + 60
        seen.append(name)
        if name.startswith("data.tar"):
            if name not in DEB_PAYLOAD_MODES:
                raise LegError(
                    "%s carries its payload as `%s`, which this resolver cannot "
                    "open (known: %s).\n  ⚠ `data.tar.zst` is the common case "
                    "here and it is a HARD wall, not an omission: zstd entered "
                    "the Python standard library in 3.14, and this project's own "
                    "hosts run 3.12 — so a zstd route would be a capability that "
                    "exists on one host and not another, which is the same defect "
                    "one level down from the one this mechanism exists to end.\n"
                    "  Pin an archive whose payload is one of the above (Debian's "
                    "are `data.tar.xz`; Ubuntu's are not)."
                    % (url, name, ", ".join(sorted(DEB_PAYLOAD_MODES))))
            return tarfile.open(fileobj=io.BytesIO(blob[body:body + size]),
                                mode=DEB_PAYLOAD_MODES[name])
        off = body + size + (size & 1)
    raise LegError("%s: no `data.tar.*` member in the `ar` archive (members: %s)"
                   % (url, ", ".join(seen) or "<none>"))


def _sha256_tree(path):
    """ONE digest over a whole directory: every relative path and every file's
    own sha256, in sorted order. A staged data directory has to be pinned exactly
    as a staged file is — otherwise the library is content-addressed and the
    scripts it loads are not, which is half a pin."""
    import hashlib
    h = hashlib.sha256()
    entries = []
    for base, dirs, files in os.walk(path):
        dirs.sort()
        for name in sorted(files):
            full = os.path.join(base, name)
            rel = os.path.relpath(full, path).replace("\\", "/")
            entries.append((rel, _sha256_file(full)))
    for rel, digest in sorted(entries):
        h.update(rel.encode("utf-8"))
        h.update(b"\0")
        h.update(digest.encode("ascii"))
        h.update(b"\n")
    return h.hexdigest()


def _extract_tree(tf, member, dest, url):
    """A DIRECTORY of the archive, materialised under `dest`.

    Every regular file below `member/` is written; nothing is filtered, because a
    data directory that is silently missing a file is the same class of defect as
    one that is missing entirely. A symlink or a device node REFUSES rather than
    being skipped — Tcl's script library has neither (✔MEASURED: 225 files, 4
    subdirectories, 0 links in the MacPorts archive), so one appearing means the
    archive is not the thing that was declared.

    ⚠ Member paths are checked to stay UNDER `dest`: a tar can name `../` and
    this code runs with the operator's own privileges.

    ★ IT BUILDS INTO A PER-PROCESS TEMPORARY AND SWAPS, for the two reasons the
    download's `.part` file already has: legs run CONCURRENTLY here, and a run
    that fails half way must not leave a WORKING cache destroyed behind it. The
    first draft cleared the destination first and a deliberately-broken
    declaration wiped a good script library on its way to refusing — loud, but
    it took a good cache with it."""
    prefix = member.rstrip("/") + "/"
    staging = "%s.%d.part" % (dest, os.getpid())
    if os.path.isdir(staging):
        shutil.rmtree(staging)
    wrote = 0
    for entry in tf.getmembers():
        name = entry.name[2:] if entry.name.startswith("./") else entry.name
        if not name.startswith(prefix):
            continue
        rel = name[len(prefix):]
        if entry.isdir():
            continue
        if not entry.isfile():
            raise LegError(
                "%s :: %s%s is a %s, not a regular file. A data directory is "
                "staged whole; skipping an entry would ship a library its own "
                "runtime data cannot satisfy."
                % (os.path.basename(url), prefix, rel,
                   "symlink" if entry.issym() else
                   "hard link" if entry.islnk() else "special file"))
        out = os.path.normpath(os.path.join(staging, rel))
        if not out.startswith(staging + os.sep):
            raise LegError("%s :: %s escapes the staging directory (%s)"
                           % (os.path.basename(url), name, out))
        os.makedirs(os.path.dirname(out), exist_ok=True)
        src = tf.extractfile(entry)
        if src is None:
            raise LegError("%s :: %s could not be read from the archive"
                           % (os.path.basename(url), name))
        with open(out, "wb") as f:
            shutil.copyfileobj(src, f)
        wrote += 1
    if not wrote:
        if os.path.isdir(staging):
            shutil.rmtree(staging)
        raise LegError(
            "%s declares the data directory %r, and the archive has NO files "
            "under it. An empty stage is worse than none: TCL_LIBRARY would "
            "point at a directory with no init.tcl in it and the failure would "
            "surface as `Can't find a usable init.tcl`, one interpreter later."
            % (os.path.basename(url), member))
    if os.path.isdir(dest):
        shutil.rmtree(dest)
    os.replace(staging, dest)


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


def _run_translator(argv, timeout=None):
    """(rc, stdout, stderr). rc is taken DIRECTLY off the process, and BOUNDED.

    ⚠ A DEADLINE HERE IS A NAMED REFUSAL, NOT AN EXIT STATUS, and that is the
    one way this spawn differs from the kernel probe's. `kernel_probe_argv`
    calls translate_path TWICE — the script and the catalogue — BEFORE the
    bounded probe child is ever started, so an unbounded wait here hung `--plan`,
    step 1 of every corpus run, with no timeout and no diagnostic, in the one
    place `measure_kernel_environments`' `except LegError` could not see it.
    Raising is what puts the failure back inside that handler, where the kernel
    becomes `unreachable`, its verdicts INDETERMINATE and its rows INACTIVE.

    ⚠ AND `check_launcher` SPAWNS THROUGH HERE TOO — every launcher `probeArgv`
    does. A deadline must NOT read as "the requirement is absent" there: absent
    is a MEASUREMENT ("wsl.exe is not installed"), and a wedged distro is the
    ABSENCE of one. That is the same asymmetry `unreachable` carries one
    function along, and quietly answering "missing" would skip a leg for a
    reason nobody observed.

    `timeout` is exposed so --self-test can drive the deadline against a real
    child in under a second; no production caller passes it."""
    if timeout is None:
        timeout = RESOLVER_SPAWN_BUDGET_SECONDS
    rc, out, err = _captured(argv, timeout)
    if rc is None:
        raise LegError(
            "`%s` gave no answer within %g s and was killed. This spawn is "
            "BOUNDED because it runs during plan RESOLUTION — step 1 of every "
            "corpus run — and `wsl.exe` on a wedged or cold-booting distro "
            "blocks indefinitely: an unbounded wait here is a harness that "
            "hangs with no diagnostic instead of one that reports a kernel it "
            "could not reach. Neither the path nor the launcher requirement "
            "this asked about has an answer, and neither may be guessed."
            % (" ".join(argv), timeout))
    return rc, out, err


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

def plan_leg(leg, host_os, host_arch, available, kernel_measurements=None):
    """Resolve ONE leg against ONE host.

    Build is unconditional. Run is answered from the leg's declaration.
    Returns a dict; `run.verdict` is populated ONLY for a non-run (it is one of
    the skip names) — a planned run has verdict None because the verdict is the
    OUTCOME (`ran` / `poisoned`) and only the driver can know it.

    `kernel_measurements` is an INJECTED MEASUREMENT, exactly as `available` is:
    this function stays pure, and a plan is therefore the same on every machine
    given the same measurements. `None` means nothing was measured, which makes
    every conditional confound row INACTIVE and stamps the leg `confoundGating:
    unprobed` — see leg_confound_decisions.

    ⓘ AND THAT PURITY IS WHY THE MEASUREMENT IS TWO-PHASE AT THE CLI: which kernel
    a leg executes in is decided HERE, so the caller resolves once with `None` to
    learn the kernels, measures them, and resolves again. Measuring from inside
    this function would make a plan depend on the machine that printed it.
    """
    spec = leg["spec"]
    arch = spec_target_arch(spec)
    run_on = leg.get("runOn", [])
    os_ok = host_os in run_on
    arch_ok = host_arch == arch

    # `pathTranslation` is ALWAYS present and ALWAYS a declared verb — "none" for
    # a native run, because "this driver's paths are the ones the callee sees" is
    # a claim worth stating rather than an absence a driver has to interpret.
    # `runFilesystem` seeds to "driver" for the same reason `pathTranslation`
    # seeds to "none": on a NATIVE run the process is this machine's own and it
    # writes onto this driver's own filesystem — a claim, and a true one, rather
    # than an absence. Only a LAUNCHER can make it false, and only by declaring so.
    # `requires` seeds to [] for the same reason, and it means the same thing it
    # means in the catalogue: NOTHING beyond what is already here is needed. On a
    # native run that is true by construction — there is no launcher to need
    # anything — and the rows are RESOLVED (paths expanded, probe argv built) but
    # never EXECUTED here, because `plan_leg` is pure and injectable and a plan
    # that consulted the filesystem would stop being reproducible.
    # `sameIsa` RECORDS `arch_ok`, which this function has always computed and
    # always thrown away. `fidelity` is the answer it makes sayable: does the
    # artefact execute on ITS OWN INSTRUCTION SET on this host?
    # [D-HARNESS-RUN-FIDELITY-IS-COMPUTED-BUT-NEITHER-RECORDED-NOR-SELECTABLE]
    # ★★ WHY A THIRD VALUE AND NOT A BOOLEAN `emulated`. `mode == "launched"`
    # conflates two situations that are not the same evidence, and the catalogue
    # already says so in prose it could not enforce: elf64-arm64 declares BOTH
    # `(windows, arm64) -> wsl.exe` — same ISA, real hardware, foreign kernel —
    # and `(windows, x86_64) -> wsl.exe -e qemu-aarch64` — genuine translation.
    # A boolean keyed on "went through a launcher" calls both emulated, which is
    # the exact error legs.json's `scopeLegacyBlocker` documents as unfixable
    # from inside the `scope` vocabulary. Keyed on the ISA instead, all four
    # cases fall out right, INCLUDING Rosetta (`arch -x86_64` on darwin/arm64 is
    # os_ok with arch_ok false -> emulated, correctly).
    # ⓘ `fidelity` is populated ONLY FOR A RUN, exactly as `verdict` is populated
    # only for a NON-run — the two are complements, and a leg that never executes
    # has no fidelity to report rather than a defaulted one a reader would trust.
    run = {"mode": None, "launcher": [], "env": {}, "verdict": None, "detail": "",
           "pathTranslation": "none", "pathTranslator": [],
           "envTransfer": "inherit", "runFilesystem": "driver", "requires": [],
           "sameIsa": arch_ok, "fidelity": None}

    if os_ok and arch_ok:
        run["mode"] = "native"
        run["fidelity"] = "native"
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
            # The launcher's FILESYSTEM, declared beside the other two. Same
            # refusal on an unknown verb, same reason: `driver` claims that the
            # launched process writes where this driver writes, and a launcher
            # that crosses into another OS's kernel usually does not.
            fs_verb = entry.get("runFilesystem", "driver")
            run_filesystem(fs_verb)
            # WHAT THE LAUNCHER NEEDS BEYOND argv[0], resolved but NOT executed.
            # Emitted on BOTH outcomes below: a driver reporting an unusable
            # launcher wants the list as much as one about to run — and a caller
            # that only ever saw the rows on the happy path would be reading a
            # different declaration from the one it is failing on.
            requires = resolve_launcher_requirements(entry, leg["label"])
            run["requires"] = requires
            # A launcher is USABLE only if its translator is too. They are one
            # capability: `wsl.exe` present with no distro behind it resolves
            # neither, and the honest verdict for that machine is the
            # environmental skip, not a run that dies on its first path.
            needed = [command] + ([translator] if translator else [])
            missing = [c for c in needed if not launcher_available(c, available)]
            if not missing:
                run["mode"] = "launched"
                # SAME ISA through a launcher is a FOREIGN KERNEL, not emulation:
                # wsl.exe on an arm64 Windows host executes aarch64 instructions
                # on aarch64 hardware. Calling that `emulated` is what the
                # mode-only vocabulary cannot avoid.
                run["fidelity"] = "foreign-kernel" if arch_ok else "emulated"
                run["launcher"] = command
                run["env"] = dict(entry.get("env", {}))
                run["pathTranslation"] = verb
                run["pathTranslator"] = translator
                run["envTransfer"] = env_verb
                run["runFilesystem"] = fs_verb
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

    # ── THE MEASUREMENT IN FORCE FOR *THIS LEG*, AND THE ROWS IT DECIDES ─────
    # ★ ONCE. `leg_confound_decisions` was called three times here (for
    # `confounds`, for `confoundDecisions` and again for `confoundReport`) — pure
    # and deterministic, so never divergent, but "one fact, one owner" cannot be
    # read off three call sites, and the third call is the one that took the RAW
    # verdicts and a `run` the other two did not have.
    # ★★ AND IT IS BUILT AFTER `run` DELIBERATELY: the gate needs the leg's
    # RESOLVED launcher to know WHICH KERNEL'S measurement this leg reads, which is
    # precisely what the decision function used not to have.
    # [D-HARNESS-ENVIRONMENT-PROBE-MEASURES-THE-DRIVERS-KERNEL-NOT-THE-LAUNCHED-ONE]
    gate = probe_gate(kernel_measurements, run)
    decisions = leg_confound_decisions(leg, gate)

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
        # `configStageKey` is its twin for the staged `sqlite_cfg.h` (the leg's
        # TARGET OS — see configure_stages() for why that key and not the
        # transform), so the drivers join a key onto a root there too and neither
        # of them decides which target's configure answers a leg compiles against.
        "build": dict(build, attempt=True, headerStageKey=header_stage_key(leg),
                      configStageKey=configure_stage_key(leg)),
        # THE PATTERNS, resolved to the wire grammar both drivers already speak,
        # and the ROWS behind them so a driver can print WHY it excused something
        # instead of only that it did. Host-free: a confound is a property of the
        # LEG (its target, its libraries, its upstream), which is exactly the
        # claim the old per-driver lists could not make.
        # ★ THE GATED ANSWER, THE ROWS BEHIND IT, THE PER-ROW DECISION AND THE
        # REPORT — four fields because they answer four different questions and
        # collapsing any two of them is how this defect happened in the first
        # place. `confounds` is what the classifier matches against; `confoundRows`
        # is the catalogue's own text (provenance included); `confoundDecisions`
        # says which rows are ACTIVE and WHY; `confoundReport` is the human account
        # both drivers print verbatim. `confoundGating` is the one a driver must
        # CHECK: only `probed` is usable, and `unprobed`/`injected` each mean no
        # measurement of THIS machine backs the gating, so the list is fail-safe
        # (conditional rows dropped, or honoured on a file) but not fit to run on.
        # ⓘ `confoundDecisions` IS READ, and by the one reader that can hold this
        # mechanism to account without parsing prose:
        # tests/harness/test_sqlite_harness_legs.cpp asserts the per-row ACTIVE /
        # INACTIVE / SCOPED-OUT decision structurally, including both directions of
        # the cross-kernel rule. It was inert for one cycle — emitted and read by
        # nothing — and an inert field is a claim nobody can be wrong about.
        # UNIT rows only. An `abort-file` row leaking into the unit matcher
        # would let a pattern written for `perm/file` excuse a unit that
        # happened to be spelled like one — one ledger, but never one
        # name space. [D-HARNESS-ABORT-HAS-NO-EARNED-CONFOUND-VOCABULARY]
        "confounds": [d["wire"] for d in decisions
                      if d["active"] and d["matches"] == "unit"],
        "abortConfounds": [d["wire"] for d in decisions
                           if d["active"] and d["matches"] == "abort-file"],
        "confoundRows": [dict(r) for r in leg.get("confounds", [])],
        "confoundDecisions": decisions,
        "confoundGating": gate["gating"],
        "confoundReport": confound_report_lines(leg["label"], decisions, gate),
        "run": run,
    }


def plan(host_os, host_arch, available, path=CATALOGUE, kernel_measurements=None):
    legs = load_catalogue(path)
    return {
        "host": {"os": host_os, "arch": host_arch},
        # THE PROBES THIS PLAN WAS GATED BY, verbatim, at the TOP of the plan —
        # so a reader (and a diff of two runs) sees the measurement that decided
        # every leg's excusals, once, rather than reconstructing it per leg.
        # ★ KEYED ON THE KERNEL, each entry carrying the OUTCOME and the argv or
        # words that produced it. A flat map here would print two kernels' answers
        # as though a run had one environment, which is the reading this anchor
        # exists to stop.
        "environmentProbes": ({} if kernel_measurements is None
                              else {k: dict(v)
                                    for k, v in kernel_measurements.items()}),
        "environmentProbesRun": kernel_measurements is not None,
        "legs": [plan_leg(leg, host_os, host_arch, available,
                          kernel_measurements)
                 for leg in legs],
    }


# ── EXECUTING WHAT THE PLAN RESOLVED ────────────────────────────────────────
#
# DELIBERATELY A SEPARATE ACT FROM `plan_leg`, and the split is the design.
# `plan_leg` is PURE and its launcher availability is INJECTED, so a plan is the
# same on every machine and both drivers can be tested against it without owning
# a qemu. Probing the filesystem is the opposite kind of operation — it is the
# machine ANSWERING — so it lives here, behind its own subcommand, and its result
# is a verdict rather than a plan.

def _probe_present(kind, path):
    """The IN-PROCESS answer, for a launcher that shares this process's
    filesystem. NOTHING IS SPAWNED: the question is about this machine, and a
    subprocess could only give a second opinion about a fact this process can
    read directly."""
    if kind == "file":
        return os.path.isfile(path)
    if kind == "directory":
        return os.path.isdir(path)
    if kind == "command":
        return shutil.which(path) is not None
    raise LegError("unknown launcher requirement kind %r" % kind)


def check_launcher(leg, host_os, host_arch, available, runner=None, checker=None,
                   artifact=None):
    """Are this leg's launcher's DECLARED prerequisites present on this machine?

    Returns {ok, verdict, missing, ...}. `runner(argv) -> (rc, out, err)` and
    `checker(kind, path) -> bool` are injected by the self-test so every arm is
    asserted on any host — and so that a `driver`-filesystem probe can be PROVEN
    not to spawn, by handing it a runner that raises if it is ever called.

    `artifact` turns on the 4-D cross-check: the built binary's own PT_INTERP and
    DT_NEEDED are read and every one must be covered by a declared row. That is
    what keeps this declaration honest in the direction it can be checked in —
    the other direction cannot be checked at all, which is why the rows carry
    prose evidence (see `dependency_coverage_findings`, and libgcc_s.so.1)."""
    resolved = plan_leg(leg, host_os, host_arch, available)
    run = resolved["run"]
    rows = run.get("requires", [])
    missing, checked = [], []
    for row in rows:
        argv = row.get("probe", [])
        if argv:
            rc, _out, _err = (runner or _run_translator)(argv)
            present = rc == 0
            how = " ".join(argv)
        else:
            present = (checker or _probe_present)(row["kind"], row["path"])
            how = "in-process %s(%s)" % (row["kind"], row["path"])
        checked.append({"path": row["path"], "present": present, "probe": how})
        if not present:
            missing.append(dict(row, probe=argv))
    detail = run.get("detail", "")
    if not rows:
        detail = ("this leg's run mode is '%s' and no launcher prerequisite is "
                  "declared for (%s, %s)" % (run.get("mode"), host_os, host_arch))
    uncovered, cross_check = [], "not requested"
    if artifact:
        try:
            with open(artifact, "rb") as fh:
                blob = fh.read()
        except OSError as exc:
            raise LegError(
                "--artifact %s could not be read (%s). The cross-check is the "
                "only thing that keeps the declared prerequisite list from "
                "silently shrinking below what the artefact demands, so an "
                "unreadable artefact stops the check rather than passing it."
                % (_fwd(artifact), exc))
        interp, needed = elf_runtime_dependencies(blob)
        if interp or needed:
            uncovered = dependency_coverage_findings(
                leg["label"], rows, interp, needed, _staged_library_names(leg))
            cross_check = ("applied: PT_INTERP=%s, %d DT_NEEDED"
                           % (interp or "<none>", len(needed)))
        else:
            # SAID OUT LOUD rather than passing quietly. Only the ELF reader
            # exists here, so a PE or Mach-O artefact — or a statically linked
            # ELF — yields nothing to cross-check, and "no findings" would read
            # as "checked and clean". A check that silently did not apply is the
            # shape of every instrument in this project that reported success
            # over something it could not observe.
            container = ""
            try:
                container = binary_target_identity(blob)[1]
            except LegError:
                container = "unreadable"
            cross_check = ("NOT APPLIED: %s declares no PT_INTERP and no "
                           "DT_NEEDED that this module can read (container=%s). "
                           "The declared rows stand on their own evidence here."
                           % (_fwd(artifact), container or "?"))
    ok = not missing and not uncovered
    return {
        "label": leg["label"],
        "ok": ok,
        # "" on success, for the same reason build_loadext_helper's verdictClass
        # is: a verdict names a NON-outcome, and inventing one for the happy path
        # would put a skip name in a log line about a leg that is about to run.
        "verdict": "" if ok else "skipped-launcher-prerequisite-missing",
        "launcher": run.get("launcher", []),
        "runFilesystem": run.get("runFilesystem", "driver"),
        "checked": checked,
        "missing": missing,
        "uncovered": uncovered,
        "crossCheck": cross_check,
        "detail": detail,
    }


def _staged_library_names(leg):
    """The library file names this harness stages beside a leg's artefact.

    Read off the leg's own acquisition declaration, so the dependency
    cross-check knows the difference between "the launcher must supply this" and
    "we ship this ourselves"."""
    out = []
    libs = leg.get("build", {}).get("libraries", {})
    for archive in libs.get("acquire", {}).get("archives", []):
        for member in archive.get("members", []):
            if member.get("as"):
                out.append(member["as"])
    return out


def launcher_for_target(legs, target, host_os, host_arch, available):
    """(argv, reason) — how THIS host runs a binary built for `target`.

    `target` is `<arch>:<container>:<targetOs>`, i.e. exactly the triple
    `binary_target_identity` reads out of a file, so a caller that has a binary
    in hand never has to translate between two vocabularies.

    NO NEW VOCABULARY AND NO NEW DECISION: the leg is found by its own declared
    `spec` and the answer comes from `plan_leg`, so this cannot disagree with the
    plan a driver is running."""
    parts = (target or "").split(":")
    if len(parts) != 3 or not all(parts):
        raise LegError(
            "--launcher-for-target takes <arch>:<container>:<targetOs> (e.g. "
            "arm64:elf64:linux), got %r. That is the triple --identify-binary "
            "prints, so the two can be piped together." % target)
    arch, container, target_os = parts
    for leg in legs:
        spec = leg.get("spec", "")
        if (spec_target_arch(spec) == arch
                and spec_format_container(spec) == container
                and spec_target_os(spec) == target_os):
            run = plan_leg(leg, host_os, host_arch, available)["run"]
            if run["mode"] == "launched":
                return (run["launcher"],
                        "leg '%s' runs on %s/%s through its declared launcher"
                        % (leg["label"], host_os, host_arch))
            if run["mode"] == "native":
                return ([], "leg '%s' runs NATIVELY on %s/%s — no launcher"
                            % (leg["label"], host_os, host_arch))
            return (None,
                    "leg '%s' cannot run on %s/%s: %s (%s)"
                    % (leg["label"], host_os, host_arch, run["verdict"],
                       run["detail"]))
    return (None,
            "no leg in this catalogue declares a spec for target %s — its legs "
            "are: %s. A binary of a target this harness does not build is not "
            "this harness's to attribute."
            % (target, ", ".join("%s (%s)" % (l.get("label"), l.get("spec"))
                                 for l in legs)))


def run_dir_plan(leg, host_os, host_arch, available, driver_run_dir):
    """WHERE this leg's corpus runs, and HOW the driver builds that directory.

    ONE answer, computed once, consumed by both drivers — the same division of
    labour as `--translate-path` and `--env-transfer`. A driver reads the fields;
    it never decides which filesystem a launcher lives in, never spells the
    working-directory option, and never learns where in the launcher argv that
    option has to be spliced.

    `driverPath` is always this driver's own run directory: it exists on every
    leg because the driver stages into it (the loadext helper is BUILT by a
    process on this machine and can only be written where this machine can
    write). `launcherPath` is "" exactly when the two filesystems are the same,
    which is how a driver tells "run here" from "install into there and run
    THERE" without knowing any verb name."""
    resolved = plan_leg(leg, host_os, host_arch, available)
    run = resolved["run"]
    verb = run["runFilesystem"]
    spec = run_filesystem(verb)
    label = resolved["label"]
    launcher_path = launcher_run_dir(verb, label, driver_run_dir)
    return {
        "leg": label,
        "runMode": run["mode"],
        "runFilesystem": verb,
        "driverPath": driver_run_dir,
        # "" => this driver's own directory IS the run directory.
        "launcherPath": launcher_path,
        # The launcher argv the fixture must actually be spawned through: the
        # DECLARED command with the working-directory option spliced in. Equal to
        # the declared command whenever the verb needs no option.
        "launcher": splice_working_dir(run["launcher"], verb,
                                       launcher_path or driver_run_dir),
        # argv PREFIXES. Empty => the driver performs the operation natively on
        # its own filesystem, which is what `driver` means.
        "mkdirArgv": list(spec["mkdirArgv"]),
        "rmTreeArgv": list(spec["rmTreeArgv"]),
        "copyArgv": list(spec["copyArgv"]),
        "detail": (
            "run mode '%s', runFilesystem '%s' — %s"
            % (run["mode"], verb,
               "this driver's own filesystem; the run directory is %s"
               % driver_run_dir if not launcher_path else
               "the launcher writes onto ITS OWN filesystem, so the corpus runs "
               "in %s and NOT in %s, which it would reach only through a "
               "compatibility mount whose POSIX semantics are approximate "
               "(D-HARNESS-WSL-LAUNCHED-LEG-RUNDIR-IS-DRVFS)"
               % (launcher_path, driver_run_dir))),
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
        # [D-HARNESS-RUN-FIDELITY-IS-COMPUTED-BUT-NEITHER-RECORDED-NOR-SELECTABLE]
        # ★ TRANSPORTED, not re-derived. The .ps1 reads the JSON plan and sees
        # `run.fidelity` directly; the .sh reads THESE flattened arrays, so a
        # fidelity it could not see is a selector only one driver could honour —
        # which is this project's canonical silent harness bug. "" for a leg that
        # never runs, exactly as LEG_RUN_VERDICT is "" for one that does: a
        # defaulted value here would be a measurement a reader could trust.
        put("LEG_RUN_FIDELITY", run.get("fidelity") or "")
        put("LEG_RUN_SAME_ISA", "1" if run.get("sameIsa") else "0")
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
        # WHICH FILESYSTEM the launched corpus writes onto. Same story again: the
        # driver reads the VERB and asks this resolver for the directory, the
        # launcher argv and the argv prefixes (`--run-dir-plan`), because the run
        # directory's filesystem is a property of the LAUNCHER and not of the
        # machine that happens to be driving. D-HARNESS-WSL-LAUNCHED-LEG-RUNDIR-
        # IS-DRVFS.
        put("LEG_RUN_FILESYSTEM", run["runFilesystem"])
        # THE EARNED CONFOUNDS, PER LEG, IN THE DRIVERS' OWN WIRE GRAMMAR. This
        # is the whole of D-HARNESS-CONFOUND-LEDGER-IS-PER-DRIVER-NOT-PER-LEG:
        # both drivers now read the SAME declaration, so a failure cannot be a
        # confound under one and a compiler defect under the other. The
        # provenance behind each pattern stays in legs.json, where the lint can
        # require it — a driver needs the pattern, a reader needs the evidence.
        put("LEG_CONFOUNDS", " ".join(q(x) for x in leg["confounds"]))
        put("LEG_ABORT_CONFOUNDS",
            " ".join(q(x) for x in leg.get("abortConfounds", [])))
        # ★ AND WHETHER A MACHINE MEASUREMENT BACKS THAT LIST. `unprobed` is
        # fail-safe (every conditional row was dropped) but it is NOT fit to run
        # on, and the driver refuses it — a run that silently under-excuses reads
        # as a compiler regression, which is the mirror of the defect this gate
        # exists to remove. [D-HARNESS-CONFOUND-SCOPE-IS-A-RUN-MODE-NOT-A-HOST]
        put("LEG_CONFOUND_GATING", leg.get("confoundGating", ""))
        # ★ HOW MANY ROWS THE CATALOGUE DECLARED FOR THIS LEG, so an EMPTY supply
        # can be told from a catalogue that declares nothing. Both drivers used to
        # infer the second from the first: "this leg's catalogue entry declares
        # `confounds: []`, i.e. nothing has ever been measured as a non-DSS
        # confound HERE" — a claim about the CATALOGUE derived from an empty array,
        # which is ALSO what you get when every declared row is gated off by a
        # probe (and, before this cycle, when the transport failed).
        # [D-HARNESS-CONFOUND-SCOPE-IS-A-RUN-MODE-NOT-A-HOST]
        put("LEG_CONFOUND_DECLARED", str(len(leg.get("confoundRows", []))))
        # THE VISIBLE ACCOUNT, generated once in the resolver and printed verbatim
        # by both drivers (dss:confound-report). NEWLINE-separated inside one
        # variable: the report is a block, and splitting it per line would make
        # the two drivers each decide how to reassemble it.
        put("LEG_CONFOUND_REPORT", "\n".join(leg.get("confoundReport", [])))
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
        # The leg's OWN staged sqlite_cfg.h directory name + the answers that made
        # it. Same division of labour as the zinc pair directly above: the driver
        # joins the key onto its cfg/ root and never decides the mapping, and the
        # answer string is emitted so a build log states, per leg, WHICH configure
        # answers that leg was compiled against.
        put("LEG_CONFIG_STAGE_KEY", b.get("configStageKey", ""))
        put("LEG_CONFIGURE_ANSWERS",
            " ".join("%s=%d" % (k, 1 if v else 0)
                     for k, v in sorted(b.get("configureAnswers", {}).items())))
        put("LEG_STACK_RESERVE", str(b.get("stackReserveBytes", 0)))
        put("LEG_SHARED_FLAGS", " ".join(b.get("sharedLibFlags", [])))
        # The object format DSS emits this leg's loadext helper in, and the
        # combined `--target` argument built from it. Both emitted so a build log
        # can STATE them per leg; neither driver assembles the combined form
        # itself, which is the trap `shared_lib_spec` exists to close (the arch
        # and the format spell arm64 differently).
        put("LEG_SHARED_LIB_FORMAT", b.get("sharedLibFormat", ""))
        put("LEG_SHARED_LIB_SPEC",
            "%s:%s" % (leg["targetArch"], b.get("sharedLibFormat", "")))
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


def _lint_member_runtime_data(who, m, as_name, import_name):
    """The RUNTIME-DATA half of one acquired member's declaration.

    D-HARNESS-ACQUIRED-TCL-DYLIB-HAS-NO-SCRIPT-LIBRARY. Split out because the
    rules are about a different question from the rest of `_lint_acquire`: not
    "can we fetch this?" but "once fetched, is it SELF-SUFFICIENT?" — and the
    answer turns entirely on which copy of the library actually runs."""
    out = []
    copy = m.get("runtimeCopy", "")
    if copy not in RUNTIME_COPIES:
        out.append(
            "%s member %r: runtimeCopy %r is not one of %s. WHICH COPY RUNS is "
            "not a detail: if the staged file is the one that runs, every data "
            "directory it bakes in must be staged with it, and if the target "
            "supplies its own, staging ours would ship the wrong data. There is "
            "no safe default to omit it in favour of."
            % (who, as_name, copy, ", ".join(sorted(RUNTIME_COPIES))))
    # CROSS-CHECKED against the identity actually recorded, where that identity
    # decides the answer — a declaration nothing contradicts is a declaration
    # nothing tests.
    beside = import_name.startswith(IMPORT_NAME_BESIDE_PREFIXES)
    absolute = import_name.startswith("/") or (
        len(import_name) > 2 and import_name[1] == ":")
    if beside and copy == "target-supplies-its-own":
        out.append(
            "%s member %r: importName %r resolves NEXT TO THE LOADING BINARY, so "
            "the copy that runs IS the one staged here — `runtimeCopy: "
            "target-supplies-its-own` contradicts it."
            % (who, as_name, import_name))
    if absolute and copy == "staged-beside-artefact":
        out.append(
            "%s member %r: importName %r is an ABSOLUTE path, so the loader takes "
            "the target machine's copy at that path, not the one staged here."
            % (who, as_name, import_name))
    for e in m.get("embeddedPaths", []):
        path = e.get("path", "")
        if not path.startswith("/") and not (len(path) > 2 and path[1] == ":"):
            out.append("%s member %r: embeddedPaths entry %r is not an absolute "
                       "path" % (who, as_name, path))
        kind = e.get("kind", "")
        if kind not in EMBEDDED_PATH_KINDS:
            out.append("%s member %r: embeddedPaths %r declares kind %r, not one "
                       "of %s" % (who, as_name, path, kind,
                                  ", ".join(sorted(EMBEDDED_PATH_KINDS))))
        elif kind == "runtime-data":
            if e.get("role", "") not in DATA_DIR_ROLES:
                out.append(
                    "%s member %r: embeddedPaths %r is `runtime-data` but its "
                    "role %r is not one of %s — the role is what ties it to the "
                    "directory that satisfies it"
                    % (who, as_name, path, e.get("role", ""),
                       ", ".join(sorted(DATA_DIR_ROLES))))
        elif not e.get("$why"):
            out.append(
                "%s member %r: embeddedPaths %r is declared `inert` with no "
                "`$why`. \"Nothing reads this\" is exactly the sentence that was "
                "WRONG about /opt/local/lib/tcl8.6, and it cost this project its "
                "first Mach-O unit corpus. State what makes it unread."
                % (who, as_name, path))
    roles = {}
    for d in m.get("dataDirs", []):
        if not d.get("member"):
            out.append("%s member %r: a dataDirs entry declares no `member` path"
                       % (who, as_name))
        if not d.get("as"):
            out.append("%s member %r: a dataDirs entry declares no `as` name — "
                       "it is the directory the driver is handed" % (who, as_name))
        role = d.get("role", "")
        if role not in DATA_DIR_ROLES:
            out.append("%s member %r: dataDirs %r declares role %r, not one of %s"
                       % (who, as_name, d.get("as", ""), role,
                          ", ".join(sorted(DATA_DIR_ROLES))))
        roles[role] = roles.get(role, 0) + 1
        if copy == "target-supplies-its-own":
            out.append(
                "%s member %r: stages the data directory %r, but the TARGET "
                "supplies its own copy of this library — so the target's own "
                "data directory is the one its loader will use, and this one "
                "would be a second, silently-unused copy (or worse, one paired "
                "with a different build of the library)."
                % (who, as_name, d.get("as", "")))
    if roles.get("tclScriptLibrary", 0) > 1:
        out.append("%s member %r: %d dataDirs claim role 'tclScriptLibrary'; "
                   "TCL_LIBRARY names exactly one directory"
                   % (who, as_name, roles["tclScriptLibrary"]))
    # ★ THE DECLARATION-LEVEL HALF OF THE GUARD. `_audit_embedded_paths` catches
    # this too, but only once the bytes are on disk — i.e. on the machine that
    # runs the build, not in the lint that runs everywhere. A `runtime-data`
    # path with nothing staged for it is EXACTLY the state the macho legs
    # shipped in, and it is visible from the declaration alone.
    if copy == "staged-beside-artefact":
        for e in m.get("embeddedPaths", []):
            if e.get("kind") != "runtime-data":
                continue
            if e.get("role") not in roles:
                out.append(
                    "%s member %r: %r is declared `runtime-data` with role %r "
                    "and NO dataDirs entry stages it. The acquired library is "
                    "the one that runs, so that path has to resolve on the "
                    "TARGET — and it is the packager's own prefix, which the "
                    "target does not have. This is the defect that cost the "
                    "first Mach-O unit corpus: the link succeeded, the binary "
                    "ran, and it died at `interp create`."
                    % (who, as_name, e.get("path", ""), e.get("role", "")))
    return out


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
            out.extend(_lint_member_runtime_data(who, m, as_name, imp))
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
    # The run-wide stage build configuration is linted FIRST and by the same
    # loader the drivers use, so `--lint` cannot pass over a declaration that
    # would abort every run. Reported as a finding rather than raised: lint's
    # job is to enumerate everything wrong, not to stop at the first thing.
    try:
        stage_build(path)
    except LegError as exc:
        findings.append("stageBuild: %s" % exc)
    except (OSError, ValueError) as exc:
        findings.append("stageBuild could not be read from %s: %s" % (path, exc))
    legs = load_catalogue(path)
    # ── THE ENVIRONMENT-PROBE REGISTRY ──────────────────────────────────────
    # Linted BEFORE the legs, because every conditional confound row names an
    # entry here and a broken registry would report as N broken rows.
    # [D-HARNESS-CONFOUND-SCOPE-IS-A-RUN-MODE-NOT-A-HOST]
    registry = {}
    try:
        registry = environment_probes(load_catalogue_doc(path))
    except LegError as exc:
        findings.append("environmentProbes: %s" % exc)
    except (OSError, ValueError) as exc:
        findings.append("environmentProbes could not be read from %s: %s"
                        % (path, exc))
    for pname in sorted(registry):
        entry = registry[pname] if isinstance(registry[pname], dict) else {}
        if not isinstance(registry[pname], dict):
            findings.append("environmentProbes['%s'] must be an object, got %r"
                            % (pname, type(registry[pname]).__name__))
            continue
        for k in PROBE_DECLARATION_KEYS:
            v = entry.get(k, "")
            if not isinstance(v, str) or not v.strip():
                findings.append(
                    "environmentProbes['%s'] declares no '%s'. A probe decides "
                    "whether a failing test is excused, so it states what it "
                    "MEASURES, what a PRESENT verdict would mean, and which "
                    "anchor holds the long form (required: %s)."
                    % (pname, k, ", ".join(PROBE_DECLARATION_KEYS)))
        verb = entry.get("verb", "")
        try:
            spec = probe_verb(verb)
        except LegError as exc:
            findings.append("environmentProbes['%s']: %s" % (pname, exc))
            continue
        config = entry.get("config", {})
        if not isinstance(config, dict):
            findings.append("environmentProbes['%s'].config must be an object, "
                            "got %r" % (pname, type(config).__name__))
            continue
        want = set(spec["configKeys"])
        missing = sorted(want - set(config))
        extra = sorted(set(config) - want)
        if missing:
            findings.append(
                "environmentProbes['%s'] (verb '%s') omits config key(s) %s. "
                "Every threshold is DECLARED, never defaulted in code: a probe "
                "whose sensitivity a reader has to find in python is one nobody "
                "can tighten by editing this file."
                % (pname, verb, ", ".join(missing)))
        if extra:
            findings.append(
                "environmentProbes['%s'] (verb '%s') declares unknown config "
                "key(s) %s (known: %s). The set is closed because a typo'd "
                "threshold would be SILENTLY IGNORED and the probe would run at "
                "a sensitivity nobody chose."
                % (pname, verb, ", ".join(extra), ", ".join(spec["configKeys"])))
        # ★★ THE FAIL-SAFE FLOOR. Config may TIGHTEN a threshold and may not
        # loosen it below what can still support a `present`. A guard config can
        # weaken without limit is one that gets re-cut to fit each new case
        # (D-TEST-PE64-CONFOUND-PIN-WEAKENED-BY-ITS-OWN-SUBJECT, twice).
        for key in spec.get("raiseOnly", ()):
            floor = spec["floors"][key]
            got = config.get(key)
            try:
                bad = got is None or float(got) < float(floor)
            except (TypeError, ValueError):
                bad = True
            if bad:
                findings.append(
                    "environmentProbes['%s'] (verb '%s') sets %s=%r, below the "
                    "floor %r. This threshold may only be RAISED. The errors are "
                    "not symmetric: a probe that says PRESENT on a healthy "
                    "machine SILENTLY EXCUSES a real compiler defect, while one "
                    "that says ABSENT on a defective machine merely produces "
                    "noisy reds somebody then investigates."
                    % (pname, verb, key, got, floor))
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
            # ── the launcher's FILESYSTEM ──────────────────────────────────
            # Declared on EVERY entry, third of three, and the one whose absence
            # cost 55 unit failures across 6 families plus a fixture ABORT — none
            # of them DSS's, all of them reported as if they were.
            if "runFilesystem" not in entry:
                findings.append(
                    "leg '%s': launcher for (%s, %s) declares no runFilesystem. "
                    "Every launcher states which FILESYSTEM its leg's run "
                    "directory lives in — 'driver' when the launched process "
                    "writes where this driver writes (Wine, qemu, arch), a named "
                    "verb when it crosses into another kernel's own filesystem "
                    "(known: %s). Omitting it is how a database engine's corpus "
                    "came to run over a mount that derives every POSIX mode bit "
                    "from one Windows attribute."
                    % (label, entry["hostOs"], entry["hostArch"],
                       ", ".join(sorted(RUN_FILESYSTEMS))))
            else:
                fverb = entry["runFilesystem"]
                if fverb not in RUN_FILESYSTEMS:
                    findings.append(
                        "leg '%s': launcher for (%s, %s) declares unknown "
                        "runFilesystem %r (known: %s)"
                        % (label, entry["hostOs"], entry["hostArch"], fverb,
                           ", ".join(sorted(RUN_FILESYSTEMS))))
                else:
                    want_os = RUN_FILESYSTEMS[fverb]["validHostOs"]
                    if want_os and entry["hostOs"] != want_os:
                        findings.append(
                            "leg '%s': launcher for (%s, %s) declares "
                            "runFilesystem '%s', whose mechanism only exists on "
                            "a '%s' host"
                            % (label, entry["hostOs"], entry["hostArch"], fverb,
                               want_os))
                    # DERIVABLE, so CHECKED rather than trusted, and it is the
                    # pair that actually bites: a launcher in a foreign PATH
                    # namespace is in a foreign FILESYSTEM almost by definition —
                    # translating a path into `/mnt/c/...` is the compatibility
                    # mount. Declaring `driver` there is the exact defect.
                    if (fverb == "driver"
                            and entry.get("pathTranslation", "none") != "none"):
                        findings.append(
                            "leg '%s': launcher for (%s, %s) declares "
                            "pathTranslation '%s' but runFilesystem 'driver'. A "
                            "launcher whose paths must be RE-SPELLED to reach it "
                            "is reaching this driver's files through a "
                            "compatibility mount, and 'driver' claims the "
                            "opposite — that it writes onto this filesystem with "
                            "this filesystem's semantics. That claim is what put "
                            "a Linux sqlite corpus onto DrvFs."
                            % (label, entry["hostOs"], entry["hostArch"],
                               entry.get("pathTranslation")))
            # ── the launcher's ENVIRONMENT, and what it points at ──────────
            # `env` was OPTIONAL and validated NOWHERE until requires paths began
            # expanding over it — it was read once, at plan_leg, and copied
            # through to the drivers unexamined. A map that decides WHAT GETS
            # CHECKED is load-bearing, so it earns the same treatment as its
            # three sibling keys: required, no default, cross-checked against the
            # host OS its namespace belongs to.
            findings.extend(launcher_env_findings(label, entry))
            # ── what the launcher needs BEYOND its own argv[0] ─────────────
            # The gate that did not exist: `launcher_available` resolves
            # `command[0]` and stops, so `wsl.exe` present with no distro, no
            # qemu inside it and no sysroot passed every check this harness had.
            findings.extend(launcher_requires_findings(label, entry))
            if entry["hostOs"] in leg.get("runOn", []) and entry["hostArch"] == arch:
                findings.append("leg '%s': launcher declared for (%s, %s), which "
                                "is this leg's NATIVE host — dead config: the "
                                "resolver never consults it" % (label, entry["hostOs"],
                                                                entry["hostArch"]))
            key = (arch, entry["hostOs"], entry["hostArch"])
            vocab.setdefault(key, set()).add(" ".join(cmd))
        # ── THE EARNED CONFOUNDS, AND THE EVIDENCE FOR EACH ────────────────
        # The lint is the only thing standing between "a failure we proved is not
        # ours" and "a failure we got used to". It therefore refuses a pattern
        # that does not show its work, and it refuses SILENCE: a leg with nothing
        # earned must SAY it has nothing earned.
        if "confounds" not in leg:
            findings.append(
                "leg '%s': declares no `confounds`. Every leg states which unit "
                "failures have been PROVEN non-DSS on it, `[]` when none have. "
                "Required, because a missing key cannot be told from an empty "
                "one and the difference decides whether a failing test is "
                "reported as a compiler defect." % label)
        elif not isinstance(leg["confounds"], list):
            findings.append("leg '%s': `confounds` must be a list, got %r"
                            % (label, type(leg["confounds"]).__name__))
        else:
            seen_patterns = set()
            for row in leg["confounds"]:
                if not isinstance(row, dict):
                    findings.append("leg '%s': confound entry %r is not an object "
                                    "— every pattern carries its provenance"
                                    % (label, row))
                    continue
                pat = row.get("pattern", "")
                if not pat or not isinstance(pat, str):
                    findings.append("leg '%s': a confound entry declares no "
                                    "`pattern`" % label)
                    continue
                if pat in seen_patterns:
                    findings.append(
                        "leg '%s': confound pattern %r is declared twice. Two "
                        "rows for one pattern means two provenances, and a "
                        "reader cannot tell which one is load-bearing."
                        % (label, pat))
                seen_patterns.add(pat)
                try:
                    re.compile(pat)
                except re.error as exc:
                    findings.append(
                        "leg '%s': confound pattern %r does not compile as a "
                        "regex (%s) — it would silently match NOTHING and every "
                        "failure it names would be reported as a DSS defect"
                        % (label, pat, exc))
                # ── `requires`: THE CONDITION, MACHINE-READABLE ─────────────
                # REQUIRED on every row; `[]` is the ordinary answer. See
                # leg_confound_decisions for why it is not defaulted.
                # [D-HARNESS-CONFOUND-SCOPE-IS-A-RUN-MODE-NOT-A-HOST]
                reqs = row.get("requires")
                if "requires" not in row:
                    # Told apart from a WRONG-TYPE `requires` on purpose: "you
                    # forgot the key" and "the key is the wrong shape" send a
                    # reader to two different places, and the first is the one
                    # that used to be spelled `scope: any`.
                    findings.append(
                        "leg '%s': confound %r declares no `requires`. It is "
                        "REQUIRED on every row and `[]` is the ordinary answer — "
                        "'this excusal depends on nothing this harness can "
                        "measure'. A missing key cannot be told from an empty "
                        "one, and the difference is whether a family is excused "
                        "on a machine never shown to have the defect."
                        % (label, pat))
                elif not isinstance(reqs, list):
                    findings.append(
                        "leg '%s': confound %r declares `requires` %r — it must "
                        "be a LIST of environment-probe names, `[]` when the "
                        "excusal depends on nothing this harness measures."
                        % (label, pat, reqs))
                else:
                    for nm in reqs:
                        if nm not in registry:
                            findings.append(
                                "leg '%s': confound %r requires environment probe "
                                "%r, which `environmentProbes` does not declare "
                                "(declared: %s). An undeclared probe cannot be "
                                "measured, so the row would be honoured on "
                                "nothing — the exact state `scope: any` was in."
                                % (label, pat, nm,
                                   ", ".join(sorted(registry)) or "<none>"))
                # ── `scope`: LEGACY, and it must name its blocker ───────────
                if "scope" in row:
                    scope = row.get("scope", "")
                    if scope in RETIRED_CONFOUND_SCOPES:
                        findings.append(
                            "leg '%s': confound %r declares scope %r, which is "
                            "RETIRED. That claim is now `requires: []`. The "
                            "`scope` axis survives only where a row's real "
                            "mechanism has no probe yet."
                            % (label, pat, scope))
                    elif scope not in CONFOUND_SCOPES:
                        findings.append(
                            "leg '%s': confound %r declares scope %r (known: %s). "
                            "An unrecognised scope cannot be read as "
                            "unconditional, because unconditional is the widest "
                            "possible excusal."
                            % (label, pat, scope, ", ".join(CONFOUND_SCOPES)))
                    blocker = row.get("scopeLegacyBlocker", "")
                    if not isinstance(blocker, str) or not blocker.strip():
                        findings.append(
                            "leg '%s': confound %r still uses the LEGACY `scope` "
                            "axis and declares no `scopeLegacyBlocker`. A row may "
                            "stay on `scope` only while it NAMES what stops it "
                            "migrating to a measured `requires` probe — otherwise "
                            "the axis becomes an inert alternative that the next "
                            "row reaches for, and a run mode is not a host. "
                            "[D-HARNESS-CONFOUND-SCOPE-IS-A-RUN-MODE-NOT-A-HOST]"
                            % (label, pat))
                else:
                    scope = ""
                # ── `matches`: WHICH NAME SPACE THIS ROW IS WRITTEN FOR ─────
                # [D-HARNESS-ABORT-HAS-NO-EARNED-CONFOUND-VOCABULARY]
                kind = row.get("matches", CONFOUND_MATCH_DEFAULT)
                if kind not in CONFOUND_MATCH_KINDS:
                    findings.append(
                        "leg '%s': confound %r declares matches=%r (known: %s). "
                        "It is NOT defaulted: a mistyped abort row silently read "
                        "as a unit row matches no unit name, excuses nothing, and "
                        "the abort it was written for is still charged to the "
                        "compiler."
                        % (label, pat, kind, ", ".join(sorted(CONFOUND_MATCH_KINDS))))
                elif kind == "abort-file" and not str(
                        row.get("abortDiagnostic", "")).strip():
                    # ★★ REQUIRED, EXACTLY LIKE `earnedOn`.
                    # [D-HARNESS-ABORT-CONFOUND-KEYED-ON-LOCATION-NOT-IDENTITY]
                    # A row constraining only the FILE excuses a LOCATION, so any
                    # future abort in that file — a codegen crash, a stack
                    # overflow — is silently forgiven by a row earned for
                    # something else. Not optional: an optional field is one the
                    # next row omits, and we are back here.
                    findings.append(
                        "leg '%s': confound %r declares matches='abort-file' but "
                        "no 'abortDiagnostic'. An abort row must constrain the "
                        "failure's IDENTITY (the text the fixture died with), "
                        "not only WHERE it happened — otherwise a DIFFERENT "
                        "abort in the same file inherits an excusal nobody "
                        "earned for it." % (label, pat))
                elif kind == "abort-file" and "/" not in pat:
                    # An abort's only name is `permutation/file`. A pattern with
                    # no separator was written against a unit name and would
                    # match nothing here — dead config that reads as an earned
                    # exemption, which is the worst of both.
                    findings.append(
                        "leg '%s': confound %r declares matches='abort-file' but "
                        "its pattern contains no '/'. An abort is named "
                        "`permutation/file` (e.g. `^veryquick/nolock\\.test$`); a "
                        "pattern without the separator can never match one and "
                        "would sit here reading as a proven exemption while "
                        "excusing nothing." % (label, pat))
                elif kind == "build-tu":
                    findings.extend(build_tu_row_findings(label, row))
                for k in CONFOUND_PROVENANCE_KEYS:
                    v = row.get(k, "")
                    if not isinstance(v, str) or not v.strip():
                        findings.append(
                            "leg '%s': confound %r declares no '%s'. A confound "
                            "asserts the COMPILER IS INNOCENT of a failing test; "
                            "it has to show its work — what was measured (%s), "
                            "on which leg and host, when, and which anchor holds "
                            "the long form. An unearned confound is how a real "
                            "defect becomes furniture."
                            % (label, pat, k, ", ".join(CONFOUND_PROVENANCE_KEYS)))
                # A scope that no host could ever satisfy is dead config, and it
                # reads as coverage. `native` on a leg no declared host runs
                # natively can never fire; `emulated` on a leg with no launcher
                # likewise. Both are derivable from this very file.
                if scope == "emulated" and not leg.get("launchers"):
                    findings.append(
                        "leg '%s': confound %r is scoped 'emulated' but this leg "
                        "declares NO launcher, so the scope can never be "
                        "satisfied and the pattern excuses nothing — dead config "
                        "that reads as a documented confound" % (label, pat))
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
        # ── the sqlite configure answers this leg's staged header carries ─────
        # The same four checks the zconf guards get — REQUIRED, CLOSED, BOOLEAN,
        # CROSS-CHECKED — because it is the same defect one layer up: the recipe's
        # `_HAVE_SQLITE_CONFIG_H` makes sqliteInt.h include the DERIVING host's
        # generated sqlite_cfg.h, so without a per-target answer the Darwin legs
        # compile against a Linux machine's ./configure run.
        answers = build.get("configureAnswers")
        if not isinstance(answers, dict):
            findings.append("leg '%s': missing build.configureAnswers — every leg "
                            "must declare, for ITS OWN target, each sqlite "
                            "./configure answer that VARIES across the targets "
                            "this harness builds (%s). Omitting it is how the "
                            "Darwin legs came to compile against the deriving "
                            "Linux host's HAVE_PREAD64/HAVE_PWRITE64 and fail on "
                            "`off64_t`, a type Darwin does not have."
                            % (label, ", ".join(CONFIGURE_ANSWER_NAMES)))
        else:
            for name in sorted(set(answers) - set(CONFIGURE_ANSWER_NAMES)):
                findings.append("leg '%s': unknown configure answer %r (known: %s) "
                                "— an answer nothing stages is a declaration that "
                                "reads as configuration and is not. ⛔ In "
                                "particular there is NO `HAVE_OFF64_T` anywhere "
                                "in sqlite: off64_t arrives through "
                                "HAVE_PREAD64+HAVE_PWRITE64 -> USE_PREAD64."
                                % (label, name, ", ".join(CONFIGURE_ANSWER_NAMES)))
            for name in CONFIGURE_ANSWER_NAMES:
                if name not in answers:
                    findings.append("leg '%s': build.configureAnswers omits %r — "
                                    "every answer is declared for every leg, so a "
                                    "reader never has to know a default"
                                    % (label, name))
                elif not isinstance(answers[name], bool):
                    findings.append("leg '%s': build.configureAnswers[%r] is %r, "
                                    "not a JSON boolean — a string is truthy in "
                                    "bash, PowerShell and python alike, and this "
                                    "value decides whether a #define lands in a "
                                    "header every TU parses"
                                    % (label, name, answers[name]))
                elif target_os in TARGET_OS_NAMES:
                    # DERIVABLE from the target, so it is CHECKED rather than
                    # trusted — the POSIX_ONLY_ZCONF_GUARDS discipline. Each
                    # entry's `why` is quoted so the finding carries the evidence
                    # that settled it, not just the verdict.
                    want = configure_answer_for_target_os(name, target_os)
                    if answers[name] is not want:
                        findings.append(
                            "leg '%s': targets OS '%s' but declares %s=%r; that "
                            "answer is DERIVABLE from the target and should be "
                            "%r. %s A declaration that disagrees with the target "
                            "would stage a sqlite_cfg.h measured on a different "
                            "machine, which is the whole defect this key closes."
                            % (label, target_os, name, answers[name], want,
                               CONFIGURE_ANSWERS[name]["why"]))
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
            # ⚠ NOT "the helper could not be built" — it can, and by DEFAULT it is.
            # DSS is the PRIMARY builder of the loadext helper and needs nothing
            # from this machine (`build_loadext_helper`, builder 'dss'); targetCc
            # only ever names the CONTROL arm's third-party cross-compiler, which
            # is optional by construction ("that is the ENTIRE reason this arm is a
            # control and not the default"). This message claimed the opposite,
            # which would send a reader hunting for a cross-toolchain to fix a
            # missing control.
            findings.append("leg '%s': no targetCc candidates — DSS still builds "
                            "the corpus's dlopen()ed helper extension for it, but "
                            "the leg declares no CONTROL compiler, so a loadext-* "
                            "red on it can never be cross-checked against a "
                            "reference build" % label)
        if not build.get("sharedLibFlags"):
            findings.append("leg '%s': no sharedLibFlags" % label)
        # ── the object format the helper is EMITTED in ────────────────────────
        # DECLARED (so a reader of legs.json sees the exact `--target` argument —
        # the string that has cost this project failed invocations), then
        # cross-checked against the leg's OWN spec, which already names the
        # container, the arch and the OS. Same discipline as loadExtHelperName
        # below and POSIX_ONLY_ZCONF_GUARDS above.
        declared_fmt = build.get("sharedLibFormat")
        want_fmt = derived_shared_lib_format(spec)
        if not declared_fmt:
            findings.append(
                "leg '%s': no build.sharedLibFormat — every leg declares the "
                "object format DSS emits its loadext helper in, because that "
                "string IS the `--target <arch>:<format>` argument and there is "
                "no separate --object-format flag. For this leg's spec %r it is "
                "%r. Without it the helper cannot be built by the compiler this "
                "repository ships, and the leg falls back to needing a "
                "third-party cross-compiler on the host "
                "[D-HARNESS-CROSS-HOST-ANY-TARGET]."
                % (label, spec, want_fmt or "<underivable from this spec>"))
        elif not want_fmt:
            findings.append(
                "leg '%s': declares sharedLibFormat %r, but no shared-library "
                "format can be DERIVED from its spec %r to check it against "
                "(format names are <container><bits>-<arch>-<os>-<kind> and the "
                "known containers are %s) — so the declaration cannot be "
                "cross-checked and would be trusted blind"
                % (label, declared_fmt, spec,
                   ", ".join(sorted(SHARED_LIB_KIND_BY_CONTAINER))))
        elif declared_fmt != want_fmt:
            findings.append(
                "leg '%s': declares sharedLibFormat %r, but its spec %r names "
                "container/arch/OS whose shared-library format is %r. A format "
                "that is not this leg's own emits the helper for a DIFFERENT "
                "target — the exact wrong-target helper "
                "D-HARNESS-ARM64-LEG-HOST-ARCH-HELPER-SO is named after, one "
                "layer down: every loadext-* unit would then false-red as a "
                "genuine DSS failure."
                % (label, declared_fmt, spec, want_fmt))
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
    # And the same for the staged sqlite configure header, whose stage key is the
    # TARGET OS: two legs targeting one OS share one header, so two different
    # declarations for one OS is a catalogue defect and not a preference.
    try:
        configure_stages(legs)
    except LegError as exc:
        findings.append("%s" % exc)
    for key, spellings in sorted(vocab.items()):
        if len(spellings) > 1:
            findings.append("(targetArch=%s, hostOs=%s, hostArch=%s) has %d "
                            "launcher spellings: %s — one triple must have one "
                            "vocabulary" % (key[0], key[1], key[2], len(spellings),
                                            ", ".join(sorted(spellings))))
    return findings


# ── THE `dss:` REGIONS, AND WHO ACTUALLY CHECKS EACH ONE ────────────────────
#
# D-HARNESS-CORPUS-ENGINE-MIRROR-CLAIMS-A-VERIFIER-THAT-DOES-NOT-EXIST.
#
# ✔MEASURED (TF-C123): the `dss:corpus-engine` header said "the verifier extracts
# it from this file by these sentinels". `grep -rl 'dss:corpus-engine'` returned
# exactly the two driver files themselves. NOTHING read the sentinel. The
# mirrored region was entirely unenforced, the two copies could diverge silently,
# and the region carried a note saying they could not — a comment crediting an
# instrument that was never built, which is strictly worse than no comment
# because it retires the reader's suspicion.
#
# ★ THE GENERALISATION, WHICH IS THE PART THAT STOPS THIS RECURRING UNDER A
# DIFFERENT SENTINEL NAME. It is not enough to build one verifier for one
# region: the defect was that a region's verification status was INVISIBLE. So
# every `dss:` region is DECLARED here, with who checks it, and the declaration
# is checked in BOTH directions:
#
#   · a region marked in a driver with no row here            -> LOUD
#   · a row here naming a region no driver marks              -> LOUD
#   · a row claiming a verifier FILE that does not mention    -> LOUD
#     the region  (the exact defect: a claim with no reader)
#   · a row claiming NO verifier without stating WHY          -> LOUD
#
# The last one is the load-bearing line. "Unverified" stays possible — several
# regions are read markers for a human and nothing more — but it becomes a
# STATED DECISION with a reason, never an omission a reader has to detect.
#
#   drivers    the driver files that must mark this region, exactly.
#   verifiers  files that read the region BY ITS SENTINEL. Checked to exist and
#              to actually contain the sentinel name.
#   mirror     True  => the region is claimed to be MIRRORED between the two
#              drivers, and check_dss_regions runs the differential battery on
#              it (below): the two copies are executed on identical input and
#              their answers must be identical.
#   why        required when `verifiers` is empty.
DSS_REGIONS = {
    # TF-C136. Both regions delimit a capability that must exist in BOTH drivers
    # or the missing side runs a corpus that cannot start and charges every
    # failure to the compiler — which is exactly what happened before they
    # existed. `mirror` is deliberately NOT claimed: the two copies are the same
    # CAPABILITY expressed in two languages, not the same text, and the
    # differential battery would be asserting a sameness that was never true.
    # What IS asserted is the pairing itself, by the verifiers below, which read
    # these sentinels by name.
    "launcher-prereq": {
        "drivers": ["build-and-test.sh", "build-and-test.ps1"],
        "verifiers": ["test-confound-scope.sh", "test-confound-scope.ps1"]},
    "smoke-targets": {
        "drivers": ["build-and-test.sh", "build-and-test.ps1"],
        "verifiers": ["test-confound-scope.sh", "test-confound-scope.ps1"]},
    "corpus-engine": {
        "drivers": ["build-and-test.sh", "build-and-test.ps1"],
        "verifiers": ["harness_legs.py"], "mirror": True},
    "clone-lock": {
        "drivers": ["build-and-test.sh"],
        "verifiers": ["build-and-test.ps1"],
        "$comment": "the .ps1 EXTRACTS this block out of the .sh and injects it "
                    "into the staging step, so the .ps1 is a real consumer of "
                    "the sentinel — not a second copy."},
    "src-provenance": {
        "drivers": ["build-and-test.sh", "build-and-test.ps1"],
        "verifiers": ["test-confound-scope.sh", "test-confound-scope.ps1"]},
    "src-clone": {"drivers": ["build-and-test.sh"],
                  "verifiers": ["test-confound-scope.sh"]},
    "src-gate": {"drivers": ["build-and-test.sh"],
                 "verifiers": ["test-confound-scope.sh"]},
    # BOTH DRIVERS, and `mirror` is deliberately NOT claimed — for a reason that is
    # the design and not an omission.
    # ANCHOR, ONE LINE, DO NOT WRAP (the registry guard matches the whole name):
    # D-HARNESS-CONFOUND-SUPPLY-PS1-HALF-IS-IN-NO-REGION
    # The two halves answer the same question from GENUINELY DIFFERENT
    # TRANSPORTS: `leg_confound_patterns` reads emit_sh's flattened
    # `LEG_CONFOUNDS[leg]` / `LEG_CONFOUND_GATING[leg]` associative arrays, while
    # `Get-LegConfounds` reads the resolved leg OBJECT out of the JSON plan. A
    # differential case would therefore have to re-type one side's data in a shape
    # it never receives, which is the pin defect the bar names outright — and it is
    # how the .sh's refusal came to be proven by a pin that captured a rc the
    # production call site discards. So the pairing is what is enforced here (the
    # region must exist in BOTH drivers), and each half is driven THROUGH ITS OWN
    # REAL INPUT PATH by its own verifier.
    "confound-supply": {"drivers": ["build-and-test.sh", "build-and-test.ps1"],
                        "verifiers": ["test-confound-scope.sh",
                                      "test-driver-contracts.ps1"]},
    # TF: D-HARNESS-CONFOUND-SCOPE-IS-A-RUN-MODE-NOT-A-HOST. `mirror` IS claimed
    # here — unlike launcher-prereq/smoke-targets, this pair really is the same
    # PURE FUNCTION in two languages (take the resolver's report text, print one
    # tagged line per non-empty line, refuse an empty report), so the differential
    # battery can drive it on identical input and compare byte for byte. That is
    # the twin-parity proof for the visibility half of the gate: a driver that
    # silently stopped printing WHY a failure was excused would otherwise look
    # exactly like a driver with nothing to excuse.
    "confound-report": {
        "drivers": ["build-and-test.sh", "build-and-test.ps1"],
        "verifiers": ["harness_legs.py"], "mirror": True},
    # [D-HARNESS-BUILD-FAILURE-HAS-NO-PER-TU-ATTRIBUTION] BOTH DRIVERS, and
    # `mirror` deliberately NOT claimed — the two halves are the same CAPABILITY
    # in two languages, not the same text, and asserting a byte-sameness that was
    # never true is the defect the corpus-engine row records. What IS enforced is
    # the PAIRING: a driver that failed to ask "whose failure is this?" would
    # charge an upstream defect to dss on its side only, and the two drivers would
    # render different verdicts on the same tree — this project's canonical silent
    # harness bug. The DECISION itself is single-implementation in this module
    # (attribute_build_failure) and pinned by the self-test, so what the drivers
    # can still get wrong is asking at all, which is exactly what these verify.
    "build-attribution": {
        "drivers": ["build-and-test.sh", "build-and-test.ps1"],
        "verifiers": ["test-confound-scope.sh", "test-confound-scope.ps1"]},
    # [D-HARNESS-RUN-FIDELITY-IS-COMPUTED-BUT-NEITHER-RECORDED-NOR-SELECTABLE]
    # BOTH DRIVERS, `mirror` NOT claimed: the two halves read the fidelity through
    # genuinely different transports (the .sh from emit_sh's flattened
    # LEG_RUN_FIDELITY array, the .ps1 from `run.fidelity` in the JSON plan), which
    # is the same argument confound-supply makes. An operator switch honoured by
    # one driver and ignored by the other is the worst shape available here — the
    # ignoring side would run legs the operator excluded and report them as
    # covered, which is a FALSE claim of coverage rather than a missing one.
    "run-fidelity-select": {
        "drivers": ["build-and-test.sh", "build-and-test.ps1"],
        "verifiers": ["test-confound-scope.sh", "test-confound-scope.ps1"]},
    "loadext-stage": {"drivers": ["build-and-test.sh"],
                      "verifiers": ["test-confound-scope.sh",
                                    "test-confound-scope.ps1"]},
    "loadext-stage-ps1": {"drivers": ["build-and-test.ps1"],
                          "verifiers": ["test-confound-scope.ps1"]},
    "loadext-verdict": {"drivers": ["build-and-test.sh"],
                        "verifiers": ["test-confound-scope.sh"]},
    "verdict-vocabulary": {"drivers": ["build-and-test.sh"],
                           "verifiers": ["test-driver-contracts.sh"]},
    # ── the regions that are READER MARKERS and nothing more ────────────────
    # Each states WHY, because "nobody checks this" must be a decision on the
    # record rather than a gap someone has to notice.
    "selftest": {
        "drivers": ["build-and-test.sh", "build-and-test.ps1"],
        "verifiers": [],
        "why": "the region IS the refuse-to-start gate; it is exercised every "
               "time either driver starts, and its contents are pinned by "
               "tests/harness/test_sqlite_harness_legs.cpp rather than by "
               "sentinel extraction."},
    "run-lock": {
        "drivers": ["build-and-test.sh", "build-and-test.ps1"],
        "verifiers": [],
        "why": "a navigation marker over per-driver lock code that is not "
               "mirrored — the two hosts' locking primitives differ (flock vs a "
               "lock FILE), so there is no shared answer to compare."},
    "preflight": {
        "drivers": ["build-and-test.sh", "build-and-test.ps1"],
        "verifiers": [],
        "why": "a navigation marker; the preflight CALLS the corpus-engine "
               "helpers, which is where the shared logic lives and where the "
               "differential battery already reaches it."},
    "corpus-loop": {
        "drivers": ["build-and-test.sh", "build-and-test.ps1"],
        "verifiers": [],
        "why": "the per-leg driving loop: it is control flow over the "
               "corpus-engine helpers, and its OUTCOMES are pinned by "
               "tests/harness/test_sqlite_harness_legs.cpp (the segment-queue "
               "and verdict-ladder contracts) rather than by text comparison."},
    "artifact-report": {
        "drivers": ["build-and-test.sh"], "verifiers": [],
        "why": "a navigation marker over reporting code with no .ps1 twin."},
    "fresh-inode": {
        "drivers": ["build-and-test.sh"], "verifiers": [],
        "why": "a navigation marker over the POSIX inode-freshness install "
               "dance; there is no Windows twin (D-HARNESS-INODE64-MISBINDING "
               "is a Darwin/Linux concern)."},
    "fresh-inode-install": {
        "drivers": ["build-and-test.sh"], "verifiers": [],
        "why": "the second half of dss:fresh-inode; same reason."},
    "run-dir": {
        "drivers": ["build-and-test.sh"], "verifiers": [],
        "why": "a navigation marker; the run-directory PLAN it applies is "
               "resolved by harness_legs.py --run-dir-plan and pinned there."},
}


# ── THE MIRROR CONTRACT for a region declared `mirror: True` ────────────────
#
# The two copies are NOT text-identical and never could be: one is bash driving
# awk, the other is PowerShell driving .NET regex objects. A textual diff after
# "normalising incidental syntax" would be theatre in one direction (normalise
# hard enough and every real difference washes out) or noise in the other.
#
# So the contract is stated as CAPABILITIES, and checked two ways:
#
#   1. PAIRING. Every symbol defined in either copy is accounted for here —
#      paired with its twin, or declared single-driver WITH A REASON. A helper
#      added to one driver and not the other is a LOUD failure at the moment it
#      is added, which is the capability-pair defect class this project keeps
#      paying for ([[D-HARNESS-LIBRARY-ACQUISITION-BUILT-FOR-ONE-LEG-IN-ONE-
#      DRIVER]] and its siblings).
#
#   2. DIFFERENTIAL EXECUTION, for the pairs that are pure functions of their
#      input. Both copies are EXTRACTED FROM THE SHIPPED DRIVERS by their
#      sentinels, executed on byte-identical input, and their answers compared.
#      This is the half that catches a changed REGEX, which pairing alone never
#      would — and it is why this verifier is not theatre.
#
# `sh`/`ps1` name the defining symbol in each copy. `differential` names the
# battery case that drives the pair, or "" for a pair checked by presence only
# (a process-hygiene helper is not a pure function of its input and driving it
# would mean spawning processes to kill).
MIRROR_PAIRS = [
    {"sh": "corpus_files", "ps1": "Get-CorpusFiles", "differential": "corpus-files"},
    {"sh": "tier_prefixes", "ps1": "Get-TierPrefixes", "differential": "tier-prefixes"},
    {"sh": "tier_permutations", "ps1": "Get-TierPermutations",
     "differential": "tier-permutations"},
    {"sh": "parse_segment", "ps1": "Read-CorpusSegment", "differential": "parse-segment"},
    {"sh": "zero_progress_signature", "ps1": "Get-ZeroProgressSignature",
     "differential": "zero-progress-signature"},
    {"sh": "resolve_abort_file", "ps1": "Resolve-AbortFile",
     "differential": "resolve-abort-file"},
    {"sh": "files_after", "ps1": "Get-FilesAfter", "differential": "files-after"},
    {"sh": "our_fixture_pids", "ps1": "Get-OurFixtureProcesses", "differential": "",
     "why": "enumerates live processes; driving it differentially would mean "
            "spawning processes for the two shells to find and kill."},
    {"sh": "stop_our_fixtures", "ps1": "Stop-OurFixtures", "differential": "",
     "why": "kills processes; see our_fixture_pids."},
    {"sh": "run_fixture_segment", "ps1": "Invoke-Fixture", "differential": "",
     "why": "spawns the fixture with timeouts and output capture — the one "
            "helper here that is deliberately NOT a pure function."},
    {"sh": "fact", "ps1": None, "differential": "",
     "why": "the .sh persists parse_segment's answer as a TAB-separated FACT "
            "FILE (a subshell cannot return a structure); the .ps1 keeps the "
            "hashtable Read-CorpusSegment returns, so it needs no reader."},
    {"sh": "facts", "ps1": None, "differential": "", "why": "see fact."},
    {"sh": "group_digits", "ps1": None, "differential": "",
     "why": "thousands separators: PowerShell has ToString('N0') natively, bash "
            "has nothing locale-free."},
    {"sh": "str_gt", "ps1": None, "differential": "",
     "why": "byte-wise string comparison: PowerShell has "
            "[StringComparer]::Ordinal natively."},
    {"sh": "first_file_after", "ps1": None, "differential": "",
     "why": "a .sh-only hazard, not a missing capability. The sh driver runs under "
            "`set -Eeuo pipefail`, where `files_after | head -1` returns 141 (SIGPIPE) "
            "as soon as there is MORE THAN ONE match, because head closes the pipe — "
            "measured, and it killed a real run inside the resume path. This helper "
            "does the same job with `awk '{ print; exit }'`, no pipe. PowerShell has "
            "no pipeline exit status to propagate and no SIGPIPE: verified, 5000 items "
            "through `Select-Object -First 1` completes cleanly, and the .ps1's "
            "Get-FilesAfter callers consume the WHOLE list anyway. So a twin would be "
            "ceremony, not coverage."},
    {"sh": "ps_enum_available", "ps1": None, "differential": "",
     "why": "probes whether `ps -eo pid=,args=` can enumerate at all; the .ps1 "
            "uses Get-CimInstance, which has no equivalent failure mode to "
            "probe for."},
    {"sh": "leg_loader_path_var", "ps1": None, "differential": "",
     "why": "the .ps1 resolves the loader variable ONCE per leg into "
            "$LegLoaderVar at Step 8, outside this region; the .sh needs it as "
            "a function because run_leg re-execs the shell."},
    {"sh": "run_leg", "ps1": None, "differential": "",
     "why": "the .sh REPLACES its own process with the launcher; the .ps1 "
            "spawns via Start-Process inside Invoke-Fixture, so there is no "
            "separate exec step."},
    {"sh": None, "ps1": "Get-OurFixtureProcessesUnder", "differential": "",
     "why": "matches a fixture by the DIRECTORY it runs under, which the .ps1 "
            "needs because a Windows image path can be spelled several ways; "
            "the .sh matches the full argv directly."},
    {"sh": None, "ps1": "Stop-OurFixturesUnder", "differential": "",
     "why": "see Get-OurFixtureProcessesUnder."},
    {"sh": None, "ps1": "Stop-FixtureProcesses", "differential": "",
     "why": "the shared tail of Stop-OurFixtures / Stop-OurFixturesUnder; the "
            ".sh has one caller and needs no split."},
    # ── dss:confound-report ─────────────────────────────────────────────────
    {"sh": "print_confound_report", "ps1": "Write-ConfoundReport",
     "differential": "confound-report"},
]

# The symbol-definition grammar of each driver language, used to enumerate what
# a region DEFINES. Deliberately anchored at column 0: a nested definition is
# not a capability of the region, it is an implementation detail of one.
MIRROR_SYMBOL_RE = {
    "sh": re.compile(r"^([A-Za-z_][A-Za-z0-9_]*)\s*\(\)\s*\{"),
    "ps1": re.compile(r"^function\s+([A-Za-z][A-Za-z0-9-]*)\s*(\(|\{)"),
}


def dss_region_spans(text, name):
    """[(start, end)] line indices (0-based, inclusive) of every `dss:<name>`
    region in `text`. A region whose sentinels are unbalanced or crossed yields
    a LegError — an unreadable marker is not a region, and silently returning
    nothing would read exactly like "this region is not here"."""
    opens, closes = [], []
    for i, line in enumerate(text.split("\n")):
        if re.search(r">>>\s*dss:%s\s*>>>" % re.escape(name), line):
            opens.append(i)
        if re.search(r"<<<\s*dss:%s\s*<<<" % re.escape(name), line):
            closes.append(i)
    if len(opens) != len(closes):
        raise LegError("dss:%s has %d opening and %d closing sentinel(s) — an "
                       "unbalanced region cannot be extracted, and a verifier "
                       "that silently extracted the wrong lines would be worse "
                       "than none" % (name, len(opens), len(closes)))
    spans = []
    for o, c in zip(opens, closes):
        if c <= o:
            raise LegError("dss:%s closes at line %d before it opens at line %d"
                           % (name, c + 1, o + 1))
        spans.append((o, c))
    return spans


def dss_region_text(text, name):
    """The region's body, sentinel lines EXCLUDED. Multiple spans concatenate in
    file order — the .sh's dss:corpus-engine is one span today, but a region
    that grew a second one must not silently lose it."""
    lines = text.split("\n")
    out = []
    for start, end in dss_region_spans(text, name):
        out.extend(lines[start + 1:end])
    return "\n".join(out)


def dss_region_symbols(text, lang):
    """The symbols a region body DEFINES, in file order."""
    rx = MIRROR_SYMBOL_RE[lang]
    out = []
    for line in text.split("\n"):
        m = rx.match(line)
        if m and m.group(1) not in out:
            out.append(m.group(1))
    return out


# ── THE DIFFERENTIAL BATTERY ────────────────────────────────────────────────
#
# THE HALF THAT MAKES THIS A VERIFIER RATHER THAN A HEADCOUNT. Both copies are
# EXTRACTED FROM THE SHIPPED DRIVERS by their sentinels — never re-typed here —
# executed on byte-identical input, and their answers compared line for line.
#
# ★ EXACTLY WHAT IS NORMALISED, AND WHY EACH IS INCIDENTAL RATHER THAN SEMANTIC.
# This list is the whole defence against a vacuous pass, so it is short and it is
# stated rather than buried:
#
#   1. LINE ENDINGS. PowerShell writes CRLF on Windows; CRLF -> LF on both sides.
#      Nothing in this answer vocabulary can contain a line terminator, so no
#      semantic difference can hide inside this.
#   2. TRAILING WHITESPACE on each emitted line, and trailing blank lines at the
#      end of the capture. `printf` and the PowerShell pipeline pad differently.
#      Leading whitespace is NOT stripped: it is inside the value.
#   3. THE FAILING-TEST-NAME SET is emitted SORTED by both sides. The .sh streams
#      names as it meets them and the .ps1 accumulates them in a hashtable; both
#      drivers CONSUME the answer as a set (the classifier iterates it). This is
#      the ONLY answer compared as a set. Every other answer — the completed-file
#      list, the permutation, the last test, all counts — is compared as an
#      ORDERED list, where order and multiplicity are semantic and a difference
#      reds.
#
# NOTHING ELSE. No case folding, no whitespace collapsing inside a value, no
# numeric coercion, no truncation, no "ignore if empty".
#
# The projection into a common answer vocabulary for parse_segment is the .sh
# region's OWN documented fact alphabet (F/X/S/E/C/P/T/G/N/D/K/Q/A) — not a new
# one invented here — and the .sh side reads it back through the region's own
# `fact`/`facts` helpers, so those are exercised rather than bypassed.
MIRROR_PRELUDE_SH = """set -Eeuo pipefail
warn() { :; }
info() { :; }
die()  { echo "DIE: $*" >&2; exit 9; }
"""

MIRROR_PRELUDE_PS1 = """$ErrorActionPreference = 'Stop'
function Warn($m) { }
function Info($m) { }
function Die($m)  { Write-Error $m; exit 9 }
"""

MIRROR_CASES = {
    "corpus-files": {
        "region": "corpus-engine",
        "sh": 'corpus_files "$CORPUSDIR"',
        "ps1": "foreach ($f in (Get-CorpusFiles $CORPUSDIR)) { $f }",
    },
    "tier-permutations": {
        "region": "corpus-engine",
        "sh": 'tier_permutations "$TIERFILE"',
        "ps1": "foreach ($p in (Get-TierPermutations $TIERFILE)) { $p }",
    },
    "tier-prefixes": {
        "region": "corpus-engine",
        "sh": 'tier_prefixes "$PERMSFILE"',
        "ps1": "foreach ($p in (Get-TierPrefixes $PERMSFILE)) { $p }",
    },
    "resolve-abort-file": {
        "region": "corpus-engine",
        "sh": ('while IFS= read -r nm; do\n'
               '  printf "%s\\t%s\\n" "$nm" "$(resolve_abort_file "$nm" "$LISTFILE")"\n'
               'done < "$NAMESFILE"'),
        "ps1": ("$corpus = @(Get-Content -LiteralPath $LISTFILE)\n"
                "foreach ($nm in (Get-Content -LiteralPath $NAMESFILE)) {\n"
                "  \"$nm`t$(Resolve-AbortFile $nm $corpus)\" }"),
    },
    # ── THE ABORTING FILE, END TO END, ON A REAL LOG ────────────────────────
    #
    # D-HARNESS-ABORT-FILE-NAMED-ONLY-BY-THE-TRACEBACK. This is the ONE case that
    # drives the WHOLE chain the resume engine actually uses — parse_segment ->
    # the B fact -> resolve_abort_file — rather than a helper in isolation, and
    # it drives it on `REAL_ABORT_SEGMENT_LOG`, the verbatim bytes of a segment
    # log this harness really produced and really failed to read (provenance on
    # that constant).
    #
    # ★ IT CARRIES AN `expect`, so it is checked for being RIGHT and not merely
    # for the two drivers AGREEING. Agreement is the property the rest of this
    # battery can offer, and it is a real one, but two copies of a resolver that
    # both answer "" agree perfectly — which is precisely the state this case was
    # written to end.
    "abort-file-from-traceback": {
        "region": "corpus-engine",
        "sh": ('parse_segment "$ABORTLOG" "$ABORTFACTS"\n'
               'facts B "$ABORTFACTS" | sed "s/^/B /"\n'
               'printf "T %s\\n" "$(fact T "$ABORTFACTS")"\n'
               'printf "N %s\\n" "$(fact N "$ABORTFACTS")"\n'
               'printf "FILE %s\\n" '
               '"$(resolve_abort_file "$(fact B "$ABORTFACTS")" "$LISTFILE")"'),
        "ps1": ("$corpus = @(Get-Content -LiteralPath $LISTFILE)\n"
                "$r = Read-CorpusSegment $ABORTLOG\n"
                "foreach ($b in $r.Blamed) { \"B $b\" }\n"
                "\"T $($r.LastTest)\"\n"
                "\"N $($r.Completed.Count)\"\n"
                "$b = if ($r.Blamed.Count) { $r.Blamed[$r.Blamed.Count - 1] } else { '' }\n"
                "\"FILE $(Resolve-AbortFile $b $corpus)\""),
        # The exact answer, stated here rather than derived, because a derived
        # expectation is the same code twice. `T` and `N` are EMPTY/0 on purpose:
        # they are what the old resolver had to work with, and they are why it
        # could say nothing. `B` is the innermost frame of the ONE traceback in
        # that log — the outer `permutations.test` frame of the same block is
        # correctly NOT emitted.
        "expect": [
            "B Z:/home/rafael/src/sqlite/test/symlink2.test",
            "T",
            "N 0",
            "FILE symlink2.test",
        ],
    },
    # ── THE ZERO-PROGRESS SIGNATURE, WITH THE RIGHT ANSWER STATED ───────────
    #
    # D-HARNESS-PRECONDITION-DISCRIMINATOR-BLIND-TO-A-SILENT-CRASH. This decides
    # whether a leg spends ONE resume on a fixture that never started or all ten,
    # so the two copies agreeing is not enough — row 2 is the whole defect and
    # rows 3-5 are the resilience rule, and both are asserted by `expect` rather
    # than by the two drivers agreeing with each other.
    #
    # ⚠ THE SENTINEL IS ASCII ON PURPOSE. It is compared BYTE-FOR-BYTE between a
    # bash string and a PowerShell string that reach this battery through two
    # different file readers; a non-ASCII character would put an encoding question
    # inside the one value the discriminator rests on.
    #
    # ⓘ The arguments are LITERALS rather than a fixture file: this pair is a pure
    # function of four scalars, and a table file would add a TSV-parsing step to
    # each arm whose divergence this case would then be measuring instead.
    "zero-progress-signature": {
        "region": "corpus-engine",
        "sh": ('printf "1 %s\\n" "$(zero_progress_signature "boom: cannot open libtcl" 0 0 "")"\n'
               'printf "2 %s\\n" "$(zero_progress_signature "" 0 0 "")"\n'
               'printf "3 %s\\n" "$(zero_progress_signature "" 5 0 "")"\n'
               'printf "4 %s\\n" "$(zero_progress_signature "" 0 2 "")"\n'
               'printf "5 %s\\n" "$(zero_progress_signature "" 0 0 "select1-1.1")"\n'
               'printf "6 %s\\n" "$(zero_progress_signature "boom" 9 9 "x")"'),
        "ps1": ('"1 $(Get-ZeroProgressSignature \'boom: cannot open libtcl\' 0 0 \'\')"\n'
                '"2 $(Get-ZeroProgressSignature \'\' 0 0 \'\')"\n'
                '"3 $(Get-ZeroProgressSignature \'\' 5 0 \'\')"\n'
                '"4 $(Get-ZeroProgressSignature \'\' 0 2 \'\')"\n'
                '"5 $(Get-ZeroProgressSignature \'\' 0 0 \'select1-1.1\')"\n'
                '"6 $(Get-ZeroProgressSignature \'boom\' 9 9 \'x\')"'),
        # 1 — a diagnostic is returned verbatim (the case that already worked).
        # 2 — SILENCE gets the sentinel. This is the defect: it used to be empty,
        #     so two consecutive silent segments never compared equal and the leg
        #     burned all ten resumes on a fixture that had never started.
        # 3/4/5 — no diagnostic BUT the segment produced test-level output: the
        #     answer is EMPTY, i.e. keep resuming. These three are the resilience
        #     rule, and they are why the sentinel is not simply `${diag:-…}`.
        # 6 — a diagnostic wins over every counter.
        "expect": [
            "1 boom: cannot open libtcl",
            "2 <SILENT: the fixture produced no diagnostic, no test result and no test name>",
            "3",
            "4",
            "5",
            "6 boom",
        ],
    },
    "files-after": {
        "region": "corpus-engine",
        "sh": 'files_after "$BOUNDARY" "$LISTFILE"',
        "ps1": ("$corpus = @(Get-Content -LiteralPath $LISTFILE)\n"
                "foreach ($f in (Get-FilesAfter $corpus $BOUNDARY)) { $f }"),
    },
    # THE ONE THAT CARRIES THE PARSING SEMANTICS. Both sides project into the
    # .sh region's own fact alphabet; see the normalisation note above.
    "parse-segment": {
        "region": "corpus-engine",
        # ★ `I` AND `M` ARE PROJECTED HERE OR THEY ARE NOT COMPARED AT ALL. This
        #   projection is a FIXED alphabet, so a fact the two engines both
        #   compute but neither side lists is silently outside the differential —
        #   and `--check-regions` still reports "identical answers", which reads
        #   as coverage. The inert counter is exactly the kind of thing that
        #   needs this: it exists in two independent implementations (an awk rule
        #   and a PowerShell regex) whose ONLY guarantee of agreement is this
        #   comparison, and the teardown-matching detail they hinge on is one a
        #   first cut already got wrong once.
        "sh": ('parse_segment "$LOGFILE" "$FACTFILE"\n'
               'facts F "$FACTFILE"\n'
               'facts I "$FACTFILE" | sed "s/^/I /"\n'
               'facts X "$FACTFILE" | LC_ALL=C sort -u | sed "s/^/X /"\n'
               'facts B "$FACTFILE" | sed "s/^/B /"\n'
               'for k in S E C P T G N M D K Q A; do\n'
               '  printf "%s %s\\n" "$k" "$(fact "$k" "$FACTFILE")"\n'
               'done'),
        "ps1": ("$r = Read-CorpusSegment $LOGFILE\n"
                "foreach ($f in $r.Completed) { $f }\n"
                "foreach ($f in $r.Inert) { \"I $f\" }\n"
                "foreach ($n in ($r.FailNames.Keys | Sort-Object)) { \"X $n\" }\n"
                "foreach ($b in $r.Blamed) { \"B $b\" }\n"
                "\"S $($r.Summary)\"\n"
                "\"E $(if ($r.Summary) { $r.Errors } else { '' })\"\n"
                "\"C $(if ($r.Summary) { $r.Tests } else { '' })\"\n"
                "\"P $($r.Permutation)\"\n"
                "\"T $($r.LastTest)\"\n"
                "\"G $(if ($r.GaveUp) { '1' } else { '' })\"\n"
                "\"N $($r.Completed.Count)\"\n"
                "\"M $($r.Inert.Count)\"\n"
                "\"D $(if ($r.Completed.Count) { $r.Completed[$r.Completed.Count - 1] } else { '' })\"\n"
                "\"K $($r.OkLines)\"\n"
                "\"Q $($r.FailMarkers)\"\n"
                "\"A $($r.Diagnostic)\""),
    },
    # ── THE VISIBILITY HALF OF THE CONFOUND GATE ────────────────────────────
    #
    # D-HARNESS-CONFOUND-SCOPE-IS-A-RUN-MODE-NOT-A-HOST.
    #
    # A confound row may be honoured only where a named environment probe MEASURED
    # its defect. `earnedOn` failed because it is prose nothing reads, so a probe
    # verdict nobody SEES would be the same failure with extra steps — which makes
    # the printing a load-bearing capability and not decoration. A driver that
    # silently stopped printing the account would look exactly like a driver with
    # nothing to excuse.
    #
    # ★ THE INPUT IS THE RESOLVER'S OWN TEXT, PASSED THROUGH — the report is
    # generated ONCE in harness_legs.py precisely so the two drivers cannot compose
    # different accounts of the same decision. So what this case proves is the part
    # that IS duplicated: line splitting, blank-line handling, CR tolerance, the
    # `Info` tagging, and the refusal of an empty report.
    #
    # ⚠ EVERY LINE HERE IS ASCII, and confound_report_lines ASSERTS that at the
    # generator: this text is compared byte-for-byte between a bash string and a
    # PowerShell string that reach the battery through two different file readers,
    # so a non-ASCII character would make this a test of two codepages. The `\r` in
    # row 3 is deliberate — the .ps1 reads CRLF natively and the .sh does not, and a
    # trailing CR left on a line would show up in a log as a mangled tag.
    # ⓘ The empty-report REFUSAL is exercised by its own case below rather than
    # here: a `Die` arm cannot share a capture with a success arm.
    # ⓘ `info`/`Info` are NO-OPS in the shared prelude, because every corpus-engine
    # case needs the drivers silent. So this case OVERRIDES them per arm to echo
    # with a marker: the emission is the answer here, and a battery that compared
    # two silences would pass over a driver that had stopped reporting.
    "confound-report": {
        "region": "confound-report",
        # ★ THE CR CLAIM, MADE REAL. [D-HARNESS-MIRROR-CR-CLAIM-IS-VACUOUS.] Row 3
        # of the fixture carries a TRAILING CR, and this flag is what makes that
        # fixture prove something: each arm's RAW output is checked for a CR beyond
        # its own line terminator, BEFORE _mirror_normalise strips CR from both.
        # Without it the row proved nothing at all — ✔MEASURED, and it was hiding a
        # real asymmetry (the .ps1 trimmed, the .sh did not).
        "crClean": True,
        "sh": ('info() { printf "REPORT> %s\\n" "$*"; }\n'
               'print_confound_report "elf64-x86_64" "$REPORT"'),
        "ps1": ('function Info($m) { "REPORT> $m" }\n'
                'Write-ConfoundReport "elf64-x86_64" $REPORT'),
        "expect": [
            "REPORT> [elf64-x86_64] environment probe clock-realtime-steps = ABSENT   [wall-clock-step: 0 step(s)]",
            "REPORT> [elf64-x86_64] confound rows ACTIVE (1 of 2): ^zipfile-25.0$",
            "REPORT> [elf64-x86_64] confound row INACTIVE: ^walsetlk- - NOT honoured here: clock-realtime-steps: absent",
        ],
    },
}


def _ps1_single_quote(value):
    """A PowerShell single-quoted literal: only `'` needs doubling inside one."""
    return "'%s'" % str(value).replace("'", "''")


def _mirror_normalise(raw):
    """See the normalisation note above: line endings, then trailing whitespace,
    then trailing blank lines. Nothing else."""
    lines = raw.replace("\r\n", "\n").replace("\r", "\n").split("\n")
    lines = [ln.rstrip() for ln in lines]
    while lines and lines[-1] == "":
        lines.pop()
    return lines


# ── THE ONE PROPERTY THE NORMALISER ERASES, CHECKED BEFORE IT RUNS ──────────
#
# [D-HARNESS-MIRROR-CR-CLAIM-IS-VACUOUS.]
#
# ★★★ A COMMENT CLAIMING A PROOF THAT DOES NOT EXIST. The confound-report fixture
# carries a TRAILING CR on row 3 and its comment said that row proves the two
# copies handle CR alike. It cannot: `_mirror_normalise` strips every `\r` and
# then rstrips, from BOTH arms, BEFORE the comparison. ✔MEASURED: with the .sh
# NOT trimming and the .ps1 trimming (`$line.TrimEnd("`r")`), the raw bytes
# differed and the normalised lines were IDENTICAL — so the differential passed
# over a real asymmetry while a comment credited it with catching one.
#
# ⇒ THE CLAIM IS MADE REAL HERE, ON THE RAW BYTES, BEFORE NORMALISATION. The
# instrument is per-INTERPRETER because the thing being distinguished is each
# interpreter's OWN line terminator: bash's `printf '\n'` emits LF, pwsh emits
# CRLF, so "a CR that is not the terminator" is `\r` surviving a split on that
# arm's own EOL. Declared as a table for the same reason MIRROR_SYMBOL_RE is —
# the languages really do differ here, and stating it beats inferring it.
MIRROR_RAW_EOL = {"sh": "\n", "ps1": "\r\n"}


def mirror_stray_cr_lines(raw, lang):
    """The lines of one arm's RAW output that carry a CR beyond this
    interpreter's own line terminator. `[]` is the pass."""
    if lang not in MIRROR_RAW_EOL:
        raise LegError("no line terminator declared for arm %r (known: %s)"
                       % (lang, ", ".join(sorted(MIRROR_RAW_EOL))))
    return [ln for ln in raw.split(MIRROR_RAW_EOL[lang]) if "\r" in ln]


# ── A REAL SEGMENT LOG, VERBATIM ────────────────────────────────────────────
#
# D-HARNESS-ABORT-FILE-NAMED-ONLY-BY-THE-TRACEBACK. These are the bytes of a log
# THIS HARNESS PRODUCED AND THEN COULD NOT READ — not a reconstruction of one.
#
#   provenance  build/real-examples/c/sqlite/pe64-x86_64/corpus.resume1.log,
#               captured 2026-08-06, 1621 bytes, md5
#               7c32798f56de31a672648722447eefc5. Split on CRLF and re-joined
#               with it below; the round-trip was checked against that md5.
#
# WHAT IT IS: the pe64-x86_64 testfixture under the `wine` launcher on a Linux
# host, re-entering symlink2.test after the previous segment aborted inside it.
# It died at symlink2.test line 48 — inside canCreateWin32Symlink, BEFORE the
# first do_test — so the log contains not one `name...` line and the T fact is
# EMPTY. The harness reported "the UNNAMED file that aborted ... (last test:
# none)", forced the resume boundary past symlink2.test, and that unit went
# through the entire run WITHOUT A VERDICT.
#
# And the file is named, in plain text, on line 10:
#     (file "Z:/home/rafael/src/sqlite/test/symlink2.test" line 48)
#
# ⚠ DO NOT TIDY THIS. The `Z:` prefix, the backslash-spelled first line, the CRLF
# terminators, the outer `permutations.test` frame that must NOT win, and the
# truncated `run_tests … vacuum5.test..."` argv dump are each a shape one of the
# two copies has to survive. A cleaned-up fixture is a fixture for a log that
# does not exist.
REAL_ABORT_SEGMENT_LOG = [
    b'Z:\\home\\rafael\\src\\dss-code-prime\\build\\real-examples\\c\\sqlite\\pe64-x86_64\\pe64-x86_64-windows-exec\\testfixture.exe: Z:\\home\\rafael\\src\\sqlite\\test\\lnk220.sym: File Not Found',
    b'    while executing',
    b'"exec -- $::env(ComSpec) /c del [file nativename $link]"',
    b'    (procedure "deleteWin32Symlink" line 2)',
    b'    invoked from within',
    b'"deleteWin32Symlink $link"',
    b'    (procedure "canCreateWin32Symlink" line 6)',
    b'    invoked from within',
    b'"canCreateWin32Symlink"',
    b'    (file "Z:/home/rafael/src/sqlite/test/symlink2.test" line 48)',
    b'    invoked from within',
    b'"source Z:/home/rafael/src/sqlite/test/symlink2.test"',
    b'    invoked from within',
    b'"interp eval tinterp $script"',
    b'    (procedure "slave_test_script" line 30)',
    b'    invoked from within',
    b'"slave_test_script [list source $zFile] "',
    b'    invoked from within',
    b'"time { slave_test_script [list source $zFile] }"',
    b'    (procedure "slave_test_file" line 23)',
    b'    invoked from within',
    b'"slave_test_file $file"',
    b'    (procedure "run_tests" line 36)',
    b'    invoked from within',
    b'"run_tests veryquick -presql {} -files {shared3.test func7.test upfrom4.test Z:/home/rafael/src/sqlite/test/../ext/fts5/test/fts5misc.test vacuum5.test..."',
    b'    ("eval" body line 1)',
    b'    invoked from within',
    b'"eval [list run_tests $suite] $S $extra"',
    b'    (procedure "main" line 34)',
    b'    invoked from within',
    b'"main $argv"',
    b'    (file "/home/rafael/src/sqlite/test/permutations.test" line 1270)',
    b'    invoked from within',
    b'"source $argv0"',
    b'    invoked from within',
    b'"if {[llength $argv]>=1} {',
    b'set new [list]',
    b'foreach arg $argv {',
    b'if {[string match -* $arg] || [file exists $arg]} {',
    b'lappend new $arg',
    b'} else {',
    b'set once 0',
    b'..."',
]


def _mirror_write_fixtures(work):
    """The byte-identical inputs both copies are driven with. Written ONCE and
    handed to both, so the two answers are answers to the same question.

    Every value here is chosen because some line of the region reads it: the
    exclusion block sqlite's permutations.test really writes, a `-prefix ""`
    suite and a `-prefix "mm-"` one, a CRLF log line, a summary with trailing
    text after the counts, a `!Failures on these tests:` line with its leading
    bang, and a traceback frame whose closing quote abuts the name."""
    corpus = os.path.join(work, "corpus")
    os.makedirs(corpus)
    for name in ["alter.test", "wal2.test", "swarmvtab.test",
                 "swarmvtabfault.test", "zipfile.test", "walsetlk.test",
                 "all.test", "permutations.test", "veryquick.test"]:
        with open(os.path.join(corpus, name), "w", encoding="utf-8") as fh:
            fh.write("# %s\n" % name)
    perms = os.path.join(corpus, "permutations.test")
    with open(perms, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(
            "set alltests [test_set $alltests -exclude {\n"
            "  all.test permutations.test\n"
            "  veryquick.test\n"
            "}]\n"
            'test_suite "veryquick" -prefix "" -description {\n'
            "}\n"
            'test_suite "mmap" -prefix "mm-" -description {\n'
            "}\n"
            'test_suite "inmemory_journal" -description {\n'
            "}\n")
    tier = os.path.join(work, "tier.test")
    with open(tier, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("run_test_suite veryquick\n"
                 "  run_test_suite inmemory_journal\n"
                 "# run_test_suite mmap\n")
    # BYTE-SORTED, like the real thing: files_after and first_file_after are
    # ordinal comparisons over it. `symlink.test` sorts BEFORE `symlink2.test`
    # ('.' 0x2E < '2' 0x32), which is exactly the adjacency that made the real
    # defect visible — the name-based resolver answered the first while the
    # traceback named the second.
    listfile = os.path.join(work, "corpus.lst")
    with open(listfile, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("alter.test\nswarmvtab.test\nswarmvtabfault.test\n"
                 "symlink.test\nsymlink2.test\n"
                 "wal2.test\nwalsetlk.test\nzipfile.test\n")
    # ★ THE LAST FOUR ARE PATHS, AND THEY ARE THE POINT OF THIS FIXTURE NOW.
    # resolve_abort_file is handed a `(file "…" line N)` frame as well as a test
    # NAME, so it has to answer the same thing for every spelling of the same
    # file: the launcher's drive-letter one (wine maps the Linux root to Z:), the
    # driver's own POSIX one (the same log carries both), the backslash form, and
    # a bare basename.
    # ⚠ THE BACKSLASH ROW IS LOAD-BEARING AND NOT DECORATION: it is the row that
    # reds if the .sh copy ever goes back to `awk -v name="$1"`, whose escape
    # processing turns `Z:\home\rafael\test` into `Z:homeafael<TAB>est`
    # (✔MEASURED, gawk 5.3.2). The .ps1 has no such hazard, so the two answers
    # DIVERGE and this battery says so.
    namesfile = os.path.join(work, "names.lst")
    with open(namesfile, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("inmemory_journal.swarmvtabfault-1.1-oom-persistent.143\n"
                 "mm-wal2-3.3\n"
                 "walsetlk-2.1.3\n"
                 "nothing-matches-here.1\n"
                 "symlink.test-sharedcachesetting\n"
                 "Z:/home/rafael/src/sqlite/test/symlink2.test\n"
                 "/home/rafael/src/sqlite/test/symlink2.test\n"
                 "Z:\\home\\rafael\\src\\sqlite\\test\\symlink2.test\n"
                 "symlink2.test\n")
    log = os.path.join(work, "segment.log")
    # BINARY, so the CRLF line below really is CRLF on every host. The .sh
    # region strips a trailing CR in its first awk rule and says why; if either
    # copy stops doing that, this fixture is what notices.
    with open(log, "wb") as fh:
        fh.write(b"Can't find a usable init.tcl in the following directories:\n"
                 b"alter-1.1... Ok\n"
                 b"Time: alter.test 12 ms\n"
                 b"wal2-2.1... Ok\r\n"
                 b"Time: wal2.test 34 ms\n"
                 b'"run_test_suite inmemory_journal"\n'
                 b"! walsetlk-2.1.3 expected: [1]\n"
                 b"! walsetlk-2.1.3 got: [0]\n"
                 b"!Failures on these tests: walsetlk-2.1.3 zipfile-25.0\n"
                 # ── TWO TRACEBACK BLOCKS, which is what the B rule is for ────
                 # Block 1 has TWO frames and must contribute exactly its FIRST
                 # (Tcl prints errorInfo innermost-first, so the outer
                 # permutations.test frame is the driver, not the unit). The
                 # `Time:` line then RESETS the block flag, so block 2
                 # contributes its own — spelled with BACKSLASHES, which the
                 # extraction must pass through untouched.
                 b'    (file "/opt/x/wal2.test" line 12)\n'
                 b'    (file "/opt/x/permutations.test" line 99)\n'
                 b"Time: zipfile.test 7 ms\n"
                 b'    (file "Z:\\opt\\x\\zipfile.test" line 7)\n'
                 # ── AN INERT FILE, AND THE TEARDOWN SHAPE THAT DEFINES ONE ───
                 # `swarmvtab.test` here emits ONLY the two results the harness
                 # emits for every file, so both copies must call it inert; the
                 # files above emit real results and must not be. Without these
                 # lines the differential could not see the inert counter at all
                 # — and the counter's whole correctness hinges on a detail this
                 # is the only fixture that carries: the teardown names end in
                 # `...` with NO SPACE before it, so a matcher anchored on
                 # `-closeallfiles$` never fires and every file looks busy. That
                 # was a real first cut, and it reported 0 inert over a corpus
                 # with 362.
                 b"swarmvtab.test-closeallfiles... Ok\n"
                 b"swarmvtab.test-sharedcachesetting... Ok\n"
                 b"Time: swarmvtab.test 2 ms\n"
                 b"*** Giving up...\n"
                 b"2 errors out of 41 tests on somehost Linux 64-bit\n")
    # ── THE REAL LOG, VERBATIM ──────────────────────────────────────────────
    # Written as BYTES, CRLF preserved, because it is a real Windows-fixture log
    # and the CR strip is one of the things it proves.
    abortlog = os.path.join(work, "abort-segment.log")
    with open(abortlog, "wb") as fh:
        fh.write(b"\r\n".join(REAL_ABORT_SEGMENT_LOG) + b"\r\n")
    # ★ RELATIVE, and both arms run with `work` as their CWD. MEASURED while
    # writing this: handing Git Bash an absolute `C:\Users\…` path ate every
    # backslash (`C:UsersrafaeAppData…`), and an absolute path is a namespace
    # claim this battery has no business making — the two arms only have to
    # agree with EACH OTHER, and a bare relative name means the same thing to
    # both interpreters on every host.
    # ── THE CONFOUND REPORT, AS A MULTI-LINE SCALAR ─────────────────────────
    # Passed as a VALUE rather than a file, because that is how the drivers really
    # receive it (one newline-joined variable out of the resolver: LEG_CONFOUND_
    # REPORT / $leg.confoundReport), and a file would add a reader whose divergence
    # this case would then be measuring instead.
    #
    # ★ THE THREE THINGS IT MAKES THE ARMS PROVE, none of which is free:
    #   · row 3 carries a TRAILING CR, which each arm must strip BEFORE printing.
    #     ⚠ THAT CLAIM STOOD HERE FOR A CYCLE AND WAS VACUOUS: _mirror_normalise
    #     strips every CR from BOTH arms before comparing, so a surviving CR was
    #     indistinguishable from pwsh's own CRLF — ✔MEASURED, raw bytes differed and
    #     the normalised lines were identical, while the .sh did not trim at all.
    #     It is real now because the case declares `crClean` and the battery checks
    #     each arm's RAW output. [D-HARNESS-MIRROR-CR-CLAIM-IS-VACUOUS]
    #   · there is a BLANK line, which both must skip rather than print as a tag
    #     with nothing after it.
    #   · a line contains `%s` and one contains a `$`, so an arm that pushed the
    #     text through a FORMAT string or a second expansion diverges loudly.
    # ⚠ ASCII only, deliberately: see confound_report_lines, which asserts it at the
    # generator. This is compared byte-for-byte across two file readers.
    report = ("[elf64-x86_64] environment probe clock-realtime-steps = ABSENT   "
              "[wall-clock-step: 0 step(s)]\n"
              "\n"
              "[elf64-x86_64] confound rows ACTIVE (1 of 2): ^zipfile-25.0$\r\n"
              "[elf64-x86_64] confound row INACTIVE: ^walsetlk- - NOT honoured "
              "here: clock-realtime-steps: absent")
    return {"CORPUSDIR": "corpus", "PERMSFILE": "corpus/permutations.test",
            "TIERFILE": "tier.test", "LISTFILE": "corpus.lst",
            "NAMESFILE": "names.lst", "LOGFILE": "segment.log",
            "FACTFILE": "facts.tsv", "BOUNDARY": "swarmvtab.test",
            "ABORTLOG": "abort-segment.log", "ABORTFACTS": "abort-facts.tsv",
            "REPORT": report}


# The two languages a mirrored region is written in, in the order every report
# names them. Iterating this instead of spelling `("sh", "ps1")` at each site is
# what lets the availability probe, the spawn and the skip accounting stay one
# fact — see mirror_interpreter.
MIRROR_LANGS = ("sh", "ps1")


def mirror_case_inventory(case):
    """The assertions one differential case OWNS, as (lang, kind) pairs — the
    same list on every host, whatever that host can execute.

    `lang is None` marks the one assertion that genuinely needs BOTH arms (the
    comparison); every other entry is a PER-ARM correctness property that stands
    on its own. This is the list the verifier reconciles its own counters
    against, so "the inventory is host-independent" is CHECKED rather than
    claimed — add an assertion to the battery without adding it here and the
    reconciliation reds."""
    spec = MIRROR_CASES[case]
    inv = []
    for lang in MIRROR_LANGS:
        if spec.get("crClean"):
            inv.append((lang, "crClean"))
        if spec.get("expect") is not None:
            inv.append((lang, "expect"))
    inv.append((None, "identical"))
    return inv


def mirror_interpreter(lang):
    """The interpreter argv PREFIX one arm is executed with.

    ★ NAMED ONCE, because the AVAILABILITY PROBE and the SPAWN must be talking
    about the same program. They were two literals — `_sh.which("pwsh")` in the
    verifier and `["pwsh", …]` in the runner — and a pair like that fails in
    both directions: probe a spelling the spawn does not use and a "present"
    interpreter OSErrors into a FAIL; probe a spelling the spawn does not use
    the other way and an available interpreter is skipped as absent. The probe
    below takes argv[0] of THIS list, so neither can happen."""
    if lang == "sh":
        # `$BASH` first: on macOS a bare `bash` is /bin/bash 3.2, not the bash 4+
        # the drivers re-exec themselves into — the same rule
        # D-HARNESS-SELFTEST-BSD-SED-PORTABILITY made for the self-tests.
        return [os.environ.get("BASH", "bash")]
    return ["pwsh", "-NoProfile", "-NonInteractive", "-File"]


def _mirror_run(lang, region, case, work, env):
    """(ok, lines, detail, raw) — one copy's answer, or why it could not be had.

    `raw` is the UNNORMALISED stdout, carried out so a case can assert a property
    the normaliser erases — see mirror_stray_cr_lines and, anchor on ONE LINE,
    D-HARNESS-MIRROR-CR-CLAIM-IS-VACUOUS."""
    import subprocess  # local, matching the rest of this module's spawn sites
    body = MIRROR_CASES[case][lang]
    # The script is named RELATIVELY and the child's cwd is `work`, for the same
    # reason the fixtures are: an absolute Windows path handed to Git Bash loses
    # its backslashes.
    # ★ THE FIXTURE PATHS ARE WRITTEN INTO THE SCRIPT, NOT PASSED THROUGH THE
    # ENVIRONMENT. ✔MEASURED while writing this: Git Bash's `bash`, spawned from
    # Python with an explicit `env=`, did NOT see the added variables at all
    # (`CORPUSDIR: unbound variable`). A battery whose inputs can silently fail
    # to arrive is the same class of defect as the one it exists to find, so the
    # inputs are literals in the generated script and cannot go missing.
    if lang == "sh":
        rel = "arm_%s.sh" % case
        head = "".join("%s=%s\n" % (k, shlex.quote(v)) for k, v in sorted(env.items()))
        text = MIRROR_PRELUDE_SH + head + region + "\n" + body + "\n"
    else:
        rel = "arm_%s.ps1" % case
        head = "".join("$%s = %s\n" % (k, _ps1_single_quote(v))
                       for k, v in sorted(env.items()))
        text = MIRROR_PRELUDE_PS1 + head + region + "\n" + body + "\n"
    argv = mirror_interpreter(lang) + [rel]
    script = os.path.join(work, rel)
    with open(script, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(text)
    try:
        proc = subprocess.Popen(argv, stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE, cwd=work)
        out, err = proc.communicate()
        rc = proc.returncode
    except OSError as exc:
        return False, [], "could not run %s: %s" % (argv[0], exc), ""
    out = out.decode("utf-8", "replace")
    err = err.decode("utf-8", "replace")
    if rc != 0:
        return False, [], ("%s arm exited %s: %s"
                           % (lang, rc, (err.strip() or out.strip())[:400])), out
    return True, _mirror_normalise(out), "", out


def check_dss_regions(harness_dir, out=None):
    """The verifier the `dss:corpus-engine` header promised. Prints one line per
    assertion and a final `passed=N failed=N skipped=N`; returns that triple.

    ★ A SKIP IS NOT A PASS AND IS NOT SILENT. The differential battery needs BOTH
    interpreters; a host with only one gets a SKIP with the interpreter named,
    and both drivers turn a nonzero skip count into a WARN saying what went
    unproven. That is the same rule the other self-tests already run under."""
    import shutil as _sh
    import tempfile as _tf
    out = out or sys.stdout
    counts = {"passed": 0, "failed": 0, "skipped": 0}

    def check(label, ok, detail=""):
        if ok:
            counts["passed"] += 1
            out.write("  ok   %s\n" % label)
        else:
            counts["failed"] += 1
            out.write("  FAIL %s%s\n" % (label, ("\n       " + detail) if detail else ""))

    def skip(label, why):
        counts["skipped"] += 1
        out.write("  SKIP %s — %s\n" % (label, why))

    texts = {}
    for fn in ("build-and-test.sh", "build-and-test.ps1"):
        path = os.path.join(harness_dir, fn)
        try:
            with open(path, "r", encoding="utf-8", errors="replace") as fh:
                texts[fn] = fh.read()
        except OSError as exc:
            check("driver readable: %s" % fn, False, str(exc))
            texts[fn] = ""
    out.write("--- every dss: region is DECLARED, and its verifier claim is CHECKED ---\n")

    # 1. Every region MARKED in a driver has a declaration, and vice versa.
    # ★ A COMMENT LINE, not "the string appears somewhere". MEASURED while
    # writing this: build-and-test.ps1 carries `-match '>>> dss:clone-lock >>>'`
    # — it REFERENCES the .sh's marker to extract that region, and a looser scan
    # read that as the .ps1 declaring a region of its own.
    marked = {}
    for fn, text in texts.items():
        for name in sorted(set(re.findall(r"(?m)^\s*#\s*>>>\s*dss:([a-z0-9-]+)\s*>>>",
                                          text))):
            marked.setdefault(name, set()).add(fn)
    for name in sorted(marked):
        check("dss:%s is declared in DSS_REGIONS (%s)"
              % (name, ", ".join(sorted(marked[name]))), name in DSS_REGIONS,
              "it is marked in %s and nothing states who checks it — that is the "
              "exact silence D-HARNESS-CORPUS-ENGINE-MIRROR-CLAIMS-A-VERIFIER-"
              "THAT-DOES-NOT-EXIST is about" % ", ".join(sorted(marked[name])))
    for name in sorted(DSS_REGIONS):
        check("dss:%s is still marked in a driver" % name, name in marked,
              "DSS_REGIONS declares it but no driver marks it — a stale "
              "declaration is a claim about code that is gone")
        if name not in marked:
            continue
        want = set(DSS_REGIONS[name].get("drivers", []))
        check("dss:%s is marked in exactly the declared driver(s)" % name,
              marked[name] == want,
              "declared %s, marked in %s"
              % (sorted(want) or "<none>", sorted(marked[name])))

    # 2. Sentinels well-formed. dss_region_spans raises on unbalanced/crossed.
    for name in sorted(marked):
        for fn in sorted(marked[name]):
            try:
                spans = dss_region_spans(texts[fn], name)
                ok, detail = bool(spans), ""
            except LegError as exc:
                ok, detail = False, str(exc)
            check("dss:%s in %s has balanced sentinels" % (name, fn), ok, detail)

    # 3. THE CLAIM ITSELF. A verifier file that does not name the region is the
    #    defect this whole checker exists for, so it is an assertion and not a
    #    docstring.
    for name in sorted(DSS_REGIONS):
        spec = DSS_REGIONS[name]
        verifiers = spec.get("verifiers", [])
        if not verifiers:
            check("dss:%s states WHY nothing verifies it" % name,
                  bool(spec.get("why")),
                  "a region with no verifier must say so on the record; an "
                  "empty list with no reason is indistinguishable from an "
                  "oversight")
            continue
        for vf in verifiers:
            path = os.path.join(harness_dir, vf)
            try:
                with open(path, "r", encoding="utf-8", errors="replace") as fh:
                    body = fh.read()
                present = ("dss:%s" % name) in body
                detail = ""
            except OSError as exc:
                present, detail = False, str(exc)
            check("dss:%s — its claimed verifier %s really reads it"
                  % (name, vf), present,
                  detail or ("%s does not contain the sentinel `dss:%s`. A "
                             "comment crediting an instrument that does not "
                             "read it is worse than no comment: it retires the "
                             "reader's suspicion." % (vf, name)))

    # 4. THE MIRROR CONTRACT — pairing, then differential execution.
    # {region: (sh symbols, ps1 symbols)}, accumulated so the cross-region
    # staleness check below can see the UNION rather than one region at a time.
    all_syms = {}
    # Every mirrored region whose differential battery ran with an arm missing,
    # as (region, absent langs, absent interpreters, cases). Accumulated so the
    # summary can NAME the reduction instead of leaving it as a bare count.
    reduced = []
    for name in sorted(n for n in DSS_REGIONS if DSS_REGIONS[n].get("mirror")):
        out.write("--- dss:%s — the MIRROR contract ---\n" % name)
        sh_text = dss_region_text(texts.get("build-and-test.sh", ""), name)
        ps_text = dss_region_text(texts.get("build-and-test.ps1", ""), name)
        check("dss:%s has a body in BOTH drivers" % name,
              bool(sh_text.strip()) and bool(ps_text.strip()),
              "sh=%d chars ps1=%d chars" % (len(sh_text), len(ps_text)))
        sh_syms = dss_region_symbols(sh_text, "sh")
        ps_syms = dss_region_symbols(ps_text, "ps1")
        declared_sh = [p["sh"] for p in MIRROR_PAIRS if p.get("sh")]
        declared_ps = [p["ps1"] for p in MIRROR_PAIRS if p.get("ps1")]
        # ★ THE ANTI-DRIFT PROPERTY: you cannot add a helper to one driver
        #   without declaring whether it needs a twin.
        for s in sh_syms:
            check("sh `%s` is accounted for in MIRROR_PAIRS" % s,
                  s in declared_sh,
                  "a helper added to build-and-test.sh with no row saying "
                  "whether the .ps1 needs it is exactly how a capability comes "
                  "to exist in one driver only")
        for s in ps_syms:
            check("ps1 `%s` is accounted for in MIRROR_PAIRS" % s,
                  s in declared_ps,
                  "a helper added to build-and-test.ps1 with no row saying "
                  "whether the .sh needs it")
        for pair in MIRROR_PAIRS:
            if pair.get("sh") and pair.get("ps1"):
                # ★ "IF EITHER IS HERE, BOTH MUST BE" — and NOT "both are in this
                #   region", which is what this asserted while there was exactly
                #   one mirrored region. With two, the unconditional form reds
                #   every OTHER region's pairs against a body that never defined
                #   them. The conditional form keeps the whole anti-drift property
                #   (a pair whose halves drifted into different regions is still
                #   LOUD here, and a pair defined nowhere is caught by the
                #   cross-region check below) without asserting a co-location that
                #   was never the contract.
                here = pair["sh"] in sh_syms or pair["ps1"] in ps_syms
                if here:
                    check("pair %s <-> %s: both still defined in dss:%s"
                          % (pair["sh"], pair["ps1"], name),
                          pair["sh"] in sh_syms and pair["ps1"] in ps_syms,
                          "sh=%s ps1=%s" % (pair["sh"] in sh_syms,
                                            pair["ps1"] in ps_syms))
                continue
            only = "sh" if pair.get("sh") else "ps1"
            check("single-driver %s `%s` states WHY"
                  % (only, pair.get("sh") or pair.get("ps1")),
                  bool(pair.get("why")),
                  "a capability present in one driver only must say why, or "
                  "it is indistinguishable from one that was forgotten")
        # ★ AND THE OTHER HALF OF THE CONDITIONAL PAIRING ABOVE: a row naming a
        #   symbol that exists in NO mirrored region at all. That is a stale claim
        #   about code that has been deleted, and without this it would now pass
        #   silently — the exact failure mode the conditional form could introduce,
        #   closed in the same edit rather than left for a reader to spot.
        all_syms[name] = (sh_syms, ps_syms)

        # ── DIFFERENTIAL EXECUTION ──────────────────────────────────────────
        # EVERY case in MIRROR_CASES runs, not only those a MIRROR_PAIRS row
        # names. A case reachable from no row would otherwise sit in this file
        # looking like coverage and never execute — the same shape as a region
        # whose verifier does not read it, one level in. Pair-named cases keep
        # their order; the rest follow, sorted.
        for pair in MIRROR_PAIRS:
            c = pair.get("differential")
            if c:
                check("MIRROR_PAIRS case `%s` is a defined MIRROR_CASES entry" % c,
                      c in MIRROR_CASES,
                      "the row names a battery case that does not exist, so that "
                      "pair is verified by PAIRING ONLY while claiming otherwise")
        # Keys are closed: a typo turns an `expect` into a silently-absent
        # assertion, which is the vacuity class this battery exists to refuse.
        for c in sorted(MIRROR_CASES):
            unknown = sorted(set(MIRROR_CASES[c])
                             - {"sh", "ps1", "expect", "region", "crClean"})
            check("MIRROR_CASES `%s` declares only known keys" % c, not unknown,
                  "unknown key(s): %s" % ", ".join(unknown))
            # ★ `region` IS REQUIRED, AND THAT REQUIREMENT IS THE FIX FOR A LATENT
            #   TRAP THIS LOOP HAD WHILE THERE WAS EXACTLY ONE MIRRORED REGION:
            #   every case ran against EVERY mirrored region's extracted text, so
            #   the day a second region was declared, every corpus-engine case
            #   would have been executed against a body that does not define its
            #   helpers — a wall of failures naming the wrong thing. Defaulting to
            #   `corpus-engine` would have hidden it again, so it is declared.
            check("MIRROR_CASES `%s` names the region it drives" % c,
                  MIRROR_CASES[c].get("region") in DSS_REGIONS
                  and DSS_REGIONS[MIRROR_CASES[c]["region"]].get("mirror"),
                  "region=%r — a case must name a DSS_REGIONS entry declared "
                  "`mirror: True`, because the region's text is what its two arms "
                  "are executed against"
                  % MIRROR_CASES[c].get("region"))
        # ★ AND THE CR PROPERTY CANNOT QUIETLY LEAVE. Dropping `crClean` from every
        # case would take the only assertion about raw CR handling with it and leave
        # the fixture's trailing CR proving nothing again — which is exactly the
        # state this cycle found it in. [D-HARNESS-MIRROR-CR-CLAIM-IS-VACUOUS]
        check("some MIRROR_CASES entry still declares `crClean`",
              any(MIRROR_CASES[c].get("crClean") for c in MIRROR_CASES),
              "the trailing CR in a fixture is only a proof while some case asks "
              "for the RAW check; without it the normaliser erases the property "
              "and the comparison passes over a real asymmetry")
        named = [p["differential"] for p in MIRROR_PAIRS
                 if p.get("differential") and p["differential"] in MIRROR_CASES]
        cases = [c for c in (named + [c for c in sorted(MIRROR_CASES)
                                      if c not in named])
                 if MIRROR_CASES[c].get("region") == name]
        # ── WHICH INTERPRETERS THIS HOST CAN EXECUTE, AND WHAT ITS ABSENCE COSTS ─
        # ★★ THE SKIP USED TO BE ALL-OR-NOTHING PER CASE, AND THAT WAS TWO
        # DEFECTS AT ONCE. ANCHOR, ONE LINE, DO NOT WRAP:
        # D-HARNESS-MIRROR-SKIP-MISCOUNTS-AND-DISCARDS-SINGLE-ARM-COVERAGE
        # ✔MEASURED 2026-08-11 on macOS AND on the arm64 Linux VPS, identically —
        # `OK (275 assertions) - but 9 assertion(s) SKIPPED on this host` — against
        # `passed=292 failed=0 skipped=0` on Windows:
        #   (1) THE COUNT WAS WRONG BY CONSTRUCTION. `skip()` fired ONCE PER CASE,
        #       so the SEVENTEEN assertions those nine cases own were reported as
        #       "9 assertion(s)". passed+skipped did not reconstruct the battery's
        #       size (275+9=284, not 292), so the two hosts' numbers could not be
        #       compared at all — and the number that WAS printed understated the
        #       loss by nearly half.
        #   (2) IT DISCARDED COVERAGE THAT NEEDS NO SECOND ARM. `expect` and
        #       `crClean` are PER-ARM properties: they ask whether THIS copy is
        #       RIGHT, not whether the two AGREE. Refusing to check the .sh arm
        #       because pwsh is absent throws away four assertions the host could
        #       have made. Only `identical answers` genuinely needs both arms.
        # ⇒ THE INVENTORY IS NOW HOST-INDEPENDENT: every assertion this battery
        # owns is either CHECKED or SKIPPED, never omitted, so passed+failed+
        # skipped is the SAME total on every host and a reduced host announces the
        # reduction in the same breath as the count. See mirror_case_inventory,
        # which is what makes that claim checkable rather than asserted.
        region_text = {"sh": sh_text, "ps1": ps_text}
        interp = dict((lang, mirror_interpreter(lang)[0]) for lang in MIRROR_LANGS)
        have = dict((lang, _sh.which(interp[lang])) for lang in MIRROR_LANGS)
        absent = [lang for lang in MIRROR_LANGS if not have[lang]]
        why_absent = ("%s is not on PATH, so this host cannot execute the .%s "
                      "copy of a mirrored region"
                      % (" and ".join(interp[l] for l in absent),
                         "/.".join(absent)) if absent else "")
        if absent:
            reduced.append((name, list(absent),
                            [interp[l] for l in absent], list(cases)))
        work = None
        # The reconciliation the inventory claim rests on. `ran_failures` is the
        # ONE assertion class that is not in the inventory: it exists only when a
        # PRESENT interpreter fails to execute its arm, which is a defect, not a
        # host property.
        inventory_size = sum(len(mirror_case_inventory(c)) for c in cases)
        before = counts["passed"] + counts["failed"] + counts["skipped"]
        ran_failures = 0
        try:
            env = None
            if len(absent) < len(MIRROR_LANGS):
                work = _tf.mkdtemp(prefix="dss-mirror-")
                env = _mirror_write_fixtures(work)
            for case in cases:
                answer, raw_out = {}, {}
                for lang in MIRROR_LANGS:
                    if not have[lang]:
                        continue
                    ok, lines, detail, raw = _mirror_run(
                        lang, region_text[lang], case, work, env)
                    # A PRESENT interpreter that could not run its arm is a
                    # FAILURE, never a skip — the host had the capability and the
                    # code broke. Only an ABSENT interpreter skips.
                    if not ok:
                        ran_failures += 1
                        check("differential %s: the .%s copy RAN" % (case, lang),
                              False, detail)
                        continue
                    answer[lang], raw_out[lang] = lines, raw
                # ── the PER-ARM assertions: one arm's own correctness ─────────
                # ★ THE PROPERTY THE NORMALISER ERASES, ASSERTED ON RAW BYTES AND
                # PER ARM. [D-HARNESS-MIRROR-CR-CLAIM-IS-VACUOUS.] A case whose
                # fixture carries a trailing CR declares `crClean`, and each arm
                # must have removed it BEFORE printing — which the line-for-line
                # comparison below cannot see, because _mirror_normalise strips CR
                # from both arms first.
                # ★ AND, WHERE THE CASE DECLARES ONE, THE RIGHT ANSWER. Agreement
                # is what a differential battery can offer on its own, and it is
                # real — but two copies that both answer "" agree perfectly, and
                # that was the exact state of the abort-file resolver before this
                # case existed. `expect` is checked PER ARM so a wrong-but-
                # identical pair reds, and it names which arm is wrong when only
                # one is — which is also precisely why it survives a missing twin.
                want = MIRROR_CASES[case].get("expect")
                for lang in MIRROR_LANGS:
                    labels = []
                    if MIRROR_CASES[case].get("crClean"):
                        labels.append(("cr", "differential %s: the .%s arm leaves "
                                             "NO stray CR in its output"
                                             % (case, lang)))
                    if want is not None:
                        labels.append(("expect", "differential %s: the .%s answer "
                                                 "is CORRECT" % (case, lang)))
                    if not have[lang]:
                        for _kind, label in labels:
                            skip(label, why_absent)
                        continue
                    if lang not in answer:
                        for _kind, label in labels:
                            check(label, False,
                                  "the .%s arm did not run — see the RAN failure "
                                  "above; its own correctness is unproven and that "
                                  "is a RED, not a skip: this host HAS %s"
                                  % (lang, interp[lang]))
                        continue
                    for kind, label in labels:
                        if kind == "cr":
                            stray = mirror_stray_cr_lines(raw_out[lang], lang)
                            check(label, not stray,
                                  "a CR that is not this interpreter's own line "
                                  "terminator survived into the output, where it "
                                  "mangles a log line - and the normalised "
                                  "comparison below cannot see it: %r" % stray[:3])
                        else:
                            got = answer[lang]
                            check(label, got == want,
                                  "expected: %s\n       got:      %s"
                                  % (" | ".join(want) or "<empty>",
                                     " | ".join(got) or "<empty>"))
                # ── the DIFFERENTIAL itself: the one assertion needing BOTH ───
                # A differential with one arm missing is not a differential, so
                # this is the assertion an interpreter-less host genuinely loses —
                # and it says so under its own name instead of under a count.
                if absent:
                    skip("differential %s: identical answers" % case, why_absent)
                elif len(answer) < len(MIRROR_LANGS):
                    check("differential %s: identical answers" % case, False,
                          "an arm did not run, so the two copies were never "
                          "compared — see the RAN failure above")
                else:
                    same = answer["sh"] == answer["ps1"]
                    detail = ""
                    if not same:
                        detail = ("the two drivers answer DIFFERENTLY on "
                                  "identical input:\n         .sh : %s\n"
                                  "         .ps1: %s"
                                  % (" | ".join(answer["sh"]) or "<empty>",
                                     " | ".join(answer["ps1"]) or "<empty>"))
                    check("differential %s: identical answers (%d line(s))"
                          % (case, len(answer["sh"])), same, detail)
        finally:
            if work:
                _sh.rmtree(work, ignore_errors=True)
        # ★ THE HOST-INDEPENDENCE CLAIM, CHECKED. The battery must have ACCOUNTED
        # for every assertion its inventory names — as a pass, a fail or a named
        # skip — so `passed+failed+skipped` is the same total on a host with one
        # interpreter as on a host with two. Without this, the next person to add
        # a per-arm property can quietly reintroduce the shrink-in-silence this
        # block was rewritten to end.
        made = (counts["passed"] + counts["failed"] + counts["skipped"]) - before
        check("dss:%s — the differential inventory is fully ACCOUNTED FOR "
              "(%d case(s), %d assertion(s), host-independent)"
              % (name, len(cases), inventory_size),
              made == inventory_size + ran_failures,
              "the battery emitted %d verdict(s) where the inventory names %d "
              "(+%d arm-did-not-run failure(s)). An assertion that is neither "
              "checked nor skipped VANISHES on the hosts that cannot make it, "
              "which is how twin-parity coverage shrinks without anyone seeing "
              "a number change" % (made, inventory_size, ran_failures))

    # 4b. A MIRROR_PAIRS ROW WHOSE CODE IS GONE.
    # The per-region pairing above became CONDITIONAL when a second mirrored region
    # appeared ("if either half is here, both must be"), and that alone would let a
    # row survive whose code has been deleted outright. Checked here against the
    # UNION, so the two halves of the property are complete together.
    #
    # ★ TWO KINDS OF ROW, TWO PROPERTIES, AND THE STRICT ONE IS NOT WEAKENED.
    # [D-HARNESS-MIRROR-PAIRS-4B-OVER-STRICT-FOR-A-SINGLE-DRIVER-HELPER.]
    #   · A PAIR row (both `sh` and `ps1`) is a claim that these two are twins, and
    #     the differential battery only reaches a symbol INSIDE a mirrored region.
    #     So both halves must still live in one — that catches deletion AND the
    #     quieter regression of a twin MOVED OUT of the mirror, where its
    #     differential coverage would vanish silently. Unchanged.
    #   · A SINGLE-DRIVER row (one side plus a stated reason) has no twin to compare
    #     and therefore contributes NO differential coverage wherever it lives; the
    #     only thing that can go stale about it is the code disappearing. So it is
    #     checked against every top-level definition in ITS OWN driver. That admits
    #     a future single-driver helper in a non-mirrored region — which used to red
    #     for a reason that had nothing to do with the property — while still
    #     failing the moment the symbol is gone.
    if all_syms:
        union_sh = set().union(*[s for s, _ in all_syms.values()])
        union_ps = set().union(*[p for _, p in all_syms.values()])
        whole_sh = set(dss_region_symbols(texts.get("build-and-test.sh", ""), "sh"))
        whole_ps = set(dss_region_symbols(texts.get("build-and-test.ps1", ""),
                                         "ps1"))
        for pair in MIRROR_PAIRS:
            paired = bool(pair.get("sh")) and bool(pair.get("ps1"))
            for lang, sym, mirrored, whole in (
                    ("sh", pair.get("sh"), union_sh, whole_sh),
                    ("ps1", pair.get("ps1"), union_ps, whole_ps)):
                if not sym:
                    continue
                if paired:
                    check("MIRROR_PAIRS %s `%s` (a declared TWIN) exists in some "
                          "mirrored region" % (lang, sym), sym in mirrored,
                          "no dss: region declared `mirror: True` defines it — "
                          "either the code is gone, or it has moved OUT of the "
                          "mirror and the differential battery no longer reaches "
                          "it. Both are claims about code that is not there.")
                else:
                    check("MIRROR_PAIRS %s `%s` (declared SINGLE-DRIVER) still "
                          "exists in its driver" % (lang, sym), sym in whole,
                          "nothing in build-and-test.%s defines it at top level — "
                          "a stale row is the same shape of defect as an "
                          "unverified region" % lang)

    # ── WHAT THIS HOST COULD NOT PROVE, IN WORDS, BESIDE THE COUNT ───────────
    # ★ A BARE SKIP COUNT IS NOT A CLASSIFICATION. Both drivers used to render a
    # nonzero skip as "an unmet prerequisite, normally 'no git on PATH'" — a
    # sentence that is true of test-confound-scope.sh and simply WRONG here, so
    # the one host-shaped hole in twin parity was reported under another
    # self-test's reason. The reduction is named where it happens: which region,
    # which interpreter, which cases, and what class of assertion was lost.
    if reduced:
        out.write("TWIN-PARITY COVERAGE REDUCED ON THIS HOST\n")
        for name, langs, exes, region_cases in reduced:
            lost = sum(1 for c in region_cases
                       for lang, _kind in mirror_case_inventory(c)
                       if lang is None or lang in langs)
            out.write(
                "  dss:%s — %s not on PATH, so the .%s copy could not be "
                "EXECUTED. %d of this region's %d differential assertion(s) "
                "were skipped across %d case(s): every `identical answers` "
                "comparison (a differential with one arm missing is not a "
                "differential) plus the .%s arm's own correctness checks. The "
                "arms this host DOES have were still checked against their "
                "declared `expect`/CR properties.\n"
                % (name, " and ".join(exes), "/.".join(langs), lost,
                   sum(len(mirror_case_inventory(c)) for c in region_cases),
                   len(region_cases), "/.".join(langs)))
            out.write("    cases: %s\n" % ", ".join(region_cases))
    out.write("passed=%d failed=%d skipped=%d\n"
              % (counts["passed"], counts["failed"], counts["skipped"]))
    return counts


# ── THE REGISTRY AS AN INSTRUMENT ───────────────────────────────────────────
#
# D-PROCESS-CHECK-THE-REGISTRY-FOR-A-MATCHED-CONTROL-BEFORE-COMMISSIONING-ONE.
#
# ✔THE MOTIVATING CASE, MEASURED (TF-C123): a 2×2 attribution (compiler × rundir
# filesystem) was commissioned from scratch for 57 unit failures whose IDENTICAL
# experiment and IDENTICAL verdict were already in the registry from seven cycles
# earlier. The un-cited row let three false statements reach a commit message,
# each of which that row would have pre-empted. The row was findable — the leg
# name, the driver and the word `rundir` all appear in it — and nobody looked,
# because looking is a thing you have to REMEMBER to do. At 868 rows, "remember
# to grep" is not a durable answer.
#
# So the harness looks instead, AT the point it reports a failure, and prints
# what it found beside the failure. That turns the registry from a document into
# an instrument: the prior control arrives WITH the failure rather than waiting
# to be recalled.
#
# ⚠ THIS RUNS ON A FAILURE PATH AND MUST NEVER BECOME ONE ITSELF. Everything
# here is fail-soft by construction: an unreadable, absent, malformed or
# gigantic registry produces ONE line saying so and rc 0. A run that already
# failed must not also lose its report because the lookup tripped.
#
# ⚠ AND IT IS A POINTER, NOT A VERDICT. A matched row means "someone has looked
# at something with this name before", never "this failure is explained". The
# output says so, because a harness that appears to excuse a failure it merely
# pattern-matched would be far worse than one that says nothing.
REGISTRY_CONTROL_MAX_ROWS = 8
REGISTRY_CONTROL_MIN_TOKEN = 4
REGISTRY_CONTROL_MAX_BYTES = 8 * 1024 * 1024
REGISTRY_CONTROL_EXCERPT = 220


def registry_control_tokens(name, kind):
    """The searchable tokens in a name, most specific FIRST.

    ★ THE KIND IS THE CALLER'S KNOWLEDGE AND IT IS NOT GUESSABLE, which is why
    it is an argument rather than a shape test:

      `leg`   a leg LABEL (`elf64-x86_64`) searches WHOLE and only whole.
              ✔MEASURED while writing this: decomposing it into `elf64` and
              `x86_64` matched 310 of the 868 rows — an instrument that answers
              with a third of the document answers nothing.
      `test`  a failing test NAME (`inmemory_journal.walsetlk-2.1.3`) searches
              whole AND by component, because that is how a row actually spells
              a test FAMILY: rows say `walsetlk`, never `walsetlk-2.1.3`.
              Numeric and short components are dropped — a 3-character token
              matches half the corpus."""
    out = []
    name = (name or "").strip()
    if not name:
        return out
    out.append(name)
    if kind != "test":
        return out
    for tok in re.split(r"[.\-/\\ ,;:()\[\]]+", name):
        if (len(tok) >= REGISTRY_CONTROL_MIN_TOKEN
                and re.match(r"^[A-Za-z][A-Za-z0-9_]*$", tok)
                and tok not in out):
            out.append(tok)
    return out


def _registry_row_matches(row_lower, token):
    """Is `token` present in `row_lower` on WORD BOUNDARIES? A bare substring
    test would have `wal2` hit `wal2xyz`, and the whole value of this lookup is
    that the row it names is the row a reader would have grepped for."""
    return re.search(r"(?<![A-Za-z0-9_])%s(?![A-Za-z0-9_])"
                     % re.escape(token), row_lower) is not None


def registry_controls(registry_path, legs, tests,
                      max_rows=REGISTRY_CONTROL_MAX_ROWS):
    """(lines, note) — the registry rows whose text names this failure.

    `lines` is ready to print, one row per entry. `note` is a single line
    explaining why there is nothing to print, or "" when the lookup ran
    normally. NEITHER path raises: see the section header.

    ★ A FAILING TEST FAMILY OUTRANKS THE LEG, and when there is one the leg
    alone is not enough to print a row. The leg names every row that ever
    mentioned that target; the FAMILY is what makes a row the matched control
    for THIS failure. A row naming both is the one to read first, so it sorts
    to the top rather than being the only thing kept."""
    try:
        leg_toks, test_toks = [], []
        for name in legs or []:
            for tok in registry_control_tokens(name, "leg"):
                if tok not in leg_toks:
                    leg_toks.append(tok)
        for name in tests or []:
            for tok in registry_control_tokens(name, "test"):
                if tok not in test_toks:
                    test_toks.append(tok)
        if not leg_toks and not test_toks:
            return [], "no leg or test-family name to look up"
        if not os.path.isfile(registry_path):
            return [], ("registry not readable here (%s) — check it by hand for "
                        "a matched control" % registry_path)
        size = os.path.getsize(registry_path)
        if size > REGISTRY_CONTROL_MAX_BYTES:
            return [], ("registry is %d bytes, past this lookup's %d-byte cap — "
                        "not read" % (size, REGISTRY_CONTROL_MAX_BYTES))
        hits = []
        with open(registry_path, "r", encoding="utf-8", errors="replace") as fh:
            for lineno, row in enumerate(fh, 1):
                if not row.startswith("|"):
                    continue
                cells = row.split("|")
                if len(cells) < 3:
                    continue
                anchor = re.search(r"`(D-[A-Z0-9-]+)`", cells[1] or "")
                if not anchor:
                    continue
                low = row.lower()
                hit_tests = [t for t in test_toks
                             if _registry_row_matches(low, t.lower())]
                hit_legs = [t for t in leg_toks
                            if _registry_row_matches(low, t.lower())]
                if test_toks and not hit_tests:
                    continue          # the leg alone is not a matched control
                if not hit_tests and not hit_legs:
                    continue
                matched = hit_tests + hit_legs
                hits.append((len(hit_tests) * 3 + len(hit_legs),
                             max(len(t) for t in matched),
                             lineno, anchor.group(1), matched, cells[2]))
        if not hits:
            named = ", ".join((test_toks + leg_toks)[:6])
            return [], ("no registry row names %s — nothing matched, which is "
                        "itself worth recording" % named)
        hits.sort(key=lambda h: (-h[0], -h[1], h[2]))
        lines = []
        for _, _, lineno, anchor, matched, cell in hits[:max_rows]:
            excerpt = re.sub(r"\s+", " ", re.sub(r"[*`]", "", cell)).strip()
            if len(excerpt) > REGISTRY_CONTROL_EXCERPT:
                excerpt = excerpt[:REGISTRY_CONTROL_EXCERPT] + " …"
            lines.append("%s  [line %d, matched %s]\n      %s"
                         % (anchor, lineno, " ".join(matched[:4]), excerpt))
        if len(hits) > max_rows:
            lines.append("… and %d more row(s) — grep the registry for %s"
                         % (len(hits) - max_rows,
                            " ".join((test_toks + leg_toks)[:4])))
        return lines, ""
    except Exception as exc:                       # noqa: BLE001 — see the header
        return [], ("registry lookup did not run (%s: %s) — this is a POINTER "
                    "only and never fails a run"
                    % (type(exc).__name__, exc))


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
        # ★ AND THE SAME CORRECTION AGAIN, ONE LEG LATER (TF-C126, 2026-08-07).
        # This entry read `skipped-by-runOn` until a Windows run printed it beside
        # a working aarch64 testfixture on disk — the identical shape the comment
        # above records for elf64-x86_64, which should have been the hint to check
        # its sibling at the time. The leg declared a Windows launcher only for
        # hostArch arm64 (WSL2 runs the HOST's arch, so an arm64 Windows box gets
        # an arm64 distro and needs no emulator); an x86_64 Windows host needs
        # qemu-aarch64 INSIDE WSL, which is exactly how this project's WSL column
        # has always run the leg. `launched:wsl.exe` — the oracle keys on
        # launcher[0], and the emulator is the argument that follows it.
        "elf64-arm64": "launched:wsl.exe",
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
    TypeError is a bug in the resolver, not the refusal under test.

    ⚠ ANY OTHER EXCEPTION PROPAGATES and takes the runner down with it, which
    prints NO `FAIL` line at all. That is fine where the surrounding code cannot
    raise anything else, and it is exactly wrong where the property under test is
    "this is a NAMED refusal and not a python traceback" — use
    `_refuses_namedly` there. ✔MEASURED while red-on-disabling the injected-verdict
    validation: removing the key check made `_inject({"why": "x"})` raise KeyError,
    which aborted the whole self-test with a traceback instead of failing one
    assertion."""
    try:
        thunk()
    except LegError:
        return True
    return False


def _refuses_namedly(thunk):
    """True only if `thunk` raised a LegError. A python-level exception is caught
    and reported as a FAILURE OF THIS PROPERTY rather than allowed to kill the
    runner — the distinction between a named diagnostic and a traceback IS the
    property here. See anchor, ONE LINE, DO NOT WRAP:
    D-HARNESS-PROBE-VERDICTS-FLAG-INJECTS-AN-UNVALIDATED-PRESENT"""
    try:
        thunk()
    except LegError:
        return True
    except BaseException:                                       # noqa: BLE001
        return False
    return False


def _raise_text(thunk):
    """The LegError text `thunk` refused with, or "" if it did not refuse. A
    refusal that does not NAME its subject is a refusal an operator cannot act
    on, so the diagnostic is asserted as well as the raise."""
    try:
        thunk()
    except LegError as exc:
        return str(exc)
    return ""


def _forbidden_runner(argv):
    """A translator that must never be reached. Raises a non-LegError on purpose,
    so `_raises` cannot mistake "it spawned something" for "it refused"."""
    raise AssertionError("a translator was invoked when none should have been: %r"
                         % (argv,))


def self_test(path=CATALOGUE, out=sys.stdout):
    import struct
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

    # ── the stage build configuration ────────────────────────────────────────
    # Asserted on CONTENT, not on shape. "four keys are present" was satisfied
    # by an earlier draft in which optionDefines carried a `-D` prefix that
    # `make OPTIONS=` then passed to the compiler as a bare, ignored token — a
    # capability lost with no error anywhere.
    sb = stage_build(path)
    check("stageBuild names the sqlite3 capability the operator asked for",
          "SQLITE_ENABLE_FTS5" in sb["requiredDefines"],
          "requiredDefines=%r" % (sb["requiredDefines"],))
    check("stageBuild's makeOptions carries a -D per optionDefine",
          sb["makeOptions"].split() ==
          ["-D" + d for d in sb["optionDefines"]],
          "makeOptions=%r optionDefines=%r"
          % (sb["makeOptions"], sb["optionDefines"]))
    check("no optionDefine is spelled with its own -D "
          "(it would reach the compiler twice-prefixed and be ignored)",
          not any(d.startswith("-D") for d in sb["optionDefines"]))
    check("every configure flag is a --flag with no embedded whitespace",
          all(f.startswith("--") and not any(c.isspace() for c in f)
              for f in sb["configureFlags"]),
          "configureFlags=%r" % (sb["configureFlags"],))
    # A witness whose define is not among requiredDefines would wait on a
    # capability this configuration never turns on, so its gate could only ever
    # be red. ⚠ The pairing is checked against the DECLARED `define`, not
    # derived from the capability name: the obvious structural rule ("the
    # capability name appears inside its define") fails on the perfectly correct
    # `mem5` ↔ `SQLITE_ENABLE_MEMSYS5`, and a heuristic that has to be weakened
    # to accept real data is worth less than the explicit pairing it avoids.
    for cap, w in sorted(sb["capabilityWitnesses"].items()):
        check("witness capability '%s' names a required define" % cap,
              w["define"] in sb["requiredDefines"],
              "%r is not in requiredDefines" % w["define"])
        check("witness capability '%s' names a test file stem, not a path" % cap,
              "/" not in w["file"] and not w["file"].endswith(".test"),
              "file=%r" % w["file"])
    # build-and-test.sh EVALS this text, so it must be assignments and nothing
    # else — the same guarantee emit_sh() carries, proved the same way and
    # against the SHIPPED emitter rather than a re-typing of it.
    sb_sh = stage_build_sh(sb)
    sb_statements = sh_statements(sb_sh)
    check("the stage-build sh emitter emits one statement per variable",
          len(sb_statements) == 4,
          "got %d: %r" % (len(sb_statements), sb_statements))
    check("every stage-build sh statement is an assignment, never a command",
          all(re.match(r"^DSS_STAGE_[A-Z_]+=", s) for s in sb_statements),
          "%r" % (sb_statements,))
    # And the values SURVIVE the round trip. "four assignments" was already true
    # of an emitter that dropped the -D prefix; only reading the value back
    # catches that.
    check("the stage-build sh emitter carries makeOptions verbatim",
          ("DSS_STAGE_MAKE_OPTIONS=" + shlex.quote(sb["makeOptions"])) in sb_statements,
          "%r" % (sb_statements,))

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

    # ── WHAT A FORWARDED VARIABLE'S VALUE MEANS ON THE OTHER SIDE ───────────
    # D-HARNESS-PS1-TCL-LIBRARY-NOT-FORWARDED-ACROSS-THE-WSL-BOUNDARY. The
    # translator is INJECTED, exactly as translate_path's own battery does it, so
    # the contract is exercised on a host with no `wslpath` — the assertion is
    # about which door the value goes through, not about what wslpath answers.
    _xlated = lambda argv: (0, "/mnt/c/tcl/tcl8.6", "")  # noqa: E731
    check("an UNDECLARED forwarded variable is REFUSED, never assumed opaque",
          _raises(lambda: forward_kind("SOME_NEW_VAR")))
    check("...and the refusal names the variable",
          "SOME_NEW_VAR" in _raise_text(lambda: forward_kind("SOME_NEW_VAR")))
    check("the two corpus hooks are declared namespace-NEUTRAL",
          forward_kind("SQLITE_TEST_PATTERN_LIST") == "opaque"
          and forward_kind("QUICKTEST_OMIT") == "opaque")
    check("TCL_LIBRARY is declared a DRIVER PATH, not opaque",
          forward_kind("TCL_LIBRARY") == "driver-path")
    check("'inherit' still needs no assignments, path-valued or not",
          launch_forward_assignments(
              "inherit", "windows-to-wsl", [("TCL_LIBRARY", r"C:\tcl\tcl8.6")],
              runner=_xlated) == [])
    check("an opaque variable crosses by NAME only",
          launch_forward_assignments(
              "wslenv", "none", [("QUICKTEST_OMIT", "a,b")])
          == ["WSLENV=QUICKTEST_OMIT"])
    # ★ THE ASSERTION THE ANCHOR EXISTS FOR: the value that crosses is the
    # TRANSLATED one, and it is assigned BEFORE the carrier names it.
    check("a DRIVER-PATH variable crosses TRANSLATED, and first",
          launch_forward_assignments(
              "wslenv", "windows-to-wsl", [("TCL_LIBRARY", r"C:\tcl\tcl8.6")],
              runner=_xlated)
          == ["TCL_LIBRARY=/mnt/c/tcl/tcl8.6", "WSLENV=TCL_LIBRARY"])
    check("...and a launcher sharing this namespace gets it VERBATIM",
          launch_forward_assignments(
              "wslenv", "none", [("TCL_LIBRARY", "/opt/tcl8.6")])
          == ["TCL_LIBRARY=/opt/tcl8.6", "WSLENV=TCL_LIBRARY"])
    check("a DRIVER-PATH variable with NO DECLARED TRANSLATION is REFUSED",
          _raises(lambda: launch_forward_assignments(
              "wslenv", "", [("TCL_LIBRARY", r"C:\tcl\tcl8.6")])))
    check("...and the refusal names the variable and its value",
          "TCL_LIBRARY" in _raise_text(lambda: launch_forward_assignments(
              "wslenv", "", [("TCL_LIBRARY", r"C:\tcl\tcl8.6")])))
    check("an UNDECLARED variable is refused on the assignment path too",
          _raises(lambda: launch_forward_assignments(
              "wslenv", "none", [("SOME_NEW_VAR", "x")])))
    check("a catalogue-DECLARED launcher variable needs no kind and no value",
          launch_forward_assignments(
              "wslenv", "windows-to-wsl", [], ["QEMU_LD_PREFIX"])
          == ["WSLENV=QEMU_LD_PREFIX"])
    check("a path-valued variable named with NO VALUE is REFUSED",
          _raises(lambda: launch_forward_assignments(
              "wslenv", "windows-to-wsl", [("TCL_LIBRARY", "")],
              runner=_xlated)))
    check("the unknown-VERB diagnosis wins over the unknown-variable one",
          "envTransfer" in _raise_text(lambda: launch_forward_assignments(
              "copy-the-block", "none", [("SOME_NEW_VAR", "x")])))

    # ── WHAT A LAUNCHER NEEDS BEYOND ITS OWN argv[0] ─────────────────────────
    # The gate that did not exist. `launcher_available` resolves `command[0]`
    # and stops, so `["wsl.exe","-e","qemu-aarch64"]` passed every check this
    # harness had on a box with WSL and no qemu — 14 units then exited 255 and
    # were charged to DSS — and on the arm64 VPS a PRESENT qemu-x86_64 with an
    # incomplete sysroot aborted three corpus segments inside glibc.
    _lreq = {"kind": "file", "path": "${QEMU_LD_PREFIX}/lib/ld-linux-aarch64.so.1",
             "provides": "the ELF interpreter", "why": "MEASURED",
             "install": "apt-get install libc6-arm64-cross"}
    _lentry = {"hostOs": "windows", "hostArch": "x86_64",
               "command": ["wsl.exe", "-e", "qemu-aarch64"],
               "env": {"QEMU_LD_PREFIX": "/usr/aarch64-linux-gnu"},
               "pathTranslation": "windows-to-wsl", "envTransfer": "wslenv",
               "runFilesystem": "wsl-linux", "requires": [_lreq]}

    # ★ REACHABILITY GUARD (the sibling of "some declared launcher needs a
    # non-inherit envTransfer"). Every assertion below is about a mechanism, and
    # a mechanism no shipped leg/host cell reaches is a mechanism whose tests
    # prove nothing about this catalogue.
    check("some shipped leg/host cell resolves a NON-EMPTY requires list",
          any(leg["run"].get("requires")
              for h in SELF_TEST_HOSTS
              for leg in plan(h[0], h[1], every, path)["legs"]))
    check("...and at least one of those rows is probed THROUGH a launcher "
          "rather than in-process",
          any(row.get("probe")
              for h in SELF_TEST_HOSTS
              for leg in plan(h[0], h[1], every, path)["legs"]
              for row in leg["run"].get("requires", [])),
          "an all-in-process catalogue would never exercise the wsl-linux "
          "probe templates, which are the ones the defect was made of")

    # ── ${VAR} EXPANDS OVER THE ENTRY'S OWN env AND NOTHING ELSE ────────────
    check("${VAR} expands from THIS ENTRY'S env",
          expand_launcher_requirement_path(
              "${QEMU_LD_PREFIX}/lib/ld-linux-aarch64.so.1",
              {"QEMU_LD_PREFIX": "/usr/aarch64-linux-gnu"})
          == "/usr/aarch64-linux-gnu/lib/ld-linux-aarch64.so.1")
    check("an UNDECLARED ${VAR} RAISES rather than expanding to empty",
          _raises(lambda: expand_launcher_requirement_path("${NOPE}/lib/x", {})))
    check("...and the refusal NAMES the variable",
          "NOPE" in _raise_text(
              lambda: expand_launcher_requirement_path("${NOPE}/lib/x", {})),
          "an empty expansion turns ${X}/lib/ld.so into an ABSOLUTE path that "
          "exists on any Linux box — a check that passes for the wrong reason")
    # ★ THE PROCESS ENVIRONMENT IS NOT A FALLBACK, asserted with a variable that
    # is certainly set in this process: if os.environ were ever consulted, this
    # would expand instead of refusing, and two machines with identical
    # catalogues would be checking different files.
    check("a variable that exists in THIS PROCESS but not in the entry's env "
          "is still REFUSED",
          _raises(lambda: expand_launcher_requirement_path("${PATH}/x", {})),
          "os.environ has PATH=%r" % (os.environ.get("PATH", "")[:20] + "...",))
    check("an entry env value that is empty RAISES rather than vanishing",
          _raises(lambda: expand_launcher_requirement_path("${Q}/x", {"Q": ""})))

    # ── THE PROBE ARGV IS A PROPERTY OF THE LAUNCHER'S FILESYSTEM ───────────
    # Asserted as EXACT argv, so a template edit is VISIBLE here rather than
    # merely still-passing: these strings are the whole instrument.
    check("a wsl-linux FILE probe is the exact argv",
          requirement_probe_argv("wsl-linux", "file", "/usr/lib/x.so")
          == ["wsl.exe", "-e", "test", "-f", "/usr/lib/x.so"],
          "%r" % (requirement_probe_argv("wsl-linux", "file", "/usr/lib/x.so"),))
    check("a wsl-linux DIRECTORY probe is the exact argv",
          requirement_probe_argv("wsl-linux", "directory", "/usr/aarch64-linux-gnu")
          == ["wsl.exe", "-e", "test", "-d", "/usr/aarch64-linux-gnu"])
    # ⚠ THE ONE SHELL IN THE TABLE. Pinned in full, including the `--` and the
    # `"$1"`, because the ONLY thing that makes it safe is that the path arrives
    # as a POSITIONAL ARGUMENT and is never interpolated into the script text.
    # "Simplifying" it to `sh -c 'command -v %s'` would hand back exactly the
    # property `wsl.exe -e` exists to guarantee.
    check("a wsl-linux COMMAND probe passes the path as $1, NEVER inside the "
          "script text",
          requirement_probe_argv("wsl-linux", "command", "qemu-aarch64")
          == ["wsl.exe", "-e", "sh", "-c", 'command -v "$1" >/dev/null', "--",
              "qemu-aarch64"],
          "%r" % (requirement_probe_argv("wsl-linux", "command", "qemu-aarch64"),))
    check("...and the script text does NOT contain the path",
          "qemu-aarch64" not in
          requirement_probe_argv("wsl-linux", "command", "qemu-aarch64")[4])
    check("a `driver` launcher has NO probe argv at all — the answer is "
          "in-process",
          requirement_probe_argv("driver", "file", "/x") == []
          and requirement_probe_argv("driver", "command", "cc") == [])
    check("an unknown requirement kind RAISES rather than probing something else",
          _raises(lambda: requirement_probe_argv("wsl-linux", "socket", "/x")))
    # Every filesystem that probes at all must probe EVERY kind: a kind with no
    # template on some filesystem is a requirement silently never checked there.
    for _fs, _spec in sorted(RUN_FILESYSTEMS.items()):
        if not _spec["probeArgv"]:
            continue
        check("runFilesystem '%s' implements a probe for every kind" % _fs,
              set(_spec["probeArgv"]) == LAUNCHER_REQUIREMENT_KINDS,
              "implements %r, kinds are %r"
              % (sorted(_spec["probeArgv"]), sorted(LAUNCHER_REQUIREMENT_KINDS)))

    # ── check_launcher, WITH AN INJECTED PROBER ─────────────────────────────
    _probe_leg = {"label": "fixture-leg", "spec": "arm64:elf64-aarch64-linux-exec",
                  "runOn": ["linux"], "confounds": [], "build": {},
                  "launchers": [_lentry]}
    _seen = []

    def _prober(argv):
        _seen.append(list(argv))
        return (0, "", "")

    _rep = check_launcher(_probe_leg, "windows", "x86_64", {"wsl.exe"},
                          runner=_prober)
    check("check_launcher reports MET when every probe answers rc 0",
          _rep["ok"] and _rep["verdict"] == "" and _rep["missing"] == [],
          "%r" % (_rep,))
    check("...and it probed THROUGH the launcher, with the expanded path",
          _seen == [["wsl.exe", "-e", "test", "-f",
                     "/usr/aarch64-linux-gnu/lib/ld-linux-aarch64.so.1"]],
          "%r" % (_seen,))
    _rep = check_launcher(_probe_leg, "windows", "x86_64", {"wsl.exe"},
                          runner=lambda argv: (1, "", ""))
    check("check_launcher reports UNMET when a probe answers non-zero",
          not _rep["ok"] and len(_rep["missing"]) == 1, "%r" % (_rep,))
    check("...under the NEW verdict, not `skipped-emulator-missing`",
          _rep["verdict"] == "skipped-launcher-prerequisite-missing",
          "the launcher is PRESENT and functional-looking; saying 'emulator "
          "missing' of it is the conflation this verdict exists to end")
    # A missing row must carry its REMEDY as well as its subject. A diagnostic
    # without one is a diagnostic nobody acts on — the whole cost of the VPS
    # abort was the hours between "signal 6" and "install libgcc-s1-*-cross".
    check("a missing row carries provides AND install AND its probe",
          all(_rep["missing"][0].get(k) for k in
              ("kind", "path", "provides", "why", "install", "probe")),
          "%r" % (_rep["missing"][0],))
    check("...and the path in the report is the EXPANDED one, not ${VAR}",
          "${" not in _rep["missing"][0]["path"],
          "%r" % (_rep["missing"][0]["path"],))
    # Per KIND, present and absent, so no arm rests on one template.
    for _kind, _path in (("file", "/x/ld.so"), ("directory", "/x"),
                         ("command", "qemu-aarch64")):
        _e = dict(_lentry, requires=[dict(_lreq, kind=_kind, path=_path)])
        _l = dict(_probe_leg, launchers=[_e])
        _yes = check_launcher(_l, "windows", "x86_64", {"wsl.exe"},
                              runner=lambda a: (0, "", ""))
        _no = check_launcher(_l, "windows", "x86_64", {"wsl.exe"},
                             runner=lambda a: (1, "", ""))
        check("kind '%s' reports present and absent correctly" % _kind,
              _yes["ok"] and not _no["ok"] and _no["missing"][0]["kind"] == _kind,
              "%r / %r" % (_yes["ok"], _no["ok"]))
    # ★ A `driver` FILESYSTEM MUST NOT SPAWN. `_forbidden_runner` raises a
    # non-LegError, so "it spawned something" cannot be mistaken for a refusal.
    _drv_entry = {"hostOs": "linux", "hostArch": "x86_64",
                  "command": ["qemu-aarch64"],
                  "env": {"QEMU_LD_PREFIX": "/usr/aarch64-linux-gnu"},
                  "pathTranslation": "none", "envTransfer": "inherit",
                  "runFilesystem": "driver",
                  "requires": [dict(_lreq, kind="directory",
                                    path="${QEMU_LD_PREFIX}")]}
    _drv_leg = dict(_probe_leg, launchers=[_drv_entry])
    _rep = check_launcher(_drv_leg, "linux", "x86_64", {"qemu-aarch64"},
                          runner=_forbidden_runner,
                          checker=lambda kind, p: True)
    check("a `driver`-filesystem probe answers IN-PROCESS and spawns NOTHING",
          _rep["ok"] and _rep["checked"][0]["probe"].startswith("in-process"),
          "%r" % (_rep["checked"],))
    check("...and the in-process checker sees the EXPANDED path",
          _rep["checked"][0]["path"] == "/usr/aarch64-linux-gnu")
    _rep = check_launcher(_drv_leg, "linux", "x86_64", {"qemu-aarch64"},
                          runner=_forbidden_runner,
                          checker=lambda kind, p: False)
    check("...and an absent one is UNMET, still without spawning",
          not _rep["ok"] and _rep["missing"][0]["path"] == "/usr/aarch64-linux-gnu")
    # A leg with no launcher for this host has nothing to check and says so —
    # never a silent ok with no explanation.
    _rep = check_launcher(_probe_leg, "linux", "arm64", {"wsl.exe"},
                          runner=_forbidden_runner)
    check("a leg that needs no launcher here is MET, with a stated reason",
          _rep["ok"] and _rep["missing"] == [] and _rep["detail"],
          "%r" % (_rep,))
    check("the artefact cross-check announces that it was NOT requested",
          _rep["crossCheck"] == "not requested", "%r" % (_rep["crossCheck"],))
    check("an UNREADABLE --artifact stops the check instead of passing it",
          _raises(lambda: check_launcher(
              _probe_leg, "windows", "x86_64", {"wsl.exe"}, runner=_prober,
              artifact=os.path.join(HERE, "no-such-artefact-fixture"))),
          "a cross-check that silently did not run is the shape of every "
          "instrument that ever reported success over what it could not see")

    # ── plan_leg EMITS THE ROWS, RESOLVED, AND STAYS PURE ───────────────────
    _planned = plan_leg(_probe_leg, "windows", "x86_64", {"wsl.exe"})["run"]
    check("plan_leg emits run.requires with the path EXPANDED",
          [r["path"] for r in _planned["requires"]]
          == ["/usr/aarch64-linux-gnu/lib/ld-linux-aarch64.so.1"],
          "%r" % (_planned["requires"],))
    check("...and with the probe argv BUILT, so the plan is the whole answer",
          _planned["requires"][0]["probe"][:4]
          == ["wsl.exe", "-e", "test", "-f"])
    check("...and every declared field survives into the plan",
          all(k in _planned["requires"][0]
              for k in LAUNCHER_REQUIREMENT_KEYS + ("probe",)))
    # The rows are emitted on the UNAVAILABLE path too: a driver reporting an
    # unusable launcher needs the list as much as one about to run.
    _planned_none = plan_leg(_probe_leg, "windows", "x86_64", set())["run"]
    check("an UNAVAILABLE launcher still carries its declared requirements",
          _planned_none["verdict"] == "skipped-emulator-missing"
          and len(_planned_none["requires"]) == 1,
          "%r" % (_planned_none,))
    check("a NATIVE run declares an empty requires list",
          plan_leg(_probe_leg, "linux", "arm64", every)["run"]["requires"] == [])

    # ── RUN FIDELITY: DOES THE ARTEFACT EXECUTE ON ITS OWN ISA? ─────────────
    # [D-HARNESS-RUN-FIDELITY-IS-COMPUTED-BUT-NEITHER-RECORDED-NOR-SELECTABLE]
    # ★★ THE PAIR THIS WHOLE FIELD EXISTS FOR, and the reason `mode` alone could
    # not answer it: ONE leg, TWO Windows hosts, SAME `mode` — and they are not
    # the same evidence. On arm64 Windows `wsl.exe` runs aarch64 instructions on
    # aarch64 silicon; on x86_64 Windows the same leg goes through qemu. Asserted
    # as a DISAGREEMENT rather than as two separate values, because the defect
    # this pin guards is exactly the two collapsing back into one.
    # The REAL catalogue leg, not the fixture: this pin is about the three
    # launchers `elf64-arm64` actually declares, and a fixture with one entry
    # could not express the pair.
    _arm_leg = leg_by_label(legs, "elf64-arm64")
    _wsl_native = plan_leg(_arm_leg, "windows", "arm64", every)["run"]
    _wsl_qemu = plan_leg(_arm_leg, "windows", "x86_64", every)["run"]
    check("the wsl.exe pair AGREES on mode — which is why mode cannot decide it",
          _wsl_native["mode"] == _wsl_qemu["mode"] == "launched",
          "%r vs %r" % (_wsl_native["mode"], _wsl_qemu["mode"]))
    check("...and DISAGREES on fidelity, which is the distinction being recorded",
          (_wsl_native["fidelity"], _wsl_qemu["fidelity"])
          == ("foreign-kernel", "emulated"),
          "%r vs %r" % (_wsl_native["fidelity"], _wsl_qemu["fidelity"]))
    check("...carried by sameIsa, the value plan_leg always computed and dropped",
          _wsl_native["sameIsa"] is True and _wsl_qemu["sameIsa"] is False)
    # A translation layer on the leg's OWN OS is still emulation: Rosetta is
    # os_ok with arch_ok false, so a rule keyed on runOn alone would call it
    # native. Keyed on the ISA it falls out right without naming Rosetta at all.
    _rosetta = plan_leg(leg_by_label(legs, "macho64-x86_64"),
                        "darwin", "arm64", every)["run"]
    check("a same-OS TRANSLATION layer (Rosetta) is emulated, not native",
          _rosetta["mode"] == "launched" and _rosetta["fidelity"] == "emulated",
          "%r" % (_rosetta,))
    # Complementary to `verdict`, which is populated only for a NON-run. A leg
    # that never executes has no fidelity to report, and a defaulted one would
    # be a value a reader could mistake for a measurement.
    check("a SKIPPED leg reports no fidelity at all, as verdict's complement",
          all(p["run"]["fidelity"] is None
              for p in (plan_leg(l, "darwin", "arm64", set()) for l in legs)
              if p["run"]["mode"] == "skip"))
    check("every RUNNING leg reports a fidelity from the closed vocabulary",
          all(p["run"]["fidelity"] in RUN_FIDELITIES
              for host in (("linux", "arm64"), ("windows", "x86_64"),
                           ("darwin", "arm64"))
              for p in (plan_leg(l, host[0], host[1], every) for l in legs)
              if p["run"]["mode"] != "skip"))

    # ── THE NEW VERDICT IS IN THE VOCABULARY, IN THE LEDGER'S POSITION ──────
    # `tests/harness/test_sqlite_harness_legs.cpp` compares this list against
    # armVerdictName() over kAllArmVerdicts IN ORDER, so the position is not a
    # style choice — it is the pin. Asserted here too so a Python-side reorder
    # is caught by the resolver's own self-test, not only by the C++ gate.
    check("the launcher-prerequisite verdict is in VERDICTS",
          "skipped-launcher-prerequisite-missing" in VERDICTS)
    check("...immediately after its sibling skipped-emulator-missing",
          VERDICTS.index("skipped-launcher-prerequisite-missing")
          == VERDICTS.index("skipped-emulator-missing") + 1,
          "%r" % (VERDICTS,))
    check("...and before skipped-build-input-missing, so the two RUN-side "
          "environmental skips stay adjacent",
          VERDICTS.index("skipped-launcher-prerequisite-missing")
          < VERDICTS.index("skipped-build-input-missing"))

    # ── THE DECLARATION RULES, EACH REFUSED BY NAME ────────────────────────
    # These are also driven end-to-end through `lint()` by the mutation table at
    # the bottom of this self-test; asserted here as units so a failure says
    # WHICH rule broke instead of only that some finding disappeared.
    def _req_findings(**over):
        return launcher_requires_findings("fx", dict(_lentry, **over))

    def _env_findings(**over):
        return launcher_env_findings("fx", dict(_lentry, **over))

    check("a shipped-shaped entry has NO findings (the positive control)",
          not _req_findings() and not _env_findings(),
          "%r %r" % (_req_findings(), _env_findings()))
    _no_key = dict(_lentry)
    _no_key.pop("requires")
    check("NO `requires` KEY AT ALL is refused — `[]` is a CLAIM",
          any("requires" in f
              for f in launcher_requires_findings("fx", _no_key)))
    check("an empty `requires` is ACCEPTED — it is the claim, not the absence",
          not _req_findings(requires=[]))
    for _field in LAUNCHER_REQUIREMENT_KEYS:
        _row = dict(_lreq)
        _row.pop(_field)
        check("a requirement with no `%s` is refused, naming the field" % _field,
              any(_field in f for f in _req_findings(requires=[_row])))
    check("an unknown requirement KIND is refused",
          any("kind" in f for f in _req_findings(
              requires=[dict(_lreq, kind="socket")])))
    check("a ${VAR} this entry's env does not declare is refused BY NAME",
          any("NOPE" in f for f in _req_findings(
              requires=[dict(_lreq, path="${NOPE}/lib/x")])))
    _no_env = dict(_lentry)
    _no_env.pop("env")
    check("NO `env` KEY AT ALL is refused — `{}` is a CLAIM too",
          any("env" in f for f in launcher_env_findings("fx", _no_env)))
    check("an env KEY that is not a variable name is refused",
          any("2BAD" in f for f in _env_findings(env={"2BAD": "/x"})))
    check("an env VALUE that is empty is refused",
          any("QEMU_LD_PREFIX" in f
              for f in _env_findings(env={"QEMU_LD_PREFIX": ""})))
    # ★ A DRIVER-namespace path on a TRANSLATING launcher. The carrier forwards
    # BYTES; it does not translate them, so this value arrives meaningless.
    check("a windows-drive path in a windows-to-wsl launcher's env is refused",
          any("windows-drive" in f for f in _env_findings(
              env={"QEMU_LD_PREFIX": "C:\\sysroot"})))
    check("...and the SAME value is fine on a non-translating launcher, "
          "because there it is this driver's own namespace",
          not launcher_env_findings("fx", dict(
              _drv_entry, env={"QEMU_LD_PREFIX": "C:\\sysroot"},
              requires=[dict(_lreq, kind="directory",
                             path="${QEMU_LD_PREFIX}")])))
    check("a PATH-VALUED env with no requires row referencing it is refused",
          any("QEMU_LD_PREFIX" in f for f in _env_findings(
              requires=[dict(_lreq, path="/absolute/ld.so")])))
    check("...and a NON-path env value owes no row",
          not launcher_env_findings("fx", dict(
              _lentry, env={"SQLITE_TEST_PATTERN_LIST": "a,b"},
              requires=[dict(_lreq, path="/absolute/ld.so")])))

    # ── THE REGISTRY AS AN INSTRUMENT ───────────────────────────────────────
    # D-PROCESS-CHECK-THE-REGISTRY-FOR-A-MATCHED-CONTROL-BEFORE-COMMISSIONING-ONE.
    # ★ DRIVEN THROUGH THE REAL INPUT PATH: a file on disk, in the registry's own
    # row format, parsed by the shipped parser. Nothing here is re-typed data
    # handed straight to an assertion.
    # ⚠ THE FIXTURE ANCHOR NAMES ARE DELIBERATELY INERT, AND THAT IS NOT STYLE.
    # ✔MEASURED (TF-C124): `real-examples/` is a SCANNED ROOT of the anchor guard
    # (tools/check-anchor-registry.sh, over *.sh *.ps1 *.py — added by
    # [[D-HARNESS-ANCHOR-GUARD-SKIPS-HARNESS-DRIVERS]]), so ANY anchor-shaped
    # literal in THIS file — test data, sample output, docstring — is a CITATION
    # as far as the guard is concerned, and a first draft of this fixture failed
    # the gate by citing a row that does not exist. A fixture must therefore
    # never wear a name a real row could ever have: naming a REAL anchor here
    # would be worse still, because deleting that row would then point the guard
    # at test data instead of at code. Runtime output is not scanned — only
    # literals in this source are — so printing a real row's text is fine.
    import tempfile as _rtf
    # The two fixture names, ASSEMBLED rather than written: with either spelled
    # as one literal the guard extracts it and fails the gate. `_FX` is what
    # makes them inert AND unmistakable in the fixture's own output.
    _FX = "D" + "-FIXTURE-NOT-A-REAL-ANCHOR-"
    _FX_A, _FX_B = _FX + "RUNDIR", _FX + "OTHER-LEG"
    _rdir = _rtf.mkdtemp(prefix="dss-regctl-")
    try:
        _reg = os.path.join(_rdir, "_deferred-anchor-registry.md")
        with open(_reg, "w", encoding="utf-8") as _fh:
            _fh.write(
                "| Anchor | Status | Resolution | Files |\n"
                "| --- | --- | --- | --- |\n"
                "| `%s` | the elf64-x86_64 leg runs its " % _FX_A +
                "databases over DrvFs; wal2 and walsetlk fail identically under "
                "gcc | run it on ext4 | build-and-test.ps1 |\n"
                "| `%s` | about the pe64-x86_64 leg and " % _FX_B +
                "nothing else | n/a | x |\n"
                "| not a row at all, no anchor here |\n")
        _lines, _note = registry_controls(_reg, ["elf64-x86_64"], ["walsetlk-2.1.3"])
        check("a row naming BOTH the leg and the failing family is found",
              _note == "" and len(_lines) == 1
              and _FX_A in _lines[0],
              "note=%r lines=%r" % (_note, _lines))
        check("...and it reports WHICH tokens matched, so the hit is auditable",
              "walsetlk" in _lines[0] and "elf64-x86_64" in _lines[0])
        check("a row naming only the OTHER leg is not offered",
              all(_FX_B not in ln for ln in _lines))
        # ★ THE ANTI-NOISE RULE, and it is the one that decides whether this is an
        # instrument or a wall of text: with a failing family in hand, a row that
        # matches only the LEG is not a matched control.
        _lines2, _ = registry_controls(_reg, ["pe64-x86_64"], ["walsetlk-2.1.3"])
        check("the LEG alone does not qualify a row once a family is known",
              all(_FX_B not in ln for ln in _lines2))
        _lines3, _ = registry_controls(_reg, ["pe64-x86_64"], [])
        check("...but with NO failing family the leg is all there is, and is used",
              any(_FX_B in ln for ln in _lines3))
        check("a 3-character fragment is not a family (it matches everything)",
              registry_control_tokens("wal-1.2", "test") == ["wal-1.2"])
        check("a leg label is searched WHOLE, never decomposed",
              registry_control_tokens("elf64-x86_64", "leg") == ["elf64-x86_64"])
        check("a family token matches on WORD BOUNDARIES, not as a substring",
              registry_controls(_reg, [], ["wal2-1.0"])[0]
              and not registry_controls(_reg, [], ["wal2x-1.0"])[0])
        # FAIL-SOFT, asserted rather than assumed: this runs on a failure path.
        _miss, _mnote = registry_controls(os.path.join(_rdir, "nope.md"),
                                          ["elf64-x86_64"], [])
        check("an ABSENT registry yields no rows and ONE explanatory line",
              _miss == [] and "not readable" in _mnote)
        check("nothing MATCHING yields no rows and says so, never silence",
              registry_controls(_reg, [], ["zzzznosuchfamily-1.0"])[0] == []
              and "nothing matched"
              in registry_controls(_reg, [], ["zzzznosuchfamily-1.0"])[1])
        check("a registry that is a DIRECTORY is fail-soft, not an exception",
              registry_controls(_rdir, ["elf64-x86_64"], [])[0] == [])
    finally:
        shutil.rmtree(_rdir, ignore_errors=True)

    # ── the launcher's run FILESYSTEM ────────────────────────────────────────
    # D-HARNESS-WSL-LAUNCHED-LEG-RUNDIR-IS-DRVFS. The third namespace, asserted
    # in the same shape as the two above: every planned run carries a DECLARED
    # verb, an unknown verb RAISES rather than degrading to `driver`, and at least
    # one cell in this catalogue actually needs a non-`driver` answer — otherwise
    # every assertion here is about a mechanism nothing reaches.
    for h in SELF_TEST_HOSTS:
        for leg in plan(h[0], h[1], every, path)["legs"]:
            verb = leg["run"].get("runFilesystem")
            check("run plan carries a DECLARED runFilesystem (%s/%s %s)"
                  % (h[0], h[1], leg["label"]),
                  verb in RUN_FILESYSTEMS, "got %r" % verb)
    check("an unknown runFilesystem verb raises rather than meaning 'driver'",
          _raises(lambda: run_filesystem("nope")))
    check("an unknown runFilesystem verb raises from the splice path too",
          _raises(lambda: splice_working_dir(["x"], "nope", "/d")))
    check("some declared launcher needs a non-driver runFilesystem",
          any(leg["run"].get("runFilesystem") not in (None, "driver")
              for h in SELF_TEST_HOSTS
              for leg in plan(h[0], h[1], every, path)["legs"]))
    # THE SPLICE, ASSERTED WHOLE. The working-directory option goes immediately
    # after the launcher's PROGRAM NAME, before the option that introduces the
    # child command — `wsl.exe --cd <dir> -e <prog>`, never `wsl.exe -e --cd …`,
    # which would hand `--cd` to the fixture. ✔MEASURED on a Windows host:
    # `wsl.exe --cd /tmp -e pwd` -> /tmp.
    check("the working-directory option is spliced after the program name",
          splice_working_dir(["wsl.exe", "-e"], "wsl-linux", "/tmp/x")
          == ["wsl.exe", "--cd", "/tmp/x", "-e"],
          "got %r" % (splice_working_dir(["wsl.exe", "-e"], "wsl-linux", "/tmp/x"),))
    check("a driver-filesystem launcher's argv is returned UNCHANGED",
          splice_working_dir(["qemu-x86_64"], "driver", "/anything")
          == ["qemu-x86_64"])
    # A `driver` leg's run directory is THIS driver's own — the empty
    # `launcherPath` is how both drivers tell the two cases apart, so it is the
    # one field whose emptiness is load-bearing.
    _elf = leg_by_label(legs, "elf64-x86_64", "self-test")
    _win = run_dir_plan(_elf, "windows", "x86_64", every, r"C:\o\run")
    check("a launched wsl leg gets a run directory in the LAUNCHER's filesystem",
          _win["launcherPath"].startswith("/tmp/") and "elf64-x86_64" in _win["launcherPath"],
          "got %r" % _win["launcherPath"])
    check("...whose launcher argv carries the working-directory option",
          _win["launcher"] == ["wsl.exe", "--cd", _win["launcherPath"], "-e"],
          "got %r" % (_win["launcher"],))
    check("...and argv PREFIXES that never route through a local shell",
          all(a[:2] == ["wsl.exe", "-e"]
              for a in (_win["mkdirArgv"], _win["rmTreeArgv"], _win["copyArgv"])),
          "a `sh -c` form would hand the argv back to the local shell "
          "D-TOOLS-WSL-EXE-WITHOUT-DASH-E-RUNS-A-LOCAL-SHELL exists to keep it "
          "away from; got %r" % ([_win["mkdirArgv"], _win["rmTreeArgv"],
                                  _win["copyArgv"]],))
    _lin = run_dir_plan(_elf, "linux", "x86_64", every, "/o/run")
    check("a NATIVE leg's run directory stays this driver's own",
          _lin["launcherPath"] == "" and _lin["driverPath"] == "/o/run",
          "got %r" % _lin)
    check("...and no argv prefix is offered for it",
          not any((_lin["mkdirArgv"], _lin["rmTreeArgv"], _lin["copyArgv"])))
    # TWO OUTPUT ROOTS MUST NOT COLLIDE IN ONE SHARED /tmp — the digest is what
    # makes a second checkout, or a second DSS_OUT, safe to run concurrently.
    check("the launcher run directory is derived from the DRIVER's own",
          run_dir_plan(_elf, "windows", "x86_64", every, r"C:\a\run")["launcherPath"]
          != _win["launcherPath"])
    check("...and is STABLE for one driver directory (a re-run re-wipes, never litters)",
          run_dir_plan(_elf, "windows", "x86_64", every, r"C:\o\run")["launcherPath"]
          == _win["launcherPath"])

    # ── the EARNED CONFOUNDS, per leg ────────────────────────────────────────
    # D-HARNESS-CONFOUND-LEDGER-IS-PER-DRIVER-NOT-PER-LEG. The catalogue is the
    # ledger and the lint is what keeps it honest; these assert the RESOLUTION,
    # i.e. that both drivers are handed the same rows in the grammar they speak.
    # ⓘ WRAPPED, so a raise here REPORTS instead of taking the runner down. This is
    # the FIRST call in the file that resolves confounds with no probe verdicts, so
    # it is the first casualty of any defect on the unprobed path — and an
    # uncaught raise made the whole self-test exit with a traceback and NO `FAIL`
    # line, i.e. a red that named nothing. ✔MEASURED while red-on-disabling the
    # unprobed guard.
    # ── THE RUNS THESE FIXTURES ARE JUDGED AGAINST ──────────────────────────
    # ★ EVERY confound resolution now STATES WHICH KERNEL the leg's fixture runs
    # in, because that is what decides whether a measurement of this machine
    # applies to it at all. Two runs, and both are used below in both directions.
    # [D-HARNESS-ENVIRONMENT-PROBE-MEASURES-THE-DRIVERS-KERNEL-NOT-THE-LAUNCHED-ONE]
    _same_kernel = {"mode": "native", "runFilesystem": "driver"}
    _cross_kernel = {"mode": "launched", "runFilesystem": "wsl-linux"}
    _launched_same = {"mode": "launched", "runFilesystem": "driver"}

    def _gate(verdicts, run, outcome="entered", kernel=None):
        """probe_gate over fixture verdicts FILED UNDER A KERNEL.

        ★ THE `kernel` OVERRIDE IS THE POINT OF THE HELPER, not a convenience:
        passing a kernel OTHER than the run's is how the cross-kernel case is
        constructed — a measurement of one machine offered to a leg that executes
        on another. Defaulting it to the run's own kernel keeps every other call
        site reading as "this leg's kernel answered X"."""
        if verdicts is None:
            return probe_gate(None, run)
        k = kernel if kernel is not None else probe_kernel(run["runFilesystem"])
        return probe_gate(
            {k: kernel_measurement(k, outcome, "self-test fixture", verdicts)},
            run)

    def _resolves(leg):
        try:
            return isinstance(leg_confounds(leg, _gate(None, _same_kernel)),
                              list)
        except BaseException:                                   # noqa: BLE001
            return False
    check("every leg's confounds resolve (a missing key is FATAL, never empty)",
          all(_resolves(l) for l in legs),
          "an unprobed resolution must still ANSWER — with every conditional row "
          "dropped — rather than raise: %r"
          % [l["label"] for l in legs if not _resolves(l)])
    check("a leg with no `confounds` key RAISES rather than defaulting to []",
          _raises(lambda: leg_confounds({"label": "x"},
                                        _gate(None, _same_kernel))))
    # ★★ AND A CONSUMER HANDED THE RAW VERDICT MAP REFUSES. This is the shape V1
    # shipped in: one function knew about the launcher and the other did not, and
    # nothing said so. A gate is a distinct object precisely so that mistake is a
    # LegError and not a silently-unfiltered `present`.
    check("a raw verdict map handed where a GATE belongs RAISES (both consumers)",
          _raises(lambda: leg_confound_decisions(
              legs[0], {"clock-realtime-steps": {"verdict": "present"}}))
          and _raises(lambda: confound_report_lines("x", [], {"a": 1})),
          "a consumer reading a raw map would honour a cross-kernel `present`, "
          "which is the exact defect the gate object exists to make impossible")
    check("an unknown confound scope raises rather than meaning unconditional",
          _raises(lambda: confound_scope_prefix("sometimes")))
    # ★★ THE MESSAGE IS ASSERTED, NOT JUST THE REFUSAL, and red-on-disable is what
    # forced that. `any` is refused TWICE over — by the RETIRED branch and, if that
    # goes, by the generic "unknown scope" branch below it — so deleting the RETIRED
    # branch left `_raises` perfectly satisfied and the mutation read as VACUOUS.
    # ✔MEASURED. The generic refusal is still SAFE (the value never becomes an
    # excusal), but its diagnostic sends the reader hunting for a scope that was
    # never a scope instead of telling them the claim is now `requires: []`. That is
    # a real property and it needs a real assertion.
    _any_why = ""
    try:
        confound_scope_prefix("any")
    except LegError as _exc:
        _any_why = str(_exc)
    check("scope 'any' is refused AS RETIRED, naming its replacement",
          "RETIRED" in _any_why and "requires: []" in _any_why,
          "'any' meant 'excused however this leg runs', which is now the explicit "
          "`requires: []`. It must be refused, and the refusal must say WHICH "
          "spelling replaced it — 'unknown confound scope' is true and useless. "
          "got: %r [D-HARNESS-CONFOUND-SCOPE-IS-A-RUN-MODE-NOT-A-HOST]" % _any_why)
    check("scope 'emulated' becomes the drivers' `emulated:` prefix",
          confound_scope_prefix("emulated") == "emulated:")
    # ★ THE ASYMMETRY IS THE POINT, AND IT IS ASSERTED RATHER THAN DESCRIBED.
    # A catalogue in which every leg carried the same list would be the global
    # list again, wearing a per-leg shape — so the self-test refuses that.
    # ⓘ RESOLVED WITH EVERY PROBE PRESENT, so these checks keep asking what they
    # have always asked — "which patterns does this leg DECLARE" — rather than
    # silently becoming "which survive on the machine running the test". The
    # GATING is asserted separately, immediately below, on its own fixtures.
    _probes_present = {nm: {"verdict": "present", "why": "self-test fixture",
                            "verb": "wall-clock-step", "evidence": {},
                            "source": PROBE_SOURCE_MEASURED}
                       for nm in environment_probes(load_catalogue_doc(path))}
    # ⓘ ON A LAUNCHED, SAME-KERNEL RUN (wine/qemu/`arch`), so an `emulated:` row is
    # in force and a measured verdict applies — i.e. these keep asking what the
    # catalogue DECLARES, which is what they have always asked.
    _sets = {l["label"]: set(leg_confounds(
        l, _gate(_probes_present, _launched_same))) for l in legs}
    check("the legs do NOT all carry the same confound set",
          len({frozenset(v) for v in _sets.values()}) > 1,
          "identical sets on every leg would be the old global list in a per-leg "
          "costume; got %r" % _sets)
    _pe_leg_for_oracle = leg_by_label(legs, "pe64-x86_64", path)
    # ── THE PER-LEG ATTRIBUTION ORACLE ──────────────────────────────────────
    # [D-HARNESS-PE64-HAS-NO-SAME-PLATFORM-ORACLE] The defect was an OUTPUT LINE:
    # one `oracle : <path>` for the whole run, printed for legs whose platform
    # that binary is not. These drive the real classifier and the real report over
    # the SHIPPED catalogue, and they assert the WORDS, because the words are the
    # thing that was wrong.
    _elf_ref = "x86_64:elf64-x86_64-linux-exec"
    for _l in legs:
        _cls, _why = oracle_class_for_leg(_l, _elf_ref, "/out/reference-testfixture")
        _same = _l["spec"] == _elf_ref
        check("oracle class for %s against an ELF/Linux reference is %s"
              % (_l["label"], "same-platform" if _same else "cross-platform"),
              _cls == ("same-platform" if _same else "cross-platform"),
              "got %r — %s" % (_cls, _why))
        _lines = "\n".join(oracle_report_lines(
            _l, _elf_ref, "/out/reference-testfixture", None,
            ("", "", ["<fixture: nothing on PATH>"])))
        if _same:
            check("the %s report claims the oracle" % _l["label"],
                  ": SAME-PLATFORM" in _lines, _lines)
        else:
            # ★★ THE ASSERTION THAT WOULD HAVE CAUGHT THE ORIGINAL DEFECT: a leg
            # whose reference is another platform's binary must SAY it has none,
            # and must name the fallback. "Prints something" is not the property;
            # "cannot be read as an available control" is.
            # ⚠ THE MATCHER IS `": SAME-PLATFORM"`, NOT `"SAME-PLATFORM"`. The
            # bare substring also occurs inside the ANCHOR NAME the fallback
            # paragraph cites, so the loose form failed on every cross-platform
            # leg — a pin whose witness is not unique in its own subject, which
            # is the discipline this file applies to red-on-disable and must
            # apply to itself. Only the CLAIM (`<leg>: SAME-PLATFORM`) counts.
            check("the %s report says NO ORACLE, in those terms" % _l["label"],
                  ": NO ORACLE" in _lines and ": SAME-PLATFORM" not in _lines,
                  _lines)
            check("the %s report names the FALLBACK control" % _l["label"],
                  "FALLBACK CONTROL" in _lines and "vary the RUNTIME" in _lines,
                  _lines)
    # An UNMEASURED reference is not a control, and neither is an absent one.
    _cls, _ = oracle_class_for_leg(legs[0], "", "/out/reference-testfixture")
    check("a reference whose target could not be MEASURED is not an oracle",
          _cls == "absent")
    check("no reference at all is not an oracle",
          oracle_class_for_leg(legs[0], _elf_ref, "")[0] == "absent")
    # ── D-HARNESS-FAILING-REFERENCE-ORACLE-COLLAPSES-TO-NO-ORACLE ───────────
    # ★★★ THE THREE STATES `absent` USED TO SPELL AS ONE. The defect was never
    # that the harness lacked the fact — `--build-reference-oracle` returns it
    # and `attribute_build_failure` reads it — it was that the VERDICT LINE threw
    # it away, so "the control ran and failed" and "no control was possible" came
    # out as the same sentence. That cost P13 and P14 two full cycles hunting a
    # dss miscompile that was upstream. These assert the CLASS and the WORDS,
    # because the words are what a reader acts on.
    _no_bin = ""
    check("a control that RAN AND FAILED is its own class, never 'absent'",
          oracle_class_for_leg(legs[0], _elf_ref, _no_bin,
                               ORACLE_STATUS_BUILD_FAILED)[0] == "build-failed")
    check("'no compiler on this host' is its own class, never 'absent'",
          oracle_class_for_leg(legs[0], _elf_ref, _no_bin,
                               ORACLE_STATUS_NO_COMPILER)[0]
          == "no-reference-compiler")
    check("a control that was never called is still 'absent'",
          oracle_class_for_leg(legs[0], _elf_ref, _no_bin,
                               ORACLE_STATUS_NOT_CALLED)[0] == "absent")
    # ★ THE THREE MUST NOT SHARE A SENTENCE. Asserting the classes differ is not
    # enough — the defect was in the PROSE, so the prose is what must differ.
    _whys = {oracle_class_for_leg(legs[0], _elf_ref, _no_bin, _s)[1]
             for _s in (ORACLE_STATUS_BUILD_FAILED, ORACLE_STATUS_NO_COMPILER,
                        ORACLE_STATUS_NOT_CALLED)}
    check("...and each states a DIFFERENT next step, in different words",
          len(_whys) == 3, sorted(_whys))
    # ★★ A STATUS CANNOT PROMOTE A BINARY. The status only ever refines the
    # NO-BINARY case; a cross-platform binary stays cross-platform however the
    # build went, or a green status would launder another platform's reference
    # into this leg's oracle — the exact defect the sibling row closed.
    _cross = [_l for _l in legs if _l["spec"] != _elf_ref]
    check("a status never re-classifies a binary that EXISTS",
          all(oracle_class_for_leg(_l, _elf_ref, "/out/ref", _s)[0]
              == "cross-platform"
              for _l in _cross
              for _s in ORACLE_STATUSES))
    # ⚠ AN UNKNOWN STATUS RAISES rather than degrading to the pessimistic class.
    # A typo'd driver flag must not buy a quiet, plausible, wrong verdict line.
    check("an unrecognised oracle status is REFUSED, never guessed",
          _raises(lambda: oracle_class_for_leg(legs[0], _elf_ref, _no_bin,
                                               "build-faild")))
    # THE REPORT'S WORDS on the build-failed leg: it must still say NO ORACLE
    # (there is genuinely no binary to run) while naming the log as the lead, and
    # it must NOT be readable as "no control was possible here".
    _bf = "\n".join(oracle_report_lines(
        legs[0], _elf_ref, _no_bin, None, ("", "", ["<fixture>"]),
        ORACLE_STATUS_BUILD_FAILED))
    check("a build-failed leg still says NO ORACLE — there is no binary to run",
          ": NO ORACLE" in _bf and ": SAME-PLATFORM" not in _bf, _bf)
    check("...but says the control was ATTEMPTED and points at its log",
          "RAN and" in _bf and "log" in _bf, _bf)
    check("...and never claims no control was possible",
          "no reference binary was produced or preserved" not in _bf, _bf)
    # ★ ONE VOCABULARY, TWO READERS. `attribute_build_failure` and the classifier
    # must agree on what "the control ran" means; a second literal list is how
    # they would drift apart silently.
    # ★★ THE SAFETY PROPERTY, and it is the one worth having: NO status may
    # conjure an oracle out of a MISSING BINARY. A `built` status with nothing on
    # disk is still `absent` — the binary is the control, the status is only its
    # provenance, and a green status must never be able to launder an absent
    # control into an available one.
    check("no oracle status can conjure a control out of a missing binary",
          all(oracle_class_for_leg(legs[0], _elf_ref, _no_bin, _s)[0]
              != "same-platform" for _s in ORACLE_STATUSES))
    check("a 'built' status with no surviving binary is still absent",
          oracle_class_for_leg(legs[0], _elf_ref, _no_bin,
                               ORACLE_STATUS_BUILT)[0] == "absent")
    # ONE VOCABULARY, TWO READERS: the attributor's notion of "the control ran"
    # is a SUBSET of the declared statuses and excludes both no-control states.
    check("'attempted' is a subset of the declared statuses, excluding both no-control states",
          set(ORACLE_STATUSES_ATTEMPTED) <= set(ORACLE_STATUSES)
          and ORACLE_STATUS_NO_COMPILER not in ORACLE_STATUSES_ATTEMPTED
          and ORACLE_STATUS_NOT_CALLED not in ORACLE_STATUSES_ATTEMPTED)
    check("every declared oracle status is a class this catalogue can render",
          all(oracle_class_for_leg(legs[0], _elf_ref, _no_bin, _s)[0]
              in ORACLE_CLASSES for _s in ORACLE_STATUSES))
    # A leg that DID get its own same-platform oracle claims it, and says by what.
    _own = "\n".join(oracle_report_lines(
        _pe_leg_for_oracle, _elf_ref, "/out/reference-testfixture",
        {"path": "/out/pe64/reference-testfixture.exe",
         "cc": "x86_64-w64-mingw32-gcc", "triple": "x86_64-w64-mingw32"}))
    check("a leg with its OWN same-platform oracle reports it, naming the compiler",
          ": SAME-PLATFORM" in _own and "x86_64-w64-mingw32-gcc" in _own
          and ": NO ORACLE" not in _own, _own)
    # The oracle's FILE NAME is a target fact, and an unknown target OS RAISES.
    check("the oracle file name carries the TARGET's executable suffix",
          reference_oracle_name(_pe_leg_for_oracle) == "reference-testfixture.exe"
          and reference_oracle_name(legs[0]) == "reference-testfixture")
    check("an unknown target OS is REFUSED an oracle name, never guessed",
          _raises(lambda: reference_oracle_name({"label": "x",
                                                 "spec": "x86_64:elf64-x86_64-plan9-exec"})))
    # The build argv is composed from the leg's OWN manifest — one declaration,
    # two compilers. Asserted on CONTENT, because a missing -D or -I is a
    # different program compiled and would read as a codegen difference.
    _argv = reference_oracle_argv(
        "cc", {"defines": ["A=1"], "includes": ["/inc"], "sources": ["/a.c"],
               "resolveLibraries": ["/lib/z.so", {"path": "/lib/tcl.so",
                                                  "importName": "libtcl8.6.so"}]},
        "/out/ref", ["-lm"])
    check("the oracle build argv carries the manifest's defines, includes, "
          "sources and resolved libraries",
          _argv == ["cc", "-o", "/out/ref", "-DA=1", "-I/inc", "/a.c",
                    "/lib/z.so", "/lib/tcl.so", "-lm"],
          "got %r" % (_argv,))
    # ── THE ABORT HALF OF THE SAME LEDGER ───────────────────────────────────
    # [D-HARNESS-ABORT-HAS-NO-EARNED-CONFOUND-VOCABULARY] Driven through the
    # SHIPPED catalogue's own rows, never a retyped fixture: these assert about
    # the ledger this harness actually runs on.
    _pe = leg_by_label(legs, "pe64-x86_64", path)
    _pe_gate = _gate(_probes_present, _launched_same)
    _pe_abort = leg_abort_confounds(_pe, _pe_gate)
    check("pe64 declares its EARNED abort row in the abort name space",
          "^veryquick/nolock\\.test$" in _pe_abort,
          "abort rows = %r" % _pe_abort)
    # ★★ THE SEPARATION IS THE POINT: one ledger, never one name space. An
    # `abort-file` pattern reaching the UNIT matcher would excuse a unit that
    # merely looked like a file path, and a unit pattern reaching the abort
    # matcher would excuse an abort nobody earned.
    check("an `abort-file` row is NOT handed to the unit matcher",
          not (set(_pe_abort) & _sets["pe64-x86_64"]),
          "overlap = %r" % sorted(set(_pe_abort) & _sets["pe64-x86_64"]))
    check("a `unit` row is NOT handed to the abort matcher",
          "^win32longpath-1\\.3$" not in [p.split(":", 1)[-1] for p in _pe_abort],
          "abort rows = %r" % _pe_abort)
    # ── THE DIAGNOSTIC IS THE IDENTITY; THE FILE IS ONLY THE LOCATION ───────
    # [D-HARNESS-ABORT-CONFOUND-KEYED-ON-LOCATION-NOT-IDENTITY] Driven on the
    # VERBATIM fixture text, laid out as a real aborted segment log is: passing
    # tests first, the fatal tail last. A pin that handed the classifier a bare
    # diagnostic string would never exercise abort_diagnostic_text, which is the
    # half that decides WHAT the diagnostic even is.
    _real_tail = 'error copying "test.db" to "sv_test.db": permission denied'
    _abort_log = ("nolock-5.0... Ok\n"
                  "nolock-5.1... Ok\n"
                  "%s\n"
                  "    while executing\n" % _real_tail)
    _diag = abort_diagnostic_text(_abort_log)
    check("the abort diagnostic is the log's FATAL TAIL, not its first line",
          _real_tail in _diag and "nolock-5.1" not in _diag,
          "got %r" % _diag)
    # ★ EARNED vs UNEARNED, BOTH DIRECTIONS, through the real classifier.
    _row, _why = classify_abort(_pe, _pe_gate, "veryquick/nolock.test", _diag)
    check("a PROVEN abort is EARNED and arrives with its provenance",
          _row is not None
          and all(_row["row"].get(k, "").strip() for k in CONFOUND_PROVENANCE_KEYS),
          "row=%r why=%s" % (_row and _row["pattern"], _why))
    # ★★★ THE PIN THE OPERATOR ASKED FOR, AND THE WHOLE POINT OF THE FIELD: the
    # SAME FILE with a DIFFERENT DIAGNOSTIC must NOT match. Without it, a codegen
    # crash inside nolock.test inherits an excusal earned for a Windows lock
    # violation — a silent acquittal of the compiler by the very ledger that
    # exists to convict it.
    _crash = abort_diagnostic_text(
        "nolock-3.1... Ok\n"
        "child process exited abnormally\n"
        "    Segmentation fault (core dumped)\n")
    _row_c, _why_c = classify_abort(_pe, _pe_gate, "veryquick/nolock.test",
                                    _crash)
    check("the SAME FILE with a DIFFERENT DIAGNOSTIC is NOT earned and still "
          "fails the leg", _row_c is None,
          "it matched %r — an abort row would then excuse a LOCATION rather "
          "than a failure, and any crash in that file would be forgiven"
          % (_row_c and _row_c["pattern"]))
    check("...and the refusal SAYS the file has a row but this is not its failure",
          "DIFFERENT failure in the same file" in _why_c, _why_c)
    # ★ NO EXTRACTABLE DIAGNOSTIC — a silent, zero-byte crash — matches NOTHING.
    # ⚠ AND THE PIN ASSERTS THE *REASON*, NOT ONLY THE `None`. Its first form
    # checked `row is None` and survived deleting the guard it exists for: with
    # `abortDiagnostic` required and non-empty, `re.search(want, "")` already
    # answers no, so the None was over-determined and the pin was VACUOUS about
    # the thing it names. ✔MEASURED by red-on-disable, which refused to certify
    # it. What the guard uniquely buys is the DIAGNOSIS a triager gets — "this
    # abort could not be identified" rather than "no row matched" — so that is
    # what is asserted.
    _row_e, _why_e = classify_abort(_pe, _pe_gate, "veryquick/nolock.test", "")
    check("an abort with NO extractable diagnostic is NOT earned",
          _row_e is None and "NO extractable diagnostic" in _why_e
          and "Absence of evidence never satisfies a matcher" in _why_e,
          "row=%r why=%s" % (_row_e and _row_e["pattern"], _why_e))
    check("a zero-byte log yields no diagnostic at all",
          abort_diagnostic_text("") == ""
          and abort_diagnostic_text("nolock-5.1... Ok\n") == "")
    _row2, _why2 = classify_abort(_pe, _pe_gate, "veryquick/symlink2.test",
                                  _diag)
    check("an UNEARNED abort is REFUSED, so it still fails the leg",
          _row2 is None, "it matched %r, which would silence an unproven abort"
                         % (_row2 and _row2["pattern"]))
    # And the same file under a DIFFERENT permutation is a different abort: the
    # pattern is anchored on the whole `perm/file` name, not on the file alone.
    _row3, _ = classify_abort(_pe, _pe_gate, "full/nolock.test", _diag)
    check("the abort pattern is matched against `perm/file`, not the file alone",
          _row3 is None,
          "a row earned under one permutation must not excuse another")
    # Every abort row must CARRY the field the lint demands — asserted here too,
    # because the lint proves the catalogue is well-formed and this proves the
    # matcher is actually given something to conjoin with.
    for _l in legs:
        for _r in _l.get("confounds", []):
            if confound_match_kind(_r) == "abort-file":
                check("abort row %s on %s constrains a DIAGNOSTIC"
                      % (_r["pattern"], _l["label"]),
                      bool(str(_r.get("abortDiagnostic", "")).strip()))
    # ── THE BUILD HALF OF THE SAME LEDGER ───────────────────────────────────
    # [D-HARNESS-BUILD-FAILURE-HAS-NO-PER-TU-ATTRIBUTION]
    # ★★ THE FIXTURES ARE VERBATIM LOG TEXT, in the exact shape each compiler
    # really emits — dss's `error[CODE]: [target=…] <subject>` + `  --> path:l:c`
    # and gcc's `path:l:c: error: '<name>' undeclared`. A pin that handed the
    # attributor pre-parsed rows would be testing the stub, and the parsers are
    # the half that can silently return an empty set (which reads as "the
    # compiler said nothing" and grants or denies amnesty on nothing).
    _tu_up = "/s/ext/misc/fileio.c"
    _tu_dss = "/s/src/test_fs.c"
    _dss_log = (
        "info[X_OptPassSkipped]: [target=t] opt::Licm: skipped loop\n"
        "error[S0006]: [target=t] LPFILETIME \n"
        "  --> %s:15737:3\n"
        "   |\n"
        "error[S0001]: [target=t] MultiByteToWideChar\n"
        "  --> %s:15600:11\n"
        "   |\n"
        "error[S0006]: [target=t] ULARGE_INTEGER \n"
        "  --> %s:15740:3\n"
        "   |\n"
        "error[S0011]: [target=t] arrow operator '->' pointee is not a composite type\n"
        "  --> %s:15750:9\n"
        "   |\n"
        "error[S0001]: [target=t] open\n"
        "  --> %s:30194:10\n"
        "   |\n" % (_tu_up, _tu_up, _tu_up, _tu_up, _tu_dss))
    _ref_log = (
        "cc -o ref -DA=1 %s %s\n\n"
        "%s: In function 'winUtf8To16':\n"
        "%s:136:31: error: 'CP_UTF8' undeclared (first use in this function)\n"
        "%s:136:11: warning: implicit declaration of function "
        "'MultiByteToWideChar' [-Wimplicit-function-declaration]\n"
        "%s:296:3: error: unknown type name 'LPFILETIME'; did you mean 'ETIME'?\n"
        % (_tu_up, _tu_dss, _tu_up, _tu_up, _tu_up, _tu_up))
    _sources = [_tu_up, _tu_dss]
    _row_up = {"pattern": r"ext/misc/fileio\.c$", "matches": "build-tu",
               "upstreamSubjects": ["ULARGE_INTEGER"], "requires": [],
               "earnedOn": "x", "earnedAt": "x", "mechanism": "x", "anchor": "x"}
    _decs = leg_confound_decisions({"label": "L", "confounds": [_row_up]},
                                   _gate(_probes_present, _launched_same))
    # ★ THE PARSERS FIRST, ON CONTENT — a count assertion is satisfied by the
    # right number of wrong rows, which is precisely how a subject-set comparison
    # goes quietly wrong.
    _dparsed = [r for r in dss_build_diagnostics(_dss_log) if r["severity"] == "error"]
    check("the dss log reader takes the SUBJECT and the FILE off a real "
          "diagnostic, and ignores info[] lines",
          [(r["subject"], r["file"]) for r in _dparsed]
          == [("LPFILETIME", _tu_up), ("MultiByteToWideChar", _tu_up),
              ("ULARGE_INTEGER", _tu_up),
              ("arrow operator '->' pointee is not a composite type", _tu_up),
              ("open", _tu_dss)],
          "got %r" % ([(r["subject"], r["file"]) for r in _dparsed],))
    check("a dss subject that is one identifier or a header name IS a name; "
          "prose is NOT, and \"\" never reads as `fine`",
          dss_subject_identifier("LPFILETIME") == "LPFILETIME"
          and dss_subject_identifier("unistd.h") == "unistd.h"
          and dss_subject_identifier("arrow operator '->' pointee is not a "
                                     "composite type") == "")
    # ⚠ THE SUGGESTION STRIP, ASSERTED BY ITS EFFECT. `did you mean 'ETIME'?`
    # names something the reference is NOT complaining about; left in, it would
    # corroborate a dss error about `ETIME` and hand out an amnesty nobody
    # earned. The strip fails in the direction that makes amnesty HARDER.
    _rparsed = reference_build_diagnostics(_ref_log)
    _names = sorted({n for r in _rparsed for n in r["names"]})
    check("the reference reader names what the diagnostic is ABOUT and NOT its "
          "spelling suggestion",
          _names == ["CP_UTF8", "LPFILETIME", "MultiByteToWideChar"],
          "got %r" % (_names,))
    check("the reference reader keeps WARNING severity, because C23 makes "
          "gcc's implicit-declaration warning an error",
          {r["severity"] for r in _rparsed} == {"error", "warning"}
          and any(r["severity"] == "warning" and "MultiByteToWideChar" in r["names"]
                  for r in _rparsed))
    # ★★ THE VERDICTS, BOTH DIRECTIONS, THROUGH THE REAL ATTRIBUTOR.
    _att = attribute_build_failure(_dss_log, _ref_log, "build-failed", _sources,
                                   _decs, "L")
    _by_tu = {t["tu"]: t for t in _att["tus"]}
    check("a TU the reference ALSO rejects, named by an earned row, with every "
          "dss identifier corroborated, is UPSTREAM",
          _by_tu[_tu_up]["attribution"] == "upstream",
          _by_tu[_tu_up]["why"])
    check("...and the ONE name the reference never mentioned was cleared by the "
          "row's DECLARED `upstreamSubjects`, not by the measurement",
          _by_tu[_tu_up]["excusedByDeclaration"] == ["ULARGE_INTEGER"]
          and _by_tu[_tu_up]["residue"] == [],
          "%r" % (_by_tu[_tu_up],))
    # ★★★ AND THE ROW IS LOAD-BEARING, NOT DECORATION. ✔MEASURED by red-on-disable
    # 2026-08-18: with this pin absent, deleting the `row is None` gate outright
    # left the whole battery GREEN — every other fixture happened to carry a row,
    # so nothing exercised the unearned path and a measured-but-undeclared TU
    # would have been silently acquitted. The refusal must also SAY what to do,
    # or a triager reads "charged to dss" and starts hunting a codegen bug.
    _att_norow = attribute_build_failure(_dss_log, _ref_log, "build-failed",
                                         _sources, [], "L")
    _norow = [t for t in _att_norow["tus"] if t["tu"] == _tu_up][0]
    check("a TU the reference ALSO rejects but that NO earned `build-tu` row "
          "names is charged to dss, and the refusal names the missing row",
          _norow["attribution"] == "dss"
          and "NO `matches: build-tu` row" in _norow["why"]
          and "write the row" in _norow["why"], _norow["why"])
    check("a TU the reference ACCEPTED is charged to dss even though the "
          "reference build as a whole FAILED",
          _by_tu[_tu_dss]["attribution"] == "dss"
          and "ACCEPTED what dss rejected" in _by_tu[_tu_dss]["why"],
          _by_tu[_tu_dss]["why"])
    check("the cascade errors are COUNTED and named as not attributable, never "
          "silently folded into either side",
          _by_tu[_tu_up]["dssCascade"] == 1
          and any("name no identifier" in l
                  for l in build_attribution_report_lines(_att)))
    # ★★★ THE REGRESSION-INSIDE-A-BROKEN-TU PIN, and it is the reason the residue
    # exists at all. A NEW dss error naming something the reference never named
    # must re-charge the TU to dss even though the TU is genuinely upstream-broken
    # and has an earned row. Same fixtures, one added diagnostic.
    _regressed = _dss_log + ("error[S0001]: [target=t] _wchmod\n"
                             "  --> %s:15631:8\n   |\n" % _tu_up)
    _att_r = attribute_build_failure(_regressed, _ref_log, "build-failed",
                                     _sources, _decs, "L")
    _up_r = [t for t in _att_r["tus"] if t["tu"] == _tu_up][0]
    check("a NEW dss-only identifier inside an UPSTREAM-broken TU re-charges "
          "that TU to dss and NAMES it",
          _up_r["attribution"] == "dss" and _up_r["residue"] == ["_wchmod"]
          and "_wchmod" in _up_r["why"], _up_r["why"])
    # ★★ AN ABSENT CONTROL EXCUSES NOTHING — the discipline the whole mechanism
    # rests on. Three ways the control can be absent, all three refused.
    for _status, _srcs, _what in (
            ("no-reference-compiler", _sources, "no compiler on this host"),
            ("", _sources, "the oracle was never invoked"),
            ("build-failed", [_tu_dss], "the TU is not in the manifest")):
        _a = attribute_build_failure(_dss_log, _ref_log, _status, _srcs, _decs, "L")
        _t = [t for t in _a["tus"] if t["tu"] == _tu_up][0]
        check("an amnesty is REFUSED when %s" % _what,
              _t["attribution"] == "unattributable", "%s -> %r" % (_what, _t))
    # An unreadable reference log (a shape this reader does not know) must SAY so
    # and still refuse every amnesty — a silent empty parse would deny amnesties
    # for a reason no reader could see.
    _a_gap = attribute_build_failure(_dss_log, "cc: fatal error\n", "build-failed",
                                     _sources, _decs, "L")
    check("a reference log this reader parses to ZERO diagnostics is a REPORTED "
          "harness gap, not a silent denial",
          "ZERO diagnostics" in _a_gap["parserGap"]
          and _a_gap["verdictClass"] == "dss"
          and any("HARNESS GAP" in l
                  for l in build_attribution_report_lines(_a_gap)))
    # An unanchored TU pattern silently attributes every longer sibling path.
    check("the lint REFUSES a `build-tu` pattern that is not anchored at '$'",
          any("not anchored" in f for f in build_tu_row_findings("L", 
              dict(_row_up, pattern=r"ext/misc/fileio\.c"))))
    check("the lint REFUSES a `build-tu` row with no `upstreamSubjects` key, "
          "because missing cannot be told from empty",
          any("upstreamSubjects" in f for f in build_tu_row_findings("L", 
              {k: v for k, v in _row_up.items() if k != "upstreamSubjects"})))
    # A mistyped match kind must be REFUSED, never defaulted — a row meant for an
    # abort that quietly became a unit row excuses nothing and reads as coverage.
    check("an unknown `matches` value RAISES rather than defaulting to `unit`",
          _raises(lambda: confound_match_kind({"pattern": "^x", "matches": "abortfile"})))
    check("`matches` defaults to `unit` when the key is absent",
          confound_match_kind({"pattern": "^x"}) == "unit")
    # zipfile-25.0 turns on POSIX fopen() SUCCEEDING on a directory. It is
    # declared on every POSIX leg and MUST NOT be on the Windows one, where the
    # test passes — the single sharpest OS split in the ledger, and the one the
    # old .ps1 had backwards.
    _zip = "^zipfile-25\\.0$"
    for l in legs:
        posix = spec_target_os(l["spec"]) in ("linux", "darwin")
        check("zipfile-25.0 is declared exactly on the POSIX legs (%s)" % l["label"],
              (_zip in _sets[l["label"]]) == posix,
              "target OS %r, declared=%r" % (spec_target_os(l["spec"]),
                                             _zip in _sets[l["label"]]))
    # ★ THE INTENT, unchanged since TF-C123: pe64 must never re-inherit a SIBLING's
    # confounds. All six patterns it once carried were earned on LINUX, and its own
    # native tier measured 0 errors / 979,736 — so a bare pattern here is a copy.
    # ⚠ THIS PIN HAS NOW HAD TO GIVE GROUND TWICE, AND THE SECOND TIME IS WHY IT IS
    # WRITTEN THE WAY IT IS BELOW — a guard that is weakened every time it fires ends
    # up asserting nothing, so the question each time must be "what did it actually
    # protect?", never "what is the smallest edit that makes it green?".
    # [D-TEST-PE64-CONFOUND-PIN-WEAKENED-BY-ITS-OWN-SUBJECT.]
    #   TF-C123: it asserted the SET WAS EMPTY. True when written; WRONG in TF-C124,
    #     when `win32longpath-1.3` was genuinely EARNED on pe64 UNDER WINE with a
    #     matched control (the same dss-built testfixture.exe runs
    #     `win32longpath-1.3... Ok` on REAL Windows — compiler held constant, only the
    #     runtime varied). It was re-pinned as: every pe64 confound is `emulated:`.
    #   2026-08-10: that too became wrong, and in the honest direction. `sessionnoact-4.3`
    #     was earned on pe64 by an experiment run on NATIVE WINDOWS — one testfixture.exe,
    #     three file sets, three outcomes (pair -> 1 error out of 40 with
    #     `got: [invalid command name "log"]`; the subject ALONE -> 0 of 34; the
    #     session3 pair, which resets the leaked callback -> 0 of 50). An
    #     `emulated:`-only rule would have forced that row to lie about where it was
    #     earned, or to be dropped.
    # ★ SO PIN THE RULE THE PROXY WAS STANDING IN FOR. The intent was never "wine only";
    #   it was pe64 MUST NOT RE-INHERIT A SIBLING'S CONFOUNDS — all six patterns it once
    #   carried were earned on LINUX while its own native tier measured 0 errors /
    #   979,736. Two checks say that directly:
    #     (a) every pe64 row's provenance NAMES pe64 — a sibling-only `earnedOn` is
    #         exactly the copy that shipped before, and this rejects all six of them.
    #     (b) a row that can excuse a NATIVE Windows failure (scope `any`) must SAY it
    #         was earned natively. That blocks the one bad move the scope proxy really
    #         guarded: taking a wine-only observation and widening it to cover the
    #         platform that actually proves this leg.
    _pe = _sets["pe64-x86_64"]
    _pe_rows = [l for l in legs if l["label"] == "pe64-x86_64"][0]["confounds"]
    # startswith, NOT `"pe64" in ...`: a containment test is satisfied by a row that
    # merely MENTIONS this leg while being earned on another ("…not re-measured on
    # pe64"), which is the exact sentence a transfer would carry. The leg's own
    # $confoundsComment forbids transfers here — every row must be earned on pe64 —
    # so the provenance must OPEN with the leg, and both shipped rows do.
    check("every pe64 confound names pe64 FIRST in its own provenance (no sibling copies)",
          all(r["earnedOn"].startswith("pe64-x86_64") for r in _pe_rows),
          "a pe64 row earned on a sibling leg is the TF-C123 defect returning, and "
          "this leg admits no transfers at all; got %r"
          % [(r["pattern"], r["earnedOn"][:60]) for r in _pe_rows])
    # ⓘ RE-EXPRESSED FOR THE POST-`scope` VOCABULARY, and it is the SAME RULE, not a
    # weakening: a row with no `scope` and no `requires` excuses a failure however
    # this leg runs — which on pe64 includes REAL WINDOWS, the platform that proves
    # it — so such a row must say it was earned natively. Previously the trigger was
    # spelled `scope == "any"`; it is now "unconditional", which is what `any`
    # always meant. The one row that is NOT unconditional (win32longpath, wine) is
    # still exempt, exactly as before.
    check("an UNCONDITIONAL pe64 confound declares it was earned NATIVELY",
          all(r.get("scope") or r.get("requires")
              or "NATIVE" in r["earnedOn"].upper() for r in _pe_rows),
          "an unconditional row excuses a failure on real Windows, which is the "
          "platform that proves this leg — a wine-only observation may not be "
          "widened to cover it; got %r"
          % [(r["pattern"], r.get("scope", ""), r.get("requires"),
              r["earnedOn"][:60]) for r in _pe_rows])
    # The six that shipped on this leg in the old global list, by name. `_pe` holds
    # WIRE strings, so drop the scope prefix first — and drop it as a KNOWN prefix
    # from the scope vocabulary, not with lstrip() (a character SET, correct here
    # only by the accident that every pattern starts with `^`) and not with a bare
    # split(":") (a regex is free to contain a colon, and that one would then eat
    # the pattern's own head and read as clean).
    def _unscoped(wire):
        for s in CONFOUND_SCOPES:
            pre = confound_scope_prefix(s)
            if pre and wire.startswith(pre):
                return wire[len(pre):]
        return wire
    _POSIX_EARNED = ("^walsetlk", "^busy2", "^recoverfault", "^date-2", "^zipfile")
    check("pe64 still carries no copy of a POSIX-earned pattern",
          not any(_unscoped(p).startswith(_POSIX_EARNED) for p in _pe),
          "these were earned on Linux and once shipped here wholesale; got %r" % _pe)
    # A scoped pattern must reach the drivers WITH its scope, or the qemu-only
    # writecrash excusal silently becomes a bare one and suppresses a future
    # genuine regression on a native run.
    check("the qemu writecrash excusal keeps its `emulated:` scope",
          "emulated:^writecrash-" in _sets["elf64-arm64"],
          "got %r" % _sets["elf64-arm64"])
    check("...and is declared on NO other leg",
          not any("writecrash" in p
                  for lbl, s in _sets.items() if lbl != "elf64-arm64"
                  for p in s),
          "macho64-x86_64 runs through `arch -x86_64`, so its run mode is "
          "`launched` and an `emulated:` pattern WOULD match there — the per-leg "
          "ledger is what stops the scope name carrying it across; got %r" % _sets)

    # ── THE ENVIRONMENT-PROBE GATE ───────────────────────────────────────────
    #
    # D-HARNESS-CONFOUND-SCOPE-IS-A-RUN-MODE-NOT-A-HOST.
    #
    # ★★ EVERY ARM IS DRIVEN WITH INJECTED CLOCKS, so the whole verb is asserted on
    # any host, in milliseconds, and — the part that matters — the PRESENT arm is
    # EXERCISED rather than described. A probe that can only be seen to say ABSENT
    # (which is what this project's boxes mostly are) is a probe whose detecting
    # half has never run.
    _doc = load_catalogue_doc(path)
    _reg = environment_probes(_doc)
    check("the catalogue declares an environment-probe registry", bool(_reg),
          "the clock rows require one; an empty registry means the lint is "
          "refusing every `requires` name and nothing is gated")
    check("`clock-realtime-steps` is declared and names a known verb",
          _reg.get("clock-realtime-steps", {}).get("verb") in
          ENVIRONMENT_PROBE_VERBS, "got %r" % _reg.get("clock-realtime-steps"))

    def _fake_clocks(step_at, step_by, interval, nap_at=(), nap_by=0.0):
        """THREE clocks off one sleeper, because a host suspend separates two
        that a healthy machine keeps together:

          awake      advances by `interval` a tick and NOT AT ALL during a nap
          reference  the CONTINUOUS clock: `interval` a tick PLUS the whole nap
          wall       tracks the reference, PLUS `step_by` on the ticks in
                     `step_at` — the CLOCK_REALTIME defect, and only it

        So `nap_at` moves wall and reference together (real time passed; the
        machine did not run) while `step_at` moves wall alone. The verb is handed
        all three separately, so the instrument under test really is the
        DIFFERENCE of two clocks and the self-test decides WHICH two."""
        state = {"n": 0, "awake": 0.0, "ref": 0.0, "wall": 1_000_000.0}

        def sleeper(_secs):
            state["n"] += 1
            state["awake"] += interval
            state["ref"] += interval
            state["wall"] += interval
            if state["n"] in nap_at:
                state["ref"] += nap_by
                state["wall"] += nap_by
            if state["n"] in step_at:
                state["wall"] += step_by
        return (lambda: state["wall"], lambda: state["awake"],
                lambda: state["ref"], sleeper)

    _cfg = dict(_reg["clock-realtime-steps"]["config"])
    _iv = float(_cfg["sampleIntervalMs"]) / 1000.0

    def _measure(step_at, step_by, config=None, interval=None, nap_at=(),
                 nap_by=0.0):
        w, a, r, s = _fake_clocks(step_at, step_by, interval or _iv, nap_at,
                                  nap_by)
        return _measure_wall_clock_step(config or _cfg, clock=w, awake=a,
                                        reference=r, sleeper=s)
    # A HEALTHY clock: the wall clock tracks real elapsed time exactly.
    _v, _why, _ev = _measure(set(), 0.0)
    check("a clock that tracks elapsed time measures ABSENT",
          _v == "absent" and _ev["steps"] == 0, "%s / %r" % (_v, _ev))
    # ★ THE MEASURED SIGNATURE: ±34.47 s flipping every ~5 s. At 250 ms that is a
    # step every 20 ticks, in ALTERNATING directions — so this fixture also proves
    # the |.| is real and a negative step is not read as no step.
    _flips = {20: 1, 40: -1, 60: 1, 80: -1}
    _st = {"n": 0, "mono": 0.0, "wall": 1_000_000.0}

    def _osc_sleep(_secs):
        _st["n"] += 1
        _st["mono"] += _iv
        _st["wall"] += _iv + (34.47 * _flips.get(_st["n"], 0))
    _v, _why, _ev = _measure_wall_clock_step(
        _cfg, clock=lambda: _st["wall"], awake=lambda: _st["mono"],
        reference=lambda: _st["mono"], sleeper=_osc_sleep)
    check("the MEASURED 34.47 s oscillation measures PRESENT",
          _v == "present" and _ev["steps"] == 4, "%s / %r / %s" % (_v, _ev, _why))
    check("...and the PRESENT verdict quotes its own evidence",
          "34.4" in _why and "step(s)" in _why, _why)
    # ★★ THE FAIL-SAFE BOUNDARY, BOTH SIDES OF `minStepsRequired`. ONE step is not
    # enough — a single jump is a suspend/resume, not a stepping clock, and the
    # floor exists so a probe can never be satisfied by one pair of readings.
    _v, _, _ev = _measure({20}, 34.47)
    check("a SINGLE 34.47 s jump measures ABSENT (one step < the floor of 2)",
          _v == "absent" and _ev["steps"] == 1, "%s / %r" % (_v, _ev))
    _v, _, _ev = _measure({20, 40}, 34.47)
    check("...and TWO jumps measure PRESENT", _v == "present" and _ev["steps"] == 2,
          "%s / %r" % (_v, _ev))
    check("...at FULL drift — the reference did not move, so the whole 34.47 s "
          "survives the subtraction",
          abs(_ev["worstDriftSeconds"] - 34.47) < 1e-3
          and abs(_ev["spreadSeconds"] - 68.94) < 1e-3, "%r" % _ev)
    # ★★★ THE HOST SUSPEND, WHICH IS NOT A CLOCK STEP AND MUST NEVER SCORE AS ONE.
    # D-HARNESS-PROBE-READS-A-HOST-SUSPEND-AS-A-WALL-CLOCK-STEP: the wall clock and
    # the CONTINUOUS reference both advance by the nap — real time passed, and a
    # healthy wall clock is SUPPOSED to show it — while only the AWAKE clock stops.
    # ✔THE MEASURED SHAPE, 2026-08-13 on a healthy Mac: a 32.9 s nap scored `worst
    # per-tick drift 32.8999 s and offset spread 32.9004 s (need 5.000 s)`, i.e. it
    # cleared BOTH magnitude thresholds; two of them in one window is the forged
    # `present` that activates every requires-gated confound row.
    _v, _why, _ev = _measure(set(), 0.0, nap_at={20, 40}, nap_by=32.9)
    check("two 32.9 s HOST SUSPENDS measure ABSENT — a nap is not a clock step",
          _v == "absent" and _ev["steps"] == 0
          and _ev["worstDriftSeconds"] == 0.0 and _ev["spreadSeconds"] == 0.0,
          "%s / %r / %s" % (_v, _ev, _why))
    check("...and the nap did NOT eat the sampling window",
          _ev["samples"] == 80 and _ev["windowSeconds"] == 20.0,
          "the window is bounded on the AWAKE clock precisely so a suspend cannot "
          "shorten the sample and weaken the detection; got %r" % _ev)
    # ★★ AND THE FIXTURE REALLY IS NAP-SHAPED — hand the SAME three clocks the OLD
    # reference (the awake one) and the identical nap forges a full-magnitude
    # PRESENT. The defect, reproduced in milliseconds, one argument away.
    _w, _a, _r, _s = _fake_clocks(set(), 0.0, _iv, {20, 40}, 32.9)
    _v, _why, _ev = _measure_wall_clock_step(_cfg, clock=_w, awake=_a,
                                             reference=_a, sleeper=_s)
    check("...against the OLD awake reference that very nap forges PRESENT",
          _v == "present" and _ev["steps"] == 2
          and abs(_ev["worstDriftSeconds"] - 32.9) < 1e-3,
          "if this arm ever says ABSENT the nap fixture has stopped being a nap "
          "and the arm above proves nothing; got %s / %r" % (_v, _ev))
    # ★★ THE ONE THAT SETTLES IT: a napping host that ALSO has the real defect.
    # [[D-ENV-WSL2-CLOCK-REALTIME-STEPS-34S]] must stay fully visible through the
    # naps, and EXACTLY the two real steps may be counted — not the naps too.
    _v, _why, _ev = _measure({30, 60}, 34.47, nap_at={20, 40}, nap_by=32.9)
    check("a REAL 34.47 s step survives two naps in the same window",
          _v == "present" and _ev["steps"] == 2
          and abs(_ev["worstDriftSeconds"] - 34.47) < 1e-3,
          "the WSL2 clock defect this probe exists to catch must remain "
          "detectable on a host that also suspends; got %s / %r" % (_v, _ev))
    # THE OTHER THRESHOLD: many small jumps, well under minStepSeconds, are
    # scheduler noise. A probe that fired on those would excuse the clock family on
    # every loaded machine in the world.
    _v, _, _ev = _measure({10, 20, 30, 40, 50, 60}, 0.05)
    check("six 50 ms jumps measure ABSENT (below minStepSeconds)",
          _v == "absent" and _ev["steps"] == 0, "%s / %r" % (_v, _ev))
    # AN UNREADABLE CLOCK is an UNMEASURED machine: indeterminate, never present.
    def _boom(_secs):
        raise OSError("clock_gettime: EINVAL")
    _v, _why, _ = _measure_wall_clock_step(_cfg, clock=lambda: 0.0,
                                           awake=lambda: 0.0,
                                           reference=lambda: 0.0,
                                           sleeper=_boom)
    check("a clock that cannot be sampled measures INDETERMINATE",
          _v == "indeterminate" and "cut short" in _why, "%s / %s" % (_v, _why))
    check("an interpreter with NO monotonic clock measures INDETERMINATE",
          _measure_wall_clock_step(_cfg, clock=lambda: 0.0, awake=None,
                                   reference=lambda: 0.0,
                                   sleeper=lambda _s: None)[0]
          == "indeterminate")
    # ── THE REFERENCE CLOCK THIS HOST ACTUALLY RESOLVES TO ──────────────────
    # Not a fixture: the real selection, on the machine running the self-test, so
    # a platform whose vocabulary this derivation does not cover is caught HERE
    # rather than by a forged verdict months later.
    _ref_read, _ref_name, _ref_suspend = _resolve_continuous_clock()
    check("the continuous-clock resolution names a clock and it ticks",
          bool(_ref_name) and all(ord(c) < 127 for c in _ref_name)
          and _ref_read() > 0.0 and _ref_read() <= _ref_read(),
          "got name=%r reading=%r" % (_ref_name, _ref_read()))
    check("a clock that counts suspend can never be BEHIND the awake one",
          _ref_suspend is None or _ref_suspend >= 0.0,
          "%s reports %r s of recorded suspend since boot, which is impossible: "
          "a suspend can only ADD to the continuous clock" % (_ref_name,
                                                              _ref_suspend))
    check("a host that exposes no second clock id says so instead of guessing",
          (_ref_suspend is None) == (_ref_name == "time.monotonic()"),
          "the fallback and the corroboration must agree about whether this "
          "platform distinguishes awake time from elapsed time; got name=%r "
          "recordedSuspend=%r" % (_ref_name, _ref_suspend))
    # ★ THE REVERT GUARD, and the only arm that can catch one: every fixture
    # above INJECTS its clocks, so restoring the pre-fix default would sail past
    # them. This one asks the running interpreter what it publishes and refuses
    # the fallback on any host that publishes the pair — vacuous on Windows,
    # where the fallback is the honest answer, and load-bearing everywhere else.
    # ⚠ It asks the INTERPRETER, not the resolver: a guard that read the fact
    # off the thing it is guarding would be reverted along with it.
    import time as _t_mod
    _publishes_pair = (
        hasattr(_t_mod, "clock_gettime") and hasattr(_t_mod, "CLOCK_MONOTONIC")
        and (hasattr(_t_mod, "CLOCK_BOOTTIME")
             or hasattr(_t_mod, "CLOCK_UPTIME_RAW")))
    check("a host that DOES publish both clocks must not fall back to the "
          "awake one",
          (not _publishes_pair)
          or (_ref_suspend is not None
              and _ref_name in ("CLOCK_BOOTTIME", "CLOCK_MONOTONIC")),
          "this interpreter names both an awake and an elapsed monotonic clock, "
          "so resolving the reference to %r is the pre-fix behaviour that reads "
          "a host suspend as a wall-clock step" % _ref_name)
    # ★★★ THE ASYMMETRY, ASSERTED. `indeterminate` must NOT honour a row.
    # ★★ EVERY ARM'S `why` IS ASCII, ASSERTED AT THE SOURCE. It is published into
    # the confound report, which the differential battery compares BYTE FOR BYTE
    # across two shells — and this is not hypothetical: confound_report_lines
    # REFUSED an em-dash that the ABSENT arm was emitting, which is the guard
    # firing one level below where it was written. Caught here too, so the next one
    # reds in the self-test rather than in a `--plan` on whichever host.
    _real_ref = {"t": 0.0}

    def _real_ref_tick(_secs):
        _real_ref["t"] += float(_cfg["sampleSeconds"]) / 2.0
    _whys = [_measure(set(), 0.0)[1], _measure({20, 40}, 34.47)[1],
             _measure({20}, 34.47)[1],
             _measure(set(), 0.0, nap_at={20, 40}, nap_by=32.9)[1],
             _measure_wall_clock_step(_cfg, clock=lambda: 0.0, awake=None,
                                      reference=lambda: 0.0,
                                      sleeper=lambda _s: None)[1],
             _measure_wall_clock_step(_cfg, clock=lambda: 0.0,
                                      awake=lambda: 0.0,
                                      reference=lambda: 0.0, sleeper=_boom)[1],
             # THE REAL RESOLUTION's own words, not a fixture's: the resolved
             # reference clock's NAME reaches the report, so a non-ASCII spelling
             # would break the byte-for-byte differential exactly like the em-dash
             # did. Only `reference` is left to resolve; the wall clock is frozen
             # and the awake clock is driven two ticks so the window closes at
             # once, with no dependence on how fast this host is.
             _measure_wall_clock_step(_cfg, clock=lambda: 1.0,
                                      awake=lambda: _real_ref["t"],
                                      sleeper=_real_ref_tick)[1]]
    # ── WHOSE KERNEL DID THE PROBE MEASURE? ─────────────────────────────────
    # D-HARNESS-ENVIRONMENT-PROBE-MEASURES-THE-DRIVERS-KERNEL-NOT-THE-LAUNCHED-ONE.
    # A launcher that crosses into another kernel gets a probe verdict about the
    # WRONG machine. Declared per runFilesystem verb — never derived from the verb's
    # NAME — and REQUIRED, for the same reason `runFilesystem` itself is: `True` is
    # the CLAIM, and it was the unexamined one.
    check("every runFilesystem verb declares whether it shares this kernel",
          all("sharesDriverKernel" in RUN_FILESYSTEMS[v]
              for v in RUN_FILESYSTEMS),
          "missing on: %r" % [v for v in sorted(RUN_FILESYSTEMS)
                              if "sharesDriverKernel" not in RUN_FILESYSTEMS[v]])
    check("`driver` shares this kernel; `wsl-linux` does NOT",
          RUN_FILESYSTEMS["driver"]["sharesDriverKernel"] is True
          and RUN_FILESYSTEMS["wsl-linux"]["sharesDriverKernel"] is False,
          "Wine/qemu/`arch` are in-process translation on one kernel; wsl.exe "
          "crosses into the very kernel whose clock defect this probe looks for")
    # ── HOW THAT KERNEL IS ENTERED, DECLARED AND CHECKED ────────────────────
    # Every field below DECIDES something (an argv, a translation, a refusal); a
    # field only the report reads is what put this anchor in the registry.
    _KERNEL_FIELDS = ("kernelEntryArgv", "kernelProbeInterpreter",
                      "kernelEntryPathTranslation")
    check("every runFilesystem verb declares how its kernel is ENTERED",
          all(f in RUN_FILESYSTEMS[v] for v in RUN_FILESYSTEMS
              for f in _KERNEL_FIELDS),
          "missing: %r" % [(v, f) for v in sorted(RUN_FILESYSTEMS)
                           for f in _KERNEL_FIELDS if f not in RUN_FILESYSTEMS[v]])
    check("an entry argv comes WITH an interpreter, and an empty one comes WITHOUT",
          all(bool(RUN_FILESYSTEMS[v]["kernelEntryArgv"])
              == bool(RUN_FILESYSTEMS[v]["kernelProbeInterpreter"])
              for v in RUN_FILESYSTEMS),
          "entering a kernel and having something there to run this script are two "
          "different facts and they fail differently; a verb stating one without "
          "the other can report neither")
    # ★★ THE BOUNDARY IS SPELLED ONCE. `kernelEntryArgv` duplicates the prefix of
    # every other argv template on its entry, so the duplication is turned into a
    # CHECKED invariant rather than left as two places to keep in step.
    # ⚠ AND THE CHECK IS EQUALITY WITH THE *LONGEST* COMMON PREFIX, NOT "IS A
    # PREFIX OF EACH". It used to be the latter (`o[:len(entry)] == entry`), which
    # is true of EVERY truncation: ✔MEASURED, the candidate ['wsl.exe'] PASSED it
    # — i.e. it would have accepted an entry mechanism with `-e` DROPPED, which is
    # D-TOOLS-WSL-EXE-WITHOUT-DASH-E-RUNS-A-LOCAL-SHELL, and the table comment
    # claiming "the table cannot drift into two answers" was false while it said
    # so. A weaker test than the property it is named after asserts nothing.
    check("the common-prefix instrument returns the LONGEST one, not merely A one",
          argv_common_prefix([["a", "b", "c"], ["a", "b", "d"]]) == ["a", "b"]
          and argv_common_prefix([["a", "b"], ["c", "d"]]) == []
          and argv_common_prefix([["a", "b"]]) == ["a", "b"]
          and argv_common_prefix([]) == [],
          "an instrument that answered a SHORTER prefix would accept dropping "
          "`-e` from the kernel boundary and call the table coherent")
    for _v, _spec in sorted(RUN_FILESYSTEMS.items()):
        _entry_argv = list(_spec["kernelEntryArgv"])
        _others = [list(o) for o in
                   ([_spec["mkdirArgv"], _spec["rmTreeArgv"], _spec["copyArgv"]]
                    + list(_spec["probeArgv"].values())) if o]
        _lcp = argv_common_prefix(_others)
        check("runFilesystem '%s' enters its kernel by EXACTLY the longest prefix "
              "every other argv on it shares" % _v,
              _entry_argv == _lcp,
              "kernelEntryArgv=%r is not EQUAL to the longest common prefix %r of "
              "%r - the table would hold two answers about where its own kernel "
              "begins, and the shorter of them is how `-e` goes missing"
              % (_entry_argv, _lcp, _others))
        check("runFilesystem '%s' declares a path translation valid on its own host"
              % _v,
              (_spec["kernelEntryPathTranslation"] in PATH_TRANSLATIONS
               and PATH_TRANSLATIONS[_spec["kernelEntryPathTranslation"]]
               ["validHostOs"] == _spec["validHostOs"]),
              "a probe crossing into '%s' spells this driver's paths with '%s', "
              "whose validHostOs is %r while the filesystem's is %r"
              % (_v, _spec["kernelEntryPathTranslation"],
                 PATH_TRANSLATIONS.get(_spec["kernelEntryPathTranslation"], {})
                 .get("validHostOs"), _spec["validHostOs"]))
    check("every kernel NAMESPACE is itself a declared runFilesystem verb",
          probe_kernel_names() <= set(RUN_FILESYSTEMS),
          "the namespace is what measure_kernel_environments looks up for an entry "
          "mechanism, so one that is not in the table could never be entered; got "
          "%r" % sorted(probe_kernel_names()))
    check("a leg's kernel is read from the DECLARED table, never from the verb name",
          probe_kernel("driver") == "driver"
          and probe_kernel("wsl-linux") == "wsl-linux"
          and _raises(lambda: probe_kernel("somewhere-else")),
          "two verbs that both share this kernel must collapse to ONE drawer, and "
          "an unknown verb must not invent one")
    # ── `sharesDriverKernel` AND `kernelEntryArgv` MUST AGREE ────────────────
    # ✔MEASURED BEFORE THIS GUARD EXISTED: a verb declaring `sharesDriverKernel:
    # False` with an EMPTY `kernelEntryArgv` — a NEW launcher whose author
    # answered "which drawer" and forgot "how do I get there" — produced outcome
    # `in-process`, why "measured in this process, which is this driver's own
    # kernel", FILED UNDER THE FOREIGN KERNEL'S NAME, and `in-process` is in
    # KERNEL_OUTCOMES_IN_FORCE so BOTH ELF legs honoured it. This anchor's defect
    # restored verbatim, by a record that contradicts itself in its own `why`.
    # [D-HARNESS-ENVIRONMENT-PROBE-MEASURES-THE-DRIVERS-KERNEL-NOT-THE-LAUNCHED-ONE]
    #
    # ★ THE PIN DRIVES A SYNTHETIC VERB THROUGH THE REAL ACCESSOR. The two shipped
    # verbs are correct, so a pin that only reads them proves nothing whatsoever
    # about the NEXT launcher — which is the only one that can carry this defect.
    _FS_SNAPSHOT = {_k: dict(_s) for _k, _s in RUN_FILESYSTEMS.items()}
    _SYNTH_VERB = "self-test-synthetic-verb"

    def _with_synthetic_fs(shares, entry, thunk):
        """`thunk(verb)` with a synthetic verb TEMPORARILY in the REAL table, so
        the assertion goes through run_filesystem()'s own lookup rather than a
        re-typed copy of the rule."""
        RUN_FILESYSTEMS[_SYNTH_VERB] = dict(RUN_FILESYSTEMS["driver"],
                                            sharesDriverKernel=shares,
                                            kernelEntryArgv=list(entry))
        try:
            return thunk(_SYNTH_VERB)
        finally:
            del RUN_FILESYSTEMS[_SYNTH_VERB]

    check("a verb claiming ANOTHER kernel with NO WAY TO ENTER IT is REFUSED",
          _with_synthetic_fs(False, [],
                             lambda v: _raises(lambda: run_filesystem(v)))
          and _with_synthetic_fs(False, [],
                                 lambda v: _raises(lambda: probe_kernel(v))),
          "that combination measures THIS process and files the answer under the "
          "foreign kernel's name, stamped `in-process`, which is in force - the "
          "original defect, arriving through a new launcher")
    check("...and a verb claiming THIS kernel WITH a way to enter it is REFUSED too",
          _with_synthetic_fs(True, ["wsl.exe", "-e"],
                             lambda v: _raises(lambda: run_filesystem(v)))
          and _with_synthetic_fs(True, ["wsl.exe", "-e"],
                                 lambda v: _raises(lambda: probe_kernel(v))),
          "both directions, because the invariant is an EQUIVALENCE: a kernel this "
          "process claims to already be in AND a mechanism for entering it is one "
          "claim too many, and neither half may be preferred over the other")
    check("...and BOTH COHERENT combinations are accepted",
          _with_synthetic_fs(True, [], lambda v: probe_kernel(v) == "driver")
          and _with_synthetic_fs(False, ["wsl.exe", "-e"],
                                 lambda v: probe_kernel(v) == v),
          "the guard refuses the CONJUNCTION; a guard that refused either field on "
          "its own would make the table undeclarable")
    check("...and the guard is NOT `sharesDriverKernel := not kernelEntryArgv`",
          _with_synthetic_fs(False, [], lambda v: _raises(
              lambda: fs_shares_driver_kernel(v))),
          "deriving one field from the other RELOCATES this defect instead of "
          "removing it: the author who forgot the entry argv would be silently "
          "DECLARED to share this driver's kernel - the same wrong answer, "
          "arrived at more quietly, with no second field left to disagree")
    check("...and the synthetic verb left the real table exactly as it found it",
          RUN_FILESYSTEMS == _FS_SNAPSHOT,
          "a pin that mutates the declared vocabulary and does not put it back "
          "makes every check after it a measurement of the pin; got %r"
          % sorted(RUN_FILESYSTEMS))
    # ── THE ARGV THAT CROSSES THE BOUNDARY ──────────────────────────────────
    # ⓘ The translator is INJECTED, exactly as translate_path's is, so this runs
    # on a machine with no wsl.exe and still exercises the real construction.
    def _xlate(argv):
        return (0, "/mnt/c" + argv[-1][2:].replace("\\", "/"), "")
    _kargv = kernel_probe_argv("wsl-linux", "C:\\r\\harness_legs.py",
                               "C:\\r\\legs.json", ["clock-realtime-steps"],
                               _xlate)
    # ⓘ THE EXPECTED HEAD IS READ OUT OF THE TABLE, NOT RE-TYPED. It used to be
    # the literal ["wsl.exe", "-e", "python3"], which spelled the kernel boundary
    # a SECOND time — in the very file whose check above exists to prove the table
    # spells it once. With that check now asserting EQUALITY with the longest
    # common prefix, the literal was redundant as well as duplicative.
    _wslfs = RUN_FILESYSTEMS["wsl-linux"]
    _khead = list(_wslfs["kernelEntryArgv"]) + [_wslfs["kernelProbeInterpreter"]]
    check("the kernel probe argv ENTERS the kernel and re-enters THIS script there",
          _kargv[:len(_khead)] == _khead
          and _kargv[len(_khead)] == "/mnt/c/r/harness_legs.py"
          and "--probe-environment" in _kargv,
          "the entry mechanism is the runFilesystem's, not the leg's launcher: for "
          "elf64-arm64 the launcher is `wsl.exe -e qemu-aarch64`, and qemu is "
          "user-mode translation INSIDE the kernel wsl.exe already entered - it "
          "cannot change a clock, and there is no aarch64 python3 to run under it; "
          "got %r" % _kargv)
    check("...and it asks for EXACTLY the probes that kernel's legs require",
          _kargv[-2:] == ["--probe-only", "clock-realtime-steps"],
          "the in-process arm passes the same `only` set; an arm that measured "
          "more would make the cost model false and one that measured less would "
          "raise at the leg. got %r" % _kargv)
    check("...and EVERY path in it is translated before it crosses",
          not any(_looks_like_windows_drive_path(a) for a in _kargv),
          "an untranslated path does not fail as a path error over there: python "
          "opens a relative file by that name, misses, and it reads as a broken "
          "script. got %r" % _kargv)
    check("an in-process kernel builds NO argv at all",
          kernel_probe_argv("driver", "x.py", "l.json", ["p"], _xlate) == [],
          "spawning a copy of this script to ask about THIS process's own kernel "
          "would be a round-trip whose answer could differ from the caller's view")
    # ── WHICH KERNELS THIS RESOLUTION MUST MEASURE, AND HOW MANY TIMES ──────
    # ★★★ THE COST MODEL, ASSERTED ON THE SHIPPED CATALOGUE. Two ELF legs on a
    # Windows host share ONE kernel and therefore ONE 20 s sample; the driver's own
    # kernel is not sampled at all, because every row on the legs that execute
    # there is unconditional. Keying on the LEG would have sampled the same clock
    # twice to get the same answer.
    _win_legs = plan("windows", "x86_64", {"wsl.exe"}, path)["legs"]
    _needs_win = kernel_probe_needs(_win_legs)
    check("a Windows plan measures the WSL2 kernel ONCE and the driver's NOT AT ALL",
          _needs_win == {"wsl-linux": ["clock-realtime-steps"]},
          "elf64-x86_64 and elf64-arm64 both execute in the WSL2 kernel, and the "
          "legs that execute in this driver's kernel (pe64, and the skipped macho "
          "legs) declare no conditional row; got %r" % _needs_win)
    _needs_nix = kernel_probe_needs(plan("linux", "x86_64", set(), path)["legs"])
    check("...and a native Linux plan measures the driver's kernel, IN PROCESS",
          _needs_nix == {"driver": ["clock-realtime-steps"]}
          and kernel_probe_argv("driver", "x.py", "l.json",
                                _needs_nix["driver"], _xlate) == [],
          "got %r" % _needs_nix)
    check("...and a plan whose rows are all unconditional measures NOTHING",
          kernel_probe_needs([{"confoundRows": [{"pattern": "^a",
                                                 "requires": []}],
                               "run": {"runFilesystem": "wsl-linux"}}]) == {},
          "a catalogue that gates nothing must pay nothing at all")
    # ⓘ `_refuses_namedly`: "named refusal, not a traceback" is the whole
    # property, and `_raises` would let the KeyError it is guarding against
    # propagate and kill the runner instead of failing this one assertion.
    check("...and a leg with no resolved `run` REFUSES instead of raising KeyError",
          _refuses_namedly(lambda: kernel_probe_needs(
              [{"confoundRows": [{"pattern": "^a",
                                  "requires": ["clock-realtime-steps"]}]}])),
          "a bare subscript names neither the leg nor the missing key, and a "
          "traceback out of the resolver reads as a broken harness rather than a "
          "transport defect between plan_leg and this function")
    # ── WHETHER THIS INVOCATION MEASURES AT ALL ─────────────────────────────
    # ★ THE RULE: an invocation measures only when some leg whose decisions it
    # will actually READ declares a conditional row.
    # ⛔ ✔MEASURED on a Windows host BEFORE this: `--classify-abort pe64-x86_64
    # --abort veryquick/nolock.test --abort-log …` took 20.6 s, ALL of it a
    # `wsl.exe -e python3 … --probe-environment` child measuring the WSL2 kernel —
    # which pe64-x86_64 does not execute in, whose drawer its gate never opens,
    # and none of whose verdicts that path reads (the only abort row is
    # `requires: []`). Both drivers call it in a LOOP, on the ABORT path.
    # ⚠ PURE, so this is pinned WITHOUT SPAWNING: the decision is a function of
    # the resolved plan and the labels the invocation will consult.
    check("an invocation that emits the WHOLE plan consults every leg, so it "
          "still measures",
          invocation_probe_needs(_win_legs, None) == _needs_win,
          "--plan hands every leg's decisions to the driver; narrowing THAT would "
          "be the subset this must never take; got %r"
          % invocation_probe_needs(_win_legs, None))
    check("...and a single-leg invocation on a leg with NO conditional row "
          "measures NOTHING",
          invocation_probe_needs(_win_legs, {"pe64-x86_64"}) == {},
          "pe64-x86_64 executes in kernel 'driver' and every row it declares is "
          "unconditional, so a 20 s foreign-kernel spawn measured a machine whose "
          "drawer nothing on that path opens; got %r"
          % invocation_probe_needs(_win_legs, {"pe64-x86_64"}))
    check("...and a single-leg invocation on a leg WITH one still measures",
          invocation_probe_needs(_win_legs, {"elf64-x86_64"})
          == {"wsl-linux": ["clock-realtime-steps"]},
          "the cheap answer must not be the only answer: a leg whose rows ARE "
          "gated has to pay for its measurement; got %r"
          % invocation_probe_needs(_win_legs, {"elf64-x86_64"}))
    check("...and a leg the plan does not contain REFUSES rather than quietly "
          "answering 'nothing to measure'",
          _raises(lambda: invocation_probe_needs(_win_legs, {"no-such-leg"})),
          "a decision taken about a leg that does not exist is not a cheap "
          "answer, it is no answer at all")
    check("...and the CONSULTED set is the legs themselves, not a re-derivation",
          [l["label"] for l in consulted_legs(_win_legs, None)]
          == [l["label"] for l in _win_legs]
          and [l["label"] for l in consulted_legs(_win_legs, {"pe64-x86_64"})]
          == ["pe64-x86_64"],
          "which kernel a leg executes in is the output of launcher resolution; a "
          "second implementation of that here is how one ledger becomes two")
    # ★★ AND THE WIRING, NOT ONLY THE PREDICATE. A pure "does it need to measure"
    # that the CLI consults with an `if` leaves the `if` unpinned: delete it and
    # every check above stays green while every invocation pays the spawn again.
    # `measure` is INJECTED here for the same reason `runner` is, so this observes
    # WHETHER it was called AND WHAT IT WAS ASKED FOR, without spawning.
    _asked = []

    def _recording_measure(needs):
        _asked.append(needs)
        return {k: kernel_measurement(k, "in-process", "self-test fixture",
                                      _probes_present) for k in needs}
    del _asked[:]
    _got_pe = resolved_kernel_measurements(_win_legs, {"pe64-x86_64"},
                                           _recording_measure)
    check("a single-leg invocation with nothing to gate TAKES NO MEASUREMENT",
          _got_pe is None and _asked == [],
          "`--classify-abort pe64-x86_64` measured the WSL2 kernel for 20.6 s and "
          "then read ZERO verdicts from it, per abort, in a loop, on the path that "
          "runs when something has already gone wrong; got %r / asked %r"
          % (_got_pe, _asked))
    del _asked[:]
    _got_all = resolved_kernel_measurements(_win_legs, None, _recording_measure)
    check("...and an invocation that DOES need one measures EVERY kernel in the "
          "plan, never the consulted subset",
          _asked == [_needs_win] and set(_got_all or {}) == set(_needs_win),
          "plan() decides EVERY leg eagerly, so a leg whose rows require a probe "
          "whose drawer is missing hits the loud 'carries NO verdict' refusal: "
          "measure ALL of the plan's kernels or NONE. got asked=%r" % _asked)
    del _asked[:]
    _got_elf = resolved_kernel_measurements(_win_legs, {"elf64-x86_64"},
                                            _recording_measure)
    check("...and a single-leg invocation on a GATED leg measures the whole plan too",
          _asked == [_needs_win] and set(_got_elf or {}) == set(_needs_win),
          "narrowing to the consulted leg's own kernel is the subset variation "
          "that turns a cheap answer into a refusal; got asked=%r" % _asked)
    # ★★★ AND THE SUBSET VARIATION IS PINNED ON A PLAN WHERE IT IS VISIBLE. On
    # the SHIPPED Windows plan the consulted leg's kernels and the plan's kernels
    # are the same set — only the two ELF legs are conditional and they share one
    # kernel — so `measure(kernel_probe_needs(all))` and `measure(needs of the
    # consulted)` are indistinguishable there, and the check above passes either
    # way. ✔MEASURED: swapping the argument to the consulted set left --self-test
    # at passed=1916 failed=0. THIS fixture makes the two answers differ, which is
    # the only way the "ALL the plan's kernels, or none" rule can be held.
    _two_kernel_legs = [
        {"label": "gated-elsewhere", "run": {"runFilesystem": "wsl-linux"},
         "confoundRows": [{"pattern": "^a", "requires": ["clock-realtime-steps"]}]},
        {"label": "gated-here", "run": {"runFilesystem": "driver"},
         "confoundRows": [{"pattern": "^b", "requires": ["clock-realtime-steps"]}]},
    ]
    del _asked[:]
    resolved_kernel_measurements(_two_kernel_legs, {"gated-elsewhere"},
                                 _recording_measure)
    check("...and measuring a SUBSET of the plan's kernels is what must never "
          "happen: one consulted leg still measures BOTH",
          _asked == [{"driver": ["clock-realtime-steps"],
                      "wsl-linux": ["clock-realtime-steps"]}],
          "plan() decides EVERY leg eagerly, so the OTHER leg - conditional, and "
          "in a kernel this invocation would not have measured - reaches "
          "leg_confound_decisions with its drawer missing and hits the loud "
          "'carries NO verdict for it' refusal. Whether to measure is the "
          "consulted legs' question; WHICH KERNELS is never theirs; got %r"
          % _asked)
    check("...and an UNPROBED resolution is the DECLARED safe state, not an error",
          confound_gating(None) == "unprobed"
          and CONFOUND_GATINGS["unprobed"]["usable"] is False
          and not ({"^walsetlk-", "^busy2-"}
                   & set(leg_confounds(_elf, probe_gate(None, _cross_kernel)))),
          "that is why declining to measure is correctness-neutral for an "
          "invocation whose consulted leg has no conditional row: every "
          "conditional row goes INACTIVE and nothing raises")
    # ── EVERY ARM OF THE MEASUREMENT, WITH THE SPAWN INJECTED ───────────────
    # The runner is injected exactly as translate_path's is, so a machine with no
    # wsl.exe still exercises the REAL construction, the REAL parser and the REAL
    # failure paths. ⚠ EXERCISED, not read: every one of these arms decides
    # whether a failing test is excused.
    _sample = json.dumps({"clock-realtime-steps": {
        "verdict": "present", "why": "8 step(s) >= 2 required",
        "verb": "wall-clock-step", "evidence": {"steps": 8},
        "source": PROBE_SOURCE_MEASURED}})

    def _measured(runner):
        """One kernel's measurement, or a VISIBLY WRONG one naming the raise.

        ⚠ WRAPPED FOR THE `_report_for` REASON, AT THE SITE THAT ACTUALLY
        MATTERS: every arm below is a way a kernel can fail to answer, and the
        whole property is that each lands on `unreachable` rather than escaping.
        ✔MEASURED with the parser's non-string refusal removed: the runner arm
        that returns the `(0, None, None)` subprocess.run ACTUALLY produces
        raised TypeError out of json.loads, which is neither LegError nor
        OSError — it walked past both handlers and killed the self-test with NO
        `passed=`/`failed=` line at all. A pin that can only red by crashing its
        runner reports nothing about which property broke; an outcome nobody
        declared reds visibly, by name, in the arm that produced it."""
        try:
            return measure_kernel_environments(
                _doc, {"wsl-linux": ["clock-realtime-steps"]},
                "C:\\r\\harness_legs.py", "C:\\r\\legs.json",
                runner=runner, translator=_xlate)["wsl-linux"]
        except BaseException as exc:                            # noqa: BLE001
            return {"kernel": "wsl-linux", "verdicts": {}, "why": "",
                    "outcome": "<ESCAPED %s: %s>" % (type(exc).__name__, exc)}

    def _verdict_word(measurement, name="clock-realtime-steps"):
        """One probe's verdict word, or a VISIBLY WRONG string naming what was
        missing instead of a KeyError out of the middle of a check.

        ⚠ THE SAME LESSON `_report_for` AND `_gated` CARRY, AT THE THIRD SITE,
        and exercising the mutants is what found it: `probe_kernel` broken to
        answer 'driver' for every verb, and `_indeterminate_verdicts` broken to
        file nothing, both killed --self-test with a bare KeyError here — the
        second one printing NO `passed=`/`failed=` summary line at all. A pin
        that can only red by crashing its own runner reports nothing about
        WHICH property broke, and a suite that prints no summary reports
        nothing about anything.

        ⓘ The sentinel is not a verdict word, so a check comparing it against
        'indeterminate' reds; and because `and` short-circuits, the
        probe_verdict_honours() call after such a comparison is never reached
        with it — which is right, since that function refuses an invented
        verdict, correctly, and loudly, in the wrong place."""
        try:
            return _measurement_verdicts(measurement)[name]["verdict"]
        except BaseException as exc:                            # noqa: BLE001
            return "<NO VERDICT FOR %r: %s: %s>" % (name, type(exc).__name__,
                                                    exc)

    def _verdict_names(measurement):
        """Which probes a measurement filed for, guarded for the same reason."""
        try:
            return set(_measurement_verdicts(measurement))
        except BaseException as exc:                            # noqa: BLE001
            return {"<NO VERDICT MAP: %s: %s>" % (type(exc).__name__, exc)}

    def _answer(thunk):
        """`thunk()`, or a VISIBLY WRONG STRING naming the raise.

        ★ THE FOURTH TIME THIS FILE HAS NEEDED THIS SHAPE (`_gated`,
        `_report_for`, `_measured`, `_verdict_word`), so the general form is
        stated once. Every one of them exists because a pin that can only red by
        CRASHING its runner reports nothing about WHICH property broke — and a
        run that dies before its `passed=`/`failed=` line reports nothing about
        anything at all, which is strictly worse than a red."""
        try:
            return thunk()
        except BaseException as exc:                            # noqa: BLE001
            return "<RAISED %s: %s>" % (type(exc).__name__, exc)

    def _confounds_of(thunk):
        """`set(thunk())`, or a one-element set naming the raise — `_answer` for
        a caller that compares SETS and would otherwise get a TypeError from the
        sentinel instead of a failed comparison."""
        try:
            return set(thunk())
        except BaseException as exc:                            # noqa: BLE001
            return {"<RAISED %s: %s>" % (type(exc).__name__, exc)}
    _ok = _measured(lambda argv, _t: (0, _sample, ""))
    check("a kernel that ANSWERS is `entered`, and its verdict decides its legs",
          _ok["outcome"] == "entered"
          and _verdict_word(_ok) == "present"
          and "wsl.exe -e python3" in _ok["why"],
          "the measurement must carry the argv that produced it, or it is a "
          "verdict with no provenance; got %r" % _ok)
    for _label, _runner in (
            ("the entry mechanism is absent",
             lambda a, t: (127, "", "wsl.exe: not found")),
            ("the kernel has no python3",
             lambda a, t: (127, "", "exec: python3: not found")),
            ("the kernel never answers at all",
             lambda a, t: (124, "", "no answer within %.0f s" % t)),
            ("the answer is not JSON",
             lambda a, t: (0, "<3>Warning: no init\n", "")),
            ("the answer is JSON of the wrong shape",
             lambda a, t: (0, "[1,2]", "")),
            ("the answer OMITS the probe that was asked for",
             lambda a, t: (0, "{}", "")),
            ("the answer claims to have been READ FROM A FILE",
             lambda a, t: (0, json.dumps({"clock-realtime-steps": dict(
                 json.loads(_sample)["clock-realtime-steps"],
                 source=PROBE_SOURCE_INJECTED)}), "")),
            # ⚠ AND A RUNNER THAT HANDS BACK A None IT NEVER DECODED. This is
            # what subprocess.run(text=True) ACTUALLY returns for a child whose
            # bytes the locale codec cannot decode (rc=0, stdout None) —
            # ✔MEASURED — and json.loads(None) raises TypeError, which is
            # neither LegError nor OSError and killed `--plan` outright.
            ("the runner hands back output it never decoded",
             lambda a, t: (0, None, None)),
            ("the spawn itself raises", lambda a, t: _boom(a))):
        _bad = _measured(_runner)
        _bad_word = _verdict_word(_bad)
        check("UNREACHABLE, never absent, when %s" % _label,
              _bad["outcome"] == "unreachable"
              and _bad_word == "indeterminate"
              and not probe_verdict_honours(_bad_word),
              "'we could not measure' must never become 'we measured absent' - "
              "that reads as a clean bill - nor 'present', which excuses a real "
              "miscompile; got %r" % _bad)
        check("...and it files a verdict for the probe rather than nothing (%s)"
              % _label,
              _verdict_names(_bad) == {"clock-realtime-steps"},
              "an unreachable kernel that filed NOTHING would raise at the first "
              "leg whose row requires the probe, stopping the run instead of "
              "reporting it; got %r" % _bad)
    # ★ AND A KERNEL THAT ANSWERS IN UTF-8 DOES NOT BECOME A FATAL. Everything
    # here reaches confound_report_lines, which REFUSES non-ASCII because the
    # twin-parity proof compares those lines byte for byte.
    # \u2605 THE CHILD IS BOUNDED, AND THE BOUND COMES FROM THE DECLARED SAMPLE WINDOW.
    # Before this change the resolution path spawned NOTHING, so a hang was not
    # reachable; it is now, and step 1 of every corpus run goes through here.
    _budget = kernel_probe_budget_seconds(_doc, ["clock-realtime-steps"])
    check("the child gets a deadline DERIVED from the window it was asked to sample",
          _budget > float(_doc["environmentProbes"]["clock-realtime-steps"]
                          ["config"]["sampleSeconds"])
          and _budget == (KERNEL_PROBE_ENTRY_ALLOWANCE_SECONDS
                          + KERNEL_PROBE_SAMPLE_SLACK
                          * float(_doc["environmentProbes"]
                                  ["clock-realtime-steps"]["config"]
                                  ["sampleSeconds"]))
          and kernel_probe_budget_seconds(_doc, []) > 0,
          "an unbounded spawn makes the PLAN hang on a wedged distro, and the plan "
          "is step 1 of every corpus run; got %r" % _budget)
    check("...and a deadline is passed to the child, not merely computed",
          _measured(lambda a, t: (0, _sample, "") if t == _budget
                    else (1, "", "no deadline"))["outcome"] == "entered",
          "a budget nothing hands over is a comment")
    check("...and the OTHER resolution-path spawns are bounded on the SAME "
          "allowance, derived rather than reinvented",
          RESOLVER_SPAWN_BUDGET_SECONDS == KERNEL_PROBE_ENTRY_ALLOWANCE_SECONDS
          and RESOLVER_SPAWN_BUDGET_SECONDS == kernel_probe_budget_seconds(
              _doc, []),
          "`wslpath` and a launcher `probeArgv` sample nothing, so their budget "
          "IS the kernel-entry allowance at a zero window; a second literal here "
          "is a number that stops tracking the one it was copied from; got %r"
          % RESOLVER_SPAWN_BUDGET_SECONDS)
    # ★★ AND THE BUDGET IS PUBLISHABLE, because a caller OUTSIDE this process has
    # to bound this script too and had been typing its own number.
    # [D-HARNESS-ENV-PROBE-TEST-TIMEOUT-IS-A-MAGIC-NUMBER-NOT-THE-DERIVED-BUDGET]
    # The property that matters is not the value but the TRACKING: widen a
    # declared window and the published number must widen with it, by the slack
    # the arithmetic states. A constant satisfies neither clause.
    _wide = json.loads(json.dumps(_doc))
    _wide["environmentProbes"]["clock-realtime-steps"]["config"][
        "sampleSeconds"] += 30.0
    check("the published budget TRACKS the declared window rather than sitting "
          "at a constant",
          kernel_probe_budget_seconds(_wide, ["clock-realtime-steps"])
          - kernel_probe_budget_seconds(_doc, ["clock-realtime-steps"])
          == KERNEL_PROBE_SAMPLE_SLACK * 30.0
          and kernel_probe_budget_seconds(_wide, []) ==
              kernel_probe_budget_seconds(_doc, []),
          "a caller that bounds this script's spawn reads this number; if it "
          "stops moving with the catalogue the caller kills a healthy child the "
          "day a window is widened, and reports it as a defect in the probe")
    check("...and a run that SAMPLES is priced above one that samples nothing, "
          "so a caller may bound every invocation with the larger",
          kernel_probe_budget_seconds(_doc, ["clock-realtime-steps"])
          > kernel_probe_budget_seconds(_doc, []),
          "the two published budgets are what a caller chooses between; if the "
          "sampling one does not dominate, one deadline can no longer bound "
          "both and the choice comes back to the caller - which is where the "
          "typed number came from")
    # ★★★ THE DEADLINE WHERE IT IS *APPLIED*, AGAINST A REAL CHILD THAT OUTLIVES
    # IT. The two checks above assert the ARITHMETIC and that an INJECTED runner
    # receives the number; neither touches `_captured`, the only code that hands a
    # deadline to a process. ✔MEASURED: deleting `timeout=timeout` from that spawn
    # left --self-test at passed=1888 failed=0, so the whole anti-hang property —
    # the one that keeps `--plan`, step 1 of every corpus run, from blocking
    # forever on a wedged distro — had NO red-on-disable coverage at all.
    # ⓘ A REAL python, not `wsl.exe`: the property under test is "a deadline is
    # applied to a child", which needs a child that outlives one and nothing else.
    # Sub-second, so the pin costs a quarter of a second on a healthy machine.
    _slow_child = [sys.executable, "-c", "import time; time.sleep(10)"]
    _timed_rc, _, _timed_err = _run_kernel_probe(_slow_child, 0.25)
    check("the REAL kernel-probe spawn KILLS a child that outlives its deadline",
          _timed_rc == 124 and "no answer within" in _timed_err,
          "an unbounded spawn on the resolution path is a PLAN THAT HANGS, with "
          "no timeout and no diagnostic, and `except LegError` cannot catch a "
          "hang; got rc=%r err=%r" % (_timed_rc, _timed_err))
    check("...and the deadline it PRINTS is the deadline it APPLIED",
          "0.25 s" in _timed_err,
          "`%%.0f` rendered every sub-second budget as '0 s' - a diagnostic that "
          "contradicts its own bound the first time anyone tightens one; got %r"
          % _timed_err)
    _xl_raised, _xl_said = False, ""
    try:
        _run_translator(_slow_child, timeout=0.25)
    except LegError as _xl_exc:
        _xl_raised, _xl_said = True, "%s" % _xl_exc
    check("the REAL translator spawn REFUSES BY NAME when its child outlives its "
          "deadline",
          _xl_raised and "no answer within" in _xl_said,
          "kernel_probe_argv calls translate_path TWICE - the script and the "
          "catalogue - BEFORE the bounded probe child is ever started, so an "
          "unbounded wait THERE hung --plan in the one place "
          "measure_kernel_environments' `except LegError` could not see; got "
          "raised=%r said=%r" % (_xl_raised, _xl_said))
    check("...and that refusal reaches the unreachable-kernel handler, not the top",
          _measured(lambda a, t: _boom(a))["outcome"] == "unreachable"
          and measure_kernel_environments(
              _doc, {"wsl-linux": ["clock-realtime-steps"]},
              "C:\\r\\harness_legs.py", "C:\\r\\legs.json",
              runner=lambda a, t: (0, _sample, ""),
              translator=lambda argv: _run_translator(_slow_child, timeout=0.25)
          )["wsl-linux"]["outcome"] == "unreachable",
          "a translator that cannot answer is a kernel that could not be "
          "ADDRESSED - INDETERMINATE verdicts and INACTIVE rows - and never a "
          "traceback out of step 1 of a corpus run")
    # ★★ AND THE DECODE STEP ITSELF, AGAINST A REAL CHILD EMITTING REAL BYTES.
    # ⚠ The UTF-8 arm below injects an ALREADY-DECODED str, so it proves
    # _ascii_snippet works and never touches the decode that actually failed — a
    # stub testing the stub. ✔MEASURED on this host (Windows, cp1252, py3.14.3)
    # with no explicit codec: this exact child makes subprocess.run's reader
    # THREAD die and run() return `rc=0, stdout=None`, after which
    # json.loads(None) raises TypeError — neither LegError nor OSError, so it
    # escaped both handlers and killed `--plan`.
    _undecodable = [sys.executable, "-c",
                    "import sys; sys.stdout.buffer.write(bytes([129,141,144]))"]
    _raw_rc, _raw_out, _raw_err = _captured(_undecodable, _budget)
    check("a child whose bytes the locale cannot decode still yields TEXT, never None",
          _raw_rc == 0 and isinstance(_raw_out, str) and isinstance(_raw_err, str)
          and _raw_out != "",
          "rc=0 with stdout None is the shape of a SUCCESSFUL call carrying no "
          "output at all, and every consumer downstream of it is written for a "
          "string; got rc=%r out=%r err=%r"
          % (_raw_rc, _raw_out, _raw_err))
    _undec = _measured(lambda a, t: _run_kernel_probe(_undecodable, t))
    check("...and a kernel whose REAL output cannot be decoded is UNREACHABLE",
          _undec["outcome"] == "unreachable"
          and _verdict_word(_undec) == "indeterminate"
          and all(ord(c) < 127 for c in _undec["why"]),
          "this drives the REAL spawn, the REAL decode and the REAL parser: one "
          "non-UTF-8 byte printed by a distro ahead of its JSON must be a named "
          "'did not answer with JSON' refusal, never a TypeError that takes the "
          "plan down; got %r" % _undec)
    # ⓘ `_refuses_namedly`, not `_raises`: the property here IS "a NAMED refusal
    # rather than a python traceback", and `_raises` lets a TypeError propagate
    # and take the runner down — which is the very shape being fixed.
    check("...and the parser REFUSES a non-string by name, whoever produced it",
          _refuses_namedly(lambda: parse_kernel_probe_output(
              None, _doc, ["clock-realtime-steps"], "a self-test fixture")),
          "the promise of a named refusal for 'a distro that printed a warning "
          "before the JSON' must hold unconditionally, not only while the "
          "warning is ASCII and only for the runner this file ships")
    # \u26a0 AND AN INJECTED INSTRUMENT CANNOT SILENTLY NOT APPLY. Instruments are
    # CLOCKS; there is no way to send one into another kernel, so a caller that
    # asked for one there gets a refusal instead of a real measurement wearing a
    # fixture's name.
    check("instruments aimed at a kernel that must be ENTERED are refused",
          _raises(lambda: measure_kernel_environments(
              _doc, {"wsl-linux": ["clock-realtime-steps"]},
              "C:\\r\\harness_legs.py", "C:\\r\\legs.json",
              runner=lambda a, t: (0, _sample, ""), translator=_xlate,
              clock=lambda: 0.0)),
          "a self-test that thinks it is driving a stepping clock and is actually "
          "measuring the real one passes for the wrong reason")
    _u8 = _measured(lambda a, t: (1, "", "python3: \u00e9chec de l'ex\u00e9cution"))

    def _report_for(measurement, run):
        """The report lines, or ONE visibly-wrong line naming the raise.

        \u26a0 WRAPPED FOR THE REASON THE `_gated` HELPER ABOVE IS, AND EXERCISING A
        MUTANT IS WHAT ASKED FOR IT: with `probe_kernel` broken to answer 'driver'
        for every verb, this fixture files under 'wsl-linux', nothing opens that
        drawer, and the leg's LOUD 'carries NO verdict' refusal \u2014 correct, and
        exactly what should happen \u2014 took the whole self-test down two checks
        early with no FAIL line of its own. A pin that can only red by crashing
        its runner reports nothing about which property broke."""
        try:
            gate = probe_gate(measurement, run)
            return confound_report_lines(
                "elf64-x86_64", leg_confound_decisions(_elf, gate), gate)
        except BaseException as exc:                            # noqa: BLE001
            return ["<RAISED %s: %s>" % (type(exc).__name__, exc)]
    check("a kernel that fails in UTF-8 stays REPORTABLE, not fatal",
          _u8["outcome"] == "unreachable"
          and all(ord(c) < 127 for c in _u8["why"])
          and all(ord(c) < 127 for l in _report_for({"wsl-linux": _u8},
                                                    _cross_kernel) for c in l),
          "a distro answering in its own locale would otherwise turn 'we could "
          "not measure that kernel' - recoverable, and the whole point of the "
          "unreachable outcome - into a raise at the report generator; got %r"
          % _u8["why"])
    # \u2605\u2605 THE SECOND LAYER, PINNED ON ITS OWN. measure_kernel_environments already
    # files INDETERMINATE for an unreachable kernel, so the gate's own force is
    # BELT AND BRACES and neither layer can be shown to bite through the other.
    # This fixture is one no honest producer emits \u2014 a real verdict beside a
    # failed outcome \u2014 which is the only way to hold the second layer to account.
    _liar = kernel_measurement(
        "wsl-linux", "unreachable", "the kernel was never reached",
        {"clock-realtime-steps": {"verdict": "present", "why": "self-test "
                                  "fixture: a producer that contradicts itself",
                                  "verb": "wall-clock-step", "evidence": {},
                                  "source": PROBE_SOURCE_MEASURED}})
    check("a PRESENT filed beside a FAILED outcome still decides nothing",
          _answer(lambda: probe_gate({"wsl-linux": _liar}, _cross_kernel)
                  ["verdicts"]["clock-realtime-steps"]["verdict"])
          == "indeterminate"
          and not ({"^walsetlk-", "^busy2-"} & _confounds_of(
              lambda: leg_confounds(
                  _elf, probe_gate({"wsl-linux": _liar}, _cross_kernel)))),
          "whoever wrote it, a verdict from an outcome that is not in force must "
          "not excuse a failing test")
    check("EVERY probe arm's evidence string is ASCII",
          all(ord(c) < 127 for w in _whys for c in w),
          "these strings land in the confound report, which is compared byte-for-"
          "byte between a bash arm and a PowerShell arm; got %r"
          % [w for w in _whys if any(ord(c) > 126 for c in w)])
    check("only PRESENT honours a conditional row", probe_verdict_honours("present"))
    check("ABSENT does not honour it", not probe_verdict_honours("absent"))
    check("INDETERMINATE does not honour it — it is not a maybe",
          not probe_verdict_honours("indeterminate"),
          "a row honoured on 'I could not measure' is honoured on nothing, which "
          "is the state `scope: any` was already in")
    check("an invented verdict RAISES rather than being read as present",
          _raises(lambda: probe_verdict_honours("probably")))
    # ── THE GATE, END TO END, ON THE REAL CATALOGUE ──────────────────────────
    _elf = [l for l in legs if l["label"] == "elf64-x86_64"][0]
    _absent = {nm: {"verdict": "absent", "why": "self-test fixture: healthy clock",
                    "verb": "wall-clock-step", "evidence": {},
                    "source": PROBE_SOURCE_MEASURED} for nm in _reg}
    _ind = {nm: {"verdict": "indeterminate", "why": "self-test fixture",
                 "verb": "wall-clock-step", "evidence": {},
                 "source": PROBE_SOURCE_MEASURED} for nm in _reg}
    _on = set(leg_confounds(_elf, _gate(_probes_present, _same_kernel)))
    _off = set(leg_confounds(_elf, _gate(_absent, _same_kernel)))
    check("with the clock defect PRESENT, the clock families are honoured",
          {"^walsetlk-", "^walsetlk_recover-", "^busy2-"} <= _on, "got %r" % _on)
    # ★★★ THE WHOLE POINT OF THIS CHANGE, IN ONE ASSERTION. On a healthy-clock box
    # — the arm64 VPS is the real one — those three patterns are NOT in force, so a
    # genuine walsetlk failure there is reported as GENUINE for the first time.
    check("with the clock HEALTHY, the clock families are NOT honoured",
          not ({"^walsetlk-", "^walsetlk_recover-", "^busy2-"} & _off),
          "this is the blind spot closing: at `scope: any` these excused a "
          "walsetlk failure on the arm64 VPS, where the clock has never been shown "
          "to step; got %r" % _off)
    check("...and INDETERMINATE behaves exactly like ABSENT here",
          set(leg_confounds(_elf, _gate(_ind, _same_kernel))) == _off)
    check("the UNCONDITIONAL rows survive a healthy clock",
          {"^zipfile-25\\.0$", "^date-2\\.4c$", "^recoverfault"} <= _off,
          "a row with `requires: []` rests on its own control and must not be "
          "gated on anything; got %r" % _off)
    # ★★★ THE KERNEL QUESTION, ALL FOUR DIRECTIONS, AS A DECISION AND NOT A
    # PRINTOUT.
    # [D-HARNESS-ENVIRONMENT-PROBE-MEASURES-THE-DRIVERS-KERNEL-NOT-THE-LAUNCHED-ONE]
    # ✔MEASURED at 0ecec160: `--host-os windows --host-arch x86_64
    # --launchers-available wsl.exe` with a `present` verdict printed the caveat
    # saying "rows go INACTIVE" and then `confound rows ACTIVE (7 of 7)`. The
    # scenario that made it dangerous: a Windows host whose OWN clock steps (VM
    # checkpoint, chrony `makestep`) driving the ELF legs through wsl.exe would
    # excuse every ^walsetlk-/^busy2- failure produced inside the WSL2 kernel,
    # including a genuine WAL blocking-lock miscompile.
    #
    # ★ (1) THE DANGEROUS DIRECTION, NOW STRUCTURAL. A `present` measured in the
    # DRIVER's kernel is filed under 'driver'; a leg executing in 'wsl-linux'
    # opens its own drawer, finds no verdict for a probe its rows require, and
    # REFUSES. Not "forced to indeterminate" — there is nothing to force, and a
    # verdict about another machine cannot arrive here at all.
    check("a verdict measured in the DRIVER's kernel cannot decide a leg that runs "
          "in another one - it is not in that leg's drawer, and the absence RAISES",
          _raises(lambda: leg_confounds(
              _elf, _gate(_probes_present, _cross_kernel, kernel="driver"))),
          "guessing either way is the hazard: 'present' silently excuses a real "
          "WAL blocking-lock miscompile produced in a kernel nobody measured, and "
          "'absent' hides a broken probe run behind plausible reds")
    # ★ (2) THE CAPABILITY THIS CYCLE ADDS, and the reason the row was reopened:
    # measured IN the launched kernel, the SAME verdict IS honoured there.
    _cross_measured = set(leg_confounds(_elf, _gate(_probes_present, _cross_kernel)))
    check("...and the same verdict MEASURED IN THE LAUNCHED KERNEL *is* honoured "
          "there",
          {"^walsetlk-", "^walsetlk_recover-", "^busy2-"} <= _cross_measured,
          "this is the withheld-excusal recovery: 4 walsetlk reds on elf64-x86_64 "
          "and 3 on elf64-arm64 were charged to DSS by a Windows-driven run at "
          "52cf784d while the arm64 VPS ran the same tests green from the same "
          "commit; got %r" % _cross_measured)
    check("...and the same verdict on a SAME-KERNEL leg IS honoured (not a blanket off)",
          {"^walsetlk-", "^walsetlk_recover-", "^busy2-"} <= _on,
          "a fix that turned the rows off everywhere would pass the direction above "
          "while removing the mechanism; got %r" % _on)
    check("...and a cross-kernel leg's UNCONDITIONAL rows are untouched",
          {"^zipfile-25\\.0$", "^date-2\\.4c$", "^recoverfault"}
          <= _cross_measured,
          "the kernel question is about MEASURED verdicts; a row that rests on its "
          "own earned control is not gated on any probe at all; got %r"
          % _cross_measured)
    # ★ (3) UNREACHABLE IS NOT ABSENT AND IS NOT PRESENT. The kernel was the right
    # one and it could not be asked, so the rows go INACTIVE and the report says
    # the measurement is MISSING rather than negative.
    _unreach = _gate(_probes_present, _cross_kernel, outcome="unreachable")
    check("a kernel that could not be ASKED honours nothing, whatever the file "
          "beside it says",
          not ({"^walsetlk-", "^walsetlk_recover-", "^busy2-"}
               & _confounds_of(lambda: leg_confounds(_elf, _unreach)))
          and _answer(lambda: _unreach["verdicts"]["clock-realtime-steps"]
                      ["verdict"]) == "indeterminate"
          and _unreach["appliesToThisLeg"] is False,
          "'we could not measure' must never become 'we measured absent' (a clean "
          "bill) nor 'present' (an excusal); got %r" % _unreach)
    _cross_gate = _gate(_probes_present, _cross_kernel)
    check("the gate NAMES the kernel it read, and how that answer was obtained",
          (_cross_gate["kernel"] == "wsl-linux"
           and _cross_gate["kernelOutcome"] == "entered"
           and _cross_gate["appliesToThisLeg"] is True
           and _gate(_probes_present, _same_kernel)["kernel"] == "driver"),
          "a verdict with no machine attached is this anchor's whole subject; got "
          "%r" % _cross_gate)
    check("`sharesDriverKernel` is read from the DECLARED table, and RAISES rather "
          "than defaulting to True",
          fs_shares_driver_kernel(run_filesystem_verb(_same_kernel)) is True
          and fs_shares_driver_kernel(run_filesystem_verb(_cross_kernel)) is False
          and _raises(lambda: run_filesystem_verb({"mode": "native"}))
          and _raises(lambda: run_filesystem_verb(None))
          and _raises(lambda: fs_shares_driver_kernel("somewhere-else")),
          "both lookups used to default to 'shares the kernel, apply the verdict, "
          "print no caveat' — the permissive direction, and a correctness bug now "
          "that this value decides whether a measurement applies")
    # ── M1: A `scope`d ROW IS SUPPLIED, AND THE ACCOUNT SAYS WHETHER IT CAN FIRE ─
    # ✔MEASURED at 0ecec160 (`--host-os linux --host-arch arm64 --launchers-none`,
    # run mode `native`): `emulated:^writecrash-` was reported ACTIVE with the
    # reason "unconditional (`requires: []`) ... nothing this harness measures per
    # run". Safe, and wrong twice: the matcher never applies an `emulated:` pattern
    # on a native run, so the row is neither unconditional nor in force.
    _arm = [l for l in legs if l["label"] == "elf64-arm64"][0]
    _arm_native = leg_confound_decisions(_arm, _gate(_absent, _same_kernel))
    _wc = [d for d in _arm_native if d["pattern"] == "^writecrash-"][0]
    check("a scope'd row on a NATIVE run is reported SCOPED OUT, never 'unconditional'",
          _wc["scopedOut"] is True and "unconditional" not in _wc["reason"]
          and "NOT IN FORCE HERE" in _wc["reason"],
          "the report is the account of why a failure was excused; a row the "
          "matcher cannot apply must not read as the widest kind of excusal. "
          "got %r" % _wc["reason"])
    check("...and it is STILL SUPPLIED to the matcher, which owns scope matching",
          _wc["active"] is True and _wc["wire"] == "emulated:^writecrash-",
          "dropping the pattern here would make the planner a second owner of "
          "scope matching — the D-HARNESS-CONFOUND-LEDGER-IS-PER-DRIVER-NOT-PER-"
          "LEG defect, one axis along; got %r" % _wc)
    _arm_launched = leg_confound_decisions(_arm,
                                          _gate(_absent, _launched_same))
    _wcl = [d for d in _arm_launched if d["pattern"] == "^writecrash-"][0]
    check("...and on a LAUNCHED run the same row reports IN FORCE",
          _wcl["scopedOut"] is False and "IN FORCE here" in _wcl["reason"],
          "a pin that only ever saw one direction would pass over a scope note "
          "that was hard-wired; got %r" % _wcl["reason"])
    check("an unscoped row carries NO scope clause either way",
          all("SCOPED" not in d["reason"] and d["scopedOut"] is False
              for d in _arm_native if not d["scope"]),
          "a note printed where it does not apply teaches a reader to skip it")
    check("a scope with no declared run modes RAISES rather than matching everything",
          _raises(lambda: confound_scope_run_modes("sometimes"))
          and confound_scope_run_modes("emulated") == ("launched",),
          "an unknown scope answered with 'every mode' would report a dead row as "
          "in force")
    check("every declared confound scope declares the run modes it can match",
          all(s in CONFOUND_SCOPE_RUN_MODES for s in CONFOUND_SCOPES)
          and all(m in RUN_MODES for ms in CONFOUND_SCOPE_RUN_MODES.values()
                  for m in ms),
          "a scope whose modes nobody declared cannot be reported either way: "
          "scopes=%r modes=%r" % (CONFOUND_SCOPES, CONFOUND_SCOPE_RUN_MODES))
    # ★★ ROBUST TO *HOW* THE GUARD IS REMOVED, and red-on-disable is what forced
    # that too. Deleting the `probe_verdicts is None` branch does not make this
    # answer WRONG — it makes the function CRASH (`'NoneType' has no attribute
    # 'get'`, ✔MEASURED), which took the whole self-test down and printed no FAIL
    # line at all. A pin that can only red by crashing its own runner reports
    # nothing about which property broke, so the raise is CAUGHT and turned into a
    # visibly wrong answer.
    def _gated(leg, verdicts):
        try:
            return set(leg_confounds(leg, _gate(verdicts, _same_kernel)))
        except BaseException as exc:                            # noqa: BLE001
            return {"<RAISED %s: %s>" % (type(exc).__name__, exc)}
    check("an UNPROBED plan honours no conditional row (the fail-safe direction)",
          _gated(_elf, None) == _off,
          "unprobed must behave as absent, never as present: the noisy direction "
          "gets investigated, the silent one hides a miscompile. got: %r"
          % sorted(_gated(_elf, None)))
    # A verdict that never arrived is a TRANSPORT defect, not a healthy machine.
    check("a MISSING verdict for a required probe RAISES",
          _raises(lambda: leg_confounds(_elf, _gate({}, _same_kernel))),
          "guessing 'present' would excuse a real miscompile in silence; guessing "
          "'absent' would hide a broken probe run behind plausible reds")
    check("a row with no `requires` key RAISES rather than defaulting",
          _raises(lambda: leg_confound_decisions(
              {"label": "x", "confounds": [{"pattern": "^a"}]},
              _gate(_absent, _same_kernel))))
    # ── THE VISIBLE ACCOUNT ─────────────────────────────────────────────────
    _dec = leg_confound_decisions(_elf, _gate(_absent, _same_kernel))
    check("EVERY declared row appears in the decision ledger, active or not",
          len(_dec) == len(_elf["confounds"]),
          "an inactive row that vanished would be indistinguishable from a row "
          "nobody declared")
    _rep = confound_report_lines("elf64-x86_64", _dec,
                                 _gate(_absent, _same_kernel))
    # ★★ THE ACCOUNT NAMES THE KERNEL, AND THE CAVEAT FIRES ON THE MEASUREMENT
    # HAVING FAILED — never on the launcher being foreign. Its old condition was
    # `sharesDriverKernel == False`, which was the right thing to say only while
    # the probe could not GO there; a cross-kernel leg that WAS measured in its
    # own kernel and still printed "NOT APPLIED" would be a fresh instance of the
    # claim-rot this anchor exists to remove.
    # [D-HARNESS-ENVIRONMENT-PROBE-MEASURES-THE-DRIVERS-KERNEL-NOT-THE-LAUNCHED-ONE]
    _cross_dec = leg_confound_decisions(_elf, _cross_gate)
    _cross_rep = confound_report_lines("elf64-x86_64", _cross_dec, _cross_gate)
    check("the report SAYS WHICH KERNEL the verdicts below are about",
          any("executes in kernel 'wsl-linux'" in l and "runFilesystem "
              "'wsl-linux'" in l for l in _cross_rep)
          and any("executes in kernel 'driver'" in l for l in _rep),
          "a verdict printed with no machine attached is what let ABSENT-on-"
          "Windows read as a fact about a fixture running in WSL2; got %r"
          % _cross_rep)
    check("...and a leg MEASURED IN ITS OWN KERNEL carries NO not-applied caveat",
          not any("CAVEAT" in l for l in _cross_rep)
          and any("confound rows ACTIVE (7 of 7)" in l for l in _cross_rep),
          "the measurement came from the right place, so the old caveat would now "
          "be false in the other direction - and it would sit one line above "
          "`ACTIVE (7 of 7)`, which is exactly the pairing V1 shipped; got %r"
          % _cross_rep)
    # THE OTHER HALF: the kernel was right and could not be asked.
    _un_dec = leg_confound_decisions(_elf, _unreach)
    _un_rep = confound_report_lines("elf64-x86_64", _un_dec, _unreach)
    check("a kernel that could not be MEASURED prints the caveat, and it is TRUE",
          any("CAVEAT: NOT MEASURED IN KERNEL 'wsl-linux'" in l
              for l in _un_rep)
          and any("confound rows ACTIVE (4 of 7)" in l for l in _un_rep)
          and sum(1 for l in _un_rep
                  if "INACTIVE" in l and "confound row" in l) == 3,
          "a caveat that contradicts the decision beside it is worse than none; "
          "got %r" % _un_rep)
    check("...and it says the ABSENCE of a measurement, never a measured absence",
          any("absence of a measurement, never a measurement of absence" in l
              for l in _un_rep)
          and not any("= ABSENT" in l for l in _un_rep),
          "reporting ABSENT for a kernel nobody reached is a clean bill nobody "
          "earned; got %r" % _un_rep)
    check("...and a leg whose own kernel answered does NOT carry the caveat",
          not any("CAVEAT" in l
                  for l in confound_report_lines(
                      "elf64-x86_64",
                      leg_confound_decisions(
                          _elf, _gate(_absent, _launched_same)),
                      _gate(_absent, _launched_same))),
          "a caveat printed where it does not apply teaches a reader to skip it")
    check("the report names the probe, its VERDICT and its measured evidence",
          any("clock-realtime-steps = ABSENT" in l and "healthy clock" in l
              for l in _rep), "got %r" % _rep)
    check("the report names every INACTIVE row and says a match will be GENUINE",
          sum(1 for l in _rep if "INACTIVE" in l) == 3
          and all("GENUINE" in l for l in _rep if "INACTIVE" in l),
          "a run whose report cannot say why a failure was excused has not earned "
          "the exclusion; got %r" % _rep)
    check("the report is ASCII, and non-ASCII RAISES at the generator",
          all(ord(c) < 127 for l in _rep for c in l)
          and _raises(lambda: confound_report_lines(
              "x", [{"pattern": "^a", "wire": "^a", "scope": "", "requires": [],
                     "active": False, "scopedOut": False,
                     "reason": "an em dash — here"}],
              _gate(None, _same_kernel))),
          "this text is compared byte-for-byte between a bash arm and a PowerShell "
          "arm; a non-ASCII character makes the twin-parity proof a test of two "
          "codepages")
    # ── H1/M3: THE INJECTION DOOR IS VALIDATED, AND IT IS LOUD ──────────────
    # [D-HARNESS-PROBE-VERDICTS-FLAG-INJECTS-AN-UNVALIDATED-PRESENT]
    # ✔MEASURED at 0ecec160: a hand-written `{"verdict":"present","why":"I said
    # so","evidence":{}}` yielded gating=probed, 7 of 7 rows ACTIVE and a report
    # line indistinguishable from a measurement; and two malformed shapes raised
    # python's own ValueError / KeyError from three frames deeper.
    def _inject(obj):
        return validate_probe_verdicts(obj, _doc, "<self-test>")

    def _at(kernel, entry):
        """The injected file's shape: a KERNEL naming the verdicts measured in
        it. Spelled through a helper so every clause below still reads as a
        statement about ONE verdict entry."""
        return {kernel: {"clock-realtime-steps": entry}}
    _entry = {"verdict": "present", "why": "captured elsewhere",
              "verb": "wall-clock-step", "evidence": {}}
    _good = _at("driver", _entry)
    check("a well-formed injected map is ACCEPTED and stamped `injected`",
          _inject(_good)["driver"]["verdicts"]["clock-realtime-steps"]["source"]
          == PROBE_SOURCE_INJECTED
          and _inject(_good)["driver"]["outcome"] == "injected")
    check("...and a file claiming `source: measured` cannot launder itself",
          _inject(_at("driver", dict(_entry, source=PROBE_SOURCE_MEASURED)))
          ["driver"]["verdicts"]["clock-realtime-steps"]["source"]
          == PROBE_SOURCE_INJECTED,
          "the stamp is applied by the door, never read from the file")
    # ★★★ THE FLAT SHAPE IS REFUSED BY NAME, NOT READ AS THE DRIVER'S. A file with
    # no kernel on it is a verdict about an unnamed machine, and reading it as
    # this one is precisely how a Windows measurement came to decide a fixture
    # running in WSL2.
    # [D-HARNESS-ENVIRONMENT-PROBE-MEASURES-THE-DRIVERS-KERNEL-NOT-THE-LAUNCHED-ONE]
    _flat_why = ""
    try:
        _inject({"clock-realtime-steps": _entry})
    except LegError as _exc:
        _flat_why = str(_exc)
    check("the OLD FLAT shape is refused, and the refusal says how to spell it",
          "keyed on PROBE NAMES" in _flat_why and "wsl-linux" in _flat_why,
          "silently reading it as the driver's kernel would restore the defect by "
          "flag; got %r" % _flat_why)
    check("a verdict filed under a kernel NO leg resolves to is refused",
          _raises(lambda: _inject(_at("some-other-box", _entry))),
          "nobody opens that drawer, so those verdicts would decide nothing - "
          "silently, which is the direction that hides an unapplied measurement")
    check("a BARE STRING verdict is a named refusal, not a ValueError",
          _refuses_namedly(lambda: _inject(_at("driver", "present"))))
    check("a verdict object with no `verdict` key is a named refusal, not a KeyError",
          _refuses_namedly(lambda: _inject(_at("driver", {"why": "x"}))))
    check("an UNDECLARED probe name is refused",
          _raises(lambda: _inject({"driver": {"clock-goes-backwards": _entry}})))
    check("an invented verdict word is refused",
          _raises(lambda: _inject(_at("driver", dict(_entry,
                                                     verdict="probably")))))
    check("a verdict naming ANOTHER probe's verb is refused",
          _raises(lambda: _inject(_at("driver", dict(_entry,
                                                     verb="guess-the-clock")))),
          "a replayed verdict from a different procedure may not decide an excusal")
    check("an EMPTY `why` is refused (a verdict with no evidence is `earnedOn`)",
          _raises(lambda: _inject(_at("driver", dict(_entry, why="   ")))))
    check("non-object `evidence` is refused",
          _raises(lambda: _inject(_at("driver", dict(_entry, evidence="lots")))))
    check("a verdict with NO `source` RAISES rather than being read as measured",
          _raises(lambda: probe_verdict_source({"verdict": "present"})),
          "reading it as measured is the permissive direction; reading it as "
          "injected would slander a real measurement")
    # ★★ AND THE INJECTION IS VISIBLE, AND CANNOT RUN A CORPUS.
    _inj_gate = probe_gate(_inject(_good), _same_kernel)
    _inj_rep = confound_report_lines(
        "elf64-x86_64", leg_confound_decisions(_elf, _inj_gate), _inj_gate)
    check("an injected verdict is HONOURED for resolution",
          {"^walsetlk-", "^busy2-"}
          <= set(leg_confounds(_elf, _inj_gate)),
          "the flag exists so a caller and the pins can drive the honouring path "
          "without sampling a clock for 20 s")
    check("...and EVERY report line derived from it says INJECTED, in words",
          all("INJECTED by --probe-verdicts" in l for l in _inj_rep
              if "environment probe clock-realtime-steps" in l),
          "nothing distinguished an injected verdict from a measurement; got %r"
          % _inj_rep)
    check("...and the plan's gating is `injected`, which NO driver accepts",
          _inj_gate["gating"] == "injected"
          and CONFOUND_GATINGS["injected"]["usable"] is False,
          "a verdict file captured on the WSL2 box and replayed on the arm64 VPS "
          "must STOP the driver, not silently restore the blind spot this gate "
          "closed")
    check("exactly ONE gating is usable, and it is spelled `probed`",
          [g for g in sorted(CONFOUND_GATINGS)
           if CONFOUND_GATINGS[g]["usable"]] == ["probed"],
          "both drivers spell the test as `== 'probed'`, so a second usable gating "
          "added here would let a corpus run on gating neither driver checked")
    _measured_driver = {"driver": kernel_measurement(
        "driver", "in-process", "self-test fixture", _absent)}
    # ⓘ WRAPPED IN `_answer` BECAUSE THE FIXTURE ITSELF CAN RAISE, and a check
    # whose fixture construction aborts the runner reports nothing about the
    # property it was written for. ✔MEASURED with `probe_kernel` mutated to
    # answer 'driver' for every verb: `_inject(_at("wsl-linux", …))` is then
    # CORRECTLY refused ("not a kernel any declared runFilesystem resolves to")
    # and that correct refusal took the self-test down 13 FAIL lines in, with no
    # `passed=`/`failed=` summary at all.
    check("gating is derived from the verdicts' own SOURCE, not from which flag ran",
          _answer(lambda: (
              confound_gating(None) == "unprobed"
              and confound_gating(_measured_driver) == "probed"
              and confound_gating(_inject(_good)) == "injected"
              # ONE injected verdict in ONE kernel taints the whole plan: a
              # majority vote — or a per-kernel gating — would be the quiet door
              # again.
              # ⓘ THE INJECTED HALF IS INJECTED *AS* wsl-linux, not the driver's
              # measurement re-filed under that key. It used to be the latter,
              # which is now REFUSED: a measurement whose own `kernel` field
              # disagrees with the drawer it sits in describes a machine nobody
              # asked about, and the fixture was quietly asserting the
              # mixed-source rule on a shape no producer can emit. See
              # _measurement_filed_as.
              and confound_gating(
                  {"driver": _measured_driver["driver"],
                   "wsl-linux": _inject(_at("wsl-linux", _entry))["wsl-linux"]})
              == "injected")) is True,
          "unprobed / probed / injected, and ONE injected verdict anywhere in the "
          "map taints the whole plan's gating")
    # ★★ AND THE KEY AND THE FIELD ARE ONE CLAIM: DISAGREEING IS REFUSED.
    # ✔MEASURED before this: a measurement self-labelled 'wsl-linux' filed under
    # the map key 'driver' was ACCEPTED AND HONOURED (gate.kernel='driver'
    # applies=True verdict='present') — the WSL2 kernel's answer deciding a leg
    # that executes on this driver's machine. `kernel` is REQUIRED by
    # KERNEL_MEASUREMENT_KEYS and was read only inside an error string; a declared
    # field whose only consumer prints it decides nothing at all.
    # [D-HARNESS-ENVIRONMENT-PROBE-MEASURES-THE-DRIVERS-KERNEL-NOT-THE-LAUNCHED-ONE]
    _mislabelled = {"driver": kernel_measurement(
        "wsl-linux", "in-process", "self-test fixture: a mislabelled drawer",
        _probes_present)}
    check("a measurement whose `kernel` disagrees with the drawer it sits in is "
          "REFUSED, at the gate and at the gating",
          _raises(lambda: probe_gate(_mislabelled, _same_kernel))
          and _raises(lambda: confound_gating(_mislabelled)),
          "believing the KEY applies one machine's answer to another's legs and "
          "believing the FIELD files it where no leg looks; neither may be "
          "preferred, so the disagreement itself is the refusal")
    check("...and the AGREEING measurement it was built from is accepted",
          probe_gate({"wsl-linux": kernel_measurement(
              "wsl-linux", "in-process", "self-test fixture", _probes_present)},
              _cross_kernel)["appliesToThisLeg"] is True,
          "a guard that refused the coherent case too would be refusing the "
          "wrapper, not the disagreement")
    # ★ AN UNREACHABLE KERNEL IS STILL `probed`, AND THAT IS A DECISION, NOT AN
    # OVERSIGHT: this plan DID measure in the declared way and the answer is
    # INDETERMINATE, which honours nothing. Stamping it `unprobed` would make both
    # drivers REFUSE TO RUN on a host whose distro has no python3 — turning "no
    # excusals available here" into "no corpus today".
    check("a kernel that could not be reached is still `probed`, never `unprobed`",
          confound_gating({"wsl-linux": kernel_measurement(
              "wsl-linux", "unreachable", "no python3 in the distro",
              _ind)}) == "probed"
          and CONFOUND_GATINGS["unprobed"]["usable"] is False,
          "fail toward REPORTING: the run continues and every conditional row is "
          "INACTIVE, which is the safe direction and is visible in the report")
    # ── THE CONFIG FLOORS ───────────────────────────────────────────────────
    # A config that could be loosened without limit is a guard that gets re-cut to
    # fit each new case. The lint refuses it; asserted here on a real catalogue
    # copy so the refusal is exercised and not merely present.
    def _lint_with_probe_config(**over):
        import tempfile as _t
        d = json.loads(json.dumps(_doc))
        d["environmentProbes"]["clock-realtime-steps"]["config"].update(over)
        fd, p2 = _t.mkstemp(suffix=".json"); os.close(fd)
        try:
            with open(p2, "w", encoding="utf-8") as fh:
                json.dump(d, fh)
            return [f for f in lint(p2) if "environmentProbes" in f]
        finally:
            os.unlink(p2)
    check("the shipped probe config clears every floor",
          not _lint_with_probe_config(), "%r" % _lint_with_probe_config())
    check("a 5 s sample window is REFUSED (the floor is 15 s)",
          any("sampleSeconds" in f for f in _lint_with_probe_config(sampleSeconds=5)))
    check("requiring only ONE step is REFUSED (never a single pair of readings)",
          any("minStepsRequired" in f
              for f in _lint_with_probe_config(minStepsRequired=1)))
    check("a 10 ms step threshold is REFUSED (scheduler noise would fire it)",
          any("minStepSeconds" in f
              for f in _lint_with_probe_config(minStepSeconds=0.01)))
    check("a TIGHTER config is accepted — config may raise, never lower",
          not _lint_with_probe_config(sampleSeconds=60, minStepsRequired=4,
                                      minStepSeconds=10))
    check("an unknown config key is REFUSED, not silently ignored",
          any("unknown config key" in f
              for f in _lint_with_probe_config(minStepSecs=5)))
    check("an unknown probe VERB raises rather than defaulting",
          _raises(lambda: probe_verb("guess-the-clock")))
    def _lint_with_mutated_catalogue(mutate, needle):
        """Lint a COPY of the shipped catalogue with one thing changed. The copy is
        the real file, so a finding here is the real refusal firing on real data —
        not a hand-built fixture that only resembles it."""
        import tempfile as _t
        d = json.loads(json.dumps(_doc))
        mutate(d)
        fd, p2 = _t.mkstemp(suffix=".json"); os.close(fd)
        try:
            with open(p2, "w", encoding="utf-8") as fh:
                json.dump(d, fh)
            return [f for f in lint(p2) if needle in f]
        finally:
            os.unlink(p2)

    def _bogus_requires(d):
        d["legs"][0]["confounds"][0]["requires"] = ["clock-goes-backwards"]
    check("a `requires` naming an undeclared probe is a lint finding",
          _lint_with_mutated_catalogue(_bogus_requires, "clock-goes-backwards"),
          "an undeclared probe cannot be measured, so the row would be honoured "
          "on nothing — the exact state `scope: any` was in")

    def _drop_requires(d):
        del d["legs"][0]["confounds"][0]["requires"]
    check("a row with NO `requires` key is a lint finding, not a default",
          _lint_with_mutated_catalogue(_drop_requires, "declares no `requires`"))

    def _readd_any(d):
        d["legs"][0]["confounds"][0]["scope"] = "any"
    check("re-adding `scope: any` is a lint finding",
          _lint_with_mutated_catalogue(_readd_any, "RETIRED"))

    def _scope_without_blocker(d):
        for r in d["legs"][1]["confounds"]:
            if r["pattern"] == "^writecrash-":
                del r["scopeLegacyBlocker"]
    check("a legacy `scope` row that names no blocker is a lint finding",
          _lint_with_mutated_catalogue(_scope_without_blocker,
                                       "scopeLegacyBlocker"),
          "without this the axis becomes an inert alternative the next row reaches "
          "for, which is how a proxy gets re-cut to fit each new case")
    # ── THE MIGRATION OFF `scope`, ASSERTED SO IT CANNOT SILENTLY REGROW ─────
    _scoped = [(l["label"], r["pattern"]) for l in legs for r in l["confounds"]
               if "scope" in r]
    check("only the two rows with a NAMED blocker remain on the legacy `scope` axis",
          sorted(_scoped) == [("elf64-arm64", "^writecrash-"),
                              ("pe64-x86_64", "^win32longpath-1\\.3$")],
          "the axis retires when the last row leaves it. A NEW row on `scope` is "
          "either a migration that was not done or a proxy being reached for; got %r"
          % sorted(_scoped))
    check("every surviving `scope` row NAMES what blocks its migration",
          all(r.get("scopeLegacyBlocker", "").strip()
              for l in legs for r in l["confounds"] if "scope" in r))
    check("no row anywhere still spells the retired `scope: any`",
          not any(r.get("scope") == "any"
                  for l in legs for r in l["confounds"]))

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

    # ── the staged sqlite CONFIGURE header plan ──────────────────────────────
    # The twin of the block above, and HOST-INVARIANT for the same reason: which
    # ./configure answers a leg compiles against is a fact about its TARGET. The
    # defect this replaces was precisely a host fact reaching a target: the
    # deriving Linux box's `sqlite_cfg.h`, applied to Darwin.
    cfg_stages = configure_stages(legs)
    check("one staged configure header per distinct TARGET OS",
          sorted(cfg_stages) == sorted({spec_target_os(leg["spec"]) for leg in legs}),
          "stages=%r targetOS=%r" % (sorted(cfg_stages),
                                     sorted({spec_target_os(leg["spec"]) for leg in legs})))
    check("more than one configure stage is declared", len(cfg_stages) > 1,
          "only %r — with a single stage this whole mechanism is untested by the "
          "catalogue it ships with" % sorted(cfg_stages))
    # ★ THE STAGE THAT MADE THE CASE. The four `recipeTransform: "none"` legs are
    # TWO different configurations, and a key that could not tell them apart is
    # what shipped Linux answers to Darwin. Asserted rather than assumed, because
    # the moment it stops being true this whole family is decorative.
    check("the transform key CANNOT stand in for the configure key",
          len({configure_stage_key(l) for l in legs
               if header_stage_key(l) == "none"}) > 1,
          "every 'none'-transform leg resolves to one configure stage — if that "
          "is really so, this second stage family has no reason to exist")
    for host in SELF_TEST_HOSTS:
        resolved = plan(host[0], host[1], set(), path)
        for leg in resolved["legs"]:
            key = leg["build"]["configStageKey"]
            check("configure stage key is host-invariant on %s/%s for %s"
                  % (host[0], host[1], leg["label"]),
                  key == spec_target_os(leg["spec"]),
                  "key=%r targetOS=%r" % (key, spec_target_os(leg["spec"])))
            check("the leg's configure stage exists in the stage plan (%s/%s %s)"
                  % (host[0], host[1], leg["label"]), key in cfg_stages)
            check("the leg's declared answers ARE its stage's answers (%s/%s %s)"
                  % (host[0], host[1], leg["label"]),
                  cfg_stages.get(key) == leg["build"].get("configureAnswers"),
                  "stage=%r leg=%r" % (cfg_stages.get(key),
                                       leg["build"].get("configureAnswers")))
    # Every declared answer is in the closed vocabulary, is a real JSON boolean,
    # and AGREES with what the target derivation says — the same three properties
    # the lint checks, asserted here so a catalogue edit cannot ship without them
    # even if someone runs the drivers without `--lint`.
    for leg in legs:
        answers = configure_answers(leg)
        target_os = spec_target_os(leg["spec"])
        check("leg '%s' declares every configure answer, and only known ones"
              % leg["label"],
              sorted(answers) == sorted(CONFIGURE_ANSWER_NAMES),
              "declared %r, vocabulary %r" % (sorted(answers),
                                              sorted(CONFIGURE_ANSWER_NAMES)))
        for name, value in sorted(answers.items()):
            check("leg '%s': %s is a JSON boolean" % (leg["label"], name),
                  isinstance(value, bool), "got %r" % (value,))
            check("leg '%s': %s agrees with target OS '%s'"
                  % (leg["label"], name, target_os),
                  value is configure_answer_for_target_os(name, target_os),
                  "declared %r, derivation says %r"
                  % (value, configure_answer_for_target_os(name, target_os)))
    # A conflict must RAISE rather than pick a winner: two legs on one target OS
    # share one staged header, and "either one" is how a leg ends up compiled
    # against a configuration nobody declared for it.
    _clash = json.loads(json.dumps(legs))
    for _l in _clash:
        if spec_target_os(_l["spec"]) == "darwin":
            _l["build"]["configureAnswers"] = dict(
                _l["build"]["configureAnswers"], HAVE_MALLOC_H=True)
            break
    check("two legs on one target OS with different answers RAISE",
          _raises(lambda: configure_stages(_clash)))
    # And a spec whose OS cannot be derived is refused outright — never keyed to
    # "" or "unknown", which a driver would join onto its cfg/ root as a path.
    check("a leg whose spec names no known OS is refused a configure stage key",
          _raises(lambda: configure_stage_key({"label": "x", "spec": "x86_64:weird"})))

    # The sh emitter must round-trip every leg, and must emit assignments ONLY —
    # build-and-test.sh `eval`s this text, so a line that is not an assignment is
    # a command it would execute.
    sh = emit_sh(plan("linux", "x86_64", every, path))
    check("the sh emitter names every leg", all(lbl in sh for lbl in labels))
    statements = sh_statements(sh)
    check("the sh emitter emitted one statement per leg field",
          len(statements) == 1 + len(labels) * 37,
          "got %d statements for %d legs" % (len(statements), len(labels)))
    # [D-HARNESS-RUN-FIDELITY-IS-COMPUTED-BUT-NEITHER-RECORDED-NOR-SELECTABLE]
    # ★★ THE TRANSPORT IS THE WHOLE RISK HERE. The .ps1 reads `run.fidelity` out
    # of the JSON plan and needs nothing; the .sh reads ONLY these flattened
    # arrays, so a fidelity that never reached them would leave DSS_RUN_FIDELITY
    # honoured by one driver and silently ignored by the other — a capability in
    # one driver and not the other is this project's canonical silent harness bug.
    check("the sh emitter carries THIS LEG'S run FIDELITY, so the .sh's selector "
          "reads the same fact the .ps1 reads out of the JSON plan",
          "LEG_RUN_FIDELITY[" in sh and "LEG_RUN_SAME_ISA[" in sh)
    check("...and it is the RESOLVED value, not a placeholder",
          any(s.startswith("LEG_RUN_FIDELITY[") and s.rstrip().endswith("native")
              for s in statements),
          "no leg carried a resolved fidelity: %r"
          % [s for s in statements if s.startswith("LEG_RUN_FIDELITY[")])
    check("the sh emitter carries the launcher's run FILESYSTEM",
          "LEG_RUN_FILESYSTEM[" in sh,
          "without it build-and-test.sh cannot tell a launcher that shares this "
          "filesystem from one that reaches it through a compatibility mount, "
          "which is D-HARNESS-WSL-LAUNCHED-LEG-RUNDIR-IS-DRVFS")
    check("the sh emitter carries THIS LEG'S earned confounds",
          "LEG_CONFOUNDS[" in sh,
          "without it build-and-test.sh falls back to a global list applied to "
          "every leg, which is D-HARNESS-CONFOUND-LEDGER-IS-PER-DRIVER-NOT-PER-LEG")
    check("the sh emitter carries THIS LEG'S earned ABORT confounds",
          "LEG_ABORT_CONFOUNDS[" in sh,
          "without it the .sh cannot tell a PROVEN-upstream abort from an "
          "unproven one and convicts the compiler of both, which is "
          "D-HARNESS-ABORT-HAS-NO-EARNED-CONFOUND-VOCABULARY")
    check("the sh emitter carries the leg's staged configure header",
          "LEG_CONFIG_STAGE_KEY[" in sh and "LEG_CONFIGURE_ANSWERS[" in sh,
          "without it build-and-test.sh has no way to give a leg the sqlite_cfg.h "
          "staged for its own target, which is "
          "D-HARNESS-MACHO-LEG-INHERITS-THE-DERIVING-LINUX-HOSTS-CONFIGURE-PROBES")
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
    #
    # ★ THE SUBJECT IS SYNTHETIC, and that is the point. Every leg in the
    # catalogue now acquires (TF-C123 converted the last three), so drawing this
    # leg from the catalogue made the assertion vanish the moment the gap it
    # guards was closed — the shape of a check that quietly stops testing
    # anything. A rule about "a leg with no route" is stated with a leg that has
    # no route, whether or not one is currently shipped.
    no_route = {"label": "synthetic-no-route",
                "spec": "x86_64:elf64-x86_64-linux-exec",
                "build": {"libraries": {"provider": "host-system",
                                        "tclNames": [], "zNames": [],
                                        "searchPaths": []}}}
    check("a non-acquiring leg declares no archives",
          not acquire_plan(no_route, root)["archives"])
    check("a non-acquiring leg stages no script library",
          acquire_plan(no_route, root)["scriptLibraryDir"] == "",
          "a driver would export TCL_LIBRARY pointing at nothing")
    tcl_i, z_i = acquired_import_names(no_route)
    check("a non-acquiring leg overrides no identity",
          not tcl_i and not z_i,
          "a host-supplied library already carries the right one")

    # ── A LIBRARY IS NOT ALWAYS SELF-CONTAINED ──────────────────────────────
    # D-HARNESS-ACQUIRED-TCL-DYLIB-HAS-NO-SCRIPT-LIBRARY. The dylib linked, the
    # binary ran, one `.test` file passed — and the TIER driver died at
    # `interp create` because acquisition had obtained Tcl's CODE and not its
    # SCRIPT LIBRARY. Everything below is pure (plan-level or byte-level), so it
    # holds on a gate machine with no network at all.
    for leg in acquiring:
        lbl = leg["label"]
        ap = acquire_plan(leg, root)
        members = [m for a in ap["archives"] for m in a["members"]]
        check("every acquired member says WHICH COPY RUNS (%s)" % lbl,
              all(m["runtimeCopy"] in RUNTIME_COPIES for m in members),
              "%r — without it, nothing decides whether a baked-in data "
              "directory has to be staged"
              % [(m["as"], m["runtimeCopy"]) for m in members])
        # ★ THE ONE THAT WOULD HAVE CAUGHT IT. A member that declares a
        # `runtime-data` path and stages no directory for it is the exact state
        # the macho legs shipped in.
        for m in members:
            for e in m["embeddedPaths"]:
                if e["kind"] != "runtime-data":
                    continue
                check("a baked-in runtime data directory is STAGED (%s :: %s)"
                      % (lbl, m["as"]),
                      any(d["role"] == e["role"] for d in m["dataDirs"]),
                      "%s is declared runtime-data with role %r and no dataDirs "
                      "entry provides it — the library would look for it at the "
                      "PACKAGER's prefix on the target machine"
                      % (e["path"], e["role"]))
        check("a staged data directory lands inside its own leg's cache (%s)" % lbl,
              all(d["path"].startswith(ap["cacheDir"])
                  for m in members for d in m["dataDirs"]),
              "two legs sharing one staged tree makes 'which leg staged this?' "
              "unanswerable")
    # Every leg that stages a Tcl script library must SAY where it is, and the
    # value must be one of the directories actually planned — a driver sets
    # TCL_LIBRARY from this and from nothing else.
    for leg in acquiring:
        ap = acquire_plan(leg, root)
        staged = [d["path"] for a in ap["archives"] for m in a["members"]
                  for d in m["dataDirs"] if d["role"] == "tclScriptLibrary"]
        check("scriptLibraryDir names a directory the plan actually stages (%s)"
              % leg["label"],
              ap["scriptLibraryDir"] in staged if staged
              else ap["scriptLibraryDir"] == "",
              "plan says %r, staged %r" % (ap["scriptLibraryDir"], staged))
        check("every leg with an acquired Tcl stages its SCRIPT LIBRARY (%s)"
              % leg["label"],
              bool(staged),
              "the acquired Tcl would hunt for init.tcl at the packager's "
              "prefix and die at `interp create` — one interpreter later than "
              "anything a build could see")
    # THE FAILURE RETURN CARRIES THE SAME FIELDS AS THE SUCCESS RETURN.
    # D-HARNESS-PINNED-ARCHIVE-FAILURE-RETURN-OMITS-ACQUIRED, closed once as an
    # instance and stated here as the rule: one record type, both paths.
    if acquiring:
        ap = acquire_plan(acquiring[0], root)
        ok = acquisition_record(ap, libraries=[{"as": "x"}], from_cache=True)
        bad = acquisition_record(ap, error="boom")
        check("the acquisition record has ONE shape on success and failure",
              set(ok) == set(bad),
              "success-only keys %r · failure-only keys %r"
              % (sorted(set(ok) - set(bad)), sorted(set(bad) - set(ok))))
        check("the FAILURE record still carries scriptLibraryDir",
              bad["scriptLibraryDir"] == ap["scriptLibraryDir"] != "",
              "a driver needs it most when acquisition just failed and it is "
              "reporting why")
        check("the failure record says what failed", bad["error"] == "boom")
        check("a success record carries no error", ok["error"] == "")

    # ── The embedded-path guard, on SYNTHESISED bytes ───────────────────────
    # The rule is about BYTES, so it is tested against bytes rather than against
    # a declaration — a guard asserted only through its own declaration is a
    # guard that passes because it was told to.
    _probe_blob = b"\x00stuff\x00/pkg/prefix/lib/tcl8.6\x00/pkg/prefix/bin\x00"
    check("the scanner finds an absolute path baked into a library",
          embedded_absolute_paths(_probe_blob)
          == ["/pkg/prefix/bin", "/pkg/prefix/lib/tcl8.6"])
    check("the scanner ignores a fragment that is not an absolute path",
          embedded_absolute_paths(b"\x00relative/dir/thing\x00/one\x00") == [],
          "a one-component path and a mid-word slash are noise, and a guard "
          "that flags noise is a guard that gets switched off")

    def _member(**over):
        m = {"as": "libx.so", "runtimeCopy": "staged-beside-artefact",
             "embeddedPaths": [], "dataDirs": []}
        m.update(over)
        return m

    check("an UNDECLARED baked-in path is a refusal",
          _raises(lambda: _audit_embedded_paths("L", _member(), _probe_blob)),
          "this is the whole guard: a data directory nobody declared is a "
          "runtime death nothing at build time can see")
    check("a declared path that is NOT in the bytes is a refusal",
          _raises(lambda: _audit_embedded_paths("L", _member(embeddedPaths=[
              {"path": "/pkg/prefix/lib/tcl8.6", "kind": "runtime-data",
               "role": "tclScriptLibrary"},
              {"path": "/pkg/prefix/bin", "kind": "inert", "role": ""},
              {"path": "/pkg/prefix/share/icu", "kind": "inert", "role": ""}],
              dataDirs=[{"as": "tcl8.6", "role": "tclScriptLibrary"}]),
              _probe_blob)),
          "a path list nobody checks against the file is how this guard would "
          "come to pass for the wrong reason")
    check("a declared `runtime-data` path with NOTHING STAGED is a refusal",
          _raises(lambda: _audit_embedded_paths("L", _member(embeddedPaths=[
              {"path": "/pkg/prefix/lib/tcl8.6", "kind": "runtime-data",
               "role": "tclScriptLibrary"},
              {"path": "/pkg/prefix/bin", "kind": "inert", "role": ""}]),
              _probe_blob)),
          "EXACTLY the state the macho legs shipped in: the path was declared "
          "by the packager, and nothing staged it")
    check("a fully declared and staged member passes",
          _audit_embedded_paths("L", _member(embeddedPaths=[
              {"path": "/pkg/prefix/lib/tcl8.6", "kind": "runtime-data",
               "role": "tclScriptLibrary"},
              {"path": "/pkg/prefix/bin", "kind": "inert", "role": ""}],
              dataDirs=[{"as": "tcl8.6", "role": "tclScriptLibrary"}]),
              _probe_blob) is None)
    check("the audit does NOT apply where the target supplies its own copy",
          _audit_embedded_paths(
              "L", _member(runtimeCopy="target-supplies-its-own"),
              _probe_blob) is None,
          "our file is never loaded there; staging its data would ship the "
          "wrong data with a straight face")
    # A Mach-O's own load commands are the loader's business, not data. The
    # image is SYNTHESISED here (one LC_ID_DYLIB, nothing else) so the assertion
    # does not depend on any file being present on the machine running it.
    _id_name = b"/opt/x/lib/libz.1.dylib\0"
    _id_name += b"\0" * (-(24 + len(_id_name)) % 8)
    _lc_id = struct.pack("<IIIIII", LC_ID_DYLIB, 24 + len(_id_name), 24,
                         0, 0, 0) + _id_name
    _macho = (struct.pack("<IIIIIII", MH_MAGIC_64, 0x0100000C, 0, MH_DYLIB,
                          1, len(_lc_id), 0) + b"\0" * 4 + _lc_id)
    check("a Mach-O's LC_ID_DYLIB is excused from the data audit",
          "/opt/x/lib/libz.1.dylib" in embedded_absolute_paths(_macho)
          and "/opt/x/lib/libz.1.dylib" in loader_dependency_paths(_macho)
          and _audit_embedded_paths("L", _member(), _macho) is None,
          "it is IN the bytes, it IS reported as a loader dependency, and it "
          "must not have to be declared as a data directory")

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
              _raises(lambda: acquire(no_route, root, offline=True,
                                      downloader=_forbidden_downloader)))

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

    # ── WHO BUILDS IT: DSS, on any host; the reference compiler as a CONTROL ──
    # D-HARNESS-CROSS-HOST-ANY-TARGET. Every assertion below is on the PURE parts
    # (the argv, the format derivation, the header reader, the builder verb), so
    # the whole rule is provable on a machine with no compiler of any kind. The
    # end-to-end evidence — five formats emitted from ONE Windows host, three of
    # them LOADED through sqlite's own sqlite3_load_extension() — is recorded in
    # the section header above; a self-test that shelled out to DSS would be
    # asserting the compiler's availability, not this module's rules.
    check("the shared-library kind is keyed on the CONTAINER, one per container",
          sorted(SHARED_LIB_KIND_BY_CONTAINER.items())
          == [("elf64", "dyn"), ("macho64", "dylib"), ("pe64", "dll")])
    for _spec, _want in [
        ("x86_64:elf64-x86_64-linux-exec", "elf64-x86_64-linux-dyn"),
        ("arm64:elf64-aarch64-linux-exec", "elf64-aarch64-linux-dyn"),
        ("x86_64:pe64-x86_64-windows-exec", "pe64-x86_64-windows-dll"),
        ("arm64:macho64-arm64-darwin-exec", "macho64-arm64-darwin-dylib"),
        ("x86_64:macho64-x86_64-darwin-exec", "macho64-x86_64-darwin-dylib"),
        # A container nothing here knows yields "" rather than a guess, so the
        # lint reports "cannot be cross-checked" instead of trusting blind.
        ("wasm32:wasm32-v1", ""),
    ]:
        check("the shared-library format of %s derives to %r" % (_spec, _want),
              derived_shared_lib_format(_spec) == _want,
              "got %r" % derived_shared_lib_format(_spec))
    for leg in legs:
        check("leg '%s' declares the shared-library format its own spec implies"
              % leg["label"],
              shared_lib_format(leg) == derived_shared_lib_format(leg["spec"]),
              "declared %r, derived %r"
              % (shared_lib_format(leg), derived_shared_lib_format(leg["spec"])))
    # ★ THE ARGUMENT THAT HAS COST FAILED INVOCATIONS: ONE combined `--target`,
    # and the arch and the format spell arm64 DIFFERENTLY.
    check("the arm64 leg's --target argument spells arm64 BOTH ways, in one arg",
          shared_lib_spec(leg_by_label(legs, "elf64-arm64", path))
          == "arm64:elf64-aarch64-linux-dyn",
          "got %r" % shared_lib_spec(leg_by_label(legs, "elf64-arm64", path)))
    _pe_argv = loadext_helper_dss_argv(
        leg_by_label(legs, "pe64-x86_64", path), "/dss", "/sq/src/test_loadext.c",
        ["/sq/src", "/bld"], "/work/dss", "release")
    check("the DSS argv passes ONE --target and no --object-format",
          _pe_argv.count("--target") == 1
          and "--object-format" not in _pe_argv
          and _pe_argv[_pe_argv.index("--target") + 1] == "x86_64:pe64-x86_64-windows-dll",
          "%r" % (_pe_argv,))
    check("...uses --include-dir, never -I",
          _pe_argv.count("--include-dir") == 2
          and not any(a.startswith("-I") for a in _pe_argv), "%r" % (_pe_argv,))
    check("...points --output at a DIRECTORY, never at the artefact",
          _pe_argv[_pe_argv.index("--output") + 1] == "/work/dss",
          "DSS writes <outdir>/<stem><ext> and NAMES it itself; a file path here "
          "is how a 1-byte 'artefact' turns out to be a directory being stat-ed")
    check("...and asks for the driver's own build config",
          "--config=release" in _pe_argv, "%r" % (_pe_argv,))
    _ref_argv = loadext_helper_reference_argv(
        leg_by_label(legs, "elf64-x86_64", path), "cc", "/sq/src/test_loadext.c",
        ["/sq/src", "/bld"], "/work/reference/libtestloadext.so")
    check("the CONTROL argv is the leg's own declared sharedLibFlags, unchanged",
          _ref_argv[:3] == ["cc", "-shared", "-fPIC"], "%r" % (_ref_argv,))
    # The builder verb: default, explicit, and the typo that must NOT be silently
    # read as the default.
    check("the default builder is dss", loadext_helper_builder(None, env={}) == "dss")
    check("the operator's env var selects the control arm",
          loadext_helper_builder(None, env={LOADEXT_HELPER_BUILDER_ENV: "reference"})
          == "reference")
    check("an explicit argument beats the environment",
          loadext_helper_builder("dss", env={LOADEXT_HELPER_BUILDER_ENV: "reference"})
          == "dss")
    check("a builder nothing implements RAISES, it is not read as the default",
          _raises(lambda: loadext_helper_builder("mingw", env={})),
          "silently defaulting a typo would report a control that never ran")

    # ── "it compiled" is NOT "it is a loadable shared library" ────────────────
    # The header reader, over bytes rather than files, so every arm is asserted
    # on any machine. The three POSITIVE cells are the exact headers ✔MEASURED
    # from the artefacts DSS emitted on 2026-08-05 (readelf/objdump/file agreeing
    # with this reader on all three).
    # ONE set of builders for BOTH header readers — `binary_shared_lib_shape`
    # (what KIND of image is this?) and `binary_target_identity` (what TARGET is
    # it for?). The identity fields carry defaults so every assertion below
    # reads exactly as it did before they existed, and the endianness parameter
    # is there because `binary_target_identity` takes byte order from EI_DATA
    # and must be shown doing so.
    def _elf(etype, cls=2, machine=EM_X86_64, osabi=0, endian="<"):
        order = "little" if endian == "<" else "big"
        head = bytearray(b"\x7fELF" + bytes([cls]) + b"\0" * 15 + b"\0" * 4)
        head[5] = 1 if endian == "<" else 2      # EI_DATA
        head[7] = osabi                          # EI_OSABI
        head[16:18] = etype.to_bytes(2, order)   # e_type
        head[18:20] = machine.to_bytes(2, order)  # e_machine
        return bytes(head)

    def _pe(chars, machine=IMAGE_FILE_MACHINE_AMD64):
        head = bytearray(b"MZ" + b"\0" * 0x3E)
        head[0x3C:0x40] = (0x40).to_bytes(4, "little")
        pe = bytearray(b"PE\0\0" + b"\0" * 20)
        pe[4:6] = machine.to_bytes(2, "little")
        pe[22:24] = chars.to_bytes(2, "little")
        return bytes(head) + bytes(pe)

    def _macho(filetype, cputype=MACHO_CPU_TYPES["x86_64"]):
        return (MH_MAGIC_64.to_bytes(4, "little")
                + cputype.to_bytes(4, "little") + b"\0" * 4
                + filetype.to_bytes(4, "little") + b"\0" * 16)

    for _blob, _want in [
        (_elf(ET_DYN), ("elf64", True)),
        (_elf(2), ("elf64", False)),              # ET_EXEC — compiled, not loadable
        (_elf(ET_DYN, cls=1), ("", False)),       # 32-bit
        (_pe(IMAGE_FILE_DLL | 0x22), ("pe64", True)),
        (_pe(0x22), ("pe64", False)),             # an EXE emitted under a .dll name
        (_macho(MH_DYLIB), ("macho64", True)),
        (_macho(2), ("macho64", False)),          # MH_EXECUTE
        (b"", ("", False)),                       # the empty "artefact"
        (b"!<arch>\n", ("", False)),              # a static archive
    ]:
        _c, _s, _d = binary_shared_lib_shape(_blob)
        check("a %d-byte artefact reads as %r" % (len(_blob), _want),
              (_c, _s) == _want, "got (%r, %r) — %s" % (_c, _s, _d))
    check("every shape carries a reason, on both outcomes",
          all(binary_shared_lib_shape(b)[2]
              for b in (b"", _elf(ET_DYN), _pe(0x22), _macho(2), b"junk")))

    # ── WHICH TARGET A BINARY IS FOR, READ OUT OF THE BINARY ────────────────
    # The other half of the same question, and the input a smoke gate needs
    # before it can attribute anything: a binary that will not run because it is
    # for another machine is not a compiler defect, and an exit code cannot tell
    # the two apart. ALL FIVE of this catalogue's targets, over SYNTHESISED
    # headers, so every arm is asserted on whatever host is running.
    for _blob, _want in [
        (_elf(2, machine=EM_X86_64), ("x86_64", "elf64", "linux")),
        (_elf(2, machine=EM_AARCH64), ("arm64", "elf64", "linux")),
        (_pe(0x22, machine=IMAGE_FILE_MACHINE_AMD64),
         ("x86_64", "pe64", "windows")),
        (_pe(0x22, machine=IMAGE_FILE_MACHINE_ARM64),
         ("arm64", "pe64", "windows")),
        (_macho(2, cputype=MACHO_CPU_TYPES["x86_64"]),
         ("x86_64", "macho64", "darwin")),
        (_macho(2, cputype=MACHO_CPU_TYPES["arm64"]),
         ("arm64", "macho64", "darwin")),
    ]:
        check("a synthetic %s/%s/%s header identifies as itself" % _want,
              binary_target_identity(_blob) == _want,
              "got %r" % (binary_target_identity(_blob),))
    # ✔MEASURED, this host, through the shipped CLI: a Windows notepad.exe reads
    # x86_64/pe64/windows, WSL's /bin/ls reads x86_64/elf64/linux, and
    # /usr/aarch64-linux-gnu/lib/ld-linux-aarch64.so.1 reads arm64/elf64/linux.
    # The synthetic cells above are what make the OTHER arms assertable here.
    #
    # A BIG-ENDIAN ELF is read correctly — byte order from EI_DATA, never from
    # the host. Mirrors the same assertion on the Tcl reader below; a host-keyed
    # struct format would report this one as x86_64 on every machine this
    # project owns (0xB7 byte-swapped is 0xB700, which is in no table, so the
    # failure would at least be loud — but it would be loud about the wrong
    # thing).
    check("the identity reader is not host-endian",
          binary_target_identity(_elf(2, machine=EM_AARCH64, endian=">"))
          == ("arm64", "elf64", "linux"))
    # ★ AND THE REFUSALS. Every one of these must RAISE rather than default:
    # the caller is deciding whether a binary that would not run is this
    # compiler's fault, and a guess there is a verdict about the compiler.
    check("an UNKNOWN EI_OSABI RAISES rather than defaulting to linux",
          _raises(lambda: binary_target_identity(
              _elf(2, machine=EM_X86_64, osabi=9))),
          "ELFOSABI_FREEBSD=9 — ELF's identity does not carry an OS the way PE "
          "and Mach-O do, so an unmapped value is a target nobody declared")
    check("...and the refusal names EI_OSABI and the values it does know",
          "EI_OSABI" in _raise_text(lambda: binary_target_identity(
              _elf(2, machine=EM_X86_64, osabi=9))))
    check("EI_OSABI 0 (SysV) and 3 (GNU) both read as linux — the two values "
          "this catalogue's own compilers emit",
          binary_target_identity(_elf(2, osabi=0))[2] == "linux"
          and binary_target_identity(_elf(2, osabi=3))[2] == "linux",
          "MEASURED: every shipped elf64-*.format.json declares osabi 'sysv' "
          "(0), and /bin/ls on this host's WSL carries 0 too")
    for _what, _blob in (
            ("an e_machine no leg targets", _elf(2, machine=0x28)),
            ("a PE Machine no leg targets", _pe(0x22, machine=0x1C0)),
            ("a Mach-O cputype no leg targets", _macho(2, cputype=0x0000000C)),
            ("a 32-bit ELF", _elf(2, cls=1)),
            ("an empty file", b""),
            ("bytes in no container at all", b"junk-not-a-binary"),
            ("a Mach-O universal archive",
             (0xCAFEBABE).to_bytes(4, "big") + (0).to_bytes(4, "big"))):
        check("%s is REFUSED, never identified as something else" % _what,
              _raises(lambda b=_blob: binary_target_identity(b)))

    # ── 4-D: THE ARTEFACT'S OWN LOAD-TIME DEMANDS vs THE DECLARATION ────────
    # After a leg builds, what it will ask of whatever runs it is READABLE, and
    # every one of those demands must be covered by a declared `requires` row.
    # ⚠ AND THE LIMIT OF THAT, WHICH IS THE POINT: libgcc_s.so.1 is in NO
    # DT_NEEDED and NO PT_INTERP — glibc dlopen()s it inside pthread_exit, at
    # teardown — so a derived list would have been complete, correct, and would
    # still have missed the thing that aborted three corpus segments.
    def _elf_exec(interp, needed, endian="<"):
        """The smallest ELF64 with a PT_INTERP segment and DT_NEEDED entries —
        the two tables `elf_runtime_dependencies` actually walks, and nothing
        else."""
        import struct
        strtab = bytearray(b"\0")
        offs = {}
        for nm in needed:
            offs[nm] = len(strtab)
            strtab += nm.encode() + b"\0"
        interp_b = interp.encode() + b"\0"
        dyn = b"".join(struct.pack(endian + "qQ", DT_NEEDED, offs[n])
                       for n in needed)
        dyn += struct.pack(endian + "qQ", DT_NULL, 0)
        o_interp = 64 + 56                       # after ehdr + one phdr
        o_str = o_interp + len(interp_b)
        o_dyn = o_str + len(strtab)
        o_sh = o_dyn + len(dyn)
        phdr = struct.pack(endian + "IIQQQQQQ", PT_INTERP, 4, o_interp, 0, 0,
                           len(interp_b), len(interp_b), 1)
        shdrs = b"".join(
            struct.pack(endian + "IIQQQQIIQQ", 0, st, 0, 0, off, size, link,
                        0, 0, esz)
            for st, off, size, link, esz in (
                (0, 0, 0, 0, 0),                              # SHT_NULL
                (SHT_STRTAB, o_str, len(strtab), 0, 0),       # 1 .dynstr
                (SHT_DYNAMIC, o_dyn, len(dyn), 1, 16)))       # 2 .dynamic
        ehdr = bytearray(64)
        ehdr[0:6] = b"\x7fELF" + bytes([2, 1 if endian == "<" else 2])
        ehdr[16:18] = struct.pack(endian + "H", 2)            # ET_EXEC
        ehdr[18:20] = struct.pack(endian + "H", EM_AARCH64)
        ehdr[0x20:0x28] = struct.pack(endian + "Q", 64)       # e_phoff
        ehdr[0x28:0x30] = struct.pack(endian + "Q", o_sh)     # e_shoff
        ehdr[0x36:0x3A] = struct.pack(endian + "HH", 56, 1)   # phentsize/phnum
        ehdr[0x3A:0x3E] = struct.pack(endian + "HH", 64, 3)   # shentsize/shnum
        return bytes(ehdr) + phdr + interp_b + bytes(strtab) + dyn + shdrs

    _ARTEFACT_INTERP = "/lib/ld-linux-aarch64.so.1"
    _ARTEFACT_NEEDED = ["libtcl8.6.so", "libz.so.1", "libm.so.6", "libc.so.6"]
    _art = _elf_exec(_ARTEFACT_INTERP, _ARTEFACT_NEEDED)
    check("the ELF reader recovers PT_INTERP and every DT_NEEDED",
          elf_runtime_dependencies(_art) == (_ARTEFACT_INTERP, _ARTEFACT_NEEDED),
          "got %r" % (elf_runtime_dependencies(_art),))
    check("...and it is not host-endian either",
          elf_runtime_dependencies(_elf_exec(_ARTEFACT_INTERP, _ARTEFACT_NEEDED,
                                             endian=">"))
          == (_ARTEFACT_INTERP, _ARTEFACT_NEEDED))
    check("a binary with neither segment yields empty, not a crash",
          elf_runtime_dependencies(_elf(2)) == ("", []))
    # THE SHIPPED DECLARATION, against a synthetic artefact with the real
    # interpreter and library set. This is the POSITIVE CONTROL for the mutation
    # row at the bottom of this self-test: if it did not pass here, that row
    # would "red" for free and prove nothing.
    _arm_leg = leg_by_label(legs, "elf64-arm64", path)
    _arm_entry = launcher_for(_arm_leg, "windows", "x86_64")
    _arm_rows = resolve_launcher_requirements(_arm_entry, "elf64-arm64")
    _cov = dependency_coverage_findings("elf64-arm64", _arm_rows,
                                        _ARTEFACT_INTERP, _ARTEFACT_NEEDED,
                                        _staged_library_names(_arm_leg))
    check("the SHIPPED elf64-arm64 Windows launcher covers everything its "
          "artefact asks for at load time", not _cov, "\n      ".join(_cov))
    check("dropping the ld-linux row leaves PT_INTERP UNCOVERED",
          any("PT_INTERP" in f for f in dependency_coverage_findings(
              "elf64-arm64",
              [r for r in _arm_rows if "ld-linux" not in r["path"]],
              _ARTEFACT_INTERP, _ARTEFACT_NEEDED,
              _staged_library_names(_arm_leg))),
          "a sysroot DIRECTORY row must NOT be allowed to cover the "
          "interpreter: 'the sysroot exists' and 'the loader is inside it' are "
          "two facts and it was the second one that was false")
    check("a staged library is NOT demanded of the launcher",
          "libtcl8.6.so" in _staged_library_names(_arm_leg)
          and not dependency_coverage_findings(
              "elf64-arm64", _arm_rows, "", ["libtcl8.6.so"],
              _staged_library_names(_arm_leg)))
    check("with NO directory row, an unstaged DT_NEEDED is reported",
          any("libc.so.6" in f for f in dependency_coverage_findings(
              "fx", [r for r in _arm_rows if r["kind"] == "file"],
              "", ["libc.so.6"], [])))
    # ★ AND THE CHECK MUST SAY WHEN IT DID NOT APPLY. Only the ELF reader lives
    # here, so a PE or Mach-O artefact yields nothing to cross-check — and "no
    # findings" would read as "checked and clean", which is the shape of every
    # instrument in this project that has ever reported success over something
    # it could not observe.
    import tempfile as _atf
    _adir = _atf.mkdtemp(prefix="dss-legs-artefact-")
    try:
        _apaths = {}
        for _nm, _bytes in (("elf", _art), ("pe", _pe(0x22)),
                            ("junk", b"not an object file")):
            _apaths[_nm] = os.path.join(_adir, _nm + ".bin")
            with open(_apaths[_nm], "wb") as _fh:
                _fh.write(_bytes)
        _r = check_launcher(_arm_leg, "windows", "x86_64", {"wsl.exe"},
                            runner=lambda a: (0, "", ""),
                            artifact=_apaths["elf"])
        check("the cross-check reports that it APPLIED, and to what",
              _r["crossCheck"].startswith("applied:")
              and "ld-linux-aarch64.so.1" in _r["crossCheck"],
              "%r" % (_r["crossCheck"],))
        for _nm in ("pe", "junk"):
            _r = check_launcher(_arm_leg, "windows", "x86_64", {"wsl.exe"},
                                runner=lambda a: (0, "", ""),
                                artifact=_apaths[_nm])
            check("a %s artefact reports NOT APPLIED rather than clean" % _nm,
                  _r["crossCheck"].startswith("NOT APPLIED") and _r["ok"],
                  "%r" % (_r["crossCheck"],))
    finally:
        shutil.rmtree(_adir, ignore_errors=True)

    # ── the FOURTH, PER-LEG Tcl coherence check ──────────────────────────────
    # [D-HARNESS-TCL-HEADER-IS-HOST-CHOSEN-WHILE-EVERY-LEG-LIBRARY-IS-PINNED]
    #
    # WHY THE IMAGES ARE SYNTHESISED. The three READERS were ✔MEASURED against
    # five REAL Tcl 8.6 libraries (see the banner above `library_exports`) — but
    # no Tcl 9 library exists on the machines this project builds on, and the
    # whole point of the check is the 8-vs-9 discrimination. A synthetic image
    # per container is the only way to exercise the "major 9" arm on ANY host,
    # which is also the rule the rest of this self-test follows: assert every arm
    # on every machine rather than only the arms this machine happens to have.
    # These builders emit the smallest images the readers accept — the tables the
    # readers actually walk, and nothing else.
    def _elf_lib(exports, soname, endian="<"):
        import struct
        strtab = bytearray(b"\0")
        offs = {}
        for nm in list(exports) + [soname]:
            if nm and nm not in offs:
                offs[nm] = len(strtab)
                strtab += nm.encode() + b"\0"
        syms = bytearray(24)                       # index 0 is the null symbol
        for nm in exports:
            syms += struct.pack(endian + "IBBHQQ", offs[nm],
                                (STB_GLOBAL << 4) | 2, 0, 1, 0, 0)
        dyn = struct.pack(endian + "qQ", DT_SONAME, offs.get(soname, 0)) \
            + struct.pack(endian + "qQ", DT_NULL, 0)
        o_str = 64
        o_sym = o_str + len(strtab)
        o_dyn = o_sym + len(syms)
        o_sh = o_dyn + len(dyn)
        shdrs = b"".join(
            struct.pack(endian + "IIQQQQIIQQ", 0, st, 0, 0, off, size, link, 0, 0, esz)
            for st, off, size, link, esz in (
                (0, 0, 0, 0, 0),                                   # SHT_NULL
                (SHT_STRTAB, o_str, len(strtab), 0, 0),            # 1 .dynstr
                (SHT_DYNSYM, o_sym, len(syms), 1, 24),             # 2 .dynsym
                (SHT_DYNAMIC, o_dyn, len(dyn), 1, 16)))            # 3 .dynamic
        ehdr = bytearray(64)
        ehdr[0:6] = b"\x7fELF" + bytes([2, 1 if endian == "<" else 2])
        ehdr[16:18] = struct.pack(endian + "H", ET_DYN)
        ehdr[0x28:0x30] = struct.pack(endian + "Q", o_sh)
        ehdr[0x3A:0x3E] = struct.pack(endian + "HH", 64, 4)
        return bytes(ehdr) + bytes(strtab) + bytes(syms) + dyn + shdrs

    def _macho_lib(exports, install_name):
        import struct
        strtab = bytearray(b"\0")
        offs = {}
        for nm in exports:
            offs[nm] = len(strtab)
            strtab += b"_" + nm.encode() + b"\0"
        idname = install_name.encode() + b"\0"
        idname += b"\0" * (-(24 + len(idname)) % 8)
        lc_id = struct.pack("<IIIIII", LC_ID_DYLIB, 24 + len(idname), 24,
                            0, 0, 0) + idname
        o_sym = 32 + len(lc_id) + 24
        syms = b"".join(struct.pack("<IBBHQ", offs[nm], N_EXT | N_SECT, 1, 0, 0)
                        for nm in exports)
        lc_symtab = struct.pack("<IIIIII", LC_SYMTAB, 24, o_sym, len(exports),
                                o_sym + len(syms), len(strtab))
        head = struct.pack("<IIIIIII", MH_MAGIC_64, 0x0100000C, 0, MH_DYLIB,
                           2, len(lc_id) + 24, 0) + b"\0" * 4
        return head + lc_id + lc_symtab + syms + bytes(strtab)

    def _pe_lib(exports, dll_name):
        import struct
        RVA = 0x1000
        RAW = 0x200
        body = bytearray(40 + 4 * len(exports))
        def _put(text):
            at = len(body)
            body.extend(text.encode() + b"\0")
            return RVA + at
        name_rva = _put(dll_name)
        for i, nm in enumerate(exports):
            struct.pack_into("<I", body, 40 + 4 * i, _put(nm))
        struct.pack_into("<IIIIIII", body, 12, name_rva, 1, len(exports),
                         len(exports), 0, RVA + 40, 0)
        opt = bytearray(240)
        struct.pack_into("<H", opt, 0, 0x20B)
        struct.pack_into("<II", opt, 112, RVA, len(body))
        coff = struct.pack("<HHIIIHH", 0x8664, 1, 0, 0, 0, 240,
                           IMAGE_FILE_DLL | 0x22)
        sec = (b".rdata\0\0" + struct.pack("<IIII", len(body), RVA, len(body), RAW)
               + b"\0" * 16)
        head = bytearray(0x40)
        head[0:2] = b"MZ"
        struct.pack_into("<I", head, 0x3C, 0x40)
        img = bytearray(head + b"PE\0\0" + coff + bytes(opt) + sec)
        img += b"\0" * (RAW - len(img))
        return bytes(img + body)

    _T9 = list(TCL9_ONLY_EXPORTS) + [TCL_SENTINEL_EXPORT]
    _T8 = ["Tcl_GetBoolean", "Tcl_GetBooleanFromObj", TCL_SENTINEL_EXPORT]
    _libs = {
        "elf 9.0":    _elf_lib(_T9, "libtcl9.0.so"),
        "elf 8.6":    _elf_lib(_T8, "libtcl8.6.so"),
        "elf 8.6 BE": _elf_lib(_T8, "libtcl8.6.so", endian=">"),
        "macho 9.0":  _macho_lib(_T9, "/opt/homebrew/opt/tcl-tk/lib/libtcl9.0.dylib"),
        "macho 8.6":  _macho_lib(_T8, "/opt/local/lib/libtcl8.6.dylib"),
        "pe 9.0":     _pe_lib(_T9, "tcl90.dll"),
        "pe 8.6":     _pe_lib(_T8, "tcl86.dll"),
    }
    for _name, _blob in sorted(_libs.items()):
        _want = _name.split()[1]
        _f = tcl_library_facts(_blob)
        check("a synthetic %s libtcl measures as Tcl %s" % (_name, _want),
              _f["version"] == _want and not _f["undetermined"],
              "got %r (%s%s)" % (_f["version"], _f["method"], _f["undetermined"]))
        check("%s: BOTH instruments spoke, and they agree" % _name,
              _f["symbolMajor"] == _want.split(".")[0]
              and _f["identityVersion"] == _want,
              "symbolMajor=%r identityVersion=%r"
              % (_f["symbolMajor"], _f["identityVersion"]))
    # A BIG-ENDIAN ELF is read correctly: the reader takes byte order from
    # EI_DATA, never from the host. A host-keyed struct format would report this
    # one as "cannot determine" on every machine this project owns.
    check("the ELF reader is not host-endian",
          tcl_library_facts(_libs["elf 8.6 BE"])["version"] == "8.6")

    # UNDETERMINED — the failure mode that must never become a silent pass.
    for _what, _blob, _needle in (
            ("a MIX of the Tcl-9 markers (an 8.7-shaped library)",
             _elf_lib([TCL9_ONLY_EXPORTS[0], TCL_SENTINEL_EXPORT], "libtcl.so"),
             "no single release explains"),
            ("a library that is not Tcl at all",
             _elf_lib(["sqlite3_open"], "libsqlite3.so.0"),
             TCL_SENTINEL_EXPORT),
            ("bytes in no container this reader knows", b"junk", "could not be read"),
            ("a Mach-O universal archive", (0xCAFEBABE).to_bytes(4, "big")
             + (0).to_bytes(4, "big"), "could not be read")):
        _f = tcl_library_facts(_blob)
        check("UNDETERMINED, with a reason: %s" % _what,
              not _f["version"] and _needle in _f["undetermined"],
              "version=%r undetermined=%r" % (_f["version"], _f["undetermined"]))

    check("a versionless self-declared identity still leaves the MAJOR measured",
          tcl_library_facts(_elf_lib(_T8, "libtcl.so"))["symbolMajor"] == "8")
    for _ident, _want in (("libtcl8.6.so", "8.6"), ("libtcl8.6.so.0", "8.6"),
                          ("/opt/local/lib/libtcl8.6.dylib", "8.6"),
                          ("libtcl9.0.dylib", "9.0"), ("tcl86.dll", "8.6"),
                          ("tcl90.dll", "9.0"), ("libtcl.so", ""),
                          # ✔MEASURED on the pe64 leg's ACQUIRED library: the
                          # threaded Windows build calls itself `tcl86t.dll`,
                          # and until TF-C123 that matched nothing — silently
                          # dropping this leg to a major-only version.
                          ("tcl86t.dll", "8.6"), ("tcl90t.dll", "9.0"),
                          ("tcl.dll", ""), ("tcl8t.dll", ""),
                          ("libtcl8.6.a", ""), ("", "")):
        check("self-declared identity %r -> %r" % (_ident, _want),
              tcl_identity_version(_ident) == _want,
              "got %r" % tcl_identity_version(_ident))

    def _entry(label, key):
        return (label, "/lib/" + key.replace(" ", "-"),
                tcl_library_facts(_libs[key]))

    _ok, _lines, _warn, _fatal = tcl_coherence(
        "8.6", [_entry("elf64-arm64", "elf 8.6"),
                _entry("macho64-arm64", "macho 8.6"),
                _entry("pe64-x86_64", "pe 8.6")])
    check("three legs pinned at 8.6 under an 8.6 header are COHERENT",
          _ok and not _warn and len(_lines) == 3, _fatal)

    # ★ THE DEFECT, REPRODUCED. A 9.0 header (this Mac's Homebrew default) over
    # the 8.6 library every leg's provider pins. Before this check the run got
    # four K_SymbolUndefined hours later; now it refuses in the first seconds.
    _ok, _lines, _warn, _fatal = tcl_coherence(
        "9.0", [_entry("elf64-arm64", "elf 8.6"), _entry("macho64-arm64", "macho 8.6")])
    check("a 9.0 header over a PINNED 8.6 library is FATAL", not _ok)
    check("the refusal names the leg, both versions and the remedy",
          all(t in _fatal for t in ("elf64-arm64", "9.0", "8.6",
                                    "DSS_TCL_VERSION=8.6")),
          _fatal)
    check("the refusal names the SYMBOLS the skew is made of",
          all(n in _fatal for n in TCL9_ONLY_EXPORTS), _fatal)
    # …and the mirror image, which is what a Tcl-9 host with a Tcl-9 library
    # would hit the day the pinned archives move.
    check("an 8.6 header over a 9.0 library is FATAL too",
          not tcl_coherence("8.6", [_entry("macho64-arm64", "macho 9.0")])[0])
    # The MINOR is only visible to the self-declared identity — the marker set
    # cannot see 8.5-vs-8.6. Either instrument may veto on its own.
    check("a MINOR-only skew is caught by the self-declared identity alone",
          not tcl_coherence("8.6", [("elf64-x86_64", "/lib/x", tcl_library_facts(
              _elf_lib(_T8, "libtcl8.5.so")))])[0])

    _ok, _lines, _warn, _fatal = tcl_coherence(
        "8.6", [_entry("elf64-arm64", "elf 8.6"), _entry("macho64-arm64", "macho 9.0")])
    check("legs that disagree WITH EACH OTHER are structurally incoherent",
          not _ok and "structurally incoherent" in _fatal
          and "elf64-arm64" in _fatal and "macho64-arm64" in _fatal, _fatal)

    _ok, _lines, _warn, _fatal = tcl_coherence(
        "8.6", [_entry("elf64-arm64", "elf 8.6"),
                ("pe64-x86_64", "/lib/junk", tcl_library_facts(b"junk"))])
    check("a leg whose version cannot be measured WARNS and is skipped, never "
          "silently passed",
          _ok and len(_warn) == 1 and "pe64-x86_64" in _warn[0], "%r" % _warn)
    check("every leg is reported, measured or not", len(_lines) == 2)
    check("no legs resolved a libtcl is not, by itself, an incoherence",
          tcl_coherence("8.6", [])[0])

    # The staged header's version comes out of the FILE. Tcl 9 indents its
    # `#   define`; 8.6 does not. Both spellings, and a file that declares
    # nothing (which the CLI turns into a refusal, not a pass).
    import tempfile as _tf
    _hd = _tf.mkdtemp(prefix="dss-tclh-")
    try:
        for _text, _want in (('#define TCL_VERSION "8.6"\n', "8.6"),
                             ('#   define TCL_VERSION\t"9.0"\n', "9.0"),
                             ('/* #define TCL_VERSION "7.6" */\n', ""),
                             ('#define TCL_PATCH_LEVEL "8.6.14"\n', "")):
            _p = os.path.join(_hd, "tcl.h")
            with open(_p, "w", encoding="utf-8") as _f:
                _f.write("#ifndef _TCL\n" + _text + "#endif\n")
            check("a staged tcl.h containing %r reports %r" % (_text.strip(), _want),
                  tcl_header_version(_p) == _want,
                  "got %r" % tcl_header_version(_p))
        check("a tcl.h that is not there reports nothing, and never a version",
              tcl_header_version(os.path.join(_hd, "absent.h")) == "")
    finally:
        shutil.rmtree(_hd, ignore_errors=True)

    # ── the artefact line, matched the way BOTH base harnesses match it ───────
    _log = ("dss-code-prime: artifact x86_64:pe64-x86_64-windows-dll /out/a.dll\n"
            "info[R_Something] noise\n"
            "dss-code-prime: artifact x86_64:pe64-x86_64-windows-dll /out/a.dll\n"
            "dss-code-prime: artifact x86_64:elf64-x86_64-linux-dyn /out/a.so\n")
    check("the reported artefact is selected by SPEC and de-duplicated",
          dss_reported_artifacts(_log, "x86_64:pe64-x86_64-windows-dll")
          == ["/out/a.dll"],
          "%r" % dss_reported_artifacts(_log, "x86_64:pe64-x86_64-windows-dll"))
    check("a sibling spec's artefact is never returned",
          dss_reported_artifacts(_log, "x86_64:elf64-x86_64-linux-dyn")
          == ["/out/a.so"])
    check("TWO DIFFERENT paths for one spec are BOTH returned, so the caller can "
          "refuse to guess",
          len(dss_reported_artifacts(
              _log + "dss-code-prime: artifact x86_64:pe64-x86_64-windows-dll "
                     "/out/b.dll\n", "x86_64:pe64-x86_64-windows-dll")) == 2)
    check("the prefix is the one base-harness.{sh,ps1} grep for",
          DSS_ARTIFACT_LINE_PREFIX == "dss-code-prime: artifact ")
    # BOTH spellings. Grepping only `error[` has cost this project a diagnosis.
    check("diagnostics are found as `error[` AND as `error:`",
          dss_log_errors("error[P0016]: got quote include not found: sqlite3.h")
          and dss_log_errors("test_loadext.c:14: error: no such thing"))

    # ── build_loadext_helper: every verdict class, with the compiler injected ─
    # A stub runner, so these cells are reproducible on a machine with no
    # compiler at all — the same discipline as the resolve_target_cc cells above.
    import tempfile as _tf
    _bl_dir = _tf.mkdtemp(prefix="dss-legs-loadext-")
    try:
        _sq = os.path.join(_bl_dir, "sq", "src")
        _bld = os.path.join(_bl_dir, "bld")
        os.makedirs(_sq)
        os.makedirs(_bld)
        with open(os.path.join(_sq, LOADEXT_HELPER_SOURCE), "w") as _f:
            _f.write("int x;\n")
        _pe_leg = leg_by_label(legs, "pe64-x86_64", path)

        def _fake_dss(blob, spec_override=None):
            """A runner that writes `blob` where DSS would and reports it."""
            def run(argv):
                if argv[0] != "/dss":                    # the reference arm
                    out_i = argv.index("-o")
                    with open(argv[out_i + 1], "wb") as f:
                        f.write(blob)
                    return 0, ""
                outdir = argv[argv.index("--output") + 1]
                spec = spec_override or argv[argv.index("--target") + 1]
                art = _fwd(os.path.join(outdir, "test_loadext.dll"))
                os.makedirs(outdir, exist_ok=True)
                with open(art, "wb") as f:
                    f.write(blob)
                return 0, "%s%s %s\n" % (DSS_ARTIFACT_LINE_PREFIX, spec, art)
            return run

        def _call(**kw):
            n = len(os.listdir(_bl_dir))
            kw.setdefault("dss", "/dss")
            return build_loadext_helper(
                _pe_leg, kw.pop("dss"), _sq, _bld,
                os.path.join(_bl_dir, "dest%d" % n),
                os.path.join(_bl_dir, "work%d" % n), **kw)

        _r = _call(runner=_fake_dss(_pe(IMAGE_FILE_DLL | 0x22)))
        check("a DSS-built DLL stages under the leg's DECLARED name",
              _r["verdictClass"] == "" and _r["staged"].endswith("/testloadext.dll")
              and os.path.isfile(_r["staged"]),
              "%r / %r" % (_r["verdictClass"], _r["staged"]))
        check("...and the report names DSS as the builder, with no control here",
              _r["builder"] == "dss" and _r["primary"]["builder"] == "dss"
              and not _r["control"]["available"]
              and "NO CONTROL ON THIS HOST" in _r["crossCheck"], _r["crossCheck"])
        # ★ RED ON DISABLE, and it is THE one that matters: an artefact that
        # "succeeded" but is not loadable must NOT be staged as if it were. Same
        # runner, same rc=0, ONE byte-level difference — IMAGE_FILE_DLL cleared.
        _r = _call(runner=_fake_dss(_pe(0x22)))
        check("★ an EXE emitted under a .dll name is POISONED, not staged as OK",
              _r["verdictClass"] == "poisoned"
              and "not a loadable pe64 shared library" in _r["detail"],
              "%r / %s" % (_r["verdictClass"], _r["detail"]))
        _r = _call(runner=_fake_dss(b""))
        check("...and so is a ZERO-BYTE artefact the build called a success",
              _r["verdictClass"] == "poisoned" and "EMPTY" in _r["detail"],
              _r["detail"])
        # An ELF where a PE was asked for: the leg's declared format is the only
        # thing that decides, and it catches a wrong --target as readily as a
        # broken emitter.
        _r = _call(runner=_fake_dss(_elf(ET_DYN)))
        check("an artefact for the WRONG container is refused by the leg's own "
              "declared format",
              _r["verdictClass"] == "poisoned" and "elf64 image" in _r["detail"],
              _r["detail"])
        # A build that says nothing and claims nothing.
        _r = _call(runner=lambda argv: (0, "") if argv[0] == "/dss" else (0, ""))
        check("a silent build that reports NO artefact is poisoned",
              _r["verdictClass"] == "poisoned"
              and "reported NO artefact" in _r["detail"], _r["detail"])
        # dss-code-prime EXITS 0 ON FATAL ERRORS — the diagnostics decide.
        _r = _call(runner=lambda argv: (0, "error[P0016]: got quote include not "
                                           "found: sqlite3.h\n"))
        check("rc=0 with `error[` diagnostics is a FAILURE, never a success",
              _r["verdictClass"] == "poisoned" and "P0016" in _r["detail"],
              _r["detail"])
        check("...and the refusal says it will NOT fall back to the other builder",
              "NOT falling back" in _r["detail"], _r["detail"])
        # THE CONTROL ARM, present.
        _r = _call(runner=_fake_dss(_pe(IMAGE_FILE_DLL | 0x22)),
                   reference_cc="gcc", reference_machine="x86_64-w64-mingw32")
        check("a verified target compiler is built as a CONTROL beside the primary",
              _r["verdictClass"] == "" and _r["builder"] == "dss"
              and _r["control"]["ok"] and _r["control"]["builder"] == "reference"
              and "CONTROL PRESENT" in _r["crossCheck"], _r["crossCheck"])
        check("...the control is NOT what gets staged",
              _r["staged"] != _r["control"]["artifact"], _r["staged"])
        check("...and a failing control does NOT gate the run",
              _call(runner=lambda argv: _fake_dss(_pe(IMAGE_FILE_DLL))(argv)
                    if argv[0] == "/dss" else (1, "cc: fatal"),
                    reference_cc="gcc")["verdictClass"] == "")
        # THE DIFFERENTIAL SWITCH: the control becomes the staged artefact.
        _r = _call(runner=_fake_dss(_pe(IMAGE_FILE_DLL | 0x22)),
                   builder="reference", reference_cc="gcc")
        check("DSS_LOADEXT_HELPER=reference STAGES the control arm",
              _r["verdictClass"] == "" and _r["builder"] == "reference"
              and _r["primary"]["builder"] == "reference"
              and _r["control"]["builder"] == "dss", "%r" % _r["detail"])
        # ★ THE VERDICT-CLASS CONTRACT: the operator asked for an arm this host
        # cannot provide. ENVIRONMENTAL (the default would have worked), so
        # `skipped-build-input-missing` — NOT `poisoned`, which would red a run
        # in which nothing is wrong.
        _r = _call(runner=_fake_dss(_pe(IMAGE_FILE_DLL)), builder="reference")
        check("...and with no verified compiler it is ENVIRONMENTAL, not a failure",
              _r["verdictClass"] == "skipped-build-input-missing"
              and "would have staged this leg's helper here" in _r["detail"],
              "%r / %s" % (_r["verdictClass"], _r["detail"]))
        # The two catalogue-shaped refusals, which the lint also catches.
        _noname = json.loads(json.dumps(_pe_leg))
        _noname["build"].pop("loadExtHelperName")
        check("a leg with no declared helper name is refused, naming the key",
              build_loadext_helper(_noname, "/dss", _sq, _bld,
                                   os.path.join(_bl_dir, "dn"),
                                   os.path.join(_bl_dir, "wn"),
                                   runner=_fake_dss(b"x"))["detail"]
              .find("loadExtHelperName") >= 0)
        _nofmt = json.loads(json.dumps(_pe_leg))
        _nofmt["build"].pop("sharedLibFormat")
        check("a leg with no declared sharedLibFormat is refused, naming the key",
              build_loadext_helper(_nofmt, "/dss", _sq, _bld,
                                   os.path.join(_bl_dir, "df"),
                                   os.path.join(_bl_dir, "wf"),
                                   runner=_fake_dss(b"x"))["detail"]
              .find("sharedLibFormat") >= 0)
        check("a missing extension source is refused, naming the path",
              LOADEXT_HELPER_SOURCE in build_loadext_helper(
                  _pe_leg, "/dss", os.path.join(_bl_dir, "no-such-tree"), _bld,
                  os.path.join(_bl_dir, "ds"), os.path.join(_bl_dir, "ws"),
                  runner=_fake_dss(b"x"))["detail"])
    finally:
        shutil.rmtree(_bl_dir, ignore_errors=True)

    # RED ON DISABLE: the lint must actually catch a wrong declaration, so it is
    # fed one. A mutated COPY on disk — never the shipped catalogue.
    _mut_dir = _tf.mkdtemp(prefix="dss-legs-lint-")
    try:
        with open(path, "r", encoding="utf-8") as _f:
            _doc = json.load(_f)
        # (target OS the mutation is applied to, key it breaks, what it does,
        # how [, the CHECKER that must red]). The KEY is carried explicitly
        # rather than sniffed out of the label: a finding about the OTHER key
        # would let a mutation pass for the wrong reason, which is how a
        # red-on-disable becomes decorative. The target OS is carried too because
        # a wrong declaration is only wrong on the target it disagrees with —
        # `HAVE_PREAD64: true` is CORRECT on the Linux legs and is the shipped
        # defect on the Darwin ones.
        #
        # ★ THE OPTIONAL FIFTH ELEMENT is the checker the mutation must red
        # under, defaulting to `lint`. One row needs it: the artefact's
        # PT_INTERP/DT_NEEDED cross-check cannot live in `lint` because at lint
        # time there IS no artefact — and it is exactly the check that decides
        # whether the ld-linux declaration is load-bearing or decorative, so it
        # gets driven here rather than trusted.
        def _interp_cross_check(mutated_path):
            _l = leg_by_label(load_catalogue(mutated_path), "elf64-arm64",
                              mutated_path)
            _e = launcher_for(_l, "windows", "x86_64")
            _interp, _needed = elf_runtime_dependencies(_art)
            return dependency_coverage_findings(
                "elf64-arm64",
                resolve_launcher_requirements(_e, "elf64-arm64"),
                _interp, _needed, _staged_library_names(_l))

        def _each_launcher(leg, fn):
            for _e in leg.get("launchers", []):
                fn(_e)

        for _row in (
            ("windows", "loadExtHelperName", "a POSIX helper name on the Windows leg",
             lambda l: l["build"].update(loadExtHelperName="libtestloadext.so")),
            ("windows", "loadExtHelperName", "no helper name at all",
             lambda l: l["build"].pop("loadExtHelperName", None)),
            # ★ RED ON DISABLE for the NEW declaration. The wrong-format case is
            # the dangerous one: `elf64-x86_64-linux-dyn` on the pe64 leg emits a
            # LINUX shared object for a WINDOWS fixture — byte-for-byte the
            # wrong-target helper D-HARNESS-ARM64-LEG-HOST-ARCH-HELPER-SO is
            # named after, arriving through a different door.
            ("windows", "sharedLibFormat", "a Linux shared-library format on the Windows leg",
             lambda l: l["build"].update(sharedLibFormat="elf64-x86_64-linux-dyn")),
            ("windows", "sharedLibFormat", "the leg's own EXEC format where the shared one belongs",
             lambda l: l["build"].update(sharedLibFormat="pe64-x86_64-windows-exec")),
            ("windows", "sharedLibFormat", "no sharedLibFormat at all",
             lambda l: l["build"].pop("sharedLibFormat", None)),
            # ★ RED ON DISABLE FOR configureAnswers, and the first mutation is
            # THE SHIPPED DEFECT ITSELF: `HAVE_PREAD64: true` on a Darwin leg is
            # exactly what the deriving Linux host's sqlite_cfg.h asserted, and
            # what made the macho64-arm64 CLI fail on `off64_t`/`pread64`/
            # `pwrite64`. If the lint cannot catch it, the declaration is
            # decorative and the header staged from it would be wrong again.
            ("darwin", "HAVE_PREAD64", "the deriving Linux host's HAVE_PREAD64 on a Darwin leg",
             lambda l: l["build"]["configureAnswers"].update(HAVE_PREAD64=True)),
            ("darwin", "HAVE_MALLOC_H", "glibc's HAVE_MALLOC_H on a Darwin leg",
             lambda l: l["build"]["configureAnswers"].update(HAVE_MALLOC_H=True)),
            ("linux", "HAVE_PWRITE64", "HAVE_PWRITE64 denied on a Linux leg",
             lambda l: l["build"]["configureAnswers"].update(HAVE_PWRITE64=False)),
            ("darwin", "configureAnswers", "a string where a JSON boolean belongs",
             lambda l: l["build"]["configureAnswers"].update(HAVE_PREAD64="false")),
            ("darwin", "configureAnswers", "no configureAnswers at all",
             lambda l: l["build"].pop("configureAnswers", None)),
            ("darwin", "HAVE_PWRITE64", "one answer of the three omitted",
             lambda l: l["build"]["configureAnswers"].pop("HAVE_PWRITE64", None)),
            # ⛔ THE MACRO THAT DOES NOT EXIST. legs.json used to name
            # `HAVE_OFF64_T` in prose; sqlite has no such symbol, and a
            # declaration of one would read as configuration while staging
            # nothing.
            ("darwin", "HAVE_OFF64_T", "an invented configure answer",
             lambda l: l["build"]["configureAnswers"].update(HAVE_OFF64_T=False)),
            # ★ RED ON DISABLE FOR `launchers[].requires` AND ITS `env`. Every
            # one of these was a state the catalogue could reach BEFORE this
            # change and nothing anywhere said a word about it.
            ("linux", "no `requires`", "a launcher declaring no requires at all",
             lambda l: _each_launcher(l, lambda e: e.pop("requires", None))),
            ("linux", "kind", "a requirement of a kind nothing implements",
             lambda l: _each_launcher(
                 l, lambda e: e["requires"] and e["requires"][0].update(kind="socket"))),
            ("linux", "${QEMU_LD_PREFIX}",
             "a ${VAR} this entry's own env does not declare",
             lambda l: _each_launcher(
                 l, lambda e: "QEMU_LD_PREFIX" in e.get("env", {})
                 and e["env"].update({"SYSROOT_TYPO": e["env"].pop("QEMU_LD_PREFIX")}))),
            ("linux", "install", "a requirement that states no remedy",
             lambda l: _each_launcher(
                 l, lambda e: [r.pop("install", None) for r in e.get("requires", [])])),
            ("linux", "windows-drive",
             "a DRIVER-namespace path in a windows-to-wsl launcher's env",
             lambda l: _each_launcher(
                 l, lambda e: e.get("pathTranslation") != "none"
                 and "QEMU_LD_PREFIX" in e.get("env", {})
                 and e["env"].update(QEMU_LD_PREFIX="C:\\sysroot"))),
            ("linux", "QEMU_LD_PREFIX",
             "a path-valued env with NO requires row referencing it",
             lambda l: _each_launcher(
                 l, lambda e: e.update(requires=[
                     r for r in e.get("requires", [])
                     if "${QEMU_LD_PREFIX}" not in r.get("path", "")]))),
            # ★★ AND THE SHIPPED DEFECT ITSELF. `${QEMU_LD_PREFIX}/lib/
            # ld-linux-aarch64.so.1` is the exact file whose absence produced
            # rc=255 with no diagnostic and 14 units charged to this compiler.
            # Drop that row and the artefact's own PT_INTERP is covered by
            # NOTHING — if this does not red, the declaration is decorative and
            # the next person to tidy the catalogue removes it for free.
            ("linux", "ld-linux-aarch64.so.1",
             "the ld-linux row dropped, against the artefact's own PT_INTERP",
             lambda l: _each_launcher(
                 l, lambda e: e.update(requires=[
                     r for r in e.get("requires", [])
                     if "ld-linux" not in r.get("path", "")])),
             _interp_cross_check),
        ):
            _os, _key, _variant, _mutate = _row[:4]
            _checker = _row[4] if len(_row) > 4 else lint
            _copy = json.loads(json.dumps(_doc))
            for _l in _copy["legs"]:
                if spec_target_os(_l["spec"]) == _os:
                    _mutate(_l)
            _p = os.path.join(_mut_dir, "legs.json")
            with open(_p, "w", encoding="utf-8") as _f:
                json.dump(_copy, _f)
            _found = [f for f in _checker(_p) if _key in f]
            check("the lint REDS on %s" % _variant, bool(_found),
                  "nothing said anything about %s — the check is dead config"
                  % _key)
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
    #
    # ★ AND `newline="\n"`, ADDED TF-C124 FOR THE SECOND HALF OF THE SAME FACT.
    # ✔MEASURED: on Windows this module's stdout is TEXT MODE, so every `\n` it
    # writes leaves as `\r\n` — `--env-transfers` really emits
    # `inherit\t\r\nwslenv\tWSLENV\r\n`. This module's output is a MACHINE
    # INTERFACE read by both drivers, and a stray CR is invisible in every log
    # while changing what the consumer got: build-and-test.sh's own note records
    # a classified skip token being rejected as a HARNESS DEFECT for exactly
    # this reason, and it cost a whole assertion here before it was found again.
    # Fixed at the SOURCE — one line, every mode, both drivers — rather than by
    # teaching each consumer to strip a CR it should never have received.
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace", newline="\n")
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
    p.add_argument("--config-stages", action="store_true",
                   help="print the distinct staged sqlite_cfg.h directories the "
                        "drivers must materialise: '<targetOs>\\t<HAVE_*>=<0|1> "
                        "...', one per line. HOST-FREE — which ./configure "
                        "answers a leg compiles against is a fact about its "
                        "TARGET, and inheriting the DERIVING host's is "
                        "D-HARNESS-MACHO-LEG-INHERITS-THE-DERIVING-LINUX-HOSTS-"
                        "CONFIGURE-PROBES.")
    p.add_argument("--stage-build", action="store_true",
                   help="print the declared sqlite stage build configuration — "
                        "the configure flags, the `make OPTIONS=` defines, the "
                        "defines that MUST show up in the derived recipe, and "
                        "the per-capability witness files. RUN-WIDE, not "
                        "per-leg: one staged tree feeds every leg, so the "
                        "capability set cannot be a leg property and must not "
                        "be a driver property. `--format sh` emits shell "
                        "assignments for build-and-test.sh and for the derive "
                        "script build-and-test.ps1 runs.")
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
                   help="an environment variable the launched process must see, "
                        "whose value is NAMESPACE-NEUTRAL. Repeatable. Its kind "
                        "must be DECLARED `opaque` in LAUNCH_FORWARD_KINDS; a "
                        "path-valued or unclassified name is REFUSED here and "
                        "names --forward-path in the diagnostic, so a HOST path "
                        "cannot reach a foreign launcher by this door.")
    p.add_argument("--forward-path", action="append", default=None,
                   metavar="NAME=VALUE",
                   help="a forwarded variable whose value is a path IN THIS "
                        "DRIVER'S namespace (TCL_LIBRARY). Repeatable. Its "
                        "value is put through --path-translation and printed "
                        "back as the assignment the driver must make BEFORE the "
                        "carrier names it. ★ USE THE `=` FORM: a Windows path "
                        "may begin with a drive letter but a future value may "
                        "not, and the space form would have argparse read one "
                        "starting with `-` as an option.")
    p.add_argument("--forward-declared", action="append", default=None,
                   metavar="NAME",
                   help="a forwarded variable whose value the CATALOGUE wrote "
                        "for THIS launcher (legs.json `launchers[].env`, e.g. "
                        "QEMU_LD_PREFIX). Repeatable. It is already in the "
                        "launcher's namespace by construction, so it crosses "
                        "verbatim and needs no declared kind — stated as its "
                        "own flag so that fact is visible rather than inferred.")
    p.add_argument("--check-regions", action="store_true",
                   help="THE VERIFIER THE `dss:corpus-engine` HEADER PROMISED "
                        "[D-HARNESS-CORPUS-ENGINE-MIRROR-CLAIMS-A-VERIFIER-"
                        "THAT-DOES-NOT-EXIST]. Checks every `dss:` region "
                        "against its declaration in DSS_REGIONS (a region with "
                        "no declaration, a declaration with no region, a "
                        "claimed verifier that does not read the region, and an "
                        "unverified region with no stated reason are each a "
                        "LOUD failure), then verifies the MIRROR contract of "
                        "every region declared mirrored: the symbol pairing, "
                        "and DIFFERENTIAL EXECUTION of both copies — extracted "
                        "from the shipped drivers — on byte-identical input. "
                        "Prints `passed=N failed=N skipped=N`.")
    p.add_argument("--registry-controls", default=None, metavar="REGISTRY",
                   help="print any _deferred-anchor-registry.md row whose text "
                        "names one of the --for NAMEs (a leg label, a failing "
                        "test name or family). Called by both drivers AT the "
                        "point they report a failure, so a prior matched "
                        "control arrives WITH the failure instead of waiting to "
                        "be remembered "
                        "[D-PROCESS-CHECK-THE-REGISTRY-FOR-A-MATCHED-CONTROL-"
                        "BEFORE-COMMISSIONING-ONE]. ⚠ ALWAYS exits 0: it runs "
                        "on a failure path and must never become one. A row is "
                        "a POINTER, never a verdict.")
    p.add_argument("--for-leg", action="append", default=None, dest="for_legs",
                   metavar="LABEL",
                   help="a LEG LABEL to look up under --registry-controls. "
                        "Repeatable. Searched WHOLE: decomposing `elf64-x86_64` "
                        "matched a third of the registry.")
    p.add_argument("--for-test", action="append", default=None, dest="for_tests",
                   metavar="NAME",
                   help="a FAILING TEST NAME to look up under "
                        "--registry-controls. Repeatable. Searched whole AND by "
                        "family component, because that is how a row spells a "
                        "test family. When any is given, a row matching only "
                        "the leg is NOT printed — the leg alone is not a "
                        "matched control.")
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
    # ── the ATTRIBUTION ORACLE, per leg ─────────────────────────────────────
    p.add_argument("--oracle-report", default=None, metavar="LABEL",
                   help="print LABEL's ORACLE VERDICT — the lines a driver's "
                        "results section prints verbatim. A leg whose reference "
                        "is a DIFFERENT platform's binary is reported as having "
                        "NO ORACLE, in those terms, and the fallback control is "
                        "named. Needs --reference-target (what --identify-binary "
                        "MEASURED off the reference) and --reference-path.")
    p.add_argument("--reference-target", default="", metavar="ARCH:FORMAT",
                   help="the run reference's own MEASURED target, from "
                        "--identify-binary. NEVER a guess: an unmeasured "
                        "reference is reported as no oracle at all.")
    p.add_argument("--reference-path", default="", metavar="PATH",
                   help="the run reference binary, or empty when none survived")
    p.add_argument("--leg-oracle", default="", metavar="PATH",
                   help="this leg's OWN same-platform reference, when "
                        "--build-reference-oracle produced one")
    p.add_argument("--leg-oracle-cc", default="", metavar="CC")
    p.add_argument("--leg-oracle-triple", default="", metavar="TRIPLE")
    p.add_argument("--classify-abort", default=None, metavar="LABEL",
                   help="decide whether a fixture ABORT on LABEL is an EARNED "
                        "confound. Takes --abort '<permutation>/<file>'. rc 0 = "
                        "earned (the row's provenance is printed on stdout, one "
                        "`key: value` per line, for the driver to relay), rc 3 = "
                        "UNEARNED, which still fails the leg. Uses the SAME "
                        "`confounds` ledger as the unit matcher, through the "
                        "`matches: abort-file` rows.")
    p.add_argument("--abort", default="", metavar="PERM/FILE",
                   help="the abort's LOCATION — permutation/file. NOT enough on "
                        "its own: --abort-log supplies its IDENTITY.")
    p.add_argument("--abort-log", default="", metavar="PATH",
                   help="the ABORTED SEGMENT'S LOG. Its FATAL TAIL — everything "
                        "printed after the fixture last did its job — is the "
                        "abort's DIAGNOSTIC, and a row matches only when the "
                        "file pattern AND its `abortDiagnostic` both do. A log "
                        "yielding no diagnostic (a silent crash) or one that "
                        "cannot be read matches NOTHING: absence of evidence "
                        "never satisfies a matcher "
                        "[D-HARNESS-ABORT-CONFOUND-KEYED-ON-LOCATION-NOT-IDENTITY].")
    p.add_argument("--build-reference-oracle", default=None, metavar="LABEL",
                   help="build LABEL's SAME-PLATFORM attribution oracle from "
                        "the leg's OWN manifest, with the compiler "
                        "--resolve-target-cc verifies against this leg's "
                        "target. Prints a JSON report; rc 0 built, 3 the build "
                        "failed, 4 no declared compiler on this host targets "
                        "this leg (the leg then HAS NO ORACLE and says so).")
    p.add_argument("--manifest", default="", metavar="PATH",
                   help="the leg's .dss-project.json — the SAME file dss "
                        "consumed, so the oracle compiles one declaration "
                        "rather than a second, drifting copy of it")
    p.add_argument("--oracle-output", default="", metavar="PATH",
                   help="where to write the oracle binary. Prefer --oracle-dir: "
                        "the FILE NAME carries the target's executable suffix "
                        "and is this catalogue's business, not a driver's.")
    p.add_argument("--oracle-dir", default="", metavar="DIR",
                   help="write the oracle into DIR under the name this leg's "
                        "TARGET requires (reference_oracle_name). A driver that "
                        "spells the suffix itself has a `.exe` branch in it.")
    p.add_argument("--oracle-log", default="", metavar="PATH",
                   help="where to write the reference compiler's own output")
    # ── PER-TU ATTRIBUTION OF A BUILD FAILURE ───────────────────────────────
    p.add_argument("--attribute-build", default=None, metavar="LABEL",
                   help="decide, PER TRANSLATION UNIT, whether LABEL's failed "
                        "dss build is charged to dss or to upstream. Needs "
                        "--compile-log (dss's own log), --oracle-log (the "
                        "reference's, from THIS run), --oracle-status (what "
                        "--build-reference-oracle reported) and --manifest. "
                        "Prints a JSON report with the driver's report lines in "
                        "it; rc 0 = every rejected TU is upstream-attributable, "
                        "rc 3 = at least one is charged to dss. An amnesty is "
                        "granted only where the reference ACTUALLY failed on "
                        "that TU AND an earned `matches: build-tu` row names it "
                        "[D-HARNESS-BUILD-FAILURE-HAS-NO-PER-TU-ATTRIBUTION].")
    p.add_argument("--compile-log", default="", metavar="PATH",
                   help="dss's own build log for this leg")
    p.add_argument("--oracle-status", default="", metavar="STATUS",
                   help="--build-reference-oracle's reported `status`, verbatim. "
                        "Only `built`/`build-failed` mean the control RAN; "
                        "anything else grants no amnesty to anything.")
    # ── the loadext helper the corpus dlopen()s ─────────────────────────────
    # ONE implementation, called by BOTH drivers — the same argument this
    # module's header makes for putting the leg decision here, and the reason
    # the .ps1 can finally stage a helper at all (it never could, because it had
    # no way to build one for a target its host has no compiler for).
    p.add_argument("--build-loadext-helper", default=None, metavar="LABEL",
                   help="build this leg's loadext helper extension and stage it "
                        "under the leg's declared loadExtHelperName. DSS is the "
                        "primary/default builder; the leg's verified target C "
                        "compiler is an optional CONTROL. Prints a JSON report; "
                        "rc 0 staged, 3 poisoned, 4 skipped-build-input-missing.")
    # A REFUSE-TO-START QUERY, deliberately separate from --build-loadext-helper.
    # Both drivers resolve the operator's choice ONCE, at Step 6, before any leg
    # has been compiled: a typo'd DSS_LOADEXT_HELPER must stop the run in the
    # first seconds, not after ~50 s..8 min of fixture build per leg. It prints
    # the resolved name and exits non-zero (rc 2, the LegError code) on a value
    # nothing implements.
    p.add_argument("--loadext-builder", action="store_true",
                   help="print the resolved loadext helper builder ($%s, else "
                        "'dss') and refuse an unimplemented value"
                        % LOADEXT_HELPER_BUILDER_ENV)
    p.add_argument("--helper-builder", default=None, metavar="dss|reference",
                   help="which arm STAGES the helper (default: $%s, else 'dss'). "
                        "'reference' makes the corpus itself the differential."
                        % LOADEXT_HELPER_BUILDER_ENV)
    p.add_argument("--sqlite-src", default="", metavar="DIR",
                   help="the staged sqlite tree's src/ (holds %s + sqlite3ext.h)"
                        % LOADEXT_HELPER_SOURCE)
    p.add_argument("--sqlite-bld", default="", metavar="DIR",
                   help="the build dir holding the GENERATED sqlite3.h")
    p.add_argument("--dest-dir", default="", metavar="DIR",
                   help="where the staged helper must land (the run's testdir)")
    p.add_argument("--work-dir", default="", metavar="DIR",
                   help="scratch for the build outputs and their logs")
    p.add_argument("--dss-config", default="release", metavar="debug|release",
                   help="the --config= DSS is given for the helper")
    p.add_argument("--reference-cc", default="", metavar="CC",
                   help="the VERIFIED target compiler for the control arm, or "
                        "empty when this host has none (never a guess: it is "
                        "whatever --resolve-target-cc accepted)")
    p.add_argument("--reference-machine", default="", metavar="TRIPLE",
                   help="the triple that compiler reported, for the log")
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
    # ── the FOURTH, PER-LEG Tcl coherence check ────────────────────────────
    # [D-HARNESS-TCL-HEADER-IS-HOST-CHOSEN-WHILE-EVERY-LEG-LIBRARY-IS-PINNED]
    # The other three Tcl checks live in the drivers and are all HOST-scoped.
    # This one is per-LEG, so it lives here, where both drivers can call it and
    # neither can drift from the other.
    p.add_argument("--tcl-coherence", action="store_true",
                   help="verify the STAGED Tcl header against the Tcl library "
                        "EACH LEG WILL LINK. Prints '<label>\\t<version|?>\\t"
                        "<how it was measured>\\t<path>' per leg on stdout; "
                        "warnings and the refusal go to stderr. rc 0 coherent, "
                        "5 INCOHERENT, 2 the header could not be read. A leg "
                        "whose version cannot be measured is a WARNING and is "
                        "skipped — never a silent pass.")
    p.add_argument("--staged-tcl-header", default="", metavar="PATH",
                   help="the tcl.h the run actually stages (its TCL_VERSION is "
                        "read from the FILE, not from the directory's name)")
    p.add_argument("--leg-tcl-library", action="append", default=None,
                   metavar="LABEL=PATH",
                   help="a leg and the libtcl it resolved. Repeatable — pass "
                        "every leg that resolved one, so a run whose legs "
                        "disagree WITH EACH OTHER is caught as well.")
    p.add_argument("--probe-environment", action="store_true",
                   help="RUN the declared environment-defect probes IN THIS "
                        "PROCESS'S OWN KERNEL and print the verdicts as JSON. The "
                        "measurement half of the confound gate: a probe answers "
                        "present/absent/indeterminate, and only `present` lets a "
                        "row that requires it be honoured. ★ IN-PROCESS ONLY, "
                        "NEVER ORCHESTRATING: --plan re-enters this script with "
                        "this flag inside each kernel a leg executes in, so a "
                        "flag that measured elsewhere could recurse. It is also "
                        "the instrument an operator runs by hand "
                        "(`wsl.exe -e python3 harness_legs.py "
                        "--probe-environment`) to check what --plan concluded. "
                        "[D-HARNESS-CONFOUND-SCOPE-IS-A-RUN-MODE-NOT-A-HOST]")
    p.add_argument("--print-probe-budget", action="store_true",
                   help="print, WITHOUT sampling anything, how many seconds a "
                        "--probe-environment run of the SAME probes may take: "
                        "{probeEnvironmentSeconds, noSampleSeconds} as JSON. ★ "
                        "FOR A CALLER THAT HAS TO BOUND THIS SCRIPT'S OWN SPAWN. "
                        "The budget is DERIVED from the catalogue's declared "
                        "sample windows, so a caller that types its own number "
                        "kills a healthy child the day a window is widened — and "
                        "reports it as a defect in the probe. Honours "
                        "--probe-only exactly as --probe-environment does, "
                        "because the same lines compute both. "
                        "[D-HARNESS-ENV-PROBE-TEST-TIMEOUT-IS-A-MAGIC-NUMBER-NOT-"
                        "THE-DERIVED-BUDGET]")
    p.add_argument("--probe-only", action="append", default=None, metavar="NAME",
                   help="restrict --probe-environment to these declared probes. "
                        "Repeatable. Used by --plan when it measures a kernel, so "
                        "the child asks exactly the probes some leg IN THAT "
                        "KERNEL requires — the same `only` set the in-process arm "
                        "uses, so neither arm can quietly measure more or less "
                        "than the other.")
    p.add_argument("--probe-verdicts", default=None, metavar="FILE",
                   help="read probe verdicts from FILE instead of measuring. The "
                        "file maps a KERNEL to the verdicts measured in it "
                        "({\"driver\": {...}, \"wsl-linux\": {...}}, each inner "
                        "object the JSON --probe-environment prints); the old flat "
                        "shape is refused, because which kernel a verdict "
                        "describes is what decides whether it may excuse a leg. "
                        "For a caller that already probed, and for tests. Without "
                        "it --plan MEASURES. "
                        "[D-HARNESS-ENVIRONMENT-PROBE-MEASURES-THE-DRIVERS-KERNEL-"
                        "NOT-THE-LAUNCHED-ONE]")
    p.add_argument("--environment-probes", default="measure",
                   choices=("measure", "skip"), metavar="measure|skip",
                   help="`skip` resolves a plan WITHOUT measuring, which marks "
                        "it `confoundGating: unprobed`; every conditional "
                        "confound row is then INACTIVE and both drivers REFUSE "
                        "to run a corpus on it. For structural callers only.")
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
    p.add_argument("--run-filesystems", action="store_true",
                   help="print the closed runFilesystem vocabulary, one verb "
                        "per line (the drivers echo it; the pins assert it)")
    p.add_argument("--run-fidelities", action="store_true",
                   help="print the closed RUN FIDELITY vocabulary, one value per "
                        "line — what KIND of evidence a leg's run produces here "
                        "(native / foreign-kernel / emulated), as opposed to "
                        "`mode`, which says only HOW the artefact is reached. "
                        "Both drivers VALIDATE the operator's DSS_RUN_FIDELITY "
                        "against this list at the door rather than carrying a "
                        "second copy of it "
                        "[D-HARNESS-RUN-FIDELITY-IS-COMPUTED-BUT-NEITHER-"
                        "RECORDED-NOR-SELECTABLE].")
    p.add_argument("--run-dir-plan", default=None, metavar="LABEL",
                   help="resolve WHERE this leg's corpus runs: the directory in "
                        "the LAUNCHER's own filesystem, the launcher argv with "
                        "its working-directory option spliced in, and the argv "
                        "prefixes that create/clear/populate it. Needs "
                        "--driver-run-dir. JSON on stdout. "
                        "D-HARNESS-WSL-LAUNCHED-LEG-RUNDIR-IS-DRVFS")
    p.add_argument("--driver-run-dir", default="", metavar="DIR",
                   help="this driver's OWN run directory for the leg, as this "
                        "driver spells it (--run-dir-plan)")
    # ── what a launcher needs BEYOND its own argv[0] ────────────────────────
    p.add_argument("--check-launcher", default=None, metavar="LABEL",
                   help="EXECUTE this leg's declared launcher prerequisites on "
                        "THIS machine (legs.json `launchers[].requires`) and "
                        "print the result as JSON: {ok, verdict, missing[]}. "
                        "rc 0 met, 3 unmet. The plan stays pure — it RESOLVES "
                        "the rows (paths expanded over the entry's own `env`, "
                        "probe argv built from its own runFilesystem) and this "
                        "runs them. Closes the hole where "
                        "`shutil.which('wsl.exe')` answered for a launcher whose "
                        "real argv[0] is `qemu-aarch64` INSIDE the distro.")
    p.add_argument("--artifact", default="", metavar="PATH",
                   help="with --check-launcher: ALSO cross-check the built "
                        "binary's own PT_INTERP and DT_NEEDED against the "
                        "declared rows, so the declaration cannot silently "
                        "shrink below what the artefact demands. ⚠ It can never "
                        "GROW the list either: libgcc_s.so.1 is dlopen()ed by "
                        "glibc at pthread_exit and appears in no DT_NEEDED, "
                        "which is why the rows are declared prose with evidence "
                        "rather than a computed set.")
    p.add_argument("--identify-binary", default=None, metavar="PATH",
                   help="print '<arch>\\t<container>\\t<targetOs>' read from the "
                        "binary's OWN header (ELF e_machine + EI_OSABI, PE "
                        "Machine, Mach-O cputype). No external tool. rc 3 and a "
                        "named diagnostic on bytes it cannot identify — never a "
                        "default, because the caller is deciding whether a "
                        "binary that would not run is this compiler's fault.")
    p.add_argument("--launcher-for-target", default=None,
                   metavar="ARCH:CONTAINER:TARGETOS",
                   help="print the launcher argv THIS host runs that target's "
                        "binaries with, shlex-quoted exactly as a plan's "
                        "LEG_LAUNCH. Takes the triple --identify-binary prints, "
                        "so the two pipe together. Empty stdout + a NAMED reason "
                        "on stderr when the leg runs natively (rc 0) or when no "
                        "leg matches / this host cannot run it (rc 3).")
    args = p.parse_args(argv)

    if not (args.verdict_vocabulary or args.plan or args.lint or args.self_test
            or args.header_stages or args.config_stages or args.path_translations
            or args.translate_path or args.assert_translated
            or args.env_transfers or args.env_transfer
            or args.acquire or args.acquire_plan or args.resolve_library_argv
            or args.resolve_target_cc or args.build_loadext_helper
            or args.oracle_report or args.build_reference_oracle
            or args.classify_abort or args.attribute_build
            or args.loadext_builder or args.tcl_coherence
            or args.run_filesystems or args.run_fidelities or args.run_dir_plan
            or args.stage_build or args.check_launcher or args.identify_binary
            or args.launcher_for_target
            or args.registry_controls or args.check_regions
            or args.probe_environment or args.print_probe_budget):
        p.error("one of --verdict-vocabulary / --plan / --probe-environment / "
                "--print-probe-budget / "
                "--header-stages / "
                "--config-stages / --stage-build / --lint "
                "/ --self-test / --path-translations / --translate-path / "
                "--assert-translated / --env-transfers / --env-transfer / "
                "--registry-controls / --check-regions / "
                "--acquire / --acquire-plan / --resolve-library-argv / "
                "--resolve-target-cc / --oracle-report / --classify-abort / "
                "--attribute-build / "
                "--build-reference-oracle / --build-loadext-helper / "
                "--loadext-builder / --tcl-coherence / --run-filesystems / "
                "--run-fidelities / "
                "--run-dir-plan / --check-launcher / --identify-binary / "
                "--launcher-for-target is required")
    if args.artifact and not args.check_launcher:
        p.error("--artifact is the 4-D cross-check of --check-launcher (the "
                "artefact's own PT_INTERP/DT_NEEDED against the declared rows) "
                "and means nothing without it")
    if args.run_dir_plan and not args.driver_run_dir:
        p.error("--run-dir-plan requires --driver-run-dir <dir> — the launcher's "
                "run directory is DERIVED from this driver's own so that two "
                "checkouts cannot collide in one shared /tmp, and inventing one "
                "here would make the answer depend on who asked")
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
        if args.check_regions:
            counts = check_dss_regions(HERE)
            return 0 if counts["failed"] == 0 else 1
        if args.registry_controls:
            # OUTSIDE the LegError contract on purpose — this mode has no
            # failure mode by design, so it is answered before anything that
            # could raise and it returns 0 on every path.
            lines, note = registry_controls(args.registry_controls,
                                            args.for_legs or [],
                                            args.for_tests or [])
            for line in lines:
                sys.stdout.write("%s\n" % line)
            if note:
                sys.stdout.write("(%s)\n" % note)
            return 0
        if args.env_transfers:
            for verb in sorted(ENV_TRANSFERS):
                sys.stdout.write("%s\t%s\n"
                                 % (verb, ENV_TRANSFERS[verb]["nameCarrier"]))
            return 0
        if args.env_transfer:
            # `--forward` is the NAMESPACE-NEUTRAL door and it stays that way:
            # a name whose declared kind is not `opaque` (or which has no
            # declared kind at all) is refused HERE, naming --forward-path, so
            # the raw-path forward cannot be spelled at the CLI either.
            plain = []
            for n in (args.forward or []):
                kind = forward_kind(n)
                if kind != "opaque":
                    raise LegError(
                        "--forward %s: that variable is declared %r, not "
                        "'opaque'. Its value is a path in THIS driver's "
                        "namespace, so it must cross as "
                        "`--forward-path %s=<value>` (with "
                        "--path-translation), which translates it. Forwarding "
                        "it by name alone would hand the launched process a "
                        "path from the wrong namespace."
                        % (n, kind, n))
                plain.append((n, ""))
            for spec in (args.forward_path or []):
                if "=" not in spec:
                    raise LegError(
                        "--forward-path expects NAME=VALUE, got %r. The VALUE "
                        "is required: it is the path being translated."
                        % spec)
                nm, _, val = spec.partition("=")
                plain.append((nm, val))
            for line in launch_forward_assignments(
                    args.env_transfer, args.path_translation, plain,
                    args.forward_declared or [], args.carrier_current):
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
        if args.oracle_report:
            leg = leg_by_label(load_catalogue(args.catalogue),
                               args.oracle_report, args.catalogue)
            leg_oracle = None
            if args.leg_oracle:
                leg_oracle = {"path": args.leg_oracle, "cc": args.leg_oracle_cc,
                              "triple": args.leg_oracle_triple}
            # The resolver ladder is part of the ANSWER when there is no oracle:
            # "no oracle" without "and here is every compiler this host was asked
            # for" is a verdict a reader cannot act on. Only asked for when it is
            # going to be printed, so a leg that HAS an oracle spawns nothing.
            resolver = None
            if not leg_oracle:
                cls, _why = oracle_class_for_leg(leg, args.reference_target,
                                                 args.reference_path,
                                                 args.oracle_status)
                if cls != "same-platform":
                    resolver = resolve_target_cc(leg)
            for line in oracle_report_lines(leg, args.reference_target,
                                            args.reference_path, leg_oracle,
                                            resolver, args.oracle_status):
                sys.stdout.write("%s\n" % line)
            return 0
        if args.build_reference_oracle:
            leg = leg_by_label(load_catalogue(args.catalogue),
                               args.build_reference_oracle, args.catalogue)
            if bool(args.oracle_output) == bool(args.oracle_dir):
                p.error("--build-reference-oracle needs exactly one of "
                        "--oracle-dir (preferred: the catalogue names the file) "
                        "or --oracle-output (an explicit path)")
            oracle_output = args.oracle_output or os.path.join(
                args.oracle_dir, reference_oracle_name(leg))
            for flag, value in (("--manifest", args.manifest),
                                ("--oracle-log", args.oracle_log)):
                if not value:
                    p.error("--build-reference-oracle requires %s" % flag)
            cc, machine, rejections = resolve_target_cc(leg)
            for line in rejections:
                sys.stderr.write("  rejected %s\n" % line)
            if not cc:
                # rc 4 is NOT a failure of this run — it is the honest statement
                # that this host cannot produce a control for this leg. The leg
                # then reports NO ORACLE, which is the whole point of the anchor.
                sys.stderr.write(
                    "no declared targetCc candidate for leg '%s' (%s) both "
                    "exists on this host AND targets it, so NO same-platform "
                    "attribution oracle can be built here. The leg reports that "
                    "it HAS NO ORACLE rather than inheriting another platform's "
                    "reference [D-HARNESS-PE64-HAS-NO-SAME-PLATFORM-ORACLE].\n"
                    % (leg.get("label"), leg.get("spec")))
                sys.stdout.write(json.dumps(
                    {"status": "no-reference-compiler",
                     "leg": leg.get("label"), "spec": leg.get("spec"),
                     "rejections": rejections}) + "\n")
                return 4
            with open(args.manifest, "r", encoding="utf-8") as fh:
                manifest = json.load(fh)
            argv = reference_oracle_argv(
                cc, manifest, oracle_output,
                leg.get("build", {}).get("referenceLinkFlags", []))
            import subprocess
            with open(args.oracle_log, "w", encoding="utf-8") as log:
                log.write("%s\n\n" % " ".join(argv))
                log.flush()
                # rc DIRECTLY off the process, never after a pipe.
                proc = subprocess.run(argv, stdout=subprocess.PIPE,
                                      stderr=subprocess.STDOUT)
                log.write(proc.stdout.decode("utf-8", "replace"))
            built = proc.returncode == 0 and os.path.isfile(oracle_output)
            report = {"status": "built" if built else "build-failed",
                      "leg": leg.get("label"), "spec": leg.get("spec"),
                      "cc": cc, "triple": machine, "rc": proc.returncode,
                      "path": oracle_output if built else "",
                      "log": args.oracle_log, "sources": len(manifest.get("sources", [])),
                      "rejections": rejections}
            sys.stdout.write(json.dumps(report) + "\n")
            if not built:
                # LOUD, and it does not degrade to the cross-platform reference:
                # a control that failed to build is an absent control, and saying
                # so is the entire discipline this anchor enforces.
                sys.stderr.write(
                    "the same-platform oracle for leg '%s' did NOT build (%s "
                    "exited %d). The leg reports NO ORACLE; read %s.\n"
                    % (leg.get("label"), cc, proc.returncode, args.oracle_log))
                return 3
            return 0
        if args.loadext_builder:
            sys.stdout.write("%s\n"
                             % loadext_helper_builder(args.helper_builder))
            return 0
        if args.build_loadext_helper:
            leg = leg_by_label(load_catalogue(args.catalogue),
                               args.build_loadext_helper, args.catalogue)
            # REQUIRED, and refused by name rather than defaulted: every one of
            # these is a path only the calling driver knows, and a helper built
            # against a guessed include root silently resolves a SYSTEM
            # sqlite3.h of an unrelated version — the very fall-through
            # build-and-test.sh's staging comment warns about.
            for flag, value in (("--dss", args.dss),
                                ("--sqlite-src", args.sqlite_src),
                                ("--sqlite-bld", args.sqlite_bld),
                                ("--dest-dir", args.dest_dir),
                                ("--work-dir", args.work_dir)):
                if not value:
                    p.error("--build-loadext-helper requires %s" % flag)
            report = build_loadext_helper(
                leg, args.dss, args.sqlite_src, args.sqlite_bld, args.dest_dir,
                args.work_dir, dss_config=args.dss_config,
                builder=args.helper_builder,
                reference_cc=args.reference_cc,
                reference_machine=args.reference_machine)
            sys.stdout.write(json.dumps(report, indent=1, sort_keys=True) + "\n")
            # The REPORT is on stdout on every outcome — a driver needs the
            # detail most when it failed — and the rc names the verdict CLASS so
            # a caller never has to classify prose. 3 and 4 sit apart from the
            # LegError rc 2 above and from --resolve-target-cc's 3.
            return {"": 0, "poisoned": 3,
                    "skipped-build-input-missing": 4}[report["verdictClass"]]
        if args.acquire or args.acquire_plan:
            label = args.acquire or args.acquire_plan
            legs = load_catalogue(args.catalogue)
            root = cache_root(args.cache_root)
            leg = leg_by_label(legs, label, args.catalogue)
            if args.acquire_plan:
                result = acquire_plan(leg, root)
            else:
                # ★ THE RESULT IS PRINTED ON BOTH OUTCOMES, and it is the SAME
                # RECORD SHAPE either way — D-HARNESS-PINNED-ARCHIVE-FAILURE-
                # RETURN-OMITS-ACQUIRED: "a function whose SUCCESS return and
                # FAILURE return carry different field sets is a silent-omission
                # generator". A driver needs `scriptLibraryDir` most when
                # acquisition has just failed and it is reporting why, so the
                # failure record carries it too, computed from the PURE plan.
                # The rc still names the failure; nothing here makes a failure
                # look like a success.
                try:
                    result = acquire(leg, root, offline=args.offline)
                except LegError as exc:
                    try:
                        failed = acquisition_record(acquire_plan(leg, root),
                                                    error=str(exc))
                    except LegError:
                        # The PLAN itself is unbuildable, so there is no record
                        # to print. Let the original refusal through unchanged
                        # rather than replacing it with a second one.
                        raise exc
                    sys.stdout.write(
                        json.dumps(failed, indent=1, sort_keys=True) + "\n")
                    raise
            sys.stdout.write(json.dumps(result, indent=1, sort_keys=True) + "\n")
            return 0
        if args.header_stages:
            for key, guards in header_stages(load_catalogue(args.catalogue)).items():
                sys.stdout.write("%s\t%s\n" % (key, " ".join(
                    "%s=%d" % (n, 1 if v else 0) for n, v in sorted(guards.items()))))
            return 0
        if args.config_stages:
            for key, answers in configure_stages(load_catalogue(args.catalogue)).items():
                sys.stdout.write("%s\t%s\n" % (key, " ".join(
                    "%s=%d" % (n, 1 if v else 0) for n, v in sorted(answers.items()))))
            return 0
        if args.stage_build:
            sb = stage_build(args.catalogue)
            if args.format == "sh":
                sys.stdout.write(stage_build_sh(sb))
            else:
                sys.stdout.write(json.dumps(sb, indent=1, sort_keys=True) + "\n")
            return 0
        if args.tcl_coherence:
            if not args.staged_tcl_header:
                p.error("--tcl-coherence requires --staged-tcl-header <path to "
                        "the tcl.h this run stages>")
            header_version = tcl_header_version(args.staged_tcl_header)
            if not header_version:
                # NOT a pass. Every leg's library is pinned; if the one thing
                # chosen from the HOST cannot even state its version, the
                # comparison this check exists for cannot be made at all.
                raise LegError(
                    "the staged Tcl header %s declares no `#define TCL_VERSION "
                    "\"x.y\"` (or could not be read). That is the ONE Tcl input "
                    "this harness takes from the host rather than from a leg's "
                    "own declaration, so it cannot be checked against the "
                    "libraries the legs will link "
                    "[D-HARNESS-TCL-HEADER-IS-HOST-CHOSEN-WHILE-EVERY-LEG-"
                    "LIBRARY-IS-PINNED]." % _fwd(args.staged_tcl_header))
            entries = []
            for raw in (args.leg_tcl_library or []):
                label, sep, path = raw.partition("=")
                if not sep or not label or not path:
                    p.error("--leg-tcl-library takes LABEL=PATH, got %r" % raw)
                entries.append((label, path, tcl_library_facts_at(path)))
            ok, lines, warnings, fatal = tcl_coherence(header_version, entries)
            sys.stdout.write("staged-header\t%s\t%s\n"
                             % (header_version, _fwd(args.staged_tcl_header)))
            for line in lines:
                sys.stdout.write("%s\n" % line)
            for w in warnings:
                sys.stderr.write("WARN: %s\n" % w)
            if not ok:
                sys.stderr.write("harness_legs.py: Tcl INCOHERENCE: %s\n" % fatal)
                return 5
            return 0
        if args.lint:
            findings = lint(args.catalogue)
            for f in findings:
                sys.stdout.write("LINT: %s\n" % f)
            sys.stdout.write("findings=%d\n" % len(findings))
            return 1 if findings else 0
        if args.self_test:
            return self_test(args.catalogue)
        if args.run_filesystems:
            sys.stdout.write("\n".join(sorted(RUN_FILESYSTEMS)) + "\n")
            return 0
        if args.run_fidelities:
            # DECLARATION ORDER, not sorted: the values are ordered by how much
            # they prove (native > foreign-kernel > emulated), and a reader
            # picking a floor for a run needs that order, not the alphabet.
            sys.stdout.write("\n".join(RUN_FIDELITIES) + "\n")
            return 0
        if args.identify_binary:
            # Host-free: it reads bytes. Deliberately OUTSIDE the LegError
            # handler's rc 2 — an unidentifiable binary is this subcommand's
            # own named outcome (rc 3), not a catalogue defect, and a caller
            # that has to tell the two apart cannot do it from one code.
            try:
                with open(args.identify_binary, "rb") as fh:
                    blob = fh.read()
            except OSError as exc:
                sys.stderr.write("harness_legs.py: cannot read %s: %s\n"
                                 % (_fwd(args.identify_binary), exc))
                return 3
            try:
                arch, container, target_os = binary_target_identity(blob)
            except LegError as exc:
                sys.stderr.write("harness_legs.py: %s: %s\n"
                                 % (_fwd(args.identify_binary), exc))
                return 3
            sys.stdout.write("%s\t%s\t%s\n" % (arch, container, target_os))
            return 0

        if args.launchers_none and args.launchers_available is not None:
            p.error("--launchers-none and --launchers-available are exclusive")
        available = None
        if args.launchers_none:
            available = set()
        elif args.launchers_available is not None:
            available = {x for x in args.launchers_available.split(",") if x}
        host_os = canon_os(args.host_os) if args.host_os else detect_host_os()
        host_arch = canon_arch(args.host_arch) if args.host_arch else detect_host_arch()
        if args.run_dir_plan:
            legs = load_catalogue(args.catalogue)
            leg = leg_by_label(legs, args.run_dir_plan, "--run-dir-plan")
            json.dump(run_dir_plan(leg, host_os, host_arch, available,
                                   args.driver_run_dir), sys.stdout, indent=2)
            sys.stdout.write("\n")
            return 0
        if args.check_launcher:
            legs = load_catalogue(args.catalogue)
            leg = leg_by_label(legs, args.check_launcher, "--check-launcher")
            report = check_launcher(leg, host_os, host_arch, available,
                                    artifact=args.artifact or None)
            # The REPORT on stdout on BOTH outcomes — a driver needs the rows
            # most when one of them is missing — and the rc names the outcome so
            # a caller never classifies prose. rc 3 sits with --resolve-target-cc's
            # and --build-loadext-helper's, apart from the LegError rc 2.
            sys.stdout.write(json.dumps(report, indent=1, sort_keys=True) + "\n")
            return 0 if report["ok"] else 3
        if args.launcher_for_target:
            legs = load_catalogue(args.catalogue)
            argv_, reason = launcher_for_target(legs, args.launcher_for_target,
                                                host_os, host_arch, available)
            sys.stderr.write("harness_legs.py: %s\n" % reason)
            if argv_ is None:
                return 3
            # Quoted EXACTLY as emit_sh's LEG_LAUNCH, so a driver that already
            # consumes that variable consumes this identically — a second
            # quoting convention for the same argv is a silent harness bug.
            # A NATIVE run prints NOTHING (its reason is on stderr) rather than
            # an empty line: "" and "\n" are different answers to `read`.
            if argv_:
                sys.stdout.write(" ".join(shlex.quote(x) for x in argv_) + "\n")
            return 0
        # ── THE MEASUREMENT HALF ────────────────────────────────────────────
        # Probing is an IMPURE act and it lives out here at the CLI boundary,
        # exactly where launcher availability is resolved before being injected
        # into the pure planner.
        # [D-HARNESS-CONFOUND-SCOPE-IS-A-RUN-MODE-NOT-A-HOST]
        #
        # ★★★ --probe-environment IS THE IN-PROCESS INSTRUMENT AND IT IS SERVED
        # FIRST, BEFORE ANY ORCHESTRATION. It measures the kernel it is RUNNING in
        # and nothing else, which is what makes it safe for --plan to re-enter this
        # script with it inside another kernel: the child cannot spawn a
        # grandchild, so "how many processes does a plan start" is answerable by
        # reading one branch. It is also, unchanged, the command an operator runs
        # by hand through `wsl.exe -e` to check a plan's conclusion against.
        # [D-HARNESS-ENVIRONMENT-PROBE-MEASURES-THE-DRIVERS-KERNEL-NOT-THE-LAUNCHED-ONE]
        #
        # ★★★ AND `--print-probe-budget` IS SERVED FROM INSIDE THIS SAME BRANCH,
        # WHICH IS THE WHOLE POINT OF IT.
        # [D-HARNESS-ENV-PROBE-TEST-TIMEOUT-IS-A-MAGIC-NUMBER-NOT-THE-DERIVED-BUDGET]
        #
        # A CALLER THAT SPAWNS THIS SCRIPT HAS TO BOUND IT, and the number it
        # needs is one this file already computes: `kernel_probe_budget_seconds`
        # is what --plan hands its OWN --probe-environment children. The C++ gate
        # spawned the same child under a deadline typed into the test instead
        # (120 000 ms), so the two could disagree — and did, on a Darwin host,
        # where the caller SIGKILLed a probe that was doing exactly the work this
        # catalogue asked for and reported it as `child timed out`, i.e. as a
        # defect in the measurement rather than in the caller's arithmetic.
        # ⇒ THE SCRIPT OWNS THE NUMBER AND THE CALLER ASKS FOR IT. One
        # computation, one owner, and tightening a `sampleSeconds` retightens
        # every caller's deadline with no second edit anywhere.
        #
        # ⚠ IT IS PRICED BY THE LINES THAT PERFORM IT, deliberately: the two
        # doors, the catalogue load and the `only` narrowing below are shared, so
        # the budget printed is the budget of THIS code path and cannot describe
        # a different invocation than the one that runs. A separate arm computing
        # `only` its own way would be the two-derivations defect one flag along.
        if args.probe_environment or args.print_probe_budget:
            if args.probe_environment and args.print_probe_budget:
                # One PRICES the measurement, the other PERFORMS it. Answering
                # both would print a number under the name of a measurement, and
                # the caller asking for a deadline would get no verdicts while
                # believing it had measured this kernel.
                raise LegError(
                    "--print-probe-budget prices a measurement and "
                    "--probe-environment performs one. Ask for one of them: the "
                    "budget first, then the measurement under it.")
            if args.environment_probes != "measure":
                # A caller asking to measure and not to measure. Refused rather
                # than silently answered one way.
                raise LegError(
                    "--probe-environment / --print-probe-budget with "
                    "--environment-probes skip asks for a measurement (or for "
                    "its price) and forbids measuring. Drop one of them.")
            if args.probe_verdicts:
                # Same refusal, the other door. Printing a FILE's contents from
                # the flag whose whole meaning is "this machine, now" is how a
                # replayed verdict gets mistaken for a measurement.
                raise LegError(
                    "--probe-environment MEASURES this kernel and "
                    "--print-probe-budget prices that measurement; "
                    "--probe-verdicts READS a file. Asking for both would print "
                    "somebody else's answer under the name of a measurement. "
                    "[D-HARNESS-PROBE-VERDICTS-FLAG-INJECTS-AN-UNVALIDATED-"
                    "PRESENT]")
            doc = load_catalogue_doc(args.catalogue)
            only = required_probe_names(load_catalogue(args.catalogue))
            if args.probe_only is not None:
                declared = environment_probes(doc)
                unknown = [n for n in args.probe_only if n not in declared]
                if unknown:
                    raise LegError(
                        "--probe-only names %s, which the catalogue's "
                        "`environmentProbes` registry does not declare (declared: "
                        "%s). A probe nobody declared measures nothing, and a "
                        "typo'd name would silently narrow the measurement to "
                        "NOTHING while still printing a successful-looking map."
                        % (", ".join(repr(n) for n in unknown),
                           ", ".join(sorted(declared)) or "<none>"))
                only = set(args.probe_only)
            if args.print_probe_budget:
                # TWO NUMBERS, because a caller has two kinds of spawn to bound
                # and deriving the second one itself is how the first got typed.
                # `noSampleSeconds` is this same function at a ZERO window —
                # identically RESOLVER_SPAWN_BUDGET_SECONDS, which --self-test
                # asserts — and it is what EVERY invocation of this script that
                # samples nothing may take. A caller that fetched only the
                # sampling budget would still be typing the other one.
                json.dump({"probeEnvironmentSeconds":
                               kernel_probe_budget_seconds(doc, only),
                           "noSampleSeconds":
                               kernel_probe_budget_seconds(doc, [])},
                          sys.stdout, indent=2, sort_keys=True)
                sys.stdout.write("\n")
                return 0
            json.dump(run_environment_probes(doc, only=only), sys.stdout,
                      indent=2, sort_keys=True)
            sys.stdout.write("\n")
            # rc 0 whatever the verdicts are: ABSENT is a successful measurement,
            # not a failure. A non-zero rc for "your clock is fine" would teach a
            # driver to treat a healthy machine as a broken run.
            return 0
        if args.probe_only is not None:
            raise LegError(
                "--probe-only narrows a measurement and only --probe-environment "
                "measures on demand; --plan derives its own per-kernel probe set "
                "from the rows each leg declares. A flag that silently did "
                "nothing here would look like it had narrowed something.")
        # ★★★ ONE MEASUREMENT PER KERNEL, AND WHICH KERNELS COMES FROM THE PLAN.
        # Resolved TWICE on purpose: once with no verdicts to learn which kernel
        # each leg's fixture executes in (pure, cheap, and unaffected by verdicts —
        # they only ever decide confound rows), then once more with the answers.
        # Deriving the kernel set any other way would mean a second implementation
        # of launcher resolution, which is how one ledger becomes two.
        verdicts = None
        if args.probe_verdicts:
            try:
                with open(args.probe_verdicts, "r", encoding="utf-8") as fh:
                    verdicts = json.load(fh)
            except (OSError, ValueError) as exc:
                raise LegError(
                    "--probe-verdicts %s could not be read (%s). A caller that "
                    "asked for measured gating and got an unreadable file must "
                    "stop: falling back to 'measure it again' would answer a "
                    "different question than the one asked, and falling back to "
                    "'no probes' would silently drop every conditional excusal."
                    % (_fwd(args.probe_verdicts), exc))
            # ★★ VALIDATED, AND STAMPED AS INJECTED. An unvalidated map reached
            # three frames deeper and raised python's own `ValueError`/`KeyError`
            # in the one place whose job is to say what this harness believes
            # about a machine; and nothing distinguished a verdict this process
            # measured from one somebody typed. Both fixed at the door.
            # [D-HARNESS-PROBE-VERDICTS-FLAG-INJECTS-AN-UNVALIDATED-PRESENT]
            verdicts = validate_probe_verdicts(
                verdicts, load_catalogue_doc(args.catalogue),
                "--probe-verdicts %s" % _fwd(args.probe_verdicts))
        elif args.environment_probes == "measure":
            doc = load_catalogue_doc(args.catalogue)
            # ★ WHETHER and WHAT are two different questions and conflating them
            # is the one variation that breaks — resolved_kernel_measurements
            # owns both, so the `if` is pinned rather than living here where only
            # a wall clock could catch its removal. `--classify-abort LABEL`
            # consults exactly that leg; every other invocation emits the WHOLE
            # plan and therefore consults all of them.
            verdicts = resolved_kernel_measurements(
                plan(host_os, host_arch, available, args.catalogue)["legs"],
                ({args.classify_abort or args.attribute_build}
                 if (args.classify_abort or args.attribute_build) else None),
                lambda needs: measure_kernel_environments(
                    doc, needs, os.path.abspath(__file__),
                    os.path.abspath(args.catalogue)))
        if args.classify_abort:
            if not args.abort:
                p.error("--classify-abort requires --abort '<permutation>/<file>'")
            if not args.abort_log:
                p.error("--classify-abort requires --abort-log <segment log>. "
                        "The abort's LOCATION alone cannot IDENTIFY it, and a "
                        "row keyed on the file would excuse any failure in that "
                        "file — the defect this flag closes "
                        "[D-HARNESS-ABORT-CONFOUND-KEYED-ON-LOCATION-NOT-IDENTITY].")
            try:
                with open(args.abort_log, "r", encoding="utf-8",
                          errors="replace") as fh:
                    _diag = abort_diagnostic_text(fh.read())
            except OSError as exc:
                # UNREADABLE IS NOT EXCUSED, same direction as an empty log.
                sys.stderr.write(
                    "the aborted segment's log %s could not be read (%s), so "
                    "this abort cannot be identified and stays UNEARNED — it "
                    "fails the leg.\n" % (args.abort_log, exc))
                return 3
            # ★ RESOLVED THROUGH `plan()`, NOT THROUGH A SECOND GATE. The rows
            # this consults must be the SAME rows, decided by the SAME probe
            # gate, that the driver was handed for its unit matcher — a private
            # gate here is how one ledger becomes two that disagree.
            _resolved = plan(host_os, host_arch, available, args.catalogue,
                             verdicts)
            _leg = None
            for _l in _resolved["legs"]:
                if _l["label"] == args.classify_abort:
                    _leg = _l
                    break
            if _leg is None:
                raise LegError(
                    "--classify-abort names leg %r, which this host's plan does "
                    "not contain (planned: %s)"
                    % (args.classify_abort,
                       ", ".join(l["label"] for l in _resolved["legs"])))
            # ONE matcher — the same function the self-test drives — handed the
            # decisions THIS host's plan resolved. A second inline loop here is
            # how the CLI and the pinned rule come to disagree.
            _row, _why = classify_abort_decisions(
                _leg.get("confoundDecisions", []), args.classify_abort,
                args.abort, _diag)
            if _row is None:
                sys.stderr.write("%s\n" % _why)
                return 3
            # The PROVENANCE is the answer, not a footnote: an abort that stops
            # failing the leg must arrive with the work that earned it, or the
            # exemption is indistinguishable from the silence it replaced.
            sys.stdout.write("pattern: %s\n" % _row["pattern"])
            sys.stdout.write("abortDiagnostic: %s\n"
                             % _row["row"].get("abortDiagnostic", ""))
            for k in CONFOUND_PROVENANCE_KEYS:
                sys.stdout.write("%s: %s\n" % (k, _row["row"].get(k, "")))
            return 0
        if args.attribute_build:
            for flag, value in (("--compile-log", args.compile_log),
                                ("--oracle-log", args.oracle_log),
                                ("--manifest", args.manifest)):
                if not value:
                    p.error("--attribute-build requires %s" % flag)
            if not args.oracle_status:
                p.error("--attribute-build requires --oracle-status. It is NOT "
                        "defaulted and NOT inferred from whether the oracle log "
                        "exists: a log left behind by a previous run would then "
                        "grant an amnesty this run never earned "
                        "[D-HARNESS-BUILD-FAILURE-HAS-NO-PER-TU-ATTRIBUTION].")

            def _read(path, what):
                """UNREADABLE IS "" AND IT IS REPORTED — never an abort. An empty
                reference log denies every amnesty (the safe direction) and an
                empty dss log attributes nothing; both are visible in the JSON's
                counts, which is what a reader checks."""
                try:
                    with open(path, "r", encoding="utf-8", errors="replace") as fh:
                        return fh.read()
                except OSError as exc:
                    sys.stderr.write("could not read the %s at %s (%s) — it "
                                     "contributes NOTHING, and an absent control "
                                     "excuses nothing\n" % (what, path, exc))
                    return ""

            try:
                with open(args.manifest, "r", encoding="utf-8") as fh:
                    _sources = json.load(fh).get("sources", [])
            except (OSError, ValueError) as exc:
                raise LegError(
                    "--attribute-build could not read the manifest %s (%s). The "
                    "manifest is what proves the reference was asked about a TU "
                    "at all; without it nothing here is attributable and "
                    "guessing the source list is the one thing this must not do."
                    % (args.manifest, exc))
            # ★ THE SAME ROWS, THE SAME GATE — resolved through plan(), exactly as
            # --classify-abort is, and for the identical reason: a private gate
            # here is how one ledger becomes two that disagree.
            _resolved = plan(host_os, host_arch, available, args.catalogue,
                             verdicts)
            _leg = None
            for _l in _resolved["legs"]:
                if _l["label"] == args.attribute_build:
                    _leg = _l
                    break
            if _leg is None:
                raise LegError(
                    "--attribute-build names leg %r, which this host's plan does "
                    "not contain (planned: %s)"
                    % (args.attribute_build,
                       ", ".join(l["label"] for l in _resolved["legs"])))
            _report = attribute_build_failure(
                _read(args.compile_log, "dss compile log"),
                _read(args.oracle_log, "reference oracle log"),
                args.oracle_status, _sources,
                _leg.get("confoundDecisions", []), args.attribute_build)
            # The report lines travel INSIDE the JSON so a driver prints ONE
            # account it did not compose — the same transport build_loadext_helper
            # uses, and the reason the two drivers cannot drift into two stories.
            _report["report"] = build_attribution_report_lines(_report)
            sys.stdout.write(json.dumps(_report, indent=1, sort_keys=True) + "\n")
            return 0 if _report["verdictClass"] == "upstream" else 3
        resolved = plan(host_os, host_arch, available, args.catalogue, verdicts)
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
