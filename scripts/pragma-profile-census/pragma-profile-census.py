#!/usr/bin/env python3
# TF-C85 — THE PROFILE-CENSUS GUARD, TIER (b): the CORPUS census.
# TF-C86 — relocated from `pragma-profile-census.sh` into one Python
#          implementation with `.sh` + `.ps1` launchers (see the note at the
#          bottom of this header).
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
# configured by `real-examples/c/sqlite/build-and-test.sh`), and no ctest entry
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
# (`scripts/pragma-profile-census/pragma-profile-census.expected`). A non-empty diff is a REVIEWABLE
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
# ═══ ONE IMPLEMENTATION, TWO LAUNCHERS (TF-C86) ══════════════════════════════
#
# The logic lives HERE. `scripts/pragma-profile-census/pragma-profile-census.sh` and
# `scripts/pragma-profile-census/pragma-profile-census.ps1` locate a Python 3 and exec this file,
# propagating the exit code and doing nothing else. Before TF-C86 this census
# existed ONLY as bash — half-shipped, and the missing half was the one Windows
# CI runs. Two hand-written ports would drift instead, and a drifted census is
# worse than a missing one. Neither launcher ever invokes the other.
#
# ═══ USAGE ═══════════════════════════════════════════════════════════════════
#
#   scripts/pragma-profile-census/pragma-profile-census.py [--update]
#
#   --update   rewrite the expected file from this run (review the diff first!)
#
# Environment:
#   DSS_BIN          the compiler (default: the newest build/**/dsscp)
#   SQLITE_MANIFEST  a .dss-project.json to census (default: the host manifest
#                    the sqlite harness generates under build/real-examples/)
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

# Resolved from THIS FILE, which now lives one level deeper
# (`scripts/pragma-profile-census/`) than it used to. Counting `..` hops is
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


def die(msg: str) -> "NoReturn":                       # noqa: F821
    print(f"pragma-census: FATAL: {msg}", file=sys.stderr)
    sys.exit(EXIT_CANNOT_RUN)


def info(msg: str) -> None:
    print(f"pragma-census: {msg}", file=sys.stderr)


def find_dss_bin(explicit: str | None) -> Path:
    if explicit:
        candidate = Path(explicit)
    elif os.environ.get("DSS_BIN"):
        candidate = Path(os.environ["DSS_BIN"])
    else:
        default = REPO_ROOT / "build" / "bin" / "dss" / "dsscp"
        if default.is_file():
            candidate = default
        else:
            found = sorted(REPO_ROOT.glob("build/**/dsscp"))
            if not found:
                die("dsscp not found; build the project or set DSS_BIN.")
            candidate = found[0]
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
    doc = json.loads(lang.read_text())
    pp = doc.get("preprocess")
    if pp is None or "pragmaEffects" not in pp:
        die("c.lang.json no longer has preprocess.pragmaEffects")
    pp["pragmaEffects"] = []             # claim NOTHING
    pp["unknownPragmaIsError"] = True    # so every reached pragma names itself
    lang.write_text(json.dumps(doc, indent=2, ensure_ascii=False))
    return scratch


def census_leg(dss_bin: Path, manifest: dict, name: str, spec: str,
               scratch: Path, env: dict) -> list[str]:
    info(f"censusing predefine class '{name}' ({spec}) ...")
    derived = dict(manifest)
    derived["targets"] = [spec]
    derived.pop("resolveLibraries", None)   # link-tier only; the census stops at PP
    leg_manifest = scratch / f"{name}.dss-project.json"
    leg_manifest.write_text(json.dumps(derived, indent=2))
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
    for line in log.read_text(errors="replace").splitlines():
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
            f"real-examples/c/sqlite/build-and-test.sh first, or set "
            f"SQLITE_MANIFEST.")
    manifest = json.loads(manifest_path.read_text())

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
                header = [ln for ln in EXPECTED_FILE.read_text().splitlines()
                          if ln.startswith("#")]
            EXPECTED_FILE.write_text("\n".join(header + actual) + "\n")
            info(f"expected set UPDATED -> {EXPECTED_FILE}")
            print("\n".join(actual), file=sys.stderr)
            return EXIT_MATCH

        if not EXPECTED_FILE.is_file():
            info("no expected set yet; run with --update after reviewing:")
            print("\n".join(actual), file=sys.stderr)
            return EXIT_DIFFERS

        # `#`-comment lines in the expected file are documentation, not data.
        expected = [ln for ln in EXPECTED_FILE.read_text().splitlines()
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
