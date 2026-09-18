#!/usr/bin/env python3
"""manual-end-to-end.py -- THE MANUAL END-TO-END CORPUS DRIVER.

★★★ WHY A SECOND CORPUS EXISTS AT ALL, and it is one measured fact rather than a
preference. Both shipped corpus harnesses discover their ctest entries with a
GLOB over ``<repo>/examples/*/*/expected.json`` -- exactly that ROOT. An
end-to-end test that must NOT run by default therefore has a structural place to
live: anywhere that glob cannot reach. A test placed here is invisible to both
harnesses, so **the default ctest entry count cannot move**, and it cannot move
by construction rather than by a flag every future caller has to remember to
pass.

⚠ THE ROOT IS WHAT PROTECTS THIS, NOT THE DEPTH -- and that distinction became
load-bearing the day this corpus adopted ``corpus/<language>/<name>/`` to mirror
``examples/`` and ``real-examples/``. Both shapes are now two levels deep; only
the ROOT differs. This was previously argued as "exactly two levels, exactly
that root", which read as though depth were half the guarantee. It never was.
⛔ Do not restore a one-level layout believing depth is what keeps these entries
out of the gate -- discovery below is RECURSIVE, so depth here is a matter of
taste where the root is a matter of correctness. A registered-but-skipped entry still appears in ``ctest -N``
and would perturb the cross-leg identity the gate checks; this cannot.

★★ THE MANIFEST STILL DECLARES ITS INTENT. ``manualRun`` defaults to FALSE and an
entry without it is SKIPPED here, loudly, by name. The location decides what the
DEFAULT run sees; the flag decides what THIS runner will execute. Both halves are
needed: without the flag this directory would be a place where a test runs by
accident, which is the same defect one direction over.

★★★ WHAT THIS DRIVER IS FOR. The relaxation escape at +/-1 MiB is an ordinary
corpus example (``examples/c/long_branch_imm19_escape``). The BRANCH ISLAND edge
is not: it fires only beyond +/-128 MiB on AArch64, so reaching it means a
function of more than 134 MB of emitted code. That is a real run, on real
hardware, once -- not something a gate can afford every round. The operator's
ruling is that "cannot run every round" is not "cannot run", so it runs here, on
demand, and PRINTS ITS EVIDENCE.

★★ IT REPORTS AS IT WORKS, AND THAT IS A CONTRACT RATHER THAN A COURTESY.
DssHarness deliberately uses no wall-clock timeout for a phase -- a time budget is
a guess about workload size and kills honest long runs -- and bounds a step by its
output STALL instead. A run that prints nothing for twenty minutes and then prints
everything is indistinguishable from a hang, and no stall bound can be chosen
honestly for it. So this driver emits a heartbeat while a compile is in flight,
carrying the elapsed time and the output size so far. Choose ``stallSeconds``
above the heartbeat interval and nothing else.

WHAT IT PRINTS, per entry, and why each number is there:
  wall_s          how long the compile took, which is the affordability answer
  peak_rss_kb     how much it needed, which is the FEASIBILITY answer -- an
                  island-sized function is bounded by memory long before time
  emitted_bytes   the artifact, so a reader can check the size was reached
  text_bytes      the ``.text`` span, closer to what the resolver actually walked
  relax_passes    how many times the function was re-encoded
  islands         how many landing pads were placed
  host            uname/platform, because a one-host measurement is a
                  portability claim and must be readable as one

``relax_passes`` and ``islands`` are read from the compiler's own diagnostic
output when it reports them and are printed as ``unreported`` when it does not.
⚠ THAT IS A FACT ABOUT THE COMPILER'S OUTPUT, NOT A ZERO. Printing 0 for "the
compiler said nothing" would be a measurement that fails toward clean, which is
the failure mode this repository keeps naming.

LAYOUT: ``corpus/<language>/<name>/expected.json``, mirroring ``examples/`` and
``real-examples/``, discovered RECURSIVELY.

Usage:
    python3 ./manual-end-to-end.py --tree-root <path-to-repo-root>
                                   [--only <language>/<name> | <name>]
                                   [--dsscp PATH] [--heartbeat SECONDS]
"""

import argparse
import json
import os
import platform
import re
import subprocess
import sys
import threading
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
CORPUS = HERE / "corpus"
HEARTBEAT_DEFAULT = 15.0


def say(msg):
    """One place that writes, and it FLUSHES. An unflushed heartbeat is not a
    heartbeat: the stall bound reads the stream, not the intent."""
    sys.stdout.write(msg + "\n")
    sys.stdout.flush()


def find_dsscp(tree_root):
    """Locate the built compiler under the leg's own tree.

    ⚠ REFUSES RATHER THAN GUESSES when it cannot find exactly one plausible
    answer, and names every candidate it saw. A driver that silently picked the
    oldest build tree would measure a compiler nobody asked about, and every
    number below it would be a true measurement of the wrong thing.

    ⓘ THE ONE ASSUMPTION, STATED BECAUSE IT IS UNVERIFIED HERE: the search
    pattern is `<tree>/build/<name>/bin/dss/dsscp[.exe]`, which is this
    repository's own one-root layout (`build/` is a CONTAINER, one subdirectory
    per build). `config.json` declares no build directory for a leg, so if
    DssHarness places one elsewhere this refuses and names the path it looked
    under — a refusal a reader can act on — and `--dsscp` is the direct answer.
    Fix the search here rather than moving the build if that ever happens."""
    root = Path(tree_root).resolve()
    names = ("dsscp", "dsscp.exe")
    found = []
    build_root = root / "build"
    if build_root.is_dir():
        for tree in sorted(build_root.iterdir()):
            for n in names:
                p = tree / "bin" / "dss" / n
                if p.is_file():
                    found.append(p)
    if not found:
        raise SystemExit(
            "manual-end-to-end: no dsscp under {}/build/*/bin/dss/. The runner "
            "entry should declare requireBuild so the leg is built first; "
            "otherwise pass --dsscp.".format(root)
        )
    found.sort(key=lambda p: p.stat().st_mtime, reverse=True)
    if len(found) > 1:
        say("manual-end-to-end: NOTE {} candidate compilers, taking the newest:"
            .format(len(found)))
        for p in found:
            say("    {}  mtime {}".format(
                p, time.strftime("%Y-%m-%dT%H:%M:%S",
                                 time.localtime(p.stat().st_mtime))))
    return found[0]


def sample_rss_kb(pid):
    """RESIDENT SET of one live process, in KB, or None when this platform
    cannot be asked.

    ★ IT IS SAMPLED RATHER THAN SUMMED, and it is asked of ONE pid rather than
    of "my children", because the number wanted here is the high-water mark of
    the COMPILE — the figure that decides whether the island edge is reachable
    on a given host at all. `getrusage(RUSAGE_CHILDREN)` would answer about every
    child this driver ever started, which is a different question once the corpus
    holds two entries.

    ⚠ EACH PLATFORM'S OWN UNIT IS CONVERTED HERE RATHER THAN ASSUMED. Windows
    reports bytes through `GetProcessMemoryInfo`, Linux kilobytes through
    `VmHWM`, macOS kilobytes through `ps`. Getting that wrong by 1024x produces a
    number that reads exactly like a measurement and is not one.

    ⓘ Windows and Linux report a PEAK the kernel maintains, so a single read is
    exact. macOS has no such counter here, so its figure is the maximum over the
    samples this driver took and is therefore a FLOOR — said plainly rather than
    presented as a peak."""
    if sys.platform == "win32":
        try:
            import ctypes
            from ctypes import wintypes

            class COUNTERS(ctypes.Structure):
                _fields_ = [("cb", wintypes.DWORD),
                            ("PageFaultCount", wintypes.DWORD),
                            ("PeakWorkingSetSize", ctypes.c_size_t),
                            ("WorkingSetSize", ctypes.c_size_t),
                            ("QuotaPeakPagedPoolUsage", ctypes.c_size_t),
                            ("QuotaPagedPoolUsage", ctypes.c_size_t),
                            ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t),
                            ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
                            ("PagefileUsage", ctypes.c_size_t),
                            ("PeakPagefileUsage", ctypes.c_size_t)]

            h = ctypes.windll.kernel32.OpenProcess(0x0400 | 0x0010, False, pid)
            if not h:
                return None
            try:
                c = COUNTERS()
                c.cb = ctypes.sizeof(c)
                if not ctypes.windll.psapi.GetProcessMemoryInfo(
                        h, ctypes.byref(c), c.cb):
                    return None
                return c.PeakWorkingSetSize // 1024
            finally:
                ctypes.windll.kernel32.CloseHandle(h)
        except Exception:
            return None
    status = Path("/proc/{}/status".format(pid))
    if status.exists():
        try:
            for line in status.read_text().splitlines():
                if line.startswith("VmHWM:"):
                    return int(line.split()[1])
        except OSError:
            return None
        return None
    try:
        out = subprocess.run(["ps", "-o", "rss=", "-p", str(pid)],
                             capture_output=True, text=True, timeout=10)
        return int(out.stdout.strip()) if out.stdout.strip() else None
    except Exception:
        return None


def peak_rss_method():
    if sys.platform == "win32":
        return "GetProcessMemoryInfo.PeakWorkingSetSize (exact peak)"
    if Path("/proc/self/status").exists():
        return "/proc/<pid>/status VmHWM (exact peak)"
    return "ps rss, sampled — a FLOOR, not a peak"


def run_with_heartbeat(cmd, log_path, heartbeat, label):
    """Run `cmd`, streaming a heartbeat so a STALL bound is meaningful on it,
    and sampling the child's resident set on the same tick.

    ★ THE HEARTBEAT AND THE MEMORY SAMPLER ARE ONE LOOP ON PURPOSE. A run that
    reports nothing for twenty minutes and then reports everything cannot be
    bounded by a stall rule at all, and a peak memory figure that was never
    sampled during the peak is not a measurement. Both needs are served by the
    same tick, and the tick's period is the one number the runner entry has to
    choose `stallSeconds` against."""
    stop = threading.Event()
    started = time.monotonic()
    peak = {"kb": None}

    with log_path.open("wb") as log:
        proc = subprocess.Popen(cmd, stdout=log, stderr=subprocess.STDOUT)

        def beat():
            n = 0
            # The FIRST sample is taken immediately, not after one period: a
            # compile that dies in its first seconds would otherwise report no
            # memory figure at all.
            while True:
                s = sample_rss_kb(proc.pid)
                if s is not None and (peak["kb"] is None or s > peak["kb"]):
                    peak["kb"] = s
                if stop.wait(heartbeat):
                    return
                n += 1
                size = log_path.stat().st_size if log_path.exists() else 0
                say("manual-end-to-end progress {} beat={} elapsed_s={:.1f} "
                    "rss_kb={} compiler_output_bytes={}"
                    .format(label, n, time.monotonic() - started,
                            "unavailable" if peak["kb"] is None else peak["kb"],
                            size))

        t = threading.Thread(target=beat, daemon=True)
        t.start()
        rc = proc.wait()
        stop.set()
        t.join(timeout=heartbeat)
    return rc, time.monotonic() - started, peak["kb"]


def text_bytes_of(artifact):
    """`.text` size via readelf when one is on PATH, else None. Never a zero:
    an absent tool and an empty section are different answers."""
    for tool in ("readelf", "llvm-readelf", "aarch64-linux-gnu-readelf"):
        try:
            out = subprocess.run([tool, "-S", "-W", str(artifact)],
                                 capture_output=True, text=True, timeout=120)
        except (FileNotFoundError, subprocess.TimeoutExpired):
            continue
        if out.returncode != 0:
            continue
        for line in out.stdout.splitlines():
            if " .text " in line:
                parts = line.split()
                for i, p in enumerate(parts):
                    if p == ".text" and i + 4 < len(parts):
                        try:
                            return int(parts[i + 4], 16)
                        except ValueError:
                            return None
    return None


RELAX_RE = re.compile(r"relax(?:ation)?[ _]?pass(?:es)?[ =:]+(\d+)", re.I)
ISLAND_RE = re.compile(r"island(?:s)?[ _]?(?:placed|count)?[ =:]+(\d+)", re.I)


def scrape(log_text, rx):
    m = rx.search(log_text)
    return m.group(1) if m else "unreported"


def run_entry(entry_dir, dsscp, tree_root, heartbeat):
    manifest = json.loads((entry_dir / "expected.json").read_text(encoding="utf-8"))
    # ⚠ AN ENTRY'S NAME IS ITS PATH UNDER THE CORPUS ROOT, NOT ITS LEAF. Once
    # discovery recurses, `c/foo` and `cpp/foo` are two entries sharing a leaf,
    # and a leaf-keyed label would report both runs under one name — a reader
    # could not tell which language's entry produced which EVIDENCE line.
    name = entry_dir.relative_to(CORPUS).as_posix()
    if not manifest.get("manualRun", False):
        say("manual-end-to-end SKIP {} (manualRun absent or false - this runner "
            "executes only entries that declare it)".format(name))
        return None

    spec = manifest["targets"][0]["spec"]
    source = entry_dir / manifest["source"]
    # ⚠ REFUSE BY NAME rather than let `source.stat()` raise below. A manifest
    # naming a file that is not there is an ordinary consequence of moving an
    # entry between directories, and a bare FileNotFoundError traceback names
    # the LINE that tripped instead of the ENTRY that is wrong — which is the
    # harder failure to act on, and the one a reader meets right after a move.
    if not source.is_file():
        raise SystemExit(
            "manual-end-to-end: entry {} declares source {!r}, which does not "
            "exist at {}".format(name, manifest["source"], source))
    # ⚠ THE OUTPUT GOES TO `<tree>/.temp/`, NEVER BESIDE THE MANIFEST. An island
    # run emits a >134 MB artifact; written next to its source it would be a
    # nine-figure file inside a source directory, and `.harness-config/**` is
    # SYNCED to every leg. `.temp/` is git-ignored AND on the sync's
    # `neverTransfer` list, so the artifact neither reaches a commit nor crosses
    # a transport.
    # ⚠ KEYED ON THE RELATIVE PATH for the same reason the label is: two entries
    # sharing a leaf under different languages would otherwise resolve to ONE
    # output directory, and this function DELETES that directory before it runs
    # — so the second entry would destroy the first's artifact mid-run.
    out_dir = (Path(tree_root).resolve() / ".temp" / "manual-end-to-end"
               / entry_dir.relative_to(CORPUS))
    if out_dir.exists():
        for f in sorted(out_dir.rglob("*"), reverse=True):
            f.unlink() if f.is_file() else f.rmdir()
        out_dir.rmdir()
    out_dir.mkdir(parents=True)
    log_path = out_dir / "compile.log"

    env_root = os.environ.get("DSS_CONFIG_ROOT")
    if not env_root:
        os.environ["DSS_CONFIG_ROOT"] = str(Path(tree_root).resolve())

    cmd = [str(dsscp), "--compile", str(source), "--language",
           manifest.get("language", "c"), "--target", spec,
           "--output", str(out_dir)]

    say("manual-end-to-end START {} spec={} source_bytes={}"
        .format(name, spec, source.stat().st_size))
    say("manual-end-to-end command {}".format(" ".join(cmd)))

    rc, wall, peak_kb = run_with_heartbeat(cmd, log_path, heartbeat, name)
    peak = "unavailable" if peak_kb is None else str(peak_kb)

    log_text = log_path.read_text(encoding="utf-8", errors="replace")
    artifact = out_dir / manifest["targets"][0].get("artifact", "main")
    emitted = artifact.stat().st_size if artifact.is_file() else 0
    text = text_bytes_of(artifact) if artifact.is_file() else None

    say("manual-end-to-end EVIDENCE {} rc={} wall_s={:.1f} peak_rss_kb={} "
        "emitted_bytes={} text_bytes={} relax_passes={} islands={} host={} "
        "rss_method=\"{}\""
        .format(name, rc, wall, peak, emitted,
                "unreported" if text is None else text,
                scrape(log_text, RELAX_RE), scrape(log_text, ISLAND_RE),
                "{}-{}-{}".format(platform.system(), platform.machine(),
                                  platform.release()),
                peak_rss_method()))
    if rc != 0:
        say("manual-end-to-end compiler output (first 40 lines):")
        for line in log_text.splitlines()[:40]:
            say("    " + line)
    return rc == 0 and emitted > 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tree-root", required=True)
    ap.add_argument("--only", metavar="PATH_OR_NAME",
                    help="an entry's path under the corpus root, e.g. "
                         "c/branch_island_arm64_edge - or its bare leaf name "
                         "when that is unambiguous")
    ap.add_argument("--dsscp")
    ap.add_argument("--heartbeat", type=float, default=HEARTBEAT_DEFAULT)
    args = ap.parse_args()

    dsscp = Path(args.dsscp) if args.dsscp else find_dsscp(args.tree_root)
    say("manual-end-to-end compiler {}".format(dsscp))

    if not CORPUS.is_dir():
        raise SystemExit("manual-end-to-end: no corpus directory at {} — the "
                         "manual entries live beside this action by design"
                         .format(CORPUS))
    # ★ RECURSIVE, so the corpus can group by LANGUAGE the way `examples/` and
    # `real-examples/` do. The depth is free precisely because the invisibility
    # guarantee is a property of the ROOT, not of how deep this tree goes — see
    # the module docstring.
    entries = sorted(p.parent for p in CORPUS.rglob("expected.json"))
    if not entries:
        # NON-VACUITY. A driver that found nothing and exited 0 is the vacuous
        # pass this repository refuses everywhere else.
        raise SystemExit("manual-end-to-end: the corpus is EMPTY ({}). A run "
                         "over no entries is not a pass.".format(CORPUS))

    # ⚠ AN ENTRY NESTED INSIDE ANOTHER ENTRY IS AMBIGUOUS, and recursion is what
    # made that expressible at all. Refuse it by name rather than silently run
    # the outer one over a tree that contains the inner one's sources.
    nested = [(a, b) for a in entries for b in entries
              if a != b and b in a.parents]
    if nested:
        raise SystemExit(
            "manual-end-to-end: an entry is nested inside another entry, which "
            "is ambiguous:\n" + "\n".join(
                "    {} is inside {}".format(a.relative_to(CORPUS).as_posix(),
                                             b.relative_to(CORPUS).as_posix())
                for a, b in nested))

    names = [e.relative_to(CORPUS).as_posix() for e in entries]
    if args.only:
        # Accepts either the path under the corpus (`c/branch_island_arm64_edge`)
        # or the bare leaf, because the leaf is what a reader types. ⚠ BUT A LEAF
        # MATCHING MORE THAN ONE ENTRY IS REFUSED WITH BOTH NAMED: settling it by
        # sort order is the silent-wrong-answer class refused everywhere else
        # here, and recursion is what made two entries able to share a leaf.
        want = args.only.replace("\\", "/")
        exact = [e for e in entries
                 if e.relative_to(CORPUS).as_posix() == want]
        byleaf = [e for e in entries if e.name == want]
        if exact:
            entries = exact
        elif len(byleaf) > 1:
            raise SystemExit(
                "manual-end-to-end: --only {} is AMBIGUOUS - {} entries share "
                "that leaf: {}. Name the path under the corpus instead."
                .format(args.only, len(byleaf), ", ".join(
                    e.relative_to(CORPUS).as_posix() for e in byleaf)))
        elif byleaf:
            entries = byleaf
        else:
            raise SystemExit("manual-end-to-end: --only {} matched none of {}"
                             .format(args.only, names))

    ran = 0
    failed = 0
    for e in entries:
        verdict = run_entry(e, dsscp, args.tree_root, args.heartbeat)
        if verdict is None:
            continue
        ran += 1
        if not verdict:
            failed += 1

    # ★★★ THE WITNESS LINE, AND A VACUOUS RUN MUST NOT BE ABLE TO PRINT IT.
    # `successPattern` is matched as PLAIN TEXT by the existing shipped action,
    # so a pattern like `complete ran=` would match `ran=0` as happily as
    # `ran=7`: a run that skipped every entry would satisfy its own witness. The
    # counts therefore decide WHICH LINE IS PRINTED rather than what it says, so
    # the pattern cannot be satisfied by a run that executed nothing — the same
    # reason the clock-step probe witnesses its count and not its exit code.
    if ran == 0:
        say("manual-end-to-end executed-nothing skipped={} - every entry left "
            "`manualRun` absent or false, so there is no measurement here and "
            "this is not a pass".format(len(entries)))
        return 1
    say("manual-end-to-end complete ran={} failed={} skipped={}"
        .format(ran, failed, len(entries) - ran))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
