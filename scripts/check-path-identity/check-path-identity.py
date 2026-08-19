#!/usr/bin/env python3
# PURPOSE: refuse a second path canonicalizer -- path resolution lives in exactly one place.
"""Path resolution lives in ONE place. This refuses a second one.

WHY THIS EXISTS
---------------
`core::PathIdentity` makes the ordinary mistake UNCOMPILABLE: a raw `fs::path`
cannot be a key of the containers that hold path identity, because those
containers are typed on `PathIdentity` and `PathIdentity` has exactly one
constructor. That is the guarantee, and it is the compiler's, not this script's.

This script is the BACKSTOP for what a type cannot reach: a NEW file that starts
doing path work of its own -- resolving, normalizing, or comparing spellings --
without anyone deciding that it should.

★★★ IT DEFINES A COMPLEMENT, IT DOES NOT ENUMERATE.
The tempting shape is "reject these N function names", and this repo has already
been bitten TWICE by exactly that shape in its own anchor instrument -- both
times by missing a case nobody had listed. `fs::canonical`, `fs::absolute`,
`GetFullPathNameW`, `_fullpath`, `realpath`, or a hand-rolled string compare all
sail past a grep for `weakly_canonical`. So the primary rule is the complement:

    IN `src/`, A FILE MAY ONLY INCLUDE <filesystem> IF IT IS ON THE ALLOWLIST.

That fails SAFE. A new file that genuinely needs `<filesystem>` trips this and
either joins the allowlist deliberately or uses the helper -- and either outcome
is a decision on the record rather than a habit.

⚠ THE ALLOWLIST IS NOT SMALL, AND THAT IS MEASURED, NOT ASSUMED: 50 of 441
`src/` files include <filesystem> today. So this rule is a "no NEW path code
without a decision" gate rather than a claim that path code is rare. Rule 2
below is what actually watches the existing 50.

RULE 2 (secondary, and yes it IS an enumeration): a RESOLUTION call outside
`core/substrate/path_identity.cpp`. It is a net under the net -- to bypass both
you would have to already be on the allowlist AND use a spelling nobody listed,
and rule 1 is what makes the allowlist the thing you have to edit.

Usage:
    python scripts/check-path-identity/check-path-identity.py            # check
    python scripts/check-path-identity/check-path-identity.py --selftest # prove the matcher detects
    python scripts/check-path-identity/check-path-identity.py --regen    # reprint the allowlist
"""
from __future__ import annotations

import pathlib
import re
import sys

REPO = pathlib.Path(__file__).resolve().parent.parent.parent
SRC = REPO / "src"

# The ONE file allowed to resolve paths. Everything else asks it.
CHOKEPOINT = "core/substrate/path_identity.cpp"

# Resolution spellings for rule 2. Deliberately NOT the primary guarantee --
# see the module docstring on why an enumeration cannot be one.
RESOLUTION_CALLS = (
    "weakly_canonical",
    "fs::canonical",
    "filesystem::canonical",
    "GetFullPathName",
    "GetLongPathName",
    "GetShortPathName",
    "GetFinalPathNameByHandle",
    "_fullpath",
    "realpath(",
)

# ★ THE ONE RULE-2 EXEMPTION, AND WHY IT IS NOT A HOLE.
# `config_path_walk.cpp` resolves the RUNNING EXECUTABLE's symlink in order to
# derive a directory to WALK -- `/usr/bin/dsscp -> ../lib/dss/dsscp` must land on
# the real image's directory or the relative hop to the data dir starts from the
# wrong place. Its result is never compared against another path, never used as a
# map key, and never printed as an identity. Forcing it through `PathIdentity`
# would convert a resolution into an identity and buy nothing.
# ⚠ Reviewed 2026-08-18 against all 13 call sites: 12 were true identity keys and
# were converted; this is the 1 that was not. A SECOND entry here should be
# argued, not appended.
RESOLUTION_EXEMPT = frozenset({
    "core/types/config_path_walk.cpp",
})

# Files that legitimately touch <filesystem> today. Generated from the tree with
# --regen; a NEW entry is a decision, which is the entire point.
ALLOWLIST = frozenset("""
analysis/compilation_unit/compilation_unit.cpp
analysis/compilation_unit/compilation_unit.hpp
analysis/compilation_unit/import_resolver.hpp
analysis/preprocess/preprocessor.hpp
analysis/semantic/semantic_analyzer.cpp
core/substrate/path_identity.hpp
core/substrate/process_spawn.hpp
core/types/config_path_walk.hpp
core/types/glob_match.cpp
core/types/glob_match.hpp
core/types/grammar_schema.hpp
core/types/grammar_schema_json.cpp
core/types/header_case_diagnostic.hpp
core/types/include_path_resolve.hpp
core/types/predefined_macro_json.cpp
core/types/project_config.hpp
core/types/resolve_library_spec.hpp
core/types/source_buffer.hpp
core/types/target_schema.cpp
core/types/target_schema.hpp
ffi/binary_reader.hpp
ffi/c_header_parser.hpp
ffi/ingest.cpp
ffi/ingest.hpp
ffi/shipped_lib_descriptor.cpp
ffi/shipped_lib_descriptor.hpp
link/object_format_schema.cpp
link/object_format_schema.hpp
link/writer.cpp
link/writer.hpp
lsp/lsp_server.cpp
lsp/lsp_server.hpp
lsp/schema_cache.cpp
lsp/schema_cache.hpp
lsp/workspace_project.hpp
opt/optimizer_json.cpp
program/build_scripts.hpp
program/cli_args.hpp
program/compile_pipeline.hpp
program/dependency_cache.hpp
program/dependency_lockfile.hpp
program/dependency_resolver.hpp
program/dump_predefined_macros.cpp
program/git_acquire.hpp
program/input_resolver.hpp
program/program.cpp
program/program.hpp
program/project_sources.hpp
program/runtime_object_cache.cpp
program/runtime_object_cache.hpp
""".split())


def rel(p: pathlib.Path) -> str:
    return p.relative_to(SRC).as_posix()


def strip_comments_and_strings(text: str) -> str:
    """Blank out // and /* */ comments and "..." literals.

    A rule that fires on a MENTION rather than a CALL trains people to reword
    their comments, which is worse than no rule: the next real occurrence hides
    behind the habit.
    """
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            j = text.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
        elif c == "/" and i + 1 < n and text[i + 1] == "*":
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append(" " * (j - i))
            i = j
        elif c == '"':
            j = i + 1
            while j < n and text[j] != '"':
                j += 2 if text[j] == "\\" else 1
            j = min(j + 1, n)
            out.append(" " * (j - i))
            i = j
        else:
            out.append(c)
            i += 1
    return "".join(out)


INCLUDE_FS = re.compile(r"^\s*#\s*include\s*<filesystem>", re.M)


def scan() -> list[str]:
    problems: list[str] = []
    for p in sorted(SRC.rglob("*")):
        if p.suffix not in (".cpp", ".hpp", ".h", ".cc"):
            continue
        r = rel(p)
        text = p.read_text(encoding="utf-8", errors="replace")
        code = strip_comments_and_strings(text)

        # Rule 1 — the complement.
        if INCLUDE_FS.search(code) and r not in ALLOWLIST:
            problems.append(
                f"{r}: includes <filesystem> but is not on the path allowlist "
                f"in scripts/check-path-identity/check-path-identity.py.\n"
                f"    Path IDENTITY belongs to core::PathIdentity — key your "
                f"maps/sets on it and this file needs no path resolution of its "
                f"own. If this file genuinely needs <filesystem> for I/O "
                f"(streams, exists, directory_iterator), add it to ALLOWLIST "
                f"in the same commit, so the decision is on the record."
            )

        # Rule 2 — the net under the net.
        if r != CHOKEPOINT and r not in RESOLUTION_EXEMPT:
            for call in RESOLUTION_CALLS:
                if call in code:
                    problems.append(
                        f"{r}: calls `{call}` outside the one canonicalizer "
                        f"({CHOKEPOINT}).\n"
                        f"    A second resolver is a second owner of one fact, "
                        f"and the two disagree silently — which is precisely "
                        f"how 8.3 short names split every path key in the "
                        f"compiler while every test stayed green. Call "
                        f"core::PathIdentity::of() / canonicalIdentityKey()."
                    )
    return problems


def selftest() -> int:
    """A checker that reads nothing reports an all-clear. Prove it reads."""
    bad_include = "#include <filesystem>\nint x;\n"
    bad_call = "int f() { return fs::weakly_canonical(p); }\n"
    ok_comment = "// mentions weakly_canonical in prose only\nint y;\n"
    ok_string = 'char const* s = "weakly_canonical";\n'

    failures = []
    if not INCLUDE_FS.search(strip_comments_and_strings(bad_include)):
        failures.append("the <filesystem> matcher missed a real include")
    if "weakly_canonical" not in strip_comments_and_strings(bad_call):
        failures.append("the resolution matcher missed a real call")
    if "weakly_canonical" in strip_comments_and_strings(ok_comment):
        failures.append("a COMMENT mention was treated as a call — this rule "
                        "would train people to reword comments")
    if "weakly_canonical" in strip_comments_and_strings(ok_string):
        failures.append("a STRING literal was treated as a call")

    for f in failures:
        print(f"SELFTEST FAIL: {f}")
    if failures:
        return 1
    print("check-path-identity selftest: OK (4 controls)")
    return 0


def regen() -> int:
    hits = []
    for p in sorted(SRC.rglob("*")):
        if p.suffix not in (".cpp", ".hpp", ".h", ".cc"):
            continue
        code = strip_comments_and_strings(
            p.read_text(encoding="utf-8", errors="replace"))
        if INCLUDE_FS.search(code):
            hits.append(rel(p))
    print("\n".join(hits))
    print(f"\n# {len(hits)} file(s)", file=sys.stderr)
    return 0


def main() -> int:
    if "--selftest" in sys.argv:
        return selftest()
    if "--regen" in sys.argv:
        return regen()
    problems = scan()
    if problems:
        print(f"check-path-identity: {len(problems)} problem(s)\n")
        for p in problems:
            print(f"  {p}\n")
        return 1
    print("check-path-identity: OK — one canonicalizer, allowlist respected")
    return 0


if __name__ == "__main__":
    sys.exit(main())
