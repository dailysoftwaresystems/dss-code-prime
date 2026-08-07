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
#   validHostOs     the only host OS the mechanism exists on, so the lint CHECKS.
RUN_FILESYSTEMS = {
    "driver": {
        "root": "",
        "workingDirArgv": [],
        "mkdirArgv": [],
        "rmTreeArgv": [],
        "copyArgv": [],
        "validHostOs": "",
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
        "validHostOs": "windows",
    },
}


def run_filesystem(verb):
    """The declared verb's spec, or a LegError — never a permissive default.

    Defaulting to `driver` for an unknown verb is exactly the defect this
    vocabulary exists to prevent: `driver` is itself a CLAIM — that the launched
    process writes onto the same filesystem this driver does — and it was the
    unstated, unexamined assumption that put a Linux sqlite corpus onto DrvFs."""
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
    return spec


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
# `scope` reproduces the `native:`/`emulated:` prefix vocabulary the drivers have
# always used, as a FIELD rather than a string prefix an operator has to spell:
#   any        excused however this leg runs.
#   native     excused only when THIS HOST executes the artefact directly.
#   emulated   excused only when it goes through a declared launcher. The name is
#              the operator-facing DSS_CONFOUNDS vocabulary and is deliberately
#              NOT renamed to `launched`: renaming it would silently un-excuse
#              every `emulated:` pattern an operator has ever typed.
CONFOUND_SCOPES = ("any", "native", "emulated")

# Required on EVERY declared pattern, all non-empty. These are the four questions
# a reader must be able to answer without leaving the file: what does it match,
# where was it proven, when, and by what mechanism + which anchor holds the long
# form. A confound is an ASSERTION THAT THE COMPILER IS INNOCENT; it has to show
# its work.
CONFOUND_PROVENANCE_KEYS = ("earnedOn", "earnedAt", "mechanism", "anchor")


def confound_scope_prefix(scope):
    """The `native:`/`emulated:` prefix a scope becomes on the wire, so the two
    drivers' long-standing pattern grammar is produced in ONE place instead of
    being re-spelled in each of them."""
    if scope not in CONFOUND_SCOPES:
        raise LegError(
            "unknown confound scope %r (known: %s). A scope decides whether a "
            "pattern excuses a failure at all; an unrecognised one cannot be "
            "treated as 'any', because 'any' is the widest possible excusal."
            % (scope, ", ".join(CONFOUND_SCOPES)))
    return "" if scope == "any" else scope + ":"


def leg_confounds(leg):
    """This leg's DECLARED confound patterns, in wire form (`emulated:^re`).

    A leg with no `confounds` key RAISES rather than returning []: an empty list
    is a claim a catalogue must make out loud, and a missing key would make
    "nothing was ever earned here" indistinguishable from "this leg predates the
    declaration" — which is the ambiguity the old per-driver lists lived in."""
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
        out.append(confound_scope_prefix(row.get("scope", "any")) + row["pattern"])
    return out

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
    pipe. Kept next to its callers for the same reason `_run_machine_probe` is."""
    import subprocess
    try:
        proc = subprocess.run(argv, capture_output=True, text=True, cwd=cwd)
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
    # `runFilesystem` seeds to "driver" for the same reason `pathTranslation`
    # seeds to "none": on a NATIVE run the process is this machine's own and it
    # writes onto this driver's own filesystem — a claim, and a true one, rather
    # than an absence. Only a LAUNCHER can make it false, and only by declaring so.
    run = {"mode": None, "launcher": [], "env": {}, "verdict": None, "detail": "",
           "pathTranslation": "none", "pathTranslator": [],
           "envTransfer": "inherit", "runFilesystem": "driver"}

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
            # The launcher's FILESYSTEM, declared beside the other two. Same
            # refusal on an unknown verb, same reason: `driver` claims that the
            # launched process writes where this driver writes, and a launcher
            # that crosses into another OS's kernel usually does not.
            fs_verb = entry.get("runFilesystem", "driver")
            run_filesystem(fs_verb)
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
        "confounds": leg_confounds(leg),
        "confoundRows": [dict(r) for r in leg.get("confounds", [])],
        "run": run,
    }


def plan(host_os, host_arch, available, path=CATALOGUE):
    legs = load_catalogue(path)
    return {
        "host": {"os": host_os, "arch": host_arch},
        "legs": [plan_leg(leg, host_os, host_arch, available) for leg in legs],
    }


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
                scope = row.get("scope", "")
                if scope not in CONFOUND_SCOPES:
                    findings.append(
                        "leg '%s': confound %r declares scope %r (known: %s). "
                        "The scope is REQUIRED and never defaulted: 'any' is the "
                        "widest possible excusal and must be chosen, not fallen "
                        "into." % (label, pat, scope, ", ".join(CONFOUND_SCOPES)))
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
    "confound-supply": {"drivers": ["build-and-test.sh"],
                        "verifiers": ["test-confound-scope.sh"]},
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
        "sh": 'corpus_files "$CORPUSDIR"',
        "ps1": "foreach ($f in (Get-CorpusFiles $CORPUSDIR)) { $f }",
    },
    "tier-permutations": {
        "sh": 'tier_permutations "$TIERFILE"',
        "ps1": "foreach ($p in (Get-TierPermutations $TIERFILE)) { $p }",
    },
    "tier-prefixes": {
        "sh": 'tier_prefixes "$PERMSFILE"',
        "ps1": "foreach ($p in (Get-TierPrefixes $PERMSFILE)) { $p }",
    },
    "resolve-abort-file": {
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
    "files-after": {
        "sh": 'files_after "$BOUNDARY" "$LISTFILE"',
        "ps1": ("$corpus = @(Get-Content -LiteralPath $LISTFILE)\n"
                "foreach ($f in (Get-FilesAfter $corpus $BOUNDARY)) { $f }"),
    },
    # THE ONE THAT CARRIES THE PARSING SEMANTICS. Both sides project into the
    # .sh region's own fact alphabet; see the normalisation note above.
    "parse-segment": {
        "sh": ('parse_segment "$LOGFILE" "$FACTFILE"\n'
               'facts F "$FACTFILE"\n'
               'facts X "$FACTFILE" | LC_ALL=C sort -u | sed "s/^/X /"\n'
               'facts B "$FACTFILE" | sed "s/^/B /"\n'
               'for k in S E C P T G N D K Q A; do\n'
               '  printf "%s %s\\n" "$k" "$(fact "$k" "$FACTFILE")"\n'
               'done'),
        "ps1": ("$r = Read-CorpusSegment $LOGFILE\n"
                "foreach ($f in $r.Completed) { $f }\n"
                "foreach ($n in ($r.FailNames.Keys | Sort-Object)) { \"X $n\" }\n"
                "foreach ($b in $r.Blamed) { \"B $b\" }\n"
                "\"S $($r.Summary)\"\n"
                "\"E $(if ($r.Summary) { $r.Errors } else { '' })\"\n"
                "\"C $(if ($r.Summary) { $r.Tests } else { '' })\"\n"
                "\"P $($r.Permutation)\"\n"
                "\"T $($r.LastTest)\"\n"
                "\"G $(if ($r.GaveUp) { '1' } else { '' })\"\n"
                "\"N $($r.Completed.Count)\"\n"
                "\"D $(if ($r.Completed.Count) { $r.Completed[$r.Completed.Count - 1] } else { '' })\"\n"
                "\"K $($r.OkLines)\"\n"
                "\"Q $($r.FailMarkers)\"\n"
                "\"A $($r.Diagnostic)\""),
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
    return {"CORPUSDIR": "corpus", "PERMSFILE": "corpus/permutations.test",
            "TIERFILE": "tier.test", "LISTFILE": "corpus.lst",
            "NAMESFILE": "names.lst", "LOGFILE": "segment.log",
            "FACTFILE": "facts.tsv", "BOUNDARY": "swarmvtab.test",
            "ABORTLOG": "abort-segment.log", "ABORTFACTS": "abort-facts.tsv"}


def _mirror_run(lang, region, case, work, env):
    """(ok, lines, detail) — one copy's answer, or why it could not be had."""
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
        argv = [os.environ.get("BASH", "bash"), rel]
    else:
        rel = "arm_%s.ps1" % case
        head = "".join("$%s = %s\n" % (k, _ps1_single_quote(v))
                       for k, v in sorted(env.items()))
        text = MIRROR_PRELUDE_PS1 + head + region + "\n" + body + "\n"
        argv = ["pwsh", "-NoProfile", "-NonInteractive", "-File", rel]
    script = os.path.join(work, rel)
    with open(script, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(text)
    try:
        proc = subprocess.Popen(argv, stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE, cwd=work)
        out, err = proc.communicate()
        rc = proc.returncode
    except OSError as exc:
        return False, [], "could not run %s: %s" % (argv[0], exc)
    out = out.decode("utf-8", "replace")
    err = err.decode("utf-8", "replace")
    if rc != 0:
        return False, [], ("%s arm exited %s: %s"
                           % (lang, rc, (err.strip() or out.strip())[:400]))
    return True, _mirror_normalise(out), ""


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
                check("pair %s <-> %s: both still defined"
                      % (pair["sh"], pair["ps1"]),
                      pair["sh"] in sh_syms and pair["ps1"] in ps_syms,
                      "sh=%s ps1=%s" % (pair["sh"] in sh_syms,
                                        pair["ps1"] in ps_syms))
            else:
                only = "sh" if pair.get("sh") else "ps1"
                check("single-driver %s `%s` states WHY"
                      % (only, pair.get("sh") or pair.get("ps1")),
                      bool(pair.get("why")),
                      "a capability present in one driver only must say why, or "
                      "it is indistinguishable from one that was forgotten")

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
            unknown = sorted(set(MIRROR_CASES[c]) - {"sh", "ps1", "expect"})
            check("MIRROR_CASES `%s` declares only known keys" % c, not unknown,
                  "unknown key(s): %s" % ", ".join(unknown))
        named = [p["differential"] for p in MIRROR_PAIRS
                 if p.get("differential") and p["differential"] in MIRROR_CASES]
        cases = named + [c for c in sorted(MIRROR_CASES) if c not in named]
        have_pwsh = _sh.which("pwsh")
        have_bash = _sh.which(os.environ.get("BASH", "bash")) or _sh.which("bash")
        if not (have_pwsh and have_bash):
            missing = " and ".join(
                [w for w, h in (("pwsh", have_pwsh), ("bash", have_bash)) if not h])
            for case in cases:
                skip("differential %s" % case,
                     "%s is not on PATH, so the two copies cannot be executed "
                     "on the same input from this host" % missing)
        else:
            work = _tf.mkdtemp(prefix="dss-mirror-")
            try:
                env = _mirror_write_fixtures(work)
                for case in cases:
                    ok_sh, sh_out, d_sh = _mirror_run("sh", sh_text, case, work, env)
                    ok_ps, ps_out, d_ps = _mirror_run("ps1", ps_text, case, work, env)
                    if not (ok_sh and ok_ps):
                        check("differential %s: both copies RAN" % case, False,
                              "; ".join(x for x in (d_sh, d_ps) if x))
                        continue
                    same = sh_out == ps_out
                    detail = ""
                    if not same:
                        detail = ("the two drivers answer DIFFERENTLY on "
                                  "identical input:\n         .sh : %s\n"
                                  "         .ps1: %s"
                                  % (" | ".join(sh_out) or "<empty>",
                                     " | ".join(ps_out) or "<empty>"))
                    check("differential %s: identical answers (%d line(s))"
                          % (case, len(sh_out)), same, detail)
                    # ★ AND, WHERE THE CASE DECLARES ONE, THE RIGHT ANSWER.
                    # Agreement is what a differential battery can offer on its
                    # own, and it is real — but two copies that both answer ""
                    # agree perfectly, and that was the exact state of the
                    # abort-file resolver before this case existed. `expect` is
                    # checked PER ARM so a wrong-but-identical pair reds, and it
                    # names which arm is wrong when only one is.
                    want = MIRROR_CASES[case].get("expect")
                    if want is None:
                        continue
                    for lang, got in (("sh", sh_out), ("ps1", ps_out)):
                        check("differential %s: the .%s answer is CORRECT"
                              % (case, lang), got == want,
                              "expected: %s\n       got:      %s"
                              % (" | ".join(want) or "<empty>",
                                 " | ".join(got) or "<empty>"))
            finally:
                _sh.rmtree(work, ignore_errors=True)

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
    TypeError is a bug in the resolver, not the refusal under test."""
    try:
        thunk()
    except LegError:
        return True
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
    check("every leg's confounds resolve (a missing key is FATAL, never empty)",
          all(isinstance(leg_confounds(l), list) for l in legs))
    check("a leg with no `confounds` key RAISES rather than defaulting to []",
          _raises(lambda: leg_confounds({"label": "x"})))
    check("an unknown confound scope raises rather than meaning 'any'",
          _raises(lambda: confound_scope_prefix("sometimes")))
    check("scope 'any' carries NO prefix (the drivers' bare-pattern grammar)",
          confound_scope_prefix("any") == "")
    check("scope 'emulated' becomes the drivers' `emulated:` prefix",
          confound_scope_prefix("emulated") == "emulated:")
    # ★ THE ASYMMETRY IS THE POINT, AND IT IS ASSERTED RATHER THAN DESCRIBED.
    # A catalogue in which every leg carried the same list would be the global
    # list again, wearing a per-leg shape — so the self-test refuses that.
    _sets = {l["label"]: set(leg_confounds(l)) for l in legs}
    check("the legs do NOT all carry the same confound set",
          len({frozenset(v) for v in _sets.values()}) > 1,
          "identical sets on every leg would be the old global list in a per-leg "
          "costume; got %r" % _sets)
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
    # ⚠ THE ORIGINAL FORM OF THIS PIN ASSERTED THE SET WAS EMPTY, which was a true
    # measurement in TF-C123 and became WRONG in TF-C124: `win32longpath-1.3` was then
    # genuinely EARNED on pe64 UNDER WINE, with a matched control (the same dss-built
    # testfixture.exe runs `win32longpath-1.3... Ok` on REAL Windows — compiler held
    # constant, only the runtime varied). Pinning the empty SET would have made an
    # honestly-earned entry fail the self-test, i.e. it would have punished the very
    # discipline it exists to enforce. So pin the RULE, not the count:
    #   every pe64 confound must be `emulated:`-scoped.
    # That is what keeps a native-Windows failure unexcusable, which is the platform
    # that actually proves this leg.
    _pe = _sets["pe64-x86_64"]
    check("every pe64 confound is emulated-scoped (native Windows stays unexcusable)",
          all(p.startswith("emulated:") for p in _pe),
          "got %r" % _pe)
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
          len(statements) == 1 + len(labels) * 31,
          "got %d statements for %d legs" % (len(statements), len(labels)))
    check("the sh emitter carries the launcher's run FILESYSTEM",
          "LEG_RUN_FILESYSTEM[" in sh,
          "without it build-and-test.sh cannot tell a launcher that shares this "
          "filesystem from one that reaches it through a compatibility mount, "
          "which is D-HARNESS-WSL-LAUNCHED-LEG-RUNDIR-IS-DRVFS")
    check("the sh emitter carries THIS LEG'S earned confounds",
          "LEG_CONFOUNDS[" in sh,
          "without it build-and-test.sh falls back to a global list applied to "
          "every leg, which is D-HARNESS-CONFOUND-LEDGER-IS-PER-DRIVER-NOT-PER-LEG")
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
    def _elf(etype, cls=2):
        return (b"\x7fELF" + bytes([cls]) + b"\0" * 11
                + etype.to_bytes(2, "little") + b"\0" * 8)

    def _pe(chars):
        head = bytearray(b"MZ" + b"\0" * 0x3E)
        head[0x3C:0x40] = (0x40).to_bytes(4, "little")
        pe = bytearray(b"PE\0\0" + b"\0" * 20)
        pe[22:24] = chars.to_bytes(2, "little")
        return bytes(head) + bytes(pe)

    def _macho(filetype):
        return (MH_MAGIC_64.to_bytes(4, "little") + b"\0" * 8
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
        # how). The KEY is carried explicitly rather than sniffed out of the
        # label: a finding about the OTHER key would let a mutation pass for the
        # wrong reason, which is how a red-on-disable becomes decorative. The
        # target OS is carried too because a wrong declaration is only wrong on
        # the target it disagrees with — `HAVE_PREAD64: true` is CORRECT on the
        # Linux legs and is the shipped defect on the Darwin ones.
        for _os, _key, _variant, _mutate in (
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
        ):
            _copy = json.loads(json.dumps(_doc))
            for _l in _copy["legs"]:
                if spec_target_os(_l["spec"]) == _os:
                    _mutate(_l)
            _p = os.path.join(_mut_dir, "legs.json")
            with open(_p, "w", encoding="utf-8") as _f:
                json.dump(_copy, _f)
            _found = [f for f in lint(_p) if _key in f]
            check("the lint REDS on %s" % _variant, bool(_found),
                  "lint said nothing about %s — the check is dead config" % _key)
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
    args = p.parse_args(argv)

    if not (args.verdict_vocabulary or args.plan or args.lint or args.self_test
            or args.header_stages or args.config_stages or args.path_translations
            or args.translate_path or args.assert_translated
            or args.env_transfers or args.env_transfer
            or args.acquire or args.acquire_plan or args.resolve_library_argv
            or args.resolve_target_cc or args.build_loadext_helper
            or args.loadext_builder or args.tcl_coherence
            or args.run_filesystems or args.run_dir_plan
            or args.registry_controls or args.check_regions):
        p.error("one of --verdict-vocabulary / --plan / --header-stages / "
                "--config-stages / --lint "
                "/ --self-test / --path-translations / --translate-path / "
                "--assert-translated / --env-transfers / --env-transfer / "
                "--registry-controls / --check-regions / "
                "--acquire / --acquire-plan / --resolve-library-argv / "
                "--resolve-target-cc / --build-loadext-helper / "
                "--loadext-builder / --tcl-coherence / --run-filesystems / "
                "--run-dir-plan is required")
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
