#!/usr/bin/env python3
# TF-C85 — THE PROFILE-CENSUS GUARD, TIER (b): the CORPUS census.
# TF-C86 — relocated from `pragma-profile-census.sh` into this one Python
#          implementation, which every caller now starts directly (see the
#          note at the bottom of this header).
#
# ═══ WHY THIS IS A SCRIPT AND NOT A CTEST ENTRY — STATED PLAINLY ═════════════
#
# Tier (a) — `Preprocessor.TfC85NoUnclaimedPragmaUnderAnyPredefineClass` plus
# its non-vacuity twin — is ALWAYS ON in ctest, over the in-repo fixture
# `tests/corpus/c/pragma_profile_census.c`. It is the guard that cannot
# rot.
#
# Tier (b) is THIS: the same census over the REAL 189-TU sqlite corpus. It is
# NOT wired into ctest, and that is a limitation being reported rather than
# papered over. The corpus lives OUTSIDE the repo (~/src/sqlite, cloned and
# configured by the sqlite driver, `.harness-config/runner/actions/real-examples/c/sqlite/build_and_test.py`), and no ctest entry
# in this project reads an out-of-repo path — the established shape for a
# witness that cannot run unattended is a `DISABLED_`-gated test (see
# tests/link/test_ar_writer.cpp:468, :522), which would be a test that never
# runs. A runnable script that a human or a CI job invokes deliberately is the
# honest form.
#
# ═══ WHAT IT MEASURES ════════════════════════════════════════════════════════
#
# The FULL reached pragma vocabulary, per predefine class, by DISARMING the
# registry: it copies `src/dss-config` to a scratch dir, empties
# `preprocess.pragmaEffects` there, and points DSS at the copy via
# `DSS_CONFIG_ROOT`. With no row claiming anything, EVERY reached pragma emits
# a `P0020` naming itself — which is exactly a census. The repo's own config is
# never touched.
#
# The result is diffed against the CHECKED-IN expected set
# (`.harness-config/runner/actions/pragma-profile-census/pragma-profile-census.expected`). A non-empty diff is a REVIEWABLE
# CHANGE, not automatically a failure.
#
# ═══ ★★ THE EXPECTED SET IS A FLOOR, NOT A TOTAL — A DIFF IS NOT A BUG ══════
#
# A reached-set is a function of (i) the manifest's defines, (ii) the predefine
# class, and (iii) HOW FAR EACH TU GETS before a hard error stops it. All three
# move. MEASURED examples of each:
#   * (i)/(iii): sqlite's `ext/rtree/rtree.c` carries two `#pragma intrinsic`
#     lines that contribute ZERO, because the manifest defines `SQLITE_CORE`
#     without `SQLITE_ENABLE_RTREE` and the whole file body is therefore an
#     elided `#if` branch (C 6.10p1 — an elided pragma is entirely silent).
#   * (ii): the ENTIRE `warning`/`intrinsic`/`optimize` vocabulary — 2135 lines
#     — is invisible on macho and elf and visible only on pe, because its guard
#     is `#if defined(_MSC_VER)`.
# So as other cycles clear blockers, TUs get further and this set GROWS. That
# is expected. The point of the diff is that new pragma vocabulary arrives as
# something a human reads and decides about, instead of as a silent pass or a
# surprise build break three cycles later.
#
# ═══ ONE IMPLEMENTATION, STARTED DIRECTLY (TF-C86) ═══════════════════════════
#
# The logic lives HERE, and a caller starts this file with a Python interpreter
# directly: the action runner as `python3 ./pragma-profile-census.py`, a person
# with `python3` (`python` works on Windows). Before TF-C86 this census existed
# ONLY as bash — half-shipped, and the missing half was the one Windows CI runs.
# Two hand-written ports would drift instead, and a drifted census is worse than
# a missing one. ⓘ The `.sh` and `.ps1` launchers TF-C86 added to find an
# interpreter retired on 2026-09-21 with every other `.sh`/`.ps1` under the
# actions directory; the exit code is now this process's own.
# ✔MEASURED 2026-09-21, Windows 11 / CPython 3.14.3 (locale encoding cp1252):
# the Windows half still could not run then. Every text read and write here used
# the LOCALE encoding, and `src/dss-config/sources/c.lang.json` does not decode as
# cp1252 (byte 0x9d at offset 24910), so `disarm_config` raised
# `UnicodeDecodeError` before the first compile. Every text read and write now
# names UTF-8, and every write LF. The compiler is also found as `dsscp.exe`
# there — the name this file never looked for.
#
# ═══ USAGE ═══════════════════════════════════════════════════════════════════
#
#   python3 .harness-config/runner/actions/pragma-profile-census/pragma-profile-census.py [--update]
#
#   --update   rewrite the expected file from this run (review the diff first!)
#   --dss-bin  the compiler to measure (overrides DSS_BIN)
#   --manifest the corpus manifest to census (overrides SQLITE_MANIFEST)
#
# Environment:
#   DSS_BIN          the compiler (default: build/bin/dss/dsscp, else the first
#                    build/**/dsscp in sorted path order; on Windows `dsscp.exe`
#                    is looked for first, the same way)
#   SQLITE_MANIFEST  a .dss-project.json to census (default:
#                    build/real-examples/c/sqlite/host/host.dss-project.json -- ⚠ a
#                    path the sqlite driver does not write: it writes
#                    <OUT_DIR>/<leg>/<leg>.dss-project.json per declared leg, so name
#                    one of those)
#
# Exit: 0 = census matches the expected set; 1 = it differs (review + --update);
#       2 = the census could not run at all (missing corpus, missing binary).
#
# NOTE FOR MAINTAINERS: every exit code below is captured DIRECTLY from the
# command that produced it, never through a pipe whose tail swallows the status.

from __future__ import annotations

import argparse
import collections
import difflib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

# ── OUTPUT ENCODING, AT IMPORT, BEFORE ANYTHING BELOW CAN PRINT ─────────────────
# ✔MEASURED 2026-08-23 (CPython 3.14.3, Windows, both streams pipes -- how the
# action runner starts this file): stdout comes up cp1252, so a line holding a
# character outside it raises `UnicodeEncodeError`, and stderr mangles it into an
# escape. This census prints corpus pragma text and a unified diff of it. Placed
# ABOVE `_repo_root()`, which runs at import and refuses on stderr.
for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError, OSError):   # pragma: no cover - an odd stream
        pass

# Resolved from THIS FILE, which now lives one level deeper
# (`.harness-config/runner/actions/pragma-profile-census/`) than it used to. Counting `..` hops is
# exactly the fragility that broke on the move, so the walk is ANCHORED on a
# marker the repo root always has and a script directory never does.
EXIT_MATCH, EXIT_DIFFERS, EXIT_CANNOT_RUN = 0, 1, 2

def _repo_root() -> Path:
    here = Path(__file__).resolve()
    for cand in here.parents:
        if (cand / "CMakeLists.txt").is_file() and (cand / "src").is_dir():
            return cand
    # TF-C87: there is deliberately NO hop-counting fallback here. A
    # `here.parent.parent.parent` guess is EXACTLY what silently produced a wrong
    # REPO_ROOT when this family moved one level deeper, and a wrong root does not
    # fail -- it censuses the wrong tree and reports the result as fact. Failing
    # loud is the only honest answer when the marker walk finds nothing.
    print(f"pragma-profile-census: FATAL: no repo root at or above {here} "
          f"(looked for a directory holding both CMakeLists.txt and src/)",
          file=sys.stderr)
    sys.exit(EXIT_CANNOT_RUN)

REPO_ROOT = _repo_root()
# The golden file is this script's SIBLING, so it is resolved from the script
# rather than from REPO_ROOT: a repo-root-anchored literal silently breaks the
# next time this family is relocated, which is precisely what happened here.
EXPECTED_FILE = Path(__file__).resolve().parent / "pragma-profile-census.expected"

# MEASURED: `availableObjectFormats` keys on format KIND, so the 24 shipped
# format files collapse to exactly these three classes.
LEGS = [
    ("pe",    "x86_64:pe64-x86_64-windows-exec"),
    ("macho", "arm64:macho64-arm64-darwin-exec"),
    ("elf",   "x86_64:elf64-x86_64-linux-exec"),
]

UNRECOGNIZED_RE = re.compile(r"unrecognized pragma '([^']*)'")

# Windows spells the compiler `dsscp.exe`. Used ONLY when auto-discovering the
# binary (the same rule as `corpus-census.py`); an explicit `--dss-bin` / `DSS_BIN`
# is taken verbatim.
EXE_SUFFIX = ".exe" if os.name == "nt" else ""


def die(msg: str) -> "NoReturn":                       # noqa: F821
    print(f"pragma-census: FATAL: {msg}", file=sys.stderr)
    sys.exit(EXIT_CANNOT_RUN)


def info(msg: str) -> None:
    print(f"pragma-census: {msg}", file=sys.stderr)


def read_text(path: Path) -> str:
    """A text file of this repository or of a run, read as UTF-8 -- never in the
    locale's encoding (see the TF-C86 note in the header)."""
    return path.read_text(encoding="utf-8")


def write_text(path: Path, text: str) -> None:
    """UTF-8 with LF line endings on every host. `open(newline=...)` rather than
    `Path.write_text(newline=...)`, which Python 3.9 does not have."""
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(text)


def find_dss_bin(explicit: str | None) -> Path:
    if explicit:
        candidate = Path(explicit)
    elif os.environ.get("DSS_BIN"):
        candidate = Path(os.environ["DSS_BIN"])
    else:
        # The executable SUFFIX is part of the name on Windows: without it the
        # default path and the glob can never match the `dsscp.exe` a Windows
        # build produces. Only the NAME is widened -- the directories searched
        # are unchanged, because quietly picking up whichever compiler some other
        # build tree happens to hold is a run-identity problem, not a convenience.
        names = ["dsscp"]
        if EXE_SUFFIX:
            names.insert(0, "dsscp" + EXE_SUFFIX)
        candidate = None
        for name in names:
            default = REPO_ROOT / "build" / "bin" / "dss" / name
            if default.is_file():
                candidate = default
                break
            found = sorted(REPO_ROOT.glob(f"build/**/{name}"))
            if found:
                candidate = found[0]
                break
        if candidate is None:
            die("dsscp not found (looked for %s under %s); build the project or set "
                "DSS_BIN." % (" and ".join(names), REPO_ROOT / "build"))
    if not candidate.is_file() or not os.access(candidate, os.X_OK):
        die(f"dsscp not executable at {candidate}; "
            f"build the project or set DSS_BIN.")
    return candidate


def disarm_config(scratch: Path) -> Path:
    """Copy src/ to the scratch dir and empty `preprocess.pragmaEffects` there.

    With no row claiming anything, every REACHED pragma emits a P0020 naming
    itself. The repo's own config is never modified."""
    dst = scratch / "src"
    shutil.copytree(REPO_ROOT / "src", dst)
    lang = dst / "dss-config" / "sources" / "c.lang.json"
    if not lang.is_file():
        die("no c.lang.json in the scratch copy")
    doc = json.loads(read_text(lang))
    pp = doc.get("preprocess")
    if pp is None or "pragmaEffects" not in pp:
        die("c.lang.json no longer has preprocess.pragmaEffects")
    pp["pragmaEffects"] = []             # claim NOTHING
    pp["unknownPragmaIsError"] = True    # so every reached pragma names itself
    write_text(lang, json.dumps(doc, indent=2, ensure_ascii=False))
    return scratch


def census_leg(dss_bin: Path, manifest: dict, name: str, spec: str,
               scratch: Path, env: dict) -> list[str]:
    info(f"censusing predefine class '{name}' ({spec}) ...")
    derived = dict(manifest)
    derived["targets"] = [spec]
    derived.pop("resolveLibraries", None)   # link-tier only; the census stops at PP
    leg_manifest = scratch / f"{name}.dss-project.json"
    write_text(leg_manifest, json.dumps(derived, indent=2))
    log = scratch / f"{name}.log"
    with log.open("wb") as fh:
        proc = subprocess.run(
            [str(dss_bin), "--project", str(leg_manifest),
             "--output", str(scratch / f"{name}-out")],
            stdout=fh, stderr=subprocess.STDOUT, env=env, check=False)
    rc = proc.returncode                # captured DIRECTLY
    info(f"  (compiler exit {rc} — a census run is EXPECTED to fail the build)")
    # `unrecognized pragma '<text>'` -> the pragma's leading WORD, counted.
    counts: collections.Counter = collections.Counter()
    for line in log.read_text(encoding="utf-8", errors="replace").splitlines():
        for match in UNRECOGNIZED_RE.finditer(line):
            word = match.group(1).split(" ", 1)[0]
            counts[word] += 1
    return [f"{name} {word} {n}" for word, n in counts.items()]


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(
        prog="pragma-profile-census",
        description="Census the reached #pragma vocabulary over the real corpus.")
    ap.add_argument("--update", action="store_true",
                    help="rewrite the expected file from this run "
                         "(review the diff first!)")
    ap.add_argument("--dss-bin", default=None, help="the compiler to measure")
    ap.add_argument("--manifest", default=None, help="corpus manifest to census")
    args = ap.parse_args(argv)

    dss_bin = find_dss_bin(args.dss_bin)
    manifest_path = Path(args.manifest or os.environ.get("SQLITE_MANIFEST")
                         or (REPO_ROOT / "build" / "real-examples" / "c"
                             / "sqlite" / "host" / "host.dss-project.json"))
    if not manifest_path.is_file():
        die(f"no corpus manifest at {manifest_path} — run "
            f"`python3 .harness-config/runner/actions/real-examples/c/sqlite/build_and_test.py` first (it writes <OUT_DIR>/<leg>/<leg>.dss-project.json per "
            f"declared leg, OUT_DIR = build/real-examples/c/sqlite, with /windows on a Windows "
            f"host), or set SQLITE_MANIFEST.")
    manifest = json.loads(read_text(manifest_path))

    scratch = Path(tempfile.mkdtemp(prefix="dss-pragma-census."))
    try:
        disarm_config(scratch)
        env = dict(os.environ)
        env["DSS_CONFIG_ROOT"] = str(scratch)
        actual: list[str] = []
        for name, spec in LEGS:
            actual.extend(census_leg(dss_bin, manifest, name, spec, scratch, env))
        actual.sort()

        if args.update:
            header = []
            if EXPECTED_FILE.is_file():
                header = [ln for ln in read_text(EXPECTED_FILE).splitlines()
                          if ln.startswith("#")]
            write_text(EXPECTED_FILE, "\n".join(header + actual) + "\n")
            info(f"expected set UPDATED -> {EXPECTED_FILE}")
            print("\n".join(actual), file=sys.stderr)
            return EXIT_MATCH

        if not EXPECTED_FILE.is_file():
            info("no expected set yet; run with --update after reviewing:")
            print("\n".join(actual), file=sys.stderr)
            return EXIT_DIFFERS

        # `#`-comment lines in the expected file are documentation, not data.
        expected = [ln for ln in read_text(EXPECTED_FILE).splitlines()
                    if ln and not ln.startswith("#") and ln.strip()]
        if expected == actual:
            info("census MATCHES the checked-in expected set.")
            return EXIT_MATCH

        info("census DIFFERS from the checked-in expected set.")
        info("This is a REVIEWABLE CHANGE, not automatically a bug — the reached")
        info("set grows as other cycles let TUs get further. Read the diff, decide")
        info("whether each new prefix needs a 'preprocess.pragmaEffects' row, then")
        info("--update.")
        for line in difflib.unified_diff(expected, actual,
                                         fromfile="expected", tofile="actual",
                                         lineterm=""):
            print(line, file=sys.stderr)
        return EXIT_DIFFERS
    finally:
        shutil.rmtree(scratch, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
