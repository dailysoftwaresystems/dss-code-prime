#!/usr/bin/env python3
# PURPOSE: refuse a new anchor id inside a C++ string literal under src/: operator output states the condition and the action, never the bookkeeping.
"""check-emitted-anchor-ids.py -- the EMITTED-ANCHOR-ID ratchet.

★★★ WHY THIS EXISTS, and it is a measured population rather than a style rule. A diagnostic that names a
deferral row tells the operator "a known deferral, wait for it". When the row closes, the SAME text -- still
compiled into every build -- tells them something false: the refusal may be real and deliberate while the row it
blames has moved on (the first adjudicated instance refused correctly; only its provenance was stale). An anchor
id is internal bookkeeping. It belongs in the SOURCE, beside the emit site, where closing a row cannot turn it
into compiler output; the message states the CONDITION and the ACTION.
✔MEASURED 2026-09-23 (P68 round 9) by this guard's own census: 647 anchor ids in 630 string literals across 70
files under src/ (646 in 69 once a preprocessing directive's literal is exempt). The row that asked for this
guard had counted 71 through a list of reporting entrypoints -- a list is the enumerate-the-set mistake, so the
predicate here is the LITERAL, whatever consumes it.

⚠ THIS IS NOT `check-stale-refusal-citations` WIDENED. That guard's predicate demands a persistence word near a
refusal word in PROSE, and these messages carry none; widening it would reopen the false-positive surface its
measured narrowings closed. Different subject, different predicate, different owner.

THE PREDICATE: an anchor id inside a C++ string literal in a file under src/ (the compiler's own sources:
`src/dss-config/` -- configuration and the shipped runtime's C sources -- is not the compiler's output and is not
scanned). Comments never count: that is where the id belongs. ONE exemption, structural rather than listed: a
literal inside a `static_assert(...)` is read only by DSS's own build, never by an operator.

ONE OWNER PER FACT:
  * the LITERALS come from `check-no-abort-in-tests`' `scan_code` -- the one owner of "what is code, what is a
    comment, what is a string", whose stripped text the other guards import -- never from a second C++ parser
    here;
  * the ID SHAPE is `check-anchor-balance`'s `ANCHOR_TOKEN` (the vocabulary the registry guards share), read at a
    word boundary, so an id-shaped run that starts inside a longer word is not an id.

THE RATCHET: `INVENTORY` holds a per-FILE count of anchor ids in literals -- per file, never per line, so an edit
above a site is not a finding and a site cannot move sideways unseen. A file above its count, or a file not
listed, FAILS; a file BELOW its count fails too until the count is lowered in the same commit, because unclaimed
headroom is where the next one hides. The inventory only ever comes DOWN; it is empty when the row is done.
The burn-down of a site moves the id into a COMMENT at the emit site and rewrites the message to its condition and
its action; a test that asserted the id moves to the condition.

★★ THE NO-ARGUMENT FORM (the ctest form) VERIFIES THE TREE **AND THEN RUNS THE SELF-TEST**, honouring both
statuses -- a guard whose proof sat behind a flag nothing passes proved nothing (the siblings' measured lesson).

Exit codes: 0 clean (within the ratchet) -- 1 a new, grown or stale count -- 2 the scan collapsed -- 3 usage.
Usage:
    python .harness-config/runner/actions/check-emitted-anchor-ids/check-emitted-anchor-ids.py
    python .harness-config/runner/actions/check-emitted-anchor-ids/check-emitted-anchor-ids.py --list
    python .harness-config/runner/actions/check-emitted-anchor-ids/check-emitted-anchor-ids.py --selftest
"""
import importlib.util
import io
import os
import re
import shutil
import sys
import tempfile

sys.dont_write_bytecode = True  # a by-path load must not write __pycache__ beside another action (the rule: check-scripts-index)

for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError, OSError):   # pragma: no cover - odd stream
        pass

NAME = "emitted-anchor-ids"
ACTIONS = os.path.dirname(os.path.dirname(os.path.realpath(__file__)))
SCAN_ROOT = "src"
# Not the compiler's output: the configuration documents and the shipped runtime's own C sources.
EXCLUDED = ("src/dss-config",)
EXTS = (".cpp", ".hpp", ".h", ".cc", ".cxx", ".hh", ".inl")
# Far below the live figure, so ordinary churn never trips it: this catches a COLLAPSED scan (a moved subtree, a
# drifted extension filter), not drift.
FILE_FLOOR = 400
STATIC_ASSERT = re.compile(r"\bstatic_assert\s*\(")

# ══ INVENTORY — PRE-EXISTING DEBT, A RATCHET ══
# Per-file count of anchor ids inside string literals, as measured when this guard landed, minus each burn-down.
# ⛔ It may only be LOWERED (the guard names the new value); raising an entry or adding a file is a FAILURE.
INVENTORY = {
    "src/analysis/preprocess/preprocessor.cpp": 2,
    "src/analysis/semantic/inline_asm_facts.hpp": 1,
    "src/analysis/semantic/semantic_analyzer.cpp": 11,
    "src/asm/asm.cpp": 36,
    "src/asm/format/fixed32.cpp": 2,
    "src/asm/format/walker_util.hpp": 3,
    "src/core/types/grammar_schema_json.cpp": 4,
    "src/core/types/object_format_kind.hpp": 1,
    "src/core/types/target_schema.cpp": 4,
    "src/core/types/target_schema_json.cpp": 7,
    "src/ffi/binary_reader.cpp": 2,
    "src/ffi/binary_readers/ar_reader.cpp": 4,
    "src/ffi/ingest.cpp": 2,
    "src/ffi/shipped_lib_descriptor.cpp": 6,
    "src/hir/hir_verifier.cpp": 2,
    "src/hir/lowering/cst_to_hir.cpp": 12,
    "src/link/branch_veneers.cpp": 15,
    "src/link/entry_trampoline.cpp": 12,
    "src/link/format/ar.cpp": 12,
    "src/link/format/coff_object_reader.cpp": 7,
    "src/link/format/dwarf_cfi.hpp": 1,
    "src/link/format/dwarf_cfi_decode.hpp": 1,
    "src/link/format/elf.cpp": 32,
    "src/link/format/elf_backend.cpp": 7,
    "src/link/format/elf_object_reader.cpp": 4,
    "src/link/format/exec_data_section.hpp": 3,
    "src/link/format/exec_reloc_apply.hpp": 5,
    "src/link/format/interior_block_symbol_va.hpp": 1,
    "src/link/format/macho.cpp": 47,
    "src/link/format/macho_backend.cpp": 10,
    "src/link/format/macho_object_reader.cpp": 5,
    "src/link/format/object_atom_coverage.hpp": 2,
    "src/link/format/pe.cpp": 31,
    "src/link/format/pe_backend.cpp": 7,
    "src/link/format/unwind_pointer_reloc.hpp": 2,
    "src/link/format/weak_definition_gate.hpp": 2,
    "src/link/image_request.cpp": 5,
    "src/link/linker.cpp": 18,
    "src/link/object_format_schema.cpp": 25,
    "src/link/object_format_schema_json.cpp": 9,
    "src/lir/lir_callconv.cpp": 29,
    "src/lir/lir_verifier.cpp": 3,
    "src/lir/lowering/mir_to_lir.cpp": 48,
    "src/mir/lowering/hir_to_mir.cpp": 77,
    "src/mir/merge/synth_seh_funclets.cpp": 12,
    "src/program/compile_pipeline.cpp": 22,
    "src/program/cross_validate_target_format.cpp": 2,
    "src/program/program.cpp": 1,
    "src/program/runtime_object_cache.cpp": 2,
    "src/program/runtime_object_cache.hpp": 1,
}


def _sibling(action, what):
    """`<actions>/<action>/<action>.py`, loaded by path (a hyphen is not a module name). Missing is a COLLAPSE:
    this guard must not carry a second copy of what its sibling owns."""
    path = os.path.join(ACTIONS, action, action + ".py")
    if not os.path.isfile(path):
        print("%s: cannot find %s -- %s is owned there and nowhere else" % (NAME, path, what), file=sys.stderr)
        sys.exit(2)
    spec = importlib.util.spec_from_file_location("dss_%s" % action.replace("-", "_"), path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def _own_tree():
    """The tree THIS FILE lives in, by `owning-tree`'s walk: never the caller's cwd."""
    ot = _sibling("owning-tree", "which tree this file is in")
    try:
        return ot.resolve(__file__)
    except ot.Refusal as exc:
        print("%s: %s" % (NAME, exc), file=sys.stderr)
        sys.exit(2)


_SCAN = _TOKEN = None


def scan_code():
    global _SCAN
    if _SCAN is None:
        _SCAN = _sibling("check-no-abort-in-tests", "the C++ comment/string scanner").scan_code
    return _SCAN


def anchor_token():
    """`check-anchor-balance`'s ANCHOR_TOKEN, at a word boundary on the left."""
    global _TOKEN
    if _TOKEN is None:
        shape = _sibling("check-anchor-balance", "the anchor-id shape").ANCHOR_TOKEN.pattern
        _TOKEN = re.compile(r"(?<![A-Za-z0-9_])" + shape)
    return _TOKEN


def static_assert_spans(stripped):
    """[(open, close)] offsets of every `static_assert( ... )` argument list in STRIPPED text (strings and
    comments are already gone, so every paren counted is code). An unbalanced one runs to the end."""
    spans = []
    for m in STATIC_ASSERT.finditer(stripped):
        depth, i = 0, m.end() - 1
        while i < len(stripped):
            if stripped[i] == "(":
                depth += 1
            elif stripped[i] == ")":
                depth -= 1
                if depth == 0:
                    break
            i += 1
        spans.append((m.end() - 1, i))
    return spans


def on_directive_line(stripped, at):
    """Is offset `at` of STRIPPED text on a preprocessing-directive line (`#error "..."`, `#pragma message`,
    `#include "..."`), continuation lines included? Such a literal is read by DSS's own BUILD, never by an
    operator of the compiler it builds -- the `static_assert` exemption's twin."""
    start = stripped.rfind("\n", 0, at) + 1
    while start > 0:
        prev_end = start - 1                       # the '\n' that ends the previous line
        prev_start = stripped.rfind("\n", 0, prev_end) + 1
        if not stripped[prev_start:prev_end].rstrip("\r").endswith("\\"):
            break
        start = prev_start
    return stripped[start:at].lstrip().startswith("#")


def sites_in(text):
    """[(line, [ids])] for every string literal of C++ `text` that carries an anchor id, outside every
    `static_assert` and every preprocessing directive, in source order."""
    stripped, literals = scan_code()(text)
    spans = static_assert_spans(stripped)
    token = anchor_token()
    out = []
    for lit in literals:
        if any(a < lit.at <= b for a, b in spans) or on_directive_line(stripped, lit.at):
            continue
        ids = [m.group(0) for m in token.finditer(lit.body)]
        if ids:
            out.append((lit.line, ids))
    return out


def census(root):
    """-> ({relpath: [(line, [ids])]} for every file with a site, files scanned)."""
    per_file, scanned = {}, 0
    base = os.path.join(root, SCAN_ROOT)
    excluded = [os.path.normcase(os.path.join(root, *e.split("/"))) for e in EXCLUDED]
    for dirpath, dirnames, filenames in os.walk(base):
        dirnames[:] = sorted(d for d in dirnames
                             if os.path.normcase(os.path.join(dirpath, d)) not in excluded)
        for fn in sorted(filenames):
            if not fn.endswith(EXTS):
                continue
            path = os.path.join(dirpath, fn)
            with io.open(path, encoding="utf-8", errors="replace") as fh:
                text = fh.read()
            scanned += 1
            sites = sites_in(text)
            if sites:
                per_file[os.path.relpath(path, root).replace("\\", "/")] = sites
    return per_file, scanned


def judge(per_file, scanned, inventory, floor=FILE_FLOOR):
    """-> (rc, lines): the ratchet's verdict, printed by the caller. PURE, so the self-test drives it."""
    if scanned < floor:
        return 2, ["%s: FAIL - scanned only %d file(s) under %s, below the floor of %d. This does NOT mean the "
                   "sources are clean - it means THIS SCAN COLLAPSED. Fix the scan; do not lower the floor."
                   % (NAME, scanned, SCAN_ROOT, floor)]
    counts = dict((path, sum(len(ids) for _ln, ids in sites)) for path, sites in per_file.items())
    grown, stale = [], []
    for path in sorted(counts):
        ceiling = inventory.get(path, 0)
        if counts[path] > ceiling:
            grown.append("    %s: %d anchor id(s) in string literals, the inventory allows %d"
                         % (path, counts[path], ceiling))
            for line, ids in per_file[path]:
                grown.append("        line %d: %s" % (line, ", ".join(ids)))
    for path, ceiling in sorted(inventory.items()):
        actual = counts.get(path, 0)
        if actual < ceiling:
            stale.append("    %s: %d now, the inventory still says %d -> lower it to %d%s"
                         % (path, actual, ceiling, actual, " (delete the entry)" if actual == 0 else ""))
    if grown:
        return 1, (["%s: FAIL - an anchor id NEW in a C++ string literal under %s:" % (NAME, SCAN_ROOT)] + grown
                   + ["  An anchor id is bookkeeping: it goes in a COMMENT at the emit site, where closing the "
                      "row cannot turn it into compiler output. The message states the CONDITION and the ACTION.",
                      "  Do NOT raise the INVENTORY to make this pass - it only ever comes DOWN."])
    if stale:
        return 1, (["%s: FAIL - the INVENTORY is STALE and grants unused headroom:" % NAME] + stale
                   + ["  Sites were burned down without lowering the ceiling. Unclaimed headroom is exactly "
                      "where the next one hides. Update the dict in the same commit."])
    total = sum(counts.values())
    return 0, ["%s: OK (%d file(s) scanned under %s; %d anchor id(s) in string literals across %d file(s), all "
               "within the INVENTORY ratchet). DEBT, not a pass - each is operator output that a closed row "
               "turns false. Green means only that no NEW one landed." % (NAME, scanned, SCAN_ROOT, total,
                                                                         len(counts))]


def main(argv):
    root = _own_tree()
    per_file, scanned = census(root)
    if "--list" in argv:
        for path in sorted(per_file):
            print("%3d  %s" % (sum(len(ids) for _l, ids in per_file[path]), path))
            for line, ids in per_file[path]:
                print("       line %d: %s" % (line, ", ".join(ids)))
        print("%s: %d file(s) scanned, %d with a site" % (NAME, scanned, len(per_file)))
        return 0
    rc, lines = judge(per_file, scanned, INVENTORY)
    for line in lines:
        print(line, file=sys.stderr if rc else sys.stdout)
    return rc


# ── the self-test: the predicate on real C++ shapes, and the ratchet's four verdicts ──

def selftest():
    bad = 0

    def arm(label, ok, detail=""):
        nonlocal bad
        if not ok:
            bad += 1
        print("  [%s] %s%s" % ("ok " if ok else "FAIL", label, "" if ok else "\n         " + str(detail)))

    # Built at run time: an anchor-shaped LITERAL in this file would itself read as a citation of a row that
    # does not exist to the registry guards, which scan `.harness-config` too.
    idx = "-".join(("D", "SELFTEST", "EMITTED", "ID"))
    cases = (
        ("an id in a string literal is a site",
         'report(Code::S_X, "refused (%s)");' % idx, [(1, [idx])]),
        ("an id in a comment is not",
         '// see %s\nreport(Code::S_X, "refused");' % idx, []),
        ("an id in a static_assert message is not (DSS's own build reads it)",
         'static_assert(sizeof(int) == 4, "%s");' % idx, []),
        ("...and a literal AFTER that static_assert still is",
         'static_assert(sizeof(int) == 4, "x");\nf("%s");' % idx, [(2, [idx])]),
        ("an id in an #error directive is not (DSS's own build reads it)",
         '#ifndef X\n#    error "define X (see %s)"\n#endif\n' % idx, []),
        ("...nor on a continuation line of a directive",
         '#define MSG \\\n    "%s"\nf(1);' % idx, []),
        ("...but a literal on the line AFTER a directive is a site",
         '#include <cstdio>\nf("%s");' % idx, [(2, [idx])]),
        ("an id in a raw string is a site",
         'auto m = R"(%s)";' % idx, [(1, [idx])]),
        ("a char literal is not a string literal",
         "char c = 'D'; f(c);", []),
        ("an id-shaped run inside a word is not an id",
         'f("%s");' % "".join(("AN", "D", "-OR-XY")), []),
        ("two ids in one literal are two",
         'f("%s and %s-TWO");' % (idx, idx), [(1, [idx, idx + "-TWO"])]),
        ("a literal split over lines is placed on the line it opens",
         'f("head"\n  "%s");' % idx, [(2, [idx])]),
    )
    for label, src, want in cases:
        got = sites_in(src)
        arm(label, got == want, "got %r want %r" % (got, want))
    sites = {"src/a.cpp": [(3, [idx])], "src/b.cpp": [(1, [idx, idx])]}
    rc, lines = judge(sites, 5, {"src/a.cpp": 1, "src/b.cpp": 2}, floor=1)
    arm("within the inventory: OK", rc == 0, lines)
    rc, lines = judge(dict(sites, **{"src/c.cpp": [(9, [idx])]}), 5, {"src/a.cpp": 1, "src/b.cpp": 2}, floor=1)
    arm("a NEW file with a site: FAIL naming the line",
        rc == 1 and any("src/c.cpp: 1" in ln for ln in lines) and any("line 9" in ln for ln in lines), lines)
    rc, lines = judge(sites, 5, {"src/a.cpp": 1, "src/b.cpp": 1}, floor=1)
    arm("a GROWN file: FAIL", rc == 1 and any("src/b.cpp: 2" in ln for ln in lines), lines)
    rc, lines = judge({"src/a.cpp": sites["src/a.cpp"]}, 5, {"src/a.cpp": 1, "src/b.cpp": 2}, floor=1)
    arm("a BURNED-DOWN file whose ceiling was not lowered: FAIL (stale)",
        rc == 1 and any("lower it to 0" in ln for ln in lines), lines)
    rc, lines = judge(sites, 0, {"src/a.cpp": 1, "src/b.cpp": 2}, floor=1)
    arm("a scan that found nothing is a COLLAPSE (2), never OK", rc == 2, lines)
    # The census over a synthetic tree: the EXCLUDED subtree is not scanned, a non-C++ file is not read.
    tmp = tempfile.mkdtemp(prefix="emitted-ids-")
    try:
        for rel, body in (("src/x/a.cpp", 'f("%s");\n' % idx), ("src/dss-config/runtime/r.c", 'f("%s");\n' % idx),
                          ("src/x/b.json", '{"k": "%s"}\n' % idx), ("src/x/c.hpp", "// %s\n" % idx)):
            path = os.path.join(tmp, *rel.split("/"))
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with io.open(path, "w", encoding="utf-8", newline="\n") as fh:
                fh.write(body)
        got, scanned = census(tmp)
        arm("the census scans src/'s C++ only: the config subtree and a JSON file are not read",
            got == {"src/x/a.cpp": [(1, [idx])]} and scanned == 2, (got, scanned))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print("selftest: %s (%d failure(s))" % ("FAIL" if bad else "OK", bad))
    return 1 if bad else 0


if __name__ == "__main__":
    _argv = sys.argv[1:]
    _unknown = [a for a in _argv if a not in ("--selftest", "--list")]
    if _unknown or len(_argv) > 1:
        print("%s: unknown or combined argument(s): %s" % (NAME, " ".join(_argv)), file=sys.stderr)
        sys.exit(3)
    if _argv == ["--selftest"]:
        sys.exit(selftest())
    if _argv == ["--list"]:
        sys.exit(main(_argv))
    # The ctest form: verify the tree, THEN prove the instrument can fail -- both unconditionally.
    _rc = main(_argv)
    print("")
    _rc_self = selftest()
    sys.exit(_rc or _rc_self)
