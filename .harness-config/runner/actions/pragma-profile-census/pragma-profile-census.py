#!/usr/bin/env python3
# TF-C85 — THE PROFILE-CENSUS GUARD, TIER (b): the CORPUS census.
# TF-C86 — relocated from `pragma-profile-census.sh` into this one Python
#          implementation (the `.sh`/`.ps1` launchers retired 2026-09-21).
#
# ═══ WHY THIS IS A RUNNER STEP AND NOT A CTEST ENTRY — STATED PLAINLY ════════
#
# Tier (a) — `Preprocessor.TfC85NoUnclaimedPragmaUnderAnyPredefineClass` plus
# its non-vacuity twin — is ALWAYS ON in ctest, over the in-repo fixture
# `tests/corpus/c/pragma_profile_census.c`. It is the guard that cannot rot.
#
# Tier (b) is THIS: the same census over the REAL 189-TU sqlite corpus, which
# lives OUTSIDE the tracked tree -- staged by the sqlite harness into the leg's
# own output tree -- so it runs as a step of this action, on the leg whose
# corpus it reads, never as a ctest entry that would have to read it.
#
# ═══ WHAT IT MEASURES ════════════════════════════════════════════════════════
#
# The FULL reached pragma vocabulary of ONE predefine class, by DISARMING the
# registry: it copies `src/` to a scratch dir, empties `preprocess.pragmaEffects`
# there, and points DSS at the copy via `DSS_CONFIG_ROOT`. With no row claiming
# anything, EVERY reached pragma emits a `P0020` naming itself — which is
# exactly a census. The repo's own config is never touched.
#
# ★★ ONE CLASS PER CORPUS, THE CORPUS'S OWN (2026-10-01, P69 lane hm; the P69
# review's MAJOR 2). Until then one manifest was compiled under all three classes
# (pe, macho, elf, each by overriding the manifest's target). ✔MEASURED that day,
# lane hm's runs 20261001-150248-2c3fb5bd (windows-x86_64-debug, its own pe64
# recompile manifest), 20261001-150919-da3987ea (linux-x86_64-debug, its own elf64
# one) and 20261001-155346-6b59e0da: EVERY class reached NO pragma on either
# host, the Windows run's `--update` wrote an expected set with no data line and
# the linux check called it a MATCH. Two causes, both measured:
#   * a manifest is ONE platform's configuration -- its staged config headers,
#     its TU preludes -- and another class cannot compile it at all: over the
#     Windows corpus the macho and elf classes stopped at the first TU line,
#     `quote include not found: windows.h`, in every TU;
#   * the `pe` class no longer defines `_MSC_VER`: it presents the GNU-on-Windows
#     identity mingw-w64 has (c.lang.json's `mutuallyExclusivePredefinedMacros`,
#     D-LANG-PE64-DEFINES-BOTH-MSC-VER-AND-GNUC), so the MSVC-only vocabulary the
#     2026-07-29 set recorded for it (`warning`, `intrinsic`, `optimize`, all
#     under `#if defined(_MSC_VER)`) is no longer reached by any leg.
# So the class is the manifest's OWN: its target's object-format KIND (`kind`
# in `<format>.format.json`'s `format`), and a leg censuses the class whose
# corpus it stages (Windows pe, Linux elf, the Mac macho). That is also what the
# armed build meets: a real leg compiles its own platform's corpus, never
# another's.
#
# ★ A RUN THAT DID NOT MEASURE THE WHOLE CORPUS SAYS SO (2026-10-06, the P69
# re-review's MAJOR 1). Every TU is compiled through a wrapper of three lines: an
# OPENING sentinel pragma, `#pragma mark dss_pragma_census_open_<i>`, the TU's
# `#include`, and a CLOSING one, `#pragma mark dss_pragma_census_close_<i>` (<i>
# the TU's index in the manifest). The real config claims `mark` (annotationOnly),
# so a sentinel speaks only under the DISARMED config. The census measured the
# corpus only when ALL THREE hold, and otherwise it FAILS (exit 2), naming the
# TUs and the codes, and changes nothing:
#   * BOTH sentinels spoke in EVERY TU. The closing one is a TU's LAST pragma, so
#     it speaks only when the preprocessor reached the end of the TU's text. Until
#     then one leading sentinel proved only that a TU STARTED: a TU stopped right
#     after it passed (the re-review's finding);
#   * the log holds NO ERROR that is not an unclaimed pragma. The preprocessor
#     reports a missing include or a refused directive and goes on, so such a TU
#     still reaches its closing sentinel with part of its text unread -- the
#     2026-10-01 class mix-up exactly (`windows.h` not found in every TU);
#   * the reporter ELIDED none of the pragma diagnostics. Today none can be: the
#     code is unsuppressable, and the reporter's delivery gate lets such a code
#     past every volume gate (DOCUMENTED, `mustDeliver` in diagnostic_reporter.cpp;
#     ✔MEASURED 2026-10-06 without the two flags below: 62 in one TU, past the
#     per-code cap of 50, run 20261006-234908-75a980c0, and 1104 over two TUs,
#     past the run-wide cap of 1000, run 20261006-235410-2dd7be57, all delivered,
#     no notice). The refusal holds the census to that should the code change.
# The compile also lifts the reporter's volume caps (`--max-diagnostics`,
# `--max-per-code`, at UNCAPPED below), because a delivered pragma still COUNTS
# toward the run-wide cap: past it, the next diagnostic of a suppressable code is
# dropped and the cap's own `P_TooManyDiagnostics`, an ERROR, takes its place, so
# a large enough corpus would be refused for its size alone (✔MEASURED
# 2026-10-06: 1100 pragmas and then a `#warning`, refused that way under the
# default caps, run 20261007-000625-898787ca, and measured with the caps lifted,
# run 20261006-235501-3e1952ee). Lifting the per-code cap keeps the other codes'
# counts in the printed tally whole.
# A class that reaches no pragma in a run that holds all three is a
# measurement, not an empty run: `elf` reached none on 2026-07-29.
#
# ═══ ★★ THE EXPECTED SET IS A FLOOR, NOT A TOTAL — A DIFF IS NOT A BUG ══════
#
# A reached-set is a function of (i) the manifest's defines, (ii) the predefine
# class, and (iii) HOW FAR EACH TU GETS before a hard error stops it. All three
# move. As other cycles clear blockers, TUs get further and the set GROWS. The
# point of the diff is that new pragma vocabulary arrives as something a human
# reads and decides about, instead of as a silent pass or a surprise build break
# three cycles later.
#
# ═══ USAGE — through DssHarness, on the leg whose corpus it reads ═══════════
#
#   dssharness run pragma-profile-census --legs <leg> --input manifest=<that leg's corpus manifest>
#       the CHECK: this class's census against the tracked expected set
#   dssharness run pragma-profile-census-update --legs <leg> --input manifest=<...>
#       REGENERATES the expected set with this class's lines measured anew, into
#       the step's kept output -- never the tracked file. A remote leg's is
#       brought back with `dssharness sync --legs <leg> --pull <its kept path>`
#       (the run's --json names it, `keptOutputs`); review it, and it replaces
#       the tracked file in the change that reviews it.
#   The manifest is one a sqlite run on that leg wrote (the recompile's, e.g.
#   build/real-examples/c/sqlite/windows/recompile/pe64-x86_64/pe64-x86_64.dss-project.json
#   on Windows), relative to the tree.
#   The program's own arms, host-independent, run as ctest `harness/pragma_profile_census_selftest`.
#
# Exit: 0 = the census matches (or, with --update, the regenerated set is
#       written); 1 = it differs (review, then regenerate); 2 = the census could
#       not run, or did not measure the whole corpus (no corpus, no compiler, a
#       sentinel silent, an error other than an unclaimed pragma, an elision).
#
# NOTE FOR MAINTAINERS: every exit code below is captured DIRECTLY from the
# command that produced it, never through a pipe whose tail swallows the status.
# Every text read and write names UTF-8 (TF-C86: the locale's cp1252 could not
# decode c.lang.json), and every write LF.

from __future__ import annotations

import argparse
import collections
import difflib
import importlib.util
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

sys.dont_write_bytecode = True  # it loads the one redactor by path: never a __pycache__ beside another action

# ── OUTPUT ENCODING, AT IMPORT, BEFORE ANYTHING BELOW CAN PRINT ─────────────────
# ✔MEASURED 2026-08-23 (CPython 3.14.3, Windows, both streams pipes -- how the
# action runner starts this file): stdout comes up cp1252, so a line holding a
# character outside it raises `UnicodeEncodeError`.
for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError, OSError):   # pragma: no cover - an odd stream
        pass

EXIT_MATCH, EXIT_DIFFERS, EXIT_CANNOT_RUN = 0, 1, 2


def _repo_root() -> Path:
    """The tree this file lives in, by a MARKER (TF-C87: no hop counting -- a guess silently censuses the wrong
    tree when this family moves)."""
    here = Path(__file__).resolve()
    for cand in here.parents:
        if (cand / "CMakeLists.txt").is_file() and (cand / "src").is_dir():
            return cand
    print(f"pragma-profile-census: FATAL: no repo root at or above {here} "
          f"(looked for a directory holding both CMakeLists.txt and src/)", file=sys.stderr)
    sys.exit(EXIT_CANNOT_RUN)


REPO_ROOT = _repo_root()
# The golden file is this script's SIBLING, resolved from the script.
EXPECTED_FILE = Path(__file__).resolve().parent / "pragma-profile-census.expected"
EXPECTED_NAME = EXPECTED_FILE.name
FORMATS_DIR = REPO_ROOT / "src" / "dss-config" / "object-formats"

UNRECOGNIZED_RE = re.compile(r"unrecognized pragma '([^']*)'")
# A diagnostic's HEADER line, `<severity>[<code>]: <message>` -- the four severities DSS renders (`severityName`).
DIAGNOSTIC_RE = re.compile(r"^\s*(error|warning|info|hint)\[([A-Za-z_0-9]+)\]:")
# The one code a census's data arrives under: an unclaimed pragma (`unrecognized pragma '<its words>'`).
PRAGMA_CODE = "P_PreprocessorPragma"
# The reporter's notice that it withheld diagnostics of a code: `<prefix> (<code name>) diagnostics were ELIDED ...`.
ELIDED_RE = re.compile(r"\(([A-Za-z_0-9]+)\) diagnostics were ELIDED")
# The two sentinels each wrapper puts around its TU: a pragma the REAL config claims (`mark`, annotationOnly), so they
# speak only where the disarmed config is the one in force, each naming the TU's index. ONE identifier after `mark`:
# DSS names an unclaimed pragma by its pp-tokens joined with blanks, so a hyphenated word comes back as `a - b`
# (✔MEASURED 2026-10-01, run 20261001-234135-e61e17ad: `mark dss - pragma - census - sentinel`, which matched
# nothing).
SENTINEL_PREFIX = "mark"
SENTINEL_RE = re.compile(r"mark dss_pragma_census_(open|close)_([0-9]+)")
# The reporter's volume caps, lifted for the census compile: every pragma diagnostic counts toward the run-wide cap
# (none is ever dropped by it), and a suppressable diagnostic past that cap becomes a `P_TooManyDiagnostics` error,
# which would refuse the census for the corpus's size alone (see the header).
UNCAPPED = 1_000_000_000
FIRST_ERRORS = 8
_REDACT: list = []


def sentinel(kind: str, index: int) -> str:
    """The words of TU `index`'s `open` or `close` sentinel, as DSS reports them."""
    return "%s dss_pragma_census_%s_%d" % (SENTINEL_PREFIX, kind, index)


def die(msg: str) -> "NoReturn":                       # noqa: F821
    print(f"pragma-census: FATAL: {msg}", file=sys.stderr)
    sys.exit(EXIT_CANNOT_RUN)


def info(msg: str) -> None:
    print(f"pragma-census: {msg}", file=sys.stderr)


def read_text(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def write_text(path: Path, text: str) -> None:
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(text)


def shown(text: str) -> str:
    """What may be printed of a compiler's log: THE redactor (`redact/redact.py`, loaded once by path) over the
    whole text, built on first use (its `lazy_redactor`) -- a log names the scratch tree under the system temp
    directory, marked `<temp>`, and the corpus under the tree."""
    if not _REDACT:
        path = Path(__file__).resolve().parent.parent / "redact" / "redact.py"
        if not path.is_file():
            die(f"cannot find {path} -- the redaction rule lives there and nowhere else")
        spec = importlib.util.spec_from_file_location("dss_redact", path)
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        _REDACT.append(mod.lazy_redactor(tree=str(REPO_ROOT), places={"<temp>": tempfile.gettempdir()}))
    return _REDACT[0](text)


def find_dss_bin(explicit: str | None) -> Path:
    """The compiler the step names (`{product}`, the leg's own build). Taken verbatim: guessing a build tree is a
    run-identity problem, not a convenience."""
    if not explicit:
        die("--dss-bin names no compiler: the step hands it the leg's own build (`{product}`)")
    candidate = Path(explicit)
    if not candidate.is_file() or not os.access(candidate, os.X_OK):
        die(f"dsscp not executable at {shown(str(candidate))}: a step that names `{{product}}` runs only after "
            f"DssHarness built the leg, so this path was not handed in by that step.")
    return candidate


def manifest_class(manifest: dict) -> str:
    """The predefine class of a corpus: its ONE target's object-format KIND (`format.kind` in the format file)."""
    targets = manifest.get("targets") or []
    if len(targets) != 1 or ":" not in str(targets[0]):
        die("the manifest names %d target(s); a census is of ONE platform's corpus, so it names exactly one "
            "<arch>:<format>" % len(targets))
    fmt = str(targets[0]).split(":", 1)[1]
    path = FORMATS_DIR / (fmt + ".format.json")
    try:
        kind = json.loads(read_text(path)).get("format", {}).get("kind")
    except (OSError, ValueError) as exc:
        die(f"the manifest's format {fmt!r} has no readable format file ({path.name}: {exc})")
    if not isinstance(kind, str) or not kind:
        die(f"{path.name} declares no `format.kind`: the class of the corpus cannot be named")
    return kind


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


def wrapper_text(index: int, tu: Path) -> str:
    """TU `index`'s wrapper: its OPENING sentinel, the TU by its absolute path (so its own quote includes resolve
    beside it as before), its CLOSING sentinel."""
    return "#pragma %s\n#include \"%s\"\n#pragma %s\n" % (sentinel("open", index), tu.resolve().as_posix(),
                                                          sentinel("close", index))


def wrapped_manifest(manifest: dict, scratch: Path) -> tuple[dict, list[str]]:
    """The manifest with every TU compiled through its wrapper (`wrapper_text`). -> (manifest, the TUs' names, in
    the manifest's order). A TU this host does not have refuses the run: the manifest is another host's corpus."""
    sources = manifest.get("sources") or []
    if not sources:
        die("the manifest names no sources")
    missing = [s for s in sources if not Path(s).is_file()]
    if missing:
        die(f"{len(missing)} of the manifest's {len(sources)} TU(s) are not on this host (first: {shown(missing[0])}): "
            f"a manifest is the corpus of the host that staged it -- name this leg's own")
    wrap_dir = scratch / "tus"
    wrap_dir.mkdir()
    wrapped = []
    for i, src in enumerate(sources):
        w = wrap_dir / ("%03d-%s" % (i, Path(src).name))
        write_text(w, wrapper_text(i, Path(src)))
        wrapped.append(w.as_posix())
    derived = dict(manifest)
    derived["sources"] = wrapped
    derived.pop("resolveLibraries", None)   # link-tier only; the census stops at PP
    return derived, [Path(s).name for s in sources]


class Log:
    """What one census compile's log says: `lines` ("<class> <word> <count>", each unclaimed pragma counted by its
    leading WORD, the sentinels apart), the TU indices whose `opened` / `closed` sentinel spoke, the `tally` of every
    diagnostic code, the `foreign` errors -- every error that is not an unclaimed pragma, the first FIRST_ERRORS of
    them kept with their location line, all of them counted in `foreign_count` -- and the codes the reporter
    `elided`, with its notices (`notices`, the first FIRST_ERRORS)."""

    def __init__(self, text: str, klass: str):
        counts: collections.Counter = collections.Counter()
        self.tally: collections.Counter = collections.Counter()
        self.opened: set[int] = set()
        self.closed: set[int] = set()
        self.foreign: list[str] = []
        self.foreign_count = 0
        self.elided: list[str] = []
        self.notices: list[str] = []
        lines = text.splitlines()
        for k, line in enumerate(lines):
            head = DIAGNOSTIC_RE.match(line)
            if not head:
                continue
            severity, code = head.group(1), head.group(2)
            self.tally["%s[%s]" % (severity, code)] += 1
            elided = ELIDED_RE.search(line)
            if elided:
                self.elided.append(elided.group(1))
                if len(self.notices) < FIRST_ERRORS:
                    self.notices.append(line.strip())
            pragma = UNRECOGNIZED_RE.search(line) if code == PRAGMA_CODE else None
            if pragma:
                words = pragma.group(1)
                mark = SENTINEL_RE.fullmatch(words)
                if mark:
                    (self.opened if mark.group(1) == "open" else self.closed).add(int(mark.group(2)))
                else:
                    counts[words.split(" ", 1)[0]] += 1   # the pragma's leading WORD, counted
                continue
            if severity == "error":
                self.foreign_count += 1
                if len(self.foreign) < FIRST_ERRORS:
                    where = next((ln.strip() for ln in lines[k + 1:k + 3] if ln.strip().startswith("-->")), "")
                    self.foreign.append(line.strip() + ("  " + where if where else ""))
        self.lines = sorted(f"{klass} {word} {n}" for word, n in counts.items())


def refusals(log: Log, names: list[str]) -> list[str]:
    """Why this log is NOT a census of the whole corpus -- empty when it is: in every TU both sentinels spoke, no
    error is anything but an unclaimed pragma, and the reporter elided none of the pragma diagnostics."""
    why = []
    every = set(range(len(names)))

    def named(indices: set[int]) -> str:
        shown_ = sorted(indices)[:5]
        return ", ".join("%03d-%s" % (i, names[i]) for i in shown_) + (", ..." if len(indices) > 5 else "")

    if not names:
        why.append("the manifest names no TU")
    if every - log.opened:
        why.append("the OPENING sentinel is silent in %d of %d TU(s) (%s): the compiler never preprocessed them under "
                   "the disarmed config" % (len(every - log.opened), len(names), named(every - log.opened)))
    if every - log.closed:
        why.append("the CLOSING sentinel is silent in %d of %d TU(s) (%s): the preprocessor never reached the end "
                   "of each" % (len(every - log.closed), len(names), named(every - log.closed)))
    if log.foreign_count:
        why.append("%d error(s) that are not an unclaimed pragma: part of a TU was not preprocessed as the armed "
                   "build preprocesses it, so its pragmas cannot all have been reached" % log.foreign_count)
    if PRAGMA_CODE in log.elided:
        why.append("the reporter ELIDED %s diagnostics, so every count here would be a floor" % PRAGMA_CODE)
    return why


def measured_whole(log: Log, names: list[str]) -> bool:
    """Whether the log is a census of the whole corpus: `refusals` names nothing -- both sentinels spoke in every
    TU, no error is anything but an unclaimed pragma, no pragma diagnostic was elided."""
    return not refusals(log, names)


def census_command(dss_bin: Path, project: Path, out: Path) -> list[str]:
    """The census compile: the project in `--project` mode, with the reporter's volume caps lifted (UNCAPPED)."""
    return [str(dss_bin), "--project", str(project), "--output", str(out),
            "--max-diagnostics=%d" % UNCAPPED, "--max-per-code=%d" % UNCAPPED]


def census(dss_bin: Path, manifest: dict, klass: str, scratch: Path, env: dict):
    """-> (the compile's Log, compiler rc, the TUs' names)."""
    derived, names = wrapped_manifest(manifest, scratch)
    project = scratch / "census.dss-project.json"
    write_text(project, json.dumps(derived, indent=2))
    log = scratch / "census.log"
    info(f"censusing predefine class '{klass}' over {len(derived['sources'])} TU(s) ...")
    with log.open("wb") as fh:
        proc = subprocess.run(census_command(dss_bin, project, scratch / "out"),
                              stdout=fh, stderr=subprocess.STDOUT, env=env, check=False)
    rc = proc.returncode                # captured DIRECTLY
    return Log(log.read_text(encoding="utf-8", errors="replace"), klass), rc, names


def class_lines(text: str, klass: str) -> list[str]:
    """The data lines of one class in an expected set (`#` lines are documentation)."""
    return [ln for ln in text.splitlines() if ln.strip() and not ln.startswith("#") and ln.split(" ", 1)[0] == klass]


def regenerated(text: str, klass: str, actual: list[str]) -> str:
    """The expected set with `klass`'s lines replaced by `actual`: the header and every other class kept."""
    head = [ln for ln in text.splitlines() if ln.startswith("#")]
    others = [ln for ln in text.splitlines() if ln.strip() and not ln.startswith("#")
              and ln.split(" ", 1)[0] != klass]
    return "\n".join(head + sorted(others + actual)) + "\n"


# ── the self-test (ctest `harness/pragma_profile_census_selftest`) ──────────────
# Every arm below is host-independent: no compiler runs. An EXACT ratchet: an arm lost -- or a block that stopped
# running -- is a red, never a smaller green.
EXPECTED_ARMS = 17


def self_test() -> int:
    ran, failed = [0], [0]

    def arm(label: str, ok: bool, detail: object = "") -> None:
        ran[0] += 1
        if not ok:
            failed[0] += 1
        print("pragma-census self-test: %-62s %s" % (label[:62], "ok" if ok else
                                                       "FAIL" + ((": " + shown(str(detail))) if detail else "")))

    def refused(fn, *a) -> bool:
        try:
            with open(os.devnull, "w") as quiet:
                saved, sys.stderr = sys.stderr, quiet
                try:
                    fn(*a)
                finally:
                    sys.stderr = saved
        except SystemExit as exc:
            return exc.code == EXIT_CANNOT_RUN
        return False

    # ── the class of a corpus is its own target's format KIND, read from the shipped format files ──
    for spec, want in (("x86_64:pe64-x86_64-windows-exec", "pe"), ("x86_64:elf64-x86_64-linux-exec", "elf"),
                       ("arm64:macho64-arm64-darwin-exec", "macho")):
        got = manifest_class({"targets": [spec]})
        arm("the class of a %s corpus is %r" % (spec.split(":")[1], want), got == want, got)
    arm("a manifest naming two targets is refused (a census is of ONE platform's corpus)",
        refused(manifest_class, {"targets": ["x86_64:pe64-x86_64-windows-exec", "x86_64:elf64-x86_64-linux-exec"]}))
    arm("a target whose format has no file is refused", refused(manifest_class, {"targets": ["x86_64:no-such-fmt"]}))
    # ── both sentinels are silent under the REAL config: some pragmaEffects row claims their leading words ──
    real = json.loads(read_text(REPO_ROOT / "src" / "dss-config" / "sources" / "c.lang.json"))
    rows = real.get("preprocess", {}).get("pragmaEffects", [])
    both = [sentinel(kind, 7).split() for kind in ("open", "close")]
    claimed = [[r.get("prefix") for r in rows if r.get("prefix") and words[:len(r["prefix"])] == r["prefix"]]
               for words in both]
    arm("the real config CLAIMS both sentinels, so they speak only when disarmed", all(claimed), claimed)
    arm("each sentinel is ONE word after its prefix (DSS joins a pragma's tokens with blanks)",
        all(len(w) == 2 and re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", w[1]) and SENTINEL_RE.fullmatch(" ".join(w))
            for w in both), both)
    # ── the log: pragmas by leading word, the sentinels apart and per TU, every code counted ──
    def said(words: str) -> str:
        return "error[%s]: unrecognized pragma '%s' — no row claims it" % (PRAGMA_CODE, words)
    # The two locations are filled from `%d`, as check-plan-citations' own fixtures are: spelled out here, a TU
    # name followed by its line would be counted as a positional citation of a file this tree does not carry.
    at_a, at_b = "a.c:%d:%d" % (1, 1), "b.h:%d:%d" % (9, 9)
    names = ["a.c", "b.c"]
    head = [said(sentinel("open", 0)), "  --> <tree>/" + at_a, said(sentinel("close", 0)), said(sentinel("open", 1)),
            said("pack ( push , 8 )"), "  --> <tree>/" + at_b, said("pack ( pop )"),
            "warning[H_UnreachableCode]: unreachable"]
    whole = "\n".join(head + [said(sentinel("close", 1))])
    log = Log(whole, "macho")
    arm("a log: pragmas counted by leading word, the sentinels apart, per TU",
        log.lines == ["macho pack 2"] and log.opened == {0, 1} and log.closed == {0, 1},
        (log.lines, log.opened, log.closed))
    arm("...every diagnostic code counted; no other error, nothing elided: WHOLE",
        log.tally == {"error[P_PreprocessorPragma]": 6, "warning[H_UnreachableCode]": 1} and log.foreign_count == 0
        and not log.elided and refusals(log, names) == [], (dict(log.tally), log.foreign, refusals(log, names)))
    arm("a census measured the corpus only when both sentinels spoke in EVERY TU",
        measured_whole(log, names) and not measured_whole(log, names + ["c.c"])
        and not measured_whole(Log("\n".join(head[2:] + [said(sentinel("close", 1))]), "macho"), names)
        and not measured_whole(Log("", "macho"), []))
    # ★ The re-review's MAJOR 1, its pin: the same log, except that TU 1 stopped after its OPENING sentinel.
    cut = refusals(Log("\n".join(head), "macho"), names)
    arm("a TU whose body stops after its OPENING sentinel is NOT measured",
        len(cut) == 1 and "CLOSING sentinel is silent in 1 of 2 TU(s) (001-b.c)" in cut[0], cut)
    # ★ A part of a TU unread while its closing sentinel still speaks: an include not found, which DSS reports and
    # goes on past -- every TU closed, and the census still refuses.
    not_found = "error[P_PreprocessorIncludeError]: quote include not found: windows.h"
    missing = Log("\n".join(head + [not_found, "  --> <built-in>", said(sentinel("close", 1))]), "macho")
    why = refusals(missing, names)
    arm("an error that is not an unclaimed pragma refuses the census, every TU closed",
        missing.closed == {0, 1} and len(why) == 1 and "1 error(s) that are not an unclaimed pragma" in why[0]
        and missing.foreign == [not_found + "  --> <built-in>"], (why, missing.foreign))
    notice = "info[P_DiagnosticsElided]: P0020 (%s) diagnostics were ELIDED and NOT shown, so any count is a FLOOR"
    elided, other = (Log(whole + "\n" + notice % code, "macho") for code in (PRAGMA_CODE, "H_UnreachableCode"))
    arm("an ELIDED pragma diagnostic refuses the census; another code's elision does not",
        any("ELIDED" in w for w in refusals(elided, names)) and refusals(other, names) == []
        and elided.notices == [notice % PRAGMA_CODE], (refusals(elided, names), refusals(other, names)))
    command = census_command(Path("dsscp"), Path("p.json"), Path("out"))
    arm("the census compile lifts the reporter's volume caps",
        command[1:3] == ["--project", "p.json"] and "--max-diagnostics=%d" % UNCAPPED in command
        and "--max-per-code=%d" % UNCAPPED in command, command)
    # ── the wrapper, and a corpus this host does not have ──
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        tu = root / "a.c"
        write_text(tu, "int a;\n")
        (root / "s").mkdir()
        derived, tus = wrapped_manifest({"sources": [str(tu)], "targets": ["x"], "resolveLibraries": [1]},
                                        root / "s")
        text = read_text(Path(derived["sources"][0]))
        arm("each TU's wrapper: its opening sentinel, the TU by its absolute path, its closing sentinel",
            tus == ["a.c"] and text == "#pragma %s\n#include \"%s\"\n#pragma %s\n" % (
                sentinel("open", 0), tu.resolve().as_posix(), sentinel("close", 0))
            and "resolveLibraries" not in derived and derived["targets"] == ["x"], text)
        arm("a manifest naming a TU this host does not have is refused (another host's corpus)",
            refused(wrapped_manifest, {"sources": [str(root / "gone.c")]}, root / "s2"))
    # ── one class regenerated, the header and every other class kept ──
    tracked = "# a header\nmacho pack 22\npe warning 1685\n"
    arm("the regeneration replaces ONE class's lines and keeps the header and the others",
        regenerated(tracked, "pe", ["pe GCC 3"]) == "# a header\nmacho pack 22\npe GCC 3\n"
        and regenerated(tracked, "pe", []) == "# a header\nmacho pack 22\n"
        and class_lines(tracked, "macho") == ["macho pack 22"], regenerated(tracked, "pe", ["pe GCC 3"]))
    if ran[0] != EXPECTED_ARMS:
        failed[0] += 1
        print("pragma-census self-test: ARM COUNT %d, expected %d -- EXPECTED_ARMS is the ratchet"
              % (ran[0], EXPECTED_ARMS))
    print("pragma-census self-test: %d arm(s), %d failed" % (ran[0], failed[0]))
    return 0 if failed[0] == 0 else 1


def main(argv: list[str]) -> int:
    if argv == ["--self-test"]:
        return self_test()
    ap = argparse.ArgumentParser(prog="pragma-profile-census", allow_abbrev=False,
                                 description="Census the reached #pragma vocabulary of one corpus's own class.")
    ap.add_argument("--update", action="store_true",
                    help="regenerate the expected set with this class's lines, into --out (never the tracked file)")
    ap.add_argument("--out", default=None, help="with --update: the directory the regenerated set is written to")
    ap.add_argument("--dss-bin", default=None, help="the compiler to measure: the leg's own build")
    ap.add_argument("--manifest", required=True, help="the corpus manifest a sqlite run on this leg wrote")
    args = ap.parse_args(argv)
    if args.update and not args.out:
        die("--update writes the regenerated set into --out, the step's own directory, and never the tracked file "
            "(a run on a remote leg would rewrite only that host's copy)")
    if args.out and not args.update:
        die("--out is --update's directory; a check writes nothing")

    dss_bin = find_dss_bin(args.dss_bin)
    manifest_path = Path(args.manifest)
    # ★ A RELATIVE MANIFEST IS TAKEN FROM THE TREE (2026-09-30), not from the directory the program runs in.
    if not manifest_path.is_absolute():
        manifest_path = REPO_ROOT / manifest_path
    if not manifest_path.is_file():
        die(f"no corpus manifest at {shown(str(manifest_path))} — a sqlite run on this leg writes one (its "
            f"recompile: `dssharness run sqlite --legs <leg> --manual-step recompile`); name it relative to the tree.")
    manifest = json.loads(read_text(manifest_path))
    klass = manifest_class(manifest)

    scratch = Path(tempfile.mkdtemp(prefix="dss-pragma-census."))
    try:
        disarm_config(scratch)
        env = dict(os.environ)
        env["DSS_CONFIG_ROOT"] = str(scratch)
        log, rc, names = census(dss_bin, manifest, klass, scratch, env)
        actual = log.lines
        ended = len(log.opened & log.closed & set(range(len(names))))
        summary = ", ".join("%s %d" % (k, n) for k, n in
                            sorted(log.tally.items(), key=lambda kv: (-kv[1], kv[0]))[:12])
        info(f"class {klass}: compiler exit {rc} (a census run is EXPECTED to fail the build); both sentinels "
             f"spoke in {ended} of {len(names)} TU(s) under the disarmed config; "
             f"{sum(int(l.rsplit(' ', 1)[1]) for l in actual)} pragma line(s) in {len(actual)} word(s) besides the "
             f"sentinels; {log.foreign_count} other error(s); diagnostics: {summary or 'none'}")
        # ★ A RUN THAT DID NOT MEASURE THE WHOLE CORPUS SAYS SO (see the header).
        why = refusals(log, names)
        if why:
            for reason in why:
                info("FAIL: " + reason)
            info("so the census neither matches nor regenerates anything.")
            if log.foreign:
                info("The first errors that are not an unclaimed pragma:")
                for line in log.foreign:
                    info("    " + shown(line))
            for line in log.notices:
                info("    " + shown(line))
            return EXIT_CANNOT_RUN

        tracked = read_text(EXPECTED_FILE) if EXPECTED_FILE.is_file() else ""
        expected = sorted(class_lines(tracked, klass))
        if args.update:
            out_dir = Path(args.out)
            out_dir.mkdir(parents=True, exist_ok=True)
            target = out_dir / EXPECTED_NAME
            text = regenerated(tracked, klass, actual)
            write_text(target, text)
            for line in difflib.unified_diff(tracked.splitlines(), text.splitlines(), fromfile="tracked",
                                             tofile="regenerated", lineterm=""):
                print(line, file=sys.stderr)
            info(f"expected set REGENERATED for class {klass} -> {shown(str(target))} (the step's kept output; "
                 f"review it, bring it back with `dssharness sync --pull` from a remote leg, and replace the tracked "
                 f"file in the change that reviews it)")
            return EXIT_MATCH
        if expected == actual:
            info(f"census of class {klass} MATCHES the checked-in expected set ({len(actual)} line(s); the other "
                 f"classes are their own legs' to judge).")
            return EXIT_MATCH
        info(f"census of class {klass} DIFFERS from the checked-in expected set.")
        info("This is a REVIEWABLE CHANGE, not automatically a bug — the reached")
        info("set grows as other cycles let TUs get further. Read the diff, decide")
        info("whether each new prefix needs a 'preprocess.pragmaEffects' row, then")
        info("regenerate it with the runner pragma-profile-census-update on this leg.")
        for line in difflib.unified_diff(expected, actual, fromfile="expected", tofile="actual", lineterm=""):
            print(line, file=sys.stderr)
        return EXIT_DIFFERS
    finally:
        shutil.rmtree(scratch, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
