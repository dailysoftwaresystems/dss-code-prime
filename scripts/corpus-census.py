#!/usr/bin/env python3
# TF-C86 (D-CSUBSET-STDARG-F001A) — THE CORPUS CENSUS INSTRUMENT.
#
# ═══ WHY THIS EXISTS ═════════════════════════════════════════════════════════
#
# Before this instrument the only corpus measurement was
# `build/real-examples/c/sqlite/host/compile.log`: ONE OVERWRITTEN PATH WITH NO
# RUN IDENTITY. Different cycles reported 213 / 181 / 69 / 523 diagnostics and
# every one of those numbers wore the same filename, so no reader could tell
# which run produced which — `D-GATE-CORPUS-MEASURE-INSTRUMENT-INVALID`.
#
# The filename was never the real problem. The number that path carries IS NOT
# A CENSUS, for THREE independently-measured reasons:
#
# ── (1) THE PROJECT-WIDE PARSE GATE ──────────────────────────────────────────
#   `Program::runCusToTargets` (src/program/program.cpp) builds ALL N CUs
#   (`buildCus`, no early break — the FRONT END really does cover every TU) and
#   then, before the per-target loop:
#
#       // If parsing already failed, the per-target loop would only produce
#       // derivative noise.
#       if (rep.hasErrors()) { drainDiagnosticsToStderr(rep, bufs); return 1; }
#
#   `compileOneTarget` owns the SEMANTIC / HIR / MIR / LIR / codegen / link
#   tiers. So ONE parse error in ONE TU deletes the semantic-and-later census
#   for ALL of them. MEASURED on this corpus at bb75fb8: the whole-project run
#   was silent about `src/util.c` and `src/mutex_unix.c`, while compiling those
#   two in isolation yielded 5 `S0006` (`__uint128_t`) and 1 `S0001`
#   (`__sync_synchronize`). That is `D-MEASURE-FIRST-FAILURE-MASKS-RESIDUAL` at
#   PROJECT scope.
#
# ── (2) TWO DIFFERENT CODE RENDERINGS ────────────────────────────────────────
#   A diagnostic WITH a source buffer renders through
#   `DiagnosticReporter::format` as `error[P0009]` (`diagnosticCodePrefix`).
#   A BUFFER-LESS one renders through `drainDiagnosticsToStderr` as
#   `error[K_SymbolUndefined]` — the SYMBOLIC NAME (`diagnosticCodeName`).
#   Both are deliberate (program.cpp:110-119), but the consequence is not: any
#   census that greps for `[A-Z][0-9A-F]{4}` IS STRUCTURALLY BLIND to every
#   buffer-less diagnostic. MEASURED at bb75fb8: 133 of 189 per-TU runs
#   reported ONLY a symbolic-name diagnostic. This instrument parses BOTH forms
#   and normalizes the name back to its canonical code by reading the enum out
#   of `src/core/types/parse_diagnostic.hpp`.
#
# ── (3) THE PER-CODE CAP COALESCES SILENTLY ──────────────────────────────────
#   `DiagnosticReporter::Config` caps at `maxPerCode = 50`, and
#   diagnostic_reporter.cpp:215 says plainly: "Per-code cap: silently coalesce.
#   We don't emit a marker here". Unlike the total cap there is NO
#   `P_TooManyDiagnostics` to notice. Any code landing on exactly the cap is
#   reported here as a FLOOR, not a count.
#
# ═══ WHAT THIS INSTRUMENT DOES ═══════════════════════════════════════════════
#
# It compiles EVERY TU IN THE MANIFEST IN ISOLATION — one derived single-source
# manifest per source, carrying that manifest's own defines / includes — and
# captures each run's exit code DIRECTLY. No TU can mask another, because no
# two TUs share a reporter or a driver gate. It does this FOR EVERY TARGET LEG
# (pe / macho / elf by default), because a blocker cleared on one format is
# routinely still live on another.
#
# ★ THE ISOLATION FORMAT. A lone TU cannot form an EXECUTABLE: it has no
#   `main`, so the exec writer fails loud ("zero functions") and that failure is
#   an ARTIFACT OF THE MEASUREMENT, not a corpus defect. So the per-TU leg
#   compiles to the object format that serves the `staticlib` profile FOR THE
#   SAME FORMAT FAMILY as the leg's target. That sibling is DERIVED FROM THE
#   SHIPPED CONFIG (the format whose `artifactProfiles` contains `staticlib`
#   and whose name shares the leg format's base), never from a hardcoded
#   per-format table — the census stays as target/linker-agnostic as the
#   compiler it measures. MEASURED: under it, `ext/fts3/fts3_hash.c` and
#   `bld-dss/fts5.c` compile with exit 0 and zero diagnostics, while
#   `src/util.c`'s `S0006` and `src/mutex_unix.c`'s `S0001` still surface.
#
# Each leg also runs WHOLE-PROJECT exactly as the real build does, so the gap
# between "what the build prints" and "what is actually in the corpus" is a
# measured column rather than an argument.
#
# ═══ RUN IDENTITY IS IN-BAND, ALWAYS ═════════════════════════════════════════
#
# Every report carries, INSIDE THE FILE: the git HEAD (+ dirty marker), the UTC
# timestamp, every target spec, the compile config, the DSS binary path with
# size and mtime, the manifest path, and — the field that makes the artifact
# trustworthy — TUs-in-manifest vs attempted vs completed, PER LEG. Report
# files are named with the timestamp and HEAD, so two runs can never collide.
#
# ★ IF FEWER TUs ARE ATTEMPTED THAN THE MANIFEST DECLARES, THIS INSTRUMENT SAYS
#   SO LOUDLY AND EXITS NON-ZERO. A census that silently measured a subset and
#   reported the subset's count as the total is the exact defect it replaces.
#   `--self-test` demonstrates that failure mode on demand (RED-ON-DISABLE):
#   it drops one TU from the attempted set and the run must go INCOMPLETE.
#
# ═══ WHY PYTHON, WITH .sh AND .ps1 LAUNCHERS ═════════════════════════════════
#
# The census logic lives HERE, once. `scripts/corpus-census.sh` and
# `scripts/corpus-census.ps1` are thin launchers that locate an interpreter and
# exec this file, propagating the exit code and nothing else. Two hand-written
# ports of one census WOULD drift, and a drifted census is worse than a missing
# one: the platforms then disagree about the corpus and no one knows which to
# believe — the exact disease this cycle exists to cure. Neither launcher ever
# invokes the other.
#
# Exit: 0 = census completed and every manifest TU was attempted on every leg
#       1 = census ran but coverage was INCOMPLETE somewhere
#       2 = the census could not run at all (missing corpus, missing binary)

from __future__ import annotations

import argparse
import collections
import concurrent.futures
import datetime
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent

EXIT_OK, EXIT_INCOMPLETE, EXIT_CANNOT_RUN = 0, 1, 2

# The three predefine classes. `availableObjectFormats` keys on format KIND, so
# the shipped format files collapse to exactly these (same list, same reason, as
# scripts/pragma-profile-census.py).
DEFAULT_TARGETS = [
    "x86_64:pe64-x86_64-windows-exec",
    "arm64:macho64-arm64-darwin-exec",
    "x86_64:elf64-x86_64-linux-exec",
]

ISOLATION_PROFILE = "staticlib"
PER_CODE_CAP = 50   # DiagnosticReporter::Config::maxPerCode

# `error[P0009]:` (positioned) and `error[K_SymbolUndefined]` (buffer-less).
DIAG_RE = re.compile(r"^(error|warning|info)\[([A-Za-z_][A-Za-z_0-9]*)\]:?\s*(.*)$")
HEX_CODE_RE = re.compile(r"^[A-Z][0-9A-F]{4}$")

# `diagnosticCodePrefix` (src/core/types/parse_diagnostic.cpp): the high nibble
# carries the phase letter. Mirrored here ONLY to normalize the two renderings
# onto one code; the enum VALUES are read from the header, never duplicated.
NIBBLE_LETTER = {0x1000: "A", 0x4000: "R", 0x5000: "F", 0x8000: "K",
                 0xA000: "I", 0xB000: "L", 0xC000: "C", 0xD000: "D",
                 0xE000: "S", 0xF000: "H"}


def die(msg: str) -> "NoReturn":                       # noqa: F821
    print(f"corpus-census: FATAL: {msg}", file=sys.stderr)
    sys.exit(EXIT_CANNOT_RUN)


def info(msg: str) -> None:
    print(f"corpus-census: {msg}", file=sys.stderr)


def loud(msg: str) -> None:
    print(f"corpus-census: ** {msg}", file=sys.stderr)


# ── run identity ─────────────────────────────────────────────────────────────

def git(*args: str) -> str:
    try:
        out = subprocess.run(["git", "-C", str(REPO_ROOT), *args],
                             capture_output=True, text=True, check=False)
        return out.stdout.strip() if out.returncode == 0 else "UNKNOWN"
    except OSError:
        return "UNKNOWN"


def canonical_code(value: int) -> str:
    nib = value & 0xF000
    letter = NIBBLE_LETTER.get(nib, "P")
    lo = (value & 0x0FFF) if nib in NIBBLE_LETTER else value
    return f"{letter}{lo:04X}"


def read_code_name_table(header: Path) -> dict[str, str]:
    """name -> canonical code, read from the DiagnosticCode enum.

    This is what lets ONE census row cover both renderings. Read rather than
    duplicated so a new code cannot silently fall outside the census."""
    table: dict[str, str] = {}
    if not header.is_file():
        return table
    enum_re = re.compile(r"^\s*([A-Za-z_][A-Za-z_0-9]*)\s*=\s*0x([0-9A-Fa-f]{4})\s*,")
    for line in header.read_text(errors="replace").splitlines():
        m = enum_re.match(line)
        if m:
            table[m.group(1)] = canonical_code(int(m.group(2), 16))
    return table


# ── the isolation target, DERIVED FROM THE SHIPPED CONFIG ────────────────────

def derive_isolation_target(spec: str, formats_dir: Path) -> tuple[str, str | None, str]:
    """(target, profile, note) for compiling ONE TU alone.

    Structural, not tabular: the format whose name shares this format's base
    (everything up to the last '-') and whose `artifactProfiles` contains
    `staticlib`. No branch on a format's identity."""
    arch, _, fmt = spec.partition(":")
    base = fmt.rsplit("-", 1)[0] if "-" in fmt else fmt
    for path in sorted(formats_dir.glob("*.format.json")):
        name = path.name[: -len(".format.json")]
        if not name.startswith(base + "-"):
            continue
        try:
            profiles = json.loads(path.read_text()).get("artifactProfiles") or []
        except (OSError, ValueError):
            continue
        if ISOLATION_PROFILE in profiles:
            return (f"{arch}:{name}" if arch else name, ISOLATION_PROFILE,
                    f"derived staticlib sibling of '{fmt}'")
    # Fail LOUD in the report rather than silently measuring with a format that
    # will manufacture isolation artifacts and then call them defects.
    return (spec, None,
            f"** NO staticlib-serving sibling found for '{fmt}' — this leg runs "
            f"the manifest's own format and WILL manufacture isolation "
            f"artifacts (a lone TU has no entry point)")


# ── one compiler invocation ──────────────────────────────────────────────────

def run_dss(dss_bin: Path, manifest: Path, out_dir: Path, config: str | None,
            log: Path) -> int:
    cmd = [str(dss_bin), "--project", str(manifest), "--output", str(out_dir)]
    if config:
        cmd += ["--config", config]
    with log.open("wb") as fh:
        proc = subprocess.run(cmd, stdout=fh, stderr=subprocess.STDOUT,
                              check=False)
    return proc.returncode        # captured DIRECTLY from the process


def scan_log(path: Path, name_to_code: dict[str, str]):
    """-> (Counter(code), {code: severity}, {code: Counter(msg)}, set(unresolved))"""
    counts: collections.Counter = collections.Counter()
    sev: dict[str, str] = {}
    msgs: dict[str, collections.Counter] = collections.defaultdict(collections.Counter)
    unresolved: set[str] = set()
    if not path.is_file():
        return counts, sev, msgs, unresolved
    for line in path.read_text(errors="replace").splitlines():
        m = DIAG_RE.match(line)
        if not m:
            continue
        severity, raw, msg = m.groups()
        if HEX_CODE_RE.match(raw):
            code = raw
        elif raw in name_to_code:
            code = name_to_code[raw]
        else:
            code, _ = raw, unresolved.add(raw)
        counts[code] += 1
        sev[code] = severity
        # `got X` is the generic renderer's prefix; keep the payload readable.
        msgs[code][msg.strip()[:140]] += 1
    return counts, sev, msgs, unresolved


# ── one target leg ───────────────────────────────────────────────────────────

class Leg:
    def __init__(self, spec: str, manifest: dict, manifest_path: Path,
                 scratch: Path, formats_dir: Path):
        self.spec = spec
        self.tu_target, self.tu_profile, self.tu_note = \
            derive_isolation_target(spec, formats_dir)
        self.manifest = manifest
        self.manifest_path = manifest_path
        safe = re.sub(r"[^A-Za-z0-9._-]", "-", spec)
        self.dir = scratch / safe
        (self.dir / "manifests").mkdir(parents=True, exist_ok=True)
        (self.dir / "logs").mkdir(parents=True, exist_ok=True)
        (self.dir / "out").mkdir(parents=True, exist_ok=True)
        self.slots: list[tuple[str, str]] = []
        self.rc: dict[str, int] = {}
        self.whole_rc: int | None = None
        # aggregate
        self.per_code: collections.Counter = collections.Counter()
        self.per_code_sev: dict[str, str] = {}
        self.per_code_tus: dict[str, set] = collections.defaultdict(set)
        self.per_code_msgs: dict[str, collections.Counter] = \
            collections.defaultdict(collections.Counter)
        self.whole_counts: collections.Counter = collections.Counter()
        self.unresolved: set[str] = set()
        self.saturated: list[tuple[str, str, int]] = []
        self.clean: list[str] = []
        self.silent_fail: list[tuple[str, int]] = []
        self.noisy_ok: list[str] = []
        self._attempted: int | None = None

    def derive(self) -> None:
        sources = self.manifest["sources"]
        for i, src in enumerate(sources):
            # The slot id carries the INDEX, so two sources sharing a basename
            # (sqlite really does ship several) cannot collide onto one log.
            slot = "%04d-%s" % (i, os.path.basename(src))
            derived = dict(self.manifest)
            derived["sources"] = [src]
            derived["targets"] = [self.tu_target]
            if self.tu_profile:
                derived["artifactProfile"] = self.tu_profile
            # `resolveLibraries` is a whole-IMAGE concern; one TU is not an image.
            derived.pop("resolveLibraries", None)
            derived["artifactName"] = "census_%04d" % i
            (self.dir / "manifests" / f"{slot}.json").write_text(
                json.dumps(derived, indent=2))
            self.slots.append((slot, src))
        whole = dict(self.manifest)
        whole["targets"] = [self.spec]
        (self.dir / "whole.json").write_text(json.dumps(whole, indent=2))

    def compile_all(self, dss_bin: Path, config: str | None, jobs: int,
                    skip_slots: set[str]) -> None:
        def one(slot: str) -> tuple[str, int]:
            rc = run_dss(dss_bin, self.dir / "manifests" / f"{slot}.json",
                         self.dir / "out" / slot, config,
                         self.dir / "logs" / f"{slot}.log")
            return slot, rc
        todo = [s for s, _ in self.slots if s not in skip_slots]
        with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
            for slot, rc in pool.map(one, todo):
                self.rc[slot] = rc
        self.whole_rc = run_dss(dss_bin, self.dir / "whole.json",
                                self.dir / "whole-out", config,
                                self.dir / "whole.log")

    def aggregate(self, name_to_code: dict[str, str]) -> None:
        # Latch coverage while the scratch tree still exists (see `attempted`).
        self._attempted = len(list((self.dir / "logs").glob("*.log")))
        for slot, src in self.slots:
            counts, sev, msgs, unk = scan_log(
                self.dir / "logs" / f"{slot}.log", name_to_code)
            self.unresolved |= unk
            for code, n in counts.items():
                self.per_code[code] += n
                self.per_code_sev[code] = sev[code]
                self.per_code_tus[code].add(src)
                self.per_code_msgs[code].update(msgs[code])
                if n == PER_CODE_CAP:
                    self.saturated.append((src, code, n))
            rc = self.rc.get(slot)
            if not counts:
                self.clean.append(src)
                if rc not in (None, 0):
                    self.silent_fail.append((src, rc))
            elif rc == 0:
                self.noisy_ok.append(src)
        wc, _, _, wunk = scan_log(self.dir / "whole.log", name_to_code)
        self.whole_counts = wc
        self.unresolved |= wunk

    # coverage
    @property
    def in_manifest(self) -> int:
        return len(self.manifest["sources"])

    @property
    def attempted(self) -> int:
        # Latched at aggregate() time: the scratch tree is deleted before the
        # final coverage verdict prints, and a property that re-globbed a
        # removed directory would report 0 attempted and contradict the report
        # it just wrote. (Caught by `--self-test`, which is what it is for.)
        if self._attempted is not None:
            return self._attempted
        return len(list((self.dir / "logs").glob("*.log")))

    @property
    def completed(self) -> int:
        return len(self.rc)

    @property
    def covered(self) -> bool:
        return self.in_manifest == self.attempted == self.completed

    @property
    def errors(self) -> int:
        return sum(n for c, n in self.per_code.items()
                   if self.per_code_sev[c] == "error")


# ── report ───────────────────────────────────────────────────────────────────

def write_report(out: "list[str]", legs: list[Leg], meta: dict,
                 name_to_code: dict[str, str], diag_header: Path) -> None:
    w = out.append
    w("=" * 78)
    w("DSS CORPUS CENSUS — per-TU isolated, full-manifest coverage")
    w("=" * 78)
    w("")
    w("-- RUN IDENTITY (in-band; this block is what makes the file citable) --")
    for label, key in (("utc", "utc"), ("git HEAD", "head"),
                       ("git branch", "branch"), ("tree state", "dirty"),
                       ("config", "config"), ("dss binary", "dss_bin"),
                       ("dss binary size", "bin_size"),
                       ("dss binary mtime", "bin_mtime"),
                       ("manifest", "manifest"), ("concurrency", "jobs"),
                       ("instrument", "instrument")):
        w(f"  {label:<22}: {meta[key]}")
    w(f"  {'code-name table':<22}: "
      + (f"{len(name_to_code)} enum rows read from {diag_header}"
         if len(name_to_code) > 100
         else "** UNAVAILABLE — symbolic-name diagnostics stay unnormalized"))
    if meta.get("self_test"):
        w(f"  {'MODE':<22}: ** SELF-TEST — one TU deliberately skipped per leg;")
        w(f"  {'':<22}   this run MUST report INCOMPLETE and exit 1.")
    w("")

    w("-- COVERAGE PER LEG (a census that measured a subset must say so) --")
    w(f"  {'target':<38} {'manifest':>8} {'tried':>6} {'done':>6}  verdict")
    for leg in legs:
        w(f"  {leg.spec:<38} {leg.in_manifest:>8} {leg.attempted:>6} "
          f"{leg.completed:>6}  "
          + ("COMPLETE" if leg.covered else "** INCOMPLETE — NOT A TOTAL"))
    w("")

    w("-- PER-LEG SUMMARY --")
    w(f"  {'target':<38} {'errors':>7} {'whole-rc':>9}  isolation target")
    for leg in legs:
        w(f"  {leg.spec:<38} {leg.errors:>7} {str(leg.whole_rc):>9}  {leg.tu_target}")
    w("")

    for leg in legs:
        w("=" * 78)
        w(f"LEG  {leg.spec}")
        w("=" * 78)
        w(f"  per-TU isolation target : {leg.tu_target}")
        w(f"  per-TU profile          : {leg.tu_profile}")
        w(f"  per-TU leg note         : {leg.tu_note}")
        rc_hist = collections.Counter(leg.rc.values())
        w("  per-TU exit codes       : "
          + ", ".join(f"rc={k} x{v}" for k, v in sorted(rc_hist.items())))
        w(f"  TUs clean (rc=0, no diagnostics) : "
          f"{len(leg.clean) - len(leg.silent_fail)}")
        w("")
        if leg.silent_fail:
            w(f"  -- ** SILENT FAILURES: non-zero exit, ZERO diagnostics "
              f"({len(leg.silent_fail)}) --")
            w("     A compiler that fails without saying why is the archetype this")
            w("     project forbids. Each of these needs a diagnostic.")
            for src, rc in leg.silent_fail[:40]:
                w(f"        rc={rc:<3} {src}")
            if len(leg.silent_fail) > 40:
                w(f"        ... and {len(leg.silent_fail) - 40} more")
            w("")
        if leg.unresolved:
            w(f"  -- ** CODES THAT COULD NOT BE NORMALIZED ({len(leg.unresolved)}) --")
            w("     Rendered as a symbolic name with no matching enum row.")
            for n in sorted(leg.unresolved):
                w(f"        {n}")
            w("")
        if leg.saturated:
            w("  -- ** PER-CODE CAP SATURATION — THESE COUNTS ARE FLOORS --")
            w(f"     DiagnosticReporter coalesces SILENTLY past maxPerCode="
              f"{PER_CODE_CAP} (src/core/types/diagnostic_reporter.cpp:215).")
            for src, code, n in leg.saturated:
                w(f"        {code:<8} {n}  {src}")
            w("")
        w(f"  -- PER-CODE CENSUS (isolated per-TU runs, all {leg.in_manifest} TUs) --")
        w(f"     {'code':<10} {'severity':<9} {'count':>6}  {'TUs':>5}  distinct msgs")
        for code, n in sorted(leg.per_code.items(), key=lambda kv: (-kv[1], kv[0])):
            w(f"     {code:<10} {leg.per_code_sev[code]:<9} {n:>6}  "
              f"{len(leg.per_code_tus[code]):>5}  {len(leg.per_code_msgs[code])}")
        if not leg.per_code:
            w("     (no diagnostics at all)")
        w(f"     {'TOTAL':<10} {'error':<9} {leg.errors:>6}")
        w("")
        w(f"  -- WHOLE-PROJECT LEG (what the real build prints; exit {leg.whole_rc}) --")
        w("     ** A FLOOR, NOT A CENSUS: program.cpp returns before the semantic")
        w("        and later tiers run when ANY TU has a front-end error.")
        w(f"     {'code':<10} {'whole':>6}  {'census':>6}   hidden by the gate")
        for code in sorted(set(leg.whole_counts) | set(leg.per_code)):
            wv, cv = leg.whole_counts.get(code, 0), leg.per_code.get(code, 0)
            w(f"     {code:<10} {wv:>6}  {cv:>6}   "
              + ("yes" if wv == 0 and cv > 0 else ""))
        w("")
        w("  -- PER-CODE DETAIL --")
        for code, n in sorted(leg.per_code.items(), key=lambda kv: (-kv[1], kv[0])):
            w(f"\n     [{code}] {leg.per_code_sev[code]} x{n} across "
              f"{len(leg.per_code_tus[code])} TU(s)")
            for msg, c in leg.per_code_msgs[code].most_common(30):
                w(f"         x{c:<5} {msg}")
            extra = len(leg.per_code_msgs[code]) - 30
            if extra > 0:
                w(f"         ... and {extra} more distinct messages")
            for src in sorted(leg.per_code_tus[code])[:30]:
                w(f"         TU  {src}")
            extra = len(leg.per_code_tus[code]) - 30
            if extra > 0:
                w(f"         TU  ... and {extra} more TUs")
        w("")
        good = [p for p in leg.clean if p not in {q for q, _ in leg.silent_fail}]
        w(f"  -- TUs THAT COMPILE CLEAN IN ISOLATION ({len(good)} of "
          f"{leg.in_manifest}) --")
        for p in good:
            w(f"     {p}")
        w("")


# ── main ─────────────────────────────────────────────────────────────────────

def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(
        prog="corpus-census",
        description="Per-TU isolated diagnostic census of the sqlite corpus.")
    ap.add_argument("--target", action="append", default=None,
                    help="target spec to census; repeatable "
                         "(default: the three predefine classes)")
    ap.add_argument("--config", default=None,
                    help="compile config passed to DSS (debug|release)")
    ap.add_argument("--jobs", type=int, default=None,
                    help="concurrent TU compiles (default: cores/2, min 1)")
    ap.add_argument("--out", default=None, help="report directory")
    ap.add_argument("--manifest", default=None, help="corpus manifest to census")
    ap.add_argument("--dss-bin", default=None, help="the compiler to measure")
    ap.add_argument("--keep-logs", action="store_true",
                    help="keep the per-TU logs beside the report")
    ap.add_argument("--self-test", action="store_true",
                    help="RED-ON-DISABLE: skip one TU per leg; the run MUST "
                         "then report INCOMPLETE and exit 1")
    args = ap.parse_args(argv)

    # ── the compiler ──
    dss_bin = Path(args.dss_bin) if args.dss_bin else None
    if dss_bin is None:
        env = os.environ.get("DSS_BIN")
        if env:
            dss_bin = Path(env)
        else:
            default = REPO_ROOT / "build" / "bin" / "dss" / "dss-code-prime"
            if default.is_file():
                dss_bin = default
            else:
                found = sorted(REPO_ROOT.glob("build/**/dss-code-prime"))
                dss_bin = found[0] if found else None
    if dss_bin is None or not dss_bin.is_file() or not os.access(dss_bin, os.X_OK):
        die("dss-code-prime not found; build the project or pass --dss-bin.")

    # ── the corpus manifest ──
    manifest_path = Path(args.manifest or os.environ.get("SQLITE_MANIFEST")
                         or (REPO_ROOT / "build" / "real-examples" / "c"
                             / "sqlite" / "host" / "host.dss-project.json"))
    if not manifest_path.is_file():
        die(f"no corpus manifest at {manifest_path} — run "
            f"real-examples/c/sqlite/build-and-test.sh first, or pass --manifest.")
    try:
        manifest = json.loads(manifest_path.read_text())
    except ValueError as exc:
        die(f"manifest is not valid JSON: {exc}")
    if not isinstance(manifest.get("sources"), list) or not manifest["sources"]:
        die("manifest has no 'sources' array")

    targets = args.target or DEFAULT_TARGETS
    jobs = args.jobs or max(1, (os.cpu_count() or 4) // 2)

    now = datetime.datetime.now(datetime.timezone.utc)
    stat = dss_bin.stat()
    meta = {
        "utc": now.strftime("%Y-%m-%dT%H:%M:%SZ"),
        "head": git("rev-parse", "HEAD"),
        "branch": git("rev-parse", "--abbrev-ref", "HEAD"),
        "dirty": ("DIRTY — the tree does not match this HEAD"
                  if git("status", "--porcelain") else "clean"),
        "config": args.config or "<compiler default>",
        "dss_bin": str(dss_bin),
        "bin_size": f"{stat.st_size} bytes",
        "bin_mtime": datetime.datetime.fromtimestamp(
            stat.st_mtime, datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "manifest": str(manifest_path),
        "jobs": jobs,
        "instrument": str(Path(__file__).resolve()),
        "self_test": args.self_test,
    }

    diag_header = REPO_ROOT / "src" / "core" / "types" / "parse_diagnostic.hpp"
    name_to_code = read_code_name_table(diag_header)

    info(f"HEAD={meta['head']} ({meta['branch']}, {meta['dirty']})")
    info(f"legs: {', '.join(targets)}")
    info(f"{len(manifest['sources'])} TUs per leg, jobs={jobs}")

    scratch = Path(tempfile.mkdtemp(prefix="dss-corpus-census."))
    legs: list[Leg] = []
    formats_dir = REPO_ROOT / "src" / "dss-config" / "object-formats"
    try:
        for spec in targets:
            leg = Leg(spec, manifest, manifest_path, scratch, formats_dir)
            leg.derive()
            skip = set()
            if args.self_test and leg.slots:
                # RED-ON-DISABLE: withhold exactly one TU. A census that cannot
                # notice a missing TU is not an instrument.
                skip = {leg.slots[0][0]}
            info(f"leg {spec}: isolation target {leg.tu_target} "
                 f"[{leg.tu_note}] — compiling ...")
            leg.compile_all(dss_bin, args.config, jobs, skip)
            leg.aggregate(name_to_code)
            info(f"leg {spec}: {leg.errors} errors, coverage "
                 f"{leg.attempted}/{leg.in_manifest}")
            legs.append(leg)

        out_dir = Path(args.out) if args.out else (
            REPO_ROOT / "build" / "real-examples" / "c" / "sqlite" / "census")
        out_dir.mkdir(parents=True, exist_ok=True)
        stamp = now.strftime("%Y%m%dT%H%M%SZ")
        report = out_dir / f"census-{stamp}-{meta['head'][:12]}.txt"
        lines: list[str] = []
        write_report(lines, legs, meta, name_to_code, diag_header)
        report.write_text("\n".join(lines) + "\n")
        info(f"report written: {report}")

        if args.keep_logs:
            keep = out_dir / f"logs-{stamp}-{meta['head'][:12]}"
            if keep.exists():
                shutil.rmtree(keep)
            shutil.copytree(scratch, keep)
            info(f"per-TU logs kept in {keep}")
    finally:
        shutil.rmtree(scratch, ignore_errors=True)

    incomplete = [leg for leg in legs if not leg.covered]
    if incomplete:
        for leg in incomplete:
            loud(f"COVERAGE INCOMPLETE on {leg.spec}: manifest declares "
                 f"{leg.in_manifest} TUs, attempted={leg.attempted} "
                 f"completed={leg.completed}.")
        loud("The per-code counts for those legs are a SUBSET, not a total.")
        return EXIT_INCOMPLETE
    info(f"coverage COMPLETE on every leg: {legs[0].in_manifest} TUs x "
         f"{len(legs)} targets.")
    return EXIT_OK


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
