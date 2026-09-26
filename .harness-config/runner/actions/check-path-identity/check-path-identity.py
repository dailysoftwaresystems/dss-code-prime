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

★ WHAT IS CODE IS DECIDED BY THE ONE SHARED SCANNER, IMPORTED, NOT COPIED:
`check-no-abort-in-tests` owns what is code, what is a comment and what is a
string for every guard that reads C++. ✔MEASURED 2026-09-24: this file carried its
own copy until then, with no character-literal, raw-string or digit-separator rule,
and it flattened block comments -- its reading of code differed from the owner's in
107 of the 503 files it scans (a `'"'` opened a string that hid the code after it;
14 src files hold one) -- while no verdict differed; the switch changed none.

Usage:
    python .harness-config/runner/actions/check-path-identity/check-path-identity.py            # check
    python .harness-config/runner/actions/check-path-identity/check-path-identity.py --selftest # prove the matcher detects
    python .harness-config/runner/actions/check-path-identity/check-path-identity.py --regen    # reprint the allowlist
"""
from __future__ import annotations

import importlib.util
import pathlib
import re
import sys
sys.dont_write_bytecode = True  # a by-path load must not write __pycache__ beside another action (the rule: check-scripts-index)

# ── OUTPUT ENCODING — NOT COSMETIC, AND THE STREAM IS HALF THE FACT ─────────────
# ✔MEASURED 2026-08-23 (CPython 3.14.3, Windows, BOTH streams PIPES, which is
# exactly how ctest runs every guard): `sys.stdout` comes up
# `encoding='cp1252' errors='surrogateescape'` and `sys.stderr` comes up
# `errors='backslashreplace'`. `surrogateescape` rescues only lone surrogates left
# by an earlier decode; it does NOTHING for an ordinary unencodable character. So a
# report printed on STDOUT — where this guard names every file that resolves paths
# outside the one canonicalizer —
# raises `UnicodeEncodeError` and kills the guard INSIDE ITS OWN REPORT: the run
# still reds, but the finding is lost and the traceback names a `print` rather than
# the thing that was wrong. STDERR merely mangles the glyph into an escape.
# ⚠ Paths are ASCII in this tree TODAY, which makes this prophylactic rather than a
# live red — and one non-ASCII filename away from not being.
# Applied at IMPORT, so every path this module can print on is covered.
for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError, OSError):   # pragma: no cover - odd stream
        pass


def _own_tree():
    """The tree THIS FILE lives in, by `owning-tree`'s walk -- never a count of parents.

    `.parent.parent.parent` named `.harness-config/runner` the day this file moved out of
    `scripts/` (2026-09-18); a guard pointed at the wrong root finds nothing to refuse. The
    owner is loaded as a SIBLING and a missing owner is a structural failure (exit 2).
    """
    path = pathlib.Path(__file__).resolve().parent.parent / "owning-tree" / "owning-tree.py"
    if not path.is_file():
        print("check-path-identity: cannot find %s -- this guard's tree is resolved there "
              "and nowhere else" % path, file=sys.stderr)
        sys.exit(2)
    spec = importlib.util.spec_from_file_location("dss_owning_tree", str(path))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    try:
        return pathlib.Path(mod.resolve(__file__))
    except mod.Refusal as exc:
        print("check-path-identity: %s" % exc, file=sys.stderr)
        sys.exit(2)


REPO = _own_tree()
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
core/substrate/checked_file_read.cpp
core/substrate/checked_file_read.hpp
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
core/types/project_sources.hpp
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
program/runtime_object_cache.cpp
program/runtime_object_cache.hpp
""".split())


def rel(p: pathlib.Path) -> str:
    return p.relative_to(SRC).as_posix()


def _load_stripper():
    """The comment/string scanner of `check-no-abort-in-tests`, or a loud death (exit 2).

    A rule that fires on a MENTION rather than a CALL trains people to reword
    their comments, which is worse than no rule: the next real occurrence hides
    behind the habit -- so this guard reads CODE only, through the one scanner every
    guard shares, and a second copy here is the drift that owner exists to stop.
    """
    sibling = (pathlib.Path(__file__).resolve().parent.parent
               / "check-no-abort-in-tests" / "check-no-abort-in-tests.py")
    if not sibling.is_file():
        print("check-path-identity: cannot find the shared comment/string scanner at %s -- this "
              "guard reads code through it and nowhere else; restore the sibling, do NOT copy it "
              "here" % sibling, file=sys.stderr)
        sys.exit(2)
    spec = importlib.util.spec_from_file_location("_no_abort_in_tests", str(sibling))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod.strip_comments_and_strings


strip_comments_and_strings = _load_stripper()


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
                f"in .harness-config/runner/actions/check-path-identity/check-path-identity.py.\n"
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
    # The two shapes the private copy misread: red the day a copy comes back.
    bad_after_char_quote = "char q = '\"'; auto r = fs::weakly_canonical(p);\n"
    ok_raw_string = 'auto m = R"(a "b weakly_canonical c)";\n'

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
    if "weakly_canonical" not in strip_comments_and_strings(bad_after_char_quote):
        failures.append("a character literal holding a double quote HID the real call after it "
                        "(the private copy's reading)")
    if "weakly_canonical" in strip_comments_and_strings(ok_raw_string):
        failures.append("a RAW string's body was treated as a call (the private copy's reading)")
    if strip_comments_and_strings.__module__ != "_no_abort_in_tests":
        failures.append("the comment/string scanner is not the SHARED one")

    for f in failures:
        print(f"SELFTEST FAIL: {f}")
    if failures:
        return 1
    print("check-path-identity selftest: OK (7 controls)")
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
    # ★★ THE NO-ARGUMENT FORM VERIFIES THE TREE **AND THEN** DRIVES THE
    # SELF-TEST, so the ctest entry cannot pass without also proving this guard
    # is able to fail.
    # ⛔ DO NOT make `--selftest` the registered form to get the arms: it RETURNS
    # from the self-test and never scans the tree, so it would trade the check
    # for the proof rather than adding the proof to the check. ✔MEASURED
    # 2026-08-24 (cycle P29, independent step-10 audit): registered bare, this
    # entry ran the scan and ZERO arms; the sibling defect was measured live in
    # `no_abort_in_tests_guard`, which ran 0 arms at HEAD with no CMakeLists
    # change needed to fix it — registration is not execution.
    return selftest()


if __name__ == "__main__":
    sys.exit(main())
