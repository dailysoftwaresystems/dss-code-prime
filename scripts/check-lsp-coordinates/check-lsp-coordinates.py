#!/usr/bin/env python3
# PURPOSE: refuse a raw coordinate conversion in src/lsp/ outside lsp_coordinates.cpp — the anti-regression device for D-LSP-POSITIONS-RESOLVED-IN-SYNTHESIZED-PREPROCESSOR-COORDINATES.
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

⚠ COMMENTS AND STRINGS ARE EXEMPT, deliberately. The routed handlers explain
themselves by NAMING the old spelling ("`tree.source()` does not appear below"),
and a guard that punished its own documentation would be quietly deleted the
first time it fired on prose. Only CODE is scanned.

════════════════════════════════════════════════════════════════════════════
RED-ON-DISABLE
════════════════════════════════════════════════════════════════════════════
Put `positionToByteOffset(tree.source(), pos)` back into a handler in
`src/lsp/lsp_server.cpp` and this exits 1 naming the file and the spelling.
Exercised by `lsp/test_lsp_coordinates` through the ctest entry, not by reading.

POSIX-only twin: NONE, and that is deliberate — this is a `.py`, which runs
unchanged on the Windows leg and in WSL, so a `.ps1` would be a second
implementation of something that was never split.
"""
import os
import re
import sys

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

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
LSP = os.path.join(REPO, "src", "lsp")

# The OWNER of every conversion, plus the primitive layer it composes. These two
# are where the spellings are DEFINED; everywhere else they are a regression.
EXEMPT = {"lsp_coordinates.cpp", "lsp_coordinates.hpp",
          "lsp_semantic_query.cpp", "lsp_semantic_query.hpp"}

BANNED = [
    (re.compile(r"(?<![\w])(?:\.|->)source\s*\("),
     "a Tree's source() is the SYNTHESIZED buffer"),
    (re.compile(r"(?<![\w])positionToByteOffset\s*\("),
     "the inbound primitive"),
    (re.compile(r"(?<![\w])spanToRange\s*\("),
     "the outbound primitive"),
    (re.compile(r"(?<![\w])byteOffsetToPosition\s*\("),
     "the outbound primitive"),
]


def strip_comments_and_strings(text):
    """Blank out //, /* */, "..." and '...' so only CODE is scanned.

    Replaces with spaces rather than deleting, so reported line numbers stay
    the file's own.
    """
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        two = text[i:i + 2]
        if two == "//":
            j = text.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
        elif two == "/*":
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append("".join(ch if ch == "\n" else " " for ch in text[i:j]))
            i = j
        elif c in "\"'":
            j = i + 1
            while j < n and text[j] != c:
                j += 2 if text[j] == "\\" else 1
            j = min(j + 1, n)
            out.append("".join(ch if ch == "\n" else " " for ch in text[i:j]))
            i = j
        else:
            out.append(c)
            i += 1
    return "".join(out)


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
        for lineno, line in enumerate(code.split("\n"), start=1):
            for pattern, why in BANNED:
                m = pattern.search(line)
                if m:
                    violations.append((name, lineno, m.group(0).strip(), why))

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


if __name__ == "__main__":
    sys.exit(main())
