#!/usr/bin/env python3
# PURPOSE: census the diagnostics that RETYPE a closed vocabulary instead of projecting it.
"""Retyped-closed-set census (D-CONFIG-ENUM-KEYED-MAP-DIAGNOSTICS-RETYPE-THEIR-CLOSED-SET).

THE CLASS
---------
A loader (or any tier) decides acceptance with a TABLE -- an `EnumNameTable`, a
`...FromName` if-chain, a `std::array<string_view, N>` closed-key vocabulary --
and then states the accepted set in the diagnostic beside it as a STRING
LITERAL. Two owners of one fact. The rows keep working while the SENTENCE
becomes a lie, and the sentence is the half a config author reads: they are told
BY NAME that a spelling the loader accepts is not allowed, or that one it
refuses is fine. A test asserting the sentence CONTAINS a name stays green the
whole way down.

Not hypothetical: seven such messages were measured live in one cycle (P23) --
`operandKinds` named 3 of 7 rows, `resultSlot`/`slotKind` 8 of 32,
`terminatorKind` 6 of 7, four FFI-descriptor refusals 3 of 5,
`availableObjectFormats` 3 of 5, `bitFieldStrategy` 1 of 3, and the
`aggregateLayout` key sentence 2 of 3.

WHY THIS IS AN INSTRUMENT AND NOT A GATE -- read before proposing a ratchet
--------------------------------------------------------------------------
It reports; it does not fail. Exit is 0 with hits and 0 without (2 only on a
usage error). That is deliberate, and the reason is this script's own history:

  * The count is a property of the REGEX, not of the class. The first version
    matched SINGLE-quoted tokens only, so every message rendering its set in
    escaped double quotes was invisible; adding the second arm moved the
    tree-wide count from 40 to 77 WITHOUT ANY CODE CHANGING. A ratchet pinned to
    40 would have been asserting a fact about a regular expression, and the
    lane that read "40" read it as a fact about the tree.
  * The floor is not zero and cannot be computed. An honest `e.g.` hint, a TYPE
    error that merely mentions a vocabulary, and a comment quoting an old
    sentence all match and are all correct code. Deciding which is which is a
    JUDGEMENT about intent, which no matcher decides.
  * A non-zero ceiling therefore needs an allowlist -- and this project has
    already ruled on exactly that shape. The `.sh`/`.ps1` pairing gate was
    WITHDRAWN (operator, 2026-08-19) because it would have needed an allowlist
    of eleven correct exceptions: "the convention written twice, in the place
    least likely to be read, reddening honest work by default." The same
    argument applies here unchanged.
  * And a count ratchet is satisfiable in the wrong direction: convert one
    honest site, add one real defect, and the number is unmoved.

WHAT GATES THE CLASS INSTEAD, and it is a real gate: the ctest pins in
`tests/core/test_config_enum_vocabulary_projection.cpp`,
`tests/core/test_config_closed_key_vocabulary.cpp`,
`tests/core/test_vocabulary_projection_ffi_and_lir.cpp`,
`tests/core/test_target_vocabulary_projection.cpp`,
`tests/link/test_object_format_vocabulary_projection.cpp` and
`tests/link/test_linker_diagnostic_vocabulary.cpp`. Each drives the real loader,
reads the refusal it actually emits, and asserts it names EVERY spelling the
table accepts and NO spelling it does not -- driven from the table, so adding a
row without touching the message reds the test. That is a property, decidable
per site, and it is what a converted site is worth. This script's job is to
FIND the sites, not to judge them.

WHAT THIS INSTRUMENT CANNOT SEE -- state this with any count you quote
----------------------------------------------------------------------
  1. UNQUOTED lists. `--bare` adds a heuristic arm for them, off by default
     because it also matches ordinary prose that happens to use a vocabulary
     word. MEASURED: `vaListLayout.strategy` rendered its whole closed set as
     `(sysv_register_save / homogeneous_pointer / aapcs64_dual_cursor)` and was
     invisible at every `--min-tokens` until that arm existed.
  2. Sets whose members are ASSEMBLED at runtime. A message built as
     `"only " + std::to_string(n) + " symbols"` has no literal to match; a
     literal grep for the rendered sentence finds nothing and reads as "this
     text exists nowhere in src/", which has already been reported once as a
     fact. Grep the invariant FRAGMENT, never the rendered sentence.
  3. Vocabularies this script does not harvest: a closed set held in a
     `switch` with no name table, in a JSON schema file, or in a `std::map`.
     ⚠ THE INLINE `==` CHAIN USED TO BE ON THIS LINE AND IS NOT ANY MORE --
     see arm (d) below and the paragraph beneath this list, which records what
     that blind spot cost while it was open.
  4. Sets of ONE below the `--min-tokens` threshold (default 2). A one-element
     closed set is still a second owner -- `bitFieldStrategy`'s live 1-of-3
     drift and `scalarAlignment`'s `(expected "natural")` were both invisible at
     the default. Run `--min-tokens 1` before claiming a file is clean.

★★★ A FIFTH BLIND SPOT WAS FOUND BY AN AUDIT AND IS NOW CLOSED, AND THE WAY IT
WAS FOUND IS THE POINT. It was not on the list above, so nothing warned the
lane that quoted this script's numbers: arms (a)-(c) harvest an
`EnumNameTable`, a `…FromName` if-chain and a `std::array` key table, and an
INLINE `==` chain in a loader body is none of the three.

✔MEASURED 2026-08-21, both halves in ONE process so the delta is attributable
(three other lanes were editing `src/` at the time, and the ABSOLUTE numbers
moved between two runs an hour apart -- 51 hits then 56 -- with none of that
motion mine). Arms (a)-(c): **138 vocabularies / 56 hits** at `--min-tokens 2`.
Arms (a)-(d): **223 vocabularies / 78 hits**. So the inline `==` chain is
**85 vocabulary owners** this instrument could not see at all -- more owners
than the three documented arms harvest between them -- and **22** of the hits
measured against them were invisible. One was a LIVE drift:
`fieldChildren.compositeKind`'s type-error arm named 2 of the 3 spellings its
own chain accepts. The same cycle had already watched this count move 40 -> 77
on a quoting style. **A census that cannot see one of its subject's owner
shapes is not a smaller measurement -- it is a different one wearing the same
number, and a CLOSURE CLAIM built on it inherits the blindness.** Say which
arms produced any count you quote.

`--self-test` exercises all four arms against fixtures, plus the four grouping
rules that decide what arm (d) calls one chain -- including the two that were
MEASURED WRONG in its first draft (adjacent lambdas sharing a parameter name
must stay TWO vocabularies; an `else if` cascade with a 40-line arm must stay
ONE). It exits 1 on any arm that stops seeing its own subject -- a guard nobody
can make fail is a guard nobody has tested.

NO `.ps1` TWIN, deliberately: a `.py` already runs on both hosts, and a twin
would be a second implementation of something that was never split (the
`scripts/` convention's stated carve-out).

USAGE
-----
    python scripts/check-retyped-closed-sets/check-retyped-closed-sets.py \
        [--min-tokens N] [--bare] [--vocab] [--self-test] [PATH-SUBSTRING ...]

`PATH-SUBSTRING` filters to the files whose repo-relative path contains it.
`--vocab` lists the harvested vocabularies instead of the hits.
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / "src"

# -- 1. harvest the vocabularies -------------------------------------------
#
# Four shapes, because four shapes really are in the tree and the class is
# defined by "SOMETHING decides acceptance and the message states the same set
# again", not by which construct does the deciding.

# (a) `EnumNameTable<E, N> kXxxTable{{{ {E::A, "a"}, ... }}}` -- value-first rows.
ENUM_TABLE_RE = re.compile(
    r"EnumNameTable<\s*(\w+)\s*,\s*[\w:]+\s*>\s*\n?\s*(k\w+)\s*\{\{\{(.*?)\}\}\}",
    re.S)
TABLE_ROW_RE = re.compile(r"\{\s*[\w:]+::\w+\s*,\s*\"([^\"]*)\"\s*\}")

# (b) a hand-written `...FromName(std::string_view)` if-chain -- the pre-table
#     shape. Still present, and still a vocabulary owner while it is.
FROMNAME_RE = re.compile(
    r"(\w*[fF]romName)\s*\(\s*std::string_view\s+\w+\s*\)[^{]*\{(.*?)\n\}", re.S)
FROMNAME_LIT_RE = re.compile(r"==\s*\"([^\"]+)\"")

# (c) THE SIBLING CLASS ON THE KEY HALF, which the first two versions of this
#     instrument could not see at all: `std::array<std::string_view, N> kXxxKeys
#     {"a", "b"}`. A shape sentence spelling `{ "role": ..., "image": ... }`
#     beside a `kRuntimeLibraryRowKeys{"role","image"}` is the same defect one
#     axis over -- MEASURED: the `aggregateLayout` shape sentence named two of
#     the three keys its own `rejectUnknownKeys` accepted.
KEY_TABLE_RE = re.compile(
    r"std::array<\s*std::string_view\s*,\s*\d+\s*>\s*\n?\s*(k\w+)\s*\{([^{}]*)\}",
    re.S)
KEY_ROW_RE = re.compile(r"\"([^\"]+)\"")

# (d) THE OWNER SHAPE ARM (b) COULD NOT SEE: a BARE `==` CHAIN in a loader body,
#     owned by no function at all.
#
# ★★ WHY THIS ARM EXISTS, AND WHY ITS ABSENCE WAS NOT A SMALLER MEASUREMENT BUT
# A DIFFERENT ONE. Arm (b) requires the chain to sit inside something spelled
# `…FromName(std::string_view)`. A loader that writes the same three-way
# decision INLINE --
#
#     if      (k == "struct") fcd.compositeKind = CompositeKind::Struct;
#     else if (k == "union")  fcd.compositeKind = CompositeKind::Union;
#     else if (k == "enum")   fcd.compositeKind = CompositeKind::Enum;
#
# -- owns the vocabulary just as completely, and every message beside it is the
# same second owner. ✔MEASURED 2026-08-20 (cycle P23): with arms (a)-(c) only,
# `grammar_schema_json.cpp`'s `fieldChildren.compositeKind` block reported ZERO
# hits at every `--min-tokens`, while one of its three sentences had ALREADY
# drifted to 2 of the 3 spellings the chain accepts -- a config author writing
# `compositeKind: 5` was told by name that `enum` is not allowed. The site was
# invisible because the CENSUS had no `CompositeKind` vocabulary to match
# against, not because the file was clean.
#
# ⚠ SO A CLOSURE CLAIM BUILT ON THIS SCRIPT INHERITS THE SCRIPT'S DOMAIN.
# "N sites converted" is a statement about what the harvester can see; the
# blind-spot list below is the other half of every count this file prints.
#
# THE SHAPE MATCHED: a run of `<expr> == "literal"` comparisons sharing one
# left-hand expression. Two comparisons JOIN when either
#
#   * the second is written `else if (` -- SYNTACTIC PROOF that it continues the
#     same cascade, so no distance limit applies; or
#   * they are a bare `if` run within `IF_CHAIN_GAP_LINES` of each other AND no
#     `};` (a lambda / aggregate terminator) sits between them.
#
# ⚠ BOTH HALVES WERE MEASURED, NOT GUESSED. The first draft of this arm grouped
# on PROXIMITY ALONE and was wrong in BOTH directions on this tree:
#   * TOO WIDE -- `parseConstructor` and `parseNameMatch` in
#     `grammar_schema_json.cpp` are adjacent lambdas three lines apart that both
#     name their parameter `name`, and the window fused
#     `pointer/reference/nullable/optional/slice` with `self/lastIdentifier`
#     into one seven-name vocabulary that exists nowhere. The hits stayed real;
#     the OWNER they named did not, and the owner is the half a reader opens the
#     file to find.
#   * TOO NARROW -- `imports.strategy`'s middle arm is ~60 lines long, so the
#     window cut a THREE-name chain down to two. That is the dangerous
#     direction: a message naming 2 of 3 measured against a truncated 2-name
#     vocabulary reads as CONSISTENT, and the census would have certified
#     exactly the drift it exists to find.
#
# The mirrored spelling (`"literal" == expr`) is NOT matched -- it appears
# nowhere in this tree, and an arm with no subject is an untested arm.
IF_CHAIN_CMP_RE = re.compile(
    r"([A-Za-z_]\w*(?:(?:\.|->)\w+)*(?:\(\))?)\s*==\s*\"([^\"\\]*)\"")
IF_CHAIN_ELSE_RE = re.compile(r"\belse\s+if\s*\(\s*$")
IF_CHAIN_GAP_LINES = 8


def harvest_inline_chains(text: str) -> list[tuple[str, int, set[str]]]:
    """Group `<expr> == "lit"` comparisons into inline vocabularies.

    Returns `(lhs, first-line, names)` per group. Separated from the file walk
    so `--self-test` can drive it over a fixture: an arm nobody can exercise in
    isolation is an arm whose failure mode is a guess.
    """
    groups: list[tuple[str, int, set[str]]] = []
    prev_lhs: str | None = None
    prev_line = 0
    prev_end = 0
    for m in IF_CHAIN_CMP_RE.finditer(text):
        lhs, lit = m.group(1), m.group(2)
        line = line_of(text, m.start())
        prefix = text[text.rfind("\n", 0, m.start()) + 1:m.start()]
        continues = bool(IF_CHAIN_ELSE_RE.search(prefix)) or (
            line - prev_line <= IF_CHAIN_GAP_LINES
            and "};" not in text[prev_end:m.start()])
        if groups and prev_lhs == lhs and continues:
            groups[-1][2].add(lit)
        else:
            groups.append((lhs, line, {lit}))
        prev_lhs, prev_line, prev_end = lhs, line, m.end()
    return [g for g in groups if len({n for n in g[2] if n}) >= 2]


def harvest_from_text(text: str, rel: str) -> dict[str, set[str]]:
    """Every vocabulary one translation unit OWNS. `rel` labels them only."""
    vocab: dict[str, set[str]] = {}
    for enum, table, body in ENUM_TABLE_RE.findall(text):
        names = {n for n in TABLE_ROW_RE.findall(body) if n}
        if len(names) >= 2:
            vocab[f"{table} (EnumNameTable<{enum}>) @ {rel}"] = names
    for fn, body in FROMNAME_RE.findall(text):
        names = set(FROMNAME_LIT_RE.findall(body))
        if len(names) >= 2:
            vocab[f"{fn}() if-chain @ {rel}"] = names
    for table, body in KEY_TABLE_RE.findall(text):
        names = {n for n in KEY_ROW_RE.findall(body) if n}
        if len(names) >= 2:
            vocab[f"{table} (closed-key table) @ {rel}"] = names
    for lhs, line, names in harvest_inline_chains(text):
        live = {n for n in names if n}
        if len(live) < 2:
            continue
        # The position DISAMBIGUATES -- two chains on a variable both spelled
        # `k` are two vocabularies, and keying them by name alone would silently
        # keep one and drop the other. It is a position, never a citation.
        vocab[f"{lhs} ==-chain @ {rel} (line {line}, POSITION ONLY)"] = live
    return vocab


def harvest_vocabularies() -> dict[str, set[str]]:
    vocab: dict[str, set[str]] = {}
    for path in sorted(SRC.rglob("*.hpp")) + sorted(SRC.rglob("*.cpp")):
        text = path.read_text(encoding="utf-8", errors="replace")
        vocab.update(harvest_from_text(text, str(path.relative_to(ROOT))))
    return vocab


# -- 2. harvest the quoted-token literals ----------------------------------
STRING_RUN_RE = re.compile(r'(?:"(?:[^"\\]|\\.)*"\s*)+')

# EITHER quoting style. The runtime payload of `"accepted: \"syscall\""` is
# `accepted: "syscall"` -- a plain double quote -- so the two arms are symmetric
# once the C++ escaping is undone. The single-quote-only version of this regex
# is what made the v1 census a lower bound while reading as a measurement.
QUOTED_TOKEN_RE = re.compile(r"'([^' ,]{1,24})'|\"([^\" ,]{1,24})\"")

# A `{}`/`{0}` std::format placeholder is not a vocabulary token. Scrubbed to a
# NUL so a token that swallowed one is dropped rather than half-matched.
PLACEHOLDER_RE = re.compile(r"\{[^{}]*\}")

# `--bare`: an unquoted run of vocabulary-shaped words. Deliberately narrow --
# alphanumerics with `_`/`-`, at least 3 chars -- and still noisy, which is why
# it is opt-in.
BARE_TOKEN_RE = re.compile(r"[A-Za-z][A-Za-z0-9_-]{2,23}")


def line_of(text: str, offset: int) -> int:
    return text.count("\n", 0, offset) + 1


def harvest_literals(path: Path, min_tokens: int, bare: bool):
    return harvest_literals_from_text(
        path.read_text(encoding="utf-8", errors="replace"), min_tokens, bare)


def harvest_literals_from_text(text: str, min_tokens: int, bare: bool):
    out = []
    for m in STRING_RUN_RE.finditer(text):
        run = m.group(0)
        payload = "".join(re.findall(r'"((?:[^"\\]|\\.)*)"', run))
        payload = payload.replace('\\"', '"')      # undo the C++ escaping
        scrubbed = PLACEHOLDER_RE.sub("\x00", payload)
        toks = [a or b for a, b in QUOTED_TOKEN_RE.findall(scrubbed)]
        toks = [t for t in toks if "\x00" not in t]
        kind = "quoted"
        if len(toks) < min_tokens and bare:
            toks = BARE_TOKEN_RE.findall(scrubbed)
            kind = "bare"
        if len(toks) >= min_tokens:
            out.append((line_of(text, m.start()), payload, toks, kind))
    return out


# THE MATCH RULE, in ONE place. A literal whose tokens are ALL owned by some
# vocabulary is STRICT; otherwise a vocabulary sharing at least `min_tokens` (and
# never fewer than 2) of them is a PARTIAL. Factored out because the self-test
# below has to exercise the same rule the census applies, and a second copy of a
# matching rule is the exact defect this whole script is a census of.
def owners_for(tokset: set[str], vocab: dict[str, set[str]],
               min_tokens: int) -> list[str]:
    owners = [f"[STRICT ] {name}" for name, names in vocab.items()
              if tokset <= names]
    if owners:
        return owners
    return [f"[PARTIAL] {name} - matched {sorted(tokset & names)}"
            for name, names in vocab.items()
            if len(tokset & names) >= max(2, min_tokens)]


# -- 3. the self-test -------------------------------------------------------
#
# ★ WHY A CENSUS THAT NEVER FAILS STILL NEEDS ONE. Every number this script
# prints is a claim about what its four arms can see, and an arm that quietly
# stopped matching would lower the count -- which reads as PROGRESS. The four
# positive cases below each carry a vocabulary only ONE arm can harvest, so
# breaking that arm's regex fails this and nothing else. The two negative cases
# are arm (d)'s honesty: proximity is what separates a chain from two unrelated
# comparisons, and without them the safe-looking widening of the window would
# never show a cost.
_FIXTURE_ENUM_TABLE = """
inline constexpr EnumNameTable<Kind, 2> kFixtureTable{{{
    { Kind::Alpha, "alpha" },
    { Kind::Beta,  "beta"  },
}}};
"""

_FIXTURE_FROMNAME = """
constexpr std::optional<Kind> fixtureFromName(std::string_view s) noexcept {
    if (s == "alpha") return Kind::Alpha;
    if (s == "beta")  return Kind::Beta;
    return std::nullopt;
}
"""

_FIXTURE_KEY_TABLE = """
inline constexpr std::array<std::string_view, 2> kFixtureKeys{"alpha", "beta"};
"""

# Arm (d)'s subject: no table, no `…FromName`, no `std::array` -- the decision
# itself is the vocabulary.
_FIXTURE_INLINE_CHAIN = """
        auto const k = fc.at("fixtureKind").get<std::string>();
        if (k == "alpha") {
            out.kind = Kind::Alpha;
        } else if (k == "beta") {
            out.kind = Kind::Beta;
        }
"""

_FIXTURE_FAR_APART = ("\nif (name == \"alpha\") { alpha(); }\n"
                      + "// filler\n" * (IF_CHAIN_GAP_LINES + 3)
                      + "if (name == \"beta\") { beta(); }\n")

_FIXTURE_SINGLE = "\nif (k == \"alpha\") { alpha(); }\n"

# The real false fusion this arm shipped with for one draft: two ADJACENT
# lambdas whose parameters share a name. Three lines apart, so proximity alone
# joins them; the `};` between is what says they are two scopes. Expected: two
# vocabularies, never one of four names.
_FIXTURE_ADJACENT_LAMBDAS = """
        auto const parseOne = [](std::string_view name) {
            if (name == "alpha") return One::Alpha;
            if (name == "beta")  return One::Beta;
            return std::nullopt;
        };

        auto const parseTwo = [](std::string_view name) {
            if (name == "gamma") return Two::Gamma;
            if (name == "delta") return Two::Delta;
            return std::nullopt;
        };
"""

# The other direction: an `else if` cascade whose middle arm is longer than any
# proximity window. `else if (` is syntactic proof of continuation, so all three
# names must land in ONE vocabulary -- a chain truncated to two of its three
# names makes a 2-of-3 message read as consistent.
_FIXTURE_LONG_ELSE_ARM = ("""
        if (s == "alpha") {
            cfg.kind = Kind::Alpha;
"""
                          + "            doSomething();\n" * 40
                          + """        } else if (s == "beta") {
            cfg.kind = Kind::Beta;
        } else if (s == "gamma") {
            cfg.kind = Kind::Gamma;
        }
""")

# The retyped sentence arm (d) exists to catch: it states the set the chain
# above owns, so the census must attribute it to that chain.
_FIXTURE_RETYPED_MESSAGE = """
    coll.emit(Code::X, "/fixtureKind",
              "'fixtureKind' must be 'alpha' or 'beta'");
"""


def run_self_test() -> int:
    expected = {"alpha", "beta"}
    failures: list[str] = []

    def harvested(text: str) -> list[set[str]]:
        return list(harvest_from_text(text, "<fixture>").values())

    for label, text in (("(a) EnumNameTable", _FIXTURE_ENUM_TABLE),
                        ("(b) ...FromName if-chain", _FIXTURE_FROMNAME),
                        ("(c) closed-key table", _FIXTURE_KEY_TABLE),
                        ("(d) inline == chain", _FIXTURE_INLINE_CHAIN)):
        if expected not in harvested(text):
            failures.append(
                f"{label}: harvested {harvested(text)}, expected a vocabulary "
                f"{sorted(expected)}. This arm has stopped seeing its own "
                f"subject, and the tree-wide count would DROP -- which reads "
                f"as progress.")

    for label, text in (("(d) two far-apart comparisons", _FIXTURE_FAR_APART),
                        ("(d) a lone comparison", _FIXTURE_SINGLE)):
        if harvested(text):
            failures.append(
                f"{label}: harvested {harvested(text)}, expected NOTHING. "
                f"Arm (d) is fusing comparisons that are not a chain, which "
                f"invents vocabularies and makes every hit against them noise.")

    # The `};` scope break: two owners must stay two.
    lambdas = harvested(_FIXTURE_ADJACENT_LAMBDAS)
    if sorted(lambdas, key=sorted) != [{"alpha", "beta"}, {"delta", "gamma"}]:
        failures.append(
            f"(d) adjacent lambdas sharing a parameter name: harvested "
            f"{lambdas}, expected TWO vocabularies. Fusing two owners is worse "
            f"than missing one -- the hits stay real while the OWNER they name "
            f"exists nowhere, so a reader sent to find the table finds nothing.")

    # The `else if` continuation: one owner must stay one, at any arm length.
    long_arm = harvested(_FIXTURE_LONG_ELSE_ARM)
    if long_arm != [{"alpha", "beta", "gamma"}]:
        failures.append(
            f"(d) an `else if` cascade with a long middle arm: harvested "
            f"{long_arm}, expected ONE three-name vocabulary. A chain truncated "
            f"by a proximity window makes a message naming 2 of 3 compare as "
            f"CONSISTENT -- the census certifying the drift it exists to find.")

    # End to end: harvest the chain, then match the sentence that retypes it.
    vocab = harvest_from_text(_FIXTURE_INLINE_CHAIN, "<fixture>")
    lits = harvest_literals_from_text(_FIXTURE_RETYPED_MESSAGE, 2, False)
    matched = [o for _l, _p, toks, _k in lits
               for o in owners_for(set(toks), vocab, 2)]
    if not matched:
        failures.append(
            "end-to-end: the sentence retyping the inline chain's set was "
            "attributed to NO owner. Harvest and match are wired together by "
            "the token-subset rule; an arm that harvests into a name nothing "
            "compares against finds nothing.")

    checks = 9   # 4 positive arms + 4 arm-(d) grouping rules + 1 end-to-end
    for f in failures:
        print(f"SELF-TEST FAILED: {f}", file=sys.stderr)
    print(f"# self-test: {checks - len(failures)}/{checks} checks passed")
    return 1 if failures else 0


def main() -> int:
    ap = argparse.ArgumentParser(
        description="census diagnostics that retype a closed vocabulary")
    ap.add_argument("paths", nargs="*", metavar="PATH-SUBSTRING")
    ap.add_argument("--min-tokens", type=int, default=2,
                    help="how many vocabulary tokens a literal must carry to "
                         "count (default 2). 2 HIDES EVERY ONE-ELEMENT SET, "
                         "and two live drifts were found there -- run 1 before "
                         "calling a file clean")
    ap.add_argument("--bare", action="store_true",
                    help="also match UNQUOTED vocabulary runs (noisy; finds the "
                         "`(a / b / c)` shape no quoted regex can see)")
    ap.add_argument("--vocab", action="store_true",
                    help="list the harvested vocabularies and stop")
    ap.add_argument("--self-test", action="store_true",
                    help="exercise every harvester arm against a fixture and "
                         "exit 1 if any of them stopped seeing its subject")
    args = ap.parse_args()

    # Windows consoles and redirected pipes default to cp1252, and this
    # script's own subject matter is diagnostics full of em dashes and arrows.
    # Without this the run died mid-listing with a UnicodeEncodeError AFTER
    # printing the count line -- a reader greps for their file, does not find
    # it, and concludes it is clean. A census that can stop early must not do
    # so silently.
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except (AttributeError, ValueError):
            pass

    if args.min_tokens < 1:
        print("--min-tokens must be >= 1", file=sys.stderr)
        return 2

    if args.self_test:
        return run_self_test()

    vocab = harvest_vocabularies()
    print(f"# vocabularies harvested: {len(vocab)}")
    if args.vocab:
        for name in sorted(vocab):
            print(f"  {name}\n    {sorted(vocab[name])}")
        return 0

    hits = []
    for path in sorted(SRC.rglob("*.cpp")) + sorted(SRC.rglob("*.hpp")):
        rel = str(path.relative_to(ROOT)).replace("\\", "/")
        if args.paths and not any(p in rel for p in args.paths):
            continue
        for lineno, payload, toks, kind in harvest_literals(
                path, args.min_tokens, args.bare):
            tokset = set(toks)
            if not tokset:
                continue
            owners = owners_for(tokset, vocab, args.min_tokens)
            if owners:
                hits.append((rel, lineno, payload, toks, kind, owners))

    n_strict = sum(1 for h in hits if h[5][0].startswith("[STRICT"))
    print(f"# retyped-set literals found: {len(hits)}"
          f"  (STRICT {n_strict} / PARTIAL {len(hits) - n_strict})")
    print(f"# settings: --min-tokens {args.min_tokens}"
          f"{' --bare' if args.bare else ''}")
    print("# this count is what the MATCHER sees, not the size of the class "
          "-- see this file's header for the four documented blind spots\n")
    for rel, lineno, payload, toks, kind, owners in hits:
        print(f"--- {rel}  (line {lineno} -- POSITION ONLY, never a citation)")
        print(f"    tokens : {toks}  [{kind}]")
        short = payload if len(payload) < 300 else payload[:300] + "..."
        print(f"    text   : {short}")
        for o in owners:
            print(f"    OWNER  : {o}")
        print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
