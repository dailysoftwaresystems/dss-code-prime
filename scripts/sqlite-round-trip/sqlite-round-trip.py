#!/usr/bin/env python3
# PURPOSE: carry a sqlite leg's DSS-built artefacts to a machine that runs their target and prove they EXECUTE there (the round trip).
#
# ─────────────────────────────────────────────────────────────────────────────
# WHY THIS EXISTS, and why it is not part of build-and-test.{sh,ps1}
#
# The two harness drivers answer "can THIS host build every leg, and run the
# ones it can reach?".  They are single-machine by construction: a leg either
# executes where it was built (natively or under a DECLARED launcher) or it
# reaches no verdict at all.  Table 2 of the cross-leg matrix asks a different
# question -- "does the artefact BEHAVE on the machine it was emitted for?" --
# and that question spans two machines, so no single driver can answer it.
#
# Until now every Table-2 cell was hand-carried, and the registry records what
# hand-carrying cost:
#   * a macOS-built sqlite3.exe scp'd ALONE to Windows failed all 14 smoke
#     assertions with STATUS_DLL_NOT_FOUND.  The binary was fine; the probe had
#     left zlib.dll behind.  (report-shape.md, 2026-08-07)
#   * a VPS-built testfixture.exe carried with its DLLs but WITHOUT Tcl's script
#     library passed all 192 tests and still exited rc=1 on
#     `unknown encoding "cp1252"`.  (report-shape.md, 2026-08-06)
#   * two WSL-built Mach-O binaries came back 13/14 with `source-id-token-exact`
#     CHARGED TO DSS, because the expectation had been read off "the tree" while
#     a concurrent run had moved it forward.  The binaries were right and the
#     expectation was stale.  (report-shape.md, 2026-08-07)
#
# All three are transport defects that arrive wearing a compiler's face.  This
# script makes each one structurally impossible rather than remembered:
#   * `pack` carries the leg's OUTPUT DIRECTORY, never a hand-picked file, so a
#     staged non-system library cannot be left behind;
#   * `pack` copies the staged `sqlite3.h` INTO the payload, so the expectation
#     travels WITH the binary and cannot be re-read off a tree that has moved;
#   * `verify` re-hashes every carried byte on arrival, so "corrupted in
#     transport" is excluded BEFORE any assertion runs rather than argued about
#     afterwards.
#
# ★ WHAT IT DELIBERATELY DOES NOT DO: it does not move bytes.  The carriage is
# `scripts/ssh-macos/`, `scripts/ssh-arm64-vps/` or a plain filesystem copy, and
# each already handles its own host's quoting, keys and rsync contract.  A
# second transport here would be a fourth spelling of something that exists.
#
# ★ NO `.ps1` TWIN, AND THAT IS THE JUDGEMENT NOT AN OMISSION: this is a `.py`,
# which runs on Windows, WSL, the arm64 VPS and macOS alike, so a PowerShell
# sibling would be a SECOND IMPLEMENTATION of something that was never split.
# Windows is a required leg of this very matrix and reaches this file directly.
#
# ─────────────────────────────────────────────────────────────────────────────
# USAGE
#
#   # on the BUILD host, right after build-and-test.{sh,ps1} produced OUT_DIR:
#   sqlite-round-trip.py pack --out-dir <OUT_DIR> --leg pe64-x86_64 \
#       --built-on wsl-x86_64 --sqlite-header <BLD>/sqlite3.h \
#       --sqlite-sha <upstream sha> --dss-sha <compiler sha> --dest <payload dir>
#
#   # ... carry <payload dir> to the target machine with the appropriate carriage ...
#
#   # on the TARGET host:
#   sqlite-round-trip.py verify --payload <payload dir>
#   sqlite-round-trip.py run --payload <payload dir> --workdir <scratch> \
#       --ran-on windows --json <result.json>
#
# EXIT CODES (the same on every host, so a caller can branch on them):
#   0  every requested arm passed
#   1  an arm FAILED (the artefact ran and gave a wrong answer, or did not run)
#   2  usage / a precondition this script refuses to guess at
#   3  the payload does not verify (a transport fault, NOT an artefact verdict)
# ─────────────────────────────────────────────────────────────────────────────

from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

# Guard output encoding, at IMPORT and covering BOTH streams. This carries file
# names and hashes through a pipe, and on a cp1252 console a non-ASCII path would
# be MANGLED rather than reported -- a transport tool that garbles the name of the
# file it refuses is worse than one that fails. Inside main() is not enough:
# argument parsing and --help print before it runs.
# Refused by `guard_output_encoding_guard` on the P46 integration gate, in the
# fold after the lane that wrote this file had already gone green -- the lane's
# own ctest could not see it, because the script was untracked there.
for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError, OSError):
        pass

PAYLOAD_MANIFEST = "payload.json"
PAYLOAD_VERSION = 1


def die(msg: str, code: int = 2) -> "NoReturn":  # noqa: F821
    sys.stderr.write("sqlite-round-trip: %s\n" % msg)
    raise SystemExit(code)


def sha256_of(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def hash_tree(root: Path) -> "dict[str, str]":
    """sha256 of every regular file under root, keyed by POSIX-spelled relpath.

    The key spelling is normalised to '/' DELIBERATELY: a payload packed on
    Windows is verified on Linux and vice versa, and a manifest keyed by the
    packing host's separator would refuse every cross-OS transport -- i.e. the
    integrity check would fail on exactly the cases it exists to protect.
    """
    out = {}
    for dirpath, _dirnames, filenames in os.walk(root):
        for name in sorted(filenames):
            p = Path(dirpath) / name
            if p.is_symlink() or not p.is_file():
                continue
            out[p.relative_to(root).as_posix()] = sha256_of(p)
    return out


def read_sqlite_expectations(header: Path) -> "tuple[str, str]":
    """SQLITE_VERSION / SQLITE_SOURCE_ID, read from the header the binary was built against.

    Same two `#define`s build-and-test.sh reads at its `CLI_EXPECT_VERSION`
    assignment -- but read from a COPY that travels inside the payload, which is
    the whole point: an expectation re-derived on arrival is an expectation about
    whatever the tree says today, and that has already fabricated a DSS-charged
    failure in this project.
    """
    version = source_id = ""
    text = header.read_text(encoding="utf-8", errors="replace")
    for line in text.splitlines():
        m = re.match(r'^#define\s+SQLITE_VERSION\s+"(.*)"', line)
        if m and not version:
            version = m.group(1)
        m = re.match(r'^#define\s+SQLITE_SOURCE_ID\s+"(.*)"', line)
        if m and not source_id:
            source_id = m.group(1)
    if not version or not source_id:
        die("could not read SQLITE_VERSION / SQLITE_SOURCE_ID out of %s. They are what the\n"
            "      smoke gate compares the carried CLI's --version against; without them the\n"
            "      round trip would assert nothing, which must never pass quietly." % header)
    return version, source_id


def format_dir(parent: Path, leg_spec: str, what: str) -> Path:
    """The <format> subdirectory the harness writes a leg's artefacts into.

    ★ NAMED BY THE LEG'S OWN SPEC, NOT DISCOVERED.  `<OUT>/<leg>/` also holds
    `cli/`, `run/`, `loadext-helper/` and `cli-smoke/`, so "the one directory
    under here" is not a well-defined question -- it was measured wrong on the
    first run against a real tree.  The leg spec is `<arch>:<format>` and the
    directory is named for the format half, which makes this exact instead of
    heuristic.  FAIL LOUD when it is absent: a round trip that ran the wrong
    artefact is the failure this whole file exists to prevent.
    """
    if ":" not in leg_spec:
        die("--leg-spec must be <arch>:<format> (e.g. x86_64:pe64-x86_64-windows-exec); got %r.\n"
            "      It NAMES the artefact directory; this script will not guess which one the\n"
            "      round trip is about." % leg_spec)
    d = parent / leg_spec.split(":", 1)[1]
    if not d.is_dir():
        siblings = ", ".join(sorted(p.name for p in parent.iterdir() if p.is_dir())) \
            if parent.is_dir() else "<parent missing>"
        die("%s: %s does not exist. Present under %s: %s.\n"
            "      Run the harness for this leg on this host first."
            % (what, d, parent, siblings or "nothing"))
    return d


def pick_executable(d: Path, stems: "list[str]") -> Path:
    """The artefact itself, by STEM, so `sqlite3` and `sqlite3.exe` are one rule."""
    for stem in stems:
        for p in sorted(d.iterdir()):
            if p.is_file() and (p.name == stem or p.stem == stem):
                return p
    die("no artefact named %s under %s (found: %s)"
        % (" or ".join(stems), d, ", ".join(p.name for p in sorted(d.iterdir())) or "nothing"))


# ── pack ─────────────────────────────────────────────────────────────────────

def cmd_pack(a: argparse.Namespace) -> int:
    out_dir = Path(a.out_dir).resolve()
    leg_dir = out_dir / a.leg
    if not leg_dir.is_dir():
        die("no leg directory %s -- is '%s' a leg this host built?" % (leg_dir, a.leg))

    fixture_dir = format_dir(leg_dir, a.leg_spec, "the testfixture artefact directory")
    cli_dir = format_dir(leg_dir / "cli", a.leg_spec, "the sqlite3 CLI artefact directory")

    dest = Path(a.dest).resolve()
    if dest.exists():
        shutil.rmtree(dest)
    (dest / "cli").mkdir(parents=True)
    (dest / "units").mkdir(parents=True)

    # ★ THE DIRECTORY, NEVER A HAND-PICKED FILE.  report-shape.md: "The cheapest
    # correct-by-construction transport is the leg's OUTPUT DIRECTORY, which
    # already contains the staged libraries beside the binary.  Copy the
    # directory, not the file.  Hand-picking is how the omission happens."
    for src, sub in ((cli_dir, "cli"), (fixture_dir, "units")):
        for p in sorted(src.iterdir()):
            if p.is_file():
                shutil.copy2(p, dest / sub / p.name)

    cli_bin = pick_executable(dest / "cli", ["sqlite3"])
    fixture_bin = pick_executable(dest / "units", ["testfixture"])

    header = Path(a.sqlite_header).resolve()
    if not header.is_file():
        die("--sqlite-header %s does not exist. The expectation must travel WITH the binary." % header)
    shutil.copy2(header, dest / "sqlite3.h")
    version, source_id = read_sqlite_expectations(dest / "sqlite3.h")

    # The loadext helper the corpus dlopen()s.  Carried when the build host made
    # one: it is emitted for the LEG's format, so the target host cannot
    # substitute its own.  Absent is a stated fact, not a silent gap.
    helper_note = "not built on the packing host"
    helper_src = leg_dir / "loadext-helper" / "dss"
    if helper_src.is_dir():
        (dest / "units" / "loadext").mkdir(exist_ok=True)
        n = 0
        for p in sorted(helper_src.iterdir()):
            if p.is_file():
                shutil.copy2(p, dest / "units" / "loadext" / p.name)
                n += 1
        helper_note = "carried %d file(s) from %s" % (n, helper_src)

    # ★★ TCL'S SCRIPT LIBRARY TRAVELS WITH THE FIXTURE, AND THAT IS NOT A
    # CONVENIENCE.  ✔MEASURED 2026-08-06 (report-shape.md): a VPS-built
    # `testfixture.exe` hand-carried to Windows with its DLLs but WITHOUT the
    # script library passed all 192 tests and still exited rc=1 on
    # `unknown encoding "cp1252"` raised from `finish_test` -- a non-zero exit
    # arriving AFTER every assertion had already succeeded.  A library's CODE is
    # not all a library needs.  These are Tcl SCRIPTS, so they are the same bytes
    # on every host and carrying them costs nothing.
    tcl_note = "not carried (--tcl-script-dir not given)"
    if a.tcl_script_dir:
        tsrc = Path(a.tcl_script_dir).resolve()
        if not tsrc.is_dir():
            die("--tcl-script-dir %s does not exist" % tsrc)
        shutil.copytree(tsrc, dest / "tcl" / tsrc.name)
        tcl_note = "carried from %s" % tsrc

    manifest = {
        "payloadVersion": PAYLOAD_VERSION,
        "tclScriptLibrary": tcl_note,
        "tclScriptLibraryRel": ("tcl/" + Path(a.tcl_script_dir).name) if a.tcl_script_dir else "",
        "packedAtUtc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "builtOn": a.built_on,
        "builtOnUname": {"system": platform.system(), "machine": platform.machine(),
                         "release": platform.release()},
        "leg": a.leg,
        "legSpec": a.leg_spec,
        "cliBinary": cli_bin.relative_to(dest).as_posix(),
        "fixtureBinary": fixture_bin.relative_to(dest).as_posix(),
        "expectVersion": version,
        "expectSourceId": source_id,
        "sqliteUpstreamSha": a.sqlite_sha,
        "dssSha": a.dss_sha,
        "loadextHelper": helper_note,
        "sourceCliDir": str(cli_dir),
        "sourceFixtureDir": str(fixture_dir),
    }
    manifest["files"] = hash_tree(dest)
    (dest / PAYLOAD_MANIFEST).write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")

    print("packed  : %s" % dest)
    print("  leg   : %s (%s)  built on %s" % (a.leg, a.leg_spec, a.built_on))
    print("  cli   : %s" % manifest["cliBinary"])
    print("  units : %s" % manifest["fixtureBinary"])
    print("  expect: %s / %s" % (version, source_id))
    print("  files : %d hashed" % len(manifest["files"]))
    return 0


# ── verify ───────────────────────────────────────────────────────────────────

def load_manifest(payload: Path) -> dict:
    mf = payload / PAYLOAD_MANIFEST
    if not mf.is_file():
        die("no %s under %s -- that directory is not a round-trip payload" % (PAYLOAD_MANIFEST, payload))
    return json.loads(mf.read_text(encoding="utf-8"))


def verify_payload(payload: Path) -> dict:
    m = load_manifest(payload)
    expected = m.get("files") or {}
    if not expected:
        die("payload manifest carries no file hashes", 3)
    actual = hash_tree(payload)
    actual.pop(PAYLOAD_MANIFEST, None)
    missing = sorted(set(expected) - set(actual))
    extra = sorted(set(actual) - set(expected))
    moved = sorted(k for k in set(expected) & set(actual) if expected[k] != actual[k])
    if missing or moved:
        for k in missing:
            sys.stderr.write("  MISSING  %s\n" % k)
        for k in moved:
            sys.stderr.write("  CHANGED  %s (packed %s, arrived %s)\n" % (k, expected[k][:12], actual[k][:12]))
        die("the payload does NOT verify. This is a TRANSPORT fault and no artefact verdict\n"
            "      may be taken from it -- a wrong answer from a corrupted binary is not a\n"
            "      compiler result.", 3)
    for k in extra:
        sys.stderr.write("  note: %s arrived but was not in the manifest (ignored)\n" % k)
    return m


def cmd_verify(a: argparse.Namespace) -> int:
    m = verify_payload(Path(a.payload).resolve())
    print("payload VERIFIES: %d file(s), leg %s built on %s, sqlite %s"
          % (len(m["files"]), m["leg"], m["builtOn"], m["expectVersion"]))
    return 0


# ── run ──────────────────────────────────────────────────────────────────────

def make_executable(path: Path) -> None:
    """Restore the exec bit a cross-filesystem transport can drop.

    A payload that crossed DrvFs, a zip, or an rsync from a Windows checkout
    arrives with modes that are a property of the TRANSPORT, not of the artefact.
    Windows has no exec bit at all, so this is a no-op there rather than a
    branch a caller has to remember.
    """
    if os.name == "nt":
        return
    os.chmod(path, 0o755)


def run_capture(argv: "list[str]", env: dict, cwd: "str | None" = None,
                timeout: "int | None" = None) -> "tuple[int, str]":
    try:
        p = subprocess.run(argv, env=env, cwd=cwd, stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, timeout=timeout)
    except FileNotFoundError as exc:
        return 127, "could not launch %r: %s" % (argv[0], exc)
    except subprocess.TimeoutExpired:
        return 124, "TIMED OUT after %ss" % timeout
    return p.returncode, p.stdout.decode("utf-8", "replace")


def leg_env(base: dict, payload: Path, m: dict, a: argparse.Namespace, subdir: str) -> dict:
    """The runtime environment the carried artefact needs on THIS host.

    The loader search variable is a property of the TARGET, never of the host --
    the same rule build-and-test.sh's `run_leg` states, and the reason
    LD_LIBRARY_PATH was once exported for a Darwin leg that cannot read it.
    """
    env = dict(base)
    spec = m.get("legSpec", "") or ""
    libdir = str(payload / subdir)
    if "windows" in spec or spec.endswith(".exe"):
        var, sep = "PATH", os.pathsep
    elif "darwin" in spec:
        var, sep = "DYLD_LIBRARY_PATH", ":"
    else:
        var, sep = "LD_LIBRARY_PATH", ":"
    env[var] = libdir + ((sep + env[var]) if env.get(var) else "")
    for pair in a.env:
        if "=" not in pair:
            die("--env takes NAME=VALUE, got %r" % pair)
        k, v = pair.split("=", 1)
        env[k] = v
    # The carried script library wins over this host's own: the fixture links the
    # LEG's Tcl, and a host whose native Tcl is a different minor version would
    # otherwise be silently mixed in.  An explicit --tcl-library overrides both.
    carried = m.get("tclScriptLibraryRel") or ""
    if carried and (payload / carried).is_dir():
        env["TCL_LIBRARY"] = str(payload / carried)
    if a.tcl_library:
        env["TCL_LIBRARY"] = a.tcl_library
    return env


def cmd_run(a: argparse.Namespace) -> int:
    payload = Path(a.payload).resolve()
    m = verify_payload(payload)
    work = Path(a.workdir).resolve()
    if work.exists():
        shutil.rmtree(work)
    work.mkdir(parents=True)

    result = {
        "leg": m["leg"], "legSpec": m.get("legSpec", ""),
        "builtOn": m["builtOn"], "ranOn": a.ran_on,
        "launcher": a.launcher, "payload": str(payload),
        "sqliteUpstreamSha": m.get("sqliteUpstreamSha", ""),
        "dssSha": m.get("dssSha", ""),
        "expectVersion": m["expectVersion"], "expectSourceId": m["expectSourceId"],
        "arms": {},
    }
    rc_overall = 0

    # ── CLI ──────────────────────────────────────────────────────────────────
    if not a.skip_cli:
        cli = payload / m["cliBinary"]
        make_executable(cli)
        smoke = Path(a.cli_smoke).resolve() if a.cli_smoke else None
        if smoke is None or not smoke.is_file():
            die("--cli-smoke must name the repository's cli-smoke.py; it is the sanctioned\n"
                "      14-assertion gate and this script will not re-implement a weaker one.")
        triple = a.cli_target
        if not triple:
            legs_py = smoke.with_name("harness_legs.py")
            rc, out = run_capture([sys.executable, str(legs_py), "--identify-binary", str(cli)],
                                  dict(os.environ))
            if rc != 0:
                die("could not IDENTIFY the carried CLI (%s): %s" % (cli, out.strip()))
            triple = ":".join(out.split())
        smoke_work = work / "cli-smoke"
        smoke_work.mkdir()
        argv = [sys.executable, str(smoke), "--cli", str(cli),
                "--expect-version", m["expectVersion"],
                "--expect-source-id", m["expectSourceId"],
                "--leg-spec", m.get("legSpec", ""),
                "--cli-target", triple,
                "--workdir", str(smoke_work), "--label", "%s->%s" % (m["builtOn"], a.ran_on),
                "--json", str(smoke_work / "result.json")]
        for tok in a.launcher:
            argv.append("--launcher=%s" % tok)
        env = leg_env(dict(os.environ), payload, m, a, "cli")
        rc, out = run_capture(argv, env, timeout=a.timeout)
        detail = {}
        rj = smoke_work / "result.json"
        if rj.is_file():
            try:
                detail = json.loads(rj.read_text(encoding="utf-8"))
            except Exception:
                detail = {}
        result["arms"]["cli"] = {"rc": rc, "verdict": "PASS" if rc == 0 else "FAIL",
                                 "target": triple, "log": out[-20000:], "smoke": detail}
        print("== CLI  %s built-on %s ran-on %s : rc=%d ==" % (m["leg"], m["builtOn"], a.ran_on, rc))
        print(out)
        if rc != 0:
            rc_overall = 1

    # ── UNITS ────────────────────────────────────────────────────────────────
    if not a.skip_units:
        if not a.test_file:
            die("--test-file is required for the UNITS arm (or pass --skip-units and say so\n"
                "      in the report: an omitted row reads as coverage that was never taken).")
        fixture = payload / m["fixtureBinary"]
        make_executable(fixture)
        units = []
        units_failed = False
        for idx, tf in enumerate(a.test_file):
            # ONE RUN DIRECTORY PER TEST FILE.  sqlite's fixture writes its
            # scratch databases into the CWD and several files assert on a
            # freshly-empty one; sharing a directory across files makes a later
            # file fail on an earlier file's leftovers, which reads exactly like
            # a miscompile.
            rundir = work / ("units-%02d" % idx)
            rundir.mkdir()
            if a.testdir_seed:
                seed = Path(a.testdir_seed).resolve()
                if not seed.is_dir():
                    die("--testdir-seed %s does not exist" % seed)
                shutil.copytree(seed, rundir / "testdir")
            else:
                (rundir / "testdir").mkdir()
            # The loadext helper is emitted for the LEG's format, so the carried
            # one is the only correct copy on this host.
            helper_dir = payload / "units" / "loadext"
            if helper_dir.is_dir():
                for p in sorted(helper_dir.iterdir()):
                    if p.is_file():
                        shutil.copy2(p, rundir / "testdir" / p.name)
            argv = list(a.launcher) + [str(fixture), str(Path(tf).resolve())]
            env = leg_env(dict(os.environ), payload, m, a, "units")
            rc, out = run_capture(argv, env, cwd=str(rundir), timeout=a.timeout)
            errors = total = None
            mm = None
            for mm in re.finditer(r"(\d+)\s+errors?\s+out\s+of\s+(\d+)\s+tests?", out):
                pass
            if mm:
                errors, total = int(mm.group(1)), int(mm.group(2))
            # A run that produced NO tally is a FAILURE, never a pass: the tally
            # is the only evidence the fixture reached its own summary.
            verdict = "PASS" if (rc == 0 and errors == 0 and total) else "FAIL"
            failing = re.findall(r"^!\s*(\S+)", out, re.M)
            units.append({"rc": rc, "verdict": verdict, "errors": errors, "tests": total,
                          "testFile": str(tf), "failingUnits": failing[:200],
                          "log": out[-40000:]})
            print("== UNITS %s built-on %s ran-on %s  %s : rc=%d  %s errors out of %s tests =="
                  % (m["leg"], m["builtOn"], a.ran_on, Path(tf).name, rc, errors, total))
            if verdict != "PASS":
                units_failed = True
                print(out[-8000:])
        result["arms"]["units"] = units
        if units_failed:
            rc_overall = 1

    if a.json:
        Path(a.json).parent.mkdir(parents=True, exist_ok=True)
        Path(a.json).write_text(json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        print("result json: %s" % a.json)
    return rc_overall


def main(argv=None) -> int:
    p = argparse.ArgumentParser(prog="sqlite-round-trip.py")
    sub = p.add_subparsers(dest="cmd", required=True)

    q = sub.add_parser("pack", help="collect a leg's artefacts into a verifiable payload")
    q.add_argument("--out-dir", required=True, help="the harness OUT_DIR on the BUILD host")
    q.add_argument("--leg", required=True)
    q.add_argument("--leg-spec", required=True, metavar="ARCH:FORMAT",
                   help="the leg's spec; its FORMAT half names the artefact directory")
    q.add_argument("--built-on", required=True, help="a label for the build host")
    q.add_argument("--sqlite-header", required=True,
                   help="the staged sqlite3.h the artefacts were built against")
    q.add_argument("--tcl-script-dir", default="",
                   help="the leg's staged Tcl script library (harness-libs/<leg>/tcl8.6)")
    q.add_argument("--sqlite-sha", default="", help="upstream sqlite commit -- a corpus number without it is meaningless")
    q.add_argument("--dss-sha", default="")
    q.add_argument("--dest", required=True)
    q.set_defaults(fn=cmd_pack)

    q = sub.add_parser("verify", help="re-hash an arrived payload")
    q.add_argument("--payload", required=True)
    q.set_defaults(fn=cmd_verify)

    q = sub.add_parser("run", help="execute a carried payload on THIS host")
    q.add_argument("--payload", required=True)
    q.add_argument("--workdir", required=True)
    q.add_argument("--ran-on", required=True)
    q.add_argument("--launcher", action="append", default=[], metavar="TOKEN",
                   help="use the =FORM (--launcher=-x86_64): a launcher token may lead with a dash")
    q.add_argument("--env", action="append", default=[], metavar="NAME=VALUE")
    q.add_argument("--tcl-library", default="")
    q.add_argument("--cli-smoke", default="")
    q.add_argument("--cli-target", default="")
    q.add_argument("--test-file", action="append", default=[], metavar="PATH",
                   help="repeatable; each upstream .test file gets its OWN fresh run directory")
    q.add_argument("--testdir-seed", default="")
    q.add_argument("--skip-cli", action="store_true")
    q.add_argument("--skip-units", action="store_true")
    q.add_argument("--timeout", type=int, default=3600)
    q.add_argument("--json", default="")
    q.set_defaults(fn=cmd_run)

    a = p.parse_args(argv)
    return a.fn(a)


if __name__ == "__main__":
    sys.exit(main())
