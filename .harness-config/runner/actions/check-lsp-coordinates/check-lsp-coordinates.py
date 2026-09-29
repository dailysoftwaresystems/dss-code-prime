#!/usr/bin/env python3
# PURPOSE: refuse a raw coordinate conversion in src/lsp/ outside lsp_coordinates.cpp — the anti-regression device for LSP positions resolved in synthesized preprocessor coordinates.
"""Keep `src/lsp/` unable to re-acquire the defect it just lost.

════════════════════════════════════════════════════════════════════════════
WHY THIS IS A SCRIPT AND NOT A COMMENT
════════════════════════════════════════════════════════════════════════════
The defect was never one wrong line of arithmetic. It was that the LSP layer
named a BUFFER nowhere while three coordinate spaces were in play (document /
synth / header-origin), so every handler was free to convert a position against
whichever buffer was nearest to hand — and `tree.source()` is always nearest to
hand. Fixing the arithmetic at the five red call sites would have left the sixth
channel open; the registry already records this class three times.

So the fix is a TYPE (`dss::lsp::DocumentCoordinates`) plus this guard, which
makes the old spelling unreachable. A rule only a reader enforces is exactly the
hole this class keeps coming back through.

════════════════════════════════════════════════════════════════════════════
WHAT IS REFUSED
════════════════════════════════════════════════════════════════════════════
In any `src/lsp/*.cpp` or `*.hpp` other than the OWNER (`lsp_coordinates.*`) and
the primitive layer it is built from (`lsp_semantic_query.*`):

  * `.source()` / `->source()`      — a Tree's SYNTH buffer. Reaching for it is
                                      how a handler ends up interpreting a
                                      document position in synth coordinates.
  * `positionToByteOffset(`         — the inbound primitive. Correct only when
                                      handed the DOCUMENT's buffer, which only
                                      the owner can guarantee.
  * `spanToRange(` / `byteOffsetToPosition(`
                                    — the outbound primitives. Correct only
                                      against the ORIGIN buffer a span resolves
                                      to, which only the owner computes.

⚠ ✔MEASURED 2026-09-24: the `.source()` rule could not match `tree.source()` or
`tree->source()` until then -- a word-boundary lookbehind stood BEFORE the `.`/`->`,
where an identifier always stands -- so only a spelling like `f().source()` was
refused. Fixed; in code the spelling occurs only inside the exempt owner today, so
no verdict changed. The self-test's first two arms keep it reachable.

⚠ COMMENTS AND STRINGS ARE EXEMPT, deliberately. The routed handlers explain
themselves by NAMING the old spelling ("`tree.source()` does not appear below"),
and a guard that punished its own documentation would be quietly deleted the
first time it fired on prose. Only CODE is scanned.

★ WHAT IS CODE IS DECIDED BY THE ONE SHARED SCANNER, IMPORTED, NOT COPIED:
`check-no-abort-in-tests` owns what is code, what is a comment and what is a
string for every guard that reads C++. ✔MEASURED 2026-09-24: this file carried
its own copy until then, with no raw-string rule and no digit-separator rule --
it read the body of a raw string in `json_rpc.cpp` as CODE -- while no verdict
differed over the 20 files it scans; the switch changed none.

════════════════════════════════════════════════════════════════════════════
RED-ON-DISABLE
════════════════════════════════════════════════════════════════════════════
Put `positionToByteOffset(tree.source(), pos)` back into a handler in
`src/lsp/lsp_server.cpp` and this exits 1 naming the file and the spelling.
★ The no-argument form (the ctest entry `lsp_coordinate_ownership_guard`) verifies
the tree AND THEN runs the self-test, so the entry cannot pass without proving the
guard can fail: a banned spelling in CODE is found on its own line, and the same
spelling in a comment, a string or a raw string is not. ✔MEASURED 2026-09-24: until
then nothing exercised it -- `lsp/test_lsp_coordinates` never runs this script.

POSIX-only twin: NONE, and that is deliberate — this is a `.py`, which runs
unchanged on the Windows leg and in WSL, so a `.ps1` would be a second
implementation of something that was never split.
"""
import importlib.util
import os
import re
import sys
sys.dont_write_bytecode = True  # a by-path load must not write __pycache__ beside another action (the rule: check-scripts-index)

# guard_output_encoding_guard: this script prints non-cp1252 characters, and a
# pipe on the Windows leg would otherwise MANGLE or DROP them -- a guard whose
# refusal text is unreadable is a guard nobody acts on.
# AT MODULE SCOPE, covering BOTH streams, deliberately: inside main() is too
# late, because argparse and --help print before it runs. Adding this file to
# the encoding inventory instead is the exact move that ratchet exists to
# refuse.
for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError, OSError):
        pass

def _own_tree():
    """The tree THIS FILE lives in, by `owning-tree`'s walk -- never a count of `..`.

    A count named `.harness-config/runner` the day this file moved out of `scripts/`
    (2026-09-18), and a guard pointed at the wrong root finds nothing to refuse. The owner
    is loaded as a SIBLING (a hyphen is not a module name) and a missing owner is a
    structural failure (exit 2), never a clean pass.
    """
    path = os.path.join(os.path.dirname(os.path.dirname(os.path.realpath(__file__))),
                        "owning-tree", "owning-tree.py")
    if not os.path.isfile(path):
        print("check-lsp-coordinates: cannot find %s -- this guard's tree is resolved there "
              "and nowhere else" % path, file=sys.stderr)
        sys.exit(2)
    spec = importlib.util.spec_from_file_location("dss_owning_tree", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    try:
        return mod.resolve(__file__)
    except mod.Refusal as exc:
        print("check-lsp-coordinates: %s" % exc, file=sys.stderr)
        sys.exit(2)


REPO = _own_tree()
LSP = os.path.join(REPO, "src", "lsp")

# The OWNER of every conversion, plus the primitive layer it composes. These two
# are where the spellings are DEFINED; everywhere else they are a regression.
EXEMPT = {"lsp_coordinates.cpp", "lsp_coordinates.hpp",
          "lsp_semantic_query.cpp", "lsp_semantic_query.hpp"}

BANNED = [
    # `.`/`->` then `source(`, spaces allowed between them. NO word-boundary lookbehind
    # before the `.`: an identifier ALWAYS stands there (`tree.source()`), so the one this
    # line carried until 2026-09-24 made the rule's own spelling unreachable.
    (re.compile(r"(?:\.|->)\s*source\s*\("),
     "a Tree's source() is the SYNTHESIZED buffer"),
    (re.compile(r"(?<![\w])positionToByteOffset\s*\("),
     "the inbound primitive"),
    (re.compile(r"(?<![\w])spanToRange\s*\("),
     "the outbound primitive"),
    (re.compile(r"(?<![\w])byteOffsetToPosition\s*\("),
     "the outbound primitive"),
]


def _load_stripper():
    """The comment/string scanner of `check-no-abort-in-tests`, or a loud death (exit 2): a guard
    that cannot tell code from prose must not scan at all, and a second copy here is the drift the
    one owner exists to stop."""
    here = os.path.dirname(os.path.realpath(__file__))
    sibling = os.path.join(os.path.dirname(here), "check-no-abort-in-tests",
                           "check-no-abort-in-tests.py")
    if not os.path.isfile(sibling):
        print("check-lsp-coordinates: cannot find the shared comment/string scanner at %s -- this "
              "guard reads code through it and nowhere else; restore the sibling, do NOT copy it "
              "here" % sibling, file=sys.stderr)
        sys.exit(2)
    spec = importlib.util.spec_from_file_location("_no_abort_in_tests", sibling)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod.strip_comments_and_strings


strip_comments_and_strings = _load_stripper()


def findings_in(code):
    """[(line, spelling, why)] for every banned spelling in the STRIPPED `code`, in line order: the
    one predicate the tree check and the self-test share."""
    out = []
    for lineno, line in enumerate(code.split("\n"), start=1):
        for pattern, why in BANNED:
            m = pattern.search(line)
            if m:
                out.append((lineno, m.group(0).strip(), why))
    return out


def main():
    violations = []
    for name in sorted(os.listdir(LSP)):
        if not name.endswith((".cpp", ".hpp")):
            continue
        if name in EXEMPT:
            continue
        path = os.path.join(LSP, name)
        with open(path, encoding="utf-8", newline="") as fh:
            code = strip_comments_and_strings(fh.read())
        for lineno, spelling, why in findings_in(code):
            violations.append((name, lineno, spelling, why))

    if not violations:
        print("check-lsp-coordinates: OK - every conversion in src/lsp/ goes "
              "through DocumentCoordinates")
        return 0

    print("check-lsp-coordinates: REFUSED\n")
    print("D-LSP-POSITIONS-RESOLVED-IN-SYNTHESIZED-PREPROCESSOR-COORDINATES:")
    print("a coordinate conversion outside its one owner. A byte offset is")
    print("meaningless without the buffer that produced it, and src/lsp/ has")
    print("THREE in play (document / synth / header-origin).\n")
    for name, lineno, spelling, why in violations:
        # The file and the SPELLING, never a bare line number as the citation:
        # the number is here to find it, the spelling is what identifies it.
        print("  src/lsp/%s  '%s'  (%s)  [line %d]"
              % (name, spelling, why, lineno))
    print("\nRoute it through `dss::lsp::DocumentCoordinates`:")
    print("  a position  -> coords.toSynth(pos)      (nullopt = no synth image)")
    print("  a tree span -> coords.locate(tree, span) (origin uri + range)")
    return 1


def selftest():
    """Each arm fails LOUDLY if this guard stops reading code the way the shared scanner does. The
    raw-string and digit-separator arms are the two shapes the private copy misread: red the day a
    copy comes back."""
    bad = 0

    def arm(label, got, want):
        nonlocal bad
        ok = got == want
        if not ok:
            bad += 1
        print("  [%s] %s%s" % ("ok " if ok else "FAIL", label,
                               "" if ok else "\n         got  %r\n         want %r" % (got, want)))

    def spelled(src):
        return [(ln, sp) for ln, sp, _why in findings_in(strip_comments_and_strings(src))]

    arm("a banned spelling in CODE is found, on its own line",
        spelled("int a;\nauto p = positionToByteOffset(tree.source(), pos);\n"),
        [(2, ".source("), (2, "positionToByteOffset(")])
    arm("`tree.source()` and `tree->source()` are found (a misplaced word boundary had made them "
        "unreachable)", spelled("auto a = tree.source();\nauto b = tree->source();\n"),
        [(1, ".source("), (2, "->source(")])
    arm("the same spellings in a COMMENT are not",
        spelled("// tree.source() does not appear below\n/* spanToRange( */ int a;\n"), [])
    arm("a spelling inside a STRING is not",
        spelled('auto s = "positionToByteOffset(x)";\n'), [])
    arm("a spelling inside a RAW string is not (the private copy read its body as code)",
        spelled('auto m = R"(a "b x.source() c)";\nint z;\n'), [])
    arm("code after a DIGIT SEPARATOR is still read (the private copy blanked it as a char literal)",
        spelled("int n = 1'000;\nauto p = tree.source();\n"), [(2, ".source(")])
    arm("a line after a literal that spans a line end keeps its number",
        spelled('auto s = "one\\\ntwo";\nauto p = tree.source();\n'), [(3, ".source(")])
    arm("the comment/string scanner is the SHARED one, not a copy",
        strip_comments_and_strings.__module__, "_no_abort_in_tests")
    print("check-lsp-coordinates selftest: %s (%d failure(s))" % ("FAIL" if bad else "OK", bad))
    return 1 if bad else 0


if __name__ == "__main__":
    if sys.argv[1:] == ["--selftest"]:
        sys.exit(selftest())
    if sys.argv[1:]:
        print("check-lsp-coordinates: unknown argument(s): %s (the only one is --selftest)"
              % " ".join(sys.argv[1:]), file=sys.stderr)
        sys.exit(2)
    # The ctest form: verify the tree, THEN prove the guard can fail -- both, unconditionally.
    _rc = main()
    print("")
    _rc_self = selftest()
    sys.exit(_rc or _rc_self)
