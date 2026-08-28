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
# ★ ...AND CHANGING THE TARGET CHANGES WHAT THE MANIFEST MAY SAY. Swapping the
#   format is only half of deriving an isolation manifest: some manifest keys
#   are requests the NEW format cannot honour, and DSS refuses them — correctly
#   — so the census would count the compiler's refusal of the INSTRUMENT's own
#   request as a corpus defect. See THE ISOLATION MANIFEST'S KEY CLASSIFICATION
#   below; that gap cost 189 phantom errors on pe64 before TF-C112 closed it.
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
# ★ AND THE CORPUS, NOT JUST THE COMPILER (TF-C112). Recording HEAD / branch /
#   config / binary to the byte while saying nothing about WHICH TREE was
#   measured let two reports agree on every identity field and still count
#   different corpora — which is precisely why a "pe 13" and a "pe 10" figure
#   were never reconcilable. Every report now also carries the corpus root, its
#   `SQLITE_VERSION` / `SQLITE_SOURCE_ID` (read from the generated header the
#   compile itself resolves) and a machine-independent SHA-256 over the source
#   set. A corpus that cannot be identified STOPS THE RUN before one TU is
#   compiled — see CORPUS IDENTITY below for why "unknown" was not an option.
#
# ★ IF FEWER TUs ARE ATTEMPTED THAN THE MANIFEST DECLARES, THIS INSTRUMENT SAYS
#   SO LOUDLY AND EXITS NON-ZERO. A census that silently measured a subset and
#   reported the subset's count as the total is the exact defect it replaces.
#   `--self-test` demonstrates that failure mode on demand (RED-ON-DISABLE):
#   it drops one TU from the attempted set and the run must go INCOMPLETE.
#
# ═══ WHY PYTHON, WITH .sh AND .ps1 LAUNCHERS ═════════════════════════════════
#
# The census logic lives HERE, once. `scripts/corpus-census/corpus-census.sh` and
# `scripts/corpus-census/corpus-census.ps1` are thin launchers that locate an interpreter and
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
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

# Resolved from THIS FILE, which now lives one level deeper
# (`scripts/corpus-census/`) than it used to. Counting `..` hops is exactly
# the fragility that broke on the move, so the walk is ANCHORED on a marker
# the repo root always has and a script directory never does.
EXIT_OK, EXIT_INCOMPLETE, EXIT_CANNOT_RUN = 0, 1, 2

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
    print(f"corpus-census: FATAL: no repo root at or above {here} "
          f"(looked for a directory holding both CMakeLists.txt and src/)",
          file=sys.stderr)
    sys.exit(EXIT_CANNOT_RUN)

REPO_ROOT = _repo_root()

# The three predefine classes. `availableObjectFormats` keys on format KIND, so
# the shipped format files collapse to exactly these (same list, same reason, as
# scripts/pragma-profile-census/pragma-profile-census.py).
DEFAULT_TARGETS = [
    "x86_64:pe64-x86_64-windows-exec",
    "arm64:macho64-arm64-darwin-exec",
    "x86_64:elf64-x86_64-linux-exec",
]

ISOLATION_PROFILE = "staticlib"

# ★★★ `PER_CODE_CAP = 50` IS GONE (D-DIAG-MAXPERCODE-SILENT-COALESCE, P36).
#
# It was a hand-copy of `DiagnosticReporter::Config::maxPerCode`, used to detect
# a saturated count by asking `n == PER_CODE_CAP`. TWO things were wrong with
# that, and the second is the one that mattered:
#   * it was a MIRROR -- move the C++ default and this instrument silently
#     re-labels floors as totals;
#   * it was a ROUND-NUMBER SNIFF WITH NO FALSE-NEGATIVE BOUND. A code capped at
#     50 out of 51 is indistinguishable from one that genuinely occurred 50
#     times, so the detector could not tell a floor from a total in the one case
#     where it matters most.
# The compiler now says so IN BAND: `P_DiagnosticsElided` (renders `P9007`) is
# emitted once per elided code carrying the code, the cap and the counts. Floor
# detection is therefore EXACT and needs no knowledge of the cap's value.
ELISION_MARKER_CODE = "P9007"   # dss::DiagnosticCode::P_DiagnosticsElided

# The marker names the code it abbreviates in its rendered spelling, e.g.
# `S0006 (S_UnknownTypeName) diagnostics were ELIDED ...`.
ELIDED_CODE_RE = re.compile(r"^([A-Z?][0-9A-F]{4})\s*\(")

# Windows spells the compiler `dsscp.exe`. Used ONLY when auto-
# discovering the binary; an explicit `--dss-bin` / `DSS_BIN` is taken verbatim.
EXE_SUFFIX = ".exe" if os.name == "nt" else ""

# ── THE ISOLATION MANIFEST'S KEY CLASSIFICATION (TF-C112) ────────────────────
#
# Deriving a single-TU manifest is not "copy it and swap `sources`". Some
# manifest keys describe THE WHOLE IMAGE, and an image is exactly what a lone TU
# is not. Hand one of those to the isolation target and DSS does the RIGHT thing
# — it REFUSES the build — and the census then books the compiler's correct
# refusal of the INSTRUMENT'S OWN REQUEST as a corpus defect.
#
# MEASURED (TF-C112, pe64, complete 189/189 coverage): carrying the manifest's
# `stackReserve` onto the derived staticlib sibling produced 189 errors, ALL of
# them `K_FormatLacksStackReserveControl` and not one of them from the corpus.
# And the damage is worse than inflation — `linker::link` runs that gate FIRST
# and `return`s on failure (src/link/linker.cpp:613), so the per-TU LINK TIER
# never ran at all. The phantom errors did not sit on top of a valid link
# census; they REPLACED it.
#
# Every top-level manifest key is therefore classified into exactly one of the
# four buckets below. They are kept exhaustive by `classify_manifest_keys`,
# which REFUSES to census a manifest carrying a key in none of them — so the
# next field added to the schema's `kKnownKeys`
# (src/program/project_config.cpp) cannot silently start inflating the count the
# way `stackReserve` did.

# (1) Carried VERBATIM — genuine per-TU compile inputs.
PER_TU_KEYS = ("language", "includes", "defines", "output")

# (2) REPLACED outright by the isolation manifest.
ISOLATION_REPLACED_KEYS = ("sources", "targets", "artifactProfile",
                           "artifactName")

# (3) Whole-IMAGE scope, independent of any format capability: a lone TU is not
#     an image, so an export surface to resolve this build's externs against is
#     meaningless here (and would drag a whole DLL's symbol set into a per-TU
#     measurement).
IMAGE_SCOPE_KEYS = ("resolveLibraries",)

# (4) FORMAT-CONDITIONAL. The driver forwards each of these to the link step as
#     an `ImageRequest` (src/link/image_request.hpp), where the CHOSEN OBJECT
#     FORMAT must DECLARE the matching capability or DSS refuses
#     (`enforceImageRequest`, src/link/image_request.cpp:44-63).
#
#     So the census ASKS THE FORMAT — reading the very `.format.json` key the
#     linker gate reads — rather than filtering a diagnostic code or branching
#     on a format's identity. Filtering the code would hide a GENUINE refusal
#     (an out-of-range value still earns `K_InvalidStackReserveRequest`);
#     asking the capability drops the request only where the format provably
#     cannot carry it, and carries it wherever the format can. Today exactly one
#     shipped format declares `stackReserveControl` (pe64-x86_64-windows-exec);
#     every staticlib sibling declares `stackReserveUnsupportedReason` instead.
#
#     The REAL target's request is never suppressed: the WHOLE-PROJECT leg
#     compiles the manifest UNMODIFIED, so a genuine refusal on the real format
#     still lands in this report.
FORMAT_CAPABILITY_KEYS = {
    "stackReserve": "stackReserveControl",
}

# ── CORPUS IDENTITY (TF-C112) ────────────────────────────────────────────────
#
# The run-identity block used to pin the COMPILER exactly and the CORPUS not at
# all, so two reports could agree on git HEAD, branch, config, binary and
# manifest path and still have counted different trees. Not hypothetical: a
# "pe 13" and a "pe 10" figure were never reconcilable because the shared sqlite
# clone moved between the two runs and no field in either report could show it.
# A census whose corpus is unnamed cannot be compared with any other census, so
# the number it prints is unquotable.
#
# TWO independent identities are recorded, because neither alone suffices:
#
#   * THE UPSTREAM STAMP — `SQLITE_VERSION` / `SQLITE_SOURCE_ID` out of the
#     generated `sqlite3.h`. It names the upstream check-in, and it is the field
#     a human can match against a git log. It CANNOT see a local edit: the stamp
#     is baked when the amalgamation is generated, so a patched `src/util.c`
#     still reports the pristine SOURCE_ID.
#
#   * THE CONTENT DIGEST — SHA-256 folded over every source the manifest names
#     (corpus-relative path + that file's own digest, in sorted order).
#     Machine-independent and byte-exact: equal digests mean the same corpus,
#     full stop. It is what actually proves "never patch the staged sqlite tree"
#     was honoured. It covers the SOURCE SET ONLY, not the include tree — stated
#     in the report rather than left for a reader to assume.
#
# ★ A MISSING STAMP IS FATAL, NOT "unknown". A run-identity field that can read
#   "unknown" is decoration: the 13-vs-10 argument happened WITH both numbers in
#   hand, and a blank corpus row would not have stopped it. Refusing to census
#   is the only outcome nobody can quote by mistake — and the check runs BEFORE
#   the first TU compiles, so refusing costs seconds, not an hour.
CORPUS_VERSION_HEADER = "sqlite3.h"
CORPUS_VERSION_MACROS = ("SQLITE_VERSION", "SQLITE_SOURCE_ID")
# Anchored at both ends: the trailing `\s+` is what keeps `SQLITE_VERSION` from
# also matching `SQLITE_VERSION_NUMBER` (a bare integer, not the stamp).
CORPUS_VERSION_MACRO_RE = re.compile(
    r'^\s*#\s*define\s+(' + "|".join(CORPUS_VERSION_MACROS) + r')\s+"([^"]*)"')

# `error[P0009]:` (positioned) and `error[K_SymbolUndefined]` (buffer-less).
DIAG_RE = re.compile(r"^(error|warning|info)\[([A-Za-z_][A-Za-z_0-9]*)\]:?\s*(.*)$")
HEX_CODE_RE = re.compile(r"^[A-Z][0-9A-F]{4}$")

# `diagnosticCodePrefix` (src/core/types/parse_diagnostic.cpp): the high nibble
# carries the phase letter. Mirrored here ONLY to normalize the two renderings
# onto one code; the enum VALUES are read from the header, never duplicated.
#
# ⚠ THIS DICT IS A HAND-MIRRORED COPY AND IT HAS ALREADY DRIFTED ONCE
# (D-DIAG-OPT-FAMILY-NIBBLE-CLAIMED-IN-HEADER-BUT-NOT-IN-RENDERER, TF-C118,
# 2026-08-04). `0x2000: "X"` (the optimizer band) was missing here for the same
# reason it was missing from the renderer itself: the X_* family was claimed in
# parse_diagnostic.hpp and nowhere else. The consequence was NOT cosmetic for
# this instrument — `canonical_code` falls back to letter "P" AND skips the
# nibble strip for an unlisted nibble, so every optimizer diagnostic would have
# been censused as `P2001`..`P2008`, i.e. attributed to the PARSER, in the very
# tool whose job is to attribute failures to a tier. Adding a family means
# updating THREE places, not two: the header, `diagnosticCodePrefix`, and here.
# ★★★ THE MIRROR IS RETIRED (D-DIAG-CODE-PREFIX-DEFAULT-IS-SILENT, P36).
#
# This dict used to be hand-copied from `diagnosticCodePrefix`, and it had
# drifted identically: `0x2000: "X"` was missing here for exactly as long as it
# was missing there, so every optimizer diagnostic was censused as
# `P2001..P2008` -- attributed to the PARSER, in the instrument whose entire job
# is attributing failures to a tier. A hand-copy of a table cannot be kept
# honest by asking people to remember; it has to be READ.
#
# It is now parsed out of the compiler's own `kNibbleFamilies` table, which is
# the idiom this file already uses for the enum itself (`read_code_name_table`
# reads `parse_diagnostic.hpp` rather than duplicating it, on the stated ground
# that anything asking a human to keep a second list in sync has the failure
# mode it exists to catch).
UNALLOCATED_FAMILY_LETTER = "?"   # must match dss::kUnallocatedFamilyLetter

# `/* 0xE */ {'S', true,  "semantic analysis"},`
_NIBBLE_ROW_RE = re.compile(
    r"/\*\s*0x([0-9A-Fa-f])\s*\*/\s*\{\s*"
    r"(?:'(.)'|kUnallocatedFamilyLetter)\s*,\s*(true|false)\s*,")


def read_nibble_families(source: Path) -> dict[int, tuple[str, bool]]:
    """high-nibble -> (letter, strips_nibble), read from parse_diagnostic.cpp.

    ⚠ FAILS LOUD ON AN EMPTY OR SHORT PARSE. A census that silently read zero
    families would fall back to guessing for every code -- the same class of
    defect as a scan reporting a clean tree it never actually read."""
    if not source.is_file():
        die(f"cannot read the diagnostic family table: {source} does not exist")
    rows: dict[int, tuple[str, bool]] = {}
    for m in _NIBBLE_ROW_RE.finditer(source.read_text(encoding="utf-8",
                                                      errors="replace")):
        nib = int(m.group(1), 16)
        letter = m.group(2) if m.group(2) else UNALLOCATED_FAMILY_LETTER
        rows[nib] = (letter, m.group(3) == "true")
    if len(rows) != 16:
        die(f"{source}: parsed {len(rows)} of 16 diagnostic family nibbles from "
            f"`kNibbleFamilies`. That table is addressed by index and has one "
            f"row per high nibble, so anything but 16 means this parser has "
            f"drifted from the table it reads -- fix the parser rather than "
            f"letting the census guess a family letter.")
    return rows


NIBBLE_FAMILIES = read_nibble_families(
    REPO_ROOT / "src" / "core" / "types" / "parse_diagnostic.cpp")


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
    # ⚠ NO `"P"` FALLBACK. The old `.get(nib, "P")` was row 1's defect in
    # Python: an unlisted nibble rendered under the PARSER's letter AND skipped
    # the nibble strip, so a family this instrument had not been told about was
    # not merely unlabelled -- it was labelled as somebody else's. The table is
    # now total over all 16 nibbles, so the lookup cannot miss; an unallocated
    # family renders `?` exactly as the compiler renders it, which no reader and
    # no `[A-Z][0-9A-F]{4}` scraper can mistake for a real code.
    letter, strips = NIBBLE_FAMILIES[(value & 0xF000) >> 12]
    lo = (value & 0x0FFF) if strips else value
    return f"{letter}{lo:04X}"


def read_code_name_table(header: Path) -> dict[str, str]:
    """name -> canonical code, read from the DiagnosticCode enum.

    This is what lets ONE census row cover both renderings. Read rather than
    duplicated so a new code cannot silently fall outside the census."""
    table: dict[str, str] = {}
    if not header.is_file():
        return table
    enum_re = re.compile(r"^\s*([A-Za-z_][A-Za-z_0-9]*)\s*=\s*0x([0-9A-Fa-f]{4})\s*,")
    for line in header.read_text(encoding="utf-8", errors="replace").splitlines():
        m = enum_re.match(line)
        if m:
            table[m.group(1)] = canonical_code(int(m.group(2), 16))
    return table


# ── corpus identity ──────────────────────────────────────────────────────────
#
# Everything below runs BEFORE the first compile, so a corpus this instrument
# cannot name costs seconds rather than an hour of TU compiles.

def classify_manifest_keys(manifest: dict, manifest_path: Path) -> None:
    """Refuse to census a manifest carrying a key nobody classified.

    The four buckets above have to stay EXHAUSTIVE, and `stackReserve` is the
    proof that a silent gap here is expensive: the key was added to the manifest
    schema, the census went on copying it into every isolation manifest, and 189
    correct compiler refusals were reported as corpus errors. An unclassified
    key stops the run instead of quietly riding along, because the ONE thing
    this instrument may never do is publish a count it cannot defend."""
    known = (set(PER_TU_KEYS) | set(ISOLATION_REPLACED_KEYS)
             | set(IMAGE_SCOPE_KEYS) | set(FORMAT_CAPABILITY_KEYS))
    unknown = sorted(k for k in manifest if k not in known)
    if unknown:
        die(f"{manifest_path}: manifest key(s) {', '.join(unknown)} are not "
            f"classified by this census.\n"
            f"  Deriving a single-TU manifest has to decide, for EVERY key, "
            f"whether it survives isolation:\n"
            f"    carried verbatim   : {', '.join(PER_TU_KEYS)}\n"
            f"    replaced           : {', '.join(ISOLATION_REPLACED_KEYS)}\n"
            f"    dropped (image)    : {', '.join(IMAGE_SCOPE_KEYS)}\n"
            f"    dropped if the format lacks the capability: "
            f"{', '.join(f'{k} -> {v}' for k, v in FORMAT_CAPABILITY_KEYS.items())}\n"
            f"  Add the new key to the right tuple in this file. Guessing is "
            f"how `stackReserve` produced 189 phantom errors: it was carried "
            f"onto a format that cannot honour it, and the refusal was counted "
            f"as a corpus defect.")


def _candidate_corpus_dirs(manifest: dict) -> list[Path]:
    """Where the corpus version header might live, in resolution order.

    The manifest's own `includes` come FIRST and in manifest order — that is the
    order the COMPILE resolves them, so the census reads the same header the
    build saw. The directories of the manifest's own sources follow, which is
    what makes this work for a manifest whose include list happens not to name
    the generated tree. Nothing here is a hardcoded corpus path."""
    seen: set = set()
    out: list[Path] = []
    for raw in list(manifest.get("includes") or []) + \
               [os.path.dirname(s) for s in manifest["sources"]]:
        if not raw:
            continue
        p = Path(raw)
        try:
            key = p.resolve()
        except OSError:
            key = p
        if key in seen:
            continue
        seen.add(key)
        out.append(p)
    return out


def find_corpus_version(manifest: dict) -> tuple[str, str, list[str]]:
    """(SQLITE_VERSION, SQLITE_SOURCE_ID, headers read) — or die() trying.

    EVERY candidate is read, not just the first hit. Two copies of the header
    disagreeing about SOURCE_ID means the staged tree is a MIX of two check-ins,
    and silently picking one is how an uncomparable report gets written with a
    confident-looking identity block on the front of it."""
    stamped: dict[str, tuple[str, str]] = {}   # header path -> (version, id)
    unusable: list[str] = []
    for d in _candidate_corpus_dirs(manifest):
        hdr = d / CORPUS_VERSION_HEADER
        if not hdr.is_file():
            continue
        macros: dict[str, str] = {}
        for line in hdr.read_text(encoding="utf-8",
                                  errors="replace").splitlines():
            m = CORPUS_VERSION_MACRO_RE.match(line)
            if m:
                macros.setdefault(m.group(1), m.group(2))
                if len(macros) == len(CORPUS_VERSION_MACROS):
                    break
        version = macros.get("SQLITE_VERSION", "").strip()
        source_id = macros.get("SQLITE_SOURCE_ID", "").strip()
        if not version or not source_id:
            # A file of this NAME that carries no stamp is most likely a decoy
            # (a system or vendored header on the include path), not the corpus
            # header. It is skipped — but RECORDED and printed, because "the
            # census quietly ignored a file it looked at" is the habit this
            # whole instrument exists to break.
            unusable.append(str(hdr))
            continue
        stamped[str(hdr)] = (version, source_id)

    if not stamped:
        searched = "\n".join(f"      {d}" for d in _candidate_corpus_dirs(manifest))
        skipped = ("\n    found but carrying no stamp:\n"
                   + "\n".join(f"      {u}" for u in unusable)) if unusable else ""
        die(f"the CORPUS could not be identified: no '{CORPUS_VERSION_HEADER}' "
            f"declaring both {' and '.join(CORPUS_VERSION_MACROS)} was found.\n"
            f"    searched (manifest 'includes' first, then source dirs):\n"
            f"{searched}{skipped}\n"
            f"  This census REFUSES to run rather than emit a report that names "
            f"the compiler precisely and the corpus not at all. Such a report "
            f"cannot be compared with any other report — which is exactly how "
            f"two pe64 error counts were argued over for two cycles without "
            f"either side being able to show they had measured the same tree.")

    distinct = sorted(set(stamped.values()))
    if len(distinct) > 1:
        rows = "\n".join(f"      {v} / {sid}\n        {p}"
                         for p, (v, sid) in sorted(stamped.items()))
        die(f"the CORPUS is AMBIGUOUS: {len(distinct)} different "
            f"{CORPUS_VERSION_HEADER} stamps are reachable from this "
            f"manifest —\n{rows}\n"
            f"  The staged tree is a MIX of check-ins. Picking one would put a "
            f"confident identity on a report that measured neither.")

    version, source_id = distinct[0]
    return version, source_id, sorted(stamped)


def corpus_content_digest(sources: list[str]) -> tuple[str, str, int, int]:
    """(root label, sha256, file count, total bytes) over the manifest's sources.

    Machine-independent BY CONSTRUCTION: each file contributes its path RELATIVE
    to the corpus root (posix separators) plus its own SHA-256, and the files
    are folded in sorted relative-path order. Absolute paths or the manifest's
    own ordering would make a Windows and a Linux staging of the IDENTICAL tree
    digest differently — and cross-machine comparison is the entire reason this
    field exists.

    The upstream stamp cannot see a local edit; this can. That is why both are
    recorded."""
    dirs = [os.path.dirname(s) for s in sources]
    try:
        root: Path | None = Path(os.path.commonpath(dirs))
        root_label = str(root)
    except ValueError:
        # Sources spanning two roots (different drives on Windows) have no
        # common prefix. Say so in the report instead of inventing one — the
        # digest is then machine-specific and the reader has to know that.
        root, root_label = None, ("** NOT A SINGLE TREE — the manifest's "
                                  "sources span more than one root, so the "
                                  "digest folds ABSOLUTE paths and is "
                                  "MACHINE-SPECIFIC")

    entries: list[tuple[str, Path]] = []
    for s in sources:
        p = Path(s)
        if root is None:
            entries.append((p.as_posix(), p))
            continue
        try:
            entries.append((p.relative_to(root).as_posix(), p))
        except ValueError:
            die(f"corpus source {p} is not under the derived corpus root "
                f"{root}. The digest would not be reproducible on another "
                f"machine, so it must not be written.")
    entries.sort(key=lambda e: e[0])

    digest = hashlib.sha256()
    total = 0
    for rel, path in entries:
        try:
            data = path.read_bytes()
        except OSError as exc:
            die(f"corpus source could not be read: {path} ({exc}). A census "
                f"cannot identify a corpus it cannot read.")
        total += len(data)
        digest.update(rel.encode("utf-8"))
        digest.update(b"\0")
        digest.update(hashlib.sha256(data).digest())
    return root_label, digest.hexdigest(), len(entries), total


# ── the isolation target, DERIVED FROM THE SHIPPED CONFIG ────────────────────

def read_format_doc(path: Path) -> dict | None:
    """A parsed `.format.json`, or None if the file does not exist.

    A file that EXISTS but cannot be read or parsed is FATAL, never skipped. The
    first cut wrapped `read_text()` + `loads()` in one `except (OSError,
    ValueError): continue` — and `UnicodeDecodeError` IS a `ValueError`, so on a
    host whose locale is not UTF-8 a single non-Latin-1 byte in a shipped
    `.format.json` (17 of them carry UTF-8 today) would have dropped that format
    out of the search and changed the isolation target ON ONE PLATFORM ONLY,
    without a word. Pinning `encoding="utf-8"` closes the decode half; refusing
    to continue past an unreadable file closes the "said nothing" half."""
    if not path.is_file():
        return None
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        die(f"{path} exists but could not be read as JSON: {exc}. The isolation "
            f"target and its declared capabilities come from these files, so "
            f"stepping past an unreadable one would measure with the wrong "
            f"format and never say so.")


def derive_isolation_target(
        spec: str,
        formats_dir: Path) -> tuple[str, str | None, str, frozenset[str] | None]:
    """(target, profile, note, declared) for compiling ONE TU alone.

    Structural, not tabular: the format whose name shares this format's base
    (everything up to the last '-') and whose `artifactProfiles` contains
    `staticlib`. No branch on a format's identity.

    `declared` is the set of TOP-LEVEL keys that format's `.format.json`
    declares — the census's window onto the same capability declarations the
    linker gate consults (see FORMAT_CAPABILITY_KEYS). None only when no format
    document could be located at all."""
    arch, _, fmt = spec.partition(":")
    base = fmt.rsplit("-", 1)[0] if "-" in fmt else fmt
    for path in sorted(formats_dir.glob("*.format.json")):
        name = path.name[: -len(".format.json")]
        if not name.startswith(base + "-"):
            continue
        doc = read_format_doc(path)
        if doc is None:
            continue
        if ISOLATION_PROFILE in (doc.get("artifactProfiles") or []):
            return (f"{arch}:{name}" if arch else name, ISOLATION_PROFILE,
                    f"derived staticlib sibling of '{fmt}'", frozenset(doc))
    # Fail LOUD in the report rather than silently measuring with a format that
    # will manufacture isolation artifacts and then call them defects. The
    # manifest's OWN format is still read, so the format-conditional keys can be
    # decided against real declarations even on this degraded path.
    own = read_format_doc(formats_dir / f"{fmt}.format.json")
    return (spec, None,
            f"** NO staticlib-serving sibling found for '{fmt}' — this leg runs "
            f"the manifest's own format and WILL manufacture isolation "
            f"artifacts (a lone TU has no entry point)",
            None if own is None else frozenset(own))


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
    """-> (Counter(code), {code: severity}, {code: Counter(msg)}, set(unresolved),
           {elided_code: marker_text})

    ★ THE FIFTH RETURN IS THE FLOOR ORACLE (D-DIAG-MAXPERCODE-SILENT-COALESCE).
    Every `P_DiagnosticsElided` marker in this log names, in band, a code whose
    surviving count is a FLOOR rather than a total. That replaces the old
    `n == PER_CODE_CAP` round-number sniff, which could not tell 50-of-51 from a
    genuine 50 and went stale the moment the C++ default moved."""
    counts: collections.Counter = collections.Counter()
    sev: dict[str, str] = {}
    msgs: dict[str, collections.Counter] = collections.defaultdict(collections.Counter)
    unresolved: set[str] = set()
    elided: dict[str, str] = {}
    if not path.is_file():
        return counts, sev, msgs, unresolved, elided
    # `encoding` pinned (not the host locale): a diagnostic carrying a non-ASCII
    # byte must decode the SAME WAY on every leg, or the two platforms disagree
    # about a message they both received.
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
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
        # The elision marker is NOT a census row about the corpus -- it is a
        # statement ABOUT the other rows. Recorded as the floor oracle and not
        # counted as a diagnostic, or the instrument would report the compiler's
        # own bookkeeping as a corpus finding.
        if code in (ELISION_MARKER_CODE, "P_DiagnosticsElided"):
            m2 = ELIDED_CODE_RE.match(msg.strip())
            if m2:
                elided[m2.group(1)] = msg.strip()[:200]
            continue
        counts[code] += 1
        sev[code] = severity
        # `got X` is the generic renderer's prefix; keep the payload readable.
        msgs[code][msg.strip()[:140]] += 1
    return counts, sev, msgs, unresolved, elided


# ── one target leg ───────────────────────────────────────────────────────────

class Leg:
    def __init__(self, spec: str, manifest: dict, manifest_path: Path,
                 scratch: Path, formats_dir: Path):
        self.spec = spec
        self.tu_target, self.tu_profile, self.tu_note, self.tu_format_keys = \
            derive_isolation_target(spec, formats_dir)
        # Manifest keys the isolation manifest DROPS, with the reason each was
        # dropped — reported per leg so a reader can see exactly how the
        # measured manifest differs from the committed one.
        self.tu_strips: list[tuple[str, str]] = []
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
        self.whole_sev: dict[str, str] = {}
        self.whole_msgs: dict[str, collections.Counter] = \
            collections.defaultdict(collections.Counter)
        self.unresolved: set[str] = set()
        # (src, code, shown_count, marker_text) -- the marker text travels so
        # the report can quote the compiler rather than paraphrase it.
        self.saturated: list[tuple[str, str, int, str]] = []
        self.clean: list[str] = []
        self.silent_fail: list[tuple[str, int]] = []
        self.noisy_ok: list[str] = []
        self._attempted: int | None = None

    def isolation_strips(self) -> list[tuple[str, str]]:
        """[(manifest key, why)] — the keys this leg's isolation manifest drops.

        Computed once per leg, from the manifest's OWN keys and the isolation
        format's OWN declarations. Nothing here knows a format by name."""
        strips: list[tuple[str, str]] = []
        for key in IMAGE_SCOPE_KEYS:
            if key in self.manifest:
                strips.append((key, "whole-IMAGE scope — a lone TU is not an "
                                    "image, so this describes nothing the "
                                    "per-TU compile emits"))
        fmt_name = self.tu_target.partition(":")[2] or self.tu_target
        for key, capability in sorted(FORMAT_CAPABILITY_KEYS.items()):
            if key not in self.manifest:
                continue
            if self.tu_format_keys is None:
                die(f"leg {self.spec}: the manifest declares '{key}', which "
                    f"only a format declaring '{capability}' can honour — but "
                    f"no '{fmt_name}.format.json' could be found to ask. "
                    f"Refusing to guess: carrying it would manufacture a "
                    f"refusal and counting that as a corpus defect is the bug "
                    f"this check exists to prevent, while dropping it blind "
                    f"could hide a real one.")
            if capability in self.tu_format_keys:
                # The isolation format CAN carry it — leave the request in
                # place, so a genuine range/alignment refusal is still measured.
                continue
            strips.append((key,
                           f"object format '{fmt_name}' declares no "
                           f"'{capability}', so DSS would REFUSE the request "
                           f"(correctly) — a refusal of the INSTRUMENT's "
                           f"request, not a corpus defect"))
        return strips

    def derive(self) -> None:
        self.tu_strips = self.isolation_strips()
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
            for key, _why in self.tu_strips:
                derived.pop(key, None)
            derived["artifactName"] = "census_%04d" % i
            (self.dir / "manifests" / f"{slot}.json").write_text(
                json.dumps(derived, indent=2), encoding="utf-8")
            self.slots.append((slot, src))
        # The WHOLE-PROJECT leg keeps the manifest EXACTLY as committed —
        # including every key stripped above. That is what makes the strips
        # safe: this leg is where a genuine refusal on the real format lands.
        whole = dict(self.manifest)
        whole["targets"] = [self.spec]
        (self.dir / "whole.json").write_text(json.dumps(whole, indent=2),
                                             encoding="utf-8")

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
            counts, sev, msgs, unk, elided = scan_log(
                self.dir / "logs" / f"{slot}.log", name_to_code)
            self.unresolved |= unk
            # EXACT, and stated by the compiler rather than inferred from the
            # shape of a number: every code the TU emitted an elision marker for
            # has a count that is a floor. A code can now be reported as a floor
            # at ANY value, including one, which the round-number sniff could
            # never do.
            for code, marker in elided.items():
                self.saturated.append((src, code, counts.get(code, 0), marker))
            for code, n in counts.items():
                self.per_code[code] += n
                self.per_code_sev[code] = sev[code]
                self.per_code_tus[code].add(src)
                self.per_code_msgs[code].update(msgs[code])
            rc = self.rc.get(slot)
            if not counts:
                self.clean.append(src)
                if rc not in (None, 0):
                    self.silent_fail.append((src, rc))
            elif rc == 0:
                self.noisy_ok.append(src)
        # The whole-project leg's SEVERITIES and MESSAGES are kept, not
        # discarded. They used to be dropped on the floor, which left every
        # whole-only code rendered as a bare `K001A  1  0` — a number with no
        # text, no severity and nothing to act on. That is precisely the row
        # that matters now: an image request the REAL format refuses (an
        # out-of-range `stackReserve`, say) can ONLY appear here, because the
        # isolation manifest drops what its format cannot carry. Saying "the
        # whole-project leg still measures it" is worth nothing if the report
        # then declines to say WHAT it measured.
        wc, wsev, wmsgs, wunk, welided = scan_log(self.dir / "whole.log",
                                                  name_to_code)
        self.whole_counts = wc
        self.whole_sev = wsev
        self.whole_msgs = wmsgs
        self.unresolved |= wunk
        # The whole-project leg's floors count too. Its elisions used to be
        # invisible for a second reason on top of the silence: this leg's counts
        # never went through the `n == PER_CODE_CAP` sniff at all, so a
        # saturated whole-project code was reported as a total with nothing
        # anywhere saying otherwise.
        for code, marker in welided.items():
            self.saturated.append(("<whole-project>", code,
                                   wc.get(code, 0), marker))

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
    # TWO halves, and the file is only citable with BOTH: which COMPILER ran,
    # and which CORPUS it ran over. A report carrying only the first names the
    # measuring instrument and not the thing measured.
    for label, key in (("utc", "utc"), ("git HEAD", "head"),
                       ("git branch", "branch"), ("tree state", "dirty"),
                       ("config", "config"), ("dss binary", "dss_bin"),
                       ("dss binary size", "bin_size"),
                       ("dss binary mtime", "bin_mtime"),
                       ("manifest", "manifest"),
                       ("corpus root", "corpus_root"),
                       ("corpus SQLITE_VERSION", "corpus_version"),
                       ("corpus SQLITE_SOURCE_ID", "corpus_source_id"),
                       ("corpus version header", "corpus_version_header"),
                       ("corpus sources", "corpus_files"),
                       ("corpus bytes", "corpus_bytes"),
                       ("corpus sha256", "corpus_digest"),
                       ("concurrency", "jobs"),
                       ("instrument", "instrument")):
        w(f"  {label:<24}: {meta[key]}")
    w(f"  {'code-name table':<24}: "
      + (f"{len(name_to_code)} enum rows read from {diag_header}"
         if len(name_to_code) > 100
         else "** UNAVAILABLE — symbolic-name diagnostics stay unnormalized"))
    if meta.get("self_test"):
        w(f"  {'MODE':<24}: ** SELF-TEST — one TU deliberately skipped per leg;")
        w(f"  {'':<24}   this run MUST report INCOMPLETE and exit 1.")
    w("")
    # Scope stated, not assumed. A digest whose coverage the reader has to guess
    # invites the same over-reading the corpus rows exist to end.
    w("  NOTE `corpus sha256` folds each source's corpus-RELATIVE path and its")
    w("    own SHA-256, in sorted order, so the SAME tree staged on Windows and")
    w("    on Linux digests IDENTICALLY. Equal digest = same corpus bytes. It")
    w("    covers the manifest's SOURCE SET ONLY — a change confined to a header")
    w("    on the include path does not move it, and SQLITE_SOURCE_ID (baked at")
    w("    amalgamation time) does not move for a local edit either. Quote BOTH.")
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
        # Every difference between the committed manifest and the one actually
        # measured is stated here. An undeclared difference is how a census
        # starts answering a question nobody asked it.
        if leg.tu_strips:
            w("  per-TU manifest strips  : keys the ISOLATION target cannot carry.")
            w("     The WHOLE-PROJECT LEG below compiles the manifest UNMODIFIED,")
            w("     so a genuine refusal on the REAL format is still measured.")
            for key, why in leg.tu_strips:
                w(f"        {key:<18} {why}")
        else:
            w("  per-TU manifest strips  : none")
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
            w("  -- ** ELIDED COUNTS — THESE ARE FLOORS, NOT TOTALS --")
            w("     Reported by the COMPILER, in band, not inferred from the")
            w("     shape of a number: each row below is a `P_DiagnosticsElided`")
            w("     marker naming a code whose surviving count is short by the")
            w("     amount the marker states. Raise it with --max-per-code=N.")
            for src, code, n, marker in leg.saturated:
                w(f"        {code:<8} shown {n:<5} {src}")
                w(f"                 {marker}")
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
        w(f"     {'code':<10} {'severity':<9} {'whole':>6}  {'census':>6}   "
          f"hidden by the gate")
        whole_only: list[str] = []
        for code in sorted(set(leg.whole_counts) | set(leg.per_code)):
            wv, cv = leg.whole_counts.get(code, 0), leg.per_code.get(code, 0)
            sev = leg.whole_sev.get(code) or leg.per_code_sev.get(code, "?")
            w(f"     {code:<10} {sev:<9} {wv:>6}  {cv:>6}   "
              + ("yes" if wv == 0 and cv > 0 else ""))
            if wv > 0 and cv == 0:
                whole_only.append(code)
        # Spell out anything the per-TU census could not see. A whole-only code
        # is either a genuinely IMAGE-level finding (link resolution across the
        # real TU set, or a request the real format refuses) or a driver-level
        # one — in both cases the reader needs the text, and this is the only
        # place in the report it can appear.
        if whole_only:
            w("")
            w("     -- WHOLE-PROJECT ONLY (absent from the per-TU census: these")
            w("        are IMAGE-level findings; a lone TU cannot produce them) --")
            for code in whole_only:
                w(f"        [{code}] {leg.whole_sev.get(code, '?')} "
                  f"x{leg.whole_counts[code]}")
                for msg, c in leg.whole_msgs.get(code,
                                                 collections.Counter()).most_common(10):
                    w(f"            x{c:<5} {msg}")
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
            # The executable SUFFIX is part of the name on Windows, and leaving
            # it out made auto-discovery unable to succeed on that platform at
            # all: the built binary is `dsscp.exe`, so neither the
            # fixed default path nor the glob could ever match and `--dss-bin`
            # was mandatory on the one platform the `.ps1` launcher exists for.
            # Only the NAME is widened here — the directories searched are
            # unchanged on purpose, because quietly picking up whichever
            # compiler some other build tree happens to hold is a run-identity
            # problem, not a convenience.
            names = ["dsscp"]
            if EXE_SUFFIX:
                names.insert(0, "dsscp" + EXE_SUFFIX)
            for name in names:
                default = REPO_ROOT / "build" / "bin" / "dss" / name
                if default.is_file():
                    dss_bin = default
                    break
                found = sorted(REPO_ROOT.glob(f"build/**/{name}"))
                if found:
                    dss_bin = found[0]
                    break
    if dss_bin is None or not dss_bin.is_file() or not os.access(dss_bin, os.X_OK):
        die("dsscp not found; build the project or pass --dss-bin.")

    # ── the corpus manifest ──
    manifest_path = Path(args.manifest or os.environ.get("SQLITE_MANIFEST")
                         or (REPO_ROOT / "build" / "real-examples" / "c"
                             / "sqlite" / "host" / "host.dss-project.json"))
    if not manifest_path.is_file():
        die(f"no corpus manifest at {manifest_path} — run "
            f"real-examples/c/sqlite/build-and-test.sh first, or pass --manifest.")
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except ValueError as exc:
        die(f"manifest is not valid JSON: {exc}")
    if not isinstance(manifest.get("sources"), list) or not manifest["sources"]:
        die("manifest has no 'sources' array")

    # ── everything the census must know BEFORE it spends an hour compiling ──
    # Both checks are cheap and both are fatal, so a run that could not have
    # produced a citable report never starts. Discovering at the end that the
    # corpus cannot be named would cost the whole run and still teach nothing.
    classify_manifest_keys(manifest, manifest_path)
    corpus_version, corpus_source_id, version_headers = \
        find_corpus_version(manifest)
    corpus_root, corpus_digest, corpus_files, corpus_bytes = \
        corpus_content_digest(manifest["sources"])

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
        # WHICH CORPUS — the half the identity block used to omit entirely.
        "corpus_root": corpus_root,
        "corpus_version": corpus_version,
        "corpus_source_id": corpus_source_id,
        "corpus_version_header": (version_headers[0] if len(version_headers) == 1
                                  else f"{len(version_headers)} agreeing copies: "
                                       + " | ".join(version_headers)),
        "corpus_files": f"{corpus_files} sources (the manifest's own set)",
        "corpus_bytes": f"{corpus_bytes} bytes",
        "corpus_digest": corpus_digest,
        "jobs": jobs,
        "instrument": str(Path(__file__).resolve()),
        "self_test": args.self_test,
    }

    diag_header = REPO_ROOT / "src" / "core" / "types" / "parse_diagnostic.hpp"
    name_to_code = read_code_name_table(diag_header)

    info(f"HEAD={meta['head']} ({meta['branch']}, {meta['dirty']})")
    info(f"corpus {corpus_version} / {corpus_source_id}")
    info(f"corpus sha256 {corpus_digest} "
         f"({corpus_files} sources, {corpus_bytes} bytes, root {corpus_root})")
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
        # UTF-8, not the host locale: two legs of the same census must produce
        # BYTE-COMPARABLE reports, and a report written in cp1252 on Windows and
        # UTF-8 on Linux is neither comparable nor (for any character outside
        # cp1252) writable at all.
        report.write_text("\n".join(lines) + "\n", encoding="utf-8")
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
